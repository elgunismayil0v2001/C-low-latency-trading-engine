#!/usr/bin/env bash
set -euo pipefail

build_root=${TICKBOOK_BUILD_ROOT:-build/check}
compiler=${CXX:-c++}

cmake -S . -B "$build_root/plain" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER="$compiler"
cmake --build "$build_root/plain" --parallel
ctest --test-dir "$build_root/plain" --output-on-failure

cmake -S . -B "$build_root/asan" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER="$compiler" -DTICKBOOK_SANITIZE=address,undefined
cmake --build "$build_root/asan" --parallel
ctest --test-dir "$build_root/asan" --output-on-failure

cmake -S . -B "$build_root/tsan" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER="$compiler" -DTICKBOOK_SANITIZE=thread
cmake --build "$build_root/tsan" --parallel
ctest --test-dir "$build_root/tsan" --output-on-failure

scripts/check-static.sh "$build_root/static"

if [ -d python/tests ]; then
  python3 -m pytest python/tests
fi

cmake -S . -B "$build_root/fuzz" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER="${CLANGXX:-clang++}" -DBUILD_TESTING=OFF \
  -DTICKBOOK_BUILD_FUZZERS=ON
cmake --build "$build_root/fuzz" --parallel
fuzz/run_smoke.sh "$build_root/fuzz"
