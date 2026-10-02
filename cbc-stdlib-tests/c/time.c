#include <time.h>

int main(void) {
  time_t t = time(0);
  if (t == (time_t)-1)
    return 1;
  if (t <= 0)
    return 2;
  struct tm *parts = gmtime(&t);
  if (parts == 0)
    return 3;
  if (parts->tm_year < 100)
    return 4;
  return 0;
}
