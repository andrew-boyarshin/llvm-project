#include <stdio.h>

int main(void) {
  char buf[16];
  int n = snprintf(buf, sizeof buf, "%s %d", "ok", 3);
  if (n != 4 || buf[0] != 'o' || buf[3] != '3' || buf[4] != 0)
    return 1;
  n = snprintf(buf, 2, "abcd");
  if (n != 4 || buf[0] != 'a' || buf[1] != 0)
    return 2;
  return 0;
}
