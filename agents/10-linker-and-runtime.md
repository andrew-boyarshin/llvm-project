# 10 — Link Step and Runtime Libraries

## 1. Artifacts

| Artifact | Form | Contents |
|---|---|---|
| `cbc-ld` (`ld.lld -flavor cbc`) | host executable | bitcode linker + whole-program CBC code generation |
| `crt-cbc.bc` | bitcode | entry helpers, image init, `argv`, fiber control block accessors, emulated TLS, `__cbc_raise`, the exit-handler family, the signal guards, the `longjmp` family, and the `_Unwind_*` / landing-pad helpers (`08-exceptions-and-sjlj.md`, `14-libcxx.md` §1.1, §7). The engine allocates the FCB and owns the shadow stack (E8). One module, not an archive |
| `cbc_can_catch_stub.bc` | bitcode | non-C++ definition of `__cbc_can_catch` / `__cbc_matches_filter`; omitted when `-lc++abi` is linked (`14-libcxx.md` §1.1) |
| `libclang_rt.builtins.a` (CBC) | bitcode archive | compiler-rt generic C builtins: `i128` arithmetic, `half` conversions, bit counts, `__atomic_*` libcalls |
| `libc++.a`, `libc++abi.a` (CBC) | bitcode archives | C++ standard library |
| `cbc-run` | script/executable | launcher wrapper (part of the engine distribution) |

There is **no libc overlay archive** and **no `libcbcunwind`**: native libc implements
every C library function the program calls, except the few that `crt-cbc.bc` must define
(§5.2). The Itanium entry points are in that same module (`14-libcxx.md` §1.1).
Functions that cannot work natively are either transformed by the compiler (§5.4) or
unsupported and rejected at link time (§5.5).

All CBC libraries are shipped as **bitcode** in phase 1 because code generation happens
once, at link time.

## 2. `cbc-ld`, phase 1 (whole program)

### 2.1 Where it lives

`lld/CBC/` as a new lld flavour (precedent: `lld/wasm`, which also links bitcode and emits
a non-ELF format). Reuse: archive handling, `--whole-archive`, `-u`, `--gc-sections`
semantics, symbol resolution, `lld::lto` wrappers, diagnostics, response files. A minimal
alternative for bring-up is a standalone `llvm/tools/cbc-ld` built on `llvm::lto::LTO`.

### 2.2 Pipeline

1. **Input loading.** `.o`/`.bc` bitcode objects and bitcode archives (lazy members).
   Anything that is not bitcode for the same CBC triple is an error (mixing flavours is
   rejected by `Triple::isCompatibleWith`).
2. **Symbol resolution.** Standard ELF-like rules (strong/weak/common, archives pulled on
   demand, `-u`). Unresolved symbols after archives are **native** symbols — not errors yet.
3. **Libcall preservation.** Mark as used every runtime libcall name defined in the inputs
   (`lto::LTO::getRuntimeLibcallSymbols(Triple)`, as `lld/ELF` does), so `__divti3`,
   `__cxa_atexit`, etc. survive internalization; instruction selection may introduce
   calls to them after LTO.
4. **Native validation.** For each native symbol, search the native libraries
   (`--native-lib=` list plus defaults `libc.so.6`, `libm.so.6`, `libpthread.so.0`,
   `libdl.so.2`, resolved via `--native-search-path`) using `llvm::object` to read dynamic
   symbol tables. Unknown → `error: undefined symbol 'foo' (not found in CBC inputs or in
   native libraries)`. This replaces a run-time `FATAL` in the engine's rewriter.
   Unsupported libc functions (§5.5) are **not** rejected here: a reference from code that
   LTO later removes as dead must not fail the link. They are rejected after LTO by
   `CBCNativeCallLegalizer` (`07-ir-passes.md` §6), which sees only live references.
5. **LTO.** One regular-LTO partition (`Config.ThinLTO*` unused, `CGOptLevel` from `-O`),
   internalize everything except the entry, run the full LTO pipeline; the CBC target's
   pass-builder callbacks append the whole-program passes of `07-ir-passes.md` at the end.
6. **Code generation** with `CodeGenFileType::ObjectFile`: the CBC object format writes
   the `.cbc` directly (`05-llvm-core-changes.md` §4). `aotDeps` comes from the native
   libraries that are not already provided by the launcher process.
7. **Limits check** (`01-cbc-platform-facts.md` §12): reference tables are bounded by
   2^32 entries with E3, so no reference-count check is needed, and function pointers have
   no budget (E5 descriptors, `13-engine-changes.md` §9). The check that remains: every
   method has ≤ 65 535 untyped slots and ≤ 4 096 RT literals by the backend's conservative
   estimate (`06-backend.md` §11); violations are internal errors with the method name.

### 2.3 Options

| Option | Meaning |
|---|---|
| `-o a.cbc` | output |
| `--cbc-program-name=<id>` | program identifier used in type names (default: output stem + 8 hex digits of a hash of the input set) |
| `--cbc-loop-safepoints=0\|1` | `CBCInsertSafepoints` |
| `--native-lib=<name or path>`, `--native-search-path=<dir>` | validation and `aotDeps` |
| `--no-native-validation` | skip §2.2 step 4 |
| `--cbc-split=<n>` | emit `n` partition files plus the entry file |
| `--cbc-emit-asm` | write CBC assembly instead of a binary (debugging) |
| `--Map=<file>` | method list, image layout, the list of address-taken functions (each gets an engine descriptor when the first method referencing it with `ld.fnptr` is rewritten), native symbols and the libraries they were found in |

### 2.4 Splitting a program across several `.cbc` files (optional)

With E3 the per-file reference tables no longer limit program size, so splitting is not
required for correctness. It remains available for build-system reasons (shipping a
rarely changing part separately, smaller files to inspect). The engine resolves type names
across all loaded files, so a program can be split:

* the entry file holds `Entry` and `Data` (the image);
* partition files hold `Prog.<k>` types with subsets of the functions (call-graph
  clustering to minimize cross-partition references);
* method references to functions in other partitions name the other partition's type;
  the data image static is reached from every file by a field reference to
  `$cbc.<p>:Data.image` (resolved by name).

Each file has its own reference tables. All files are given to the launcher with
`--cbc-path`. Function pointers stay consistent across partitions: a function referenced
by `ld.fnptr` from two partitions gets one descriptor, because both references resolve to
the method's single `DynamicFunctionHandle` and the descriptor map is keyed by the handle
(`13-engine-changes.md` §9).

## 3. Why validation at link time matters

The engine resolves references lazily when a method is first prepared and treats any
failure as fatal for the process. A misspelled native symbol in a rarely executed function
would otherwise crash the program long after start-up. `cbc-ld` therefore guarantees that
every method reference (CBC or native) resolves, and checks the structural rules the engine
does not verify (`cbc-engine-initial-stage/agents/cbc_verification.md` L1/L2): signature
terms match definitions, liveness entries exist for all state points, exception region
bounds are boundaries, every `RECORD` typed slot has a flat size, and every `ld.fnptr`
references either a CBC method defined in the program or an AOT symbol found in the native
libraries. (Untyped-slot stores at index ≥ 512 are valid with E1 and are no longer
checked.)

## 4. Phase 2: separate code generation (postponed indefinitely)

> **Deferred — not in the work scope.** Every build in the current plan is a whole-program
> bitcode link (§2). This section is kept as a design sketch so that a future phase 2 does
> not start from scratch; nothing in it is implemented, tested or scheduled
> (`11-testing-and-roadmap.md` §3 lists it under "deferred"). The cost of postponing it:
> every link runs LTO over the whole program, including libc++ when used.

Goal: compile each translation unit to native CBC code (`-c` without LTO), link quickly,
and ship non-bitcode CBC libraries.

### 4.1 Object format

ELF relocatable objects with `e_machine = EM_CBC` (an unofficial number reserved locally)
and CBC sections:

| Section | Content |
|---|---|
| `.cbc.code.<sym>` | method bytecode with padded LEB operands for relocatable fields |
| `.cbc.meth.<sym>` | method header: frame info, masks, typed slots, signature term (as a string), EH regions and liveness positions as section-relative offsets |
| `.cbc.image.self` | the object's **own image**: initialized bytes of all non-COMDAT, non-over-aligned data of the translation unit, laid out by the compiler (`07-ir-passes.md` §3.6); size, alignment (≤ 16) and the zero-initialized tail length in the section header |
| `.cbc.image.aligned` | the object's over-aligned data (alignment > 16), laid out the same way; absent when there is none |
| `.cbc.image.g.<group>` | one image per COMDAT group that contains data (vtables, typeinfo objects and names, template static data members, `inline` variables, guard variables), in an ELF `SHT_GROUP` with the group's signature |
| `.rela.cbc.image.*` | data relocations (`R_CBC_ABS64`) inside the images |
| `.symtab` | data symbols are defined **relative to their image section** (`st_shndx` = the image, `st_value` = offset in it); function symbols relative to their `.cbc.code` section |
| `.cbc.fninfo` | per function: escapes? signature class string |
| `.cbc.refs` | method/field reference descriptors (name, owner kind, signature string, AOT flag) |
| `.cbc.notes` | per call site whose legality depends on whether the callee ends up native (§4.5): callee symbol, argument facts, source location |

### 4.2 Data images in phase 2

Phase 1 has exactly one data image for the whole program (`04-architecture.md` §6.1). A
separately compiled object cannot use a program-wide layout, so in phase 2 **every image
section of every object becomes one image of the program**:

* image `k` of the program is the static field `$cbc.<prog>:Data.<k>.image` of the type
  `$cbc.<prog>:Data.<k>`, of type `FST(VARRAY(size/8, U64))`; `k` is assigned by the
  linker in input order (own images, then aligned images, then the surviving copy of each
  COMDAT group in the order the groups are first seen);
* one type per image, because `StaticsManager` allocates one block per type
  (`01-cbc-platform-facts.md` §8): each image gets its own zero-filled, never-moving
  allocation, aligned to 16, created when the first method referencing it is rewritten;
* an over-aligned image (`.cbc.image.aligned`) uses base-pointer mode exactly as in phase 1:
  it is `aligned_alloc`ed at startup and its address is stored in the primitive static
  `$cbc.<prog>:Data.<k>.base`;
* COMDAT deduplication drops the entire image of every discarded group copy (that is why
  each group has its own image); a weak non-COMDAT definition that is overridden by a
  strong one stays in its own image as unused bytes;
* common symbols are rejected (`-fno-common` is the CBC default, `09-clang.md` §4.3).

How code addresses data:

| Reference from code to data symbol `g` | Instruction sequence | Relocations |
|---|---|---|
| `g` defined in this object's own image (or aligned image), not weak, not in a COMDAT group | `lea.s Rb, Rdead, @image(self)` (or `ld.static Rb, @base(aligned)`), displacement `off(g) + c` known to the compiler | `R_CBC_FREF_ULEB21` against the object's image section symbol |
| anything else: `extern` declaration, weak definition, COMDAT member (even if defined in this object) | `lea.s Rb, Rdead, @imageof(g)`, displacement `off(g) + c` | `R_CBC_FREF_ULEB21` with the `IMAGEOF` variant against `g`, and `R_CBC_IMAGE_DISP` against `g` with addend `c` |

The `IMAGEOF` pair is resolved by the linker after symbol resolution and COMDAT selection:

* `g` resolves to CBC data in image `k` (direct mode): the field reference becomes
  `$cbc.<prog>:Data.<k>.image` and the displacement `off(g) + c`;
* `g` resolves to **native data** (`stdout`, `environ`): the field reference becomes the AOT
  static-field reference of `g` (the same reference `LEA_NATIVE` produces in phase 1) and
  the displacement is `c`. Both `LeaStatic` variants yield "the address of a static
  location", so the instruction sequence does not depend on whether `g` is native, which
  the compiler cannot know in phase 2;
* `g` resolves to data in an image in base-pointer mode: `error: 'g' requires alignment
  <a> > 16 and is referenced from another translation unit; this is not supported with
  separate code generation — link with LTO`. The compiler keeps over-aligned data out of
  the own image precisely so that only references to these few symbols are affected;
* `g` resolves to a **function**: a data-style reference to a function symbol cannot occur
  (function addresses are `ld.fnptr`, §4.3); the linker reports an internal error.

Each distinct `@image(...)`/`@imageof(g)` used in a method costs one `lea.s` and one RT
literal (rematerializable, `06-backend.md` §4.3). Code that touches many distinct external
symbols therefore pays more than in phase 1, where one base serves all globals.

### 4.3 Relocations

| Relocation | Field | Linker action |
|---|---|---|
| `R_CBC_MREF_ULEB21` | 3-byte padded ULEB in `call` | patch with the final method-ref index |
| `R_CBC_FREF_ULEB21` | 3-byte padded ULEB in `lea.s`/`ld.static`/atomics | final field-ref index: of the named image section, or (`IMAGEOF` variant) of the image or native static holding the resolved symbol (§4.2) |
| `R_CBC_STR_ULEB28` | 4-byte padded ULEB in `initstr` | final string-pool offset |
| `R_CBC_IMAGE_DISP` | 4-byte padded SLEB high part (+ low nibble) of a raw load/store displacement or of a `BinaryImm64` immediate (`addi` when the address is materialized rather than folded) | `off(sym) + addend` within the symbol's image; `addend` for native data |
| `R_CBC_MREF_ULEB21` on `ld.fnptr` | 3-byte padded ULEB in `ld.fnptr` | final method-ref index (the same relocation as for `call`; a function address in code is just a method reference) |
| `R_CBC_ABS64` (data, inside an image) | 8 bytes | becomes an image relocation (§4.4), chosen by the kind of the target symbol |

Function pointers need nothing phase-specific: objects use `ld.fnptr` for function
addresses and `call.indirect` for indirect calls, exactly like whole-program code, because
a function's pointer is its engine descriptor regardless of the rest of the program.

The engine's LEB decoder accepts non-minimal encodings (it simply reads continuation bits),
so padded LEBs are valid in the final file and the linker never re-encodes or resizes code
(the WebAssembly object format uses the same technique: `R_WASM_*_LEB` with 5-byte padding).
Branch displacements are intra-method and already final in the object.

The `lo4` nibble of a displacement shares a byte with the load/store kind (or the left
operand of `addi`), so `R_CBC_IMAGE_DISP` patches only the low nibble of that byte and the
padded SLEB that follows (`02-isa-encoding.md` §1). Images are smaller than 2^31 bytes, so
the padded 4-byte SLEB always suffices.

### 4.4 What the linker synthesizes

The entry method (including the `ld.fnptr @native(abort)` engine probe) and image
initialization, generated directly as CBC bytecode from templates. This is the work done
by IR passes in phase 1, moved into `lld/CBC`.

Image initialization in phase 2, `__cbc_image_init` and its chunks:

1. **One blob for all images.** The initialized bytes of every surviving image are
   concatenated (each padded to 8) into a single string-pool blob `B`; one `InitString`
   gives its address. For each image `k` with initialized bytes: `lea.s` (or `ld.static`
   of the base for an aligned image), then a native `memcpy(image_k, B + start_k,
   len_k)`. Images whose initialized part is smaller than 64 bytes get constant stores
   instead of a `memcpy`.
2. **Relocations**, after all copies (every image already holds its blob bytes, which
   contain `off(target) + addend` or the addend). For an `R_CBC_ABS64` at offset `o` of
   image `i` against symbol `S` + `a`:

   | `S` resolves to | Image relocation | Applied by |
   |---|---|---|
   | CBC data in image `j` | `IMAGE_ABS64(i, o → j)` | relative lists, one per pair `(i, j)`: `__cbc_apply_image_relocs(image_i, offsets, n, base_j)` adds `base_j` to every listed slot. Phase 1 is the special case `i = j`. |
   | CBC function | `FNPTR64` | straight-line `ld.fnptr Rx, @S` + `st.raw.64 Rx, [image_i + o]` |
   | native function | `NATIVE_FNPTR64` | straight-line `ld.fnptr Rx, @aot(S)` (+ `addi a`) + store |
   | native data | `NATIVE_ABS64` | straight-line `lea.s Rx, Rdead, @aot(S)` (+ `addi a`) + store |

   The relative lists of all pairs are one blob `R` of `(u32 offset)` runs. Straight-line
   relocations are split into functions `__cbc_image_init.<n>` of at most 1 000
   relocations each (literal budget, `07-ir-passes.md` §3.5).
3. **Base-pointer images** are allocated (`aligned_alloc`, zero-filled) and their base
   statics written before step 1.

The relocation targets may live in any image because every image's address is a
rewrite-time constant available through its own field reference; there is no ordering
constraint between images.

### 4.5 Native-library rules in phase 2

In phase 1 `CBCNativeCallLegalizer` knows which callees are native. A separately compiled
object does not: an undefined function may be defined by another object or be native.
The rules of §5.4 and §5.5 are therefore split between compiler and linker:

* **Transformations** (`long double` renames, `div` family expansion, address wrappers,
  §5.4) are applied by the compiler to calls of **declarations** whose names are C library
  names in `CBCNativeLibc.def`. These names are reserved identifiers (C17 7.1.3), so a
  program cannot portably define them itself. If the linker finds a CBC **definition** of a
  name that the compiler transformed calls to (it is listed in `.cbc.notes`), it reports
  `error: 'ldiv' is a C library function and was compiled as the native one; defining it
  in CBC code requires LTO`.
* **Unsupported functions** (§5.5) are checked by the linker after `--gc-sections`: every
  remaining reference to a symbol of the unsupported list that resolves to a native
  library is an error, with the referencing function and the source location from
  `.cbc.notes`. A CBC definition of such a name (the program brings its own `qsort`) is an
  ordinary CBC function and is accepted.
* **Function pointers passed to native code** (§5.6): the compiler records in `.cbc.notes`
  every call to a declaration that passes a function-pointer-typed argument, classified as
  "the address of function symbol `f`" / "null" / "unknown". Once the callee resolves to a
  native symbol, the linker reports an error for the first kind when `f` resolves to a CBC
  function (nothing when `f` is native), nothing for null, and a warning for unknown.

## 5. The C library: native libc, a minimal runtime, and compiler rules

### 5.1 Principle

* **Native libc implements the C library.** Every libc function the program calls is the
  host's native function, reached by an AOT direct call (`04-architecture.md` §8.1). There
  is no overlay archive that shadows libc with CBC re-implementations.
* **Callback-taking libc functions are unsupported.** Native code calling CBC code is out of
  scope (`13-engine-changes.md` §8), and the toolchain does not re-implement such functions
  in CBC. They are rejected at link time (§5.5) instead of failing at run time. A program
  that needs one brings its own C implementation (for example musl's `qsort.c`); it is
  then ordinary CBC code.
* **Exactly one exception**: the exit-handler family (`exit`, `atexit`, `__cxa_atexit`, …).
  Its handlers are called by CBC code (the CBC `exit`), never by native code, so it does not
  need native→CBC calls; and C++ static and `thread_local` destructors, which clang
  registers with `__cxa_atexit`/`__cxa_thread_atexit`, cannot work without it. It lives in
  `crt-cbc.bc` (§5.2).
* **Other native signature problems** (two-register results, `long double`, floating-point
  variadic arguments) are solved at compile time by the rules of §5.4, or rejected.
* **One table drives everything**: `llvm/lib/Target/CBC/CBCNativeLibc.def`, read by
  `CBCNativeCallLegalizer` (phase 1, `07-ir-passes.md` §6), by the phase-2 compiler and
  linker (§4.5), and by `TargetLibraryInfo` (`05-llvm-core-changes.md` §10).

### 5.2 What the CBC runtime libraries define

The only libc-named symbols with CBC definitions:

| Symbols | Library | Behaviour |
|---|---|---|
| `exit`, `atexit`, `at_quick_exit`, `quick_exit`, `on_exit`, `__cxa_atexit`, `__cxa_finalize`, `__cxa_thread_atexit`, `__cxa_thread_atexit_impl`, `__dso_handle` (data) | `crt-cbc.bc` | handler lists in the image (fixed table of 64 entries, then `malloc`-grown); `exit(s)` runs `__cbc_exit_handlers()` (§6.1) and then native `exit(s)`; `quick_exit` runs the `at_quick_exit` list and then native `_exit`; `__cxa_thread_atexit*` record per-fiber destructors in the FCB, run when the fiber's entry returns or `exit` is called; `__cxa_finalize(dso)` runs the handlers registered with that `dso` (only `&__dso_handle` exists) |
| `signal`, `bsd_signal`, `sysv_signal`, `__sysv_signal`, `sigset`, `sigaction` | `crt-cbc.bc` | **guards**, not implementations: if the new handler value is `SIG_DFL` or `SIG_IGN` (for `sigset` also `SIG_HOLD`), forward to the native function unchanged; any other value (which can only be a CBC function pointer, or a native handler being re-installed) fails: `signal`-style functions return `SIG_ERR` with `errno = EINVAL`, `sigaction` returns −1 with `errno = EINVAL`. For `sigaction` the checked value is the `sa_handler`/`sa_sigaction` union of `*act` (when `act != NULL`), independent of `SA_SIGINFO`. Querying (`act == NULL`) always forwards. No message is printed at run time; the compile-time warning is in §5.6 |
| `longjmp`, `_longjmp`, `siglongjmp`, `__longjmp_chk` | `crt-cbc.bc` | the marker-exception implementation (`08-exceptions-and-sjlj.md` §8.4); `siglongjmp` restores the signal mask natively first |
| `setjmp`, `_setjmp`, `sigsetjmp`, `__sigsetjmp` | **none** | direct calls are lowered by `CBCLowerSjLj`; taking the address of one is a compile-time error (`08-exceptions-and-sjlj.md` §8.5) |
| `__cbc_*` helpers (`__cbc_fcb`, `__cbc_args`, `__cbc_apply_image_relocs`, `__cbc_raise`, …), `__emutls_get_address`, `__cxa_get_globals` | `crt-cbc.bc`, except `__cxa_get_globals` and `__cbc_can_catch` which are libc++abi | runtime internals, not libc entry points |

Why the guards are needed although handler installation is unsupported: the handler often
reaches `sigaction` inside a `struct sigaction`, where no compile-time rule can see it, and
`signal(SIGPIPE, SIG_IGN)` / `sigaction` with `SIG_IGN` are common and must keep working.
Without the guard, a descriptor address would be installed as a real signal handler and
the process would crash (on the non-executable descriptor page) as soon as the signal
arrives.

Known limitation of the guards: a program that saves a *native* handler installed by a
native library (`old = signal(SIGINT, SIG_IGN)`) and re-installs it (`signal(SIGINT, old)`)
gets `SIG_ERR`, because the guard accepts only `SIG_DFL`/`SIG_IGN`. The native handler then
stays replaced by `SIG_IGN`. (C code cannot ask the engine whether a value is a descriptor,
so the guard does not try to tell native handlers apart.)

### 5.3 Fiber control block and TLS

* `__cbc_fcb()` is `always_inline` and returns `llvm.cbc.fcb()`, which is `ld.fcb`
  (`04-architecture.md` §5.7). The engine allocated the block when the fiber started.
* `__emutls_get_address(struct __emutls_control *c)`: index lazily assigned with an atomic
  counter; `fcb->tls_blocks` grown with `realloc`; the block is `aligned_alloc(c->align,
  c->size)` initialized from `c->value` or zeroed.
* `__cxa_get_globals()` → `&__cbc_fcb()->cxa_globals`.

### 5.4 Compile-time rules for native libc calls (`CBCNativeLibc.def`)

The table has one macro per rule kind:

```cpp
// llvm/lib/Target/CBC/CBCNativeLibc.def
CBC_LIBC_RENAME(sinl, sin)                 // long double = double on CBC
CBC_LIBC_RENAME(strtold, strtod)
CBC_LIBC_EXPAND_DIV(ldiv, 64)              // two-register result, expanded inline
CBC_LIBC_VARIANT(printf, vprintf, 1)       // only for &printf: the CBC wrapper calls vprintf
CBC_LIBC_GUARDED(signal, 1)                // defined by crt as a guard; handler = arg 1
CBC_LIBC_CRT(atexit)                       // defined by crt-cbc
CBC_LIBC_UNSUPPORTED(qsort, Callback, "comparator is called by native code")
```

`CBCNativeCallLegalizer` applies the rules to every call to, and every address use of, a
**native** function (phase 1: a declaration left after linking; phase 2: §4.5), in this
order:

1. **Unsupported** (§5.5): error, with the referencing function and the debug location of
   the use if the input has any (`-g`, `-gline-tables-only`, `-gmlt`; the engine has no
   line tables, but the LLVM metadata survives until this pass).
2. **`long double` renames.** Clang marks every function declaration whose prototype
   contains `long double`, `long double *` or `_Complex long double` with the function
   attribute `"cbc-long-double"` (`09-clang.md` §3.4). For such a native callee:
   * if the table has `CBC_LIBC_RENAME(name, target)`, the call (or address use) is
     redirected to `target`. This is exact because on CBC `long double` *is* `double`:
     the IR types already match the `double` function, only the native symbol differs;
   * otherwise: error `native function 'foo' has 'long double' in its prototype; on CBC
     long double is double and does not match the native ABI`.

   The rename list is generated from the glibc symbol lists: every `<f>l` in libm whose
   `double` counterpart `<f>` has the same parameter structure (`acosl` … `y1l`,
   `frexpl → frexp`, `ldexpl → ldexp`, `modfl → modf`, `remquol → remquo`,
   `sincosl → sincos`, `lgammal_r → lgamma_r`, `nanl → nan`, `nexttowardl → nextafter`,
   `nexttoward → nextafter`), plus `strtold → strtod`, `wcstold → wcstod`,
   `strtold_l → strtod_l`, `wcstold_l → wcstod_l`, `strfroml → strfromd`,
   `qecvt → ecvt`, `qfcvt → fcvt`, `qgcvt → gcvt`, `qecvt_r → ecvt_r`,
   `qfcvt_r → fcvt_r`, and the classification helpers glibc macros may call
   (`__finitel → __finite`, `__isinfl → __isinf`, `__isnanl → __isnan`,
   `__signbitl → __signbit`, `__fpclassifyl → __fpclassify`,
   `__issignalingl → __issignaling`, `__iseqsigl → __iseqsig`). `nexttowardf(float,
   long double)` has no exact `double` counterpart and is left to the error.
3. **`div` family expansion.** `div`, `ldiv`, `lldiv`, `imaxdiv` (and only when the
   declaration has the expected prototype: two integer parameters of the right width) are
   replaced inline: `q = sdiv a, b`, `r = srem a, b`; the result is built by storing
   `{q, r}` into a temporary of the C struct layout and loading the call's IR return type
   from it (whatever coercion clang chose: `i64` for `div_t` on both hosts, `{i64, i64}`
   on x86-64 and `[2 x i64]` on AArch64 for `ldiv_t`). SROA removes the temporary.
   Division by zero and `INT_MIN / -1` are undefined in C, as natively. `ldiv`, `lldiv`
   and `imaxdiv` return two registers and could not be called natively; `div` could, but
   the inline form is cheaper than any call.
4. **Two-register results** left after rules 2 and 3 (for example `_Complex double`
   functions such as `csqrt`, `cexp`, `cpow`; AArch64 `_Complex float` functions, which
   return an HFA in `s0`/`s1`; any third-party function returning a 16-byte struct in
   registers): error `native function 'csqrt' returns its value in two registers; the
   CBC engine returns only one`. Functions returning one register work natively, including
   `cabs`, `carg`, and on x86-64 `_Complex float` functions (packed into `xmm0`).
   `creal`, `cimag`, `conj` are clang builtins and are inlined.
5. **Floating-point variadic arguments: no rule.** E7 makes the engine set `al = 8`
   before every native call (`13-engine-changes.md` §10), so `printf("%d %f", i, d)` is a
   plain native variadic call with `d` in `xmm0`, exactly as a native compiler emits it.
   `CBC_LIBC_VARIANT` entries are used only when the *address* of a native variadic
   function is taken: the address is replaced by a CBC wrapper that calls the `v*` variant
   (`07-ir-passes.md` §4 step 1), because CBC indirect variadic calls use the buffer
   convention. Without a `v*` variant, taking the address is an error.
6. **Function-pointer arguments** (§5.6).

Guarded and crt-defined names (`CBC_LIBC_GUARDED`, `CBC_LIBC_CRT`) are CBC functions, not
native ones, so rules 1–6 do not apply to calls to them; the legalizer only emits the
signal-handler warning of §5.6.

`TargetLibraryInfo` (`05-llvm-core-changes.md` §10) marks every unsupported name and every
renamed `long double` name unavailable, so no optimization creates new calls to them.

### 5.5 Unsupported functions

A reference that remains after LTO (phase 1) or after `--gc-sections` (phase 2) to any of
these names, resolving to a native library, is a link error for **standalone `.cbc` emit**:
`error: 'qsort' is not supported on CBC: its comparator would be called by native code
(referenced from 'sort_entries' at util.c:42)`.

**Native shared libraries** (`-fcbc -shared`, `25b`): every CBC method gets an N2C stub, so
callback-taking libc (`qsort`, `qsort_r`, `bsearch`, `lfind`/`lsearch`, `tsearch*`,
`ftw*`, `pthread_create`, `pthread_once`) and passing CBC function pointers to native
code are **allowed**. Still rejected: `pthread_atfork` (fork, not attach) and
`signal`/`sigaction` CBC handlers (async — existing crt guard).

| Group | Functions | Reason | What to use instead |
|---|---|---|---|
| sorting and searching | `qsort`, `qsort_r`, `bsearch`, `lfind`, `lsearch`, `tsearch`, `tfind`, `tdelete`, `twalk`, `twalk_r`, `tdestroy` | comparator/action callback | `.cbc`: `std::sort` / own C impl. Shared library: native `qsort` with CBC comparator OK |
| file-tree walking | `ftw`, `nftw`, `ftw64`, `nftw64` | per-entry callback | `.cbc`: `opendir`/`readdir`. Shared library: OK via N2C |
| threads and once-initialization | `pthread_create`, `pthread_once`, `pthread_atfork`, `call_once`, `thrd_create`, `__pthread_register_cancel`, `_pthread_cleanup_push`, `_pthread_cleanup_push_defer` (what `pthread_cleanup_push` expands to) | start routine / init routine / cleanup handler | Shared library: `pthread_create`/`pthread_once` OK; `pthread_atfork` still unsupported |
| thread-specific data | `pthread_key_create`, `pthread_key_delete`, `pthread_getspecific`, `pthread_setspecific`, `tss_create`, `tss_delete`, `tss_get`, `tss_set` | destructor callback; and native per-OS-thread storage is wrong for a fiber the scheduler may move between OS threads | `thread_local`/`_Thread_local` (emulated TLS on the fiber control block, §5.3) |
| custom streams and hooks | `fopencookie`, `funopen`, `register_printf_function`, `register_printf_specifier`, `register_printf_modifier`, `register_printf_type`, `argp_parse`, `dl_iterate_phdr`, `ssignal`, `sigvec` | callbacks | — |
| execution contexts | `getcontext`, `setcontext`, `makecontext`, `swapcontext`, `vfork` | operate on the native stack and control flow | — |
| process startup | `__libc_start_main` | the entry is the CBC entry method | — |

Functions whose callback parameter is **optional** stay supported natively, provided no
CBC function is passed (§5.6): `glob`/`glob64` (`errfunc`), `scandir`, `scandirat`,
`scandir64`, `scandirat64` (`filter`, `compar` — note that `alphasort`/`versionsort` are
native functions and may be passed), `_obstack_begin`/`_obstack_begin_1` (the allocation
functions are normally native `malloc`/`free`). Functions taking struct-borne callbacks
that no rule can see — `timer_create`, `mq_notify`, `aio_*`, `lio_listio`,
`getaddrinfo_a` with `SIGEV_THREAD` notification — are supported only with
`SIGEV_NONE`/`SIGEV_SIGNAL`; `SIGEV_THREAD` with a CBC function is undefined behaviour for
this toolchain and is not diagnosed.

A program that **defines** one of these names itself (in phase 1, or in phase 2 for names
that the compiler does not transform) gets its own CBC function; the rule applies only to
references that resolve to native libraries.

### 5.6 Function pointers passed to native code

Clang adds the call attribute `"cbc-fnptr-args"="i,j"` listing the parameters whose
declared type is a pointer to function (`09-clang.md` §3.4). For each such argument of a
call to a native function, the legalizer classifies the value by looking through casts,
`select` and `phi`:

| Argument value | Result |
|---|---|
| a CBC function (`ptr @f` of a defined function, including compiler-generated wrappers), or a `select`/`phi` that may be one | **`.cbc` emit**: **error**. **Native shared library**: accepted (N2C stub; CBC `call.indirect` of `&foo` is I2I via `FunctionDescriptors::Lookup`) |
| null, or a native function | accepted |
| unknown (loaded from memory, a parameter, a call result) | warning `-Wcbc-native-fnptr` (on by default): `function pointer passed to native function 'foo' may be a CBC function` |

Arguments whose parameter type is not a function pointer (`void *` context arguments,
`printf("%p", f)`) are not diagnosed: native code may legitimately store or print a
function address without calling it.

For the crt **signal guards**, the same classification gives a warning instead of an error
(`-Wcbc-signal-handler`: `signal handlers are not supported on CBC; this call fails with
SIG_ERR/EINVAL at run time`), because the guarded call is well defined: it fails and the
program continues. This keeps programs that install an optional `SIGINT` handler (the Lua
and SQLite shells, for example) working without a handler.

Diagnostics outside the legalizer:

* **`%L` floating conversions** in a constant `printf`/`scanf`-family format string are a
  clang error (`09-clang.md` §2): native `printf` would read a 16-byte `long double`, and
  native `scanf` would write one into an 8-byte `double`.
* **`setjmp` address taken**: compile-time error (`08-exceptions-and-sjlj.md` §8.5).

### 5.7 Consequences for programs and test suites

* Everything that does not involve callbacks works natively and unchanged: stdio, strings,
  memory, time, files, sockets, `dlopen`/`dlsym` (a native function pointer from `dlsym`
  is called through `call.indirect`), locale functions, `getopt`, `hsearch`, `iconv`.
* C++ is barely affected: `std::sort` and the other algorithms are templates compiled as
  CBC; libc++ and libc++abi are built with threads off (§7) and never call
  `pthread_once`/`pthread_key_*`; static destructors use the crt `__cxa_atexit`;
  `std::signal` reaches the guards.
* C programs calling `qsort`/`bsearch` must be ported: either replace the call or compile
  a C implementation into the program.
* Test suites: the end-to-end harness (`11-testing-and-roadmap.md` §2.5) records the
  link-time "not supported on CBC" error as **UNSUPPORTED**, not FAIL, and checks the list
  of such tests into the repository so that changes are visible in review.

## 6. Startup and shutdown

### 6.1 Sequence

See `04-architecture.md` §13 and `07-ir-passes.md` §2. Before anything runs, the engine
rewrites the entry method; its `ld.fnptr Rdead, @native(abort)` probe makes an engine
without E5 stop in the bytecode parser (the group switch's
`FATAL("Should not reach here")`) instead of misbehaving later (`13-engine-changes.md` §1).
The functions:

| Function | Implementation |
|---|---|
| `__cbc_check_host()` | `uname(&u)`; compare `u.machine` with `"x86_64"`/`"aarch64"` per the flavour baked in at link time; mismatch → message, `_exit(127)` |
| `__cbc_apply_image_relocs(img, offs, n, base)` | `for (i < n) *(uint64_t *)(img + offs[i]) += base;` — `base == img` for relocations inside one image (always the case in phase 1); in phase 2 `base` is the target image's address (§4.4) |
| `__cbc_args(&argv)` | §6.3 |
| `__cbc_exit_handlers()` | run `__cxa_atexit`/`atexit` lists in reverse registration order, then `llvm.global_dtors` entries, then `fflush(NULL)` |

### 6.2 `exit()` from user code

The crt's `exit(status)` (§5.2) runs `__cbc_exit_handlers()` then calls native
`exit(status)`; native `atexit` handlers registered by native libraries still run.
`_Exit`/`_exit`/`abort` are native and run no handlers, as in C. A native library that
calls native `exit` itself bypasses the CBC handlers (CBC `atexit` handlers and C++ static
destructors do not run); this is inherent in having no native→CBC calls.

### 6.3 Program arguments

The launcher passes its own `argv` to the Cangjie runtime, not to the CBC entry. On Linux
`__cbc_args` (crt) reads `/proc/self/cmdline` (NUL-separated) with native `open`/`read`,
then:

* skips launcher options (`--cbc-path <p>`, `--dasm`, `--raw-dasm`, and anything up to and
  including the first argument ending in `.cbc`),
* sets `argv[0]` to that `.cbc` path, `argv[1..]` to the remaining arguments,
* allocates the vector with `malloc` and NUL-terminates it.

`environ` is the native one. On Darwin `_NSGetArgc()`/`_NSGetArgv()` (native) give the same
data.

## 7. Building the runtimes

Authoritative detail is `[14-libcxx.md](14-libcxx.md)`: which files of libc++abi are
compiled, why libunwind is not a CBC runtime, why the unwind entry points live in
`crt-cbc.bc` rather than a separate archive,
and the host cmake failure (`LIBCXXABI_USE_LLVM_UNWINDER` defaults `ON` and refuses to
configure unless `libunwind` is in `LLVM_ENABLE_RUNTIMES` — that configure is the native
`x86_64` library, not the CBC one). The flags below match `14` §13. Where this section
and `14` disagree, `14` wins.

All CBC runtimes are built with the just-built clang for each CBC triple, as bitcode
archives (`-flto`), through a separate LLVM runtimes build (not the host
`LLVM_ENABLE_RUNTIMES` list):

```
cmake -S llvm-project/runtimes -B build-cbc-x64 \
  -DLLVM_ENABLE_RUNTIMES="compiler-rt;libcxx;libcxxabi" \
  -DLLVM_DEFAULT_TARGET_TRIPLE=cbc_x86_64-unknown-linux-gnu \
  -DCMAKE_C_COMPILER=<clang> -DCMAKE_CXX_COMPILER=<clang++> \
  -DCMAKE_C_COMPILER_TARGET=cbc_x86_64-unknown-linux-gnu \
  -DCMAKE_CXX_COMPILER_TARGET=cbc_x86_64-unknown-linux-gnu \
  -DCMAKE_AR=<llvm-ar> -DCMAKE_RANLIB=<llvm-ranlib> \
  -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
  -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
  -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF \
  -DCOMPILER_RT_BAREMETAL_BUILD=OFF -DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF \
  -DCOMPILER_RT_LIBATOMIC_USE_PTHREAD=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLIBCXXABI_USE_COMPILER_RT=ON \
  -DLIBCXXABI_ENABLE_THREADS=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_STATIC=ON \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON \
  -DLIBCXX_ENABLE_THREADS=OFF -DLIBCXX_HAS_PTHREAD_API=OFF \
  -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_HAS_ATOMIC_LIB=OFF \
  -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF \
  -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ENABLE_FILESYSTEM=ON \
  -DLIBCXX_ENABLE_LOCALIZATION=ON -DLIBCXX_ENABLE_EXCEPTIONS=ON \
  -DLIBCXX_ENABLE_RTTI=ON
```

Code changes in the runtimes, all guarded by `__CBC__`. The file list, the `unwind.h`
layout, and the cmake guard that skips the `libgcc_s` probe are in `14-libcxx.md` §6, §7,
§10 and §15. Summary:

* compiler-rt: a `cbc` builtin arch list containing only generic C files (no assembly);
  `emutls.c`, `gcc_personality_v0.c` and `enable_execute_stack.c` excluded;
  `atomic.c` uses a CAS spin lock table (works across fibers; not pthreads).
* libc++abi: `cxa_personality.cpp` replaced by `cbc_eh_select.cpp` (the matching logic of
  `08` §4.3); `cxa_exception_storage.cpp` replaced by the FCB version;
  `cxa_thread_atexit.cpp` not linked; `__cxa_guard_*` single-threaded (threads off).
* libc++: nothing beyond the threads-off configuration. `std::thread` is unavailable
  (configuration), `std::mutex` compiles to no-ops.

## 8. Packaging

```
<resource-dir>/cbc/
  include/                         compat headers (09-clang.md §5.1)
  cbc_x86_64-unknown-linux-gnu/
    include/c++/v1/                libc++ headers (+ __config_site for CBC)
    lib/crt-cbc.bc
    lib/cbc_can_catch_stub.bc
    lib/libclang_rt.builtins.a
    lib/libc++.a  lib/libc++abi.a
  cbc_aarch64-unknown-linux-gnu/   same layout — follow-up flavour, not shipped in the
                                   current work scope (04-architecture.md §2)
```

## 9. Debugging aids

* `cbc-run --dasm a.cbc` (launcher option) prints the engine's disassembly of every method
  when it is prepared; `--raw-dasm` shows the original bytecode with the engine's parser,
  which cross-checks the LLVM encoder.
* `llvm-objdump -d a.cbc` with the CBC disassembler; `llvm-readobj --cbc-types
  --cbc-methods --cbc-refs a.cbc`.
* `cbc-ld --Map=a.map` for the image layout and the list of address-taken functions.
  Decoding a function pointer value seen in a debugger: if it lies in the engine's
  descriptor region (one anonymous 16 MiB mapping, visible in `/proc/<pid>/maps`), the
  first 8 bytes at that address are the `DynamicFunctionHandle*` of the CBC function;
  otherwise it is a native address (`dladdr` names it). The engine's `--dasm` output of
  the method containing the `ld.fnptr` shows the baked-in value.
* `-mllvm -cbc-trace-calls` inserts a native `fprintf(stderr, …)` at each function entry.
