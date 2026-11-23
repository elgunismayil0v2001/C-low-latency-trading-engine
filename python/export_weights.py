#!/usr/bin/env python3
"""Export a NumPy checkpoint to the validated little-endian C++ weights format."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys
import zlib

import numpy as np


MAGIC = b"TICKNN01"
VERSION = 1
ENDIANNESS = 0x01020304
FEATURE_COUNT = 11
HIDDEN_WIDTH = 16
MAXIMUM_HIDDEN_WIDTH = 32
ACTIVATION_LINEAR = 0
ACTIVATION_RELU = 1


def encode_weights(
    input_mean: np.ndarray,
    input_standard_deviation: np.ndarray,
    layers: list[tuple[np.ndarray, np.ndarray, int]],
) -> bytes:
    mean = np.asarray(input_mean, dtype=np.dtype("<f4"))
    standard_deviation = np.asarray(input_standard_deviation, dtype=np.dtype("<f4"))
    if mean.shape != (FEATURE_COUNT,) or standard_deviation.shape != (FEATURE_COUNT,):
        raise ValueError("normalization arrays must contain 11 values")
    if not np.isfinite(mean).all() or not np.isfinite(standard_deviation).all():
        raise ValueError("normalization values must be finite")
    if np.any(standard_deviation <= 0):
        raise ValueError("input standard deviations must be positive")
    if len(layers) != 3:
        raise ValueError("weights must have exactly three affine layers")

    expected_input = FEATURE_COUNT
    hidden_width = 0
    descriptors: list[tuple[int, int, int]] = []
    encoded_layers: list[tuple[np.ndarray, np.ndarray]] = []
    for layer_index, (weights_value, biases_value, activation) in enumerate(layers):
        weights = np.asarray(weights_value, dtype=np.dtype("<f4"))
        biases = np.asarray(biases_value, dtype=np.dtype("<f4"))
        if weights.ndim != 2 or weights.shape[1] != expected_input:
            raise ValueError(f"layer {layer_index} weight dimensions are invalid")
        output_width, input_width = weights.shape
        if biases.shape != (output_width,):
            raise ValueError(f"layer {layer_index} bias dimensions are invalid")
        if layer_index == 0:
            hidden_width = output_width
            if not 1 <= hidden_width <= MAXIMUM_HIDDEN_WIDTH:
                raise ValueError(f"hidden width must be between 1 and {MAXIMUM_HIDDEN_WIDTH}")
        if layer_index < 2 and output_width != hidden_width:
            raise ValueError(f"the two hidden layers must have width {hidden_width}")
        if layer_index == 2 and output_width not in (1, 3):
            raise ValueError("the output layer must have width 1 or 3")
        expected_activation = ACTIVATION_RELU if layer_index < 2 else ACTIVATION_LINEAR
        if activation != expected_activation:
            raise ValueError("hidden layers must use ReLU and the output must be linear")
        if not np.isfinite(weights).all() or not np.isfinite(biases).all():
            raise ValueError("weights and biases must be finite")
        descriptors.append((input_width, output_width, activation))
        encoded_layers.append((weights, biases))
        expected_input = output_width

    payload = bytearray(struct.pack("<8sIII", MAGIC, VERSION, ENDIANNESS, len(layers)))
    for input_width, output_width, activation in descriptors:
        payload.extend(struct.pack("<III", input_width, output_width, activation))
    payload.extend(mean.tobytes(order="C"))
    payload.extend(standard_deviation.tobytes(order="C"))
    for weights, biases in encoded_layers:
        payload.extend(weights.tobytes(order="C"))
        payload.extend(biases.tobytes(order="C"))
    payload.extend(struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF))
    return bytes(payload)


def export_checkpoint(checkpoint_path: str | Path, output_path: str | Path) -> None:
    with np.load(checkpoint_path, allow_pickle=False) as checkpoint:
        layers = []
        for index in range(3):
            activation = ACTIVATION_RELU if index < 2 else ACTIVATION_LINEAR
            layers.append(
                (
                    checkpoint[f"layer{index}_weights"],
                    checkpoint[f"layer{index}_biases"],
                    activation,
                )
            )
        blob = encode_weights(checkpoint["input_mean"], checkpoint["input_std"], layers)
    destination = Path(output_path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_bytes(blob)
    temporary.replace(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", help="checkpoint.npz written by train.py")
    parser.add_argument("output", help="destination weights.bin")
    args = parser.parse_args()
    try:
        export_checkpoint(args.checkpoint, args.output)
    except (OSError, ValueError, KeyError) as error:
        print(f"export_weights.py: {error}", file=sys.stderr)
        return 1
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
