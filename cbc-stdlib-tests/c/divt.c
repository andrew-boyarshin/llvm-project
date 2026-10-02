#include <stdlib.h>

int main(void) {
  div_t d = div(7, 3);
  if (d.quot != 2 || d.rem != 1)
    return 1;
  d = div(-7, 3);
  if (d.quot != -2 || d.rem != -1)
    return 2;
  return 0;
}
