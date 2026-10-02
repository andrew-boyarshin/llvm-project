#include <stdio.h>
#include <string.h>

int main(void) {
  const char *path = "/tmp/cbc-stdlib-stdio.txt";
  FILE *f = fopen(path, "w");
  if (!f)
    return 1;
  if (fprintf(f, "abc %d", 7) < 0)
    return 2;
  if (fclose(f) != 0)
    return 3;
  f = fopen(path, "r");
  if (!f)
    return 4;
  char word[8];
  int n = 0;
  if (fscanf(f, "%7s %d", word, &n) != 2)
    return 5;
  if (fseek(f, 0, SEEK_SET) != 0)
    return 6;
  char line[16];
  if (!fgets(line, sizeof line, f))
    return 7;
  fclose(f);
  remove(path);
  if (strcmp(word, "abc") != 0 || n != 7)
    return 8;
  return strcmp(line, "abc 7") == 0 ? 0 : 9;
}
