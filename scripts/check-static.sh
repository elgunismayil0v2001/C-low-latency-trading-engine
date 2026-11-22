#!/usr/bin/env bash
set -euo pipefail

clang_format=${CLANG_FORMAT:-clang-format}
clang_tidy=${CLANG_TIDY:-clang-tidy}
build_dir=${1:-build/static}

find include tools -type f \( -name '*.hpp' -o -name '*.cpp' \) \
  -exec "$clang_format" --dry-run --Werror {} +
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel
find include tools -type f \( -name '*.hpp' -o -name '*.cpp' \) -print0 |
  while IFS= read -r -d '' source; do
    "$clang_tidy" -p "$build_dir" \
      -checks='-*,bugprone-*,performance-*,concurrency-*' \
      -warnings-as-errors='*' -header-filter='(include/tickbook|tools)/.*' \
      "$source" -- -std=c++20 -Iinclude
  done
