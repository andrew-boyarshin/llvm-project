# 21 — Native aggregate returns

`div(7, 3)` compiled, ran, and returned the wrong `div_t`. The check
`d.quot != 2 || d.rem != 1` failed (`exit` status 5) in a function where `atoi`,
`strtol`, `abs`, and `calloc` had already succeeded. Those return a scalar in
`rax`. `div` returns a struct. The guest and the host disagree on how that struct
is returned.

`09-clang.md` §3 specifies the fix and it was not implemented. This document is
the observed failure, the ABI mismatch, and the clang-side change. The CBC
backend does not need a new return instruction once clang classifies the call
the way the host does.

## 1. What the two ABIs do

Host x86-64 System V classifies `div_t`, which is `struct { int quot; int rem; }`
(8 bytes, two `INTEGER` fields), as one `INTEGER` eightbyte. glibc's `div`
returns it in `rax`: `quot` in the low 32 bits, `rem` in the high 32 bits. It
does not take a hidden pointer.

Clang's code generator for CBC does not have a target ABI. There is no
`clang/lib/CodeGen/Targets/CBC.cpp`, and `createTargetCodeGenInfo` has no `cbc`
case. The fallback is `DefaultABIInfo`. That class returns every aggregate
indirectly: the caller allocates a slot and passes its address, and the callee
is expected to store the struct there.

The CBC call lowerer (`CBCISelLowering::LowerCall`) then does what the IR says.
If the IR has an `sret` pointer, that pointer is an integer argument, and the
value the caller reads back is the slot. glibc never stored to that slot. The
slot contains whatever was there before the call. `quot` and `rem` are not 2
and 1.

Scalar integer returns are unaffected. `LowerCall` copies the result from `IR1`,
and the native adapter places `rax` where the engine's integer return register
is. That path is why `atoi` in the same function worked.

## 2. The change

Implement `09` §3, limited to what this bug needs. The rest of that section
(va_arg against the host `va_list`, inline asm rejection) stays as written there.

* Expose `createX86_64ABIInfo` from `clang/lib/CodeGen/TargetInfo.h`. The class
  is file-local in `clang/lib/CodeGen/Targets/X86.cpp` today.
* Add `clang/lib/CodeGen/Targets/CBC.cpp` with `CBCTargetCodeGenInfo` holding
  that `ABIInfo`. For `cbc_x86_64-unknown-linux-gnu` construct it with
  `X86AVXABILevel::None`. CBC has no SSE classification beyond what the integer
  and xmm register files already model; `09` §3 uses `None` for the same reason.
* Register it from `CodeGenModule::createTargetCodeGenInfo` on `Triple::cbc`.
* The AArch64 factory stays unhooked until that flavour is compiled. A CBC
  AArch64 codegen info that pointed at `AArch64ABIInfo` would be the same patch;
  the driver already rejects AArch64 code generation.

After this, `div`'s return type is an `i64` in the IR (the coerced eightbyte),
not an `sret` pointer. The existing `IR1` return path reads `rax`. The caller
extracts `quot` with a truncate and `rem` with a shift of 32. Both are
operations the backend already selects.

Apply the same classification to arguments. A native function that takes
`div_t` by value receives it in an integer register, not as a pointer. CBC
functions that take or return a struct to other CBC functions must use this same
classification. Otherwise a CBC wrapper around `div` would un-coerce at the
boundary. One `ABIInfo` for the target does that.

Do not special-case `div` in the backend. `ldiv`, `lldiv`, and any other host
function that returns a small aggregate (`struct timespec` is larger and stays
indirect; `div_t` is the 8-byte case) go through the same classification.
`09` §6 already asks for IR tests of structs of one to four words. The runtime
test below is the one that catches a classification that matches clang's own IR
but not glibc.

## 3. What not to change

* `long double` stays `double` (`09` §1.1, `CBCTargetInfo`). The host's
  `long double` is 80-bit. Guest `long double` is not passed to host `*l`
  functions; those are renamed or rejected by the native-libcall table
  (`10` §5.4, `14` §9.4). This document does not reopen that.
* Aggregates larger than 16 bytes stay indirect on x86-64. That matches System V
  and matches `DefaultABIInfo`, so they already work by accident. The bug is
  only the aggregates System V returns in registers.
* Two-register integer returns (`i128`, a struct of two `long`s) come back in
  `rax` and `rdx`. `02` §4.1 says a CBC function that returns `{i64, i64}`
  leaves the second half in `IR2`. The host's second integer return register
  has to be where the engine puts `rdx` after a native return. Confirm that
  against `01` §4 before declaring `lldiv` done. `div_t` is one register and
  does not depend on this. If `rdx` is not preserved into `IR2`, `lldiv` is a
  follow-up in this file, not a reason to block `div`.

## 4. Acceptance

* `div(7, 3)` has `quot == 2` and `rem == 1`. `div(-7, 3)` matches glibc
  (toward zero). Put this in `cbc-stdlib-tests/c/stdlib-conv.c`, which already
  calls `atoi` and `strtol`.
* `clang -emit-llvm -S` on `div_t d = div(7, 3);` shows an `i64` result, not an
  `sret` parameter.
* A CBC function that returns `div_t` to another CBC function, and a CBC
  function that returns `div_t` obtained by calling host `div`, agree on both
  fields.
