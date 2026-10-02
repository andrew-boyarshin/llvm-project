#!/usr/bin/env bash
# Build and install the CBC compiler, then the CBC libc++ / libc++abi /
# compiler-rt runtimes. configure.sh configures the compiler;
# configure-runtimes.sh configures the runtimes. The compiler install
# includes crt-cbc.bc.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$ROOT/build-install}"
BUILD="${BUILD:-$ROOT/build}"
RUNTIME_BUILD="${RUNTIME_BUILD:-$ROOT/build-cbc-runtimes}"

if [[ ! -f "$BUILD/build.ninja" ]]; then
  echo "build.sh: $BUILD is not configured; run ./configure.sh first" >&2
  exit 1
fi

cmake --build "$BUILD"
cmake --install "$BUILD"

if [[ ! -f "$RUNTIME_BUILD/build.ninja" ]]; then
  "$ROOT/configure-runtimes.sh" "$PREFIX" "$RUNTIME_BUILD"
fi
if [[ ! -f "$RUNTIME_BUILD/build.ninja" ]]; then
  echo "build.sh: $RUNTIME_BUILD is not configured" >&2
  exit 1
fi

cmake --build "$RUNTIME_BUILD"
cmake --install "$RUNTIME_BUILD"

required=(
  "$PREFIX/bin/clang"
  "$PREFIX/bin/clang++"
  "$PREFIX/bin/cbc-ld"
  "$PREFIX/lib/cbc/crt-cbc.bc"
  "$PREFIX/lib/cbc/cbc_can_catch_stub.bc"
  "$PREFIX/lib/cbc/include/unwind.h"
  "$PREFIX/lib/cbc/include/c++/v1/__config_site"
  "$PREFIX/lib/cbc/lib/libc++.a"
  "$PREFIX/lib/cbc/lib/libc++abi.a"
  "$PREFIX/lib/cbc/lib/libclang_rt.builtins-cbc.a"
)
for path in "${required[@]}"; do
  if [[ ! -e "$path" ]]; then
    echo "build.sh: installed toolchain is missing $path" >&2
    exit 1
  fi
done

while IFS= read -r stray; do
  echo "build.sh: host C++ runtime installed outside lib/cbc: $stray" >&2
  exit 1
done < <(find "$PREFIX" \( -name 'libc++.a' -o -name 'libc++abi.a' \) ! -path '*/lib/cbc/*')

echo "installed CBC toolchain in $PREFIX"
echo "stdlib examples: $ROOT/cbc-stdlib-tests/run.sh"
