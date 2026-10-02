#include <string.h>

int main(void) {
  char buf[32];
  if (strlen("abc") != 3)
    return 1;
  if (strcmp("abc", "abc") != 0 || strcmp("abc", "abd") >= 0)
    return 2;
  strcpy(buf, "hi");
  strcat(buf, "!");
  if (strcmp(buf, "hi!") != 0)
    return 3;
  if (strncmp("abcdef", "abcXYZ", 3) != 0)
    return 4;
  if (strstr("xxhaystack", "hay") == 0)
    return 5;
  if (strchr("abc", 'c') == 0 || strchr("abc", 'z') != 0)
    return 6;
  strncpy(buf, "toolong", 3);
  buf[3] = 0;
  return strcmp(buf, "too") == 0 ? 0 : 7;
}
