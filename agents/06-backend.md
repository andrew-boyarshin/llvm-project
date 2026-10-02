# 06 — `llvm/lib/Target/CBC`: The Backend

The backend is a conventional SelectionDAG backend for a 64-bit register machine with
two register files, plus three unconventional parts: frames are engine-managed (no
prologue or epilogue at all; the untyped memory block is part of the frame, and dynamic
`alloca` is an instruction the engine frees), the object format is module-level
(tables assembled by the AsmPrinter), and the instruction encoding is variable-length with
LEB128 operands (hand-written encoder/decoder). Models to copy from: **RISC-V** (register
file shape, `X0`-style zero register, compressed-instruction emitter for 2-address forms),
**WebAssembly** (hand-written MC code emitter with LEBs, module-level emission, custom EH
streamer), **SPIR-V** (module analysis in the AsmPrinter, custom object writer).

SelectionDAG is the selector for the first implementation: the operation table in §4.2 is
small, every needed expansion already exists in the DAG legalizer, and the reference
backends above use it. GlobalISel can be added later (`CBCGISel.td` reusing the same
patterns), but nothing in this design depends on it.

## 1. Directory layout

```
llvm/lib/Target/CBC/
  CMakeLists.txt
  CBC.h                         pass factory declarations, CBCStackID, opcode helpers
  CBC.td                        top-level: includes, processors, features, CBCInstrInfo, AsmParser/Writer
  CBCRegisterInfo.td            registers, classes
  CBCCallingConv.td             CC_CBC_*, RetCC_CBC_*, CSR lists
  CBCInstrFormats.td            CBCInst base class, TSFlags (format, ldk/stk, op nibble)
  CBCInstrInfo.td               instructions, operands, SDNodes, patterns
  CBCInstrCompress.td           CompressPat: 3-address → 2-address short forms
  CBCSubtarget.{h,cpp}
  CBCTargetMachine.{h,cpp}      CBCTargetMachine, CBCPassConfig, pipeline
  CBCTargetObjectFile.{h,cpp}   TargetLoweringObjectFileCBC (text only)
  CBCISelLowering.{h,cpp}       CBCTargetLowering
  CBCISelDAGToDAG.cpp           CBCDAGToDAGISel (addressing modes, constants, frame indices)
  CBCFrameLowering.{h,cpp}
  CBCRegisterInfo.{h,cpp}
  CBCInstrInfo.{h,cpp}
  CBCMachineFunctionInfo.{h,cpp}  frame/slot bookkeeping, masks, literal budget, EH info
  CBCAsmPrinter.{h,cpp}         method emission, module builder feeding
  CBCMCInstLower.{h,cpp}        MachineOperand → MCOperand (pool indices)
  CBCModuleAnalysis.{h,cpp}     immutable pass: callee classification, refs, types (SPIR-V style)
  CBCTargetTransformInfo.{h,cpp}
  CBCSelectionDAGInfo.{h,cpp}   memcpy/memset thresholds
  CBCExceptionInfo / CBCException.{h,cpp}   EH table emission (EHStreamer subclass)
  passes (IR):    CBCResolveSymbols, CBCSynthesizeEntry, CBCPrepareFunctionAddresses,
                  CBCNativeCallLegalizer, CBCLowerEH, CBCLowerSjLj, CBCLowerGlobals,
                  CBCInsertSafepoints          (07-ir-passes.md)
  CBCNativeLibc.def             native libc rule table (10-linker-and-runtime.md §5.4)
  passes (MIR):   CBCLandingPadFixup, CBCPeephole, CBCLiteralBudget, CBCVerifyFrame
  MCTargetDesc/
    CBCMCTargetDesc.{h,cpp}     registration
    CBCMCAsmInfo.{h,cpp}
    CBCMCCodeEmitter.cpp        hand-written variable-length encoder
    CBCAsmBackend.cpp           branch relaxation, fixups
    CBCFixupKinds.h
    CBCInstPrinter.{h,cpp}
    CBCTargetStreamer.{h,cpp}   directive printing (asm) / module builder (obj)
    CBCModuleBuilder.{h,cpp}    strings, terms, refs, AOT, types, fields, methods
    CBCObjectWriter.cpp         .cbc serializer (03-cbc-file-format.md)
    CBCBaseInfo.h               format enum, CC/ldk/stk/op enums (mirror of isa.h)
  AsmParser/CBCAsmParser.cpp
  Disassembler/CBCDisassembler.cpp
  TargetInfo/CBCTargetInfo.{h,cpp}   getTheCBCTarget()
```

## 2. Subtarget and target machine

* `CBCSubtarget(TT, CPU, FS)`: features derived from the triple, not from `-mattr`:
  `HostX86_64` / `HostAArch64` (exactly one), `HostApple` for Darwin variadic and stack
  rules. CPU `generic` only. Optional features: `loop-safepoints` (default on),
  `trapv` (checked arithmetic).
* **Work scope:** only `HostX86_64` (Linux) is implemented in the current plan. The
  AArch64 column below is the design for the follow-up flavour (`04-architecture.md` §2);
  until then `CBCTargetMachine` rejects the AArch64 sub-architecture with a clear error,
  and the AArch64-specific code paths (calling convention tables, `AltOrders`, HFA
  handling) are written only when the follow-up starts.
* Derived constants exposed by the subtarget:

| Constant | x86-64 flavour | AArch64 flavour |
|---|---|---|
| integer argument registers | `IR1..IR6` | `IR1..IR8` |
| float argument registers | `FR0..FR7` | `FR0..FR7` |
| tail register | `IR7` | `IR10` |
| sret register | `IR1` (shifts integer args) | `IR9` |
| callee-saved GPRs | `IR8..IR12` | `IR11, IR12` |
* `CBCTargetMachine`: `LLVMTargetMachine` with `getObjFileLowering()` returning
  `TargetLoweringObjectFileCBC`; `Options.ExceptionModel = ExceptionHandling::CBC` (or
  `None` with the IR-level lowering of `08-exceptions-and-sjlj.md` §4.2); 
  `setRequiresStructuredCFG(false)`; `usesPhysRegsForValues() = true`;
  `isMachineVerifierClean() = true`.
* Reject triples with `NoSubArch` (`report_fatal_error("cbc triple needs a host flavour: cbc_x86_64 or cbc_aarch64")`),
  and, in the current scope, `CBCSubArch_aarch64` (`report_fatal_error("the AArch64 flavour of the CBC target is not implemented yet")`).

## 3. Registers (`CBCRegisterInfo.td`)

```tablegen
class CBCReg<bits<16> Enc, string n, list<string> alt = []> : Register<n, alt> {
  let HWEncoding = Enc;
  let Namespace = "CBC";
}
def IRZ   : CBCReg<0, "irz", ["ir0"]>;
foreach I = 1-13 in def IR#I : CBCReg<I, "ir"#I>;
def IRACC : CBCReg<14, "iracc">;
foreach I = 0-15 in def FR#I : CBCReg<I, "fr"#I>;

// Integer registers hold i32 and i64; i32 values have undefined bits 63..32.
def GPR   : RegisterClass<"CBC", [i64, i32], 64,
              (add IR1, IR2, IR3, IR4, IR5, IR6, IR7, IR8, IR9, IR10,  // volatile first
                   IR11, IR12, IR13, IRACC)>;                          // IRACC reserved
def GPRZ  : RegisterClass<"CBC", [i64, i32], 64, (add GPR, IRZ)>;     // source operands only
def GPRCallTarget : RegisterClass<"CBC", [i64], 64, (sub GPR, IRACC)>; // call.indirect target
def FPR   : RegisterClass<"CBC", [f64, f32], 64, (sequence "FR%u", 0, 15)>;
```

Notes:

* The allocation order puts argument/volatile registers first; the AArch64 flavour
  overrides the order with `AltOrders`/`AltOrderSelect` so that `IR11, IR12, IR13` come last.
* `IRZ` and `IRACC` are reserved (`getReservedRegs`). `IRZ` appears only as a source
  (compare against zero, `addi rd, irz, imm` is never needed since `Mov64i` exists).
  `IR13` is an ordinary callee-saved register (mask bit 5). `IRACC` is never allocated
  because the engine writes it behind the compiler's back: on exception delivery (the
  exception object) and in a callee's prologue when the fiber stack grows
  (`.Lstack_overflow` stores `rsp` into it).
  The backend reads it only through `llvm.cbc.catch` (`Catch Rd`) and never writes it.
* Spill size is 64 bits for all classes (one untyped slot).
* No sub-registers: `trunc i64→i32` and `anyext i32→i64` are `COPY`s inside `GPR`
  (same physical register). LLVM's `isTruncateFree(i64, i32) = true`,
  `isZExtFree(i32, i64) = false`.

## 4. Instruction selection

### 4.1 Legal types and promotion

* Legal: `i32`, `i64` (GPR), `f32`, `f64` (FPR).
* `i1`, `i8`, `i16`: promoted to `i32`. Booleans are `ZeroOrOneBooleanContent`.
* `i128`: expanded to `i64` pairs.
* `f16`: `softPromoteHalfType() = true` (storage as `i16`, arithmetic through `f32`
  libcalls `__extendhfsf2`/`__truncsfhf2`).
* `f80`, `f128`, `ppcf128`: not legal; clang never produces them for CBC (`long double` is
  `double`).
* Vectors: none legal; all vector operations are scalarized/split by type legalization.
  `TTI` reports zero vector registers so the vectorizers stay off.

### 4.2 Operation actions (`CBCTargetLowering` constructor)

| ISD node | i32 | i64 | Lowering |
|---|---|---|---|
| `ADD SUB MUL AND OR XOR SHL SRL SRA SDIV UDIV SREM UREM` | Legal | Legal | `Binary32/64`, `BinaryImm32/64` |
| `SDIVREM UDIVREM` | Expand | Expand | div + rem |
| `MULHS MULHU SMUL_LOHI UMUL_LOHI` | Expand | Expand | half-width expansion |
| `ROTL ROTR` | Expand | Expand | shifts + or |
| `BSWAP BITREVERSE CTPOP CTLZ CTTZ` (+`_ZERO_UNDEF`) | Expand | Expand | bit tricks; `CTPOP` via the SWAR sequence |
| `SIGN_EXTEND_INREG` (i1, i8, i16, i32) | Legal | Legal | `BFX sx` (`i32` in `i64`: `Convert I64, I32` is 1 byte shorter; either) |
| `ZERO_EXTEND` i32→i64 | — | Custom/pattern | `Convert I64, U32` or `BFX zx 0, 32` |
| `ANY_EXTEND` i32→i64, `TRUNCATE` i64→i32 | — | pattern | `COPY` |
| `SETCC` | Legal | Legal | `Scc32/64`, `SccImm32/64`; `gt le ugt ule` by operand swap |
| `SELECT` | Custom | Custom | `SELECT_GPR`/`SELECT_FPR` pseudos with a custom inserter (branch diamond) |
| `SELECT_CC`, `BRCOND` | Expand | Expand | → `SETCC` + `SELECT`, → `BR_CC` |
| `BR_CC` | Legal | Legal | `Bcc*`, `BccImm*`, compare with `IRZ` for 0 |
| `BR_JT`, `JumpTable`, `BRIND` | Expand | — | `areJTsAllowed()` returns false; `indirectbr` removed by `IndirectBrExpandPass` |
| `UADDO USUBO UADDO_CARRY USUBO_CARRY SADDO SSUBO SMULO UMULO` | Expand | Expand | generic expansions (needed for `i128` add/sub) |
| `SADDSAT UADDSAT SSUBSAT USUBSAT SSHLSAT USHLSAT` | Legal | Legal | `SBin32/64` (`i8`/`i16`: Custom to `SBin8/16` on the promoted operands) |
| `ABS SMIN SMAX UMIN UMAX` | Expand | Expand | compare + select |
| `Constant` | Legal | Legal | `Mov64i` (0 → copy of `IRZ`) |
| `GlobalAddress` | — | Custom | **function** used as a value (CBC function or native function, `04-architecture.md` §7.1): `LOAD_FNPTR Rd, @method` (E5), which yields the descriptor address of a CBC function or the real address of a native one. **Data**: never reaches ISel (`CBCLowerGlobals`); a stray one is a fatal error. (In the deferred module-local mode data addresses would arrive only as `llvm.cbc.image.addr.of(@g)`, §9.) |
| `ExternalSymbol` | — | Custom | libcall target → method reference (§8.3) |
| `FrameIndex` | — | Custom | object in the untyped memory block → `LEA_FRAME Rd, offset` |
| `DYNAMIC_STACKALLOC` | — | Custom | `ALLOCA Rd, Rsize, alignLog`. Not a frame object (`04-architecture.md` §5.4) |
| `STACKSAVE STACKRESTORE` | — | Custom | `STACKSAVE` / `STACKRESTORE` |
| `FRAMEADDR` | — | Custom | `LEA_FRAME` of 0 when the block exists, 0 otherwise; depth ≠ 0 is 0 |
| `RETURNADDR` | — | Custom | 0 |
| `VASTART VAARG VACOPY VAEND` | — | — | removed by `ExpandVariadics`; leftovers are a fatal error |
| `TRAP DEBUGTRAP UBSANTRAP` | — | Custom | native call to `abort` |
| `ATOMIC_LOAD ATOMIC_STORE` (8/16/32/64) | Legal | Legal | `AtomicLoad`/`AtomicStore` with `$cbc:A*` field refs |
| `ATOMIC_SWAP ATOMIC_LOAD_{ADD,SUB,AND,OR,XOR}` | Legal | Legal | `Swap`, `AtomicFetch*` |
| `ATOMIC_LOAD_{NAND,MIN,MAX,UMIN,UMAX,FADD,FSUB,UINC_WRAP,UDEC_WRAP}` | — | — | `AtomicExpandPass` CAS loop (`shouldExpandAtomicRMWInIR` → `CmpXChg`) |
| `ATOMIC_CMP_SWAP_WITH_SUCCESS` | — | — | expanded in IR (`shouldExpandAtomicCmpXchgInIR` → `CustomExpand`, `07-ir-passes.md` §9) |
| `ATOMIC_FENCE` | — | Custom | `AtomicFetchAdd` 0 on the fence word |
| `PREFETCH` | — | Expand | dropped |
| `READCYCLECOUNTER`, `READSTEADYCOUNTER` | — | Custom | 0 |
| `MEMCPY MEMMOVE MEMSET` | — | — | inline for small constant sizes (`MaxStoresPerMem*` = 4), else native libcall |

Floating point (`f32`, `f64`):

| ISD node | Action | Lowering |
|---|---|---|
| `FADD FSUB FMUL FDIV FNEG FABS FSQRT` | Legal | `Float32/64` |
| `FSIN FCOS` | Legal | `FMathUnary32/64` |
| `FPOW` | Legal | `Float32/64 fpow` |
| `FREM` | LibCall | `fmod`/`fmodf` (native) |
| `FMA` | LibCall | `fma`/`fmaf` |
| `FMAD` | Expand | mul + add |
| `FCOPYSIGN` | Expand | via `f2i`/`i2f` bit operations |
| `FFLOOR FCEIL FTRUNC FRINT FNEARBYINT FROUND FROUNDEVEN FEXP FEXP2 FEXP10 FLOG FLOG2 FLOG10 FTAN FASIN FACOS FATAN FATAN2 FSINH FCOSH FTANH LROUND LLROUND LRINT LLRINT` | LibCall | native libm |
| `FMINNUM FMAXNUM` | LibCall | `fmin`/`fmax` |
| `FMINIMUM FMAXIMUM FMINIMUMNUM FMAXIMUMNUM` | Expand | |
| `FP_EXTEND` f32→f64, `FP_ROUND` f64→f32 | Legal | `Convert F64,F32` / `Convert F32,F64` |
| `SINT_TO_FP UINT_TO_FP` from i32/i64 | Legal | `Convert F*, I32/U32/I64/U64` |
| `FP_TO_SINT FP_TO_UINT` to i32/i64 | Legal | `Convert I32/U32/I64/U64, F*` |
| `FP_TO_SINT_SAT FP_TO_UINT_SAT` | Expand | |
| `BITCAST` i64↔f64, i32↔f32 | Legal | `Float64 i2f`/`f2i` (raw 64-bit copy) |
| `SETCC` | Legal / Custom | `oeq olt oge une uge ult` direct; `ogt ole ugt ule` swapped; `one ueq ord uno` Custom (two compares) |
| `BR_CC` | Legal / Custom | generic `Bcc32/64` with float `cc`; same expansions |
| `ConstantFP` | Legal | `FMov32i`/`FMov64i` raw bits (`isFPImmLegal` = true) |
| `STRICT_*` | Expand to non-strict | no FP environment |

### 4.3 Patterns (selection)

Integer arithmetic (illustrative; one multiclass per `Common` op):

```tablegen
multiclass BinOp<bits<4> op, SDPatternOperator node, string mn> {
  def 32rr : CBCInstBin<0x35, op, (outs GPR:$d), (ins GPR:$l, GPR:$r), mn#".w32\t$d, $l, $r",
             [(set i32:$d, (node i32:$l, i32:$r))]>;
  def 64rr : CBCInstBin<0x36, op, (outs GPR:$d), (ins GPR:$l, GPR:$r), mn#".w64\t$d, $l, $r",
             [(set i64:$d, (node i64:$l, i64:$r))]>;
  def 32ri : CBCInstBinImm<0x37, op, (outs GPR:$d), (ins GPR:$l, simm64:$i), mn#"i.w32\t$d, $l, $i",
             [(set i32:$d, (node i32:$l, imm:$i))]>;
  def 64ri : CBCInstBinImm<0x38, op, (outs GPR:$d), (ins GPR:$l, simm64:$i), mn#"i.w64\t$d, $l, $i",
             [(set i64:$d, (node i64:$l, imm:$i))]>;
}
defm ADD : BinOp<0, add, "add">;   defm SUB : BinOp<1, sub, "sub">;
defm MUL : BinOp<2, mul, "mul">;   defm AND : BinOp<3, and, "and">;
defm OR  : BinOp<4, or,  "or">;    defm XOR : BinOp<5, xor, "xor">;
defm SDIV: BinOp<6, sdiv,"sdiv">;  defm SREM: BinOp<7, srem,"srem">;
defm UDIV: BinOp<8, udiv,"udiv">;  defm UREM: BinOp<9, urem,"urem">;
defm SRL : BinOp<10, srl, "lsr">;  defm SRA : BinOp<11, sra, "asr">;
defm SHL : BinOp<12, shl, "lsl">;
```

* Immediates: any 64-bit value is encodable (`sleb`). Prefer values in [-2048, 2047]
  (no RT literal). `sub x, C` → `addi x, -C`.
* Constants: `(i64 imm)` → `MOVi64`; `(i64 0)` → `COPY IRZ`; `(i32 imm)` → `MOVi64`
  (the upper half is don't-care for `i32`).
* `(sext_inreg x, i8)` → `BFX x, sx, 0, 8`; `(and x, 0xff)` → `BFX x, zx, 0, 8` (no
  literal, one instruction) — prefer over `andi` for masks `2^k - 1`; `(srl (shl x, a), b)`
  forms and `(and (srl x, s), mask)` → `BFX` with offset/size.
* `(zext i32 → i64)` → `CVT_I64_U32`; `(sext i32 → i64)` → `CVT_I64_I32`.
* Compare-and-branch: `(brcc seteq, x, 0, bb)` → `BCC64EQ x, IRZ, bb`;
  `(brcc setlt, x, C, bb)` → `BCCI64 lt, x, C, bb`; `(brcc setne, (and x, y), 0, bb)` →
  `BCC64 nz, x, y, bb` (generic form with `TESTNZ`).
* `setcc` producing a value: `SCC64 cc, d, l, r`; with an immediate `SCCI64`.
* Loads (raw): `(i64 (load (add base, uimm31:$d)))` → `LDRAW_64 base, $d`; zextload i8
  → `LDRAW_U8`; sextload i8 → `LDRAW_S8`; i16 likewise; `(i32 load)` and
  `(zextload i32 → i64)` → `LDRAW_32`; `(sextload i32 → i64)` → `LDRAW_S32`; `f32` →
  `LDRAW_F32`; `f64` → `LDRAW_F64`; extload (anyext) uses the zero-extending form.
* Stores: `truncstore i8/i16/i32` → `STRAW_8/16/32`; `i64` → `STRAW_64`; floats →
  `STRAW_F32/F64`.
* Static addresses: `LEA_IMAGE` and `LEA_NATIVE` are defined with **two** outputs,
  `(outs GPR:$d, GPR:$baseref)`, the second always dead. Two defs of one instruction always
  receive different registers, which satisfies the engine's `baseRef ≠ dst` requirement,
  and the clobber is visible to the register allocator (unlike a reserved register).
  `LEA_NATIVE` is for native **data** symbols only. `LEA_IMAGE` takes an image number
  operand (`(ins i32imm:$img)`: 0 = the program's image in phase 1, or the module's own,
  aligned or group image in module-local mode, `07-ir-passes.md` §3.6).
* Deferred phase 2 only (not implemented in the current scope):
  `LEA_IMAGEOF (outs GPR:$d, GPR:$baseref), (ins cbcdatasym:$g)` gives the
  base of "the image holding `g`" (or `g`'s AOT static if `g` is native data); the address
  of `g + c` is that base plus the displacement operand `IMGOFF(g) + c`, an
  `MO_CBC_IMGOFF` target-global-address operand. `SelectAddrRegImm` folds it into the
  displacement of raw loads and stores exactly like a constant; when the address itself is
  needed it is materialized as `ADDI64ri $d, $base, IMGOFF(g) + c`. `LEA_IMAGEOF` is
  rematerializable like `LEA_IMAGE`; two uses of the same `g` share one base, uses of
  different external symbols do not (their images are unknown until link time).
* Function addresses (E5): `LOAD_FNPTR (outs GPR:$d), (ins cbcmethod:$m)`, pattern
  `(i64 (CBCISD::FNADDR tglobaladdr:$m))` produced by the custom `GlobalAddress` lowering.
  The operand is the function's method reference (CBC method or `AOT` native). The value is
  a rewrite-time constant: `isReMaterializable`, `isAsCheapAsAMove`, no side effects, no
  memory access, not a state point. CSE across a function is free; rematerialization
  near each use is preferred to keeping the value live across calls.

### 4.4 Addressing mode (`CBCDAGToDAGISel::SelectAddrRegImm`)

`(add base, C)` with `0 <= C < 2^31` folds into the displacement; any other form
(negative, large, `or` that is not disjoint) is computed into a register first. A frame
index is `lea.frame` of the object's offset, folded into a raw displacement when the
use is a load or store (`06` §6.4). There is no register+register addressing.

### 4.5 `select`

`SELECT_GPR`, `SELECT_FPR` pseudos with `usesCustomInserter = 1`:

```
  BCC64NE cond, IRZ, tbb      ; or the original compare if cond is a setcc
fbb: d = f ; JMP join
tbb: d = t
join:
```

`EmitInstrWithCustomInserter` builds the diamond (template: `MSP430`/`AVR`
`EmitInstrWithCustomInserter`). For `select (setcc a, b, cc), t, f` the compare is folded
into the branch. An arithmetic form (`d = f ^ ((t ^ f) & -c)`, 3 instructions, no branch)
is selected when both values are already in registers and the target cost model prefers it
(`isSelectSupported`/`shouldConvertSelectToBranch`: the interpreter has no branch
misprediction, so the diamond is usually as cheap).

### 4.6 Floating-point specifics

* `fneg` is `Float fneg` (exact sign flip, works on NaN).
* `copysign(x, y)`: `f2i` both, `and`/`or` the sign bit, `i2f`.
* `f32 ↔ i32` bitcast: `f2i`/`i2f` copy 64 bits; the low 32 are the payload, the high 32
  undefined, consistent with the `i32` convention.
* `fp_to_sint` of `f32/f64` to `i8`/`i16`: through `i32`.
* There is no `F16` path in `Convert`; half is software.

## 5. Calling convention lowering

### 5.1 `CBCCallingConv.td`

```tablegen
def CC_CBC_X64 : CallingConv<[
  CCIfSRet<CCAssignToReg<[IR1]>>,
  CCIfType<[i1, i8, i16], CCPromoteToType<i32>>,
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2, IR3, IR4, IR5, IR6]>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1, FR2, FR3, FR4, FR5, FR6, FR7]>>,
  CCIfByVal<CCPassByVal<8, 8>>,
  CCIfType<[i32, i64, f32, f64], CCAssignToStack<8, 8>>
]>;
def CC_CBC_A64 : CallingConv<[
  CCIfSRet<CCAssignToReg<[IR9]>>,
  CCIfType<[i1, i8, i16], CCPromoteToType<i32>>,
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2, IR3, IR4, IR5, IR6, IR7, IR8]>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1, FR2, FR3, FR4, FR5, FR6, FR7]>>,
  CCIfByVal<CCPassByVal<8, 8>>,          // only for clang-produced byval (AAPCS64 uses indirect)
  CCIfType<[i32, i64, f32, f64], CCAssignToStack<8, 8>>
]>;
def RetCC_CBC : CallingConv<[
  CCIfType<[i1, i8, i16], CCPromoteToType<i32>>,
  CCIfType<[i32, i64], CCAssignToReg<[IR1, IR2]>>,
  CCIfType<[f32, f64], CCAssignToReg<[FR0, FR1]>>
]>;
def CSR_CBC_X64 : CalleeSavedRegs<(add IR8, IR9, IR10, IR11, IR12, IR13,
                                       (sequence "FR%u", 8, 15))>;
def CSR_CBC_A64 : CalleeSavedRegs<(add IR11, IR12, IR13, (sequence "FR%u", 8, 15))>;
def CSR_CBC_Native : CalleeSavedRegs<(add (sub GPR, IR1), (sub FPR, FR0))>;  // regmask for native calls
```

Notes:

* `i128` arrives as two `i64` (split by type legalization) and takes two consecutive
  integer registers (SysV) or an even/odd pair (AAPCS64: `CCIfConsecutiveRegs` +
  alignment rule, as `AArch64CallingConvention.td` does).
* Struct arguments are already coerced by clang (`09-clang.md` §3); `byval` appears only
  for x86-64 MEMORY-class aggregates and is copied into the outgoing area (8-byte slots).
* Stack slots of 16-byte-aligned values start at even slots (`CCAssignToStack<16, 16>`
  for `i128` pieces marked by `CCIfSplit`).

### 5.2 `LowerFormalArguments`

* Register arguments: `CopyFromReg` from the assigned registers (live-ins).
* Stack arguments: for each stack location `k`, emit **in the entry block**
  `LDTAIL_64 vreg, TAIL, k` (or the `_F64` form). The chain is glued so that all of them
  precede any call. The tail register is a live-in copied once into a vreg.
* `byval` arguments on the stack: allocate an object in the untyped memory block of the
  aggregate's size and alignment, `memcpy`-expand (word copies from `TAIL + 8k` into that
  object), and use the object's address as the argument value. The copy is in the entry
  block, before the first call, because the tail register is then overwritten.
* The function's CBC signature term is recorded in `CBCMachineFunctionInfo`: exactly the
  lowered parameter classes in order (integer → `I64`, `float` → `F32`, `double` → `F64`),
  no padding (E2 makes the engine's `hasTailReg` correct for float stack parameters,
  `01-cbc-platform-facts.md` §3.4).

### 5.3 `LowerCall`

1. Classify the callee: CBC method, native (`cbc_nativecc` or a declaration resolved as
   native by `CBCModuleAnalysis`), or **indirect** (the callee is not a
   `GlobalAddress`/`ExternalSymbol`). Every indirect call in the IR reaches ISel unchanged
   (there is no IR-level indirect-call rewriting); its target is a CBC function's
   descriptor or a native function's address.
2. Assign arguments with the flavour's `CC_CBC_*`. For an indirect call the callee value is an ordinary virtual register
   operand constrained to `GPRCallTarget` = `GPR` minus `IRACC` (and `IRZ`, which
   is not in `GPR`); the register allocator may pick an argument register only if the value
   is not also an argument, which `call.indirect` tolerates because the target is read
   before the arguments are loaded.
3. Stack arguments: `STU_64 val, k` / `STU_F64 val, k` into outgoing slot `k`
   (`STU` with an *immediate slot index*, not a frame index). Record
   `MaxOutgoingSlots = max(MaxOutgoingSlots, n)` in `CBCMachineFunctionInfo`. byval:
   word-by-word copy from the source address into consecutive outgoing slots.
4. Emit `CBCISD::CALL` (CBC callee), `CBCISD::CALL_NATIVE`, or
   `CBCISD::CALL_INDIRECT` (E6). Direct forms take the callee as a
   `TargetGlobalAddress`/`TargetExternalSymbol` operand; `CALL_INDIRECT` takes the target
   register. Argument registers are uses. Regmask: `CSR_CBC_X64`/`CSR_CBC_A64` for CBC
   callees **and indirect calls** (the target may be a CBC function);
   `CSR_CBC_Native` for direct native calls. The tail register is an implicit def of
   `CALL` and `CALL_INDIRECT` (the engine overwrites it on CBC→CBC calls).
5. Results: `CopyFromReg` from `IR1`/`IR2`/`FR0`/`FR1`. For `CALL_NATIVE`, a second result
   register is an error (`DiagnosticInfoUnsupported`: "native function '%s' returns a
   value in two registers; the CBC engine copies back only one"). For `CALL_INDIRECT` a
   second result register is allowed (correct for CBC targets); the compiler has already
   rejected programs that take the address of a native function returning two registers
   (`04-architecture.md` §9.6, `07-ir-passes.md` §4).
6. No `CALLSEQ_START`/`CALLSEQ_END` stack adjustment code; the pseudos are emitted only to
   carry `MaxCallFrameSize` and are erased by `eliminateCallFramePseudoInstr`.
7. Variadic native calls, including those with floating-point variadic arguments: same as
   a normal call; the host ABI puts unnamed arguments in the same places as named ones on
   x86-64 SysV (and on AArch64 Linux, follow-up flavour). `al` needs no code: the engine's
   native-call adapter sets it to 8 (E7, `13-engine-changes.md` §10). Apple arm64 (not
   planned): unnamed arguments would always go to stack slots
   (`CCIfNotFixed<CCAssignToStack<8, 8>>`).
8. `isTailCall` is always false; `musttail` → `report_fatal_error`.

### 5.4 `LowerReturn`

Copy values to `IR1`/`IR2`/`FR0`/`FR1` per `RetCC_CBC`, then `CBCISD::RET` (`Ret64 IR1`,
or `FRet64 FR0` when the only result is a float). For `void`, `Ret64 IR1`. The return
has no stack adjustment. The engine frees this frame's dynamic allocations (§6.3).

### 5.5 Calls that are state points

`CALL`, `CALL_NATIVE`, `CALL_INDIRECT`, `CALL_VIRT`, `GCPOINT` MachineInstrs have the target flag
`CBCII::IsStatePoint`. The AsmPrinter emits a temporary label right after each of them
and registers it as a liveness position (§8.5).

## 6. Frame lowering (`CBCFrameLowering`)

### 6.1 Two stack ids

| Stack id | Objects | Address | Laid out by |
|---|---|---|---|
| `TargetStackID::Default` | static allocas, byval copies, sret buffers, variadic buffers | `lea.frame` + offset inside the untyped memory block | PEI (`calculateFrameObjectOffsets`) |
| `CBCStackID::EngineFrame` (= `TargetStackID::NoAlloc`-style custom id) | register-allocator spill slots | untyped slot index | `CBCFrameLowering::processFunctionBeforeFrameFinalized` |

Dynamic `alloca` is not a frame object. `LowerOperation` replaces
`DYNAMIC_STACKALLOC`, `STACKSAVE` and `STACKRESTORE` with the E8 instructions before a
variable-sized object is created, so `hasVarSizedObjects()` stays false and `hasFP`
stays false. Fixed objects do not move when `alloca` runs.

`processFunctionBeforeFrameFinalized` walks `MFI`, moves every spill slot
(`MFI.isSpillSlotObjectIndex(FI)`) to `EngineFrame`, and assigns slot numbers
`MaxOutgoingSlots, MaxOutgoingSlots + 1, …` (8 bytes each; a 16-byte spill takes two).
`isSupportedStackID(EngineFrame)` returns true; PEI then ignores those objects. Its
`StackSize`, rounded up to 16, is `untypedMemSize`.

`untypedSlotCount = MaxOutgoingSlots + spillSlots`, at most 65 535 (the `u16` slot operand;
`CBCVerifyFrame` reports an error beyond it). With E1 every slot is storable; on an
unpatched engine only slots below 512 would be (`01-cbc-platform-facts.md` Q4), which this
backend does not support. `usesAlloca` is 1 when the function contains `ALLOCA`,
`STACKSAVE` or `STACKRESTORE`.

### 6.2 Prologue and epilogue

Both are empty. The engine allocates and zeroes the untyped memory block as part of the
frame (`04-architecture.md` §5.2). For `usesAlloca = 1` it also saves the shadow cursor
in the engine-private slot and restores it on return and on unwind (`04-architecture.md`
§5.5). Callee-saved registers are saved by the engine. There is no frame pointer.

### 6.3 Callee-saved registers

* `determineCalleeSaves` runs normally; `SavedRegs` are the CSRs the function modifies.
* `spillCalleeSavedRegisters` / `restoreCalleeSavedRegisters` emit **nothing** and return
  true.
* `CBCMachineFunctionInfo::computeNonVolMasks()` converts `SavedRegs` to
  `usedNonVolIRegMask` (bit `r - IR8` for `IR8..IR13`) and `usedNonVolFRegMask` (bit
  `r - FR8`).

### 6.4 Frame index elimination (`CBCRegisterInfo::eliminateFrameIndex`)

* `Default` object: the address is `LEA_FRAME` of the object's offset. A raw load or
  store that already has the address in a register keeps its displacement; the offset is
  folded into that displacement when the sum is in `[0, 2^31)`. An object with alignment
  above 16 is materialized and then aligned with `add` / `and`
  (`04-architecture.md` §5.2).
* `EngineFrame` object in `STU_*`/`LDU_*`: replace the frame index with the slot number.
* No other instruction may reference an `EngineFrame` index.

### 6.5 Call frames

`hasReservedCallFrame` true; outgoing arguments are written to slots `0..k-1` directly
(§5.3). `eliminateCallFramePseudoInstr` erases `ADJCALLSTACKDOWN/UP`. Those slots are
not part of `untypedMemSize`.

## 7. `CBCInstrInfo`

| Hook | Implementation |
|---|---|
| `copyPhysReg` | GPR→GPR `MOV64rr`; FPR→FPR `FMOV64rr`; GPR→FPR `I2F64`; FPR→GPR `F2I64`; from `IRZ` → `MOV64rr d, irz` (encodes as `mov.W64 d, IR0`) |
| `storeRegToStackSlot` / `loadRegFromStackSlot` | GPR: `STU_64`/`LDU_64`; FPR: `STU_F64`/`LDU_F64` (raw 64 bits, preserves `f32` payload); frame index operand (an `EngineFrame` object) |
| `isLoadFromStackSlot` / `isStoreToStackSlot` | recognise the four forms |
| `analyzeBranch` | conditional: all `BCC*` forms with operands (cc, l, r/imm); unconditional `JMP`; no indirect |
| `insertBranch` / `removeBranch` | standard |
| `reverseBranchCondition` | `cc ^ 1` (`CC::Negated`: eq↔ne, lt↔ge, ult↔uge, req↔rne, feq↔fne, flt↔fnlt, fge↔fnge, z↔nz — all exact inverses including NaN behaviour) |
| `isBranchOffsetInRange` | always true (MC relaxes); `BranchRelaxation` pass not used |
| `getInstSizeInBytes` | narrow encoding size, LEB sizes from operand values (used by the literal-budget and size heuristics) |
| `isReMaterializableImpl` / `isAsCheapAsAMove` | `MOVi64`, `FMOVi32/64`, `LOAD_FNPTR`, `LEA_IMAGE`, `LEA_NATIVE` (the second, dead definition of the `LEA_*` forms is a virtual register; when rematerialized the allocator gives it any free register) |
| `expandPostRAPseudo` | `SELECT` leftovers (none) |
| `getRegClass`/`getCommutableOperands` | standard; `ADD/MUL/AND/OR/XOR` commutable |
| `isCopyInstrImpl` | `MOV64rr`, `FMOV64rr` |

## 8. AsmPrinter and the module builder

### 8.1 Emission model

* `CBCAsmPrinter::runOnMachineFunction` emits one method per function. Each function's
  instructions go into its own code section `cbc.code.<method>`; method metadata is
  recorded through the target streamer:

```
	.cbc_method	"__cbc_main", "(I64, I64) -> I64", flags=0x5
	.cbc_frame	untyped=4, outargs=1, inonvol=0x03, fnonvol=0x00, maxstackargs=1, native=1
	.cbc_typed	"(U64, U64)"                  ; typed slots (startup method only)
__cbc_main:
	...
	.cbc_end_method
```

* There is no data emission: `emitGlobalVariable` asserts (all globals were lowered).
* `CBCModuleAnalysis` (an immutable pass run before the AsmPrinter, SPIR-V style) records
  for each callee name whether it is a CBC method or a native symbol, the program type
  names, the image size and the blobs. Function pointers need no module-level table: each
  `LOAD_FNPTR` is just a method reference (`04-architecture.md` §7.1).

### 8.2 `CBCModuleBuilder` (in MCTargetDesc)

Owns the module: string pool (append-only, region-first so offsets are final), term table
(interning by structural key), method-ref and field-ref tables (interning), AOT entries,
type/field/method definitions, per-method code headers, exception regions (symbol
triples), liveness positions (symbols). `CBCTargetObjectStreamer` forwards directives to
it; `CBCTargetAsmStreamer` prints them.

### 8.3 Operand lowering (`CBCMCInstLower`)

| MachineOperand | MCOperand |
|---|---|
| callee `GlobalAddress` of a CBC function `f` | `Imm(builder.methodRef(Prog, name(f), sigTerm(f), flags 0))` |
| callee `GlobalAddress`/`ExternalSymbol` of a native function | `Imm(builder.methodRef(cbc.native, name, sigTerm, AOT))` + direct-call AOT entry |
| `LOAD_FNPTR @f`, `f` a CBC function | `Imm(builder.methodRef(Prog, name(f), sigTerm(f), flags 0))` — the same reference a direct call to `f` uses; the engine turns it into `f`'s descriptor address |
| `LOAD_FNPTR @n`, `n` a native function | `Imm(builder.methodRef(cbc.native, name(n), sigTerm(n), AOT))` + direct-call AOT entry — the same reference a direct call to `n` uses |
| `LEA_IMAGE img` (whole program) | `Imm(builder.fieldRef(Data, "image", imageTerm))` |
| `LEA_IMAGE img` (module-local) | padded ULEB placeholder + fixup `fixup_cbc_fref` against the image section symbol of `img` (§8.6) |
| `LEA_IMAGEOF g` (module-local) | padded ULEB placeholder + fixup `fixup_cbc_fref_imageof` against `g` |
| displacement `IMGOFF(g) + c` (module-local) | `MCSymbolRefExpr(g, VK_CBC_IMGOFF) + c` + fixup `fixup_cbc_image_disp` |
| `LEA_NATIVE sym` (native data only) | `Imm(builder.fieldRef(cbc.native, sym, U64))` + static-field AOT entry |
| atomic access width | `Imm(builder.fieldRef(A64, "v", I64))` etc. |
| `INITSTR blob` | `Imm(builder.stringOffset(blob))` |
| MBB | `MCSymbolRefExpr` (branch fixup) |

Because every pool index is final when the instruction is lowered, every non-branch
instruction has its exact size at encoding time.

### 8.4 Exception regions

For each `invoke` call site (begin/end `EH_LABEL`s from `MachineFunction`'s landing pad
info), the AsmPrinter records `{beginLabel, endLabel, landingPadLabel}`. The end label is
emitted after the call instruction (the next instruction boundary), matching the
`[start, end)` convention of the Cangjie producer. Regions are emitted innermost first
(they never overlap: one call site, one region). Implementation: `CBCException : EHStreamer`
(template `WasmException`), or directly in `CBCAsmPrinter::emitFunctionBodyEnd` from
`MF->getLandingPads()`.

### 8.5 State points

After each `IsStatePoint` instruction the AsmPrinter creates a temp symbol and calls
`builder.addLiveness(method, sym)`. The writer emits `{offset(sym), 0, 0, 0}` for each.

### 8.6 Relocatable output for separate compilation (phase 2, deferred)

> **Deferred — not in the work scope** (`04-architecture.md` §3). Recorded as the design
> for a future phase 2; none of it is implemented in the current plan, and the
> module-local `LEA_IMAGE`/`LEA_IMAGEOF` rows of §8.3 apply only then.

In module-local mode the AsmPrinter writes the ELF relocatable object of
`10-linker-and-runtime.md` §4.1 instead of a `.cbc` (`CBCELFObjectWriter`, a thin
`MCELFObjectTargetWriter` with `EM_CBC`). Pool indices are then not final, so every
pool-index operand is emitted as a **padded** LEB of fixed width plus a fixup, and the
instruction size is still known at encoding time:

| Fixup | Field | ELF relocation |
|---|---|---|
| `fixup_cbc_mref` | 3-byte padded ULEB (method reference in `call`, `ld.fnptr`) | `R_CBC_MREF_ULEB21` |
| `fixup_cbc_fref` | 3-byte padded ULEB (field reference in `lea.s`/`ld.static`/atomics) against an image section symbol or a native data symbol | `R_CBC_FREF_ULEB21` |
| `fixup_cbc_fref_imageof` | same field, `IMAGEOF` variant against data symbol `g` | `R_CBC_FREF_ULEB21` with the `IMAGEOF` flag in `r_info` |
| `fixup_cbc_image_disp` | low nibble + 4-byte padded SLEB of a raw load/store displacement or `BinaryImm64` immediate | `R_CBC_IMAGE_DISP` |
| `fixup_cbc_str` | 4-byte padded ULEB (`initstr` blob offset) | `R_CBC_STR_ULEB28` |
| `FK_Data_8` in image sections | 8-byte pointer | `R_CBC_ABS64` |

Branch fixups are resolved inside the object as in whole-program mode. Image sections are
emitted from `!cbc.image.placement` (`07-ir-passes.md` §3.6): own image, aligned image and
one section per COMDAT group in an `SHT_GROUP` with the group's signature. The 3- and
4-byte paddings are chosen so that every final index fits: 2^21 method/field references
and 2^28 bytes of string pool per program.

### 8.7 Target streamers

* `CBCTargetAsmStreamer::emitMethod/emitFrame/emitTyped/emitEndMethod/emitType/
  emitStatic/emitBlob/emitNative/emitEntry/emitFnIndex` print directives.
* `CBCTargetObjectStreamer` implements them on the module builder.
* `CBCAsmParser` parses the same directives (so `llvm-mc` can assemble hand-written CBC).

## 9. Intrinsics (`IntrinsicsCBC.td`)

| Intrinsic | Purpose | Selected to |
|---|---|---|
| `llvm.cbc.image.base(i32 img) → i64` (`readnone`) | address of image `img` (0 in phase 1) | `LEA_IMAGE Rd, Rdead, img` → `LeaStatic Rd, Rdead, @image`; an image in base-pointer mode instead selects `LdStatic Rd, @base` |
| `llvm.cbc.image.addr.of(ptr @g) → i64` (`readnone`, module-local mode only) | address of a data symbol whose image is unknown until link time | `LEA_IMAGEOF Rb, Rdead, @g` + `IMGOFF(g)` folded into the user's displacement (§4.3) |
| `llvm.cbc.native.addr(metadata !"sym") → i64` | address of a native **data** symbol | `LEA_NATIVE Rd, Rdead` → `LeaStatic Rd, Rdead, @aot(sym)` |
| (no intrinsic) function address `ptr @f` used as a value | CBC function (descriptor) or native function (real address) | `LOAD_FNPTR Rd, @f` → `ld.fnptr Rd, @mref` (E5) |
| (no intrinsic) `call ptr %fp(...)` | any indirect call | `CALL_INDIRECT Rt` → `call.indirect Rt` (E6) |
| `llvm.cbc.fcb() → i64` (`readnone` is wrong: it is per fiber) | this fiber's FCB | `ld.fcb Rd` (E8). `__cbc_fcb` is `always_inline` around it |
| `llvm.cbc.initstr(i32 blob) → i64` | address of a blob's bytes | `InitString t_blob, #off; LoadStackRec; LoadRawMemory 64; addi 16` |
| `llvm.cbc.raise()` (noreturn) | raise the marker exception | `NullCheck IRZ` |
| `llvm.cbc.rethrow(i64 obj)` (noreturn) | rethrow the caught engine object | `Throw r` |
| `llvm.cbc.catch() → i64` | engine exception object at a landing pad | `Catch Rd` (copy of `IR_ACC`) |
| `llvm.cbc.gcpoint()` | safepoint | `GcPoint` |
| `llvm.cbc.cas.{i8,i16,i32,i64}(ptr, exp, new) → i1` | raw CAS | `CAS` with `$cbc:A*` |
| `llvm.cbc.divcheck(i64)` | sanitizer division guard | `DivCheck` |
| `llvm.cbc.require.engine.ext()` (has side effects, result-less) | make an unpatched engine fail while rewriting the entry method (`13-engine-changes.md` §1) | `ld.fnptr Rdead, @native(abort)` — uses E5 on an AOT reference, creates no descriptor |

## 10. Pass pipeline (`CBCPassConfig`)

IR level, in `CBCTargetMachine::registerPassBuilderCallbacks` (link-time full LTO pipeline
end) and in `addIRPasses` for `llc`:

1. `CBCSynthesizeEntry` (link only) — entry method, constructor calls, startup.
2. `CBCPrepareFunctionAddresses` — CBC wrappers for the addresses of native variadic and
   `div`-family functions, renames for `long double` ones; all other function addresses
   stay `ptr @f` and are selected to `LOAD_FNPTR`.
3. (No indirect-call pass: indirect calls are selected directly to `CALL_INDIRECT`.)
4. `CBCNativeCallLegalizer` — table-driven native libc rules (`CBCNativeLibc.def`,
   `10-linker-and-runtime.md` §5.4): unsupported functions → error, `long double` renames,
   inline `div` family, two-register results → error, function-pointer arguments →
   error/warning. Must run before debug info is stripped (its diagnostics use debug
   locations).
5. `ExpandVariadics` (lowering mode, CBC ABI).
6. `CBCLowerSjLj`, `CBCLowerEH` (before `DwarfEHPrepare`-equivalent; the generic
   `addPassesToHandleExceptions` for `ExceptionHandling::CBC` runs `DwarfEHPrepare` only to
   turn `resume` into calls, which `CBCLowerEH` already did).
7. `CBCLowerGlobals` — data image; must run after every pass that can create globals
   (EH clause tables, `ExpandVariadics`).
8. `CBCInsertSafepoints` (loop latches).
9. `AtomicExpandPass` (+ the CBC custom cmpxchg expansion).
10. `IndirectBrExpandPass`.
11. `LowerInvoke` is **not** used (it would drop EH).
12. Standard `CodeGenPrepare`.

Machine level:

* `addInstSelector`: `CBCDAGToDAGISel`.
* `addPreRegAlloc`: none special.
* `addPostRegAlloc`: none.
* `addPreEmitPass`: `CBCPeephole`
  (fold `mov` chains around calls, turn `addi d, l, 0` into `mov`),
  `CBCLiteralBudget` (estimate RT literals per function, §11), `CBCVerifyFrame`.
* `addPreEmitPass2`: none. Compression to 2-address forms happens in the MC layer
  (`CBCInstrCompress.td`, `CompressInstEmitter`, as RISC-V's `RISCVInstrInfoC.td`).

## 11. Literal budget and function splitting

`CBCLiteralBudget` sums, per function, the worst-case RT literal usage of each MI using
the table in `02-isa-encoding.md` §8 (immediates outside [-2048, 2047], every direct call,
every `LOAD_FNPTR`, every static access, branches whose estimated RT distance exceeds 2047
bytes; `CALL_INDIRECT` costs none — estimate RT size
as 1.5× original size). Above 3 500:

1. Re-materialize large constants once into callee-saved registers (hoist) — a
   pre-RA option `cbc-hoist-large-immediates` that reduces duplicates.
2. Otherwise, emit `error: function too large for the CBC engine literal table`. A later
   improvement is automatic splitting with the `MachineOutliner`-like
   `HotColdSplitting`/`FunctionSplitter` at the IR level, triggered by a size estimate
   before codegen.

## 12. `CBCTargetTransformInfo`

| Hook | Value / behaviour |
|---|---|
| `getNumberOfRegisters(vector)` | 0 |
| `getRegisterBitWidth(vector)` | 0 (no vectorization) |
| `getInliningThresholdMultiplier` | 3 (calls are expensive; code size matters less) |
| `getArithmeticInstrCost` | 1 for legal ops; div/rem 2; expanded ops by expansion size |
| `getMemoryOpCost` | 1 |
| `getCFInstrCost` | 1 (no misprediction penalty) |
| `getCallInstrCost` | 20 (CBC callee, or indirect call: direct-call cost plus the engine's descriptor check), 8 (native callee) |
| `isLoweredToCall` | true for `fmod`, `floor`, `fma`, … |
| `haveFastSqrt` | true |
| `isTruncateFree(i64, i32)` | true |
| `enableInterleavedAccessVectorization` etc. | false |
| `getMaxInterleaveFactor` | 1 |
| `prefersVectorizedAddressing` | false |
| `shouldExpandReduction` | true |
| `supportsTailCalls` | false |
| `isLegalAddImmediate` | all 64-bit values |
| `getIntImmCost` | 0 for [-2048, 2047], 1 otherwise (literal) |

## 13. MC layer

### 13.1 Encoding formats (`CBCBaseInfo.h`, stored in `TSFlags`)

| Format | Encoding (`02-isa-encoding.md`) | Instructions |
|---|---|---|
| `FmtBccSpec` | `op [l:r] s16` | `BCC32/64{EQ,NE,LT,GE,ULT,UGE}` |
| `FmtBccGen` | `op [cc:l] [r:dlo] s16` | `BCC32/64` generic, float branches |
| `FmtBccImm` | `op [cc:l] sleb s16` | `BCCI32/64` |
| `FmtJump` | `12 s16` | `JMP` |
| `FmtRR` | `op [a:b]` | `MOV64rr`, `FMOV64rr`, 2-address ALU (compressed) |
| `FmtMovImm` | `op [d:lo4] sleb` | `MOVi64` |
| `FmtFImm32` / `FmtFImm64` | `op [0:d] u32/u64` | `FMOVi32/64` |
| `FmtBFX` | `1E [d:s] b1 b2` | `BFX` |
| `FmtOpRR` | `op [opn:d] [l:r]` | `Binary*`, `SBin*`, `Scc*`, `Float*` (binary and unary, i2f, f2i), `Convert` (`[to:from]`) |
| `FmtOpRImm` | `op [opn:d] [l:lo4] sleb` | `BinaryImm*`, `SBinImm*`, `SccImm*` |
| `FmtFMath` | `op [opn:0] [d:s]` | `FMathUnary*` |
| `FmtRawMem` | `op [r:base] [k:lo4] sleb` | `LDRAW_*`, `STRAW_*` |
| `FmtUntyped` | `op [r:k] u16` | `LDU_*`, `STU_*` |
| `FmtTail` | `84 [d:t] [k:0] uleb` | `LDTAIL_*` |
| `FmtStaticRef` | `op [d:x] uleb` | `LDSTATIC`, `STSTATIC` |
| `FmtLeaStatic` | `7C [d:b] uleb` | `LEA_IMAGE`, `LEA_NATIVE` |
| `FmtAtomic2` | `op [d:p] uleb` | `ATOMIC_LOAD*`, `ATOMIC_STORE*` |
| `FmtAtomic3` | `op [d:p] [s:x] uleb` | `SWAP*`, `ATOMIC_FETCH_*` |
| `FmtCAS` | `67 [d:p] [e:n] uleb` | `CAS*` |
| `FmtRegSym` | `44 [sel:r] uleb` | `CALL`, `CALL_NATIVE` (sel 3), `CALL_VIRT` (sel 4), `LOAD_FNPTR` (sel 13, E5) |
| `FmtRegGrp` | `45 [sel:r]` | `RET`, `FRET`, `CATCH`, `THROW`, `RAISE`, `DIVCHECK`, `CALL_INDIRECT` (sel 9, E6) |
| `FmtInitStr` | `47 uleb u16` | `INITSTR` |
| `FmtLdStackRec` | `4C [d:0] u16` | `LDSTACKREC` |
| `FmtNone` | `op` | `GCPOINT`, `NOP` |

`CBCMCCodeEmitter::encodeInstruction` switches on the format; for branch formats it emits
a fixup `fixup_cbc_br{16,20,32,36}` at the displacement field and zero bytes. Encoding of
the wide form prepends `0x13`.

### 13.2 `CBCAsmBackend`

* Fixup value = `target - (instruction end)`. Since the fixup is not at the end of the
  instruction, the backend stores the instruction length in the fixup kind info or computes
  `value = Target - FixupOffset - remainingBytesAfterField` (for specialized Bcc the field
  is the last 2 bytes, for generic Bcc the last 2 bytes plus a nibble in the previous byte,
  for `BccImm` the last 2 bytes, for `Jump` the last 2 bytes).
* `mayNeedRelaxation`: all narrow branch forms. `fixupNeedsRelaxationAdvanced`: value
  out of `s16` (or 20-bit split) range. `relaxInstruction`: switch to the wide opcode
  variant (`*_W`), which prepends `13` and widens the field.
* `applyFixup`: patch little-endian `s16`/`s32`; for generic `Bcc`, the low nibble goes
  into the low nibble of the `[r:dlo]` byte and the rest into the following field.
* No relocations are ever emitted (`writeNopData` emits `0x4D`).

### 13.3 `CBCInstPrinter` syntax

Lower-case, `.w32`/`.w64` width suffixes, condition codes as words, destination first:

```
mov.w64   ir3, ir4             movi.w64 ir3, 500         fmovi.w64 fr2, 0x400c000000000000
add.w64   ir3, ir4, ir5        addi.w64 ir3, ir4, 1000   add.w64 ir3, ir5   (2-address form)
bcc.w64   lt, ir3, ir4, .LBB0_2                          bcci.w64 ge, ir3, -100, .LBB0_3
bcc.w64   nz, ir3, ir4, .LBB0_4    bcc.w64 flt, fr1, fr2, .LBB0_5    jmp .LBB0_6
scc.w64   ult, ir3, ir4, ir5   scci.w32 eq, ir3, ir4, 20
bfx       ir3, ir4, sx, 8, 24  cvt i64, u32, ir3, ir4    cvt f64, i32, fr3, ir4
fadd.w64  fr3, fr4, fr5        fsqrt.w64 fr3, fr4        i2f.w64 fr3, ir4   f2i.w64 ir3, fr4
ld.raw.s32 ir3, [ir4 + 24]     st.raw.64 ir3, [ir8 + 8]  ld.raw.f64 fr2, [ir5 + 0]
lea.frame ir8, 16            alloca ir1, ir2, 16       stacksave ir3    stackrestore ir3
ld.fcb    ir1
ld.u.64   ir3, [u5]            st.u.f64 fr2, [u7]        ld.tail.64 ir8, [ir7 + 2]
lea.s     ir3, ir5, @image     lea.s ir4, ir6, @native(stdout)     (second operand: dead base-ref)
call      ir1, @__cbc_main     call ir1, @native(printf)
ld.fnptr  ir4, @cmp            ld.fnptr ir5, @native(strcmp)        call.indirect ir5
ret.w64   ir1                  fret.w64 fr0              catch ir3      raise    throw ir3
atomic.ld.32 ir3, [ir4]        cas.64 ir3, [ir4], ir5, ir6   atomic.fetch.add.64 ir3, [ir4], ir5
initstr   t0, @blob(0)         ld.stack.rec ir3, t0      gcpoint   nop
```

### 13.4 `CBCAsmParser` and `CBCDisassembler`

* The parser uses the TableGen-generated matcher (`-gen-asm-matcher`) plus custom operand
  parsers for `[reg + disp]`, `[uN]`, `@sym`, condition codes, conversion types, and the
  CBC directives (§8.7).
* The disassembler is hand-written (formats in §13.1),   mirroring
  `cbc-engine-initial-stage/src/cbc/isa_parser.cpp`, and decodes **all** 148 opcodes and
  all group selectors including the E5/E6/E8 ones (and ones the compiler never emits) so
  `llvm-objdump` can read Cangjie-produced `.cbc` files. Unknown bytes decode as
  `<invalid 0x..>` rather than aborting. Pool indices print symbolically when an object file context is available
  (`@mref(12)` → `@"$cbc.ex:Prog.foo"`).

## 14. Correctness checks built into the backend

* `CBCVerifyFrame` (pre-emit): untyped slot count ≤ 65 535; `untypedMemSize` is a
  multiple of 16 and every `LEA_FRAME` displacement is strictly inside it; `usesAlloca`
  is 1 iff the function contains `ALLOCA`, `STACKSAVE` or `STACKRESTORE`; no write to
  `IRZ`; `CALL` destination is `IR1`; the base-ref operand of every `LEA_*` differs from
  its destination and is neither `IRZ` nor `IRACC`; nothing writes `IRACC`; the
  `CALL_INDIRECT` target register is neither `IRZ` nor `IRACC`; every `LOAD_FNPTR`
  operand names a function (CBC method or native function), never data.
* `CBCLiteralBudget` (§11).
* `CBCAsmPrinter`: every state point has a liveness entry; exception region labels are
  instruction boundaries (guaranteed by construction).
* Machine verifier enabled in tests (`-verify-machineinstrs`).
