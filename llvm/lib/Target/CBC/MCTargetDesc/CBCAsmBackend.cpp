#include "CBCMCTargetDesc.h"
#include "llvm/MC/MCCBCObjectWriter.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCObjectWriter.h"

using namespace llvm;

namespace {
class CBCAsmBackend : public MCAsmBackend {
public:
  CBCAsmBackend() : MCAsmBackend(llvm::endianness::little) {}
  void applyFixup(const MCFragment &, const MCFixup &, const MCValue &,
                  uint8_t *, uint64_t, bool) override {}
  bool writeNopData(raw_ostream &, uint64_t, const MCSubtargetInfo *) const override {
    return true;
  }
  std::unique_ptr<MCObjectTargetWriter> createObjectTargetWriter() const override {
    return std::make_unique<MCCBCObjectTargetWriter>();
  }
};
} // namespace

MCAsmBackend *llvm::createCBCAsmBackend(const Target &, const MCSubtargetInfo &,
                                        const MCRegisterInfo &,
                                        const MCTargetOptions &) {
  return new CBCAsmBackend();
}
