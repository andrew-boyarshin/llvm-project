#include "CBCEncoding.h"
#include "CBCMCTargetDesc.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"

using namespace llvm;

unsigned llvm::cbcEncodeReg(MCRegister Reg) {
  switch (Reg.id()) {
  case CBC::IRZ: return 0;
  case CBC::IR1: return 1;
  case CBC::IR2: return 2;
  case CBC::IR3: return 3;
  case CBC::IR4: return 4;
  case CBC::IR5: return 5;
  case CBC::IR6: return 6;
  case CBC::IR7: return 7;
  case CBC::IR8: return 8;
  case CBC::IR9: return 9;
  case CBC::IR10: return 10;
  case CBC::IR11: return 11;
  case CBC::IR12: return 12;
  case CBC::IR13: return 13;
  case CBC::IRACC: return 14;
  case CBC::FR0: return 0;
  case CBC::FR1: return 1;
  case CBC::FR2: return 2;
  case CBC::FR3: return 3;
  case CBC::FR4: return 4;
  case CBC::FR5: return 5;
  case CBC::FR6: return 6;
  case CBC::FR7: return 7;
  case CBC::FR8: return 8;
  case CBC::FR9: return 9;
  case CBC::FR10: return 10;
  case CBC::FR11: return 11;
  case CBC::FR12: return 12;
  case CBC::FR13: return 13;
  case CBC::FR14: return 14;
  case CBC::FR15: return 15;
  default:
    report_fatal_error("CBC encoder: unsupported register");
  }
}

static void appendSLEB(SmallVectorImpl<char> &CB, int64_t V) {
  uint8_t Buf[16];
  unsigned Len = encodeSLEB128(V, Buf);
  CB.append(Buf, Buf + Len);
}

static void appendBinImm(SmallVectorImpl<char> &CB, uint8_t Opcode, unsigned Op,
                         unsigned D, unsigned L, int64_t Imm) {
  unsigned Lo = static_cast<unsigned>(Imm) & 0xF;
  CB.push_back(Opcode);
  CB.push_back(static_cast<char>((Op << 4) | (D & 0xF)));
  CB.push_back(static_cast<char>((L << 4) | Lo));
  appendSLEB(CB, Imm >> 4);
}

static void appendMem(SmallVectorImpl<char> &CB, uint8_t Opcode, unsigned A,
                      unsigned B, unsigned Kind, int64_t Disp) {
  unsigned Lo = static_cast<unsigned>(Disp) & 0xF;
  CB.push_back(Opcode);
  CB.push_back(static_cast<char>((A << 4) | (B & 0xF)));
  CB.push_back(static_cast<char>((Kind << 4) | Lo));
  appendSLEB(CB, Disp >> 4);
}

static MCRegister irFromEnc(unsigned E) {
  static const uint16_t T[] = {CBC::IRZ,  CBC::IR1,  CBC::IR2,   CBC::IR3,
                               CBC::IR4,  CBC::IR5,  CBC::IR6,   CBC::IR7,
                               CBC::IR8,  CBC::IR9,  CBC::IR10,  CBC::IR11,
                               CBC::IR12, CBC::IR13, CBC::IRACC};
  if (E >= std::size(T))
    return MCRegister();
  return T[E];
}

static unsigned encReg(MCRegister Reg) { return cbcEncodeReg(Reg); }

void llvm::encodeCBCInst(const MCInst &MI, SmallVectorImpl<char> &CB) {
  switch (MI.getOpcode()) {
  case CBC::MOV64rr: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned S = encReg(MI.getOperand(1).getReg());
    CB.push_back(0x15);
    CB.push_back(static_cast<char>((D << 4) | (S & 0xF)));
    break;
  }
  case CBC::MOV64ri: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    int64_t Imm = MI.getOperand(1).getImm();
    unsigned Lo = static_cast<unsigned>(Imm) & 0xF;
    CB.push_back(0x17);
    CB.push_back(static_cast<char>((D << 4) | Lo));
    appendSLEB(CB, Imm >> 4);
    break;
  }
  case CBC::RET:
    CB.push_back(0x45);
    CB.push_back(0x11);
    break;
  case CBC::FRET:
    CB.push_back(0x45);
    CB.push_back(0x30); // FRet64 FR0
    break;
  case CBC::FMOV64i: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    uint64_t Bits = static_cast<uint64_t>(MI.getOperand(1).getImm());
    CB.push_back(0x1C);
    CB.push_back(static_cast<char>(D & 0xF));
    for (int I = 0; I < 8; ++I)
      CB.push_back(static_cast<char>((Bits >> (8 * I)) & 0xFF));
    break;
  }
  case CBC::FMOV32i: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    uint32_t Bits = static_cast<uint32_t>(MI.getOperand(1).getImm());
    CB.push_back(0x1B);
    CB.push_back(static_cast<char>(D & 0xF));
    for (int I = 0; I < 4; ++I)
      CB.push_back(static_cast<char>((Bits >> (8 * I)) & 0xFF));
    break;
  }
  case CBC::ADD32rr:
  case CBC::ADD64rr:
  case CBC::SUB32rr:
  case CBC::SUB64rr:
  case CBC::AND32rr:
  case CBC::AND64rr:
  case CBC::OR32rr:
  case CBC::OR64rr:
  case CBC::XOR32rr:
  case CBC::XOR64rr:
  case CBC::MUL32rr:
  case CBC::MUL64rr:
  case CBC::SHL32rr:
  case CBC::SHL64rr:
  case CBC::SRL32rr:
  case CBC::SRL64rr:
  case CBC::SRA32rr:
  case CBC::SRA64rr:
  case CBC::SDIV64rr:
  case CBC::SREM64rr:
  case CBC::UDIV64rr:
  case CBC::UREM64rr:
  case CBC::SDIV32rr:
  case CBC::SREM32rr:
  case CBC::UDIV32rr:
  case CBC::UREM32rr: {
    unsigned Opc = MI.getOpcode();
    unsigned Op = 0;
    uint8_t Opcode = 0x36;
    if (Opc == CBC::SUB32rr || Opc == CBC::SUB64rr)
      Op = 1;
    else if (Opc == CBC::MUL32rr || Opc == CBC::MUL64rr)
      Op = 2;
    else if (Opc == CBC::AND32rr || Opc == CBC::AND64rr)
      Op = 3;
    else if (Opc == CBC::OR32rr || Opc == CBC::OR64rr)
      Op = 4;
    else if (Opc == CBC::XOR32rr || Opc == CBC::XOR64rr)
      Op = 5;
    else if (Opc == CBC::SRL32rr || Opc == CBC::SRL64rr)
      Op = 10;
    else if (Opc == CBC::SRA32rr || Opc == CBC::SRA64rr)
      Op = 11;
    else if (Opc == CBC::SHL32rr || Opc == CBC::SHL64rr)
      Op = 12;
    else if (Opc == CBC::SDIV64rr || Opc == CBC::SDIV32rr)
      Op = 6;
    else if (Opc == CBC::SREM64rr || Opc == CBC::SREM32rr)
      Op = 7;
    else if (Opc == CBC::UDIV64rr || Opc == CBC::UDIV32rr)
      Op = 8;
    else if (Opc == CBC::UREM64rr || Opc == CBC::UREM32rr)
      Op = 9;
    if (Opc == CBC::ADD32rr || Opc == CBC::SUB32rr || Opc == CBC::AND32rr ||
        Opc == CBC::OR32rr || Opc == CBC::XOR32rr || Opc == CBC::MUL32rr ||
        Opc == CBC::SHL32rr || Opc == CBC::SRL32rr || Opc == CBC::SRA32rr ||
        Opc == CBC::SDIV32rr || Opc == CBC::SREM32rr || Opc == CBC::UDIV32rr ||
        Opc == CBC::UREM32rr)
      Opcode = 0x35;
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned L = encReg(MI.getOperand(1).getReg());
    unsigned R = encReg(MI.getOperand(2).getReg());
    CB.push_back(Opcode);
    CB.push_back(static_cast<char>((Op << 4) | (D & 0xF)));
    CB.push_back(static_cast<char>((L << 4) | (R & 0xF)));
    break;
  }
  case CBC::ADD32ri:
  case CBC::ADD64ri:
  case CBC::AND32ri:
  case CBC::AND64ri:
  case CBC::MUL32ri:
  case CBC::MUL64ri:
  case CBC::SHL32ri:
  case CBC::SHL64ri:
  case CBC::SRL32ri:
  case CBC::SRL64ri:
  case CBC::SRA32ri:
  case CBC::SRA64ri: {
    unsigned Opc = MI.getOpcode();
    unsigned Op = 0;
    if (Opc == CBC::MUL32ri || Opc == CBC::MUL64ri)
      Op = 2;
    else if (Opc == CBC::AND32ri || Opc == CBC::AND64ri)
      Op = 3;
    else if (Opc == CBC::SRL32ri || Opc == CBC::SRL64ri)
      Op = 10;
    else if (Opc == CBC::SRA32ri || Opc == CBC::SRA64ri)
      Op = 11;
    else if (Opc == CBC::SHL32ri || Opc == CBC::SHL64ri)
      Op = 12;
    uint8_t Opcode =
        (Opc == CBC::ADD32ri || Opc == CBC::AND32ri || Opc == CBC::MUL32ri ||
         Opc == CBC::SHL32ri || Opc == CBC::SRL32ri || Opc == CBC::SRA32ri)
            ? 0x37
            : 0x38;
    appendBinImm(CB, Opcode, Op, encReg(MI.getOperand(0).getReg()),
                 encReg(MI.getOperand(1).getReg()), MI.getOperand(2).getImm());
    break;
  }
  case CBC::FMOV64rr: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned S = encReg(MI.getOperand(1).getReg());
    CB.push_back(0x1A);
    CB.push_back(static_cast<char>((D << 4) | (S & 0xF)));
    break;
  }
  case CBC::FBIN64:
  case CBC::FBIN32: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned Op = static_cast<unsigned>(MI.getOperand(1).getImm());
    unsigned L = encReg(MI.getOperand(2).getReg());
    unsigned R = encReg(MI.getOperand(3).getReg());
    CB.push_back(MI.getOpcode() == CBC::FBIN32 ? 0x4A : 0x4B);
    CB.push_back(static_cast<char>((Op << 4) | (D & 0xF)));
    CB.push_back(static_cast<char>((L << 4) | (R & 0xF)));
    break;
  }
  case CBC::FUNARY64:
  case CBC::FUNARY32: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned Op = static_cast<unsigned>(MI.getOperand(1).getImm());
    unsigned S = encReg(MI.getOperand(2).getReg());
    CB.push_back(MI.getOpcode() == CBC::FUNARY32 ? 0x4A : 0x4B);
    CB.push_back(static_cast<char>((Op << 4) | (D & 0xF)));
    CB.push_back(static_cast<char>(S & 0xF));
    break;
  }
  case CBC::I2F: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned S = encReg(MI.getOperand(1).getReg());
    CB.push_back(0x4B);
    CB.push_back(static_cast<char>((8 << 4) | (D & 0xF)));
    CB.push_back(static_cast<char>(S & 0xF));
    break;
  }
  case CBC::F2I: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned S = encReg(MI.getOperand(1).getReg());
    CB.push_back(0x4B);
    CB.push_back(static_cast<char>((9 << 4) | (D & 0xF)));
    CB.push_back(static_cast<char>(S & 0xF));
    break;
  }
  case CBC::CVT_IF:
  case CBC::CVT_FI:
  case CBC::CVT_FF: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned To = static_cast<unsigned>(MI.getOperand(1).getImm());
    unsigned From = static_cast<unsigned>(MI.getOperand(2).getImm());
    unsigned S = encReg(MI.getOperand(3).getReg());
    CB.push_back(0x39);
    CB.push_back(static_cast<char>((To << 4) | (From & 0xF)));
    CB.push_back(static_cast<char>((D << 4) | (S & 0xF)));
    break;
  }
  case CBC::JMP:
    CB.push_back(0x12);
    CB.push_back(0);
    CB.push_back(0);
    break;
  case CBC::BCC64: {
    unsigned CC = MI.getOperand(0).getImm();
    unsigned L = encReg(MI.getOperand(1).getReg());
    unsigned R = encReg(MI.getOperand(2).getReg());
    CB.push_back(static_cast<char>(0x06 + (CC & 7)));
    CB.push_back(static_cast<char>((L << 4) | (R & 0xF)));
    CB.push_back(0);
    CB.push_back(0);
    break;
  }
  case CBC::BCCF: {
    unsigned CC = MI.getOperand(0).getImm();
    unsigned L = encReg(MI.getOperand(1).getReg());
    unsigned R = encReg(MI.getOperand(2).getReg());
    bool F32 = MI.getOperand(3).getImm() != 0;
    CB.push_back(F32 ? 0x0E : 0x0F);
    CB.push_back(static_cast<char>((CC << 4) | (L & 0xF)));
    CB.push_back(static_cast<char>((R << 4))); // dlo patched later
    CB.push_back(0);
    CB.push_back(0);
    break;
  }
  case CBC::SCCF: {
    unsigned CC = MI.getOperand(1).getImm();
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned L = encReg(MI.getOperand(2).getReg());
    unsigned R = encReg(MI.getOperand(3).getReg());
    bool F32 = MI.getOperand(4).getImm() != 0;
    CB.push_back(F32 ? 0x3E : 0x3F);
    CB.push_back(static_cast<char>(((CC & 0xF) << 4) | (D & 0xF)));
    CB.push_back(static_cast<char>((L << 4) | (R & 0xF)));
    break;
  }
  case CBC::SCC64: {
    unsigned CC = MI.getOperand(1).getImm();
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned L = encReg(MI.getOperand(2).getReg());
    unsigned R = encReg(MI.getOperand(3).getReg());
    CB.push_back(0x3F);
    CB.push_back(static_cast<char>(((CC & 0xF) << 4) | (D & 0xF)));
    CB.push_back(static_cast<char>((L << 4) | (R & 0xF)));
    break;
  }
  case CBC::LDRAW8:
  case CBC::LDRAW32:
  case CBC::LDRAW64:
  case CBC::LDRAWF32:
  case CBC::LDRAWF64: {
    unsigned Kind = MI.getOpcode() == CBC::LDRAW8    ? 0
                    : MI.getOpcode() == CBC::LDRAW32 ? 2
                    : MI.getOpcode() == CBC::LDRAW64 ? 11
                    : MI.getOpcode() == CBC::LDRAWF32 ? 6
                                                     : 7;
    appendMem(CB, 0x60, encReg(MI.getOperand(0).getReg()),
              encReg(MI.getOperand(1).getReg()), Kind,
              MI.getOperand(2).getImm());
    break;
  }
  case CBC::STRAW8:
  case CBC::STRAW32:
  case CBC::STRAW64:
  case CBC::STRAWF32:
  case CBC::STRAWF64: {
    unsigned Kind = MI.getOpcode() == CBC::STRAW8     ? 0
                    : MI.getOpcode() == CBC::STRAW32  ? 2
                    : MI.getOpcode() == CBC::STRAW64  ? 3
                    : MI.getOpcode() == CBC::STRAWF32 ? 6
                                                      : 7;
    appendMem(CB, 0x61, encReg(MI.getOperand(0).getReg()),
              encReg(MI.getOperand(1).getReg()), Kind,
              MI.getOperand(2).getImm());
    break;
  }
  case CBC::STU64: {
    unsigned S = encReg(MI.getOperand(0).getReg());
    unsigned Slot = static_cast<unsigned>(MI.getOperand(1).getImm());
    CB.push_back(0x4F);
    CB.push_back(static_cast<char>((S << 4) | 3));
    CB.push_back(static_cast<char>(Slot & 0xFF));
    CB.push_back(static_cast<char>((Slot >> 8) & 0xFF));
    break;
  }
  case CBC::LDU64:
  case CBC::LDUF64: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned Slot = static_cast<unsigned>(MI.getOperand(1).getImm());
    unsigned Kind = MI.getOpcode() == CBC::LDUF64 ? 7 : 11;
    CB.push_back(0x4E);
    CB.push_back(static_cast<char>((D << 4) | Kind));
    CB.push_back(static_cast<char>(Slot & 0xFF));
    CB.push_back(static_cast<char>((Slot >> 8) & 0xFF));
    break;
  }
  case CBC::STUF64: {
    unsigned S = encReg(MI.getOperand(0).getReg());
    unsigned Slot = static_cast<unsigned>(MI.getOperand(1).getImm());
    CB.push_back(0x4F);
    CB.push_back(static_cast<char>((S << 4) | 7));
    CB.push_back(static_cast<char>(Slot & 0xFF));
    CB.push_back(static_cast<char>((Slot >> 8) & 0xFF));
    break;
  }
  case CBC::LEA_FRAME: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    uint32_t Disp = static_cast<uint32_t>(MI.getOperand(1).getImm());
    CB.push_back(0x45);
    CB.push_back(static_cast<char>((0xA << 4) | D));
    for (unsigned I = 0; I < 4; ++I)
      CB.push_back(static_cast<char>((Disp >> (8 * I)) & 0xFF));
    break;
  }
  case CBC::ALLOCA: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned Sz = encReg(MI.getOperand(1).getReg());
    unsigned Log = static_cast<unsigned>(MI.getOperand(2).getImm());
    CB.push_back(0x45);
    CB.push_back(static_cast<char>((0xB << 4) | D));
    CB.push_back(static_cast<char>((Sz << 4) | (Log & 0xF)));
    break;
  }
  case CBC::STACKSAVE: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    CB.push_back(0x45);
    CB.push_back(static_cast<char>((0xC << 4) | D));
    break;
  }
  case CBC::STACKRESTORE: {
    unsigned S = encReg(MI.getOperand(0).getReg());
    CB.push_back(0x45);
    CB.push_back(static_cast<char>((0xD << 4) | S));
    break;
  }
  case CBC::LD_FCB: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    CB.push_back(0x45);
    CB.push_back(static_cast<char>((0xE << 4) | D));
    break;
  }
  case CBC::CVT_F32_F64: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned S = encReg(MI.getOperand(1).getReg());
    CB.push_back(0x39);
    CB.push_back(static_cast<char>((10 << 4) | 9)); // F64 from F32
    CB.push_back(static_cast<char>((D << 4) | (S & 0xF)));
    break;
  }
  case CBC::NULLCHECK:
    CB.push_back(0x45);
    CB.push_back(static_cast<char>(0x80));
    break;
  case CBC::LEA_IMAGE: {
    unsigned D = encReg(MI.getOperand(0).getReg());
    unsigned S = encReg(MI.getOperand(1).getReg());
    CB.push_back(0x7C);
    CB.push_back(static_cast<char>((D << 4) | (S & 0xF)));
    CB.push_back(0); // field reference 0, the data image
    break;
  }
  default:
    report_fatal_error("CBC encoder has no encoding for this opcode");
  }
}

bool llvm::decodeCBCInst(ArrayRef<uint8_t> Bytes, MCInst &MI, uint64_t &Size) {
  if (Bytes.empty())
    return false;
  unsigned Op = Bytes[0];
  if (Op == 0x15 && Bytes.size() >= 2) {
    unsigned Nib = Bytes[1];
    MI.setOpcode(CBC::MOV64rr);
    MI.addOperand(MCOperand::createReg(irFromEnc(Nib >> 4)));
    MI.addOperand(MCOperand::createReg(irFromEnc(Nib & 0xF)));
    Size = 2;
    return true;
  }
  if (Op == 0x17 && Bytes.size() >= 2) {
    unsigned Nib = Bytes[1];
    unsigned N = 0;
    const char *Err = nullptr;
    int64_t Hi = decodeSLEB128(Bytes.data() + 2, &N, Bytes.end(), &Err);
    if (Err)
      return false;
    int64_t Imm = (Hi << 4) | (Nib & 0xF);
    MI.setOpcode(CBC::MOV64ri);
    MI.addOperand(MCOperand::createReg(irFromEnc(Nib >> 4)));
    MI.addOperand(MCOperand::createImm(Imm));
    Size = 2 + N;
    return true;
  }
  if (Op == 0x45 && Bytes.size() >= 2 && Bytes[1] == 0x11) {
    MI.setOpcode(CBC::RET);
    Size = 2;
    return true;
  }
  return false;
}
