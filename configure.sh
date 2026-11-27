#!/usr/bin/env bash
# Configure the CBC compiler (clang + lld) with X86 + AArch64 backends (AArch64 is
# also required for clang to generate arm_neon.h and friends). This build does not build a host
# libc++, libc++abi, or compiler-rt. CBC runtimes are configured by
# configure-runtimes.sh after this compiler is installed.
# Must be executed, not sourced (it sets shell options and calls exit).
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  echo "${BASH_SOURCE[0]}: do not source this script; run it" >&2
  return 1
fi
set -euo pipefail

# Usage: ./configure.sh <debug|release> [flavor...]
#   flavors (default x86_64): x86_64, aarch64 (Android; needs an NDK and, until the AArch64
#   CBC backend lands, only configures the crt targets, see agents/28)
ROOT="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/cbc-env.sh"
load_cbc_env "${1:-}"

# CBC flavors whose crt (llvm/tools/cbc-ld) is built and installed.
FLAVORS=("${@:2}")
[[ ${#FLAVORS[@]} -gt 0 ]] || FLAVORS=(x86_64)
FLAVOR_TRIPLES=()
FLAVOR_ARGS=()
for f in "${FLAVORS[@]}"; do
  cbc_resolve_flavor "$f"
  FLAVOR_TRIPLES+=("$CBC_TRIPLE")
  if [[ "$f" == aarch64 ]]; then
    FLAVOR_ARGS+=("-DANDROID_NDK_ROOT=$CBC_NDK_ROOT" "-DCBC_ANDROID_API=$CBC_ANDROID_API")
  fi
done
CBC_FLAVORS_LIST="$(IFS=';'; echo "${FLAVOR_TRIPLES[*]}")"

if [[ -n "${CC:-}" ]]; then
  C_COMPILER="$CC"
elif command -v clang-21 >/dev/null 2>&1; then
  C_COMPILER="$(command -v clang-21)"
else
  C_COMPILER="$(command -v clang)"
fi
if [[ -n "${CXX:-}" ]]; then
  CXX_COMPILER="$CXX"
elif command -v clang++-21 >/dev/null 2>&1; then
  CXX_COMPILER="$(command -v clang++-21)"
else
  CXX_COMPILER="$(command -v clang++)"
fi

LINKER_ARGS=()
if [[ -n "${LLVM_USE_LINKER:-}" ]]; then
  LINKER_ARGS+=("-DLLVM_USE_LINKER=${LLVM_USE_LINKER}")
elif command -v lld-21 >/dev/null 2>&1; then
  LINKER_ARGS+=("-DLLVM_USE_LINKER=lld-21")
elif command -v ld.lld >/dev/null 2>&1; then
  LINKER_ARGS+=("-DLLVM_USE_LINKER=lld")
fi

cmake -G Ninja -S "$ROOT/llvm" -B "$BUILD" \
  -DCMAKE_C_COMPILER="$C_COMPILER" \
  -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES= \
  -DLLVM_PARALLEL_LINK_JOBS="${LLVM_PARALLEL_LINK_JOBS:-2}" \
  -DBUILD_SHARED_LIBS=ON \
  -DLLVM_BUILD_TOOLS=ON \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=CBC \
  -DCBC_FLAVORS="$CBC_FLAVORS_LIST" \
  "${FLAVOR_ARGS[@]}" \
  "${LINKER_ARGS[@]}"

echo "configured $BUILD (install prefix $PREFIX, CBC flavors: $CBC_FLAVORS_LIST)"
echo "next: ./build.sh $1"
