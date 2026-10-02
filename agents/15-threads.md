# 15 — Threads on CBC

Not in the current work scope. The M4 libc++ port builds with threads off
(`14-libcxx.md` §9, `04-architecture.md` §14): `std::thread` is not compiled,
`std::mutex` is a no-op, function-local statics use the single-threaded guard,
and the unsupported-symbol list keeps `pthread_create`, `pthread_once`,
`pthread_key_*`, `thrd_create`, `call_once`, `pthread_atfork` and the
cancellation cleanups (`10-linker-and-runtime.md` §5.5).

This file is what a later engine `pthread_create` still would not be sufficient
for. It is not a license to turn `LIBCXX_ENABLE_THREADS` on.

## 1. What an engine `pthread_create` would have to provide

A CBC function pointer is a non-executable descriptor. libpthread calling it
faults. The useful engine change is a guest thread, not a callable descriptor:

* The new OS thread is registered with the Cangjie runtime before any guest
  instruction, and unregistered after the start routine returns. Stop-the-world
  GC sees it.
* It has its own `Ectype`, its own FCB (engine-allocated, found with `ld.fcb`),
  and an empty shadow cursor. The first `alloca` on it maps the first segment
  (`04-architecture.md` §5.4). Nothing has to be stored in `IR13`.
* libpthread's start routine is an engine trampoline. The trampoline enters the
  interpreter and calls the guest function as an ordinary interpreted call.
  Descriptors stay non-executable, so `qsort` is still rejected.
* The guest thread is pinned 1:1 to that OS thread. Host `pthread_mutex`
  ownership, `errno` (`*__errno_location()`) and glibc's thread-local locale are
  OS-thread state. The "fiber may migrate" model in `08-exceptions-and-sjlj.md`
  §1 makes those wrong as soon as the thread blocks in `pthread_cond_wait`.
* A blocking native call is a safepoint. Today it is not
  (`01-cbc-platform-facts.md` §5.2): a thread in `read` or `pthread_cond_wait`
  holds the fiber, and another thread's Cangjie allocation waits forever.
  Every waiting mutex hits it. The fiber stack itself does not move
  (`01-cbc-platform-facts.md` §7).
* The trampoline runs the FCB `__cxa_thread_atexit` list when the start routine
  returns, then drops the runtime registration.
* The outermost interpreter frame has the same catch-all as `__cbc_main`. An
  exception that reaches the native boundary is rethrown into the Cangjie
  runtime (`01-cbc-platform-facts.md` §6). On a worker that must be
  `std::terminate` instead.

Implementing this on `Spawn` does not meet the bar. `Spawn` starts a fiber the
scheduler can run M:N (`04-architecture.md` §14). `pthread_join`, `pthread_self`
and mutex ownership match host libc only if the `pthread_t` is a real pthread.

## 2. What that engine change still does not cover

Once §1 exists, these are already native and have no callback:
`pthread_join`, `pthread_detach`, `pthread_self`, `pthread_equal`,
`sched_yield`, `nanosleep`, `pthread_mutex_*`, `pthread_rwlock_*`,
`pthread_cond_*`. CBC `thread_local` is already per FCB.

libc++ with `LIBCXX_ENABLE_THREADS=ON` still does not configure on top of
`pthread_create` alone. In this tree:

| libc++ need | Call | After §1 |
|---|---|---|
| `std::thread` | `pthread_create` / `join` / `detach` (`libcxx/include/__thread/support/pthread.h`) | create is the trampoline; join and detach are native |
| `this_thread::yield`, `sleep_for` | `sched_yield`, `nanosleep` | native |
| `mutex`, `condition_variable` | `pthread_mutex_*`, `pthread_cond_*` | native, if the thread is pinned 1:1 |
| `std::call_once` | `libcxx/src/call_once.cpp` uses one mutex and one condvar and calls the function itself. It does not call `pthread_once`. | works, CBC calling CBC |
| function-local statics | `__cxa_guard_*`. `NoThreadsGuard` is a data race. The threaded default is `GlobalMutexGuard` (pthread mutex + condvar). `_LIBCXXABI_USE_FUTEX` is atomics plus `syscall(SYS_futex)`. | either works; futex has no callback |
| EH globals | must stay `&fcb->cxa_globals` | do not compile the threads-on `cxa_exception_storage.cpp` (`14-libcxx.md` §6.3) |
| `thread_local` destructors | crt `__cxa_thread_atexit` | keep it. Do not compile `cxa_thread_atexit.cpp` (`14-libcxx.md` §6.4) |
| `std::thread` per-thread state | `__thread_specific_ptr` calls `pthread_key_create` with `__at_thread_exit` (`libcxx/include/__thread/thread.h`) | still a callback. libpthread runs it after the start routine returns |

`__libcpp_execute_once` → `pthread_once` is not on the `std::call_once` path.
The only caller in this tree is the EH-storage fallback `14-libcxx.md` §6.3
replaces.

The missing library piece is the key API. Implement `pthread_key_create`,
`pthread_key_delete`, `pthread_getspecific` and `pthread_setspecific` in the
crt, storage on the FCB, destructors run by the trampoline before it returns
to libpthread (iterate up to `PTHREAD_DESTRUCTOR_ITERATIONS` if a destructor
stores a new value). Same pattern as `atexit`: the caller is CBC code. Host
libpthread keeps its own keys for native libraries. The two key spaces are
distinct, which is what "C++ does not cross the boundary" already requires.

C11 `thrd_create` is a different glibc function. It calls the user start
routine itself, so an engine hook on `pthread_create` does not see it. libc++
on Linux uses pthread, not C11 threads. Wrap `thrd_create` in the crt or leave
`<threads.h>` unavailable.

With the crt keys in place, `LIBCXX_ENABLE_THREADS=ON` covers `std::thread`,
mutexes, condition variables, `call_once`, `future` / `promise` / `async`,
`latch`, `barrier`, `semaphore`, `stop_token`. `std::barrier`'s completion
function runs on the guest thread that arrives.

Still absent after that, and out of any libc++ cmake switch:

* **crt `atexit` lists.** A fixed table grown with `malloc`
  (`10-linker-and-runtime.md` §5.2). Two threads registering handlers need a
  lock. Emulated-TLS index allocation is already atomic; the block pointer is
  per FCB.
* **`pthread_exit`.** Native `pthread_exit` from the start routine unwinds the
  OS stack out of the interpreter and never returns to the trampoline, so FCB
  destructors do not run. `std::thread` returns from the function and does not
  hit this. A crt wrapper runs the FCB lists, then calls native `pthread_exit`.
* **`exit`.** Process-wide. Other guest threads die inside whatever native call
  they are in. The Cangjie runtime has to accept that. Their `thread_local`
  destructors do not run. That matches native C++.
* **`fork`.** The other threads vanish holding mutexes, their shadow segments
  unmapped with the dying address space. `pthread_atfork` handlers are callbacks. Unsupported once a guest
  thread exists. glibc's own malloc atfork handlers are native and stay native.
* **Signals.** Unchanged. `pthread_kill` delivers onto a thread inside the
  interpreter. A CBC handler is still a callback.
* **`pthread_cancel` and `_pthread_cleanup_push`.** Native unwind through guest
  frames. libc++ does not need them. Forced unwind (`_Unwind_ForcedUnwind`)
  stays undefined (`14-libcxx.md` §7).
* **Stack size.** `pthread_attr_setstacksize` sizes the OS stack the
  interpreter runs on. Static locals consume that fiber stack. Guest `alloca`s
  live on the per-fiber shadow stack, which starts empty and grows by segments
  (`04-architecture.md` §5.4). Different fibers, different cursors.
* **Threads the host creates.** The hook is on the `pthread_create` the guest
  calls. A host thread pool that enters the plugin has no `Ectype` and no FCB
  unless every native entry performs the same registration. That is the general
  callback problem (`13-engine-changes.md` §8), and a guest `pthread_create`
  does not solve it.
* **OpenMP, GCD.** Outlined functions called from a native runtime on threads
  that runtime created.
