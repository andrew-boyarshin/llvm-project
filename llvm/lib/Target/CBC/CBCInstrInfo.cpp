#include "CBCInstrInfo.h"
#include "CBC.h"
#include "CBCSubtarget.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "CBCGenInstrInfo.inc"

using namespace llvm;

CBCInstrInfo::CBCInstrInfo(const CBCSubtarget &STI)
    : CBCGenInstrInfo(STI, RI), RI() {}

void CBCInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator I, const DebugLoc &DL,
                               Register DestReg, Register SrcReg, bool KillSrc,
                               bool, bool) const {
  if (CBC::GPRRegClass.contains(DestReg) && CBC::GPRRegClass.contains(SrcReg)) {
    BuildMI(MBB, I, DL, get(CBC::MOV64rr), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }
  if (CBC::FPRRegClass.contains(DestReg) && CBC::FPRRegClass.contains(SrcReg)) {
    BuildMI(MBB, I, DL, get(CBC::FMOV64rr), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }
  // Bitcast-style moves between the integer and float banks.
  if (CBC::FPRRegClass.contains(DestReg) && CBC::GPRRegClass.contains(SrcReg)) {
    BuildMI(MBB, I, DL, get(CBC::I2F), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }
  if (CBC::GPRRegClass.contains(DestReg) && CBC::FPRRegClass.contains(SrcReg)) {
    BuildMI(MBB, I, DL, get(CBC::F2I), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }
  report_fatal_error("CBC copyPhysReg: unsupported register class");
}

void CBCInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MI,
                                       Register SrcReg, bool IsKill,
                                       int FrameIndex,
                                       const TargetRegisterClass *RC,
                                       Register, MachineInstr::MIFlag Flags) const {
  unsigned Opc = RC == &CBC::FPRRegClass ? CBC::STUF64 : CBC::STU64;
  BuildMI(MBB, MI, DebugLoc(), get(Opc))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FrameIndex)
      .setMIFlags(Flags);
}

void CBCInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator MI,
                                        Register DestReg, int FrameIndex,
                                        const TargetRegisterClass *RC, Register,
                                        unsigned,
                                        MachineInstr::MIFlag Flags) const {
  unsigned Opc = RC == &CBC::FPRRegClass ? CBC::LDUF64 : CBC::LDU64;
  BuildMI(MBB, MI, DebugLoc(), get(Opc), DestReg)
      .addFrameIndex(FrameIndex)
      .setMIFlags(Flags);
}

static bool isCBCBranch(const MachineInstr &MI) {
  unsigned Opc = MI.getOpcode();
  return Opc == CBC::JMP || Opc == CBC::BCC64 || Opc == CBC::BCCF;
}

static bool analyzeCBCCondBranch(const MachineInstr &MI,
                                 MachineBasicBlock *&TBB,
                                 SmallVectorImpl<MachineOperand> &Cond) {
  if (MI.getOpcode() == CBC::BCC64) {
    TBB = MI.getOperand(3).getMBB();
    Cond.push_back(MI.getOperand(0));
    Cond.push_back(MI.getOperand(1));
    Cond.push_back(MI.getOperand(2));
    return true;
  }
  if (MI.getOpcode() == CBC::BCCF) {
    // Cond: cc, l, r, f32 — insertBranch distinguishes this from BCC64 by size.
    TBB = MI.getOperand(4).getMBB();
    Cond.push_back(MI.getOperand(0));
    Cond.push_back(MI.getOperand(1));
    Cond.push_back(MI.getOperand(2));
    Cond.push_back(MI.getOperand(3));
    return true;
  }
  return false;
}

bool CBCInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                 MachineBasicBlock *&TBB,
                                 MachineBasicBlock *&FBB,
                                 SmallVectorImpl<MachineOperand> &Cond,
                                 bool) const {
  TBB = FBB = nullptr;
  Cond.clear();
  MachineBasicBlock::iterator I = MBB.end();
  if (I == MBB.begin())
    return false;
  do {
    --I;
  } while (I->isDebugInstr() && I != MBB.begin());
  if (!I->isTerminator())
    return false;
  if (analyzeCBCCondBranch(*I, TBB, Cond))
    return false;
  if (I->getOpcode() != CBC::JMP)
    return true;
  FBB = I->getOperand(0).getMBB();
  if (I == MBB.begin()) {
    TBB = FBB;
    FBB = nullptr;
    return false;
  }
  MachineBasicBlock::iterator J = I;
  do {
    --J;
  } while (J->isDebugInstr() && J != MBB.begin());
  if (!analyzeCBCCondBranch(*J, TBB, Cond)) {
    TBB = FBB;
    FBB = nullptr;
    return false;
  }
  return false;
}

unsigned CBCInstrInfo::removeBranch(MachineBasicBlock &MBB, int *) const {
  unsigned Removed = 0;
  while (!MBB.empty() && isCBCBranch(MBB.instr_back())) {
    MBB.instr_back().eraseFromParent();
    ++Removed;
  }
  return Removed;
}

unsigned CBCInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *TBB,
                                    MachineBasicBlock *FBB,
                                    ArrayRef<MachineOperand> Cond,
                                    const DebugLoc &DL, int *) const {
  if (Cond.empty()) {
    assert(!FBB && "unconditional branch has two destinations");
    BuildMI(&MBB, DL, get(CBC::JMP)).addMBB(TBB);
    return 1;
  }
  if (Cond.size() == 4) {
    BuildMI(&MBB, DL, get(CBC::BCCF))
        .addImm(Cond[0].getImm())
        .addReg(Cond[1].getReg())
        .addReg(Cond[2].getReg())
        .addImm(Cond[3].getImm())
        .addMBB(TBB);
  } else {
    assert(Cond.size() == 3 && "unexpected CBC branch condition");
    BuildMI(&MBB, DL, get(CBC::BCC64))
        .addImm(Cond[0].getImm())
        .addReg(Cond[1].getReg())
        .addReg(Cond[2].getReg())
        .addMBB(TBB);
  }
  if (!FBB)
    return 1;
  BuildMI(&MBB, DL, get(CBC::JMP)).addMBB(FBB);
  return 2;
}
