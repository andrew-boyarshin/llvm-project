/* Itanium unwind declarations for the CBC target.
   One header: libcxxabi/src/cxa_exception.h includes "unwind.h" only.
   LP64 layout, 32 bytes, matching getSizeOfUnwindException(). */
#ifndef CBC_UNWIND_H
#define CBC_UNWIND_H

#include <stdint.h>

typedef enum {
  _URC_NO_REASON = 0,
  _URC_OK = 0,
  _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
  _URC_FATAL_PHASE2_ERROR = 2,
  _URC_FATAL_PHASE1_ERROR = 3,
  _URC_NORMAL_STOP = 4,
  _URC_END_OF_STACK = 5,
  _URC_HANDLER_FOUND = 6,
  _URC_INSTALL_CONTEXT = 7,
  _URC_CONTINUE_UNWIND = 8
} _Unwind_Reason_Code;

typedef enum {
  _UA_SEARCH_PHASE = 1,
  _UA_CLEANUP_PHASE = 2,
  _UA_HANDLER_FRAME = 4,
  _UA_FORCE_UNWIND = 8,
  _UA_END_OF_STACK = 16
} _Unwind_Action;

typedef uint64_t _Unwind_Exception_Class;

struct _Unwind_Exception {
  _Unwind_Exception_Class exception_class;
  void (*exception_cleanup)(_Unwind_Reason_Code, struct _Unwind_Exception *);
  uintptr_t private_1;
  uintptr_t private_2;
} __attribute__((__aligned__));

typedef struct _Unwind_Exception _Unwind_Exception;

#ifdef __cplusplus
extern "C" {
#endif

_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *);
void _Unwind_Resume(struct _Unwind_Exception *);
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *);
void _Unwind_DeleteException(struct _Unwind_Exception *);

#ifdef __cplusplus
}
#endif

#endif
