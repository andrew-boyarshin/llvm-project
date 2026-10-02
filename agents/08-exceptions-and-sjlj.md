# 08 — C++ Exceptions and `setjmp`/`longjmp` on CBC

E1–E7 are not needed here; E6 only adds one more source of engine-originated exceptions
(§4.3). E8 is: the unwinder frees a popped frame's dynamic allocations, and `ld.fcb`
replaces the old stack-pointer mask (`04-architecture.md` §5). The engine provides:
exception regions
`[start, end) → target` per method, unwinding through interpreted frames with restoration
of each frame's callee-saved registers, `IR_ACC` = engine exception object at the handler,
and two ways for compiled code to raise: `nullcheck IRZ` (runtime-created
`NoneValueException`) and `throw r` (rethrow an object held in a register). This design
uses only the first; `throw r` is avoided because it would require keeping an engine
object alive across C code (§2). Everything about C++ semantics is implemented in the
compiler and in CBC-compiled runtime code.

## 1. Per-fiber runtime state: the fiber control block

The engine allocates one FCB per fiber at fiber start (`malloc`, zeroed, never moved)
and stores the pointer on the `Ectype`. CBC code loads it with `ld.fcb`
(`04-architecture.md` §5.7). It is not derived from a stack pointer, and it is not the
shadow stack: the shadow cursor is a separate engine-private field, so stores through
the FCB cannot move it. The FCB holds:

```c
struct __cbc_fcb {
  uint64_t magic;                 // 'CBCFCB01'
  struct {
    int      kind;                // 0 none, 1 C++ exception, 2 longjmp
    int      in_pad;              // 1 while landing-pad code for the current exception runs
    void    *exc;                 // _Unwind_Exception* (kind 1)
    uint64_t jmp_token;           // frame token (kind 2)
    uint32_t jmp_label;           // setjmp site id (kind 2)
    int      jmp_value;           // longjmp value (kind 2)
  } eh;                           // no engine object is ever stored here (§2)
  void   **tls_blocks;            // per-fiber emutls storage (10-linker-and-runtime.md §5.3)
  __cxa_eh_globals cxa_globals;   // libc++abi caught-exception stack, uncaught count
};
```

`__cbc_fcb()` is `always_inline` and lowers to one `ld.fcb`. This replaces thread-local
storage for all runtime state and is correct even if the engine migrates a fiber between
OS threads. The dynamic shadow stack (`04-architecture.md` §5.4) is not part of this
struct.

## 2. The marker-exception protocol

* **Raise.** `__cbc_raise()` (CBC runtime, `noinline`, `noreturn`) executes
  `nullcheck IRZ`. The engine creates a `NoneValueException`, looks for a region in the
  current frame (none: `__cbc_raise` has no regions), unwinds it, and continues in the
  callers. The payload is never in the engine object; it is in `fcb->eh`.
* **Continue unwinding** (after a cleanup, or when a pad has no matching handler): call
  `__cbc_raise()` again, which creates a fresh `NoneValueException`. The caught engine
  object is deliberately *not* rethrown with `throw r`: keeping it across cleanup code
  would mean storing a GC reference in malloc'd fiber memory, which is not a GC root and
  may be moved or collected meanwhile. The price is one runtime allocation per landing pad
  on the unwinding path.
* **Handler entry.** The engine jumps to the region's target with `IR_ACC = obj`. The
  landing pad reads `fcb->eh` (`ld.fcb`); it does not need `obj`. It does not adjust the
  shadow cursor: popped frames were freed by the unwinder (`04-architecture.md` §5.5),
  and this frame was not popped.
* **Why `__cbc_raise` must be `noinline`.** If the `nullcheck IRZ` were inlined into a
  function at a position covered by one of that function's regions, the function's own
  handler would catch it, which is correct; but LLVM cannot `invoke` an intrinsic (verifier
  rule), so inlining `llvm.cbc.raise` into an `invoke` context is invalid IR. Keeping the
  raise in its own frame avoids the problem (alternatively, add `llvm.cbc.raise` to the
  verifier's list of invokable intrinsics, as was done for `llvm.wasm.throw`).

## 3. Throw

libc++abi is compiled for CBC unchanged except for the unwinder interface:
`__cxa_throw` → `_Unwind_RaiseException(&header->unwindHeader)`. The CBC runtime
(`crt-cbc.bc`, which replaces libunwind; there is no separate unwind archive,
`14-libcxx.md` §1.1) provides:

```c
_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *ue) {
  struct __cbc_fcb *f = __cbc_fcb();
  f->eh.kind   = 1;
  f->eh.in_pad = 0;
  f->eh.exc    = ue;
  __cbc_raise();                 // does not return
}
void _Unwind_Resume(struct _Unwind_Exception *ue)           { same as above }
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *ue) { same }
void _Unwind_DeleteException(struct _Unwind_Exception *ue)  { if (ue->exception_cleanup) ue->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, ue); }
```

Single-phase unwinding: cleanups run while unwinding even when no handler exists, then the
entry method calls `std::terminate`. The C++ standard leaves stack unwinding before
`terminate` implementation-defined ([except.handle]/9), so this is conforming.
`_Unwind_Backtrace`, `_Unwind_GetIP`, `_Unwind_FindEnclosingFunction` return
`_URC_END_OF_STACK`/0 (no frame introspection).

## 4. Landing pads

### 4.1 Region emission (backend)

SelectionDAG lowers each `invoke` with `EH_LABEL`s before and after the call; the
AsmPrinter emits one region per call site: `start = begin label`, `end = end label`
(position just after the call instruction), `target = landing pad MBB`. Call sites of the
same function never overlap, so the engine's first-match rule is irrelevant. Landing pads
are ordinary basic blocks; the engine only needs the target to be an instruction boundary.

Exception pointer register: `getExceptionPointerAddress/Register` returns `IRACC`;
selector register: none (the selector is computed in IR, §4.2).

### 4.2 IR rewriting (`CBCLowerEH`)

For each function with a personality (`__gxx_personality_v0` or
`__gcc_personality_v0`), `CBCLowerEH`:

1. Builds the function's **type table**: the distinct type-info pointers referenced by
   catch clauses and filters across all its landing pads, numbered 1, 2, … in order of
   first appearance. Each `llvm.eh.typeid.for(T)` becomes the constant number of `T`.
2. For each landing pad emits a **clause table** global:

```c
struct __cbc_lpad_clauses {
  uint32_t count;
  uint32_t flags;                // bit 0: cleanup
  struct { int32_t kind;         // 0 catch, 1 filter (negative id), 2 catch-all
           int32_t id;           // typeid number (catch) or -filterIndex (filter)
           const void *tinfo;    // catch: type_info*; filter: pointer to a null-terminated list
         } c[];
};
```

3. Rewrites the landing pad:

```llvm
lpad:
  %lp = landingpad { ptr, i32 } cleanup catch ptr @_ZTIi catch ptr null
  ; inserted:
  %exn = call ptr @__cbc_eh_landing()
  %sel = call i32 @__cbc_eh_select(ptr %exn, ptr @lpad.clauses.3)
  ; every use of extractvalue %lp, 0 → %exn ; extractvalue %lp, 1 → %sel
```

   `landingpad` itself stays (IR validity); its results become dead.
4. Rewrites `resume { ptr %exn, i32 %sel }` into
   `call void @__cbc_eh_resume(ptr %exn)` + `unreachable`.

### 4.3 Runtime routines (CBC, in `crt-cbc.bc`; type matching in the libc++abi port)

State of `fcb->eh` over the life of one exception:

| Event | `kind` | `in_pad` |
|---|---|---|
| `_Unwind_RaiseException` / `longjmp` raise | 1 / 2 | 0 |
| a landing pad starts (`__cbc_eh_landing`) | unchanged | 1 |
| the pad is a cleanup and finishes (`__cbc_eh_resume`) or does not handle it (rethrow from `__cbc_eh_select`) | unchanged | 0, then re-raise |
| a C++ handler takes it (`__cxa_begin_catch`) / the `setjmp` dispatch pad takes the `longjmp` | 0 | 0 |

An exception arriving at a landing pad is **ours** exactly when `kind != 0 && in_pad == 0`.
Anything else is engine-originated: `NoneValueException` from `call.indirect` through a
null pointer (E6), `ArithmeticException` from `DivCheck` (sanitizer mode), or an engine
exception raised by code running inside a cleanup pad. C programs contain no Cangjie code,
so these are always errors, and the runtime reports them and aborts — there is no
"foreign exception" path.

```c
static void __cbc_eh_check(void) {
  struct __cbc_fcb *f = __cbc_fcb();
  if (f->eh.kind == 0 || f->eh.in_pad)
    __cbc_engine_exception();            // "CBC engine exception (call through a null
                                         //  function pointer, or division by zero under
                                         //  -fsanitize=integer-divide-by-zero)"; abort()
}

void *__cbc_eh_landing(void) {
  struct __cbc_fcb *f = __cbc_fcb();
  __cbc_eh_check();
  f->eh.in_pad = 1;
  return f->eh.kind == 1 ? f->eh.exc : __cbc_sjlj_marker;   // longjmp passing through (§8)
}

_Noreturn void __cbc_continue_unwinding(void) {
  __cbc_fcb()->eh.in_pad = 0;
  __cbc_raise();                         // fresh marker, same payload in fcb->eh
}

int32_t __cbc_eh_select(void *exn, const struct __cbc_lpad_clauses *t) {
  if (exn == __cbc_sjlj_marker) {                     // let longjmp propagate
    if (t->flags & 1) return 0;                        // run C++ cleanups on the way (§8.4)
    __cbc_continue_unwinding();
  }
  for (uint32_t i = 0; i < t->count; ++i) {
    switch (t->c[i].kind) {
    case 2: return t->c[i].id;                         // catch (...)
    case 0: if (__cbc_can_catch(t->c[i].tinfo, exn)) return t->c[i].id; break;
    case 1: if (!__cbc_matches_filter(t->c[i].tinfo, exn)) return t->c[i].id; break;
    }
  }
  if (t->flags & 1) return 0;                          // cleanup only
  __cbc_continue_unwinding();                          // no handler here: keep unwinding
}
```

`__cxa_begin_catch` (libc++abi port) sets `kind = 0, in_pad = 0` before its usual work;
`__cbc_sjlj_test` (§8.3) does the same when it accepts a `longjmp`.

* `__cbc_can_catch(tinfo, exn)` is libc++abi's personality-internal matching
  (`exception_spec_can_catch`/`__shim_type_info::can_catch`) moved into a function. On
  success it stores the **adjusted pointer** and the handler switch value into the
  `__cxa_exception` header, exactly as phase 1 of `__gxx_personality_v0` would, so that
  `__cxa_begin_catch` returns the right base-class subobject address.
* `__cbc_continue_unwinding` is called from inside `__cbc_eh_select`, i.e. from a frame that has
  no region, so the engine continues unwinding into the landing pad function's caller —
  the landing pad function itself is not re-entered because the call to `__cbc_eh_select`
  is not an `invoke`.

### 4.4 `__cbc_eh_resume`

```c
void __cbc_eh_resume(void *exn) {
  struct __cbc_fcb *f = __cbc_fcb();
  f->eh.exc    = (exn == __cbc_sjlj_marker) ? f->eh.exc : exn;
  f->eh.kind   = (exn == __cbc_sjlj_marker) ? 2 : 1;
  __cbc_continue_unwinding();
}
```

### 4.5 Handlers and `__cxa_begin_catch`

Unchanged libc++abi: `__cxa_begin_catch(exn)` pushes onto `fcb->cxa_globals.caughtExceptions`,
`__cxa_end_catch` pops and destroys. `std::current_exception`, `std::rethrow_exception`,
`std::exception_ptr`, nested exceptions, `std::uncaught_exceptions()` work unchanged.
`__cxa_get_globals()` returns `&__cbc_fcb()->cxa_globals`.

## 5. Interaction with the code generator

* `ExceptionHandling::CBC` in `MCAsmInfo` (or `None` plus an `EHStreamer` driven from the
  landing-pad info; either way `DwarfEHPrepare` must not insert `_Unwind_Resume` calls,
  because `CBCLowerEH` already rewrote `resume`).
* Landing pads do not reload a stack pointer. There is no `CBCLandingPadFixup`. Spill
  slots and the untyped memory block are at fixed frame offsets, so the first instruction
  of the pad may use them. The engine has already restored callee-saved registers and
  freed the shadow allocations of every frame it popped.
* Values live into a landing pad: SSA values defined before the `invoke`. Callee-saved
  registers hold correct values (the engine restored them while unwinding); values in
  volatile registers are dead across the call by construction (the call clobbers them);
  spilled values are in the frame, which is intact.
* `llvm.cbc.catch` (`Catch Rd`) is optional and `CBCLowerEH` does not emit it: the
  runtime never needs the engine object (§2). The intrinsic stays available for tests and
  for future interop; its position in a landing pad is irrelevant to the engine.

## 6. `noexcept`, terminate, and the entry

* Clang emits terminate landing pads (`catch ptr null` + `__clang_call_terminate`) for
  `noexcept` functions; they work as ordinary catch-alls.
* The entry method wraps `__cbc_main` in an `invoke` with a catch-all landing pad that calls
  `__cbc_uncaught(exn)`: print `terminate called after throwing an instance of '<type>'`
  (via `__cxa_current_exception_type`), then `abort()`.
* An engine-originated exception never reaches the entry's handler as an exception: the
  first landing pad it meets reports it and aborts (§4.3). If it meets no landing pad at
  all, the entry's own landing pad (which uses the same `__cbc_eh_landing`) does.

## 7. Costs

* No cost on the non-throwing path except region table bytes and the landing-pad code.
* Throwing: one runtime allocation (`NoneValueException`) per raise and one more per
  landing pad that continues unwinding (§2), one call to `__cbc_eh_select` per landing pad
  on the way. Unwinding frames without landing pads is done by the engine at native speed.

## 8. `setjmp`/`longjmp`

### 8.1 Model

Same marker mechanism; payload kind 2. Modelled on
`llvm/lib/Target/WebAssembly/WebAssemblyLowerEmscriptenEHSjLj.cpp` ("Wasm SjLj" mode),
whose control-flow and SSA rebuilding logic carries over.

### 8.2 `jmp_buf` contents

The host `jmp_buf` type is kept (headers are native), only its first 32 bytes are used:

```c
struct __cbc_jmp { uint64_t magic; uint64_t token; uint32_t label; uint32_t pad; uint64_t shadow; };
```

### 8.3 Transformation of a function `F` that calls `setjmp` (`CBCLowerSjLj`)

1. **Frame token.** At entry, `%tok = ptrtoint (alloca i8)` — the address of a byte in
   `F`'s untyped memory block, unique while `F`'s activation is live.
2. **setjmp sites.** Each `%r = call i32 @setjmp(ptr %buf)` (also `_setjmp`, `sigsetjmp`,
   `__sigsetjmp`) at site `i` becomes `call void @__cbc_setjmp(ptr %buf, i64 %tok, i32 i)`
   with `%r = 0` on the direct path; the block is split after the call and the continuation
   block gets a PHI for `%r`.
3. **Protection.** Every call in `F` that may call `longjmp` (every call not marked
   `nounwind` *and* not known not to longjmp — in practice every non-intrinsic call after
   the first setjmp in program order) is turned into an `invoke` to a dispatch landing pad
   (or, if it already is an `invoke`, its existing landing pad gets the dispatch prepended).
4. **Dispatch landing pad.**

```llvm
sjlj.lpad:
  %lp  = landingpad { ptr, i32 } cleanup
  %v   = call i32 @__cbc_sjlj_test(i64 %tok)   ; returns label+1 if the pending
                                                ; longjmp targets %tok, else 0
  switch i32 %v, label %not.ours [ i32 1, label %cont.0  ; value via @__cbc_sjlj_value()
                                   i32 2, label %cont.1 ... ]
not.ours:
  ; if the call had a real C++ landing pad, branch to it (it calls __cbc_eh_landing itself);
  ; else continue unwinding
  call void @__cbc_continue_unwinding()
  unreachable
```

```c
int32_t __cbc_sjlj_test(uint64_t tok) {
  struct __cbc_fcb *f = __cbc_fcb();
  __cbc_eh_check();                        // engine-originated exception: report, abort
  if (f->eh.kind != 2 || f->eh.jmp_token != tok) return 0;   // state left untouched
  f->eh.kind = 0;                          // this longjmp is consumed here
  return (int32_t)f->eh.jmp_label + 1;
}
```

   `__cbc_sjlj_test` only inspects the state when it does not accept the `longjmp` (it
   does not set `in_pad`), so the original C++ pad reached via `not.ours` sees the same
   state as if the dispatch had not been prepended.

   `%cont.i` receives `@__cbc_sjlj_value()` (0 mapped to 1) as the second incoming value of
   the setjmp-result PHI.
5. **SSA repair.** Values defined before a setjmp and used after it may now be reached
   from the dispatch pad; rebuild SSA with `SSAUpdater` exactly as the WebAssembly pass does
   (`rebuildSSA`). C semantics only guarantee `volatile` locals here; the repair makes all
   values well-defined, which is stronger.
6. **Shadow cursor.** At the `setjmp`, `stacksave` stores the cursor in `jmp_buf.shadow`.
   The sjlj landing pad is the first place that runs in this frame after the unwinder has
   popped the frames above it, and it starts with `stackrestore` of that token. The entry
   watermark alone is not enough: the setjmp frame is not popped, so allocations made in
   it after `setjmp` would otherwise survive (`04-architecture.md` §5.6). The function
   has `usesAlloca = 1` because of these two instructions. Unwound frames were already
   freed by the engine.

### 8.4 `longjmp`

Defined in `crt-cbc.bc` (`10-linker-and-runtime.md` §5.2); there is no libc overlay.

```c
_Noreturn void longjmp(jmp_buf b, int v) {           // also _longjmp, siglongjmp
  struct __cbc_jmp *j = (struct __cbc_jmp *)b;
  struct __cbc_fcb *f = __cbc_fcb();
  f->eh.kind = 2; f->eh.jmp_token = j->token; f->eh.jmp_label = j->label;
  f->eh.jmp_value = v ? v : 1;
  __cbc_raise();
}
```

* C++ cleanups between the `longjmp` and the `setjmp` frame: `__cbc_eh_select` returns 0 for
  cleanup pads, so destructors run (the C++ standard makes skipping them undefined; running
  them is a valid behaviour). Pure catch pads ignore kind 2 and rethrow.
* `longjmp` to a frame that has returned: the token no longer matches any live activation;
  the marker reaches the entry, which reports "longjmp to a returned frame" and aborts
  (native behaviour is undefined).
* `siglongjmp` restores the signal mask with native `sigprocmask` if the `sigsetjmp` saved it.

### 8.5 Functions that are not transformed

A function that does not call `setjmp` needs nothing: the marker unwinds through it like
any exception (and runs its C++ cleanups).

**Where the functions live, and address-taken uses.** The `longjmp` family (`longjmp`,
`_longjmp`, `siglongjmp`, `__longjmp_chk`) is real CBC code in `crt-cbc.bc` (§8.4), so
its address may be taken and called indirectly like any CBC function. The `setjmp` family
(`setjmp`, `_setjmp`, `sigsetjmp`, `__sigsetjmp`) has **no** definition anywhere: a
`setjmp` call only means something at its call site, where `CBCLowerSjLj` rewrites it
(§8.3). Any other use of a `setjmp`-family symbol — taking its address, an indirect call
whose target could be it — is a compile-time error in `CBCLowerSjLj`:
`taking the address of 'setjmp' is not supported on CBC` (C17 7.13.1.1 already
restricts `setjmp` to a few call contexts, so portable code never does this).

## 9. Testing

* `llvm/test/CodeGen/CBC/eh-*.ll`: region tables, landing-pad prologue, typeid numbering,
  clause tables.
* End-to-end (engine required): libc++abi's `test/` subset (catch by base, by pointer,
  rethrow, nested, `exception_ptr`, `noexcept` terminate), plus `setjmp` torture tests
  from the LLVM test-suite (`SingleSource/Regression/C/longjmp*`).
