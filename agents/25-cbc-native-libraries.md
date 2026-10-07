# 25 — Native ELF/Mach-O libraries with an embedded `.cbc`

Depends on `24-lld-integration.md` (CBC emit lives in `lld/CBC`, selected by
`--cbc`). Authoritative over `24` §4 / §6.1 / §10.3 where they say CBC mode
never runs ELF `Writer` and always writes a `.cbc`. Authoritative over
`07-ir-passes.md` §2 for **pure** library entries (no `__cbc_main`): those
use `__cbc_lib_start` and `mainTypeName = −1`. When the link **has** a C
`main`, the wrapped blob keeps `Entry.main` (§4, §4.1) so the launcher can
run it after `dlopen`. Authoritative over `04-architecture.md` §7.3 /
`10-linker-and-runtime.md` §5.5–§5.6 / `13-engine-changes.md` §8 for
**exported and address-taken** functions only: those become native-callable
`@C`-style stubs. Internal CBC function pointers stay E5 descriptors.

Delivery is two stages (§12): **25a** wrap blob + ctor + `launcher` on
`.cbc`/`.so`; **25b** native ELF symbols (N2C stubs) for CBC exports.
`26-cbc-as-native-flavor.md` extends this further (**26a** / **26b**). This
work does **not** implement `cbc_native`. Clang invoke is `-fcbc` on the
host triple (same as `24` §11 / `26`); do not write `--target=cbc_*`.

The engine distribution’s runner is **`launcher`**
(`cbc-engine-initial-stage/tools/launcher/`), not `cbc-run`.

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
loads. This work makes the **same** `--cbc` invocation produce a **host
artifact** when the output name is not `.cbc`:

```
ld.lld --cbc -o a.cbc              →  CBC container          (today, 24)
ld.lld --cbc -shared -o libfoo.so
ld.lld --cbc -dylib  -o libfoo.dylib
ld.lld --cbc --lto-emit-llvm -o libfoo.bc
ld.lld --cbc -r -o foo.o
                               →  host ELF / Mach-O / bitcode / relocatable
                                  with the .cbc as a byte array,
                                  a constructor that loads it,
                                  and native stubs for exports
```

There is **no** user-visible wrap tool, no second `ld.lld` process, and no
`cbc-wrap`. Clang still calls `ld.lld --cbc` (or `ld64.lld --cbc`). The
linker decides the product from `-o` plus the ordinary host flags
(`-shared`, `-dylib`, `-r`, `--lto-emit-llvm`).

**Runnable by the launcher** (§4.1): `.cbc` and ELF/Mach-O **shared
libraries** only (`launcher a.cbc` or `launcher libfoo.so`). Host bitcode
(`.bc`), static archives (`.a`), and relocatables (`-r`) are packaging /
LTO inputs — the existing launcher does not run them. **Host executable**
wrap (`-o a.out` without `-shared`) remains out of scope; use `.cbc` or a
DSO.

Success:

1. Every program that links to `a.cbc` today still does, byte-compatible
   with `24`.
2. `clang --target=x86_64-unknown-linux-gnu -fcbc -shared -o libfoo.so …`
   produces an ELF DSO whose `.cbc` payload loads on first use of the DSO,
   and whose default-visibility / address-taken functions are C-ABI symbols
   native code can call.
3. If that DSO’s CBC image has a `main`, `launcher libfoo.so …` runs it the
   same way as `launcher a.cbc` (§4.1) — no exported “run main” symbol, no
   host `main` shim.
4. The same wrap module as a static archive of host bitcode (or
   `--lto-emit-llvm`) is **LLVM LTO-ready**: unused stubs are IR functions
   and die under internalization + GlobalDCE.
5. Mach-O is in scope for the wrap product (`-dylib`, `__mod_init_func`).
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
   **then** runs the flavor `Writer` on *that* object (DSO or relocatable).

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

| `-o` name | Other flags | Product | Launcher |
|---|---|---|---|
| `*.cbc`, or `-o` omitted (default `a.cbc`) | | CBC container (`24` P13). Host flags `-shared`/`-r` are errors. | `launcher a.cbc` |
| `*.bc` or `*.ll`, or `--lto-emit-llvm` | | Host-triple LLVM bitcode of the wrapper. Static-lib LTO input. | no |
| `*.so`, or `-shared` | ELF driver | ELF DSO. `.so` without `-shared` implies `-shared` (warn once). | `launcher lib.so` (§4.1) |
| `*.dylib` / `*.tbd` output, or `-dylib` | Mach-O driver | Mach-O dylib. | later (`dlopen`) |
| `*.a` | | `llvm-ar` of one host bitcode member (LTO-ready static lib). A thin archive of a single relocatable is acceptable if bitcode emit is off. | no |
| `*.o` / `*.obj`, or `-r` | | Host relocatable. | no |
| anything else (`a.out`, no extension, `*.exe`) | | **Error** in v1. No host-executable wrap; use `.cbc` or `-shared`. | — |

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
        │  returns { cbcBytes, exportSet, triple, aotDeps, hasMain }
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
re-enter flavor Writer              // native link of 1 object
        │  -shared / -dylib / -r
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

## 4. What the `.cbc` inside a shared library is

Whole-program CBC of **this** link:

* one data image, one `$cbc.<prog>` name (output stem + hash, `10` §2.3)
* `aotDeps` from needed `SharedFile` SONAMEs (`24` §8)
* crt, legalizer — unchanged

Whether the blob is “a program” or “a pure library” depends on whether the
link defines `__cbc_main` (the user’s `main`):

| | Container `.cbc` (launcher file) | DSO wrap, **has** `__cbc_main` | DSO wrap, **no** `main` |
|---|---|---|---|
| `mainTypeName` | `Entry.main` | `Entry.main` (same) | **−1** |
| Host ctor | n/a | `load_buffer` + N2C `__cbc_lib_start` | same |
| `__cbc_lib_start` | n/a (folded into `Entry.main`) | probes, image init, `llvm.global_ctors` only | same |
| `Entry.main` | probes, image init, ctors, `__cbc_main`, exit handlers | **`__cbc_args` / `__cbc_main` / exit handlers only** — no second image init / ctor pass | absent |
| Who runs `Entry.main` | launcher trampoline after `Load(path)` | launcher trampoline after `dlopen` (§4.1) | nobody |
| Shutdown | process exit / CBC `exit` | same when run via launcher; else `.fini_array` on `dlclose` | `.fini_array` / `__cxa_finalize` |
| Function pointers of exports | E5 descriptor | **host stub address** (§6) | same |

`__cbc_lib_start` still contains the dead `ld.fnptr @abort` and `ld.fcb`
probes so an unpatched engine fails while rewriting the library entry,
before any export (or `Entry.main`) runs.

**Do not double-init.** When both `__cbc_lib_start` and `Entry.main` exist,
image init and `llvm.global_ctors` run **only** in `__cbc_lib_start` (DSO
ctor). `Entry.main` is the “run program” half. The container `.cbc` path
keeps today’s single `Entry.main` that does both (no host ctor).

Per-DSO `__dso_handle`: the wrap module defines a unique object; CBC
`__cxa_atexit` registrations from this blob use that pointer. The host
destructor calls `__cxa_finalize` via N2C (or a small CBC helper). Today’s
single-handle crt (`10` §5.2) is a process-wide executable assumption and
must be generalized.

### 4.1 Execution: launcher accepts `.cbc` and ELF DSOs

Today’s launcher (`cbc-engine-initial-stage/tools/launcher/launcher.c`):

1. `engine_set_main_cbc(path)` — first non-option argument.
2. `InitCJRuntime` / `InitCJInterpreter` / `SetCJCommandLineArgs`.
3. `engine_initialize()` → `Loader::Load` of that path as a CBC file, `Build`.
4. `engine_get_entrypoint_trampoline()` → `FindMain(session, g_mainCbc)` via
   `mainTypeName`, then `RunCJTask`.

There is **no** ELF `main` and **no** exported “run main” symbol. Entry is
always the CBC `Entry.main` trampoline.

**Extended launcher** (same binary):

```
init_cangjie_runtime(arg_count);   // argv as today

if (file_starts_with_elf_magic(path)) {   // 0x7F 'E' 'L' 'F'
  // Do NOT Loader::Load(path) as CBC — it is not a CBC file.
  dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  // DSO ctor: cbc_engine_ensure_init + load_buffer(name=path) + __cbc_lib_start
} else {
  g_engine.set_main_cbc(path);
  g_engine.initialize();              // existing Load + Build
}

return run_interpreter_in_managed_ctx();  // get_trampoline → FindMain → RunCJTask
```

Recognizing ELF is magic-byte sniff only. No need to parse dynamic sections
to “run.”

**`FindMain` after `dlopen`:** `load_buffer` must register the image under
`name` equal to the path the launcher passed (realpath of the `.so`).
`engine_set_main_cbc` for an ELF path still sets `g_mainCbc` to that string
so `FindMain(session, g_mainCbc)` finds the buffer-loaded file. Alternatively
`FindMain` may mean “the unique loaded image with `mainTypeName` set” when
exactly one such image exists — path registration is simpler and matches
today.

**Ctor vs launcher init order:** the launcher owns `InitCJRuntime` first.
The DSO ctor’s `cbc_engine_ensure_init` must be idempotent when the runtime
is already up (`25` §5). Do not call today’s `initialize()` path that opens
`g_mainCbc` as a CBC file when the path is ELF.

**Pure plugin (no `main`):** `launcher libplugin.so` → `dlopen` succeeds,
ctor loads CBC, `FindMain` fails → error (same as a missing trampoline
today). Plugins are for `dlopen` + stubs from another process, not
`launcher`.

**No host executable wrap.** `./myapp` as a CBC product is out of scope; use
`launcher a.cbc` or `launcher libapp.so`.

## 5. Engine API (cbc-engine)

Today: `Loader::Load(RandomAccessFile)` then `Build()` once;
`EnsureEngineInitialized` is a no-op afterwards; `ByteArrayRandomAccessFile`
is test-only. A DSO constructor cannot load a second blob, and cannot load
from `.rodata`.

Required exports (names indicative):

```c
// Idempotent. Starts Cangjie runtime + interpreter_bridge_init if needed.
// Safe when the launcher already called InitCJRuntime.
void cbc_engine_ensure_init(void);

// Install a CBC image that outlives the call (DSO mapping). Safe after
// the engine already exists (dlopen of a second CBC library).
// `name` is the identity FindMain / aotDeps use (launcher: path of the .so).
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
`engine_all_regs_c2i_call` / `engine_iregs_only_c2i_call`. The **program**
entry still uses `engine_get_entrypoint_trampoline` / `FindMain` as today
(§4.1).

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

`__cbc_main` / `Entry.main` are **not** ELF-exported stubs. The launcher
enters them through `FindMain` + the existing trampoline, not through a
host `main` or an exported “run main” symbol.

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
other reason. Naked asm keeps the host backends stock.

### 7.3 Constructor and comdat GC

```llvm
define internal void @__cbc_lib_init() {
  call void @cbc_engine_ensure_init()
  call void @cbc_engine_load_buffer(ptr @__cbc_blob.<id>, i64 N, ptr @.name)
  ; .name is the DSO path / SONAME identity the launcher also passes to FindMain
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
A finished DSO is not re-LTO’d by consumers; a `.a` / `.bc` of the wrap is.

| Output | LTO sees | Can do | Cannot do | Launcher |
|---|---|---|---|---|
| Static `.a` of host bitcode | stubs, ctor, blob constant | Internalize + DCE unused stubs; ICF identical enter thunks | GC individual CBC methods inside the array | no |
| DSO built with `-flto` | same, at library build | Layout of ctor/stubs | Consumers LTO into the DSO (unless a bitcode sidecar) | yes (§4.1) |
| `--lto-emit-llvm` / `.bc` | the wrap module | Input to a later `clang -flto` | — | no |

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
| `-r` | host relocatable wrap |

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
incremental `load_buffer` is what makes `dlopen` order work. Only one
loaded image should carry `mainTypeName` if `launcher` is to call
`FindMain` unambiguously (or the launcher’s path names that image).

## 11. File-level change list

On top of `24`:

| File | Change |
|---|---|
| `lld/CBC/CBCLink.h` | `linkToMemory`; `CBCLinkResult { bytes, exports, triple, aotDeps, hasMain }` |
| `lld/CBC/CBCWrap.cpp` **new** | host module: blob, `__cbc_lib_init`, naked stubs, comdat |
| `lld/CBC/CBCWrapAsm.{x86_64,aarch64}.inc` | asm strings for N2C stub + enter thunk (PIC, ELF and Mach-O variants) |
| `lld/ELF/Driver.cpp` | after `cbc::linkToMemory`: `.cbc` write **or** wrap DSO then `Writer` |
| `lld/MachO/Driver.cpp` | same when `--cbc` exists |
| `clang/lib/Driver/ToolChains/CBC.cpp` | `-shared`/`-dynamiclib`, visibility, `-r` / `--lto-emit-llvm` / `-static` as wrap modes; no second linker |
| `llvm/lib/Target/CBC` | `__cbc_lib_start`; split `Entry.main` when wrap+hasMain; `FNPTR64` of stubbed fns as host symbols; `mainTypeName = −1` only if no `__cbc_main` |
| `cbc-engine` | `ensure_init`, `load_buffer(name)`, `lookup`; incremental load; `FindMain` by buffer name |
| launcher | ELF magic → `dlopen` then trampoline; else existing `.cbc` path |
| tests | `clang -shared` DSO from native C; `launcher libapp.so` with `main`; LTO static `.a` drops unused stub (IR check); pure plugin has no trampoline |

No changes to in-tree X86/AArch64 asm printers.

## 12. Two delivery stages (25a / 25b)

This document describes one wrap design. **Ship it in two stages.** Stage 2
adds native symbols (N2C stubs) for CBC functions; stage 1 does not.

### 12.1 Stage 1 (25a) — wrap CBC into a native library

**Goal:** Host artifact that embeds a finished `.cbc`, loads it from a DSO
constructor, and is runnable by `launcher` when the blob has `main`.

| Work | Steps | Notes |
|---|---|---|
| Memory emit | **W0** | `linkToMemory`; `-o foo.cbc` parity with `24` |
| Engine buffer load | **W1** | `ensure_init`, `load_buffer(name)`, `lookup`, incremental load |
| Wrap without stubs | **W2** | Blob + ctor + `__cbc_lib_start`; ELF `-shared` |
| Launcher ELF path | **W2b** | Magic sniff → `dlopen` → `FindMain`; keep `Entry.main` when hasMain (§4 / §4.1) |

**No** naked N2C stubs, **no** ELF exports for CBC functions, **no**
`FNPTR64` → stub rewrite, **no** stubbed `qsort` callbacks.

**Exit criteria for 25a:** `clang -fcbc -shared -o libapp.so …` produces a
DSO whose ctor loads CBC; `launcher libapp.so args…` returns `main`’s
status when present; a pure plugin `dlopen`s and runs CBC ctors without a
trampoline; `-o a.cbc` unchanged.

Optional in 25a: emit wrap as `.bc` / thin `.a` / `-r` with **only**
blob+ctor (no stub DCE story yet). Full LTO packaging is **25b** / **W5**.

### 12.2 Stage 2 (25b) — native symbols for CBC functions

**Goal:** Default-visibility / address-taken CBC functions appear as C-ABI
ELF symbols whose bodies are the N2C stub sequence (§6–§7).

| Work | Steps | Depends on |
|---|---|---|
| Naked N2C stubs | **W3** | **W2**; gated on `13` §8 (GC, unwind, nested attach) |
| Address-taken / `FNPTR64` = stub; legalizer | **W4** | W3 |
| Packaging LTO DCE | **W5** | Stubs exist so unused exports can die |
| Mach-O `-dylib` | **W6** | Can land 25a-shaped first, then stubs; or after ELF 25b |

**Exit criteria for 25b:** native C calls an exported CBC `foo`; `qsort`
with a CBC comparator works when the comparator is stubbed; unused stubs
DCE from a bitcode `.a`.

### 12.3 Relation to `26`

```
24 ──► 25a (W0–W2b) ──► 25b (W3–W5) ──► 26b (cbc_native)
              │
              └──► 26a (-fcbc) may run in parallel
```

`26b` needs wrap **W2+** (somewhere to put Module N `.text`). It does not
strictly need W3 if native code only lives in `cbc_native` functions, but
calling CBC from native still needs 25b stubs (or a separate exported
DSO).

`__attribute__((cbc_native))` is **not** part of 25a or 25b.

## 13. Implementation order

Relative to §12. Steps are tagged **25a** or **25b**.

| Step | Stage | Work | Exit |
|---|---|---|---|
| **W0** | 25a | `linkToMemory`; `-o foo.cbc` still the `24` path | parity |
| **W1** | 25a | Engine `load_buffer` + `lookup` + incremental load; register `name` | unit tests |
| **W2** | 25a | `__cbc_lib_start`; wrap module with blob + ctor, **no** stubs; ELF `-shared` | `dlopen` + side effect in a CBC ctor |
| **W2b** | 25a | `launcher`: ELF sniff + `dlopen`; `FindMain` after ctor; keep `Entry.main` when hasMain | `launcher libapp.so args…` returns `main`’s status |
| **W3** | 25b | Naked N2C stubs; N2C+C2I tests (GC, unwind, nested attach) | native C calls `foo` |
| **W4** | 25b | Address-taken / `FNPTR64` = stub; legalizer allows stubbed callbacks | `qsort` with a CBC comparator |
| **W5** | 25b | `--lto-emit-llvm` / `.a` / `-r`; LTO DCE of unused stubs | `opt`/`lld` IR check |
| **W6** | 25b† | Mach-O `-dylib` wrap (driver hook may still be Linux-only resolution) | `dlopen` on Darwin once the engine+runtime exist |

† W6 may ship a 25a-shaped Mach-O wrap before stubs; full parity with ELF
25b follows.

W3 is gated on `13` §8 verification. Do not ship stubs that only work on
the launcher thread.

## 14. Risks

| Risk | Mitigation |
|---|---|
| Naked asm + LTO relocaxes PIC / mangling | Constrained operands on IR symbols; lit tests per OS/arch |
| Lazy `fuh` lookup races | `call_once` / atomic in the enter thunk; ctor eager-fill is a fallback |
| `Writer` after wrap inherits CBC `elf::Ctx` state (scripts, SharedFiles) | Build a **fresh** native link request: one object, `DT_NEEDED` from `aotDeps` + engine/runtime, no CBC bitcode files |
| Accidental ELF from a job that wanted `.cbc` | Default `-o a.cbc`; only non-`.cbc` names wrap |
| Double image init if `Entry.main` still runs ctors | §4 split: lib_start = init/ctors; Entry.main = argv/`main`/atexit only when wrap |
| `FindMain` cannot see buffer-loaded image | `load_buffer` name = launcher path; set `g_mainCbc` to the same string |
| Ctor runs before launcher `InitCJRuntime` if someone `dlopen`s without launcher | `ensure_init` starts runtime; launcher path still prefers Init first |
| 1024 trampolines | Never call `GetDirectCallTrampoline` for exports; program entry uses existing trampoline |
| Module asm sneaks back in for “simplicity” | Reject in review; stubs are `Function`s |
| iOS W^X vs engine code heap | macOS first; iOS is a product decision, not this design |

## 15. Summary

`--cbc` always builds a `.cbc` in memory. If `-o` ends in `.cbc`, that is
the output (`24`). Otherwise `lld/CBC` synthesizes a host module — payload
array, constructor that calls `cbc_engine_load_buffer`, and **naked
functions whose bodies are the N2C stub sequence** — then either writes
bitcode / archives or codegens and runs the flavor `Writer`. That is not
“CBC mode emits ELF of CBC IR”; it is native linking of a wrapper, inside
the same process, so clang does not grow a wrap step. Host **executable**
wrap is out of scope.

**Running:** `launcher a.cbc` or `launcher libapp.so` (ELF magic → `dlopen` →
ctor loads CBC → same `FindMain` trampoline). `.bc` / `.a` / `-r` are not
launcher inputs. No host `main`, no exported “run main” symbol. Blobs with a C `main`
keep `mainTypeName`; pure libraries set it to −1.

Function-level assembly exists so unused exports are IR DCE, which
module-level `asm` cannot be. Native→CBC for those exports is in scope and
is the Cangjie `@C` / `CJ_MCC_N2CStub` path, with C2I as the managed
callee instead of AOT Cangjie text.
