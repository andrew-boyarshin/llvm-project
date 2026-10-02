#ifndef LLVM_MC_MCCBCOBJECTWRITER_H
#define LLVM_MC_MCCBCOBJECTWRITER_H

#include "llvm/MC/MCObjectWriter.h"
#include <memory>

namespace llvm {

class MCCBCObjectTargetWriter : public MCObjectTargetWriter {
public:
  Triple::ObjectFormatType getFormat() const override { return Triple::CBC; }
  static bool classof(const MCObjectTargetWriter *W) {
    return W->getFormat() == Triple::CBC;
  }
};

class LLVM_ABI CBCObjectWriter final : public MCObjectWriter {
  raw_pwrite_stream &OS;
  std::unique_ptr<MCCBCObjectTargetWriter> TargetObjectWriter;

public:
  CBCObjectWriter(std::unique_ptr<MCCBCObjectTargetWriter> MOTW,
                  raw_pwrite_stream &OS)
      : OS(OS), TargetObjectWriter(std::move(MOTW)) {}

private:
  uint64_t writeObject() override;
};

LLVM_ABI std::unique_ptr<MCObjectWriter>
createCBCObjectWriter(std::unique_ptr<MCCBCObjectTargetWriter> MOTW,
                      raw_pwrite_stream &OS);

} // namespace llvm

#endif
