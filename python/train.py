#!/usr/bin/env python3
"""Train the offline 11-16-16-output MLP on chronological labelled feature dumps."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import random
import sys

import numpy as np

HIDDEN_WIDTH = 16

from tickbook_data import (
    FEATURE_COUNT,
    FEATURE_SCALE,
    LABEL_DIRECTION,
    LABEL_RETURN,
    combine_label_dumps,
    walk_forward_folds,
)


def _load_torch():
    try:
        import torch
        from torch import nn
    except ImportError as error:
        raise RuntimeError("training requires PyTorch; install python/requirements.txt") from error
    return torch, nn


def _make_network(nn, output_count: int):
    class MLP(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            self.layer0 = nn.Linear(FEATURE_COUNT, HIDDEN_WIDTH)
            self.layer1 = nn.Linear(HIDDEN_WIDTH, HIDDEN_WIDTH)
            self.layer2 = nn.Linear(HIDDEN_WIDTH, output_count)

        def forward(self, inputs):
            return self.layer2(self.layer1(self.layer0(inputs).relu()).relu())

    return MLP()


def _training_labels(dataset, task: str) -> np.ndarray:
    if task == "direction":
        return dataset.labels.astype(np.int64, copy=False)
    return (dataset.labels.astype(np.float64) / FEATURE_SCALE).astype(np.float32)


def _fit_logistic(torch, nn, x_train, y_train, config, seed: int):
    nonflat = y_train != (1 if y_train.dtype == torch.int64 else 0)
    selected_x = x_train[nonflat]
    selected_y = y_train[nonflat]
    if selected_y.numel() == 0:
        return None
    binary_y = (selected_y == 2).to(torch.float32) if y_train.dtype == torch.int64 else (
        selected_y > 0
    ).to(torch.float32)
    if binary_y.min() == binary_y.max():
        return None
    torch.manual_seed(seed)
    model = nn.Linear(FEATURE_COUNT, 1)
    optimizer = torch.optim.Adam(
        model.parameters(),
        lr=float(config["learning_rate"]),
        weight_decay=float(config["weight_decay"]),
    )
    loss_function = nn.BCEWithLogitsLoss()
    batch_size = int(config["batch_size"])
    model.train()
    for _ in range(int(config["baseline_epochs"])):
        for start in range(0, selected_x.shape[0], batch_size):
            end = min(start + batch_size, selected_x.shape[0])
            optimizer.zero_grad(set_to_none=True)
            loss = loss_function(model(selected_x[start:end]).squeeze(1), binary_y[start:end])
            loss.backward()
            optimizer.step()
    model.eval()
    return model


def _fit_mlp(torch, nn, x_train, y_train, task: str, config, seed: int):
    torch.manual_seed(seed)
    output_count = 3 if task == "direction" else 1
    model = _make_network(nn, output_count)
    optimizer = torch.optim.Adam(
        model.parameters(),
        lr=float(config["learning_rate"]),
        weight_decay=float(config["weight_decay"]),
    )
    if task == "direction":
        counts = torch.bincount(y_train, minlength=3).to(torch.float32)
        weights = torch.zeros_like(counts)
        present = counts > 0
        weights[present] = counts.sum() / (present.sum() * counts[present])
        loss_function = nn.CrossEntropyLoss(weight=weights)
    else:
        loss_function = nn.HuberLoss(delta=float(config["huber_delta_ticks"]))
    batch_size = int(config["batch_size"])
    model.train()
    for _ in range(int(config["epochs"])):
        # Contiguous batches preserve time order; no DataLoader shuffle is used.
        for start in range(0, x_train.shape[0], batch_size):
            end = min(start + batch_size, x_train.shape[0])
            optimizer.zero_grad(set_to_none=True)
            output = model(x_train[start:end])
            if task == "return":
                output = output.squeeze(1)
            loss = loss_function(output, y_train[start:end])
            loss.backward()
            optimizer.step()
    model.eval()
    return model


def _prepare_normalization(features: np.ndarray, train_indices: np.ndarray):
    # Match C++ exactly: raw int64 values become float32 before normalization.
    raw = features.astype(np.float32)
    training = raw[train_indices].astype(np.float64)
    mean = training.mean(axis=0).astype(np.float32)
    standard_deviation = training.std(axis=0).astype(np.float32)
    standard_deviation[standard_deviation == 0] = 1.0
    normalized = ((raw - mean) / standard_deviation).astype(np.float32)
    return normalized, mean, standard_deviation


def _fold_metrics(torch, model, logistic, x_test, y_test, features_test, task: str):
    with torch.no_grad():
        model_output = model(x_test)
        if task == "direction":
            prediction = model_output.argmax(dim=1).cpu().numpy()
            target = y_test.cpu().numpy()
            nonflat = target != 1
            model_hit = float(np.mean(prediction[nonflat] == target[nonflat])) if nonflat.any() else None
            overall_accuracy = float(np.mean(prediction == target))
            model_metrics = {"directional_hit_rate": model_hit, "overall_accuracy": overall_accuracy}
            target_direction = target
        else:
            prediction = model_output.squeeze(1).cpu().numpy()
            target = y_test.cpu().numpy()
            nonflat = target != 0
            model_hit = (
                float(np.mean(np.sign(prediction[nonflat]) == np.sign(target[nonflat])))
                if nonflat.any()
                else None
            )
            residual = prediction - target
            model_metrics = {
                "directional_hit_rate": model_hit,
                "mae_ticks": float(np.mean(np.abs(residual))),
                "rmse_ticks": float(np.sqrt(np.mean(residual * residual))),
            }
            target_direction = np.where(target > 0, 2, np.where(target < 0, 0, 1))

        raw_imbalance = features_test[:, 1]
        nonflat_mask = target_direction != 1
        sign_hit = (
            np.sign(raw_imbalance[nonflat_mask])
            == np.where(target_direction[nonflat_mask] == 2, 1, -1)
        )
        sign_hit_rate = float(np.mean(sign_hit)) if nonflat_mask.any() else None

        logistic_hit_rate = None
        if logistic is not None:
            scores = logistic(x_test).squeeze(1).sigmoid().cpu().numpy()
            logistic_prediction = scores >= 0.5
            logistic_hit_rate = (
                float(
                    np.mean(
                        logistic_prediction[nonflat_mask]
                        == (target_direction[nonflat_mask] == 2)
                    )
                )
                if nonflat_mask.any()
                else None
            )

    return {
        "mlp": model_metrics,
        "baseline_sign_imbalance_l1_hit_rate": sign_hit_rate,
        "baseline_logistic_all_features_hit_rate": logistic_hit_rate,
        "nonflat_test_rows": int(nonflat_mask.sum()),
        "test_rows": int(target_direction.size),
    }


def train(input_paths: list[str], task: str, output_dir: str, config_path: str) -> dict:
    config = json.loads(Path(config_path).read_text(encoding="utf-8"))
    if task == "direction":
        kind = LABEL_DIRECTION
    elif task == "return":
        kind = LABEL_RETURN
    else:
        raise ValueError("task must be direction or return")
    dataset = combine_label_dumps(input_paths, kind)
    if dataset.horizon_seconds not in (1, 3, 10):
        raise ValueError("unsupported prediction horizon")
    folds = walk_forward_folds(
        dataset.timestamps_ns,
        dataset.horizon_seconds,
        minimum_training_days=int(config["minimum_training_days"]),
    )
    torch, nn = _load_torch()
    torch.set_num_threads(1)
    torch.use_deterministic_algorithms(True)
    seed = int(config["seed"])
    random.seed(seed)
    np.random.seed(seed)
    labels = _training_labels(dataset, task)

    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    metrics = {
        "task": task,
        "symbol": dataset.symbol,
        "horizon_seconds": dataset.horizon_seconds,
        "label_dead_zone_ticks": dataset.dead_zone_ticks,
        "sample_count": int(dataset.timestamps_ns.size),
        "day_count": int(np.unique(dataset.timestamps_ns // 86_400_000_000_000).size),
        "seed": int(config["seed"]),
        "split": "expanding UTC days, no shuffle; purge train labels reaching test day",
        "configuration": config,
        "folds": [],
    }

    final_model = None
    final_fold = None
    final_mean = None
    final_standard_deviation = None
    for fold_index, fold in enumerate(folds):
        fold_normalized, fold_mean, fold_std = _prepare_normalization(
            dataset.features, fold.train_indices
        )
        x_all = torch.from_numpy(fold_normalized)
        y_all = torch.from_numpy(labels)
        y_train = y_all[fold.train_indices]
        if task == "return":
            y_train = y_train.to(torch.float32)
        model = _fit_mlp(
            torch,
            nn,
            x_all[fold.train_indices],
            y_train,
            task,
            config,
            int(config["seed"]) + fold_index,
        )
        y_test = y_all[fold.test_indices]
        if task == "return":
            y_test = y_test.to(torch.float32)
        logistic = _fit_logistic(
            torch,
            nn,
            x_all[fold.train_indices],
            y_all[fold.train_indices],
            config,
            int(config["seed"]) + 10_000 + fold_index,
        )
        fold_result = _fold_metrics(
            torch,
            model,
            logistic,
            x_all[fold.test_indices],
            y_test,
            dataset.features[fold.test_indices],
            task,
        )
        fold_result.update(
            {
                "test_day_utc": str(
                    np.datetime64(fold.test_day, "D").astype("datetime64[D]")
                ),
                "first_test_timestamp_ns": fold.first_test_timestamp_ns,
                "training_rows": int(fold.train_indices.size),
                "purged_training_rows": fold.purged_train_rows,
            }
        )
        metrics["folds"].append(fold_result)
        final_model, final_fold = model, fold
        final_mean, final_standard_deviation = fold_mean, fold_std

    checkpoint: dict[str, np.ndarray] = {
        "input_mean": final_mean,
        "input_std": final_standard_deviation,
        "task_direction": np.asarray(1 if task == "direction" else 0, dtype=np.int32),
        "horizon_seconds": np.asarray(dataset.horizon_seconds, dtype=np.int32),
    }
    for layer_index in range(3):
        layer = getattr(final_model, f"layer{layer_index}")
        checkpoint[f"layer{layer_index}_weights"] = layer.weight.detach().cpu().numpy().astype(
            np.float32, copy=True
        )
        checkpoint[f"layer{layer_index}_biases"] = layer.bias.detach().cpu().numpy().astype(
            np.float32, copy=True
        )
    np.savez(output_path / "checkpoint.npz", **checkpoint)
    metrics["exported_training_rows"] = int(final_fold.train_indices.size)
    metrics["exported_training_day_count"] = int(
        np.unique(dataset.timestamps_ns[final_fold.train_indices] // 86_400_000_000_000).size
    )
    (output_path / "metrics.json").write_text(
        json.dumps(metrics, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return metrics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", nargs="+", required=True, help="labelled .tfl dumps in any order")
    parser.add_argument("--task", choices=("direction", "return"), required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--config", default=str(Path(__file__).with_name("config.json")))
    args = parser.parse_args()
    try:
        report = train(args.input, args.task, args.output_dir, args.config)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"train.py: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"folds": len(report["folds"]), "samples": report["sample_count"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
