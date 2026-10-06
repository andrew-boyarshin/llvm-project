# 23 — Review Findings: "ClangCBC: initial commit"

Review of the last commit in two repositories, covering correctness, reliability and
fragility. This file records what was found, how it was established, how each finding
relates to the design documents `01`–`22`, and what to do about it.

| Repository | Commit | Reviewed |
|---|---|---|
| `llvm-project-upstream` (branch `cbc`) | `414400f0dd01` | everything except `agents/` and `cbc-stdlib-tests/` |
| `cbc-engine-initial-stage` (branch `cbc`) | `7a5c5cd2` | everything except `agents/` and `cbc-stdlib-tests/` |
| `cbc_compiler` (branch `master`, read-only cross-check) | `b318c77` | only the CBC file encoder, as the other producer |

Date of review: 2026-10-06. No source file in any repository was modified by the review.

## 0. How to read this report

### 0.1 Method

1. **Read.** Every changed file was read, including all of `CBCISelLowering.cpp`,
   `CBCISelDAGToDAG.cpp` (976 lines), `CBCEncoding.cpp`, `CBCAsmPrinter.cpp`,
   `CBCLowerGlobals.cpp`, `CBCLowerEH.cpp`, `CBCLowerSjLj.cpp`, `CBCFileWriter.cpp`,
   `cbc-ld.cpp`, `crt-cbc.c`, the Clang target and driver, the shared-code diffs
   (`Triple`, `MC*`, `ExpandVariadics`, compiler-rt, libc++/libc++abi CMake), and on the
   engine side the reader, rewriter, trampolines, shadow stack and function descriptors.
2. **Run.** Suspicions were turned into probes and executed with the built toolchain
   (`llvm-project-upstream/build-install/bin/clang`, `cbc-ld`) and the launcher from
   `hotfix-sdk` (engine `libcbcengine.so`, a **Debug** build with assertions). Section 8
   lists every probe with expected and actual output.
3. **Cross-check against the design documents.** Each finding is classified (below).
   Findings that are decisions recorded in `01`–`22` are not defects in themselves; findings
   where the code contradicts the documents, or where the documents are silent, are.

### 0.2 Classification of each finding

| Tag | Meaning |
|---|---|
| **DEVIATION** | the documents specify behaviour X; the code does Y |
| **UNIMPLEMENTED** | specified, on the roadmap (`11-testing-and-roadmap.md`), not yet in the tree |
| **UNSPECIFIED** | neither required nor excluded by the documents |
| **DOCUMENTED** | a recorded design decision or accepted limitation (listed for completeness) |

### 0.3 Severity

| Level | Meaning |
|---|---|
| **High** | silent wrong results, a crash in a common construct, or data loss |
| **Medium** | a feature fails to compile or behaves wrongly in a narrower case; a correctness hazard that was not reproduced |
| **Low** | fragility, maintainability, performance, hygiene |

### 0.4 Confidence

Each finding states how it was established: **Reproduced** (run and observed),
**Verified by reading** (code and documents agree on the facts, not executed), or
**Hypothesis** (plausible cause, not confirmed).

## 1. Summary

| ID | Sev | Tag | Area | One line |
|---|---|---|---|---|
| F-01 | High | DEVIATION | entry | `main` always receives `argc = 0`, `argv = NULL` |
| F-02 | High | UNIMPLEMENTED | native ABI | second return register of a native call is lost (`ldiv` returns a wrong remainder) |
| F-03 | High | DEVIATION | variadics | indirect call to a CBC variadic function crashes the engine |
| F-04 | High | DEVIATION | exceptions | `std::exception_ptr` / `rethrow_exception` yields garbage |
| F-05 | High | DEVIATION | engine/producer | the Cangjie encoder was not updated for the new code-record fields |
| F-06 | High | UNIMPLEMENTED | atomics | `cmpxchg` and `fence` cannot be selected; the compiler then crashes |
| F-07 | High | DEVIATION | linker | no native-symbol validation; unresolved symbols fail at engine load |
| F-08 | Medium | UNSPECIFIED | static init | `__attribute__((destructor))` is silently dropped |
| F-09 | Medium | UNIMPLEMENTED | `long double` | 8-byte `long double` against an 80-bit host libc gives wrong results |
| F-10 | Medium | UNIMPLEMENTED | 128-bit | `__int128` divide/remainder and 128-bit↔float conversions do not compile |
| F-11 | Medium | UNIMPLEMENTED | TLS | `thread_local` crashes `cbc-ld` |
| F-12 | Medium | DEVIATION | globals | `__attribute__((used))` data is a fatal error |
| F-13 | Medium | UNSPECIFIED | determinism | C++ builds with exception handlers are not byte-reproducible |
| F-14 | Medium | Hypothesis | EH lowering | PHI-of-landing-pad splitting substitutes null/0 for unrecognised inputs |
| F-15 | Medium | UNSPECIFIED | backend | every instruction is flagged `UnmodeledSideEffects`; no machine-level optimisation |
| F-16 | Medium | Verified by reading | `cbc-ld` | dangling `StringRef`s in the archive-extraction loop |
| F-17 | Medium | UNSPECIFIED | `CBCLowerGlobals` | landing-pad clause nulling; 32-bit image size; guessed caps |
| F-18 | Low | DEVIATION | isel | `VASTART`/`VAEND`/`VACOPY` are silent no-ops; the spec says fatal |
| F-19 | Low | DEVIATION | engine | function-descriptor region has a hard 16 MiB cap; pointer identity is not stable |
| F-20 | Low | UNSPECIFIED | engine | FCB size and layout are duplicated by hand in three places |
| F-21 | Low | UNSPECIFIED | engine | shadow stack: boundary thrash, abort on overflow, dead helper, thin tests |
| F-22 | Low | UNSPECIFIED | crt | `__cbc_args` and `__cbc_check_host` are hand-parsed hacks; test-suite shims in production |
| F-23 | Low | UNSPECIFIED | `cbc-ld` | heuristic callback check, fixed O2 pipeline, dead flag |
| F-24 | Low | UNSPECIFIED | encoder | raw opcode bytes duplicated across repos with no shared table |
| F-25 | Low | UNSPECIFIED | tooling | disassembler and asm parser are stubs |
| F-26 | Low | DEVIATION | target | unused `CBC_Native`/`CBC_Entry`; single preserved-register mask |
| F-27 | Low | DEVIATION | EH | `ExceptionHandling::Wasm` borrowed instead of a CBC mode |
| F-28 | Low | UNSPECIFIED | shared code | `ExpandVariadics` edited for every target (invoke support, unindented block) |
| F-29 | Low | UNSPECIFIED | hygiene | fatal errors end in a segfault; hardcoded user paths; build script bug |
| F-30 | Low | UNIMPLEMENTED | runtime | no loop safepoint pass |
| F-31 | Info | DOCUMENTED | various | items found and confirmed to be documented decisions (section 6) |

## 2. High severity

### F-01 `main` always receives `argc = 0`, `argv = NULL`

* **Where:** `llvm/lib/Target/CBC/CBCSynthesizeEntry.cpp` (`Args.push_back(Constant::getNullValue(...))`),
  `llvm/tools/cbc-ld/crt/crt-cbc.c` (`__cbc_args`, never called).
* **Tag / confidence:** DEVIATION / Reproduced.
* **Spec:** `07-ir-passes.md` §2 (entry synthesis, `%argc = call i32 @__cbc_args(...)`) and
  `10-linker-and-runtime.md` §6.3 (`argv[0]` is the `.cbc` path, `argv[1..]` the rest).
* **Evidence:** `int main(int argc, char **argv)` printing both prints `argc=0 argv=(nil)`
  when launched with arguments.
* **Impact:** every program that reads its arguments or `argv[0]` misbehaves or crashes.
* **Fix:** have the synthesized entry call `__cbc_args` and pass the result to `main`. Note
  `__cbc_args` itself does not match the spec (F-22): it drops the `.cbc` path, so
  `argv[0]` would be the first user argument.

### F-02 The second return register of a native call is lost

* **Where:** `CBCCallingConv.td` (`RetCC_CBC`), `CBCISelLowering.cpp` (`LowerCall` return
  handling), `CBCEncoding.cpp` (`RET` = `Ret64 IR1`, `FRET` = `FRet64 FR0`), engine `i2c` adapter.
* **Tag / confidence:** UNIMPLEMENTED (workaround not present) / Reproduced.
* **Spec:** `13-engine-changes.md` §0 lists "second result registers in `i2c`" as **not
  accepted**; the workaround is the `div` family expansion by `CBCNativeCallLegalizer`
  (`10` §5, `11` M3). `21-native-aggregate-abi.md` explicitly asks to confirm that `rdx`
  reaches `IR2` after a native return.
* **Evidence:** `ldiv(17, 5)` through native libc returns `quot=3 rem=5` (expected `2`). A
  CBC-compiled function returning `struct { long a, b; }` and one returning
  `struct { long a; double d; }` work correctly.
* **Answer to the open question in `21`:** `rdx` is **not** preserved into `IR2`.
* **Impact:** `ldiv`, `lldiv`, `imaxdiv`, and any native function returning a two-register
  aggregate (probably also `_Complex double` results in `xmm0:xmm1`, not tested) return
  wrong values with no diagnostic.
* **Fix:** implement the legalizer rewrite (call a CBC-compiled equivalent) or reject such
  calls at link time; until then, document the list of affected functions.

### F-03 Indirect call to a CBC variadic function crashes the engine

* **Where:** `llvm/lib/Transforms/IPO/ExpandVariadics.cpp` (`CBC::ignoreFunction`).
* **Tag / confidence:** DEVIATION / Reproduced (root cause confirmed with `opt`).
* **Spec:** `05-llvm-core-changes.md` §7: `ignoreFunction(F)` must be **false for
  `nullptr`**, because indirect calls "must be expanded". The code returns
  `!F || F->isDeclaration()`, i.e. **true** for `nullptr`, and its comment argues the
  opposite.
* **Evidence:** with `int (*volatile fp)(int, ...) = sum;`, `sum(3,1,2,3)` works and
  `fp(3,1,2,3)` raises SIGSEGV at address `0x11` inside `libcbcengine.so`.
  `opt -passes=expand-variadics -expand-variadics-override=lowering` on the module shows only
  `define i32 @sum(i32, ptr %varargs)`: the definition is rewritten in place, no variadic
  wrapper remains, and the indirect caller still passes the arguments the old way.
* **Impact:** any program that stores or passes a CBC variadic function through a pointer
  (callbacks, vtable-like tables, `printf`-style loggers).
* **Fix:** follow the spec (expand indirect calls) and keep native variadic function pointers
  behind the CBC wrapper described in `04` §7.3 (address of a native variadic function).
  See also F-18 (a no-op `VASTART` would have turned this into a compile error).

### F-04 `std::exception_ptr` / `std::rethrow_exception` yield garbage

* **Where:** `libcxxabi/src/cbc_eh_select.cpp` (`header_from_unwind`, `thrown_object`,
  `__cbc_can_catch`).
* **Tag / confidence:** DEVIATION / Reproduced; cause is a **Hypothesis**.
* **Spec:** `08-exceptions-and-sjlj.md` §4.5 ("`std::current_exception`,
  `std::rethrow_exception`, `std::exception_ptr`, nested exceptions … work unchanged").
* **Evidence:** after `ep = std::current_exception(); std::rethrow_exception(ep);` a
  `catch (const D &d)` reads `d.code = -699481232` instead of `42` (native: `42`). All other
  nested try/catch/rethrow/cleanup cases in the same program matched native output at both
  `-O0` and `-O2`.
* **Probable cause:** the code assumes the thrown object is at `ue + 1` and reads
  `exceptionType` from the `__cxa_exception` layout. Exceptions re-thrown through
  `rethrow_exception` are `__cxa_dependent_exception`s whose layout and object location
  differ (`14-libcxx.md` mentions dependent exceptions).
* **Fix:** resolve dependent exceptions via `__cxa_get_exception_ptr`/the primary exception
  before reading type and object; add a regression test.

### F-05 The Cangjie encoder was not updated for the new code-record fields

* **Where:** engine `src/engine/decode/reader.cpp` (reads `untypedMemSize` ULEB and
  `usesAlloca` U8 between `mayHaveNativeCalls` and `codeSize`);
  `cbc_compiler/core/assembler/src/com/huawei/excelsior/jet/assembler/cbc/CbcFileEncoder.scala:770-771`
  (writes `mayHaveNativeCalls`, then continues with the old layout).
* **Tag / confidence:** DEVIATION (the producer half of an accepted change is missing) /
  Verified by reading. **Not run**: no pre-change engine was built.
* **Spec:** `13-engine-changes.md` §1: "E8 inserts `untypedMemSize` and `usesAlloca` into
  the method-code header; the Cangjie encoder writes `0, 0` there **in the same patch**.
  There is no version byte, so an old encoder and an E8 reader are not a supported pair —
  CI pins them together." The format change itself is therefore an accepted decision.
* **State found:** `cbc_compiler` HEAD is `b318c77` (2026-10-01), five days **before** the
  engine commit (2026-10-06). Nothing in the repository mentions the new fields. The file
  version byte is still `1` on both sides, and `agents/cbc_structures.md` in the engine
  repository still documents the old record. (The engine's own version check is dead code,
  see section 6.)
* **Impact:** a patched engine misparses every `.cbc` file produced by the Scala toolchain.
  Because no version byte changed, the reader does not detect it; the result is garbage or
  an engine `FATAL`.
* **Fix:** land the encoder change; consider bumping the file version anyway and wiring up the
  reader's version gate, which is dead code today (section 6).

### F-06 Atomics and fences cannot be selected; the compiler then crashes

* **Where:** `CBCISelLowering.cpp` (no `ATOMIC_*` actions), `CBCISelDAGToDAG.cpp` (only
  `ATOMIC_LOAD`/`ATOMIC_STORE` handled), `clang/lib/Basic/Targets/CBC.cpp`
  (`MaxAtomicInlineWidth = 64`).
* **Tag / confidence:** UNIMPLEMENTED / Reproduced.
* **Spec:** `04-architecture.md` §11 (atomics lowering: `cmpxchg`, `atomicrmw`, `fence`),
  `06-backend.md` §4.2 (`ATOMIC_FENCE` custom).
* **Evidence:** `atomic_compare_exchange_strong` fails with
  `CBC cannot select: AtomicCmpSwap`, `__sync_synchronize` with `CBC cannot select: AtomicFence`;
  both are followed by `Segmentation fault (core dumped)` from `cbc-ld`.
* **Additional bug:** `ATOMIC_LOAD`/`ATOMIC_STORE` map every memory type other than `i8` and
  `i32` to the 64-bit access, so an `_Atomic short` load would read 8 bytes. It was not
  reachable in the probe because the compile failed earlier; it is visible in the code
  (`CBCISelDAGToDAG.cpp`, `ISD::ATOMIC_LOAD` / `ISD::ATOMIC_STORE`).
* **Impact:** any C11 `<stdatomic.h>`, `__sync_*`, `__atomic_*`, or C++ `std::atomic` use in
  user code fails to build, while Clang advertises 64-bit inline atomics.
* **Fix:** implement the specified lowering, or until then set `MaxAtomicInlineWidth = 0` so
  Clang emits libcalls and fails with a clear message.

### F-07 No native-symbol validation at link time

* **Where:** `llvm/tools/cbc-ld/cbc-ld.cpp` (`NoValidate` defaults to `true`, is
  `(void)`-discarded; the driver also always passes `--no-native-validation`).
* **Tag / confidence:** DEVIATION / Reproduced (the failure mode).
* **Spec:** `10-linker-and-runtime.md` §2.2 step 4 and §3: every native symbol is resolved
  against the native libraries at link time ("replaces a run-time `FATAL` in the engine's
  rewriter"); an unknown symbol is a link error.
* **Evidence:** a C++ program with exception handling built at `-O2` linked successfully, and
  the launcher then failed with
  `[resolution] [FATAL] not found function: _ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev`
  followed by the engine assertion `Rewriter failed: cannot rewrite code` and SIGILL.
* **Related:** an unrecognised `-lfoo` silently becomes a runtime `dlopen` dependency
  (`NativeLibs`), so a misspelled library is also a run-time failure.
* **Open question (Hypothesis):** why `D2Ev` stayed undefined. Archive extraction and the
  `_Z` check run **before** the O2 pipeline, so a reference introduced by optimisation would
  never be resolved or rejected (F-23).
* **Fix:** implement step 4; make an undefined non-native symbol a link error; run the
  validation after the pipeline.

## 3. Medium severity

### F-08 `__attribute__((destructor))` is silently dropped

* **Where:** `CBCSynthesizeEntry.cpp` (erases `llvm.global_dtors` without running it).
* **Tag / confidence:** UNSPECIFIED (the documents contradict each other) / Reproduced.
* **Spec:** `07-ir-passes.md` §2 and `10-linker-and-runtime.md` §6.1 say `__cbc_exit_handlers`
  runs the `atexit` list, "then `llvm.global_dtors` entries"; `19-static-initialization.md`
  says "Do not also walk `llvm.global_dtors`". The `19` rule is correct for C++ static
  destructors (registered through `__cxa_atexit`) but not for `__attribute__((destructor))`,
  which clang emits only into `llvm.global_dtors`.
* **Evidence:** a program with a destructor-attributed function prints only `main`'s output.
* **Fix:** reconcile the documents; register `llvm.global_dtors` entries that are not already
  covered by `__cxa_atexit` (e.g. wrap each in a `__cxa_atexit` call from the entry).

### F-09 `long double` is 8 bytes; the host ABI is 80-bit

* **Where:** `clang/lib/Basic/Targets/CBC.cpp` (`LongDoubleWidth = 64`),
  `llvm/tools/cbc-ld/crt/include/bits/floatn.h` (claims `__HAVE_FLOAT64X_LONG_DOUBLE 1`).
* **Tag / confidence:** UNIMPLEMENTED (mitigations) / Reproduced.
* **Spec:** `09-clang.md` §2, `10` §5.4, `21` §3, risks R7/R14 in `11`: `long double` calls are renamed to
  the `double` functions, `%L` formats rejected, unknown `*l` natives rejected. None of that
  exists yet (`CBCNativeCallLegalizer` is absent).
* **Evidence:** `sizeof(long double) == 8`; `printf("%Lf", 1.5L)` prints `0.000000`;
  `strtold("2.25", 0)` returns `nan`.
* **Fix:** implement the rename/reject legalizer; correct the stub header, which contradicts
  the 8-byte `long double`.

### F-10 `__int128` divide/remainder and 128-bit ↔ float conversions do not compile

* **Where:** `llvm/lib/Target/CBC/CBCSubtarget.cpp` (`initLibcallLoweringInfo`).
* **Tag / confidence:** UNIMPLEMENTED / Reproduced.
* **Spec:** `05-llvm-core-changes.md` §8 lists `__divti3`, `__udivti3`, `__modti3`,
  `__umodti3` and the shifts as CBC-compiled builtins.
* **Evidence:** `/`, `%` on `__int128` and `unsigned __int128`, `(unsigned __int128)double`
  and `(double)unsigned __int128` all stop with `LLVM ERROR: unsupported library call operation`.
  128-bit multiplication and shifts compile and run correctly.
* **Cause:** only a hand-picked list of libm and two `__fix*ti` entries is registered; the
  other `RTLIB` entries have no implementation for the CBC triple.
* **Fix:** register the missing libcalls (the builtins archive already carries the
  implementations, and `udivmodti4.c` already has a `__CBC__` guard).

### F-11 `thread_local` crashes `cbc-ld`

* **Where:** the pipeline order in `CBCTargetMachine.cpp` and `crt-cbc.c`.
* **Tag / confidence:** UNIMPLEMENTED / Reproduced (crash); cause is a **Hypothesis**.
* **Spec:** `10` §5.3 and `04` §6.4: `__emutls_get_address` lives in `crt-cbc`; `emutls.c` is
  excluded from compiler-rt (`14` §10), so nothing else defines it.
* **Evidence:** `__thread int t = 5; t++;` fails with
  `Assertion isa<To>(Val)` in `llvm::cast<GlobalValue>` inside `cbc-ld`, then a segfault.
  `crt-cbc.c` defines no `__emutls_get_address`.
* **Probable cause:** `-femulated-tls` is forced by the driver; `LowerEmuTLS` is run inside
  `TargetPassConfig::addIRPasses()`, **after** the CBC passes, so it creates new globals after
  `CBCLowerGlobals` has already run.
* **Fix:** implement the emutls runtime; run emutls lowering before the CBC passes.

### F-12 `__attribute__((used))` data is a fatal error

* **Where:** `CBCLowerGlobals.cpp` (`report_fatal_error("CBC global is used outside a function")`).
* **Tag / confidence:** DEVIATION / Reproduced.
* **Spec:** `07-ir-passes.md` §3.2: intrinsic globals (`llvm.used`, `llvm.compiler.used`, …)
  are excluded and consumed.
* **Evidence:** `__attribute__((used)) static int keep = 3;` fails to link with the fatal
  error above, then a segfault.
* **Cause:** the placed global is still referenced by the `llvm.used` array, whose user is a
  `GlobalVariable`, not an instruction.
* **Fix:** erase `llvm.used` and `llvm.compiler.used` (or drop their uses) before rewriting
  users.

### F-13 C++ builds with exception handlers are not byte-reproducible

* **Where:** `CBCAsmPrinter.cpp`, region emission (`for (const auto &LC : LastCall)` iterates
  a pointer-keyed `DenseMap`).
* **Tag / confidence:** UNSPECIFIED / Reproduced (non-determinism); cause is a **Hypothesis**.
* **Evidence:** the same C++ source with several `try` blocks, compiled six times, gives six
  different SHA-1 hashes. A C program compiled four times gives one hash. Execution results
  are correct each time.
* **Impact:** breaks reproducible builds, build caches, and any diff-based test of output.
* **Fix:** emit regions in a stable order (iterate `Calls`, or key by MBB number), then check
  that no other pointer-ordered container leaks into output.

### F-14 PHI-of-landing-pad splitting substitutes null/0 for unrecognised inputs

* **Where:** `CBCLowerEH.cpp` (`PadPhis` loop: `ExnIn`/`SelIn` default to `null`/`0`).
* **Tag / confidence:** Hypothesis (no failing program found).
* **Reasoning:** this pass runs **after** the O2 pipeline of `cbc-ld`, when landing-pad
  aggregates commonly flow through PHIs after inlining and merging. Only a PHI whose incoming
  value is directly a `landingpad` is translated; an incoming PHI (phi-of-phi), a `select`, or
  an `insertvalue` chain is replaced by `null`/`0` silently, which would change which handler
  runs. A stress program with nested cleanups, rethrow, `catch(...)`, and a throwing destructor
  matched native output at `-O0` and `-O2`, so the case may be rare or unreachable.
* **Fix:** handle phi-of-phi and other producers, or fail loudly instead of defaulting.

### F-15 Every instruction is flagged `UnmodeledSideEffects`

* **Where:** `CBCInstrInfo.td` (no instruction sets `mayLoad`, `mayStore`, or
  `hasSideEffects`).
* **Tag / confidence:** UNSPECIFIED / Verified in the generated `CBCGenInstrInfo.inc`.
* **Evidence:** `ADD64rr`, `MOV64ri`, `LEA_FRAME`, `LDRAW64`, `STRAW64`, `STU64` all carry
  `UnmodeledSideEffects | ExtraSrcRegAllocReq | ExtraDefRegAllocReq`, and none carries
  `MayLoad`/`MayStore`. Because instructions have no patterns, TableGen assumes the most
  conservative flags.
* **Impact:** machine CSE, sinking, LICM and dead-instruction elimination cannot touch any
  instruction; scheduling is fully constrained. This is a code-size and speed cost, not a
  correctness bug. The size effect was not measured (relevant given `rom_size_analysis.md`).
* **Fix:** set the real flags per instruction (`mayLoad`/`mayStore` on memory ops,
  `hasSideEffects = 0` on pure arithmetic) and measure.

### F-16 `cbc-ld`: dangling `StringRef`s in the archive-extraction loop

* **Where:** `cbc-ld.cpp`, `DenseSet<StringRef> Undef` is built from the composite module's
  value names and queried while `linkInModule` runs in the same round.
* **Tag / confidence:** Verified by reading (not triggered).
* **Reasoning:** when a definition is linked in, the previous declaration is replaced and
  erased, freeing its name storage; later archives in the same round compare against stale
  `StringRef`s (`Undef.contains(Name)` can read freed memory).
* **Fix:** store `std::string`s, or rebuild `Undef` after each successful link.

### F-17 `CBCLowerGlobals`: hacks and unchecked limits

* **Where:** `CBCLowerGlobals.cpp`.
* **Tag / confidence:** UNSPECIFIED / Verified by reading.
* **Items:**
  1. Landing-pad clause operands that reference placed globals are overwritten with `null`
     to avoid an infinite loop in `SelectionDAGBuilder::findUnwindDestinations` (a comment says
     so). A `catch ptr null` clause means *catch-all*; any later pass that inspects or merges
     landing pads would now see unrelated pads as identical. Correctness depends on
     `CBCLowerEH` having already recorded the real type infos in `cbc.lpad.clauses`.
  2. `ConstantInt::get(Int32Ty, ImageSize)` truncates an image larger than 4 GiB silently.
     Relocation offsets are checked, the image size is not.
  3. `FnPtrCap = 1000` is a guess derived from a 4095-entry literal table and is not tied to
     any computed budget.
  4. Empty inline asm is "handled" by parsing constraint strings (`=r,0`) and falling back to
     argument 0; an output with no tie silently receives an input.
  5. The data blob is stored as an `MDString` in module metadata, copying the whole initialised
     data section through IR metadata.
  6. Over-aligned globals are accepted without checking that the image base satisfies them.
     The probe with 64- and 4096-byte alignment passed, so this is a robustness note only.

## 4. Low severity

### F-18 `VASTART`, `VAEND`, `VACOPY` are silent no-ops

`CBCISelDAGToDAG.cpp` selects them as nodes with no effect. `06-backend.md` §4.2 says leftovers
after `ExpandVariadics` must be a **fatal error**. Making them fatal would have turned F-03 from
an engine crash into a compile error. *DEVIATION / Verified by reading.*

### F-19 Function descriptors: hard cap and unstable identity

* `function_descriptors.cpp` reserves a 16 MiB region (`kRegionBytes`), so at 16 bytes each the
  limit is 1,048,576 descriptors and then `FATAL`. `04-architecture.md` §7.1 states the number of
  address-taken functions is "limited only by" the committed pages. *DEVIATION.*
* Descriptors are never freed and are keyed by `DynamicFunctionHandle*`. If a handle is ever
  released or reused (the project targets hot-patching), a descriptor would point at stale or
  reused memory. Lifetime was not verified.
* `LoadFuncPtr` captures either `compiled->funcPtr` or the descriptor **at rewrite time**, so
  the address of the same function can differ between methods rewritten before and after it
  is compiled, which breaks pointer equality.
* `CALL_REG` sends any value that is not a descriptor to the generic native-call adapter
  without validation.

### F-20 Fiber control block: layout and size duplicated by hand

The engine allocates `calloc(1, 64)` and a magic constant in `shadow_stack.cpp`; the layout is
repeated in `crt-cbc.c` and `libcxxabi/src/cbc_exception_storage.cpp` ("Must match…"). Today
`8 + 32 + 8 + 16 = 64`, so the three agree exactly; one added field would overrun the heap
block silently. `08-exceptions-and-sjlj.md` specifies the layout but not a shared size
constant. *UNSPECIFIED / Verified by reading.* Fix: a shared header and `static_assert`s.

### F-21 Shadow stack and frame layout

* A loop that crosses a segment boundary maps and unmaps a segment per call.
* Overflow is a process `FATAL`, not a guest-visible stack-overflow exception; segments are
  `mprotect`ed read-write in full (no `MAP_NORESERVE`), and `kMaxRequest = 1 GiB` is a magic
  constant.
* `LocalsBeforeSavedRegs` (`frame.h`) is used only by its test and duplicates the logic in
  `makeFrameLayout`; the test therefore does not cover the real frame layout.
* `ExecBytecodeInfo::shadowPad` is unused.
* New `FATAL`s for malformed input (`untypedMemSize % 16`, `usesAlloca > 1`, `lea.frame`
  displacement) abort the process instead of using the rewriter's `Fail()` path.
* The commit adds one unit test file (`shadow_stack_test.cpp`); there are no tests for
  `LoadFuncPtr`, `CallIndirect`, the reader fields, or the rewriter paths.
* `build_install.sh` hardcodes `/home/user/dev/...` and `/home/user/Downloads/hotfix-sdk`, a
  Debug build and `-j 24`, and copies into the SDK tree.

Trampolines (`x86_64_linux`, `aarch64_linux`) were checked separately and are **correct**
(section 5).

### F-22 `crt-cbc.c`

* `__cbc_args` reads `/proc/self/cmdline` into a 4095-byte stack buffer (silent truncation),
  uses the first argument ending in `.cbc` as the marker, and `argv[0]` becomes the first
  argument **after** it, which contradicts `10` §6.3 (`argv[0]` = the `.cbc` path). `malloc`
  results are unchecked.
* `__cbc_check_host` hardcodes `utsname` offset `65 * 4` and accepts only `x86_64`; it exits
  with status 127 and no message.
* `__builtin_exit` and `__builtin_puts` exist only to satisfy GCC torture tests built with
  `-Wno-implicit-function-declaration`; they pollute the production runtime.
* Handlers registered *while* the exit handlers run are never executed (`cbc_exit_run` fixes
  its loop bound at entry).

### F-23 `cbc-ld` heuristics

* The "CBC function passed to a native function" check inspects only direct call arguments
  after O2; a function pointer stored in a struct or global is not seen.
* The pipeline is always `O2`, regardless of the user's `-O` level; extraction stops after 64
  rounds with an error.
* `NoValidate` is dead (F-07). Unsupported-function and `_Z` rejection run on the pre-optimised
  module, so references introduced by optimisation escape them.
* The triple is forced to `cbc_x86_64-unknown-linux-gnu` regardless of the inputs.

### F-24 Opcode bytes are duplicated by hand across repositories

`CBCEncoding.cpp`, `CBCAsmPrinter.cpp` and `CBCFileWriter.cpp` emit raw bytes (`0x45`, `0x44`,
`0x47`, `0x4C`, `0x60`, `0x7C`, nibble selectors `0x9`–`0xE`, `0x80`, …) that must equal the
engine's `ISA_OPCODES` and `RegGroup` order. All values were checked and agree **today**. There
is no shared table, `appendSLEB` exists three times, and `RegGroup` now uses 15 of its 16
nibble values. `CBCFileWriter` also hardcodes the header size (57), term-id base (20), a
single-bucket hash table (`nameHash` is dead code), empty GC liveness maps, and overwrites a
method's `TypedSlots` when adding the image prologue. *UNSPECIFIED.*

### F-25 Disassembler and asm parser are stubs

`decodeCBCInst` handles three opcodes (`MOV64rr`, `MOV64ri`, `RET`); the asm parser accepts
`mov.W64` and `ret.W64` only. `llvm-mc`, `llvm-objdump` and text round-trips do not work for
CBC. *UNSPECIFIED.*

### F-26 Calling-convention scaffolding

`CBC_Native = 128` and `CBC_Entry = 129` are added to the upstream `CallingConv.h`, the parser
and the printer, but nothing in the backend uses them. `06-backend.md` specifies a
`CSR_CBC_Native` mask (preserving everything except `IR1`/`FR0`) for direct native calls; the
code uses one preserved-register mask for all calls, which costs register allocation quality.
The ID range also risks colliding with a future upstream assignment. *DEVIATION.*

### F-27 `ExceptionHandling::Wasm` borrowed for CBC

`Triple::getDefaultExceptionHandling` returns `Wasm` for CBC. `05-llvm-core-changes.md` §5
proposes a dedicated `ExceptionHandling::CBC`. Side effects of running the Wasm EH path were
not examined. `getExceptionPointerRegister`/`getExceptionSelectorRegister` remain in
`CBCISelLowering.cpp` as unused leftovers, and the selector register (`IR12`) is callee-saved.
*DEVIATION.*

### F-28 `ExpandVariadics` is edited for every target

* Invoke expansion is switched on for all targets (previously `isa<InvokeInst>` returned
  false); `05-llvm-core-changes.md` §7 does not mention it.
* The new `if (ABI->hasRegisterSaveArea()) { … } else {` block leaves the existing code
  unindented.
* Register-save size `176` and the x86-64 slot rules are hardcoded.

### F-29 Hygiene

* Every `LLVM ERROR` from `cbc-ld` (cannot select, unsupported libcall, global used outside a
  function) is followed by `Segmentation fault (core dumped)` and the driver reports exit
  code `-2`. The cause was not investigated.
* `cbc-stdlib-tests/run.sh` falls back to `/home/andrew/dev/hotfix-sdk/...`.
* `build.sh` ignores a `PREFIX` override for `cmake --install` and checks the wrong tree
  (this was also raised by the automated review).
* The untracked `cbc-stdlib-tests/gcc-c-torture.log` lists 28 compile `FAIL`s; the eight
  distinct tests sampled (`packed-1`, `mul-sext`, `multdi-1`, …) compile successfully now, so
  the log is stale.
* Plain `cbc` (no sub-architecture) hits the "AArch64 flavour not implemented" error; the
  AArch64 flavour is plumbed through `Triple`, data layout, driver and crt but rejected in
  `CBCTargetMachine`.

### F-30 No loop safepoints

`07-ir-passes.md` §8 specifies a loop-safepoint pass (`CBCInsertSafepoints`, `-cbc-loop-safepoints`); `LLVMInitializeCBCTarget`
registers no such pass. Effects on GC/preemption of long-running loops were not tested.
*UNIMPLEMENTED.*

## 5. Verified correct

These were examined, in most cases with a probe, and found sound:

* **x86-64 and aarch64 trampolines:** all three frame-exit sites (entry, return, unwind) save
  and restore the shadow-stack token; call-clobbered registers are reloaded before use;
  stack alignment holds (the aarch64 file was reviewed less closely).
* **Struct `byval` arguments:** a callee that modifies its by-value struct does not change the
  caller's copy.
* **CBC-to-CBC aggregate returns** (`{long,long}`, `{long,double}`).
* **Source-level inline asm** with constraints (`=r`, `0`) and empty-asm barriers.
* **Packed and 16-bit loads/stores** (emulated with two byte accesses): results correct.
* **Over-aligned globals** (64 and 4096) and stack objects (`FrameIndex` re-alignment plus
  frame-object padding).
* **C++ exceptions:** nested `try`/`catch`, rethrow, `catch(...)`, cleanups on unwind, a
  throwing destructor, `std::uncaught_exceptions`, class hierarchies, in loops with
  `std::vector` locals; identical to native at `-O0` and `-O2` (exception: F-04).
* **setjmp/longjmp lowering:** sound within its documented limits (fatal if the function also
  has a C++ handler, taking the address of `setjmp` rejected). Every call in such a function
  becomes an `invoke`, which costs code size.
* **Numerics:** a differential test against native x86-64 compared about 65,000 results across
  `i8/u8/i16/u16/i32/u32/i64/u64` (add, sub, mul, and/or/xor, all six compares, min, not, neg,
  div, rem, shifts, `__builtin_{add,sub,mul}_overflow`), clz/ctz/popcount/bswap/rotate/parity,
  `__int128` add/mul/shift/compare, and `double`/`float` arithmetic, compares including NaN and
  infinity, and conversions to and from 32/64-bit integers. All sections matched except one
  result that came from undefined behaviour in the probe itself (`-INT32_MIN`).
* **Encoder consistency:** every `RegGroup` selector in `CBCEncoding.cpp` (`0x1`, `0x3`, `0x8`,
  `0x9`–`0xE`) matches the engine's enum order today; branch relaxation and displacement
  patching are correct for the 16-bit, split-nibble and wide forms.
* **Floating-point condition codes** (`feq`, `fne`, `flt`, `fnlt`, `fge`, `fnge`) are mapped
  correctly for the ordered and unordered predicates, including the two-compare `ONE`/`UEQ`
  cases.

## 6. Documented decisions (not defects)

An earlier pass of this review flagged these; they are recorded decisions and are listed so
they are not re-reported:

* The code-record format change, `STORE_LONG_FRAME` (E1), `hasTailReg` for stack-passed floats
  (E2), 32-bit reference counts (E3), and `FATAL` on unknown opcodes or stray prefixes (E4)
  are accepted changes in `13-engine-changes.md`. The only outstanding part is F-05.
* "No version byte … CI pins them together" (`13` §1); the engine's `VersionMetadata` check
  being unused (and comparing `bytecodeVersion` against the *file* version range) is
  pre-existing. Version checks are listed under "not accepted" in `13` §0.
* `__builtin_return_address` returns `0` (`06-backend.md` §4.2, `RETURNADDR`), and
  `__builtin_frame_address` is limited (`FRAMEADDR`). Note: `FRAMEADDR` is a constant `0` in
  the code, while `06` describes `LEA_FRAME` of 0 when the block exists.
* Division by zero is checked only in sanitizer mode (`DivCheck`, `04` §16); otherwise it is
  undefined behaviour. On the Debug engine it raises an assertion and SIGILL.
* `emutls.c` is excluded from compiler-rt on purpose (`14`), `-femulated-tls` is forced, and
  threads, native callbacks and `pthread_create` are out of scope.
* `__cbc_eh_check` aborting for engine-originated exceptions seen in a pad is the documented
  reporting model (`08`).

## 7. Recommended order of work

1. **F-05** land the Scala encoder change (and decide on a version bump), otherwise the engine
   and the existing producer are incompatible.
2. **F-01, F-03, F-18, F-07** entry arguments, indirect variadic expansion (and make leftover
   `VASTART` fatal), and native validation. These turn silent or late failures into
   immediate, understandable ones.
3. **F-02, F-09** implement the native call legalizer (`div` family, `long double` renames and
   `%L` rejection), or at least reject the affected calls at link time.
4. **F-04, F-08, F-12, F-11** exception pointers, `llvm.global_dtors`, `llvm.used`, emutls.
5. **F-06, F-10** atomics and the missing 128-bit libcalls.
6. **F-13, F-16, F-20** determinism, dangling references, shared FCB definition.
7. **F-15** correct instruction flags, then measure code size.
8. Everything else as maintenance; add the tests listed in F-21.

## 8. Reproductions

Common setup:

```bash
source /home/user/Downloads/hotfix-sdk/cangjie/envsetup.sh        # provides `launcher`
CLANG=/home/user/dev/llvm-project-upstream/build-install/bin/clang
T=--target=cbc_x86_64-unknown-linux-gnu
$CLANG $T -O1 -w prog.c -o prog.cbc && launcher prog.cbc [args]    # use clang++ for C++
```

| Finding | Source | Expected | Observed |
|---|---|---|---|
| F-01 | `int main(int c,char**v){printf("argc=%d argv=%p\n",c,(void*)v);}` run with `a1 a2` | `argc=3` and a non-null pointer | `argc=0 argv=(nil)` |
| F-02 | `volatile long n=17,d=5; ldiv_t q=ldiv(n,d); printf("%ld %ld",q.quot,q.rem);` | `3 2` | `3 5` |
| F-03 | `static int sum(int n,...){…}` with `int (*volatile fp)(int,...)=sum; fp(3,1,2,3)` | `6` | engine SIGSEGV at `0x11`; the direct call `sum(3,1,2,3)` prints `6` |
| F-03 (cause) | `opt -mtriple=cbc_x86_64-unknown-linux-gnu -passes=expand-variadics -expand-variadics-override=lowering -S in.ll` | a variadic wrapper or expanded call sites | only `define i32 @sum(i32, ptr %varargs)` |
| F-04 | `ep = std::current_exception(); std::rethrow_exception(ep);` caught as `const D&` with `code = 42` | `42` | `-699481232` (at `-O0`); `-O2` run was blocked by F-07 |
| F-06 | `atomic_compare_exchange_strong(&ai,&e,100);` and `__sync_synchronize();` | builds | `CBC cannot select: AtomicCmpSwap` / `AtomicFence`, then segfault |
| F-07 | C++ program with exceptions and `std::string`, `-O2` | link error or a working program | links; launcher: `not found function: …basic_string…D2Ev`, then SIGILL |
| F-08 | `__attribute__((destructor)) static void bye(void){puts("destructor ran");}` | prints it after `main` | not printed |
| F-09 | `printf("%zu %Lf",sizeof(long double),1.5L); strtold("2.25",0)` | `16 1.500000`, `2.25` | `8 0.000000`, `nan` |
| F-10 | `__int128 x/y`; `(unsigned __int128)dbl`; `(double)u128` | builds | `LLVM ERROR: unsupported library call operation` |
| F-11 | `__thread int t = 5; t++;` | builds | assertion in `cast<GlobalValue>` inside `cbc-ld`, segfault |
| F-12 | `__attribute__((used)) static int keep = 3;` | builds | `CBC global is used outside a function` |
| F-13 | C++ file with several `try` blocks; build six times; `sha1sum` | one hash | six distinct hashes (the C program gave one hash in four builds) |
| section 6 | `__builtin_return_address(0)`, `__builtin_frame_address(0)` | non-null | both `(nil)` (documented in `06` §4.2) |

Differential numeric test (section 5): one C file with `noinline` functions reading `volatile`
operands from a 34-value edge-case table (values around every width boundary, ±1, ±2, powers of
two, sign bits, 32/64-bit extremes) and a 22-value `double` table (±0, ±1, halves, 1e±300,
infinities, NaN), folding every result into an FNV-style hash per section; build natively with
the host clang and with `$CLANG $T` at `-O0` and `-O2`; compare per-section hashes. When a
section differs, rerun that section printing every result and `diff` to find the operation.

## 9. Limits of this review

* Nothing was run on aarch64; the AArch64 flavour is not implemented.
* The engine was exercised only as the Debug build shipped in the SDK. Several outcomes
  (assertion plus SIGILL on a division by zero or a rewriter failure) are Debug behaviour.
* F-05 was established by reading both sides; no old-format `.cbc` was fed to the new engine.
* F-04, F-11, F-13 and F-14 have unconfirmed causes, stated as hypotheses above.
* Performance and code-size effects (F-15, the unfolded addressing modes, the widened
  comparisons, `i16` access splitting) were not measured.
* The loop-safepoint pass, native-callback checks and threads were out of scope or not
  implemented and were not tested.
* `cbc-stdlib-tests/` was excluded from review; its harness was read only to understand how
  programs are built and run, and its suite was not re-run.
