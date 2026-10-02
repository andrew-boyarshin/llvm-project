#include <stdlib.h>

int main(void) {
  srand(1);
  int a1 = rand();
  int a2 = rand();
  srand(1);
  int b1 = rand();
  int b2 = rand();
  if (a1 != b1 || a2 != b2)
    return 1;
  return (a1 != 0 || a2 != 0) ? 0 : 2;
}
