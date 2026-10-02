# 22 — Method lookup failures in libc++

Two libc++ programs compiled, launched, and died inside the engine while it
resolved a `call.direct`. A third compiled and then flooded a GC error at a
bytecode position that has no liveness entry. All three are calls the backend
emitted as CBC calls. The bodies are either missing from the method table or
present under a signature the engine does not consider equal. This is not an
ISA hole: the same `call.direct` encoding works for `__cbc_image_init`, for
`std::vector`, and for `throw 1`.

No engine change. Lookup is doing what `01-cbc-platform-facts.md` §5 describes.

## 1. The three failures

### 1.1 `std::map` emplace

`std::map<int,int>` insert of two keys aborted in the launcher:

```text
[resolution] [ERROR] Failed to resolve method
@$cbc.ex:Entry._ZNSt3__125__try_key_extraction_impl...__emplace_unique...
```

The mangled name is the lambda inside
`std::__tree::__emplace_unique` / `__try_key_extraction_impl`. The call was
encoded as a CBC call on the entry type (the `Entry.` prefix). The engine
searched that type's method index by name and signature and did not find a
match (`01` §5: the superclass chain is not searched, and the signature term
must be identical).

`std::set<int>` inserts and erases successfully (`cbc-stdlib-tests/cxx/set.cpp`).
The tree itself lowers. The missing piece is this one local lambda, which
`map::operator[]` instantiates and `set::insert` of an `int` does not.

### 1.2 `std::runtime_error`

A `throw std::runtime_error("e")` also failed at resolve time, before any
handler ran:

```text
Failed to resolve method
@$cbc.ex:Entry._ZNSt13runtime_errorC1EPKc(Int64, Int64) -> Int64
```

`throw 1` works (`cbc-stdlib-tests/cxx/throw.cpp`). `catch` of that `int` runs
destructors of locals (`cxx/unwind.cpp`). The constructor
`runtime_error::runtime_error(const char *)` is a libc++abi definition that
should have been extracted from `libc++abi.a` and emitted as a method. The
call site treats it as a CBC method (it is printed as `Entry.<mangled name>`,
not as an AOT native). The definition is not in the table the engine searched,
or it is there with a different signature term.

The `(Int64, Int64) -> Int64` in the message is the engine printing the
signature term. A constructor returns `void`. If the definition's term says
`void` and the call's term says `I64`, lookup fails and the message still
shows the call's type. That mismatch is the first thing to check (§3).

### 1.3 Class throw and a missing liveness position

Throwing a user type with a virtual destructor (a `Derived` with `int code`,
caught as `const Base &`) did not fail at resolve time. The launcher printed,
repeatedly:

```text
[GC] [ERROR] cannot find info for position (fuh=..., ip=..., fp=..., pos=0x2)
```

`01` §5.1: after every `CallDirect` the rewriter records a state point at the
original bytecode offset immediately after the instruction, and
`CalculatePositionalGCInfo` requires a `LivenessInfo` entry at that exact
`cbcPos`. The C target's entries are empty (`regMask = 0`, no slots). An
offset that is not in the table aborts the scan.

`pos=0x2` is two bytes into some method. `call.direct` is at least three bytes
(`44 31` plus a uleb), so this position is not "after a direct call that we
forgot". It is either a different instruction the rewriter treats as a state
point, or an RT position that failed to map back onto an original boundary.
`throw 1` does not take this path. The extra work in the class throw is the
copy constructor and the destructor of the temporary, both CBC methods, and
the typeinfo / vtable loads for the `catch` match. One of those methods has
a state point at original offset 2 with no liveness row, or the row was
emitted at a different offset than the rewriter computed.

## 2. How a call becomes a method reference

`CBCAsmPrinter::encodeCall` records the callee's name, whether it is a
declaration (`isDeclarationForLinker`), the parameter kinds, and `RetFloat`.
`buildCBCFile` then:

* emits every compiled function as a method on `$cbc.ex:Entry`, with a
  signature term built from `CBCCompiledMethod::Params` and `RetFloat`;
* emits a method reference for each call, with a signature term built again
  from the kinds stored on the `CALL` instruction.

The engine matches those two terms exactly. `CBCFileWriter.cpp` `sigTerm`
always uses builtin `I64` (12) or `F64` (19) as the return type. It never
emits `void` (1). Both sides go through that function today, so a constructor
and its call site agree with each other even though both disagree with the
language-level return type. A mismatch appears only if one side is built
differently: a signature written by an older object, a call whose `RetFloat`
immediate was dropped, or a definition whose `Params` string was built from
the LLVM function type while the call was built from the DAG's
`CCValAssign`s and those two lists differ in length or in float-vs-integer.

The parameter string is the likely divergence for the lambda. `LowerCall`
pushes one kind per register argument and skips memory arguments
(`CBCISelLowering.cpp`: the `VA.isMemLoc()` branch does not append to
`Params`). The definition side (`CBCAsmPrinter::runOnMachineFunction`) pushes
one character per LLVM argument, including stack arguments. A lambda with
more arguments than `IR1`–`IR6` plus `FR0`–`FR7` can hold, or a lambda the
DAG classifies differently from the LLVM type, gets two different signature
terms. Lookup prints the call's term and finds no method.

`runtime_error::runtime_error(const char *)` takes `this` and a pointer. Both
fit in integer registers, so this particular constructor should not hit the
stack-argument bug. For it, the definition is the thing to count: if `disasm`
of the linked file has no method of that name, the archive member was not
extracted or LTO deleted the body while leaving the call. If the method is
present, print both signature terms (the method def and the call's method
reference) and compare them.

## 3. Diagnosis before changing code

On a failing `map` binary and a failing `throw std::runtime_error` binary:

1. `disasm` the file and search for the mangled name.
2. If the name is absent from the method list but present as a method
   reference, the body was dropped. Find the pass. `cbc-ld` runs the O2
   pipeline before codegen (`cbc-ld.cpp`). An `available_externally` or
   `linkonce_odr` definition can be deleted as unreferenced if the call was
   rewritten to a declaration, and the backend then still emits a CBC call
   because `isDeclarationForLinker` was true at a different point than the
   reference was formed. The fix is to emit an AOT/native call only for
   callees that are declarations at codegen time, and to keep every callee
   that is a CBC call. A CBC call whose body is gone is a link error, not a
   runtime resolve error: `cbc-ld` should refuse to emit the file.
3. If the name is present twice or the call's signature term differs from the
   definition's, fix the writer so both terms are built by one function of
   `(params, retfloat)` and the call's kind string includes stack arguments.
   The definition's `Params` is the authority (one character per LLVM
   argument, `f` or `i`). The call must use that string, not the shorter
   register-only string `LowerCall` builds. Stack arguments are still passed
   in untyped slots; they still count in the signature term because the
   engine matches the term, not the slot list.
4. For the GC error, `disasm` every method in the class-throw program and
   list `LivenessInfo` `cbcPos` values. Find the method whose code contains
   a state-point instruction that ends at offset 2, or whose rewriter trace
   (a debug engine, or a one-off print in `CalculatePositionalGCInfo`) names
   that method. Add the missing entry. `CBCAsmPrinter` records a state point
   only for `CALL` and `CALL_INDIRECT`. Any other opcode the rewriter treats
   as a state point (`GcPoint`, and any call-like opcode the float or EH
   work adds) needs a row at the same offset. Empty rows are correct for C
   and C++ (`01` §5.1); the bug is a missing row, not a missing root.

Do this diagnosis in that order. The signature-term bug in §3.3 is real in
the writer regardless of which failure it explains: call and definition must
use the same term, and stack arguments are part of the LLVM type. Fix it
even if the map lambda turns out to be a dropped body.

## 4. Signature term

`sigTerm` in `CBCFileWriter.cpp`:

* Keep return type `I64` for integer and `void`, and `F64` for floating
  results. Changing constructors to `void` in the term would also work, but
  only if every existing CBC file is rebuilt and every call site changes in
  the same patch. Staying with `I64` matches every method the current writer
  has already shipped, including `__cbc_image_init`. Do not mix the two.
* The parameter list is `f` or `i` per source-level argument, in order,
  including arguments that `CC_CBC` assigned to the stack. `LowerCall` must
  append those kinds. Today it `continue`s past `VA.isMemLoc()` without
  appending.

A method reference and a method definition that share a name and this term
resolve. A reference with a shorter parameter list does not, and the engine
will keep reporting `Failed to resolve method` with a name that `disasm`
shows as present. That is the map failure mode to confirm or rule out with
the disassembly in §3.

## 5. Dropped bodies

If §3 shows the body is absent:

* Internal and `linkonce_odr` functions that are the callee of a remaining
  CBC `call` must survive the O2 pipeline. They are used. If O2 deleted them,
  the call was not marked as a use (a naked bitcast, a blockaddress, or a
  call through a ConstantExpr that O2 folded to a declaration). Print the
  callee at the point `cbc-ld` finishes O2 and again at codegen; the name
  that disappeared is the bug.
* After O2, a CBC `call` to a declaration that is not in the native list and
  not in the crt is a link error that names the mangled symbol. Runtime
  resolution is too late. `runtime_error::runtime_error(const char *)` should
  fail the link, with the archive member named, if the member was not
  extracted. The extraction loop in `cbc-ld` is the place to look when the
  symbol is in `libc++abi.a` and not in the composite module.

## 6. Acceptance

* `std::map<int,int>` assigns `m[1] = 10` and `m[2] = 20`, iterates in order,
  and `erase`s one key. Add `cbc-stdlib-tests/cxx/map.cpp` back.
* `throw std::runtime_error("boom")` caught as `const std::exception &`
  returns 0 when `what()` is `"boom"`. This also needs a working `std::string`
  return path (`16-float-codegen.md` §6) because `what()` returns a
  `const char *` owned by a `std::string` inside the exception. If `what()`
  still pulls `as_float_helper`, land §6 of document 16 first; the resolve
  error and the float-return error are different and both block this test.
* Throwing a polymorphic `Derived` and catching `const Base &` returns the
  derived field, and the launcher prints no `cannot find info for position`.
  `cbc-stdlib-tests/cxx/rtti.cpp` already covers `dynamic_cast` without a
  throw. This test is the throw.
* `disasm` of each of those three programs: every `call.direct` name appears
  as a method definition, and the two signature terms for that name are
  identical.
