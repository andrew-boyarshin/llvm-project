#ifndef LLVM_LIB_TARGET_CBC_CBCTARGETMACHINE_H
#define LLVM_LIB_TARGET_CBC_CBCTARGETMACHINE_H

#include "CBCFileWriter.h"
#include "CBCSubtarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/MachineFunction.h"

namespace llvm {
struct CBCMachineFunctionInfo : public MachineFunctionInfo {
  unsigned MaxOutgoingSlots = 0;
  CBCMachineFunctionInfo(const Function &, const TargetSubtargetInfo *) {}
};

class CBCTargetMachine : public CodeGenTargetMachineImpl {
  CBCSubtarget Subtarget;
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
public:
  /// Filled by CBCAsmPrinter::emitEndOfAsmFile for native shared-library wrap.
  SmallVector<CBCExport, 0> Exports;

  CBCTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                   StringRef FS, const TargetOptions &Options,
                   std::optional<Reloc::Model> RM,
                   std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                   bool JIT);
  const CBCSubtarget *getSubtargetImpl(const Function &) const override {
    return &Subtarget;
  }
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;
  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }
  MachineFunctionInfo *
  createMachineFunctionInfo(BumpPtrAllocator &Allocator, const Function &F,
                            const TargetSubtargetInfo *STI) const override;
};
} // namespace llvm

#endif
