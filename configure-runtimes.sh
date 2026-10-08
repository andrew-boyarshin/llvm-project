#!/usr/bin/env bash
# Configure the CBC runtimes (compiler-rt, libc++, libc++abi) against the
# installed CBC clang.
#
# Usage: ./configure-runtimes.sh <prefix> <runtime-build-dir>
#
# <prefix> is the CBC compiler install prefix (clang must already be there).
# <runtime-build-dir> is the CMake build directory for the runtimes.
set -euo pipefail

if [[ $# -ne 2 || -z "${1:-}" || -z "${2:-}" ]]; then
  echo "usage: ./configure-runtimes.sh <prefix> <runtime-build-dir>" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$1"
RUNTIME_BUILD="$2"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
TARGET=x86_64-unknown-linux-gnu
# -Wno-cbc-unsupported: libc++/abi headers are -I not -isystem, so Sema
# long-double / asm warnings would flood (and fail under -Werror).
CBC_FLAGS="-fcbc -Wno-cbc-unsupported"

CLANG="$PREFIX/bin/clang"
CLANGXX="$PREFIX/bin/clang++"
LLVM_AR="$PREFIX/bin/llvm-ar"
LLVM_RANLIB="$PREFIX/bin/llvm-ranlib"
LLVM_NM="$PREFIX/bin/llvm-nm"
for tool in "$CLANG" "$CLANGXX" "$LLVM_AR" "$LLVM_RANLIB" "$LLVM_NM"; do
  if [[ ! -x "$tool" ]]; then
    echo "configure-runtimes.sh: missing installed tool $tool; run ./configure.sh and ./build.sh first" >&2
    exit 1
  fi
done

cmake -G Ninja -S "$ROOT/runtimes" -B "$RUNTIME_BUILD" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_COMPILER="$CLANG" \
  -DCMAKE_CXX_COMPILER="$CLANGXX" \
  -DCMAKE_ASM_COMPILER="$CLANG" \
  -DCMAKE_C_COMPILER_TARGET="$TARGET" \
  -DCMAKE_CXX_COMPILER_TARGET="$TARGET" \
  -DCMAKE_ASM_COMPILER_TARGET="$TARGET" \
  -DCMAKE_C_FLAGS="$CBC_FLAGS" \
  -DCMAKE_CXX_FLAGS="$CBC_FLAGS" \
  -DCMAKE_AR="$LLVM_AR" \
  -DCMAKE_RANLIB="$LLVM_RANLIB" \
  -DCMAKE_NM="$LLVM_NM" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DLLVM_ENABLE_RUNTIMES="compiler-rt;libcxx;libcxxabi" \
  -DLLVM_DEFAULT_TARGET_TRIPLE="$TARGET" \
  -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
  -DCOMPILER_RT_DEFAULT_TARGET_ARCH=cbc \
  -DCOMPILER_RT_BUILD_BUILTINS=ON \
  -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
  -DCOMPILER_RT_BUILD_XRAY=OFF \
  -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
  -DCOMPILER_RT_BUILD_PROFILE=OFF \
  -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
  -DCOMPILER_RT_BUILD_MEMPROF=OFF \
  -DCOMPILER_RT_BUILD_ORC=OFF \
  -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
  -DCOMPILER_RT_BUILD_COPYPROF=OFF \
  -DCOMPILER_RT_BAREMETAL_BUILD=OFF \
  -DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF \
  -DCOMPILER_RT_LIBATOMIC_USE_PTHREAD=OFF \
  -DCOMPILER_RT_INSTALL_LIBRARY_DIR="$PREFIX/lib/cbc/lib" \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLIBCXXABI_USE_COMPILER_RT=ON \
  -DLIBCXXABI_ENABLE_THREADS=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DLIBCXXABI_ENABLE_STATIC=ON \
  -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
  -DLIBCXXABI_ENABLE_NEW_DELETE_DEFINITIONS=ON \
  -DLIBCXXABI_INSTALL_LIBRARY_DIR=lib/cbc/lib \
  -DLIBCXX_CXX_ABI=libcxxabi \
  -DLIBCXX_ENABLE_SHARED=OFF \
  -DLIBCXX_ENABLE_STATIC=ON \
  -DLIBCXX_ENABLE_THREADS=OFF \
  -DLIBCXX_HAS_PTHREAD_API=OFF \
  -DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON \
  -DLIBCXX_ENABLE_EXCEPTIONS=ON \
  -DLIBCXX_ENABLE_RTTI=ON \
  -DLIBCXX_ENABLE_FILESYSTEM=ON \
  -DLIBCXX_ENABLE_LOCALIZATION=ON \
  -DLIBCXX_ENABLE_UNICODE=ON \
  -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON \
  -DLIBCXX_ENABLE_RANDOM_DEVICE=ON \
  -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=ON \
  -DLIBCXX_ENABLE_VENDOR_AVAILABILITY_ANNOTATIONS=OFF \
  -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF \
  -DLIBCXX_USE_COMPILER_RT=ON \
  -DLIBCXX_HAS_ATOMIC_LIB=OFF \
  -DLIBCXX_ENABLE_NEW_DELETE_DEFINITIONS=OFF \
  -DLIBCXX_ENABLE_LONG_DOUBLE=OFF \
  -DLIBCXX_INSTALL_INCLUDE_DIR=lib/cbc/include/c++/v1 \
  -DLIBCXX_INSTALL_LIBRARY_DIR=lib/cbc/lib

echo "configured $RUNTIME_BUILD"
echo "next: ./build.sh"
