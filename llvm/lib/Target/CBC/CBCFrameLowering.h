#ifndef LLVM_LIB_TARGET_CBC_CBCFRAMELOWERING_H
#define LLVM_LIB_TARGET_CBC_CBCFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {
class CBCFrameLowering : public TargetFrameLowering {
public:
  CBCFrameLowering()
      : TargetFrameLowering(StackGrowsDown, Align(16), 0, Align(16)) {}

  void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  bool spillCalleeSavedRegisters(MachineBasicBlock &, MachineBasicBlock::iterator,
                                 ArrayRef<CalleeSavedInfo>,
                                 const TargetRegisterInfo *) const override {
    return true;
  }
  bool restoreCalleeSavedRegisters(MachineBasicBlock &,
                                   MachineBasicBlock::iterator,
                                   MutableArrayRef<CalleeSavedInfo>,
                                   const TargetRegisterInfo *) const override {
    return true;
  }
  bool hasFPImpl(const MachineFunction &MF) const override;
  void processFunctionBeforeFrameFinalized(MachineFunction &MF,
                                           RegScavenger *RS) const override;
  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator I) const override {
    return MBB.erase(I);
  }
};
} // namespace llvm

#endif
