# 13 — Accepted Engine Changes

The original plan treated the engine as immutable. Eight engine changes have since
been accepted. E1–E7 are small; E8 is the frame-memory and runtime-alloca change.
This file specifies each one precisely enough to implement and review
(they are **not implemented yet**), states what it changes for the compiler, and lists
the suggestions that were **not** accepted, whose compiler-side workarounds therefore stay.

Paths are relative to `cbc-engine-initial-stage/` unless stated otherwise. Line numbers
refer to the revision analysed for this plan (`4e4a8ed8`).

## 0. Summary

| Id | Change | Size | Removes from the compiler plan |
|---|---|---|---|
| **E1** | `Emitter::StoreFrame` long form uses `STORE_LONG_FRAME` instead of `STORE_LONG_REC` | 1 line | the 512-untyped-slot store limit, the frame-offset < 4 096 rule for typed slots, the `CBCVerifyFrame` slot check |
| **E2** | `BuildAbiInfo` sets `hasTailReg` when float parameters overflow to the stack | 1 line | the dummy-`I64` padding of signature terms |
| **E3** | method-ref and field-ref pool sizes read as `uint32_t` | 2 lines | the 65 535 reference limit and the program-splitting mode it forced |
| **E4** | the bytecode parser rejects unknown opcodes and stray prefixes with `FATAL` | ~5 lines | the infinite-loop failure mode (a safety fix; the compiler never emits such bytes) |
| **E5** | new instruction `LoadFuncPtr dst, @mref` (`RegSymGroup` selector 13): the native address of an AOT function, or the address of a CBC method's **function descriptor** — a 16-byte record in an engine-reserved, non-executable region (§9) | ~25 lines + ~80 lines (descriptor region) | the AOT-static-field trick for native function addresses; gives every CBC function a stable address-sized identity usable by E6, with no per-process limit |
| **E6** | new instruction `CallIndirect reg` (`RegGroup` selector 9): a descriptor address is called exactly like `DIRECT_CALL_2I`, any other address exactly like `DIRECT_CALL_2C`; null raises `NoneValueException` | ~40 lines | every dispatcher, every index-backed function pointer, the dispatcher calling convention and the trampoline budget |
| **E7** | `engine_i2c_call` (x86-64) sets `al = 8` before every native call (§10) | 1 instruction | the redirection of floating-point variadic native calls to `v*` variants (`printf("%f")` becomes a plain native call) |
| **E8** | untyped memory in the frame, `lea.frame`, runtime `alloca` / `stacksave` / `stackrestore`, freeing on return and unwind, `ld.fcb` (§11) | frame layout, five `RegGroup` selectors, shadow-stack runtime | the compiler-managed shadow stack, the reserved `IR13`, the startup `mmap`, the landing-pad reload of a stack pointer |

**Explicitly out of scope:** native code calling CBC code (callbacks). E5 deliberately
returns *non-executable* descriptors for CBC methods, so native code cannot call a CBC
function pointer at all: such a call faults on the non-executable page. Whether a native
entry into the interpreter would be safe depends on Cangjie runtime behaviour that has not
been verified (§8). Nothing in the compiler design relies on it.

**Not accepted (yet)** — the corresponding workarounds remain in force:

| Suggestion | Workaround that stays | Where |
|---|---|---|
| signed displacement in `LoadRec`/`StoreRec` long forms | fold only displacements in `[0, 2^31)` | `01` §12 Q5, `06` §4.4 |
| implement `LD_U8TO64`, `LD_U16TO64`, `LD_U32TO64`, `LD_S8TO64`, `LD_S16TO64` | use only the supported load kinds | `01` Q7, `02` §3 |
| `FrameDescProvider` reads `SourceFullName` when only `SourceFile` is set | emit both tags or neither | `01` Q12, `03` §9.2 |
| literal-table overflow as a release-mode rewrite failure | `CBCLiteralBudget` | `01` Q3, `06` §11 |
| raise `TRAMPOLINE_COUNT` (1 024), or allocate executable trampoline pages at run time | not needed: E5 returns descriptors, which need no executable memory and have no fixed limit (§9) | — |
| second result registers in `i2c`, static initial values, raw-address atomics, `argc/argv` to `main`, frame-zeroing skip, zero-extended 32-bit results, more math ops, jump tables, version checks, throwing stubs | all corresponding workarounds | `04`, `07`, `10` |

## 1. Compatibility rules for all changes

* **No renumbering.** The top-level opcode table (`ISA_OPCODES`) is unchanged. E5, E6 and
  E8 use free *selector* values of the two group opcodes (`RegSymGroup` `0x44`,
  `RegGroup` `0x45`), so an old instruction still decodes as that instruction.
* **Loud failure on old engines.** An engine without the new selectors reaches the
  `default:` branch of the group switch (`FATAL("Should not reach here")`,
  `src/cbc/isa_parser.cpp:703`, `:724`) when it rewrites a method that uses them. The
  generated entry method contains `ld.fnptr` of `abort` (E5) and a dead `ld.fcb` (E8),
  so the failure happens while rewriting the entry, before any user code runs. E1–E4 and
  E7 cannot be detected this way. All eight changes ship as one engine patch, and
  programs document the minimum engine revision in the linker map and release notes.
* **The Cangjie producer gains two zero fields.** It never emits the new selectors. E1–E4
  only fix behaviour that was already wrong or undefined. E7 only sets a register that
  the SysV ABI defines as an upper bound and that non-variadic callees ignore. E8 inserts
  `untypedMemSize` and `usesAlloca` into the method-code header (§11); the Cangjie
  encoder writes `0, 0` there in the same patch. There is no version byte, so an old
  encoder and an E8 reader are not a supported pair — CI pins them together.
* **RT bytecode is internal**, so adding RT opcodes (E6, E8) has no compatibility impact
  (`MAIN_TABLE` is generated from `CBC_RT_OPCODES`).

## 2. E1 — `StoreFrame` long form

### 2.1 Problem

```cpp
// src/cbc/emitter/emitter.cpp:957-966 (current)
void Emitter::StoreFrame(StoreAccessKind stk, Reg src, uint32_t offset)
{
    if (MathUtils::IsNBits(offset, 12)) {
        auto opc = !stk.IsFloat() ? RT::Opcode::STORE_FRAME : RT::Opcode::STORE_FRAME_F;
        LoadStore(stk, src, IReg::IRZ, offset, opc);
    } else {
        auto opc = !stk.IsFloat() ? RT::Opcode::STORE_LONG_REC : RT::Opcode::STORE_LONG_REC_F;
        LoadStoreLong(stk, src, IReg::IRZ, IReg::IRZ, offset, opc);
    }
}
```

`STORE_LONG_REC` with base `IRZ` is executed by `Interpreter::StoreRec`, which treats a
zero base as "the offset is the whole address" (`src/interpreter/interpreter.h:405-426`).
Every frame store at offset ≥ 4 096 therefore writes to the absolute address `offset`.
Affected callers: `StoreUntyped` (slot ≥ 512), `StoreTyped`/`StTyped` (typed slots placed
after ≥ 4 KiB of untyped slots), and every other `StoreFrame` user.

### 2.2 Change

```cpp
    } else {
        auto opc = !stk.IsFloat() ? RT::Opcode::STORE_LONG_FRAME : RT::Opcode::STORE_LONG_FRAME_F;
        LoadStoreLong(stk, src, IReg::IRZ, IReg::IRZ, offset, opc);
    }
```

`LABEL(STORE_LONG_FRAME_F)`/`LABEL(STORE_LONG_FRAME)` already exist
(`src/interpreter/interpretation_loop.cpp:864-870`) and call `Interpreter::StoreFrame`
with the decoded 32-bit offset, the mirror image of the correct `LoadFrame` long path
(`emitter.cpp:946-955`).

Not part of E1 (cosmetic, may be done in the same commit): `Emitter::LoadFrame`'s short
path passes `RT::Opcode::LOAD_FRAME` instead of the computed `opc`; behaviour is identical
because `MemoryLocation::LoadPrim` dispatches on the access kind.

### 2.3 Tests

* `test/memaccess_test.cpp`: a method with 600 untyped slots that stores distinct values
  into slots 0, 511, 512, 599 and loads them back; a method whose typed slot sits at frame
  offset > 4 096 and is written with `StTyped`.
* Run under ASan or with a guard page at a low address to prove no absolute write occurs.

### 2.4 Compiler impact

* `01` Q4 is resolved. Frames may use up to 65 535 untyped slots (the `u16` slot index).
* `06` §6.1/§14: the `CBCVerifyFrame` "slot < 512" check is dropped; the `u16` limit
  remains.

## 3. E2 — `hasTailReg` and float stack parameters

### 3.1 Problem

```cpp
// src/interpreter/code.cpp:85 (current)
bool hasTailReg = (iargIdx > IREG_PARAM_PASSING_AMOUNT);
```

Only integer parameters are counted. A method whose stack-passed parameters are all
floating point (more than `FREG_ABI_AMOUNT = 8` float parameters, few integer ones) gets
`hasTailReg = false`, so `StackExpansion::VisitFrameRootsForStackPtrs` does not adjust the
tail register when that method's prologue triggers a stack relocation, and the method then
reads its stack parameters through a dangling pointer.

### 3.2 Change

```cpp
bool hasTailReg = (iargIdx > IREG_PARAM_PASSING_AMOUNT) || (fargIdx > FREG_ABI_AMOUNT);
```

`fargIdx` counts float parameters from 0, so `fargIdx > 8` means at least one float
parameter is on the stack. The later clamping of `iargIdx`/`fargIdx` (`code.cpp:93-96`) is
unchanged. The integer condition keeps its conservative off-by-one (`iargIdx` starts at 1,
so exactly six integer parameters on x86-64 also set the bit); that is harmless because
adjusting a tail pointer that is not used for parameters has no effect.

### 3.3 Tests

`test/cbc_test.cpp` (or a new `stack_growth_test.cpp`): a method with 10 `F64` parameters
called from a deeply recursive caller so that its prologue triggers stack growth; check the
9th and 10th parameters.

### 3.4 Compiler impact

The signature-term padding rule (`01` §3.4, `03` §4.3, `06` §5.2) is removed. Signature
terms list exactly the lowered parameters.

## 4. E3 — reference pool sizes

### 4.1 Problem

```cpp
// src/engine/decode/reader.cpp:504-508 (current)
uint16_t methodIndexSize = reader.ReadULEB();
uint32_t methodIndexOffs = reader.ReadU32();
uint16_t fieldIndexSize = reader.ReadULEB();
uint32_t fieldIndexOffs = reader.ReadU32();
```

A file with more than 65 535 method or field references silently loses the high bits of the
count; references with larger indices are then out of the pool's bounds (`assert` only).

### 4.2 Change

```cpp
uint32_t methodIndexSize = reader.ReadULEB();
uint32_t methodIndexOffs = reader.ReadU32();
uint32_t fieldIndexSize = reader.ReadULEB();
uint32_t fieldIndexOffs = reader.ReadU32();
```

`OffsetPool` already stores `uint32_t size` (`src/engine/image/cbc_file.h:812`) and
`ErasedOffsetPool::size` is `uint32_t` (`:787-792`); reference ids are 32-bit
(`RefIdentifier`). Nothing else narrows the counts. The unused `typeIdxSize` (`u16`) stays.

### 4.3 Tests

A generated `.cbc` with 70 000 method references and a method that calls through index
69 999.

### 4.4 Compiler impact

* `01` Q1, `03` §5/§6/§12/§14: limits become 2^32 − 1 (practically unbounded).
* `10` §2.4 program splitting is no longer needed for reference counts. It remains
  available as an option for other reasons (very large programs, rewrite latency), but
  `cbc-ld` no longer triggers it automatically.

## 5. E4 — unknown opcodes

### 5.1 Problem

`IsaParserImpl::ParseOne` (`src/cbc/isa_parser.cpp:244-255`) switches over `ISA_OPCODES`
without a `default:`. A byte ≥ `0x94` matches no case and does not advance the cursor, so
`ParseAll` (`:999-1005`) loops forever. `_END` (`0x94`) and a second `WidePrefix` map to
`Unreachable`, an empty function (`:990`), with the same effect or a silently dropped
prefix.

### 5.2 Change

```cpp
static void ParseOne(IsaParser& parser)
{
    int w    = 0;
    auto opc = parser.reader.Read8();
    if (opc == Opcode::WidePrefix) {
        w   = 1;
        opc = parser.reader.Read8();
    }
    switch (opc) {
        ISA_OPCODES(PARSE_ONE_CASES)
        default: FATAL("unknown CBC opcode 0x%02x", (unsigned)opc);
    }
}

static void Unreachable(IsaParser& parser) { FATAL("invalid CBC opcode (stray prefix or _END)"); }
```

The message should include the byte offset (`parser.reader.Cursor() - parser.reader.Start()`)
and, where available, the method being rewritten; `IsaRewriter` can catch the case earlier
by overriding `ParseOne` (it already does, `isa_rewriter.cpp:1663-1669`) — a later
improvement is to call `StopRewrite()` there instead of aborting.

### 5.3 Tests

Feed `0x95`, `0x13 0x13 0x15 0x12`, and a lone `0x13` at the end of a method to `RawDisasm`
in a death test (`EXPECT_DEATH`).

### 5.4 Compiler impact

None on generated code. `01` Q11 becomes "unknown opcode aborts with a message".
`tools/dis` stops hanging on corrupt input.

## 6. E5 — `LoadFuncPtr dst, @mref`

### 6.1 Encoding and semantics

```
44 [13:dst] uleb(methodRefIndex)          RegSymGroup selector 13 = LoadFuncPtr
disassembly:  ld.fnptr IR3, @5
```

`dst` receives an **address** for the method designated by the method reference, resolved
exactly like `CallDirect` (`ResolverProxy::ResolveCall(DirectCall)`,
`src/resolution/resolution.cpp:699-759`):

| Resolved target | Value | Cost |
|---|---|---|
| AOT reference (`AOT` flag, direct-call AOT entry) | the `dlsym`ed native address | none beyond resolution |
| CBC method with a `StaticFunctionHandle` (an `AOT`-flagged definition) | its native function pointer | none |
| CBC method (`DynamicFunctionHandle`) | the address of its **function descriptor** (`FunctionDescriptors::Get(fuh)`, §9) | the first request for a method creates its 16-byte descriptor; later requests for the same method return the same address |

The value is computed **while the containing method is rewritten** and baked into the RT
code as a 64-bit immediate (`MOVI`/`ADDI` from `IRZ` with a literal). It is not a state point.
Resolution failure is a rewrite failure. Exhaustion of the descriptor region (2^20
descriptors with the default reservation, §9) is a `FATAL` at rewrite time; it is not a
limit any C program reaches in practice.

Properties the compiler relies on:

* the value is stable for the life of the process and identical for every request for the
  same method (the descriptor map is keyed by the handle), so it works as a
  function-pointer identity: equality, ordering, casts to integers, storage in memory;
* for a CBC method it lies inside the descriptor region, which `CallIndirect` (E6)
  recognizes with one range check and turns into an ordinary interpreted call;
* descriptors are 16-byte aligned (even values; the C++ member-function-pointer encoding
  needs the low bit free);
* for an AOT target it is the real native address, which `CallIndirect` calls natively,
  and which may be passed to native code as an ordinary C function pointer.

What a descriptor is **not**: executable code. The region is mapped `PROT_READ |
PROT_WRITE`; a native `call` to a descriptor address faults with `SIGSEGV` on the
non-executable page. This is deliberate: native code calling CBC code is out of scope
(§8), and the engine therefore never needs executable memory beyond its static
trampolines, which also keeps E5 usable on systems that forbid writable-then-executable
mappings.

Why not the existing direct-call trampolines (the earlier version of this change): they
are limited to `TRAMPOLINE_COUNT = 1 024` per process (`src/asm_export.h:15`), shared with
the entry point and any other loaded `.cbc`, and exhausting them is a `FATAL`
(`src/runtimesupport/impl/adapters.cpp:52-54`). Keeping C programs under that limit forced
a second, index-backed function-pointer representation with per-signature dispatchers in
the compiler. Their only advantage — being callable by native code — is exactly what is
out of scope.

### 6.2 Implementation

`src/cbc/isa_opcodes.h`:

```cpp
#define ISA_REG_SYM_GROUP_OPCODES(X) \
    X(LoadTypeInfoSig) X(LoadTypeInfoGeneric) X(NewObj) X(CallDirect) X(CallVirt) \
    X(CallInterf) X(Spawn) X(SpawnFuture) X(CallClosure) X(NewClosure) \
    X(CallClosureGeneric) X(NewObjGeneric) X(NewClosureGeneric) \
    X(LoadFuncPtr)          /* 13: new */ \
    X(_END)
```

`src/cbc/isa_parser.h`: `virtual void LoadFuncPtr(IReg dst, uint32_t method) = 0;`

`src/cbc/isa_parser.cpp`, `IsaParserImpl::RegSymGroup`:
`case Cbc::RegSymGroup::LoadFuncPtr: parser.LoadFuncPtr(reg, id); break;`

`src/cbc/isa_rewriter.cpp`:

```cpp
void LoadFuncPtr(IReg dst, uint32_t methodId) override
{
    auto m = resolver.Query(Index<DirectCall>(methodId));
    if (!m.has_value()) {
        return Fail("cannot resolve method for ld.fnptr");
    }
    uintptr_t address;
    if (auto compiled = std::get_if<DirectCall::Compiled>(&(*m)->data)) {
        address = compiled->funcPtr;
    } else {
        auto fuh = std::get<Interpretation::DynamicFunctionHandle*>((*m)->data);
        address  = RTSupport::FunctionDescriptors::Get(fuh);   // §9
    }
    emit.MovImm(Format::Width::W64, dst, address);
}
```

`src/cbc/isa_disasm.cpp`, `IsaDisasm`:
`void LoadFuncPtr(IReg dst, uint32_t m) override { stream.PrintLn("ld.fnptr {}, @{}", dst, m); }`
(and the resolving variant that prints the method name, like `CallDirect`).

Toolchain sync (optional, keeps the Scala assembler/disassembler able to read files that use
the instruction): `cbc_compiler/core/cbc-asm` (`NewAsmParser.scala`, `CodeGenerator.scala`)
and the selector enum in `cbc_compiler/core/assembler/.../isa12/forked/Assembler.scala`.

### 6.3 Thread safety and publication

`FunctionDescriptors::Get` takes its own mutex, writes the handle into the next free
descriptor, and then publishes the new descriptor count with a release store (§9). The
handle is fully constructed before it is published (`FunctionHandleManager::AcquireTagged`
issues a `seq_cst` fence). Readers in E6 never take the lock: they range-check the address
and compare its index with the count loaded with acquire semantics, so they can only
dereference descriptors whose handle store happened before. A descriptor address reaches
another thread only through RT code that was itself published after `Get` returned.

### 6.4 Tests

* Rewrite a method containing `ld.fnptr IR1, @m` (CBC target) twice in different methods:
  same value; value inside the descriptor region and 16-byte aligned.
* 5 000 distinct methods with `ld.fnptr`: 5 000 distinct values, all accepted by E6 (the
  old 1 024 limit does not exist).
* AOT target (`puts`): value equals `dlsym(RTLD_DEFAULT, "puts")`.
* `ld.fnptr` of an unresolvable method → rewrite failure, not a bogus value.
* Death test: a native function (in the test binary) that calls a descriptor address →
  `SIGSEGV` (documents that descriptors are not executable).

## 7. E6 — `CallIndirect reg`

### 7.1 Encoding and semantics

```
45 [9:reg]                                RegGroup selector 9 = CallIndirect
disassembly:  call.indirect IR5
```

Calls the address held in `reg` — a value produced by `LoadFuncPtr`, or any native function
address (from `dlsym`, from a native data structure, from `LoadFuncPtr` of an AOT reference).
Arguments are in the host-ABI registers
and the caller's outgoing stack slots, exactly as for `CallDirect`; results are in `IR1`
(and `FR0`). The instruction is a state point (liveness entry at the position after it).

Run-time behaviour (`LABEL(CALL_REG)`):

1. `target = IR[reg]`.
2. `target == 0` → raise `NoneValueException` (implicit exception), so a null function
   pointer call is catchable instead of a segmentation fault.
3. `target` is a function descriptor (§9: inside the region, 16-byte aligned, index below
   the published count) → **fast path**: call the descriptor's `DynamicFunctionHandle`
   through its `i2call` adapter (`engine_i2i_call`), exactly as `DIRECT_CALL_2I` does. No native code runs; the call is an ordinary CBC→CBC call
   (tail register set, non-volatile registers saved by the callee, exceptions unwind
   normally, two-register results in `IR1`/`IR2` reach the caller).
4. Otherwise → native call through `GenericI2CCallInstance` (`engine_i2c_call`), exactly as
   `DIRECT_CALL_2C` does.

`reg` may be any integer register; its value is read before argument registers are loaded,
so it may even be an argument register (the compiler avoids that anyway).

### 7.2 Implementation

`src/cbc/isa_opcodes.h`:

```cpp
#define ISA_REG_GROUP_OPCODES(X) \
    X(Ret32) X(Ret64) X(FRet32) X(FRet64) X(DivCheck) X(Catch) X(Throw) X(RetRef) X(NullCheck) \
    X(CallIndirect)         /* 9: new */ \
    X(_END)
```

`src/cbc/isa_parser.h`: `virtual void CallIndirect(IReg target) = 0;`

`src/cbc/isa_parser.cpp`, `IsaParserImpl::RegGroup`:
`case Cbc::RegGroup::CallIndirect: parser.CallIndirect(reg); break;`

`src/cbc/isa_rewriter.cpp`:

```cpp
void CallIndirect(IReg target) override
{
    EmitLogCall("call.reg", target);   // trace-level logging, like the other calls
    emit.CallReg(target);
    BindStatePoint();
    EmitReturnedTo();
}
```

`src/cbc/emitter/emitter.{h,cpp}`:

```cpp
void Emitter::CallReg(IReg target)
{
    Encode(segment, RT::B2xr { .opc = RT::Opcode::CALL_REG, .xr = XR { .imm = 0, .r = target } });
}
```

`src/cbc/isa_rt.h` (`CBC_RT_OPCODES`, next to `DIRECT_CALL_2C`):
`X(CALL_REG, B2xr, "call.reg $1ir")`

The descriptor test is `FunctionDescriptors::Lookup` (§9).

`src/interpreter/interpretation_loop.cpp`:

```cpp
LABEL(CALL_REG) {
    auto args   = B2xr::Decode(reader);
    LOG_INSTR;
    auto target = ectype->GetPrimitive(args.xr.r.IR()).u64;
    if (target == 0) {
        THROW_IMPLICIT(Type::NoneValueException);
    }
    reader0 = reader; // save current pc (the frame is dropped by the thunk, as for DIRECT_CALL_*)
    if (auto fuh = RTSupport::FunctionDescriptors::Lookup(target)) {
        return { fuh->base.i2call, reinterpret_cast<void*>(fuh) };
    }
    return { Adapters::GenericI2CCallInstance(), reinterpret_cast<void*>(target) };
}
```

(`DynamicFunctionHandle` has `base` (`FunctionHandle`, holding `i2call`) as its first
member, which is what `DIRECT_CALL_2I` relies on when it casts the literal to
`FunctionHandle*`.)

`src/cbc/isa_disasm.cpp`: `void CallIndirect(IReg r) override { stream.PrintLn("call.indirect {}", r); }`

`src/cbc/formater_rt.cpp`: nothing if the formatter is driven by the `CBC_RT_OPCODES` format
strings; otherwise add the `CALL_REG` case.

Unit-test framework: `interpretation_loop.cpp` is shared with the tests
(`:48-50`); the test harness that intercepts thunks must accept the new thunk sources (the
same two adapters it already handles for `DIRECT_CALL_2I`/`2C`).

### 7.3 Exceptions

* Fast path: identical to a `CallDirect` to a CBC method.
* Native path: identical to a `CallDirect` to an AOT method. A native target never raises a
  CBC exception (it cannot call back, §8).
* Null target: the implicit exception is raised at the `call.indirect` instruction, inside
  the caller's exception region if the call is an `invoke`.

### 7.4 Tests

* `call.indirect` to `ld.fnptr` of a CBC method: fast path taken (trace log shows `call.2i`
  semantics), arguments in 6 registers + 3 stack slots, two-register result.
* `call.indirect` to a value inside the descriptor region but beyond the published count,
  or misaligned: not treated as a descriptor (native path; the test uses a value whose
  native call is caught by a `SIGSEGV` death test).
* `call.indirect` to `ld.fnptr` of `puts`: native path.
* `call.indirect` to an address obtained from `dlsym` at run time: native path.
* Null: catchable `NoneValueException`.
* Exception thrown by a CBC callee reached through the fast path, caught by the caller's
  region.

## 8. Out of scope: native code calling CBC code

The engine has a mechanism for native code to enter the interpreter (direct-call
trampolines → `engine_c2i_call`, used by the launcher for `main`), but this plan does
**not** use it, and E5 deliberately hands out non-executable descriptors instead of
trampoline addresses, so a CBC function pointer cannot reach that path. The reason is that
the safety of native→CBC calls depends on behaviour that has not been verified:

* the fiber stack does not relocate (`01-cbc-platform-facts.md` §7), so a native frame's
  pointers into itself are not the problem they were under stack relocation; the other
  three items below still are;
* whether the runtime's GC stack walk can cross native frames between two interpreter
  activations;
* whether an exception leaving the inner activation can unwind through native frames back
  into the outer interpreter frame;
* asynchronous entry (signal handlers, new native threads, process-exit callbacks), which
  would run without a valid Cangjie thread register or would clobber the interrupted
  code's `Ectype` registers.

Consequences:

* Callback-taking libc functions (`qsort`, `qsort_r`, `bsearch`, `lfind`, `lsearch`, the
  `tsearch` family, `ftw`, `nftw`, `pthread_create`, `pthread_once`, `pthread_atfork`,
  `pthread_key_*`, `call_once`, `thrd_create`, `fopencookie`, `dl_iterate_phdr`, …) are
  **unsupported** and rejected at link time; they are not re-implemented in CBC
  (`10-linker-and-runtime.md` §5.5). The exit-handler family (`atexit`, `__cxa_atexit`, …)
  is defined in CBC by `crt-cbc`, because its handlers are called by CBC code. Signal
  handlers are refused at run time by crt guards that accept only `SIG_DFL`/`SIG_IGN`
  (§5.2 there).
* Passing a CBC function to a function-pointer parameter of a native function is a
  link-time error when provable, and a warning otherwise (`10-linker-and-runtime.md` §5.6).
  If such a value reaches native code anyway and is called, the call faults on the
  non-executable descriptor page — a crash at the faulty call, not silent corruption.

If the runtime team later confirms the conditions above and callbacks are wanted, a
separate change would have to give descriptors an executable entry (for example a static
or dynamically allocated trampoline per descriptor, created on demand when a pointer is
passed to native code). That is a separate decision with its own test plan.

## 9. The function descriptor region

New files `src/runtimesupport/impl/function_descriptors.{h,cpp}`:

```cpp
namespace RTSupport::FunctionDescriptors {

struct Descriptor {                                   // 16 bytes, 16-byte aligned
    Interpretation::DynamicFunctionHandle* fuh;
    uint64_t reserved;                                // 0; room for a future executable entry
};

constexpr size_t kRegionBytes = size_t(16) << 20;     // 16 MiB = 1 048 576 descriptors
constexpr size_t kCommitStep  = size_t(64) << 10;     // commit 4 096 descriptors at a time

uintptr_t Get(Interpretation::DynamicFunctionHandle* fuh);              // E5
Interpretation::DynamicFunctionHandle* Lookup(uintptr_t address);       // E6, lock-free

} // namespace RTSupport::FunctionDescriptors
```

```cpp
// function_descriptors.cpp
static std::atomic<uintptr_t> g_base{0};               // 0 until the region is reserved
static std::atomic<size_t>    g_count{0};              // published descriptors
static size_t                 g_committed = 0;         // bytes committed (under g_mutex)
static std::mutex             g_mutex;
static std::unordered_map<Interpretation::DynamicFunctionHandle*, uintptr_t> g_byHandle;

uintptr_t Get(Interpretation::DynamicFunctionHandle* fuh)
{
    std::lock_guard guard(g_mutex);
    if (auto it = g_byHandle.find(fuh); it != g_byHandle.end()) return it->second;
    uintptr_t base = g_base.load(std::memory_order_relaxed);
    if (base == 0) {
        void* p = mmap(nullptr, kRegionBytes, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) FATAL("cannot reserve the CBC function descriptor region");
        base = reinterpret_cast<uintptr_t>(p);         // page-aligned, hence 16-aligned
        g_base.store(base, std::memory_order_release);
    }
    size_t index = g_count.load(std::memory_order_relaxed);
    size_t end   = (index + 1) * sizeof(Descriptor);
    if (end > kRegionBytes) FATAL("too many CBC function descriptors");
    if (end > g_committed) {
        if (mprotect(reinterpret_cast<void*>(base + g_committed), kCommitStep,
                     PROT_READ | PROT_WRITE) != 0)
            FATAL("cannot commit the CBC function descriptor region");
        g_committed += kCommitStep;
    }
    auto* d = reinterpret_cast<Descriptor*>(base) + index;
    d->fuh = fuh;
    d->reserved = 0;
    g_count.store(index + 1, std::memory_order_release); // publish
    uintptr_t address = reinterpret_cast<uintptr_t>(d);
    g_byHandle.emplace(fuh, address);
    return address;
}

Interpretation::DynamicFunctionHandle* Lookup(uintptr_t address)
{
    uintptr_t base = g_base.load(std::memory_order_acquire);
    if (base == 0) return nullptr;
    uintptr_t delta = address - base;                  // wraps for address < base
    if (delta >= kRegionBytes || (delta & 15) != 0) return nullptr;
    if (delta / sizeof(Descriptor) >= g_count.load(std::memory_order_acquire)) return nullptr;
    return reinterpret_cast<const Descriptor*>(address)->fuh;
}
```

Design points:

* **Address space only.** The 16 MiB reservation costs no memory; committed pages cost
  4 KiB per 256 descriptors. A large C++ program with 50 000 address-taken functions (every
  surviving virtual function) uses about 800 KiB.
* **No executable memory.** Nothing in the region is ever executable, so the change works
  under W^X policies (SELinux `deny_execmem`, systemd `MemoryDenyWriteExecute=yes`) and on
  JIT-restricted platforms.
* **Identity.** One descriptor per handle for the life of the process; handles are never
  freed (the same assumption the static trampoline table already makes).
* **Lookup cost.** One acquire load of the base, a subtraction, two compares, one acquire
  load of the count and one load of the handle — no lock, no hash lookup on the call path.
* **Unaffected users of trampolines.** The static direct-call and dynamic-call trampolines
  keep serving the launcher's entry point, virtual calls of Cangjie code and `PATCH`.
  `GetDirectCallTrampoline` is no longer reached from E5.
* **Tests:** concurrent `Get` from several threads for the same and for different handles
  (same address for the same handle, no duplicates); `Lookup` of the region base minus 16,
  of the last committed descriptor plus 16, and of misaligned addresses returns null;
  growth across a commit step.

## 10. E7 — `al` before native calls (x86-64)

### 10.1 Problem

The SysV x86-64 ABI requires the caller of a variadic function to put in `al` an upper
bound on the number of vector registers used for arguments. glibc's variadic functions
(`printf`, `fprintf`, `snprintf`, `syslog`, …, as compiled by GCC or clang) begin with a
prologue that tests `al` and, when it is zero, skips spilling `xmm0..xmm7` into the
register save area; `va_arg(ap, double)` then reads garbage. `engine_i2c_call` never sets
`al` (`src/arch_os/x86_64_linux/trampolines.S:582-594`):

```asm
.macro i2c_call iregs_count fregs_count
  mov r10, rdi

  i2c_emit_iregs_abi \iregs_count
  i2c_emit_fregs_abi \fregs_count

  call r10

  mov  [ECTYPE_REG + ECTYPE_IREGS_OFFSET + ECTYPE_REG_SIZE], rax
  movq [ECTYPE_REG + ECTYPE_FREGS_OFFSET], xmm0

  jmp .Lrestart_interpretation
.endm
```

At `call r10`, `rax` still holds the address of `engine_i2c_call` that the interpreter
jumped through (`.Linterpreter_call: jmp rax`), whose low byte is a multiple of 16 —
possibly 0.

### 10.2 Change

```asm
  i2c_emit_iregs_abi \iregs_count
  i2c_emit_fregs_abi \fregs_count

  mov eax, \fregs_count          // E7: al = upper bound of vector argument registers (8)

  call r10
```

* `rax` is not an argument register in SysV, and neither `i2c_emit_iregs_abi` (moves into
  `rdi..r9`) nor `i2c_emit_fregs_abi` (`movsd` into `xmm0..xmm7`) uses it after this
  point, so the instruction can go immediately before `call r10`.
* `\fregs_count` is `FREG_ABI_AMOUNT` = 8, the maximum the ABI allows, which is a correct
  upper bound for every call. Non-variadic callees ignore `al`.
* `i2c_call` is instantiated once (`engine_i2c_call`), which serves `DIRECT_CALL_2C`
  (every native direct call) and the native path of `CALL_REG` (E6). Both therefore
  benefit.
* The AArch64 adapter needs no change: AAPCS64 (Linux) passes variadic arguments like named
  ones and has no `al` equivalent.

### 10.3 Tests

* A variadic native function in the test binary, `double sum_va(int n, ...)`, called
  through `call.2c` with `n = 3` and three `double` arguments in `FR0..FR2`: correct sum.
  (Before the change this test fails whenever `al` happens to be 0.)
* End to end: `printf("%d %.3f %s\n", 7, 2.5, "x")` from compiled C code produces native
  output.

### 10.4 Compiler impact

`CBCNativeCallLegalizer` no longer redirects floating-point variadic native calls to `v*`
variants (`04-architecture.md` §10.2, `07-ir-passes.md` §6); such calls are lowered as
plain native calls. The `v*` map is kept only for the CBC wrappers that represent the
address of a native variadic function (`07-ir-passes.md` §4). Risk R8 is resolved.

## 11. E8 — Frame memory and the runtime shadow stack

### 11.1 What changed

Fiber stacks do not move (`01-cbc-platform-facts.md` §7). A local whose size is known
when the frame is built can live in the frame. Only `alloca` and VLAs need a side
stack, and the compiler should not own that stack: no reserved register, no prologue,
no landing-pad reload. The architecture is `04-architecture.md` §5. This section is
the engine work.

### 11.2 Method-code header

After `mayHaveNativeCalls` and before `codeSize` (`03-cbc-file-format.md` §9.1):

```
uleb untypedMemSize    // bytes, multiple of 16; 0 if none
u8   usesAlloca        // 0 or 1
```

The Cangjie encoder writes a `0` uleb and a `0` byte. `makeFrameLayout`
(`isa_rewriter.cpp:1687-1745`) adds the block only when `untypedMemSize > 0`: the offset
after the untyped slots is aligned up to 16, that aligned offset is `untypedMemOffset`,
and the size is added to the `u32` frame size. A zero size adds no bytes and no
alignment gap, so a Cangjie frame is the same size and the same offsets as before E8.
When `usesAlloca` is 1 it also reserves 8 bytes at the top of the frame, engine-private,
for the shadow cursor saved on entry. Bytecode cannot address that slot. A size that is
not a multiple of 16, or a `usesAlloca` other than 0 or 1, is `FATAL` at load.

### 11.3 Instructions

`RegGroup` (`0x45`) selectors, same encoding style as E6 (`02-isa-encoding.md` §4.8):

| Sel | Bytes | Effect |
|---|---|---|
| 10 | `45 [10:dst] u32 disp` | `lea.frame`. Rewritten to the same frame `lea` as `LoadStackRec`, at `untypedMemOffset + disp`. `disp >= untypedMemSize` is `FATAL` at rewrite (a compiler bug). Not a state point |
| 11 | `45 [11:dst] [size:alignLog]` | `alloca`. `align = 1 << alignLog`, `alignLog` in 4..12. Runtime, below. Not a state point |
| 12 | `45 [12:dst]` | `stacksave`. `dst` = current cursor, or 0 if this fiber has never allocated |
| 13 | `45 [13:src]` | `stackrestore`. Frees allocations newer than the token in `src` |
| 14 | `45 [14:dst]` | `ld.fcb`. `dst` = the FCB pointer stored on the current `Ectype` |

`LeaFrame` needs no new RT opcode. The other four do (`SHADOW_ALLOCA`, `SHADOW_SAVE`,
`SHADOW_RESTORE`, `LOAD_FCB`), added to `CBC_RT_OPCODES`. An `alloca`, `stacksave` or
`stackrestore` in a method with `usesAlloca = 0` is `FATAL`: the frame has no cursor slot.

### 11.4 Shadow stack

Per fiber, next to the FCB pointer, not inside the FCB structure CBC code writes:

```cpp
struct ShadowSegment {
    ShadowSegment *prev;
    uint8_t *base;   // low address of the mapping, including the guard page
    uint8_t *usable; // base + page
    uint8_t *end;    // high address; the initial cursor
};

struct ShadowCursor {
    ShadowSegment *top; // null until the first alloca
    uint8_t *sp;
};
```

`alloca(size, align)`, with the empty cursor represented as `top == null`:

1. Reject `alignLog` outside 4..12. Round `size` up to `align`. A size that overflows,
   or a single request above 1 GiB, is fatal "shadow stack overflow" and does not
   change the cursor. Size 0 returns the current `sp` when `top != null`, or allocates
   one segment and returns its aligned `end`, without moving `sp` off that `end`.
2. The stack grows down. When `top != null`, `new_sp = align_down(sp - size, align)`.
3. If `top` is null or `new_sp < top->usable`, map a new segment and link it
   (`prev = top`). Usable size is `max(64 KiB, 2 × previous usable, request)`, rounded
   up to 64 KiB. The mapping is `PROT_NONE` for the first page and
   `PROT_READ | PROT_WRITE` for the rest (`MAP_PRIVATE | MAP_ANONYMOUS`). The previous
   segment stays mapped. `mmap` failure is the same fatal overflow, with the old
   cursor intact. Then `sp = end` and `new_sp` is computed in the new segment.
4. Publish `sp = new_sp` and return it.

`stacksave` returns `sp`, or 0 when `top` is null. `stackrestore(token)`:

* `token == 0` and `top == null`: no-op. `token == 0` and `top != null`: unmap every
  segment and clear the cursor.
* `token` equals the current `sp`: no-op.
* `token` lies in the current segment and `token > sp` (an older cursor; the stack
  grows down): set `sp = token`.
* `token` lies in an older segment: unmap every newer segment, set `top` to the
  segment that contains `token`, set `sp = token`.
* anything else: fatal.

### 11.5 Entry, return, unwind

On fiber creation the engine `malloc`s the FCB (zeroed, never freed, never moved),
stores the pointer on the `Ectype`, and sets the shadow cursor to empty. `ld.fcb`
loads that pointer. It is a runtime load: methods are rewritten once and shared across
fibers, so the address cannot be baked.

Frame prologue, after the existing zero loop, when `usesAlloca` is 1: store the
current cursor (`top` and `sp`, or the token `stacksave` would return) into the
engine-private slot. The slot was just zeroed; this store replaces the zeros.

Normal return, before the frame is popped: if `usesAlloca` is 1, `stackrestore` the
saved token. `.Lunwind_top_frame` does the same, before `POP_REGS` releases the frame.
The handler frame is not popped, so its slot is left alone. Restoring a token that
bytecode already restored is a no-op (§11.4).

### 11.6 Tests

* A method with `untypedMemSize = 16` and a `lea.frame` of 0: the pointer is inside
  the frame, 16-byte aligned, and a store through it is still there after a call that
  forces fiber-stack growth.
* `alloca` of 100 bytes, value written, readable after a callee that itself `alloca`s
  and returns. After the outer return the pages may be unmapped; a test-only hook
  reports the cursor equal to its value on entry.
* Growth: one `alloca` larger than 64 KiB, then a small one. The first pointer stays
  valid. The two pointers are in different mappings.
* Throw out of a callee that `alloca`'d: the caller's earlier `alloca` pointer is
  still valid, and the cursor after the catch equals the cursor after the caller's
  own `alloca`.
* `stacksave` / `alloca` / `stackrestore` in a loop of 1000: the cursor at the save
  point is unchanged after the loop, and one segment is enough.
* `usesAlloca = 0` together with an `alloca` opcode: `FATAL` at rewrite.
* `ld.fcb` in two fibers (the launcher fiber and, if the test harness can start a
  second one, that fiber) returns two different non-null pointers.
* Entry rewrite on an engine whose `RegGroup` switch has no selector 14: `FATAL`
  before user code, same shape as the E5 probe.

### 11.7 Compiler impact

The C target lowers static allocas to `lea.frame` and dynamic allocas to the three
shadow instructions (`06-backend.md` §6). `IR13` is callee-saved. The crt no longer
maps a shadow stack. `__cbc_fcb` lowers to `ld.fcb`. Landing pads do not reload a
stack pointer; the sjlj pad does `stackrestore` the token saved at `setjmp`
(`08-exceptions-and-sjlj.md` §8).

## 12. Effect on the compiler documents

| Document | Change |
|---|---|
| `01-cbc-platform-facts.md` | §3.4 (E2), §4 (E1), §5 (new calls), §10 (callbacks still unsupported), §12 Q1/Q4/Q11/Q17 |
| `02-isa-encoding.md` | selector tables, §4.9 encodings, §6/§8 costs, §9 |
| `03-cbc-file-format.md` | §4.3 (no padding), §5 (refs for `ld.fnptr`), §12/§14 limits |
| `04-architecture.md` | §1, §3, §7 rewritten (one representation: descriptors), §8, §9 (no dispatcher convention), §10.2 (E7), §13, §16, §17 |
| `05-llvm-core-changes.md` | no `CBC_Dispatch` calling convention; `ExpandVariadics` expands indirect variadic calls (`ignoreFunction(nullptr)` is false) |
| `06-backend.md` | new MIs, `LowerCall` for indirect calls, frame limits, intrinsics, checks; no dispatcher lowering |
| `07-ir-passes.md` | §1, §2 (engine probe), §3 (relocation kinds), §4 (function address preparation), §5 removed (no indirect-call rewriting), §6 (no `v*` redirection of calls), §8 |
| `08-exceptions-and-sjlj.md` | engine-originated exceptions (null `call.indirect`) are reported and abort; exact `kind`/`in_pad` state machine; re-raise with a fresh marker instead of the caught object |
| `10-linker-and-runtime.md` | options (no budget or dispatcher options), startup |
| `11-testing-and-roadmap.md` | engine-patch milestone, tests, risks |
| `12-worked-examples.md` | §7 (descriptors), §8 (plain native `printf` call) |

E8, specified above, is already written into those documents: `01` §4 and §7, `02` §4.8,
`03` §9.1, `04` §5, `06` §6, `07` §2, `08` §1 and §8, `10` §5.3 and §6.1, `11` R2.
