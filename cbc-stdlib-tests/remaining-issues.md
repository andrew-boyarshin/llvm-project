# Remaining CBC stdlib / GCC C torture issues

Snapshot after the 2026-10-13 diagnosis of `run.log` and `gcc-c-torture.log`.
Several bugs were fixed (see “Already fixed” below). This file lists what is
still open. Do not implement the items below without an explicit go-ahead;
skipping them in `run.sh` is a harness change, not a feature.

Source logs: `cbc-stdlib-tests/run.log`, `cbc-stdlib-tests/gcc-c-torture.log`.

---

## Already fixed (not remaining)

- **C++ `Check failed: runtime != nullptr`.** PIE wrap used `-rdynamic`, so
  CBC N2C stubs for `operator new`/`delete`, `__cxa_*` (except the atexit
  family), `__gxx_personality_v0`, `__gcc_personality_v0`, and
  `__dynamic_cast` interposed `libcangjie-runtime` / `libstdc++` during ELF
  ctor/dtor. Stubs for those names are now hidden. Local re-runs of the
  previously aborting C++ tests all passed except `iostream.cpp`.
- **Empty inline asm.** `CBCRejectUnsupportedIR` rejected `asm("")` /
  `::: "memory"` / tied `"=r,0"` before `CBCLowerGlobals` could expand them.
  Empty (whitespace-only) asm is now left to that pass. `20001009-2 -O0`
  compiles and runs.
- **`iostream.cpp` register-file N2C error.** Selective stubs
  (`CBCFileWriter` keeps only `lib_start` / DefaultVis / AddressTaken;
  `CBCLink` soft-omits ABI-illegal non-address-taken candidates) drop
  libc++ `__stage2_float_loop` instead of failing the link. Stack-arg
  N2C (`cpStackSize != 0`) is still future work for true exports that
  need more than 8 int/float args.
- **`cannot select` crash.** `CBCISelDAGToDAG` used default
  `report_fatal_error` (`GenCrashDiag=true` → `abort()`), and `ld.lld`
  then SIGSEGV'd (`exit code -2`). It now passes `GenCrashDiag=false`
  (same as `CBCRejectUnsupportedIR`), so unsupported selection exits 1
  cleanly. Other `report_fatal_error` call sites with the default may
  still crash (F-29).
- **`pr122000` / 64-bit atomics.** CBC now emits `$cbc:A*.v` field-ref
  atomics (opcodes `0x65`–`0x6D`), expands cmpxchg via a custom inserter,
  lowers fence to fetch-add 0, and caps `-fcbc` inline width at 64.
  Covered by `c/atomics.c` and torture `pr122000`.

---

## 1. `long double` / x86 `fp80` / `fp128`

**Status:** compile-time `LLVM ERROR: CBC: unsupported IR ...`.  
**Kind:** unsupported IR, gated in `CBCRejectUnsupportedIR`. New numeric
type support if implemented.

CBC has no `x86_fp80` / `fp128` layout, operations, or ABI. On this x86_64
Linux target Clang’s `long double` is 80-bit. Failures are all four of
`-O0..-O3`.

### Unique execute tests (30)

`20010605-2`, `20020413-1`, `20030914-1`, `20040709-1`, `20040709-2`,
`20040709-3`, `20050121-1`, `20080502-1`, `930622-2`, `960215-1`, `960405-1`,
`960513-1`, `align-2`, `complex-6`, `complex-7`, `conversion`, `memcpy-2`,
`memset-1`, `memset-2`, `memset-3`, `pr44942`, `regstack-1`, `stdarg-1`,
`stdarg-2`, `strcmp-1`, `strcpy-1`, `strlen-1`, `strncmp-1`, `va-arg-5`,
`va-arg-6`.

(`memcpy-2` / `memset-*` / `strcmp-1` / `strcpy-1` / `strlen-1` / `strncmp-1`
fail because they use `long double` objects, not because of memcpy/memset.)

### Unique ieee tests (10)

`20010226-1`, `builtin-issignaling-1`, `cdivchkld`, `copysign1`, `copysign2`,
`fp-cmp-4l`, `fp-cmp-8l`, `inf-2`, `inf-3`, `pr36332`.

**Options:** implement 80-bit/128-bit FP in CBC, compile torture with
`-mlong-double-64`, or add these names to `torture_excluded`.

---

## 2. Already skipped by the runner (not failures)

`run.sh` `torture_excluded` already drops 34 unique sources and does not run
them. They are out of scope unless someone wants to implement the missing
piece named in the comment:

| Reason | Tests |
|---|---|
| x87 `=t` asm | `990413-2` |
| `__builtin_setjmp` / `longjmp` | `built-in-setjmp`, `pr84521` |
| GNU nested function | `pr103405` |
| `__builtin_mul_overflow_p` | `pr105777`, `pr105984`, `pr123864`, `pr30314` |
| address of vector element | `pr108292` |
| GNU `foo(...)` with no named param | `pr117432` |
| nonempty inline asm | `pr121957`, `pr123625-3` |
| vector decrement | `pr123753` |
| `qsort` + CBC comparator | `pr34456` |
| label difference as static init | `pr70460` |
| rewriter `decoder.h` overflow | `pr79286` |
| int assigned to GNU vector | `pr94591` |
| `_FloatN` / `__bf16` | `bfloat16-` and `floatN-builtin-issignaling-1` (8 files) |
| `signal()` → CBC handler | ieee `fp-cmp-1`, `fp-cmp-2`, `fp-cmp-3` |
| `scalar_storage_order` | `20230630-2`, `20230630-4` |
| compiler-rt complex div flush | ieee `cdivchkd`, `cdivchkf` |
| aggregate varargs layout | `va-arg-22`, `pr92904` |

Nonempty inline asm (real instruction text) is still unsupported. Only empty
compiler-barrier asm is lowered.

---

## Suggested decision list

1. `long double` / fp80 / fp128 — implement, force 64-bit long double, or
   exclude the 40 unique tests above.
