; AArch64 (Android) CBC flavour: IR shapes produced by clang for AAPCS64
; (agents/28-aarch64-flavour-plan.md). The module carries the "CBC" flag that
; `clang -fcbc` sets, so llc selects the CBC backend with -march=cbc.
; RUN: llc -march=cbc %s -o - | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-linux-android23"

%struct.S = type { i64, i64, i64 }
%struct.H = type { double, double, double, double }

; A 16-byte aggregate is coerced to [2 x i64] and arrives in two integer registers.
define i64 @sum2([2 x i64] %p.coerce) {
; CHECK-LABEL: sum2:
; CHECK: add.w64 ir1, {{ir[12]}}, {{ir[12]}}
  %p.coerce.fca.0.extract = extractvalue [2 x i64] %p.coerce, 0
  %p.coerce.fca.1.extract = extractvalue [2 x i64] %p.coerce, 1
  %add = add nsw i64 %p.coerce.fca.1.extract, %p.coerce.fca.0.extract
  ret i64 %add
}

; A larger aggregate is returned through an sret pointer, a plain first argument.
define void @mk(ptr noalias sret(%struct.S) align 8 %agg.result, i64 %x) {
; CHECK-LABEL: mk:
; CHECK: st.raw.mem.64 {{ir[0-9]+}}, ir1, 16
; CHECK: st.raw.mem.64 ir2, ir1, 0
  %b = getelementptr inbounds i8, ptr %agg.result, i64 8
  %c = getelementptr inbounds i8, ptr %agg.result, i64 16
  %x1 = add i64 %x, 1
  %x2 = add i64 %x, 2
  store i64 %x, ptr %agg.result
  store i64 %x1, ptr %b
  store i64 %x2, ptr %c
  ret void
}

; An HFA of four doubles does not fit the two result registers; the backend
; demotes it to a hidden sret pointer, consistently for caller and callee.
define %struct.H @mkh(double %x) {
; CHECK-LABEL: mkh:
; CHECK: st.raw.mem.f64 fr0, ir1, 24
; CHECK: st.raw.mem.f64 fr0, ir1, 0
  %a = insertvalue %struct.H poison, double %x, 0
  %b = insertvalue %struct.H %a, double %x, 1
  %c = insertvalue %struct.H %b, double %x, 2
  %d = insertvalue %struct.H %c, double %x, 3
  ret %struct.H %d
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"CBC", i32 1}
