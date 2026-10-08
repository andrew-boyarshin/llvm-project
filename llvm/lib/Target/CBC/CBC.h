#ifndef LLVM_LIB_TARGET_CBC_CBC_H
#define LLVM_LIB_TARGET_CBC_CBC_H

#include "MCTargetDesc/CBCMCTargetDesc.h"
#include "llvm/PassRegistry.h"

namespace llvm {
class CBCTargetMachine;
class FunctionPass;
class ModulePass;
FunctionPass *createCBCISelDag(CBCTargetMachine &TM);
ModulePass *createCBCLowerGlobalsPass();
ModulePass *createCBCSynthesizeEntryPass();
ModulePass *createCBCLowerSjLjPass();
ModulePass *createCBCLowerEHPass();
ModulePass *createCBCRejectUnsupportedIRPass();
void initializeCBCDAGToDAGISelLegacyPass(PassRegistry &);
void initializeCBCLowerGlobalsLegacyPass(PassRegistry &);
void initializeCBCSynthesizeEntryLegacyPass(PassRegistry &);
void initializeCBCLowerSjLjLegacyPass(PassRegistry &);
void initializeCBCLowerEHLegacyPass(PassRegistry &);
void initializeCBCRejectUnsupportedIRLegacyPass(PassRegistry &);
} // namespace llvm

#endif
