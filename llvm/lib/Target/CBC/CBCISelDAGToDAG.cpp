#include "CBC.h"
#include "CBCTargetMachine.h"
#include "llvm/CodeGen/FunctionLoweringInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// Integer codes 0-5. Float codes 8-13 (feq/fne/flt/fnlt/fge/fnge). SETULT and
// SETUGE mean unsigned-integer or unordered-float depending on the operands.
static bool cbcMapCond(ISD::CondCode CC, bool IsFloat, unsigned &Code,
                       bool &Swap) {
  Swap = false;
  if (!IsFloat) {
    switch (CC) {
    case ISD::SETEQ: Code = 0; return true;
    case ISD::SETNE: Code = 1; return true;
    case ISD::SETLT: Code = 2; return true;
    case ISD::SETGE: Code = 3; return true;
    case ISD::SETULT: Code = 4; return true;
    case ISD::SETUGE: Code = 5; return true;
    case ISD::SETGT: Code = 2; Swap = true; return true;
    case ISD::SETLE: Code = 3; Swap = true; return true;
    case ISD::SETUGT: Code = 4; Swap = true; return true;
    case ISD::SETULE: Code = 5; Swap = true; return true;
    default: return false;
    }
  }
  switch (CC) {
  case ISD::SETOEQ:
  case ISD::SETEQ: Code = 8; return true;
  case ISD::SETUNE:
  case ISD::SETNE: Code = 9; return true;
  case ISD::SETOLT: Code = 10; return true;
  case ISD::SETOGT: Code = 10; Swap = true; return true;
  case ISD::SETOGE: Code = 12; return true;
  case ISD::SETOLE: Code = 12; Swap = true; return true;
  case ISD::SETUGE: Code = 11; return true;
  case ISD::SETULE: Code = 11; Swap = true; return true;
  case ISD::SETULT: Code = 13; return true;
  case ISD::SETUGT: Code = 13; Swap = true; return true;
  default: return false;
  }
}

static SDValue cbcScc(SelectionDAG &DAG, const SDLoc &DL, unsigned Code,
                      SDValue L, SDValue R) {
  bool F32 = L.getValueType() == MVT::f32;
  SDValue Ops[] = {DAG.getTargetConstant(Code, DL, MVT::i32), L, R,
                   DAG.getTargetConstant(F32 ? 1 : 0, DL, MVT::i32)};
  return SDValue(DAG.getMachineNode(CBC::SCCF, DL, MVT::i64, Ops), 0);
}

static SDValue cbcOr(SelectionDAG &DAG, const SDLoc &DL, SDValue A, SDValue B) {
  return SDValue(DAG.getMachineNode(CBC::OR64rr, DL, MVT::i64, A, B), 0);
}

static SDValue cbcNot(SelectionDAG &DAG, const SDLoc &DL, SDValue A) {
  SDValue One = SDValue(
      DAG.getMachineNode(CBC::MOV64ri, DL, MVT::i64,
                         DAG.getTargetConstant(1, DL, MVT::i64)),
      0);
  return SDValue(DAG.getMachineNode(CBC::XOR64rr, DL, MVT::i64, A, One), 0);
}

// Ordered/unordered conditions that are not a single CBC code.
static SDValue cbcMaterializeCond(SelectionDAG &DAG, const SDLoc &DL,
                                  ISD::CondCode CC, SDValue L, SDValue R) {
  if (CC == ISD::SETONE || CC == ISD::SETUEQ) {
    SDValue Lt = cbcScc(DAG, DL, 10, L, R);
    SDValue Gt = cbcScc(DAG, DL, 10, R, L);
    SDValue One = cbcOr(DAG, DL, Lt, Gt);
    return CC == ISD::SETONE ? One : cbcNot(DAG, DL, One);
  }
  if (CC == ISD::SETUO || CC == ISD::SETO) {
    SDValue NaN = cbcOr(DAG, DL, cbcScc(DAG, DL, 9, L, L), cbcScc(DAG, DL, 9, R, R));
    return CC == ISD::SETUO ? NaN : cbcNot(DAG, DL, NaN);
  }
  report_fatal_error("CBC unsupported compare");
}

static bool cbcSignedCond(ISD::CondCode CC) {
  switch (CC) {
  case ISD::SETEQ:
  case ISD::SETNE:
  case ISD::SETLT:
  case ISD::SETGE:
  case ISD::SETGT:
  case ISD::SETLE:
    return true;
  default:
    return false;
  }
}

// Native i32 results only define the low 32 bits (the high half of rax is
// zeroed, so -1 arrives as 0xffffffff). Widen before a 64-bit compare.
static SDValue cbcExtendToI64(SelectionDAG &DAG, const SDLoc &DL, SDValue V,
                              bool Signed) {
  // A negative i32 constant must be zero-extended for an unsigned compare.
  // Sign-extending i32 -11 to i64 -11 makes ult(0xFFFFFFF5, zext(x)) false.
  if (auto *C = dyn_cast<ConstantSDNode>(V)) {
    int64_t SExt = C->getSExtValue();
    uint64_t Bits = Signed ? static_cast<uint64_t>(SExt) : C->getZExtValue();
    if (Bits == 0)
      return DAG.getRegister(CBC::IRZ, MVT::i64);
    return SDValue(DAG.getMachineNode(CBC::MOV64ri, DL, MVT::i64,
                                      DAG.getTargetConstant(Bits, DL, MVT::i64)),
                   0);
  }
  EVT VT = V.getValueType();
  if (VT == MVT::i64)
    return V;
  if (VT != MVT::i8 && VT != MVT::i16 && VT != MVT::i32)
    return V;
  SDValue Sh = DAG.getTargetConstant(64 - VT.getSizeInBits(), DL, MVT::i64);
  SDNode *Shl = DAG.getMachineNode(CBC::SHL64ri, DL, MVT::i64, V, Sh);
  unsigned Opc = Signed ? CBC::SRA64ri : CBC::SRL64ri;
  return SDValue(DAG.getMachineNode(Opc, DL, MVT::i64, SDValue(Shl, 0), Sh), 0);
}

#define DEBUG_TYPE "cbc-isel"
#define PASS_NAME "CBC DAG->DAG Pattern Instruction Selection"

namespace {
class CBCDAGToDAGISel : public SelectionDAGISel {
public:
  explicit CBCDAGToDAGISel(CBCTargetMachine &TM) : SelectionDAGISel(TM) {}
#include "CBCGenDAGISel.inc"
  void Select(SDNode *N) override {
    if (N->isMachineOpcode()) {
      N->setNodeId(-1);
      return;
    }
    SDLoc DL(N);
    switch (N->getOpcode()) {
    case ISD::EntryToken:
    case ISD::TokenFactor:
    case ISD::CopyFromReg:
    case ISD::CopyToReg:
    case ISD::Register:
    case ISD::TargetConstant:
    case ISD::TargetConstantFP:
    case ISD::TargetGlobalAddress:
    case ISD::TargetExternalSymbol:
    case ISD::MERGE_VALUES:
    case ISD::BasicBlock:
    case ISD::CONDCODE:
    case ISD::RegisterMask:
    case ISD::HANDLENODE:
    case ISD::TargetFrameIndex:
    case ISD::LIFETIME_START:
    case ISD::LIFETIME_END:
    case ISD::VASTART:
    case ISD::VAEND:
    case ISD::VACOPY:
    case ISD::PSEUDO_PROBE:
    case ISD::EH_LABEL:
    case ISD::ANNOTATION_LABEL:
      N->setNodeId(-1);
      return;
    case ISD::INLINEASM:
    case ISD::INLINEASM_BR: {
      // CBC has no native asm. Elide only empty asm with no constraint
      // operands (clang's compiler barrier: asm volatile("" ::: "memory")).
      // Anything else is unsupported.
      auto *Sym = dyn_cast<ExternalSymbolSDNode>(
          N->getOperand(InlineAsm::Op_AsmString));
      bool EmptyAsm = Sym && Sym->getSymbol() && Sym->getSymbol()[0] == '\0';
      bool NoConstraints =
          N->getNumOperands() == InlineAsm::Op_FirstOperand;
      if (N->getOpcode() == ISD::INLINEASM && EmptyAsm && NoConstraints) {
        ReplaceUses(SDValue(N, 0), N->getOperand(InlineAsm::Op_InputChain));
        if (N->getNumValues() > 1 && !SDValue(N, 1).use_empty())
          report_fatal_error(
              "CBC empty inline asm barrier produced a used glue result");
        CurDAG->RemoveDeadNode(N);
        return;
      }
      report_fatal_error("CBC does not support inline assembly");
    }
    case ISD::UNDEF: {
      SDValue Zero = CurDAG->getTargetConstant(0, DL, MVT::i64);
      unsigned Opc =
          N->getValueType(0).isFloatingPoint() ? CBC::FMOV64i : CBC::MOV64ri;
      if (N->getValueType(0) == MVT::f32)
        Opc = CBC::FMOV32i;
      CurDAG->SelectNodeTo(N, Opc, N->getValueType(0), Zero);
      return;
    }
    case ISD::POISON: {
      SDValue Zero = CurDAG->getTargetConstant(0, DL, MVT::i64);
      unsigned Opc =
          N->getValueType(0).isFloatingPoint() ? CBC::FMOV64i : CBC::MOV64ri;
      if (N->getValueType(0) == MVT::f32)
        Opc = CBC::FMOV32i;
      CurDAG->SelectNodeTo(N, Opc, N->getValueType(0), Zero);
      return;
    }
    case ISD::Constant: {
      auto *C = cast<ConstantSDNode>(N);
      SDValue Imm =
          CurDAG->getTargetConstant(C->getSExtValue(), DL, MVT::i64);
      CurDAG->SelectNodeTo(N, CBC::MOV64ri, N->getValueType(0), Imm);
      return;
    }
    case ISD::ConstantFP: {
      const APFloat &F = cast<ConstantFPSDNode>(N)->getValueAPF();
      uint64_t Bits = F.bitcastToAPInt().getLimitedValue();
      unsigned Opc = N->getValueType(0) == MVT::f32 ? CBC::FMOV32i : CBC::FMOV64i;
      CurDAG->SelectNodeTo(N, Opc, N->getValueType(0),
                           CurDAG->getTargetConstant(Bits, DL, MVT::i64));
      return;
    }
    case ISD::SUB:
    case ISD::AND:
    case ISD::OR:
    case ISD::XOR:
    case ISD::MUL:
    case ISD::SHL:
    case ISD::SRL:
    case ISD::SRA:
    case ISD::ADD: {
      EVT VT = N->getValueType(0);
      bool W32 = VT == MVT::i32;
      SDValue L = N->getOperand(0), R = N->getOperand(1);
      unsigned OpcRR = W32 ? CBC::ADD32rr : CBC::ADD64rr;
      unsigned OpcRI = W32 ? CBC::ADD32ri : CBC::ADD64ri;
      bool ImmOK = N->getOpcode() == ISD::ADD || N->getOpcode() == ISD::AND ||
                   N->getOpcode() == ISD::MUL || N->getOpcode() == ISD::SHL ||
                   N->getOpcode() == ISD::SRL || N->getOpcode() == ISD::SRA;
      if (N->getOpcode() == ISD::SUB)
        OpcRR = W32 ? CBC::SUB32rr : CBC::SUB64rr;
      else if (N->getOpcode() == ISD::AND) {
        OpcRR = W32 ? CBC::AND32rr : CBC::AND64rr;
        OpcRI = W32 ? CBC::AND32ri : CBC::AND64ri;
      } else if (N->getOpcode() == ISD::OR)
        OpcRR = W32 ? CBC::OR32rr : CBC::OR64rr;
      else if (N->getOpcode() == ISD::XOR)
        OpcRR = W32 ? CBC::XOR32rr : CBC::XOR64rr;
      else if (N->getOpcode() == ISD::MUL) {
        OpcRR = W32 ? CBC::MUL32rr : CBC::MUL64rr;
        OpcRI = W32 ? CBC::MUL32ri : CBC::MUL64ri;
      } else if (N->getOpcode() == ISD::SHL) {
        OpcRR = W32 ? CBC::SHL32rr : CBC::SHL64rr;
        OpcRI = W32 ? CBC::SHL32ri : CBC::SHL64ri;
      } else if (N->getOpcode() == ISD::SRL) {
        OpcRR = W32 ? CBC::SRL32rr : CBC::SRL64rr;
        OpcRI = W32 ? CBC::SRL32ri : CBC::SRL64ri;
      } else if (N->getOpcode() == ISD::SRA) {
        OpcRR = W32 ? CBC::SRA32rr : CBC::SRA64rr;
        OpcRI = W32 ? CBC::SRA32ri : CBC::SRA64ri;
      }
      if (OpcRI && isa<ConstantSDNode>(R) && ImmOK) {
        auto *C = cast<ConstantSDNode>(R);
        CurDAG->SelectNodeTo(
            N, OpcRI, VT, L,
            CurDAG->getTargetConstant(C->getSExtValue(), DL, MVT::i64));
        return;
      }
      if (N->getOpcode() == ISD::ADD && isa<ConstantSDNode>(L)) {
        auto *C = cast<ConstantSDNode>(L);
        CurDAG->SelectNodeTo(
            N, OpcRI, VT, R,
            CurDAG->getTargetConstant(C->getSExtValue(), DL, MVT::i64));
        return;
      }
      CurDAG->SelectNodeTo(N, OpcRR, VT, L, R);
      return;
    }
    case ISD::FrameIndex: {
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      MachineFrameInfo &MFI = CurDAG->getMachineFunction().getFrameInfo();
      Align ObjAlign = MFI.getObjectAlign(FI);
      SDValue Addr(
          CurDAG->getMachineNode(CBC::LEA_FRAME, DL, MVT::i64,
                                 CurDAG->getTargetFrameIndex(FI, MVT::i64)),
          0);
      if (ObjAlign.value() <= 16) {
        ReplaceNode(N, Addr.getNode());
        return;
      }
      uint64_t Mask = ObjAlign.value() - 1;
      SDNode *Added = CurDAG->getMachineNode(
          CBC::ADD64ri, DL, MVT::i64, Addr,
          CurDAG->getTargetConstant(Mask, DL, MVT::i64));
      SDNode *Aligned = CurDAG->getMachineNode(
          CBC::AND64ri, DL, MVT::i64, SDValue(Added, 0),
          CurDAG->getTargetConstant(~Mask, DL, MVT::i64));
      ReplaceNode(N, Aligned);
      return;
    }
    case ISD::CALLSEQ_START:
    case ISD::CALLSEQ_END:
      // Dynamic alloca uses these as scheduling barriers. CBC has no call
      // frame, so the chain is the only result that matters.
      ReplaceUses(SDValue(N, 0), N->getOperand(0));
      CurDAG->RemoveDeadNode(N);
      return;
    case ISD::BR: {
      auto *BB = cast<BasicBlockSDNode>(N->getOperand(1));
      MachineBasicBlock *Tgt = BB->getBasicBlock();
      SDValue Ops[] = {CurDAG->getBasicBlock(Tgt), N->getOperand(0)};
      CurDAG->SelectNodeTo(N, CBC::JMP, MVT::Other, Ops);
      return;
    }
    case ISD::BR_CC: {
      ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(1))->get();
      SDValue L = N->getOperand(2), R = N->getOperand(3);
      bool IsFloat = L.getValueType().isFloatingPoint();
      unsigned Code = 0;
      bool Swap = false;
      auto *BB = cast<BasicBlockSDNode>(N->getOperand(4));
      MachineBasicBlock *Tgt = BB->getBasicBlock();
      if (!cbcMapCond(CC, IsFloat, Code, Swap)) {
        SDValue Flag = cbcMaterializeCond(*CurDAG, DL, CC, L, R);
        SDValue Zero = CurDAG->getRegister(CBC::IRZ, MVT::i64);
        SDValue Ops[] = {CurDAG->getTargetConstant(1, DL, MVT::i32), Flag, Zero,
                         CurDAG->getBasicBlock(Tgt), N->getOperand(0)};
        CurDAG->SelectNodeTo(N, CBC::BCC64, MVT::Other, Ops);
        return;
      }
      if (Swap)
        std::swap(L, R);
      if (IsFloat) {
        bool F32 = L.getValueType() == MVT::f32;
        SDValue Ops[] = {CurDAG->getTargetConstant(Code, DL, MVT::i32), L, R,
                         CurDAG->getTargetConstant(F32 ? 1 : 0, DL, MVT::i32),
                         CurDAG->getBasicBlock(Tgt), N->getOperand(0)};
        CurDAG->SelectNodeTo(N, CBC::BCCF, MVT::Other, Ops);
        return;
      }
      bool Signed = cbcSignedCond(CC);
      L = cbcExtendToI64(*CurDAG, DL, L, Signed);
      R = cbcExtendToI64(*CurDAG, DL, R, Signed);
      SDValue Ops[] = {CurDAG->getTargetConstant(Code, DL, MVT::i32), L, R,
                       CurDAG->getBasicBlock(Tgt), N->getOperand(0)};
      CurDAG->SelectNodeTo(N, CBC::BCC64, MVT::Other, Ops);
      return;
    }
    case ISD::SELECT_CC: {
      ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(4))->get();
      SDValue CmpL = N->getOperand(0), CmpR = N->getOperand(1);
      bool CmpFloat = CmpL.getValueType().isFloatingPoint();
      bool ResFloat = N->getValueType(0).isFloatingPoint();
      unsigned Code = 0;
      bool Swap = false;
      auto AsInt = [&](SDValue V) -> SDValue {
        if (auto *C = dyn_cast<ConstantSDNode>(V)) {
          if (C->isZero())
            return CurDAG->getRegister(CBC::IRZ, MVT::i64);
          return SDValue(
              CurDAG->getMachineNode(
                  CBC::MOV64ri, DL, MVT::i64,
                  CurDAG->getTargetConstant(C->getSExtValue(), DL, MVT::i64)),
              0);
        }
        return V;
      };
      SDValue T = N->getOperand(2);
      SDValue F = N->getOperand(3);
      if (!ResFloat) {
        T = AsInt(T);
        F = AsInt(F);
      }
      if (!cbcMapCond(CC, CmpFloat, Code, Swap)) {
        SDValue Flag = cbcMaterializeCond(*CurDAG, DL, CC, CmpL, CmpR);
        SDValue Zero = CurDAG->getRegister(CBC::IRZ, MVT::i64);
        if (ResFloat) {
          SDValue Ops[] = {CurDAG->getTargetConstant(1, DL, MVT::i32), Flag,
                           Zero, T, F};
          CurDAG->SelectNodeTo(N, CBC::SELECT_FPR_I, N->getValueType(0), Ops);
        } else {
          SDValue Ops[] = {CurDAG->getTargetConstant(1, DL, MVT::i32), Flag,
                           Zero, T, F};
          CurDAG->SelectNodeTo(N, CBC::SELECT_GPR, N->getValueType(0), Ops);
        }
        return;
      }
      if (Swap)
        std::swap(CmpL, CmpR);
      if (CmpFloat && !ResFloat) {
        // Integer select on a float predicate: materialize 0/1 then branch on NE.
        SDValue Flag = cbcScc(*CurDAG, DL, Code, CmpL, CmpR);
        SDValue Zero = CurDAG->getRegister(CBC::IRZ, MVT::i64);
        SDValue Ops[] = {CurDAG->getTargetConstant(1, DL, MVT::i32), Flag, Zero,
                         T, F};
        CurDAG->SelectNodeTo(N, CBC::SELECT_GPR, N->getValueType(0), Ops);
        return;
      }
      if (!CmpFloat) {
        bool Signed = cbcSignedCond(CC);
        // A narrow unsigned constant must be zero-extended. Sign-extending
        // i32 -24 and then comparing it as i64 makes ult true for every
        // zero-extended 32-bit value.
        auto materializeCmp = [&](SDValue V) -> SDValue {
          if (auto *C = dyn_cast<ConstantSDNode>(V)) {
            unsigned Width = C->getValueType(0).getSizeInBits();
            uint64_t SBits = static_cast<uint64_t>(C->getSExtValue());
            uint64_t ZBits = C->getZExtValue();
            uint64_t Bits = (!Signed && Width < 64) ? ZBits : SBits;
            if (Bits == 0)
              return CurDAG->getRegister(CBC::IRZ, MVT::i64);
            return SDValue(
                CurDAG->getMachineNode(
                    CBC::MOV64ri, DL, MVT::i64,
                    CurDAG->getTargetConstant(Bits, DL, MVT::i64)),
                0);
          }
          return cbcExtendToI64(*CurDAG, DL, V, Signed);
        };
        CmpL = materializeCmp(CmpL);
        CmpR = materializeCmp(CmpR);
      }
      SDValue CCImm = CurDAG->getTargetConstant(Code, DL, MVT::i32);
      if (ResFloat && CmpFloat) {
        bool F32 = CmpL.getValueType() == MVT::f32;
        SDValue Ops[] = {CCImm, CmpL, CmpR, T, F,
                         CurDAG->getTargetConstant(F32 ? 1 : 0, DL, MVT::i32)};
        CurDAG->SelectNodeTo(N, CBC::SELECT_FPR, N->getValueType(0), Ops);
      } else if (ResFloat) {
        SDValue Ops[] = {CCImm, CmpL, CmpR, T, F};
        CurDAG->SelectNodeTo(N, CBC::SELECT_FPR_I, N->getValueType(0), Ops);
      } else {
        SDValue Ops[] = {CCImm, CmpL, CmpR, T, F};
        CurDAG->SelectNodeTo(N, CBC::SELECT_GPR, N->getValueType(0), Ops);
      }
      return;
    }
    case ISD::SETCC: {
      ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(2))->get();
      SDValue L = N->getOperand(0), R = N->getOperand(1);
      bool IsFloat = L.getValueType().isFloatingPoint();
      unsigned Code = 0;
      bool Swap = false;
      if (!cbcMapCond(CC, IsFloat, Code, Swap)) {
        SDValue Flag = cbcMaterializeCond(*CurDAG, DL, CC, L, R);
        ReplaceUses(SDValue(N, 0), Flag);
        CurDAG->RemoveDeadNode(N);
        return;
      }
      if (Swap)
        std::swap(L, R);
      if (IsFloat) {
        bool F32 = L.getValueType() == MVT::f32;
        SDValue Ops[] = {CurDAG->getTargetConstant(Code, DL, MVT::i32), L, R,
                         CurDAG->getTargetConstant(F32 ? 1 : 0, DL, MVT::i32)};
        CurDAG->SelectNodeTo(N, CBC::SCCF, N->getValueType(0), Ops);
        return;
      }
      bool Signed = cbcSignedCond(CC);
      L = cbcExtendToI64(*CurDAG, DL, L, Signed);
      R = cbcExtendToI64(*CurDAG, DL, R, Signed);
      CurDAG->SelectNodeTo(N, CBC::SCC64, N->getValueType(0),
                           CurDAG->getTargetConstant(Code, DL, MVT::i32), L, R);
      return;
    }
    case ISD::ATOMIC_LOAD: {
      auto *AN = cast<AtomicSDNode>(N);
      MVT MemVT = AN->getMemoryVT().getSimpleVT();
      unsigned Opc = MemVT == MVT::i8    ? CBC::LDRAW8
                     : MemVT == MVT::i32 ? CBC::LDRAW32
                                         : CBC::LDRAW64;
      SDValue Ops[] = {AN->getBasePtr(),
                       CurDAG->getTargetConstant(0, DL, MVT::i64), AN->getChain()};
      CurDAG->SelectNodeTo(N, Opc, N->getValueType(0), MVT::Other, Ops);
      return;
    }
    case ISD::ATOMIC_STORE: {
      auto *AN = cast<AtomicSDNode>(N);
      MVT MemVT = AN->getMemoryVT().getSimpleVT();
      unsigned Opc = MemVT == MVT::i8    ? CBC::STRAW8
                     : MemVT == MVT::i32 ? CBC::STRAW32
                                         : CBC::STRAW64;
      SDValue Ops[] = {AN->getVal(), AN->getBasePtr(),
                       CurDAG->getTargetConstant(0, DL, MVT::i64), AN->getChain()};
      CurDAG->SelectNodeTo(N, Opc, MVT::Other, Ops);
      return;
    }
    case ISD::LOAD: {
      auto *Ld = cast<LoadSDNode>(N);
      if (Ld->isIndexed() || Ld->getAddressingMode() != ISD::UNINDEXED)
        break;
      unsigned Opc = CBC::LDRAW64;
      if (Ld->getMemoryVT() == MVT::i8 || Ld->getMemoryVT() == MVT::i1) {
        Opc = CBC::LDRAW8;
        // LDRAW8 zero-extends. A SEXTLOAD i8→i32 (e.g. (zext (sext i8 to
        // i16)) after type legalization) must sign-extend or AND 0xFFFF keeps
        // 0x00FF instead of 0xFFFF.
        if (Ld->getExtensionType() == ISD::SEXTLOAD) {
          unsigned Width =
              Ld->getMemoryVT() == MVT::i1 ? 1 : 8;
          SDValue Ops[] = {Ld->getBasePtr(),
                           CurDAG->getTargetConstant(0, DL, MVT::i64),
                           Ld->getChain()};
          SDNode *Raw = CurDAG->getMachineNode(CBC::LDRAW8, DL, MVT::i64,
                                               MVT::Other, Ops);
          SDValue Sh = CurDAG->getTargetConstant(64 - Width, DL, MVT::i64);
          SDNode *Shl = CurDAG->getMachineNode(CBC::SHL64ri, DL, MVT::i64,
                                               SDValue(Raw, 0), Sh);
          SDNode *Sra = CurDAG->getMachineNode(CBC::SRA64ri, DL, MVT::i64,
                                               SDValue(Shl, 0), Sh);
          ReplaceUses(SDValue(N, 0), SDValue(Sra, 0));
          ReplaceUses(SDValue(N, 1), SDValue(Raw, 1));
          CurDAG->RemoveDeadNode(N);
          return;
        }
      } else if (Ld->getMemoryVT() == MVT::i16) {
        SDValue Base = Ld->getBasePtr();
        SDValue Chain = Ld->getChain();
        SDValue Disp = CurDAG->getTargetConstant(0, DL, MVT::i64);
        SDValue One = CurDAG->getTargetConstant(1, DL, MVT::i64);
        SDNode *PtrHi =
            CurDAG->getMachineNode(CBC::ADD64ri, DL, MVT::i64, Base, One);
        SDValue LoOps[] = {Base, Disp, Chain};
        SDNode *Lo = CurDAG->getMachineNode(CBC::LDRAW8, DL, MVT::i64, MVT::Other,
                                            LoOps);
        SDValue HiOps[] = {SDValue(PtrHi, 0), Disp, SDValue(Lo, 1)};
        SDNode *Hi = CurDAG->getMachineNode(CBC::LDRAW8, DL, MVT::i64, MVT::Other,
                                            HiOps);
        SDValue Mask = CurDAG->getTargetConstant(0xFF, DL, MVT::i64);
        SDNode *LoM = CurDAG->getMachineNode(CBC::AND64ri, DL, MVT::i64,
                                             SDValue(Lo, 0), Mask);
        SDNode *HiM = CurDAG->getMachineNode(CBC::AND64ri, DL, MVT::i64,
                                             SDValue(Hi, 0), Mask);
        SDValue Sh8 = CurDAG->getTargetConstant(8, DL, MVT::i64);
        SDNode *HiSh = CurDAG->getMachineNode(CBC::SHL64ri, DL, MVT::i64,
                                              SDValue(HiM, 0), Sh8);
        SDNode *Or = CurDAG->getMachineNode(CBC::OR64rr, DL, MVT::i64,
                                            SDValue(LoM, 0), SDValue(HiSh, 0));
        SDValue Val = SDValue(Or, 0);
        if (Ld->getExtensionType() == ISD::SEXTLOAD) {
          SDValue Sh48 = CurDAG->getTargetConstant(48, DL, MVT::i64);
          SDNode *S = CurDAG->getMachineNode(CBC::SHL64ri, DL, MVT::i64, Val, Sh48);
          SDNode *A = CurDAG->getMachineNode(CBC::SRA64ri, DL, MVT::i64,
                                             SDValue(S, 0), Sh48);
          Val = SDValue(A, 0);
        }
        ReplaceUses(SDValue(N, 0), Val);
        ReplaceUses(SDValue(N, 1), SDValue(Hi, 1));
        CurDAG->RemoveDeadNode(N);
        return;
      } else if (Ld->getMemoryVT() == MVT::i32) {
        Opc = CBC::LDRAW32;
        if (Ld->getExtensionType() == ISD::SEXTLOAD &&
            Ld->getValueType(0) == MVT::i64) {
          SDValue Ops[] = {Ld->getBasePtr(),
                           CurDAG->getTargetConstant(0, DL, MVT::i64),
                           Ld->getChain()};
          SDNode *Raw = CurDAG->getMachineNode(CBC::LDRAW32, DL, MVT::i64,
                                               MVT::Other, Ops);
          SDValue Sh = CurDAG->getTargetConstant(32, DL, MVT::i64);
          SDNode *Shl = CurDAG->getMachineNode(CBC::SHL64ri, DL, MVT::i64,
                                               SDValue(Raw, 0), Sh);
          SDNode *Sra = CurDAG->getMachineNode(CBC::SRA64ri, DL, MVT::i64,
                                               SDValue(Shl, 0), Sh);
          ReplaceUses(SDValue(N, 0), SDValue(Sra, 0));
          ReplaceUses(SDValue(N, 1), SDValue(Raw, 1));
          CurDAG->RemoveDeadNode(N);
          return;
        }
      } else if (Ld->getMemoryVT() == MVT::f32) {
        SDValue Ops[] = {Ld->getBasePtr(),
                         CurDAG->getTargetConstant(0, DL, MVT::i64),
                         Ld->getChain()};
        SDNode *Raw = CurDAG->getMachineNode(CBC::LDRAWF32, DL, MVT::f32,
                                             MVT::Other, Ops);
        SDValue Val = SDValue(Raw, 0);
        if (Ld->getValueType(0) == MVT::f64) {
          SDNode *Cvt =
              CurDAG->getMachineNode(CBC::CVT_F32_F64, DL, MVT::f64, Val);
          Val = SDValue(Cvt, 0);
        }
        ReplaceUses(SDValue(N, 0), Val);
        ReplaceUses(SDValue(N, 1), SDValue(Raw, 1));
        CurDAG->RemoveDeadNode(N);
        return;
      } else if (Ld->getMemoryVT() == MVT::f64)
        Opc = CBC::LDRAWF64;
      else if (Ld->getMemoryVT() != MVT::i64)
        break;
      SDValue Ops[] = {Ld->getBasePtr(),
                       CurDAG->getTargetConstant(0, DL, MVT::i64),
                       Ld->getChain()};
      CurDAG->SelectNodeTo(N, Opc, Ld->getValueType(0), MVT::Other, Ops);
      return;
    }
    case ISD::STORE: {
      auto *St = cast<StoreSDNode>(N);
      if (St->isIndexed() || St->getAddressingMode() != ISD::UNINDEXED)
        break;
      unsigned Opc = CBC::STRAW64;
      if (St->getMemoryVT() == MVT::i8 || St->getMemoryVT() == MVT::i1)
        Opc = CBC::STRAW8;
      else if (St->getMemoryVT() == MVT::i16) {
        SDValue Val = St->getValue();
        SDValue Base = St->getBasePtr();
        SDValue Chain = St->getChain();
        SDValue Sh = CurDAG->getTargetConstant(8, DL, MVT::i64);
        SDNode *Hi = CurDAG->getMachineNode(CBC::SRL64ri, DL, MVT::i64, Val, Sh);
        SDValue One = CurDAG->getTargetConstant(1, DL, MVT::i64);
        SDNode *PtrHi =
            CurDAG->getMachineNode(CBC::ADD64ri, DL, MVT::i64, Base, One);
        SDValue Disp = CurDAG->getTargetConstant(0, DL, MVT::i64);
        SDValue LoOps[] = {Val, Base, Disp, Chain};
        SDNode *LoSt =
            CurDAG->getMachineNode(CBC::STRAW8, DL, MVT::Other, LoOps);
        SDValue HiOps[] = {SDValue(Hi, 0), SDValue(PtrHi, 0), Disp,
                           SDValue(LoSt, 0)};
        SDNode *HiSt =
            CurDAG->getMachineNode(CBC::STRAW8, DL, MVT::Other, HiOps);
        ReplaceUses(SDValue(N, 0), SDValue(HiSt, 0));
        CurDAG->RemoveDeadNode(N);
        return;
      } else if (St->getMemoryVT() == MVT::i32)
        Opc = CBC::STRAW32;
      else if (St->getMemoryVT() == MVT::f32)
        Opc = CBC::STRAWF32;
      else if (St->getMemoryVT() == MVT::f64)
        Opc = CBC::STRAWF64;
      else if (St->getMemoryVT() != MVT::i64)
        break;
      SDValue Ops[] = {St->getValue(), St->getBasePtr(),
                       CurDAG->getTargetConstant(0, DL, MVT::i64),
                       St->getChain()};
      CurDAG->SelectNodeTo(N, Opc, MVT::Other, Ops);
      return;
    }
    case ISD::FREEZE: {
      unsigned Opc = N->getValueType(0).isFloatingPoint() ? CBC::FMOV64rr
                                                          : CBC::MOV64rr;
      CurDAG->SelectNodeTo(N, Opc, N->getValueType(0), N->getOperand(0));
      return;
    }
    case ISD::TRUNCATE:
    case ISD::ANY_EXTEND:
    case ISD::AssertSext:
    case ISD::AssertZext:
      CurDAG->SelectNodeTo(N, CBC::MOV64rr, N->getValueType(0),
                           N->getOperand(0));
      return;
    case ISD::SIGN_EXTEND_INREG: {
      unsigned Width =
          cast<VTSDNode>(N->getOperand(1))->getVT().getSizeInBits().getFixedValue();
      if (Width == 0 || Width >= 64) {
        CurDAG->SelectNodeTo(N, CBC::MOV64rr, N->getValueType(0),
                             N->getOperand(0));
        return;
      }
      SDValue Sh = CurDAG->getTargetConstant(64 - Width, DL, MVT::i64);
      SDNode *Shl = CurDAG->getMachineNode(CBC::SHL64ri, DL, MVT::i64,
                                           N->getOperand(0), Sh);
      SDNode *Sra = CurDAG->getMachineNode(CBC::SRA64ri, DL, N->getValueType(0),
                                           SDValue(Shl, 0), Sh);
      ReplaceUses(SDValue(N, 0), SDValue(Sra, 0));
      CurDAG->RemoveDeadNode(N);
      return;
    }
    case ISD::SIGN_EXTEND:
    case ISD::ZERO_EXTEND: {
      EVT InVT = N->getOperand(0).getValueType();
      unsigned Width = InVT.getSizeInBits();
      unsigned OutWidth = N->getValueType(0).getSizeInBits();
      if (Width == 0 || Width >= 64 || Width >= OutWidth) {
        CurDAG->SelectNodeTo(N, CBC::MOV64rr, N->getValueType(0),
                             N->getOperand(0));
        return;
      }
      SDValue Sh = CurDAG->getTargetConstant(64 - Width, DL, MVT::i64);
      SDNode *Shl = CurDAG->getMachineNode(CBC::SHL64ri, DL, MVT::i64,
                                           N->getOperand(0), Sh);
      unsigned Opc =
          N->getOpcode() == ISD::ZERO_EXTEND ? CBC::SRL64ri : CBC::SRA64ri;
      SDNode *Ext =
          CurDAG->getMachineNode(Opc, DL, N->getValueType(0), SDValue(Shl, 0),
                                 Sh);
      ReplaceUses(SDValue(N, 0), SDValue(Ext, 0));
      CurDAG->RemoveDeadNode(N);
      return;
    }
    case ISD::MULHU: {
      // 32-bit mulhu is bits [63:32] of the zero-extended product, not the
      // high half of a 128-bit product.
      if (N->getValueType(0).getSizeInBits() <= 32) {
        SDValue A = cbcExtendToI64(*CurDAG, DL, N->getOperand(0), false);
        SDValue B = cbcExtendToI64(*CurDAG, DL, N->getOperand(1), false);
        SDValue Prod = SDValue(
            CurDAG->getMachineNode(CBC::MUL64rr, DL, MVT::i64, A, B), 0);
        SDValue Hi = SDValue(
            CurDAG->getMachineNode(
                CBC::SRL64ri, DL, MVT::i64, Prod,
                CurDAG->getTargetConstant(32, DL, MVT::i64)),
            0);
        ReplaceUses(SDValue(N, 0), Hi);
        CurDAG->RemoveDeadNode(N);
        return;
      }
      SDValue Mask = CurDAG->getTargetConstant(0xFFFFFFFFULL, DL, MVT::i64);
      SDValue Sh32 = CurDAG->getTargetConstant(32, DL, MVT::i64);
      auto lo32 = [&](SDValue V) {
        return SDValue(
            CurDAG->getMachineNode(CBC::AND64ri, DL, MVT::i64, V, Mask), 0);
      };
      auto hi32 = [&](SDValue V) {
        return SDValue(
            CurDAG->getMachineNode(CBC::SRL64ri, DL, MVT::i64, V, Sh32), 0);
      };
      auto mul = [&](SDValue L, SDValue R) {
        return SDValue(
            CurDAG->getMachineNode(CBC::MUL64rr, DL, MVT::i64, L, R), 0);
      };
      auto add = [&](SDValue L, SDValue R) {
        return SDValue(
            CurDAG->getMachineNode(CBC::ADD64rr, DL, MVT::i64, L, R), 0);
      };
      SDValue A0 = lo32(N->getOperand(0));
      SDValue A1 = hi32(N->getOperand(0));
      SDValue B0 = lo32(N->getOperand(1));
      SDValue B1 = hi32(N->getOperand(1));
      SDValue P00 = mul(A0, B0);
      SDValue P01 = mul(A0, B1);
      SDValue P10 = mul(A1, B0);
      SDValue P11 = mul(A1, B1);
      SDValue Mid = add(add(hi32(P00), lo32(P01)), lo32(P10));
      SDValue Hi = add(add(add(P11, hi32(P01)), hi32(P10)), hi32(Mid));
      ReplaceUses(SDValue(N, 0), Hi);
      CurDAG->RemoveDeadNode(N);
      return;
    }
    case ISD::MULHS: {
      // Same as mulhu, but the inputs are sign-extended first.
      if (N->getValueType(0).getSizeInBits() <= 32) {
        SDValue A = cbcExtendToI64(*CurDAG, DL, N->getOperand(0), true);
        SDValue B = cbcExtendToI64(*CurDAG, DL, N->getOperand(1), true);
        SDValue Prod = SDValue(
            CurDAG->getMachineNode(CBC::MUL64rr, DL, MVT::i64, A, B), 0);
        SDValue Hi = SDValue(
            CurDAG->getMachineNode(
                CBC::SRL64ri, DL, MVT::i64, Prod,
                CurDAG->getTargetConstant(32, DL, MVT::i64)),
            0);
        ReplaceUses(SDValue(N, 0), Hi);
        CurDAG->RemoveDeadNode(N);
        return;
      }
      SDValue A = N->getOperand(0);
      SDValue B = N->getOperand(1);
      SDValue Mask = CurDAG->getTargetConstant(0xFFFFFFFFULL, DL, MVT::i64);
      SDValue Sh32 = CurDAG->getTargetConstant(32, DL, MVT::i64);
      SDValue Sh63 = CurDAG->getTargetConstant(63, DL, MVT::i64);
      auto lo32 = [&](SDValue V) {
        return SDValue(
            CurDAG->getMachineNode(CBC::AND64ri, DL, MVT::i64, V, Mask), 0);
      };
      auto hi32 = [&](SDValue V) {
        return SDValue(
            CurDAG->getMachineNode(CBC::SRL64ri, DL, MVT::i64, V, Sh32), 0);
      };
      auto mul = [&](SDValue L, SDValue R) {
        return SDValue(
            CurDAG->getMachineNode(CBC::MUL64rr, DL, MVT::i64, L, R), 0);
      };
      auto add = [&](SDValue L, SDValue R) {
        return SDValue(
            CurDAG->getMachineNode(CBC::ADD64rr, DL, MVT::i64, L, R), 0);
      };
      auto band = [&](SDValue L, SDValue R) {
        return SDValue(
            CurDAG->getMachineNode(CBC::AND64rr, DL, MVT::i64, L, R), 0);
      };
      SDValue A0 = lo32(A), A1 = hi32(A), B0 = lo32(B), B1 = hi32(B);
      SDValue P00 = mul(A0, B0), P01 = mul(A0, B1), P10 = mul(A1, B0),
              P11 = mul(A1, B1);
      SDValue Mid = add(add(hi32(P00), lo32(P01)), lo32(P10));
      SDValue HU = add(add(add(P11, hi32(P01)), hi32(P10)), hi32(Mid));
      auto sra63 = [&](SDValue V) {
        return SDValue(
            CurDAG->getMachineNode(CBC::SRA64ri, DL, MVT::i64, V, Sh63), 0);
      };
      SDValue Adj = add(band(sra63(A), B), band(sra63(B), A));
      SDValue Res = SDValue(
          CurDAG->getMachineNode(CBC::SUB64rr, DL, MVT::i64, HU, Adj), 0);
      ReplaceUses(SDValue(N, 0), Res);
      CurDAG->RemoveDeadNode(N);
      return;
    }
    case ISD::FADD:
    case ISD::FSUB:
    case ISD::FMUL:
    case ISD::FDIV: {
      unsigned Op = N->getOpcode() == ISD::FADD   ? 0
                    : N->getOpcode() == ISD::FSUB ? 1
                    : N->getOpcode() == ISD::FMUL ? 2
                                                  : 3;
      bool F32 = N->getValueType(0) == MVT::f32;
      SDValue Ops[] = {CurDAG->getTargetConstant(Op, DL, MVT::i32),
                       N->getOperand(0), N->getOperand(1)};
      CurDAG->SelectNodeTo(N, F32 ? CBC::FBIN32 : CBC::FBIN64,
                           N->getValueType(0), Ops);
      return;
    }
    case ISD::FNEG:
    case ISD::FABS:
    case ISD::FSQRT: {
      unsigned Op = N->getOpcode() == ISD::FNEG   ? 5
                    : N->getOpcode() == ISD::FABS ? 6
                                                  : 7;
      bool F32 = N->getValueType(0) == MVT::f32;
      SDValue Ops[] = {CurDAG->getTargetConstant(Op, DL, MVT::i32),
                       N->getOperand(0)};
      CurDAG->SelectNodeTo(N, F32 ? CBC::FUNARY32 : CBC::FUNARY64,
                           N->getValueType(0), Ops);
      return;
    }
    case ISD::BITCAST: {
      EVT VT = N->getValueType(0);
      EVT SVT = N->getOperand(0).getValueType();
      if ((VT == MVT::f64 || VT == MVT::f32) && SVT.isInteger()) {
        CurDAG->SelectNodeTo(N, CBC::I2F, VT, N->getOperand(0));
        return;
      }
      if (VT.isInteger() && (SVT == MVT::f64 || SVT == MVT::f32)) {
        CurDAG->SelectNodeTo(N, CBC::F2I, VT, N->getOperand(0));
        return;
      }
      break;
    }
    case ISD::FP_EXTEND:
    case ISD::FP_ROUND: {
      unsigned To = N->getValueType(0) == MVT::f64 ? 10 : 9;
      unsigned From = N->getOperand(0).getValueType() == MVT::f64 ? 10 : 9;
      SDValue Ops[] = {CurDAG->getTargetConstant(To, DL, MVT::i32),
                       CurDAG->getTargetConstant(From, DL, MVT::i32),
                       N->getOperand(0)};
      CurDAG->SelectNodeTo(N, CBC::CVT_FF, N->getValueType(0), Ops);
      return;
    }
    case ISD::SINT_TO_FP:
    case ISD::UINT_TO_FP: {
      unsigned To = N->getValueType(0) == MVT::f64 ? 10 : 9;
      bool Signed = N->getOpcode() == ISD::SINT_TO_FP;
      unsigned Bits = N->getOperand(0).getValueType().getSizeInBits();
      unsigned From = Bits <= 32 ? (Signed ? 4 : 5) : (Signed ? 6 : 7);
      SDValue Ops[] = {CurDAG->getTargetConstant(To, DL, MVT::i32),
                       CurDAG->getTargetConstant(From, DL, MVT::i32),
                       N->getOperand(0)};
      CurDAG->SelectNodeTo(N, CBC::CVT_IF, N->getValueType(0), Ops);
      return;
    }
    case ISD::FP_TO_SINT:
    case ISD::FP_TO_UINT: {
      bool Signed = N->getOpcode() == ISD::FP_TO_SINT;
      unsigned Bits = N->getValueType(0).getSizeInBits();
      unsigned To = Bits <= 32 ? (Signed ? 4 : 5) : (Signed ? 6 : 7);
      unsigned From = N->getOperand(0).getValueType() == MVT::f64 ? 10 : 9;
      SDValue Ops[] = {CurDAG->getTargetConstant(To, DL, MVT::i32),
                       CurDAG->getTargetConstant(From, DL, MVT::i32),
                       N->getOperand(0)};
      CurDAG->SelectNodeTo(N, CBC::CVT_FI, N->getValueType(0), Ops);
      return;
    }
    case ISD::UMUL_LOHI:
    case ISD::SMUL_LOHI: {
      SDValue A = N->getOperand(0);
      SDValue B = N->getOperand(1);
      SDValue Mask = CurDAG->getTargetConstant(0xFFFFFFFFULL, DL, MVT::i64);
      SDValue Sh32 = CurDAG->getTargetConstant(32, DL, MVT::i64);
      auto lo32 = [&](SDValue V) {
        return SDValue(
            CurDAG->getMachineNode(CBC::AND64ri, DL, MVT::i64, V, Mask), 0);
      };
      auto hi32 = [&](SDValue V) {
        return SDValue(
            CurDAG->getMachineNode(CBC::SRL64ri, DL, MVT::i64, V, Sh32), 0);
      };
      auto mul = [&](SDValue L, SDValue R) {
        return SDValue(
            CurDAG->getMachineNode(CBC::MUL64rr, DL, MVT::i64, L, R), 0);
      };
      auto add = [&](SDValue L, SDValue R) {
        return SDValue(
            CurDAG->getMachineNode(CBC::ADD64rr, DL, MVT::i64, L, R), 0);
      };
      SDValue P00 = mul(lo32(A), lo32(B));
      SDValue P01 = mul(lo32(A), hi32(B));
      SDValue P10 = mul(hi32(A), lo32(B));
      SDValue P11 = mul(hi32(A), hi32(B));
      SDValue Mid = add(add(hi32(P00), lo32(P01)), lo32(P10));
      SDValue Hi = add(add(add(P11, hi32(P01)), hi32(P10)), hi32(Mid));
      if (N->getOpcode() == ISD::SMUL_LOHI) {
        SDValue Sh63 = CurDAG->getTargetConstant(63, DL, MVT::i64);
        auto sra63 = [&](SDValue V) {
          return SDValue(
              CurDAG->getMachineNode(CBC::SRA64ri, DL, MVT::i64, V, Sh63), 0);
        };
        auto band = [&](SDValue L, SDValue R) {
          return SDValue(
              CurDAG->getMachineNode(CBC::AND64rr, DL, MVT::i64, L, R), 0);
        };
        Hi = SDValue(CurDAG->getMachineNode(CBC::SUB64rr, DL, MVT::i64, Hi,
                                            add(band(sra63(A), B), band(sra63(B), A))),
                     0);
      }
      ReplaceUses(SDValue(N, 0), mul(A, B));
      ReplaceUses(SDValue(N, 1), Hi);
      CurDAG->RemoveDeadNode(N);
      return;
    }
    case ISD::SDIV:
    case ISD::UDIV:
    case ISD::SREM:
    case ISD::UREM: {
      bool Signed =
          N->getOpcode() == ISD::SDIV || N->getOpcode() == ISD::SREM;
      SDValue OpL = N->getOperand(0), OpR = N->getOperand(1);
      // Materialize constants and widen to i64. W32 ALU leaves bits 63:32
      // undefined; rem/div must operate on a clean sext/zext i64.
      auto materialize = [&](SDValue V) {
        if (auto *C = dyn_cast<ConstantSDNode>(V)) {
          // Unsigned div must not sign-extend. i32 -1 is UINT_MAX, not i64 -1.
          uint64_t Bits = Signed ? static_cast<uint64_t>(C->getSExtValue())
                                 : C->getZExtValue();
          return SDValue(
              CurDAG->getMachineNode(
                  CBC::MOV64ri, DL, MVT::i64,
                  CurDAG->getTargetConstant(Bits, DL, MVT::i64)),
              0);
        }
        return V;
      };
      SDValue L = cbcExtendToI64(*CurDAG, DL, materialize(OpL), Signed);
      SDValue R = cbcExtendToI64(*CurDAG, DL, materialize(OpR), Signed);
      unsigned Opc = CBC::SDIV64rr;
      if (N->getOpcode() == ISD::SDIV)
        Opc = CBC::SDIV64rr;
      else if (N->getOpcode() == ISD::UDIV)
        Opc = CBC::UDIV64rr;
      else if (N->getOpcode() == ISD::SREM)
        Opc = CBC::SREM64rr;
      else
        Opc = CBC::UREM64rr;
      CurDAG->SelectNodeTo(N, Opc, N->getValueType(0), L, R);
      return;
    }
    case ISD::AssertAlign:
    case ISD::AssertNoFPClass:
      ReplaceUses(SDValue(N, 0), N->getOperand(0));
      CurDAG->RemoveDeadNode(N);
      return;
    default:
      break;
    }
    std::string Msg;
    raw_string_ostream OS(Msg);
    N->print(OS);
    report_fatal_error(Twine("CBC cannot select: ") + OS.str());
  }
};

class CBCDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  explicit CBCDAGToDAGISelLegacy(CBCTargetMachine &TM)
      : SelectionDAGISelLegacy(ID, std::make_unique<CBCDAGToDAGISel>(TM)) {}
};
} // namespace

char CBCDAGToDAGISelLegacy::ID = 0;
INITIALIZE_PASS(CBCDAGToDAGISelLegacy, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createCBCISelDag(CBCTargetMachine &TM) {
  return new CBCDAGToDAGISelLegacy(TM);
}
