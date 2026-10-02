/* CBC has no vector registers. Host headers (SDL, etc.) include this under
   __x86_64__; provide an empty include guard so they take the portable path. */
#ifndef __IMMINTRIN_H
#define __IMMINTRIN_H
#endif
