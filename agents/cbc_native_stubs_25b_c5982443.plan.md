---
name: CBC Native Stubs 25b
overview: "Implement 25b native shared-library stubs: every CBC method gets an N2C entrypoint; engine_load_buffer binds them by MethodDefinition offset; LoadFuncPtr records stub addresses so call.indirect is I2I. Engine first, then FileWriter table, then host module asm, then legalizer and tests."
todos:
  - id: engine-api-fuh
    content: "FuH.nativePtr after asm prefix; FunctionDescriptors::Add + map Lookup; remove Get from LoadFuncPtr; extend engine_load_buffer; drop enter_via_n2c from load_buffer; index 0 calls infos[0].nativePtr"
    status: pending
  - id: filewriter-exports
    content: "CBCCompiledMethod vis/varargs; buildCBCFile returns method offsets with __cbc_lib_start at index 0; plumb through AsmPrinter/TargetMachine into CBCLinkResult"
    status: pending
  - id: host-module
    content: "CBCWrap.cpp: infos/handles globals, module asm N2C+enter, ctor calls new load_buffer; clang -fvisibility=hidden on -shared"
    status: pending
  - id: legalizer-docs-tests
    content: "Relax qsort/pthread_create/CBC-fnptr-to-native on shared-library links; keep signal and pthread_atfork; engine tests; update 04/07/10"
    status: pending
isProject: false
---

# 25b — N2C stubs for native shared libraries

LLVM `--cbc -shared` already embeds a CBC payload in a host ELF DSO ([`CBCWrap.cpp`](lld/CBC/CBCWrap.cpp)). This work makes every CBC method a C-callable ELF symbol (N2C stub) and makes CBC `ld.fnptr` / `call.indirect` use that same address, with `CALL_REG` taking I2I when Lookup hits.

Standalone `.cbc` files are deprecated for this backend: `LoadFuncPtr` has no descriptor fallback.

Existing identifiers (`CBCWrap.cpp`, `emitSharedWrap`, `request.wrap`, `cbc-wrap`) are not renamed. New comments say **native shared library** / **host module**.

## Non-goals

- Mach-O, `.bc`/`.a`/`-r`, stub DCE, `engine_lookup` / `engine_ensure_init` / `init_func`
- Porting `emitCangjieCallStubInstImpl`
- Unhiding C2I (`jmp [fuh+8]` instead)
- Host executable wrap; `main` as an ELF-exported stub
- LLVM lit tests

## Data flow

```mermaid
sequenceDiagram
  participant Ctor as __cbc_lib_init
  participant Eng as engine_load_buffer
  participant FD as FunctionDescriptors
  participant Start as infos0_nativePtr
  Ctor->>Eng: blob name count infos handles
  Eng->>Eng: AddFile Acquire all methods
  Eng->>FD: Add nativePtr fuh
  Eng->>Eng: handles[i] = fuh
  Eng->>Start: call N2C stub for __cbc_lib_start
  Note over Start: N2C attaches; enter loads handles[0]
```

Native `foo` / `qsort(cmp)` / `pthread_create(start)`: N2C stub → enter_i → `jmp [fuh->c2call]`. CBC `ld.fnptr @foo` bakes the same stub address; `CALL_REG` Lookup → I2I.

---

## Phase 1 — Engine

Do this first so the host module can target a stable C API.

### 1.1 `DynamicFunctionHandle` ([function_handle.h](cbc-engine/src/interpreter/function_handle.h))

Keep the asm prefix:

```text
+0  i2call
+8  c2call     FUNCTION_HANDLE_C2CALL_OFFSET
+16 bytecode*  FUNCTION_HANDLE_BYTECODE_OFFSET
```

Add `void* nativePtr = nullptr;` **after** `methodDef` (never before `bytecode`). Extend the existing `static_assert`s in [function_handle.cpp](cbc-engine/src/interpreter/function_handle.cpp); do not add an asm offset for `nativePtr`.

### 1.2 `FunctionDescriptors` ([function_descriptors.h](cbc-engine/src/interpreter/function_descriptors.h) / [.cpp](cbc-engine/src/interpreter/function_descriptors.cpp))

Replace `Get` with:

```cpp
void Add(void* nativePtr, Interpretation::DynamicFunctionHandle* fuh);
```

`Lookup(uintptr_t)` becomes a hash map `nativePtr → fuh` (mutex, or fill-then-publish at the end of `load_buffer`). Drop the NX mmap path: the only `Get` caller is `LoadFuncPtr` ([isa_rewriter.cpp](cbc-engine/src/cbc/isa_rewriter.cpp) ~1166).

`Add` is what turns `interpreter → I2C → N2C → C2I` into `interpreter → I2I`. The FuH field is only the forward address for `LoadFuncPtr`. Both are required.

### 1.3 `LoadFuncPtr` ([isa_rewriter.cpp](cbc-engine/src/cbc/isa_rewriter.cpp) ~1154)

Exactly two cases:

```cpp
if (auto compiled = std::get_if<DirectCall::Compiled>(&(*m)->data)) {
    address = compiled->funcPtr;
} else {
    auto fuh = std::get<DynamicFunctionHandle*>((*m)->data);
    address = reinterpret_cast<uintptr_t>(fuh->nativePtr);
    RTSupport::FunctionDescriptors::Add(fuh->nativePtr, fuh);
    // nativePtr non-null: load_buffer Acquired every method first
}
```

(`Add` from `load_buffer` already registered the pair; calling `Add` again from rewrite must be idempotent.)

### 1.4 `engine_load_buffer` ([cbc_engine.h](cbc-engine/include/cbc_engine.h), [entrypoint.cpp](cbc-engine/src/runtimesupport/impl/entrypoint.cpp))

```c
typedef struct {
    void *nativePtr;
    uint32_t offset; /* pool Offset<MethodDefinition>; Read at poolOffset+offset */
} ExportedInfo;

int engine_load_buffer(
    const void *bytes, size_t n, const char *name,
    uint32_t exported_count,
    const ExportedInfo *exported_infos,
    void **exported_handles);
```

C layout of `ExportedInfo` is 16 bytes on LP64 (`ptr` + `u32` + pad). Host IR must match.

Require `exported_count >= 1`, `infos`/`handles` non-null. Index **0** is `__cbc_lib_start`.

Algorithm (under `g_InitializationGuard` except the final C call):

1. Existing empty `Loader{}.Build()` if needed; `AddFile` (FileId assigned).
2. For `i` in `0 .. count-1`:
   - `Identifier<MethodDefinition>(Offset(infos[i].offset), fileId)`
   - `AcquireTagged` → `DynamicFunctionHandle*`
   - `fuh->nativePtr = infos[i].nativePtr`
   - `FunctionDescriptors::Add(nativePtr, fuh)`
   - `exported_handles[i] = fuh`
3. Unlock, then `((void(*)(void))exported_infos[0].nativePtr)()`. Do **not** `FindMethod(__cbc_lib_start)` and do **not** `engine_enter_via_n2c`.

Remove the lib_start use of `engine_n2c_stub` / `engine_enter_via_n2c` / `engine_n2c_c2i_body`. Do not patch AArch64 `x0→x9` on that body. Optionally leave the symbols in `trampolines.S` unused.

Update [engine_load_buffer_test.cpp](cbc-engine/test/engine_load_buffer_test.cpp) only if it starts calling the C API (today it uses `AddFile` directly).

---

## Phase 2 — Method offset table (LLVM CBC codegen)

Offsets are pool offsets from `add(Def)` in [CBCFileWriter.cpp](llvm/lib/Target/CBC/CBCFileWriter.cpp) (~372–382). `Reader::Read` uses `GetMethodDefSectionOffs() + offset` (`poolOffset` = 57). AsmPrinter cannot know them per-function; **`buildCBCFile` emits the vector**.

### 2.1 `CBCCompiledMethod` ([CBCFileWriter.h](llvm/lib/Target/CBC/CBCFileWriter.h))

From `MF.getFunction()` in [CBCAsmPrinter.cpp](llvm/lib/Target/CBC/CBCAsmPrinter.cpp) `runOnMachineFunction` (~447):

- `bool DefaultVis` — `getVisibility() == DefaultVisibility`
- `bool AddressTaken` — `hasAddressTaken()`
- `bool VarArg` — `isVarArg()`

### 2.2 `buildCBCFile`

- Reorder so the method named `__cbc_lib_start` is **first** (fatal if missing when `cbc-wrap` is set; AsmPrinter already requires that flag for no-`main`).
- After each `MDefs.push_back(add(Def))`, record `{ Name, Offset, DefaultVis, AddressTaken, VarArg, Params, RetFloat }`.
- Return both the file bytes and this vector. Change `emitEndOfAsmFile` accordingly.

### 2.3 Plumb into `CBCLinkResult`

`emitEndOfAsmFile` has no direct `CBCLinkResult`. Put the vector on **`CBCTargetMachine`** (created in [CBCLink.cpp](lld/CBC/CBCLink.cpp) ~277). After `CodeGen.run(*Composite)`, copy it onto:

```cpp
struct CBCExport {
  std::string name;
  uint32_t offset;
  bool defaultVis;
  bool addressTaken;
  bool varArg;
  std::string params; // 'i'/'f'
  bool retFloat;
};
// CBCLinkResult:
SmallVector<CBCExport, 0> exports; // index 0 == __cbc_lib_start
```

Reject `varArg` or a prototype that does not fit the host register file (SysV: 6 int + 8 SSE; AAPCS: 8 int + 8 SIMD). `cpStackSize` is then **0** for v1. Struct/sret/byval: error (Params is only i/f; if `MaxCalleeStackArgs != 0`, error).

---

## Phase 3 — Host shared-library module

### 3.1 IR in [`buildWrapModule`](lld/CBC/CBCWrap.cpp)

Keep blob + `dladdr`/`realpath` name.

Add matching C types:

- `%ExportedInfo = type { ptr, i32, i32 }` (padding)
- `@__cbc_exported_handles = internal global [N x ptr] zeroinitializer`
- `@__cbc_exported_infos = internal constant [N x %ExportedInfo] [ { ptr @sym_i, i32 offset_i, i32 0 }, ... ]`

`@sym_i` is an IR `declare` of the CBC method’s name and type (reconstruct a plausible `FunctionType` from `params`/`retFloat`: pointers as `ptr`, integers as `i64`, floats as `double` — enough for a declaration; the body is asm). Linkage: `external`. Visibility: **default** if `defaultVis || addressTaken`, else **hidden**. Never default-vis for `main` / `__cbc_main` / `__cbc_entry` / `__cbc_lib_start` (lib_start stays hidden even at index 0).

Ctor calls:

```c
engine_load_buffer(blob, len, path, N, infos, handles);
```

No extra lib_start call.

### 3.2 Module asm ([`N2CStub.S`](cangjie_runtime/runtime/src/arch/x86_64_linux/N2CStub.S) / [aarch64](cangjie_runtime/runtime/src/arch/aarch64_linux/N2CStub.S))

`exported_infos` references every stub, so nothing is dead. Emit **one module-asm blob** (not naked IR functions).

Per index `i`, symbol `name`:

**x86-64** (tail plant so N2CStub’s `ret` is the C caller; after `push %rbp`, `16(%rbp)=cpStackSize`, `24(%rbp)=callee`):

```text
movq (%rsp), %r11
subq $16, %rsp
movq %r11, (%rsp)
leaq .Lenter_<i>(%rip), %r11
movq %r11, 16(%rsp)
movq $0, 8(%rsp)
jmp CJ_MCC_N2CStub@PLT
.Lenter_<i>:
movq __cbc_exported_handles+8*i(%rip), %rax
jmp *8(%rax)    /* c2call */
```

**AArch64** (`x9`=enter, `x10`=size; N2CStub `ldp`+pops 16 bytes immediately). Use `b`/`br` so `x30` stays the native return. GOT for `CJ_MCC_N2CStub`. Enter: load `handles[i]` into `x9` (C2I fuh), `br` to `[x9, #8]`. Integer args stay in `x0–x7`.

`.globl` / `.hidden` matching the IR `declare`. `$cpStackSize` is 0 in v1.

Generate in `CBCWrap.cpp` (or `CBCWrapAsm.x86_64.inc` templates with `index`/`name` filled in). `appendModuleInlineAsm`. ELF only; host triple from existing `hostTripleFromCBC`.

### 3.3 Clang ([CBC.cpp](clang/lib/Driver/ToolChains/CBC.cpp))

For `-shared`, add default `-fvisibility=hidden` so dynsym APIs are opt-in (`visibility("default")`).

### 3.4 Legalizer ([CBCLink.cpp](lld/CBC/CBCLink.cpp) ~231–269)

When `request.wrap` (shared-library link), **delete** these errors:

- live decls of `qsort`, `qsort_r`, `bsearch`, `lfind`, `lsearch`, `tsearch*`/`twalk*`, `ftw*`
- live decls of `pthread_create`, `pthread_once`
- `passing CBC function 'X' to native function 'Y'`

**Keep:** `pthread_atfork` (fork, not attach); `signal`/`sigaction` CBC handlers (async — existing crt guard / do not add a new allow). `_Z*` unresolved decls unchanged.

N2C attaches the OS thread that calls the stub, including a `pthread_create` start routine.

When `!request.wrap` (deprecated `.cbc` emit), leave today’s checks.

---

## Phase 4 — Tests and docs

`cbc-engine/test` (no lit):

1. Shared library: default-vis CBC `foo`; native `dlopen`+`dlsym`+call.
2. `qsort` with CBC comparator.
3. `pthread_create` of a CBC start routine.
4. CBC `call.indirect` of `&foo` does not nest N2C (I2I).
5. GC while a foreign pthread is in a CBC export; nested N2C; exception through N2C.

Docs: [`04-architecture.md`](agents/04-architecture.md) §7.3, [`07-ir-passes.md`](agents/07-ir-passes.md) §6, [`10-linker-and-runtime.md`](agents/10-linker-and-runtime.md) §5.5–§5.6 — callbacks legal in native shared libraries; signal/atfork still not.

---

## Implementation order

1. Engine 1.1–1.4 (compiles; old `CBCWrap.cpp` will not link until step 3).
2. FileWriter + TM plumbing + `CBCLinkResult::exports` (shared-library links still work without stubs if exports are unused — then immediately 3).
3. Host module + ctor signature (unblocks `dlopen` of a DSO with stubs).
4. Legalizer so `qsort`/`pthread_create` programs link.
5. Tests + docs.

Step 1 and 2 can proceed in parallel until the ctor call site.

---

## Files

| Area | Files |
|---|---|
| Engine API | [cbc_engine.h](cbc-engine/include/cbc_engine.h), [entrypoint.cpp](cbc-engine/src/runtimesupport/impl/entrypoint.cpp) |
| FuH / descriptors | [function_handle.h](cbc-engine/src/interpreter/function_handle.h), [function_handle.cpp](cbc-engine/src/interpreter/function_handle.cpp), [function_descriptors.h](cbc-engine/src/interpreter/function_descriptors.h), [function_descriptors.cpp](cbc-engine/src/interpreter/function_descriptors.cpp) |
| Rewrite | [isa_rewriter.cpp](cbc-engine/src/cbc/isa_rewriter.cpp) |
| Offsets | [CBCFileWriter.h](llvm/lib/Target/CBC/CBCFileWriter.h), [CBCFileWriter.cpp](llvm/lib/Target/CBC/CBCFileWriter.cpp), [CBCAsmPrinter.cpp](llvm/lib/Target/CBC/CBCAsmPrinter.cpp), CBCTargetMachine |
| Link / host module | [CBCLink.h](lld/include/lld/CBC/CBCLink.h), [CBCLink.cpp](lld/CBC/CBCLink.cpp), [CBCWrap.cpp](lld/CBC/CBCWrap.cpp) |
| Clang | [CBC.cpp](clang/lib/Driver/ToolChains/CBC.cpp) |
| Tests / docs | `cbc-engine/test`, agents `04`/`07`/`10` |
