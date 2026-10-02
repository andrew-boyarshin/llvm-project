#include "MCTargetDesc/CBCMCTargetDesc.h"
#include "TargetInfo/CBCTargetInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

namespace {
class CBCOperand : public MCParsedAsmOperand {
  enum Kind { Token, Reg, Imm } K;
  SMLoc Start, End;
  StringRef Tok;
  MCRegister Register;
  int64_t Value = 0;

public:
  CBCOperand(Kind K, SMLoc S, SMLoc E) : K(K), Start(S), End(E) {}
  void print(raw_ostream &O, const MCAsmInfo &) const override { O << "<cbc>"; }
  bool isToken() const override { return K == Token; }
  bool isImm() const override { return K == Imm; }
  bool isReg() const override { return K == Reg; }
  bool isMem() const override { return false; }
  MCRegister getReg() const override { return Register; }
  SMLoc getStartLoc() const override { return Start; }
  SMLoc getEndLoc() const override { return End; }
  StringRef getToken() const { return Tok; }
  int64_t getImm() const { return Value; }

  static std::unique_ptr<CBCOperand> createToken(StringRef T, SMLoc S) {
    auto Op = std::make_unique<CBCOperand>(Token, S, S);
    Op->Tok = T;
    return Op;
  }
  static std::unique_ptr<CBCOperand> createReg(MCRegister R, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<CBCOperand>(Reg, S, E);
    Op->Register = R;
    return Op;
  }
  static std::unique_ptr<CBCOperand> createImm(int64_t V, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<CBCOperand>(Imm, S, E);
    Op->Value = V;
    return Op;
  }
};

class CBCAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;
  bool parseRegisterName(MCRegister &Reg);

public:
  CBCAsmParser(const MCSubtargetInfo &STI, MCAsmParser &P, const MCInstrInfo &MII)
      : MCTargetAsmParser(STI, MII), Parser(P) {}

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override {
    StartLoc = Parser.getTok().getLoc();
    if (!parseRegisterName(Reg))
      return true;
    EndLoc = Parser.getTok().getLoc();
    return false;
  }
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override {
    if (Parser.getTok().isNot(AsmToken::Identifier))
      return ParseStatus::NoMatch;
    StartLoc = Parser.getTok().getLoc();
    if (!parseRegisterName(Reg))
      return ParseStatus::NoMatch;
    EndLoc = Parser.getTok().getLoc();
    return ParseStatus::Success;
  }
  bool parseInstruction(ParseInstructionInfo &, StringRef Name, SMLoc NameLoc,
                        OperandVector &Operands) override;
  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &, bool) override;
  void convertToMapAndConstraints(unsigned, const OperandVector &) override {}
};

bool CBCAsmParser::parseRegisterName(MCRegister &Reg) {
  if (Parser.getTok().isNot(AsmToken::Identifier))
    return false;
  StringRef N = Parser.getTok().getString();
  static const struct {
    const char *Name;
    uint16_t Reg;
  } Table[] = {
      {"irz", CBC::IRZ},     {"ir1", CBC::IR1},     {"ir2", CBC::IR2},
      {"ir3", CBC::IR3},     {"ir4", CBC::IR4},     {"ir5", CBC::IR5},
      {"ir6", CBC::IR6},     {"ir7", CBC::IR7},     {"ir8", CBC::IR8},
      {"ir9", CBC::IR9},     {"ir10", CBC::IR10},   {"ir11", CBC::IR11},
      {"ir12", CBC::IR12},   {"ir13", CBC::IR13},   {"iracc", CBC::IRACC},
      {"fr0", CBC::FR0},     {"fr1", CBC::FR1},     {"fr2", CBC::FR2},
      {"fr3", CBC::FR3},     {"fr4", CBC::FR4},     {"fr5", CBC::FR5},
      {"fr6", CBC::FR6},     {"fr7", CBC::FR7},     {"fr8", CBC::FR8},
      {"fr9", CBC::FR9},     {"fr10", CBC::FR10},   {"fr11", CBC::FR11},
      {"fr12", CBC::FR12},   {"fr13", CBC::FR13},   {"fr14", CBC::FR14},
      {"fr15", CBC::FR15},
  };
  for (const auto &E : Table) {
    if (N == E.Name) {
      Reg = E.Reg;
      Parser.Lex();
      return true;
    }
  }
  return false;
}

bool CBCAsmParser::parseInstruction(ParseInstructionInfo &, StringRef Name,
                                    SMLoc NameLoc, OperandVector &Operands) {
  // The lexer splits "mov.W64" into identifier, dot, identifier.
  StringRef Full = Name;
  while (Parser.getTok().is(AsmToken::Dot)) {
    Parser.Lex();
    if (Parser.getTok().isNot(AsmToken::Identifier))
      return true;
    const char *Begin = Name.data();
    const char *End = Parser.getTok().getString().data() +
                      Parser.getTok().getString().size();
    Full = StringRef(Begin, End - Begin);
    Parser.Lex();
  }
  Operands.push_back(CBCOperand::createToken(Full, NameLoc));
  if (Parser.getTok().is(AsmToken::EndOfStatement))
    return false;
  while (true) {
    SMLoc S = Parser.getTok().getLoc();
    MCRegister Reg;
    if (parseRegisterName(Reg)) {
      Operands.push_back(CBCOperand::createReg(Reg, S, Parser.getTok().getLoc()));
    } else if (Parser.getTok().is(AsmToken::Integer) ||
               Parser.getTok().is(AsmToken::Minus)) {
      int64_t Sign = 1;
      if (Parser.getTok().is(AsmToken::Minus)) {
        Sign = -1;
        Parser.Lex();
      }
      if (!Parser.getTok().is(AsmToken::Integer))
        return true;
      int64_t V = Sign * Parser.getTok().getIntVal();
      Parser.Lex();
      Operands.push_back(CBCOperand::createImm(V, S, Parser.getTok().getLoc()));
    } else {
      return true;
    }
    if (Parser.getTok().is(AsmToken::EndOfStatement))
      return false;
    if (!Parser.getTok().is(AsmToken::Comma))
      return true;
    Parser.Lex();
  }
}

bool CBCAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &,
                                           OperandVector &Operands,
                                           MCStreamer &Out, uint64_t &, bool) {
  if (Operands.empty() || !Operands[0]->isToken())
    return true;
  StringRef Name = static_cast<CBCOperand &>(*Operands[0]).getToken();
  MCInst Inst;
  if (Name.equals_insensitive("mov.W64") && Operands.size() == 3 &&
      Operands[1]->isReg()) {
    if (Operands[2]->isReg())
      Inst.setOpcode(CBC::MOV64rr);
    else if (Operands[2]->isImm())
      Inst.setOpcode(CBC::MOV64ri);
    else
      return Error(IDLoc, "expected a register or immediate");
    Inst.addOperand(MCOperand::createReg(Operands[1]->getReg()));
    if (Operands[2]->isReg())
      Inst.addOperand(MCOperand::createReg(Operands[2]->getReg()));
    else
      Inst.addOperand(
          MCOperand::createImm(static_cast<CBCOperand &>(*Operands[2]).getImm()));
  } else if (Name.equals_insensitive("ret.W64")) {
    Inst.setOpcode(CBC::RET);
  } else {
    return Error(IDLoc, "unknown CBC instruction");
  }
  Out.emitInstruction(Inst, getSTI());
  return false;
}
} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeCBCAsmParser() {
  RegisterMCAsmParser<CBCAsmParser> X(getTheCBCTarget());
}
