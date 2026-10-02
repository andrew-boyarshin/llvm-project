#!/usr/bin/env bash
# Configure the CBC compiler (clang + lld). This build does not build a host
# libc++, libc++abi, or compiler-rt. CBC runtimes are configured by
# configure-runtimes.sh after this compiler is installed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$ROOT/build-install}"
BUILD="${BUILD:-$ROOT/build}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

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
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=CBC \
  "${LINKER_ARGS[@]}"

echo "configured $BUILD (install prefix $PREFIX)"
echo "next: ./build.sh"
