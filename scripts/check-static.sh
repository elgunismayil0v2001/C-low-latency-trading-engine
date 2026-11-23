#!/usr/bin/env bash
set -euo pipefail

clang_format=${CLANG_FORMAT:-clang-format}
clang_tidy=${CLANG_TIDY:-clang-tidy}
build_dir=${1:-build/static}
# These checks flag intentional zero-length string_view, named same-type parameters, and
# push_back into FlatBook vectors whose full capacity is reserved by the constructor.
tidy_checks='-*,bugprone-*,performance-*,concurrency-*'
tidy_checks+=',-bugprone-string-constructor'
tidy_checks+=',-bugprone-easily-swappable-parameters'
tidy_checks+=',-bugprone-exception-escape'

find include tools -type f \( -name '*.hpp' -o -name '*.cpp' \) \
  -exec "$clang_format" --dry-run --Werror {} +
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel
headers_translation_unit="$build_dir/tickbook_static_headers.cpp"
while IFS= read -r header; do
  printf '#include "%s"\n' "${header#include/}"
done < <(find include -type f -name '*.hpp' | sort) > "$headers_translation_unit"

find tools -type f -name '*.cpp' -print0 |
  while IFS= read -r -d '' source; do
    "$clang_tidy" --quiet -p "$build_dir" \
      -checks="$tidy_checks" \
      -warnings-as-errors='*' -header-filter='(include/tickbook|tools)/.*' \
      "$source" -- -std=c++20 -Iinclude
  done

"$clang_tidy" --quiet -checks="$tidy_checks" -warnings-as-errors='*' \
  -header-filter='(include/tickbook|tools)/.*' \
  "$headers_translation_unit" -- -std=c++20 -Iinclude
