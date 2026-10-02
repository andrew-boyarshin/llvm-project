#include <stdlib.h>

extern "C" void __cbc_exit_handlers(void);

static int hits = 0;

static void on_exit(void) { hits += 1; }

int main() {
  if (atexit(on_exit) != 0)
    return 1;
  __cbc_exit_handlers();
  return hits == 1 ? 0 : 2;
}
