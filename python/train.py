#!/usr/bin/env python3
"""Train the offline 11-16-16-output MLP on chronological labelled feature dumps."""

from __future__ import annotations

import argparse
from decimal import Decimal, InvalidOperation
import json
from pathlib import Path
import random
import sys

import numpy as np

HIDDEN_WIDTH = 16
MAXIMUM_HIDDEN_WIDTH = 32
_BLOCK_NS = 3_600_000_000_000
_BOOTSTRAP_REPLICATES = 2_000

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


def _make_network(nn, output_count: int, hidden_width: int):
    class MLP(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            self.layer0 = nn.Linear(FEATURE_COUNT, hidden_width)
            self.layer1 = nn.Linear(hidden_width, hidden_width)
            self.layer2 = nn.Linear(hidden_width, output_count)

        def forward(self, inputs):
            return self.layer2(self.layer1(self.layer0(inputs).relu()).relu())

    return MLP()


def _shuffled_batches(sample_count: int, batch_size: int, seed: int):
    """Yield a seeded permutation of positions in the supplied training subset."""
    if sample_count < 0 or batch_size <= 0:
        raise ValueError("sample count must be non-negative and batch size must be positive")
    order = np.arange(sample_count, dtype=np.int64)
    np.random.default_rng(seed).shuffle(order)
    for start in range(0, sample_count, batch_size):
        yield order[start : start + batch_size]


def _positive_decimal(name: str, value: str | float | Decimal) -> Decimal:
    try:
        result = Decimal(str(value))
    except (InvalidOperation, ValueError) as error:
        raise ValueError(f"{name} must be a positive decimal") from error
    if not result.is_finite() or result <= 0:
        raise ValueError(f"{name} must be a positive decimal")
    return result


def _dead_zone_basis_points(
    dead_zone_ticks: int, price_tick: str | float | Decimal, reference_price: str | float | Decimal
) -> float:
    if dead_zone_ticks < 0:
        raise ValueError("dead zone ticks must be non-negative")
    tick = _positive_decimal("price tick", price_tick)
    price = _positive_decimal("reference price", reference_price)
    return float(Decimal(dead_zone_ticks) * tick / price * Decimal(10_000))


def _validate_break_even_dead_zone(
    dead_zone_ticks: int,
    price_tick: str | float | Decimal,
    reference_price: str | float | Decimal,
    break_even_move_bps: float,
    tolerance_fraction: float = 0.25,
) -> float:
    target = _positive_decimal("break-even move", break_even_move_bps)
    if not 0 < tolerance_fraction < 1:
        raise ValueError("break-even tolerance must be between zero and one")
    dead_zone_bps = _dead_zone_basis_points(dead_zone_ticks, price_tick, reference_price)
    if abs(Decimal(str(dead_zone_bps)) - target) > target * Decimal(str(tolerance_fraction)):
        raise ValueError(
            f"direction label dead zone {dead_zone_bps:.4f} bps is not near the configured "
            f"break-even move of {float(target):.4f} bps"
        )
    return dead_zone_bps


def _block_bootstrap_confidence_interval(
    timestamps_ns: np.ndarray,
    correct: np.ndarray,
    eligible: np.ndarray,
    horizon_seconds: int,
    seed: int,
) -> dict:
    """Bootstrap hit rate by disjoint UTC-hour blocks, purging labels across block ends."""
    timestamps = np.asarray(timestamps_ns, dtype=np.int64)
    correct = np.asarray(correct, dtype=bool)
    eligible = np.asarray(eligible, dtype=bool).copy()
    if timestamps.ndim != 1 or correct.shape != timestamps.shape or eligible.shape != timestamps.shape:
        raise ValueError("confidence-interval inputs must have the same one-dimensional shape")
    if horizon_seconds <= 0:
        raise ValueError("confidence-interval horizon must be positive")

    block_ids = timestamps // _BLOCK_NS
    block_ends = (block_ids + 1) * _BLOCK_NS
    eligible &= timestamps < block_ends - horizon_seconds * 1_000_000_000
    used_blocks = np.unique(block_ids[eligible])
    successes = np.empty(used_blocks.size, dtype=np.int64)
    totals = np.empty(used_blocks.size, dtype=np.int64)
    for index, block_id in enumerate(used_blocks):
        in_block = eligible & (block_ids == block_id)
        totals[index] = np.count_nonzero(in_block)
        successes[index] = np.count_nonzero(correct & in_block)

    result = {
        "block_count": int(used_blocks.size),
        "sample_count": int(totals.sum()),
        "lower": None,
        "upper": None,
    }
    if used_blocks.size < 2:
        return result
    generator = np.random.default_rng(seed)
    sampled = generator.integers(
        0, used_blocks.size, size=(_BOOTSTRAP_REPLICATES, used_blocks.size)
    )
    sampled_successes = successes[sampled].sum(axis=1)
    sampled_totals = totals[sampled].sum(axis=1)
    rates = sampled_successes / sampled_totals
    result["lower"] = float(np.quantile(rates, 0.025))
    result["upper"] = float(np.quantile(rates, 0.975))
    return result


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
    for epoch in range(int(config["baseline_epochs"])):
        for positions in _shuffled_batches(selected_x.shape[0], batch_size, seed + epoch):
            batch_indices = torch.from_numpy(positions)
            optimizer.zero_grad(set_to_none=True)
            loss = loss_function(
                model(selected_x.index_select(0, batch_indices)).squeeze(1),
                binary_y.index_select(0, batch_indices),
            )
            loss.backward()
            optimizer.step()
    model.eval()
    return model


def _fit_mlp(torch, nn, x_train, y_train, task: str, config, seed: int, hidden_width: int):
    torch.manual_seed(seed)
    output_count = 3 if task == "direction" else 1
    model = _make_network(nn, output_count, hidden_width)
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
    for epoch in range(int(config["epochs"])):
        for positions in _shuffled_batches(x_train.shape[0], batch_size, seed + epoch):
            batch_indices = torch.from_numpy(positions)
            optimizer.zero_grad(set_to_none=True)
            output = model(x_train.index_select(0, batch_indices))
            if task == "return":
                output = output.squeeze(1)
            loss = loss_function(output, y_train.index_select(0, batch_indices))
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


def _fold_metrics(
    torch,
    model,
    logistic,
    x_test,
    y_test,
    features_test,
    timestamps_test,
    task: str,
    horizon_seconds: int,
    seed: int,
):
    with torch.no_grad():
        model_output = model(x_test)
        if task == "direction":
            prediction = model_output.argmax(dim=1).cpu().numpy()
            target = y_test.cpu().numpy()
            nonflat = target != 1
            model_correct = prediction == target
            model_hit = float(np.mean(model_correct[nonflat])) if nonflat.any() else None
            overall_accuracy = float(np.mean(prediction == target))
            model_metrics = {"directional_hit_rate": model_hit, "overall_accuracy": overall_accuracy}
            target_direction = target
        else:
            prediction = model_output.squeeze(1).cpu().numpy()
            target = y_test.cpu().numpy()
            nonflat = target != 0
            model_correct = np.sign(prediction) == np.sign(target)
            model_hit = (
                float(np.mean(model_correct[nonflat])) if nonflat.any() else None
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
        expected_up = target_direction == 2
        sign_correct = np.sign(raw_imbalance) == np.where(expected_up, 1, -1)
        sign_correct &= nonflat_mask
        sign_hit_rate = float(np.mean(sign_correct[nonflat_mask])) if nonflat_mask.any() else None

        logistic_hit_rate = None
        logistic_correct = None
        if logistic is not None:
            scores = logistic(x_test).squeeze(1).sigmoid().cpu().numpy()
            logistic_prediction = scores >= 0.5
            logistic_correct = logistic_prediction == expected_up
            logistic_correct &= nonflat_mask
            logistic_hit_rate = (
                float(np.mean(logistic_correct[nonflat_mask])) if nonflat_mask.any() else None
            )

    ci = {
        "mlp_directional_hit_rate": _block_bootstrap_confidence_interval(
            timestamps_test, model_correct, nonflat_mask, horizon_seconds, seed
        ),
        "baseline_sign_imbalance_l1_hit_rate": _block_bootstrap_confidence_interval(
            timestamps_test, sign_correct, nonflat_mask, horizon_seconds, seed + 1
        ),
        "baseline_logistic_all_features_hit_rate": (
            _block_bootstrap_confidence_interval(
                timestamps_test, logistic_correct, nonflat_mask, horizon_seconds, seed + 2
            )
            if logistic_correct is not None
            else None
        ),
    }
    return {
        "mlp": model_metrics,
        "baseline_sign_imbalance_l1_hit_rate": sign_hit_rate,
        "baseline_logistic_all_features_hit_rate": logistic_hit_rate,
        "confidence_intervals_95": ci,
        "nonflat_test_rows": int(nonflat_mask.sum()),
        "test_rows": int(target_direction.size),
    }


def train(
    input_paths: list[str],
    task: str,
    output_dir: str,
    config_path: str,
    price_tick: str = "0.01",
    reference_price: str | None = None,
    break_even_move_bps: float = 20.0,
    hidden_width: int = HIDDEN_WIDTH,
) -> dict:
    if not 1 <= hidden_width <= MAXIMUM_HIDDEN_WIDTH:
        raise ValueError(f"hidden width must be between 1 and {MAXIMUM_HIDDEN_WIDTH}")
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
    dead_zone_bps = None
    if task == "direction":
        if reference_price is None:
            raise ValueError("direction training requires --reference-price to report its dead zone")
        dead_zone_bps = _validate_break_even_dead_zone(
            dataset.dead_zone_ticks, price_tick, reference_price, break_even_move_bps
        )
    elif reference_price is not None:
        dead_zone_bps = _dead_zone_basis_points(
            dataset.dead_zone_ticks, price_tick, reference_price
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
        "hidden_width": hidden_width,
        "label_dead_zone_ticks": dataset.dead_zone_ticks,
        "label_dead_zone_bps_at_reference_price": dead_zone_bps,
        "dead_zone_price_tick": str(price_tick),
        "dead_zone_reference_price": reference_price,
        "break_even_move_bps": break_even_move_bps if task == "direction" else None,
        "sample_count": int(dataset.timestamps_ns.size),
        "day_count": int(np.unique(dataset.timestamps_ns // 86_400_000_000_000).size),
        "seed": int(config["seed"]),
        "split": "expanding UTC days; purge labels across test-day boundary; shuffle training batches only",
        "confidence_interval": {
            "method": "95% percentile bootstrap over non-overlapping UTC-hour blocks",
            "block_seconds": _BLOCK_NS // 1_000_000_000,
            "replicates": _BOOTSTRAP_REPLICATES,
            "label_horizon_purged_at_each_block_end": True,
        },
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
            hidden_width,
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
            dataset.timestamps_ns[fold.test_indices],
            task,
            dataset.horizon_seconds,
            seed + 20_000 + fold_index,
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
    parser.add_argument("--price-tick", default="0.01", help="price tick as a decimal amount")
    parser.add_argument(
        "--reference-price", help="reference market price used to express the dead zone in bps"
    )
    parser.add_argument(
        "--break-even-move-bps",
        type=float,
        default=20.0,
        help="required direction-label dead zone, within 25%% (default: two 10 bps taker fees)",
    )
    parser.add_argument(
        "--hidden-width",
        type=int,
        default=HIDDEN_WIDTH,
        help="width of both hidden layers (maximum 32)",
    )
    args = parser.parse_args()
    try:
        report = train(
            args.input,
            args.task,
            args.output_dir,
            args.config,
            args.price_tick,
            args.reference_price,
            args.break_even_move_bps,
            args.hidden_width,
        )
    except (OSError, ValueError, RuntimeError) as error:
        print(f"train.py: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "folds": len(report["folds"]),
                "samples": report["sample_count"],
                "dead_zone_ticks": report["label_dead_zone_ticks"],
                "dead_zone_bps_at_reference_price": report[
                    "label_dead_zone_bps_at_reference_price"
                ],
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
