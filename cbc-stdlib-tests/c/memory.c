#include <stdlib.h>
#include <string.h>

int main(void) {
  unsigned char *p = malloc(16);
  if (!p)
    return 1;
  memset(p, 0xab, 16);
  unsigned char q[16];
  memcpy(q, p, 16);
  if (memcmp(p, q, 16) != 0)
    return 2;
  memmove(p + 1, p, 8);
  if (p[0] != 0xab || p[1] != 0xab || p[8] != 0xab)
    return 3;
  if (memchr(q, 0xab, 16) != q)
    return 4;
  free(p);
  return 0;
}
