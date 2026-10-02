#include <setjmp.h>

static jmp_buf buf;

int main(void) {
  int r = setjmp(buf);
  if (r == 0) {
    longjmp(buf, 2);
    return 1;
  }
  return r == 2 ? 0 : 3;
}
