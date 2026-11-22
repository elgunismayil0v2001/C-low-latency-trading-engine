import struct
from pathlib import Path

import numpy as np
import pytest

from tickbook_data import (
    DAY_NS,
    LABEL_DIRECTION,
    LABEL_MAGIC,
    LABEL_RECORD_SIZE,
    combine_label_dumps,
    read_label_dump,
    walk_forward_folds,
)
from train import train


def write_label_dump(path, timestamps, labels, symbol=b"BTCUSDT"):
    header = struct.pack(
        "<8sIIIIIIqQ16s",
        LABEL_MAGIC,
        1,
        0x01020304,
        64,
        LABEL_RECORD_SIZE,
        LABEL_DIRECTION,
        3,
        1,
        len(timestamps),
        symbol.ljust(16, b"\0"),
    )
    with path.open("wb") as output:
        output.write(header)
        for timestamp, label in zip(timestamps, labels, strict=True):
            features = [0] * 11
            output.write(struct.pack("<qqii8qq", timestamp, *features, label))


def test_label_dump_reader_and_multi_file_ordering(tmp_path):
    day0 = [100, 200]
    day1 = [DAY_NS + 100, DAY_NS + 200]
    path0 = tmp_path / "first.tfl"
    path1 = tmp_path / "second.tfl"
    write_label_dump(path0, day0, [0, 2])
    write_label_dump(path1, day1, [1, 2])

    loaded = read_label_dump(path1, LABEL_DIRECTION)
    assert loaded.features.shape == (2, 11)
    assert loaded.labels.tolist() == [1, 2]
    assert loaded.horizon_seconds == 3
    combined = combine_label_dumps([path1, path0], LABEL_DIRECTION)
    assert combined.timestamps_ns.tolist() == day0 + day1
    assert combined.labels.tolist() == [0, 2, 1, 2]


def test_walk_forward_splits_whole_days_and_purges_cross_boundary_labels():
    per_day = [100_000_000, DAY_NS - 20_000_000_000, DAY_NS - 5_000_000_000]
    timestamps = np.array(
        [day * DAY_NS + offset for day in range(3) for offset in per_day], dtype=np.int64
    )
    folds = walk_forward_folds(timestamps, horizon_seconds=10, minimum_training_days=2)
    assert len(folds) == 1
    fold = folds[0]
    assert fold.test_indices.tolist() == [6, 7, 8]
    assert fold.train_indices.tolist() == [0, 1, 2, 3, 4]
    assert fold.purged_train_rows == 1
    assert np.all(timestamps[fold.train_indices] + 10_000_000_000 < fold.first_test_timestamp_ns)
    assert np.all(timestamps[fold.test_indices] // DAY_NS == fold.test_day)


def test_walk_forward_requires_at_least_three_days():
    timestamps = np.array([100, DAY_NS + 100], dtype=np.int64)
    with pytest.raises(ValueError, match="at least 3 UTC days"):
        walk_forward_folds(timestamps, horizon_seconds=1)


def test_training_rejects_one_day_before_importing_torch(tmp_path):
    path = tmp_path / "one-day.tfl"
    write_label_dump(path, [100, 200], [0, 2])
    config = Path(__file__).resolve().parents[1] / "config.json"
    with pytest.raises(ValueError, match="at least 3 UTC days"):
        train([str(path)], "direction", str(tmp_path / "out"), str(config))


def test_walk_forward_purges_against_midnight_even_if_first_test_row_is_later():
    timestamps = np.array(
        [100, DAY_NS + 100, 2 * DAY_NS - 5_000_000_000, 2 * DAY_NS + 20_000_000_000],
        dtype=np.int64,
    )
    fold = walk_forward_folds(timestamps, horizon_seconds=10)[0]
    assert fold.train_indices.tolist() == [0, 1]
    assert fold.purged_train_rows == 1
    assert fold.first_test_timestamp_ns == 2 * DAY_NS


def test_label_dump_rejects_count_mismatch_and_invalid_direction_class(tmp_path):
    path = tmp_path / "bad.tfl"
    write_label_dump(path, [100], [2])
    path.write_bytes(path.read_bytes()[:-1])
    with pytest.raises(ValueError, match="sample count does not match"):
        read_label_dump(path)

    write_label_dump(path, [100], [8])
    with pytest.raises(ValueError, match="direction labels"):
        read_label_dump(path)
