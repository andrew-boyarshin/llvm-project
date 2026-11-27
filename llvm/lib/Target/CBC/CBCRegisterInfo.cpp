#include "CBCRegisterInfo.h"
#include "CBC.h"
#include "CBCFrameLowering.h"
#include "CBCSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/Support/ErrorHandling.h"

#define GET_REGINFO_TARGET_DESC
#include "CBCGenRegisterInfo.inc"

using namespace llvm;

CBCRegisterInfo::CBCRegisterInfo()
    : CBCGenRegisterInfo(CBC::IRZ, 0, 0, CBC::IRZ, 0) {}

static bool isAArch64Flavour(const MachineFunction *MF) {
  return MF && MF->getSubtarget<CBCSubtarget>().isAArch64();
}

// Callee-saved registers per host flavour (engine PlatformTraits IR_VOLATILES):
// x86-64 volatile IR1..IR7, so IR8..IR13 are preserved; AArch64 volatile
// IR1..IR10 (arguments, sret register, tail register), so IR11..IR13.
const MCPhysReg *
CBCRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  static const MCPhysReg CSRX64[] = {CBC::IR8,  CBC::IR9,  CBC::IR10, CBC::IR11,
                                     CBC::IR12, CBC::IR13, CBC::FR8,  CBC::FR9,
                                     CBC::FR10, CBC::FR11, CBC::FR12, CBC::FR13,
                                     CBC::FR14, CBC::FR15, 0};
  static const MCPhysReg CSRA64[] = {CBC::IR11, CBC::IR12, CBC::IR13, CBC::FR8,
                                     CBC::FR9,  CBC::FR10, CBC::FR11, CBC::FR12,
                                     CBC::FR13, CBC::FR14, CBC::FR15, 0};
  return isAArch64Flavour(MF) ? CSRA64 : CSRX64;
}

BitVector CBCRegisterInfo::getReservedRegs(const MachineFunction &) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(CBC::IRZ);
  Reserved.set(CBC::IRACC);
  return Reserved;
}

bool CBCRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                          int, unsigned FIOperandNum,
                                          RegScavenger *) const {
  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineOperand &FIOp = MI.getOperand(FIOperandNum);
  int Idx = FIOp.getIndex();
  if (MFI.getStackID(Idx) == TargetStackID::NoAlloc) {
    FIOp.ChangeToImmediate(MFI.getObjectOffset(Idx));
    return false;
  }
  int64_t Disp = MFI.getStackSize() + MFI.getObjectOffset(Idx);
  if (Disp < 0)
    report_fatal_error("CBC frame object offset is negative");
  FIOp.ChangeToImmediate(Disp);
  return false;
}

Register CBCRegisterInfo::getFrameRegister(const MachineFunction &) const {
  return CBC::IRZ;
}

const uint32_t *
CBCRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                      CallingConv::ID) const {
  static uint32_t MaskX64[(CBC::NUM_TARGET_REGS + 31) / 32] = {};
  static uint32_t MaskA64[(CBC::NUM_TARGET_REGS + 31) / 32] = {};
  static bool Init = false;
  if (!Init) {
    for (MCPhysReg R : {CBC::IRZ, CBC::IR8, CBC::IR9, CBC::IR10, CBC::IR11,
                        CBC::IR12, CBC::IR13, CBC::FR8, CBC::FR9, CBC::FR10,
                        CBC::FR11, CBC::FR12, CBC::FR13, CBC::FR14, CBC::FR15})
      MaskX64[R / 32] |= 1u << (R % 32);
    for (MCPhysReg R : {CBC::IRZ, CBC::IR11, CBC::IR12, CBC::IR13, CBC::FR8,
                        CBC::FR9, CBC::FR10, CBC::FR11, CBC::FR12, CBC::FR13,
                        CBC::FR14, CBC::FR15})
      MaskA64[R / 32] |= 1u << (R % 32);
    Init = true;
  }
  return isAArch64Flavour(&MF) ? MaskA64 : MaskX64;
}
