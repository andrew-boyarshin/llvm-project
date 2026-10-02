# 02 — CBC Instruction Encoding for the LLVM Backend

Ground truth: `src/cbc/isa_opcodes.h` (opcode numbering), `src/cbc/isa_parser.cpp`
(operand order and widths), `src/cbc/isa.h` (enumerations), `src/cbc/isa_rewriter.cpp`
(what each instruction becomes), `src/interpreter/*` (runtime semantics). All paths are in
`cbc-engine-initial-stage`. Every encoding in §4–§6 was cross-checked against the parser;
the samples in §10 were decoded from `sandbox/default.cbc`.

## 1. Encoding primitives

* An instruction is one opcode byte followed by operand bytes. There is no alignment and
  no length prefix; the parser consumes operands field by field.
* **Nibble pairs.** `ReadU4().ReadU4()` reads one byte; the **first** operand is the
  **high** nibble, the second the low nibble. Notation in this file: `[a:b]` is the byte
  `(a << 4) | b`.
* `ReadU4Skip4` (legacy forms only) reads one byte and uses the low nibble.
* `u16`, `u32`, `u64`, `s16`, `s32`: little-endian.
* `uleb` / `sleb`: LEB128 (`src/utils/lebencodings.h`).
* **Split immediates.** Several instructions carry a 64-bit immediate as a low nibble in
  a nibble pair plus a `sleb` "high" part: `value = (sleb << 4) | lo4`
  (`MergeLowHi`, `isa_parser.cpp:260`). To encode a signed 64-bit `v`:
  `lo4 = v & 0xF`, `hi = v >> 4` (arithmetic shift), then emit `sleb(hi)`.
* **Branch displacements** are relative to the position **after** the whole instruction
  (`IsaRewriter::Pos()`), in original-bytecode bytes. Narrow form `s16`, wide form `s32`
  after a `WidePrefix` byte (`0x13`). For the generic `Bcc32/Bcc64` the displacement is a
  split value: `lo4` in a nibble plus `s16`/`s32` high part (range ±2^19 narrow,
  ±2^35 wide).
* Register fields are 4 bits. Integer registers `IRZ=0, IR1..IR13 = 1..13, IR_ACC = 14`
  (15 is invalid). Float registers `FR0..FR15 = 0..15`. Operands typed `AnyReg` are an
  integer or float register depending on the access kind / condition code / conversion
  type.

## 2. Opcode table (current source)

`ISA_OPCODES` declaration order, `uint8_t` from 0. **This table supersedes Appendix C of
`isa_reference.md`.** Bold rows are used by the C target.

| Hex | Name | Hex | Name | Hex | Name | Hex | Name |
|---|---|---|---|---|---|---|---|
| **00** | **Bcc32Eq** | **01** | **Bcc32Ne** | **02** | **Bcc32Lt** | **03** | **Bcc32Ge** |
| **04** | **Bcc32Ult** | **05** | **Bcc32Uge** | **06** | **Bcc64Eq** | **07** | **Bcc64Ne** |
| **08** | **Bcc64Lt** | **09** | **Bcc64Ge** | **0A** | **Bcc64Ult** | **0B** | **Bcc64Uge** |
| 0C | BccReq | 0D | BccRne | **0E** | **Bcc32** | **0F** | **Bcc64** |
| **10** | **BccImm32** | **11** | **BccImm64** | **12** | **Jump** | **13** | **WidePrefix** |
| 14 | Mov32 | **15** | **Mov64** | 16 | Mov32i | **17** | **Mov64i** |
| 18 | MovRef | 19 | FMov32 | **1A** | **FMov64** | **1B** | **FMov32i** |
| **1C** | **FMov64i** | 1D | MovBP | **1E** | **BFX** | **1F** | **Add32** |
| **20** | **Sub32** | **21** | **Mul32** | **22** | **And32** | **23** | **Or32** |
| **24** | **Xor32** | **25** | **UDiv32** | **26** | **URem32** | **27** | **LSR32** |
| **28** | **ASR32** | **29** | **LSL32** | **2A** | **Add64** | **2B** | **Sub64** |
| **2C** | **Mul64** | **2D** | **And64** | **2E** | **Or64** | **2F** | **Xor64** |
| **30** | **UDiv64** | **31** | **URem64** | **32** | **LSR64** | **33** | **ASR64** |
| **34** | **LSL64** | **35** | **Binary32** | **36** | **Binary64** | **37** | **BinaryImm32** |
| **38** | **BinaryImm64** | **39** | **Convert** | 3A | NewArr | **3B** | **GcPoint** |
| 3C | PrepareRecord | 3D | TodoDelete | **3E** | **Scc32** | **3F** | **Scc64** |
| **40** | **SccImm32** | **41** | **SccImm64** | 42 | InstanceOf | 43 | LoadTypeInfoObj |
| **44** | **RegSymGroup** | **45** | **RegGroup** | 46 | InitObj | **47** | **InitString** |
| 48 | ArrayLength | 49 | ArrayIndexCheck | **4A** | **Float32** | **4B** | **Float64** |
| **4C** | **LoadStackRec** | **4D** | **Nop** | **4E** | **LoadUntyped** | **4F** | **StoreUntyped** |
| **50** | **StoreUntypedImm** | 51 | LoadArray | 52 | StoreArray | 53 | TypeArg |
| 54 | Box | 55 | BoxT | 56 | Unbox | 57 | UnboxT |
| 58 | BoxRec | 59 | UnboxRec | 5A | Offset | 5B | AddOffset |
| 5C | TagGeneric | 5D | PayloadGeneric | 5E | NewNoneGeneric | 5F | NewSomeGeneric |
| **60** | **LoadRawMemory** | **61** | **StoreRawMemory** | 62 | CallInterfGeneric | 63 | AssignGeneric |
| 64 | InstanceOfGeneric | **65** | **AtomicLoad** | **66** | **AtomicStore** | **67** | **CAS** |
| **68** | **Swap** | **69** | **AtomicFetchAdd** | **6A** | **AtomicFetchSub** | **6B** | **AtomicFetchAnd** |
| **6C** | **AtomicFetchOr** | **6D** | **AtomicFetchXor** | 6E | CBinary8 | 6F | CBinary16 |
| 70 | CBinary32 | 71 | CBinary64 | 72 | CBinaryImm8 | 73 | CBinaryImm16 |
| 74 | CBinaryImm32 | 75 | CBinaryImm64 | 76 | Ld | **77** | **LdStatic** |
| 78 | LdTyped | 79 | LdDerived | 7A | LdGeneric | 7B | Lea |
| **7C** | **LeaStatic** | 7D | LeaGeneric | 7E | LeaBox | 7F | St |
| **80** | **StStatic** | 81 | StTyped | 82 | StDerived | 83 | StGeneric |
| **84** | **LoadTailParam** | 85 | Copy | 86 | CopyGeneric | 87 | Index |
| 88 | IndexGeneric | 89 | ZeroValGeneric | **8A** | **FMathUnary32** | **8B** | **FMathUnary64** |
| **8C** | **SBin8** | **8D** | **SBin16** | **8E** | **SBin32** | **8F** | **SBin64** |
| **90** | **SBinImm8** | **91** | **SBinImm16** | **92** | **SBinImm32** | **93** | **SBinImm64** |
| 94 | `_END` (not an instruction) | | | | | | |

`RegSymGroup` (`0x44`) selector, high nibble of the first operand byte:

| Sel | Name | Sel | Name | Sel | Name | Sel | Name |
|---|---|---|---|---|---|---|---|
| 0 | LoadTypeInfoSig | 1 | LoadTypeInfoGeneric | 2 | NewObj | **3** | **CallDirect** |
| 4 | CallVirt | 5 | CallInterf | 6 | Spawn | 7 | SpawnFuture |
| 8 | CallClosure | 9 | NewClosure | 10 | CallClosureGeneric | 11 | NewObjGeneric |
| 12 | NewClosureGeneric | **13** | **LoadFuncPtr** (E5, patched engine only) | | | | |

`RegGroup` (`0x45`) selector:

| Sel | Name | Sel | Name | Sel | Name |
|---|---|---|---|---|---|
| 0 | Ret32 | **1** | **Ret64** | 2 | FRet32 |
| **3** | **FRet64** | 4 | DivCheck | **5** | **Catch** |
| **6** | **Throw** | 7 | RetRef | **8** | **NullCheck** |
| **9** | **CallIndirect** (E6) | **10** | **LeaFrame** (E8) | **11** | **Alloca** (E8) |
| **12** | **StackSave** (E8) | **13** | **StackRestore** (E8) | **14** | **LoadFCB** (E8) |

Selector 13 (`RegSymGroup`) and selectors 9–14 (`RegGroup`) are the accepted engine
extensions of `13-engine-changes.md` (E5, E6, E8). They occupy free values of each group,
so the top-level opcode table is unchanged. An unpatched engine aborts on them in the
group switch (`FATAL("Should not reach here")`) when it rewrites the method.

## 3. Operand enumerations

From `src/cbc/isa.h`.

**Width** (2 bits in BFX flags, otherwise implied by opcode): `W8=0 W16=1 W32=2 W64=3`.

**CC** (4 bits):

| Code | Name | Meaning (integer: on the selected width; float: IEEE) | Usable in |
|---|---|---|---|
| 0 | `eq` | `l == r` | Bcc, BccImm, Scc, SccImm |
| 1 | `ne` | `l != r` | all integer forms |
| 2 | `lt` | signed `<` | all integer forms |
| 3 | `ge` | signed `>=` | all integer forms |
| 4 | `ult` | unsigned `<` | all integer forms |
| 5 | `uge` | unsigned `>=` | all integer forms |
| 6 | `req` | reference equal | Bcc/Scc only (not immediate forms) |
| 7 | `rne` | reference not equal | Bcc/Scc only |
| 8 | `feq` | ordered `==` (`oeq`) | Bcc generic, Scc (FR operands) |
| 9 | `fne` | `!(l == r)` (`une`) | Bcc generic, Scc |
| 10 | `flt` | ordered `<` (`olt`) | Bcc generic, Scc |
| 11 | `fnlt` | `!(l < r)` (`uge`) | Bcc generic, Scc |
| 12 | `fge` | ordered `>=` (`oge`) | Bcc generic, Scc |
| 13 | `fnge` | `!(l >= r)` (`ult`) | Bcc generic, Scc |
| 14 | `z` | `(l & r) == 0` | integer forms |
| 15 | `nz` | `(l & r) != 0` | integer forms |

The immediate forms (`BccImm*`, `SccImm*`) accept only integer codes 0–5 and 14–15;
`req`, `rne` and float codes reach `FATAL("Unreachable")` in the interpreter.

**Common** (4 bits, `Binary*`, `BinaryImm*`): `add=0 sub=1 mul=2 and=3 or=4 xor=5 sdiv=6
srem=7 udiv=8 urem=9 lsr=10 asr=11 lsl=12 pow=13`. Shifts mask the amount to 5 bits (W32)
or 6 bits (W64). `pow` is implemented (wrapping, `PowWrapping`).

**Saturating** (4 bits, `SBin*`, `SBinImm*`): `sadd=0 ssub=1 smul=2 sdiv=3 smod=4 spow=5
sshl=6 sshr=7 suadd=8 susub=9 sumul=10 sudiv=11 sumod=12 sushl=13 sushr=14`. Results clamp
to the operand width (8/16/32/64); `sdiv`/`smod`/`sudiv`/`sumod` by zero assert.

**Checked** (4 bits, `CBinary*`): `cadd cusub …` — raise `OverflowException`; the C target
uses them only for `-ftrapv`.

**FloatOperations** (4 bits): `fadd=0 fsub=1 fmul=2 fdiv=3 fmov=4 fneg=5 fabs=6 fsqrt=7
i2f=8 f2i=9 fpow=10`. `i2f`/`f2i` are raw 64-bit copies between the register files.

**FloatMathOp** (4 bits, `FMathUnary*`): `sin=0 cos=1`.

**ConvertType** (4 bits): `I8=0 U8=1 I16=2 U16=3 I32=4 U32=5 I64=6 U64=7 F16=8 F32=9 F64=10`.

**LoadAccessKind** (4 bits):

| Code | Name | Effect | Interpreter support |
|---|---|---|---|
| 0 | `u8` | zero-extend byte | yes |
| 1 | `u16` | zero-extend halfword | yes |
| 2 | `32` | zero-extend word | yes |
| 3 | `lea` | address instead of value | yes |
| 4 | `s8` | sign-extend byte to 64 | yes |
| 5 | `s16` | sign-extend halfword to 64 | yes |
| 6 | `f32` | load float into FR | yes |
| 7 | `f64` | load double into FR | yes |
| 8 | `u8to64` | — | **FATAL** |
| 9 | `u16to64` | — | **FATAL** |
| 10 | `u32to64` | — | **FATAL** |
| 11 | `64` | doubleword | yes |
| 12 | `s8to64` | — | **FATAL** |
| 13 | `s16to64` | — | **FATAL** |
| 14 | `s32to64` | sign-extend word to 64 | yes |
| 15 | `ref` | reference load (barriered on objects) | not used |

**StoreAccessKind** (4 bits): `8=0 16=1 32=2 64=3 ref=4 special=5 f32=6 f64=7`.

## 4. Encodings of the instructions the C target emits

Notation: `op` = opcode byte; `[a:b]` nibble pair; `d` destination, `l`/`r` sources.

### 4.1 Control flow

| Instruction | Bytes | Semantics |
|---|---|---|
| `Bcc32<cc>` / `Bcc64<cc>` (cc ∈ eq ne lt ge ult uge) | `op [l:r] s16` / `13 op [l:r] s32` | if `l cc r` (32/64-bit integer) then `pc = end + delta` |
| `Bcc32` / `Bcc64` generic | `op [cc:l] [r:dlo] s16 dhi` / `13 op [cc:l] [r:dlo] s32 dhi` | `delta = (dhi << 4) \| dlo`. Required for `z`, `nz`, and every float condition (then `l`, `r` are FRs; width selects f32/f64) |
| `BccImm32` / `BccImm64` | `op [cc:l] sleb(imm) s16` / wide `s32` | compare `l` with the full 64-bit `imm` (no nibble split here) |
| `Jump` | `12 s16` / `13 12 s32` | `pc = end + delta` |
| `Ret64 src` | `45 [1:src]` | moves `src` to `IR1` if needed, returns. Use `src = IR1`. |
| `FRet64 src` | `45 [3:src]` | moves `src` to `FR0` if needed, returns. |

`Ret32`, `RetRef`, `FRet32` behave identically to their 64-bit siblings at run time; the
C target always uses `Ret64` (integer or void) and `FRet64`.

A function returning two integer registers (`{i64, i64}`, `i128`) or an integer and a
float sets `IR2`/`FR1` before `Ret64 IR1`. CBC→CBC returns leave all registers untouched,
so the caller sees them. Never use this for functions that native code could call (none
exist, `01-cbc-platform-facts.md` §10).

### 4.2 Moves and constants

| Instruction | Bytes | Notes |
|---|---|---|
| `Mov64 d, s` | `15 [d:s]` | full 64-bit copy (width ignored by the rewriter) |
| `Mov64i d, imm` | `17 [d:lo4] sleb(imm >> 4)` | defines all 64 bits. RT: `MOVI` if imm fits signed 4 bits, else `addi.64 d, IRZ, imm` (12-bit inline or literal) |
| `FMov64 d, s` | `1A [d:s]` | FR copy |
| `FMov32i d, bits` | `1B [0:d] u32` | raw IEEE single bits |
| `FMov64i d, bits` | `1C [0:d] u64` | raw IEEE double bits |
| `Float64 i2f d(FR), s(IR)` | `4B [8:d] [0:s]` | raw copy IR→FR (bitcast) |
| `Float64 f2i d(IR), s(FR)` | `4B [9:d] [0:s]` | raw copy FR→IR |

`Mov32`, `Mov32i`, `MovRef`, `FMov32` exist but are not needed: `Mov32` is identical to
`Mov64`, and `Mov32i` leaves the upper half undefined.

### 4.3 Integer arithmetic

| Instruction | Bytes | Notes |
|---|---|---|
| `Binary32/64 op, d, l, r` | `35/36 [op:d] [l:r]` | 3-address; covers every `Common` op |
| `Add32..LSL32`, `Add64..LSL64` | `op [d:r]` | 2-address short form, `d = d op r`; only for ops add sub mul and or xor udiv urem lsr asr lsl. Selected by MC-level compression when `d == l` |
| `BinaryImm32/64 op, d, l, imm` | `37/38 [op:d] [l:lo4] sleb(imm >> 4)` | immediate sign-extended to 64 bits |
| `SBin8/16/32/64 op, d, l, r` | `8C..8F [op:d] [l:r]` | saturating |
| `SBinImm8..64 op, d, l, imm` | `90..93 [op:d] [l:lo4] sleb(imm >> 4)` | saturating with immediate |

W32 results leave bits 63..32 undefined (`01-cbc-platform-facts.md` §2.1).

### 4.4 Compare and set

| Instruction | Bytes | Notes |
|---|---|---|
| `Scc32/64 cc, d, l, r` | `3E/3F [cc:d] [l:r]` | `d = (l cc r) ? 1 : 0`, all 64 bits defined. Float `cc` ⇒ `l`, `r` are FRs |
| `SccImm32/64 cc, d, l, imm` | `40/41 [cc:d] [l:lo4] sleb(imm >> 4)` | integer `cc` only. `SccImm64` now decodes as a true 64-bit compare (`isa_opcodes.h`: `SccImm<Width::W64>`) |

### 4.5 Bit-field extract

`BFX d, s, resW, argW, sx, offset, size` → `1E [d:s] b1 b2` with
`b1 = (res64 << 7) | (arg64 << 6) | offset(6 bits)` and `b2 = (sx << 7) | size(7 bits)`.
Result: bits `[offset, offset+size)` of `s`, sign-extended if `sx`, zero-extended
otherwise, into all 64 bits of `d`. `size = 0` gives 0. The width flags are ignored. Use
for `sext_inreg`, `zext` from narrow types, and constant-position bit-field extraction.

### 4.6 Conversion

`Convert to, from, dReg, sReg` → `39 [to:from] [dReg:sReg]`. The source register is an FR
when `from` is F32/F64, else an IR; the destination likewise for `to`. Supported pairs
(`src/interpreter/casts.h`):

| from \ to | I8 | U8 | I16 | U16 | I32 | U32 | I64 | U64 | F32 | F64 |
|---|---|---|---|---|---|---|---|---|---|---|
| **I32** | ✓ sext8 | ✓ zext8 | ✓ sext16 | ✓ zext16 | — | — | ✓ sext32 | — | ✓ | ✓ |
| **U32** | ✓ | ✓ | ✓ | ✓ | — | — | ✓ zext32 | — | ✓ | ✓ |
| **I64** | — | — | — | — | ✓ trunc+sext32 | — | — | — | ✓ | ✓ |
| **U64** | — | — | — | — | ✓ | ✓ trunc+zext32 | — | — | ✓ | ✓ |
| **F32** | — | — | — | — | ✓ | ✓ | ✓ | ✓ | — | ✓ |
| **F64** | — | — | — | — | ✓ | ✓ | ✓ | ✓ | ✓ | — |

Float→int saturates and maps NaN to 0 (undefined in C, harmless). Any other pair (and
anything involving F16) asserts in debug engines and stores 0 in release engines; the
backend never emits one.

### 4.7 Floating point

| Instruction | Bytes | Notes |
|---|---|---|
| `Float32/64 op, d, l, r` (fadd fsub fmul fdiv fpow) | `4A/4B [op:d] [l:r]` | binary |
| `Float32/64 op, d, s` (fneg fabs fsqrt fmov) | `4A/4B [op:d] [0:s]` | unary; **the source is the low nibble** of the second byte |
| `FMathUnary32/64 op, d, s` (sin cos) | `8A/8B [op:0] [d:s]` | `std::sin`/`std::cos` |

Division by zero yields ±inf/NaN, no traps. There is no `frem`, `floor`, `ceil`, `trunc`,
`round`, `fma`, `copysign`, `minnum`; see `06-backend.md` §4.6.

### 4.8 Memory

| Instruction | Bytes | Semantics |
|---|---|---|
| `LoadRawMemory d, base, disp, ldk` | `60 [d:base] [ldk:lo4] sleb(disp >> 4)` | `d = load<ldk>(base + disp)`; `d` is FR for `f32/f64`, IR otherwise |
| `StoreRawMemory s, base, disp, stk` | `61 [s:base] [stk:lo4] sleb(disp >> 4)` | `store<stk>(base + disp, s)` |
| `LoadUntyped d, ldk, slot` | `4E [d:ldk] u16` | frame slot `slot*8`; spill reload |
| `StoreUntyped s, stk, slot` | `4F [s:stk] u16` | spill / outgoing argument (any `u16` slot with E1; an unpatched engine corrupts memory for slot ≥ 512) |
| `StoreUntypedImm imm, slot` | `50 u16 sleb(imm)` | 64-bit store of a constant into a slot (rewriter goes through a memspace sequence) |
| `LoadTailParam d, tail, k, ldk` | `84 [d:tail] [ldk:0] uleb(k)` | `d = load<ldk>(tail + 8k)`; incoming stack argument |
| `LdStatic d, @fref` | `77 [d:x] uleb(fref)` | load a static (the low nibble is ignored; the Cangjie producer repeats `d` there) |
| `StStatic s, @fref` | `80 [s:x] uleb(fref)` | store a static |
| `LeaStatic d, baseRef, @fref` | `7C [d:baseRef] uleb(fref)` | `d = &static`; also overwrites `baseRef` with the engine's "global base" value (`baseRef` ≠ `d`, ≠ `IRZ`, ≠ `IR_ACC`; modelled as a dead definition) |
| `LoadStackRec d, ts` | `4C [d:0] u16` | `d = &typedSlot[ts]`. The frame does not move (`01` §7), so the pointer may be kept across calls |
| `LeaFrame d, disp` (E8) | `45 [10:d] u32` | `d = untypedMemBase + disp`, `disp` little-endian, `0 ≤ disp < untypedMemSize`. Rewriter expands it like `LoadStackRec` (a frame `lea` of `untypedMemOffset + disp`). Not a state point |
| `Alloca d, size, alignLog` (E8) | `45 [11:d] [size:alignLog]` | `d = shadow_alloc(size, 1 << alignLog)`. `alignLog` is 4..12 (alignment 16..4096). Not a state point. See `04` §5.4 |
| `StackSave d` (E8) | `45 [12:d]` | `d` = current shadow cursor, or 0 if the fiber has not allocated yet |
| `StackRestore s` (E8) | `45 [13:s]` | free shadow allocations newer than the token in `s` (`04` §5.4) |
| `LoadFCB d` (E8) | `45 [14:d]` | `d` = this fiber's FCB pointer. Runtime load from the `Ectype`; not a rewrite-time constant |

Raw memory displacement rule (engine quirk Q5): only `0 <= disp < 2^31`.

### 4.9 Calls

| Instruction | Bytes | Semantics |
|---|---|---|
| `CallDirect IR1, @mref` | `44 [3:1] uleb(mref)` | call; result in `IR1`/`FR0` (and `IR2`/`FR1` for CBC callees) |
| `LoadFuncPtr d, @mref` (E5) | `44 [13:d] uleb(mref)` = `44 Dd …` | `d` = function-descriptor address (CBC method) or native address (AOT reference); rewrite-time constant; not a state point |
| `CallIndirect r` (E6) | `45 [9:r]` = `45 9r` | call the address in `r`: null → `NoneValueException`; descriptor → interpreted call (`call.2i`); else native call (`call.2c`); result in `IR1`/`FR0`; state point |

For `CallDirect` the register nibble is the destination the rewriter moves `IR1` into;
always encode `IR1`. `LoadFuncPtr`'s nibble is a real destination (any integer
register except `IRZ`). `CallIndirect`'s nibble is the *target* register (any integer
register except `IRZ`); it has no destination operand.

Disassembly text (engine `IsaDisasm` after E5/E6): `ld.fnptr IR3, @5`,
`call.indirect IR5`.

### 4.10 Exceptions and checks

| Instruction | Bytes | Semantics |
|---|---|---|
| `Catch r` | `45 [5:r]` | `r = IR_ACC` (the engine exception object); optional, anywhere in the handler |
| `Throw r` | `45 [6:r]` | rethrow an exception object |
| `NullCheck IRZ` | `45 [8:0]` | always throws `NoneValueException`: the C target's "raise marker exception" |
| `DivCheck r` | `45 [4:r]` | throws `ArithmeticException` when `r == 0` (sanitizer mode only) |

### 4.11 Atomics (through record fields)

The atomic instructions address `obj + fieldOffset` where the offset comes from a field
reference. The C target defines record types `$cbc:A8`, `$cbc:A16`, `$cbc:A32`,
`$cbc:A64` (kind RECORD, one instance field `v` of type `I8`/`I16`/`I32`/`I64`), so the
field offset is 0 and `obj` can be any raw pointer (`03-cbc-file-format.md` §9.4).

| Instruction | Bytes | Semantics (all `seq_cst`) |
|---|---|---|
| `AtomicLoad d, p, @f` | `65 [d:p] uleb(f)` | `d = atomic_load(p)`, zero-extended |
| `AtomicStore s, p, @f` | `66 [s:p] uleb(f)` | `atomic_store(p, s)` |
| `CAS d, p, exp, new, @f` | `67 [d:p] [exp:new] uleb(f)` | `d = compare_exchange_strong(p, exp, new) ? 1 : 0` — **does not return the old value** |
| `Swap d, p, s, @f` | `68 [d:p] [s:0] uleb(f)` | `d = exchange(p, s)` |
| `AtomicFetch{Add,Sub,And,Or,Xor} d, p, s, @f` | `69..6D [d:p] [s:0] uleb(f)` | `d = fetch_op(p, s)` |

### 4.12 Blob loading

`InitString ts, strOffset` → `47 uleb(strOffset) u16(ts)` (**string offset first**,
verified in `default.cbc`, §10). See `01-cbc-platform-facts.md` §9.

### 4.13 Miscellaneous

`GcPoint` → `3B`. `Nop` → `4D`.

## 5. Instructions the C target does not use, and why

| Instruction(s) | Reason |
|---|---|
| `Ld`/`St`/`Lea`/`LdTyped`/`StTyped`/`LdDerived`/`StDerived`/`Ld/StGeneric`/`Copy*`/`Index*` | Need typed field references and object/record/derived pointer semantics; C memory is raw. |
| `NewObj`/`NewArr`/`Box*`/`Unbox*`/`InstanceOf*`/`TypeArg`/`*Generic` option ops | GC objects and generics. |
| `CallVirt`/`CallInterf` | Cangjie virtual and interface dispatch; C function pointers use `LoadFuncPtr`/`CallIndirect` instead. |
| `LoadArray`/`StoreArray`/`ArrayLength` | Cangjie arrays; index truncated to 32 bits. |
| `Spawn`/`SpawnFuture`/closures | Cangjie fibers and lambdas (possible future thread support, `04-architecture.md` §14). |
| `MovBP`, `PrepareRecord` | Derived-pointer and typed-slot GC bookkeeping. |
| `CBinary*` | Raise exceptions; used only for `-ftrapv`. |
| `InitObj`, `ArrayIndexCheck`, `TodoDelete` | `FATAL("not implemented")` in the rewriter. |
| `WidePrefix` before a non-branch | Harmless but misleading; never emit. |

## 6. Selection of branch forms

| Condition | Encoding chosen |
|---|---|
| integer `eq ne lt ge ult uge`, two registers | specialized `Bcc32xx`/`Bcc64xx` (4 bytes narrow) |
| integer `gt le ugt ule` | specialized form with swapped operands |
| `z`/`nz` (bit test), float conditions | generic `Bcc32`/`Bcc64` (5 bytes narrow) |
| compare against a constant | `BccImm32/64` (`sleb` imm) |
| float `ogt`, `ole`, `ugt`, `ule` | swap operands: `ogt(a,b) = flt(b,a)`, `ole(a,b) = fge(b,a)`, `ugt(a,b) = fnge(b,a)`, `ule(a,b) = fnlt(b,a)` |
| float `one`, `ueq`, `ord`, `uno` | two branches (`ord(a,b) = feq(a,a) && feq(b,b)`, etc.) |

## 7. Branch relaxation

* Narrow forms reach ±32 KiB (specialized, `BccImm`, `Jump`) or ±512 KiB (generic `Bcc`).
* The wide form adds one byte (`13`) and widens the displacement field to 32 bits.
* The MC layer starts with narrow encodings and relaxes to wide ones
  (`CBCAsmBackend::mayNeedRelaxation` / `relaxInstruction`), standard MC fixup
  relaxation. A displacement depends on the sizes of all instructions in between, which
  are known because pool indices are assigned eagerly (`06-backend.md` §8).

## 8. Rewriter expansion and literal-table cost model

The rewriter keeps a per-method literal table of at most 4 096 64-bit entries
(`LIT_TABLE_SIZE`, `src/cbc/isa_rt.h:326`). Overflow is caught only by a debug assert;
release builds truncate the 12-bit index and silently corrupt the method. The backend
must bound literal consumption. Conservative per-instruction costs (assume no
deduplication):

| Original instruction | RT expansion | Literals |
|---|---|---|
| `Mov64i` imm in [-8, 7] | `MOVI` | 0 |
| `Mov64i` imm in [-2048, 2047] | `BINI64I add` | 0 |
| `Mov64i` other | `BINI64L add` | 1 |
| `BinaryImm*`, `SccImm*` imm in [-2048, 2047] | inline | 0 |
| `BinaryImm*`, `SccImm*` other | literal form | 1 |
| `BccImm*` | `BCCI*` with value and offset each inline or literal | 0–2 |
| `Bcc*` with RT displacement outside ±2047 bytes | long form | 1 |
| `Jump` | `JMP32` | 0 |
| `CallDirect` | `call.2i` / `call.2c` | 1 |
| `LoadFuncPtr` (E5) | `movi` or `addi.64 d, IRZ, imm` with the address | 1 (addresses never fit 12 bits) |
| `CallIndirect` (E6) | `call.reg` | 0 |
| `LdStatic`/`StStatic` | `ld.addr`/`st.addr` | 1 |
| `LeaStatic` | `ld.addr.lea` + `movi` (global base) | 1–2 |
| `FMov32i`/`FMov64i` | inline immediate | 0 |
| `InitString` | `string.init` (inline 64-bit pointer) | 0 |
| `LoadRawMemory`/`StoreRawMemory` | `ld.rec`/`st.rec`, long form for disp ≥ 4096 | 0 |
| `LoadUntyped`/`StoreUntyped` | `ld.frame`/`st.frame` | 0 |
| `LeaFrame` (E8) | frame `lea` of `untypedMemOffset + disp`, same as `LoadStackRec` | 0 |
| `Alloca` / `StackSave` / `StackRestore` / `LoadFCB` (E8) | one RT opcode each (`SHADOW_ALLOCA`, `SHADOW_SAVE`, `SHADOW_RESTORE`, `LOAD_FCB`); no literal | 0 |
| `StoreUntypedImm` | memspace sequence with inline immediate | 0 |

`CBCAsmPrinter` sums the worst case per function and reports an error (or triggers the
function-splitting fallback, `06-backend.md` §11) above 3 500.

RT size: most original instructions become one RT instruction of 2–9 bytes. The engine's
interpretation cost is roughly per RT instruction, so instruction count is the primary
cost metric for the backend's cost model.

## 9. Corrections to `cbc-engine-initial-stage/agents/isa_reference.md`

| Topic | `isa_reference.md` says | Source and bytes say |
|---|---|---|
| Opcode numbers from `0x1F` on | e.g. `Add32 = 0x22`, `InitString = 0x4A`, `RegSymGroup = 0x47`, `RegGroup = 0x48` | `Add32 = 0x1F`, `InitString = 0x47`, `RegSymGroup = 0x44`, `RegGroup = 0x45` (§2). Verified on `default.cbc`. |
| `LoadTyped`/`StoreTyped`/`StoreTypedImm`, `LoadStatic`/`StoreStatic`, `LoadField`/`StoreField`, `MemHead*`, MemSpace body opcodes | listed as instructions | Not in `ISA_OPCODES`; their parser functions are dead code. MemSpace parsing no longer exists. |
| New instructions | absent | `Copy`, `CopyGeneric`, `Index`, `IndexGeneric`, `ZeroValGeneric`, `FMathUnary32/64`, `SBin8..64`, `SBinImm8..64` |
| `Common::POW` | aborts | implemented (wrapping) |
| `FloatOperations` | no `fpow` | `fpow = 10` |
| `SccImm64` | decodes as W32 | decodes as W64 |
| `LoadRawMemory` negative offsets | "encodable and supported" | Truncated to `uint32` by `Emitter::LoadRec`; negative displacements address `base + 2^32 - |d|` |
| `InitString` operand order | `ts, offset` | wire order is `uleb(offset)` then `u16(ts)` |
| `LoadStackRec` | `[r:0] u16` | same, opcode `0x4C` |
| 32-bit op results | "stores a 32-bit result" | the upper 32 bits are not defined |
| `LoadFuncPtr`, `CallIndirect` | absent | accepted engine extensions E5/E6, not yet in the source (`13-engine-changes.md`) |
| `LeaFrame`, `Alloca`, `StackSave`, `StackRestore`, `LoadFCB` | absent | accepted engine extension E8, not yet in the source (`13-engine-changes.md` §11) |

## 10. Verified byte samples from `sandbox/default.cbc`

`$P$default._CGP7defaultiiHv` (dasm lines 4790–4799), at file offset `0x920e`:

```
77 11 06        ld.s R1, [@6]          LdStatic d=IR1 (low nibble ignored), fref 6
1e 21 00 08     bfx IR2, IR1, W32, W32, false, 0, 8
01 20 21 00     bcc.W32 ne, IR2, IRZ, +33   Bcc32Ne l=IR2 r=IRZ delta=0x0021
16 11 00        mov.W32 IR1, 1         Mov32i d=IR1 lo4=1 sleb(0)
80 11 06        st.s R1, [@6]          StStatic
44 31 08        call.direct IR1, @8    RegSymGroup sel=3 reg=IR1 mref=8
16 10 00        mov.W32 IR1, 0
80 11 06        st.s R1, [@6]
80 11 07        st.s R1, [@7]
44 31 09 ...    call.direct IR1, @9 .. @13
45 11           ret.W64 IR1            RegGroup sel=1 reg=IR1
```

`$P$default.user.main` (dasm lines 5016–5030), at file offset `0x8fb7`:

```
3c 00 00              prepare.record 0
47 88 9d 02 00 00     initstr 0, #36488     uleb(36488) = 88 9d 02, then u16 ts = 0
4c 20 00 00           ld.stack.rec IR2, 0
44 31 00              call.direct IR1, @0
17 10 01              mov.W64 IR1, 16       lo4 = 0, sleb(1) → (1 << 4) | 0
44 31 01              call.direct IR1, @1
44 08 fc 01           load.ti IR8, @252     RegSymGroup sel=0 reg=IR8 uleb(252)
15 61                 mov.W64 IR6, IR1
4c 10 01 00           ld.stack.rec IR1, 1
1d 21                 mov.base..local       MovBP d=IR2 local=1
17 3a 02              mov.W64 IR3, 42       lo4 = 0xA, sleb(2) → 32 + 10
17 4c 00              mov.W64 IR4, 12
15 58                 mov.W64 IR5, IR8
15 96                 mov.W64 IR9, IR6
44 31 02              call.direct IR1, @2
```

These two samples are the first encoder regression tests for the MC layer
(`11-testing-and-roadmap.md` §2).
