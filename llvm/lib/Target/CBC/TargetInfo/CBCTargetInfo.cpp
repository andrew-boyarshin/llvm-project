#include "TargetInfo/CBCTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

Target &llvm::getTheCBCTarget() {
  static Target TheCBCTarget;
  return TheCBCTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeCBCTargetInfo() {
  RegisterTarget<Triple::cbc> X(getTheCBCTarget(), "cbc", "CBC bytecode", "CBC");
}
