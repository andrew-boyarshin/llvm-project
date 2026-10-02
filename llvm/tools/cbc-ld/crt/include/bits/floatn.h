/* CBC has no __float128. glibc's x86-64 bits/floatn.h turns __HAVE_FLOAT128
   on because this target defines __x86_64__, then uses a machine mode the
   CBC backend rejects. This header is installed ahead of the system include
   path and supplies the same macros with float128 disabled. */
#ifndef _BITS_FLOATN_H
#define _BITS_FLOATN_H

#include <features.h>

#define __HAVE_FLOAT128 0
#define __HAVE_DISTINCT_FLOAT128 0
#define __HAVE_FLOAT64X 1
#define __HAVE_FLOAT64X_LONG_DOUBLE 1

#include <bits/floatn-common.h>

#endif
