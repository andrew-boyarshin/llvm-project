# 20 — `setjmp` dies in live-variable analysis

`CBCLowerSjLj` rewrites `setjmp` into the marker-exception protocol from
`08-exceptions-and-sjlj.md` §8, and the runtime half of that protocol is already in
`crt-cbc.c` (`__cbc_setjmp`, `__cbc_sjlj_test`, `__cbc_sjlj_value`, `longjmp`). The
pass then produces a function that instruction selection cannot finish.

No engine change. `longjmp` already raises through `__cbc_raise`. The bug is the
CFG the pass hands to SelectionDAG.

## 1. The failure

`cbc-stdlib-tests/c/setjmp.c` was:

```c
int r = setjmp(buf);
if (r == 0)
  longjmp(buf, 2);
return r == 2 ? 0 : 3;
```

`cbc-ld` aborts in live-variable analysis on `@__cbc_main`:

```text
LiveVariables.cpp:141: Assertion
`MBB != &MF->front() && "Can't find reaching def for virtreg"' failed.
```

The assertion walks predecessors looking for the definition of a virtual register
that is used in some block. It reached the function entry without finding that
definition. The IR is therefore either not SSA after the pass, or instruction
selection of this CFG creates a virtual register use in a block that does not
see the definition. The function is small enough that the second case is still
the pass's CFG: there is no other transformation unique to this input.

The test was removed from the suite so the suite could pass. This document puts
it back.

## 2. What the pass builds

`lowerFunction` in `llvm/lib/Target/CBC/CBCLowerSjLj.cpp`:

1. Finds direct calls to `setjmp`, `_setjmp`, `sigsetjmp`, `__sigsetjmp`. An
   indirect call, or any use that is not a call, is a fatal error. Keep that.
2. Allocates an `i8` token in the entry block and uses its address as the integer
   token passed to `__cbc_setjmp`.
3. Splits the block after each `setjmp` call. The call's result becomes a PHI at
   the start of the continuation: incoming 0 from the `setjmp` block, incoming
   the longjmp value from a per-site block that is only reached from the landing
   pad.
4. Builds one landing pad for the whole function, a chain of `icmp eq` blocks
   that compare `__cbc_sjlj_test(token)` with `site + 1`, and a `sjlj.notours`
   block that calls `__cbc_continue_unwinding` and then `unreachable`.
5. Turns every other call in the function into an `invoke` whose unwind edge is
   that landing pad. Runtime helpers (`__cbc_setjmp`, `__cbc_sjlj_test`,
   `__cbc_sjlj_value`, `__cbc_raise`, …) are left as calls. `longjmp` is not in
   that list, so it becomes an `invoke`. That is what delivers the marker into
   the pad.

The PHI's incoming block is captured before step 5. Step 5 calls
`splitBasicBlock` on whatever block still contains an ordinary call. LLVM updates
PHI operands in the successors of the block being split, so a split of the
`setjmp` block itself would retarget the PHI. The `__cbc_setjmp` call is classified
as a helper and is not split, so that particular retarget does not happen for the
test above. A `setjmp` block that also contains an ordinary call (anything that is
not a helper) is split, and the PHI incoming can be left naming a block that is
no longer the predecessor. That is a concrete SSA break for any `setjmp` that is
not the only call in its block. The single-call test can still fail for a
different dominance reason; both have to be fixed.

The landing pad is one shared block. Every `invoke` unwinds to it, including
invokes that are inside blocks created by earlier splits. A value defined by an
`invoke` (its result) is not available in the pad. The pad does not use those
results. SelectionDAG's `FindUnwindDestinations` and live-variable analysis are
less forgiving: a vreg that the selector materializes while lowering an invoke
can be treated as live across the unwind edge. The assertion is the symptom when
that vreg's definition does not dominate the pad.

## 3. Required shape

After `CBCLowerSjLj::lowerFunction` returns, the function must pass the LLVM IR
verifier (`verifyFunction`). Run it in the pass, in asserts builds, before
returning. The failures to reject:

* A PHI incoming block that is not a predecessor.
* A use that does not dominate.
* An `invoke` whose unwind destination is not an EH pad, or whose normal
  destination is the pad.
* A landing pad that is not the first non-PHI instruction of its block.
  `CBCLowerGlobals` already had to preserve that property
  (`CBCLowerGlobals.cpp`, the landing-pad clause walk). This pass creates the
  pad and must not insert anything in front of it afterwards.

Then instruction selection of that IR must pass `MachineVerifier` and
`LiveVariables`. The fix is in the IR the pass emits, not in a special case
inside `LiveVariables`.

Concrete changes to the pass:

1. Capture PHI predecessors after the invoke rewrite, or rebuild the PHI incoming
   list from `predecessors(Cont)` once the splits are done. The incoming value
   from the `setjmp` path is the constant 0; the incoming value from the site's
   `sjlj.val` block is the `__cbc_sjlj_value()` result. No other predecessor is
   allowed. If a split inserted one, that is a bug in the rewrite, not something
   the PHI should paper over.
2. Do not convert calls that are in the landing pad, in `sjlj.val`, or in
   `sjlj.notours`. The pass already skips those blocks (`Skip`). Keep the set
   updated when new blocks are created. A call inside `sjlj.val` (there is one:
   `__cbc_sjlj_value`) is a helper and would be skipped anyway; do not let a
   future non-helper land there without being in `Skip`.
3. Mark `longjmp` `noreturn` on the declaration the pass sees, and still emit the
   `invoke`. The normal successor of a `noreturn` invoke is unreachable. Do not
   leave a path from that successor back into the PHI with an undefined incoming
   value. The test's source has no code after `longjmp`; the rewriter must not
   invent a fall-through that the PHI does not know about.
4. Give the function the personality `__gcc_personality_v0` only when it does not
   already have one (`08` §8). The pass does this. The personality is the crt
   abort stub for a non-sjlj exception that landed here; `__cbc_continue_unwinding`
   is what actually resumes the search. Do not replace the personality with
   `__gxx_personality_v0` in a function that also has C++ catch clauses. A
   function with both `setjmp` and a C++ `try` is specified in `08` §8.5: the
   landing pad distinguishes the marker (`exn == &__cbc_sjlj_marker`) from a C++
   exception. The current pass always makes its own pad and will disagree with
   `CBCLowerEH` if both ran. `CBCTargetMachine` runs `CBCLowerEH` before
   `CBCLowerSjLj`. A function that contains both must have one pad, built by
   `CBCLowerEH`, with the sjlj test added as an extra clause. Until that merge
   exists, a program that uses both is rejected with a fatal error that names
   the function, instead of silently producing two pads.

## 4. Token and sites

The token is the address of an entry-block `alloca i8`, which lives in the function's
untyped memory block (`04-architecture.md` §5.2). `__cbc_setjmp` stores the token in
the `jmp_buf` (`crt-cbc.c`). `__cbc_sjlj_test` compares it with `fcb->eh.jmp_token`.
Site ids are `0 .. n-1` in source order, and the test returns `site + 1`, with 0
meaning "not our longjmp". That matches the crt. Do not renumber sites after
splitting blocks.

At the `setjmp`, emit `stacksave` and store that cursor at byte offset 24 of the
`jmp_buf` (`08` §8.2, the `shadow` field). The sjlj landing pad begins with
`stackrestore` of that word, after the unwinder has freed every popped frame. The
setjmp frame itself is not popped, so this restore is what frees `alloca`s made in it
after the `setjmp`. The function's `usesAlloca` bit is 1 because of these instructions.

`jmp_buf` must be large enough for the words the crt writes: a flag at `unsigned long`
index 0, the token at index 1, the site at byte offset 16 (`unsigned` index 4), and
the shadow cursor at byte offset 24. The host `jmp_buf` from `<setjmp.h>` is larger
than that on x86-64. Using the host type is correct; do not invent a smaller one.

## 5. Acceptance

* The program in §1 returns 0 under `launcher`.
* `setjmp` / `longjmp` across one CBC callee: `a` calls `setjmp`, calls `b`, `b`
  calls `longjmp`. The return value is the value passed to `longjmp`, and a
  destructor of a local in `a` between `setjmp` and the call runs. That is the
  marker-exception cleanup from `08` §8.
* Taking the address of `setjmp` is still a compile-time fatal error.
* A function that contains both `setjmp` and a C++ `catch` is a compile-time
  fatal error until the pads are merged. Add the merge as a follow-up in this
  file rather than shipping two pads that `LiveVariables` will also reject.
