#include "CBCMCTargetDesc.h"
#include "CBCEncoding.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCInst.h"

using namespace llvm;

namespace {
class CBCMCCodeEmitter : public MCCodeEmitter {
public:
  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &,
                         const MCSubtargetInfo &) const override {
    encodeCBCInst(MI, CB);
  }
};
} // namespace

MCCodeEmitter *llvm::createCBCMCCodeEmitter(const MCInstrInfo &, MCContext &) {
  return new CBCMCCodeEmitter();
}
