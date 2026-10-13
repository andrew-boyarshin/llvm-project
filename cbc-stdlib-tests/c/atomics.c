#include <stdatomic.h>
#include <stdint.h>

/* Distinct exit codes per failing check. Sequential execute coverage for CBC
 * atomics (opcodes 0x65–0x6D via $cbc:A*.v). Does not prove concurrency. */

static int check_i8(void) {
  _Atomic char a = 0;
  atomic_store(&a, 42);
  if (atomic_load(&a) != 42)
    return 10;
  if (atomic_fetch_add(&a, 3) != 42)
    return 11;
  if (atomic_load(&a) != 45)
    return 12;
  if (atomic_fetch_sub(&a, 5) != 45)
    return 13;
  if (atomic_fetch_and(&a, (char)0xf0) != 40)
    return 14;
  if (atomic_fetch_or(&a, (char)0x0f) != 32)
    return 15;
  if (atomic_fetch_xor(&a, (char)0xff) != 47)
    return 16;
  if (atomic_exchange(&a, 7) != (char)(47 ^ 0xff))
    return 17;
  if (atomic_load(&a) != 7)
    return 18;
  return 0;
}

static int check_i16(void) {
  /* Naturally aligned neighbors: store to s must not clobber t (regression
   * for the old ATOMIC_STORE→STRAW64 i16 fallthrough). */
  struct {
    _Atomic short s;
    short t;
  } n = {0, 0x1234};
  atomic_store(&n.s, (short)0xabcd);
  if (n.t != 0x1234)
    return 20;
  if (atomic_load(&n.s) != (short)0xabcd)
    return 21;
  if (atomic_fetch_add(&n.s, 1) != (short)0xabcd)
    return 22;
  return 0;
}

static int check_i32(void) {
  _Atomic int a = 0;
  atomic_store(&a, 0x12345678);
  if (atomic_load(&a) != 0x12345678)
    return 30;
  if (atomic_fetch_add(&a, 1) != 0x12345678)
    return 31;
  if (atomic_load(&a) != 0x12345679)
    return 32;
  return 0;
}

static int check_i64(void) {
  /* Value that does not fit in a narrower width — wrong $cbc:A* FRef fails. */
  _Atomic unsigned long long a = 0;
  const unsigned long long V = 0x123456789abcdefULL;
  atomic_store(&a, V);
  if (atomic_load(&a) != V)
    return 40;
  if (atomic_fetch_add(&a, 1) != V)
    return 41;
  if (atomic_load(&a) != V + 1)
    return 42;
  if (atomic_fetch_xor(&a, 0xffffffffffffffffULL) != V + 1)
    return 43;
  return 0;
}

static int check_cas(void) {
  _Atomic int a = 100;
  int expected = 100;
  if (!atomic_compare_exchange_strong(&a, &expected, 200))
    return 50;
  if (atomic_load(&a) != 200)
    return 51;
  expected = 100; /* stale */
  if (atomic_compare_exchange_strong(&a, &expected, 300))
    return 52;
  if (expected != 200) /* must return live old value */
    return 53;
  if (atomic_load(&a) != 200)
    return 54;

  _Atomic short s = 7;
  short se = 7;
  if (!atomic_compare_exchange_strong(&s, &se, 9))
    return 55;
  se = 0;
  if (atomic_compare_exchange_strong(&s, &se, 1))
    return 56;
  if (se != 9)
    return 57;
  return 0;
}

static int check_nand(void) {
  /* Not a native CBC opcode — AtomicExpand → cmpxchg → custom inserter. */
  unsigned x = 0xf0f0f0f0u;
  unsigned prev = __atomic_fetch_nand(&x, 0x0f0f0f0fu, __ATOMIC_SEQ_CST);
  if (prev != 0xf0f0f0f0u)
    return 60;
  if (x != ~(0xf0f0f0f0u & 0x0f0f0f0fu))
    return 61;
  return 0;
}

static int check_fence(void) {
  __sync_synchronize();
  _Atomic int a = 1;
  atomic_fetch_add(&a, 0);
  if (atomic_load(&a) != 1)
    return 70;
  return 0;
}

int main(void) {
  int r;
  if ((r = check_i8()) != 0)
    return r;
  if ((r = check_i16()) != 0)
    return r;
  if ((r = check_i32()) != 0)
    return r;
  if ((r = check_i64()) != 0)
    return r;
  if ((r = check_cas()) != 0)
    return r;
  if ((r = check_nand()) != 0)
    return r;
  if ((r = check_fence()) != 0)
    return r;
  return 0;
}
