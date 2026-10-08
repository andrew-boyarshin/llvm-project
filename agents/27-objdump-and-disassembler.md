# 27 — `llvm-objdump` and the CBC Disassembler

What it would take for `llvm-objdump` to disassemble CBC bytecode, including CBC
embedded in today’s wrap ELFs, and how much of `decodeCBCInst` can be driven by
TableGen. Complements `05-llvm-core-changes.md` §4.4 / §6, `06-backend.md` §13,
`10-linker-and-runtime.md` §9, and review finding F-25 in `23-review-findings.md`.

Paths below are under `llvm/` unless noted. Engine paths are under `cbc-engine/`.

---

## 1. Current state

### 1.1 What already exists

| Piece | Location | Status |
|---|---|---|
| Triple `Triple::cbc`, default object format `Triple::CBC` | `include/llvm/TargetParser/Triple.h`, `lib/TargetParser/Triple.cpp` | Done |
| Target registration `"cbc"` | `lib/Target/CBC/TargetInfo/CBCTargetInfo.cpp` | Done |
| InstPrinter | `lib/Target/CBC/MCTargetDesc/CBCInstPrinter.cpp` | Registered; covers opcodes present in `.td` |
| Hand-written encoder | `lib/Target/CBC/MCTargetDesc/CBCEncoding.cpp` `encodeCBCInst` | Many ALU/mem/branch ops; CALL / `ld.fnptr` often emitted as raw bytes from `CBCAsmPrinter` |
| CBC object writer | `lib/MC/CBCObjectWriter.cpp`, `MCCBCStreamer` | Writes the single CBC text section as a `.cbc` container (not ELF) |
| `CBCDisassembler` + registry | `lib/Target/CBC/Disassembler/CBCDisassembler.cpp` | Registered via `LLVMInitializeCBCDisassembler` |
| `decodeCBCInst` | `CBCEncoding.cpp` | **Stub**: only `MOV64rr` (`0x15`), `MOV64ri` (`0x17`), `RET` (`0x45 0x11`) |
| llvm-objdump linkage | `tools/llvm-objdump` links `AllTargetsDisassemblers` | Calls `InitializeAllDisassemblers()`; CBC is included **if** built |

CBC is not in `LLVM_ALL_TARGETS` / `LLVM_ALL_EXPERIMENTAL_TARGETS`; it builds only with
`-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=CBC`.

### 1.2 What does not exist

- `EM_CBC` in `BinaryFormat/ELF.h`
- `file_magic::cbc_object` / recognition of `CBC\x01` in `Magic.cpp`
- `CBCObjectFile` (`05` §6 planned; not implemented)
- ObjectYAML / `ELFRelocs/CBC.def` / `ELFObjectFile::getArch` CBC cases
- Complete ISA decoder; `-gen-disassembler` is not enabled in `lib/Target/CBC/CMakeLists.txt`

### 1.3 How CBC actually lands in ELF today

LLD wrap (`lld/CBC/CBCWrap.cpp`):

1. Rebuilds a **host** triple from the CBC subarch (`hostTripleFromCBC`).
2. Embeds the finished `.cbc` bytes as `__cbc_blob` (typically `.rodata`, **not**
   `SHF_EXECINSTR`).
3. Emits host N2C stubs in `.text` and an `.init_array` ctor that calls
   `engine_load_buffer`.
4. Native `ld.lld -shared` produces a host DSO.

`lld/ELF/InputFiles.cpp` maps `Triple::cbc` → `EM_X86_64` or `EM_AARCH64`. So every
ELF that currently contains CBC bytecode has a **host** `e_machine`. Objdump’s default
disassembler is the host one.

The `.cbc` container layout (`03-cbc-file-format.md`, `CBCFileWriter.cpp`) is:

```
[0, 57)   header  magic CBC\x01
[57, …)   unified pool (strings, terms, method defs, code blocks, …)
then      type index, AOT tables, offset arrays, region trailer
```

A **code block** is not a raw instruction stream. It has a ULEB/u8 preamble
(untyped slots, typed slots, masks, `codeSize`, …) then `u8[codeSize]` bytecode, then
exception / liveness / stack-pointer tables. Method defs point at that block via a pool
offset. Feeding the whole blob to `MCDisassembler` mixes header and pool with code.

### 1.4 How `llvm-objdump` fails today

| Command | Result |
|---|---|
| `llvm-objdump -d wrap.so` | Disassembles host `.text` (N2C stubs). `__cbc_blob` is data → skipped. |
| `llvm-objdump -D wrap.so` | Also dumps `.rodata` as **host** instructions (garbage over the CBC blob). |
| `llvm-objdump -D -triple=cbc -j .rodata wrap.so` | If CBC is built: stub decoder over the whole container → mostly `<unknown>`. |
| `llvm-objdump -d foo.cbc` | `invalid_file_type` — magic not recognized. |

Selection path (`tools/llvm-objdump/llvm-objdump.cpp`): open via `createBinary` →
`Obj->makeTriple()` from `e_machine` (or `--triple=`) → `createMCDisassembler` → walk
sections with `Section.isText()` (unless `-D` / `-j`). Registration is not the missing
piece once CBC is in the build.

---

## 2. Paths to useful `-d`

There are two different products. They do not share the ELF machine-type path.

### Path A — `llvm-objdump -d a.cbc` (design plan; not ELF)

Precedent: WebAssembly / DXContainer (custom `ObjectFile`, not `EM_*`).

| Work | Files |
|---|---|
| `file_magic::cbc_object` on `CBC\x01` | `BinaryFormat/Magic.h`, `Magic.cpp` |
| Header / tag constants | new `BinaryFormat/CBC.h` (planned in `05` §3) |
| `CBCObjectFile : ObjectFile` | new `Object/CBCObjectFile.{h,cpp}` — one `isText()` section per method; contents = bytecode only; symbols at method starts; `getArch() → Triple::cbc` |
| Dispatch | `Object/ObjectFile.cpp`, `Object/Binary.cpp` |
| Complete `decodeCBCInst` | `CBCEncoding.cpp` (+ `CBCInstrInfo.td` / InstPrinter for names) |

Already present for Path A: TargetRegistry CBC target, InstPrinter, stub disassembler
registration, llvm-objdump `AllTargetsDisassemblers`.

### Path B1 — CBC inside today’s wrap ELF

Keep `e_machine` as host (dynamic loader and N2C stubs require it). On top of Path A:

- Find `__cbc_blob` / a PROGBITS slice starting with `CBC\x01`.
- Parse with the same `CBCObjectFile` logic.
- Disassemble extracted method bodies with the **CBC** target (not the host one on
  `.rodata`).

Optional: `tools/llvm-objdump/CBCDump.cpp` (like `WasmDump.cpp`) for CBC-specific
headers. `--triple=cbc` alone is not enough: the blob is a container.

### Path B2 — native `EM_CBC` ELF (deferred Phase 2)

Only if relocatable CBC ELF with raw bytecode in `SHF_EXECINSTR` sections becomes a
real product (`05` §4.4, `24` — postponed):

- `EM_CBC`, `ELFObjectFile::getArch`, `convertTripleArchTypeToEMachine`
- `CBCELFObjectWriter`, `ELFRelocs/CBC.def`, ObjectYAML `ECase(EM_CBC)`
- Stop mapping `Triple::cbc` → `EM_X86_64` in lld
- Sections such as `.cbc.code.<sym>` containing **raw** bytecode only (not the 57-byte
  header + code-block prefixes)

Do **not** retarget wrap DSOs to `EM_CBC`; that would break `ld.so` and the N2C stubs.

### Shared blocker

Even with perfect Object/ELF wiring, `-d` is useless until `decodeCBCInst` covers the
original ISA. F-25 already records this: three opcodes only; text round-trips do not
work.

Oracle to port (do not invent):

| Piece | Path |
|---|---|
| Parser | `cbc-engine/src/cbc/isa_parser.cpp` |
| Opcodes | `cbc-engine/src/cbc/isa_opcodes.h` |
| Pretty printer | `cbc-engine/src/cbc/isa_disasm.cpp` |
| Standalone tool | `cbc-engine/tools/dis/` |
| Encoding docs | `agents/02-isa-encoding.md`, `agents/06-backend.md` §13.1 |

---

## 3. Special CBC-in-ELF considerations

1. **Bytecode is not at section offset 0.** Header + pool wrap the code; each code block
   has a preamble before the instruction stream.
2. **Variable-length encoding.** Fixed nibbles plus ULEB/SLEB tails. Decoder `Size` must
   be exact; on failure skip 1 (or implement `suggestBytesToSkip`).
3. **Grouped opcodes.** `RegSymGroup` (`0x44`), `RegGroup` (`0x45`), float groups, mem
   ops — second nibble/selectors distinguish `ret` vs `alloca` vs `call.indirect`, etc.
4. **Wrap ELF: CBC is data, host code is `.text`.** Do not set `SHF_EXECINSTR` on
   `__cbc_blob` without switching architecture.
5. **One MC section vs many methods.** Today the AsmPrinter emits the whole file image
   into a single CBC text section. Objdump must slice methods in the reader (Path A) or
   change the writer (Phase 2).
6. **No CBC relocs in linked images.** Method/field/string indices are baked ULEBs;
   symbolization is a table lookup, not ELF reloc processing.
7. **Two ISAs in one DSO.** A complete wrap dump might show host `.text` *and* CBC
   methods from `__cbc_blob`.

---

## 4. Recommended priority

| Priority | Work |
|---|---|
| 1 | Complete `decodeCBCInst` (+ TableGen opcodes for printing) |
| 2 | `CBCObjectFile` + magic → `llvm-objdump -d a.cbc` (Wasm-style) |
| 3 | Optional: objdump special-case for `__cbc_blob` in wrap ELF |
| 4 | Defer `EM_CBC` until relocatable CBC ELF is real |

**Minimal useful product:** Path A + full decoder. ELF `EM_CBC` is a different,
deferred product and is not what wrap files use.

### Analogues

| Target | Why it matters |
|---|---|
| **WebAssembly** | Closest model for `.cbc`: custom ObjectFile, LEB-aware hand (or custom-TableGen) disassembler |
| **BPF** | Closest model for Phase 2 CBC ELF: `EM_*`, executable `.text`, complete fixed-size decoder |
| **DXContainer** | Unusual object format that objdump actually opens |
| **SPIR-V** | Warning: has magic/target but `createBinary` rejects it — registration alone is not enough |

---

## 5. Can `decodeCBCInst` be mostly automated with TableGen?

**Not with stock LLVM `-gen-disassembler`.** CBC does not fit
`FixedLenDecoderEmitter`. Useful automation is still possible; it looks like WebAssembly
or a format table, not “enable `-gen-disassembler` and get a decoder.”

### 5.1 Why stock TableGen fails

Fixed-length decoder assumes:

- a fixed-width bit pattern in `Inst{...}`
- known size before decode
- operands as bitfields

CBC (`02-isa-encoding.md`) has:

- variable length (SLEB/ULEB tails; size unknown until parsed)
- nibble packing + split immediates (`lo4` + `sleb << 4`)
- group opcodes (`0x44` / `0x45`) where a second nibble selects the real op
- `WidePrefix` (`0x13`) that widens the *following* instruction

Today’s `.td` also has nothing for a decoder to consume: `CBCInst` is outs/ins/asm
string only, `Size = 1`, no `Inst` bits, no TSFlags. Encoding lives in the
`encodeCBCInst` switch. `DisassemblerEmitter.cpp` already special-cases WebAssembly
for the same reason (custom emitter, not fixed-len).

### 5.2 What can be automated

**Format + TSFlags (planned in `06` §13.1).** Store `FmtRR`, `FmtMovImm`, `FmtRegGrp`,
… in TSFlags; hand-write one decoder/encoder per format (~15 parsers). Adding
`ADD64rr` = declare format once. Data-driven, not “TableGen emits the whole decoder.”

**Wasm-style custom TableGen emitter.** Wasm emits opcode → MC opcode + operand-type
list; `WebAssemblyDisassembler.cpp` still hand-parses LEBs. A
`CBCDisassemblerEmitter` could emit byte/`[sel]` → `CBC::Opcode` and operand
descriptors (`NibbleReg`, `SplitSlebImm`, `U16`, …). Still hand-written: LEB, nibble
merge, wide-prefix, group dispatch. Worth it only after formats exist in `.td`.

**Generate from the engine (outside LLVM TableGen).** `isa_opcodes.h` +
`isa_parser.cpp` are the ISA source of truth. A script that emits C++ decode tables
from those can automate more than LLVM’s stock emitter with less fighting the
bit-pattern model.

### 5.3 Recommendation for the decoder

| Goal | Approach |
|---|---|
| Useful `llvm-objdump` soon | Port `isa_parser.cpp` into `decodeCBCInst` (or a format switch); don’t wait on TableGen |
| Long-term maintainability | Add `CBCInstrFormats.td` + TSFlags; share format enum between encode and decode |
| Max TableGen | Custom emitter like Wasm — only after formats exist in `.td` |

TableGen can automate **opcode / format / operand metadata** and keep encoder and
decoder in sync. It cannot replace the LEB/nibble/group decode loop the way it does
for RISC-V or AArch64. Stock `-gen-disassembler` is the wrong tool; the hand-written,
format-driven decoder in `06` §13.4 (optionally plus a Wasm-like table) is the right
one.

---

## 6. File-level checklist

### Must-have for Path A (`.cbc`)

| File | Work |
|---|---|
| `include/llvm/BinaryFormat/Magic.h`, `lib/BinaryFormat/Magic.cpp` | `cbc_object`; identify `"CBC\x01"` |
| `include/llvm/BinaryFormat/CBC.h` **(new)** | Magic, header size, tags, opcode constants |
| `include/llvm/Object/CBCObjectFile.h`, `lib/Object/CBCObjectFile.cpp` **(new)** | Per-method text sections; bytecode only |
| `lib/Object/ObjectFile.cpp`, `Binary.cpp`, `CMakeLists.txt` | Dispatch + build |
| `lib/Target/CBC/MCTargetDesc/CBCEncoding.cpp` | Complete `decodeCBCInst`; never abort; unknown → Fail + sensible `Size` |
| `lib/Target/CBC/CBCInstrInfo.td` | Opcodes for everything decoded so InstPrinter has names |
| `llvm/CMakeLists.txt` | Optionally add CBC to `LLVM_ALL_EXPERIMENTAL_TARGETS` |

### Must-have for Path B1 (wrap ELF), on top of A

| File | Work |
|---|---|
| `tools/llvm-objdump/llvm-objdump.cpp` (or Object layer) | Detect `__cbc_blob` / `CBC\x01` in PROGBITS; disassemble method bodies with CBC |
| Optional `tools/llvm-objdump/CBCDump.cpp` | CBC-specific header dump |

### Must-have for Path B2 (deferred)

| File | Work |
|---|---|
| `BinaryFormat/ELF.h`, `ELF.cpp`, `ELFRelocs/CBC.def` | `EM_CBC`, reloc names |
| `Object/ELFObjectFile.h`, `ObjectYAML/ELFYAML.cpp` | `getArch`, YAML |
| `lib/Target/CBC/MCTargetDesc/CBCELFObjectWriter.cpp` **(new)** | Writer with `ELF::EM_CBC` |
| `lld/ELF/InputFiles.cpp` | Stop mapping CBC → host `e_machine` |

### Optional quality

| File | Why |
|---|---|
| `CBCInstPrinter` + object context | Symbolic `@mref(N)` / `@image` |
| `CBCMCInstrAnalysis` | Branch targets for local labels |
| Tests under `test/tools/llvm-objdump/CBC/`, `test/MC/Disassembler/CBC/` | Round-trips vs engine `isa_disasm` |

---

## 7. Bottom line

| Question | Answer |
|---|---|
| Is there a `CBCDisassembler`? | Yes, registered; decodes three opcodes. |
| Is `EM_CBC` defined? | No. |
| Does Object/ELF/YAML support CBC? | No. |
| How does objdump fail today? | `.cbc`: not an object file. Wrap ELF: host `.text` only. `-triple=cbc` on the blob: stub decoder + container bytes. |
| Minimal work for useful `-d`? | `CBCObjectFile` (slice method bytecode) + complete `decodeCBCInst`. |
| Best analog | Wasm for `.cbc`; BPF only if CBC ELF with executable code sections is real. |
| Automate decode with stock TableGen? | No. Use format/TSFlags and/or a Wasm-style custom emitter; port `isa_parser` first. |
