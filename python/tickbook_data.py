"""Readers and chronological split helpers for tickbook's versioned offline dumps."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct

import numpy as np


FEATURE_COUNT = 11
FEATURE_SCALE = 1 << 20
DAY_NS = 86_400_000_000_000
LABEL_MAGIC = b"TBLABEL1"
LABEL_VERSION = 1
LABEL_ENDIANNESS = 0x01020304
LABEL_HEADER_SIZE = 64
LABEL_RECORD_SIZE = 96
LABEL_DIRECTION = 1
LABEL_RETURN = 2

_RECORD_DTYPE = np.dtype(
    [
        ("ts_ns", "<i8"),
        ("spread_ticks", "<i8"),
        ("imbalance_l1_q20", "<i4"),
        ("imbalance_k_q20", "<i4"),
        ("microprice_offset_q20_ticks", "<i8"),
        ("ofi_10", "<i8"),
        ("ofi_50", "<i8"),
        ("ofi_200", "<i8"),
        ("trade_flow_100ms", "<i8"),
        ("trade_flow_1s", "<i8"),
        ("mid_return_100ms_q20_ticks", "<i8"),
        ("mid_return_1s_q20_ticks", "<i8"),
        ("label", "<i8"),
    ],
    align=False,
)


@dataclass(frozen=True)
class LabelDump:
    features: np.ndarray
    labels: np.ndarray
    timestamps_ns: np.ndarray
    kind: int
    horizon_seconds: int
    dead_zone_ticks: int
    symbol: str


@dataclass(frozen=True)
class WalkForwardFold:
    train_indices: np.ndarray
    test_indices: np.ndarray
    purged_train_rows: int
    first_test_timestamp_ns: int
    test_day: int


def read_label_dump(path: str | Path, expected_kind: int | None = None) -> LabelDump:
    source = Path(path)
    file_size = source.stat().st_size
    if file_size < LABEL_HEADER_SIZE:
        raise ValueError(f"{source}: label dump header is truncated")
    with source.open("rb") as input_file:
        header = input_file.read(LABEL_HEADER_SIZE)
    if header[:8] != LABEL_MAGIC:
        raise ValueError(f"{source}: label dump magic is invalid")
    version, endian, header_size, record_size, kind, horizon = struct.unpack_from("<6I", header, 8)
    dead_zone, sample_count = struct.unpack_from("<qQ", header, 32)
    if version != LABEL_VERSION:
        raise ValueError(f"{source}: unsupported label dump version {version}")
    if endian != LABEL_ENDIANNESS:
        raise ValueError(f"{source}: label dump endianness is invalid")
    if header_size != LABEL_HEADER_SIZE or record_size != LABEL_RECORD_SIZE:
        raise ValueError(f"{source}: label dump record layout is unsupported")
    if kind not in (LABEL_DIRECTION, LABEL_RETURN):
        raise ValueError(f"{source}: unknown label kind {kind}")
    if expected_kind is not None and kind != expected_kind:
        raise ValueError(f"{source}: expected label kind {expected_kind}, got {kind}")
    if horizon not in (1, 3, 10):
        raise ValueError(f"{source}: unsupported horizon {horizon}")
    if (kind == LABEL_DIRECTION and dead_zone < 1) or (kind == LABEL_RETURN and dead_zone != 0):
        raise ValueError(f"{source}: invalid dead-zone metadata")
    expected_size = LABEL_HEADER_SIZE + sample_count * LABEL_RECORD_SIZE
    if expected_size != file_size:
        raise ValueError(f"{source}: sample count does not match file length")

    try:
        symbol = header[48:64].split(b"\0", 1)[0].decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError(f"{source}: symbol is not ASCII") from error
    if sample_count:
        rows = np.memmap(
            source,
            dtype=_RECORD_DTYPE,
            mode="r",
            offset=LABEL_HEADER_SIZE,
            shape=(sample_count,),
        )
    else:
        rows = np.empty((0,), dtype=_RECORD_DTYPE)
    timestamps = rows["ts_ns"].copy()
    if timestamps.size and (timestamps[0] < 0 or np.any(timestamps[1:] <= timestamps[:-1])):
        raise ValueError(f"{source}: timestamps are negative or not strictly increasing")
    features = np.empty((sample_count, FEATURE_COUNT), dtype=np.int64)
    feature_names = _RECORD_DTYPE.names[1:-1]
    for column, name in enumerate(feature_names):
        features[:, column] = rows[name]
    labels = rows["label"].copy()
    if kind == LABEL_DIRECTION and np.any((labels < 0) | (labels > 2)):
        raise ValueError(f"{source}: direction labels must be down=0, flat=1 or up=2")
    return LabelDump(features, labels, timestamps, kind, horizon, dead_zone, symbol)


def combine_label_dumps(paths: list[str | Path], expected_kind: int) -> LabelDump:
    if not paths:
        raise ValueError("at least one labelled dump is required")
    parts = [read_label_dump(path, expected_kind) for path in paths]
    first = parts[0]
    for part in parts[1:]:
        if (part.kind, part.horizon_seconds, part.dead_zone_ticks, part.symbol) != (
            first.kind,
            first.horizon_seconds,
            first.dead_zone_ticks,
            first.symbol,
        ):
            raise ValueError("label dumps must agree on kind, horizon, dead zone and symbol")
    timestamps = np.concatenate([part.timestamps_ns for part in parts])
    features = np.concatenate([part.features for part in parts])
    labels = np.concatenate([part.labels for part in parts])
    order = np.argsort(timestamps, kind="stable")
    timestamps = timestamps[order]
    if timestamps.size and np.any(timestamps[1:] <= timestamps[:-1]):
        raise ValueError("combined label dumps contain duplicate or non-increasing timestamps")
    return LabelDump(
        features[order], labels[order], timestamps, first.kind, first.horizon_seconds,
        first.dead_zone_ticks, first.symbol
    )


def walk_forward_folds(
    timestamps_ns: np.ndarray,
    horizon_seconds: int,
    minimum_training_days: int = 2,
) -> list[WalkForwardFold]:
    """Use each later UTC day as a test fold; purge labels reaching UTC midnight."""
    if timestamps_ns.ndim != 1 or timestamps_ns.size == 0:
        raise ValueError("timestamps must be a non-empty one-dimensional array")
    if np.any(timestamps_ns[1:] <= timestamps_ns[:-1]) or timestamps_ns[0] < 0:
        raise ValueError("timestamps must be non-negative and strictly increasing")
    if horizon_seconds not in (1, 3, 10):
        raise ValueError("horizon must be 1, 3 or 10 seconds")
    days = timestamps_ns // DAY_NS
    unique_days = np.unique(days)
    if unique_days.size <= minimum_training_days:
        raise ValueError(
            f"walk-forward training needs at least {minimum_training_days + 1} UTC days; "
            f"found {unique_days.size}"
        )

    horizon_ns = horizon_seconds * 1_000_000_000
    folds: list[WalkForwardFold] = []
    for day_index in range(minimum_training_days, unique_days.size):
        test_day = int(unique_days[day_index])
        test_indices = np.flatnonzero(days == test_day)
        candidates = np.flatnonzero(days < test_day)
        split_timestamp = test_day * DAY_NS
        if split_timestamp < horizon_ns:
            keep = np.zeros(candidates.size, dtype=bool)
        else:
            keep = timestamps_ns[candidates] < split_timestamp - horizon_ns
        train_indices = candidates[keep]
        if train_indices.size == 0:
            continue
        folds.append(
            WalkForwardFold(
                train_indices,
                test_indices,
                int(candidates.size - train_indices.size),
                split_timestamp,
                test_day,
            )
        )
    if not folds:
        raise ValueError("walk-forward splits produced no train/test folds after purging")
    return folds
