#include "CBCTargetObjectFile.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionCBC.h"

using namespace llvm;

void CBCTargetObjectFile::Initialize(MCContext &Ctx, const TargetMachine &TM) {
  TargetLoweringObjectFile::Initialize(Ctx, TM);
  TextSection = Ctx.getCBCSection();
}
