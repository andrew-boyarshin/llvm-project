#include <math.h>

int main(void) {
  if (lround(sqrt(4.0)) != 2)
    return 1;
  if (lround(fabs(-3.25)) != 3)
    return 2;
  if (lround(pow(2.0, 3.0)) != 8)
    return 3;
  if (lround(fmod(5.0, 2.0)) != 1)
    return 4;
  if (lround(floor(3.2)) != 3 || lround(ceil(3.2)) != 4)
    return 5;
  return 0;
}
