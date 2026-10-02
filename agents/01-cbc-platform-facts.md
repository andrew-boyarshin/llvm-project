# 01 — The CBC Engine as a Code-Generation Target

This file describes the engine strictly from the point of view of a compiler backend that
must produce correct code for it. The engine is taken as analysed (revision `4e4a8ed8`)
**plus the eight accepted changes E1–E8** of `13-engine-changes.md`; where a fact differs
between the unpatched and the patched engine, both are stated and the backend targets the
patched one. Paths are relative to `/home/user/dev/cbc-engine-initial-stage` unless stated
otherwise.

Facts are given for both engine builds, x86-64 and AArch64 Linux. Only the x86-64 flavour
is in the current work scope; the AArch64 facts are recorded for the follow-up flavour
(`04-architecture.md` §2).

## 1. Execution pipeline

```
.cbc file ──load──▶ engine (lazy) ──first call of a method──▶ IsaRewriter
   original CBC bytecode                                      │
                                                              ▼
                                            internal RT bytecode + literal table
                                                              │
                                                              ▼
                                            interpretation loop (per fiber Ectype)
```

* The backend emits **original CBC bytecode** (`src/cbc/isa_opcodes.h`, parsed by
  `src/cbc/isa_parser.cpp`). It never emits RT bytecode (`src/cbc/isa_rt.h`).
* A method is rewritten the first time it is called (`FunctionHandleManager::Prepare`,
  `src/interpreter/function_handle.cpp:115`). Rewriting resolves every method/field/type
  reference the method uses, builds the frame layout, translates instructions, and joins
  the method's GC/stack-pointer tables with the rewritten positions.
* Any resolution failure during rewriting is fatal for the process
  (`FATAL("Rewriter failed: cannot rewrite code.")`, `src/cbc/isa_rewriter.cpp:1874`).
  Unresolvable native symbols in AOT direct calls also abort when the method is rewritten
  (`ResolveAotDirectCall` returns `nullopt` → rewriter failure). Consequence for the
  toolchain: **every method a program may call must resolve**; link-time checking is
  essential (see `10-linker-and-runtime.md` §3).

## 2. Register file

From `src/cbc/isa.h` and `src/interpreter/ectype.h`:

| Register | Index | Notes |
|---|---|---|
| `IRZ` (`IR0`) | 0 | Reads as 0. Never written by correct code. Debug builds assert on writes; release builds overwrite the slot and destroy the zero register. |
| `IR1` .. `IR13` | 1..13 | 64-bit untyped slots. |
| `IR_ACC` | 14 | Accumulator. Encodable in a 4-bit field. The engine writes it on `catch` delivery (exception object), in `newobj.acc`, boxing, `spawn`. |
| `FR0` .. `FR15` | 0..15 | 64-bit slots. `f32` values live in the low 32 bits (`Value::Primitive.f32`), `f64` in all 64. |

Registers are **memory** (`Ectype::iregs[15]`, `fregs[16]`), one `Ectype` per fiber,
allocated at first entry into the interpreter on that fiber (`engine_fiber_data_init`,
`src/runtimesupport/impl/entrypoint.cpp:174`). Native code never sees or modifies them;
they are copied into and out of machine registers only at native call boundaries.

### 2.1 Width semantics

* All slots are 64 bits. Width operands select how many bits an operation reads.
* **32-bit integer operations do not define the upper 32 bits of their destination.**
  `Arith<W32>` returns `Value::Primitive { .u32 = ... }`
  (`src/interpreter/internal/operations.h:364-419`) and `Ectype::Put` stores the whole
  union, whose upper half is not initialized by that aggregate initializer. The backend
  must treat the upper half of any `W32` result as undefined: truncation to `i32` is free,
  zero/sign extension to `i64` is an explicit instruction.
* Loads define all 64 bits: `LD_32` zero-extends, `LD_S8`/`LD_S16`/`LD_S32TO64`
  sign-extend, `LD_U8`/`LD_U16` zero-extend (`MemoryLocation::Load`,
  `operations.h:809-853`).
* `Mov32i` is rewritten to `AddI(W32, d, IRZ, imm)` when the constant does not fit 4 bits
  (`Emitter::MovImm`, `src/cbc/emitter/emitter.cpp:645-658`), so it also leaves the upper
  half undefined. The backend should always use `Mov64i` to materialize constants.
* Comparisons with `W32` compare the low 32 bits only (`Compare<cc, W32>`); `W64` compares
  all 64 bits.

## 3. The calling convention is the host C ABI

### 3.1 Register mapping at native boundaries

`src/arch_os/x86_64_linux/abi.inc` and `src/arch_os/aarch64_linux/abi.inc` define how
CBC registers are copied to and from machine registers:

| Host | Integer argument registers | Float argument registers | Native result copied back | Tail (stack-args) register | sret |
|---|---|---|---|---|---|
| x86-64 SysV | `IR1..IR6` = `rdi, rsi, rdx, rcx, r8, r9` (`IREG_ABI_AMOUNT 6`) | `FR0..FR7` = `xmm0..xmm7` (`movsd`) | `rax → IR1`, `xmm0 → FR0` | `IR7` (`TAIL_REG`) | `IR1` (= `rdi`, `HAS_SRET_SHIFT 1`) |
| AArch64 AAPCS64 | `IR1..IR9` = `x0..x8` (`IREG_ABI_AMOUNT 9`) | `FR0..FR7` = `d0..d7` | `x0 → IR1`, `d0 → FR0` | `IR10` | `IR9` (= `x8`, `HAS_SRET_SHIFT 0`) |

(`src/interpreter/platform_traits.h`, `src/arch_os/*/platform_asm_export.h`.)

The engine's `i2c` path (`engine_i2c_call`, `trampolines.S:582-602` for x86-64,
`557-578` for AArch64) loads **all** argument registers and **all** eight float argument
registers unconditionally, calls the target, then stores exactly **one** integer and
**one** float result. Consequences:

* A native callee sees its arguments exactly where the host ABI puts them, provided the
  CBC caller placed them in the mapped CBC registers.
* **Second result registers are lost**: `rdx`/`x1` and `xmm1`/`d1..d3` are not copied
  back. Native functions returning `__int128`, two-eightbyte aggregates
  (`ldiv_t`, `lldiv_t`, `imaxdiv_t`, `{double,double}` complex values, HFAs of 2–4
  elements on AArch64) cannot be called correctly. The compiler expands the `div` family
  inline and rejects every other such native call at link time
  (`10-linker-and-runtime.md` §5.4).
* Unpatched engine, x86-64: `al` is not set before a native call; it holds the low byte of
  the address of `engine_i2c_call` (the interpreter reaches the adapter with `jmp rax`,
  `trampolines.S:511`). The adapter is 16-byte aligned, so `al` is any multiple of 16,
  including 0, and a native variadic callee that receives floating-point variadic
  arguments may skip saving vector registers. **Fixed by E7**: the adapter sets `al = 8`
  before `call r10` (`13-engine-changes.md` §10), so native variadic calls with `double`
  arguments are plain calls (`04-architecture.md` §10.2).
* A native call clobbers only `IR1` and `FR0` in the CBC register file. All other CBC
  registers (including the "volatile" ones) survive, because native code cannot see the
  `Ectype`.

### 3.2 Stack-passed arguments

* The interpreted frame's untyped slot 0 is at the native stack pointer at the moment of
  a call (`perform_2i_call`: frame is reserved by `sub rsp, frameSize`, the interpreter
  receives `rsp` as `Frame.start`; `trampolines.S:466-503`).
* For a **native** callee, the CBC caller's untyped slots `0, 1, 2, …` are the host's
  outgoing stack-argument words (x86-64: the callee sees them at `[rsp+8]` after `call`
  pushed the return address; AArch64: at `[sp]`).
* For a **CBC** callee, `engine_i2i_call` stores the caller's `rsp`/`sp` into the tail
  register before entering the callee (`mov [ECTYPE + TAIL_REG_OFFS], rsp`,
  `trampolines.S:608-613`; AArch64 `546-553`). The callee reads stack argument *k* as
  `[TAIL + 8k]` (`LoadTailParam`, or `LoadRawMemory` with base `TAIL`).
* For a CBC callee entered from native code (only the entry point, §10), the tail
  register is `rbp + 16` of the `c2i` adapter frame, i.e. the native caller's stack
  arguments.
* The tail register is therefore **written by the engine on every CBC→CBC call**: it is
  call-clobbered and live-in at function entry.

### 3.3 Callee-saved registers

* `IR8..IR13` and `FR8..FR15` are saved and restored by the engine **only if the callee
  declares them** in `usedNonVolIRegMask` (bit *i* = `IR(8+i)`, 6 meaningful bits) and
  `usedNonVolFRegMask` (bit *i* = `FR(8+i)`, 8 bits) of its `MethodCode`
  (`makeFrameLayout`, `isa_rewriter.cpp:1691-1700`; `PUSH_REGS`/`POP_REGS`,
  `trampolines.S:169-223`). Bit 6 of the integer mask would refer to `IR_ACC`; never set
  it.
* The save area sits at the top of the frame, above typed slots; it is engine-managed and
  invisible to the bytecode.
* Because the tail register is overwritten **before** the callee prologue saves
  registers, the tail register must not be treated as callee-saved. On AArch64 this
  removes `IR10` from the callee-saved set; `IR8`/`IR9` there are argument/sret
  registers and are also excluded. Callee-saved sets (`04-architecture.md` §5,
  `06-backend.md` §3):
  * x86-64 flavour: `IR8..IR13`, `FR8..FR15`.
  * AArch64 flavour: `IR11, IR12, IR13`, `FR8..FR15`.
* `IR13` is an ordinary callee-saved register (mask bit 5). The engine already saves it
  when that bit is set. It is not reserved.
* During exception unwinding, each unwound frame's declared registers are restored
  (`.Lunwind_top_frame` runs `POP_REGS`, `trampolines.S` after `.Lexception_handlers`), so
  callee-saved values are intact when a landing pad starts. Volatile registers are not.

### 3.4 `AbiInfo` and why signature terms matter

`BuildAbiInfo` (`src/interpreter/code.cpp:31-107`) derives, from the method's signature
term and flags, bitmaps used only by the GC and by stack relocation: which incoming
registers are references, which point into the stack, whether the tail register is in
use. For C code no parameter is a reference or a record, so only one bit matters:

* `hasTailReg`: when the callee's prologue triggers a stack move, the tail register is
  adjusted only if this bit is set.
  * Unpatched engine: `hasTailReg = iargIdx > IREG_PARAM_PASSING_AMOUNT`, where `iargIdx`
    counts **non-float** parameters starting at 1; float parameters that overflow
    `FR0..FR7` onto the stack do not set it, so a function whose only stack-passed
    parameters are floats would read a stale tail pointer after a stack move.
  * Patched engine (**E2**): the bit is also set when `fargIdx > FREG_ABI_AMOUNT`. The
    signature term therefore only has to list the lowered parameters correctly — integer
    vs. float classification and count — and no padding is needed.
* The signature term is also used to choose the `c2i` adapter (`CountRegs`,
  `src/interpreter/adapters.cpp:13-29`: integer-only adapter when no term is a float).
  Only the entry method is ever entered through `c2i` in this plan, but terms are kept
  accurate everywhere.
* Do not set the method `SRET` flag for C functions. It changes the bitmaps only and would
  mark the sret register as a managed stack pointer. C sret buffers are ordinary bytes in
  the caller's untyped memory block (`04-architecture.md` §5).

## 4. Frames

`makeFrameLayout` (`isa_rewriter.cpp:1687-1745`) builds the frame from the `MethodCode`
header:

```
low address (Frame.start = native SP during the method, 16-byte aligned)
  untyped slot 0           ┐ 8 bytes each, untypedSlotCount of them
  untyped slot 1           │ (outgoing stack args must be slots 0..k-1;
  ...                      ┘  spills follow the outgoing area)
  --- only when untypedMemSize > 0 ---
  pad to 16                so the block itself is 16-byte aligned
  untyped memory           untypedMemSize bytes (E8)
  --- end of that region ---
  typed slot 0             ┐ one per stackAllocSigs entry; size = flat size of the term,
  typed slot 1             │ each aligned up to 8; only RECORD terms allowed
  ...                      ┘
  saved non-volatile regs    engine-managed
  alloca watermark           8 bytes, present only when usesAlloca = 1 (E8);
                             engine-private, not addressable by bytecode
high address               ; total aligned up to 16 (FRAME_ALIGNMENT)
```

`untypedMemSize == 0` inserts nothing: no block and no alignment gap. Typed slots and
saved registers then start at the same offset as they did before E8, and the frame size
is unchanged. That is every Cangjie method (the encoder writes 0). The pad exists only
to align a block that is actually present.

* The **whole frame is zeroed** on every entry (`.Lframe_zero_loop`,
  `trampolines.S:480-492`). Frame size is a per-call cost.
* Untyped slots are addressed by index (`LoadUntyped`/`StoreUntyped`, offset `8*us`).
* Unpatched engine: `Emitter::StoreFrame` (`emitter.cpp:957-966`) encodes offsets ≥ 4096
  with `STORE_LONG_REC` and base `IRZ`, which the interpreter treats as an **absolute
  address**, so any `StoreUntyped` to slot ≥ 512 or a typed-slot store at frame offset
  ≥ 4096 writes to a wild address. Patched engine (**E1**): the long form is
  `STORE_LONG_FRAME`, frame-relative like the already-correct `LOAD_LONG_FRAME`. The only
  remaining limit is the `u16` slot index of `LoadUntyped`/`StoreUntyped` (65 535 slots,
  512 KiB) and the `u32` frame size.
* Frame memory is on the fiber stack, which does not move (§7). `lea.frame` (E8) takes
  the address of a byte in the untyped memory block. That address stays valid until the
  frame returns. The tail register still points at the caller's frame and is overwritten
  on the next CBC→CBC call.

## 5. Calls

| Instruction | Use in the C target | Rewriter behaviour (`isa_rewriter.cpp`) |
|---|---|---|
| `CallDirect dst, @mref` (`RegSymGroup` sel 3) | Calls to CBC functions and to native functions | Resolves the reference. CBC target: `call.2i` through a `DynamicFunctionHandle` (lazy rewrite on first call). AOT reference: `call.2c` to the `dlsym`ed address. Binds a state point after the call. Moves `IR1` to `dst` if different (`dst` must therefore be `IR1`; never `IRZ`). |
| `CallVirt` / `CallInterf` | Not used | Dispatch through the receiver's method table; receiver in `IR1`. |
| `LoadFuncPtr dst, @mref` (`RegSymGroup` sel 13, **E5**) | Every function-pointer value: CBC functions and native functions | Resolves the reference like `CallDirect`. AOT target: `dst` = the `dlsym`ed address. CBC target: `dst` = the address of the method's 16-byte **function descriptor** in the engine's non-executable descriptor region, created on the first request for that method (no fixed limit, `13-engine-changes.md` §9). Baked in as a 64-bit immediate. Not a state point. |
| `CallIndirect reg` (`RegGroup` sel 9, **E6**) | Every indirect call | New RT opcode `CALL_REG`. At run time: 0 → `NoneValueException`; a descriptor address → the method's `i2call` adapter, i.e. exactly `call.2i`; any other address → `engine_i2c_call`, i.e. exactly `call.2c`. Binds a state point. Result in `IR1`/`FR0` (no destination operand). |

Resolution of a direct call (`ResolverProxy::ResolveCall(DirectCall)`,
`src/resolution/resolution.cpp:699-759`):

* If the reference has the `AOT` flag: the linkage name from the direct-call AOT table is
  looked up with `dlsym` across the main executable and the file's `aotDeps` libraries
  (`Dependencies::FindSymbol`, `src/engine/dependencies.cpp`). The reference's type term
  and signature are not used to locate the target (they still must parse).
* Otherwise: the reference type term is resolved to a type definition by **name** across
  all loaded files, then the method is found by name in the type's member index or virtual
  list, matching the signature term exactly. **The superclass chain is not searched.**

### 5.1 State points and GC maps

After each `CallDirect`/`CallIndirect` (E6)/`CallVirt`/`CallInterf`/`NewObj`/`NewArr`/
`GcPoint`/`Spawn`/`Box` (and a few generic instructions), the rewriter records a *state
point* at the original
position **immediately after the instruction**. When joining tables
(`CalculatePositionalGCInfo`, `isa_rewriter.cpp:1747-1795`) every state point must have a
`LivenessInfo` entry with exactly that `cbcPos`, otherwise the engine aborts with
`FATAL("Unknown position")`.

For C code every entry is `{cbcPos, regMask = 0, refSlots = [], mutPairs = []}`
(4–6 bytes each). `StackPtrsInfo` entries are optional (sparse) and the C target never
emits them. The sample `default.dasm` shows the pattern: `user.main` has liveness entries
at 16, 22, 47, 62, 77, 96, 108, 121, 130 — the positions after each `call.direct`.

### 5.2 Safepoints

`GcPoint` checks whether the runtime requested a safepoint. A CBC loop without calls
never reaches one, which can stall a stop-the-world GC requested by another runtime
thread. The C target inserts `gcpoint` on loop back edges by default (each with an empty
liveness entry). Native calls do not perform a managed→native state transition; a
blocking native call blocks GC as well. This is acceptable for single-threaded C programs.

## 6. Exceptions

* `MethodCode` carries `ExceptionRegion {start, end, target}` triples (original byte
  positions, ULEB each).
* On a throw, `engine_get_exception_handler` (`src/runtimesupport/impl/exception_handling.cpp:12-61`)
  takes `exPos = rtPos - 1` (inside the throwing RT instruction), maps every region's
  `start` and `end` to RT offsets, and picks the **first** region with
  `rtStart <= exPos <= rtEnd` (inclusive of the RT start of the instruction at `end`).
  Regions should use `end` = the original position just after the call they protect, as
  the Cangjie producer does (`[0, 9) -> 24` in `default.dasm` covers a call at 6..9).
  Never let regions overlap; order inner regions first.
* `start`, `end`, `target` must be original instruction boundaries (the offsets index
  maps only boundaries; a miss is `FATAL`).
* When a handler is found, the exception object is put in `IR_ACC` and execution resumes
  at `target`. Handlers conventionally execute `Catch reg`, which copies `IR_ACC` to `reg`
  and falls through when it is non-null; the engine does not require it (nothing checks
  what instruction is at `target`, and no "exception in flight" state exists besides
  `IR_ACC`). The C target never needs the engine object (`08-exceptions-and-sjlj.md` §2)
  and does not emit `Catch`.
* Frames without a handler are unwound: their saved non-volatile registers are restored,
  the frame is popped, and the search continues in the caller. If unwinding reaches the
  native boundary, the exception is rethrown into the Cangjie runtime.
* Throw sources usable from compiled code without any runtime type dependency:
  * `NullCheck IRZ` raises `NoneValueException`, `DivCheck IRZ` raises
    `ArithmeticException` (objects created by the runtime through the Cangjie helper
    library, `src/runtimesupport/impl/rt_syms.cpp`).
  * `Throw reg` rethrows an exception object the code already holds (e.g. from `catch`).
* Engine-originated exceptions the C target can encounter without raising them on purpose:
  `NoneValueException` from `CallIndirect` through a null pointer (E6) and
  `ArithmeticException` from `DivCheck` in sanitizer mode. The runtime treats them as fatal
  diagnostics (`08-exceptions-and-sjlj.md` §4.3).
* There is no exception type filtering in the engine; all type matching is the compiled
  code's job (`08-exceptions-and-sjlj.md`).

## 7. The fiber stack does not move

The interpreter runs on a fiber stack whose existing frames stay where they are. Growth,
when the engine grows the stack, extends the mapping; it does not relocate a live frame.
An address taken from the current frame therefore remains valid until that frame returns,
including after the pointer has been stored in the heap or passed to native code. The usual
C rule still applies: the pointer is dangling once the frame has returned.

`StackExpansion::VisitFrameRootsForStackPtrs`
(`src/runtimesupport/impl/stack_expansion.cpp:53-155`) is not on the path this target
relies on. The C target emits empty `StackPtrsInfo`. It does not set the method `SRET`
flag. Consequences for code generation:

* Static-sized locals, byval copies, sret buffers and variadic buffers live in the frame's
  untyped memory block. `MethodCode` bakes the block's size. `lea.frame` materializes
  `untypedMemBase + disp` (`04-architecture.md` §5, E8).
* Dynamic `alloca` and VLAs do not fit in that size. They are the `alloca` instruction.
  The engine keeps their bytes on a shadow stack and frees them when the frame returns or
  is unwound (E8).
* The tail register is still overwritten on every CBC→CBC call, so the callee reads
  incoming stack arguments, or copies a `byval` aggregate into its untyped memory, in the
  entry block before the first call. A pointer saved from the tail register remains valid
  for the rest of the callee, because the caller's frame does not move; the callee still
  has to capture it before the tail register is overwritten.
* `LoadStackRec` (address of a typed slot) may be kept across calls. The `InitString`
  sequence (§9) still consumes it immediately.

## 8. Statics

`StaticsManager` (`src/engine/statics_manager.cpp`) allocates one bundle per type the
first time any static of that type is resolved, with `std::make_unique<char[]>`
(heap, never freed, **never moved**, zero-filled). Layout: primitives (8 bytes each), then
references (8 bytes each), then records (each aligned to 8, in declaration order).

* A type with a **single** static field of record kind (e.g. `VArray<U64, N>`) places that
  field at offset 0 of the bundle, which is aligned like `operator new[]` (16 bytes on
  x86-64 and AArch64 glibc).
* `LdStatic`/`StStatic`/`LeaStatic` resolve the static's address during rewriting and bake
  it into the RT code as a literal (`emit.NewAddressSym(location)`), so the address is a
  constant at run time.
* `LeaStatic dst, baseRef, @field` also writes the "global base pointer" immediate into
  `baseRef` (`isa_rewriter.cpp:489-496`). `baseRef` must be a register that is dead at that
  point and different from `dst` (the backend models it as a dead second definition of an
  ordinary integer register); never `IRZ`, and not `IR_ACC`, which the engine writes on
  exception delivery and stack growth and the backend therefore never allocates
  (`04-architecture.md` §9.1).
* AOT static fields: a field reference whose owner term is an AOT type and that has a
  `StaticFieldAotData{linkageName}` entry resolves to `dlsym(linkageName)`
  (`ResolveAotStaticField`, `resolution.cpp:205-222`). This yields the address of any
  native data symbol (`stdout`, `environ`, `optarg`). (It also works for native
  *functions*, but with E5 the C target obtains native function addresses with
  `LoadFuncPtr` on the same AOT method reference it uses for calls, so a function never
  needs both a method reference and a field reference.)

## 9. `InitString` as a blob loader

`InitString ts, strOffset` (`isa_rewriter.cpp:1367-1387`; `LABEL(STRING_INIT)`,
`src/interpreter/interpretation_loop.cpp:1254-1272`):

* At rewrite time: reads the string-pool entry (ULEB length + bytes, no encoding
  validation), `malloc`s `StringStorage { TypeInfo* typeInfo; uint64_t size; char bytes[size+1]; }`
  (`src/interpreter/literals.h:10-14`), copies the bytes, NUL-terminates. The storage is
  never freed and never moved.
* At run time: writes `{storage*, start = 0, length = size}` (16 bytes) into typed slot
  `ts` of the frame.
* The typed slot's term only has to be a RECORD with flat size ≥ 16. The C target uses
  the tuple term `(U64, U64)`; no dependency on `std.core:String`.
* The method is rewritten once, so the storage pointer is stable for the process
  lifetime. `bytes` is at `storage + 16`; `malloc` alignment (16) makes it 16-byte
  aligned.

This is how the compiler loads the initial contents of the data image and other
read-only tables at startup (`04-architecture.md` §6).

## 10. Entry point and process model

* The engine is started by a launcher (`tools/launcher/launcher.c`) that initializes the
  Cangjie runtime, loads the main `.cbc`, and runs the entry trampoline in a Cangjie task.
* `Engine::FindMain` (`src/engine/engine.cpp:354-367`) takes the file's `mainTypeName` and
  requires **exactly one** member named `main` on that type (`ASSERTION(mcount == 1)`).
* `main` is called with **no arguments** through a direct-call trampoline; its integer
  result (`long`) becomes the process exit status (`run_interpreter_in_managed_ctx`).
* Program arguments are given to the Cangjie runtime (`SetCJCommandLineArgs`), not to the
  CBC `main`. On Linux the C runtime reads `/proc/self/cmdline` (`10-linker-and-runtime.md`
  §6.3).
* Native code calling CBC code is **out of scope**. No instruction available to the C
  target exposes a direct-call trampoline: E5 (`LoadFuncPtr`) returns non-executable
  function descriptors for CBC methods, which serve only as function-pointer identities
  and as `CallIndirect` targets. Whether a native entry into the interpreter would be safe
  (fiber stack relocation and GC stack walks across native frames, exceptions crossing
  native frames, asynchronous entry) has not been verified (`13-engine-changes.md` §8).

## 11. Symbol lookup and native libraries

* `aotDeps` (header string, `':'`-separated names) lists native libraries; each name `X`
  is opened as `libX.so` (`UpdateSharedObjName`, `engine.cpp:188-207`; `.dylib` on Apple).
  `dlopen` failures are not reported.
* The current executable is always searched first (`OpenCurrentExecutable`), so symbols of
  the launcher's dependencies (glibc, libstdc++, libm typically) resolve without listing
  them.
* glibc's `libm.so` is an `ld` script, not a shared object; versioned names like
  `libm.so.6` cannot be expressed through `lib<name>.so`. Rely on the executable's global
  scope or ship symlinks.

## 12. Limits and engine quirks the backend must respect

| # | Constraint | Source | Backend rule |
|---|---|---|---|
| Q1 | Unpatched: method-reference and field-reference pool sizes are truncated to `uint16_t`. **Fixed by E3** (read as `uint32_t`). | `ReadRegion`, `src/engine/decode/reader.cpp:504-507` | No practical limit on the patched engine. |
| Q2 | `FUNCTIONAL` term arity is a `u8` | `terms.cpp:916` | ≤ 255 parameters per CBC signature. |
| Q3 | Per-method RT literal table holds 4 096 entries; overflow is a debug-only assert | `src/cbc/isa_rt.h:326`, `emitter/symbols.cpp` | Count literal-consuming instructions per function (`02-isa-encoding.md` §8) and split or reject functions that exceed a safe budget (3 500). |
| Q4 | Unpatched: `StoreFrame` offsets ≥ 4096 write to an absolute address. **Fixed by E1.** | `emitter.cpp:957-966` | Untyped slot index < 65 536 (the `u16` operand). |
| Q5 | `LoadRawMemory`/`StoreRawMemory` displacements are passed to `Emitter::LoadRec/StoreRec(uint32_t offset)`; the short form is a 12-bit **unsigned** field, the long form a `u32` that the interpreter zero-extends | `isa_rewriter.cpp:612-620`, `emitter.cpp:924-944`, `interpretation_loop.cpp:872-884` | Fold only displacements in `[0, 2^31)` into raw memory instructions; materialize negative displacements with an `add`/`sub`. |
| Q6 | `LoadRec`/`StoreRec` with a base register holding 0 is `FATAL` | `interpreter.h:383-440` | Null dereference aborts the process (acceptable: undefined behaviour in C). Never use `IRZ`-based absolute addressing for real addresses. |
| Q7 | `LD_U8TO64`, `LD_U16TO64`, `LD_U32TO64`, `LD_S8TO64`, `LD_S16TO64` are `FATAL` in the interpreter | `operations.h:839-854` | Use `LD_U8`, `LD_U16`, `LD_32`, `LD_S8`, `LD_S16`, `LD_S32TO64`, `LD_64`. |
| Q8 | Division/remainder by zero fails the RT instruction and jumps to `HALT` (`FATAL`) | `operations.h:326-360`; `cbc_scala_gaps.md` appendix | Undefined behaviour in C; optionally guard with `DivCheck` under `-fsanitize=integer-divide-by-zero`. `INT_MIN / -1` returns `INT_MIN`, remainder 0. |
| Q9 | `Convert` supports only certain pairs; others assert (debug) or produce 0 (release) | `src/interpreter/casts.h` | Use only the pairs in `02-isa-encoding.md` §4.6. |
| Q10 | Branch and exception positions must be instruction boundaries; a missing label is `-1` in release | `emitter/symbols.cpp`, `exception_handling.cpp` | Guaranteed by construction in MC. |
| Q11 | Unpatched: an unknown opcode makes the parser loop forever. **E4**: it aborts with a message. | `isa_parser.cpp:244-255` (no `default:`) | Never emit opcode ≥ `0x94`, `_END`, or a stray `WidePrefix`; never emit `RegSymGroup` selector ≥ 14 or `RegGroup` selector ≥ 15. Selectors 10–14 are E8 (`lea.frame`, `alloca`, `stacksave`, `stackrestore`, `ld.fcb`). An engine without E8 rejects them in the group switch. |
| Q12 | `FrameDescProvider` reads `SourceFullName().value()` when `SourceFile()` is present | `exception_handling.cpp:153-159` | Emit both `sourceFile` and `sourceFullName` tags, or neither. |
| Q13 | Direct calls do not search the superclass chain | `resolution.cpp:714` | C functions are static methods of a single program type; no inheritance involved. |
| Q14 | The method-code header field after `stackAllocSigs` is a single ULEB (`ohmSlotCount`) | `reader.cpp:219` vs. the Scala encoder's `variableSizeTypes` list | Write a single `0`. |
| Q15 | `maxCalleeStackArgsCount` and `mayHaveNativeCalls` are read but unused | `reader.cpp:223-225` | Fill them accurately anyway (future engines may use them). |
| Q16 | Dynamic-call trampolines are a 1 024-entry table without bound check | `cbc_scala_gaps.md` §1.7, `adapters.cpp:61-66` | Not relevant: the C target never emits virtual methods or `CallVirt`. |
| Q17 | Direct-call trampolines are limited to 1 024 distinct interpreted methods per process (`TRAMPOLINE_COUNT`); exhaustion is `FATAL("Too many direct call links")` | `adapters.cpp:44-58`, `asm_export.h:15` | Not relevant: E5 gives CBC functions descriptors instead of trampolines (`13-engine-changes.md` §9), so the C target uses no direct-call trampoline except the launcher's one for `main`. |
| Q18 | `Catch` with a null `IR_ACC` jumps to `HALT` | `interpretation_loop.cpp:1320-1327` | Handlers are entered only through the engine's unwinder, which always sets `IR_ACC`. |

## 13. What the engine gives the C target for free

* A register machine with 13 general and 16 float registers, cheap 3-address arithmetic,
  32- and 64-bit operations, full 64-bit immediates.
* Raw memory loads/stores of 8/16/32/64-bit integers and 32/64-bit floats with sign or
  zero extension; unaligned accesses work (plain C++ dereferences on x86-64/AArch64).
* Atomic operations (sequentially consistent) on any address — through a record-field trick
  that needs no engine change (`04-architecture.md` §11).
* Exact host-ABI native calls by symbol name (including variadic ones with `double`
  arguments, E7), and (E5/E6) indirect calls through any native function address or CBC
  function descriptor.
* Precise exceptions with unwinding through interpreted frames.
* A growable call stack that does not move live frames (§7). Deep recursion works.
  Addresses of the current frame stay valid until the frame returns.
