add_llvm_component_library(LLVMCBCDisassembler
  CBCDisassembler.cpp
  LINK_COMPONENTS
  CBCDesc
  CBCInfo
  MC
  MCDisassembler
  Support
  ADD_TO_COMPONENT CBC
)
