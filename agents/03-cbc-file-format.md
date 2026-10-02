# 03 — The `.cbc` File: Byte-Exact Writer Specification

This is the specification `CBCObjectWriter` (`05-llvm-core-changes.md` §4) implements.
It merges what the engine reads (`src/engine/engine.cpp:103-172`,
`src/engine/decode/reader.cpp`, `src/engine/decode/decoder.cpp`, `src/engine/terms.cpp`)
with what the reference producer writes
(`cbc_compiler/core/assembler/src/com/huawei/excelsior/jet/assembler/cbc/CbcFileEncoder.scala`
and `CbcFileFormat.scala`). Where the two disagree, the engine wins.

All integers are little-endian. `uleb`/`sleb` are LEB128. "Pool offset" means an offset
relative to the start of the pool, which **must** begin at file offset 57 (§2).

## 1. Overall layout

```
offset 0   ┌──────────────────────────────┐
           │ header (57 bytes)            │
offset 57  ├──────────────────────────────┤ ◀── poolOffset (must be 57)
           │ pool: strings, terms, refs,  │     all intra-file references are pool
           │ AOT data, field defs, method │     offsets (u32) or table indices (uleb)
           │ code, method defs, type defs │
           ├──────────────────────────────┤
           │ type index (hash table)      │ ◀── typeIndexOffset (absolute)
           │ 5 AOT tables (hash tables)   │ ◀── *AotTableOffset (absolute)
           │ method-ref offset array      │
           │ field-ref offset array       │
           │ term offset array            │
           │ extension sequence           │ ◀── extensionOffset (absolute)
           │ (pad to 4)                   │
           │ region trailer               │ ◀── regionOffset (absolute)
           └──────────────────────────────┘
```

File size must stay below 4 GiB (all positions are `u32`).

## 2. Header

| Offset | Size | Field | Value written by the C target |
|---|---|---|---|
| 0 | 4 | magic | `43 42 43 01` (`"CBC"`, file version 1) |
| 4 | 4 | `typeIndexOffset` u32 | absolute offset of the type index |
| 8 | 4 | `poolOffset` u32 | **57** |
| 12 | 4 | `directCallAotTableOffset` u32 | absolute |
| 16 | 4 | `virtualCallAotTableOffset` u32 | absolute (empty table) |
| 20 | 4 | `interfaceCallAotTableOffset` u32 | absolute (empty table) |
| 24 | 4 | `staticFieldAotTableOffset` u32 | absolute |
| 28 | 4 | `instanceFieldAotTableOffset` u32 | absolute (empty table) |
| 32 | 4 | `extensionOffset` u32 | absolute offset of the extension sequence |
| 36 | 4 | `regionOffset` u32 | absolute offset of the region trailer |
| 40 | 4 | `mainTypeName` s32 | pool offset of the entry type's name string, or −1 |
| 44 | 4 | `cbcDeps` s32 | −1 (or pool offset of a `':'`-separated string; unused by the engine today) |
| 48 | 4 | `aotDeps` s32 | pool offset of `':'`-separated native library names (e.g. `"z:ssl"` → `libz.so`, `libssl.so`), or −1 when the program needs only libraries already loaded in the launcher process |
| 52 | 4 | `foreignLibs` s32 | −1 |
| 56 | 1 | `coverageId` uleb | `00` |

Why 57: the AOT tables and the `aotDeps` string are read with a hard-coded adjustment
`POOL_OFFSET_ADJUSTMENT = 57` (`src/engine/image/cbc_file.h:912`,
`src/engine/decode/decoder.cpp:16`, `engine.cpp:251`) while everything else uses
`poolOffset`. They agree only if the pool starts right after a 57-byte header, i.e.
`coverageId` is a 1-byte uleb.

## 3. Strings

```
uleb  length
u8[]  bytes        (no terminator, no encoding validation)
```

Referenced by pool offset: `u32` in definitions and references, `uleb` in method tags and
`InitString`. Deduplicate identical strings. Arbitrary binary blobs are legal strings
(used for the data image, `04-architecture.md` §6).

## 4. Terms

### 4.1 Term identifiers

A term id (`RefId<Term>`, encoded `uleb`) is either a builtin (0–19) or
`20 + i` where `i` indexes the term offset array (§12).

| Id | Builtin | Flat size | CBC kind |
|---|---|---|---|
| 0 | `Nil` | — | invalid; "no type" (e.g. no super type) |
| 1 | `Void` | 0 | return type of `void` functions |
| 2 | `Unit` | 0 | not used by the C target |
| 3 | `Nothing` | — | not used |
| 4 | `Bool` | 1 | |
| 5 | `I8` | 1 | |
| 6 | `U8` | 1 | |
| 7 | `I16` | 2 | |
| 8 | `U16` | 2 | |
| 9 | `I32` | 4 | |
| 10 | `U32` | 4 | |
| 11 | `UChar32` | 4 | |
| 12 | `I64` | 8 | |
| 13 | `U64` | 8 | |
| 14 | `IAddr` | 8 | |
| 15 | `UAddr` | 8 | |
| 16 | `BString` | 8 | |
| 17 | `F16` | 2 | |
| 18 | `F32` | 4 | float register |
| 19 | `F64` | 8 | float register |

### 4.2 Term encodings

Each non-builtin term is `u8 tag` + payload; subterm references are `uleb` term ids.

| Tag | Name | Payload | Used by the C target for |
|---|---|---|---|
| `0x01` | `REF` | `uleb nameStringOffset` | owner types of method/field refs (`$cbc.<p>:Prog`, `:Data`, `:Entry`) |
| `0x02` | `AOT_REF` | `uleb nameStringOffset` | owner of native calls and native static fields (`cbc.native`) |
| `0x04` | `VARRAY` | `uleb length`, `uleb elem` | the data image `VArray<U64, N>` |
| `0x06` | `C_POINTER` | `uleb pointee` | optional (pointer parameters may simply be `I64`) |
| `0x0C` | `FUNCTIONAL` | `u8 paramCount`, `paramCount × uleb param`, `uleb ret` | method signatures |
| `0x0D` | `REC` | `uleb nameStringOffset` | the atomic access records `$cbc:A8..A64` |
| `0x12` | `TUPLE` | `uleb count`, `count × uleb elem` | `(U64, U64)` for `InitString` typed slots |
| `0x14` | `FST` | `uleb termId` | fixed-size wrapper around `VARRAY` (as the Cangjie encoder does for `VArray`, `CPointer`, `Box`) |

Other tags (generic types, options, enums, boxes, nullable, type variables, Cangjie
arrays) are not needed. Never emit `CLASS_TYPE_VAR`/`FUNC_TYPE_VAR`, and keep every
method's `arity` at 0: a non-zero arity adds hidden type-info parameters to the ABI
(`cbc_scala_gaps.md` §1.24).

`REF`/`REC` must agree with the definition's kind: a `REF` to a RECORD type (or `REC` to
a CLASS) fails resolution.

Terms are structural; deduplicate them (the reference producer interns terms).

### 4.3 Signature terms for C functions

`FUNCTIONAL(params..., ret)` where each parameter is the CBC builtin of its **lowered**
register class, in call order:

| LLVM type after clang ABI lowering | CBC term |
|---|---|
| `i1`, `i8`, `i16`, `i32` (promoted to a 64-bit register) | `I64` (or the exact narrow builtin; both are "integer register") |
| `i64`, `ptr` | `I64` |
| `float` | `F32` |
| `double` | `F64` |
| `void` return | `Void` |
| two-register return | ret term = `TUPLE(I64, I64)` etc. — metadata only |

Only the order and the integer/float/record classification matter to the engine: for GC
bitmaps (C has none), for `hasTailReg` (which, with E2, counts integer and float stack
overflow correctly from these terms — no padding), and for the choice of `c2i` adapter
(only relevant for the entry method). Using `I64` for every integer argument makes
signature terms maximally shareable. The signature term of a method and of every method
reference to it must be **identical** (direct-call resolution compares them exactly).

## 5. Method references

```
u32   nameStringOffset
u8    flags            SRET=0x01 HAS_THIS_TI=0x02 HAS_OUTER_TI=0x04 MUT=0x08
                       HAS_FTVARS=0x10 AOT=0x20 REC_RECEIVER=0x40 REF_RECEIVER=0x80
uleb  refTypeTermId
uleb  signatureTermId
[uleb tvarsTermId]     only if HAS_FTVARS
```

* CBC callee: name = the CBC method name of the function, refType = `REF $cbc.<p>:Prog`,
  flags = 0.
* Native callee: name = the linkage name, refType = `AOT_REF cbc.native`, flags = `AOT`
  (0x20), plus one direct-call AOT entry (§8) with the linkage name.
* `LoadFuncPtr` (E5) operands are ordinary method references of the two kinds above: the
  CBC method's reference for a CBC function pointer (the engine turns it into the
  function's descriptor), the native function's `AOT` reference for a native function
  pointer. A function that is both called and has its address taken uses **one** reference
  for both instructions (the `DedupPool` behaviour of the reference producer).

The method-reference offset array (§12) gives each reference its index; instructions
encode that index as `uleb`. With E3 the count is a full 32-bit value; an unpatched
engine truncates it to 16 bits (engine quirk Q1).

## 6. Field references

```
u8 tag
  0x00 SINGLE:      u32 nameStringOffset, uleb refTypeTermId, uleb fieldTypeTermId
  0x01 CONST_INDEX: u32 index,            uleb refTypeTermId, uleb fieldTypeTermId
  0x02 MULTI:       uleb count, count × uleb fieldRefIndex
  0x03 NONE:        uleb sigTermId
```

The C target uses `SINGLE` only:

| Purpose | refType | name | fieldType | AOT entry |
|---|---|---|---|---|
| data image (§9.3) | `REF $cbc.<p>:Data` | `image` | `FST(VARRAY(N, U64))` | — |
| data image `k` of a separately compiled program (`04-architecture.md` §6.5) | `REF $cbc.<p>:Data.<k>` | `image` | `FST(VARRAY(N_k, U64))` | — |
| base pointer of an over-aligned image (phase 1: `Data`; phase 2: `Data.<k>`) | `REF $cbc.<p>:Data[.<k>]` | `base` | `U64` | — (in base-pointer mode the type has **only** this field (the image memory comes from `aligned_alloc`), so a direct-mode type always has `image` as its sole static and therefore at offset 0 of its 16-byte-aligned bundle, `01-cbc-platform-facts.md` §8) |
| native data symbol (`stdout`, `environ`, …) | `AOT_REF cbc.native` | linkage name | `U64` | static-field AOT entry with the linkage name |
| atomic access on a raw pointer | `REC $cbc:A{8,16,32,64}` | `v` | `I8`/`I16`/`I32`/`I64` | — |
Native *function* addresses do not use field references (they use `LoadFuncPtr` on a
method reference, §5). With E3 the field-reference count is a full 32-bit value.

## 7. Field definitions

```
u32   nameStringOffset
u8    regionId = 0
uleb  fieldTypeTermId
u8    flags        access: PUBLIC=1 PRIVATE=2 PROTECTED=3 (low 2 bits),
                   STATIC=0x04 FINAL=0x08 VOLATILE=0x10 AOT=0x20
u8    0            tag terminator (constant values are rejected by the engine)
```

## 8. AOT data entries and AOT tables

Each AOT entry lives in the pool:

```
u32   referenceIndex       index into the method-ref or field-ref array
then, by table:
  direct call:     u32 linkageNameStringOffset
  virtual call:    u16 methodNum, u16 extDefNum
  interface call:  u16 interfaceMethodNum
  static field:    u32 linkageNameStringOffset
  instance field:  u32 ordinal
```

Each AOT table is a hash table (§11) keyed by `referenceIndex` with the identity hash,
whose bucket entries are the **pool offsets** of the AOT entries. The engine scans the
bucket and compares `u32` at `entryOffset + 57` with the reference index
(`FindAotData`, `decoder.cpp`). The C target fills the direct-call and static-field tables;
the other three are empty tables (`bucketTableSize = 1`, `bucketsSize = 0`, one `u32 0`).

## 9. Method code

### 9.1 Code block

```
uleb   untypedSlotCount        8-byte frame slots (outgoing args first, then spills)
uleb   stackAllocSigsCount
uleb[] stackAllocSigs          term ids of RECORD types (typed frame slots)
uleb   0                        "ohmSlotCount" — must be a single 0 (engine quirk Q14)
u8     usedNonVolIRegMask      bit i = IR(8+i), i in 0..5 (bit 5 = IR13, an ordinary
                               callee-saved register; E8)
u8     usedNonVolFRegMask      bit i = FR(8+i), i in 0..7
uleb   maxCalleeStackArgsCount
u8     mayHaveNativeCalls      0/1
uleb   untypedMemSize          E8: byte size of the frame's untyped memory block, a
                               multiple of 16, 0 if the method has none. The Cangjie
                               encoder writes 0 (`13-engine-changes.md` §11)
u8     usesAlloca              E8: 1 if the method contains alloca, stacksave or
                               stackrestore; 0 otherwise. The Cangjie encoder writes 0
uleb   codeSize
uleb   0                        literalsOffset (unused)
u8[]   code                    original CBC bytecode, codeSize bytes
uleb   exTableBytes
  repeat: uleb start, uleb end, uleb target          original positions
uleb   livenessBytes
  repeat: uleb cbcPos, u16 regMask, uleb nSlots, nSlots × uleb slot,
          uleb nPairs, nPairs × (uleb base, uleb derived)
uleb   stackPtrsBytes
  repeat: uleb cbcPos, uleb nResources, nResources × uleb resource
```

For C functions:

* `stackAllocSigs` is empty except in functions that use `InitString` (the startup
  function), which declare one `TUPLE(U64, U64)` slot per blob.
* `untypedMemSize` is the static addressable frame (`04-architecture.md` §5.2): allocas
  that survived `SROA`/`mem2reg`, byval copies, sret buffers, variadic buffers. It is not
  a slot count. `usesAlloca` is 1 exactly when the bytecode contains `alloca`,
  `stacksave` or `stackrestore` (including the `stacksave` a `setjmp` function emits).
* Liveness: one entry `{pos, 0x0000, 0, 0}` for every state point, i.e. for the position
  right after every `CallDirect`/`CallIndirect`/`GcPoint`. Entries are sorted by position.
* Stack pointer info: empty (`uleb 0`).
* Exception table: one triple per protected call (`08-exceptions-and-sjlj.md` §4).

### 9.2 Method definitions

```
u32   nameStringOffset
u32   typeNameStringOffset      owner type name (stack traces)
u8    regionId = 0
uleb  signatureTermId
u16   flags
tags (u8 tag, payload), terminated by u8 0:
  0x01 code            uleb poolOffset of the code block
  0x02 sourceFullName  uleb stringOffset
  0x03 sourceFile      uleb stringOffset
  0x04 linkageName     uleb stringOffset
  0x05 arity           uleb (never emitted by the C target)
```

Method flags (`u16`): access `PUBLIC=1 PRIVATE=2 PROTECTED=3`, `STATIC=0x0004`,
`FINAL=0x0008`, `FOREIGN=0x0010`, `ABSTRACT=0x0020`, `MUT=0x0040`, `VIRTUAL=0x0080`,
`AOT=0x0100`, `PKG_INIT=0x0200`, `LIT_INIT=0x0400`, `SRET=0x0800`, `HAS_THIS_TI=0x1000`,
`HAS_OUTER_TI=0x2000`, `REC_RECEIVER=0x4000`, `REF_RECEIVER=0x8000`.

C functions: `PUBLIC | STATIC` (0x0005) with a `code` tag. Emit `sourceFile` and
`sourceFullName` together or not at all (engine quirk Q12). The method name is the
function's symbol name (C++: the mangled name; internal symbols get a unique suffix at
link time).

### 9.3 Type definitions

```
u32   nameStringOffset
u8    regionId = 0
u16   flags        PUBLIC=0x001 FINAL=0x002 ABSTRACT=0x004 SEALED=0x008
                   INTERFACE=0x010 LAMBDA=0x020 RECORD=0x040 AOT=0x080
                   PATCH=0x100 ENUM=0x200            (kind = CLASS when no kind bit)
uleb  superOrEnumTermId        0 (Nil) for the C target's types
hash index of ALL methods       keyed by method name, values = method-def pool offsets
offset sequence                 virtual methods in slot order
hash index of STATIC fields     keyed by field name, values = field-def pool offsets
offset sequence                 instance fields in layout order
tags, terminated by u8 0:
  0x01 interfaces      ref sequence of term ids
  0x05 arity           uleb
  0x06 union fields    ref sequence (union enum)
  0x07 OPTION0, 0x08 OPTION1, 0x09 PRIMITIVE (enum kinds, no payload)
```

A *sequence* is `uleb totalBytes` followed by that many bytes of `uleb` values (pool
offsets for offset sequences, term ids for ref sequences) — `Utils.writeSequence` in the
Scala encoder, `ReadOffsSeq`/`ReadRefSeq` in the engine.

The field hash index holds **static** fields only; instance fields are reached through
the ordered instance-field sequence (that is what the reference producer emits and what
`StaticsManager` and the layout manager consume).

### 9.4 Types emitted by the C target

| Name | Kind / flags | Members |
|---|---|---|
| `$cbc.<prog>:Prog` | CLASS, `PUBLIC` | one static method per CBC-compiled function |
| `$cbc.<prog>:Entry` | CLASS, `PUBLIC` | exactly one method `main() -> I64`, static (the only member named `main`) |
| `$cbc.<prog>:Data` | CLASS, `PUBLIC` | one static field `image : FST(VARRAY(N/8, U64))` |
| `$cbc:A8`, `$cbc:A16`, `$cbc:A32`, `$cbc:A64` | RECORD (`0x041`) | one instance field `v : I8/I16/I32/I64` |
`<prog>` is a program identifier chosen by the linker (output file stem plus a hash) so
that type names stay globally unique in a process that loads several `.cbc` files. The
`$cbc:A*` records are identical in every program and may be shared by name.

## 10. Extensions

Not used. Emit an empty sequence: `uleb 0`.

## 11. Hash tables

Format (type index, per-type method/field indices, AOT tables):

```
u32   bucketTableSize      = bucketCount + 1
u32   bucketsSize          = number of entries N
u32[] bucketTable          bucketTableSize entries; bucket b spans
                           entries[bucketTable[b] .. bucketTable[b+1])
u32[] entries              N pool offsets, grouped by bucket
```

Lookup (`FindBucketRange`, `decoder.cpp`): if `N == 0` the table is empty; otherwise
`bucket = hash(key) % (bucketTableSize - 1)`.

Hash functions:

* names: `uint32_t h = 0; for each byte b: h = h * 31 + b` (written as
  `(h << 5) - h + b`, wrapping 32-bit),
* AOT tables: the reference index itself.

Bucket count used by the reference producer: `max(floor(N / 0.75), N)`, at least 1 when
`N > 0`. Any count ≥ 1 is valid. An empty table is `u32 1, u32 0, u32 0`.

## 12. Index arrays and the region trailer

After the hash tables:

```
u32[] methodRefOffsets    pool offsets; index i = method-ref id i
u32[] fieldRefOffsets     pool offsets; index i = field-ref id i
u32[] termOffsets         pool offsets; index i = term id 20 + i
uleb  0                    extension sequence (extensionOffset points here)
pad with zero bytes to a multiple of 4
region trailer (regionOffset points here):
  u16  0                   typeIdxSize (unused)
  u32  0                   typeIdxOffs (unused)
  uleb methodRefCount
  u32  absolute offset of methodRefOffsets
  uleb fieldRefCount
  u32  absolute offset of fieldRefOffsets
  uleb termCount
  u32  absolute offset of termOffsets
```

`ReadRegion` (`reader.cpp:497-517`) reads the two reference counts as `uint32_t` on the
patched engine (E3); the unpatched engine truncates them to `uint16_t` (engine quirk Q1).

## 13. Pool ordering for a single-pass writer

Every forward reference in the format is either a table index (`uleb`, known when the
entity is registered) or a pool offset. Laying out the pool so that every referenced item
precedes every referencing item removes the need for multiple passes:

1. **Strings** (all of them, including blobs). Registered on demand; an offset is final at
   registration because the string region is the first region of the pool.
2. **Terms.** Reference strings by offset and other terms by id.
3. **Method references, field references.**
4. **AOT data entries.**
5. **Field definitions.**
6. **Method code blocks.** Need final instruction layout and label positions.
7. **Method definitions.** Reference code blocks by `uleb` pool offset.
8. **Type definitions.** Reference method/field definitions by `u32` (hash indices) and
   `uleb` (sequences).

Because the string region comes first, the `uleb` string offsets embedded in code
(`InitString`) have their final size at instruction-encoding time.

## 14. Size and count limits

| Item | Limit | Reason |
|---|---|---|
| file | < 4 GiB | `u32` positions |
| method references | 2^32 − 1 (E3; 65 535 on an unpatched engine) | `uint32_t` count |
| field references | 2^32 − 1 (E3; 65 535 on an unpatched engine) | `uint32_t` count |
| parameters per signature | 255 | `u8` arity in `FUNCTIONAL` |
| untyped slots | 65 535 (E1; only 512 storable on an unpatched engine) | `u16` slot index |
| untyped memory block | whatever still fits in the `u32` frame size, multiple of 16 | E8; not counted in slots |
| typed slots | `uleb` count; frame size `u32` | |
| literal-consuming instructions per method | ~3 500 | 4 096-entry RT literal table (not changed) |
| distinct address-taken CBC functions, process-wide | ≤ 1 048 576 (descriptor region reservation; not a practical limit) | E5 descriptors (`13-engine-changes.md` §9) |
| code size per method | `uleb`, < 4 GiB | |

## 15. Minimal example: what `int main(void){return 42;}` becomes

Entities (string offsets shown symbolically):

```
strings:  s0="$cbc.ex:Entry"  s1="main"  s2="$cbc.ex:Prog"  s3="__cbc_c_main" ...
terms:    t20 = FUNCTIONAL(0 params; ret I64)          bytes: 0c 00 0c
          t21 = REF s2                                  bytes: 01 <uleb s2>
mrefs:    m0  = { s3, flags 0, refType t21, sig t20 }
code (Entry.main):
          44 31 00        call.direct IR1, @m0        (C main via the startup path)
          45 11           ret.W64 IR1
          liveness: { cbcPos 3, regMask 0, 0 slots, 0 pairs }
code (Prog.__cbc_c_main):
          17 1a 02        mov.W64 IR1, 42             ([d=1 : lo4=0xA], sleb(2))
          45 11           ret.W64 IR1
```

The real entry method also runs the startup sequence (`10-linker-and-runtime.md` §6).
