#!/usr/bin/env bash
# Build and install the CBC compiler, then the CBC libc++ / libc++abi /
# compiler-rt runtimes. configure.sh configures the compiler;
# configure-runtimes.sh configures the runtimes. The compiler install
# includes crt-cbc.bc.
# Must be executed, not sourced (it sets shell options and calls exit).
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  echo "${BASH_SOURCE[0]}: do not source this script; run it" >&2
  return 1
fi
set -euo pipefail

# Usage: ./build.sh <debug|release> [x86_64|aarch64]
# The flavor selects which CBC runtimes are built (directory name cbc.<host triple>).
ROOT="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/cbc-env.sh"
load_cbc_env "${1:-}" "${2:-x86_64}" --check-configured

cmake --build "$BUILD"
cmake --install "$BUILD"

if [[ ! -f "$RUNTIME_BUILD/build.ninja" ]]; then
  "$ROOT/configure-runtimes.sh" "$PREFIX" "$RUNTIME_BUILD" "$FLAVOR"
fi
if [[ ! -f "$RUNTIME_BUILD/build.ninja" ]]; then
  echo "build.sh: $RUNTIME_BUILD is not configured" >&2
  exit 1
fi

cmake --build "$RUNTIME_BUILD"
cmake --install "$RUNTIME_BUILD"

RES_DIR="$("$PREFIX/bin/clang" -print-resource-dir)"
P="$CBC_TARGET_DIR"
required=(
  "$PREFIX/bin/clang"
  "$PREFIX/bin/clang++"
  "$PREFIX/bin/ld.lld"
  "$PREFIX/lib/$P/crt-cbc.bc"
  "$PREFIX/lib/$P/cbc_can_catch_stub.bc"
  "$PREFIX/include/$P/unwind.h"
  "$PREFIX/include/$P/c++/v1/__config_site"
  "$PREFIX/include/c++/v1/__assertion_handler"
  "$PREFIX/lib/$P/libc++.a"
  "$PREFIX/lib/$P/libc++abi.a"
  "$RES_DIR/lib/$P/libclang_rt.builtins.a"
)
# Host-triple main for PIE executables; Android is shared-library only (agents/28 D3).
[[ "$FLAVOR" == aarch64 ]] || required+=("$PREFIX/lib/$P/native/libcbc-main.a")
for path in "${required[@]}"; do
  if [[ ! -e "$path" ]]; then
    echo "build.sh: installed toolchain is missing $path" >&2
    exit 1
  fi
done

# Guard rail: the generic libc++ headers are shared with native builds. The
# only option-dependent one is __assertion_handler; it must still be libc++'s
# default handler, otherwise CBC and native builds would silently disagree.
if ! cmp -s "$PREFIX/include/c++/v1/__assertion_handler" \
            "$ROOT/libcxx/vendor/llvm/default_assertion_handler.in"; then
  echo "build.sh: $PREFIX/include/c++/v1/__assertion_handler differs from libcxx/vendor/llvm/default_assertion_handler.in;" >&2
  echo "          it is shared with native libc++ builds (overwritten by whichever installed last)" >&2
  exit 1
fi

# Native per-target runtimes may legitimately share this prefix, but this CBC
# build must never install into the native locations (lib/ or lib/<triple>/):
# that would mean the install-dir overrides in configure-runtimes.sh were lost.
for d in "$PREFIX/lib" "$PREFIX/lib/$CBC_TRIPLE" "$PREFIX/lib/$CBC_COMPILE_TARGET" \
         "$PREFIX/lib/$CBC_TRIPLE${CBC_ANDROID_API}"; do
  for f in libc++.a libc++abi.a; do
    if [[ -e "$d/$f" ]]; then
      echo "build.sh: CBC build installed $d/$f (native location); expected lib/$P" >&2
      exit 1
    fi
  done
done

echo "installed CBC toolchain in $PREFIX ($FLAVOR flavor: $P)"
echo "stdlib examples: $ROOT/cbc-stdlib-tests/run.sh"
