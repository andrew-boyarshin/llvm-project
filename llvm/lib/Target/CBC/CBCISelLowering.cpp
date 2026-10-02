#include "CBCISelLowering.h"
#include "CBC.h"
#include "CBCSubtarget.h"
#include "CBCTargetMachine.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/Support/MathExtras.h"
#include <string>

using namespace llvm;

#define DEBUG_TYPE "cbc-lower"

#define GET_CALLING_CONV_IMPL
#include "CBCGenCallingConv.inc"

CBCTargetLowering::CBCTargetLowering(const TargetMachine &TM,
                                     const CBCSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &CBC::GPRRegClass);
  addRegisterClass(MVT::i64, &CBC::GPRRegClass);
  addRegisterClass(MVT::f32, &CBC::FPRRegClass);
  addRegisterClass(MVT::f64, &CBC::FPRRegClass);
  computeRegisterProperties(STI.getRegisterInfo());
  setBooleanContents(ZeroOrOneBooleanContent);
  for (MVT VT : {MVT::i32, MVT::i64, MVT::f32, MVT::f64}) {
    setOperationAction(ISD::LOAD, VT, Legal);
    setOperationAction(ISD::STORE, VT, Legal);
  }
  for (MVT VT : {MVT::i32, MVT::i64}) {
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::i8, Legal);
    setLoadExtAction(ISD::ZEXTLOAD, VT, MVT::i8, Legal);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i8, Legal);
    setTruncStoreAction(VT, MVT::i8, Legal);
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::i16, Legal);
    setLoadExtAction(ISD::ZEXTLOAD, VT, MVT::i16, Legal);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i16, Legal);
    setTruncStoreAction(VT, MVT::i16, Legal);
  }
  setOperationAction(ISD::ADD, MVT::i32, Legal);
  setOperationAction(ISD::ADD, MVT::i64, Legal);
  setOperationAction(ISD::SUB, MVT::i64, Legal);
  setOperationAction(ISD::AND, MVT::i64, Legal);
  setOperationAction(ISD::OR, MVT::i64, Legal);
  setOperationAction(ISD::XOR, MVT::i64, Legal);
  for (MVT VT : {MVT::i32, MVT::i64}) {
    setOperationAction(ISD::MUL, VT, Legal);
    setOperationAction(ISD::SHL, VT, Legal);
    setOperationAction(ISD::SRL, VT, Legal);
    setOperationAction(ISD::SRA, VT, Legal);
  }
  setOperationAction(ISD::SETCC, MVT::i32, Legal);
  setOperationAction(ISD::SETCC, MVT::i64, Legal);
  setOperationAction(ISD::BR_CC, MVT::i32, Legal);
  setOperationAction(ISD::BR_CC, MVT::i64, Legal);
  setOperationAction(ISD::BRCOND, MVT::Other, Expand);
  for (MVT VT : {MVT::i32, MVT::i64}) {
    setOperationAction(ISD::SELECT, VT, Expand);
    setOperationAction(ISD::SELECT_CC, VT, Legal);
  }
  setOperationAction(ISD::ConstantFP, MVT::f32, Legal);
  setOperationAction(ISD::ConstantFP, MVT::f64, Legal);
  setOperationAction(ISD::GlobalAddress, MVT::i64, Custom);
  for (MVT VT : {MVT::i32, MVT::i64}) {
    setOperationAction(ISD::UMUL_LOHI, VT, Legal);
    setOperationAction(ISD::SMUL_LOHI, VT, Legal);
    setOperationAction(ISD::SDIV, VT, Legal);
    setOperationAction(ISD::UDIV, VT, Legal);
    setOperationAction(ISD::SREM, VT, Legal);
    setOperationAction(ISD::UREM, VT, Legal);
    for (unsigned Opc :
         {ISD::CTPOP, ISD::CTLZ, ISD::CTTZ, ISD::CTLZ_ZERO_POISON,
          ISD::CTTZ_ZERO_POISON, ISD::BSWAP, ISD::BITREVERSE, ISD::ROTL,
          ISD::ROTR, ISD::FSHL, ISD::FSHR})
      setOperationAction(static_cast<unsigned>(Opc), VT, Expand);
  }
  for (MVT VT : {MVT::f32, MVT::f64}) {
    setOperationAction(ISD::FADD, VT, Legal);
    setOperationAction(ISD::FSUB, VT, Legal);
    setOperationAction(ISD::FMUL, VT, Legal);
    setOperationAction(ISD::FDIV, VT, Legal);
    setOperationAction(ISD::FNEG, VT, Legal);
    setOperationAction(ISD::FABS, VT, Legal);
    setOperationAction(ISD::FSQRT, VT, Legal);
    setOperationAction(ISD::SETCC, VT, Legal);
    setOperationAction(ISD::BR_CC, VT, Legal);
    setOperationAction(ISD::SELECT_CC, VT, Legal);
    setOperationAction(ISD::FREM, VT, LibCall);
    setOperationAction(ISD::FMA, VT, LibCall);
    setOperationAction(ISD::FMINNUM, VT, LibCall);
    setOperationAction(ISD::FMAXNUM, VT, LibCall);
    setOperationAction(ISD::FCEIL, VT, LibCall);
    setOperationAction(ISD::FFLOOR, VT, LibCall);
    setOperationAction(ISD::FTRUNC, VT, LibCall);
    setOperationAction(ISD::FROUND, VT, LibCall);
    setOperationAction(ISD::FNEARBYINT, VT, LibCall);
    setOperationAction(ISD::FRINT, VT, LibCall);
    setOperationAction(ISD::FCOPYSIGN, VT, Expand);
    setOperationAction(ISD::SELECT, VT, Expand);
  }
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64, Custom);
  setOperationAction(ISD::STACKSAVE, MVT::Other, Custom);
  setOperationAction(ISD::STACKSAVE, MVT::i64, Custom);
  setOperationAction(ISD::STACKRESTORE, MVT::Other, Custom);
  setOperationAction(ISD::FRAMEADDR, MVT::i64, Custom);
  for (unsigned Opc : {ISD::SHL_PARTS, ISD::SRL_PARTS, ISD::SRA_PARTS})
    setOperationAction(Opc, MVT::i64, Custom);
}

SDValue CBCTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_CBC);
  SDValue Glue;
  if (RVLocs.empty()) {
    Chain = DAG.getCopyToReg(Chain, DL, CBC::IR1,
                             DAG.getConstant(0, DL, MVT::i64), Glue);
    Glue = Chain.getValue(1);
  }
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[i], Glue);
    Glue = Chain.getValue(1);
  }
  SmallVector<SDValue, 2> RetOps(1, Chain);
  if (Glue.getNode())
    RetOps.push_back(Glue);
  bool FloatRet = !RVLocs.empty() && RVLocs[0].getLocReg() == CBC::FR0;
  unsigned Opc = FloatRet ? CBC::FRET : CBC::RET;
  return SDValue(DAG.getMachineNode(Opc, DL, MVT::Other, RetOps), 0);
}

SDValue CBCTargetLowering::LowerCall(CallLoweringInfo &CLI,
                                     SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc DL = CLI.DL;
  CLI.IsTailCall = false;
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CLI.CallConv, CLI.IsVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeCallOperands(CLI.Outs, CC_CBC);

  SDValue Chain = CLI.Chain;
  SDValue Glue;
  std::string Params;
  MachineRegisterInfo &MRI = DAG.getMachineFunction().getRegInfo();
  for (unsigned I = 0; I != ArgLocs.size(); ++I) {
    CCValAssign &VA = ArgLocs[I];
    SDValue Val = CLI.OutVals[I];
    if (VA.isMemLoc()) {
      if (Val.getValueType() == MVT::i32)
        Val = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i64, Val);
      else if (Val.getValueType() == MVT::f32) {
        Val = DAG.getNode(ISD::BITCAST, DL, MVT::i32, Val);
        Val = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i64, Val);
      } else if (Val.getValueType() == MVT::f64)
        Val = DAG.getNode(ISD::BITCAST, DL, MVT::i64, Val);
      Register VReg = MRI.createVirtualRegister(&CBC::GPRRegClass);
      // Chain only. Gluing this copy to the call as well cycles the scheduler:
      // the call would depend on the copy by glue and on the store by chain,
      // while the store depends on the copy.
      Chain = DAG.getCopyToReg(Chain, DL, VReg, Val);
      unsigned Slot = VA.getLocMemOffset() / 8;
      auto *Info =
          DAG.getMachineFunction().getInfo<CBCMachineFunctionInfo>();
      Info->MaxOutgoingSlots = std::max(Info->MaxOutgoingSlots, Slot + 1);
      SDValue Ops[] = {DAG.getRegister(VReg, MVT::i64),
                       DAG.getTargetConstant(Slot, DL, MVT::i64), Chain};
      Chain = SDValue(DAG.getMachineNode(CBC::STU64, DL, MVT::Other, Ops), 0);
      Glue = SDValue();
      continue;
    }
    Params.push_back(VA.getLocVT().isFloatingPoint() ? 'f' : 'i');
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), CLI.OutVals[I], Glue);
    Glue = Chain.getValue(1);
  }

  SDValue Callee = CLI.Callee;
  StringRef CalleeName;
  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee)) {
    const GlobalValue *GV = G->getGlobal();
    // Itanium complete-object constructors are aliases of the base constructor.
    if (const auto *Alias = dyn_cast<GlobalAlias>(GV))
      if (const auto *Fn = dyn_cast<Function>(Alias->getAliaseeObject()))
        GV = Fn;
    CalleeName = GV->getName();
    Callee = DAG.getTargetGlobalAddress(GV, DL, MVT::i64);
  } else if (auto *E = dyn_cast<ExternalSymbolSDNode>(Callee)) {
    const char *Sym = E->getSymbol();
    if (!Sym)
      Sym = "memset";
    CalleeName = Sym;
    Callee = DAG.getTargetExternalSymbol(Sym, MVT::i64);
  }
  bool Indirect = CalleeName.empty() &&
                  !isa<GlobalAddressSDNode>(CLI.Callee) &&
                  !isa<ExternalSymbolSDNode>(CLI.Callee);

  if (CalleeName == "__cbc_fcb") {
    SDValue V = SDValue(
        DAG.getMachineNode(CBC::LD_FCB, DL, DAG.getVTList(MVT::i64, MVT::Other),
                           Chain),
        0);
    InVals.push_back(V);
    return V.getValue(1);
  }

  /* The nullcheck must sit in __cbc_raise's frame, not the caller's, so a
     region covering the call does not catch the marker. */
  if (CalleeName == "__cbc_nullcheck")
    return SDValue(DAG.getMachineNode(CBC::NULLCHECK, DL, MVT::Other, Chain), 0);

  if (CalleeName == "__cbc_image_base") {
    SDVTList VTs = DAG.getVTList(MVT::i64, MVT::i64, MVT::Other, MVT::Glue);
    SDValue Addr = SDValue(DAG.getMachineNode(CBC::LEA_IMAGE, DL, VTs, Chain), 0);
    InVals.push_back(Addr);
    return Addr.getValue(2);
  }

  bool RetFloat = !CLI.Ins.empty() && CLI.Ins[0].VT.isFloatingPoint();
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Callee);
  Ops.push_back(DAG.getTargetConstant(Params.size(), DL, MVT::i32));
  for (char K : Params)
    Ops.push_back(DAG.getTargetConstant(K == 'f' ? 1 : 0, DL, MVT::i32));
  Ops.push_back(DAG.getTargetConstant(RetFloat ? 1 : 0, DL, MVT::i32));
  const uint32_t *Mask =
      DAG.getSubtarget().getRegisterInfo()->getCallPreservedMask(
          DAG.getMachineFunction(), CLI.CallConv);
  if (Mask)
    Ops.push_back(DAG.getRegisterMask(Mask));
  Ops.push_back(Chain);
  if (Glue.getNode())
    Ops.push_back(Glue);

  SDVTList VTs = DAG.getVTList(MVT::Other, MVT::Glue);
  unsigned CallOpc = Indirect ? CBC::CALL_INDIRECT : CBC::CALL;
  SDValue Call = SDValue(DAG.getMachineNode(CallOpc, DL, VTs, Ops), 0);
  Chain = Call.getValue(0);
  Glue = Call.getValue(1);

  unsigned IntRet = 0, FloatRetN = 0;
  for (const ISD::InputArg &In : CLI.Ins) {
    Register Reg;
    if (In.VT.isFloatingPoint())
      Reg = FloatRetN++ == 0 ? CBC::FR0 : CBC::FR1;
    else
      Reg = IntRet++ == 0 ? CBC::IR1 : CBC::IR2;
    SDValue V = DAG.getCopyFromReg(Chain, DL, Reg, In.VT, Glue);
    InVals.push_back(V);
    Chain = V.getValue(1);
    Glue = V.getValue(2);
  }
  return Chain;
}

void CBCTargetLowering::AdjustInstrPostInstrSelection(MachineInstr &MI,
                                                      SDNode *) const {
  if ((MI.getOpcode() != CBC::CALL && MI.getOpcode() != CBC::CALL_INDIRECT) ||
      MI.getNumOperands() < 2 || !MI.getOperand(1).isImm())
    return;
  unsigned N = MI.getOperand(1).getImm();
  static const MCPhysReg IRegs[] = {CBC::IR1, CBC::IR2, CBC::IR3,
                                    CBC::IR4, CBC::IR5, CBC::IR6};
  static const MCPhysReg FRegs[] = {CBC::FR0, CBC::FR1, CBC::FR2, CBC::FR3,
                                    CBC::FR4, CBC::FR5, CBC::FR6, CBC::FR7};
  unsigned IReg = 0, FReg = 0;
  for (unsigned I = 0; I < N && 2 + I < MI.getNumOperands(); ++I) {
    bool FP = MI.getOperand(2 + I).isImm() && MI.getOperand(2 + I).getImm();
    MCPhysReg R = FP ? FRegs[FReg++] : IRegs[IReg++];
    MI.addOperand(MachineOperand::CreateReg(R, /*isDef=*/false, /*isImp=*/true));
  }
  bool RetFloat = false;
  unsigned RetIdx = 2 + N;
  if (RetIdx < MI.getNumOperands() && MI.getOperand(RetIdx).isImm())
    RetFloat = MI.getOperand(RetIdx).getImm() != 0;
  MI.addOperand(MachineOperand::CreateReg(RetFloat ? CBC::FR0 : CBC::IR1,
                                          /*isDef=*/true, /*isImp=*/true));
  MI.addOperand(MachineOperand::CreateReg(CBC::IR2, /*isDef=*/true,
                                          /*isImp=*/true));
  MI.addOperand(MachineOperand::CreateReg(CBC::FR1, /*isDef=*/true,
                                          /*isImp=*/true));
}

SDValue CBCTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_CBC);
  for (unsigned I = 0; I != ArgLocs.size(); ++I) {
    CCValAssign &VA = ArgLocs[I];
    if (VA.isMemLoc()) {
      if (!MF.front().isLiveIn(CBC::IR7))
        MF.front().addLiveIn(CBC::IR7);
      unsigned Off = VA.getLocMemOffset();
      SDValue Ops[] = {DAG.getRegister(CBC::IR7, MVT::i64),
                       DAG.getTargetConstant(Off, DL, MVT::i64), Chain};
      SDNode *Ld =
          DAG.getMachineNode(CBC::LDRAW64, DL, MVT::i64, MVT::Other, Ops);
      SDValue Val = SDValue(Ld, 0);
      if (VA.getValVT() == MVT::i32)
        Val = DAG.getNode(ISD::TRUNCATE, DL, MVT::i32, Val);
      else if (VA.getValVT() == MVT::f64)
        Val = DAG.getNode(ISD::BITCAST, DL, MVT::f64, Val);
      else if (VA.getValVT() == MVT::f32) {
        Val = DAG.getNode(ISD::TRUNCATE, DL, MVT::i32, Val);
        Val = DAG.getNode(ISD::BITCAST, DL, MVT::f32, Val);
      }
      InVals.push_back(Val);
      Chain = SDValue(Ld, 1);
      continue;
    }
    const TargetRegisterClass *RC =
        VA.getLocVT().isFloatingPoint() ? &CBC::FPRRegClass : &CBC::GPRRegClass;
    Register VReg = MF.getRegInfo().createVirtualRegister(RC);
    MF.getRegInfo().addLiveIn(VA.getLocReg(), VReg);
    InVals.push_back(DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT()));
  }
  return Chain;
}

MachineBasicBlock *CBCTargetLowering::EmitInstrWithCustomInserter(
    MachineInstr &MI, MachineBasicBlock *MBB) const {
  unsigned Opc = MI.getOpcode();
  if (Opc != CBC::SELECT_GPR && Opc != CBC::SELECT_FPR &&
      Opc != CBC::SELECT_FPR_I)
    report_fatal_error("CBC custom inserter: unexpected instruction");
  bool ResFloat = Opc != CBC::SELECT_GPR;
  bool CmpFloat = Opc == CBC::SELECT_FPR;
  const TargetInstrInfo &TII = *MBB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  Register Dest = MI.getOperand(0).getReg();
  MachineFunction *MF = MBB->getParent();
  MachineBasicBlock *TBB = MF->CreateMachineBasicBlock(MBB->getBasicBlock());
  MachineBasicBlock *FBB = MF->CreateMachineBasicBlock(MBB->getBasicBlock());
  MachineBasicBlock *Join = MF->CreateMachineBasicBlock(MBB->getBasicBlock());
  MachineFunction::iterator It = ++MBB->getIterator();
  MF->insert(It, TBB);
  MF->insert(It, FBB);
  MF->insert(It, Join);
  Join->splice(Join->begin(), MBB, std::next(MI.getIterator()), MBB->end());
  Join->transferSuccessorsAndUpdatePHIs(MBB);
  MBB->addSuccessor(TBB);
  MBB->addSuccessor(FBB);
  TBB->addSuccessor(Join);
  FBB->addSuccessor(Join);
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetRegisterClass *RC =
      ResFloat ? &CBC::FPRRegClass : &CBC::GPRRegClass;
  Register TT = MRI.createVirtualRegister(RC);
  Register FF = MRI.createVirtualRegister(RC);
  unsigned MoveOpc = ResFloat ? CBC::FMOV64rr : CBC::MOV64rr;
  if (CmpFloat) {
    bool F32 = MI.getOperand(6).getImm() != 0;
    BuildMI(MBB, DL, TII.get(CBC::BCCF))
        .addImm(MI.getOperand(1).getImm())
        .addReg(MI.getOperand(2).getReg())
        .addReg(MI.getOperand(3).getReg())
        .addImm(F32 ? 1 : 0)
        .addMBB(TBB);
  } else {
    BuildMI(MBB, DL, TII.get(CBC::BCC64))
        .addImm(MI.getOperand(1).getImm())
        .addReg(MI.getOperand(2).getReg())
        .addReg(MI.getOperand(3).getReg())
        .addMBB(TBB);
  }
  BuildMI(MBB, DL, TII.get(CBC::JMP)).addMBB(FBB);
  BuildMI(TBB, DL, TII.get(MoveOpc), TT).addReg(MI.getOperand(4).getReg());
  BuildMI(TBB, DL, TII.get(CBC::JMP)).addMBB(Join);
  BuildMI(FBB, DL, TII.get(MoveOpc), FF).addReg(MI.getOperand(5).getReg());
  BuildMI(FBB, DL, TII.get(CBC::JMP)).addMBB(Join);
  BuildMI(*Join, Join->begin(), DL, TII.get(TargetOpcode::PHI), Dest)
      .addReg(TT)
      .addMBB(TBB)
      .addReg(FF)
      .addMBB(FBB);
  MI.eraseFromParent();
  return Join;
}

std::pair<unsigned, const TargetRegisterClass *>
CBCTargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                                StringRef Constraint,
                                                MVT VT) const {
  if (Constraint == "r") {
    if (VT == MVT::i64 || VT == MVT::i32 || VT == MVT::i16 || VT == MVT::i8 ||
        VT == MVT::i1)
      return std::make_pair(0U, &CBC::GPRRegClass);
  }
  if (Constraint == "f") {
    if (VT == MVT::f64 || VT == MVT::f32)
      return std::make_pair(0U, &CBC::FPRRegClass);
  }
  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

Register CBCTargetLowering::getExceptionPointerRegister(
    ExceptionHandling, const Constant *) const {
  return CBC::IRACC;
}

Register CBCTargetLowering::getExceptionSelectorRegister(
    ExceptionHandling, const Constant *) const {
  return CBC::IR12;
}

EVT CBCTargetLowering::getSetCCResultType(const DataLayout &,
                                          LLVMContext &Context, EVT VT) const {
  if (VT.isVector())
    return EVT::getVectorVT(Context, MVT::i64, VT.getVectorElementCount());
  return MVT::i64;
}

bool CBCTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_CBC);
}

static unsigned allocaAlignLog(uint64_t Align) {
  if (Align <= 16)
    return 4;
  if (Align > 4096 || !isPowerOf2_64(Align))
    report_fatal_error("CBC alloca alignment must be a power of two up to 4096");
  return Log2_64(Align);
}

SDValue CBCTargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  switch (Op.getOpcode()) {
  case ISD::DYNAMIC_STACKALLOC: {
    SDValue Size = Op.getOperand(1);
    unsigned Log = allocaAlignLog(
        cast<ConstantSDNode>(Op.getOperand(2))->getZExtValue());
    SDValue AllocOps[] = {Size, DAG.getTargetConstant(Log, DL, MVT::i32),
                          Op.getOperand(0)};
    return SDValue(DAG.getMachineNode(CBC::ALLOCA, DL, MVT::i64, MVT::Other,
                                      AllocOps),
                   0);
  }
  case ISD::STACKSAVE: {
    SDValue SaveOps[] = {Op.getOperand(0)};
    return SDValue(DAG.getMachineNode(CBC::STACKSAVE, DL, MVT::i64, MVT::Other,
                                      SaveOps),
                   0);
  }
  case ISD::STACKRESTORE:
    return SDValue(DAG.getMachineNode(CBC::STACKRESTORE, DL, MVT::Other,
                                      Op.getOperand(1), Op.getOperand(0)),
                   0);
  case ISD::FRAMEADDR:
    return DAG.getConstant(0, DL, MVT::i64);
  case ISD::SHL_PARTS:
  case ISD::SRL_PARTS:
  case ISD::SRA_PARTS: {
    SDValue Lo, Hi;
    expandShiftParts(Op.getNode(), Lo, Hi, DAG);
    return DAG.getMergeValues({Lo, Hi}, SDLoc(Op));
  }
  default:
    break;
  }
  if (Op.getOpcode() != ISD::GlobalAddress)
    return SDValue();
  auto *G = cast<GlobalAddressSDNode>(Op);
  if (G->getOffset() != 0)
    report_fatal_error("CBC function address offsets are not implemented yet");
  const GlobalValue *GV = G->getGlobal();
  // Itanium complete-object destructors are aliases to the base destructor.
  // CBCLowerGlobals only rewrites GlobalVariables, so the alias reaches isel.
  if (const auto *Alias = dyn_cast<GlobalAlias>(GV)) {
    if (const GlobalObject *Obj = Alias->getAliaseeObject())
      if (isa<Function>(Obj))
        GV = Obj;
  }
  if (!isa<Function>(GV))
    report_fatal_error("CBC data addresses must be lowered before instruction selection");
  SDValue TGA = DAG.getTargetGlobalAddress(GV, DL, MVT::i64);
  return SDValue(DAG.getMachineNode(CBC::LD_FNPTR, DL, MVT::i64, TGA), 0);
}
