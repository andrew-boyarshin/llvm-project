add_llvm_component_library(LLVMCBCDesc
  CBCAsmBackend.cpp
  CBCAsmInfo.cpp
  CBCEncoding.cpp
  CBCMCCodeEmitter.cpp
  CBCMCTargetDesc.cpp
  CBCInstPrinter.cpp
  LINK_COMPONENTS CBCInfo MC Support TargetParser
  ADD_TO_COMPONENT CBC
)
