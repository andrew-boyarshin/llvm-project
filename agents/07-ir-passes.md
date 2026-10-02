# 07 — Target-Specific IR Passes

All passes live in `llvm/lib/Target/CBC/` and are registered in
`CBCPassRegistry.def` (new pass manager) with legacy wrappers for `llc`. The link-time
driver (`cbc-ld`, `10-linker-and-runtime.md` §2) runs them on the fully linked module after
the LTO optimization pipeline; `llc -mtriple=cbc_*` runs the non-link-only subset so that
single-module tests work.

Order (each pass states why it must come where it is):

| # | Pass | Link only | Section |
|---|---|---|---|
| 1 | `CBCResolveSymbols` | yes | §1 |
| 2 | `CBCSynthesizeEntry` | yes | §2 |
| 3 | `LowerEmuTLS` (generic) | no | §3.1 |
| 4 | `CBCPrepareFunctionAddresses` | no | §4 |
| 5 | — (former indirect-call lowering and dispatcher generation; removed, §5) | — | §5 |
| 6 | `CBCNativeCallLegalizer` | no | §6 |
| 7 | `ExpandVariadics` (lowering, CBC ABI) | no | §7 |
| 8 | `CBCLowerSjLj` | no | `08` §8 |
| 9 | `CBCLowerEH` | no | `08` §4 |
| 10 | `CBCLowerGlobals` | no (`llc` treats its module as the whole program) | §3 |
| 11 | `CBCInsertSafepoints` | no | §8 |
| 12 | `AtomicExpandPass` + CBC hooks | no | §9 |
| 13 | `IndirectBrExpandPass` (generic) | no | §10 |
| 14 | `CodeGenPrepare` (generic) | no | — |

There is deliberately **no frame-memory IR pass**. A static `alloca` that survives the
optimizer becomes a `Default`-stack-id frame object, and the backend addresses it with
`lea.frame` (`06-backend.md` §6). A dynamic `alloca` is the `alloca` instruction. The
engine allocates the block and owns the shadow stack (`04-architecture.md` §5).

## 1. `CBCResolveSymbols`

Input: the fully linked module (all bitcode objects and runtime archives).

1. Every function **declaration** left after linking is a native symbol. Set its calling
   convention to `cbc_nativecc` and the same on all direct call sites. Attach
   `!cbc.native !{!"<linkage name>"}`.
2. Validate native symbols against the native libraries given to the linker
   (`--native-lib=/lib/x86_64-linux-gnu/libc.so.6`, defaults from the sysroot): read
   their dynamic symbol tables (`llvm::object::ELFObjectFile`) and report undefined names
   at link time. Without this check, a typo becomes a `FATAL` when the calling method is
   first rewritten at run time.
3. Every external **global variable declaration** (`@stdout = external global ptr`) is a
   native data symbol; same validation.
4. Every function **definition** is a CBC method. Assign its CBC method name: the symbol
   name; for internal-linkage symbols, append `.<unique number>` if the name collides.
   Record it in `!cbc.method`.
5. Reject what CBC cannot represent: `ifunc`s, aliases to declarations (aliases to
   definitions are resolved to the aliasee), `thread_local` variables after emutls (none
   remain), module-level inline asm, functions with `naked`.

## 2. `CBCSynthesizeEntry`

Creates `define i64 @__cbc_entry() cbc_entrycc` which the AsmPrinter emits as
`$cbc.<prog>:Entry.main`:

```llvm
define i64 @__cbc_entry() cbc_entrycc {
  call void @llvm.cbc.require.engine.ext()          ; ld.fnptr of @abort: unpatched engines
                                                    ; abort while rewriting this method
  call i64 @llvm.cbc.fcb()                          ; dead ld.fcb: an engine without E8
                                                    ; aborts in the same rewrite
  call void @__cbc_check_host()                     ; host flavour guard (crt)
  call void @__cbc_image_init()                     ; generated in §3.5
  ; (no libc initialization: native libc is already initialized in the launcher process,
  ;  the crt's exit-handler lists are zero-initialized image data)
  ; llvm.global_ctors, sorted by priority, then by order of appearance
  call void @_GLOBAL__sub_I_a.cpp()
  call void @init_fn_with_constructor_attr()
  %argc = call i32 @__cbc_args(ptr @__cbc_argv_slot)
  %argv = load ptr, ptr @__cbc_argv_slot
  %envp = load ptr, ptr @environ                    ; native data symbol
  %r = invoke i32 @__cbc_main(i32 %argc, ptr %argv, ptr %envp)
          to label %ok unwind label %lp
ok:
  call void @__cbc_exit_handlers()                  ; atexit, global_dtors, fflush(NULL)
  %r64 = sext i32 %r to i64
  ret i64 %r64
lp:
  %x = landingpad { ptr, i32 } catch ptr null
  call void @__cbc_uncaught(ptr %x) noreturn        ; std::terminate semantics
  unreachable
}
```

* The user's `main` is renamed to `__cbc_main`. Its three accepted shapes
  (`int main(void)`, `int main(int, char**)`, `int main(int, char**, char**)`) are adapted
  by a small wrapper. `main` returning `void` (non-standard) returns 0.
* `llvm.global_ctors`/`llvm.global_dtors` are consumed and erased; destructors are passed
  to `__cbc_exit_handlers` through a generated table.
* A program without `main` (a library) gets no entry; the `.cbc` header's
  `mainTypeName` is −1.

## 3. `CBCLowerGlobals`

### 3.1 Preconditions

* `LowerEmuTLS` has run: no `thread_local` globals remain, only `__emutls_v.*` control
  blocks (ordinary globals) and calls to `__emutls_get_address`.
* EH clause tables and variadic helper globals already exist (passes 6–9 may create
  globals).
* Function addresses are `ptr @f` (CBC function or native function, after the
  substitutions of §4); they are turned into relocations here (§3.3) and into
  `LOAD_FNPTR` in code.

### 3.2 Layout

1. Collect all global variable definitions except intrinsic globals (`llvm.used`,
   `llvm.compiler.used`, `llvm.global_ctors/dtors` — already consumed, `llvm.metadata`
   section globals).
2. Split into *initialized* (non-zero initializer) and *zero* (zero or undef initializer).
   Sort each group by decreasing alignment, then by size (stable by original order for
   determinism).
3. Assign offsets: initialized first, then zero-initialized; each aligned to
   `max(ABI alignment, explicit align)`. Let `N = align8(end)` and `MaxAlign` = the largest
   alignment.
4. If `MaxAlign > 16`: base-pointer mode (`04-architecture.md` §6.1).

### 3.3 Initializer serialization

For the initialized prefix, produce a byte array `B` and three relocation lists by
walking each initializer with `DataLayout` (`ConstantDataSequential`,
`ConstantArray/Struct/Vector`, `ConstantInt`, `ConstantFP`, `ConstantPointerNull`,
`UndefValue/PoisonValue` → zeros, `ConstantExpr`):

| Constant pointer value | Bytes in `B` | Relocation |
|---|---|---|
| `@g + c` (`@g` a global in the image) | `off(g) + c` | `IMAGE_ABS64 at o` |
| CBC function `@f` | 0 | `FNPTR64 at o, method f` (the descriptor address, written at startup) |
| native function `@n + c` | `c` | `NATIVE_FNPTR64 at o, sym n` |
| native data `@n + c` | `c` | `NATIVE_ABS64 at o, sym n` |
| `ptrtoint(@a) - ptrtoint(@b)` with both in the image | difference | none |
| `blockaddress(@f, %bb)` | the block id assigned by `IndirectBrExpand` (a small integer) | none |
| `ptrtoint` truncated to 32 bits of an image address | **unsupported** (error: needs `IMAGE_ABS32`, the image address is not known at link time) | — |

### 3.4 Use rewriting

* `convertUsersOfConstantsToInstructions` expands every constant expression that uses a
  global into instructions.
* In each function that uses globals, insert `%base = call i64 @llvm.cbc.image.base(i32 0)` at
  the top of the entry block (the intrinsic is `readnone`/`willreturn`; the backend
  rematerializes it, so register pressure is not a concern), and replace each use of
  `@g` with `inttoptr(add %base, off(g))` expressed as `getelementptr i8, ptr %basep, i64 off`.
* Uses of native data symbols become `call i64 @llvm.cbc.native.addr(metadata !"sym")`.
* Erase the globals. Record the image description in named metadata
  `!cbc.image = !{size, maxalign, !blobB, !relocsImage, !relocsNative}` for the AsmPrinter
  (which registers the blob strings and the `$cbc.<prog>:Data.image` static).

### 3.5 `__cbc_image_init`

Generated function called by the entry:

```llvm
define internal void @__cbc_image_init() {
  %img  = call i64 @llvm.cbc.image.base(i32 0)          ; phase 1: the only image
  %blob = call i64 @llvm.cbc.initstr(i32 0)             ; bytes of B
  call ptr @memcpy(ptr %imgp, ptr %blobp, i64 <|B|>)     ; native
  %rel  = call i64 @llvm.cbc.initstr(i32 1)             ; u32 offsets of IMAGE_ABS64 slots
  call void @__cbc_apply_image_relocs(ptr %imgp, ptr %relp, i64 <count>, i64 %img)   ; crt, CBC; base = own image
  ; one group per NATIVE_ABS64 relocation (native data):
  %a  = call i64 @llvm.cbc.native.addr(metadata !"stdout")
  %p  = getelementptr i8, ptr %imgp, i64 <o>
  %v  = load i64, ptr %p          ; addend stored in B
  %s  = add i64 %v, %a
  store i64 %s, ptr %p
  ; one store per FNPTR64 relocation (selected to ld.fnptr + st.raw.mem.64):
  %q  = getelementptr i8, ptr %imgp, i64 <o2>
  store ptr @vfunc, ptr %q
  ; NATIVE_FNPTR64 relocations: the same, with the native function (and an add for addends)
  %q2 = getelementptr i8, ptr %imgp, i64 <o3>
  store ptr @strcmp, ptr %q2
  ret void
}
```

When `|B|` is small (< 64 bytes) the pass emits stores of constants instead of the blob.
Large `NATIVE_ABS64` lists are grouped by symbol. The relocation part is split into
functions `@__cbc_image_init.<n>` of at most 1 000 `ld.fnptr`/`lea.s` relocations each,
so that each method stays far below the 4 096-entry RT literal table (every `ld.fnptr`
and `lea.s` costs one literal).

### 3.6 Module-local mode (deferred with phase 2)

> **Deferred — not in the work scope.** Separate code generation is postponed
> indefinitely (`04-architecture.md` §3). In the current scope `CBCLowerGlobals` always
> runs in whole-program mode (§3.1–§3.5); `llc` treats its single module as the whole
> program, which is all that `llvm/test/CodeGen/CBC` needs. The rules below are the
> design sketch for a future phase 2 and are not implemented.

When the module is not the whole program (`clang -c` code generation in phase 2,
`10-linker-and-runtime.md` §4), the pass would lay out only what the module may lay out,
and leave the rest to the linker. The mode would be selected by the absence of the module
flag `!"cbc-whole-program"` (set by `cbc-ld`).

1. **Classify each global variable** of the module:

   | Global | Placement |
   |---|---|
   | definition, not in a COMDAT, alignment ≤ 16, linkage `internal`/`private`/`external`/`dso_local` strong | **own image** (`.cbc.image.self`), laid out as in §3.2 |
   | definition, not in a COMDAT, alignment > 16 | **aligned image** (`.cbc.image.aligned`), base-pointer mode |
   | definition in COMDAT group `G` (`linkonce_odr`, `weak_odr`, `comdat any`) | **group image** `.cbc.image.g.<G>` (one per group; a group containing several globals keeps them together, laid out as in §3.2) |
   | weak definition outside a COMDAT (`weak`, `extern_weak` with a definition) | own image, but every reference uses the `IMAGEOF` path below because a strong definition elsewhere may prevail |
   | declaration (`external global`) | no placement; references use the `IMAGEOF` path |
   | common symbol | error `common symbols are not supported on CBC; compile with -fno-common` (the driver default) |

2. **Rewrite uses.**
   * Uses of own-image globals that cannot be overridden: exactly as §3.4, with
     `%base = call i64 @llvm.cbc.image.base(i32 0)` (image number 0 = own image of the
     module; 1 = aligned image; `2 + g` = the `g`-th group image of the module).
   * All other uses (declarations, weak definitions, every COMDAT member — including a
     member defined in this module, because another module's copy may be the one that
     survives): replaced by `call i64 @llvm.cbc.image.addr.of(ptr @g)` plus the constant
     offset `c` of the use. The intrinsic is selected to `LEA_IMAGEOF` + an `IMGOFF`
     displacement (`06-backend.md` §4.3, §8.6) and relocated by the linker
     (`10-linker-and-runtime.md` §4.2). It keeps `@g` as an operand, so these globals are
     **not** erased: declarations stay declarations, definitions stay definitions with
     their image placement recorded in `!cbc.image.placement`.
   * Uses of globals placed in a **group image of this module** from code in the same
     COMDAT group (a guard variable used by the inline function it guards) could use the
     group image directly when both survive together, but the linker selects COMDAT groups
     per group, not per pair, so the pass does not rely on it and uses `IMAGEOF`.
3. **Initializers** are serialized per image as in §3.3, but pointer-valued constants are
   emitted as object-file relocations (`R_CBC_ABS64 sym + addend`) instead of image
   relocations, because the image of the target is known only after linking. The pass does
   **not** generate `__cbc_image_init`; the linker synthesizes it for all images
   (`10-linker-and-runtime.md` §4.4).
4. **Function pointers** need nothing special: every CBC function's pointer is its
   engine descriptor, obtained by `ld.fnptr` on a method reference that the linker
   resolves like a call (`10-linker-and-runtime.md` §4.3).

## 4. `CBCPrepareFunctionAddresses`

Every CBC function's pointer is its engine descriptor (`04-architecture.md` §7.1), so there
is no representation to choose and nothing global to compute. Address uses of CBC
functions stay `ptr @f`: the backend selects them to `LOAD_FNPTR` (E5) in code, and
`CBCLowerGlobals` emits `FNPTR64` relocations for them in initializers (§3.3). The same
holds for native functions (`LOAD_FNPTR` on the AOT method reference, `NATIVE_FNPTR64`).

What remains for this pass is the handful of native functions whose *address* cannot be
used as is. For every use of a native function (declaration) that is not the callee
operand of a direct call or invoke:

1. **Native variadic function** (`&printf`, `&fprintf`, …): replaced by the address of a
   generated CBC variadic wrapper `define internal @__cbc_vwrap.<n>(fixed…, ...)` whose
   body is `va_start`, a native call of the `v*` variant (`CBC_LIBC_VARIANT` in
   `CBCNativeLibc.def`, `10-linker-and-runtime.md` §5.4), `va_end`. It is an ordinary CBC
   variadic function, so `ExpandVariadics` later gives it the buffer convention and builds
   the host `va_list` over the buffer — which is what every CBC indirect variadic call
   site passes (`04-architecture.md` §10.1). Without a known `v*` variant: error
   `cannot take the address of native variadic function 'n'`. (Direct calls to native
   variadic functions need no wrapper; with E7 they are plain native calls.)
2. **`div` family** (`CBC_LIBC_EXPAND_DIV`: `div`, `ldiv`, `lldiv`, `imaxdiv`): replaced by
   the address of a generated internal CBC function `@__cbc_libc.ldiv` with the same
   prototype whose body is the inline expansion of §6 rule 3. CBC functions may return two
   registers, so an indirect call to the wrapper receives both halves.
3. **`long double` rename** (`CBC_LIBC_RENAME`): replaced by the address of the rename
   target (`&sinl` → `&sin`), which stays a native pointer.
4. **Unsupported function** (`CBC_LIBC_UNSUPPORTED`): left alone here and rejected by §6
   rule 1.
5. **Any other native function returning two registers**: error (an indirect call could
   not receive its second result, `04-architecture.md` §9.6).

Each generated wrapper is internal, has a deterministic name, and is created at most once
per native function. A wrapper's address is then simply its descriptor, like that of any
CBC function.

Whole-program devirtualization (`WholeProgramDevirt`) and `CalledValuePropagation` run
earlier in the LTO pipeline and turn many indirect calls into direct calls; that is an
optimization only, nothing depends on it.

## 5. (removed) Indirect-call lowering and dispatchers

Earlier versions of this plan rewrote indirect call sites into a range check between an
index-backed dispatcher path and `call.indirect`, and generated per-signature dispatchers
(switch-based or abstract-class-based). With function descriptors (E5/E6,
`13-engine-changes.md` §9) every indirect call is a single `CALL_INDIRECT`, selected
directly from the IR `call ptr %fp(...)` by `LowerCall` (`06-backend.md` §5.3). There is no
IR pass for indirect calls, no signature-class computation, no `cbc_dispatchcc` calling
convention and no `__cbc_bad_icall`. The section number is kept so that references to §6
and later stay valid.

## 6. `CBCNativeCallLegalizer`

Table-driven by `CBCNativeLibc.def`; the policy, the complete tables and the diagnostics
are specified in `10-linker-and-runtime.md` §5.4–§5.6. The pass runs after
`CBCResolveSymbols` (it needs to know which callees are native), after
`CBCPrepareFunctionAddresses` (whose wrappers replace some native addresses), and before
`ExpandVariadics` (variadic calls still carry their unnamed arguments). It visits every call to a native function (`cbc_nativecc`) and every other
use of a native function's address, and applies, in this order:

1. **Unsupported functions** (`CBC_LIBC_UNSUPPORTED`, `10-linker-and-runtime.md` §5.5):
   any remaining call or address use is an error
   `'qsort' is not supported on CBC: its comparator would be called by native code`,
   reported with `DiagnosticInfoUnsupported` on the using function and the use's debug
   location when present. Because this pass runs after LTO, references from dead code
   have already been removed and do not fail the link.
2. **`long double` renames.** For a native callee carrying the clang attribute
   `"cbc-long-double"` (`09-clang.md` §3.4): if `CBC_LIBC_RENAME(name, target)` exists,
   replace the callee (and every address use) with `target`, creating the declaration if
   needed and marking it native; otherwise error
   `native function 'foo' has 'long double' in its prototype; on CBC long double is double
   and does not match the native ABI`. The IR function types already agree, because clang
   lowered `long double` to `double`.
3. **`div` family expansion** (`CBC_LIBC_EXPAND_DIV`): a call to native `div`, `ldiv`,
   `lldiv` or `imaxdiv` whose declaration has the expected prototype is replaced by

   ```llvm
   %q   = sdiv i64 %a, %b                       ; i32 for div
   %r   = srem i64 %a, %b
   %tmp = alloca %struct.ldiv_t                 ; { i64 quot, i64 rem } (C layout)
   store i64 %q, ptr %tmp
   %p   = getelementptr inbounds i8, ptr %tmp, i64 8
   store i64 %r, ptr %p
   %res = load { i64, i64 }, ptr %tmp           ; the call's IR return type, whatever
                                                ; coercion clang chose (i64, {i64,i64},
                                                ; [2 x i64])
   ```

   Address uses of these functions never reach this pass: §4 has already replaced them
   with CBC wrappers.
4. **Two-register results** of any other native callee (`{i64, i64}`, `i128`,
   `{double, double}`, AArch64 HFA of 2–4, including `_Complex double` functions): error
   `native function 'csqrt' returns its value in two registers; the CBC engine returns
   only one`.
5. **Floating-point variadic arguments: no rule.** With E7 the engine sets `al = 8` before
   every native call (`13-engine-changes.md` §10), so `printf("%d %f", i, d)` is lowered
   as a plain native variadic call: `i` in `IR2` (`rsi`), `d` in `FR0` (`xmm0`), exactly
   as a native compiler would pass them (`04-architecture.md` §10.2). The earlier
   redirection to `v*` variants is gone; the `v*` map is used only by the address
   wrappers of §4 step 1. (The rule number is kept so that references to rule 6 stay
   valid.)
6. **Function-pointer arguments.** Clang lists the parameters whose declared type is a
   pointer to function in the call attribute `"cbc-fnptr-args"="i,j"` (`09-clang.md`
   §3.4). For each such argument the pass looks through casts, `select` and `phi`:
   * a CBC function (`ptr @f` of a defined function, including the wrappers of §4), or a
     `select`/`phi` that may yield one: **error** `passing CBC function 'cmp' to native
     function 'scandir'; native code calling CBC code is not supported`. The value would
     be a non-executable descriptor, and a native call through it faults
     (`13-engine-changes.md` §8);
   * null or a native function: accepted (`scandir(d, &list, NULL, alphasort)`);
   * anything else: warning `-Wcbc-native-fnptr`
     `function pointer passed to native function 'foo' may be a CBC function`.

   Arguments of non-function-pointer type (`void *`) are not inspected. Calls to the crt
   signal guards (`CBC_LIBC_GUARDED`: `signal`, `sigaction`, …) are CBC calls, but the
   pass inspects their handler argument the same way and gives the warning
   `-Wcbc-signal-handler` instead of an error, because the guarded call fails cleanly
   at run time (`10-linker-and-runtime.md` §5.2). For `sigaction` only a handler visible
   as a constant store into the `struct sigaction` passed as `act` in the same function is
   found; other cases are left to the run-time guard.
7. **`setjmp` family** calls are handled by `CBCLowerSjLj` (`08-exceptions-and-sjlj.md`
   §8); the context functions and `vfork` are in the unsupported list (rule 1).

(Deferred phase 2 only: in module-local mode the pass would not know which declarations
are native. It would apply rules 2 and 3 to declarations whose names are C library names
in the table — reserved identifiers, `10-linker-and-runtime.md` §4.5 — and record rules
1, 4 and 6 as `.cbc.notes` entries for the linker.)

## 7. `ExpandVariadics` configuration

See `05-llvm-core-changes.md` §7. CBC-specific behaviour:

* Only **defined** variadic functions are rewritten (they are CBC methods), together with
  their direct call sites and every indirect variadic call site (whose target is always a
  CBC function or a §4 wrapper). Direct calls to native variadic functions are left alone
  (`05-llvm-core-changes.md` §7).
* The buffer is a static `alloca` (→ the untyped memory block, `04-architecture.md` §5.2)
  with alignment 16.
* After the pass, a CBC function's `va_list` is the host-native structure, so passing it to
  native `vfprintf` works, and so does `va_copy` (plain struct copy; on x86-64 the
  `va_list` is an array type passed by pointer — clang's lowering already handles that).

## 8. `CBCInsertSafepoints`

For every loop (from `LoopInfo`) whose body contains no call that is a state point (CBC
or native direct call, indirect call), insert `call void @llvm.cbc.gcpoint()` at the top
of the latch block. Loops whose trip count is known to be ≤ 1024 (SCEV) are skipped. Option
`-cbc-loop-safepoints=0` disables the pass. The backend emits `GcPoint` and a liveness entry
for each.

## 9. Atomic expansion hooks

In `CBCTargetLowering`:

* `shouldExpandAtomicRMWInIR`: `CmpXChg` for `nand`, `min`, `max`, `umin`, `umax`, `fadd`,
  `fsub`, `fmax`, `fmin`, `uinc_wrap`, `udec_wrap`, `usub_cond`, `usub_sat`; `None` for
  `xchg`, `add`, `sub`, `and`, `or`, `xor` on 8–64 bits; `Expand` (libcall) above 64 bits.
* `shouldExpandAtomicCmpXchgInIR`: `CustomExpand` → `emitExpandAtomicCmpXchg` builds:

```llvm
loop:
  %ok = call i1 @llvm.cbc.cas.i64(ptr %p, i64 %exp, i64 %new)
  br i1 %ok, label %done_ok, label %reload
reload:
  %cur = load atomic i64, ptr %p seq_cst, align 8
  %same = icmp eq i64 %cur, %exp
  br i1 %same, label %loop, label %done_fail
done_ok:   ; result { %exp, true }
done_fail: ; result { %cur, false }
```

  Sub-word `cmpxchg` (8/16 bits) uses the matching `CAS` width directly (the engine's
  atomic instructions support 8/16-bit fields), so no masking is needed.
* `shouldInsertFencesForAtomic`: false; every atomic instruction is `seq_cst`.
* `getMaxAtomicSizeInBitsSupported`: 64.
* Volatile atomics and `syncscope("singlethread")`: same instructions.

## 10. `indirectbr` and `blockaddress`

`IndirectBrExpandPass` replaces each `indirectbr` with a `switch` over the block IDs and
rewrites `blockaddress` constants to those IDs (cast to pointers). Computed `goto` in GNU C
therefore works. Block IDs are small integers; `CBCLowerGlobals` treats a
`blockaddress` in a global initializer as its ID.

## 11. Pass interaction checklist

* Run `CBCResolveSymbols` before anything that inspects whether a callee is native.
* Run `CBCPrepareFunctionAddresses` before `ExpandVariadics` (its native-variadic
  wrappers are variadic CBC functions that must be expanded) and before
  `CBCNativeCallLegalizer` (address uses it replaced must not be diagnosed).
* Run `CBCNativeCallLegalizer` before `ExpandVariadics` and before debug info is stripped.
* Run `CBCLowerEH`/`CBCLowerSjLj` before `CBCLowerGlobals` (they create clause tables).
* Run `CBCLowerGlobals` last among the passes that create or reference globals.
* Every pass must preserve the invariant "the only `GlobalValue` operands that reach
  instruction selection are direct callees and the addresses of **functions** (CBC or
  native, selected to `LOAD_FNPTR`)"; no global variable may remain.
