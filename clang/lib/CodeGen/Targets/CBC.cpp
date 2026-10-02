#include "TargetInfo.h"

using namespace clang;
using namespace clang::CodeGen;

std::unique_ptr<TargetCodeGenInfo>
CodeGen::createCBCTargetCodeGenInfo(CodeGenModule &CGM) {
  // Host System V classification only. The x86-64 target info also installs
  // DWARF register numbers and inline-asm rewrites this target does not use.
  return std::make_unique<TargetCodeGenInfo>(
      createX86_64SysVABIInfo(CGM.getTypes()));
}
