#include <stdlib.h>

int main(void) {
  if (atoi(" -42") != -42)
    return 1;
  char *end = 0;
  if (strtol("100", &end, 10) != 100 || *end != 0)
    return 2;
  if (strtol("ff", &end, 16) != 255 || *end != 0)
    return 3;
  if (abs(-7) != 7 || labs(-9) != 9)
    return 4;
  int *p = calloc(4, sizeof(int));
  if (!p)
    return 5;
  int ok = p[0] == 0 && p[3] == 0;
  free(p);
  return ok ? 0 : 6;
}
