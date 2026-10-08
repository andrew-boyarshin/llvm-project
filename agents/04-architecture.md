# 04 — Mapping C and C++ onto CBC

This file fixes the semantics: how every C/C++ concept is represented on CBC, on the engine
plus the eight accepted changes E1–E8 (`13-engine-changes.md`) and nothing else. The LLVM
components that implement each piece are specified in `05`–`10`; references point there.

## 1. Goals and non-goals

**Goals**

* Compile ISO C17 and C++17 (C++20 where it needs no new runtime support) programs,
  including libc++ and libc++abi, to a `.cbc` that runs on the engine with E1–E8 applied.
* Call the host's native C library and other native libraries directly, with the host ABI.
* Correctness first, then interpretation cost (instruction count).

**Non-goals (and why)**

* Native code calling CBC code from a **standalone `.cbc`** (callbacks into CBC,
  `pthread_create` of CBC code, signal handlers). CBC function pointers there are
  non-executable descriptors (§7.1). Callback-taking libc functions (`qsort`, `bsearch`,
  `ftw`, `pthread_once`, …) are rejected at link time for `.cbc` emit (§8.4). **Native
  shared libraries** (`-fcbc -shared`, `25b`) give every CBC method an N2C stub: `qsort` /
  `pthread_create` / passing CBC fnptrs to native code are legal there; `signal` /
  `pthread_atfork` remain unsupported.
* Inline assembly: no CBC assembler dialect in C sources (clang rejects it).
* SIMD intrinsics (`immintrin.h`, `arm_neon.h`): there are no vector registers. Generic
  vector types (`__attribute__((vector_size))`) work through scalarization.
* `long double` with host precision: CBC has only 32- and 64-bit floats (§15).
* Debug info with line numbers: the engine has no line tables (stack traces show method
  names and file names only).

## 2. Targets and triples

One LLVM architecture, `Triple::cbc`, with two sub-architectures selecting the host ABI
flavour:

| Triple (canonical) | `SubArch` | Host ABI mirrored | Engine build it runs on | Work scope |
|---|---|---|---|---|
| `cbc_x86_64-unknown-linux-gnu` | `CBCSubArch_x86_64` | x86-64 SysV | engine for x86-64 Linux | **in scope** — the only flavour implemented and tested in the current plan |
| `cbc_aarch64-unknown-linux-gnu` | `CBCSubArch_aarch64` | AAPCS64 (Linux) | engine for AArch64 Linux | **follow-up**, designed but not in the current work scope |
| `cbc_aarch64-apple-darwin` / `-ios` | `CBCSubArch_aarch64` | Apple arm64 (variadics on stack, packed stack args) | Apple engine builds | not planned (JIT-restricted platforms are not targeted) |
| `cbc_x86_64-unknown-linux-ohos` | `CBCSubArch_x86_64` | x86-64 SysV | OpenHarmony | not planned |

**Scope of the AArch64 flavour.** The documents keep the AArch64 Linux design (register
mapping, data layout, `va_list`, HFAs, `sret` in `IR9`, the engine's AArch64 adapters)
so that the follow-up needs no redesign, and the code is structured so that the flavour
is a sub-architecture switch. The current work scope implements, tests and ships only
`cbc_x86_64-unknown-linux-gnu`: the triple parses, but `CBCTargetMachine` rejects the
AArch64 sub-architecture with `error: the AArch64 flavour of the CBC target is not
implemented yet` until the follow-up milestone (`11-testing-and-roadmap.md` §3).

The OS/environment components behave as for native targets (headers, `-m:e` vs `-m:o`
mangling, `.so` vs `.dylib`). A `.cbc` built for one flavour must not run on the other:
the CBC file has no field recording the flavour, so the linker writes it into the entry
type's name (`$cbc.<prog>.x64:Entry`) and the startup code checks the engine host at
run time (§13).

### 2.1 Data layout

Identical to the host's so that struct layout, `sizeof`, and alignment match native
headers and native libraries:

* x86-64 flavour: `e-m:e-p:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128`
* AArch64 Linux flavour: `e-m:e-p:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128`

(address spaces of the host layouts are dropped; CBC has a single address space).
`long double` is `double` in C (`09-clang.md` §2) so the `f80` entry is unused.

## 3. Compilation model

```
.c/.cpp ──clang -c──▶ .o (LLVM bitcode, target cbc_*)
                         │
     crt (includes unwind entry points), compiler-rt builtins, libc++, libc++abi
                         │
                         ▼
                  cbc-ld (link step)
     llvm-link + internalize + resolve natives + LTO pipeline
     + CBC whole-program IR passes (07-ir-passes.md)
     + CBC codegen (llc-equivalent, in-process)
                         │
                         ▼
                      a.cbc
```

Whole-program code generation is required because these are global decisions:

* the data image layout (§6),
* which callees are CBC methods and which are native symbols (an undefined symbol after
  linking all bitcode is native), which the native libc rules depend on
  (`10-linker-and-runtime.md` §5),
* the method/field reference pools (one per `.cbc`).

Function pointers are *not* a global decision: every CBC function's pointer is its
engine-allocated descriptor (§7.1), whatever the rest of the program contains.

**Separate code generation (phase 2) is postponed indefinitely.** A relocatable CBC object
format with per-object data images is sketched in `10-linker-and-runtime.md` §4 and
§6.5 here so that the design is not lost, but it is **not in the work scope**: every
build links bitcode and generates code once, at link time. `clang -c` always produces
bitcode.

## 4. Memory model

* One flat address space; pointers are 64-bit integers held in integer registers. Integer
  ↔ pointer casts are no-ops.
* Every load and store is a raw access: `LoadRawMemory`/`StoreRawMemory`
  (base + displacement, `0 <= disp < 2^31`). No GC barriers, no GC references, no typed
  field access. All GC liveness maps are therefore empty.
* Heap memory comes from native `malloc`; the Cangjie GC heap is never used.
* Alignment: unaligned accesses work on both hosts. `volatile` accesses are ordinary
  accesses (the interpreter performs exactly one access per instruction).
* A load or store through a register holding 0 aborts the process (`FATAL` in
  `LoadRec`/`StoreRec`) — acceptable for undefined behaviour.
* Allowed locations a pointer may designate: native heap/static/stack memory, the data
  image (§6), the current frame's untyped memory block (§5.2), a live dynamic `alloca`
  (§5.4), `InitString` blobs. A frame address is valid until that frame returns.

## 5. Frame memory and the shadow stack

Fiber stacks do not move (`01-cbc-platform-facts.md` §7). A local whose size is known
when the frame is built therefore needs no side stack: its address is just an offset
from the frame, and that address is stable until the frame returns. The shadow stack
exists only for allocations whose size is not known then — `alloca` and VLAs — and the
engine owns it completely.

`IR13` is not reserved. It is a callee-saved register (`06-backend.md` §3). Nothing in
the calling convention is a shadow-stack pointer.

### 5.1 Two regions in the frame

| Memory | Size known | Where | Addressed by |
|---|---|---|---|
| Outgoing stack arguments, register spills | yes, in 8-byte slots | untyped slots `0 .. untypedSlotCount-1` | `LoadUntyped` / `StoreUntyped` |
| Static allocas, byval copies, sret buffers, variadic buffers, `va_list` | yes, in bytes | untyped memory block | `lea.frame` + raw load/store |
| Dynamic `alloca`, VLAs | no | shadow stack | `alloca` instruction |

Spills stay in slots. `ld.u` / `st.u` is shorter than materializing an address, and a
spill is never address-taken. Outgoing arguments stay in slots `0 .. k-1` because a
native callee sees those bytes at the host stack pointer (`01-cbc-platform-facts.md`
§3.2).

### 5.2 The untyped memory block

`MethodCode` bakes two fields (E8, `03-cbc-file-format.md` §9.1):

* `untypedMemSize` — byte length of the block, a multiple of 16, `0` when the function
  has no static addressable locals.
* `usesAlloca` — `1` when the method contains `alloca`, `stacksave` or `stackrestore`.

When `untypedMemSize > 0`, the engine places the block immediately after the untyped
slots, at the next 16-byte boundary (`01-cbc-platform-facts.md` §4), and zeroes it with
the rest of the frame. When the size is 0 it inserts neither the block nor that pad, so
the bytes after the slots are laid out exactly as before E8. There is no prologue. A
function that does not use dynamic `alloca` never touches the shadow stack.

`lea.frame Rd, disp` (`02-isa-encoding.md` §4.8) computes `Rd = untypedMemBase + disp`
with `0 ≤ disp < untypedMemSize`. The result may be stored in memory, passed to native
code and kept across calls. It is dangling when the frame returns, which is the C rule.

The compiler lays objects out inside the block the way a normal backend lays out a
stack frame: each object's offset honours its alignment, and the size is rounded up to
16. The block base is 16-byte aligned. A local whose alignment is stricter than 16 is
given `align - 1` bytes of padding and aligned at the use:

```
raw     = lea.frame(offset_of_padding)
aligned = (raw + align - 1) & ~(align - 1)
```

The engine zeroes the whole frame on entry, so a large static array is zeroed on every
call. That is defined (C leaves the bytes uninitialized; zeros are a valid value) and it
is a real cost: a multi-megabyte local is multi-megabyte of fiber stack plus a zero loop.
The untyped-slot limit of 65 535 does not apply to the block. The engine's frame size is
a `u32`; the compiler rejects a function whose total frame does not fit in that. A large
block is real fiber-stack memory. Growth of that stack does not move the frame
(`01-cbc-platform-facts.md` §7).

### 5.3 What the compiler no longer does

* No reserved register, no `addi` of a stack pointer in the prologue or before `ret`.
* No `u_ssp` slot, no reload at a landing pad. A handler frame was not popped, so its
  block and its dynamic allocations are still there. Frames that were popped have
  already been freed by the unwinder (§5.5).
* No mapping of a shadow stack at startup, and no link-time size that has to be a power
  of two.

### 5.4 Dynamic `alloca`: the runtime shadow stack

`alloca Rd, Rsize, align` (E8) asks the engine for `Rsize` bytes, aligned to `align`
(a power of two from 16 to 4096). The engine bump-allocates downward from a per-fiber
cursor and returns the pointer. The cursor is engine-private, stored beside the fiber
control block, not in a CBC register and not in the FCB structure CBC code writes.

The cursor starts empty. The first `alloca` on a fiber maps a segment: a `PROT_NONE`
guard page and 64 KiB of usable bytes (`MAP_PRIVATE | MAP_ANONYMOUS`). Further
allocations that do not fit map a new segment, usable size
`max(2 × previous, request)` rounded up to 64 KiB, and switch the cursor to it. The
previous segment stays mapped. Pointers into it remain valid. A request the engine
cannot satisfy (mmap failure, or a single request above 1 GiB) is a fatal
"shadow stack overflow"; the cursor is unchanged.

`stacksave Rd` returns the current cursor as an integer token (`0` when the cursor is
empty). `stackrestore Rs` frees everything allocated after that token:

* same segment, token at or above the current pointer (the stack grows down): set the
  pointer;
* token in an older segment: unmap every newer segment and set the pointer;
* token equal to the current pointer, or `0` when the cursor is already empty: no-op;
* any other token: fatal.

These three instructions are not state points. Growth uses `mmap`, not the Cangjie
allocator, so it does not enter the GC. `alloca` of size 0 returns the current aligned
pointer and does not move it.

VLAs lower to a `stacksave` at the start of the scope, an `alloca`, and a
`stackrestore` at the end. Function-scoped `alloca` has no `stackrestore` in the
bytecode; §5.5 frees it.

### 5.5 Freeing on return and on unwind

For a method with `usesAlloca = 1`, frame setup (after the frame is zeroed, before the
first bytecode) stores the current shadow cursor into an 8-byte engine-private slot at
the top of the frame. Bytecode cannot address that slot.

* **Normal return.** The return trampoline restores that cursor, then pops the frame.
  Segments allocated during the call are unmapped. The caller's cursor is what it was
  when the callee was entered.
* **Exceptional unwind.** `.Lunwind_top_frame` does the same restore before it pops the
  frame. Each popped frame frees only its own allocations. The handler frame is not
  popped, so its cursor slot is left alone and its `alloca` pointers still work.
* A method with `usesAlloca = 0` does not reserve the slot and its return path does not
  touch the cursor. An `alloca` in such a method is a fatal engine check: the compiler
  always sets the bit if it emits any of the three instructions.

Restoring the entry cursor is idempotent with a `stackrestore` the bytecode already
performed. `setjmp` / `longjmp` is the one case the entry cursor is too old for, because
the setjmp frame is not popped (§5.6).

### 5.6 `stacksave` inside one frame, and `setjmp`

```c
void f(int n) {
  int *a = alloca(n);          // freed by the engine when f returns or is unwound
  for (int i = 0; i < n; i++) {
    char vla[i + 1];           // stacksave / alloca / stackrestore around the iteration
    use(vla);
  }
}
```

`longjmp` lands in the setjmp frame without popping it, so the entry cursor would keep
allocations that happened after `setjmp` in that same frame. `CBCLowerSjLj` therefore
emits `stacksave` at the `setjmp` and stores the token in the `jmp_buf`. The sjlj
landing pad emits `stackrestore` of that token after the unwinder has already restored
every popped frame (`08-exceptions-and-sjlj.md` §8). A function that calls `setjmp`
has `usesAlloca = 1` even when it contains no `alloca`, because it contains
`stacksave` / `stackrestore`.

An ordinary C++ landing pad does not emit `stackrestore`. Cleanups that already had one
in the IR (the end of a VLA scope on the exceptional path) keep it.

### 5.7 Fiber control block

The FCB is no longer implied by a stack pointer. The engine allocates one per fiber at
fiber start (`malloc`, zeroed, never moved) and stores the pointer on the `Ectype`.
`ld.fcb Rd` loads it. Layout and the fields CBC code actually writes are
`08-exceptions-and-sjlj.md` §1. The shadow cursor is a second engine-private field on
the `Ectype`, so a store through the FCB pointer cannot move it.

`__cbc_fcb()` in the crt is `always_inline` and lowers to `ld.fcb`. The entry method
contains a dead `ld.fcb` so an engine without E8 aborts while rewriting the entry,
the same way the dead `ld.fnptr` of `abort` detects a missing E5
(`13-engine-changes.md` §1, §11).

## 6. Global data: the data image

### 6.1 Layout (whole-program compilation)

This section describes the design in scope: the whole program is code-generated at once.
§6.5 sketches the per-object images that the postponed separate compilation would need.

At link time, after LTO, the `CBCLowerGlobals` pass (`07-ir-passes.md` §3) lays out every
remaining global variable of the program — `.data`, `.rodata`, `.bss`, string literals,
C++ vtables and RTTI, emutls control blocks — into one contiguous byte image:

```
image (N bytes, 16-byte aligned, N rounded up to 8)
  [rodata + data with non-zero initializers] [zero-initialized data]
```

Each global `@g` becomes `image + off(g)`. The image is the static field
`$cbc.<prog>:Data.image` of type `FST(VARRAY(N/8, U64))`.

* Address of the image: `LeaStatic Rd, Rscratch, @image` — a rewrite-time constant
  (`01-cbc-platform-facts.md` §8); `Rscratch` is a dead, otherwise unused register. Global `@g + c` = `Rd + off(g) + c`, folded into the
  displacement of raw loads/stores.
* The static bundle is zero-filled by the engine, so `.bss` costs nothing.
* Alignment: the field is the sole static of its type, so it sits at the start of a
  `new char[]` allocation (16-byte aligned). If any global needs alignment > 16, the image
  is instead allocated at startup with `aligned_alloc` and its address stored in a
  primitive static `$cbc.<prog>:Data.base` (one `LdStatic` per function that uses globals,
  CSE'd and rematerializable).

### 6.2 Initial contents

* The initial bytes of the non-zero prefix are a string-pool blob `B`.
* Pointer-valued initializers are **relocations**:

| Relocation | Value at run time | How applied |
|---|---|---|
| `IMAGE_ABS64` (pointer to another global + addend) | `image + target + addend` | blob holds `target + addend`; startup adds `image` to each listed slot |
| `FNPTR64` (pointer to a CBC function, e.g. a vtable entry) | the function's descriptor address | startup code: `ld.fnptr Rx, @method` + `StoreRawMemory` |
| `NATIVE_FNPTR64` (pointer to a native function + addend, e.g. `&strcmp` in a table) | `dlsym(name) + addend` | startup code: `ld.fnptr Rx, @aot(name)` (+ addend) + `StoreRawMemory` |
| `NATIVE_ABS64` (pointer to native data + addend: `&stdout`, `environ`) | `dlsym(name) + addend` | startup code: `LeaStatic` of the AOT static field + `StoreRawMemory` |

* The list of `IMAGE_ABS64` slot offsets is a second blob `R` (`u32` offsets).
* `FNPTR64`, `NATIVE_FNPTR64` and `NATIVE_ABS64` need one instruction with a static
  operand per distinct target, so they are emitted as straight-line code. Each costs one
  RT literal; the generated initializer is split into functions of at most 1 000 such
  relocations so that no method approaches the 4 096-entry literal table (`01` Q3).
* The descriptors of every CBC function referenced from initialized data (all vtable
  entries, for example) are created when these initializer functions are rewritten, i.e.
  at startup. This needs nothing more than memory: there is no budget.
* Startup sequence (generated by the linker, runs in `Entry.main` before constructors):

```
initstr ts0, #B            ; typed slot ts0 : (U64, U64)
ld.stack.rec IRa, ts0
ld.raw.mem.64 IRb, [IRa + 0]       ; StringStorage*
addi.W64 IRb, IRb, 16              ; blob bytes
lea.s IRc, IRs, [@image]          ; IRs: dead scratch
call memcpy(IRc, IRb, |B|)         ; native AOT call
initstr ts1, #R
... __cbc_apply_relocs(IRc, Rbytes, count)   ; CBC loop: *(u64*)(image+o) += image
... per FNPTR64 / NATIVE_FNPTR64: ld.fnptr IRx, @f; st.raw.mem.64 IRx, [IRc + o]
... per NATIVE_ABS64: lea.s IRx, IRs, [@native_sym]; st.raw.mem.64 ...
```

The `ld.stack.rec` result is consumed before `memcpy` is called. A frame address may be
kept across a call (`01-cbc-platform-facts.md` §7); this sequence just does not need to.

### 6.3 Constant data

Constants are part of the image (writable memory). Writes to `const` objects are
undefined behaviour in C, so read-only protection is not needed. String literals are
deduplicated by LLVM's constant merging before layout.

### 6.4 Thread-local variables

`thread_local`/`_Thread_local` use LLVM emulated TLS (`-femulated-tls`, default on for CBC):
each TLS variable becomes an `__emutls_v.*` control block in the image and accesses call
`__emutls_get_address`. The CBC runtime implements `__emutls_get_address` on the
**fiber control block** (`08-exceptions-and-sjlj.md` §1), reached by `ld.fcb` (§5.7), and
`fcb->tls_blocks[index]` is allocated on first use. This is per fiber (correct even if the
engine migrates a fiber between OS threads) and needs no native `pthread_key_create`
destructor, which would be a native→CBC callback. Destructors registered with
`__cxa_thread_atexit` (crt, `10-linker-and-runtime.md` §5.2) run when the fiber's entry
returns. The native `pthread_key_*`/`tss_*` functions are unsupported for the same
reasons (`10-linker-and-runtime.md` §5.5); `thread_local` is the replacement.

### 6.5 Separately compiled programs: one image per object and per COMDAT group

> **Deferred — not in the work scope.** Separate code generation (phase 2) is postponed
> indefinitely (§3). This subsection records the intended data design so that a later
> phase 2 does not have to rediscover it; nothing here is implemented or tested in the
> current plan.

A single program-wide image needs a program-wide layout, which a separately compiled object
(phase 2, `10-linker-and-runtime.md` §4) cannot have. Splitting every global into its own
static was considered and rejected: each distinct global used by a function would cost one
`LeaStatic` and one RT literal (against the 4 096-literal budget), every pointer between
globals would need a run-time relocation, and alignment above 8 would be lost for all but
the first record of a type. The chosen granularity is in between:

* **Own image per object.** All data of a translation unit that is neither in a COMDAT
  group nor over-aligned (> 16) is laid out by the compiler into the object's own image,
  with the same algorithm as §6.1. Inside the object, code reaches this data exactly as in
  phase 1: one base per function (`LeaStatic` of the own image), displacements folded into
  raw loads and stores, offsets final at compile time.
* **One image per COMDAT group.** Vtables, typeinfo objects and names, template static
  data members, `inline` variables and guard variables live in COMDAT groups that several
  objects define. Each group copy has its own image so that the linker can discard
  duplicate copies whole, as an ELF linker discards section groups.
* **One aligned image per object** for data with alignment > 16, in base-pointer mode
  (§6.1). Only such data needs the base-pointer path; the own image stays in direct mode.
* **Final program:** image `k` is the static `$cbc.<prog>:Data.<k>.image` of its own type
  `$cbc.<prog>:Data.<k>` (one type per image, so each gets its own 16-byte-aligned engine
  allocation).
* **References to data outside the own image** — `extern` declarations, weak
  definitions, COMDAT members, even when the referencing object holds a copy — use
  `LeaStatic` on a field reference that the linker binds to "the image that holds the
  prevailing definition of `g`" and a displacement relocated to `off(g)`. If `g` turns out
  to be native data, the same field reference is bound to `g`'s AOT static and the
  displacement to 0: the instruction sequence is identical in both cases, which matters
  because the compiler cannot know in phase 2 whether an `extern` variable is defined by
  another object or by a native library.
* **Restriction:** data with alignment > 16 referenced from another object is a link error
  in phase 2 (its image is in base-pointer mode, which needs a different instruction).
  Phase 1 has no such restriction.
* **Initialization:** the linker concatenates the initial bytes of all images into one
  blob, copies each image's part with `memcpy`, and applies relocations; a pointer from
  image `i` to data in image `j` is a relative relocation with base `j`
  (`10-linker-and-runtime.md` §4.4).
* **Cost relative to phase 1:** one extra `LeaStatic` (and RT literal) per distinct
  external data symbol per function, and one engine allocation per image. Phase 1 stays
  the default and the recommended mode for release builds.

## 7. Function pointers

This section relies on the accepted engine changes E5 (`LoadFuncPtr`) and E6
(`CallIndirect`) with **function descriptors** (`13-engine-changes.md` §6–§7, §9).
Native code calling CBC code is out of scope, so a CBC function pointer never has to be
executable code; it only has to identify the function.

### 7.1 Representations

A function pointer is a 64-bit integer. Every pointee has **exactly one** value for the
whole process, so equality, ordering, casts to `intptr_t` and back, storage in memory, and
`memcpy` of vtables behave as in native code.

| Pointee | Value | Materialized by | Called by |
|---|---|---|---|
| CBC function `f` | the address of `f`'s **descriptor**: a 16-byte record in the engine's descriptor region that holds `f`'s `DynamicFunctionHandle*` | `ld.fnptr Rd, @f` (E5), a rewrite-time constant | `call.indirect` (E6): the engine sees an address inside the descriptor region, loads the handle and performs an ordinary interpreted call (`call.2i` semantics, no native code involved) |
| native function `n` | its real address (`dlsym("n")`) | `ld.fnptr Rd, @aot(n)` (E5) on the same AOT method reference used for calls | `call.indirect`: native call (`call.2c` semantics) |
| null | 0 | `IRZ` | `call.indirect` raises `NoneValueException`; the runtime reports "call through a null function pointer" and aborts (`08-exceptions-and-sjlj.md` §4.3) |

Properties:

* **No budget.** The engine creates a descriptor the first time a method containing an
  `ld.fnptr` for `f` is rewritten and returns the same descriptor for every later request
  (a hash map keyed by the handle). The region is reserved once (address space only) and
  committed page by page, so the number of address-taken functions is limited only by the
  reservation (`13-engine-changes.md` §9: 1 M descriptors for 16 MiB).
* **Disjoint value spaces.** Descriptors live in one engine-reserved, non-executable
  mapping; native function addresses lie in the executable mappings of loaded objects. A
  single range check therefore tells them apart, and they never collide.
* **Even values.** Descriptors are 16-byte aligned, so the Itanium C++ ABI
  member-function-pointer encoding (low bit set = virtual offset) works. Member function
  pointers only ever point to CBC functions.
* **Not callable by native code.** The descriptor region is mapped read/write without
  execute permission. A native call through a CBC function pointer (an unsupported
  callback, §7.3) faults immediately on the non-executable page instead of running
  arbitrary bytes.
* Native function pointers received from native code (`dlsym`, `signal`'s return value,
  fields of native structures) are native addresses and work with no special handling.

### 7.2 Indirect calls

Every indirect call `call %fp(args)` is lowered as an ordinary call with the target in a
register; there is no IR-level rewriting, no dispatcher and no per-signature machinery:

```
  ; arguments in IR1.., FR0.., outgoing slots, as for any call
  call.indirect irN            ; 45 9N — descriptor → interpreted call, native → native call
```

* The call clobbers every volatile register (the target may be a CBC function), so it uses
  the CBC-call register mask even when the pointer happens to be native (§9.6).
* Cost: a CBC target costs a direct `call` plus one range check and one load in the engine
  (descriptor → handle). A native target costs a direct native call.
* When an indirect call site's possible targets are known (whole-program
  devirtualization, `WholeProgramDevirt`, `CalledValuePropagation`), LLVM turns it into a
  direct call first.
* Mismatched calls (a pointer cast to an incompatible function type) behave like native
  indirect calls through a mistyped pointer: the callee runs with whatever is in the
  argument registers. There is no detection; the behaviour is undefined in C anyway.

### 7.3 Native code and function pointers

* Native function pointers flow freely: they can be stored, passed to native code, and
  called from CBC code with `call.indirect`.
* **CBC function pointers passed to native code** — for standalone `.cbc` emit, out of
  scope (non-executable descriptors; link-time **error**). For **native shared libraries**
  (`25b`), every method has an N2C stub at the same address `ld.fnptr` / `call.indirect`
  use, so callbacks are legal and the legalizer does not reject them
  (`07-ir-passes.md` §6; `10-linker-and-runtime.md` §5.5–§5.6). Signal handlers and
  `pthread_atfork` stay unsupported.
* Taking the address of a **native variadic** function is rejected unless the function has
  a `v*` variant (`printf` → `vprintf`, …): CBC-internal indirect variadic calls use the
  buffer convention (§10.1), which a native variadic callee cannot accept. For functions
  with a `v*` variant the compiler substitutes the address of a CBC wrapper that calls the
  variant; the wrapper is an ordinary CBC function with a descriptor
  (`07-ir-passes.md` §4).

## 8. Native interoperability

### 8.1 Calls

A call to a function that is undefined after linking all bitcode is a native call:
`CallDirect IR1, @m` where `@m` is an `AOT` method reference with a direct-call AOT entry
naming the symbol. Arguments follow the host ABI exactly (`01-cbc-platform-facts.md` §3).
The backend uses a separate calling convention, `cbc_nativecc`, whose call-clobbered set
is `{IR1, FR0}` only.

Indirect calls through native function pointers use `call.indirect` (E6), which performs
exactly the same `i2c` call as `call.2c`; the same restriction (single result register)
applies. Because the target of an indirect call may also be a CBC function, such calls use
the full CBC clobber set (§9.6).

### 8.2 Data

`extern` native data (`stdout`, `stderr`, `environ`, `optarg`, `timezone`, `__progname`)
is accessed through `LeaStatic Rd, Rscratch, @f` where `@f` is an AOT static field
reference with the symbol's linkage name; the address is a rewrite-time constant. `errno`
is `*__errno_location()` (a native call) as in glibc headers.

### 8.3 What cannot be called directly, and the remedy

| Native signature property | Problem | Remedy |
|---|---|---|
| `div`, `ldiv`, `lldiv`, `imaxdiv` (`ldiv_t` etc. return in two integer registers) | `rdx`/`x1` not copied back | expanded inline by the compiler (`sdiv` + `srem`, `10-linker-and-runtime.md` §5.4 rule 3) |
| any other function returning in two integer registers (`__int128`, 16-byte structs) | `rdx`/`x1` not copied back | compile-time error |
| returns in two float registers (`_Complex double` functions such as `csqrt`, `cexp`), AArch64 HFA returns (including `_Complex float`) | `xmm1`/`d1..d3` lost | compile-time error (unsupported); `cabs`, `carg` and x86-64 `_Complex float` functions return one register and work |
| variadic with floating-point variadic arguments (`printf("%f")`) | x86-64: the callee reads `al` to decide whether to save vector registers | none needed: with E7 the engine sets `al = 8` before every native call, so the call is a plain native variadic call (§10.2) |
| `long double` in the prototype (`sinl`, `strtold`, `modfl`, …) | different representation | calls renamed to the `double` function (`sinl` → `sin`), exact because CBC `long double` is `double`; without a rename entry: compile-time error. `%L` floating conversions in constant format strings: clang error (`09-clang.md` §2) |
| takes a callback that native code would call (`qsort`, `qsort_r`, `bsearch`, `lfind`/`lsearch`, `tsearch` family, `ftw`/`nftw`, `pthread_create`, `pthread_once`, `pthread_atfork`, `call_once`, `thrd_create`, `pthread_key_*`, `tss_*`, `fopencookie`, `dl_iterate_phdr`, …) | native→CBC | **`.cbc` emit**: link-time error. **Native shared library**: N2C stubs make `qsort` / `pthread_create` / `pthread_once` / CBC fnptrs legal; keep rejecting `pthread_atfork` and signal handlers (`10-linker-and-runtime.md` §5.5) |
| optional callback (`glob` errfunc, `scandir` filter/compar) | same, but only if a CBC function is passed | `.cbc`: error if CBC fnptr; shared library: CBC fnptr OK via N2C; null / native (`alphasort`) fine either way |
| installs a signal handler (`signal`, `sigaction`, `sigset`, `bsd_signal`, `sysv_signal`) | the kernel would call a CBC function asynchronously | crt guards accept only `SIG_DFL`/`SIG_IGN` (`SIG_HOLD` for `sigset`) and fail otherwise with `SIG_ERR`/`EINVAL` (`10-linker-and-runtime.md` §5.2) |
| registers exit handlers (`atexit`, `on_exit`, `at_quick_exit`, `__cxa_atexit`, `__cxa_thread_atexit_impl`) | handlers must run in CBC | defined in CBC by `crt-cbc` and run by the CBC `exit` — the one exception to "callback-taking functions are unsupported", because the caller of the handlers is CBC code |
| any of the rows above reached through a **native function pointer** (`call.indirect`, e.g. `&qsort` stored in a table) | same as for direct calls | the rules apply to address uses as well: taking the address of an unsupported native function is the same link-time error; `&ldiv` and `&printf` get CBC wrappers, `&sinl` is renamed; unknown native pointers obtained at run time (`dlsym`) are the program's responsibility |
| `setjmp`/`longjmp`/`sigsetjmp`/`siglongjmp` | operate on the native stack, not on CBC frames | lowered by the compiler (`08-exceptions-and-sjlj.md` §8); `longjmp` family defined by `crt-cbc`; taking the address of `setjmp` is an error |
| `getcontext`/`setcontext`/`makecontext`/`swapcontext`, `vfork` | operate on the native stack | unsupported (link-time error) |
| `alloca` (libc macro to `__builtin_alloca`) | — | `alloca` instruction; the engine frees it on return and on unwind (§5) |

### 8.4 The libc strategy in one paragraph

Native glibc (or musl/bionic on other hosts) implements the C library: syscalls, stdio
(including the whole `printf`/`scanf` family), strings, memory, math, time, files,
sockets. There is no CBC re-implementation layer. CBC code is defined only for what must be
CBC: the exit-handler family, the signal guards and the `longjmp` family, all in
`crt-cbc` (`10-linker-and-runtime.md` §5.2). Native signature problems are handled by
compile-time rules from one table, `CBCNativeLibc.def`: `long double` renames and inline
expansion of the `div` family. Floating-point variadic calls such as `printf("%f", x)` need
no rule: they are plain native calls because the engine sets `al` (E7). Callback-taking
functions are unsupported and rejected at link time (§8.3). Headers are the host's,
with a few compat wrappers (`09-clang.md` §5.1).

## 9. Calling conventions

### 9.1 `cbccc` — calls between CBC functions

* Integer/pointer arguments: x86-64 flavour `IR1..IR6`; AArch64 flavour `IR1..IR8`.
* Float arguments: `FR0..FR7`.
* Remaining arguments: caller's untyped slots `0, 1, …` (8 bytes each; 16-byte-aligned
  types start at an even slot, as in the host ABI).
* sret pointer: x86-64 `IR1` (shifts the other integer arguments), AArch64 `IR9`.
* Results: `IR1` (and `IR2`), `FR0` (and `FR1`). Mixed `{i64, double}` uses `IR1` + `FR0`,
  like SysV.
* Callee-saved: x86-64 `IR8..IR13`, AArch64 `IR11..IR13`; `FR8..FR15`. Reserved:
  `IRZ`, `IR_ACC` (the engine writes it on exception delivery and, in a callee's prologue,
  when the stack grows — `trampolines.S` `.Lstack_overflow` stores `rsp` there — so it is
  never allocated). Clobbered: everything else, including the tail register.
* Aggregates follow clang's host ABI lowering (coerced to register-sized pieces, `byval`
  or indirect), because the same IR must be correct when the callee turns out to be
  native.

### 9.2 `cbc_nativecc` — calls to native functions

Same argument placement as `cbccc` (it *is* the host ABI). Only one result register of each
file. Clobbers only `IR1` and `FR0`.

### 9.3 Incoming stack arguments

The callee reads stack arguments through the tail register in its entry block
(`LoadTailParam`/raw loads) before the first call, because the tail register is
overwritten on every CBC→CBC call. The caller's frame does not move, so a pointer captured
from the tail register stays valid for the rest of the callee. `byval` aggregates are
copied into the callee's untyped memory block in that same entry block (§5.2).

### 9.4 (removed)

There is no dispatcher calling convention: indirect calls never go through dispatchers
(§7.2). The section number is kept so that references to §9.5 and §9.6 stay valid.

### 9.5 Entry

`$cbc.<prog>:Entry.main() -> I64`: no arguments, result in `IR1` is the exit status.

### 9.6 Indirect calls (`call.indirect`, E6)

* Argument placement: `cbccc` (identical to the host ABI).
* The target address is in any integer register other than `IRZ` and `IR_ACC`; it is
  read before the arguments are loaded.
* Clobbers: the `cbccc` set (all volatile registers, the tail register), because the
  target may be a CBC function. This is conservative for native targets, which clobber only
  `IR1`/`FR0`.
* Results: `IR1`/`IR2`, `FR0`/`FR1` for a CBC target; only `IR1`/`FR0` for a native target.
  A call site whose type returns two registers is therefore correct only for CBC targets.
  The compiler rejects programs in which a native function returning two registers has its
  address taken (except the `div` family, whose address is that of a CBC wrapper,
  `07-ir-passes.md` §4); native pointers obtained at run time (`dlsym`) with such
  signatures are not supported (the same rule as for direct native calls, §8.3).
* Variadic call sites: the CBC buffer convention (§10.1). A native variadic function is
  never the target, because its address can only be taken through a CBC wrapper (§7.3).

## 10. Variadic functions

### 10.1 Calls to CBC-defined variadic functions

`ExpandVariadics` (`llvm/lib/Transforms/IPO/ExpandVariadics.cpp`) runs in lowering mode
for CBC with a CBC `VariadicABIInfo` (`05-llvm-core-changes.md` §7):

* Each defined variadic function `f(fixed..., ...)` becomes `f(fixed..., ptr %va)`.
* Each call site allocates a buffer in its untyped memory block (§5.2) and stores the
  variadic arguments in **host stack-argument layout** (8-byte slots, 16-byte alignment
  for 16-aligned types,
  aggregates inline or by reference exactly as the host ABI passes them on the stack),
  then passes its address.
* `va_start` in the callee initializes a **host-native `va_list`** whose register areas
  are marked exhausted and whose overflow pointer is the buffer:
  * x86-64: `{gp_offset = 48, fp_offset = 176, overflow_arg_area = buf, reg_save_area = buf}`
  * AArch64 Linux: `{__stack = buf, __gr_top = 0, __vr_top = 0, __gr_offs = 0, __vr_offs = 0}`
  * Apple arm64: `va_list = buf`
* Clang's host-ABI `va_arg` code then reads arguments from the overflow area, and the
  `va_list` can be passed to native `vprintf`, `vsnprintf`, `vsyslog`.

### 10.2 Calls to native variadic functions

Plain host ABI, exactly as a native compiler emits them: integer and pointer arguments in
`IR1..IR6` (`rdi..r9`), floating-point arguments in `FR0..FR7` (`xmm0..xmm7`), the rest in
the outgoing untyped slots, which the native callee sees as its stack arguments. This holds
for floating-point variadic arguments too (`printf("%d %f\n", i, d)`):

* The SysV x86-64 ABI requires `al` to hold an upper bound on the number of vector
  registers used by a variadic call. glibc's variadic prologues test `al` to decide whether
  to spill `xmm0..xmm7` into the register save area; with `al = 0` a `double` argument
  would be read from an unsaved area. The engine's `engine_i2c_call` did not set `al`
  (it held the low byte of an adapter address, `01-cbc-platform-facts.md` §3.4).
* **E7** (`13-engine-changes.md` §10) makes `engine_i2c_call` set `al = 8` before every
  native call. 8 is a correct upper bound for every call (it is the number of vector
  argument registers), and non-variadic callees ignore `al`, so the change needs no
  information about the callee.
* AArch64 Linux (follow-up flavour, §2) passes variadic arguments exactly like named ones
  and has no `al` equivalent; native variadic calls are plain calls there as well.

### 10.3 (removed)

The former redirection of floating-point variadic native calls to `v*` variants is not
needed with E7. The `v*` map survives only for the CBC wrappers that stand in for the
*address* of a native variadic function (§7.3, `07-ir-passes.md` §4).

## 11. Atomics

* Sizes 1, 2, 4, 8 bytes, any address: the atomic instructions are given the pointer as
  their "object" and a field reference to `$cbc:A{8,16,32,64}.v`, whose offset is 0
  (`02-isa-encoding.md` §4.11). All operations are `seq_cst`, which satisfies every weaker
  ordering.
* `cmpxchg` (old value + success flag): `CAS` returns only the flag. Lowering:

```
loop:
  ok = CAS p, expected, new
  if ok: return {expected, true}
  cur = AtomicLoad p
  if cur != expected: return {cur, false}
  goto loop              ; value changed back between CAS and load: retry
```

  This is a correct *strong* compare-exchange (linearizable at the successful CAS or at
  the load that observed a different value).
* `atomicrmw` add/sub/and/or/xor/xchg: direct instructions. `nand`, `min`, `max`, `umin`,
  `umax`, `fadd`, `fsub`, `uinc_wrap`, `udec_wrap`: `AtomicExpandPass` CAS loops.
* `fence seq_cst`: `AtomicFetchAdd` of 0 on a per-program dummy word in the image (a
  sequentially consistent RMW is a full barrier on both hosts). Weaker fences: same.
* 16-byte atomics: `__atomic_*` libcalls in CBC compiler-rt (lock-based); `is_lock_free`
  returns false for 16 bytes.

## 12. Exceptions, `setjmp`/`longjmp`

Summary (full design in `08-exceptions-and-sjlj.md`):

* `__cxa_throw` → CBC `_Unwind_RaiseException` stores the exception in a per-fiber slot
  and executes `nullcheck IRZ`, which makes the engine unwind CBC frames.
* Every `invoke` becomes a call covered by an exception region whose target is the
  landing pad. The unwinder has already freed the shadow allocations of every frame it
  popped (§5.5). The landing pad fetches the pending exception from the
  per-fiber state (aborting with a diagnostic if the exception was not raised by the
  runtime, e.g. a `call.indirect` through null) and calls the CBC selector, which
  implements the Itanium matching rules against the landing pad's clause table and either
  returns the selector value or continues unwinding by raising a fresh marker.
* `setjmp`/`longjmp` use the same marker with a different payload.
* Native frames are never between a throw and its handler (natives cannot call CBC).

## 13. Startup and shutdown

`$cbc.<prog>:Entry.main` (generated by `cbc-ld`):

0. The method body contains `ld.fnptr Rx, @aot(abort)` (E5) as its first instruction. It
   creates no descriptor (the target is native) and its result is unused; its purpose is
   that an engine without E5/E6 aborts deterministically while rewriting the entry method,
   before any user code runs (`13-engine-changes.md` §1).
1. Check the engine host flavour (`uname`-based or `sizeof` probe on a native symbol) and
   abort with a message on mismatch.
2. The engine has already allocated this fiber's FCB (§5.7). The entry contains a dead
   `ld.fcb` so an engine without E8 fails while rewriting it.
3. Initialize the data image (§6.2), including the function-pointer relocations
   (`FNPTR64`, `NATIVE_FNPTR64`), which create the descriptors of every CBC function
   referenced from initialized data.
4. Run `llvm.global_ctors` in priority order (C++ static constructors, `__attribute__((constructor))`).
   No libc initialization is needed: native libc initialized itself in the launcher
   process, and the crt's exit-handler lists live zero-initialized in the image.
5. Build `argc`/`argv`/`envp` (`10-linker-and-runtime.md` §6.3).
6. Call the C `main` (renamed `__cbc_main` so the entry type keeps a unique `main`).
7. Run the crt's `__cbc_exit_handlers()`: CBC `atexit`/`__cxa_atexit` handlers in reverse
   order, `llvm.global_dtors`, `fflush(NULL)`, then return `status` to the engine
   (returning lets the launcher shut down the Cangjie runtime cleanly). An `exit()` called
   from user code is the crt's `exit`, which runs the same handlers and then calls native
   `exit`.
8. An exception escaping `__cbc_main` is caught by the entry method and turned into
   `std::terminate()` semantics (message + `abort()`).

## 14. Threads and signals

* CBC code runs on the single fiber the launcher created. `pthread_create`, `thrd_create`,
  `pthread_once`, `call_once`, `pthread_atfork` and the `pthread_key_*`/`tss_*` functions
  are unsupported: a reference to them is a link-time error
  (`10-linker-and-runtime.md` §5.5). Mutexes, condition variables and other
  synchronization objects are native and work trivially with one fiber.
* Not in scope (unverified): implementing `pthread_create` on the engine's `Spawn`
  instruction, which starts a Cangjie fiber running a closure (it would require emitting a
  `LAMBDA` type with exactly two virtual methods and referencing `std.core:Future` as an AOT
  type). Each new fiber would receive its own FCB and an empty shadow cursor from the
  engine (§5.7). The
  runtime pieces that would already be fiber-safe are atomics, the fiber control block and
  emulated TLS.
* Signal handlers cannot be CBC functions. The crt guards for `signal`, `sigaction`,
  `sigset`, `bsd_signal` and `sysv_signal` forward `SIG_DFL`/`SIG_IGN` to native libc and
  fail for any other handler (`SIG_ERR` or −1 with `errno = EINVAL`); the compiler warns at
  the call site (`10-linker-and-runtime.md` §5.2, §5.6). Signals with default actions
  (terminate, core dump) and ignored signals behave natively.

## 15. Floating point

* `float` and `double`: IEEE, round-to-nearest, no exceptions or rounding-mode control
  (`fenv.h` functions operate on native state the interpreter does not use; constrained FP
  intrinsics are lowered as non-strict).
* `long double` = `double` (64 bits, `__LDBL_MANT_DIG__ = 53`). This diverges from the
  host ABI; calls to native functions with `long double` in their prototype are renamed
  to the `double` functions (`sinl` → `sin`), or rejected when no such function exists
  (§8.3).
* `_Float16`/`__fp16`: storage only, arithmetic promoted to `float` via compiler-rt
  conversion routines (`softPromoteHalf`).
* `__float128`/`_Float128`: unsupported (error).
* `sin`, `cos`, `pow`, `sqrt`, `fabs`, negation: native instructions. Everything else in
  `<math.h>`: native libm calls (non-variadic, single-register results) or LLVM's generic
  expansions (`copysign` via bit operations, `fmin`/`fmax` via compares).

## 16. Feature matrix

| Feature | Status | Mechanism |
|---|---|---|
| C17 core language | supported | — |
| VLAs, `alloca` | supported | `alloca` / `stacksave` / `stackrestore`; the engine frees on return and unwind (§5) |
| `_Atomic`, `<stdatomic.h>`, C++ `<atomic>` | supported (≤ 8 bytes lock-free) | §11 |
| `_Thread_local`, `thread_local` | supported | emulated TLS |
| `<threads.h>`, `std::thread` | unsupported (single fiber) | §14 |
| `setjmp`/`longjmp` | supported | §12 |
| `signal` handlers | unsupported (`SIG_DFL`/`SIG_IGN` work) | §14 |
| callback-taking libc functions (`qsort`, `bsearch`, `tsearch`, `ftw`/`nftw`, `pthread_once`, `pthread_key_*`, …) | `.cbc`: unsupported (link-time error); native shared library: OK via N2C (not `pthread_atfork` / signal) | §8.3, `10-linker-and-runtime.md` §5.5 |
| `atexit`, `__cxa_atexit`, C++ static destructors | supported | crt (§8.3) |
| `<complex.h>` functions returning `_Complex double` | unsupported (compile-time error); complex arithmetic itself works (compiler-rt `__muldc3`/`__divdc3`) | §8.3 |
| `long double` library functions | supported as `double` (`sinl` → `sin`) | §15 |
| separately compiled programs (phase 2) | postponed indefinitely (design sketch only) | §3, §6.5, `10-linker-and-runtime.md` §4 |
| AArch64 Linux flavour | follow-up (designed, not in the current work scope) | §2 |
| C++ exceptions, RTTI, `dynamic_cast` | supported | §12; RTTI objects in the image |
| C++ virtual calls, member function pointers | supported, any number of virtual functions | §7 (descriptors + `call.indirect`) |
| function pointers: casts, comparison, storage, pointers to native functions | supported | §7.1 |
| `printf` and other native variadic functions with `double` arguments | supported, plain native calls | §10.2 (E7) |
| CBC function pointers called by native code (callbacks) | `.cbc`: unsupported (non-executable descriptors); native shared library: N2C stubs | §7.3, `25b` |
| call through a null function pointer | reported and aborted (`NoneValueException` from `call.indirect`) | §7.1 |
| `std::function`, lambdas | supported | plain C++ |
| Coroutines (C++20) | supported in principle (LLVM lowers them to ordinary functions + heap frames) | — |
| `__int128` | supported in CBC code; not across native calls returning it | pairs of registers, compiler-rt division |
| Vector extensions | supported, scalarized | — |
| Inline asm, SIMD intrinsics | unsupported | — |
| Debug info | file/method names only | — |
| Sanitizers | `-fsanitize=undefined` subset (traps via `abort`), `integer-divide-by-zero` via `DivCheck` | — |
| Profiling/coverage | `-fprofile-instr-generate` counters work (counters live in the image); writing the profile at exit uses the crt's `atexit` | — |

## 17. Performance model

* Cost ≈ number of RT instructions executed. Interpretation is roughly 10–40× slower than
  native code; calls are expensive (frame zeroing, non-volatile save/restore by the
  engine, lazy rewrite on first call).
* Consequences for tuning (`06-backend.md` §12): aggressive inlining, no vectorization,
  prefer immediates that fit 12 signed bits (inline RT encoding), keep frames small
  (the engine zeroes the whole frame, including the untyped memory block),
  devirtualize where possible, hoist the data-image base.
* Indirect calls: a call to a CBC function costs the same as a direct call plus one range
  check and one load in the engine (descriptor → handle). There is no slower path for
  programs with many address-taken functions.
* libc calls are native calls: `printf("%d %f", i, d)` is one native call with the
  arguments in `rdi`/`rsi`/`xmm0`; there is no CBC front-end and no `va_list` construction
  in between.
