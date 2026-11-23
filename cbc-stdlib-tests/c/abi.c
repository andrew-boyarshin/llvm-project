/* ABI parity for engine-staged calls (MethodAbiKind::C). Exercises every path
 * a CBC call can take: direct call (stack arguments, 10 doubles, mixed int /
 * float), indirect call that hits the interpreter fast path (CBC function
 * pointer, I2I), indirect call that misses it (real C function, I2N), native ->
 * CBC callback through an N2C stub (qsort), two-register and sret returns, and
 * variadic native calls with stack arguments. Returns 0 on success.
 *
 * Not covered here, by design: indirect calls of CBC functions that take stack
 * arguments (an address-taken function must fit the host register file), and
 * `byval` / `__int128` parameters, which the backend rejects. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>
typedef struct { long a, b; } Pair;           /* returned in rax:rdx */
typedef struct { double x, y; } DPair;        /* returned in xmm0:xmm1 */
typedef struct { long a, b, c; } Big;         /* returned via sret */
__attribute__((noinline)) Pair mkpair(long a, long b) { Pair p = {a, b}; return p; }
__attribute__((noinline)) DPair mkd(double x, double y) { DPair p = {x, y}; return p; }
__attribute__((noinline)) Big mkbig(long a, long b, long c) { Big p = {a, b, c}; return p; }
__attribute__((noinline)) long many(long a, long b, long c, long d, long e, long f, long g, long h, long i) {
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i;   /* stack args */
}
__attribute__((noinline)) double manyd(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j) {
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j;
}
__attribute__((noinline)) double mixed(int a, double b, long c, float d, char e, double f) { return a + b + c + d + e + f; }
typedef long (*manyfn)(long,long,long,long,long,long,long,long,long);
typedef Pair (*pairfn)(long,long);
typedef DPair (*dpairfn)(double,double);
typedef Big (*bigfn)(long,long,long);
typedef double (*mixfn)(int,double,long,float,char,double);
static int cmp(const void *a, const void *b) { return *(const int*)a - *(const int*)b; }  /* called from native qsort via N2C */
static long vsum(int n, ...) { va_list ap; va_start(ap, n); long s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, long); va_end(ap); return s; }
int main(void) {
  int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
  volatile pairfn pf = mkpair; volatile dpairfn df = mkd; volatile bigfn bf = mkbig; volatile mixfn xf = mixed;
  CHECK(many(1,2,3,4,5,6,7,8,9) == 285, "direct stack args");
  CHECK(manyd(1,2,3,4,5,6,7,8,9,10) == 385.0, "direct 10 doubles");
  size_t (*volatile sl)(const char*) = strlen; CHECK(sl("abcd") == 4, "indirect miss -> I2N strlen");
  double (*volatile ato)(const char*) = atof; CHECK(ato("2.5") == 2.5, "indirect miss double ret");
  void *(*volatile mc)(void*, const void*, size_t) = memcpy; char t1[8]; mc(t1, "xyz", 4); CHECK(strcmp(t1, "xyz") == 0, "indirect memcpy");
  CHECK(mixed(1, 2.5, 3, 4.5f, 5, 6.0) == 22.0, "direct mixed");
  CHECK(xf(1, 2.5, 3, 4.5f, 5, 6.0) == 22.0, "indirect mixed");
  Pair p = mkpair(7, 9); CHECK(p.a == 7 && p.b == 9, "pair %ld %ld", p.a, p.b);
  Pair q = pf(11, 13);   CHECK(q.a == 11 && q.b == 13, "indirect pair %ld %ld", q.a, q.b);
  DPair d = mkd(1.5, 2.5); CHECK(d.x == 1.5 && d.y == 2.5, "dpair");
  DPair e = df(3.5, 4.5);  CHECK(e.x == 3.5 && e.y == 4.5, "indirect dpair");
  Big b = mkbig(1, 2, 3);  CHECK(b.a == 1 && b.b == 2 && b.c == 3, "big");
  Big c = bf(4, 5, 6);     CHECK(c.a == 4 && c.b == 5 && c.c == 6, "indirect big");
  CHECK(vsum(4, 1L, 2L, 3L, 4L) == 10, "variadic cbc");
  int arr[] = {5, 3, 9, 1, 7}; qsort(arr, 5, sizeof(int), cmp);   /* native -> N2C stub -> C2I */
  CHECK(arr[0] == 1 && arr[1] == 3 && arr[2] == 5 && arr[3] == 7 && arr[4] == 9, "qsort callback %d %d %d", arr[0], arr[1], arr[2]);
  char buf[64]; snprintf(buf, sizeof buf, "%d %s %.2f %ld %ld %ld %ld", 42, "str", 3.14159, 1L, 2L, 3L, 4L);  /* varargs, stack */
  CHECK(strcmp(buf, "42 str 3.14 1 2 3 4") == 0, "snprintf '%s'", buf);
  char *m = alloca(100); strcpy(m, "alloca"); CHECK(strlen(m) == 6, "alloca");
  printf(fails ? "ABI FAILED (%d)\n" : "ABI OK\n", fails);
  return fails;
}
