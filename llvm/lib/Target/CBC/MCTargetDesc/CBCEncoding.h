#ifndef LLVM_LIB_TARGET_CBC_MCTARGETDESC_CBCENCODING_H
#define LLVM_LIB_TARGET_CBC_MCTARGETDESC_CBCENCODING_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCInst.h"

namespace llvm {
unsigned cbcEncodeReg(MCRegister Reg);
void encodeCBCInst(const MCInst &MI, SmallVectorImpl<char> &CB);

/// Decode one instruction. Returns false when Bytes does not start with an
/// instruction this backend emits.
bool decodeCBCInst(ArrayRef<uint8_t> Bytes, MCInst &MI, uint64_t &Size);
} // namespace llvm

#endif
