# 25 — Native ELF/Mach-O libraries with an embedded `.cbc`

Depends on `24-lld-integration.md` (CBC emit lives in `lld/CBC`, selected by
`--cbc`). Authoritative over `24` §4 / §6.1 / §10.3 where they say CBC mode
never runs ELF `Writer` and always writes a `.cbc`. Authoritative over
`07-ir-passes.md` §2 for library entries (no `main`). Authoritative over
`04-architecture.md` §7.3 / `10-linker-and-runtime.md` §5.5–§5.6 /
`13-engine-changes.md` §8 for **exported and address-taken** functions only:
those become native-callable `@C`-style stubs. Internal CBC function pointers
stay E5 descriptors.

`26-cbc-as-native-flavor.md` extends this: host triple + `-fcbc`, then
`__attribute__((cbc_native))`. This work is the wrap product only. It does
**not** split the linked module into CBC vs native functions, and it does
not implement `cbc_native`. Clang invoke is `-fcbc` on the host triple
(same as `24` §11 / `26`); do not write `--target=cbc_*`.

Cangjie reference (the lowering to copy, not a dependency of the LLVM tree):

* IR contract: `"cjstub"` caller, `"c2cj"` callee, `*.CJStubGV`,
  `!CallFrameSizeForCJFFI`
* Asm-printer: `AsmPrinter::tryGetCangjieStubCallNativeFunc`,
  `X86AsmPrinter::emitCangjieCallStubInstImpl`,
  `AArch64AsmPrinter::emitCangjieCallStubInstImpl`
* Runtime: `CJ_MCC_N2CStub` / `InitCJLibraryStub` in
  `cangjie_runtime/.../N2CStub.S`
* Example: `/home/user/dev/dbg/c2cj` (`my_exported_func` → `jmp CJ_MCC_N2CStub`)

This document does **not** port that asm-printer into upstream X86/AArch64.
Stubs are ordinary host LLVM functions whose bodies are inline assembly
(§7).

## 0. Goal

After `24`, `ld.lld --cbc -o a.cbc` produces a CBC container the launcher
loads. This work makes the **same** `--cbc` invocation produce a host
artifact when the output name is not `.cbc`:

```
ld.lld --cbc -o a.cbc          →  CBC container          (today, 24)
ld.lld --cbc -shared -o libfoo.so
ld.lld --cbc -dylib  -o libfoo.dylib
ld.lld --cbc --lto-emit-llvm -o libfoo.bc
ld.lld --cbc -r -o foo.o
                               →  host ELF / Mach-O / bitcode
                                  with the .cbc as a byte array,
                                  a constructor that loads it,
                                  and native stubs for exports
```

There is **no** user-visible wrap tool, no second `ld.lld` process, and no
`cbc-wrap`. Clang still calls `ld.lld --cbc` (or `ld64.lld --cbc`). The
linker decides the product from `-o` plus the ordinary host flags
(`-shared`, `-dylib`, `-r`, `--lto-emit-llvm`).

Success:

1. Every program that links to `a.cbc` today still does, byte-compatible
   with `24`.
2. `clang --target=x86_64-unknown-linux-gnu -fcbc -shared -o libfoo.so …`
   produces an ELF DSO whose `.cbc` payload loads on first use of the DSO,
   and whose default-visibility / address-taken functions are C-ABI symbols
   native code can call.
3. The same DSO (or a static archive of host bitcode) is **LLVM LTO-ready**:
   unused stubs are IR functions and die under internalization + GlobalDCE;
   the payload is kept only if something in the module is kept.
4. Mach-O is in scope for the wrap product (`-dylib`, `__mod_init_func`).
   Darwin **resolution** of TBD/dylib inputs is still the `24` follow-up
   (`ld64.lld --cbc` as a mode of the Mach-O driver). Until that exists,
   Linux ELF is the wrap that must ship; the wrapper IR is flavor-neutral.

## 1. Why `--cbc` itself writes ELF (and what `24` actually forbade)

`24` §4 listed “ELF `Writer`” as a non-goal because the **CBC program** is
not an ELF: no GOT, no PT_LOAD of CBC bytecode, no mixing of host `ET_REL`
into the CBC IR link. That prohibition is unchanged.

It does **not** forbid a later, in-process step that:

1. already has a finished `.cbc` in memory from `lld::cbc::link`,
2. builds a **host-triple** LLVM module (blob + ctor + stubs),
3. either emits that module as bitcode, or codegens it to a host object and
   **then** runs the flavor `Writer` on *that* object.

Running `Writer` on CBC IR would be wrong. Running `Writer` on a host
wrapper that *contains* CBC bytes is ordinary native linking. Putting both
in one `ld.lld --cbc` keeps clang’s `ConstructJob` the short list from `24`
§11 and avoids a second tool’s option-parsing, search paths, and `-L`
policy.

`24` §6.1 “Do not guess CBC mode from a `.cbc` output name” also stays:
`--cbc` is still required. A native link `-o foo.cbc` without `--cbc` must
not silently switch. This document uses the extension only **after** `--cbc`
is set, to choose container vs wrap.

Do **not** add `--oformat=cbc`. `OUTPUT_FORMAT(elf64-x86-64)` inside
`libm.so` scripts would still collide.

## 2. Product selection

`--cbc` is set. Then:

| `-o` name | Other flags | Product |
|---|---|---|
| `*.cbc`, or `-o` omitted (default `a.cbc`) | | CBC container (`24` P13). Host flags `-shared`/`-r` are errors. |
| `*.bc` or `*.ll`, or `--lto-emit-llvm` | | Host-triple LLVM bitcode of the wrapper. Static-lib LTO input. |
| `*.so`, or `-shared` | ELF driver | ELF DSO. `.so` without `-shared` implies `-shared` (warn once). |
| `*.dylib` / `*.tbd` output, or `-dylib` | Mach-O driver | Mach-O dylib. |
| `*.a` | | `llvm-ar` of one host bitcode member (LTO-ready static lib). A thin archive of a single relocatable is acceptable if bitcode emit is off. |
| `*.o` / `*.obj`, or `-r` | | Host relocatable. |
| anything else (`a.out`, no extension, `*.exe`) | | Host **executable**: same wrapper, plus a `main` that N2C-calls the CBC entry if the `.cbc` has one. |

The CBC bytes are always produced first. The wrap is a function of the
output kind, not a different IR-link.

Default `-o` in `--cbc` remains `a.cbc`, so existing tests and `clang` jobs
that omit `-o` do not silently start emitting ELF.

## 3. Pipeline (one `ld.lld --cbc` process)

```
clang --target=x86_64-unknown-linux-gnu -fcbc  -c / -shared / …
        │
        ▼
ld.lld --cbc -o <product> --crt crt-cbc.bc -L … -l …
        │
        ▼
lld::elf::link  (or later lld::macho::link)
        │  resolve as 24: bitcode, archives, INPUT/GROUP, SharedFile
        │  skip compileBitcodeFiles
        │
        ├─ CBCLinkRequest (24 §1)
        ▼
lld::cbc::linkToMemory(request)     // same body as 24, bytes not a file
        │  IR-link, O2, policy, CBC codegen
        │  returns { cbcBytes, exportSet, triple, aotDeps }
        │
        ├─ product is .cbc  →  write bytes, return
        │
        ▼
build host-triple Module            // lld/CBC, still no elf::Ctx
        │  blob global, lib ctor, per-export naked asm stubs
        │
        ├─ bitcode / .a     →  BitcodeWriter / llvm-ar, return
        │
        ▼
host codegen (X86/AArch64 TargetMachine; CBC bytes came from CBC TM, 24 §10.2)
        │  one relocatable object in memory
        ▼
re-enter flavor Writer              // now it is a native link of 1 object
        │  -shared / -dylib / executable
        │  DT_NEEDED / LC_LOAD_DYLIB: libcbcengine, Cangjie runtime,
        │  plus request.aotDeps
        └─ write ELF / Mach-O
```

`lld::cbc::link` as specified in `24` (write a path) becomes a wrapper
around `linkToMemory` + `writeFile`. The ELF driver still must not pass
`elf::Ctx` into emit.

Host codegen may use the in-process LLVM that lld already links. It must
**not** go through `BitcodeCompiler::compile` (that path is ELF LTO of the
*inputs*, which we skipped). The wrapper module is synthesized, not an
input.

## 4. What the `.cbc` inside a library is

Whole-program CBC of **this** link, same as an executable `.cbc`:

* one data image, one `$cbc.<prog>` name (output stem + hash, `10` §2.3)
* `aotDeps` from needed `SharedFile` SONAMEs (`24` §8)
* crt, legalizer, image init — unchanged

Differences from an executable `.cbc`:

| | Executable `.cbc` | Library `.cbc` |
|---|---|---|
| `mainTypeName` | `Entry.main` | **−1** (`07` §2) |
| Startup method | `Entry.main`: probes, image init, ctors, `__cbc_main`, exit handlers | `__cbc_lib_start`: probes, image init, `llvm.global_ctors` only |
| Who calls it | launcher trampoline | host constructor, through `CJ_MCC_N2CStub` / `InitCJLibraryStub` |
| Shutdown | process exit / CBC `exit` | `.fini_array` / `__cxa_finalize(&__dso_handle)` on `dlclose` |
| Function pointers of exports | E5 descriptor | **host stub address** (§6) |

`__cbc_lib_start` still contains the dead `ld.fnptr @abort` and `ld.fcb`
probes so an unpatched engine fails while rewriting the library entry,
before any export runs.

Per-DSO `__dso_handle`: the wrap module defines a unique object; CBC
`__cxa_atexit` registrations from this blob use that pointer. The host
destructor calls `__cxa_finalize` via N2C (or a small CBC helper). Today’s
single-handle crt (`10` §5.2) is a process-wide executable assumption and
must be generalized.

## 5. Engine API (cbc-engine)

Today: `Loader::Load(RandomAccessFile)` then `Build()` once;
`EnsureEngineInitialized` is a no-op afterwards; `ByteArrayRandomAccessFile`
is test-only. A DSO constructor cannot load a second blob, and cannot load
from `.rodata`.

Required exports (names indicative):

```c
// Idempotent. Starts Cangjie runtime + interpreter_bridge_init if needed.
void cbc_engine_ensure_init(void);

// Install a CBC image that outlives the call (DSO mapping). Safe after
// the engine already exists (dlopen of a second CBC library).
int  cbc_engine_load_buffer(const void *bytes, size_t n, const char *name);

// After load: DynamicFunctionHandle* for $cbc.<prog>:Prog.<method>
void *cbc_engine_lookup(const char *cbc_name, const char *method);
```

`aotDeps` are still `dlopen`’d by the engine. For a DSO, also emit them as
`DT_NEEDED` / `LC_LOAD_DYLIB` so the native loader binds them before
constructors. Constructor-thread `dlopen` of the same SONAME is then a
no-op.

`GetDirectCallTrampoline` (`TRAMPOLINE_COUNT = 1024`) is **not** used for
exports. E5 exists so C programs are not capped there. Each export has its
own enter thunk in the wrapper (§7) that loads a `fuh` slot and jumps to
`engine_all_regs_c2i_call` / `engine_iregs_only_c2i_call`.

### 5.1 Native→CBC is in scope for stubs

`13` §8 left callbacks unverified: GC walk across native frames, exception
unwind through `CJ_MCC_N2CStub` into C2I, foreign-thread attach. Library
exports **are** that path:

```
native caller
  → exported stub          (host text, C ABI)
      → CJ_MCC_N2CStub     (runtime: attach, LeaveSaferegion, TLS_REG)
          → enter thunk    (rax = fuh)
              → engine_*_c2i_call
                  → CBC method
```

This is the same attach `pthread_support.md` describes, with a real C
prototype instead of a closure. It must be tested before shipping stubs
(GC while a native thread is inside a CBC export; exception from CBC
caught by native `catch` / process terminate; nested N2C while already
attached). Until those tests pass, the wrap product is not done — emitting
the ELF is not enough.

`CBCNativeCallLegalizer`: passing a stubbed function to native is **not**
an error. Passing a descriptor-only function still is. `qsort` et al.
become legal when the comparator is stubbed (the libc function calls the
stub, which N2C-attaches that OS thread). Signal handlers stay refused
(async entry still unverified).

## 6. Which functions get stubs

Cangjie uses an explicit `@C`. C/C++ equivalent, after LTO of the CBC IR:

1. **Address-taken** (`hasAddressTaken()`, vtable / `FNPTR64` / stored in
   the image / passed to native).
2. **Default-visibility external** definitions, so a `.so` API is callable
   even when the body never takes its own address.
3. Driver default **`-fvisibility=hidden`**, so (2) is opt-in.

Clang attribute (optional, later): `__attribute__((cbc_export))` as a
forced stub, matching `@C`.

Internal-only functions keep E5 descriptors. A function that is both
called indirectly from CBC and passed to native has **one** identity: the
stub address. CBC `call.indirect` then takes the native path into the stub
(N2C+C2I). That is slower than the descriptor fast path; WholeProgramDevirt
still applies.

Image `FNPTR64` for stubbed functions stores the **host stub symbol**, not
`ld.fnptr` of a descriptor. Otherwise `&foo` in the image and the ELF
symbol disagree, and native code calling the pointer faults on the
non-executable descriptor page.

## 7. Wrapper IR: function-level assembly, not module asm

The wrap is a host-triple LLVM module (`x86_64-unknown-linux-gnu`,
`aarch64-apple-darwin`, …). After `26` the CBC IR uses that same triple;
the wrap is still compiled with the **host** TargetMachine, not CBC.

### 7.1 Why not `module asm`

Module-level assembly is one opaque blob. LTO internalization and GlobalDCE
cannot delete an unused export: the text is not an `llvm::Function`. Native
`--gc-sections` can drop `.text.foo` *after* codegen, but a bitcode `.a`
member pulled in because **one** stub is referenced still compiles every
stub in the blob. That is the wrong polarity for “LLVM LTO ready.”

### 7.2 Naked functions with an asm body

Each export is an IR function with `naked` + `noinline`. The body is a
single `asm sideeffect` (and `unreachable`). LLVM emits no prologue. The
C ABI argument registers are already live because callers use the IR
signature; the asm must **not** mention the IR arguments (a constraint
would make ISel move them).

IR operands that *are* referenced (the enter thunk, the `fuh` slot) are
ordinary SSA uses, so DCE of `@foo` takes `@cbc_enter_foo` and `@foo.fuh`
with it when they share a `comdat`.

Sketch (x86-64 SysV; AArch64 is the same idea with `x9`/`x10` as in
`N2CStub.S`):

```llvm
@__cbc_blob.<id> = private constant [N x i8] c"CBC\01...", align 16

@foo.fuh = private global ptr null, comdat($foo)

define hidden void @cbc_enter_foo() unnamed_addr comdat($foo) naked {
  tail call void asm sideeffect
    "movq $0, %rax\n\tjmp engine_all_regs_c2i_call",
    "*m,~{rax}"(ptr @foo.fuh)
  unreachable
}

define double @foo(ptr, i64, i64) unnamed_addr comdat($foo) naked {
  ; Cangjie cjstub: plant {cpStackSize, callee} under the return address,
  ; do not touch argument registers, jmp N2CStub.
  tail call void asm sideeffect
    "movq (%rsp), %r11\n\t"
    "subq $$16, %rsp\n\t"
    "movq %r11, (%rsp)\n\t"
    "leaq $0(%rip), %r11\n\t"
    "movq %r11, 16(%rsp)\n\t"
    "movq $$0, 8(%rsp)\n\t"
    "jmp CJ_MCC_N2CStub",
    "X,~{r11},~{dirflag},~{fpsr},~{flags}"(ptr @cbc_enter_foo)
  unreachable
}
```

`cpStackSize` is the host ABI stack-argument area, 16-byte aligned, from
the same `X86_64ABIInfo` / `AArch64ABIInfo` clang already uses. Zero when
everything fits in registers (`c2cj` is this case).

`engine_iregs_only_c2i_call` when the prototype has no float args;
`engine_all_regs_c2i_call` otherwise. Matching Cangjie’s two N2C
instantiations is unnecessary: N2CStub always saves both register files;
the *callee* of N2C is our enter thunk, which picks the C2I adapter.

Variadic exports: v1 error, or a marshal into CBC’s buffer convention.
Naked asm must preserve `al` / vector argument registers if we ever allow
them. Two-register returns: N2CStub already restores `rax`/`rdx`/`xmm0-1`
(AArch64 `x0-3`,`x8`,`d0-3`); confirm C2I copies both halves into the
`Ectype` before relying on `div_t`-style exports.

Do **not** implement stubs by porting `emitCangjieCallStubInstImpl` into
upstream unless a later change needs the `"cjstub"` IR contract for some
other reason. Naked asm keeps the host backends stock, which is what LTO
codegen of a consumer’s `-flto` link will use.

### 7.3 Constructor and comdat GC

```llvm
define internal void @__cbc_lib_init() {
  call void @cbc_engine_ensure_init()
  call void @cbc_engine_load_buffer(ptr @__cbc_blob.<id>, i64 N, ptr @.name)
  ; per surviving stub (see below):
  %h = call ptr @cbc_engine_lookup(ptr @.name, ptr @.foo)
  store ptr %h, ptr @foo.fuh
  ; N2C into __cbc_lib_start  (InitCJLibraryStub: push callee, push 0, call N2C)
  ret void
}

@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [
  { i32 65535, ptr @__cbc_lib_init, ptr @__cbc_blob.<id> }
]
```

If `@foo` dies, `@foo.fuh` must not remain a use that keeps the ctor’s
lookup. Two workable shapes:

* **A (preferred).** One `global_ctors` entry for `load_buffer` +
  `__cbc_lib_start`, associated with the blob. Each stub’s `fuh` is filled
  **lazily** on first call (enter thunk: if null, lookup; then C2I). Unused
  stubs disappear completely; the blob+lib_start stay iff any stub or a
  forced `llvm.used` (explicit `cbc_export` with no callers is a DSO
  export: those functions are live because they are `external` and the
  output is a DSO).
* **B.** Per-stub `comdat($foo)` ctor piece that only stores `fuh`.
  Internalize+DCE of `$foo` drops that piece. Slightly more ELF ctor noise.

For a **DSO**, default-visibility stubs are exported: LTO must **not**
internalize them. For a **static archive** consumed by an executable, LTO
internalizes everything except symbols referenced from outside, then DCE
drops unused stubs. That is the case function-level asm exists for.

Lazy lookup (A) also avoids a ctor that names every export in IR, which
would keep them all alive.

The blob global is referenced from `__cbc_lib_init`. `__cbc_lib_init` is in
`llvm.global_ctors` with associated `@__cbc_blob.<id>`. Archive pull:
referencing any `@foo` pulls the bitcode member; the ctor runs. If the
member is LTO-merged into a larger module, unused `@foo` die, ctor+blob
remain because other stubs or the executable’s use of the library persist;
if **nothing** from the member is used, the archive member is not pulled.

### 7.4 PIC

CBC codegen stays `Reloc::Static` (`24` §10.2): the payload is reloc-free
bytes. The wrapper is PIC (`-fPIC` / Mach-O PIC). GOT loads of
`CJ_MCC_N2CStub`, `engine_all_regs_c2i_call`, and `cbc_engine_*` are
ordinary host relocations. Naked asm must use the platform’s PIC idiom
(`foo.fuh(%rip)`, `adrp`/`ldr`, Mach-O `_foo.fuh@GOTPCREL`).

## 8. LTO

CBC bytecode is opaque. “LTO ready” means the **wrapper** is host bitcode.

| Output | LTO sees | Can do | Cannot do |
|---|---|---|---|
| Static `.a` of host bitcode | stubs, ctor, blob constant | Internalize + DCE unused stubs; ICF identical enter thunks | GC individual CBC methods inside the array |
| DSO built with `-flto` | same, at library build | Layout of ctor/stubs | Consumers LTO into the DSO (unless a bitcode sidecar) |
| `--lto-emit-llvm` / `.bc` | the wrap module | Input to a later `clang -flto` | — |

ThinLTO: the blob is a huge constant; do not import it into every ThinLTO
partition. Full LTO for the wrap, or keep the blob in a non-imported
native object, are both acceptable. v1 is full LTO of the wrap module.

CBC-level LTO across static libs (one image, internalized methods) is
**not** this product. That is `24` §16 / `10` §2.2: keep `-fcbc` bitcode
in the `.a`, IR-link at the final link, wrap once. A `.a` that already
contains a compiled blob cannot later drop methods.

## 9. Clang

`ConstructJob` stays a short argv (`24` §11): `ld.lld`, `--cbc`, `--crt`,
CBC `-L` first, host `-L`, inputs, `-lc++` / builtins. Until `26` this
still lives in `CBC.cpp`; the argv does not depend on that. Additions:

| Flag | Passed through |
|---|---|
| `-shared` / `-dynamiclib` | yes → wrap DSO |
| `-fPIC` | yes (implied by `-shared`) |
| `-flto` / `-flto=full` | `--lto-emit-llvm` only when the **output** is a static lib / `.bc`; a DSO still wrap-codegens in lld |
| `-fvisibility=hidden` | driver default for `-shared`/`-dynamiclib` |
| `-static` | static archive of bitcode, not a static ELF exe |

Do **not** start calling `gnutools::Linker::ConstructJob` (crt1, `-z relro`).
The wrap’s native `Writer` is invoked inside lld, not by clang emitting a
second link line.

A Darwin link job (when wrap on Mach-O ships) is a different `ToolChain`:
`ld64.lld --cbc`, syslibroot, no GNU `AddFilePathLibArgs`. How it is
selected (`-fcbc` on DarwinClang) is `26`.

## 10. Cross-library calls

Each wrap is a whole-program CBC of **that** link. A CBC call from libA to
libB’s `foo` is an unresolved declaration in A → AOT native call → B’s
stub → N2C → C2I. Correct and slow. When both are static archives of
**CBC bitcode** (not wrapped yet), IR-link them instead. Once wrapped, the
AOT path is the ABI.

Several blobs in one process: type names already include `<prog>`. Engine
incremental `load_buffer` is what makes `dlopen` order work.

## 11. File-level change list

On top of `24`:

| File | Change |
|---|---|
| `lld/CBC/CBCLink.h` | `linkToMemory`; `CBCLinkResult { bytes, exports, triple, aotDeps }` |
| `lld/CBC/CBCWrap.cpp` **new** | host module: blob, `__cbc_lib_init`, naked stubs, comdat |
| `lld/CBC/CBCWrapAsm.{x86_64,aarch64}.inc` | asm strings for N2C stub + enter thunk (PIC, ELF and Mach-O variants) |
| `lld/ELF/Driver.cpp` | after `cbc::linkToMemory`: `.cbc` write **or** wrap then `Writer` |
| `lld/MachO/Driver.cpp` | same when `--cbc` exists |
| `clang/lib/Driver/ToolChains/CBC.cpp` | `-shared`/`-dynamiclib`, visibility, no second linker |
| `llvm/lib/Target/CBC` | `__cbc_lib_start`; `FNPTR64` of stubbed fns as host symbols; `mainTypeName = -1` |
| `cbc-engine` | `ensure_init`, `load_buffer`, `lookup`; incremental load |
| tests | `clang -shared` DSO called from native C; LTO static `.a` drops unused stub (IR check); engine-probe on library entry |

No changes to in-tree X86/AArch64 asm printers.

## 12. Implementation order

| Step | Work | Exit |
|---|---|---|
| **W0** | `linkToMemory`; `-o foo.cbc` still the `24` path | parity |
| **W1** | Engine `load_buffer` + `lookup` + incremental load | unit tests |
| **W2** | `__cbc_lib_start`; wrap module with blob + ctor, **no** stubs; ELF `-shared` that only initializes | `dlopen` + side effect in a CBC ctor |
| **W3** | Naked N2C stubs; N2C+C2I tests (GC, unwind, nested attach) | native C calls `foo` |
| **W4** | Address-taken / `FNPTR64` = stub; legalizer allows stubbed callbacks | `qsort` with a CBC comparator |
| **W5** | `--lto-emit-llvm` / `.a`; LTO DCE of unused stubs | `opt`/`lld` IR check |
| **W6** | Mach-O `-dylib` wrap (driver hook may still be Linux-only resolution) | `dlopen` on Darwin once the engine+runtime exist |

W3 is gated on `13` §8 verification. Do not ship stubs that only work on
the launcher thread.

`__attribute__((cbc_native))` and a split of the linked module into CBC vs
native functions are **not** these steps (`26` F6, after wrap W2). The wrap
module here is blob + ctor + N2C stubs only.

## 13. Risks

| Risk | Mitigation |
|---|---|
| Naked asm + LTO relocaxes PIC / mangling | Constrained operands on IR symbols; lit tests per OS/arch |
| Lazy `fuh` lookup races | `call_once` / atomic in the enter thunk; ctor eager-fill is a fallback |
| `Writer` after wrap inherits CBC `elf::Ctx` state (scripts, SharedFiles) | Build a **fresh** native link request: one object, `DT_NEEDED` from `aotDeps` + engine/runtime, no CBC bitcode files |
| Accidental ELF from a job that wanted `.cbc` | Default `-o a.cbc`; only non-`.cbc` names wrap |
| 1024 trampolines | Never call `GetDirectCallTrampoline` for exports |
| Module asm sneaks back in for “simplicity” | Reject in review; stubs are `Function`s |
| iOS W^X vs engine code heap | macOS first; iOS is a product decision, not this design |

## 14. Summary

`--cbc` always builds a `.cbc` in memory. If `-o` ends in `.cbc`, that is
the output (`24`). Otherwise `lld/CBC` synthesizes a host module — payload
array, constructor that calls `cbc_engine_load_buffer`, and **naked
functions whose bodies are the N2C stub sequence** — then either writes
bitcode or codegens and runs the flavor `Writer`. That is not “CBC mode
emits ELF of CBC IR”; it is native linking of a wrapper, inside the same
process, so clang does not grow a wrap step.

Function-level assembly exists so unused exports are IR DCE, which
module-level `asm` cannot be. Native→CBC for those exports is in scope and
is the Cangjie `@C` / `CJ_MCC_N2CStub` path, with C2I as the managed
callee instead of AOT Cangjie text.
