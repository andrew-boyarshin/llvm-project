#include "CBC.h"
#include "CBCCallInfo.h"
#include "CBCFileWriter.h"
#include "CBCSubtarget.h"
#include "CBCTargetMachine.h"
#include "MCTargetDesc/CBCEncoding.h"
#include "MCTargetDesc/CBCOpcodes.h"
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
                        bool RetFloat, bool Synthetic = false) {
    std::string Key;
    raw_string_ostream OS(Key);
    OS << Name << ":" << Native << ":" << Params << ":" << RetFloat << ":"
       << Synthetic;
    auto It = NativeIndex.find(Key);
    if (It != NativeIndex.end())
      return It->second;
    unsigned Idx = Natives.size();
    Natives.push_back({Name.str(), Native, Params.str(), RetFloat, Synthetic});
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

  /// True when cbc.image MD has a non-zero SizeBytes (same predicate as
  /// CBCFileWriter). Data.image occupies FRef 0 when present.
  /// Do not use AsmPrinter::MF here: CBC runOnMachineFunction shadows that
  /// member and never installs it on the non-text path.
  static bool moduleHasImage(const Module *M) {
    if (!M)
      return false;
    NamedMDNode *MD = M->getNamedMetadata("cbc.image");
    if (!MD || !MD->getNumOperands())
      return false;
    MDNode *Node = MD->getOperand(0);
    if (!Node->getNumOperands())
      return false;
    auto *CAM = dyn_cast<ConstantAsMetadata>(Node->getOperand(0));
    if (!CAM)
      return false;
    return cast<ConstantInt>(CAM->getValue())->getZExtValue() != 0;
  }

  /// Field-ref index of $cbc:A{8,16,32,64}.v (after Data.image when present).
  static unsigned atomicFieldRef(const Module *M, unsigned WidthBits) {
    unsigned Base = moduleHasImage(M) ? 1 : 0;
    switch (WidthBits) {
    case 8:
      return Base + 0;
    case 16:
      return Base + 1;
    case 32:
      return Base + 2;
    case 64:
      return Base + 3;
    default:
      report_fatal_error("CBC atomic width must be 8/16/32/64");
    }
  }

  bool encodeAtomic(const MachineInstr &MI, SmallVectorImpl<char> &Buf) {
    unsigned Opc = MI.getOpcode();
    unsigned IsaOpc = 0;
    unsigned Width = 0;
    bool IsStore = false;
    bool IsCas = false;
    bool IsFetch = false;
    auto match = [&](unsigned W, unsigned LD, unsigned ST, unsigned CAS,
                     unsigned XCHG, unsigned FADD, unsigned FSUB, unsigned FAND,
                     unsigned FOR, unsigned FXOR) {
      if (Opc == LD) {
        IsaOpc = CBCISA::AtomicLoad;
        Width = W;
        return true;
      }
      if (Opc == ST) {
        IsaOpc = CBCISA::AtomicStore;
        Width = W;
        IsStore = true;
        return true;
      }
      if (Opc == CAS) {
        IsaOpc = CBCISA::CAS;
        Width = W;
        IsCas = true;
        return true;
      }
      if (Opc == XCHG) {
        IsaOpc = CBCISA::Swap;
        Width = W;
        IsFetch = true;
        return true;
      }
      if (Opc == FADD) {
        IsaOpc = CBCISA::AtomicFetchAdd;
        Width = W;
        IsFetch = true;
        return true;
      }
      if (Opc == FSUB) {
        IsaOpc = CBCISA::AtomicFetchSub;
        Width = W;
        IsFetch = true;
        return true;
      }
      if (Opc == FAND) {
        IsaOpc = CBCISA::AtomicFetchAnd;
        Width = W;
        IsFetch = true;
        return true;
      }
      if (Opc == FOR) {
        IsaOpc = CBCISA::AtomicFetchOr;
        Width = W;
        IsFetch = true;
        return true;
      }
      if (Opc == FXOR) {
        IsaOpc = CBCISA::AtomicFetchXor;
        Width = W;
        IsFetch = true;
        return true;
      }
      return false;
    };
    if (!match(8, CBC::ATOMIC_LD_8, CBC::ATOMIC_ST_8, CBC::ATOMIC_CAS_8,
               CBC::ATOMIC_XCHG_8, CBC::ATOMIC_FADD_8, CBC::ATOMIC_FSUB_8,
               CBC::ATOMIC_FAND_8, CBC::ATOMIC_FOR_8, CBC::ATOMIC_FXOR_8) &&
        !match(16, CBC::ATOMIC_LD_16, CBC::ATOMIC_ST_16, CBC::ATOMIC_CAS_16,
               CBC::ATOMIC_XCHG_16, CBC::ATOMIC_FADD_16, CBC::ATOMIC_FSUB_16,
               CBC::ATOMIC_FAND_16, CBC::ATOMIC_FOR_16, CBC::ATOMIC_FXOR_16) &&
        !match(32, CBC::ATOMIC_LD_32, CBC::ATOMIC_ST_32, CBC::ATOMIC_CAS_32,
               CBC::ATOMIC_XCHG_32, CBC::ATOMIC_FADD_32, CBC::ATOMIC_FSUB_32,
               CBC::ATOMIC_FAND_32, CBC::ATOMIC_FOR_32, CBC::ATOMIC_FXOR_32) &&
        !match(64, CBC::ATOMIC_LD_64, CBC::ATOMIC_ST_64, CBC::ATOMIC_CAS_64,
               CBC::ATOMIC_XCHG_64, CBC::ATOMIC_FADD_64, CBC::ATOMIC_FSUB_64,
               CBC::ATOMIC_FAND_64, CBC::ATOMIC_FOR_64, CBC::ATOMIC_FXOR_64))
      return false;

    const MachineFunction *MFunc = MI.getMF();
    const Module *Mod =
        MFunc ? MFunc->getFunction().getParent() : nullptr;
    unsigned FRef = atomicFieldRef(Mod, Width);
    Buf.push_back(static_cast<char>(IsaOpc));
    if (IsStore) {
      // 66 [s:p] uleb(f)
      unsigned S = cbcEncodeReg(MI.getOperand(0).getReg());
      unsigned P = cbcEncodeReg(MI.getOperand(1).getReg());
      Buf.push_back(static_cast<char>((S << 4) | (P & 0xF)));
    } else if (IsCas) {
      // 67 [d:p] [exp:new] uleb(f)
      unsigned D = cbcEncodeReg(MI.getOperand(0).getReg());
      unsigned P = cbcEncodeReg(MI.getOperand(1).getReg());
      unsigned Exp = cbcEncodeReg(MI.getOperand(2).getReg());
      unsigned New = cbcEncodeReg(MI.getOperand(3).getReg());
      Buf.push_back(static_cast<char>((D << 4) | (P & 0xF)));
      Buf.push_back(static_cast<char>((Exp << 4) | (New & 0xF)));
    } else if (IsFetch) {
      // 68..6D [d:p] [s:0] uleb(f)
      unsigned D = cbcEncodeReg(MI.getOperand(0).getReg());
      unsigned P = cbcEncodeReg(MI.getOperand(1).getReg());
      unsigned S = cbcEncodeReg(MI.getOperand(2).getReg());
      Buf.push_back(static_cast<char>((D << 4) | (P & 0xF)));
      Buf.push_back(static_cast<char>((S << 4) | 0));
    } else {
      // 65 [d:p] uleb(f)
      unsigned D = cbcEncodeReg(MI.getOperand(0).getReg());
      unsigned P = cbcEncodeReg(MI.getOperand(1).getReg());
      Buf.push_back(static_cast<char>((D << 4) | (P & 0xF)));
    }
    appendULEB(Buf, FRef);
    return true;
  }

  /// 'i' / 'f' per IR parameter, '.' before the variadic arguments of a
  /// variadic function (only the fixed ones are listed for a declaration).
  static std::string irParams(const Function &F) {
    std::string Params;
    for (const Argument &Arg : F.args())
      Params.push_back(Arg.getType()->isFloatingPointTy() ? 'f' : 'i');
    if (F.isVarArg())
      Params.push_back('.');
    return Params;
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
      Params = irParams(*F);
      RetFloat = F->getReturnType()->isFloatingPointTy();
    }
    unsigned Idx = internNative(GV->getName(), GV->isDeclarationForLinker(),
                                Params, RetFloat);
    Buf.push_back(CBCISA::RegSymGroup);
    Buf.push_back(static_cast<char>((CBCISA::SymLoadFuncPtr << 4) |
                                    (cbcEncodeReg(Dest) & 0xF)));
    appendULEB(Buf, Idx);
  }

  /// The signature and argument locations that LowerCall attached to a call.
  struct CallDesc {
    std::string Params; // 'i' / 'f' / '.' (variadic label), lowered
    bool RetFloat = false;
    SmallVector<unsigned, 16> Locs;
  };

  static CallDesc parseCallDesc(const MachineInstr &MI) {
    SmallVector<int64_t, 32> Imms;
    for (const MachineOperand &MO : MI.operands())
      if (MO.isImm())
        Imms.push_back(MO.getImm());
    CallDesc D;
    size_t P = 0;
    auto next = [&]() -> int64_t {
      if (P >= Imms.size())
        report_fatal_error("CBC call is missing its signature operands");
      return Imms[P++];
    };
    P += next();                // register arguments, consumed by isel
    D.RetFloat = next() != 0;
    for (int64_t N = next(); N > 0; --N) {
      switch (next()) {
      case CBCCall::SigInt:
        D.Params.push_back('i');
        break;
      case CBCCall::SigFloat:
        D.Params.push_back('f');
        break;
      default:
        D.Params.push_back('.');
        break;
      }
    }
    for (int64_t N = next(); N > 0; --N)
      D.Locs.push_back(static_cast<unsigned>(next()));
    return D;
  }

  /// Argument list shared by the direct and indirect call encodings: the
  /// location of every argument (not the variadic label), 0-terminated.
  void appendLocations(const CallDesc &D, SmallVectorImpl<char> &Buf) {
    unsigned NArgs = 0;
    for (char K : D.Params)
      NArgs += K != '.';
    if (NArgs != D.Locs.size())
      report_fatal_error("CBC call signature does not match its arguments");
    for (unsigned L : D.Locs)
      appendULEB(Buf, L + 1);
    Buf.push_back(0);
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
    CallDesc D = parseCallDesc(MI);
    // One synthetic method reference of ABI kind C per lowered function type;
    // it only carries the signature.
    unsigned Idx = internNative("", /*Native=*/false, D.Params, D.RetFloat,
                                /*Synthetic=*/true);
    Buf.push_back(CBCISA::CallIndirect);
    Buf.push_back(static_cast<char>((cbcEncodeReg(Tgt) & 0xF) << 4));
    appendULEB(Buf, Idx);
    appendLocations(D, Buf);
  }

  void encodeCall(const MachineInstr &MI, SmallVectorImpl<char> &Buf) {
    StringRef Name;
    bool Native = true;
    const Function *CalleeFn = nullptr;
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isGlobal()) {
        Name = MO.getGlobal()->getName();
        Native = MO.getGlobal()->isDeclarationForLinker();
        CalleeFn = dyn_cast<Function>(MO.getGlobal());
      } else if (MO.isSymbol()) {
        Name = MO.getSymbolName();
        Native = true;
      }
    }
    CallDesc D = parseCallDesc(MI);
    std::string Params = D.Params;
    bool RetFloat = D.RetFloat;
    // A callee defined here is matched by name and signature against its
    // definition, which uses the IR parameter list. The lowered list must have
    // the same length (no split / byval arguments get this far).
    if (!Native && CalleeFn && !CalleeFn->isVarArg()) {
      // One extra lowered argument is the hidden pointer of a demoted return.
      size_t Extra = D.Locs.size() - std::min(D.Locs.size(), CalleeFn->arg_size());
      if (D.Locs.size() < CalleeFn->arg_size() || Extra > 1)
        report_fatal_error("CBC call to '" + Name +
                           "' passes a different number of lowered arguments than the callee declares");
      Params = irParams(*CalleeFn);
      RetFloat = Extra == 0 && CalleeFn->getReturnType()->isFloatingPointTy();
      if (Extra == 1)
        Params.insert(Params.begin(), 'i');
    }
    unsigned Idx = internNative(Name, Native, Params, RetFloat);
    Buf.push_back(CBCISA::CallDirect);
    appendULEB(Buf, Idx);
    CallDesc Placed = D;
    Placed.Params = Params;
    appendLocations(Placed, Buf);
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
    if (encodeAtomic(MI, Buf))
      return;
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
    if (MI.getOpcode() == CBC::JMP || MI.getOpcode() == CBC::BCC32 ||
        MI.getOpcode() == CBC::BCC64 || MI.getOpcode() == CBC::BCCF) {
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
    const Function &Fn = MF.getFunction();
    // A demoted return adds a hidden pointer parameter in front (and the
    // function returns nothing in registers).
    if (Info->DemotedSRet)
      M.Params.push_back('i');
    for (const Argument &Arg : Fn.args())
      M.Params.push_back(Arg.getType()->isFloatingPointTy() ? 'f' : 'i');
    M.RetFloat = !Info->DemotedSRet && Fn.getReturnType()->isFloatingPointTy();
    // Default-visibility *external* defs are N2C-stub candidates; local
    // linkage still uses VisibilityTypes::DefaultVisibility in LLVM IR.
    M.DefaultVis = !Fn.hasLocalLinkage() &&
                   Fn.getVisibility() == GlobalValue::DefaultVisibility;
    M.AddressTaken = Fn.hasAddressTaken();
    M.VarArg = Fn.isVarArg();
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
    bool Wrap = M.getModuleFlag("cbc-wrap") != nullptr;
    if (!HasMain && !Wrap)
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
    CBCFileBuildResult Built =
        buildCBCFile(Methods, Natives, Image, AotDeps, Wrap);
    CBCStoreExportsOnTargetMachine(TM, std::move(Built.Exports));
    OutStreamer->switchSection(getObjFileLowering().getTextSection());
    OutStreamer->emitBytes(Built.Bytes);
  }
};
} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeCBCAsmPrinter() {
  RegisterAsmPrinter<CBCAsmPrinter> X(getTheCBCTarget());
}
