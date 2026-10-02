#ifndef LLVM_LIB_TARGET_CBC_CBCSUBTARGET_H
#define LLVM_LIB_TARGET_CBC_CBCSUBTARGET_H

#include "CBCFrameLowering.h"
#include "CBCISelLowering.h"
#include "CBCInstrInfo.h"
#include "llvm/CodeGen/SelectionDAGTargetInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/TargetParser/Triple.h"

#define GET_SUBTARGETINFO_HEADER
#include "CBCGenSubtargetInfo.inc"

namespace llvm {
class CBCSubtarget : public CBCGenSubtargetInfo {
  Triple TT;
  CBCInstrInfo InstrInfo;
  CBCFrameLowering FrameLowering;
  CBCTargetLowering TLInfo;
  SelectionDAGTargetInfo TSInfo;
public:
  CBCSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
               const TargetMachine &TM);
  const CBCInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const CBCRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const CBCFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const CBCTargetLowering *getTargetLowering() const override { return &TLInfo; }
  const SelectionDAGTargetInfo *getSelectionDAGInfo() const override {
    return &TSInfo;
  }
  void initLibcallLoweringInfo(LibcallLoweringInfo &Info) const override;
  bool isX86_64() const { return TT.isCBCHostX86_64(); }
  bool enableIndirectBrExpand() const override { return true; }
};
} // namespace llvm

#endif
