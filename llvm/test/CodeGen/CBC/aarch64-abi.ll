; AArch64 (Android) flavour of the CBC backend, agents/28-aarch64-flavour-plan.md.
; Argument registers, tail register and callee-saved registers per host flavour
; (engine PlatformTraits: x86-64 IR1..IR6 / tail IR7, AArch64 IR1..IR8 / tail IR10).
; RUN: llc -mtriple=cbc_x86_64-unknown-linux-gnu %s -o - | FileCheck --check-prefixes=CHECK,X64 %s
; RUN: llc -mtriple=cbc_aarch64-unknown-linux-android %s -o - | FileCheck --check-prefixes=CHECK,A64 %s

declare i64 @ext(i64, i64, i64, i64, i64, i64, i64, i64, i64, i64)
declare void @ext0()
declare double @fext(double, double, double, double, double, double, double, double, double)

; The extra integer arguments are read through the tail register.
define i64 @many(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g, i64 %h, i64 %i, i64 %j) {
; CHECK-LABEL: many:
; X64: ld.raw.mem.64 {{ir[0-9]+}}, ir7, 0
; X64: ld.raw.mem.64 {{ir[0-9]+}}, ir7, 24
; X64-NOT: ir10, 0
; A64: ld.raw.mem.64 {{ir[0-9]+}}, ir10, 0
; A64: ld.raw.mem.64 {{ir[0-9]+}}, ir10, 8
; A64-NOT: ir7, 0
; CHECK: call.direct ext
  %r = call i64 @ext(i64 %j, i64 %i, i64 %h, i64 %g, i64 %f, i64 %e, i64 %d, i64 %c, i64 %b, i64 %a)
  %s = add i64 %r, %i
  ret i64 %s
}

; A value live across a call sits in a callee-saved register: IR8.. on x86-64,
; IR11..IR13 on AArch64 (IR8..IR10 are volatile there).
define i64 @keep(i64 %a, i64 %b) {
; CHECK-LABEL: keep:
; X64: add.w64 ir8, ir1, ir2
; A64: add.w64 ir11, ir1, ir2
; CHECK: call.direct ext0
  call void @ext0()
  %s = add i64 %a, %b
  ret i64 %s
}

; The sret pointer is a plain first integer argument in both flavours.
%S = type { i64, i64, i64 }
define void @sret(ptr sret(%S) %out, i64 %x) {
; CHECK-LABEL: sret:
; CHECK: st.raw.mem.64 ir2, ir1, 0
  store i64 %x, ptr %out
  ret void
}

; Floating-point arguments: FR0..FR7 in both flavours, the ninth through the tail register.
define double @flt(double %a, double %b, double %c, double %d, double %e, double %f, double %g, double %h, double %i) {
; CHECK-LABEL: flt:
; X64: ld.raw.mem.64 ir1, ir7, 0
; A64: ld.raw.mem.64 ir1, ir10, 0
; CHECK: call.direct fext
  %r = call double @fext(double %i, double %h, double %g, double %f, double %e, double %d, double %c, double %b, double %a)
  ret double %r
}
