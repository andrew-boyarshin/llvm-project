#include <stdio.h>
#include <string.h>

int main(void) {
  const char *path = "/tmp/cbc-stdlib-puts.txt";
  if (!freopen(path, "w", stdout))
    return 1;
  if (puts("hello") < 0)
    return 2;
  if (fflush(stdout) != 0)
    return 3;
  FILE *f = fopen(path, "r");
  char buf[16];
  if (!f || !fgets(buf, sizeof buf, f))
    return 4;
  fclose(f);
  remove(path);
  return strcmp(buf, "hello\n") == 0 ? 0 : 5;
}
