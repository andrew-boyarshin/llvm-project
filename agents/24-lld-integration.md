# 24 — Integrate `cbc-ld` into lld

Authoritative over `10-linker-and-runtime.md` §2.1 (where the link step lives)
and over `09-clang.md` §4.2 (how clang invokes it). Does not change the
whole-program compilation model (`04-architecture.md` §3), the `.cbc` format
(`03-cbc-file-format.md`), or the postponed phase-2 relocatable design
(`10` §4).

Mach-O / iOS (`cbc_aarch64-apple-darwin`) is **out of scope for this work**
(`04` §2 still lists it as not planned). The split below exists so that work
does not have to be undone when a Darwin driver is added.

## 0. Goal

Move the CBC link step from the standalone tool `llvm/tools/cbc-ld` into
lld, delete `cbc-ld`, and have clang invoke `ld.lld --cbc`.

Success is:

1. **Parity with today’s `cbc-ld`.** Every program that links and runs today
   still links and runs, with the same IR-link / O2 / CBC codegen / policy
   checks. The emit pipeline is a mechanical move of `cbc-ld.cpp`, not a
   rewrite onto `lto::LTO` or onto ELF `Writer`.
2. **Native `.so` scripts work (Linux).** `-lncurses` finds `libncurses.so`,
   which on Debian/Ubuntu is a text file `INPUT(libncurses.so.6 -ltinfo)`,
   expands to the real ELF DSOs, and records both SONAMEs
   `libncurses.so.6` and `libtinfo.so.6` in `aotDeps`.
3. **Not ELF-lld parity.** No GOT/PLT, no ELF executable, no `SECTIONS`
   layout, no ICF, no thunks, no mixing of host relocatable objects into the
   CBC program.
4. **No `cbc-ld` binary.** One emit implementation. crt-cbc and compat
   headers keep a CMake install rule; the tool does not.

The engine opens each `aotDeps` token **as a SONAME first** (`dlopen` of the
string as-is), then **as a stem** (`lib` + name + `.so` / `.dylib` / `.dll`).
There is **no** `.so.N` version guessing. New Linux links write SONAMEs;
stem tokens remain accepted for older `.cbc` files.

## 1. Two layers: per-flavor resolution, shared emit

Native library lookup is **not** a common lld service. GNU `INPUT()` scripts
live only in ELF; Darwin `.tbd` / `.dylib` / re-exports live only in Mach-O;
wasm searches `libfoo.a` and has no DSO scripts. `lld/Common` is diagnostics,
memory, and flavor dispatch — not a linker.

CBC therefore splits in two:

```
                    ┌─────────────────────────────────────┐
  Linux (this work) │  ld.lld --cbc                       │
                    │  ELF: -L/-l, INPUT/GROUP, SharedFile│
                    └──────────────────┬──────────────────┘
                                       │  bitcode buffers
                                       │  opaque aotDeps tokens
                                       ▼
                    ┌─────────────────────────────────────┐
                    │  lld/CBC  (library, not a flavor)   │
                    │  IR-link + O2 + policy + codegen    │
                    │  write a.cbc                        │
                    └─────────────────────────────────────┘
                                       ▲
                    ┌──────────────────┴──────────────────┐
  Darwin (later)    │  ld64.lld --cbc                     │
                    │  Mach-O: .tbd/.dylib, re-exports    │
                    └─────────────────────────────────────┘
```

* **`lld/CBC`** is a static library linked by the ELF driver now and by the
  Mach-O driver later. It is **not** `Flavor::CBC`, **not** `lld/Common`,
  and **not** wasm-ld. COFF and wasm do not link it.
* **ELF `--cbc`** (this work) is a mode of `lld::elf::link`: resolve inputs
  with the existing GNU machinery, skip `Writer`, call `lld::cbc::link`.
* **Mach-O `--cbc`** is the same shape on `lld::macho::link` when iOS is
  in scope: resolve with `loadDylib` / `parseReexports` / syslibroot, skip
  Mach-O `Writer`, call the same `lld::cbc::link`. No ELF `LinkerScript`
  on Darwin, and no Mach-O dylib parser on Linux.

The shared entry takes **no** `lld::elf::Ctx`. The ELF driver maps its
files into a small request:

```
struct CBCLinkRequest {
  ArrayRef<MemoryBufferRef> wholeModules;   // crt, then positional bitcode
  ArrayRef<StringRef> lazyArchives;         // bitcode .a paths, or already
                                            // extracted member buffers
  SmallVector<std::string, 0> aotDeps;      // opaque dlopen tokens
  StringRef outputPath;
  Triple triple;                            // from the first bitcode module
};
bool lld::cbc::link(const CBCLinkRequest &);
```

`aotDeps` strings are opaque to the emit layer. ELF fills SONAMEs
(`libncurses.so.6`). A future Mach-O driver fills whatever the Apple engine
`dlopen`s (install names or `libX.dylib` stems — that choice is Darwin’s,
not this document’s). Errors go through `lld`’s Common `ErrorHandler`, which
both flavors already own.

Do **not** bake `cbc_x86_64-unknown-linux-gnu` into `lld::cbc::link`. Today’s
Linux inputs already carry that triple; the request copies it. An iOS
bitcode module would carry `cbc_aarch64-apple-darwin`.

Launcher-library omission (`c`, `m`, … vs Darwin `System`) is **driver**
policy. The ELF driver filters `SharedFile`s before filling `aotDeps`.
`lld::cbc::link` writes whatever list it is given.

## 2. Why Linux resolution lives in `lld/ELF`

`10` §2.1 proposed `lld/CBC/` as a **flavor** on the wasm-ld precedent.
A flavor would have to duplicate ELF’s script graph, or `#include` ELF
headers and become ELF lld in all but name. wasm-ld does not parse GNU ld
scripts and cannot open `libncurses.so`.

`LinkerScript.h` is not a reusable library. It includes `Config.h`,
`InputSection.h`, and `Writer.h`, and every interesting method takes
`lld::elf::Ctx &`. The parser that implements `INPUT()` /
`GROUP()` / `AS_NEEDED()` / `INCLUDE()` / `SEARCH_DIR()` is
`lld/ELF/ScriptParser.cpp`, and the dispatch that treats an unknown-magic
file as a script is `LinkerDriver::addFile`:

```
identify_magic(buffer) == file_magic::unknown  →  readLinkerScript(ctx, mb)
```

Linux CBC is therefore a **mode of the ELF driver**, selected by `--cbc`.
The process is `ld.lld`. There is no fifth flavor. `lld/CBC` is only the
emit library (§1).

Darwin’s equivalent hook is out of scope; when it happens it is a mode of
`ld64.lld`, not a reuse of this ELF mode.

## 3. The `libncurses.so` problem (why LinkerScript)

Today `cbc-ld` resolves `-lfoo` as **only** `libfoo.a` on `-L` paths
(`cbc-ld.cpp` `findArchive`). If that file is missing:

* `c`, `m`, `pthread`, `dl`, `rt`, `gcc`, `gcc_s`, `resolv` → omitted
  (already in the launcher);
* anything else → the bare name is written into `cbc.aotDeps` metadata.

It never opens `libfoo.so`. That is wrong for the libraries C programs
actually pass.

On a typical glibc Debian/Ubuntu sysroot:

| User flag | File found | Contents |
|---|---|---|
| `-lncurses` | `…/libncurses.so` | `INPUT(libncurses.so.6 -ltinfo)` |
| `-lm` | `…/libm.so` | `OUTPUT_FORMAT(…)` + `GROUP ( libm.so.6 AS_NEEDED ( libmvec.so.1 ) )` |
| `-lc` | `…/libc.so` | `GROUP ( libc.so.6 libc_nonshared.a AS_NEEDED ( ld-linux-x86-64.so.2 ) )` |

`libncurses.so` is not ELF. `dlopen("libncurses.so")` fails. Guessing
`libfoo.so.6` … `.1` at runtime (removed from the engine) was not
resolution:

* The `.N` list missed `.so.0`, `.so.7+`, and multi-component SONAMEs
  (`libLLVM.so.21.1`).
* `-ltinfo` was never recorded. Direct `tinfo` symbols, or a ncurses build
  that does not `DT_NEEDED` tinfo the way we assume, break.
* Link-time native validation (designed in `10` §2.2 step 4, currently
  stubbed) cannot see dynsyms of a text file.
* `GROUP()` / `AS_NEEDED()` / absolute paths inside the script are ignored.

The engine now tries each `aotDeps` token as a **SONAME** (`dlopen` as-is),
then as a **stem** (`UpdateSharedObjName`). Link-time script expansion must
therefore write the real `SharedFile` SONAMEs into `aotDeps` so the first
attempt succeeds.

Note: `RTLD_GLOBAL` on the engine `dlopen` path is **orthogonal**. It exists
so guest `dlsym(NULL, …)` / `__cbc_dlsym_*` can see AOT globals. Keep or
drop it independently of this design.

ELF lld already does the right thing: `searchLibrary` prefers `libfoo.so`
over `libfoo.a`; unknown magic is a script; `INPUT(libncurses.so.6 -ltinfo)`
calls `addFile` / `addLibrary` and produces two `SharedFile`s with real
`DT_SONAME`s. CBC mode reuses that path and then **does not write an ELF
output**.

```
-lncurses
    │
    ▼
searchLibraryBaseName: libncurses.so  (before libncurses.a)
    │
    ▼
identify_magic == unknown
    │
    ▼
readLinkerScript → INPUT(libncurses.so.6 -ltinfo)
    │                    │
    ▼                    ▼
SharedFile               addLibrary("tinfo")
  soName=libncurses.so.6      │
                              ▼
                         libtinfo.so  (script or DSO)
                              │
                              ▼
                         SharedFile soName=libtinfo.so.6
    │
    ▼
ELF driver: aotDeps tokens libncurses.so.6:libtinfo.so.6
    │
    ▼
lld::cbc::link  →  IR-link, O2, CBC codegen, write a.cbc
```

On Darwin the same user flag is a `.tbd` or `.dylib` whose **re-exports**
play the role of `INPUT(-ltinfo)`. That parser is Mach-O’s; it is not
designed here.

## 4. Non-goals (explicit)

| Out of this work | Why |
|---|---|
| New `lld` flavor / `Flavor::CBC` | Would not get `LinkerScript`; emit is a library, not a driver |
| Mach-O / iOS `--cbc`, Darwin clang toolchain, TBD/dylib resolution | Out of scope; the `CBCLinkRequest` boundary is the accommodation |
| Putting emit in `lld/Common` | Would link the CBC target into COFF and wasm |
| ELF `Writer`, program headers, GOT, PLT, copy relocs, ICF, thunks | Output is `.cbc` |
| Honouring `SECTIONS` / `MEMORY` / `PHDRS` / `INSERT` | Not required for DSO-script resolution; CBC has one data image built in IR (`CBCLowerGlobals`) |
| Phase-2 relocatable `EM_CBC` objects (`10` §4, D1) | Postponed indefinitely |
| Switching the emit path to `llvm::lto::LTO` | Behavior change vs today’s `llvm::Linker` + O2; a later commit, not a prerequisite |
| Native dynsym validation (`10` §2.2 step 4) | Stubbed in current `cbc-ld`; keep `--no-native-validation` as the default |
| ThinLTO, `--gc-sections` of CBC methods, Map files, `--cbc-split` | Not implemented in `cbc-ld` today |
| Engine `.so.N` version guessing | Removed; write SONAMEs instead (§8) |
| Inheriting clang’s GNU `ConstructJob` (crt1.o, `-dynamic-linker`, `-z relro`) | Those are ELF-output flags; CBC’s driver stays a short explicit list |
| Keeping a `cbc-ld` wrapper / symlink | Clang calls `ld.lld --cbc`; the tool is deleted |

## 5. Parity checklist (today’s `cbc-ld`)

The integrated linker must preserve all of the following. New behavior is
only allowed where this list is silent (DSO scripts, `-l` finding `.so`).

| # | Behavior | Where |
|---|---|---|
| P1 | Positional inputs are LLVM bitcode, linked whole (`Linker::linkInModule`) | `cbc-ld.cpp` |
| P2 | `--crt` (crt-cbc.bc) is linked whole, first | clang `CBC.cpp`, `cbc-ld.cpp` |
| P3 | `.a` inputs and `-l` bitcode archives are lazy: extract a member iff it defines a used `isDeclarationForLinker` name; repeat to fixpoint (≤ 64 rounds) | archive loop |
| P4 | `-lfoo` that is not a bitcode `libfoo.a` becomes a needed native `SharedFile`; launcher set `c m pthread dl rt gcc gcc_s resolv` omitted from `aotDeps` (matched by stem of SONAME) | today: bare stem; after: SONAME |
| P5 | Native libs become module metadata `cbc.aotDeps` (`:`-separated **SONAMEs**); `CBCAsmPrinter` writes the header string | `CBCAsmPrinter.cpp`; engine accepts SONAME or stem |
| P6 | Leftover used `_Z*` declarations are a hard error (“native C++ not supported”) | after IR link, **before** O2 (same order as today, including the known hole F-23) |
| P7 | Module triple taken from the bitcode (today: `cbc_x86_64-unknown-linux-gnu`); flag `cbc-whole-program` = 1 | do not hardcode the Linux triple in `lld::cbc::link` |
| P8 | `nounwind` stripped from `__cbc_raise`, `__cbc_nullcheck`, `_Unwind_RaiseException`, `_Unwind_Resume`, `_Unwind_Resume_or_Rethrow` and their call sites | |
| P9 | `PassBuilder` per-module default pipeline at **O2**, then `TargetMachine` `ObjectFile` codegen | |
| P10 | After O2: hardcoded unsupported libc names (`qsort`, `pthread_create`, …) | |
| P11 | After O2: call to a *declaration* passing a defined `Function*` is an error | |
| P12 | `--no-native-validation` accepted and ignored (default on) | F-07 |
| P13 | Output is a `.cbc` container, not a host object | `CBCFileWriter` via AsmPrinter |

P3 may be implemented by ELF’s lazy `BitcodeFile` extraction instead of the
hand-rolled 64-round loop **if and only if** a side-by-side link of the
existing tests pulls the same archive members. If it differs, keep the
cbc-ld loop **inside `lld/CBC`** (it uses `llvm::object::Archive`, not ELF
types — a Darwin caller gets it for free). Do not “fix” archive semantics
as part of the move.

## 6. Architecture (Linux, this work)

```
clang --target=cbc_x86_64-unknown-linux-gnu
        │
        ▼
ld.lld --cbc -o a.cbc --crt crt-cbc.bc -L <cbc-lib> -L <host-lib> …
        │
        ▼
lld::elf::link                    // existing
        │
        ├─ readConfigs / createFiles / addFile / searchLibrary
        ├─ readLinkerScript (INPUT/GROUP/AS_NEEDED/INCLUDE/SEARCH_DIR)
        ├─ load SharedFile, BitcodeFile, Archive
        │
        │   ctx.arg.cbcMode == true
        │
        ├─ skip: compileBitcodeFiles → ELF objects
        ├─ skip: Writer / assignAddresses / ICF / …
        │
        ├─ build CBCLinkRequest
        │    wholeModules ← crt + non-lazy bitcode
        │    lazyArchives ← bitcode .a
        │    aotDeps      ← needed SharedFile SONAMEs, launcher omitted
        │    triple       ← first bitcode module
        │
        ▼
lld::cbc::link(request)           // lld/CBC
        │
        ├─ IR-link crt + bitcode + extracted archive members   // P1–P3
        ├─ P4–P12 as in cbc-ld.cpp
        └─ TargetMachine emit ObjectFile → a.cbc               // P13
```

### 6.1 `--cbc` is the only required new ELF switch

| Option | Meaning |
|---|---|
| `--cbc` | CBC mode: resolve inputs as ELF lld, emit `.cbc`, do not write ELF |
| `--crt <file>` | Bitcode runtime, linked whole (same as today’s flag) |
| `-o`, `-L`, `-l`, `--start-group`/`--end-group`, `-(` / `-)`, `--whole-archive`, `--no-whole-archive`, `--as-needed`, `--no-as-needed`, `-Bstatic` / `-Bdynamic`, `--sysroot`, `-l:filename` | already in ELF `Options.td`; used as-is |
| `--no-native-validation` | accepted, ignored (P12) |

Do not add `--oformat=cbc`. `--oformat` already means ELF vs binary and
would collide with `OUTPUT_FORMAT(elf64-x86-64)` inside DSO scripts
(`libm.so`, `libc.so`). Do not guess CBC mode from a `.cbc` output name:
a mistaken native link with `-o foo.cbc` must not silently change mode.

Default output in CBC mode if `-o` is missing: `a.cbc` (not `a.out`).

A future Mach-O `--cbc` is a **separate** flag in Mach-O `Options.td`, not
a Common option. Flavors do not share option tables.

### 6.2 Minimize ELF surface

Touched in `lld/ELF`:

* `LinkerDriver::addFile` / `addLibrary` / `createFiles` / `loadFiles`
* `searchLibrary` / `searchLibraryBaseName` / `findFromSearchPaths`
* `readLinkerScript` and `ScriptParser::{readInput,readGroup,readAsNeeded,addFile,readInclude,readSearchDir,readOutputFormat,readOutputArch}`
* `SharedFile::parse` (SONAME; `DT_NEEDED` stored but not required for P0)
* `BitcodeFile` + archive member scan (bitcode only)
* Option parsing for the flags in §6.1
* A single early return in `LinkerDriver::link` that builds a
  `CBCLinkRequest` and calls `lld::cbc::link`

Untouched on the CBC path (must not run):

* `compileBitcodeFiles` / `BitcodeCompiler::compile` producing `lto.tmp` ELF
* `Writer.cpp`, `assignAddresses`, `createPhdrs`, `ICF`, `Thunks`,
  `Relocations.cpp` scan, `MarkLive` (ELF section GC), `MapFile`
* `SECTIONS` evaluation (`processSectionCommands`). Parsing a stray
  `SECTIONS` command is harmless if we never consume `ctx.script` for
  layout; do not special-case the parser.

## 7. Input classification in ELF CBC mode

`addFile` already branches on `identify_magic`. CBC mode tightens the
branches.

| Magic / shape | ELF lld today | CBC mode |
|---|---|---|
| `bitcode` | `BitcodeFile` | same; this is the program |
| `archive` | members that are ET_REL or bitcode | **bitcode members only**; ELF members skipped (see §7.1) |
| `elf_shared_object` | `SharedFile` | same; native provider for `aotDeps`, not IR-linked |
| `unknown` | linker script | same; this is the ncurses case |
| `elf_relocatable` | `ObjFile` | **error if the user named it**; **skip if a script/`-l` brought it in** (§7.1) |
| `formatBinary` | `BinaryFile` | error |

Mixing CBC host flavours: if a bitcode module’s triple is not the same CBC
sub-arch as the rest of the link, error. Same rule `10` §2.2 step 1
intended. Do not special-case “must be Linux x86-64” in the ELF driver
beyond “all bitcode triples must agree”; the in-scope triple is whatever
clang emitted.

`-Bstatic` / `-static` must **not** disable `.so` search in CBC mode.
Native libraries are always DSOs (or scripts pointing at DSOs). If
`ctx.arg.isStatic` is set, CBC mode still looks for `libfoo.so` first.
(`-static` as “produce a static ELF executable” is meaningless here;
warn once and ignore.)

### 7.1 ELF relocatable files that scripts drag in

`libc.so` on glibc is:

```
GROUP ( libc.so.6 libc_nonshared.a AS_NEEDED ( ld-linux-x86-64.so.2 ) )
```

`libc_nonshared.a` is an archive of **ELF** `ET_REL` objects (`elf-init.oS`,
…). A native link needs them; a CBC link must not ingest them (they are
host code, and CBC codegen cannot consume ELF sections).

Rule:

* Track `LinkerDriver::fromScript` (set around `ScriptParser::addFile` /
  `addLibrary`, and when `addFile` is called with `withLOption==true`).
* `elf_relocatable` from a script or from `-l`: drop, no error.
* `elf_relocatable` from a positional user argument:  
  `error: CBC links LLVM bitcode, not ELF objects: 'foo.o'`.
* Archive members: in `loadFiles` `LoadJob::Archive`, if `cbcMode`, only
  instantiate `BitcodeFile`; skip `elf_relocatable` silently (the existing
  warning path for “neither ET_REL nor bitcode” is the wrong polarity —
  we skip ET_REL).

The dynamic linker (`ld-linux-x86-64.so.2`) becomes a `SharedFile`. It is
not put in `aotDeps` (§8.3).

A future Mach-O mode has the same *idea* (skip host `MH_OBJECT`, accept
dylib/TBD as native providers) with Mach-O magics. That policy stays in
the Mach-O driver, not in `lld/CBC`.

## 8. Native libraries and `aotDeps` (Linux)

### 8.1 What the engine consumes

Header field `aotDeps` is a pool string of `:`-separated tokens. For each
token the engine (`engine.cpp` `ReadDependencies`):

1. **SONAME:** `dlopen(token)` as-is (e.g. `libncurses.so.6`).
2. **Stem** (if that fails and the token is not already the stem form):
   `dlopen(UpdateSharedObjName(token))` → `lib` + token + `.so` (`.dylib` /
   `.dll` on other hosts).

The launcher executable is always searched first, so glibc / libm /
libpthread symbols resolve without being listed. There is **no** `.so.N`
version guess.

Old `.cbc` files that store stems (`ncurses`) still work when
`libncurses.so` is a loadable ELF or symlink. When it is a linker script,
only a SONAME token opens successfully — which is why the Linux linker
must write SONAMEs.

### 8.2 Token written for a `SharedFile`

Write the **SONAME** (or basename if SONAME empty), e.g.:

```
libncurses.so.6
libtinfo.so.6
libc.so.6
ld-linux-x86-64.so.2
```

Do **not** strip to a stem for the metadata. The engine’s first attempt
is exactly this string.

For the **launcher-omit** test (§8.3), derive a stem only as a local
classifier in the **ELF driver**:

```
soName e.g. libncurses.so.6
strip optional "lib" prefix
strip ".so" and any trailing ".<digits>" version components
remainder: ncurses, tinfo, c, …
```

If the name does not match `lib*.so*`, treat the omit-stem as empty (do
not put the file in `aotDeps`). That drops the dynamic linker and any
non-`lib*` DSO.

Unusual basenames (`libSDL2-2.0.so.0`) are recorded verbatim as SONAMEs;
no stem round-trip is required for new links.

### 8.3 Which SharedFiles become `aotDeps`

Include `F` in `ctx.sharedFiles` when **all** of:

1. `F->isNeeded` is true (`AS_NEEDED` unused libraries stay out; matches
   GNU `--as-needed` and avoids `libmvec` from `libm.so`’s `GROUP`).
2. The omit-stem (§8.2) is **not** in the launcher set (P4):
   `c`, `m`, `pthread`, `dl`, `rt`, `gcc`, `gcc_s`, `resolv`.
3. The omit-stem is not empty after §8.2 (i.e. name matched `lib*.so*`).

Preserve **first-seen order** (INPUT order). Deduplicate by SONAME string.

For `-lncurses` this yields `libncurses.so.6:libtinfo.so.6`. The engine
`dlopen`s each as-is. That is the intended runtime.

For `-lncurses` **without** script expansion, today’s tool writes only
`ncurses` and hopes a stem open or `DT_NEEDED` loads tinfo. After this
change, both SONAMEs are first-class. That is the one intentional
user-visible improvement; stem-only old files remain loadable when the
unversioned `.so` is not a script.

Bitcode `-l` archives never become `SharedFile`s (`libfoo.a` wins if it
exists on an earlier `-L` path — §9). They do not appear in `aotDeps`.

The filtered token list is what goes in `CBCLinkRequest::aotDeps`.
`lld::cbc::link` does not know about `SharedFile` or the launcher set.

### 8.4 `cbc.aotDeps` metadata

Join tokens with `:`, store on the composite module as
`NamedMDNode "cbc.aotDeps"`. `CBCAsmPrinter` already reads the string
opaquely. No backend change.

If the list is empty, omit the metadata (header `aotDeps = -1`).

### 8.5 Native validation (not in P0)

`SharedFile` already parses dynsyms. A later change can walk leftover IR
declarations against those tables (`10` §2.2 step 4). P0 keeps P12:
`--no-native-validation` on, no walk. Do not block the move on it.

IR leftover declarations that are not `_Z*` and not on the unsupported
list remain **native AOT refs**, exactly as today — even if no DSO
defines them. Missing symbols still fail at first rewrite in the engine.
Parity over the designed (unimplemented) link-time check.

## 9. Search path order (bitcode archive vs host DSO)

ELF `searchLibraryBaseName` (`DriverUtils.cpp`):

```
for dir in searchPaths:
    if not isStatic:  try libNAME.so
    try libNAME.a
```

CBC library directory **must be first** on the clang link line, before
host `/usr/lib/…`:

* `-lc++` → `<resource>/lib/cbc/lib/libc++.a` (bitcode). That directory
  has no `libc++.so`, so the `.a` is chosen without looking at later
  dirs.
* `-lncurses` → CBC dir has nothing → host `libncurses.so` (script).
* `-lm` → host `libm.so` (script) → `SharedFile` `libm.so.6` → omit-stem `m`
  → omitted (launcher).

Do not invert `.so`/`.a` preference globally. Putting the CBC `-L` first
is enough, and matches GNU ld.

`-l:libfoo.so.6` (`searchLibrary` `:` prefix) already searches the
basename as a file; keep it.

Darwin search (`libfoo.tbd` then `libfoo.dylib`, syslibroot) is a Mach-O
driver problem later, not a change to this loop.

## 10. Shared emit (`lld/CBC`)

New library, not under `lld/ELF` and not under `llvm/lib/Target/CBC`:

| File | Role |
|---|---|
| `lld/CBC/CBCLink.h` | `CBCLinkRequest`, `lld::cbc::link` |
| `lld/CBC/CBCLink.cpp` | moved `cbc-ld.cpp` body (P1–P13) |
| `lld/CBC/CMakeLists.txt` | `add_lld_library(lldCBC …)`; LLVM `Linker`, `IRReader`, `BitReader`, `Passes`, `CodeGen`, all CBC-capable targets |

`lld/ELF/CMakeLists.txt` adds `lldCBC` to `LINK_LIBS`. Mach-O CMake does
the same when that driver exists.

`lld::cbc::link` may include LLVM IR / Linker / PassBuilder headers. It
must not include `lld/ELF/*`. Diagnostics use Common `ErrorHandler`.

### 10.1 Collect bitcode

* `request.wholeModules[0]` is crt when `--crt` was passed (P2). The ELF
  driver reads the file; emit does not know `--crt`.
* Remaining `wholeModules`: positional bitcode, `--whole-archive` members.
* Lazy archives: either
  * **Preferred if behavior matches:** ELF `parseFiles` extracts lazy
    `BitcodeFile`s first; the driver passes only non-lazy buffers and
    `lazyArchives` is empty; or
  * **Parity-safe:** pass archive paths in `lazyArchives` and run the
    existing 64-round loop in `lld/CBC` (owned `StringRef` storage —
    F-16).

Decision at implementation time: ship whichever matches P3. Document the
choice in a one-line comment on `lld::cbc::link`.

Do **not** pass DSO / dylib contents into `llvm::Linker`.

### 10.2 Rest of `cbc-ld` `main`

Copy P6–P12 from `cbc-ld.cpp` verbatim. Do not “fix” F-23 (callback
heuristic, O2-before-check order) in this move.

Set `cbc.aotDeps` from `request.aotDeps`. Set `cbc-whole-program`. Use
`request.triple` (P7).

Codegen: same `TargetRegistry::lookupTarget`, `Reloc::Static`,
`CodeModel::Small`, `addPassesToEmitFile(..., ObjectFile)`.

### 10.3 Where the ELF driver calls it

In `LinkerDriver::link`, after `createFiles` / `parseFiles` have produced
`ctx.bitcodeFiles` and `ctx.sharedFiles`, and **before**
`compileBitcodeFiles`:

```
if (ctx.arg.cbcMode) {
  if (errCount(ctx))
    return;
  CBCLinkRequest req = makeCBCRequest(ctx);
  lld::cbc::link(req);
  return;
}
```

Do **not** call `addReservedSymbols`, `scanVersionScript`,
`compileBitcodeFiles`, or `writeResult`.

Empty bitcode (only DSOs): error `no bitcode input`.

### 10.4 ELF config flag

`Ctx` / `Arg` in `lld/ELF/Config.h`:

```
bool cbcMode = false;
StringRef cbcCrt;   // --crt
```

Set in `readConfigs` from `OPT_cbc` / `OPT_crt`. `--cbc` implies
`skipLinkedOutput`-style control: never reach `Writer`.

## 11. Clang driver

`clang/lib/Driver/ToolChains/CBC.cpp` `cbc::Linker::ConstructJob` stays a
**short explicit argv**. It does not call `gnutools::Linker::ConstructJob`
(that injects `crt1.o`, `crti.o`, `-dynamic-linker`, `-z relro`, `-lc`,
libgcc). Those are ELF-output concerns and would trip §7.1.

Changes relative to today:

| Today | After |
|---|---|
| program `cbc-ld` | `ld.lld` (via `GetLinkerPath` / `getDefaultLinker`) |
| | first arg `--cbc` |
| `--crt crt-cbc.bc` | unchanged |
| `--no-native-validation` | unchanged |
| `-L <resource>/lib/cbc/lib` | **first** `-L` |
| | then `ToolChain.AddFilePathLibArgs` (host `/usr/lib`, multiarch, sysroot) so `-lncurses` resolves |
| user inputs via `AddLinkerInputs` | unchanged (`-l`, `.o` bitcode, `.a`) |
| `-lc++ -lc++abi` or catch stub; builtins `.a` | unchanged |
| | do **not** add `-lc -lm -ldl` (launcher; P4) |

`CBCToolChain::getDefaultLinker()`: `"ld.lld"`.

Host `-L` paths are why this move is worth doing: without them,
`searchLibrary` cannot see `libncurses.so`.

Forward `-L` / `-l` / `-Wl,` as today via `AddLinkerInputs`. Unknown
`ld.lld` flags from a user `-Wl,-z,relro` will error in ELF option
parsing; that is acceptable (CBC never passed them). If a later clang
change starts injecting GNU flags, drop them in `ConstructJob`, do not
teach CBC mode to honour them.

A Darwin CBC toolchain, when it exists, is a **different** `ToolChain`
(syslibroot, `ld64.lld --cbc`, no GNU `AddFilePathLibArgs`). Do not grow
the Linux `ConstructJob` to cover it.

## 12. Delete `cbc-ld`

Remove the tool. Do not leave a shim, symlink, or `argv[0] == "cbc-ld"`
hack in `DriverDispatcher.cpp`.

* Delete `add_llvm_tool(cbc-ld …)` from `llvm/tools/cbc-ld/CMakeLists.txt`
  (and the `add_llvm_tool_subdirectory(cbc-ld)` line if the directory
  no longer builds a tool).
* Keep building and installing `crt-cbc.bc`, `cbc_can_catch_stub.bc`, and
  the compat headers. That CMake can stay in `llvm/tools/cbc-ld/` as a
  non-tool target, or move next to the backend; the install layout
  (`lib/cbc/…`) does not change.
* Clang and any in-tree tests that named `cbc-ld` call `ld.lld --cbc`
  instead. No compatibility binary.

Two emit implementations must not coexist past the first landing commit
that calls `lld::cbc::link`.

## 13. Linker-script commands: handle / ignore / error

When `identify_magic` is unknown, the full `readLinkerScriptStmt` runs.
CBC mode does not need a second parser.

| Command | Action |
|---|---|
| `INPUT` | **required** — pull files |
| `GROUP` | **required** — same, with group id (cycles of `--start-group`) |
| `AS_NEEDED` | **required** — `isNeeded` for §8.3 |
| `INCLUDE` | **required** — some `*.so` scripts include others |
| `SEARCH_DIR` | **required** — add to `searchPaths` |
| `OUTPUT_FORMAT`, `OUTPUT_ARCH`, `TARGET` | parse and ignore (already ignored or no-op-ish in ELF lld) |
| `OUTPUT` | only applies if `-o` missing; clang always passes `-o` |
| `ENTRY`, `EXTERN` | ignore for CBC emit (entry is synthesized in IR) |
| `SECTIONS`, `MEMORY`, `PHDRS`, `INSERT`, `OVERWRITE_SECTIONS`, `REGION_ALIAS`, `VERSION`, `NOCROSSREFS`, assignments | parse into `ctx.script`; **never evaluated**. Do not error: a DSO script should not contain them, but a user `-T` should not crash the move. |

No new script dialect. If `unknown directive` fires, it is already an
error.

## 14. File-level change list

| File | Change |
|---|---|
| `lld/CBC/CBCLink.{h,cpp}` **new** | `lld::cbc::link`; moved `cbc-ld.cpp` body |
| `lld/CBC/CMakeLists.txt` **new** | `lldCBC` library |
| `lld/CMakeLists.txt` | add `CBC` subdirectory |
| `lld/ELF/Options.td` | `--cbc`, `--crt=`, `--no-native-validation` |
| `lld/ELF/Config.h` | `cbcMode`, `cbcCrt` |
| `lld/ELF/Driver.cpp` | set flags; CBC branches in `addFile`/`loadFiles` (skip ET_REL); `makeCBCRequest` + early return |
| `lld/ELF/CMakeLists.txt` | `LINK_LIBS lldCBC` |
| `lld/ELF/ScriptParser.cpp` | none unless `fromScript` is cleaner as a `SaveAndRestore` around `addFile` |
| `clang/lib/Driver/ToolChains/CBC.cpp` | `ld.lld --cbc`; host `-L` |
| `clang/lib/Driver/ToolChains/CBC.h` | linker name |
| `llvm/tools/cbc-ld/cbc-ld.cpp` | **deleted** |
| `llvm/tools/cbc-ld/CMakeLists.txt` | crt/header install only; no `add_llvm_tool` |
| `llvm/tools/CMakeLists.txt` | drop `cbc-ld` tool subdirectory if nothing remains to recurse as a tool |
| `llvm/lib/Target/CBC` | none for P0 |

## 15. Implementation order

Minimize reuse **except** the script/DSO spine. Put emit in `lld/CBC` from
the first codegen commit so it is not peeled out of ELF later.

| Step | Work | Exit |
|---|---|---|
| **S0** | ELF `--cbc` flag, `cbcMode`, early return that errors “unimplemented” | driver parses |
| **S1** | `lld::cbc::link` = moved `cbc-ld.cpp` using **command-line bitcode only** (ignore DSOs). ELF driver forwards those buffers. Clang → `ld.lld --cbc`. Delete the `cbc-ld` binary. Existing CBC programs still link | **parity** |
| **S2** | ELF `addFile`/`loadFiles` policy §7; request filled from `ctx.bitcodeFiles` | still green |
| **S3** | `aotDeps` from needed `SharedFile` SONAMEs §8; clang adds host `-L` | `-lncurses` records both SONAMEs |
| **S4** | Archive lazy choice (§10.1); drop a duplicate loop if ELF extraction matches | one extraction path |

S1 is the “do not rewrite the linker” step. S3 is the reason for the move.
Do not start S3 by refactoring LTO.

Estimated effort, one engineer who already knows this tree:

* S0–S1: ~2–3 days
* S2–S3: ~3–5 days (scripts, SONAMEs, clang `-L`)
* S4: ~1 day

Call it **~2 weeks**, not the 5–9 engineer-weeks of a wasm-style flavor.

## 16. Follow-ups (not this design)

* Mach-O `--cbc`: `ld64.lld` mode, TBD/dylib/re-exports, Darwin
  `CBCToolChain`, `LINK_LIBS lldCBC`. Same `CBCLinkRequest`.
* Replace `llvm::Linker` + fixed O2 with `lto::LTO` (`10` §2.2 steps 3–5):
  internalization except entry, `getRuntimeLibcallSymbols`. Behavior
  change; gate it on tests, not on the move.
* Native dynsym validation using `SharedFile` (flip the default of
  `--no-native-validation`).
* `--Map`, `--cbc-split`, `--cbc-emit-asm` from `10` §2.3.
* `CBCNativeCallLegalizer` instead of the P10/P11 heuristics (`07` §6).
* Phase 2 / D1: still a different project; it would start using more of
  ELF (relocs, COMDAT) but still not `Writer`’s PT_LOAD path.

## 17. Risks

| Risk | Mitigation |
|---|---|
| ELF lazy archives pull different members than the 64-round loop | S1 keeps the loop in `lld/CBC`; S4 only switches if behavior matches |
| Host `-L` makes `-lfoo` bind a DSO when a CBC `.a` existed later on the path | CBC `-L` is first (§9); extra `-L` before CBC dirs is the same footgun as GNU ld |
| `GROUP(libc_nonshared.a)` errors | §7.1 skip ET_REL from scripts |
| Launcher-omit stem mis-parses an unusual SONAME (`libfoo.so.1.2.3`, `ld-linux-*`) | §8.2 classifier strips `.so` + trailing numeric components; drop non-`lib*` names |
| Old stem-only `.cbc` + scripted `libfoo.so` | Expected: fails until re-linked; new links write SONAMEs. Stem path still helps when `libfoo.so` is ELF/symlink |
| Emit accidentally takes `elf::Ctx` | `CBCLinkRequest` is the API; ELF includes `lld/CBC`, not the reverse |
| Accidental ELF output | `--cbc` returns before `Writer`; output starts with `CBC\x01`, not `\x7fELF` |

## 18. Summary

Linux CBC linking is a **mode of ELF lld** because that is where `INPUT()`
lives, not because CBC wants an ELF executable. The **emit pipeline**
(`llvm::Linker` + O2 + CBC codegen) lives in **`lld/CBC`**, a library both
the ELF driver (now) and a Mach-O driver (later, out of scope) can call
with opaque native-library tokens. Delete **`cbc-ld`**. Teach clang to
pass **host `-L`** and **`--cbc`**. Derive Linux `aotDeps` from **needed
`SharedFile` SONAMEs**, so `libncurses.so` as `INPUT(libncurses.so.6
-ltinfo)` becomes `libncurses.so.6:libtinfo.so.6`. The engine `dlopen`s
each token as a SONAME first, then as a stem — no `.so.N` guessing.
