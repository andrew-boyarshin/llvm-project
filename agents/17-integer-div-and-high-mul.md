# 17 — Integer division, remainder, and signed high multiply

`06-backend.md` §4.2 lists `SDIV UDIV SREM UREM` as Legal, lowered to `Binary32/64`
and `BinaryImm32/64` with `Common` opcodes 6, 8, 7, and 9 (`02-isa-encoding.md` §3,
§4.3). `MULHS` and `MULHU` are listed as Expand. The backend implements only the
unsigned high half, and it implements none of the four division operations.

No engine change. The interpreter already executes `sdiv`, `udiv`, `srem`, and `urem`
(`02` §3). Division by zero is the engine's existing assert; the C target does not
have to emit a check.

## 1. What failed

| Observation | Cause |
|---|---|
| `libcxx/src/chrono.cpp:137` cannot select `i64 = mulhs` of a constant magic multiplier | `std::chrono::system_clock::now` reduces a nanosecond count with a signed high multiply. `Select` has a case for `ISD::MULHU` and none for `ISD::MULHS` |
| An `i64 = urem` reached `Select` while linking C++ tests | `UREM` is left at the default Legal action, and there is no `UREM64` instruction |

C programs in `cbc-stdlib-tests` do not divide, so the suite does not catch this.
`/` and `%` on `int` or `long` in guest code, and any libc++ algorithm that reduces
an integer modulo a bucket count, will hit the same fatal.

## 2. Division and remainder

Mirror the existing `MUL64rr` / `MUL64ri` pattern.

| ISD | `Common` op | Register form | Immediate form |
|---|---|---|---|
| `SDIV` | 6 | `36 [6:d] [l:r]` | `38 [6:d] [l:lo4] sleb(imm>>4)` |
| `SREM` | 7 | `36 [7:d] [l:r]` | `38 [7:d] [l:lo4] sleb` |
| `UDIV` | 8 | `36 [8:d] [l:r]` | `38 [8:d] [l:lo4] sleb` |
| `UREM` | 9 | `36 [9:d] [l:r]` | `38 [9:d] [l:lo4] sleb` |

Opcode `0x36` is `Binary64`, opcode `0x38` is `BinaryImm64`. The encoder for
`ADD64rr` / `ADD64ri` already writes this layout (`CBCEncoding.cpp`, `appendBinImm`
for the immediate form). The immediate is sign-extended by the ISA; that is correct
for `sdiv`/`srem`. For `udiv`/`urem` of an immediate greater than `2^63-1`, materialize
the constant with `MOV64ri` and use the register form. Values that fit in a signed
12-bit field after the nibble split can stay immediate; the existing `addi`/`muli`
path already does that split.

`i32` division is the `0x35` / `0x37` forms (`Binary32`, `BinaryImm32`). W32 results
leave bits 63..32 undefined (`02` §4.3). The legalizer promotes `i8`/`i16` to `i32`
before this. Mark `SDIV UDIV SREM UREM` Legal for `i32` and `i64` in
`CBCTargetLowering`'s constructor, next to `MUL`.

`SDIVREM` and `UDIVREM` stay Expand (`06` §4.2): the legalizer turns them into a
divide and a remainder. Do not add a combined instruction.

Shifts mask the count to 6 bits on W64. Division does not mask. Emit the operation
the program wrote.

## 3. Signed high multiply

`ISD::MULHU` is expanded in `Select` by a 32×32→64 schoolbook product
(`CBCISelDAGToDAG.cpp`, the `MULHU` case): four `MUL64rr`s of the low and high
halves, then the high 64 bits of the 128-bit sum. That expansion is unsigned. Using
it unchanged for `MULHS` is wrong: the high halves of negative numbers are sign bits,
and the cross terms must be subtracted.

For two's-complement `a` and `b`,

```text
mulhs(a, b) = mulhu(a, b) - (a < 0 ? b : 0) - (b < 0 ? a : 0)
```

where `<` is a signed compare and the values subtracted are the full 64-bit operands,
not their high halves. Implementation, reusing the existing `MULHU` schoolbook as a
helper that returns an `SDValue`:

1. `hu = mulhu(a, b)` via the code that is already in the `MULHU` case.
2. `sa = sra a, 63` and `sb = sra b, 63` (`SRA64ri`). These are `0` or `-1`.
3. `adj = (sa & b) + (sb & a)` with `AND64rr` and `ADD64rr`. `sa & b` is `b` when
   `a < 0` and `0` otherwise.
4. `mulhs = hu - adj` with `SUB64rr`.

Factor the schoolbook into a lambda in `Select` and call it from both `MULHU` and
`MULHS`. Do not mark `MULHS` Expand: the generic expander calls back into `MULHU`
plus the same correction, but only when `MULHU` is Legal or Expand. Today `MULHU`
is expanded inside `Select`, after the legalizer, so the generic expander would not
see it. Keeping both in `Select` matches the code that already works for `MULHU`.

`SMUL_LOHI` and `UMUL_LOHI` can stay Expand. Nothing in the chrono failure needs
them. If they reach `Select` later, the low half is `MUL64rr` and the high half is
the corresponding `MULH*`.

## 4. Acceptance

* Guest `20 / 3 == 6`, `20 % 3 == 2`, `(-20) / 3 == -6`, `(-20) % 3 == -2`, and the
  unsigned forms `(unsigned long)-1 / 2` and `% 2`. One function, integer compares
  only.
* `std::chrono::system_clock::now()` returns a time point whose `time_since_epoch`
  count is positive. That is the `mulhs` site in `chrono.cpp`. The existing
  `cbc-stdlib-tests/cxx/chrono.cpp` only adds `seconds` and does not call `now`;
  add the `now()` call once this compiles.
* Neither function may contain a libcall to `__divdi3`, `__udivdi3`, `__moddi3`, or
  `__multi3`. Those compiler-rt entry points are the wrong lowering: the ISA
  instruction is one bytecode op and does not consume a literal.
