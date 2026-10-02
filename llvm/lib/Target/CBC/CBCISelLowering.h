#ifndef LLVM_LIB_TARGET_CBC_CBCISELLOWERING_H
#define LLVM_LIB_TARGET_CBC_CBCISELLOWERING_H

#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {
class CBCSubtarget;
class CBCTargetLowering : public TargetLowering {
public:
  CBCTargetLowering(const TargetMachine &TM, const CBCSubtarget &STI);
  const char *getTargetNodeName(unsigned Opcode) const override {
    return nullptr;
  }

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;
  SDValue LowerCall(CallLoweringInfo &CLI,
                     SmallVectorImpl<SDValue> &InVals) const override;
  void AdjustInstrPostInstrSelection(MachineInstr &MI,
                                     SDNode *Node) const override;
  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool IsVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;
  MachineBasicBlock *
  EmitInstrWithCustomInserter(MachineInstr &MI,
                              MachineBasicBlock *MBB) const override;
  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;
  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg, const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;
  EVT getSetCCResultType(const DataLayout &DL, LLVMContext &Context,
                         EVT VT) const override;
  // Keep SDIV/SREM nodes for constant divisors. Magic-mul expansion is wrong
  // for CBC W32 values (bits 63:32 undefined after Binary32).
  bool isIntDivCheap(EVT VT, AttributeList) const override {
    return VT == MVT::i32 || VT == MVT::i64;
  }
  // Unaligned i16 must stay a single SEXTLOAD. If LegalizeDAG splits it into
  // two i8 ZEXTLOADs + OR, the sign bit of a packed/unaligned short is lost.
  bool allowsMisalignedMemoryAccesses(
      EVT VT, unsigned AddrSpace = 0, Align Alignment = Align(1),
      MachineMemOperand::Flags Flags = MachineMemOperand::MONone,
      unsigned *Fast = nullptr) const override {
    (void)AddrSpace;
    (void)Alignment;
    (void)Flags;
    if (!VT.isSimple())
      return false;
    MVT SVT = VT.getSimpleVT();
    if (SVT != MVT::i8 && SVT != MVT::i16 && SVT != MVT::i32 &&
        SVT != MVT::i64 && SVT != MVT::f32 && SVT != MVT::f64)
      return false;
    if (Fast)
      *Fast = 1;
    return true;
  }
  std::pair<unsigned, const TargetRegisterClass *> getRegForInlineAsmConstraint(
      const TargetRegisterInfo *TRI, StringRef Constraint,
      MVT VT) const override;
  Register getExceptionPointerRegister(ExceptionHandling,
                                       const Constant *) const override;
  Register getExceptionSelectorRegister(ExceptionHandling,
                                        const Constant *) const override;
};
} // namespace llvm

#endif
