#ifndef LLVM_MC_MCCBCSTREAMER_H
#define LLVM_MC_MCCBCSTREAMER_H

#include "llvm/MC/MCObjectStreamer.h"

namespace llvm {

class MCCBCStreamer : public MCObjectStreamer {
public:
  MCCBCStreamer(MCContext &Context, std::unique_ptr<MCAsmBackend> TAB,
                std::unique_ptr<MCObjectWriter> OW,
                std::unique_ptr<MCCodeEmitter> Emitter);

  bool emitSymbolAttribute(MCSymbol *Symbol, MCSymbolAttr Attribute) override {
    return false;
  }
  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override {}
};

MCStreamer *createCBCStreamer(MCContext &Ctx, std::unique_ptr<MCAsmBackend> &&TAB,
                              std::unique_ptr<MCObjectWriter> &&OW,
                              std::unique_ptr<MCCodeEmitter> &&CE);

} // namespace llvm

#endif
