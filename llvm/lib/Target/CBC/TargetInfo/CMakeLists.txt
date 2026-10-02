add_llvm_component_library(LLVMCBCInfo
  CBCTargetInfo.cpp
  LINK_COMPONENTS MC Support
  ADD_TO_COMPONENT CBC
)
