add_llvm_component_library(LLVMCBCAsmParser
  CBCAsmParser.cpp
  LINK_COMPONENTS
  CBCDesc
  CBCInfo
  MC
  MCParser
  Support
  ADD_TO_COMPONENT CBC
)
