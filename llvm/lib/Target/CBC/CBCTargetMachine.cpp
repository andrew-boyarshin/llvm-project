#include "CBCTargetMachine.h"
#include "CBC.h"
#include "CBCTargetObjectFile.h"
#include "TargetInfo/CBCTargetInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Transforms/IPO/ExpandVariadics.h"

using namespace llvm;

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeCBCTarget() {
  RegisterTargetMachine<CBCTargetMachine> X(getTheCBCTarget());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeCBCDAGToDAGISelLegacyPass(PR);
  initializeCBCLowerGlobalsLegacyPass(PR);
  initializeCBCSynthesizeEntryLegacyPass(PR);
  initializeCBCLowerSjLjLegacyPass(PR);
  initializeCBCLowerEHLegacyPass(PR);
}

static Reloc::Model getReloc(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

CBCTargetMachine::CBCTargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, std::optional<Reloc::Model> RM,
    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT, CPU, FS, Options, getReloc(RM),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      Subtarget(TT, CPU, FS, *this), TLOF(new CBCTargetObjectFile()) {
  if (!TT.isCBCHostX86_64())
    report_fatal_error(
        "the AArch64 flavour of the CBC target is not implemented yet");
  initAsmInfo();
}

MachineFunctionInfo *CBCTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return MachineFunctionInfo::create<CBCMachineFunctionInfo>(Allocator, F, STI);
}

namespace {
class CBCPassConfig : public TargetPassConfig {
public:
  CBCPassConfig(CBCTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}
  void addIRPasses() override {
    addPass(createExpandVariadicsPass(ExpandVariadicsMode::Lowering));
    addPass(createIndirectBrExpandPass());
    addPass(createCBCLowerEHPass());
    addPass(createCBCLowerGlobalsPass());
    addPass(createCBCSynthesizeEntryPass());
    addPass(createCBCLowerSjLjPass());
    TargetPassConfig::addIRPasses();
  }
  bool addInstSelector() override {
    addPass(createCBCISelDag(getTM<CBCTargetMachine>()));
    return false;
  }
};
} // namespace

TargetPassConfig *CBCTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new CBCPassConfig(*this, PM);
}
