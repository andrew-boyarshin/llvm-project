#include "cxa_exception.h"
#include "private_typeinfo.h"

#include <stddef.h>

using namespace __cxxabiv1;

extern "C" int __cbc_can_catch(const void *catch_tinfo, void *exn, int sel);
extern "C" int __cbc_matches_filter(const void *list, void *exn);

static __cxa_exception *header_from_unwind(_Unwind_Exception *ue) {
  return reinterpret_cast<__cxa_exception *>(ue + 1) - 1;
}

static void *thrown_object(_Unwind_Exception *ue) { return ue + 1; }

extern "C" int __cbc_can_catch(const void *catch_tinfo, void *exn, int sel) {
  if (!exn || !catch_tinfo)
    return 0;
  auto *ue = static_cast<_Unwind_Exception *>(exn);
  __cxa_exception *header = header_from_unwind(ue);
  if (!header->exceptionType)
    return 0;
  void *adjusted = thrown_object(ue);
  const auto *thrown =
      static_cast<const __shim_type_info *>(header->exceptionType);
  const auto *catch_ty = static_cast<const __shim_type_info *>(catch_tinfo);
  if (!catch_ty->can_catch(thrown, adjusted))
    return 0;
  header->adjustedPtr = adjusted;
  header->handlerSwitchValue = sel;
  return 1;
}

extern "C" int __cbc_matches_filter(const void *list, void *exn) {
  if (!exn || !list)
    return 0;
  auto *ue = static_cast<_Unwind_Exception *>(exn);
  __cxa_exception *header = header_from_unwind(ue);
  if (!header->exceptionType)
    return 0;
  const auto *thrown =
      static_cast<const __shim_type_info *>(header->exceptionType);
  const std::type_info *const *types =
      static_cast<const std::type_info *const *>(list);
  for (; *types; ++types) {
    void *adjusted = thrown_object(ue);
    const auto *spec = static_cast<const __shim_type_info *>(*types);
    if (spec->can_catch(thrown, adjusted))
      return 1;
  }
  return 0;
}
