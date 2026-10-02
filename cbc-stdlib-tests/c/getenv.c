#include <stdlib.h>

int main(void) {
  const char *path = getenv("PATH");
  if (!path || !path[0])
    return 1;
  if (getenv("CBC_STDLIB_NO_SUCH_VAR") != 0)
    return 2;
  return 0;
}
