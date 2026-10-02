#ifndef LLVM_LIB_TARGET_CBC_CBCREGISTERINFO_H
#define LLVM_LIB_TARGET_CBC_CBCREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_HEADER
#include "CBCGenRegisterInfo.inc"

namespace llvm {
class CBCRegisterInfo : public CBCGenRegisterInfo {
public:
  CBCRegisterInfo();
  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const override;
  BitVector getReservedRegs(const MachineFunction &MF) const override;
  bool eliminateFrameIndex(MachineBasicBlock::iterator II, int SPAdj,
                           unsigned FIOperandNum,
                           RegScavenger *RS = nullptr) const override;
  Register getFrameRegister(const MachineFunction &MF) const override;
  const uint32_t *getCallPreservedMask(const MachineFunction &MF,
                                       CallingConv::ID) const override;
};
} // namespace llvm

#endif
