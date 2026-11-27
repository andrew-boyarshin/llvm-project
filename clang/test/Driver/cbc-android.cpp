// CBC on Android arm64 (agents/28-aarch64-flavour-plan.md, P0): install layout names and
// driver acceptance only. Code generation for this flavour is not implemented yet, so
// every RUN line here uses -### (no compilation).

// Link step: per-flavour directory "cbc.aarch64-unknown-linux-android" (no API level),
// Cangjie runtime directory for Android, builtins from the resource dir.
// RUN: env CANGJIE_HOME=/sdk %clang --target=aarch64-linux-android23 -fcbc -shared -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK %s
// LINK: "--cbc"
// LINK: "--crt" "{{[^"]*}}/lib/cbc.aarch64-unknown-linux-android/crt-cbc.bc"
// LINK: "-L" "{{[^"]*}}/lib/cbc.aarch64-unknown-linux-android"
// LINK: "-L" "{{[^"]*}}/lib/cbc.aarch64-unknown-linux-android/native"
// LINK: "-L" "/sdk/tools/lib"
// LINK: "-L" "/sdk/runtime/lib/linux_android_aarch64_cjnative"
// LINK: "{{[^"]*}}/lib/cbc.aarch64-unknown-linux-android/libclang_rt.builtins.a"

// Compile step: CBC-only headers, then target-specific libc++ headers, then the shared
// generic ones (same order as native per-target libc++).
// RUN: %clang --target=aarch64-linux-android23 -fcbc -x c++ -fsyntax-only -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CXX %s
// CXX: "-cc1"
// CXX: "-internal-isystem" "{{[^"]*}}/include/cbc.aarch64-unknown-linux-android/c++/v1"
// CXX: "-internal-isystem" "{{[^"]*}}/include/c++/v1"

// No vector registers: NEON is disabled for the AArch64 flavour only.
// RUN: %clang --target=aarch64-linux-android23 -fcbc -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NEON %s
// NEON: "-cc1"
// NEON-SAME: "-target-feature" "-neon"
// RUN: %clang --target=x86_64-unknown-linux-gnu -fcbc -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=X86-NO-NEON %s
// X86-NO-NEON-NOT: "-neon"

// The x86-64 flavour keeps its layout.
// RUN: env CANGJIE_HOME=/sdk %clang --target=x86_64-unknown-linux-gnu -fcbc -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=X86 %s
// X86: "--crt" "{{[^"]*}}/lib/cbc.x86_64-unknown-linux-gnu/crt-cbc.bc"
// X86: "-L" "/sdk/runtime/lib/linux_x86_64_cjnative"

// AArch64 glibc (Linux) is deferred and still rejected.
// RUN: not %clang --target=aarch64-unknown-linux-gnu -fcbc -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=GNU %s
// GNU: error:

int main() { return 0; }
