#include <stdio.h>

int main(void) {
  int a = 0, b = 0;
  char word[8];
  if (sscanf("12 34 hi", "%d %d %7s", &a, &b, word) != 3)
    return 1;
  if (a != 12 || b != 34 || word[0] != 'h' || word[1] != 'i' || word[2] != 0)
    return 2;
  return 0;
}
