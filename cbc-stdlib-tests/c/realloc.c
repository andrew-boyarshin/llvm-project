#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(4);
  if (!p)
    return 1;
  memcpy(p, "abcd", 4);
  char *grown = realloc(p, 32);
  if (!grown || memcmp(grown, "abcd", 4) != 0)
    return 2;
  p = grown;
  char *shrunk = realloc(p, 2);
  if (!shrunk || memcmp(shrunk, "ab", 2) != 0)
    return 3;
  free(shrunk);
  p = realloc(0, 8);
  if (!p)
    return 4;
  free(p);
  return 0;
}
