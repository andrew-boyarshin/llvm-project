include "llvm/Target/Target.td"
include "CBCRegisterInfo.td"
include "CBCCallingConv.td"
include "CBCInstrInfo.td"

def CBCInstrInfo : InstrInfo;

def CBCAsmWriter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
}

def CBC : Target {
  let InstructionSet = CBCInstrInfo;
}

defm : RemapAllTargetPseudoPointerOperands<GPR>;

def : Processor<"generic", NoItineraries, []>;
