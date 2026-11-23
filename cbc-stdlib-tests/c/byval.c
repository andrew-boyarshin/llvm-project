/* `byval` parameters of functions that only CBC code calls. CBCLowerByVal gives
 * them an internal convention (pointer to a caller-made copy), because the
 * engine cannot place a `byval` aggregate by the host C rules. Covers a copy
 * the callee mutates, an SSE struct, mixed register/stack arguments, recursion
 * and a call inside a loop. Functions whose address is taken, and native
 * callees, stay rejected by the backend. Returns 0 on success. */
#include <stdio.h>
typedef struct { long a, b, c, d; } Big;            /* 32 bytes: MEMORY */
typedef struct { double x, y; } DD;                  /* 16 bytes, SSE */
typedef struct { char tag; long v[5]; } Odd;         /* 48 bytes, padded */
static __attribute__((noinline)) long sum(Big b) { return b.a + b.b + b.c + b.d; }
static __attribute__((noinline)) long mut(Big b) { b.a = 100; return b.a + b.d; }   /* private copy */
static __attribute__((noinline)) long many(long a, long b, long c, long d, long e, long f, Big g, long h, Odd o) {
  return a + b + c + d + e + f + g.a + g.d + h + o.tag + o.v[4];
}
static __attribute__((noinline)) double dd(double a, double b, double c, double d, double e, double f, double g, double h, DD p) { return a+b+c+d+e+f+g+h + p.x + p.y; }
static __attribute__((noinline)) long rec(Big b, int n) { if (n == 0) return b.a; b.a += 1; return rec(b, n - 1); }
int main(void) {
  Big b = {1, 2, 3, 4}; Odd o = {7, {1,2,3,4,5}}; DD p = {0.5, 0.25};
  int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d\n", __LINE__); fails++; } } while (0)
  CHECK(sum(b) == 10);
  CHECK(mut(b) == 104); CHECK(b.a == 1);                       /* caller's object untouched */
  CHECK(many(1,2,3,4,5,6, b, 8, o) == 1+2+3+4+5+6+1+4+8+7+5);
  CHECK(dd(1,2,3,4,5,6,7,8, p) == 36.75);
  CHECK(rec(b, 5) == 6);
  for (int i = 0; i < 3; i++) { Big t = {i, i, i, i}; CHECK(sum(t) == 4 * i); }   /* call in a loop */
  printf(fails ? "BYVAL FAILED\n" : "BYVAL OK\n");
  return fails;
}
