; RUN: llc -mtriple=cbc_x86_64-unknown-linux-gnu %s -o - | FileCheck %s

declare void @use(ptr)

define void @escaping() {
; CHECK-LABEL: escaping:
; CHECK: cbc-frame untypedmem=16 usesalloca=0
; CHECK: lea.frame
; CHECK-NOT: __cbc_shadow_stack_init
  %x = alloca i32, align 4
  store i32 1, ptr %x
  call void @use(ptr %x)
  ret void
}

define void @empty() {
; CHECK-LABEL: empty:
; CHECK: cbc-frame untypedmem=0 usesalloca=0
  ret void
}

define ptr @dyn(i64 %n) {
; CHECK-LABEL: dyn:
; CHECK: cbc-frame untypedmem=0 usesalloca=1
; CHECK: alloca ir
  %p = alloca i8, i64 %n, align 16
  ret ptr %p
}

define void @vla(i64 %n) {
; CHECK-LABEL: vla:
; CHECK: usesalloca=1
; CHECK: stacksave
; CHECK: alloca ir
; CHECK: stackrestore
  %s = call ptr @llvm.stacksave.p0()
  %p = alloca i8, i64 %n, align 16
  call void @use(ptr %p)
  call void @llvm.stackrestore.p0(ptr %s)
  ret void
}

declare ptr @llvm.stacksave.p0()
declare void @llvm.stackrestore.p0(ptr)

define void @eh() personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: eh:
; CHECK-NOT: st.frame ir13
  invoke void @use(ptr null) to label %ok unwind label %lpad
ok:
  ret void
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}

declare i32 @__gxx_personality_v0(...)

define void @__cbc_check_host() {
  ret void
}

define i32 @main() {
; CHECK-LABEL: __cbc_entry:
; CHECK: ld.fcb
; CHECK-NOT: __cbc_shadow_stack_init
  ret i32 0
}
