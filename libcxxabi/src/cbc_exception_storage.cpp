#include "cxa_exception.h"

#include <stdint.h>

// Must match struct __cbc_fcb in llvm/tools/cbc-ld/crt/crt-cbc.c.
struct __cbc_eh {
  int kind;
  int in_pad;
  void *exc;
  uint64_t jmp_token;
  uint32_t jmp_label;
  int jmp_value;
};

struct __cbc_fcb {
  uint64_t magic;
  __cbc_eh eh;
  void **tls_blocks;
  __cxxabiv1::__cxa_eh_globals cxa_globals;
};

extern "C" __cbc_fcb *__cbc_fcb();

namespace __cxxabiv1 {
extern "C" {
__cxa_eh_globals *__cxa_get_globals() { return &__cbc_fcb()->cxa_globals; }
__cxa_eh_globals *__cxa_get_globals_fast() { return &__cbc_fcb()->cxa_globals; }
}
} // namespace __cxxabiv1
