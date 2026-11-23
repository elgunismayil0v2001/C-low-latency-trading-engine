# tickbook

C++20 tick-data replay, L2 order book and backtest engine for crypto market data.
Header-only core, GoogleTest tests, CI with ASan/UBSan/TSan on GCC and Clang.

## Status

Milestones 1, 2.1–2.6, 2.8, 3.1–3.3, 4.2, 4.4, 5.1–5.3, 6.1–6.6, and 7.1–7.3 are implemented. Milestone 4.1 adds a Release-only
per-event replay histogram; its Linux zero-minor-fault assertion still needs a Linux run. The
milestone 4.3 five-run comparison is recorded below; its Linux `perf` flame graph remains pending.
`OffsetArrayBook` starts with a 65,536-tick window. When the best bid enters the upper quarter or
the best ask enters the lower quarter, it shifts the window by half its width and preserves every
level in the overlap; levels outside the new window are discarded. An isolated far-away update does
not trigger a shift. The 2019-12-01 `compare_books` run matched top five for all 6,907,104 events.
A 24-hour synthetic path moving BTCUSDT from $40,000 to $41,200 also matched top five for all
57,620 events with the default window. Milestone 2.7 tooling is implemented: parser fuzz targets,
CI gates, a local gate script, and a pinned Ubuntu toolchain. AppleClang plain, ASan/UBSan, and TSan
builds with warnings-as-errors each pass all 114 tests. The static job passes with clang-format-18
and clang-tidy-18; the script excludes three noisy checks documented at the call site. All five
60-second parser and weights-loader fuzz smoke targets pass locally. The Ubuntu workflow jobs have
not run here. After installing the training packages, the local Python suite passes all 16 tests
when the parity executable is built. Three monthly sample days now support one walk-forward test
fold; its results are reported below and are not enough to claim a stable edge.

## Build and test

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j
    ctest --test-dir build --output-on-failure

With sanitizers: add `-DTICKBOOK_SANITIZE=address,undefined` or `-DTICKBOOK_SANITIZE=thread`.
Building `csv_to_bin` requires the system zlib development package.

Run the local gate set with `scripts/check.sh`. It builds and tests plain, ASan/UBSan, and
TSan configurations, runs formatting and clang-tidy, runs Python tests when `python/tests`
exists, then builds and runs the fuzz smoke corpora. The fuzzer targets cover `parse_fixed`,
the converter's CSV record parser, binary headers/events, `DiffSequencer`, and weights loading.
Binary seeds are stored as hex text and materialized only in a temporary directory.

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
`MAP_POPULATE`; locking the mapped pages is optional and off by default, so normal replay
does not need a large locked-memory allowance.

    build/replay BTCUSDT.bin

Pass `--lock-pages` to request `mlock`, or `--pin-threads` to pin the producer and consumer
to separate physical cores on Linux. Both options are opt-in; replay without them works when
the process cannot lock the whole file or only one CPU is available.

`replay` reports the event count, final best levels and depth counts, plus a deterministic
checksum over the complete final book. It clears the book when it reaches a snapshot-start
event. The replay uses a lossless producer-to-consumer SPSC ring; it yields while waiting
for space or data outside latency measurement runs. macOS does not support the Linux pinning
option.

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
percentiles are the selected bucket's upper edge and `max_ns` is exact. Page locking and Linux
thread pinning are opt-in with `--lock-pages` and `--pin-threads`; default runs do not require
either system resource. Linux fails if process minor faults are nonzero. macOS cannot pin
threads; it reports process-wide minor faults without asserting zero.

One measured `BookMap` pass over BTCUSDT 2019-12-01 on the Apple M2 MacBook Air (macOS 27.2.1),
Apple LLVM 21.0.0, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Werror`, covered 6,907,104
events after one full warm-up pass. It reported p50 26 ns, p99 109 ns, p99.9 151 ns, max
61,734 ns, a 16 ns timer-overhead estimate, and 3 process-wide minor faults. This is the initial
single BookMap run; the five-run layout comparison appears below. These are replay processing
numbers, not trading latency.

### Five-run replay comparison (milestone 4.3)

Each cell shows the median of five per-run measurements and the min–max run range. All runs used
the same Binance BTCUSDT 2019-12-01 binary (6,907,104 events), with one full warm-up before
measurement and 30 seconds between runs. The BookMap and FlatBook rows are unchanged layout
baselines; OffsetArrayBook was remeasured with edge-triggered recentering.
Hardware: Apple M2 MacBook Air, 8 GB, macOS
27.2.1; compiler: Apple LLVM 21.0.0; flags: `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow
-Werror`. These are replay ring-pop-to-book-apply latencies after subtracting the measured timer
overhead, in nanoseconds. They are not trading latency.

| Book layout | p50 ns | p99 ns | p99.9 ns | Max ns |
| --- | ---: | ---: | ---: | ---: |
| `BookMap` | 28 (27–61) | 111 (111–231) | 155 (155–991) | 54,527 (11,069–3,124,728) |
| `FlatBook` | 1,055 (1,055–1,087) | 2,111 (2,111–3,007) | 2,495 (2,175–16,383) | 5,262,236 (82,901–73,262,861) |
| `OffsetArrayBook` | 26 (25–27) | 67 (26–69) | 151 (69–155) | 302,359 (81,853–28,712,652) |

The current OffsetArrayBook row includes edge-triggered recentering. Recenter copies and recounts
the retained overlap, so its rare shifts raise the tail percentiles; the five runs still measured
zero hot-path allocations. All five FlatBook runs had zero; BookMap allocated 2,671,052 times per
run. macOS reports process-scoped minor faults rather than asserting zero: run counts ranged 1–10
for BookMap, 1–24 for FlatBook, and 0–2 for OffsetArrayBook. The fanless M2 showed substantial
max-latency and fault-count spread, so percentile ranges are shown and exact maxima are not used
to set the budget. The Linux x86 comparison and flame graph remain needed for the authoritative
pinned-core profile.

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
evaluation; NN inference shares this total. The recentered OffsetArrayBook baseline measured on
the Apple M2 MacBook Air, macOS 27.2.1, had median p99 67 ns and median p99.9 151 ns over five
runs. The 500 ns target is about 3.3 times that measured p99.9, leaving room for later stages.
The book-only measurement runs from ring pop through book apply. The feature-path measurement
below extends it through feature update; signal evaluation and NN inference still need to be added
before the full budget is verified. Recheck the target against pinned Linux x86 results when available.

The five-run feature-path measurement below includes the `FeatureEngine`: its median p99 was
319 ns (run range 319–407 ns), leaving 93 ns under the p99 target at the worst observed p99.
Milestone 6 adds measured NN inference to this path; its results appear below.

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
| `OffsetArrayBook` + `FeatureEngine` | 27 (26–28) | 319 (319–407) | 447 (447–703) | 260,986 (88,360–13,798,526) |

The book-only five-run median was p99 67 ns and p99.9 151 ns. Adding feature updates raised
these to p99 319 ns and p99.9 447 ns. All five runs recorded zero hot-path allocations.
Process-wide minor faults ranged from 0 to 1. macOS does not provide the Linux zero-fault assertion
or core pinning. This one-machine result is
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

## Labeling, training and inference (milestone 6)

`label_features` streams a `.tbf` feature dump into a versioned `.tfl` file. Direction labels are
down=0, flat=1, up=2, with `abs(return_ticks) < k` as the flat class; return
labels store the forward return in integer ticks. The label dump retains the timestamp and all 11
features. Its header records symbol, horizon, label kind and dead-zone value.

`python/train.py` combines labelled dumps in timestamp order and uses expanding walk-forward folds
on whole UTC days. It purges training rows whose forward-label horizon reaches the test-day
boundary, then shuffles mini-batches within that fold's training rows only. Direction training
uses weighted cross-entropy; return training uses Huber loss. Each fold fits feature normalization
from its training rows only. `metrics.json` records fold metrics against both the `imbalance_l1`
sign baseline and logistic regression on all features, with 95% percentile-bootstrap confidence
intervals from non-overlapping UTC-hour blocks. Each block's final label horizon is purged before
resampling. The final checkpoint uses the last fold's training split. It is exported to the
validated little-endian `weights.bin` format with CRC32 and training-only normalization statistics.

Direction training requires a dead zone within 25% of the configured break-even move and a
reference price so it can report both ticks and basis points. The default break-even move is
20 bps, the fee-only round-trip cost for two fills at the configured 10 bps taker fee; spread is
not included. For this sample, 1,480 ticks at a $0.01 tick and a $7,400 reference price equals
20.00 bps. The absolute tick threshold is shared across monthly files, so the actual bps varies
with each month's market price.

The inference model has 11 inputs, two equally sized ReLU layers, and either one return output or
three direction logits. The weights file carries the hidden width; C++ accepts widths up to 32.
Training defaults to width 16, and `--hidden-width 32` trains the wider variant for direct
benchmarking. C++ provides a scalar reference and automatically uses the SIMD implementation on
NEON or SSE2 targets. Conversion from the
fixed-point feature struct to normalized `float32` inputs happens in one function. Inference uses
fixed-size storage and performs no allocation after loading weights.

Example workflow (training requires labelled dumps from at least three distinct UTC days):

    build/dump_features BTCUSDT.bin BTCUSDT-features.tbf --price-tick 1
    build/label_features BTCUSDT-features.tbf BTCUSDT-3s-direction.tfl \
      --kind direction --horizon 3 --dead-zone-ticks 1480
    python python/train.py --input BTCUSDT-2019-12-01.tfl BTCUSDT-2020-01-01.tfl \
      BTCUSDT-2020-02-01.tfl --task direction --output-dir model \
      --price-tick 0.01 --reference-price 7400.00 --break-even-move-bps 20
    python python/export_weights.py model/checkpoint.npz model/weights.bin

The local 3-second direction model used a 1,480-tick dead zone (20.00 bps at the $7,400 reference
price) on BTCUSDT samples from 2019-10-01 and 2019-11-01 for training, with 2019-12-01 held out.
This was one expanding fold with 1,522,723 training rows and 820,028 test rows from 2,342,751 total
samples; training used seed 42 and 20 epochs. Only 67 test rows were non-flat at this break-even
threshold. The 95% intervals below resample eight non-overlapping UTC-hour blocks and are too wide
to distinguish the MLP from either baseline. The shuffled return model has no dead zone; its hit
rate covers 801,187 non-flat rows and its intervals use 24 non-overlapping UTC-hour blocks:

| Task | MLP directional hit rate | `imbalance_l1` sign baseline | Logistic all-feature baseline | Other MLP metrics |
| --- | ---: | ---: | ---: | --- |
| Direction, 1,480 ticks / 20.00 bps | 71.64% (95% CI 38.30–89.41%) | 64.18% (49.21–84.45%) | 55.22% (27.02–82.77%) | 67 non-flat test rows |
| Return, no dead zone | 65.35% (64.70–65.95%) | 55.50% (54.96–56.07%) | 64.68% (63.82–65.40%) | MAE 85.76 ticks; RMSE 142.58 ticks |

The direction result is not distinguishable from either baseline with these confidence intervals.
The return model was 0.67 percentage points above the logistic hit rate on this one held-out day;
their block intervals overlap. One test day does not establish a repeatable edge; the simulated
execution result is reported below, but independent-day evidence is still missing. The new
checkpoints and metrics are kept under `/private/tmp/tickbook-break-even-training` and were not
added to the repository.

The committed tiny weights are deterministic synthetic test data, not trained trading weights.
The C++/PyTorch parity test compares 1,000 fixed vectors at `1e-5` tolerance. It is wired into the
Python CI job and passed locally. I also checked 1,000 feature vectors against each trained model;
both exported weights files passed the same PyTorch comparison tolerance.

The measurements below use the deterministic tiny weights fixture (not a trained model). The
standalone benchmark made 1,000,000 inference calls per run, with five runs per implementation.
The combined replay benchmark processed Binance BTCUSDT 2019-12-01 (6,907,104 events and
1,236,542 message-boundary feature updates) with one warm-up and five measured runs. Hardware:
Apple M2 MacBook Air, 8 GB, macOS 27.2.1; compiler: Apple LLVM 21.0.0; Release flags:
`-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow`. Timer overhead is the median of 101 batches of
64 back-to-back `steady_clock` pairs and is subtracted per observation. The estimated minimum
positive `steady_clock` step was 41 ns; median read overhead was 20 ns for the width comparison.
Standalone inference excludes the SPSC ring hop. The combined replay measurement includes the
producer push, consumer pop, and measured book, feature, and inference work. The M2 Air is fanless and
cannot pin replay threads; run-to-run ranges and exact maxima reflect that environment. These are
replay processing latencies, not trading latency.

| Inference path | p50 ns | p99 ns | p99.9 ns |
| --- | ---: | ---: | ---: |
| Scalar MLP, 11-16-16-3 | 151 (125–151) | 279 (191–487) | 463 (231–575) |
| SIMD MLP, 11-16-16-3 | 48 (25–57) | 97 (67–135) | 143 (135–175) |
| Offset book + features + SIMD inference | 27 (27–28) | 407 (407–407) | 527 (527–783) |

To compare hidden widths directly, I trained a return model at width 32 and ran five standalone
million-call measurements for each width on those trained return weights. Values are medians with
min–max run ranges; timer overhead was subtracted. Both widths had zero inference allocations and
minor faults:

| SIMD return MLP | p50 ns | p99 ns | p99.9 ns |
| --- | ---: | ---: | ---: |
| 11-16-16-1 | 63 (61–67) | 105 (65–151) | 147 (105–195) |
| 11-32-32-1 | 147 (147–151) | 231 (195–231) | 279 (235–319) |

One combined replay run per width on the same day also included the SPSC ring hop, book update,
features and inference. Width 16 measured p50/p99/p99.9 of 28/407/527 ns; width 32 measured
27/527/1,087 ns. The 32-unit run exceeded the 500 ns p99 budget. These are single-run checks, not
the five-run table above.

The SIMD inference median p99 was 65% lower than the scalar result for the same width and fixture.
All five combined replay runs recorded zero hot-path allocations; process minor faults were 0–2
on macOS. The combined median p99 was 407 ns across these runs, below the 500 ns target; all five
measured p99 at 407 ns. Their p99.9 median was 527 ns. This M2 measurement does not replace pinned-
core Linux confirmation. Exact maxima ranged from 83,152 to 2,166,401 ns and are treated as host
scheduling and thermal outliers.

To reproduce the inference and combined replay measurements on a Release build:

    cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
      -DTICKBOOK_BUILD_BENCHMARKS=ON -DBUILD_TESTING=OFF
    cmake --build build-bench --target nn_latency replay_latency -j
    build-bench/nn_latency tests/data/tiny_weights.bin --model scalar --iterations 1000000
    build-bench/nn_latency tests/data/tiny_weights.bin --model simd --iterations 1000000
    build-bench/replay_latency /path/to/BTCUSDT.bin --book offset --price-tick 1 \
      --weights tests/data/tiny_weights.bin

Repeat each command five times; leave 5 seconds between standalone inference runs and 10 seconds
between full replay runs to let the fanless M2 cool down.

One additional validation run with the trained direction weights processed the held-out
2019-12-01 replay: p50 27 ns, p99 447 ns, p99.9 575 ns, max 781,485 ns; 1,236,542 inference calls,
zero hot-path allocations and zero process minor faults. The standalone trained direction model
measured p50 27 ns, p99 69 ns and p99.9 111 ns over 1,000,000 calls in one run; the return model
measured 28 ns, 71 ns and 111 ns respectively. These single-run checks confirm the exported
weights load and meet the observed latency budget on this host; the five-run fixture benchmark
above remains the repeated comparison.

## Allocation counter (milestone 4.2)

The Release replay benchmark replaces global `operator new` and counts allocations from the
startup-done marker through producer and consumer completion. Flat and offset book runs fail on
any hot-path allocation. The `BookMap` run must record at least one allocation as a positive
control. The CI allocation job runs the benchmark on the same small deterministic fixture used by
`alloc_check`.

## Strategies, simulated exchange and backtest (milestone 7)

`backtest` streams a validated binary replay through an `OffsetArrayBook`, `FeatureEngine`, a
strategy and `SimExchange`. `ThresholdStrategy` trades the sign of `imbalance_l1`; `NnStrategy`
trades either the return output (default threshold 1 tick) or direction-logit margin from a loaded
MLP. Signal scores stay in Q20 fixed point. `--size` and `--max-position` are parsed at the replay
file's quantity scale. The simulator is long-only Spot and rejects orders beyond either limit.

    build/backtest BTCUSDT.bin --strategy threshold --latency-ms 5 --fee-ppm 1000 \
      --price-tick 0.01 --size 0.001 --max-position 0.01
    build/backtest BTCUSDT.bin --strategy nn --weights model/weights.bin --latency-ms 5 \
      --fee-ppm 1000 --price-tick 0.01 --size 0.001 --max-position 0.01

Market orders activate at `t + latency` and fill against the first complete message at or after
that time. They walk the recorded opposite side, cancel any unfilled remainder, and do not change
the replay book; the model has no market impact, collateral, liquidation or queue-position model.
All simulated fills pay the configurable taker fee. The default is 1,000 ppm (0.10%, 10 bps),
Binance's Regular User Spot taker rate without the optional BNB discount. See the current
[Binance Spot fee schedule](https://www.binance.com/en/fee/trading) and
[BNB fee-discount rules](https://www.binance.com/en/support/faq/detail/115000583311); account tier,
region and discount eligibility can change the rate, so override `--fee-ppm` for an account-specific
simulation. The quote ledger keeps six fractional digits beyond the binary file's price scale so
small per-fill fees are not rounded up to cents.

The output reports gross and fee-adjusted net mark-to-mid P&L, fees, turnover, closed round trips,
average net P&L per closed trade in bps, hit rate, drawdown, break-even fee and a per-event replay
processing histogram. Percentiles use the fixed log-linear buckets and subtract the measured
`steady_clock` read overhead. This is replay processing time, not live or trading latency.

For one local held-out day (Binance BTCUSDT 2019-12-01, 6,907,104 events and 1,236,542 messages),
the shuffled 3-second return model with 5 ms simulated order latency and the default 10 bps taker
fee had gross P&L -$176.21, fees $2,769.48, net P&L -$2,945.69 and 22,229 closed trades. The
recentered bounded book matched BookMap's top five after every event on this file. This single day
is not evidence of profitability; the negative result does not establish an out-of-sample edge.

The Release benchmark used the same held-out file and configuration, with one warm-up pass and five
measured runs per strategy, separated by a 5-second cool-down. Hardware: Apple M2 MacBook Air, 8 GB,
macOS 27.2.1; compiler: Apple LLVM 21.0.0; flags: `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow`.
Values are medians; parentheses give the min–max run range. The exact max includes host scheduling
outliers. The p99 target from milestone 4.4 is 500 ns/event.

| Strategy | p50 ns | p99 ns | p99.9 ns | max ns |
| --- | ---: | ---: | ---: | ---: |
| Threshold on `imbalance_l1` | 21 (19–22) | 319 (319–399) | 439 (439–1,023) | 144,854 (81,853–13,496,939) |
| 3-second return MLP | 22 (21–25) | 399 (399–407) | 527 (527–527) | 109,396 (85,982–1,937,481) |

Both repeated p99 medians fit the 500 ns target on this M2 host; the return model's p99.9 median
exceeds it. There is no Linux pinned-core run yet. The negative results do not establish an
out-of-sample edge.

## Order book layouts

`BookMap` is the full-depth reference. `FlatBook` stores the best 4,096 levels per side by
default. At capacity it drops the worst retained level when a better one arrives and ignores
updates below its retained range. It reserves its vectors at construction. `OffsetArrayBook`
uses a window of 65,536 price ticks by default; pass the fixed-point tick size as the second
constructor argument when it is greater than one. The first accepted level centers the window.
When the best bid reaches the upper quarter or the best ask reaches the lower quarter, the book
shifts the window by half its width. It keeps every level in the old/new overlap and drops levels
outside the new window. A far-away update by itself does not trigger recentering. Recenter shifts
reuse preallocated storage and do not allocate. Both optimized books avoid allocations during
updates.

`compare_books` uses each bounded book's fixed default capacity and compares best levels,
crossed state, and top five levels after every event. It prints each book's final depth counts;
they may differ when a level falls outside a bounded book's retained range. The binary header
does not contain the exchange tick size, so pass it as an integer number of fixed-point price
units when it is greater than one (the default is one):

    build/compare_books /path/to/day.bin --price-tick 1

With this policy, `compare_books` matched the top five against `BookMap` on all 6,907,104 events
from BTCUSDT 2019-12-01. The final BookMap had 3,415 bid levels and 3,783 ask levels; the offset
book retained 960 bids and 1,006 asks inside its final window.

A synthetic 24-hour quote path started at $40,000 and ended at $41,200, a 3% rise over 1,440
one-minute updates. Conversion produced 57,620 events. With the default 65,536-tick window and
one fixed-point unit per tick, `compare_books` matched the top five after every event; all three
books ended with 10 bid and 10 ask levels.

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
5. Milestone 7: strategy interfaces, simulated exchange and metrics complete; validate the result
   over more independent days before continuing with strategy work.
6. Milestone 8: manual live recorder and replay validation.

See [AGENTS.md](AGENTS.md) for the full project brief, milestone checklist, and correctness gates.

## Limits

- This is replay processing latency on the stated hardware, not live trading latency.
- Price-level (L2) book only: Binance does not publish individual orders.
- Free sample data covers one day per month, so strategy results are not evidence of profitability.
