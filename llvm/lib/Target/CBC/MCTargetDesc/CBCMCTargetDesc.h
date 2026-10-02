#ifndef LLVM_LIB_TARGET_CBC_MCTARGETDESC_CBCMCTARGETDESC_H
#define LLVM_LIB_TARGET_CBC_MCTARGETDESC_CBCMCTARGETDESC_H

namespace llvm {
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;
MCCodeEmitter *createCBCMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx);
MCAsmBackend *createCBCAsmBackend(const Target &T, const MCSubtargetInfo &STI,
                                  const MCRegisterInfo &MRI,
                                  const MCTargetOptions &Options);
} // namespace llvm

#define GET_REGINFO_ENUM
#include "CBCGenRegisterInfo.inc"
#define GET_INSTRINFO_ENUM
#include "CBCGenInstrInfo.inc"

#endif
