#include "CBCMCTargetDesc.h"
#include "CBCAsmInfo.h"
#include "CBCInstPrinter.h"
#include "TargetInfo/CBCTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

#define GET_INSTRINFO_MC_DESC
#include "CBCGenInstrInfo.inc"
#define GET_SUBTARGETINFO_MC_DESC
#include "CBCGenSubtargetInfo.inc"
#define GET_REGINFO_MC_DESC
#include "CBCGenRegisterInfo.inc"

using namespace llvm;

static MCInstrInfo *createCBCMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitCBCMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createCBCMCRegisterInfo(const Triple &) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitCBCMCRegisterInfo(X, CBC::IRZ, 0, 0, CBC::IRZ);
  return X;
}

static MCSubtargetInfo *createCBCMCSubtargetInfo(const Triple &TT, StringRef CPU,
                                                 StringRef FS) {
  return createCBCMCSubtargetInfoImpl(TT, CPU, CPU, FS);
}

static MCInstPrinter *createCBCInstPrinter(const Triple &, unsigned Syntax,
                                           const MCAsmInfo &MAI,
                                           const MCInstrInfo &MII,
                                           const MCRegisterInfo &MRI) {
  if (Syntax == 0)
    return new CBCInstPrinter(MAI, MII, MRI);
  return nullptr;
}

static MCAsmInfo *createCBCAsmInfo(const MCRegisterInfo &, const Triple &TT,
                                   const MCTargetOptions &Options) {
  return new CBCAsmInfo(TT, Options);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeCBCTargetMC() {
  Target &T = getTheCBCTarget();
  TargetRegistry::RegisterMCAsmInfo(T, createCBCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createCBCMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createCBCMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createCBCMCSubtargetInfo);
  TargetRegistry::RegisterMCInstPrinter(T, createCBCInstPrinter);
  TargetRegistry::RegisterMCCodeEmitter(T, createCBCMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createCBCAsmBackend);
}
