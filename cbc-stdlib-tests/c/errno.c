#include <errno.h>
#include <limits.h>
#include <stdlib.h>

int main(void) {
  errno = 0;
  char *end = 0;
  long v = strtol("9999999999999999999999", &end, 10);
  if (errno != ERANGE || v != LONG_MAX)
    return 1;
  errno = 0;
  v = strtol("12x", &end, 10);
  if (v != 12 || errno != 0 || *end != 'x')
    return 2;
  return 0;
}
