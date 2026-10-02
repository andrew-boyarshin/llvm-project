#include "unwind.h"

typedef long i64;
typedef int i32;
typedef unsigned long u64;

void *mmap(void *addr, u64 length, i32 prot, i32 flags, i32 fd, i64 offset);
i32 munmap(void *addr, u64 length);
i32 mprotect(void *addr, u64 length, i32 prot);
i32 uname(void *buf);
i32 open(const char *path, i32 flags);
i64 read(i32 fd, void *buf, u64 count);
i32 close(i32 fd);
void *malloc(u64 size);
void *realloc(void *ptr, u64 size);
i32 fflush(void *stream);
void _exit(i32 status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
i64 write(i32 fd, const void *buf, u64 count);
int puts(const char *s);

struct __cbc_eh {
  int kind;
  int in_pad;
  void *exc;
  u64 jmp_token;
  unsigned jmp_label;
  int jmp_value;
};

struct __cbc_fcb {
  u64 magic;
  struct __cbc_eh eh;
  void **tls_blocks;
  unsigned char cxa_globals[16];
};

struct __cbc_clause {
  i32 kind;
  i32 id;
  const void *tinfo;
};

struct __cbc_lpad_clauses {
  unsigned count;
  unsigned flags;
  struct __cbc_clause c[];
};

char __cbc_sjlj_marker;

static void cbc_msg(const char *s) {
  const char *p = s;
  u64 n = 0;
  while (p[n])
    n = n + 1;
  write(2, s, n);
}

__attribute__((noreturn)) static void __cbc_engine_exception(void) {
  cbc_msg("CBC engine exception (call through a null function pointer, or "
          "division by zero under -fsanitize=integer-divide-by-zero)\n");
  abort();
}

__attribute__((noreturn)) static void __cbc_personality_abort(void) {
  cbc_msg("CBC personality invoked; CBCLowerEH did not run\n");
  abort();
}

void __cbc_nullcheck(void);

struct __cbc_fcb *__cbc_fcb(void);

void __cbc_check_host(void) {
  char u[390];
  u64 i = 0;
  char *machine;
  while (i < 390) {
    u[i] = 0;
    i = i + 1;
  }
  if (uname(u) != 0)
    _exit(127);
  machine = u + 65 * 4;
  if (machine[0] != 'x' || machine[1] != '8' || machine[2] != '6' ||
      machine[3] != '_' || machine[4] != '6' || machine[5] != '4' ||
      machine[6] != 0)
    _exit(127);
}

void __cbc_apply_image_relocs(char *img, unsigned *offs, i64 n, i64 base) {
  i64 i = 0;
  while (i < n) {
    u64 *slot = (u64 *)(img + offs[i]);
    *slot = *slot + (u64)base;
    i = i + 1;
  }
}

i32 __cbc_args(char ***argv_out) {
  char buf[4096];
  i32 fd = open("/proc/self/cmdline", 0);
  i64 n;
  i64 parts;
  i64 i;
  u64 bytes;
  i64 k;
  i64 need;
  char **v;
  i64 argc;
  i64 start;
  i32 seen_cbc;
  if (fd < 0) {
    char **empty = (char **)malloc(8);
    empty[0] = 0;
    *argv_out = empty;
    return 0;
  }
  n = read(fd, buf, 4095);
  close(fd);
  if (n < 0)
    n = 0;
  buf[n] = 0;
  parts = 1;
  i = 0;
  while (i < n) {
    if (buf[i] == 0)
      parts = parts + 1;
    i = i + 1;
  }
  bytes = 0;
  k = 0;
  need = parts + 1;
  while (k < need) {
    bytes = bytes + 8;
    k = k + 1;
  }
  v = (char **)malloc(bytes);
  argc = 0;
  start = 0;
  seen_cbc = 0;
  i = 0;
  while (i <= n) {
    if (i == n || buf[i] == 0) {
      char *s = buf + start;
      i64 len = i - start;
      i32 ends = 0;
      if (len >= 4 && s[len - 4] == '.' && s[len - 3] == 'c' &&
          s[len - 2] == 'b' && s[len - 1] == 'c')
        ends = 1;
      if (seen_cbc) {
        v[argc] = s;
        argc = argc + 1;
      }
      if (ends)
        seen_cbc = 1;
      start = i + 1;
    }
    i = i + 1;
  }
  v[argc] = 0;
  *argv_out = v;
  return (i32)argc;
}

struct __cbc_exit_handler {
  void (*fn)(void *);
  void *arg;
  void *dso;
  i32 live;
};

void *__dso_handle;

static struct __cbc_exit_handler cbc_exit_fixed[64];
static i32 cbc_exit_nfixed;
static struct __cbc_exit_handler *cbc_exit_extra;
static i32 cbc_exit_nextra;
static i32 cbc_exit_extra_cap;

static void cbc_exit_call(struct __cbc_exit_handler *h) {
  void (*fn)(void *);
  void *arg;
  if (!h->live)
    return;
  fn = h->fn;
  arg = h->arg;
  h->live = 0;
  fn(arg);
}

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
  struct __cbc_exit_handler h;
  h.fn = fn;
  h.arg = arg;
  h.dso = dso;
  h.live = 1;
  if (cbc_exit_nfixed < 64) {
    cbc_exit_fixed[cbc_exit_nfixed] = h;
    cbc_exit_nfixed = cbc_exit_nfixed + 1;
    return 0;
  }
  if (cbc_exit_nextra == cbc_exit_extra_cap) {
    i32 cap = cbc_exit_extra_cap ? cbc_exit_extra_cap * 2 : 16;
    struct __cbc_exit_handler *grown =
        realloc(cbc_exit_extra, (u64)cap * sizeof(struct __cbc_exit_handler));
    if (!grown)
      return -1;
    cbc_exit_extra = grown;
    cbc_exit_extra_cap = cap;
  }
  cbc_exit_extra[cbc_exit_nextra] = h;
  cbc_exit_nextra = cbc_exit_nextra + 1;
  return 0;
}

int atexit(void (*fn)(void)) {
  return __cxa_atexit((void (*)(void *))fn, 0, &__dso_handle);
}

static void cbc_exit_run(struct __cbc_exit_handler *h, i32 n, void *dso) {
  i32 i = n;
  while (i > 0) {
    i = i - 1;
    if (dso && h[i].dso != dso)
      continue;
    cbc_exit_call(&h[i]);
  }
}

void __cxa_finalize(void *dso) {
  cbc_exit_run(cbc_exit_extra, cbc_exit_nextra, dso);
  cbc_exit_run(cbc_exit_fixed, cbc_exit_nfixed, dso);
}

void __cbc_exit_handlers(void) {
  __cxa_finalize(&__dso_handle);
  fflush(0);
}

void exit(i32 status) {
  __cbc_exit_handlers();
  _exit(status);
}

/* GCC torture uses __builtin_exit; Clang does not lower that spelling, so
   with -Wno-implicit-function-declaration it becomes an external call. */
__attribute__((noreturn)) void __builtin_exit(i32 status) { exit(status); }

/* GCC __builtin_puts is not a Clang builtin; -Wno-implicit-function-declaration
   turns it into an external call. */
int __builtin_puts(const char *s) { return puts(s); }

__attribute__((noinline, noreturn)) void __cbc_raise(void) {
  __cbc_nullcheck();
  for (;;) {
  }
}

__attribute__((noreturn)) static void raise_unwind(struct _Unwind_Exception *ue) {
  struct __cbc_fcb *f = __cbc_fcb();
  f->eh.kind = 1;
  f->eh.in_pad = 0;
  f->eh.exc = ue;
  __cbc_raise();
}

_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *ue) {
  raise_unwind(ue);
}

void _Unwind_Resume(struct _Unwind_Exception *ue) { raise_unwind(ue); }

_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *ue) {
  raise_unwind(ue);
}

void _Unwind_DeleteException(struct _Unwind_Exception *ue) {
  if (ue->exception_cleanup)
    ue->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, ue);
}

void __gxx_personality_v0(void) { __cbc_personality_abort(); }
void __gcc_personality_v0(void) { __cbc_personality_abort(); }

void __cbc_eh_caught(void) {
  struct __cbc_fcb *f = __cbc_fcb();
  f->eh.kind = 0;
  f->eh.in_pad = 0;
}

static void __cbc_eh_check(void) {
  struct __cbc_fcb *f = __cbc_fcb();
  if (f->eh.kind == 0 || f->eh.in_pad)
    __cbc_engine_exception();
}

void *__cbc_eh_landing(void) {
  struct __cbc_fcb *f = __cbc_fcb();
  __cbc_eh_check();
  f->eh.in_pad = 1;
  if (f->eh.kind == 1)
    return f->eh.exc;
  return &__cbc_sjlj_marker;
}

__attribute__((noreturn)) void __cbc_continue_unwinding(void) {
  __cbc_fcb()->eh.in_pad = 0;
  __cbc_raise();
}

int __cbc_can_catch(const void *tinfo, void *exn, int sel);
int __cbc_matches_filter(const void *tinfo, void *exn);

i32 __cbc_eh_select(void *exn, const struct __cbc_lpad_clauses *t) {
  unsigned i;
  if (exn == &__cbc_sjlj_marker) {
    if (t->flags & 1)
      return 0;
    __cbc_continue_unwinding();
  }
  i = 0;
  while (i < t->count) {
    struct __cbc_clause c = t->c[i];
    if (c.kind == 2)
      return c.id;
    if (c.kind == 0 && __cbc_can_catch(c.tinfo, exn, c.id))
      return c.id;
    if (c.kind == 1 && !__cbc_matches_filter(c.tinfo, exn))
      return c.id;
    i = i + 1;
  }
  if (t->flags & 1)
    return 0;
  __cbc_continue_unwinding();
}

__attribute__((noreturn)) void __cbc_eh_resume(void *exn) {
  struct __cbc_fcb *f = __cbc_fcb();
  if (exn == &__cbc_sjlj_marker) {
    f->eh.kind = 2;
  } else {
    f->eh.exc = exn;
    f->eh.kind = 1;
  }
  __cbc_continue_unwinding();
}

void __cbc_setjmp(void *buf, unsigned long tok, int site, unsigned long shadow) {
  unsigned long *p = (unsigned long *)buf;
  unsigned *u = (unsigned *)buf;
  p[0] = 1;
  p[1] = tok;
  u[4] = (unsigned)site;
  p[3] = shadow;
}

int __cbc_sjlj_test(unsigned long tok) {
  struct __cbc_fcb *f = __cbc_fcb();
  __cbc_eh_check();
  if (f->eh.kind != 2 || f->eh.jmp_token != tok)
    return 0;
  f->eh.kind = 0;
  f->eh.in_pad = 0;
  return (int)f->eh.jmp_label + 1;
}

int __cbc_sjlj_value(void) {
  int v = __cbc_fcb()->eh.jmp_value;
  return v ? v : 1;
}

__attribute__((noreturn)) void longjmp(void *buf, int v) {
  unsigned long *p = (unsigned long *)buf;
  unsigned *u = (unsigned *)buf;
  struct __cbc_fcb *f = __cbc_fcb();
  f->eh.kind = 2;
  f->eh.in_pad = 0;
  f->eh.jmp_token = p[1];
  f->eh.jmp_label = u[4];
  f->eh.jmp_value = v ? v : 1;
  __cbc_raise();
}
