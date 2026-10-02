# 19 — Static constructors and `__cxa_atexit`

C++ static initialization is specified and not implemented. `07-ir-passes.md` §2
shows `__cbc_entry` calling every `llvm.global_ctors` function before `__cbc_main`
and `__cbc_exit_handlers` afterwards. `10-linker-and-runtime.md` §5.2 puts
`atexit`, `__cxa_atexit`, `__cxa_finalize`, and `__dso_handle` in `crt-cbc.bc`,
because the handlers are CBC functions and must be called by CBC code.
`14-libcxx.md` §9.5 depends on both: iostream sentries and the classic locale are
dynamic initializers, and their destructors are registered with `__cxa_atexit`.

The current crt has neither the symbols nor the lists. `CBCSynthesizeEntry.cpp`
still calls `__cbc_shadow_stack_init` (the previous protocol; E8 removes it,
`04-architecture.md` §5, `07-ir-passes.md` §2), the host check, `__cbc_image_init`,
and `__cbc_main`. It never reads `llvm.global_ctors`. Clang therefore emits a call to the host
`__cxa_atexit` and passes it a CBC function pointer. `cbc-ld` rejects that, which
is the right reaction to a host callback and the wrong reaction once crt owns the
symbol.

## 1. The link error

`cbc-ld.cpp` walks every call whose callee is still a declaration. If an argument
is a defined CBC function, it prints:

```text
cbc-ld: error: passing CBC function '<destructor>' to native function
'__cxa_atexit'; native code calling CBC code is not supported
```

The destructors seen while linking iostream and filesystem tests were

* `basic_string<char>::~basic_string`
* `basic_string<wchar_t>::~basic_string`
* several `__cxx_global_array_dtor` functions

Those are the static destructors of locale and iostream globals inside the libc++
bitcode members that the user's program pulled in. A user program with its own
`static` object has the same shape. Header-only containers (`vector`, `set`,
`optional`) do not, which is why the current suite passes without this work.

The check in `cbc-ld.cpp` stays. It is what rejects `qsort`. The fix is to make
`__cxa_atexit` a defined CBC function before that walk, so the callee is not a
declaration and the argument is a CBC-to-CBC function pointer.

## 2. What clang emits

For a dynamic destructor of a global, clang emits, from the constructor function
listed in `llvm.global_ctors`:

```llvm
call i32 @__cxa_atexit(ptr @dtor, ptr @object, ptr @__dso_handle)
```

`dtor` has the Itanium signature `void (void *)`. `__dso_handle` is a global the
crt defines; guest code takes its address. There is one DSO, so every registration
uses that address. `__cxa_finalize(&__dso_handle)` runs exactly the handlers
registered with it.

Priority is the first field of each `llvm.global_ctors` element. Lower numbers run
first. The same priority keeps source order. Destructors run in reverse order of
successful registration, which is the reverse of constructor completion, not the
reverse of the ctor array. Calling `__cxa_atexit` from the constructor produces
that order. Do not also walk `llvm.global_dtors` and run those functions a second
time. Consume the array so a later pass does not try to emit it as data.

## 3. crt

Add the following to `llvm/tools/cbc-ld/crt/crt-cbc.c`. The lists live in ordinary
static storage (the data image), not on the fiber stack.

```c
struct __cbc_exit_fn {
  void (*fn)(void *);
  void *arg;
  void *dso;
};

void *__dso_handle;

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
int atexit(void (*fn)(void));
void __cxa_finalize(void *dso);
void __cbc_exit_handlers(void);
```

`__cbc_exit_handlers` already exists and only calls `fflush(0)`. Extend it; do not
add a second function.

Behaviour, matching `10` §5.2:

* Fixed table of 64 entries, then a `malloc`-grown table if a program registers
  more. 64 covers libc++ locale init. Growing is one `realloc` of the overflow
  array.
* `__cxa_atexit` appends `{fn, arg, dso}` and returns 0. It returns `-1` only if
  `malloc` fails.
* `atexit(fn)` is `__cxa_atexit((void (*)(void *))fn, 0, &__dso_handle)` with a
  small CBC trampoline that ignores the pointer and calls `fn` as `void (*)(void)`,
  or an equivalent direct entry that stores a null `arg` and a flag. The trampoline
  is CBC, so this is still not a native callback.
* `__cxa_finalize(dso)` walks the table from the last entry to the first and calls
  every handler whose `dso` matches, then clears those slots so a second finalize
  is a no-op. `__cxa_finalize(0)` runs every remaining handler.
* `__cbc_exit_handlers` calls `__cxa_finalize(&__dso_handle)` and then `fflush(0)`.

`exit` in guest code must call `__cbc_exit_handlers` before the host `exit`. Today
nothing in the crt defines `exit`, so a guest `exit` is the host function and skips
the handlers. Define `exit` in the crt: run the handlers, then call the host
through a name that is not `exit` (the host symbol has to be reached as a native
AOT call; give the import a distinct name such as `__cbc_host_exit` and make the
legalizer or a one-line asm-level alias the linker's job). The simplest match to
`10` is: crt `exit` is CBC, and the native import is recorded under a different
string so `dlsym` of `"exit"` is not required from inside the CBC function. If the
file format cannot import a native under a different name than the CBC definition,
have `__cbc_entry` call the handlers after `main` returns, and document that a
guest `exit()` in the middle of `main` does not run destructors until that path
exists. The acceptance test in §5 uses return from `main`, which the entry wrapper
can always cover.

`__cxa_thread_atexit` stays out. Threads are off (`15-threads.md`). libc++ is built
with `LIBCXXABI_ENABLE_THREADS=OFF`, and `14` already says not to compile
`cxa_thread_atexit.cpp`.

## 4. Entry

`CBCSynthesizeEntry.cpp` builds `__cbc_entry` with an `IRBuilder` and does not look
at metadata. After `__cbc_image_init` and before the call to `__cbc_main`:

1. Read `llvm.global_ctors` if present. Each element is `{i32 priority, ptr fn, ptr comdat}`.
2. Drop null functions. A null comdat slot means "always run".
3. Stable-sort by priority ascending.
4. Call each function, in that order, as a `void ()` call.
5. Erase the `llvm.global_ctors` and `llvm.global_dtors` named metadata and the
   global arrays, so `CBCLowerGlobals` does not try to place them in the image.
   `07` §3.1 says those arrays are already consumed by the time globals are laid
   out. Today `CBCSynthesizeEntry` runs after `CBCLowerGlobals`
   (`CBCTargetMachine.cpp`). Either move the ctor walk to before
   `CBCLowerGlobals`, or teach `CBCLowerGlobals` to skip global arrays whose
   section is `llvm.metadata` / whose name starts with `llvm.global_ctor`. The
   second is smaller: the arrays are metadata, not `GlobalVariable`s with
   initializers the image walker would otherwise copy, as long as
   `getNamedMetadata` is the only user. Verify that; if they are real globals,
   skip them by name in the image walk and still call them from the entry.

After `__cbc_main` returns, call `__cbc_exit_handlers` before the entry returns.
That runs the destructors registered by the constructors.

Do not run a constructor that is not in the array. `llvm.global_ctors` is the
complete list clang produced, including libc++ members that survived LTO.

## 5. The linker check

No change to the diagnostic. After crt defines `__cxa_atexit`, the call's callee
is a CBC definition, `isDeclaration()` is false, and the walk skips it. A call
that still passes a CBC function to host `qsort` keeps failing.

Confirm with `llvm-nm` on `crt-cbc.bc` that `__cxa_atexit`, `atexit`,
`__cxa_finalize`, `__dso_handle`, and `exit` are definitions, and that the user's
link does not also pull a native declaration of the same names. Guest wins
(`14` §1).

## 6. Acceptance

* A file-scope object whose constructor sets a flag and whose destructor adds one
  to a counter: after `main` returns, the process exit code is 0 only if `main`
  observed the flag already set. The destructor itself is checked by a second
  program that calls a function registered with `atexit` and stores to a global
  that `main` does not need; the observable is `main`'s return value combined with
  the handler only if the handler runs before the entry returns. Structure the
  test as: constructor sets `g = 1`, `main` returns `g == 1 ? 0 : 1`. Destructor
  coverage is a handler that writes a byte to a file opened by `main`; `main`
  registers it with `atexit` and returns 0; a host-side check is not available
  from `launcher`'s exit code. Instead, have the handler set a global and call
  a second CBC function via `atexit` that `_exit`s with that global as the status,
  if `exit` is implemented. If only the return-from-main path exists, test
  constructors in the suite and test one `atexit` handler by invoking
  `__cbc_exit_handlers` from `main` before return and reading the flag. That is
  a complete test of the list. The entry's automatic call is then the same
  function.
* `std::cout << 1` compiles and the byte written is `'1'`. This pulls the iostream
  sentry and `__cxa_atexit`. Redirect stdout with `freopen` the way
  `cbc-stdlib-tests/c/puts.c` does, from a C fragment linked in the same program,
  or use `cout.rdbuf` only after constructors have run. The compile succeeding is
  the linker half; a captured `'1'` is the constructor half.
* A program that passes a function pointer to `qsort` still gets the existing
  `cbc-ld` error.
