# CBC LLVM Target — Design and Implementation Plan

This directory specifies a new LLVM target, **CBC**, that compiles C and C++ to the
**original CBC bytecode** (`.cbc` files) executed by the CBC engine
(`/home/user/dev/cbc-engine-initial-stage`). It is written for the engineers who will
implement it. Everything here was derived from the engine sources, the existing CBC
producer (`/home/user/dev/cbc_compiler`, the Scala/Cangjie backend), the sample
disassembly `cbc-engine-initial-stage/sandbox/default.dasm` and the matching
`default.cbc` binary, and the LLVM tree at `llvm-project-upstream` (LLVM 24, main).

## Constraint: eight engine changes, nothing else

The CBC engine, its interpreter, rewriter, trampolines and runtime glue are treated as a
fixed platform **except for eight accepted changes** specified in
`[13-engine-changes.md](13-engine-changes.md)` (not implemented yet). E1–E7 are small.
E8 is the frame-memory and runtime-alloca change:

| Id | Change |
|---|---|
| E1 | frame stores at offsets ≥ 4 096 use the frame-relative long form (fixes writes to absolute addresses) |
| E2 | `hasTailReg` also accounts for float parameters passed on the stack |
| E3 | method/field reference counts are read as 32-bit (no 65 535 limit) |
| E4 | the bytecode parser aborts on unknown opcodes instead of looping |
| E5 | new `LoadFuncPtr dst, @mref` (`RegSymGroup` selector 13): native address of an AOT function, or the address of a CBC method's **function descriptor** (non-executable, engine-allocated, no fixed limit) |
| E6 | new `CallIndirect reg` (`RegGroup` selector 9): descriptor → ordinary interpreted call, other address → native call, null → `NoneValueException` |
| E7 | the native-call adapter (x86-64) sets `al = 8`, so native variadic functions receive `double` arguments correctly |
| E8 | the interpreter frame gains an untyped memory block whose size `MethodCode` bakes; `lea.frame` takes its address; `alloca` / `stacksave` / `stackrestore` are bytecode; the engine owns the shadow stack (grows it without moving live allocations, frees a frame's allocations on return and on unwind); `ld.fcb` returns the per-fiber control block |

Every other C/C++ feature the engine does not support directly is implemented in the
compiler, in the linker, or in CBC-compiled runtime libraries, and the remaining engine
quirks are worked around (`[01-cbc-platform-facts.md](01-cbc-platform-facts.md)` §12).
E1–E7 reuse existing engine paths. E8 adds frame memory and a runtime shadow stack
(`13-engine-changes.md` §11). **Native code calling CBC code (callbacks) stays out of
scope**, and CBC function pointers are deliberately not executable (`13-engine-changes.md`
§8).

## Work scope

| Item | Status |
|---|---|
| `cbc_x86_64-unknown-linux-gnu` | **in scope**: implemented, tested, shipped |
| `cbc_aarch64-unknown-linux-gnu` | **follow-up**: fully designed in these documents (register mapping, ABI, `va_list`, data layout), not in the current work scope (`04-architecture.md` §2) |
| whole-program compilation (bitcode objects, code generation in `cbc-ld`) | **in scope**: the only build model |
| separate code generation ("phase 2": relocatable CBC objects, per-object data images) | **postponed indefinitely**: kept as a design sketch (`10-linker-and-runtime.md` §4, `04-architecture.md` §6.5), not implemented |
| Apple / OpenHarmony triples, native→CBC callbacks, threads | not planned |

## Reading order


| #   | File                                                     | What it covers                                                                                                                                                                                                                                                                                                                                           |
| --- | -------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | `[01-cbc-platform-facts.md](01-cbc-platform-facts.md)`   | The engine as a code-generation target: register file, how CBC registers map onto the host C ABI, frames, calls into CBC and into native code, state points and GC maps, exception unwinding, the unmovable fiber stack, statics, `InitString`, symbol lookup, entry point, limits, engine bugs to avoid.                                                  |
| 2   | `[02-isa-encoding.md](02-isa-encoding.md)`               | Byte-exact encoding of every instruction the backend emits, verified against `isa_parser.cpp` and against real bytes in `default.cbc`. Corrections to `isa_reference.md`. Rewriter expansion and literal-table cost model.                                                                                                                               |
| 3   | `[03-cbc-file-format.md](03-cbc-file-format.md)`         | Byte-exact `.cbc` writer specification: header, unified pool, terms, references, definitions, method code blocks, exception/liveness tables, hash indices, AOT tables, region trailer; recommended pool ordering that makes a single-pass writer possible.                                                                                               |
| 4   | `[04-architecture.md](04-architecture.md)`               | The overall mapping of C/C++ semantics onto CBC: target triples, whole-program model, memory model, frame memory and the runtime shadow stack, global data image, function pointers (engine descriptors via `ld.fnptr`, every indirect call a `call.indirect`), native interop, varargs, exceptions, setjmp/longjmp, TLS, startup, threads, known limitations.                                                     |
| 5   | `[05-llvm-core-changes.md](05-llvm-core-changes.md)`     | Changes outside `lib/Target/CBC`: `Triple`, calling-convention IDs, the new `CBC` object format in `BinaryFormat`/`MC`/`Object`, `TargetLoweringObjectFile`, CMake/registration, `ExpandVariadics`, runtime libcall tables.                                                                                                                              |
| 6   | `[06-backend.md](06-backend.md)`                         | `llvm/lib/Target/CBC` in full: file inventory, TableGen (registers, formats, instructions, patterns, calling convention), `ISelLowering` operation-by-operation, frame lowering, register info, instruction info, branch analysis, AsmPrinter and module tables, MC layer (code emitter, relaxation, printer, parser, disassembler), TTI, pass pipeline. |
| 7   | `[07-ir-passes.md](07-ir-passes.md)`                     | The target-specific IR passes that run before instruction selection: global data image, function-address preparation, native-call legalization, variadics, EH and SjLj lowering, loop safepoints, entry synthesis. Frame memory and `alloca` are backend and engine work, not an IR pass.                                                                                                                    |
| 8   | `[08-exceptions-and-sjlj.md](08-exceptions-and-sjlj.md)` | C++ exceptions and `setjmp`/`longjmp` on top of CBC exception regions: the marker-exception protocol and its per-fiber state machine, reporting of engine-originated exceptions, landing pad code, selector computation, cleanups, `resume`, `std::terminate`, shadow-stack freeing by the unwinder (E8), the `_Unwind_*` replacement.                                                                                        |
| 9   | `[09-clang.md](09-clang.md)`                             | Clang: `Basic/Targets/CBC`, ABI delegation to the host's `X86_64ABIInfo`/`AArch64ABIInfo`, predefined macros, header strategy, driver tool chain, defaults (emulated TLS, no stack protector, no jump tables), diagnostics.                                                                                                                              |
| 10  | `[10-linker-and-runtime.md](10-linker-and-runtime.md)`   | The link step (`cbc-ld`), bitcode whole-program link, the postponed relocatable-object design, and the runtime: CBC `crt`, compiler-rt builtins, libc strategy (native glibc, compile-time rules, unsupported callback-taking functions), libc++/libc++abi, emulated TLS, startup and shutdown sequence, `argv`.                                                                                          |
| 11  | `[11-testing-and-roadmap.md](11-testing-and-roadmap.md)` | Test strategy (lit, MC round-trips against the engine disassembler, end-to-end runs on the engine), milestones with scope and effort, risk register.                                                                                                                                                                                                     |
| 12  | `[12-worked-examples.md](12-worked-examples.md)`         | End-to-end examples: C source → LLVM IR → CBC assembly → bytes, covering arithmetic, globals, calls to libc, struct-by-value, indirect calls, varargs, exceptions.                                                                                                                                                                                       |
| 13  | `[13-engine-changes.md](13-engine-changes.md)`           | The accepted engine changes E1–E8: problem, exact code change, tests, compatibility, effect on the compiler plan; the rejected suggestions and the workarounds that therefore stay; why callbacks remain out of scope; the function descriptor region; frame memory and the runtime shadow stack.                                                                                                             |
| 14  | `[14-libcxx.md](14-libcxx.md)`                           | libc++ and libc++abi as CBC bitcode, why libunwind is not built, why the unwind entry points live in `crt-cbc.bc`, the host-vs-guest cmake split (including the `LIBCXXABI_USE_LLVM_UNWINDER` configure failure), and the file-level port. Threads off; the later `pthread_create` analysis is `15`. Authoritative over `10-linker-and-runtime.md` §7 where they differ. |
| 15  | `[15-threads.md](15-threads.md)`                         | Not in the current work scope. What an engine `pthread_create` would have to provide, and what libc++ would still need after that (keys, safepoints, `fork`, signals, host-created threads). |
| 16  | `[16-float-codegen.md](16-float-codegen.md)`             | Floating-point arithmetic, compares, bitcasts, conversions, and float returns. The ISA already has them; the hand-written selector and `RET` do not. Blocks `std::string` and `std::complex`. |
| 17  | `[17-integer-div-and-high-mul.md](17-integer-div-and-high-mul.md)` | `sdiv`/`udiv`/`srem`/`urem` and signed high multiply. The ISA has the four division ops; `MULHU` is already expanded and `MULHS` is not. Blocks `std::chrono` system clock. |
| 18  | `[18-bit-intrinsics.md](18-bit-intrinsics.md)`           | `ctpop`, `ctlz`, `cttz`, and rotates reach instruction selection because they were never marked Expand. Blocks `std::sort`, `bitset::count`, and `std::hash`. |
| 19  | `[19-static-initialization.md](19-static-initialization.md)` | `llvm.global_ctors` are not run, and `__cxa_atexit` is still the host symbol, so the linker rejects CBC destructors. Blocks iostreams, filesystem, and locale. |
| 20  | `[20-setjmp-isel.md](20-setjmp-isel.md)`                 | `CBCLowerSjLj` builds a CFG that dies in live-variable analysis. `setjmp`/`longjmp` do not compile. |
| 21  | `[21-native-aggregate-abi.md](21-native-aggregate-abi.md)` | Clang still uses the default ABI, so native struct returns such as `div_t` are passed as hidden pointers. Host glibc returns them in `rax`. |
| 22  | `[22-method-resolution.md](22-method-resolution.md)`     | Some libc++ calls compile as `call.direct` and then the engine cannot find the method: `std::map` emplace and `std::runtime_error`. A class throw also misses a GC liveness position. |




## The ten decisions that shape everything

1. **One LLVM architecture, two ABI flavours.** CBC virtual registers are a literal image
  of the host C calling convention (on x86-64 `IR1..IR6` are `rdi..r9`, `FR0..FR7` are
   `xmm0..xmm7`; on AArch64 `IR1..IR9` are `x0..x8`). Native calls are made by copying
   those registers into machine registers. A `.cbc` file is therefore host-ABI specific.
   Triples: `cbc_x86_64-unknown-linux-gnu` (current work scope) and
   `cbc_aarch64-unknown-linux-gnu` (designed, follow-up) — `Triple::cbc` plus a
   sub-architecture. Clang reuses the host's ABI lowering.
2. **C pointers are plain 64-bit integers; all memory access is raw.** The backend uses
  `LoadRawMemory`/`StoreRawMemory` (base register plus non-negative displacement) and
   never the typed field/object instructions, so no GC references exist and every
   GC liveness map is empty.
3. **Fixed locals live in the interpreter frame. The shadow stack is only for `alloca`.**
   Fiber stacks do not move, so a pointer into the current frame stays valid until that
   frame returns. Every static-sized local lives in an untyped memory block whose size
   `MethodCode` records; the engine allocates it with the frame. `alloca` and VLAs are
   the `alloca` instruction. The engine owns that shadow stack: it grows by new mappings
   that leave existing allocations in place, and it frees a frame's allocations when the
   frame returns or is unwound. `IR13` is an ordinary callee-saved register.
4. **All global data lives in one static "data image".** The whole program's
  `.data/.rodata/.bss` is laid out by the compiler into a single static field of type
   `VArray<U64, N>` whose address is a rewrite-time constant (`LeaStatic`). Initial bytes
   are loaded at startup with `InitString` (which mallocs a permanent copy of an arbitrary
   pool blob) plus a relocation pass. (The postponed separate compilation would use one
   image per object and per COMDAT group, `04-architecture.md` §6.5.)
5. **Function pointers are engine descriptors.** A CBC function's pointer is the address
   of its 16-byte descriptor in an engine-reserved, non-executable region, produced by
   `ld.fnptr` (E5); every indirect call is `call.indirect` (E6), which recognizes a
   descriptor with one range check and performs an ordinary interpreted call. Native
   function pointers are their real addresses (`ld.fnptr` of an AOT reference) and are
   called natively by the same instruction. There is no budget, no index representation
   and no dispatcher. Casts, equality and storage in memory all work.
6. **Native code is reached through AOT direct calls by linkage name.** A call to an
  undefined external symbol becomes `call.direct` on a method reference flagged `AOT`
   with a `DirectCallAotData{linkageName}` entry; the engine `dlsym`s it. Native function
   addresses come from `ld.fnptr` of AOT references, native data addresses from AOT
   static-field references. Native libc implements the C library; there is no CBC
   re-implementation layer. Native code never calls back into CBC code (out of scope), so
   callback-taking libc functions (`qsort`, `bsearch`, `ftw`, `pthread_once`, …) are
   unsupported and rejected at link time. The only libc-named CBC definitions are the
   exit-handler family (`exit`, `atexit`, `__cxa_atexit`; its handlers are called by CBC
   code), signal guards that accept only `SIG_DFL`/`SIG_IGN`, and `longjmp`. Native
   signature mismatches are fixed by compile-time rules (`sinl` → `sin`, inline `ldiv`)
   from one table, `CBCNativeLibc.def` (`10-linker-and-runtime.md` §5); `printf` with
   `double` arguments is a plain native call thanks to E7.
7. **Whole-program compilation only.** The data image, the native/CBC split of symbols and
   the method/field reference pools are global decisions. Everything is compiled to
   bitcode and final code generation runs once at link time (`cbc-ld`). Separate code
   generation with relocatable CBC objects is postponed indefinitely; its design sketch
   stays in `10-linker-and-runtime.md` §4.
8. **Exceptions and** `longjmp` **use CBC exception regions with a marker exception.** A C++
  throw stores the `_Unwind_Exception*` in a runtime slot and raises an engine implicit
   exception (`nullcheck IRZ`); each `invoke` becomes an exception region whose handler
   calls a CBC-compiled selector routine. The engine has already freed the shadow
   allocations of every frame it popped.
   Continuing to unwind raises a fresh marker (no engine object is kept across cleanups).
   An exception the runtime did not raise itself (a `call.indirect` through a null
   pointer) is reported and aborts. `setjmp`/`longjmp` reuse the same mechanism.
9. **Two clobber sets in the backend.** Calls to CBC-defined functions and all
  `call.indirect` calls clobber all volatile registers; direct calls to native functions
  clobber only `IR1` and `FR0` (the engine copies `rax`/`x0` and `xmm0`/`d0` back and
  nothing else). Variadic calls to CBC
   functions pass arguments in a buffer through `ExpandVariadics` with a host-compatible
   `va_list`; variadic native calls use the plain host ABI (the engine sets `al`, E7).
10. **A new** `CBC` **object format in MC.** The `.cbc` container is not ELF-like (one shared
  pool, name-keyed hash indices, ULEB-packed records). It is produced by a dedicated
    `CBCObjectWriter`, modelled on the SPIR-V and DXContainer writers. Pool indices are
    assigned eagerly at AsmPrinter time so instruction sizes are known; only branch
    displacements need MC relaxation.



## Status of the reference documents

`cbc-engine-initial-stage/agents/isa_reference.md` is **out of date** relative to the engine
source: its opcode numbers, its MemSpace section, the legacy `LoadField`/`LoadStatic`/
`LoadTyped` opcodes, and several semantic notes no longer match `src/cbc/isa_opcodes.h`
and `src/cbc/isa_parser.cpp`. Where this directory disagrees with `isa_reference.md`, this
directory was checked against the source and against bytes in `sandbox/default.cbc`.
The full list of discrepancies is in `[02-isa-encoding.md](02-isa-encoding.md)` §9.