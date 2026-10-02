#ifndef LLVM_LIB_TARGET_CBC_MCTARGETDESC_CBCASMINFO_H
#define LLVM_LIB_TARGET_CBC_MCTARGETDESC_CBCASMINFO_H

#include "llvm/MC/MCAsmInfo.h"

namespace llvm {
class Triple;
class MCTargetOptions;
class CBCAsmInfo : public MCAsmInfo {
public:
  explicit CBCAsmInfo(const Triple &TT, const MCTargetOptions &Options);
};
} // namespace llvm

#endif
