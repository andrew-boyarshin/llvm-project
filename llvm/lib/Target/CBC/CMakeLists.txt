add_llvm_component_group(CBC)

set(LLVM_TARGET_DEFINITIONS CBC.td)
tablegen(LLVM CBCGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM CBCGenCallingConv.inc -gen-callingconv)
tablegen(LLVM CBCGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM CBCGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM CBCGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM CBCGenSubtargetInfo.inc -gen-subtarget)
add_public_tablegen_target(CBCCommonTableGen)

add_llvm_target(CBCCodeGen
  CBCAsmPrinter.cpp
  CBCFileWriter.cpp
  CBCFrameLowering.cpp
  CBCISelDAGToDAG.cpp
  CBCISelLowering.cpp
  CBCInstrInfo.cpp
  CBCLowerEH.cpp
  CBCLowerGlobals.cpp
  CBCLowerSjLj.cpp
  CBCRegisterInfo.cpp
  CBCSubtarget.cpp
  CBCSynthesizeEntry.cpp
  CBCTargetMachine.cpp
  CBCTargetObjectFile.cpp

  LINK_COMPONENTS
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  IPO
  MC
  CBCDesc
  CBCInfo
  SelectionDAG
  Support
  Target
  TargetParser

  ADD_TO_COMPONENT CBC
)

add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
