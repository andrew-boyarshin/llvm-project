#include "CBCRegisterInfo.h"
#include "CBC.h"
#include "CBCFrameLowering.h"
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

const MCPhysReg *
CBCRegisterInfo::getCalleeSavedRegs(const MachineFunction *) const {
  static const MCPhysReg CSR[] = {CBC::IR8,  CBC::IR9,  CBC::IR10, CBC::IR11,
                                  CBC::IR12, CBC::IR13, CBC::FR8,  CBC::FR9,
                                  CBC::FR10,
                                  CBC::FR11, CBC::FR12, CBC::FR13, CBC::FR14,
                                  CBC::FR15, 0};
  return CSR;
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
CBCRegisterInfo::getCallPreservedMask(const MachineFunction &,
                                      CallingConv::ID) const {
  static uint32_t Mask[(CBC::NUM_TARGET_REGS + 31) / 32] = {};
  static bool Init = false;
  if (!Init) {
    for (MCPhysReg R : {CBC::IRZ, CBC::IR8, CBC::IR9, CBC::IR10, CBC::IR11,
                        CBC::IR12, CBC::IR13, CBC::FR8, CBC::FR9, CBC::FR10,
                        CBC::FR11, CBC::FR12, CBC::FR13, CBC::FR14, CBC::FR15})
      Mask[R / 32] |= 1u << (R % 32);
    Init = true;
  }
  return Mask;
}
