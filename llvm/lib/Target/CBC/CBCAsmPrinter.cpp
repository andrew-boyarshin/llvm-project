#include "CBC.h"
#include "CBCFileWriter.h"
#include "CBCSubtarget.h"
#include "CBCTargetMachine.h"
#include "MCTargetDesc/CBCEncoding.h"
#include "TargetInfo/CBCTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

namespace {
class CBCAsmPrinter : public AsmPrinter {
  std::vector<CBCCompiledMethod> Methods;
  std::vector<CBCNativeCallee> Natives;
  StringMap<unsigned> NativeIndex;

  unsigned internNative(StringRef Name, bool Native, StringRef Params,
                        bool RetFloat) {
    std::string Key;
    raw_string_ostream OS(Key);
    OS << Name << ":" << Native << ":" << Params << ":" << RetFloat;
    auto It = NativeIndex.find(Key);
    if (It != NativeIndex.end())
      return It->second;
    unsigned Idx = Natives.size();
    Natives.push_back({Name.str(), Native, Params.str(), RetFloat});
    NativeIndex[Key] = Idx;
    return Idx;
  }

  void appendULEB(SmallVectorImpl<char> &Buf, uint64_t N) {
    while (true) {
      uint8_t B = N & 0x7F;
      N >>= 7;
      if (N)
        Buf.push_back(static_cast<char>(B | 0x80));
      else {
        Buf.push_back(static_cast<char>(B));
        return;
      }
    }
  }

  void encodeFnPtr(const MachineInstr &MI, SmallVectorImpl<char> &Buf) {
    const GlobalValue *GV = nullptr;
    Register Dest;
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isGlobal())
        GV = MO.getGlobal();
      else if (MO.isReg() && !MO.isImplicit() && MO.isDef())
        Dest = MO.getReg();
    }
    if (!GV || !Dest)
      report_fatal_error("CBC ld.fnptr is missing its operand");
    std::string Params;
    bool RetFloat = false;
    if (const auto *F = dyn_cast<Function>(GV)) {
      for (const Argument &Arg : F->args())
        Params.push_back(Arg.getType()->isFloatingPointTy() ? 'f' : 'i');
      RetFloat = F->getReturnType()->isFloatingPointTy();
    }
    unsigned Idx = internNative(GV->getName(), GV->isDeclarationForLinker(),
                                Params, RetFloat);
    Buf.push_back(0x44);
    Buf.push_back(static_cast<char>((0xD << 4) | (cbcEncodeReg(Dest) & 0xF)));
    appendULEB(Buf, Idx);
  }

  void encodeIndirect(const MachineInstr &MI, SmallVectorImpl<char> &Buf) {
    Register Tgt;
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isReg() && !MO.isImplicit() && !MO.isDef()) {
        Tgt = MO.getReg();
        break;
      }
    }
    if (!Tgt)
      report_fatal_error("CBC call.indirect is missing its target");
    Buf.push_back(0x45);
    Buf.push_back(static_cast<char>((0x9 << 4) | (cbcEncodeReg(Tgt) & 0xF)));
  }

  void encodeCall(const MachineInstr &MI, SmallVectorImpl<char> &Buf) {
    StringRef Name;
    bool Native = true;
    SmallVector<int64_t, 8> Imms;
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isReg())
        continue;
      if (MO.isGlobal()) {
        Name = MO.getGlobal()->getName();
        Native = MO.getGlobal()->isDeclarationForLinker();
      } else if (MO.isSymbol()) {
        Name = MO.getSymbolName();
        Native = true;
      } else if (MO.isImm())
        Imms.push_back(MO.getImm());
    }
    unsigned NParams = Imms.empty() ? 0 : Imms[0];
    std::string Params;
    for (unsigned I = 0; I < NParams && I + 1 < Imms.size(); ++I)
      Params.push_back(Imms[I + 1] ? 'f' : 'i');
    bool RetFloat = Imms.size() > NParams + 1 && Imms[NParams + 1];
    const Function *CalleeFn = nullptr;
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isGlobal())
        CalleeFn = dyn_cast<Function>(MO.getGlobal());
    }
    // The call instruction lists only register arguments. The engine matches
    // the full LLVM argument list, including values passed in stack slots.
    if (!Native && CalleeFn && !CalleeFn->isDeclarationForLinker()) {
      Params.clear();
      for (const Argument &Arg : CalleeFn->args())
        Params.push_back(Arg.getType()->isFloatingPointTy() ? 'f' : 'i');
      RetFloat = CalleeFn->getReturnType()->isFloatingPointTy();
    }
    unsigned Idx = internNative(Name, Native, Params, RetFloat);
    Buf.push_back(0x44);
    Buf.push_back(0x31);
    appendULEB(Buf, Idx);
  }

  std::vector<uint32_t> StatePoints;

  void encodeMI(const MachineInstr &MI, SmallVectorImpl<char> &Buf) {
    if (MI.getOpcode() == CBC::CALL) {
      encodeCall(MI, Buf);
      return;
    }
    if (MI.getOpcode() == CBC::CALL_INDIRECT) {
      encodeIndirect(MI, Buf);
      return;
    }
    if (MI.getOpcode() == CBC::LD_FNPTR) {
      encodeFnPtr(MI, Buf);
      return;
    }
    MCInst Inst;
    Inst.setOpcode(MI.getOpcode());
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isReg()) {
        if (!MO.isImplicit())
          Inst.addOperand(MCOperand::createReg(MO.getReg()));
      } else if (MO.isImm())
        Inst.addOperand(MCOperand::createImm(MO.getImm()));
      else if (!MO.isMBB()) {
        std::string Msg;
        raw_string_ostream OS(Msg);
        OS << "CBC operand lowering is not implemented yet: ";
        MI.print(OS);
        report_fatal_error(StringRef(Msg));
      }
    }
    encodeCBCInst(Inst, Buf);
  }

  struct Encoded {
    SmallVector<char, 16> Bytes;
    const MachineBasicBlock *MBB = nullptr;
    const MachineBasicBlock *Target = nullptr;
    int DispOff = -1;
    bool IsCall = false;
    /// Generic Bcc: low 4 bits of the displacement sit in the low nibble of
    /// the byte before DispOff, and the high part is the s16/s32 at DispOff.
    bool SplitDisp = false;
    /// WidePrefix (0x13): displacement is s32 instead of s16.
    bool Wide = false;
  };

  static void widenBranch(Encoded &E) {
    SmallVector<char, 16> W;
    W.push_back(0x13);
    W.append(E.Bytes.begin(), E.Bytes.begin() + E.DispOff);
    W.append(4, 0);
    E.DispOff = E.DispOff + 1;
    E.Bytes = std::move(W);
    E.Wide = true;
  }

  static void patchDisp(Encoded &E, int32_t Delta) {
    if (E.SplitDisp) {
      unsigned Lo = static_cast<unsigned>(Delta) & 0xF;
      int32_t Hi = Delta >> 4;
      E.Bytes[E.DispOff - 1] =
          static_cast<char>((E.Bytes[E.DispOff - 1] & 0xF0) | Lo);
      unsigned N = E.Wide ? 4 : 2;
      for (unsigned I = 0; I < N; ++I)
        E.Bytes[E.DispOff + I] = static_cast<char>((Hi >> (8 * I)) & 0xFF);
      return;
    }
    unsigned N = E.Wide ? 4 : 2;
    for (unsigned I = 0; I < N; ++I)
      E.Bytes[E.DispOff + I] = static_cast<char>((Delta >> (8 * I)) & 0xFF);
  }

  static bool dispFits(const Encoded &E, int32_t Delta) {
    if (E.Wide)
      return true;
    if (E.SplitDisp)
      return Delta >= -(1 << 19) && Delta < (1 << 19);
    return Delta >= -32768 && Delta <= 32767;
  }

  Encoded encodePiece(const MachineInstr &MI, const MachineBasicBlock &MBB) {
    Encoded E;
    E.MBB = &MBB;
    SmallVector<char, 16> Tmp;
    encodeMI(MI, Tmp);
    E.Bytes.assign(Tmp.begin(), Tmp.end());
    E.IsCall = MI.getOpcode() == CBC::CALL ||
               MI.getOpcode() == CBC::CALL_INDIRECT;
    if (MI.getOpcode() == CBC::JMP || MI.getOpcode() == CBC::BCC64 ||
        MI.getOpcode() == CBC::BCCF) {
      for (const MachineOperand &MO : MI.operands()) {
        if (MO.isMBB()) {
          E.Target = MO.getMBB();
          E.DispOff = E.Bytes.size() - 2;
          E.SplitDisp = MI.getOpcode() == CBC::BCCF;
          break;
        }
      }
    }
    return E;
  }

public:
  CBCAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)) {}
  StringRef getPassName() const override { return "CBC Assembly Printer"; }

  void emitFunctionBodyStart() override {
    if (!OutStreamer->hasRawTextSupport())
      return;
    unsigned Uses = 0;
    unsigned IMask = 0;
    for (const MachineBasicBlock &B : *MF) {
      for (const MachineInstr &MI : B) {
        unsigned Opc = MI.getOpcode();
        if (Opc == CBC::ALLOCA || Opc == CBC::STACKSAVE ||
            Opc == CBC::STACKRESTORE)
          Uses = 1;
        for (const MachineOperand &MO : MI.operands())
          if (MO.isReg() && MO.getReg() == CBC::IR13)
            IMask |= 1u << 5;
      }
    }
    OutStreamer->emitRawComment("CBC method " + MF->getName());
    OutStreamer->emitRawComment(
        "cbc-frame untypedmem=" + Twine(MF->getFrameInfo().getStackSize()) +
        " usesalloca=" + Twine(Uses) + " irmask=" + Twine(IMask));
  }

  void emitInstruction(const MachineInstr *MI) override {
    if (MI->getOpcode() == CBC::CALL) {
      StringRef Name;
      for (const MachineOperand &MO : MI->operands()) {
        if (MO.isGlobal())
          Name = MO.getGlobal()->getName();
        else if (MO.isSymbol())
          Name = MO.getSymbolName();
      }
      OutStreamer->emitRawComment("call.direct " + Name);
      return;
    }
    MCInst Inst;
    Inst.setOpcode(MI->getOpcode());
    for (const MachineOperand &MO : MI->operands()) {
      if (MO.isReg()) {
        if (!MO.isImplicit())
          Inst.addOperand(MCOperand::createReg(MO.getReg()));
      } else if (MO.isImm())
        Inst.addOperand(MCOperand::createImm(MO.getImm()));
      else
        report_fatal_error("CBC operand lowering is not implemented yet");
    }
    EmitToStreamer(*OutStreamer, Inst);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (OutStreamer->hasRawTextSupport()) {
      AsmPrinter::runOnMachineFunction(MF);
      return false;
    }
    CBCCompiledMethod M;
    StringRef FnName = MF.getName();
    M.Name = FnName == "__cbc_entry" ? std::string("main") : FnName.str();
    auto *Info = MF.getInfo<CBCMachineFunctionInfo>();
    M.MaxCalleeStackArgs = Info->MaxOutgoingSlots;
    M.UntypedSlots = Info->MaxOutgoingSlots;
    uint64_t Mem = MF.getFrameInfo().getStackSize();
    if (Mem % 16 != 0)
      report_fatal_error("CBC untyped memory size is not a multiple of 16");
    M.UntypedMemSize = static_cast<uint32_t>(Mem);
    for (const MachineBasicBlock &B : MF) {
      for (const MachineInstr &MI : B) {
        unsigned Opc = MI.getOpcode();
        if (Opc == CBC::ALLOCA || Opc == CBC::STACKSAVE ||
            Opc == CBC::STACKRESTORE)
          M.UsesAlloca = 1;
        if ((Opc == CBC::STU64 || Opc == CBC::STUF64 || Opc == CBC::LDU64 ||
             Opc == CBC::LDUF64) &&
            MI.getOperand(MI.getOperand(0).isReg() ? 1 : 0).isImm()) {
          unsigned SlotOp = MI.getOperand(0).isReg() ? 1 : 0;
          M.UntypedSlots = std::max(
              M.UntypedSlots,
              static_cast<unsigned>(MI.getOperand(SlotOp).getImm()) + 1);
        }
      }
    }
    for (const MachineBasicBlock &B : MF) {
      for (const MachineInstr &MI : B) {
        for (const MachineOperand &MO : MI.operands()) {
          if (!MO.isReg())
            continue;
          Register R = MO.getReg();
          if (R == CBC::IR8)
            M.IMask |= 1 << 0;
          else if (R == CBC::IR9)
            M.IMask |= 1 << 1;
          else if (R == CBC::IR10)
            M.IMask |= 1 << 2;
          else if (R == CBC::IR11)
            M.IMask |= 1 << 3;
          else if (R == CBC::IR12)
            M.IMask |= 1 << 4;
          else if (R == CBC::IR13)
            M.IMask |= 1 << 5;
          else if (R == CBC::FR8)
            M.FMask |= 1 << 0;
          else if (R == CBC::FR9)
            M.FMask |= 1 << 1;
          else if (R == CBC::FR10)
            M.FMask |= 1 << 2;
          else if (R == CBC::FR11)
            M.FMask |= 1 << 3;
          else if (R == CBC::FR12)
            M.FMask |= 1 << 4;
          else if (R == CBC::FR13)
            M.FMask |= 1 << 5;
          else if (R == CBC::FR14)
            M.FMask |= 1 << 6;
          else if (R == CBC::FR15)
            M.FMask |= 1 << 7;
        }
      }
    }
    StatePoints.clear();
    SmallVector<Encoded, 64> Pieces;
    for (const MachineBasicBlock &MBB : MF) {
      for (const MachineInstr &MI : MBB) {
        if (MI.isMetaInstruction() || MI.isTransient())
          continue;
        Pieces.push_back(encodePiece(MI, MBB));
      }
    }
    DenseMap<const MachineBasicBlock *, uint32_t> Start;
    auto layout = [&]() {
      Start.clear();
      uint32_t PC = 0;
      const MachineBasicBlock *Cur = nullptr;
      for (const Encoded &E : Pieces) {
        if (E.MBB != Cur) {
          Start[E.MBB] = PC;
          Cur = E.MBB;
        }
        PC += E.Bytes.size();
      }
    };
    // Narrow branches are s16 (specialized) or ±2^19 (generic). Anything
    // farther is re-encoded with WidePrefix and an s32 displacement.
    for (unsigned Pass = 0; Pass < 8; ++Pass) {
      layout();
      bool Grew = false;
      uint32_t PC = 0;
      for (Encoded &E : Pieces) {
        uint32_t End = PC + E.Bytes.size();
        if (E.Target && E.DispOff >= 0 && !E.Wide) {
          int32_t Delta = int32_t(Start.lookup(E.Target)) - int32_t(End);
          if (!dispFits(E, Delta)) {
            widenBranch(E);
            Grew = true;
          }
        }
        PC = End;
      }
      if (!Grew)
        break;
    }
    layout();
    struct CallSite {
      uint32_t Begin = 0;
      uint32_t End = 0;
      const MachineBasicBlock *MBB = nullptr;
    };
    SmallVector<CallSite, 8> Calls;
    SmallVector<char, 64> Buf;
    uint32_t PC = 0;
    for (Encoded &E : Pieces) {
      uint32_t End = PC + E.Bytes.size();
      if (E.Target && E.DispOff >= 0) {
        int32_t Delta = int32_t(Start.lookup(E.Target)) - int32_t(End);
        if (!E.Wide && !dispFits(E, Delta))
          report_fatal_error("CBC branch is out of range");
        patchDisp(E, Delta);
      }
      Buf.append(E.Bytes.begin(), E.Bytes.end());
      if (E.IsCall) {
        StatePoints.push_back(End);
        Calls.push_back({PC, End, E.MBB});
      }
      PC = End;
    }
    // One region per invoke, covering just that call. The engine takes the
    // first match and rejects overlapping ranges, so a single region over the
    // whole function would send every throw to the last pad (often terminate).
    DenseMap<const MachineBasicBlock *, const MachineBasicBlock *> Unwind;
    for (const MachineBasicBlock &B : MF)
      for (const MachineBasicBlock *S : B.successors())
        if (S->isEHPad()) {
          Unwind[&B] = S;
          break;
        }
    DenseMap<const MachineBasicBlock *, unsigned> LastCall;
    for (unsigned I = 0, N = Calls.size(); I != N; ++I)
      LastCall[Calls[I].MBB] = I;
    for (const auto &LC : LastCall) {
      const MachineBasicBlock *Pad = Unwind.lookup(LC.first);
      if (!Pad || !Start.contains(Pad))
        continue;
      const CallSite &S = Calls[LC.second];
      M.Regions.push_back({S.Begin, S.End, Start.lookup(Pad)});
    }
    M.Code.assign(reinterpret_cast<const uint8_t *>(Buf.begin()),
                  reinterpret_cast<const uint8_t *>(Buf.end()));
    for (const Argument &Arg : MF.getFunction().args())
      M.Params.push_back(Arg.getType()->isFloatingPointTy() ? 'f' : 'i');
    M.RetFloat = MF.getFunction().getReturnType()->isFloatingPointTy();
    M.StatePoints = std::move(StatePoints);
    Methods.push_back(std::move(M));
    return false;
  }

  void emitEndOfAsmFile(Module &M) override {
    if (OutStreamer->hasRawTextSupport() || Methods.empty())
      return;
    bool HasMain = false;
    for (const CBCCompiledMethod &Meth : Methods)
      HasMain |= Meth.Name == "main";
    if (!HasMain && !M.getModuleFlag("cbc-wrap"))
      report_fatal_error("CBC object emission requires a function named main");
    CBCImageInfo Image;
    std::string AotDeps;
    if (NamedMDNode *MD = M.getNamedMetadata("cbc.image")) {
      if (MD->getNumOperands()) {
        MDNode *Node = MD->getOperand(0);
        if (Node->getNumOperands() >= 1)
          if (auto *CAM = dyn_cast<ConstantAsMetadata>(Node->getOperand(0)))
            Image.SizeBytes = cast<ConstantInt>(CAM->getValue())->getZExtValue();
        if (Node->getNumOperands() >= 2)
          if (auto *S = dyn_cast<MDString>(Node->getOperand(1)))
            Image.Blob = S->getString().str();
        if (Node->getNumOperands() >= 3)
          if (auto *S = dyn_cast<MDString>(Node->getOperand(2)))
            Image.Relocs = S->getString().str();
      }
    }
    for (CBCCompiledMethod &Meth : Methods)
      if (Meth.Name == "__cbc_image_init" &&
          (!Image.Blob.empty() || !Image.Relocs.empty()))
        Meth.NeedsImagePrologue = true;
    if (NamedMDNode *Deps = M.getNamedMetadata("cbc.aotDeps")) {
      if (Deps->getNumOperands()) {
        MDNode *Node = Deps->getOperand(0);
        if (Node->getNumOperands())
          if (auto *S = dyn_cast<MDString>(Node->getOperand(0)))
            AotDeps = S->getString().str();
      }
    }
    OutStreamer->switchSection(getObjFileLowering().getTextSection());
    OutStreamer->emitBytes(buildCBCFile(Methods, Natives, Image, AotDeps));
  }
};
} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeCBCAsmPrinter() {
  RegisterAsmPrinter<CBCAsmPrinter> X(getTheCBCTarget());
}
