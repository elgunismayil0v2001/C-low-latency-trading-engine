#!/usr/bin/env bash
set -euo pipefail

fuzzer_dir=${1:-build-fuzz}
duration_seconds=${2:-60}
fuzz_tmp=$(mktemp -d "${TMPDIR:-/tmp}/tickbook-fuzz.XXXXXX")
trap 'rm -rf "$fuzz_tmp"' EXIT

for name in parse_fixed csv_line binary_file sequencer weights_loader; do
  corpus_dir="fuzz/corpus/${name}"
  prepared_dir="${fuzz_tmp}/${name}"
  mkdir -p "$prepared_dir"
  for seed in "${corpus_dir}"/*; do
    [ -f "$seed" ] || continue
    case "$seed" in
      *.hex)
        decoded_seed="${prepared_dir}/$(basename "${seed%.hex}")"
        while IFS= read -r hex_line; do
          for byte in $hex_line; do
            case "$byte" in
              [[:xdigit:]][[:xdigit:]]) printf '%b' "\\x${byte}" ;;
              *) echo "invalid hex corpus byte in $seed: $byte" >&2; exit 1 ;;
            esac
          done
        done < "$seed" > "$decoded_seed"
        ;;
      *) cp "$seed" "$prepared_dir/" ;;
    esac
  done
  "$fuzzer_dir/fuzz_${name}" "$prepared_dir" -max_total_time="$duration_seconds"
done
