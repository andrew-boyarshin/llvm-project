# 28 — AArch64 flavour plan (Android first)

Status: **plan, not implemented.** Delivers the AArch64 CBC flavour (roadmap item F1 in
`11-testing-and-roadmap.md` §3) for **Android arm64 only**, cross-compiled from an x86-64 Linux
host. AArch64 Linux (glibc) is deferred (§7). Design background: `01` §3 (calling convention
facts), `04` §2 (triples), `06` (backend), `13` (engine changes), `21` (aggregate ABI), `24`
(lld), `14` §13 and `10` §8 (install layout, `cbc.<host triple>`).

## 0. Scope and terms

A CBC *flavour* is the host ABI the `.cbc` runs on (`04` §2).

| Name | Clang spelling | Engine | libc |
|---|---|---|---|
| x86-64 Linux (done) | `--target=x86_64-unknown-linux-gnu -fcbc` | x86-64 Linux | glibc |
| **AArch64 Android (this plan)** | `--target=aarch64-linux-android23 -fcbc` | Android arm64 (builds at API 26 today, D1) | bionic |
| AArch64 Linux (deferred) | `--target=aarch64-unknown-linux-gnu -fcbc` | AArch64 Linux | glibc |

* Compile host (where clang/lld run): x86-64 Linux. Every AArch64 build is a cross-compile; the
  target headers and libs come from the **NDK sysroot**, so no extra sysroot has to be sourced.
* Module shape: Android apps load shared libraries, so only the **DSO** wrap path is in scope
  (no PIE `main`, no `libcbc-main.a` on Android).
* Code generation is plain AAPCS64; the engine uses `src/arch_os/aarch64_linux` for Android too
  (`src/arch_os/CMakeLists.txt`). Nothing in code generation is Android-specific, which is
  why the deferred AArch64 Linux flavour reuses W1 as is.

## 0.1 P0 status (plumbing, implemented, not yet built or run)

* Driver: `getTargetDirName` gives `cbc.aarch64-unknown-linux-android` for Android and keeps
  `cbc.aarch64-unknown-linux-gnu` for the deferred glibc flavour; `Linux::Linux` accepts
  `aarch64` + Android (AArch64 glibc is still rejected); Android picks
  `linux_android_aarch64_cjnative`. Test: `clang/test/Driver/cbc-android.cpp`.
* compiler-rt: `get_compiler_rt_target` strips the Android API level from the directory name.
* `llvm/tools/cbc-ld/CMakeLists.txt`: `CBC_FLAVORS`, `CBC_ANDROID_API`, `ANDROID_NDK_ROOT`; one
  `cbc_add_flavor` per flavour; Android builds crt/stub only (no `libcbc-main`, no x86 headers).
  Enabling it before P1 fails at the first compile (backend rejects AArch64).
* Scripts: `configure.sh <mode> [x86_64 aarch64]`; `build.sh <mode> [x86_64|aarch64]` where
  `aarch64` is Android (NDK discovery, `CBC_SYSROOT`, `CBC_ANDROID_API`, default 23);
  `configure-runtimes.sh` passes `CMAKE_SYSROOT` and `aarch64-linux-android23`; `build.sh` checks
  the compiler build was configured for the flavour.
* Not done in P0 (later phases): NDK sysroot use in the driver (W2.3), per-flavour crt header
  split (W3.1), anything in the backend.

## 0.2 P1 status (compile-only, in progress)

Verified with a dev build (`llc`, `clang`, `lld` built; lit: `llvm/test/CodeGen/CBC/aarch64-abi.ll`,
`aarch64-aggregates.ll`, `clang/test/Driver/cbc-android.cpp`, all passing):

* `CBCTargetMachine` accepts AArch64; `CC_CBC_A64` (IR1..IR8, FR0..FR7), tail register IR10,
  callee-saved IR11..IR13 + FR8..FR15, per-flavour preserved mask; `sret` is a plain first
  integer argument in both flavours (the engine does not flag C methods, so the `IR9` rule of
  `06` §5.1 is not used); HFAs of three or four members are demoted to a hidden sret pointer.
* `clang --target=aarch64-linux-android23 -fcbc -c` produces AAPCS64 IR (sret, `[2 x i64]`
  coercion, HFA structs, unsigned `char`), NEON disabled; `llc -march=cbc` generates code for it.

Found, not done:
* `long double` (binary128 by D5): `CBCRejectUnsupportedIR` rejects `long double/fp80/fp128`
  signatures and Sema warns "long double operations are not available in CBC code", so D5 needs
  the soft-float path enabled (reject lists, legalization, split `i128` arguments in
  `rejectUnsupportedArgs`) before it works.
* Variadics (done at IR/codegen level): `ExpandVariadics` has a `CBCAArch64` ABI
  (`llvm/lib/Transforms/IPO/ExpandVariadics.cpp`). A call packs the variadic arguments into one
  buffer laid out like the AAPCS64 stack-argument area (8-byte slots, 16 for 16-byte aligned
  types, composites larger than 16 bytes other than HFAs by reference); the va_list
  `{ __stack = buffer, __gr_top = null, __vr_top = null, __gr_offs = 0, __vr_offs = 0 }` makes
  clang's inline `va_arg` always take the on-stack path. The pass now picks the flavour from the
  module triple (before this, an AArch64 module silently got the x86-64 layout). Declarations
  keep the native variadic convention (AAPCS64 non-Apple passes variadic arguments like named
  ones). Test: `llvm/test/Transforms/ExpandVariadics/cbc-aarch64.ll`; variadic C compiles to a
  CBC object (`llc -march=cbc -filetype=obj`). Not run on the engine yet (P2).
* `llc` text output (`-filetype=asm`) cannot print branches (`operand lowering is not
  implemented yet`), so any test with control flow needs `-filetype=obj` (requires a `main`).
* Link, crt and run: P2.

## 1. State of the tree

* `Triple` parses `cbc_aarch64` / `CBCSubArch_aarch64`; lld maps it to `EM_AARCH64`
  (`lld/ELF/InputFiles.cpp:1799`) and names the entry `…aarch64…` (`lld/CBC/CBCWrap.cpp:51`).
* The LLVM build contains the AArch64 backend (`configure.sh`: `X86;AArch64`).
* Driver paths and install layout are per flavour (`getTargetDirName`, `cbc.<host triple>`);
  scripts take `[x86_64|aarch64]` (`cbc-env.sh`, `configure-runtimes.sh`, `build.sh`).
  **Today `aarch64` maps to `aarch64-unknown-linux-gnu`; P0 retargets it to Android** (D1).
* The engine has AArch64 trampolines and `abi.inc`, and builds for Android
  (`cbc-engine/build.py --target-os=android --target-arch=aarch64`, NDK, API 26).
* **Blocked today:** `CBCTargetMachine.cpp:31` (`isSupportedCBCTriple`) rejects AArch64;
  `Linux::Linux` (`Linux.cpp:228`) accepts only `x86_64` + Linux; `cbc-ld`'s CMake builds
  crt/stub/`libcbc-main` for x86-64 only; the Cangjie runtime dir for AArch64 is
  `linux_aarch64_cjnative` (`CBC.cpp:70`), wrong for Android.

## 2. Streams

### W1 — Backend (`llvm/lib/Target/CBC`) — the F1 core
1. Make `isSupportedCBCTriple` accept `CBCSubArch_aarch64` and `Triple::aarch64`; data layout
   `e-m:e-p:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128` (`04` §2.1).
2. `CBCCallingConv.td` / `CBCCallInfo.h`: per-sub-arch tables. Integer args `IR1..IR8`
   (x0–x7), `sret` in `IR9` (x8), tail register `IR10`, floats `FR0..FR7`, callee-saved
   `IR11–IR13`, `FR8–FR15` (`01` §3.3). Allocation order and `usedNonVolIRegMask` follow.
3. Aggregates (`21`): AAPCS64 rules — ≤16 bytes in registers (two registers), >16 bytes
   indirect, HFAs of 2–4 floating members in `FR`s. Native calls that return a second
   register or an HFA stay unsupported (the engine copies back one integer and one float
   result, `01` §3.1); keep the link-time rejection and extend it to HFA returns.
4. Variadics: AAPCS64 `va_list` struct (`__stack, __gr_top, __vr_top, __gr_offs, __vr_offs`),
   `VariadicABIInfo`, `ExpandVariadics` integration (`04` §10). Variadic args are passed like
   named ones (no `al`; E7 does not apply).
5. `CBCFrameLowering`, `CBCLowerByVal`, `CBCLowerSjLj`/`CBCLowerEH`: audit for x86 assumptions
   (the sub-arch appears in only `CBCTargetMachine`, `CBCSubtarget.h`, `CBCOpcodes.h`,
   `CBCRejectUnsupportedIR.cpp`, so the surface is small).
6. `CBCRejectUnsupportedIR`: keep rejecting `llvm.aarch64.neon.*` / `sve.*`. Atomics ≤ 64 bit.
7. Tests: `llc` tests for every ABI case; an **IR-level ABI-parity corpus** for both flavours
   (risk R22).

### W2 — Clang (front end and driver)
1. `TargetInfo` for the CBC-on-AArch64 type model: `char` unsigned, `wchar_t` unsigned int,
   `__int128`, `va_list` kind, `__aarch64__`/`__LP64__`/`__ANDROID__` macros, `__ARM_NEON`
   **undefined**, atomics ≤ 64 (existing guard at `InitPreprocessor.cpp`). `long double`
   **follows native** (D5): binary128, size/align 16; the x86-64 flavour keeps its policy (`09` §2).
2. Driver (`Linux.cpp`): allow `aarch64` + the Android environment in the CBC branch;
   `getTargetDirName` derives from arch **and** environment, giving
   `cbc.aarch64-unknown-linux-android` for Android (API level not part of the name, D1).
3. **Sysroot:** CBC mode currently takes the build host's `/usr/include`. For Android the driver
   must use the NDK sysroot (`--sysroot=<ndk>/toolchains/llvm/prebuilt/linux-x86_64/sysroot`,
   headers in `usr/include` and `usr/include/aarch64-linux-android`), never the host's headers.
   Include order: CBC resource headers → `include/P/c++/v1` → `include/c++/v1` → sysroot. The NDK
   sysroot also ships `usr/include/c++/v1`; it must come after ours (add a test).
4. Android specifics: API-level macro (`__ANDROID_API__`), default API 23 (the Cangjie SDK
   Android targets are `android23`), reject API < 21 (NDK r28 minimum); pick
   `CANGJIE_HOME/runtime/lib/linux_android_aarch64_cjnative` instead of
   `linux_aarch64_cjnative` (`CBC.cpp:70`).
5. Lit tests: driver `-###` tests (paths, order, errors), preprocessor `-dM -E` tests,
   unsupported-target diagnostics.

### W3 — Linker and crt (`lld/CBC`, `llvm/tools/cbc-ld`)
1. `cbc-ld/CMakeLists.txt`: replace the single `CBC_HOST_TRIPLE` by a flavour list
   (`CBC_FLAVORS`, default `x86_64-unknown-linux-gnu`; the Android entry needs
   `ANDROID_NDK_ROOT`). Build and install `crt-cbc.bc` and `cbc_can_catch_stub.bc` per flavour
   into `lib/P`, plus the per-flavour compat headers into `include/P`. `libcbc-main.a` is not
   built for Android. `crt/include/` is x86-specific (`immintrin.h`, …); split into
   `crt/include/x86_64` and `crt/include/aarch64` (the AArch64 shadow `arm_neon.h` must keep
   winning over the now-generated resource one).
2. Native calls: `CBCNativeLibc.def` and the native-call legalizer (`10` §5.4) need a per-libc
   list. bionic differs from glibc (`__errno` vs `__errno_location`, ctype tables, no
   `__isoc99_*` aliases, stdio objects, `long double` `…l` renames, `qsort_r`, 64-bit `stat`
   names). Generate the bionic list from the NDK stubs (`llvm-nm -D` on
   `sysroot/usr/lib/aarch64-linux-android/23/libc.so`, `libm.so`, `libdl.so`) instead of
   hand-editing.
3. DSO wrap path only (`CBCWrap`, `.init_array`, `-shared`); entry type name suffix per
   flavour (`$cbc.<prog>.arm64:Entry`, `04` §2). Reject non-`-shared` links for Android
   with a clear diagnostic.

### W4 — Runtimes (compiler-rt builtins, libc++, libc++abi)
1. `configure-runtimes.sh`: for the Android flavour pass `CMAKE_SYSROOT` / `--sysroot=` from
   the NDK and `-D__ANDROID_API__`-equivalent triple (`aarch64-linux-android23`); the real
   compile triple stays host-style + `-fcbc`, and the `cbc.<triple>` directories already work.
2. compiler-rt builtins: CBC uses generic C sources plus the soft-`tf` sources
   (`builtins/CMakeLists.txt:396`). With binary128 `long double` (D5) `__addtf3`, `__multf3`,
   `__extenddftf2`, `__fixtfdi`, the comparisons, … must all be present, and no AArch64-asm
   file may leak in.
3. libc++/libc++abi: `__config_site` per flavour (no threads, no pthread API, localization per
   bionic's locale support). Check `_LIBCPP_HAS_*` for bionic (musl macro off, bionic locale
   and `wchar` quirks) and that the shared generic headers stay valid (guard rail in `build.sh`).
4. `build.sh <mode> <flavour>`: flavours `x86_64` and `aarch64` (= Android arm64, API 23);
   required-files list per flavour (no `libcbc-main.a` for Android); `ANDROID_NDK_ROOT`
   with latest-NDK discovery as in `CJMP-Engine/build_android_arm64.sh`.

### W5 — Engine verification (`cbc-engine`)
E1–E6 and E8 verified on the AArch64 engine (E7 is x86-64 only, `11` F1). Run the
differential encoder test, loader test, `cbc/engine-ext/` and `cbc/engine-probe/` on an Android
emulator or device via `adb`. Confirm the AArch64 trampolines under bionic (TLS, `dlsym`,
W^X policies, `libcangjie-runtime` location).

### W6 — Cangjie / app integration
* `CANGJIE_HOME/runtime/lib/<dir>` selection (W2.4).
* **Engine library placement (D4, resolved).** Link time: copy the Android `libcbcengine.so`
  (from `cbc-engine`, `output/android_aarch64/`) into
  `runtime/lib/linux_android_aarch64_cjnative/` of the hotfix SDK, as the SDK already does for
  `linux_x86_64_cjnative`; document it in `cjmp-hotfix-toolchain/build.md`.
* App packaging: the wrapped `.so` and `libcbcengine.so` go into the app's `lib/arm64-v8a`
  alongside the Cangjie runtime libraries. That is a packaging step of whatever builds the app,
  not part of this compiler work; CJMP-Engine (`libkeels_android.so`) is only a neighbour in the
  same app.

## 3. Phasing and exit criteria

| Phase | Content | Exit criterion | Effort (weeks) |
|---|---|---|---|
| P0 | Plumbing: flavour `aarch64` retargeted to Android (`cbc.aarch64-unknown-linux-android`), NDK sysroot option in scripts and CMake; no behaviour change for x86-64 | x86-64 build/tests unchanged; `-###` tests for the Android paths | 0.5 |
| P1 | W1 + W2.1/2.2/2.5, compile-only: C source → `.cbc` object for Android; `llc` and IR ABI-parity tests | all M2 code-gen tests pass for AArch64 (compile only) | 3 |
| P2 | W2.3/2.4, W3, W5, W6 for C: link a DSO, run on an Android arm64 device | M2–M3 suites green on device | 2–3 |
| P3 | W4: builtins, libc++, libc++abi; EH | M4 (C++) suites green on device; C++ sample runs inside the CJMP app | 2 |
| P4 | M5 quality on AArch64; CI (Android emulator job) | benchmarks and CI green | 2 |

Roughly 10–11 engineer-weeks. P1 is the F1 estimate (4–6) minus runtimes, which F1 did not
count. P2 and the start of P3 can overlap with two people.

## 4. Testing and CI

* Lit: `llvm/test/CodeGen/CBC/AArch64/…`, `clang/test/Driver/cbc-android*.c`,
  `lld/test/CBC/aarch64/…`; IR-level ABI-parity corpus for both flavours.
* Execution: Android emulator or device with `adb`. The cross-compile job on the x86-64 runner
  needs the NDK (pin the version, as the other prebuilts).
* `cbc-stdlib-tests/run.sh`: parametrise by flavour (`--target`, sysroot, runner command, `adb`
  push and run).

## 5. Risks

| Id | Risk | Mitigation |
|---|---|---|
| A1 | HFA / two-register aggregate returns of native callees lose the second register (`01` §3.1) | reject at link time (already the policy), add AArch64 cases to the unsupported list |
| A2 | `long double` follows native (binary128). Layout matches native headers, but the engine copies only `d0..d7` (64-bit) at native calls, so a native call that takes or returns `long double` (`sinl`, `strtold`, `printf("%Lf")`) cannot work | reject such native calls at link time (same mechanism as HFA returns); CBC-to-CBC calls use the CBC ABI; add `long double` rows to the ABI-parity corpus |
| A3 | bionic symbol set varies with API level (23 baseline, engine at 26) | generate the list from the API 23 stubs; symbols newer than 23 are rejected at link time |
| A4 | the NDK's own `include/c++/v1` shadows ours | include-order test (W2.3) |
| A5 | AArch64 flavour designed but never executed (R22) | IR-level parity corpus from P1, early device tests in P2 |
| A6 | Android DSO shape and app lifecycle (init order, `dlopen`, no `main`) | DSO-only (D3), device tests in P2 |
| A7 | generated resource `arm_neon.h` (now built because AArch64 is enabled) vs the crt shadow | per-flavour crt header dirs; shadow stays first in the include order |
| A8 | CBC compiled against API 23 but the engine requires API 26 today | the 23 baseline only guarantees symbol availability; lowering the engine is a separate task |

## 6. Decisions

* **D1** (resolved) Android API level 23, one directory `cbc.aarch64-unknown-linux-android`
  (no API level in the name). Headers and libc++ compile at API 23 (the Cangjie SDK Android
  targets are `android23`); the driver rejects API < 21 (NDK r28 floor). CJMP-Engine
  (`build/build_plugins/config/aosp/sdk.gni`, 26; 29 in fusion mode) and `cbc-engine`
  (`build.py`, `android-26`) build at 26, so the run-time floor stays 26 until the engine is
  built lower (A8).
* **D2** (dropped) No AArch64 Linux header sysroot is needed while AArch64 Linux is deferred;
  Android uses the NDK sysroot.
* **D3** (resolved) Android is DSO-only: apps load shared libraries and cannot exec a PIE `main`
  from app storage.
* **D4** (resolved: option (a)) The hotfix SDK already fixes the convention
  (`/home/user/dev/hotfix-sdk/cangjie`, procedure in `cjmp-hotfix-toolchain/build.md` step 4):
  `libcbcengine.so` is installed into `runtime/lib/<target>_cjnative/` (today
  `linux_x86_64_cjnative`), and `tools/lib/libcbcengine.so` is only a symlink to the host
  flavour's copy (`../../runtime/lib/linux_x86_64_cjnative/libcbcengine.so`; same for
  `libcbcengine-helper.so`). The Android build follows it: copy
  `cbc-engine/output/android_aarch64/libcbcengine.so` into
  `runtime/lib/linux_android_aarch64_cjnative/` (the directory the driver adds for Android,
  W2.4); no driver change and no new search path. Note `build.md` step 4 only documents the
  `linux_*` outputs, so the Android copy step (output name and target directory) must be added
  there. The two engines are unrelated products: `cbc-engine` is the bytecode interpreter and
  builds `libcbcengine.so` (`build.py --target-os=android --target-arch=aarch64`); CJMP-Engine
  (the UI engine) plays no part.
* **D5** (resolved) `long double` follows native (binary128) on AArch64; see A2.
* **D6** (resolved) Hosting the toolchain on AArch64 is out of scope.

## 7. Deferred: AArch64 Linux (glibc)

Reuses W1, W2.1, the AArch64 half of W3.1 and W4.2/4.3. It would add: AArch64 glibc headers as
`--sysroot` (Debian `libc6-dev-arm64-cross`, a user-provided `CBC_AARCH64_LINUX_SYSROOT`, or
bundled headers), a glibc native symbol list, the PIE path with `libcbc-main.a` for AArch64,
the `linux_aarch64_cjnative` runtime directory, a QEMU or AArch64 runner, and a directory
`cbc.aarch64-unknown-linux-gnu`. Roughly 2–3 extra weeks.
