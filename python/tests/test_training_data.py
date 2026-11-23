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
from train import (
    _block_bootstrap_confidence_interval,
    _dead_zone_basis_points,
    _shuffled_batches,
    _validate_break_even_dead_zone,
    train,
)


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


def test_training_rejects_one_tick_dead_zone_far_below_break_even(tmp_path):
    timestamps = [100, DAY_NS + 100, 2 * DAY_NS + 100]
    path = tmp_path / "one-tick.tfl"
    write_label_dump(path, timestamps, [0, 1, 2])
    config = Path(__file__).resolve().parents[1] / "config.json"

    with pytest.raises(ValueError, match="not near the configured break-even move"):
        train(
            [str(path)],
            "direction",
            str(tmp_path / "out"),
            str(config),
            price_tick="0.01",
            reference_price="7400",
            break_even_move_bps=20.0,
        )


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


def test_minibatches_shuffle_only_the_supplied_training_rows_deterministically():
    first = list(_shuffled_batches(12, batch_size=4, seed=42))
    second = list(_shuffled_batches(12, batch_size=4, seed=42))
    order = np.concatenate(first)

    assert np.array_equal(order, np.concatenate(second))
    assert sorted(order.tolist()) == list(range(12))
    assert not np.array_equal(order, np.arange(12))
    assert all(0 <= index < 12 for batch in first for index in batch)


def test_dead_zone_is_reported_in_price_basis_points_and_checked_against_break_even():
    dead_zone_bps = _dead_zone_basis_points(1_480, "0.01", "7400")
    assert dead_zone_bps == pytest.approx(20.0)

    with pytest.raises(ValueError, match="near the configured break-even move"):
        _validate_break_even_dead_zone(1, "0.01", "7400", 20.0)
    assert _validate_break_even_dead_zone(1_480, "0.01", "7400", 20.0) == pytest.approx(20.0)


def test_hit_rate_confidence_interval_bootstraps_non_overlapping_utc_hour_blocks():
    hour_ns = 3_600_000_000_000
    timestamps = np.array(
        [
            hour * hour_ns + offset
            for hour in range(4)
            for offset in (0, 10_000_000_000, hour_ns - 2_000_000_000)
        ],
        dtype=np.int64,
    )
    eligible = np.ones(timestamps.size, dtype=bool)
    correct = np.array(
        [True, True, True, False, True, False, False, False, True, False, True, True]
    )

    first = _block_bootstrap_confidence_interval(
        timestamps, correct, eligible, horizon_seconds=3, seed=17
    )
    second = _block_bootstrap_confidence_interval(
        timestamps, correct, eligible, horizon_seconds=3, seed=17
    )

    assert first == second
    assert first["block_count"] == 4
    assert first["sample_count"] == 8
    assert 0.0 <= first["lower"] <= first["upper"] <= 1.0
    assert first["lower"] < first["upper"]
