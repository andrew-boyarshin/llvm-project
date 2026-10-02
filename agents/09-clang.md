# 09 — Clang Support

Paths are relative to `llvm-project-upstream/clang`.

**Work scope.** Only the x86-64 Linux flavour (`cbc_x86_64-unknown-linux-gnu`) is in the
current work scope. The AArch64 Linux columns and notes below are the design for the
follow-up flavour (`04-architecture.md` §2): `CBCTargetInfo` accepts the AArch64
sub-architecture (so that `-dM -E` and the type model can be tested early and cheaply), but
the driver and the backend reject code generation for it until the follow-up. The Darwin
cases are listed for completeness and are not planned.

## 1. `lib/Basic/Targets/CBC.{h,cpp}` — `CBCTargetInfo`

Registered in `lib/Basic/Targets.cpp` `AllocateTarget`:

```cpp
case llvm::Triple::cbc:
  if (Triple.getSubArch() != llvm::Triple::CBCSubArch_x86_64 &&
      Triple.getSubArch() != llvm::Triple::CBCSubArch_aarch64)
    return nullptr;
  switch (os) {
  case llvm::Triple::Linux:  return std::make_unique<LinuxTargetInfo<CBCTargetInfo>>(Triple, Opts);
  case llvm::Triple::Darwin: case llvm::Triple::IOS: case llvm::Triple::MacOSX:
                             return std::make_unique<DarwinTargetInfo<CBCTargetInfo>>(Triple, Opts);
  default:                   return std::make_unique<CBCTargetInfo>(Triple, Opts);
  }
```

Wrapping with `LinuxTargetInfo`/`DarwinTargetInfo` gives the OS macros (`__linux__`,
`__gnu_linux__`, `__unix__`, `__APPLE__`, …) and OS type conventions automatically.

### 1.1 Type model (must equal the host ABI)

| Property | x86-64 flavour | AArch64 Linux flavour |
|---|---|---|
| `PointerWidth/Align` | 64/64 | 64/64 |
| `long`, `long long` | 64 | 64 |
| `SizeType`, `PtrDiffType`, `IntPtrType`, `IntMaxType`, `Int64Type` | `unsigned long`, `long`, `long`, `long`, `long` | same |
| `char` signedness | signed | **unsigned** (driver default, §4.3) |
| `WCharType` | `int` | `unsigned int` |
| `WIntType` | `unsigned int` | `unsigned int` |
| `Char16Type`/`Char32Type` | `uint_least16_t`/`uint_least32_t` | same |
| `SuitableAlign`, `NewAlign` | 128 | 128 |
| `LongDoubleWidth/Align/Format` | **64 / 64 / IEEEdouble** | **64 / 64 / IEEEdouble** |
| `HasFloat128`, `HasIbm128` | false | false |
| `HasFloat16` (storage-only `_Float16`) | true | true |
| `HasLegalHalfType` | false | false |
| `HasInt128Type()` | true | true |
| `MaxAtomicPromoteWidth`, `MaxAtomicInlineWidth` | 64 | 64 |
| `UseZeroLengthBitfieldAlignment`, `UseBitFieldTypeAlignment` | host values | host values (AArch64 sets `UseZeroLengthBitfieldAlignment = true`) |
| `getBuiltinVaListKind()` | `X86_64ABIBuiltinVaList` | `AArch64ABIBuiltinVaList` (Darwin: `CharPtrBuiltinVaList`) |
| data layout | `04-architecture.md` §2.1 | |

Copy the bit-field and alignment flags from `X86_64TargetInfo` /
`AArch64leTargetInfo` constructors (`lib/Basic/Targets/X86.h`, `AArch64.cpp`) so struct
layouts match native code exactly; this is what makes host headers and native libraries
usable.

### 1.2 Predefined macros (`getTargetDefines`)

Always:

```
__CBC__ 1        __cbc__ 1        __CBC_ENGINE_ABI__ 1
__CBC_HOST_X86_64__ 1   or   __CBC_HOST_AARCH64__ 1
__CBC_SHADOW_STACK__ 1
__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1/_2/_4/_8 1
__FLT_EVAL_METHOD__ 0
```

Host identity macros needed by host headers (glibc selects word size, struct layouts and
syscall numbers from them):

* x86-64: `__x86_64__`, `__x86_64`, `__amd64__`, `__amd64`, `__LP64__`, `_LP64`.
* AArch64: `__aarch64__`, `__AARCH64EL__`, `__ARM_64BIT_STATE`, `__ARM_PCS_AAPCS64`,
  `__ARM_ARCH 8`, `__ARM_ARCH_PROFILE 'A'`, `__LP64__`, `_LP64`.

**Not** defined (there are no such instructions; code guarded by them must take the
portable path): `__SSE__`, `__SSE2__`, `__SSE_MATH__`, `__MMX__`, `__AVX*`, `__ARM_NEON`,
`__ARM_FEATURE_*`, `__ARM_FP`, `__SEG_FS`, `__SEG_GS`, `__GCC_ASM_FLAG_OUTPUTS__`.
Some portable code assumes x86-64 implies SSE2 and includes `<emmintrin.h>` under
`__x86_64__`; that code fails to compile and needs `#if !defined(__CBC__)`. This is the
price of reusing host headers; `09` §5.1 lists the mitigations.

### 1.3 Other `TargetInfo` hooks

| Hook | Behaviour |
|---|---|
| `getTargetBuiltins` | `__builtin_cbc_gcpoint()`, `__builtin_cbc_fcb()` (the fiber control block, `ld.fcb`) |
| `getGCCRegNames`, `getGCCRegAliases` | empty |
| `validateAsmConstraint` | false (inline asm rejected, §3.3) |
| `getClobbers` | `""` |
| `isCLZForZeroUndef` | false (expansion defines `clz(0)`) |
| `hasBitIntType` | true (`_BitInt(N)` up to 128 lowered by LLVM; larger via `ExpandLargeDivRem`) |
| `getABI`/`setABI` | `"cbc-sysv"` / `"cbc-aapcs"` |
| `hasFeature("cbc")` | true |
| `checkCallingConvention` | `CC_C` only (reject `vectorcall`, `regcall`, `ms_abi`, `sysv_abi`, `preserve_*`, `swiftcall`) |
| `getStaticInitSectionSpecifier` | none |
| `hasSjLjLowering` | false |
| `getUnwindWordWidth` | 64 |

## 2. `long double`

`long double` is IEEE double. Effects:

* `sizeof(long double) == 8`, `LDBL_MANT_DIG == 53`. Code that relies on extended
  precision loses it (common in some numerical code; rare elsewhere).
* Calls to native `*l` math functions and `strtold` would pass a double where native code
  expects x87/quad data. Clang marks such declarations (§3.4) and `CBCNativeCallLegalizer`
  renames the calls to the `double` functions (`sinl` → `sin`, `strtold` → `strtod`;
  table in `10-linker-and-runtime.md` §5.4); a native function with `long double` in its
  prototype and no rename entry is a link-time error.
* **Format strings.** Native `printf` reads a 16-byte `long double` for `%Lf`, and native
  `scanf` writes one for `%Lf`, so `%L` with a floating conversion (`a A e E f F g G`) is
  wrong on CBC. In `Sema::CheckFormatArguments` (the existing printf/scanf format checker,
  `clang/lib/Sema/SemaChecking.cpp`, which parses the string with
  `analyze_format_string::ParsePrintfString`/`ParseScanfString`), when the target is CBC:
  * a constant format string containing such a conversion is an **error**
    `err_cbc_format_long_double`: `'%Lf' is not supported on CBC: long double is double;
    use '%f'` (printf family) or `use '%lf'` (scanf family), with a fix-it that removes
    the `L` (printf) or replaces it with `l` (scanf). Wide-character variants
    (`wprintf(L"%Lf")`) are checked the same way;
  * a non-constant format string in a call that passes a `long double` or `long double *`
    variadic argument is a warning `-Wcbc-format-long-double` (on by default).

  The format checker applies to the functions clang knows as format functions: the
  builtin printf/scanf family and every declaration with `__attribute__((format))`.
  Rewriting the string at compile time was considered and rejected: the literal may be
  shared, and silently changing user strings is surprising.
* `-Wcbc-long-double` (on by default) warns on the first use of `long double` in a
  translation unit.

## 3. `lib/CodeGen`

### 3.1 ABI lowering delegates to the host

`X86_64ABIInfo` and `AArch64ABIInfo` are file-local classes today. Expose factories:

```cpp
// lib/CodeGen/TargetInfo.h
std::unique_ptr<ABIInfo> createX86_64ABIInfo(CodeGenTypes &CGT, X86AVXABILevel AVXLevel);
std::unique_ptr<ABIInfo> createAArch64ABIInfo(CodeGenTypes &CGT, AArch64ABIKind Kind);
std::unique_ptr<TargetCodeGenInfo> createCBCTargetCodeGenInfo(CodeGenModule &CGM);
```

`lib/CodeGen/Targets/CBC.cpp`:

```cpp
class CBCTargetCodeGenInfo : public TargetCodeGenInfo {
public:
  CBCTargetCodeGenInfo(CodeGenTypes &CGT, std::unique_ptr<ABIInfo> Host)
      : TargetCodeGenInfo(std::move(Host)) {}
  bool isNoProtoCallVariadic(const CallArgList &, const FunctionNoProtoType *) const override {
    return false;   // unprototyped calls use the fixed-argument convention (no buffer)
  }
  unsigned getSizeOfUnwindException() const override { return 32; }
  // No x86/AArch64 target attributes ("target-features", "branch-target-enforcement", …).
};

std::unique_ptr<TargetCodeGenInfo> CodeGen::createCBCTargetCodeGenInfo(CodeGenModule &CGM) {
  const auto &T = CGM.getTarget().getTriple();
  auto Host = T.isCBCHostX86_64()
      ? createX86_64ABIInfo(CGM.getTypes(), X86AVXABILevel::None)
      : createAArch64ABIInfo(CGM.getTypes(), T.isOSDarwin() ? AArch64ABIKind::DarwinPCS
                                                            : AArch64ABIKind::AAPCS);
  return std::make_unique<CBCTargetCodeGenInfo>(CGM.getTypes(), std::move(Host));
}
```

Hook it up in `lib/CodeGen/CodeGenModule.cpp` `createTargetCodeGenInfo`:
`case llvm::Triple::cbc: return createCBCTargetCodeGenInfo(*this);`.

What this gives, unchanged from native compilation: struct/union classification and
coercion (`{i64, i64}`, `{double, double}`, `byval`, `sret`, HFAs), `__int128` passing,
`va_arg` code generation against the host `va_list` (`X86_64ABIInfo::EmitVAArg`,
`AArch64ABIInfo::EmitAAPCSVAArg`), which reads from the overflow area that
`ExpandVariadics` points at (`04-architecture.md` §10). `X86_64ABIInfo` classifies
`long double` by its `fltSemantics` (IEEE double → SSE), so `long double` arguments go to
`FR` registers, consistent with §2.

### 3.2 Exceptions

Personality functions stay `__gxx_personality_v0`/`__gcc_personality_v0`; clang emits
ordinary `invoke`/`landingpad`/`resume`. `CBCLowerEH` (`08-exceptions-and-sjlj.md` §4.2)
interprets them. `-fexceptions` and `-fcxx-exceptions` keep their usual defaults;
`-fasynchronous-unwind-tables`/`-funwind-tables` are irrelevant and ignored.

### 3.3 Inline assembly

`CodeGenFunction::EmitAsmStmt`: if the target is CBC, emit
`error: inline assembly is not supported on the CBC target` (an `err_` diagnostic in
`DiagnosticFrontendKinds.td`, CBC group). Symbol renaming with asm labels
(`extern int stat(...) __asm__("stat64")`, used heavily by glibc headers) is **not** inline
assembly and works (it changes the IR symbol name; `CBCResolveSymbols` strips the `\01`
prefix).

### 3.4 Prototype facts for native calls

The IR type of a declaration loses two facts that `CBCNativeCallLegalizer`
(`07-ir-passes.md` §6) needs, so clang records them as string attributes. Opt-in by triple;
no effect elsewhere.

* **Function-pointer parameters.** When emitting a call to a function **declaration**
  (potentially native), clang adds the call attribute `"cbc-fnptr-args"="i,j"` listing
  the parameters whose declared type is a pointer to function (after typedefs, including
  `sighandler_t`, `__compar_fn_t`). The legalizer turns a provable CBC function passed in
  such a position into an error and an unknown one into a warning
  (`10-linker-and-runtime.md` §5.6). For calls through a function pointer the attribute is
  computed from the pointee's `FunctionProtoType`.
* **`long double` in the prototype.** On every function declaration whose
  `FunctionProtoType` has `long double`, `long double *` (one level, any qualifiers) or
  `_Complex long double` in its return type or parameter types, clang adds the function
  attribute `"cbc-long-double"`. In IR these types are already `double`/`ptr`, so without
  the attribute the legalizer could not tell `sinl(double)` from a function that really
  takes a `double`. Variadic `long double` arguments are covered by the format check
  (§2), not by this attribute.

Unprototyped declarations (`int f();` in C17) get neither attribute and are not
diagnosed.

## 4. Driver

### 4.1 Tool chain (`lib/Driver/ToolChains/CBC.{h,cpp}`)

`class CBCToolChain : public Generic_ELF` (Linux flavour; a Darwin variant derives from
`DarwinClang` later). Selected in `lib/Driver/Driver.cpp` `getToolChain` for
`Triple::cbc`.

| Concern | Behaviour |
|---|---|
| default triple completion | `--target=cbc` (no sub-arch) → `cbc_<host arch>-<host os>-<host env>` |
| sysroot | `--sysroot` or the host root; CBC runtime files under `<resource-dir>/cbc/<triple>/` |
| C include order | clang resource headers → `<resource-dir>/cbc/include` (compat headers, §5.1) → host system headers (`/usr/include/<multiarch>` from the **host** triple: `x86_64-linux-gnu` / `aarch64-linux-gnu`, then `/usr/include`) |
| C++ standard library | libc++ only: `<resource-dir>/cbc/<triple>/include/c++/v1` (headers configured for CBC: `_LIBCPP_HAS_NO_THREADS`, `_LIBCPP_HAS_NO_MONOTONIC_CLOCK` off, …). `-stdlib=libstdc++` is an error |
| object output | LLVM bitcode (`-c` implies `-flto=full`; `isUsingLTO()` returns true by default). `-fno-lto` with `-c` is an error (`separate code generation is not supported for the CBC target`): phase 2 is postponed indefinitely (`10-linker-and-runtime.md` §4) |
| AArch64 flavour | `--target=cbc_aarch64-…` is accepted for preprocessing (`-E`, `-dM -E`) and `-fsyntax-only`; any compilation to bitcode or linking is an error until the follow-up flavour is implemented (`04-architecture.md` §2) |
| `-S` | CBC assembly of the single TU (module-local mode, for inspection/tests) |
| `-emit-llvm` | as usual |
| link | `tools::cbc::Linker` → `cbc-ld` (§4.2) |
| default output | `a.cbc` |
| relocation model / PIC | static; `-fPIC` accepted and ignored |

### 4.2 Linker invocation

```
cbc-ld --target=<triple> -o a.cbc
       <resource-dir>/cbc/<triple>/lib/crt-cbc.bc
       <inputs: .o (bitcode), .a (bitcode archives)>
       [C++: -lc++ -lc++abi | else: cbc_can_catch_stub.bc] -lclang_rt.builtins
       --native-lib=c --native-lib=m [user -l<name> for names not found as CBC archives]
       --native-search-path=<host lib dirs>          (validation only)
       -O<n>
```

A `-l<name>` is resolved first as a CBC bitcode archive `lib<name>.a` in the CBC library
paths; otherwise it is recorded as a native library: added to the `aotDeps` header string
(unless it is `c`, `m`, `pthread`, `dl` which the launcher process already provides) and
used for symbol validation.

### 4.3 Default flags added by the tool chain

| Flag | Reason |
|---|---|
| `-femulated-tls` | TLS via the fiber control block (`04-architecture.md` §6.4) |
| `-fno-stack-protector` (diagnose explicit `-fstack-protector*` as unsupported, or map to `-mstack-protector-guard=global`) | x86-64 SSP reads `%fs:0x28` |
| `-fno-jump-tables` | no indirect branch (the backend also refuses jump tables) |
| `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` | fortified glibc entry points (`__printf_chk`, `__memcpy_chk`, …) would need their own entries in `CBCNativeLibc.def` (the `_chk` variants of the `long double` and `v*` tables) for no benefit in an interpreter; with fortification off, calls go to the plain native entry points |
| `-fno-common` (explicit `-fcommon` is an error) | common symbols cannot be placed in an image before link time (`07-ir-passes.md` §3.6); clang's default is already `-fno-common` |
| `-fno-signed-char` | AArch64 flavour only (host ABI) |
| `-mlong-double-64` semantics | implied by the target, reported by `-dM -E` |
| `-fno-omit-frame-pointer` / `-momit-leaf-frame-pointer` | ignored |
| `-g` | accepted; only file and function names survive (warning `-Wcbc-debug-info`) |
| `-fopenmp` | error (no OpenMP runtime) |
| `-pthread` | accepted; threads unsupported at run time (`04-architecture.md` §14) |

### 4.4 Running

The driver does not run programs. A `cbc-run` wrapper (shipped with the engine) invokes the
launcher: `cbc-run a.cbc args…`. For `lit` tests, `%cbc-run` substitutes the launcher path.

## 5. Headers

### 5.1 Compat headers (`<resource-dir>/cbc/include`)

Small wrappers that `#include_next` the host header and adjust what CBC cannot support:

| Header | Adjustment |
|---|---|
| `bits/floatn.h`, `bits/floatn-common.h` (glibc) | force `__HAVE_FLOAT128 0`, `__HAVE_FLOAT64X 0`, `__HAVE_FLOAT64X_LONG_DOUBLE 0` so no `_Float128`/`__float128` prototypes appear |
| `bits/long-double.h` | `__LDOUBLE_REDIRECTS_TO_FLOAT128_ABI 0`, `__NO_LONG_DOUBLE_MATH 1` (glibc then declares the `*l` functions as aliases of the double ones where it supports that configuration) |
| `immintrin.h`, `emmintrin.h`, `xmmintrin.h`, `arm_neon.h`, `cpuid.h`, `x86intrin.h` | `#error "SIMD intrinsics are not available on the CBC target"` with a clear message |
| `setjmp.h` | declares `setjmp`/`longjmp` with `__attribute__((returns_twice))` (already there in glibc) and checks `sizeof(jmp_buf) >= 32` |
| `pthread.h`, `threads.h` | mark the unsupported functions (`pthread_create`, `pthread_once`, `pthread_atfork`, `pthread_key_*`, `thrd_create`, `call_once`, `tss_*`) with `__attribute__((unavailable("not supported on CBC: …")))` when `__CBC_STRICT_LIBC__` is defined (opt-in: `unavailable` fires on every reference in the translation unit, including references in functions that LTO later removes as dead, whereas the default link-time error only fires for references that survive into the program); `threads.h` defines `__STDC_NO_THREADS__` |
| `stdlib.h`, `search.h`, `ftw.h` | the same opt-in `unavailable` attributes for `qsort`, `qsort_r`, `bsearch`, `lfind`, `lsearch`, the `tsearch` family, `ftw`, `nftw` |
| `signal.h` | comment-only: handlers other than `SIG_DFL`/`SIG_IGN` fail at run time (crt guards, `10-linker-and-runtime.md` §5.2) |
| `stdatomic.h`/`<atomic>` | clang's own; `ATOMIC_LLONG_LOCK_FREE` 2 |

### 5.2 Feature-test macros worth knowing

* `__STDC_NO_THREADS__` — defined by the compat `<threads.h>` wrapper (no C11 threads).
* `__STDC_IEC_559__` — keep (IEEE float/double). `__STDC_IEC_559_COMPLEX__` is **not**
  defined: the `<complex.h>` library functions returning `_Complex double` are unsupported
  (they return two registers natively, `10-linker-and-runtime.md` §5.4 rule 4). Complex
  arithmetic in the language (compiler-rt `__muldc3`/`__divdc3`, compiled as CBC) and the
  builtins `creal`, `cimag`, `conj`, plus native `cabs`/`carg`, work. glibc defines the
  macro in `<stdc-predef.h>`, which clang includes implicitly on Linux; the compat headers
  provide a `stdc-predef.h` wrapper that `#include_next`s it and `#undef`s the macro.
* `__CBC_STRICT_LIBC__` — user opt-in; turns uses of unsupported libc functions into
  compile-time errors through the compat headers (§5.1).

## 6. Tests (`clang/test`)

* `Preprocessor/cbc-target.c`: `-dM -E` for both flavours; checks presence/absence of the
  macros in §1.2, `__SIZEOF_LONG_DOUBLE__ 8`, `__CHAR_UNSIGNED__` on AArch64 only.
* `CodeGen/CBC/abi-x86_64.c`, `abi-aarch64.c`: IR signatures for structs of 1–4 words,
  floats/doubles mixed, `__int128`, `sret`, `byval` — must be byte-identical to the IR clang
  emits for `x86_64-linux-gnu`/`aarch64-linux-gnu` apart from the triple and data layout
  (a script diffs them).
* `CodeGen/CBC/varargs.c`: `va_arg` IR against the host `va_list`.
* `CodeGen/CBC/inline-asm.c`: the error diagnostic.
* `Driver/cbc-toolchain.c`: include paths, defaults, linker command line.
* `Sema/cbc-long-double.c`: the warning.
* `Sema/cbc-format-long-double.c`: `%Lf`, `%Le`, `%LG`, `%La` in `printf`, `fprintf`,
  `snprintf`, `wprintf`, `scanf`, `sscanf` constant formats → error with the expected
  fix-it; `%Lf` with a non-constant format → warning; `%Ld` (not a floating conversion,
  glibc accepts it as `%lld`) → no CBC diagnostic.
* `CodeGen/CBC/prototype-attrs.c`: `"cbc-long-double"` on `sinl`, `modfl`
  (`long double *`), `csqrtl`, not on `sin`; `"cbc-fnptr-args"` positions for `qsort`,
  `signal` (`sighandler_t`), `scandir`, `pthread_create`, and calls through function
  pointers.
