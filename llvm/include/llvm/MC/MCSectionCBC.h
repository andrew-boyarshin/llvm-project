#ifndef LLVM_MC_MCSECTIONCBC_H
#define LLVM_MC_MCSECTIONCBC_H

#include "llvm/MC/MCSection.h"

namespace llvm {

class MCSectionCBC final : public MCSection {
  friend class MCContext;
  MCSectionCBC()
      : MCSection("", /*IsText=*/true, /*IsVirtual=*/false, /*Begin=*/nullptr) {}
};

} // namespace llvm

#endif
