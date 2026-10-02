#ifndef LLVM_LIB_TARGET_CBC_CBCTARGETOBJECTFILE_H
#define LLVM_LIB_TARGET_CBC_CBCTARGETOBJECTFILE_H

#include "llvm/Target/TargetLoweringObjectFile.h"

namespace llvm {
class CBCTargetObjectFile : public TargetLoweringObjectFile {
public:
  void Initialize(MCContext &Ctx, const TargetMachine &TM) override;
  MCSection *SelectSectionForGlobal(const GlobalObject *GO, SectionKind Kind,
                                    const TargetMachine &TM) const override {
    return TextSection;
  }
  MCSection *getExplicitSectionGlobal(const GlobalObject *GO, SectionKind Kind,
                                      const TargetMachine &TM) const override {
    return TextSection;
  }
};
} // namespace llvm

#endif
