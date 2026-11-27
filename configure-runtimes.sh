#!/usr/bin/env bash
# Configure the CBC runtimes (compiler-rt, libc++, libc++abi) against the
# installed CBC clang.
#
# Usage: ./configure-runtimes.sh <prefix> <runtime-build-dir> [x86_64|aarch64]
#
# <prefix> is the CBC compiler install prefix (clang must already be there).
# <runtime-build-dir> is the CMake build directory for the runtimes.
# [flavor] is the CBC flavor (host ABI the .cbc runs on), default x86_64:
#   x86_64  x86_64-unknown-linux-gnu             + -fcbc, host headers
#   aarch64 aarch64-linux-android<CBC_ANDROID_API> + -fcbc (Android; default API 23),
#           headers from the NDK: CBC_SYSROOT (set by build.sh) is required
# With P = cbc.<host triple> the install layout mirrors native per-target
# runtimes, with the "cbc." prefix keeping CBC apart from native ones:
#   <prefix>/lib/clang/<ver>/lib/P/libclang_rt.builtins.a   compiler-rt
#   <prefix>/lib/P/libc++.a, libc++abi.a                    libc++, libc++abi
#   <prefix>/include/c++/v1/...                             generic libc++ headers (shared with native)
#   <prefix>/include/P/c++/v1/__config_site, module.modulemap
# P = cbc.aarch64-unknown-linux-android for aarch64 (the API level is not part of it).
# The aarch64 flavor needs a CBC compiler that implements it (CBCTargetMachine
# currently rejects it; agents/28-aarch64-flavour-plan.md).
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 || -z "${1:-}" || -z "${2:-}" ]]; then
  echo "usage: ./configure-runtimes.sh <prefix> <runtime-build-dir> [x86_64|aarch64]" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$1"
RUNTIME_BUILD="$2"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
FLAVOR="${3:-x86_64}"
case "$FLAVOR" in
  x86_64)  TARGET=x86_64-unknown-linux-gnu;  DIR_TRIPLE="$TARGET" ;;
  aarch64)
    TARGET="aarch64-linux-android${CBC_ANDROID_API:-23}"
    DIR_TRIPLE=aarch64-unknown-linux-android
    if [[ -z "${CBC_SYSROOT:-}" || ! -d "$CBC_SYSROOT/usr/include" ]]; then
      echo "configure-runtimes.sh: flavor aarch64 needs CBC_SYSROOT (NDK sysroot); run via ./build.sh" >&2
      exit 1
    fi
    ;;
  *) echo "configure-runtimes.sh: unknown flavor '$FLAVOR' (x86_64|aarch64)" >&2; exit 1 ;;
esac
P="cbc.$DIR_TRIPLE"   # must match clang/lib/Driver/ToolChains/CBC.cpp getTargetDirName
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

# Resource dir of the installed clang, e.g. <prefix>/lib/clang/24.
RES_DIR="$("$CLANG" -print-resource-dir)"

SYSROOT_ARGS=()
[[ -z "${CBC_SYSROOT:-}" ]] || SYSROOT_ARGS+=("-DCMAKE_SYSROOT=$CBC_SYSROOT")

cmake -G Ninja -S "$ROOT/runtimes" -B "$RUNTIME_BUILD" \
  "${SYSROOT_ARGS[@]}" \
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
  -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON \
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
  -DCOMPILER_RT_INSTALL_LIBRARY_DIR="$RES_DIR/lib" \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLIBCXXABI_USE_COMPILER_RT=ON \
  -DLIBCXXABI_ENABLE_THREADS=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DLIBCXXABI_ENABLE_STATIC=ON \
  -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
  -DLIBCXXABI_ENABLE_NEW_DELETE_DEFINITIONS=ON \
  -DLIBCXXABI_INSTALL_LIBRARY_DIR="lib/$P" \
  -DLIBCXXABI_INSTALL_INCLUDE_TARGET_DIR="include/$P/c++/v1" \
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
  -DLIBCXX_INSTALL_LIBRARY_DIR="lib/$P" \
  -DLIBCXX_INSTALL_INCLUDE_TARGET_DIR="include/$P/c++/v1" \
  -DLIBCXX_HARDENING_MODE=none \
  -DLIBCXX_ASSERTION_SEMANTIC=hardening_dependent \
  -DLIBCXX_ASSERTION_HANDLER_FILE=vendor/llvm/default_assertion_handler.in
# Generic headers (everything but __config_site/module.modulemap) install to the
# default <prefix>/include/c++/v1, shared with native libc++ builds. Guard
# rails: the three LIBCXX_* options above are pinned to libc++'s defaults, so the
# shared, option-dependent file include/c++/v1/__assertion_handler is
# byte-identical to what a default native build installs. build.sh verifies it.
# LIBCXX_ENABLE_LONG_DOUBLE=OFF: omit fp80 operating symbols from the archive;
# headers still declare them (no longer a __config_site feature).

echo "configured $RUNTIME_BUILD ($FLAVOR flavor, files go to */$P)"
echo "next: ./build.sh <debug|release> $FLAVOR"
