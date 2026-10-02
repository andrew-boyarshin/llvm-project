# 16 — Floating-point code generation

The engine already executes floating-point arithmetic, compares, raw bitcasts, and
float returns (`02-isa-encoding.md` §4.1, §4.4, §4.6, §4.7). `06-backend.md` §4.2 marks
those operations Legal. The running backend does not emit them. Instruction selection
is a hand-written `Select()` in `llvm/lib/Target/CBC/CBCISelDAGToDAG.cpp`, and the
instruction list in `CBCInstrInfo.td` stops at integer ALU, integer compare, `fmov`,
`f32`/`f64` loads, an `f64` store, and `cvt.f64.f32`. Anything else hits the default
case and `report_fatal_error`.

No engine change. The encodings below are the ones `02` already verified.

## 1. What failed

These came from `cbc-stdlib-tests` (the programs were then rewritten to avoid the
operations, so the suite can pass). Reintroduce them as the acceptance tests in §6.

| Program | Node that reached `Select` | Where it comes from |
|---|---|---|
| `std::complex` add | `f64 = fadd` | `std::complex<double> operator+` |
| `double` compared with `==` / `<` | `CBC unsupported branch condition` or `CBC unsupported compare` | `BR_CC` / `SETCC` on `f32` or `f64`. The condition is an ordered or unordered FP code (`SETOEQ`, `SETOLT`, `SETUNE`, …). `Select` only accepts integer codes 0–5 |
| `memcpy` of a `double`'s bits into an `i64` | `i64 = bitcast f64` | also produced by `LowerCall` and `LowerFormalArguments` when an `f32`/`f64` is passed or received in a stack slot (`CBCISelLowering.cpp`) |
| `libcxx/src/string.cpp` `as_float_helper<float>` | machine verifier: `RET implicit $ir1` uses an undefined physical register | the value was copied to `FR0` by `RetCC_CBC`, then `LowerReturn` always emits `CBC::RET`, whose TableGen `Uses = [IR1]` and whose encoder is the integer return |

`lround(sqrt(...))` already works, because `sqrt` and `lround` are native libm calls
and the value that crosses the call is an integer. That does not exercise `fadd` or
`FP_TO_SINT`.

## 2. Current return path

`RetCC_CBC` assigns `f32`/`f64` results to `FR0` and integer results to `IR1`
(`CBCCallingConv.td`). `LowerReturn` copies each result into that register and then
builds one `CBC::RET` node with no operand (`CBCISelLowering.cpp`). The encoder writes
two fixed bytes:

```text
45 11
```

That is `Ret64 IR1` (`02` §4.1: opcode `0x45`, selector nibble 1, source nibble 1).
The register allocator therefore requires `$ir1` to be live at the return. A function
whose only result was copied to `$fr0` never defines `$ir1`, and
`LiveIntervalAnalysis` rejects the function. Linking any use of `std::string` pulls
`string.cpp` into the LTO module; `as_float_helper` is in that file and is selected
even though the user's code never calls `stof`. Until this return is legal, every
program that needs an out-of-line `basic_string` method fails at compile time.

`AdjustInstrPostInstrSelection` already records a float call result as an implicit
def of `FR0`. Only the callee side is wrong.

## 3. Instructions to add

Add the following to `CBCInstrInfo.td` and `CBCEncoding.cpp`. Width `W64` is `f64`
(opcode `0x4B`); `W32` is `f32` (opcode `0x4A`). Register numbers are the existing
`cbcEncodeReg` encoding (`FR0` = 0).

| Operation | Bytes | Notes |
|---|---|---|
| `fadd fsub fmul fdiv` | `4A/4B [op:d] [l:r]` | `op` is 0, 1, 2, 3 (`02` §3 `FloatOperations`) |
| `fneg fabs fsqrt fmov` | `4A/4B [op:d] [0:s]` | `op` is 5, 6, 7, 4. The source is the **low** nibble. `fmov` is the existing `FMOV64rr` (`1A`); do not emit a second move |
| `fpow` | `4B [10:d] [l:r]` | only if a program actually forms `FPOW`. `pow` in the C suite is the native libm call and can stay that way |
| `i2f` / `f2i` | `4B [8:d] [0:s]` and `4B [9:d] [0:s]` | raw copy between `IR` and `FR`. This is `BITCAST` of `i64` and `f64`. For `i32`/`f32`, the same 64-bit copy is used after the integer side has been zero-extended; the low 32 bits are the IEEE word (`02` §4.2) |
| `Convert` | `39 [to:from] [d:s]` | `from`/`to` are `ConvertType` codes: `I32=4 U32=5 I64=6 U64=7 F32=9 F64=10`. One direction already exists: `CVT_F32_F64` is `to=F64, from=F32`. The missing pairs are `f64→f32`, both integer widths to both float widths, and both float widths to `i32`/`i64` signed and unsigned |
| `FRet64 src` | `45 [3:src]` | `src` is the FR that already holds the result. After `LowerReturn` that register is `FR0`, so the bytes are `45 30` |
| `f32` store | `61 [s:base] [6:lo4] sleb` | kind 6. `f64` store kind 7 already exists (`STRAWF64`) |
| float `Scc64` | `3F [cc:d] [l:r]` | `l` and `r` are FRs when `cc ≥ 8`. The integer `SCC64` encoder already writes this shape and masks `cc` to 4 bits; it can be reused once the operands are FRs and `d` is a GPR |
| float `Bcc64` generic | `06 [cc:l] [r:dlo] s16 dhi` | see §4. The short form the encoder writes today cannot express a float condition |

`FREM`, `FMA`, `FMINNUM`, `FMAXNUM`, and the rounding intrinsics stay libcalls, as
`06` §4.2 already says. Do not add CBC instructions for them. `FSIN`/`FCOS` (`8A`/`8B`)
can wait until something fails to select them; libm `sin`/`cos` are enough for C.

## 4. Compares

`CBCISelDAGToDAG.cpp` maps `SETEQ SETNE SETLT SETGE SETULT SETUGE` to codes 0–5 and
swaps the operands for `SETGT SETLE SETUGT SETULE`. The `default` arm is the three
fatals `CBC unsupported branch condition`, `CBC unsupported select condition`, and
`CBC unsupported compare`.

Float conditions use a different code space (`02` §3):

| ISD (ordered / unordered) | CBC code | Name |
|---|---|---|
| `SETOEQ` | 8 | `feq` |
| `SETUNE` | 9 | `fne` (`!(l == r)`, true if either operand is NaN) |
| `SETOLT` | 10 | `flt` |
| `SETOGE` | 12 | `fge` |
| `SETUGE` | 11 | `fnlt` (`!(l < r)`) |
| `SETULT` | 13 | `fnge` (`!(l >= r)`) |

`SETOGT` and `SETOLE` are the swaps of `SETOLT` and `SETOGE`, with the operands
exchanged and the code unchanged. `SETUGT` and `SETULE` are the swaps of `SETULT` and
`SETUGE`. `SETONE`, `SETO`, and `SETUO` are two compares (`06` §4.2 "Custom"):
`SETONE` is `flt || fgt`, `SETO` is `feq || fne` with the unordered case excluded by
an ordered check, `SETUO` is the negation. Implement those by emitting two `SCC64`
results and combining them with `OR64`/`XOR64` against 1, in `Select` of `SETCC`,
before the branch or select is built. Do not invent a new condition code.

The integer branch encoder is the short form:

```195:203:llvm/lib/Target/CBC/MCTargetDesc/CBCEncoding.cpp
  case CBC::BCC64: {
    unsigned CC = MI.getOperand(0).getImm();
    unsigned L = encReg(MI.getOperand(1).getReg());
    unsigned R = encReg(MI.getOperand(2).getReg());
    CB.push_back(static_cast<char>(0x06 + (CC & 7)));
    CB.push_back(static_cast<char>((L << 4) | (R & 0xF)));
```

`0x06 + cc` is only defined for integer codes 0–5 (`02` §4.1, first row). A float
code must use the generic form: opcode `0x06`, then `[cc:l] [r:dlo] s16 dhi`, with
`l` and `r` encoded as FRs. The branch displacement is already patched by
`CBCAsmPrinter` from the last two bytes of the encoded instruction; keep that
contract. Integer codes 0–5 can stay on the short form so existing tests do not
change size.

`SELECT_GPR`'s custom inserter (`EmitInstrWithCustomInserter`) builds a `BCC64` and
two `MOV64rr`s into a GPR PHI. A float select needs the same diamond with `FMOV64rr`
and an FPR destination. Add `SELECT_FPR` and teach the inserter to pick the move
opcode from the register class. The branch inside the diamond uses the float generic
`Bcc64` when the condition is a float code.

`setOperationAction` must mark `SETCC`, `BR_CC`, and `SELECT_CC` Legal for `f32` and
`f64`. Today only `i32` and `i64` are marked, but the nodes still reach `Select`
because the default action on a legal type is Legal. Setting them explicitly documents
the contract; the real work is the condition-code table.

## 5. Bitcast and the stack ABI

`LowerCall` bitcasts an `f64` stack argument to `i64` before `STU64`, and bitcasts an
`f32` to `i32` then zero-extends. `LowerFormalArguments` does the inverse on the value
loaded from the tail. Those `BITCAST` nodes are not selected, so a float that does not
fit in `FR0`–`FR7` cannot be passed. Select them with `f2i` / `i2f` (§3). An `f32`
stack slot is the low 32 bits of the `i64` slot; the zero-extend on the way out and
the truncate on the way in stay as they are.

`FP_TO_SINT`, `FP_TO_UINT`, `SINT_TO_FP`, and `UINT_TO_FP` for `i32`/`i64` and
`f32`/`f64` are the `Convert` pairs in §3. `FP_EXTEND` `f32→f64` already exists.
`FP_ROUND` `f64→f32` is `Convert F32, F64`.

Mark `FADD FSUB FMUL FDIV FNEG FABS FSQRT` Legal for both float types in
`CBCTargetLowering`'s constructor so the legalizer does not try to turn them into
compiler-rt calls that this target does not provide.

## 6. Float return

Split the return instruction.

* `CBC::RET` stays the integer/void return. Encoder unchanged (`45 11`). `Uses = [IR1]`.
* `CBC::FRET` is `FRet64 FR0`. Encoder `45 30`. `Uses = [FR0]`. No implicit `IR1`.

`LowerReturn` chooses `FRET` when every `CCValAssign` is an FP register, and `RET`
otherwise. A mixed integer-and-float return is not produced by the C ABI this target
implements; if one appears, keep writing the integer half to `IR1` and the float half
to `FR0`, then emit `FRET` only when the first result is floating and `RET` when it
is not. CBC→CBC calls leave both register files intact (`02` §4.1), and
`AdjustInstrPostInstrSelection` already defs `FR0` or `IR1` to match `RetFloat`.

The machine verifier check is the test: `as_float_helper` must compile, and
`disasm` of that method must show `ret` with an FR source (`45 30` for `FR0`), not
`45 11`.

## 7. Acceptance

Rebuild `LLVMCBCCodeGen`, copy `libLLVMCBCCodeGen.so.24.0git` into the install `lib`,
and run programs that today do not compile:

* `std::complex<double>(3, 4) + std::complex<double>(1, -4)` has real part 4 and
  imaginary part 0. Compare through `lround` of `real()` and `imag()` so the test
  does not depend on printing.
* A CBC function `float id(float x) { return x; }` called with `1.5f` returns a value
  whose `lround` is 2, and the function's disassembly contains `45 30`.
* `stof("1.5")` inside a program that also constructs a `std::string` links. This is
  the `as_float_helper` path.
* An `f64` passed as the 9th floating argument (past `FR0`–`FR7`) round-trips through
  a CBC callee. That is the stack bitcast.

`cbc-stdlib-tests/c/math.c` stays on libm. It is not a substitute for this work.
