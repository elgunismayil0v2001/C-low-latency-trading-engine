# tickbook

C++20 tick-data replay, L2 order book and (later) backtest engine for crypto market data.
Header-only core, GoogleTest tests, CI with ASan/UBSan/TSan on GCC and Clang.

## Status

Milestone 1 (this commit): fixed-point types and parser, SPSC lock-free ring buffer,
`std::map` baseline order book, Binance diff-depth sequencer. 23 tests.

## Build and test

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j
    ctest --test-dir build --output-on-failure

With sanitizers: add `-DTICKBOOK_SANITIZE=address,undefined` or `-DTICKBOOK_SANITIZE=thread`.

## Design rules

- Prices and quantities are `int64` fixed-point. No `double` on the hot path.
- One producer thread and one consumer thread, joined by `SpscRing`.
- No heap allocation on the hot path (the `std::map` book is the baseline that breaks this on purpose).
- The sequencer rules follow the Binance spot docs at the time of writing. Re-check them.

## Roadmap

1. Milestone 2: CSV to binary converter, `mmap` replay, golden-file replay test.
2. Milestone 3: flat sorted vector and offset-array books, same interface as `BookMap`, benchmark all three.
3. Milestone 4: latency histogram, allocation counter, perf flame graph, benchmark table here.
4. Milestone 5: live recorder (WebSocket + REST snapshot), then the backtest engine.

## Limits

- This is replay processing latency on the stated hardware, not live trading latency.
- Price-level (L2) book only: Binance does not publish individual orders.
- Free sample data covers one day per month, so strategy results are not evidence of profitability.
