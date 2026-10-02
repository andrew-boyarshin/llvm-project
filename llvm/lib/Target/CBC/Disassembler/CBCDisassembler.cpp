#include "MCTargetDesc/CBCEncoding.h"
#include "MCTargetDesc/CBCMCTargetDesc.h"
#include "TargetInfo/CBCTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

namespace {
class CBCDisassembler : public MCDisassembler {
public:
  CBCDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}
  DecodeStatus getInstruction(MCInst &MI, uint64_t &Size, ArrayRef<uint8_t> Bytes,
                              uint64_t, raw_ostream &) const override {
    Size = 0;
    if (!decodeCBCInst(Bytes, MI, Size))
      return Fail;
    return Success;
  }
};
} // namespace

static MCDisassembler *createCBCDisassembler(const Target &,
                                             const MCSubtargetInfo &STI,
                                             MCContext &Ctx) {
  return new CBCDisassembler(STI, Ctx);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeCBCDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheCBCTarget(),
                                         createCBCDisassembler);
}
