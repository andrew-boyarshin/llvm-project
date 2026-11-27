; RUN: opt -S -passes=expand-variadics -expand-variadics-override=lowering %s | FileCheck %s

; CBC AArch64 flavour (agents/28-aarch64-flavour-plan.md): variadic arguments are
; packed into one buffer laid out like the AAPCS64 stack-argument area, and the
; va_list { __stack, __gr_top, __vr_top, __gr_offs, __vr_offs } is initialised so
; clang's inline va_arg always takes the on-stack path.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-linux-android23"

%struct.__va_list = type { ptr, ptr, ptr, i32, i32 }

declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_end.p0(ptr)
declare void @sink(ptr)

; CHECK-LABEL: define {{.*}}i32 @callee(i32 %n, ptr %varargs)
; CHECK: call void @llvm.memcpy.p0.p0.i32(ptr %ap, ptr %varargs, i32 32, i1 false)
define i32 @callee(i32 %n, ...) {
entry:
  %ap = alloca %struct.__va_list, align 8
  call void @llvm.va_start.p0(ptr %ap)
  call void @sink(ptr %ap)
  call void @llvm.va_end.p0(ptr %ap)
  ret i32 %n
}

; Every variadic argument gets a naturally aligned 8-byte slot in order.
; CHECK-LABEL: define {{.*}}i32 @caller()
; CHECK: %vararg_buffer = alloca %caller.vararg, align 16
; CHECK: store double 1.500000e+00, ptr {{.*}}, align 8
; CHECK: store i64 20, ptr {{.*}}, align 8
; CHECK: store i32 7, ptr {{.*}}, align 4
; CHECK: store ptr %vararg_buffer, ptr %{{[0-9]+}}, align 8
; CHECK: store ptr null, ptr %{{[0-9]+}}, align 8
; CHECK: store ptr null, ptr %{{[0-9]+}}, align 8
; CHECK: store i32 0, ptr %{{[0-9]+}}, align 4
; CHECK: store i32 0, ptr %{{[0-9]+}}, align 4
; CHECK: call i32 @callee(i32 3, ptr %va_argument)
define i32 @caller() {
entry:
  %r = call i32 (i32, ...) @callee(i32 3, double 1.5, i64 20, i32 7)
  ret i32 %r
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"CBC", i32 1}
