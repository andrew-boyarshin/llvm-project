# 14 — libc++ on CBC

This file is the M4 design for the C++ standard library and the C++ ABI runtime.
It supersedes the libc++/libc++abi sketch in `10-linker-and-runtime.md` §7; where
the two disagree, this file is the one to implement. Exception lowering stays in
`08-exceptions-and-sjlj.md`. Threads stay off (`04-architecture.md` §14,
`15-threads.md`).

The library that runs inside a `.cbc` is a second build, with a different triple
and a different unwinder decision from the libc++ that the failing host configure
below is trying to produce.

## 1. Decision

Compile **libc++** and **libc++abi** to CBC bitcode and link them into the guest
image. Do **not** build or link **libunwind** for any CBC triple. The Itanium
entry points libc++abi calls, and the landing-pad helpers of `08`, are functions
in **`crt-cbc.bc`**, not a fourth archive. Host libc remains the C library
(`10-linker-and-runtime.md` §5).

C++ is not an interop boundary. `operator new`, `std::exception`, vtables, RTTI
and exception objects belong to the guest. A native C++ function is not a legal
callee (`04-architecture.md` §8, §12).

Threads are off in this port. `std::mutex` is a no-op, `std::thread` is not
compiled, function-local statics use the single-threaded guard, and EH globals
live in the fiber control block. What a later `pthread_create` would still
leave unfinished is `15-threads.md`, and it is not part of M4.

### 1.1 One guest runtime, two C++ archives

Four guest pieces were easy to draw because a native Linux toolchain has four:
`crt1.o`, `libgcc` / `libgcc_eh` or libunwind, libc++abi, libc++. On CBC the
first two are the same object.

`crt-cbc.bc` is one bitcode module, linked on every program, C or C++. It
already owns the fiber-control-block accessors, emulated TLS,
`__cbc_raise`, `atexit` / `__cxa_atexit`, and the signal guards
(`10-linker-and-runtime.md` §1, §5.2). `longjmp` and the marker-exception
helpers use that same control block (`08` §1, §8). `_Unwind_RaiseException`
is `__cbc_raise` plus three stores. A separate `libcbcunwind.a` would be a
second owner of `fcb->eh`, a second build, and a link-line entry the driver
already has to pass for C (because of `longjmp`) and for C++ (because of
`__cxa_throw`). M3 ships `longjmp` before libc++abi exists
(`11-testing-and-roadmap.md` §3), so those functions have to build as C, inside
the crt. M4 adds `_Unwind_*`, the landing-pad helpers and the personality
stubs to the same module. LTO drops the ones a given program does not call;
the module being wholly linked does not keep dead `signal` or dead
`_Unwind_RaiseException` in the `.cbc`.

There is no `libcbcunwind`. Do not reintroduce it.

What stays separate, and why merging the crt into it would be worse:

| Piece | Keep separate because |
|---|---|
| **compiler-rt builtins** (`libclang_rt.builtins.a`) | Freestanding helpers (`__divti3`, `atomic.c`) that instruction selection invents. The crt calls `mmap`, `malloc`, `exit`, `sigaction`. Upstream compiler-rt is built for bare metal and GPU targets and rejects a hosted runtime in the builtins list. M3 lists the two side by side on purpose. `emutls.c` stays excluded; `__emutls_get_address` is the crt, so the builtins archive must not define it too. |
| **libc++abi** | Upstream sources and tests, pulled member by member. C programs do not link it. Folding the crt into it makes the C runtime a product of the C++ ABI cmake, which M2/M3 cannot wait for, and it pulls `private_typeinfo` into every `setjmp` program. The other direction (folding the ABI into the crt) links the demangler and `operator new` into every C link unless LTO happens to delete them, and it forks the upstream build. |
| **libc++** | Same upstream-build reason. LTO already drops unused facets. A merged archive does not get smaller; it gets harder to update. |

The one cross-edge is type matching. `__cbc_eh_select` lives in the crt,
because C `setjmp` landing pads call it. `__cbc_can_catch` lives in libc++abi,
because it calls `__shim_type_info::can_catch` (`private_typeinfo.h`). A
strong definition of `__cbc_can_catch` inside the always-linked crt would
satisfy the reference and the libc++abi member would never be extracted
(`10` §2.2: archives are lazy). A weak definition in the crt loses for the
same reason: the strong member is not pulled if the weak one already defines
the symbol. So:

* libc++abi's `cbc_eh_select.cpp` is the only strong `__cbc_can_catch` /
  `__cbc_matches_filter`.
* A C link, which does not pass `-lc++abi`, also passes `cbc_can_catch_stub.bc`
  (ten lines, both functions return "no match"). A C++ link does not pass the
  stub. Passing both is a duplicate-symbol error, which is the failure wanted.

Do not register `can_catch` from a libc++abi global constructor. A constructor
is not a reference, so the member is still extracted only when something else
pulls it, and a throw from another priority-0 constructor runs before the
registration.

Personality stubs (`__gxx_personality_v0`, `__gcc_personality_v0`) are one
strong definition in the crt. libc++abi does not define them.
`cxa_personality.cpp` is not compiled. Clang's personality operand is a use,
so the stub stays live exactly when a landing pad exists, and `cbc-ld` does
not go looking for it in `libgcc_s`.

## 2. The configure that fails, and which libc++ it is

The command in use builds **host** runtimes, not the CBC library:

```
cmake -GNinja \
  -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX=/home/user/dev/llvm-project-upstream/build-install \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES="libcxxabi;libcxx;compiler-rt" \
  -DLLVM_PARALLEL_LINK_JOBS=2 -DLLVM_USE_LINKER=lld-21 \
  -DBUILD_SHARED_LIBS=ON -DLLVM_BUILD_TOOLS=ON \
  -DLLVM_TARGETS_TO_BUILD="X86" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=CBC \
  -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
  -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
  -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
  -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BAREMETAL_BUILD=OFF \
  -DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF -DCOMPILER_RT_BUILD_ORC=OFF \
  -DCOMPILER_RT_BUILD_GWP_ASAN=OFF -DCOMPILER_RT_BUILD_COPYPROF=OFF \
  -S ../llvm
```

The nested runtimes configure (the step that fails) logs:

```
-- LLVM host triple: x86_64-unknown-linux-gnu
-- LLVM default target triple: x86_64-unknown-linux-gnu
```

and passes `-DCMAKE_C_COMPILER_TARGET=x86_64-unknown-linux-gnu` and
`-DCMAKE_CXX_COMPILER_TARGET=x86_64-unknown-linux-gnu`. `LLVM_TARGETS_TO_BUILD`
and `LLVM_EXPERIMENTAL_TARGETS_TO_BUILD` decide which code generators the clang
binary contains. They do not change the triple of `LLVM_ENABLE_RUNTIMES`. That
sub-build compiles a native libc++ for the default triple, with the just-built
clang, and installs it under the host per-target runtime directory. The CBC
toolchain never searches that directory (`09-clang.md` §4.1 looks in
`<resource-dir>/cbc/<triple>/`).

The error is the default at `libcxxabi/CMakeLists.txt:63-66`:

```cmake
option(LIBCXXABI_USE_LLVM_UNWINDER "Build and use the LLVM unwinder." ON)
if (LIBCXXABI_USE_LLVM_UNWINDER AND NOT "libunwind" IN_LIST LLVM_ENABLE_RUNTIMES)
  message(FATAL_ERROR "LIBCXXABI_USE_LLVM_UNWINDER is set to ON, but libunwind is not specified in LLVM_ENABLE_RUNTIMES.")
endif()
```

`libunwind` is absent from `LLVM_ENABLE_RUNTIMES`, and nothing in the command
overrides the option, so the default `ON` is fatal. The check exists so a
bootstrapping runtimes build does not silently fall through to a system
unwinder that has not been built yet. It is working as written.

The same nested command line contains both `-DCOMPILER_RT_BUILD_BUILTINS=Off`
(injected by the bootstrap, which cannot build builtins before the new clang
exists) and the user's `-DCOMPILER_RT_BUILD_BUILTINS=ON`. That pair is not why
configure stopped. Configure stopped on the unwinder check, before any runtime
was compiled.

### 2.1 Making the host command configure

Two host-only fixes. Either one produces a **native x86_64** libc++. Neither
produces a CBC library, and neither is linked by `cbc-ld`.

| Host choice | CMake | What the host libc++abi unwinds with |
|---|---|---|
| System unwinder. Matches a command that deliberately left `libunwind` out. | add `-DLIBCXXABI_USE_LLVM_UNWINDER=OFF` | `libgcc_s`, via `libcxxabi/src/CMakeLists.txt:87-89` (`add_library_flags_if(LIBCXXABI_HAS_GCC_S_LIB gcc_s)`). Correct for a native ELF library on Linux. |
| In-tree unwinder. Matches the default `ON`. | add `libunwind` to `LLVM_ENABLE_RUNTIMES` | the libunwind just built for `x86_64-unknown-linux-gnu` |

Use the first unless a hermetic host toolchain is a goal of this build. The
rest of this file assumes the host command, if it still builds libc++ at all,
takes `-DLIBCXXABI_USE_LLVM_UNWINDER=OFF` and keeps `libunwind` out of
`LLVM_ENABLE_RUNTIMES`.

Copying that single flag into the CBC runtimes build is not enough. With the
LLVM unwinder off, `libcxxabi/src/CMakeLists.txt:87-89` links `libgcc_s` whenever
`check_library_exists` finds it (`libcxxabi/cmake/config-ix.cmake:12`). A probe
that runs the CBC clang as if it were a native linker can find the host
`libgcc_s` and record `-lgcc_s` on the cmake target. `cbc-ld` resolving
`_Unwind_RaiseException` from that library walks the OS stack (interpreter
frames, then the host application) and never sees a guest landing pad. §13
turns both links off for a CBC triple, in cmake, so the wrong configuration
cannot be represented.

### 2.2 The CBC library is a later cmake

Build it only after `clang --target=cbc_x86_64-unknown-linux-gnu` can emit
bitcode for C++ (personality operands, `invoke`, RTTI, `operator new` calls).
That is M4 in `11-testing-and-roadmap.md`, after M3. The recipe is §13. It is a
separate cmake of `runtimes/`, with
`CMAKE_CXX_COMPILER_TARGET=cbc_x86_64-unknown-linux-gnu`. Do not fold it into
the host `LLVM_ENABLE_RUNTIMES` list above: the per-target flags (threads off,
no libgcc_s, bitcode archives, `__CBC__` sources) are not the host flags.
`LLVM_RUNTIME_TARGETS` can package both later; it is not the way to get the
first CBC archive.

## 3. Why the guest does not use a host C++ runtime

Host libc works because the plugin boundary is the C ABI: the interpreter copies
`IR1..IR6` / `FR0..FR7` into host registers, `dlsym` finds the symbol, and the
callee does not call back and does not throw into a guest frame
(`04-architecture.md` §8, `13-engine-changes.md` §8).

A host libstdc++ or host libc++ fails that test in four independent ways.

**Throws unwind the wrong stack.** Clang instantiates most of the library into
the caller. `std::vector::at` and `std::__throw_bad_alloc` are compiled as CBC
and call `__cxa_throw`. If that symbol is the host one, `_Unwind_RaiseException`
in libgcc_s or libunwind parses `.eh_frame` of the OS stack. Guest landing pads
are engine exception regions (`08`). The OS unwind describes the interpreter,
then the host application.

**Virtual calls become callbacks.** `std::streambuf::overflow`, locale facets
and `std::error_category::message` are virtual. A native standard library
calling an override in the plugin is native code calling CBC, which stays out
of scope. With both libraries in the image, those calls are `call.indirect`
inside the guest.

**There is no single host C++ ABI.** glibc systems have libstdc++ (Itanium,
80-bit `long double` on x86-64) or libc++. Android has libc++. Darwin has
Apple's libc++. Windows has the MSVC ABI and SEH. The OS half of the triple
selects libc (`04-architecture.md` §2). It must not select the C++ ABI. The
C++ ABI on every CBC triple is the one in this file.

**`long double` does not match.** CBC `long double` is IEEE double
(`04-architecture.md` §15). Host libstdc++ headers on x86-64 Linux are 80-bit.
The guest libc++ is compiled by the CBC clang, so `numeric_limits<long double>`
and the calls in `<cmath>` follow the CBC type. Those calls then hit
`CBCNativeLibc.def` (`sinl` → `sin`, `strtold` → `strtod`, …).

`operator new` stays in the guest and is implemented with host `malloc`. The
host application may already have replaced the global `operator new`; calling
that replacement throws with the host unwinder. Guest allocations and guest
deallocations pair with each other. They are still host-heap blocks, so a
`malloc` from guest C and a `free` from host C of that same block are the same
heap. A host `delete` of a guest C++ object is not.

## 4. The three libraries

| Library | Native role | CBC |
|---|---|---|
| libc++ | Containers, iostreams, locale, filesystem, chrono, the rest of the standard library. Most of it is headers instantiated into the caller. | Bitcode archive `libc++.a`. Calls host libc for `malloc`, `FILE*`, `newlocale`, `clock_gettime`, `open`, `read`. |
| libc++abi | Itanium object model: exception header, `begin_catch` / `end_catch`, `exception_ptr`, `type_info`, `__dynamic_cast`, guards, array-new cookies, `std::terminate`, `operator new`. | Bitcode archive `libc++abi.a`, with three files replaced or dropped (§6). |
| libunwind | Walk native frames via DWARF CFI, ARM EHABI or SjLj. Two-phase search. Call a personality with `_Unwind_GetIP` / `_Unwind_SetGR` / `_Unwind_SetIP`. | Not built. Not linked. The struct `_Unwind_Exception` is still required; the library is not (§7). |

libc++abi's `cxa_personality.cpp` is the only file that is a client of
libunwind. The rest is data structures and Itanium rules. That split is why
the library is ported and the unwinder is not.

libunwind applied to a guest throw is not a degraded unwinder. It is the wrong
stack. Guest frames are unwound by the engine when bytecode executes
`nullcheck IRZ` (`08` §2). A DWARF walk from inside that bytecode starts at the
interpreter's native frame.

## 5. Rejected alternatives

**Host libstdc++ or host libc++, the way libc is host libc.** §3.

**libc++ as bitcode, host libc++abi plus libgcc_s.** `__cxa_throw` still enters
the host unwinder. `__cxa_get_globals` is pthread TLS or `__thread`, which is
the OS thread; the fiber control block is the guest's thread, including across
a future fiber migration (`08` §1). Host `__cxa_atexit` registers a destructor
the dynamic linker calls, which is a callback; the crt already defines
`__cxa_atexit` for that reason (`10-linker-and-runtime.md` §5.2).

**A hand-written mini ABI.** Enough for `throw` / `catch` of one type. Not
enough for what M4 already promises: `__dynamic_cast` with virtual bases, the
`__class_type_info` vtables clang emits (`_ZTVN10__cxxabiv1*`), `exception_ptr`
refcounts, dependent exceptions. Those live in `private_typeinfo.cpp` and
`cxa_exception.cpp` and already have `libcxxabi/test`. A mini ABI becomes a
fork of those two files.

**libc++ with exceptions compiled out** (`LIBCXX_ENABLE_EXCEPTIONS=OFF`). A
legal product switch, not a different architecture. `new` failure, `vector::at`,
`stoi` and filesystem errors then abort. Guards, pure virtual and `operator new`
remain. M4's exit criterion includes the libc++abi tests
(`11-testing-and-roadmap.md` §3), so the default build keeps exceptions. The
no-exception switch is the same sources with two cmake options off; it is not
designed separately.

**Wasm-style personality.** This tree has `__gxx_wasm_personality_v0`
(`libcxxabi/src/cxa_personality.cpp`, under `__WASM_EXCEPTIONS__`): one-phase
unwind, a fake `_Unwind_Context`, and the existing LSDA parser. Using it means
emitting `.gcc_except_table`, inventing a bytecode PC the call-site table can
name, and running `DwarfEHPrepare`, which `08` §5 exists to avoid. The engine
has already transferred to the landing pad before any personality could run.
`CBCLowerEH` already has the clauses.

**SjLj (`-fsjlj-exceptions`).** Folds C++ onto `CBCLowerSjLj`. Every potentially
throwing call pays the setjmp dispatch. Engine regions are free on the
non-throwing path (`08` §7). The reuse direction in `08` is `longjmp`
piggybacking on the marker exception.

**libcxxrt or libsupc++ instead of libc++abi.** Same `_Unwind_*` coupling, a
second copy of `type_info`, and libc++ in this tree is tested against
libc++abi. libsupc++ is not an LLVM runtime.

## 6. libc++abi

Build it static, exceptions on, RTTI on, threads off, new/delete definitions
on (the default: `LIBCXXABI_ENABLE_NEW_DELETE_DEFINITIONS` defaults `ON`;
leave `LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS` at its default so the symbols are
not also in libc++). `LIBCXX_CXX_ABI=libcxxabi`.

`type_info` equality is pointer comparison. The whole program is one LTO image,
so type-info objects are unique. Do not define `_LIBCXXABI_NON_UNIQUE_TYPEINFO`
and do not turn on `LIBCXXABI_ENABLE_FORGIVING_DYNAMIC_CAST`.

### 6.1 Files compiled unchanged

None of these walk stacks. They are compiled for `__CBC__` with the same
sources as upstream.

| File | Why it is required |
|---|---|
| `cxa_exception.cpp` | `__cxa_allocate_exception`, `__cxa_free_exception`, `__cxa_throw`, `__cxa_init_primary_exception`, begin/end catch, rethrow, uncaught count, `exception_ptr`, dependent exceptions. Throw calls `_Unwind_RaiseException` (§7) and, if that returns, `failed_throw` → `std::terminate`. The CBC entry point does not return. The reference-count updates on a primary exception that is not shared across threads are plain; with threads off that matches `_LIBCXXABI_HAS_NO_THREADS`. |
| `private_typeinfo.cpp` | `__dynamic_cast` and the vtables of `__fundamental_type_info`, `__array_type_info`, `__function_type_info`, `__enum_type_info`, `__pbase_type_info`, `__pointer_type_info`, `__pointer_to_member_type_info`, `__class_type_info`, `__si_class_type_info`, `__vmi_class_type_info`. `can_catch` is a virtual on `__shim_type_info`. This is the file a mini ABI would get wrong (virtual bases, offset-to-top, ambiguous public bases). |
| `stdlib_typeinfo.cpp` | `std::type_info::operator==`, `before`, the type-info vtable. One definition in the image, so a catch of `std::exception` matches a throw from libc++ by pointer. |
| `stdlib_exception.cpp`, `stdlib_stdexcept.cpp` | Vtables for `std::exception`, `std::bad_exception`, `std::bad_alloc`, `std::bad_cast`, `std::bad_typeid`, and the stdexcept hierarchy the ABI owns when libc++ is built against libc++abi. |
| `cxa_aux_runtime.cpp` | `__cxa_bad_cast`, `__cxa_bad_typeid`, `__cxa_throw_bad_array_new_length`. |
| `cxa_virtual.cpp` | `__cxa_pure_virtual`, `__cxa_deleted_virtual`. |
| `cxa_handlers.cpp`, `cxa_default_handlers.cpp` | `std::terminate`, `std::set_terminate`, `std::unexpected`, `std::set_unexpected`. The handler is a CBC function pointer called from CBC code. |
| `cxa_vector.cpp` | `__cxa_vec_new`, `__cxa_vec_new2`, `__cxa_vec_new3`, `__cxa_vec_ctor`, `__cxa_vec_dtor`, `__cxa_vec_cleanup`, `__cxa_vec_delete`, `__cxa_vec_delete2`, `__cxa_vec_delete3`. Clang emits these for array new-expressions. Element constructors are guest descriptors, called with `call.indirect`. |
| `stdlib_new_delete.cpp` | `operator new` / `delete`, array forms, aligned forms, nothrow forms. `malloc` / `free` / `aligned_alloc` from host libc. Failure throws guest `std::bad_alloc`. |
| `fallback_malloc.cpp` | Static emergency buffer used when `malloc` fails during a throw. No OS dependency. |
| `abort_message.cpp` | `fprintf` + `abort`, both host libc. |
| `cxa_demangle.cpp` | `__cxa_demangle`. Pure C++. Large. LTO drops it when the program does not call it. Keep it so the libc++abi tests and terminate printers can use it. |
| `cxa_guard.cpp` | With `_LIBCXXABI_HAS_NO_THREADS`, `SelectedImplementation` is `NoThreadsGuard` (`cxa_guard_impl.h`, `Implementation::NoThreads`). One byte, no `pthread_once`, no futex. |

`cxa_exception.cpp` contains an ARM assembly `_Unwind_Resume` stub under
`__arm__` / EHABI. The x86-64 CBC triple defines `__x86_64__` and not
`__USING_SJLJ_EXCEPTIONS__` and not the ARM EHABI macros, so the C call to
`_Unwind_RaiseException` is the one that is compiled. The AArch64 follow-up
flavour must keep the same property: it is AAPCS, not EHABI, and it does not
define `__USING_SJLJ_EXCEPTIONS__`.

### 6.2 `cxa_personality.cpp` is not linked

The upstream file parses an LSDA through `_Unwind_GetLanguageSpecificData` and
`_Unwind_GetIP`, then writes the landing pad through `_Unwind_SetGR` and
`_Unwind_SetIP`. It also implements `__gxx_personality_v0`, the ARM personality,
the SEH personality and `__gxx_wasm_personality_v0`. None of those run.

Replace the translation unit with `libcxxabi/src/cbc_eh_select.cpp`, compiled
only for CBC. It exports:

* `__cbc_can_catch(const void *tinfo, void *exn)` — the helper `08` §4.3
  already calls. Cast `tinfo` to `const __shim_type_info *` (the header is
  `private_typeinfo.h`) and call `can_catch`. On success, write the adjusted
  pointer and the handler switch value into the `__cxa_exception` header, in
  the fields `__cxa_begin_catch` reads. `__cxa_begin_catch` returns the base
  subobject only if that store happened. `__cbc_matches_filter` is the filter
  half of the same code (`exception_spec_can_catch` in the personality today).

Personality stubs are not in this file. One definition lives in `crt-cbc.bc`
(§1.1). A second definition here is a duplicate symbol on every C++ link.

Do not compile `cxa_noexception.cpp` in the default (exceptions on) build. That
file is the upstream replacement when `LIBCXXABI_ENABLE_EXCEPTIONS` is off.

### 6.3 `cxa_exception_storage.cpp` is replaced

The threads-off upstream file is one process-global `__cxa_eh_globals`
(`libcxxabi/src/cxa_exception_storage.cpp:17-25`). The threads-on file is either
`thread_local` or a `pthread_key` whose destructor is a callback. All three are
wrong. EH globals are per fiber, in the control block `08` §1 already reserves:

```c
__cxa_eh_globals *__cxa_get_globals(void)      { return &__cbc_fcb()->cxa_globals; }
__cxa_eh_globals *__cxa_get_globals_fast(void) { return &__cbc_fcb()->cxa_globals; }
```

A zeroed FCB field is an empty caught-exception stack and a zero uncaught
count. The upstream fast path returns NULL when the key was never created;
`__cxa_current_primary_exception` and `__cxa_uncaught_exceptions` treat NULL as
"nothing". A pointer to a zeroed struct is the same answer, and it does not
allocate during unwinding. Do not use the `thread_local` implementation in that
file: constructing the EH globals through TLS re-enters `__cxa_thread_atexit`.

`__cxa_get_globals` is the symbol `10-linker-and-runtime.md` §5.3 already
points at the FCB. This file is that implementation. It lives in libc++abi
(it returns a libc++abi type) and calls `__cbc_fcb()` from `crt-cbc`.

### 6.4 `cxa_thread_atexit.cpp` is not linked

Upstream cmake already drops it when `LIBCXXABI_ENABLE_THREADS` is off
(`libcxxabi/src/CMakeLists.txt:39-44`). Leave it dropped if threads are later
turned on (`15-threads.md` §2). The file's fallback stores a destructor in a `pthread_key` and
in `__thread` (`cxa_thread_atexit.cpp`). The crt defines
`__cxa_thread_atexit` and `__cxa_thread_atexit_impl` and runs them from the
fiber's exit path (`10-linker-and-runtime.md` §5.2). `thread_local` is a
language feature (`-femulated-tls`), independent of `std::thread`.

### 6.5 Symbols the personality used to write

`__cxa_begin_catch` trusts the header fields set during phase 1 of
`__gxx_personality_v0`: the adjusted pointer and the handler switch value.
`__cbc_can_catch` is that write, performed from `__cbc_eh_select` (`08` §4.3)
instead of from a personality. No other personality side effect is required.
`catchTemp` / `handlerCount` for unexpected-handler rethrows stay as
`cxa_exception.cpp` maintains them; dynamic exception specifications are not
part of the C++ dialect clang emits for `noexcept` (those are terminate pads,
`08` §6). Keep `__cxa_call_unexpected` if the file still defines it, so a
filter landing pad produced for a legacy `throw()` specification has a
definition.

## 7. Unwind entry points, in `crt-cbc.bc`

Not an archive and not libunwind. The functions below are C sources
`llvm-link`ed into the same `crt-cbc.bc` as `atexit`
(§1.1). They do not read DWARF, do not have a context cursor, and do not call
into the engine except through `__cbc_raise`.

### 7.1 The header

Ship one `unwind.h` under `<resource-dir>/cbc/include`, before the host
include path. `libcxxabi/src/cxa_exception.h` includes `"unwind.h"` and does
not include `unwind_itanium.h`; a second header is unused. A CBC compilation
of libc++abi must see this file and not the host's, which pulls
`unwind_arm_ehabi.h` or the SEH layout depending on macros this target must
not set. Layout must match what
`CBCTargetCodeGenInfo::getSizeOfUnwindException` returns, which is 32
(`09-clang.md` §3.1), and what `cxa_exception.cpp` computes with `alignof` /
`offsetof`.

On LP64, from `libunwind/include/unwind_itanium.h:21-41`, without the SEH arm
and without the 32-bit pad:

```c
typedef uint64_t _Unwind_Exception_Class;
struct _Unwind_Exception {
  _Unwind_Exception_Class exception_class;
  void (*exception_cleanup)(_Unwind_Reason_Code, struct _Unwind_Exception *);
  uintptr_t private_1;   /* non-zero means forced unwind; CBC never sets it */
  uintptr_t private_2;   /* phase-1 SP cache; CBC never reads it */
} __attribute__((__aligned__));
```

Size 32, alignment 16 (the attribute is "maximum useful alignment", which on
this target is 16). `exception_cleanup` is a guest function
(`exception_cleanup_func` and `dependent_exception_cleanup` in
`cxa_exception.cpp`). It is stored as a descriptor (E5) and called with
`call.indirect` from `_Unwind_DeleteException`. Descriptors live in the
reserved mmap region (`13-engine-changes.md` §9), which is not a GC heap, so
the pointer in a `malloc`'d exception object is an ordinary integer.

`private_1 != 0` is the upstream signal for a forced unwind. CBC never sets
it. `_URC_FOREIGN_EXCEPTION_CAUGHT` is passed only from
`_Unwind_DeleteException` when a handler has consumed the exception; that is
the existing libc++abi cleanup contract and does not require a foreign
exception to exist.

Do not define `__USING_SJLJ_EXCEPTIONS__`, `__ARM_EABI_UNWINDER__`, or
`__SEH__` on the CBC target. Those three switch the header and
`cxa_exception.cpp` onto layouts and call names this port does not implement.

The reason-code and action enums (`_URC_NO_REASON`, `_URC_FOREIGN_EXCEPTION_CAUGHT`,
`_URC_FATAL_PHASE2_ERROR`, `_URC_CONTINUE_UNWIND`, `_URC_END_OF_STACK`,
`_UA_SEARCH_PHASE`, `_UA_CLEANUP_PHASE`, `_UA_HANDLER_FRAME`,
`_UA_FORCE_UNWIND`) stay in the header because libc++abi names them. Guest
code does not implement a search phase.

### 7.2 Functions

```c
_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *ue) {
  struct __cbc_fcb *f = __cbc_fcb();
  f->eh.kind = 1; f->eh.in_pad = 0; f->eh.exc = ue;
  __cbc_raise();                 /* nullcheck IRZ; does not return */
}
void _Unwind_Resume(struct _Unwind_Exception *ue)            { /* same */ }
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *ue) { /* same */ }
void _Unwind_DeleteException(struct _Unwind_Exception *ue) {
  if (ue->exception_cleanup)
    ue->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, ue);
}
```

`__cxa_throw` increments `uncaughtExceptions` before the raise
(`cxa_exception.cpp`). `__cxa_begin_catch` decrements it. Single-phase
unwinding (`08` §3) runs cleanups even when the exception later hits
`std::terminate`; the standard leaves unwinding-before-terminate
implementation-defined.

Also in `crt-cbc.bc`, next to the functions `10` §5.2 already puts there
(`__cbc_raise` was already on that list; this section is where it is
specified):

| Symbol | Role |
|---|---|
| `__cbc_raise` | `noinline`, `noreturn`. The only `nullcheck IRZ`. LTO must not inline it into a frame that has a region (§11). |
| `__cbc_eh_landing`, `__cbc_eh_select`, `__cbc_eh_resume`, `__cbc_continue_unwinding`, `__cbc_eh_check` | `08` §4.3–§4.4. `__cbc_eh_select` calls `__cbc_can_catch`. |
| `longjmp`, `_longjmp`, `siglongjmp`, `__longjmp_chk` | `08` §8.4. Present from M3, not added in M4. |
| `__gxx_personality_v0`, `__gcc_personality_v0` | Abort with `CBC personality invoked; CBCLowerEH did not run`. The only definition (§1.1). |

`_Unwind_Backtrace`, `_Unwind_GetIP`, `_Unwind_GetGR`, `_Unwind_SetGR`,
`_Unwind_SetIP`, `_Unwind_GetLanguageSpecificData`, `_Unwind_GetRegionStart`,
`_Unwind_FindEnclosingFunction`, `_Unwind_ForcedUnwind`, `__register_frame`
have no caller once `cxa_personality.cpp` is gone. Do not define them. A
reference is a link error, which is the diagnostic wanted: something still
thinks there is a DWARF unwinder.

Forced unwind is pthread cancellation. This port does not implement it
(`15-threads.md`).

### 7.3 What is deliberately single-phase

The engine transfers to the region's target while unwinding and restores
callee-saved registers (`01-cbc-platform-facts.md` §6). There is no phase that
searches for a handler without running cleanups. Consequences, all accepted:

* Destructors between the throw and `std::terminate` run.
* A landing pad that does not match calls `__cbc_continue_unwinding`, which
  raises a fresh marker. The engine object from the previous marker is not
  retained (`08` §2): it is a GC reference, and neither the FCB nor the shadow
  stack is a GC root.
* `__cxa_get_exception_ptr` / the adjusted pointer are written at the landing
  pad, by `__cbc_can_catch`, before `__cxa_begin_catch`. Upstream writes them
  during phase 1. The observable result is the same: `begin_catch` returns the
  base subobject.

## 8. Contract with `CBCLowerEH`

No change to the pass in `08` §4. This section is the library side of that
contract, so the two are implemented against the same names.

1. Clang emits ordinary `invoke` / `landingpad` / `resume` and personality
   `__gxx_personality_v0` (C++) or `__gcc_personality_v0` (C with
   `-fexceptions`). `-fasynchronous-unwind-tables` and `-funwind-tables` stay
   ignored (`09-clang.md` §3.2).
2. `CBCLowerEH` builds the clause table, rewrites `llvm.eh.typeid.for` to a
   constant, and inserts `__cbc_eh_landing` / `__cbc_eh_select`. `resume`
   becomes `__cbc_eh_resume`. `DwarfEHPrepare` does not insert
   `_Unwind_Resume`.
3. The landing pad calls `__cbc_can_catch` only through `__cbc_eh_select`.
   User code never calls it.
4. `__cxa_begin_catch` (unchanged libc++abi, plus the two stores in §6.3's
   sibling — the kind/in_pad clear in `08` §4.3) runs only after a positive
   selector. It pushes onto `fcb->cxa_globals.caughtExceptions`.
5. An engine-originated exception (`kind == 0` or `in_pad` already set) aborts
   in `__cbc_eh_check`. There is no foreign-exception path: a native C++ throw
   never enters the guest, and a guest throw never enters native code.

`std::current_exception`, `std::rethrow_exception`, `std::exception_ptr` and
`std::nested_exception` then work off the unchanged `cxa_exception.cpp`
machinery, because `__cxa_init_primary_exception`, the primary/dependent class
codes `"CLNGC++\0"` / `"CLNGC++\1"`, and the FCB globals are all present.
libc++'s `exception_ptr` header calls `__cxa_allocate_exception` and
`__cxa_init_primary_exception` directly (`libcxx/include/__exception/exception_ptr.h`).

## 9. libc++

### 9.1 CMake

| Option | Value | Why |
|---|---|---|
| `LIBCXX_ENABLE_SHARED` | `OFF` | One image. No DSO, no unique-typeinfo problem across DSOs. |
| `LIBCXX_ENABLE_STATIC` | `ON` | The archive `cbc-ld` pulls from. |
| `LIBCXX_CXX_ABI` | `libcxxabi` | Default off MSVC. Set it anyway. |
| `LIBCXX_ENABLE_EXCEPTIONS` | `ON` | M4. |
| `LIBCXX_ENABLE_RTTI` | `ON` | `dynamic_cast`, `exception_ptr`, type matching. |
| `LIBCXX_ENABLE_THREADS` | `OFF` | Defines `_LIBCPP_HAS_THREADS=0`. Drops `thread.cpp`, `mutex.cpp`, `future.cpp`, `atomic.cpp`, `barrier.cpp`, `condition_variable.cpp`, `shared_mutex.cpp` from `libcxx/src/CMakeLists.txt`. `std::call_once` in `call_once.cpp` becomes the single-threaded branch (a plain flag, the function called directly). |
| `LIBCXX_HAS_PTHREAD_API` | `OFF` | Required by libc++ cmake when threads are off. |
| `LIBCXX_ENABLE_MONOTONIC_CLOCK` | `ON` | Legal with threads off (`libcxx/CMakeLists.txt:130-132` allows `OFF` only as a further restriction). `clock_gettime(CLOCK_MONOTONIC)` is host libc. |
| `LIBCXX_ENABLE_FILESYSTEM` | `ON` | `open`, `stat`, `readdir`, `mkdir`, `unlink`. No `ftw`, no callback. |
| `LIBCXX_ENABLE_LOCALIZATION` | `ON` | POSIX `newlocale` + `*_l`. See §9.3. |
| `LIBCXX_ENABLE_UNICODE` | `ON` | Tables inside the library. |
| `LIBCXX_ENABLE_WIDE_CHARACTERS` | `ON` | Host `wchar_t` / `mbstate_t` from the gnu triple. |
| `LIBCXX_ENABLE_RANDOM_DEVICE` | `ON` | Host `open("/dev/urandom")`. |
| `LIBCXX_ENABLE_TIME_ZONE_DATABASE` | `ON` on Linux (the libc++ default when `CMAKE_SYSTEM_NAME` matches Linux) | Reads the host IANA database through `FILE*`. Does not embed it. |
| `LIBCXX_ENABLE_VENDOR_AVAILABILITY_ANNOTATIONS` | `OFF` | Default. One image, no vendor dylib version. |
| `LIBCXX_HARDENING_MODE` | leave the build default | Failure goes through `verbose_abort.cpp` → host `fprintf` / `abort`. |
| `LIBCXX_ENABLE_ABI_LINKER_SCRIPT` | `OFF` | `cbc-ld` is not a GNU linker. The driver lists `-lc++ -lc++abi` itself. |
| `LIBCXX_USE_COMPILER_RT` | `ON` | Skip the libgcc probe. |
| `LIBCXX_HAS_ATOMIC_LIB` | `OFF` | Pre-set. Otherwise cmake may find host `libatomic` and add `-latomic`. 16-byte atomics come from compiler-rt `atomic.c`, linked as `-lclang_rt.builtins`. |
| `LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS` | `OFF` (the default when the ABI library provides them) | One `operator new`. It lives in libc++abi §6.1. |

`__config_site` is generated from these options (`libcxx/CMakeLists.txt`
`config_define` block, around the `_LIBCPP_HAS_THREADS` line). The CBC clang
must include that generated header with the rest of
`<resource-dir>/cbc/<triple>/include/c++/v1`, ahead of any host
`/usr/include/c++`. A host libc++ header set defines host `long double`, host
threading, and occasionally inline asm; the CBC target rejects inline asm
(`09-clang.md` §3.3).

### 9.2 What "threads off" means in the headers

`_LIBCPP_HAS_THREADS` is 0.

* `std::thread`, `std::jthread`, `std::condition_variable`, `std::future`,
  `std::async` are not declared, or they fail at compile time the way upstream
  already fails when the macro is off.
* `std::mutex`, `std::recursive_mutex`, `std::timed_mutex`, `lock_guard`,
  `unique_lock` compile and their operations are empty. One fiber: a lock that
  does not block is the right implementation, and it does not call
  `pthread_mutex_lock`.
* `shared_ptr` and `exception_ptr` refcounts use the atomic builtins when the
  target provides them. CBC provides `seq_cst` atomics up to 8 bytes in the
  ISA (`04-architecture.md` §11) and a compiler-rt lock table for 16 bytes.
  Those instructions are real CPU atomics; a single-interpreter lock would not
  be enough the day a second fiber exists, and it is the wrong lowering even
  for one fiber sharing memory with the host.
* Function-local statics are `__cxa_guard_*` in the no-threads implementation
  (§6.1), not `std::call_once`.

### 9.3 Locale and the host C library

`libcxx/src/locale.cpp` constructs facets with `newlocale` and uses the `*_l`
functions (`strcoll_l`, `iswctype_l`, `strftime_l`, …). Those take an explicit
`locale_t`. They do not call `uselocale`, so they do not write the OS thread's
locale and they survive a fiber moving between OS threads. The only `uselocale`
in tree is the z/OS shim, which this triple does not compile.

`locale::global` calls `setlocale(LC_ALL, name)` when the name is not `"*"`
(`locale.cpp`, `__locale::__setlocale`). That changes the host process's C
locale. It is the C++ rule, and it is the same host side effect as guest C
calling `setlocale`. Document it. Do not intercept it.

`<filesystem>` uses `open` / `stat` / `readdir` / `rename` / `unlink` and
throws guest `filesystem_error` on failure. It does not call `ftw` or `nftw`.

`<chrono>` system_clock and the monotonic clock use `clock_gettime`.
`std::chrono` time zones open the host zoneinfo files.

`std::ios_base::sync_with_stdio` shares the host `stdin` / `stdout` `FILE*`.
Guest `printf` and guest `cout` are the same stream. That is the intended
plugin behaviour.

`std::random_device` opens `/dev/urandom`.

### 9.4 `long double` and `<cmath>`

Compiled with the CBC clang, `long double` is `double`. `<cmath>` still names
`sinl`, `strtold`, `modfl`, `frexpl`, and the rest of the `*l` set. Every one
of those that the headers actually call must already be a `CBC_LIBC_RENAME` in
`CBCNativeLibc.def` (`10-linker-and-runtime.md` §5.4), including the `_l`
locale variants (`strtold_l`). The libc++ build is the completeness test of
that table: a missing rename is a compile or link error of the library, not a
silent ABI break. `nexttoward` / `nexttowardf` stay rejected, as §5.4 already
says.

### 9.5 Instantiation, LTO, and vtables

Header-only components (`vector`, `string`'s small-buffer paths, algorithms,
`function` for many signatures) are compiled into the user's translation unit
and into libc++'s own sources. Whole-program LTO merges COMDAT vtables and
type info. `cbc-ld` already links bitcode before code generation
(`10-linker-and-runtime.md` §2). Archive members are extracted for referenced
symbols the way a static archive is; unreferenced locale facets and the
demangler drop out.

Global constructors in the pulled-in members (iostream init sentries, classic
locale) run from `llvm.global_ctors` at the guest entry (`04-architecture.md`
§13), not from a native `.init_array`. Destructors register with the crt
`__cxa_atexit`.

### 9.6 Host side effects that are accepted

| Call | Effect |
|---|---|
| `malloc` / `free` / `aligned_alloc` | Host heap. Guest `operator new` is the only C++ entry. |
| `setlocale` from `locale::global` | Host process locale. |
| `FILE*` from iostreams | Host stdin/stdout/stderr when synced. |
| `clock_gettime`, `open`, `stat` | Host kernel, via host libc. |
| `abort` / `fprintf` from verbose abort and terminate | Host. |

### 9.7 What libc++ must not call

Confirmed against `libcxx/src`: no `qsort`, `bsearch`, `ftw`, `nftw`,
`pthread_once`, `atexit`. With threads off the pthread API header is not
included. The legalizer's unsupported-symbol list (`10` §5.5) still applies
to any reference that survives, which is the backstop if a future libc++
source file grows one.

`__cxa_atexit` is the crt symbol, not the host one. The driver's link order
and the "CBC definition wins" rule (§11) are what make that true.

## 10. compiler-rt builtins

A `cbc` builtin arch whose source list is the generic C files only. No
`x86_ARCH_SOURCES`, no `x86_64_SOURCES`, no assembly. The host macros
`__x86_64__` and `__linux__` are defined (`09-clang.md` §2), so per-arch
`#ifdef`s inside generic files are live and have to be checked one file at a
time.

| File | CBC |
|---|---|
| Generic soft-float, division, complex multiply, int-to-float, the rest of `GENERIC_SOURCES` | Include. |
| `atomic.c` | Include. `COMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF` (the user's command already sets this). `COMPILER_RT_LIBATOMIC_USE_PTHREAD` stays at its default `OFF`: the lock table must be a CAS loop on the CBC atomic instructions, not `pthread_mutex`. The comment in `10` §7 ("works across fibers") stays the requirement. |
| `emutls.c` | Exclude. `__emutls_get_address` is the FCB implementation in `crt-cbc` (`10` §5.3). Two definitions are a link failure. |
| `gcc_personality_v0.c` | Exclude. Upstream adds it when `HAVE_UNWIND_H` (`compiler-rt/lib/builtins/CMakeLists.txt`). The CBC `unwind.h` of §7 would make that true and would compile a DWARF personality. `__gcc_personality_v0` is the stub in `crt-cbc.bc` only. |
| `enable_execute_stack.c` | Exclude. It `mprotect`s a page `PROT_EXEC` for nested-function trampolines. CBC has no executable guest stack and rejects that pattern. A reference to `__enable_execute_stack` is a link error. |
| `clear_cache.c` | Include. On `__x86_64__` the body is empty (coherent I-cache). No asm. |
| `eprintf.c` | Include. Host `fprintf`. |
| `cpu_model/x86.c` | Exclude. `__builtin_cpu_supports` / `__cpu_model` are not part of this target. |
| `clear_cache` asm paths, `trampoline` asm | Not on the x86-64 empty path. Do not add a source that takes the `__aarch64__` asm path until the follow-up flavour audits it; the AArch64 flavour gets its own pass over this table. |

Sanitizers, XRay, libFuzzer, memprof, profile, ctx-profile, ORC, GWP-ASan and
copyprof stay off, matching the host command. Profile (`COMPILER_RT_BUILD_PROFILE`)
is not required for M4; leave it off in the CBC runtimes cmake as well. The
earlier sketch in `10` §7 turned profile on. This file turns it off: the
profile runtime is a separate port (callbacks into the instrumentation, a
file format, a host merge tool) and it is not on the M4 exit path.

## 11. Link, LTO, and symbol ownership

`cbc-ld` link line (`09-clang.md` §4.2):

```
crt-cbc.bc  <user bitcode>
[C++: -lc++ -lc++abi | else: cbc_can_catch_stub.bc]
-lclang_rt.builtins
--native-lib=c --native-lib=m
```

There is no `-lcbcunwind`. The stub is omitted exactly when `-lc++abi` is
present (§1.1).

Resolution rule, applied to every undefined symbol after LTO internalization:

1. A definition in the guest bitcode (user, crt, the can-catch stub or
   libc++abi, libc++, builtins) wins.
2. Otherwise the symbol is native, `dlsym`'d in the host process.

This order is what keeps `_Unwind_RaiseException`, `__cxa_throw`,
`__cxa_atexit`, `operator new` and `__gxx_personality_v0` off `libgcc_s` and
off the host libc++abi, even when those names exist in the process because the
host application is a C++ program. Step 2 must not consult `libgcc_s` for a
symbol that step 1 defined. There is no third category of "weak native
fallback" for these names.

Native C++ mangled symbols (`_Z*`) that are still undefined after step 1 are a
link error (`native C++ symbol '…' is not supported on CBC`), not a `dlsym`.
The hole that remains is `dlsym` of a mangled name from guest code, which the
legalizer cannot see. Document it next to the other callback holes
(`10` §5.5, struct-borne `SIGEV_THREAD`).

LTO runs over libc++ and libc++abi on every C++ link. Constraints:

* `__cbc_raise` is `noinline` and `noreturn` in the bitcode, and the inliner
  is not allowed to drop `noinline` on it. Inlining the `nullcheck IRZ` into a
  caller that has an exception region makes that caller catch its own marker
  (`08` §2).
* The intrinsic is a state point. Nothing deletes it as dead.
* `exception_cleanup` function pointers stored into the exception header are
  CBC function-address relocations (descriptors), not raw code addresses.
* COMDAT merging is on, so there is one `std::exception` type info.

`exit` from guest code runs crt handlers (C++ static destructors included) and
then native `exit`, which runs host `atexit` handlers
(`10` §6.2). Native `exit` called from a native library skips the guest
handlers. That is the callback limitation, already accepted.

## 12. Threads

Threading is disabled. libc++ and libc++abi are built with threads off (§9.1):
`std::thread` is absent, `std::mutex` is a no-op, and `pthread_create` stays on
the unsupported-symbol list. The analysis of a later engine `pthread_create` is
`15-threads.md`. It is not part of this port.

## 13. CBC runtimes cmake

Run after the CBC clang can compile C++ to bitcode. The compiler is the
just-built clang, invoked as a cross compiler. The toolchain default `-flto`
on `-c` (`09-clang.md` §4.1) is what makes the archives bitcode; do not compile
these sources with the host `clang++-21` directly.

```
cmake -S llvm-project/runtimes -B build-cbc-x64 -G Ninja \
  -DLLVM_ENABLE_RUNTIMES="compiler-rt;libcxx;libcxxabi" \
  -DLLVM_DEFAULT_TARGET_TRIPLE=cbc_x86_64-unknown-linux-gnu \
  -DCMAKE_C_COMPILER=<just-built-clang> \
  -DCMAKE_CXX_COMPILER=<just-built-clang++> \
  -DCMAKE_C_COMPILER_TARGET=cbc_x86_64-unknown-linux-gnu \
  -DCMAKE_CXX_COMPILER_TARGET=cbc_x86_64-unknown-linux-gnu \
  -DCMAKE_AR=<llvm-ar> -DCMAKE_RANLIB=<llvm-ranlib> \
  -DCMAKE_NM=<llvm-nm> -DCMAKE_OBJCOPY=<llvm-objcopy> \
  -DCOMPILER_RT_BUILD_BUILTINS=ON \
  -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF \
  -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF \
  -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF \
  -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
  -DCOMPILER_RT_BUILD_COPYPROF=OFF \
  -DCOMPILER_RT_BAREMETAL_BUILD=OFF \
  -DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF \
  -DCOMPILER_RT_LIBATOMIC_USE_PTHREAD=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLIBCXXABI_USE_COMPILER_RT=ON \
  -DLIBCXXABI_ENABLE_THREADS=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_STATIC=ON \
  -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
  -DLIBCXXABI_ENABLE_NEW_DELETE_DEFINITIONS=ON \
  -DLIBCXX_CXX_ABI=libcxxabi \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON \
  -DLIBCXX_ENABLE_THREADS=OFF -DLIBCXX_HAS_PTHREAD_API=OFF \
  -DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON \
  -DLIBCXX_ENABLE_EXCEPTIONS=ON -DLIBCXX_ENABLE_RTTI=ON \
  -DLIBCXX_ENABLE_FILESYSTEM=ON -DLIBCXX_ENABLE_LOCALIZATION=ON \
  -DLIBCXX_ENABLE_UNICODE=ON -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON \
  -DLIBCXX_ENABLE_RANDOM_DEVICE=ON \
  -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=ON \
  -DLIBCXX_ENABLE_VENDOR_AVAILABILITY_ANNOTATIONS=OFF \
  -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF \
  -DLIBCXX_USE_COMPILER_RT=ON \
  -DLIBCXX_HAS_ATOMIC_LIB=OFF \
  -DLIBCXX_ENABLE_NEW_DELETE_DEFINITIONS=OFF
```

`libunwind` is not in the list. `LIBCXXABI_USE_LLVM_UNWINDER=OFF` is what makes
`libcxxabi/CMakeLists.txt:64` not fatal. `LIBCXXABI_USE_COMPILER_RT=ON` skips
the `gcc` / `gcc_s` probes in `config-ix.cmake:8-14`, so
`LIBCXXABI_HAS_GCC_S_LIB` stays false and `src/CMakeLists.txt:87-89` adds
nothing. Set both. One without the other either fails the check (`ON` unwinder,
no libunwind in the list) or records `-lgcc_s` (off unwinder, probe succeeds).

Do not pass `-DLIBCXXABI_USE_LLVM_UNWINDER=ON` and add a CBC libunwind. There
is nothing for that library to walk.

Install layout (`10` §8 matches this):

```
<resource-dir>/cbc/include/                         unwind.h (CBC), compat C headers
<resource-dir>/cbc/cbc_x86_64-unknown-linux-gnu/
    include/c++/v1/                                 libc++ headers + generated __config_site
    lib/crt-cbc.bc
    lib/cbc_can_catch_stub.bc
    lib/libclang_rt.builtins.a
    lib/libc++.a
    lib/libc++abi.a
```

`crt-cbc.bc` and the stub are built with the crt, not by the runtimes cmake.
They are installed beside the archives that cmake produces. The stub is the
non-C++ definition of `__cbc_can_catch` / `__cbc_matches_filter` (§1.1).

## 14. Host compiler cmake

The command in §2, plus one cache flag:

```
-DLIBCXXABI_USE_LLVM_UNWINDER=OFF
```

Leave `LLVM_ENABLE_RUNTIMES` as `libcxxabi;libcxx;compiler-rt`. Expect a native
`libc++` / `libc++abi` for `x86_64-unknown-linux-gnu`, linked to `libgcc_s`,
with threads **on** (the host default). That library is for native programs
compiled by this clang. `cbc-ld` does not take `-l` paths from
`lib/x86_64-unknown-linux-gnu/`.

If the host build does not need an in-tree libc++ at all, drop `libcxx` and
`libcxxabi` from `LLVM_ENABLE_RUNTIMES` and keep `compiler-rt`. The CBC library
does not come from that list either way. Building host compiler-rt is
independent of the CBC builtins archive, which is the §13 build.

`BUILD_SHARED_LIBS=ON` in the host command builds shared LLVM libraries. It
must not be copied into the §13 cmake. CBC runtimes are static bitcode
archives; a shared libc++.so for the CBC triple is a native DSO the engine
cannot execute as guest code.

## 15. Source changes, all under `__CBC__` or a CBC triple check

| Location | Change |
|---|---|
| `libcxxabi/CMakeLists.txt` | If the compiler target matches `^cbc`, force `LIBCXXABI_USE_LLVM_UNWINDER=OFF` and `LIBCXXABI_USE_COMPILER_RT=ON` and skip the `libunwind`-in-`LLVM_ENABLE_RUNTIMES` fatal. A CBC configure that asks for the LLVM unwinder is an error with a message that points at this file, not a probe of `libgcc_s`. |
| `libcxxabi/src/CMakeLists.txt` | For CBC: compile `cbc_eh_select.cpp` and `cbc_exception_storage.cpp` instead of `cxa_personality.cpp` and `cxa_exception_storage.cpp`. Never add `cxa_thread_atexit.cpp`. |
| `libcxxabi/src/cbc_eh_select.cpp` | New. §6.2. |
| `libcxxabi/src/cbc_exception_storage.cpp` | New. §6.3. |
| `libcxx/CMakeLists.txt` | If the target matches `^cbc`, force `LIBCXX_HAS_ATOMIC_LIB=OFF`, `LIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF`, `LIBCXX_USE_COMPILER_RT=ON`. Do not force threads off in cmake: the §13 command does that. Do refuse `LIBCXX_HAS_PTHREAD_API=ON` until the crt key wrappers in `15-threads.md` §2 exist, so the default threads-on path cannot call host `pthread_key_create`. |
| `compiler-rt/lib/builtins/CMakeLists.txt` | A `cbc` / `cbc_x86_64` arch: generic C sources, minus `emutls.c`, `gcc_personality_v0.c`, `enable_execute_stack.c`, minus every `*_ARCH_SOURCES` assembly list. §10. |
| `compiler-rt` cmake | Recognize the `cbc_x86_64-unknown-linux-gnu` triple as having no asm builtins and as not scanning the host `unwind.h` to set `HAVE_UNWIND_H`. |
| CBC `unwind.h` | New, under the compiler-rt or clang resource headers that the CBC toolchain injects. §7.1. Not a copy of the host file: the host file pulls `unwind_arm_ehabi.h` or SEH depending on macros this target must not set. |
| `crt-cbc` | FCB including `__cxa_eh_globals` (`08` §1), `__cxa_thread_atexit`, and every function in §7.2. `cbc_can_catch_stub.bc` beside it. No `libcbcunwind`. |
| Clang CBC toolchain | C++ header path is the §13 install, not the host `c++/v1`. `-stdlib=libstdc++` remains an error (`09` §4.1). Link line §11. |
| `cbc-ld` | Guest definition wins (§11). Undefined `_Z*` after that is an error. |

No libc++ source file needs a `__CBC__` edit for M4 beyond the cmake that
generates `__config_site`. If a header fails to compile because of host inline
asm selected by `__x86_64__ && !__CBC__` being false (both are defined), the
fix is a `__CBC__` guard in that header, recorded when it happens. Do not
pre-edit headers speculatively.

## 16. Testing

M4 exit criterion (`11-testing-and-roadmap.md` §3): the libc++abi test subset
and the selected libc++ tests pass on the engine, and C++ `SingleSource` passes.

libc++abi, run as CBC bitcode, required:

* throw and catch by value, by reference, by pointer, by base, with the
  adjusted pointer checked against a virtual base
* `catch (...)`, rethrow, nested try, exception during a destructor
* `std::exception_ptr`, `std::rethrow_exception`, `std::nested_exception`,
  `std::uncaught_exceptions`
* `dynamic_cast` across a virtual base, a cross-cast, and an ambiguous base
  (expect failure)
* function-local static, constructed once, constructor throws and is retried
* `operator new` failure path under a replaced new-handler that is a CBC
  function (the handler runs on the guest)
* `noexcept` violation → terminate → abort, after destructors between the
  throw and the `noexcept` boundary have run (single-phase)
* null `call.indirect` inside a try → the engine-exception diagnostic and
  abort, not a catch of `std::exception`

libc++, required beyond that:

* `vector` growth, `string`, `map`, algorithms, `function`, `optional`,
  `variant`, iostreams to a guest `stringstream` and to synced `stdout`
* `locale("C")` and a numeric facet under the C locale
* `filesystem` `temp_directory_path` / `path` decomposition, skipping tests
  that need a writable directory the harness does not provide
* `chrono` system_clock and steady_clock (monotonic is on)
* one test that `sinl` through `<cmath>` matches `sin` (the rename table)

Explicitly not required for M4, and recorded as unsupported rather than
failures: anything under `std/thread`, `std::thread` compile tests,
`pthread_*` tests, localization tests that need a non-C locale installed,
time-zone tests that need the IANA database if the runner lacks it (skip, do
not disable the library).

Negative tests, link must fail:

* `-stdlib=libstdc++`
* a translation unit that takes the address of a CBC function and passes it to
  `qsort` (already M3)
* a reference to an undefined mangled native symbol
* a program compiled against the **host** `__config_site` (threads on, or
  include path `/usr/include/c++`) is not a supported configuration; the
  driver test checks the include order

Build tests, no engine:

* the §2 host command with `-DLIBCXXABI_USE_LLVM_UNWINDER=OFF` finishes
  configure
* the §2 host command without that flag, and without `libunwind` in
  `LLVM_ENABLE_RUNTIMES`, fails at `libcxxabi/CMakeLists.txt:65` (locks the
  diagnostic so a default change is visible)
* the §13 command does not run `check_library_exists(gcc_s …)` and the
  resulting `libc++abi` cmake target has no `gcc_s` link item
* `llvm-nm` on `libc++abi.a` shows bitcode members, a defined `__cxa_throw`,
  a defined `__cxa_get_globals`, and no defined `__gxx_personality_v0` that
  references `_Unwind_GetIP` (the stub aborts; `cxa_personality.cpp` is absent)

## 17. Risks

| # | Risk | What goes wrong | Mitigation |
|---|---|---|---|
| R21 | `LIBCXXABI_USE_LLVM_UNWINDER=OFF` copied from the host recipe onto CBC without `LIBCXXABI_USE_COMPILER_RT=ON` | configure finds host `libgcc_s`, `cbc-ld` unwinds the host | §13 sets both; §15 cmake refuses the combination on a `cbc*` triple |
| R22 | Host `libc++.so` from §14 picked up by include path or by `dlsym` of `__cxa_throw` | guest throw walks OS frames | header order in the CBC toolchain; guest definition wins (§11); `_Z*` undefined is an error |
| R23 | `__cbc_raise` inlined by LTO | a frame catches its own marker and `in_pad` logic aborts or loops | `noinline` preserved; a lit test with `-O2` LTO of a throw across two TUs |
| R24 | `cxa_exception_storage.cpp` left as the upstream no-threads global | two fibers share a caught-exception stack; even one fiber is wrong once FCB teardown assumes it owns the globals | §6.3 is a separate file, not an `#ifdef` inside the global; `nm` test in §16 |
| R25 | `<cmath>` names a `*l` function missing from `CBCNativeLibc.def` | link error, or a native 80-bit call | the libc++ build is that test (§9.4) |
| R26 | `locale::global` surprises the host process | host `printf` changes locale | documented host side effect (§9.3), same class as guest `setlocale` |
| R27 | Someone turns `LIBCXX_ENABLE_THREADS=ON` before the crt keys in `15-threads.md` §2 exist | `pthread_key_create` of a CBC destructor | cmake refuses `LIBCXX_HAS_PTHREAD_API=ON` on CBC until those wrappers exist (§15) |
| R28 | `HAVE_UNWIND_H` true while building CBC builtins | `gcc_personality_v0.c` linked beside the stub | §10 excludes the file; the triple does not scan the host `unwind.h` |
| R29 | Single-phase unwind surprises a test that expects `terminate` without destructor side effects | a destructor runs, a test fails | the libc++abi terminate test asserts the destructor ran; this matches `08` §3 |
