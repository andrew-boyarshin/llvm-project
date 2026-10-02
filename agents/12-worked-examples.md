# 12 — Worked Examples

All examples use the **x86-64 flavour** (`cbc_x86_64-unknown-linux-gnu`, the flavour in
the current work scope): integer arguments in `IR1..IR6`, floats in `FR0..FR7`, results in
`IR1`/`FR0`, `IR8..IR13` callee-saved, `IRACC` reserved (written by the engine). Byte
encodings follow `02-isa-encoding.md`;
offsets are original-bytecode positions. Method references are shown as `@m<k>`; their
`uleb` index bytes are written `mm`.

## 1. Straight-line integer code

```c
long add3(long a, long b, long c) { return a + b + c; }
```

```llvm
define i64 @add3(i64 %a, i64 %b, i64 %c) {
  %1 = add i64 %a, %b
  %2 = add i64 %1, %c
  ret i64 %2
}
```

```
add3:                         ; method "add3" : FUNCTIONAL(I64, I64, I64 -> I64), flags PUBLIC|STATIC
 0: add.w64 ir1, ir2         ; 2a 12      Add64 short form, d = l = IR1
 2: add.w64 ir1, ir3         ; 2a 13
 4: ret.w64 ir1              ; 45 11
```

Code header: `untypedSlotCount 0`, no typed slots, masks `0/0`, no state points, no
exception regions.

## 2. A loop with a load, a back edge and a safepoint

```c
long sum(const int *p, long n) {
  long s = 0;
  for (long i = 0; i < n; ++i) s += p[i];
  return s;
}
```

IR after optimization and `CBCInsertSafepoints`:

```llvm
define i64 @sum(ptr %p, i64 %n) {
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  %a = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load i32, ptr %a, align 4
  %v64 = sext i32 %v to i64
  %s.next = add nsw i64 %s, %v64
  %i.next = add nuw nsw i64 %i, 1
  call void @llvm.cbc.gcpoint()
  %c = icmp slt i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  %r = phi i64 [ 0, %entry ], [ %s.next, %loop ]
  ret i64 %r
}
```

```
sum:
 0: mov.w64  ir3, irz               ; 15 30          s = 0
 2: mov.w64  ir4, irz               ; 15 40          i = 0
 4: bcc.w64  ge, irz, ir2, .Lexit   ; 09 02 16 00    Bcc64Ge, end = 8, delta = 30 - 8 = 22
.Lloop:
 8: lsli.w64 ir5, ir4, 2            ; 38 c5 42 00    BinaryImm64 op=lsl(12) d=5 l=4 lo4=2 sleb(0)
12: add.w64  ir5, ir1, ir5          ; 36 05 15       Binary64 op=add d=5 l=1 r=5 (d != l: 3-address form)
15: ld.raw.s32 ir5, [ir5 + 0]       ; 60 55 e0 00    ldk = s32to64 (14), disp 0
19: add.w64  ir3, ir5               ; 2a 35          s += v
21: addi.w64 ir4, ir4, 1            ; 38 04 41 00
25: gcpoint                         ; 3b             state point → liveness {26, 0, 0, 0}
26: bcc.w64  lt, ir4, ir2, .Lloop   ; 08 42 ea ff    end = 30, delta = 8 - 30 = -22
.Lexit:
30: mov.w64  ir1, ir3               ; 15 13
32: ret.w64  ir1                    ; 45 11
```

Notes:

* `IRZ` serves as the constant 0 both for initialization and for the guard compare.
* `sext` of the loaded `i32` is folded into the load (`LD_S32TO64`).
* The rewriter turns each of these into one RT instruction; none needs a literal.

## 3. Globals, a native call, and the clobber set of native calls

```c
#include <stdio.h>
static int counter;
const char *msg = "hi";
int main(void) { counter++; puts(msg); return counter; }
```

Data image built by `CBCLowerGlobals` (initialized globals first, sorted by alignment):

| Global | Offset | Size | Initial bytes |
|---|---|---|---|
| `msg` | 0 | 8 | `08 00 00 00 00 00 00 00` + relocation `IMAGE_ABS64 @0` (target `.str` at 8) |
| `.str` | 8 | 3 | `68 69 00` |
| `counter` | 12 | 4 | zero (not in the blob) |

`N = 16`, blob `B` = 11 bytes, relocation blob `R` = `{0}`.

```
__cbc_main:                          ; C main renamed by CBCSynthesizeEntry
 0: lea.s     ir2, ir4, @image       ; 7c 24 ii       field ref $cbc.<p>:Data.image (uleb index ii); ir4 = dead base-ref
 3: ld.raw.32 ir3, [ir2 + 12]        ; 60 32 2c 00    ldk 32 (2), lo4 = 0xC
 7: addi.w32  ir3, ir3, 1            ; 37 03 31 00
11: st.raw.32 ir3, [ir2 + 12]        ; 61 32 2c 00    stk 32 (2)
15: ld.raw.64 ir1, [ir2 + 0]         ; 60 12 b0 00    ldk 64 (11): IR1 = msg
19: call      ir1, @native(puts)     ; 44 31 mm       state point → liveness {22, 0, 0, 0}
22: mov.w64   ir1, ir3               ; 15 13          IR3 survived the native call
24: ret.w64   ir1                    ; 45 11
```

* `@native(puts)` is method reference `{name "puts", flags AOT, refType AOT_REF
  "cbc.native", sig FUNCTIONAL(I64 -> I64)}` with a direct-call AOT entry
  `{refIndex, "puts"}`.
* Because native calls clobber only `IR1`/`FR0` (`CSR_CBC_Native`), `counter`'s value stays
  in the volatile register `IR3` across the call; `usedNonVolIRegMask = 0`.
* The upper half of `IR3` is undefined after `addi.w32`; the entry method sign-extends the
  `i32` result.

## 4. Two-register return and `sret`

```c
struct P { long x, y; };                       /* INTEGER, INTEGER → IR1, IR2 */
struct P mk(long a) { return (struct P){ a, a + 1 }; }
struct B { long v[4]; };                       /* MEMORY → sret in IR1 */
struct B big(long a) { struct B b = {{a, a, a, a}}; return b; }
```

```llvm
define { i64, i64 } @mk(i64 %a)                        ; clang's SysV coercion
define void @big(ptr sret(%struct.B) align 8 %out, i64 %a)
```

```
mk:
  mov.w64  ir2, ir1        ; 15 21
  addi.w64 ir2, ir2, 1     ; 38 02 21 00
  ret.w64  ir1             ; 45 11      IR1 = x, IR2 = y (CBC→CBC: both registers reach the caller)

big:                        ; IR1 = out (sret), IR2 = a
  st.raw.64 ir2, [ir1 + 0]   ; 61 21 30 00
  st.raw.64 ir2, [ir1 + 8]   ; 61 21 38 00   lo4 = 8
  st.raw.64 ir2, [ir1 + 16]  ; 61 21 30 01   lo4 = 0, sleb(1)
  st.raw.64 ir2, [ir1 + 24]  ; 61 21 38 01
  ret.w64  ir1               ; 45 11
```

If `mk` were a **native** function, the call would be rejected at compile time
(`native function 'mk' returns its value in two registers`), because the engine copies back
only `rax`.

## 5. An escaping local in the frame

```c
void use(int *);
int f(void) { int x = 1; use(&x); return x; }
```

```
f:                                 ; untypedMemSize = 16, usesAlloca = 0
                                   ; IR8 is callee-saved (the engine saves it)
  lea.frame ir8, 0                 ; 45 a8 00 00 00 00    &x
  movi.w64  ir1, 1                 ; 17 11 00
  st.raw.32 ir1, [ir8 + 0]         ; 61 18 20 00
  mov.w64   ir1, ir8               ; 15 18             &x → argument
  call      ir1, @use              ; 44 31 mm          liveness entry after it
  ld.raw.32 ir1, [ir8 + 0]         ; 60 18 20 00
  ret.w64   ir1                    ; 45 11
```

`x` is 16 bytes of the interpreter frame (the engine's alignment). The address stays valid
across `use`, including if that call grows the fiber stack, because the frame does not
move (`04-architecture.md` §5.2). There is no prologue. A dynamic `alloca` would be the
`alloca` instruction instead, and the engine would free it when `f` returns.

## 6. Floating point

```c
#include <math.h>
double hyp(double a, double b) { return sqrt(a * a + b * b); }
double i2d(int i) { return i; }
```

```
hyp:
  fmul.w64  fr0, fr0, fr0     ; 4b 20 00
  fmul.w64  fr1, fr1, fr1     ; 4b 21 11
  fadd.w64  fr0, fr0, fr1     ; 4b 00 01
  fsqrt.w64 fr0, fr0          ; 4b 70 00      unary: source in the low nibble
  fret.w64  fr0               ; 45 30

i2d:
  cvt       f64, i32, fr0, ir1   ; 39 a4 01   to = F64 (0xA), from = I32 (4)
  fret.w64  fr0                  ; 45 30
```

(`sqrt` is selected to `fsqrt` because `-fno-math-errno` is the CBC default; with
`-fmath-errno` it is a native `sqrt` call.)

## 7. Function pointers: a sort routine with a CBC comparator

`qsort` is unsupported on CBC (its comparator would be called by native code, §7.2), so
the example uses a sort routine that is part of the program:

```c
#include <stddef.h>
static int cmp_asc(int a, int b)  { return a - b; }
static int cmp_desc(int a, int b) { return b - a; }

__attribute__((noinline))
static void isort(int *v, size_t n, int (*cmp)(int, int)) {
  for (size_t i = 1; i < n; i++)
    for (size_t j = i; j > 0 && cmp(v[j - 1], v[j]) > 0; j--) {
      int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
    }
}

void sort_up(int *v, size_t n)   { isort(v, n, cmp_asc); }
void sort_down(int *v, size_t n) { isort(v, n, cmp_desc); }
```

* `isort` is called with two different comparators, so LTO cannot specialize it, and the
  call `cmp(v[j-1], v[j])` stays an indirect call.
* `cmp_asc` and `cmp_desc` are address-taken. Nothing has to be decided about them: their
  addresses stay `ptr @cmp_asc`/`ptr @cmp_desc` and are selected to `LOAD_FNPTR` (E5);
  their run-time values are their engine descriptors (`04-architecture.md` §7.1).

### 7.1 Taking the address and calling through it

```
sort_up:                            ; IR1 = v, IR2 = n
  ld.fnptr ir3, @cmp_asc            ; 44 D3 mm        RegSymGroup selector 13, method ref of cmp_asc
  call     ir1, @isort              ; 44 31 mm
  ret.w64  ir1                      ; 45 11
```

At the first execution of `sort_up`, the rewriter resolves `@cmp_asc`, creates (or reuses)
its `DynamicFunctionHandle`, gets (or creates) the function's 16-byte descriptor in the
engine's descriptor region, and stores the descriptor address as an RT literal. `ir3` then
holds a stable, 16-byte-aligned integer that identifies `cmp_asc` for the whole process:
`==`, ordering and conversion to `uintptr_t` and back work. It is not executable — passing
it to native code that calls it would fault, which is why callbacks are rejected at link
time (§7.2).

Inside `isort`, the comparator call stays a plain indirect call and is lowered to
`CALL_INDIRECT` (E6):

```
isort:                              ; IR1 = v, IR2 = n, IR3 = cmp
  mov.w64  ir8, ir3                 ; 15 83           cmp kept in callee-saved ir8 (x86-64)
  ...
.Linner:                            ;                 ir10 = &v[j] (callee-saved)
  addi.w64 ir11, ir10, -4           ;                 &v[j-1]: negative displacements are not
                                    ;                 folded (01-cbc-platform-facts.md §12 Q5)
  ld.raw.s32 ir1, [ir11 + 0]        ;                 v[j-1]
  ld.raw.s32 ir2, [ir10 + 0]        ;                 v[j]
  call.indirect ir8                 ; 45 98           RegGroup selector 9; state point
  ; result in IR1; all CBC volatile registers are clobbered (CBC call regmask);
  ; ir8, ir10, ir11 are callee-saved and survive
  bcci.w32 le, ir1, 0, .Lnext
  ...
```

`CALL_REG` reads `ir8`, sees that the value lies inside the descriptor region, loads the
descriptor's handle and calls `cmp_asc` through its `i2call` adapter exactly as `call.2i`
would (`13-engine-changes.md` §7.1): no native code runs, the call is an ordinary CBC→CBC
call. Its cost is that of a direct `call` plus the descriptor check (a range check and a
load). The same `call.indirect` instruction would perform a native call if `ir8` held a
native function's address. A null comparator raises
`NoneValueException`, which the first landing pad (or the entry) reports as
"call through a null function pointer" (`08-exceptions-and-sjlj.md` §4.3).

### 7.2 The same program with `qsort`

```c
#include <stdlib.h>
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
void sort(int *v, size_t n) { qsort(v, n, sizeof *v, cmp); }
```

`qsort` is not defined by any CBC input, so `CBCResolveSymbols` makes it a native symbol;
after LTO, `CBCNativeCallLegalizer` finds the live call and stops the link:

```
error: 'qsort' is not supported on CBC: its comparator would be called by native code
       (referenced from 'sort' at sort.c:3)
```

(rule 1 of `10-linker-and-runtime.md` §5.4; rule 6 would also reject passing `cmp`, a CBC
function, to a function-pointer parameter of a native function). With
`-D__CBC_STRICT_LIBC__` the compat `<stdlib.h>` marks `qsort` unavailable and clang reports
the same at compile time. The program links once `qsort` is replaced by a CBC routine such
as `isort` above, or once a C implementation of `qsort` is compiled into the program
(then `qsort` is an ordinary CBC function and §7.1 applies to its comparator calls).

## 8. Variadic calls

```c
#include <stdio.h>
void report(int i, double d) { printf("%d %f\n", i, d); }
```

### 8.1 A native variadic call with a `double` argument

`printf` is native (there is no CBC `printf`). No IR pass touches the call: clang's
`call i32 (ptr, ...) @printf(ptr @.fmt, i32 %i, double %d)` is lowered by `LowerCall` as a
plain native call with the host ABI (x86-64 flavour shown):

```
report:                             ; IR1 = i, FR0 = d (incoming arguments)
  mov.w64  ir2, ir1                 ; 15 21           i → second integer argument (rsi)
  lea.s    ir1, ir3, @image         ; 7c 13 ii        image base; ir3: dead base-ref
  addi.w64 ir1, ir1, <off(.fmt)>    ;                 format string address → rdi
                                    ;                 d stays in FR0 → xmm0
  call     ir1, @native(printf)     ; 44 31 mm        clobbers only IR1/FR0
  ret.w64  ir1                      ; 45 11
```

The engine's native-call adapter copies `IR1..IR6` to `rdi..r9` and `FR0..FR7` to
`xmm0..xmm7`, then — with E7 (`13-engine-changes.md` §10) — sets `al = 8` and calls
`printf`. glibc's `printf` prologue sees `al ≠ 0`, spills `xmm0..xmm7` into its register
save area, and `va_arg(ap, double)` reads `d` from there, exactly as when a native compiler
calls `printf`. Without E7, `al` would hold the low byte of the adapter's address and could
be 0, in which case `%f` would print garbage. `printf("%Lf", x)` does not compile: clang
rejects `%L` floating conversions on CBC (`09-clang.md` §2).

Only the *address* of `printf` needs special treatment: `&printf` is replaced by a CBC
wrapper that calls `vprintf` (`07-ir-passes.md` §4 step 1), because indirect variadic
calls in CBC code use the buffer convention of §8.2.

### 8.2 A CBC-defined variadic function

```c
#include <stdarg.h>
static int sum(int n, ...) {
  va_list ap; va_start(ap, n);
  int s = 0;
  while (n--) s += va_arg(ap, int);
  va_end(ap);
  return s;
}
int use(void) { return sum(3, 10, 20, 30); }
```

`ExpandVariadics` (CBC `VariadicABIInfo`, `05-llvm-core-changes.md` §7) rewrites the
definition to `sum(i32 %n, ptr %buf)` and the call site to

```llvm
%buf = alloca [3 x i64], align 16                ; untyped memory block, host stack-argument layout
store i64 10, ptr %buf
%s1 = getelementptr inbounds i8, ptr %buf, i64 8
store i64 20, ptr %s1
%s2 = getelementptr inbounds i8, ptr %buf, i64 16
store i64 30, ptr %s2
%r = call i32 @sum(i32 3, ptr %buf)
```

Inside `sum`, `va_start` initializes the host `va_list` exactly as in §8.1 (registers
exhausted, overflow area = `%buf`), and clang's host-ABI `va_arg` code reads the
arguments from the overflow area. The same `va_list` could be passed to a native `v*`
function unchanged. (In this small example LTO would normally inline and fold `sum`
entirely; the expansion is what remains when it does not.)

## 9. C++ exceptions

```cpp
int g(int);                     // may throw
int h(int x) {
  try { return g(x); }
  catch (int e) { return e; }
}
```

Clang emits:

```llvm
define i32 @_Z1hi(i32 %x) personality ptr @__gxx_personality_v0 {
entry:
  %r = invoke i32 @_Z1gi(i32 %x) to label %ok unwind label %lpad
ok:
  ret i32 %r
lpad:
  %lp = landingpad { ptr, i32 } catch ptr @_ZTIi
  %sel = extractvalue { ptr, i32 } %lp, 1
  %tid = call i32 @llvm.eh.typeid.for(ptr @_ZTIi)
  %m = icmp eq i32 %sel, %tid
  br i1 %m, label %catch, label %resume
catch:
  %exn = extractvalue { ptr, i32 } %lp, 0
  %obj = call ptr @__cxa_begin_catch(ptr %exn)
  %e = load i32, ptr %obj
  call void @__cxa_end_catch()
  ret i32 %e
resume:
  resume { ptr, i32 } %lp
}
```

After `CBCLowerEH`:

```llvm
lpad:
  %lp  = landingpad { ptr, i32 } catch ptr @_ZTIi           ; results now unused
  %exn = call ptr @__cbc_eh_landing()                       ; aborts on engine-originated exceptions
  %sel = call i32 @__cbc_eh_select(ptr %exn, ptr @_Z1hi.lpad0)   ; {count 1, flags 0, {catch, id 1, @_ZTIi}}
  %m = icmp eq i32 %sel, 1                                    ; llvm.eh.typeid.for(@_ZTIi) = 1
  br i1 %m, label %catch, label %resume
catch:
  %obj = call ptr @__cxa_begin_catch(ptr %exn)
  ...
resume:
  call void @__cbc_eh_resume(ptr %exn)
  unreachable
```

Machine code and the exception table:

```
_Z1hi:                             ; untypedSlotCount = 0, untypedMemSize = 0, usesAlloca = 0
 0: call      ir1, @_Z1gi          ; 44 31 mm     region [0, 3) → 6; liveness {3}
 3: ret.w64   ir1                  ; 45 11
.Llpad0:                           ; region target = 6
 6: call      ir1, @__cbc_eh_landing   ; no `catch`: the engine object is not needed
    ...
```

`exTable = { uleb 0, uleb 3, uleb 6 }`. If `g` throws, `__cbc_raise` executes
`nullcheck IRZ`; the engine unwinds `__cbc_raise`, `__cxa_throw`, `g`, finds the region in
`h` (the throwing call's RT position lies between the mapped bounds), puts the
`NoneValueException` object in `IR_ACC`, and continues at `.Llpad0`. Each popped frame
with `usesAlloca = 1` has its shadow cursor restored before the pop
(`04-architecture.md` §5.5). `h` itself has nothing to restore.

## 10. The entry method and image initialization

```
$cbc.ex:Entry.main:                        ; FUNCTIONAL(-> I64), typed slots: t0, t1 : TUPLE(U64, U64)
  ld.fnptr ir2, @native(abort)             ; 44 D2 mm  llvm.cbc.require.engine.ext: an engine
                                           ; without E5 fails to rewrite this method (13 §1)
  ld.fcb   ir2                             ; 45 e2     dead: an engine without E8 fails here
  call     ir1, @__cbc_check_host
  ; __cbc_image_init (inlined here for illustration):
  initstr  t0, @blob(B)                    ; 47 <uleb off(B)> 00 00
  ld.stack.rec ir2, t0                     ; 4c 20 00 00
  ld.raw.64 ir2, [ir2 + 0]                 ; StringStorage*
  addi.w64 ir2, ir2, 16                    ; blob bytes
  lea.s    ir1, ir4, @image                ; ir4: dead base-ref
  movi.w64 ir3, 11                         ; |B|
  call     ir1, @native(memcpy)
  initstr  t1, @blob(R)
  ld.stack.rec ir2, t1
  ld.raw.64 ir2, [ir2 + 0]
  addi.w64 ir2, ir2, 16
  lea.s    ir1, ir4, @image
  movi.w64 ir3, 1                          ; one IMAGE_ABS64 slot
  mov.w64  ir4, ir1                        ; base = the image itself (phase 1)
  call     ir1, @__cbc_apply_image_relocs
  ; constructors, argv, main, exit handlers ...
  call     ir1, @__cbc_main
  ...
  ret.w64  ir1
```

Each `ld.stack.rec` result is consumed by the very next instruction. A frame address may
be kept across a call (`01-cbc-platform-facts.md` §7); this sequence does not need to.

## 11. Separate compilation: data images per object (phase 2, deferred)

> **Deferred — not in the work scope.** Separate code generation is postponed
> indefinitely (`04-architecture.md` §3); in the current scope `clang -c -fno-lto` is an
> error. This example illustrates the design sketch of `10-linker-and-runtime.md` §4 only.

Two translation units compiled with `clang -c -fno-lto` (phase 2,
`10-linker-and-runtime.md` §4):

```cpp
// a.cpp
int counter = 5;                                // own image of a.o
inline int &shared() { static int s = 7; return s; }   // s: COMDAT group of shared()
int *pick();
int bump() { ++counter; return ++shared() + *pick(); }

// b.cpp
extern int counter;                              // defined in a.o
inline int &shared() { static int s = 7; return s; }
int *table[] = { &counter };                     // own image of b.o, pointer into a.o's image
int *pick() { ++shared(); return table[0]; }
```

**Placement by module-local `CBCLowerGlobals`** (`07-ir-passes.md` §3.6):

| Object | Image section | Contents |
|---|---|---|
| `a.o` | `.cbc.image.self` | `counter` at 0 (initial bytes `05 00 00 00`) |
| `a.o` | `.cbc.image.g._ZZ6sharedvE1s` | `_ZZ6sharedvE1s` (`s`) at 0, initial `07 00 00 00`; in the COMDAT group of `shared()` |
| `b.o` | `.cbc.image.self` | `table` at 0: 8 bytes `00…`, relocation `R_CBC_ABS64 counter + 0` |
| `b.o` | `.cbc.image.g._ZZ6sharedvE1s` | its own copy of `s` |

**Code of `bump` in `a.o`** (schematic):

```
  lea.s    ir2, ir3, @image(self)      ; 7c 23 ii ii ii   R_CBC_FREF_ULEB21 → a.o own image
  ld.raw.32 ir4, [ir2 + 0]             ;                  counter: offset known (0), no relocation
  addi.w32 ir4, ir4, 1
  st.raw.32 ir4, [ir2 + 0]
  call     ir1, @_Z6sharedv            ; 44 31 mm mm mm   R_CBC_MREF_ULEB21 (padded, 3 bytes)
  ...
```

**Code of `shared()` in either object** — `s` is a COMDAT member, so even the defining
object uses the `IMAGEOF` path, because the linker may keep the other object's copy:

```
  lea.s    ir2, ir3, @imageof(_ZZ6sharedvE1s)   ; R_CBC_FREF_ULEB21 (IMAGEOF) against s
  ld.raw.32 ir1, [ir2 + d]                      ; d: R_CBC_IMAGE_DISP against s, addend 0
```

**Linking** (`lld/CBC`):

1. Images in input order: `Data.0` = `a.o` own image, `Data.1` = `b.o` own image, `Data.2`
   = the group image of `s` from `a.o` (first seen). `b.o`'s copy of the group is
   discarded whole, together with `b.o`'s copy of `shared()`'s code.
2. `IMAGEOF(s)` everywhere → field reference `$cbc.<p>:Data.2.image`, displacement 0. So
   `bump`, `pick` and `shared` all address the one surviving `s`.
3. `b.o`'s `table[0]`: `counter` resolves to `Data.0` offset 0, so the image relocation is
   `IMAGE_ABS64(1, 0 → 0)`: the blob holds `0`, and start-up adds `base(Data.0)`.
4. Start-up (synthesized `__cbc_image_init`): one blob `B` = `05 00 00 00 00 00 00 00 |
   00 00 00 00 00 00 00 00 | 07 00 00 00 00 00 00 00`; copies of 8 bytes into each of
   `Data.0`, `Data.1`, `Data.2` (all three are below 64 bytes, so the linker emits
   constant stores instead of `memcpy`); then
   `__cbc_apply_image_relocs(Data.1, {0}, 1, base(Data.0))`.

Had `b.cpp` declared `extern FILE *stdout` and used it, the same `IMAGEOF` pair would
have resolved to the AOT static of the native `stdout` and displacement 0 — identical
code, different relocation results.
