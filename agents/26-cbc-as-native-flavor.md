# 26 — CBC as a native-target option

Depends on `24-lld-integration.md` and `25-cbc-native-libraries.md`. Authoritative
over:

* `04-architecture.md` §2 (user-facing triples), §15 (`long double` = `double`),
  §1 non-goal “inline assembly”
* `05-llvm-core-changes.md` §1 (`ArchType::cbc` as the clang/driver architecture)
* `09-clang.md` in full for `CBCTargetInfo`, `CBCToolChain`, and the `long double`
  remap
* `10-linker-and-runtime.md` §5.4 (`*l` rename table) and the cmake triple
* `README.md` decision 1 (one architecture `Triple::cbc`) as a *user* contract
* clang `TargetInfo` / driver overlay, `long double`, `cbc_native` — not the
  `24`/`25` linker argv, which already uses `-fcbc`

Not authoritative over `06-backend.md` / `07-ir-passes.md`: `llvm/lib/Target/CBC`
stays. The bytecode ISA, register file, and whole-program IR passes do not move
into X86/AArch64.

This document is a design decision, not an implementation patch. Three changes
are considered together because they couple:

1. **Product from `-o` (already `25`).** `ld.lld --cbc` always builds a `.cbc` in
   memory. If the output name ends in `.cbc`, that is the file. Otherwise the
   same process wraps it in a host artifact. No `cbc-wrap`, no second link.
2. **Drop the CBC architecture triple.** C/C++ is compiled as
   `x86_64-unknown-linux-gnu` (user spelling `x86_64-pc-linux-gnu` is the same
   target) plus an option, not as `cbc_x86_64-unknown-linux-gnu`.
3. **Stop lying about `long double`.** If the host `long double` is not bit-
   identical to `double`, CBC code must not use it. The old remap (`sizeof == 8`
   on x86-64, `sinl` → `sin`) is withdrawn.
4. **Next step: `__attribute__((cbc_native))`.** Compile this function with the
   host backend. Illegal in a `.cbc` product. The workaround for inline asm,
   SIMD, x87 `long double`, and anything else CBC codegen rejects.

## 0. Verdict

| Proposal | Verdict |
|---|---|
| Wrap vs container from `-o` after `--cbc` | **Accept `25`.** Do not add `--oformat` / `cbc-wrap`. |
| Remove `llvm/lib/Target/CBC` | **Reject.** Bytecode ISel is a backend. LLVM cannot attach a second ISA to `ArchType::x86_64`. |
| Remove user-facing `cbc_*` triples and `CBCTargetInfo` | **Accept.** Replace with host triple + `-fcbc`. No deprecated `cbc_*` alias. Keep `lib/Target/CBC` as a link-time (and `llc`-test) TargetMachine that lld constructs; do not look it up from the IR triple. |
| `long double` remap to `double` | **Withdraw.** Prohibit *operations* (not the mere mention in a system header) when host `long double` ≠ `double`. When they are identical (AArch64 Darwin, x86-64 MSVC), allow. |
| `__attribute__((cbc_native))` | **Accept as the next product after `25` wrap.** Link error if the product is `.cbc`. CBC codegen of a non-attributed function that still contains a forbidden construct is an error, not a silent scalarize/rename (target builtins and inline asm; portable `vector_size` is the exception, §6.6). |
| TU-wide `-femulated-tls` / `-fno-stack-protector` / `-fno-jump-tables` / `-U_FORTIFY_SOURCE` | **Drop all four** (§4.2). They exist for bytecode codegen; `cbc_native` needs the host defaults. Replace with per-function attributes and `LowerEmuTLS` on Module C only. |
| `#error` wrappers for `*intrin.h` | **Delete.** Real headers stay; use outside `cbc_native` is already a Sema/ISel error. Keep SIMD macros. |
| Module flag `CBC = 1` plus `"CBC ABI"` | **One key.** Presence of `"CBC"` means CBC; the value is the ABI string; merge behavior `Require` (§4.5). |
| TU-wide `-fno-vectorize` | **No.** Disable LoopVectorize/SLP on every function that is not `cbc_native` (§4.4). |

The rest of this file is why, what breaks, and the order of work.

## 1. What is already decided (`25`) and what this adds

`25` already specifies the product table:

```
ld.lld --cbc -o a.cbc              →  CBC container
ld.lld --cbc -shared -o libfoo.so  →  ELF DSO, .cbc embedded
ld.lld --cbc --lto-emit-llvm -o x.bc
                                   →  host-triple bitcode of the wrapper
```

`--cbc` remains required (`24` §6.1). The extension is consulted only after that
flag. Default `-o` stays `a.cbc`.

This document does **not** reopen that table. `24` §11 and `25` already
invoke clang as `--target=x86_64-unknown-linux-gnu -fcbc`. This file adds:

* the clang overlay that makes that spelling real (`TargetInfo`, `-fcbc`,
  module flag) and drops the `cbc_*` architecture;
* a constraint from `cbc_native` (§6): presence of a native-attributed
  definition makes a `.cbc` product a link error;
* a few edge cases `25` left implicit (§2).

The wrap is still “native linking of a synthesized host module that contains
CBC bytes,” not “ELF `Writer` of CBC IR.”

## 2. Product selection — remaining issues

### 2.1 Why the extension, not a flag

A flag (`--cbc-emit=wrap`) is clearer in isolation. It is worse in the driver:

* Clang’s `ConstructJob` for CBC is a short argv (`24` §11, `25` §9). The
  output name is already there. A second flag is a second thing to get wrong
  in every wrapper (cmake, meson, `clang -shared`).
* `--oformat=cbc` collides with `OUTPUT_FORMAT(elf64-x86-64)` inside glibc
  `libm.so` scripts (`24`, `25` §1).
* Users already think in artifacts: “I asked for `libfoo.so`.”

Con: `ld.lld --cbc -o foo` (no extension) wrapping an executable is surprising
if someone expected a container. Mitigation stays `25` §2: default `a.cbc`;
`-o foo.cbc` is the container; document that a bare `-o foo` is a host
executable.

### 2.2 Edge cases

| Input | Product | Note |
|---|---|---|
| `-o a.cbc` plus `-shared` | **Error** | Container has no SONAME / DT_NEEDED story; `25` already errors host flags on `.cbc`. |
| `-o libfoo.so` without `-shared` | DSO (warn once) | `25`. |
| `-o -` / `/dev/stdout` | Container if `--cbc` and no `-shared`/`-r`; else error | Do not sniff a non-file. |
| `-o libfoo.so.1` | Host DSO | Real SONAMEs rarely end in `.cbc`. |
| `-o foo.CBC` | Host, not container | Match `.cbc` case-sensitively (Unix). |
| Response files, `--end-lib` | Irrelevant | Decision is the driver’s `-o`, not input names. |
| `cbc_native` definitions + `-o *.cbc` | **Error** | §6.3. |
| Native ET_REL `.o` on the command line | **Error** (unchanged) | Wrap objects are synthesized inside lld; mixing precompiled ELF is a later product. |

### 2.3 Pros / cons of folding wrap into `lld --cbc`

| Pro | Con |
|---|---|
| One clang link line; no wrap tool’s `-L` / sysroot / option parser | ELF `Writer` must run on a **fresh** native request (`25` §13); easy to inherit CBC `elf::Ctx` (scripts, `SharedFile`s) and emit nonsense |
| Bitcode `.a` of the wrapper is LTO-ready without a second pipeline | Product is overloaded: same flag, two file formats. Tests and `file(1)` must always check `-o` |
| Matches “the linker decides the artifact” (ELF vs binary vs `--oformat`) | `25` W3 (N2C stubs) is still gated on engine verification; shipping wrap ELF without that is a false “done” |
| Mach-O wrap later is the same process on `ld64.lld --cbc` | Until Mach-O resolution exists, Darwin wrap is a host module that cannot yet be a dylib |

Accept. Implement W0–W2 of `25` before any `cbc_native` work: a DSO that only
loads a blob is the scaffold the attribute needs.

## 3. What “remove the CBC LLVM target” can actually mean

LLVM’s `Target` / `ArchType` / clang `TargetInfo` / driver `ToolChain` are four
different objects. The proposal is easy to over-read as “delete
`llvm/lib/Target/CBC`.” That is not possible without rewriting bytecode
codegen as something that is not a `TargetMachine`.

### 3.1 Four layers

| Layer | Today | After this document |
|---|---|---|
| **User triple** | `cbc_x86_64-unknown-linux-gnu` | `x86_64-unknown-linux-gnu` (canonical; `x86_64-pc-linux-gnu` accepted and normalized). Option: `-fcbc`. |
| **clang `TargetInfo`** | `CBCTargetInfo` (hand-copied ABI, `long double` = 64, no asm, no SIMD macros) | **Host** `X86_64TargetInfo` / `AArch64TargetInfo`, with a CBC overlay (§4). |
| **clang `ToolChain`** | `CBCToolChain` selected by `Triple::cbc` | Host `Linux` / `DarwinClang` toolchain; `-fcbc` adds the CBC include/lib overlay, forces bitcode `-c`, passes `--cbc` to lld. |
| **LLVM backend** | `lib/Target/CBC`, registered as `ArchType::cbc`, selected by the **module** triple | Same library. `lld --cbc` (and `llc` tests) **construct** `CBCTargetMachine` from the host triple + a CBC subtarget. The IR module triple stays `x86_64` / `aarch64`. `llc` without a CBC switch must **refuse** CBC-flagged bitcode rather than run X86 ISel on it. |

### 3.2 Why a second `ArchType` was attractive

`04` decision 1: one LLVM architecture, two ABI flavours, triples
`cbc_<host>`. That gives:

* a hard safety rail: `ld.lld` without `--cbc` cannot silently produce x86-64
  text from CBC IR (the triple does not select X86);
* a place to lie (`long double`, `__SSE2__`, `validateAsmConstraint`);
* a CMake “cross” target for libc++ / compiler-rt.

It also costs:

* a parallel `TargetInfo` that must track every X86/AArch64 ABI quirk
  (`21` exists because the copy drifted);
* every build system thinks this is an unknown OS/arch (`cbc_x86_64`), so
  autotools/meson/cmake “are we Linux x86-64?” tests fail or need a fake
  `--host`;
* bitcode that cannot be LTO-merged with a consumer’s `x86_64` IR without a
  triple remap — exactly what `25`’s wrap module needs to *be*;
* mixed native/CBC functions in one TU are unnatural: they would be two
  architectures.

The safety rail is the only argument that survives scrutiny. It is replaced
by a **module flag**, not by keeping a fake arch (§4.4).

### 3.3 Why `lib/Target/CBC` cannot go away

`TargetRegistry::lookupTarget("x86_64-unknown-linux-gnu")` returns X86. There
is no supported way to register two ISAs on one `ArchType`. Precedents for
“not a CPU” (SPIR-V, NVPTX, DXIL, WebAssembly) are all still `ArchType`s.

CBC bytecode is a different ISA (virtual register file that *mirrors* the host
C ABI, `02` / `06`). It is not an X86 subtarget, not a code model, not
`-mabi=`. Putting ISel into `lib/Target/X86` would mean a second `ISelLowering`
behind a flag in a backend that must not know about CBC.

Internal options that were considered and rejected:

| Option | Why not |
|---|---|
| CBC as `ObjectFormatType` only (`x86_64-unknown-linux-gnu-cbc`) | `llc` / clang `-S` still pick X86 by architecture. Fourth triple component also breaks `*-*-linux-gnu` in GNU config. |
| CBC as `environment` (`…-gnucbc`) | Same lookup problem; worse autotools matching. |
| Driver rewrites `-fcbc` to `cbc_*` internally, keep `CBCTargetInfo` | Smallest patch; does **not** give mixed codegen a single IR triple; does not fix `21`-style ABI drift; `cbc_native` still has to cross architectures. |
| Delete the backend, emit CBC from a custom IR walk | Rewrites `06`/`07`; years of work; not this proposal. |

**Keep the backend. Stop advertising it as a clang architecture.**

`ArchType::cbc` may remain as a *private* triple that `CBCTargetMachine`
reports to MC (`getTargetTriple()` for asm tests) if that is less surgery
than teaching every `isCBC()` call site to look at a subtarget flag. It must
not be what clang puts in the bitcode, and it must not be what users type.

### 3.4 Recommended user interface

```
clang --target=x86_64-unknown-linux-gnu -fcbc -c foo.c -o foo.o     # bitcode
clang --target=x86_64-unknown-linux-gnu -fcbc -o a.cbc foo.c        # lld --cbc
clang --target=x86_64-unknown-linux-gnu -fcbc -shared -o libfoo.so foo.c
```

The only spelling is `-fcbc` on the host toolchain. Do **not** accept
`--target=cbc_x86_64-unknown-linux-gnu` (no rewrite, no one-release alias).
Lit tests, cmake, and docs move in the same change. Two TargetInfos for one
release is how the ABI copy in `21` survived.

Not a CMake `CMAKE_SYSTEM_PROCESSOR=cbc`. libc++ / compiler-rt are built for
`x86_64-unknown-linux-gnu` with `CMAKE_C_FLAGS=-fcbc`.

Darwin, when wrap exists: `--target=arm64-apple-darwin -fcbc`,
`ld64.lld --cbc`.

## 4. Clang overlay on the host target

`-fcbc` is a `LangOptions` / `CodeGenOptions` bit, not an `ArchType`.

### 4.1 What the overlay must do (that `CBCTargetInfo` does today)

| Concern | Mechanism under `-fcbc` |
|---|---|
| ABI, `va_list`, `max_align_t`, struct layout | Unchanged host `TargetInfo`. **Do not copy it.** This is the point of the change, and the fix for `21` drifting from `X86_64ABIInfo`. |
| `__CBC__`, `__cbc__`, `__CBC_ENGINE_ABI__`, `__CBC_SHADOW_STACK__` | Overlay `getTargetDefines`. |
| Host identity (`__x86_64__`, `__aarch64__`, `__linux__`, **and** `__SSE2__` / `__ARM_NEON__`) | Host `TargetInfo`, unmodified. Do not undefine SIMD macros (§4.3). |
| Inline asm | Host `validateAsmConstraint` stays true (needed for `cbc_native`). Sema: asm in a non-`cbc_native` function is `err_cbc_inline_asm`. |
| Target builtins (`__builtin_ia32_*`, NEON) | Host builtins, unmodified. Sema: use in a non-`cbc_native` function is `err_cbc_target_builtin` with a fix-it to add the attribute. |
| `long double` | §5. Not a width rewrite. |
| `-c` output | Bitcode (`-flto=full` implied). `-fno-lto -c` remains an error (phase 2 postponed). |
| `-femulated-tls`, `-fno-stack-protector`, `-fno-jump-tables`, `-U_FORTIFY_SOURCE` | **Do not pass.** Per-function / link-time, §4.2. |
| `-fno-common` | Keep as a TU default (image layout cannot see commons at `-c`; `07` §3.6). Independent of `cbc_native`. |
| Compat headers, CBC libc++, crt-cbc | Resource-dir paths injected by `-fcbc` on the **host** toolchain (`09` §5), not a separate `CBCToolChain` type. **No** `*intrin.h` wrappers (§4.3). |
| CBC builtins (`__builtin_cbc_gcpoint`, `__builtin_cbc_fcb`) | `TargetInfo::getTargetBuiltins` overlay, or a `LangOptions::CBC` shard in `Builtins.td`. |
| Vectorization | Disabled on every function that is not `cbc_native` (§4.4). Not a TU `-fno-vectorize`. |

`CBC.cpp` `ConstructJob` stays the short `--cbc` list. It is selected by
`-fcbc`, not by `Triple::cbc`.

### 4.2 Driver defaults from `09` §4.3: drop the TU-wide hammer

`09` forces four flags on every CBC TU because the *bytecode* backend cannot
honour the host defaults. They are all **module-wide**. `cbc_native` is host
codegen: it wants the host defaults. None of the four should be implied by
`-fcbc`.

| Flag | Why `09` added it | Why `cbc_native` must not inherit it | Replacement |
|---|---|---|---|
| `-femulated-tls` | CBC has no `%fs` / `TPIDR`; TLS lives in the FCB (`04` §6.4) | Native functions should use real ELF/Mach-O TLS. Forced emutls would call CBC `__emutls_get_address` (FCB) from a thread that may not be attached | Do not pass the flag. Clang emits ordinary `thread_local` IR. After split (§6.4), run `LowerEmuTLS` **only on Module C**. Module N keeps `thread_local`. A `thread_local` used by both sides is already a v1 error (§6.7) |
| `-fno-stack-protector` | SSP on x86-64 is `%fs:0x28`; CBC ISel cannot emit it | Native functions should be protected like any other host function. Distros that default to `-fstack-protector-strong` are right for Module N | Do not pass the flag, and do **not** diagnose `-fstack-protector*`. Clang CodeGen: on non-`cbc_native` functions add `no_stack_protector` (and strip `ssp` / `sspstrong` / `sspreq`). CBC ISel errors if a canary load survives |
| `-fno-jump-tables` | CBC has no indirect branch; `areJTsAllowed` is false (`06`) | Host ISel *should* make jump tables for native switches | Do not pass the flag. Clang CodeGen: `"no-jump-tables"` on non-`cbc_native` only. CBC backend remains a backstop; `IndirectBrExpandPass` (`07` §10) still runs on Module C |
| `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` | `__memcpy_chk` / `__printf_chk` would need extra `CBCNativeLibc.def` rows; “no benefit in an interpreter” | Native kernels want the checks. CBC wants them too: a `_chk` entry is an ordinary AOT native call, object-size `-1` on unknown globals is the normal fortify fallback | Do not undefine. Leave the platform default (`0` / `2` / `3`). A `_chk` that is unsupported for the same reason as the unfortified name (`*l`, callbacks) errors the same way. Do not grow a `_chk` rename table |

**Net: remove all four as `-fcbc` defaults.** They were a way to make a
whole TU look like the bytecode backend. The attribute exists so the TU is
allowed to contain host functions; a TU-wide flag fights that.

What stays TU-wide, for different reasons:

* `-fno-common` — layout, not ISA. Commons are not addressable in the image
  until link (`07` §3.6). Applies to data used by either kind of function.
* bitcode `-c`, `-fno-lto -c` error — whole-program model (`04` §3).
* `-fopenmp` error — no runtime, including for native functions in v1.

`-femulated-tls` at **link** on Module C is the same pass `07` already lists.
Moving it off the clang driver also avoids F-11’s “clang lowered TLS, then
the CBC pipeline saw the wrong shape” class of bug: there is one lowering
site.

### 4.3 Platform intrinsics: keep the real headers

`09` §5.1 ships `#error` wrappers for `immintrin.h`, `emmintrin.h`,
`xmmintrin.h`, `arm_neon.h`, `cpuid.h`, `x86intrin.h`. Delete them.

Use of those headers *outside* `cbc_native` already errors: the builtins they
declare are target builtins, and Sema / CBC ISel reject those in CBC
functions (§6.5–§6.6). Hiding the headers only produces a worse diagnostic
(`#error` at include time, including from a file that also contains a
`cbc_native` kernel) and forces a parallel include path.

Consequences:

* Do **not** `undefineMacro` `__SSE__`, `__SSE2__`, `__AVX*`, `__ARM_NEON`,
  `__ARM_FEATURE_*`. The host `TargetInfo` defines them. That is correct:
  this *is* that platform, and `cbc_native` is how you use the ISA.
* Do **not** pass `-mno-sse2`. x86-64 SysV requires SSE2 in the ABI
  (`X86AVXABILevel`, scalar `double` in `xmm`). Feature subtraction would
  mis-classify types; it was never a substitute for “don’t vectorize CBC
  functions.”
* `#ifdef __SSE2__` `#include <emmintrin.h>` in a CBC function will include
  the real header and then error on `_mm_*` with a fix-it pointing at
  `cbc_native`. That is the intended loop, not a portable-path fallback.
* `cbc_native` functions include `<immintrin.h>` like any x86-64 TU. No
  compat re-define dance.

Other `09` §5.1 wrappers (`bits/floatn.h`, `setjmp.h`, optional
`unavailable` on `qsort` / `pthread_create`) are a separate question. They
are not intrinsic headers; they stay until those libc rules move.

### 4.4 Vectorization is per-function, not a TU flag

Yes: **disable the loop vectorizer and SLP on every function that is not
`cbc_native`.** Do not pass `-fno-vectorize` / `-fslp-vectorize=off` on the
driver — that would also skip compile-time vectorization of native kernels
in the same TU.

`clang -O2 -flto -c` for `x86_64` runs function passes with **X86 TTI**
before emitting bitcode. Without a per-function gate, CBC functions become
`<4 x float>` at `-c`, and CBC ISel would only see the mess at link.

Mechanism:

1. Clang CodeGen, for `-fcbc` functions without `cbc_native`: set a function
   attribute `"cbc"` (the default; `cbc_native` is the opt-in). Early-out in
   `LoopVectorizePass` and `SLPVectorizerPass` when that attribute is present
   (a handful of lines; there is no existing “no-vectorize” function
   attribute these passes honour). Unrolling is unchanged.
2. After IR-link, **split then optimize** (§6.4). Module C uses
   `CBCTargetMachine` O2 + `07` (CBC TTI already reports zero vector
   registers — second line of defence). Module N uses host O2 **with**
   vectorization.
3. CBC ISel still scalarizes leftover generic `vector_size` IR as a
   backstop (§6.6). Target intrinsics still error.

Do not run a full CBC ISel pipeline at `clang -c`. Single-TU `-S` for
inspection can construct `CBCTargetMachine` in process (today’s
“module-local mode”).

### 4.5 Module flag (the safety rail)

Presence of the key **is** the boolean. Do not store `i32 1` / `i32 0`. A
non-CBC module simply lacks the flag; there is no `CBC = 0` to merge with.

Put the ABI in the same flag’s value. Two keys (`"CBC"` + `"CBC ABI"`) would
mean two presence checks and a dummy integer.

```llvm
!llvm.module.flags = !{!0}
; behavior 3 = Require: IR-link errors if the other module lacks the key
; or if the ABI strings disagree.
!0 = !{i32 3, !"CBC", !"sysv-x86_64"}
```

ABI strings (from the **host** triple, not a parallel enum): `"sysv-x86_64"`,
`"aapcs64"`, `"apple-arm64"`.

`Require` (LangRef: error if the key is missing on either side, or the
values disagree) is the merge behavior that matches “presence means CBC.”
`Error` (behavior 1) only compares values when **both** modules have the
key; a native `-flto` `.o` without the flag would otherwise be silently
merged and inherit `"CBC"`.

| Consumer | Behavior |
|---|---|
| `ld.lld --cbc` | Every IR-linked bitcode module must have the flag (Require does this). Missing on an input → “not compiled with `-fcbc`”. |
| `ld.lld` without `--cbc` | If the flag is **present** → **error**, do not codegen as ELF. |
| `llc` default | Error if the flag is present, unless a CBC switch constructs `CBCTargetMachine`. |
| `opt` | Ordinary IR; CBC passes are opt-in. Presence of the flag is ignored. |
| Wrap sidecar (`25`) | Host IR **without** the flag, linked as a separate native object. Not IR-merged with CBC, so Require never sees it. |

This is stronger than today’s triple rail: a renamed triple can still be fed
to the wrong `llc`; a module flag is in the IR.

### 4.6 Pros / cons of dropping the clang architecture

| Pro | Con |
|---|---|
| ABI is the host ABI by construction; `21`’s factory for `X86_64ABIInfo` is used because we *are* X86, not because we delegated | Forgetting `-fcbc` compiles real native objects; they then fail at `ld.lld --cbc` (missing flag) rather than at `clang -c` |
| Autotools/CMake see `x86_64-linux-gnu`; `-fcbc` is a `CFLAGS` | Two `TargetMachine`s at link (`CBC` + host) for `cbc_native` / wrap. LLVM LTO is not built for this; we split modules instead of one `lto::LTO` |
| Wrap IR and CBC IR share a triple; LTO of consumers is ordinary | Churn: every lit test, `LLVM_DEFAULT_TARGET_TRIPLE`, compiler-rt “arch `cbc`”, `AllocateTarget`, `TripleTest` |
| `cbc_native` is a function attribute on host IR, not a second architecture in one TU | `ArchType::cbc` may still exist internally; grepping “is the CBC target gone?” will confuse reviewers |
| Deletes `clang/lib/Basic/Targets/CBC.cpp` and `CBCToolChain` as a *type* | `#ifdef __SSE2__` in a CBC function now *does* see the SIMD path and errors on the builtin — noisier than the old portable fallback, which is the point of the attribute |
| Host headers, `_Static_assert(sizeof(max_align_t) == …)`, glibc `va_list` match | |
| `cbc_native` gets real TLS, SSP, jump tables, fortify, vectorization | |

**Net: do it**, but as a dedicated clang/driver milestone, not a drive-by in
`24`. `24` already copies the module triple (P7) and constructs
`CBCTargetMachine` by CBC target name, not `lookupTarget(triple)`. In-tree
clang may still emit `cbc_*` bitcode until F4; lld does not care.

## 5. `long double`: prohibit unless it *is* `double`

### 5.1 What we do today, and why it is wrong

`09` §2 / `04` §15: on every CBC flavour, `long double` is IEEE `double`.
Clang sets `LongDoubleWidth = 64`. `CBCNativeCallLegalizer` is supposed to
rename `sinl` → `sin`, reject unknown `*l` natives, and error on `%Lf`.

`23` F-09: none of that legalizer exists, `sizeof(long double) == 8`,
`printf("%Lf", 1.5L)` prints `0`, `strtold` returns `nan`. Even with the
legalizer, **the type size is still wrong for the host libc**:

* glibc `printf` `%Lf` reads 16 bytes / an x87 value;
* `max_align_t` is laid out with `long double` alignment 16;
* `numeric_limits<long double>` in a CBC libc++ disagrees with every native
  library compiled for the same process;
* the data layout string already contains `f80:128` (`04` §2.1) while clang
  never emits `x86_fp80`.

The remap is a second ABI, not a subset. It only looks convenient because the
ISA has `f32`/`f64` and nothing else (`06` §4.1).

### 5.2 Rule

Let `HostLD` be the host `TargetInfo`’s `long double` (`fltSemantics`, width,
align). Let `HostD` be `double`.

* If `HostLD` is bit-identical to `HostD` (same `fltSemantics`, same width,
  same align, same ABI class): **`long double` is allowed** in CBC code. It
  is `double` in IR. Native `sinl` is an ordinary AOT call (same bits as
  `sin`). **No rename table.**
* Otherwise: **CBC code must not operate on `long double`.** Clang emits
  `err_cbc_long_double` (error, not `-Wcbc-long-double`). There is no
  `-mlong-double-64` implicit rewrite.

Known hosts:

| Triple | Host `long double` | CBC |
|---|---|---|
| `x86_64-unknown-linux-gnu` | x87 80-bit, width 128, align 128 | **Forbidden** |
| `aarch64-unknown-linux-gnu` | IEEE quad (typically), 128/128 | **Forbidden** |
| `aarch64-apple-darwin` | IEEE double | **Allowed** |
| `x86_64-pc-windows-msvc` | IEEE double | **Allowed** (out of scope, listed as a consequence) |
| `x86_64-pc-windows-gnu` | often 80-bit | **Forbidden** if the MinGW `TargetInfo` says so |

The test is the **host TargetInfo**, not a hard-coded OS list, so a future
`-mlong-double-64` on Linux would flip the rule. We should still **not**
enable `-mlong-double-64` ourselves: it would restore a silent remap against
the glibc we `dlsym`.

### 5.3 What “operate” means (headers have to parse)

Prohibiting the *type* at parse time makes `<math.h>` and libc++ `<cmath>`
unusable: they declare `sinl`, `numeric_limits<long double>`, `max_align_t`
fields.

Diagnose **uses**, not declarations:

| Construct | CBC function | `cbc_native` function | System header declaration |
|---|---|---|---|
| `long double x;` local/global of a definition | error | allowed (real ABI) | n/a |
| Arithmetic, conversion, compound literal | error | allowed | n/a |
| Call / invoke with `long double` / `long double*` / `_Complex long double` in the *effective* prototype | error | allowed | n/a |
| Defining a function with those in the prototype | error | allowed | n/a |
| Declaring `long double sinl(long double);` | allowed | allowed | allowed |
| `sizeof(long double)`, `alignof`, `_Generic` association, `max_align_t` | **allowed** | allowed | allowed |
| `%L` floating conversion in a constant format string | error (keep `09` §2; the format is a use) | allowed (native `printf`) | n/a |
| `va_arg(ap, long double)` | error | allowed | n/a |

`sizeof(long double) == 16` on x86-64 Linux, matching glibc. That is a
deliberate change from every current CBC test that expects `8`.

IR: clang may still emit `x86_fp80` in **types of unused declarations** (a
`sinl` declaration in a TU that never calls it). CBC ISel never sees a
function body that mentions it. A stray `load x86_fp80` in a CBC function is
a **backend error** (`06` §4.1 becomes “`f80` is not legal; producing it in a
CBC function is a compiler bug or a missed Sema”).

Do not scalarize / bitcast `x86_fp80` to `double` in CBC ISel. That would
reintroduce the remap.

### 5.4 Fallout

**Delete** `CBC_LIBC_RENAME(sinl, sin)` and the `"cbc-long-double"` IR
attribute (`09` §3.4, `10` §5.4 rule 2). They exist only for the lie.

**libc++:** long-double overloads of `<cmath>` / `<complex>` / `<limits>` are
function *definitions* in headers. Instantiating them from a CBC function is
an error (good). Compiling libc++ itself as `-fcbc` will error on those
overloads unless:

* a libc++ config (`_LIBCPP_HAS_NO_LONG_DOUBLE`, or reuse the
  `numeric_limits` paths that already key off `LDBL_MANT_DIG == DBL_MANT_DIG`
  — they will **not** fire, because we are no longer lying about
  `LDBL_MANT_DIG`), or
* those overloads are compiled `cbc_native` (only legal in a wrap product;
  a `.cbc` libc++ must omit them), or
* they are not instantiated and we compile libc++ with a flag that `#if 0`s
  them.

v1: add an explicit libc++ knob, set it for CBC-on-x86-64-Linux. Do not
compile those overloads as native until `cbc_native` exists *and* the
product is a DSO.

**compiler-rt:** `mulxc3` / x87 helpers must not be pulled into CBC ISel.
They will not be, if no `x86_fp80` arithmetic is generated.

**Compat `bits/long-double.h`:** stop forcing `__NO_LONG_DOUBLE_MATH`. Glibc
should see the real ABI. Calls to `*l` from CBC are Sema errors instead of
redirected prototypes.

### 5.5 Pros / cons

| Pro | Con |
|---|---|
| F-09 goes away as a class of bug: we never pass 8-byte values to 16-byte libc | Numerical code using `long double` on Linux x86-64 does not compile as CBC; the workaround is `cbc_native` (§6) or rewrite to `double` |
| `max_align_t`, `va_list` layout, `sizeof` match the process’s native libraries | libc++ needs a no-long-double configuration that does not exist today |
| Darwin AArch64 (ldbl == dbl) is a *full* long-double platform, which the remap would have silently damaged in the other direction if anyone had shipped it | Two CBC language dialects by host (ldbl allowed vs not); tests must be `#ifdef`’d on `LDBL_MANT_DIG` |
| Delete the rename table and the `%Lf` special case’s *ABI* motivation (keep the diagnostic: the format is still wrong to *emit* from CBC on x86-64, because CBC cannot produce the value) | Sema “use vs declaration” is subtle; a too-eager diagnostic breaks `math.h`; a too-lax one lets `x86_fp80` reach ISel |
| `cbc_native` can use real x87 `long double` among native functions | CBC **cannot** call those functions: the interpreter has no x87 argument passing. Honest, but a new restriction vs the (incorrect) `sinl`→`sin` bridge |

**Net: prohibit.** The remap is how the compiler became a source of wrong
answers rather than of compile errors.

## 6. `__attribute__((cbc_native))`

### 6.1 Meaning

```c
__attribute__((cbc_native))
void saxpy(int n, float a, float *x, float *y) {
    // host ISA: inline asm, <immintrin.h>, real long double, naked, ifunc, …
}
```

Also `[[clang::cbc_native]]`. On a function **definition**. On a declaration
it is a constraint on the eventual definition (mismatch = link error).

The function is compiled by the **host** `TargetMachine` (X86/AArch64), lives
in host `.text`, and is called from CBC as an AOT native symbol — the same
path as `memcpy`. From other native code (including other `cbc_native`
functions, wrap stubs, and C objects that `dlopen` the DSO) it is an ordinary
C function. It does **not** get an N2C stub. It **is** native.

This is the C analogue of “this translation unit is half GPU” except the
split is per function, one preprocessor run, one IR module until link.

### 6.2 Why it exists

CBC codegen cannot represent:

* inline asm (`09` §3.3);
* target SIMD builtins / `<immintrin.h>` / `<arm_neon.h>`;
* x87 / quad `long double` operations (§5);
* `naked`, `interrupt`, `ifunc`, maybe some `__attribute__((target))`.

Today those are non-goals. With wrap (`25`), the DSO already has host
`.text` (stubs + ctor). Putting user native functions in that `.text` is
the same product, not a new loader. The real `*intrin.h` headers are
visible; using them in a CBC function is the error that points at this
attribute.

Without the attribute, the only escape is a separate native `.so` and an
AOT call across a stable C ABI. That stays supported. The attribute is for
kernels that want to share headers, enums, and `static inline` helpers with
CBC code in the same TU.

### 6.3 Product constraint

| Product (`25` §2) | `cbc_native` definitions |
|---|---|
| `*.cbc` container | **Link error.** No host text segment. The engine loads bytecode, not ELF. |
| ELF/Mach-O DSO, executable, relocatable, host bitcode `.bc` / `.a` | Allowed. Native functions are extra members of the wrap module (or a second host object in the same `Writer` request). |

```
error: 'saxpy' is marked cbc_native; native machine code cannot be written to a .cbc
note: link with -shared -o libfoo.so, or remove the attribute
```

A `.cbc` that *calls* a `cbc_native` function defined in another DSO is fine
(AOT / `dlsym`). The error is “this link has nowhere to put the body.”

### 6.4 Pipeline (extends `25` §3)

```
IR-link all -fcbc bitcode + crt          // as 24, host triple
        │
        ├─ partition by cbc_native
        │     Module C: CBC functions, CBC-facing declarations of natives
        │     Module N: native functions, declarations of CBC (for calls)
        │
        ├─ O2 + 07 passes on C with CBCTargetMachine
        ├─ O2 on N with host TargetMachine
        │
        ├─ CBC codegen(C) → cbcBytes + exportSet
        ├─ host codegen(N) → relocatable object  (empty if N is empty)
        │
        ├─ product .cbc
        │     if N nonempty → error
        │     else write bytes
        │
        └─ wrap
              synthesize blob + ctor + N2C stubs for CBC exports  (25)
              merge with N’s object
              flavor Writer
```

Split **before** O2. Cross-mode inlining is then impossible by construction.
That is required: inlining a `cbc_native` body (inline asm, `x86.sse.*`) into
a CBC function would make CBC ISel fail at the backstop instead of at a
clear attribute boundary. Inlining a CBC function into a native one would
drag `llvm.cbc.*` / FCB TLS into X86 ISel.

IPO across the boundary is only what IR-link already did (constant folding of
`available_externally`, etc.). Acceptable.

`noinline` on the attribute is a backstop if someone runs a pass before
split.

### 6.5 Diagnostics: Sema first, ISel last

User rule: *if a forbidden construct is encountered when generating CBC, that
is an error; the attribute is the workaround.*

| When | What |
|---|---|
| Sema, in a function not marked `cbc_native` | inline asm; target builtins; `long double` operations if §5 forbids them |
| Sema, on the attribute | definition only on functions; not on types; not on globals; not on `main` in a `.cbc` job (driver cannot know the product at `-c`; that waits for link) |
| CBC ISel / `CBCResolveSymbols` | leftover `inlineasm`, `x86_fp80` arithmetic, `llvm.x86.*` / `llvm.aarch64.neon.*`, `ifunc`, module asm, `naked` on a CBC function |
| lld product selection | any remaining `cbc_native` definition + `.cbc` output |

Sema message always includes the attribute:

```
error: inline assembly is not available in CBC code
note: mark the function '__attribute__((cbc_native))' to compile it as host machine code
note: native functions cannot be used when the linker output is a .cbc
```

After LTO/template instantiation, Sema may not have seen the construct in the
final function. ISel is the hard gate. ISel messages should name the function
and suggest the attribute; they must not scalarize a target builtin into a
loop of `fadd` and continue.

### 6.6 Vectors: two different things

| Kind | In CBC functions | In `cbc_native` |
|---|---|---|
| Portable `__attribute__((vector_size(16))) float` | **Keep scalarizing** (`06` §4.1, `04` §16). This is C, not an intrinsic. | Host vector ISA |
| Target intrinsics (`_mm_add_ps`, `vld1q_f32`, `__builtin_ia32_*`) | **Error** | Allowed |
| Auto-vectorizer output (`<4 x float>` from a scalar loop) | Should not appear if §4.3 holds; scalarize as backstop | Allowed |

Silent scalarization of `_mm_add_ps` would violate the user’s rule and hide
missing attributes. Silent scalarization of `vector_size` is existing
documented behavior and should not become a mass breakage of portable code.

Optional later: `-fcbc-strict-vectors` errors on any vector IR in Module C.

### 6.7 What native functions may touch (v1)

The hard problem is **where globals live**. The CBC data image is not an ELF
`.data`; it is bytes relocated at `cbc_engine_load_buffer` (`25` §4). Native
ISel of `load @g` wants a GOT/PC-relative ELF symbol.

v1 restriction (deliberate, small):

A `cbc_native` function may use:

* arguments, return value, `alloca`, `malloc` / native heap;
* function-local `static` (emitted in ELF `.data` / `.rodata` of Module N);
* private constants;
* native libraries (`stdout`, `mmap`, libm);
* other `cbc_native` functions;
* CBC functions, by **call** or by **address** (the address is the N2C stub,
  `25` §6).

It may **not** in v1:

* read or write a file-scope global that Module C also uses, or that lives in
  the data image;
* share a `thread_local` with CBC (after split, CBC-owned TLS is emutls on
  the FCB; native-owned TLS is `%fs` / `TPIDR` — two implementations, §4.2);
* run as a `.init_array` constructor at a priority that precedes the wrap
  ctor (65535 in `25` §7.3) and call into CBC — document, and error if a
  `cbc_native` function is in `llvm.global_ctors` with priority `≤` the load
  ctor *and* calls a CBC function.

Passing a pointer into the image (CBC `malloc` / image object address as an
argument) is the intended way to run a SIMD kernel over CBC data:

```c
__attribute__((cbc_native))
void saxpy(int n, float a, float *x, float *y);

void go(float *x, float *y, int n) {   // CBC
    saxpy(n, 2.0f, x, y);
}
```

Follow-up (not v1): IR rewrite of Module N global accesses to
`__cbc_image_base + offset`, filled by the wrap ctor. Needed if people want
`static` lookup tables shared with CBC without passing pointers.

### 6.8 Calls and function pointers

| Direction | Mechanism |
|---|---|
| CBC → `cbc_native` | AOT native call (`cbc_nativecc`), same as libc |
| `cbc_native` → CBC | N2C stub of the callee (`25` §7). Direct `call @cbc_fn` in Module N is a declaration bound to the stub symbol. |
| CBC `%p = @native_fn` | `ld.fnptr` of an AOT reference (real ELF address) |
| Native `%p = @cbc_fn` | stub address, as `25` FNPTR64 for exports / address-taken |
| Both in one TU | Names do not collide: ELF `@cbc_fn` *is* the stub; the method name lives in the blob |

`CBCNativeCallLegalizer`: passing a CBC (non-stubbed) function to native
remains an error; passing a stubbed one is allowed (`25` §5.1). A
`cbc_native` function is native, so passing it to `qsort` is allowed.

Exceptions / GC / attach: a call from `cbc_native` into CBC **is** native→CBC
and is in scope on the same terms as `25` §5.1 (W3 tests). A `cbc_native`
leaf that does not call CBC is ordinary native code; no attach.

### 6.9 Attribute vs `__attribute__((target("avx2")))`

Compose them: `cbc_native` implies “host default features” (SSE2/NEON on,
AVX as the `-m` flags of the TU). Additional `target("avx2")` is legal only
on `cbc_native` functions (clang already supports multi-versioning on X86).
On a CBC function, `target("avx2")` is an error (would request an ISA the
bytecode backend does not have).

Do not reuse `target("cbc")` as the spelling. `target` is a CPU-feature
namespace; CBC is a codegen selector.

### 6.10 Pros / cons

| Pro | Con |
|---|---|
| Gives a local, explicit escape hatch instead of a second library and a frozen C ABI | Preprocessor is file-scope: `__SSE2__` is true for the whole TU. CBC functions that take the SIMD `#ifdef` fail at Sema with a `cbc_native` fix-it, which is the intended diagnostic |
| Inline asm / SIMD / x87 become linkable in a DSO without a wrap tool | Useless for the `.cbc` launcher product; people will hit the link error and need a DSO |
| Same TU as CBC callers; pointers to image data work as arguments | v1 global split is sharp; shared `static int counter` is an error until the image-base rewrite |
| Fits `25`’s wrap module: more host `Function`s next to the stubs, still LTO-DCEable | Two-TM link; must not paper over with one LTO pipeline and hope TTI is per-function |
| ISel-as-gate catches templates / macros Sema never saw | Diagnostics at ISel are later and uglier than Sema; both are required |
| Honest story for “CBC cannot do X” | Temptation to mark huge TUs `cbc_native` and ship a mostly-native DSO with a token blob; not forbidden, not the point |

## 7. Combined picture

```
                    ┌─────────────────────────────────────────┐
   C/C++ source     │  clang --target=x86_64-… -fcbc           │
                    │  TargetInfo = X86 (overlay macros/Sema) │
                    │  -c → bitcode, triple x86_64, !"CBC"    │
                    └──────────────────┬──────────────────────┘
                                       ▼
                    ┌─────────────────────────────────────────┐
                    │  ld.lld --cbc -o <name>                 │
                    │  ELF resolve (24): scripts, .so, .a     │
                    │  IR-link → split cbc_native             │
                    │  CBC TM on C     host TM on N           │
                    └──────────────────┬──────────────────────┘
                         ┌─────────────┴──────────────┐
                         ▼                            ▼
                   <name>.cbc                   <name>.so / .dylib
                   (N must be empty)            blob + ctor + stubs + N.text
```

Clang never selects `ArchType::cbc`. lld always does, internally, when
`--cbc` is set.

## 8. Impact

### 8.1 Documents

| Doc | Change |
|---|---|
| `README.md` decision 1, work-scope table | User triple is the host; `-fcbc` is the option; backend remains |
| `04` §2 | Replace the triple table with host triples + `-fcbc`; keep flavour as *ABI mirrored*, not as `SubArch` |
| `04` §15 / feature matrix | `long double` allowed iff host-identical; else error; inline asm/SIMD allowed in `cbc_native` |
| `05` §1 | `ArchType::cbc` not used by clang; module flag; `computeDataLayout` for CBC TM taken from the **host** triple |
| `09` | Delete `CBCTargetInfo` / `CBCToolChain` as architecture; rewrite as overlay + `-fcbc`; delete §2 remap, §4.3 TU-wide TLS/SSP/JT/fortify flags, and the `*intrin.h` `#error` wrappers |
| `10` §5.4 | Delete `*l` renames; cmake without `cbc_x86_64` |
| `14` | libc++ no-long-double knob; `CMAKE_CXX_COMPILER_TARGET` is host |
| `21` | Mostly moot: we *are* `X86_64ABIInfo` |
| `23` F-09 | Closed by prohibition, not by legalizer |
| `24` P7, clang invoke | Host triple + flag; `--cbc` unchanged |
| `25` wrap pipeline | Unchanged product table; F6 adds Module N (`cbc_native`) after W2 |

### 8.2 Code (order-of-magnitude)

Already in tree today: `clang/lib/Basic/Targets/CBC.*`,
`clang/lib/Driver/ToolChains/CBC.*`, `Driver.cpp` `Triple::cbc` hooks,
`CodeGenModule.cpp` `createCBCTargetCodeGenInfo`, `llvm/lib/Target/CBC/**`,
`llvm/lib/TargetParser` `ArchType::cbc`, compiler-rt `cbc` arch, lit
`--target=cbc_x86_64-unknown-linux-gnu`.

| Area | Effort |
|---|---|
| `-fcbc` LangOpt, driver overlay, module flag, Sema for asm/builtins/`long double` | Medium; this is the clang milestone |
| Delete `CBCTargetInfo` (no alias) and migrate tests | Medium (mechanical, many files) |
| `lld --cbc` accepts host triple + flag; still constructs CBC TM | Small on top of `24` |
| `25` wrap | As `25` §12; **do not block on this document** except clang argv |
| Split + host codegen of `cbc_native` | Medium; after wrap W2 (need a host object in the Writer request) |
| libc++ long-double config | Small-to-medium, easy to underestimate |
| compiler-rt arch name | Small (keep building builtins at `-fcbc`, stop calling the arch `cbc_x86_64`) |

### 8.3 Tests that change meaning

* `Preprocessor/cbc-target.c`: `__SIZEOF_LONG_DOUBLE__` becomes `16` on
  x86-64 Linux; `__SSE2__` **present** (host TargetInfo); `__x86_64__`
  present; `__CBC__` present. No `*intrin.h` `#error` test.
* `Sema/cbc-long-double.c`: warning → **error** on uses; `sizeof` allowed.
* `CodeGen/CBC/abi-x86_64.c`: IR must be **byte-identical** to host x86-64
  including `fp80` on `sinl` *declarations*; function *bodies* still must not
  contain `fp80`.
* Any run test using `long double` or `%Lf`: becomes a Sema fail, or moves
  under `cbc_native` in a DSO test.

### 8.4 What we should *not* do in the same milestone

* Mix precompiled native `ET_REL` with `--cbc` (still a later product).
* Per-function TTI in one LTO pipeline.
* Image-base relocation for native uses of CBC globals.
* Compiling compiler-rt builtins as `cbc_native` for speed.
* iOS / Windows.

## 9. Alternatives considered

**Keep `cbc_*` forever, add wrap and `cbc_native` on top.** Smallest delta.
`cbc_native` functions would be IR with a CBC triple compiled by X86 — every
pass that trusts `TM.getTargetTriple() == module triple` is a latent bug.
Reject as the long-term shape; acceptable only as a temporary while `-fcbc`
lands.

**`--target=x86_64-unknown-linux-gnu-cbc` (object format).** Looks like a
flavour. Breaks GNU triple matching; `llc` still selects X86; users have to
change `--target=` in every build. Worse than `-fcbc`.

**`-mlong-double-64` instead of prohibition.** Restores F-09 against glibc
the moment someone prints a `long double` or passes it to a non-renamed
symbol. Reject.

**Two preprocessor passes (CUDA-style `__host__` / `__device__`).** Would
make `__SSE2__` true only in native functions. Unnecessary: the macros stay
on, and Sema rejects the builtins in CBC functions. Doubles compile time.
Reject.

**`cbc_export` from `25` §6 as the same attribute.** No: export is “emit an
N2C stub for this CBC function.” `cbc_native` is “this function is not CBC.”
A function is never both.

## 10. Implementation order

Relative to `24` / `25`. Each line is independently shippable; later lines
assume earlier ones.

| Step | Work | Exit |
|---|---|---|
| **F0** | `24` as specified (CBC TM by name, not `lookupTarget(triple)`) | today’s programs link |
| **F1** | Module flag `!"CBC"` = ABI string, `Require` merge; `ld.lld` without `--cbc` errors if the key is present | safety rail exists before the clang architecture dies |
| **F2** | `25` W0–W2: `linkToMemory`, wrap blob+ctor, ELF `-shared` | `dlopen` inits a library `.cbc`; product table live |
| **F3** | `long double` prohibition on the **current** TargetInfo (error on uses; stop remap work) | F-09 tests inverted: `sizeof` is 16, arithmetic does not compile |
| **F4** | `-fcbc` + host `TargetInfo`; delete `cbc_*` (no alias); drop the four TU-wide driver flags; delete `*intrin.h` wrappers; lld takes host-triple bitcode | `clang --target=x86_64-unknown-linux-gnu -fcbc` is the only invoke |
| **F5** | `25` W3–W4: N2C stubs, address-taken | native C calls a CBC export |
| **F6** | `cbc_native`: Sema + split + host codegen into the wrap; `.cbc` product errors | SIMD/asm kernel in a DSO called from CBC; `.cbc` link fails cleanly |
| **F7** | libc++ no-long-double; delete rename table and `CBCTargetInfo` | no `cbc_*` in cmake |

F3 before F4 is intentional: prohibition does not depend on the triple, and
it stops anyone implementing the `sinl`→`sin` legalizer that `23` still
treats as the F-09 fix.

F6 must not start before F2: without a wrap product there is nowhere to put
native text except a `.cbc` link error, which is not a useful feature on its
own.

## 11. Risks

| Risk | Mitigation |
|---|---|
| Host-triple bitcode accidentally native-codegen’d | Module flag presence + lld/`llc` refusal (`§4.5`) |
| One TU compiled without `-fcbc` pulled into a CBC link | `Require` merge: missing `"CBC"` key is an error under `--cbc` |
| Vectorizer at `clang -O2 -c` plants AVX in CBC functions | `"cbc"` function attribute skips LoopVectorize/SLP; split before LTO O2; ISel scalarize backstop |
| Sema allows `math.h`, ISel still sees `fp80` from an inline in glibc | Treat as ISel error; if frequent, compat header wraps the inline |
| libc++ build explodes on `long double` overloads | F7 knob; do not wait for F6 to hide them as native |
| `cbc_native` + `.cbc` discovered only at link | Sema cannot know `-o`; driver can warn on `-fcbc` when the *clang* `-o` ends in `.cbc` **and** the TU parsed a `cbc_native` definition (best-effort; archives still wait for lld) |
| Reviewers delete `lib/Target/CBC` reading this title | §3 is the first reply; the backend stays |
| Naked-asm stubs (`25`) vs user inline asm | Different: stubs are linker-synthesized host IR; user asm is in Module N. Do not run CBC `ResolveSymbols` on N |

## 12. Summary

`ld.lld --cbc` already has the right product switch in `25`: the `.cbc`
suffix means a container; anything else means a host artifact with the
container inside. There is no wrap step.

CBC should not be a clang architecture. It should be `-fcbc` on
`x86_64-unknown-linux-gnu` / `aarch64-unknown-linux-gnu` / Darwin. The module
flag is the key `"CBC"` whose *presence* marks the IR; its value is the ABI
string. The bytecode backend remains `llvm/lib/Target/CBC`, constructed by
lld, because LLVM will not attach a second ISA to `ArchType::x86_64`.

`-fcbc` does not imply `-femulated-tls`, `-fno-stack-protector`,
`-fno-jump-tables`, or `-U_FORTIFY_SOURCE`. Those are bytecode constraints,
applied to non-`cbc_native` functions (and to Module C at link), not to the
TU. Vectorization is the same: off for CBC functions, on for `cbc_native`.
Platform `*intrin.h` is not wrapped.

`long double` must stop being `double` on platforms where the host libc
disagrees. That prohibition is an error at use, not a new sizeof, and it
removes the rename table that was never going to make `%Lf` correct.

`__attribute__((cbc_native))` is the next step after wrap, not a substitute
for it. It is how inline asm, vector intrinsics, and real `long double` enter
a CBC program: as host `.text` beside the blob. If those constructs appear in
a function that is still going through CBC codegen, that is a hard error
whether the output is a `.cbc` or a `.so`. If they appear in a `.cbc` link at
all, the attribute does not save you — change the product name.
