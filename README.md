# tickbook

C++20 tick-data replay, L2 order book and (later) backtest engine for crypto market data.
Header-only core, GoogleTest tests, CI with ASan/UBSan/TSan on GCC and Clang.

## Status

Milestone 1 and milestones 2.1–2.5 are complete: diff-depth messages sequence and resync
correctly; `csv_to_bin` streams and merges Tardis gzip CSVs; and `BinaryReplay` validates
and memory-maps the resulting event files, exposing zero-copy views. Parser edge cases,
invalid book quantities, message boundaries, snapshot restarts, and mapped-file validation
are covered. 45 tests pass.

## Build and test

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j
    ctest --test-dir build --output-on-failure

With sanitizers: add `-DTICKBOOK_SANITIZE=address,undefined` or `-DTICKBOOK_SANITIZE=thread`.
Building `csv_to_bin` requires the system zlib development package.

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

## Design rules

- Prices and quantities are `int64` fixed-point. No `double` on the hot path.
- One producer thread and one consumer thread, joined by `SpscRing`.
- No heap allocation on the hot path (the `std::map` book is the baseline that breaks this on purpose).
- The sequencer rules follow the Binance spot docs at the time of writing. Re-check them.

## Roadmap

1. Milestone 2.6–2.8: threaded replay, golden-file tests, CI build-out, synthetic generator and docs.
2. Milestone 3: flat sorted vector and offset-array books, same interface as `BookMap`, benchmark all three.
3. Milestone 4: latency histogram, allocation counter, perf flame graph, benchmark table here.
4. Milestone 5: live recorder (WebSocket + REST snapshot), then the backtest engine.

## Limits

- This is replay processing latency on the stated hardware, not live trading latency.
- Price-level (L2) book only: Binance does not publish individual orders.
- Free sample data covers one day per month, so strategy results are not evidence of profitability.
