#include <math.h>

double add(double a, double b) { return a + b; }

int main(void) {
  if (lround(add(1.25, 2.25)) != 4)
    return 1;
  if (lround(add(-1.5, 0.25)) != -1)
    return 2;
  return 0;
}
