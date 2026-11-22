# tickbook

C++20 tick-data replay, L2 order book and (later) backtest engine for crypto market data.
Header-only core, GoogleTest tests, CI with ASan/UBSan/TSan on GCC and Clang.

## Status

Milestones 1, 2.1–2.6, 2.8, 3.1–3.3, 4.2, 4.4, and 5.1–5.3 are complete. Milestone 4.1 adds a Release-only
per-event replay histogram; its Linux zero-minor-fault assertion still needs a Linux run. The
milestone 4.3 five-run comparison is recorded below; its Linux `perf` flame graph remains pending.
`compare_books` matched `BookMap`,
`FlatBook`, and `OffsetArrayBook` after every event in the 2019-12-01 Binance BTCUSDT replay
(6,907,104 events). Milestone 2.7 tooling is implemented: parser fuzz targets, CI gates, a
local gate script, and a pinned Ubuntu toolchain. Current AppleClang plain, ASan/UBSan, and
TSan runs each pass all 86 tests. The new milestone 5 utility sources pass targeted clang-tidy;
the project-wide static script still reports existing warning-as-error findings in unrelated
book, sequencing and converter code. All four 60-second parser fuzz smoke targets pass locally
with LLVM 23; the Ubuntu workflow jobs have not run here.

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

## Synthetic data

`gen_synthetic` writes deterministic `incremental_book_L2.csv.gz` and `trades.csv.gz`
files in Tardis format. It generates a five-level snapshot, seeded random-walk updates,
level removals, and trades. Keep generated files outside the repository.

    build/gen_synthetic --output-dir /tmp/tickbook-synthetic --seed 42 --messages 100
    build/csv_to_bin --book /tmp/tickbook-synthetic/incremental_book_L2.csv.gz \
      --trades /tmp/tickbook-synthetic/trades.csv.gz --output /tmp/tickbook-synthetic/events.bin \
      --symbol BTCUSDT --price-scale 2 --qty-scale 8
    build/replay /tmp/tickbook-synthetic/events.bin

## CSV conversion

    build/csv_to_bin --book BTCUSDT-book.csv.gz --trades BTCUSDT-trades.csv.gz \
      --output BTCUSDT.bin --symbol BTCUSDT --price-scale 2 --qty-scale 8

The trade file is optional. The converter preserves capture order within each input stream.
It clamps each stream's event time to its running maximum, then stable-merges by that clamped
time, with book rows before trade rows on ties. It counts timestamp backsteps and reports
the largest; a single backstep above `--max-backstep-ms` (default 1000) aborts conversion.
The last row of each consecutive `local_timestamp` group receives the end-of-message flag.
It skips book rows until the first snapshot. Event flag bit 1 marks the first row after a
transition into a snapshot and tells the consumer to clear its book before applying that
level. Tardis data remains local and is not included in the repository.

`BinaryReplay` keeps event records as views into the mapped file. On Linux it requests
`MAP_POPULATE` and locks the mapped pages; the process needs a locked-memory allowance
large enough for the replay file.

    build/replay BTCUSDT.bin

`replay` reports the event count, final best levels and depth counts, plus a deterministic
checksum over the complete final book. It clears the book when it reaches a snapshot-start
event. The replay uses a lossless producer-to-consumer SPSC ring; it yields while waiting
for space or data outside latency measurement runs. Linux pins producer and consumer to
distinct physical cores; macOS does not support this pinning path.

## Latency histogram (milestone 4.1)

Build the Release-only benchmark separately from sanitizer builds:

    cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
      -DTICKBOOK_BUILD_BENCHMARKS=ON -DBUILD_TESTING=OFF
    cmake --build build-bench --target replay_latency -j
    build-bench/replay_latency BTCUSDT.bin --book map --price-tick 1

`--book` accepts `map`, `flat`, or `offset`. The tool maps and pre-touches the event file,
does one full warm-up replay, then times each successful ring pop through snapshot handling
and book apply. It subtracts the average back-to-back `steady_clock` read overhead. The fixed
histogram has exact 0–31 ns buckets and 32 sub-buckets per power of two above that; reported
percentiles are the selected bucket's upper edge and `max_ns` is exact. Linux pins both threads,
uses the replay mapping's `MAP_POPULATE`/`mlock` path, and fails if process minor faults are
nonzero. macOS cannot pin threads; it reports process-wide minor faults without asserting zero.

One measured `BookMap` pass over BTCUSDT 2019-12-01 on the Apple M2 MacBook Air (macOS 27.2.1),
Apple LLVM 21.0.0, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Werror`, covered 6,907,104
events after one full warm-up pass. It reported p50 26 ns, p99 109 ns, p99.9 151 ns, max
61,734 ns, a 16 ns timer-overhead estimate, and 3 process-wide minor faults. This is the initial
single BookMap run; the five-run layout comparison appears below. These are replay processing
numbers, not trading latency.

### Five-run replay comparison (milestone 4.3)

Each cell shows the median of five per-run measurements and the min–max run range. All runs used
the same Binance BTCUSDT 2019-12-01 binary (6,907,104 events), one full warm-up before measurement,
and 30 seconds between runs. Layouts ran interleaved. Hardware: Apple M2 MacBook Air, 8 GB, macOS
27.2.1; compiler: Apple LLVM 21.0.0; flags: `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow
-Werror`. These are replay ring-pop-to-book-apply latencies after subtracting the measured timer
overhead, in nanoseconds. They are not trading latency.

| Book layout | p50 ns | p99 ns | p99.9 ns | Max ns |
| --- | ---: | ---: | ---: | ---: |
| `BookMap` | 28 (27–61) | 111 (111–231) | 155 (155–991) | 54,527 (11,069–3,124,728) |
| `FlatBook` | 1,055 (1,055–1,087) | 2,111 (2,111–3,007) | 2,495 (2,175–16,383) | 5,262,236 (82,901–73,262,861) |
| `OffsetArrayBook` | 28 (26–28) | 28 (28–109) | 111 (111–195) | 789,570 (753,527–1,832,942) |

All five OffsetArrayBook runs had zero measured hot-path allocations. All five FlatBook runs had
zero; BookMap allocated 2,671,052 times per run. macOS reports process-scoped minor faults rather
than asserting zero: run counts ranged 1–10 for BookMap, 1–24 for FlatBook, and 0–2 for
OffsetArrayBook. The fanless M2 showed substantial max-latency and fault-count spread, so the
percentile ranges are shown and the exact maxima are not used to set the budget. The Linux x86
comparison and flame graph remain needed for the authoritative pinned-core profile.

To capture that profile on Linux with `perf` and Brendan Gregg's FlameGraph scripts available on
`PATH`, build the benchmark with debug symbols and run the best measured layout:

    cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DTICKBOOK_BUILD_BENCHMARKS=ON -DBUILD_TESTING=OFF
    cmake --build build-profile --target replay_latency -j
    scripts/profile-replay-linux.sh /path/to/BTCUSDT.bin docs/replay-offset.svg 1

The script records the OffsetArrayBook replay with `perf record`, folds stacks, and writes the SVG.
The profile is diagnostic; use unprofiled runs for the benchmark table. The current Apple Silicon
workspace has no Linux `perf`, Docker, or Instruments command-line tools, so no flame graph is
claimed here.

## Per-event latency budget (milestone 4.4)

The target is **p99 <= 500 ns/event** for ring pop, book update, feature update, and signal
evaluation; NN inference shares this total. The budget is based on the OffsetArrayBook baseline
measured on the Apple M2 MacBook Air, macOS 27.2.1: median p99 28 ns and median p99.9 111 ns over
five runs. The 500 ns target is about 4.5 times that measured p99.9, leaving room for later stages.
The original book-only baseline measures ring pop through book apply. The feature-path measurement
below extends it through feature update; signal evaluation and NN inference still need to be added
before the full budget is verified. Recheck the target against pinned Linux x86 results when available.

The five-run feature-path measurement below now includes the `FeatureEngine`: its median p99 was
407 ns (run range 407–447 ns), leaving 53 ns under the p99 target for the signal stage at the
worst observed p99. The measured path still excludes signal evaluation and NN inference.

### Feature path measurement (milestone 5)

`replay_latency --features` updates the same incremental feature engine after every book event,
emitting a feature snapshot at each message boundary inside the measured loop. Warm-up also runs
the feature engine, and every run reports allocation and process minor-fault counts. Five measured
runs over BTCUSDT 2019-12-01 (6,907,104 events, 1,236,542 messages) used one complete warm-up and
30 seconds between runs. Hardware: Apple M2 MacBook Air, 8 GB, macOS 27.2.1; compiler: Apple LLVM
21.0.0; flags: `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Werror`. Percentiles are replay
ring-pop-through-book-and-feature processing times, with the measured timer overhead subtracted.

| Book and feature path | p50 ns | p99 ns | p99.9 ns | Max ns |
| --- | ---: | ---: | ---: | ---: |
| `OffsetArrayBook` + `FeatureEngine` | 27 (27–27) | 407 (407–447) | 575 (527–735) | 775,277 (764,943–1,880,027) |

The earlier book-only five-run table recorded p99 28 ns (28–109) and p99.9 111 ns (111–195).
Adding feature updates raised these to p99 407 ns (407–447) and p99.9 575 ns (527–735).
All five runs recorded zero hot-path allocations. Process-wide minor faults ranged from 0 to 2;
macOS does not provide the Linux zero-fault assertion or core pinning. This one-machine result is
not the authoritative pinned-core measurement, and the measured path does not yet include signal
calculation or inference.

### Feature generation and signal evaluation (milestone 5)

The `FeatureEngine` is a fixed-point, incremental update after `OrderBook::apply`. It emits only
after the event carrying the end-of-message flag. Imbalances, microprice offsets and returns use
Q20 (`1 << 20`) with truncation toward zero; spread is in integer ticks, and OFI/trade flow remain
in fixed-point quantity units. Defaults are top K=5, OFI windows 10/50/200 messages and time
windows 100 ms/1 s. Its time-window ring is allocated at construction; the default capacity is
262,144 messages. It rejects non-monotone event timestamps and reports capacity exhaustion instead
of allocating on the hot path.

`dump_features` samples at the first message at or after each 100 ms event-time grid point. Its
versioned little-endian `.tbf` file stores Q20 feature records and forward mid returns at 1, 3 and
10 seconds; each label uses the first later grid sample at or after the horizon. Incomplete final
labels are dropped. The dump command streams the binary event input and keeps its output temporary
until a successful completion. `eval_signals` validates the dump, reports per-feature Spearman IC
and hit rate, and evaluates the sign of `imbalance_l1` plus expanding walk-forward logistic
regression on all features. The logistic fit uses training-only normalization and purges training
labels whose horizon reaches the first test timestamp; the three chronological splits do not
shuffle rows. The IC and hit-rate results across 1/3/10 seconds form the decay series; output also
includes hourly blocks.

    build/dump_features BTCUSDT.bin BTCUSDT-features.tbf --price-tick 1
    build/eval_signals BTCUSDT-features.tbf > BTCUSDT-evaluation.csv

On the local Binance BTCUSDT 2019-12-01 sample, the dump contained 820,028 complete samples.
Walk-forward evaluation tested 410,014 rows per horizon across three folds. The overall results
were:

| Horizon | Baseline | Spearman IC | Direction hit rate |
| ---: | --- | ---: | ---: |
| 1 s | sign of `imbalance_l1` | 0.165274 | 56.6571% |
| 1 s | logistic regression | 0.426776 | 67.1497% |
| 3 s | sign of `imbalance_l1` | 0.151228 | 55.6262% |
| 3 s | logistic regression | 0.376478 | 64.9349% |
| 10 s | sign of `imbalance_l1` | 0.112980 | 54.0027% |
| 10 s | logistic regression | 0.253718 | 60.4877% |

The logistic baseline's hit rate exceeded 50% in all 13 reported hourly blocks at each horizon;
block ranges were 59.03–73.03% at 1 s, 57.09–68.20% at 3 s, and 55.67–67.63% at 10 s. These are
preliminary results from one day, not evidence of a persistent edge or profitability; fees,
execution and regime variation are not modeled. The positive single-day result is enough to
continue to multi-day evaluation, but not to claim a strategy works.

## Allocation counter (milestone 4.2)

The Release replay benchmark replaces global `operator new` and counts allocations from the
startup-done marker through producer and consumer completion. Flat and offset book runs fail on
any hot-path allocation. The `BookMap` run must record at least one allocation as a positive
control. The CI allocation job runs the benchmark on the same small deterministic fixture used by
`alloc_check`.

## Order book layouts

`BookMap` is the full-depth reference. `FlatBook` stores the best 4,096 levels per side by
default. At capacity it drops the worst retained level when a better one arrives and ignores
updates below its retained range. It reserves its vectors at construction. `OffsetArrayBook`
uses a fixed window of 65,536 price ticks by default; pass the fixed-point tick size as the
second constructor argument when it is greater than one. Its circular storage slides the
window to include out-of-range updates and drops levels that leave the window. Both optimized
books avoid allocations during updates.

`compare_books` pre-scans a binary file to size both bounded books for the observed price
range, then compares best levels, counts, crossed state, and top five levels after every event.
The binary header does not contain the exchange tick size, so pass it as an integer number of
fixed-point price units when it is greater than one (the default is one):

    build/compare_books /path/to/day.bin --price-tick 1

The validator rejects files whose price span exceeds six million ticks.

On the final BookMap state from BTCUSDT 2019-12-01 (3,415 bid levels, 3,783 ask levels),
the old full traversal averaged 51,832 ns per bid+ask top-five query (run range 46,388–53,535 ns);
stopping each visitor after five levels averaged 7.02 ns (6.59–9.81 ns). Each median is
from seven runs of 20,000 queries. Hardware: Apple M2 MacBook Air, macOS 27.2.1. Compiler:
Apple LLVM 21.0.0, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Werror`. This is a
batch-average visitor microbenchmark with the book already in memory, not replay event latency;
runs were back-to-back without a cool-down.

### Preliminary book-apply microbenchmark

This measures an in-memory event loop, not end-to-end replay latency. Each value is the
median elapsed batch time divided by 150,010 events across seven runs; the ranges show the
minimum and maximum run averages. The same seeded synthetic BTCUSDT file (seed 4242,
50,000 messages) was used for each layout. Hardware: Apple M2 MacBook Air, macOS 27.2.1.
Compiler: Apple LLVM 21.0.0, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Werror`.
No minor faults were observed in the measured loops. These are batch averages, not per-event
tail percentiles; milestone 4 adds the latency histogram. Runs were back-to-back without a
cool-down, so treat the ranges as exploratory on the fanless M2 Air.

| Layout | Median ns/event | Run range ns/event |
| --- | ---: | ---: |
| `BookMap` | 8.65 | 7.86–9.82 |
| `FlatBook` | 9.85 | 9.46–10.81 |
| `OffsetArrayBook` (circular recentering) | 3.39 | 3.24–3.42 |
| `OffsetArrayBook` before circular recentering | 23.62 | 21.59–31.56 |

The circular recentering change reduced the offset book's median on this fixture by about
86%. `FlatBook` was slower than `BookMap` on this shallow synthetic stream, so these numbers
do not establish a general speedup for the flat layout.

## Design rules

- Prices and quantities are `int64` fixed-point. No `double` on the hot path.
- One producer thread and one consumer thread, joined by `SpscRing`.
- No heap allocation on the hot path (the `std::map` book is the baseline that breaks this on purpose).
- The sequencer rules follow the Binance spot docs at the time of writing. Re-check them.

## Architecture

![tickbook architecture](docs/architecture.png)

## Roadmap

1. Milestones 3.1–3.3, 4.2, 4.4, and 5.1–5.3 complete; milestone 4.1 implemented, with a Linux zero-fault run pending.
2. Milestone 4.3: five-run Mac comparison complete; Linux x86 comparison and `perf` flame graph pending.
3. Milestone 5: feature engine, versioned feature dump, and walk-forward evaluation complete; validate across more days before model work.
4. Milestone 6: offline labels, training and C++ inference, only after multi-day evaluation supports continuing.
5. Milestone 7: strategy interfaces, simulated exchange and metrics.
6. Milestone 8: manual live recorder and replay validation.

See [AGENTS.md](AGENTS.md) for the full project brief, milestone checklist, and correctness gates.

## Limits

- This is replay processing latency on the stated hardware, not live trading latency.
- Price-level (L2) book only: Binance does not publish individual orders.
- Free sample data covers one day per month, so strategy results are not evidence of profitability.
