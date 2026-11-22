#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "Usage: $0 EVENTS.bin OUTPUT.svg [PRICE_TICK_FIXED_POINT_UNITS]" >&2
  exit 2
fi

dataset=$1
output_svg=$2
price_tick=${3:-1}
replay_latency=${REPLAY_LATENCY:-build-bench/replay_latency}
perf_bin=${PERF:-perf}
stackcollapse=${STACKCOLLAPSE_PERF:-stackcollapse-perf.pl}
flamegraph=${FLAMEGRAPH:-flamegraph.pl}

for tool in "$perf_bin" "$stackcollapse" "$flamegraph"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required profiling tool not found: $tool" >&2
    exit 1
  fi
done
if [ ! -x "$replay_latency" ]; then
  echo "Replay benchmark is not executable: $replay_latency" >&2
  exit 1
fi
if [ ! -r "$dataset" ]; then
  echo "Replay input is not readable: $dataset" >&2
  exit 1
fi

output_dir=$(dirname "$output_svg")
mkdir -p "$output_dir"
temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT
perf_data="$temporary_dir/replay-offset.data"

"$perf_bin" record -F 999 -g --call-graph dwarf -o "$perf_data" -- \
  "$replay_latency" "$dataset" --book offset --price-tick "$price_tick"
"$perf_bin" script -i "$perf_data" |
  "$stackcollapse" |
  "$flamegraph" --title "tickbook OffsetArrayBook replay" > "$output_svg"
echo "Wrote $output_svg"
