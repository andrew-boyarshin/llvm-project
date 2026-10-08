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
  initializeCBCRejectUnsupportedIRLegacyPass(PR);
  initializeCBCLowerGlobalsLegacyPass(PR);
  initializeCBCSynthesizeEntryLegacyPass(PR);
  initializeCBCLowerSjLjLegacyPass(PR);
  initializeCBCLowerEHLegacyPass(PR);
}

static Reloc::Model getReloc(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

static bool isSupportedCBCTriple(const Triple &TT) {
  // Host-triple bitcode (-fcbc) or legacy cbc_x86_64-* private triples.
  return TT.isCBCHostX86_64() || TT.getArch() == Triple::x86_64;
}

/// Keep the host arch/OS/ABI for data layout, but force CBC object format so
/// AsmPrinter uses CBCStreamer (not ELF) under -fcbc host triples.
static Triple makeCBCMachineTriple(const Triple &TT) {
  if (TT.getObjectFormat() == Triple::CBC)
    return TT;
  return Triple(TT.getArch(), TT.getSubArch(), TT.getVendor(), TT.getOS(),
                TT.getEnvironment(), Triple::CBC);
}

CBCTargetMachine::CBCTargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, std::optional<Reloc::Model> RM,
    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, makeCBCMachineTriple(TT), CPU, FS, Options,
                               getReloc(RM),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      Subtarget(makeCBCMachineTriple(TT), CPU, FS, *this),
      TLOF(new CBCTargetObjectFile()) {
  if (!isSupportedCBCTriple(TT))
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
    // Before LowerGlobals so fp80 globals get a clean error, not image layout.
    addPass(createCBCRejectUnsupportedIRPass());
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

void llvm::CBCStoreExportsOnTargetMachine(TargetMachine &TM,
                                          SmallVector<CBCExport, 0> Exports) {
  static_cast<CBCTargetMachine &>(TM).Exports = std::move(Exports);
}

SmallVector<CBCExport, 0>
llvm::CBCTakeExportsFromTargetMachine(TargetMachine &TM) {
  return std::move(static_cast<CBCTargetMachine &>(TM).Exports);
}
