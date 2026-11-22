# tickbook

C++20 tick-data replay, L2 order book and (later) backtest engine for crypto market data.
Header-only core, GoogleTest tests, CI with ASan/UBSan/TSan on GCC and Clang.

## Status

Milestone 1 and milestones 2.1–2.6 are complete. Milestone 2.7 tooling is implemented:
parser fuzz targets, CI gates, a local gate script, and a pinned Ubuntu toolchain. The
current AppleClang run passes 53 tests, including the synthetic allocation fixture. The
Ubuntu workflow is configured for the GCC/Clang sanitizer matrix and the Linux-only fuzz
and static-analysis gates; those hosted jobs have not yet run from this workspace.

## Build and test

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j
    ctest --test-dir build --output-on-failure

With sanitizers: add `-DTICKBOOK_SANITIZE=address,undefined` or `-DTICKBOOK_SANITIZE=thread`.
Building `csv_to_bin` requires the system zlib development package.

Run the local gate set with `scripts/check.sh`. It builds and tests plain, ASan/UBSan, and
TSan configurations, runs formatting and clang-tidy, runs Python tests when `python/tests`
exists, then builds and runs the fuzz smoke corpora. The fuzzer targets cover `parse_fixed`,
the converter's CSV record parser, binary headers/events, and `DiffSequencer`. Binary seeds
are stored as hex text and materialized only in a temporary directory.

The `alloc_check` test creates a deterministic 512-event fixture outside the repository.
It counts allocations around mapped event reads and SPSC transfers, and uses a separate
`BookMap` allocation as a positive control. `BookMap` remains exempt from the zero-allocation
assertion because it is the `std::map` baseline.

## CSV conversion

    build/csv_to_bin --book BTCUSDT-book.csv.gz --trades BTCUSDT-trades.csv.gz \
      --output BTCUSDT.bin --symbol BTCUSDT --price-scale 2 --qty-scale 8

The trade file is optional. Inputs must be capture ordered. The converter stable-merges
the rows by exchange timestamp, puts book rows before trade rows on ties, and marks the
last row of each consecutive `local_timestamp` group. It skips book rows until the first
snapshot. Event flag bit 1 marks the first row after a transition into a snapshot and tells
the consumer to clear its book before applying that level. Tardis data remains local and
is not included in the repository.

`BinaryReplay` keeps event records as views into the mapped file. On Linux it requests
`MAP_POPULATE` and locks the mapped pages; the process needs a locked-memory allowance
large enough for the replay file.

    build/replay BTCUSDT.bin

`replay` reports the event count, final best levels and depth counts, plus a deterministic
checksum over the complete final book. It clears the book when it reaches a snapshot-start
event. The replay uses a lossless producer-to-consumer SPSC ring; it yields while waiting
for space or data outside latency measurement runs. Linux pins producer and consumer to
distinct physical cores; macOS does not support this pinning path.

## Design rules

- Prices and quantities are `int64` fixed-point. No `double` on the hot path.
- One producer thread and one consumer thread, joined by `SpscRing`.
- No heap allocation on the hot path (the `std::map` book is the baseline that breaks this on purpose).
- The sequencer rules follow the Binance spot docs at the time of writing. Re-check them.

## Roadmap

1. Milestone 2.8: deterministic synthetic generator and docs.
2. Milestone 3: flat sorted-vector and offset-array books, same interface as `BookMap`.
3. Milestone 4: latency histogram, allocation measurements, flame graph and benchmark table.
4. Milestone 5: features, signal evaluation and out-of-sample baselines.
5. Milestone 6: offline labels, training and C++ inference, only if the signal evaluation warrants it.
6. Milestone 7: strategy interfaces, simulated exchange and metrics.
7. Milestone 8: manual live recorder and replay validation.

## Limits

- This is replay processing latency on the stated hardware, not live trading latency.
- Price-level (L2) book only: Binance does not publish individual orders.
- Free sample data covers one day per month, so strategy results are not evidence of profitability.
