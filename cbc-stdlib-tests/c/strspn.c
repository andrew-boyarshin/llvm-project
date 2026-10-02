#include <string.h>

int main(void) {
  if (strspn("12ab", "0123456789") != 2)
    return 1;
  if (strcspn("12ab", "a") != 2)
    return 2;
  const char *hit = strpbrk("12abX", "bx");
  if (!hit || *hit != 'b')
    return 3;
  if (strspn("", "abc") != 0)
    return 4;
  return 0;
}
