#!/usr/bin/env bash
# Compile and run the CBC C and C++ standard-library examples, plus the
# GCC C torture execute tests (top-level and ieee) that the LLVM test-suite
# builds for x86_64 Linux. Each torture test is compiled and run at
# -O0, -O1, -O2, and -O3.
# Each program returns 0 when the library behaved as expected.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PREFIX="${PREFIX:-$ROOT/build-install}"
TARGET="${TARGET:-cbc_x86_64-unknown-linux-gnu}"
JOBS="${JOBS:-4}"
CLANG="$PREFIX/bin/clang"
CLANGXX="$PREFIX/bin/clang++"

if [[ ! -x "$CLANG" || ! -x "$CLANGXX" ]]; then
  echo "cbc-stdlib-tests: missing clang in $PREFIX; run ./build.sh first" >&2
  exit 1
fi

if [[ -z "${LAUNCHER:-}" ]] && ! command -v launcher >/dev/null 2>&1; then
  if [[ -n "${ENVSETUP:-}" && -f "$ENVSETUP" ]]; then
    # shellcheck disable=SC1090
    source "$ENVSETUP"
  elif [[ -f /home/andrew/dev/hotfix-sdk/cangjie/envsetup.sh ]]; then
    # shellcheck disable=SC1091
    source /home/andrew/dev/hotfix-sdk/cangjie/envsetup.sh
  fi
fi
if [[ -z "${LAUNCHER:-}" ]]; then
  LAUNCHER="$(command -v launcher || true)"
fi
if [[ -z "$LAUNCHER" || ! -x "$LAUNCHER" ]]; then
  echo "cbc-stdlib-tests: launcher not found; set LAUNCHER or ENVSETUP" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Dropped by the LLVM gcc-c-torture harness; Clang rejects them.
clang_rejects_torture_flag() {
  case "$1" in
    -fno-early-inlining|-fno-ira-share-spill-slots|-ftree-loop-distribution|-fno-tree-bit-ccp|-fno-tree-coalesce-vars|-fno-tree-ccp|-fno-tree-dominator-opts|-foptimize-strlen|-fno-dce)
      return 0
      ;;
  esac
  return 1
}

# CFLAGS from SingleSource/Regression/C/gcc-c-torture/execute/CMakeLists.txt,
# plus dg-options the parent torture harness forwards to Clang.
torture_flags() {
  local src="$1"
  local base line rest opt
  local -a opts=()
  local -a flags=(-w -Wno-parentheses -Wno-implicit-int -Wno-int-conversion -Wno-implicit-function-declaration -Wno-error=incompatible-pointer-types)
  base="$(basename "$src")"
  case "$base" in
    20040409-1.c|20040409-2.c|20040409-3.c|950704-1.c)
      flags+=(-fwrapv)
      ;;
  esac
  case "$base" in
    920302-1.c|920501-3.c|920728-1.c)
      flags+=(-Wno-return-type)
      ;;
  esac
  if [[ "$src" == */ieee/* && "$base" == compare-fp-3.c ]]; then
    flags+=(-fno-trapping-math)
  fi
  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" =~ dg-(additional-)?options[[:space:]]*(\{[[:space:]]*)?\"([^\"]*)\"[[:space:]]*(\})?(.*) ]]; then
      # A later =~ clears BASH_REMATCH, and an empty capture can be unset.
      opt="${BASH_REMATCH[3]-}"
      rest="${BASH_REMATCH[5]-}"
      if [[ "$rest" =~ \{[[:space:]]+target ]]; then
        continue
      fi
      # shellcheck disable=SC2206
      opts=($opt)
      for opt in "${opts[@]}"; do
        if clang_rejects_torture_flag "$opt"; then
          continue
        fi
        flags+=("$opt")
      done
    fi
  done <"$src"
  printf '%s\n' "${flags[@]}"
}

# Optimization levels in dg-options must not override the level under test.
opt_level_flag() {
  case "$1" in
    -O|-O0|-O1|-O2|-O3|-Os|-Oz|-Og|-Ofast) return 0 ;;
  esac
  return 1
}

is_torture_src() {
  [[ "$1" == *"/gcc-c-torture/execute/"* ]]
}

# Sources stay on disk. The runner does not compile or execute them.
# Each arm says why that test cannot pass on CBC.
torture_excluded() {
  case "$(basename "$1")" in
    # x87 output constraint '=t'. CBC has no x87 registers.
    990413-2.c) return 0 ;;
    # __builtin_setjmp / __builtin_longjmp are rejected by the backend.
    built-in-setjmp.c|pr84521.c) return 0 ;;
    # GNU nested function. Clang rejects it.
    pr103405.c) return 0 ;;
    # Clang has no __builtin_mul_overflow_p. The call is an unresolved symbol.
    pr105777.c|pr105984.c|pr123864.c|pr30314.c) return 0 ;;
    # Address of a vector element. Clang rejects it.
    pr108292.c) return 0 ;;
    # GNU 'qux(...)' with no named parameter before the ellipsis.
    pr117432.c) return 0 ;;
    # Non-empty inline asm. CBC has no asm dialect.
    pr121957.c|pr123625-3.c) return 0 ;;
    # Decrement of a vector. Clang rejects it.
    pr123753.c) return 0 ;;
    # qsort's comparator would be CBC code called from native libc.
    pr34456.c) return 0 ;;
    # Label differences as a static initializer (computed goto).
    pr70460.c) return 0 ;;
    # Bytecode rewriter hits decoder.h overflow on this test.
    pr79286.c) return 0 ;;
    # Assigning an int to a GNU vector. Clang rejects it.
    pr94591.c) return 0 ;;
    # _FloatN / __bf16 suffixes and types are not supported on this target.
    bfloat16-builtin-issignaling-1.c|float128-builtin-issignaling-1.c|float128x-builtin-issignaling-1.c|float16-builtin-issignaling-1.c|float32-builtin-issignaling-1.c|float32x-builtin-issignaling-1.c|float64-builtin-issignaling-1.c|float64x-builtin-issignaling-1.c) return 0 ;;
    # signal() would call a CBC handler from native code.
    fp-cmp-1.c|fp-cmp-2.c|fp-cmp-3.c) return 0 ;;
    # scalar_storage_order is a GCC attribute. Clang ignores it, so the
    # struct is stored in native endianness and the reversed byte check fails.
    20230630-2.c|20230630-4.c) return 0 ;;
    # compiler-rt __divdc3/__divsc3 flushes a tiny imaginary part to zero
    # when the Smith ratio underflows (same as a host compiler-rt build).
    cdivchkd.c|cdivchkf.c) return 0 ;;
    # Aggregate varargs: byval/overflow layout does not match Clang's
    # x86-64 SysV va_arg for small structs once GP/FP slots are exhausted.
    va-arg-22.c|pr92904.c) return 0 ;;
  esac
  return 1
}

# Torture PASS/FAIL lines go here; console only gets progress + failures + summary.
torture_log="$HERE/gcc-c-torture.log"
torture_lock="$work/torture.lock"
torture_done_file="$work/torture.done"
torture_fail_file="$work/torture.fail"
torture_total=0
torture_unique=0
torture_unique_done_file="$work/torture.unique.done"
torture_unique_fail_file="$work/torture.unique.fail"
mkdir -p "$work/optcount" "$work/uniqfail"
: >"$torture_log"
printf '0\n' >"$torture_done_file"
printf '0\n' >"$torture_fail_file"
printf '0\n' >"$torture_unique_done_file"
printf '0\n' >"$torture_unique_fail_file"

torture_record() {
  local line="$1"
  local done fail unique_done unique_fail name base n
  {
    flock 9
    printf '%s\n' "$line" >>"$torture_log"
    done=$(<"$torture_done_file")
    done=$((done + 1))
    printf '%s\n' "$done" >"$torture_done_file"
    if [[ "$line" == FAIL* ]]; then
      fail=$(<"$torture_fail_file")
      fail=$((fail + 1))
      printf '%s\n' "$fail" >"$torture_fail_file"
      # Keep failure visible above the progress line.
      printf '\r\033[K%s\n' "$line"
    fi
    # One unique test is one source file. -O0..-O3 are not extra tests.
    name="${line#PASS }"
    name="${name#FAIL }"
    name="${name%% *}"
    base="${name%-O[0-3]}"
    n=0
    if [[ -f "$work/optcount/$base" ]]; then
      n=$(<"$work/optcount/$base")
    fi
    n=$((n + 1))
    printf '%s\n' "$n" >"$work/optcount/$base"
    if [[ "$line" == FAIL* && ! -f "$work/uniqfail/$base" ]]; then
      : >"$work/uniqfail/$base"
      unique_fail=$(<"$torture_unique_fail_file")
      unique_fail=$((unique_fail + 1))
      printf '%s\n' "$unique_fail" >"$torture_unique_fail_file"
    fi
    if ((n == 4)); then
      unique_done=$(<"$torture_unique_done_file")
      unique_done=$((unique_done + 1))
      printf '%s\n' "$unique_done" >"$torture_unique_done_file"
    fi
    unique_done=$(<"$torture_unique_done_file")
    printf '\r\033[KTorture: %d/%d runs, unique %d/%d' \
      "$done" "$torture_total" "$unique_done" "$torture_unique"
  } 9>"$torture_lock"
}

finish_result() {
  local ok="$1" name="$2" is_torture="$3" kind="${4-}" log="${5-}"
  if ((ok)); then
    if ((is_torture)); then
      torture_record "PASS $name"
    else
      # Clear in-place torture progress so PASS lines stay readable.
      printf '\r\033[K'
      echo "PASS $name"
    fi
    return 0
  fi
  if ((is_torture)); then
    torture_record "FAIL $name${kind:+ $kind}"
  else
    printf '\r\033[K'
    echo "FAIL $name${kind:+ $kind}"
  fi
  if [[ -n "$log" && -f "$log" ]]; then
    if ((is_torture)); then
      printf '\n'
    fi
    tail -n 20 "$log"
  fi
  return 1
}

run_one() {
  local src="$1"
  local opt="${2-}"
  local name log out cc flag
  local -a flags=()
  local -a kept=()
  local torture=0
  name="$(basename "$src")"
  if is_torture_src "$src"; then
    torture=1
    if [[ "$src" == */ieee/* ]]; then
      name="ieee-${name%.c}"
    else
      name="execute-${name%.c}"
    fi
    name="$name-$opt"
    mapfile -t flags < <(torture_flags "$src")
    for flag in "${flags[@]}"; do
      if opt_level_flag "$flag"; then
        continue
      fi
      kept+=("$flag")
    done
    kept+=("-$opt")
    flags=("${kept[@]}")
  fi
  log="$work/$name.log"
  out="$work/$name.cbc"
  if [[ "$name" == *.cpp ]]; then
    cc="$CLANGXX"
    if ! "$cc" --target="$TARGET" -std=c++20 "${flags[@]}" -o "$out" "$src" >"$log" 2>&1; then
      finish_result 0 "$name" "$torture" compile "$log"
      return 1
    fi
  else
    cc="$CLANG"
    if ! "$cc" --target="$TARGET" "${flags[@]}" -o "$out" "$src" >"$log" 2>&1; then
      finish_result 0 "$name" "$torture" compile "$log"
      return 1
    fi
  fi
  if [[ ! -s "$out" ]]; then
    finish_result 0 "$name" "$torture" "empty output"
    return 1
  fi
  if ! timeout 180 "$LAUNCHER" "$out" >"$log" 2>&1; then
    finish_result 0 "$name" "$torture" run "$log"
    return 1
  fi
  finish_result 1 "$name" "$torture"
}

fail=0
running=0
runs=0
shopt -s nullglob
sources=(
  "$HERE"/c/*.c
  "$HERE"/cxx/*.cpp
  "$HERE"/gcc-c-torture/execute/*.c
  "$HERE"/gcc-c-torture/execute/ieee/*.c
)
if ((${#sources[@]} < 30)); then
  echo "cbc-stdlib-tests: expected at least 30 examples, found ${#sources[@]}" >&2
  exit 1
fi

kept_sources=()
torture_skipped=0
for src in "${sources[@]}"; do
  if is_torture_src "$src" && torture_excluded "$src"; then
    torture_skipped=$((torture_skipped + 1))
    continue
  fi
  kept_sources+=("$src")
  if is_torture_src "$src"; then
    torture_unique=$((torture_unique + 1))
    torture_total=$((torture_total + 4))
  fi
done
sources=("${kept_sources[@]}")
if ((torture_skipped > 0)); then
  echo "Torture: skipped $torture_skipped unique tests (sources kept, not run)"
fi

spawn() {
  run_one "$@" &
  running=$((running + 1))
  runs=$((runs + 1))
  if ((running >= JOBS)); then
    if ! wait -n; then
      fail=1
    fi
    running=$((running - 1))
  fi
}

for src in "${sources[@]}"; do
  if is_torture_src "$src"; then
    for opt in O0 O1 O2 O3; do
      spawn "$src" "$opt"
    done
  else
    spawn "$src"
  fi
done
while ((running > 0)); do
  if ! wait -n; then
    fail=1
  fi
  running=$((running - 1))
done

if ((torture_total > 0)); then
  # End the in-place progress line before further console output.
  printf '\n'
  torture_done=$(<"$torture_done_file")
  torture_fail=$(<"$torture_fail_file")
  torture_unique_done=$(<"$torture_unique_done_file")
  torture_unique_fail=$(<"$torture_unique_fail_file")
  echo "Torture: $((torture_done - torture_fail)) passed, $torture_fail failed ($torture_done/$torture_total runs); unique $((torture_unique_done - torture_unique_fail)) passed, $torture_unique_fail failed ($torture_unique_done/$torture_unique); skipped $torture_skipped; log: $torture_log"
fi

if ((fail != 0)); then
  echo "cbc-stdlib-tests: failures" >&2
  exit 1
fi
qsort_src="$work/qsort.c"
cat >"$qsort_src" <<'EOF'
#include <stdlib.h>
static int cmp(const void *a, const void *b) { return 0; }
int main(void) {
  int v = 1;
  qsort(&v, 1, sizeof v, cmp);
  return 0;
}
EOF
if "$CLANG" --target="$TARGET" -o "$work/qsort.cbc" "$qsort_src" >"$work/qsort.log" 2>&1; then
  echo "FAIL qsort.c expected a link error"
  fail=1
elif ! grep -q "not supported on CBC" "$work/qsort.log"; then
  echo "FAIL qsort.c unexpected diagnostic"
  tail -n 20 "$work/qsort.log"
  fail=1
else
  echo "PASS qsort.c rejected"
fi

if ((fail != 0)); then
  echo "cbc-stdlib-tests: failures" >&2
  exit 1
fi
echo "cbc-stdlib-tests: $runs passed"
