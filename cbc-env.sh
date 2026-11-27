# Shared by configure.sh and build.sh; source it, never run it.
# Usage: load_cbc_env <debug|release> [x86_64|aarch64] [--check-configured]
#
# Sets and exports ROOT-relative BUILD, PREFIX, RUNTIME_BUILD and BUILD_TYPE
# plus FLAVOR, CBC_TRIPLE, CBC_COMPILE_TARGET, CBC_SYSROOT and CBC_TARGET_DIR for the
# requested mode/CBC flavor
# (any values already in the environment are ignored) and validates them.
# CBC_TARGET_DIR = "cbc.<host triple>" is the per-flavor directory name used
# under <prefix>/lib, <prefix>/include and <resource-dir>/lib (it must match
# clang/lib/Driver/ToolChains/CBC.cpp getTargetDirName).
#
# Flavors (agents/28-aarch64-flavour-plan.md):
#   x86_64   x86_64-unknown-linux-gnu, host headers, no sysroot
#   aarch64  AArch64 Android: directory triple aarch64-unknown-linux-android (no API level),
#            compile target aarch64-linux-android<CBC_ANDROID_API, default 23>,
#            sysroot from the NDK (ANDROID_NDK_ROOT, else the newest NDK in
#            $ANDROID_HOME / $ANDROID_SDK_ROOT / ~/Android/Sdk)
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo "cbc-env.sh is meant to be sourced by configure.sh / build.sh" >&2
  exit 1
fi

_cbc_die() { echo "$(basename "$0"): $*" >&2; exit 1; }

# Print CMakeCache.txt value of $2 from build dir $1.
_cbc_cache_get() { sed -n "s/^$2:[A-Z]*=//p" "$1/CMakeCache.txt" | head -n1; }

# Newest NDK under the Android SDK (or $ANDROID_NDK_ROOT if set); prints the path.
_cbc_find_ndk() {
  if [[ -n "${ANDROID_NDK_ROOT:-}" ]]; then
    echo "$ANDROID_NDK_ROOT"
    return
  fi
  local sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}" latest
  latest="$(find "$sdk/ndk" -mindepth 1 -maxdepth 1 -type d -regex '.*/[0-9]+\.[0-9]+\.[0-9]+' \
            -printf '%f\n' 2>/dev/null | sort -V | tail -n1)"
  [[ -n "$latest" ]] && echo "$sdk/ndk/$latest"
}

# cbc_resolve_flavor <x86_64|aarch64>: sets CBC_TRIPLE (directory triple),
# CBC_COMPILE_TARGET (clang --target), CBC_SYSROOT, CBC_NDK_ROOT, CBC_ANDROID_API.
cbc_resolve_flavor() {
  CBC_SYSROOT="" CBC_NDK_ROOT="" CBC_ANDROID_API="${CBC_ANDROID_API:-23}"
  case "$1" in
    x86_64)
      CBC_TRIPLE="x86_64-unknown-linux-gnu"
      CBC_COMPILE_TARGET="$CBC_TRIPLE"
      ;;
    aarch64)
      CBC_TRIPLE="aarch64-unknown-linux-android"
      CBC_COMPILE_TARGET="aarch64-linux-android$CBC_ANDROID_API"
      CBC_NDK_ROOT="$(_cbc_find_ndk)"
      [[ -n "$CBC_NDK_ROOT" && -d "$CBC_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot" ]] \
        || _cbc_die "flavor aarch64 (Android) needs an NDK: set ANDROID_NDK_ROOT (or install one under \$ANDROID_HOME/ndk)"
      [[ "$CBC_ANDROID_API" =~ ^[0-9]+$ && "$CBC_ANDROID_API" -ge 21 ]] \
        || _cbc_die "CBC_ANDROID_API='$CBC_ANDROID_API' is invalid (>= 21)"
      CBC_SYSROOT="$CBC_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
      ;;
    *)
      echo "usage: $(basename "$0") <debug|release> [x86_64|aarch64]" >&2
      exit 1
      ;;
  esac
}

load_cbc_env() {
  local mode="${1:-}" flavor="${2:-x86_64}" check_configured="${3:-}"
  [[ "$flavor" == "--check-configured" ]] && { check_configured="$flavor"; flavor=x86_64; }
  case "$mode" in
    debug)
      BUILD="$ROOT/build-debug"
      PREFIX="$ROOT/build-install-debug"
      RUNTIME_BUILD="$ROOT/build-cbc-runtimes-FLAVOR-debug"
      BUILD_TYPE="Debug"
      ;;
    release)
      BUILD="$ROOT/build-release"
      PREFIX="$ROOT/build-install-release"
      RUNTIME_BUILD="$ROOT/build-cbc-runtimes-FLAVOR-release"
      BUILD_TYPE="RelWithDebInfo"
      ;;
    *)
      echo "usage: $(basename "$0") <debug|release> [x86_64|aarch64]" >&2
      exit 1
      ;;
  esac
  cbc_resolve_flavor "$flavor"
  FLAVOR="$flavor"
  CBC_TARGET_DIR="cbc.$CBC_TRIPLE"
  RUNTIME_BUILD="${RUNTIME_BUILD//FLAVOR/$flavor}"   # build-cbc-runtimes-<flavor>-<mode>
  export BUILD PREFIX RUNTIME_BUILD BUILD_TYPE FLAVOR CBC_TRIPLE CBC_TARGET_DIR \
         CBC_COMPILE_TARGET CBC_SYSROOT CBC_NDK_ROOT CBC_ANDROID_API

  # Sanity checks on the loaded environment.
  local v
  for v in BUILD PREFIX RUNTIME_BUILD BUILD_TYPE; do
    [[ -n "${!v}" ]] || _cbc_die "$v is empty"
  done
  [[ "$BUILD" != "$PREFIX" && "$BUILD" != "$RUNTIME_BUILD" && "$PREFIX" != "$RUNTIME_BUILD" ]] \
    || _cbc_die "BUILD, PREFIX and RUNTIME_BUILD must be distinct directories"
  for v in BUILD PREFIX RUNTIME_BUILD; do
    [[ "${!v}" == "$ROOT"/* && "${!v}" == *"-$mode" ]] \
      || _cbc_die "$v='${!v}' is not a '$mode' directory under $ROOT"
  done

  if [[ "$check_configured" == "--check-configured" ]]; then
    [[ -f "$BUILD/build.ninja" ]] || _cbc_die "$BUILD is not configured; run ./configure.sh $mode first"
    local t p
    t="$(_cbc_cache_get "$BUILD" CMAKE_BUILD_TYPE)"
    p="$(_cbc_cache_get "$BUILD" CMAKE_INSTALL_PREFIX)"
    [[ "$t" == "$BUILD_TYPE" ]] || _cbc_die "$BUILD was configured as '$t', expected '$BUILD_TYPE'; rerun ./configure.sh $mode"
    local fl
    fl="$(_cbc_cache_get "$BUILD" CBC_FLAVORS)"
    [[ ";$fl;" == *";$CBC_TRIPLE;"* ]] \
      || _cbc_die "$BUILD was configured for CBC flavors '$fl', not '$CBC_TRIPLE'; rerun ./configure.sh $mode with every flavor you build (e.g. x86_64 $flavor)"
    [[ "$p" == "$PREFIX" ]] || _cbc_die "$BUILD has install prefix '$p', expected '$PREFIX'; rerun ./configure.sh $mode"
    if [[ -f "$RUNTIME_BUILD/CMakeCache.txt" ]]; then
      t="$(_cbc_cache_get "$RUNTIME_BUILD" CMAKE_BUILD_TYPE)"
      p="$(_cbc_cache_get "$RUNTIME_BUILD" CMAKE_INSTALL_PREFIX)"
      [[ "$t" == "$BUILD_TYPE" && "$p" == "$PREFIX" ]] \
        || _cbc_die "$RUNTIME_BUILD was configured as '$t' with prefix '$p'; remove it and rerun ./build.sh $mode"
    fi
  fi
}
