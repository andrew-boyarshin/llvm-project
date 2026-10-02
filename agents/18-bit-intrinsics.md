# 18 — Population count, leading/trailing zeros, and rotates

`06-backend.md` §4.2 tells the legalizer to expand `CTPOP`, `CTLZ`, `CTTZ`,
`BSWAP`, `BITREVERSE`, and `ROTL`/`ROTR` into shifts, masks, and multiplies. The
constructor in `CBCISelLowering.cpp` never sets those actions. On a type that has a
register class the default action is Legal, so the nodes survive into `Select` and
die in the default arm:

```text
CBC cannot select: i64 = ctpop
CBC cannot select: i64 = ctlz
CBC cannot select: i64 = rotr
```

There is no CBC opcode for any of these, and none should be added. The expander
already knows the sequences.

## 1. What each failure is

| Node | libc++ source | What it blocks |
|---|---|---|
| `ctlz` | `libcxx/include/__bit/countl.h`, `__countl_zero<unsigned long>` | `std::sort`. Introsort computes its recursion limit with `countl_zero`. `cbc-stdlib-tests` dropped `std::sort` for this reason. `std::array` plus `std::sort` fails the same way |
| `ctpop` | `std::bitset<N>::count` | `bitset::count()`. `set`, `test`, `flip`, and `to_ulong` already run; only the popcount method does not |
| `rotr` | `libcxx/include/__functional/hash.h`, `__murmur2_or_cityhash::__rotate` | `std::hash`, therefore `std::unordered_map` and `std::function`'s target type id. The rotate is `llvm.fshl` / `llvm.fshr` lowered to `ISD::ROTR` when the two arms are the same value |

`std::bitset` methods other than `count`, and `std::set` (a tree, not a hash), already
pass. This document is only the bit-intrinsic expansion.

## 2. Actions

In `CBCTargetLowering::CBCTargetLowering`, for `MVT::i32` and `MVT::i64`:

```text
Expand: CTPOP CTLZ CTTZ CTLZ_ZERO_UNDEF CTTZ_ZERO_UNDEF
        BSWAP BITREVERSE
        ROTL ROTR
        FSHL FSHR
```

`FSHL` and `FSHR` have to be expanded as well. libc++ writes the rotate as a funnel
shift of a value with itself. If `ROTR` expands into `FSHR` and `FSHR` is still
Legal, selection fails one node later. The generic funnel-shift expansion is two
shifts and an `or`, which this target already selects (`SHL64`, `SRL64`, `OR64`).

`i8` and `i16` are promoted to `i32` before the expander runs. Do not add actions
for them.

Leave the operations Expand. Do not add `Select` cases, and do not emit a helper
call. A libcall would be a literal in every hashed lookup.

## 3. What the expander must be able to select

Confirm, on a one-function program, that the expanded DAG uses only operations
`Select` already handles:

* `CTLZ` / `CTTZ`: a binary search of shifts and masks (`TargetLowering::expandCTLZ`).
  Shifts, `and`, `or`, `setcc`, `select`. All present for integers.
* `CTPOP`: the SWAR sequence, which multiplies by a constant (`0x0101010101010101`
  style). `MUL64rr` and `MUL64ri` exist. The multiply immediate is outside the 12-bit
  field, so it becomes one literal. That is one literal per popcount, not one per
  bit. Acceptable.
* `ROTR` by a variable amount: `(x >> n) | (x << (64 - n))` after the funnel-shift
  expansion. The shift amount is masked to 6 bits by the ISA (`02` §3). The expander
  masks as well. Both masks are correct; a second `and` with 63 is harmless.
* `ROTR` by a constant: the same, with `SRL64ri` / `SHL64ri`.

`BSWAP` and `BITREVERSE` are in the same action list because `06` groups them with
`CTPOP` and the first use (a byte swap of a network short, or `std::byteswap`) would
otherwise fail the same way. No current stdlib test hits them.

## 4. Zero-undef

`ctlz` and `cttz` have a zero-undef flag. libc++ passes `false` (the result for a
zero input is the bit width). Expand `CTLZ_ZERO_UNDEF` and `CTTZ_ZERO_UNDEF` too so
a later optimization that sets the flag still lowers. The defined-on-zero form is
the one `std::countl_zero(0)` must satisfy: the result is 64 for `unsigned long`.

## 5. Acceptance

* `std::bitset<8>("10110001").count() == 4`.
* `std::sort` of `{4, 1, 3, 2}` yields `{1, 2, 3, 4}`. Put this back in
  `cbc-stdlib-tests/cxx/algorithm.cpp`.
* `std::unordered_map<int, int>` inserts 50 keys and finds them. This is the hash
  rotate. It also depends on `19-static-initialization.md` only if the translation
  unit grows a static destructor; the container itself does not.
* `std::countl_zero(0ul) == 64` and `std::countl_zero(1ul) == 63`.
* Disassembly of the popcount and the rotate contains `lsl` / `lsr` / `and` / `mul`
  and does not contain an unknown opcode.
