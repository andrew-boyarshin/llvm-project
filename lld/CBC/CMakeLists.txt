add_lld_library(lldCBC
  CBCLink.cpp
  CBCWrap.cpp

  LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Analysis
  BitReader
  CodeGen
  Core
  IPO
  IRReader
  Linker
  MC
  Object
  Passes
  ScalarOpts
  Support
  Target
  TargetParser
  TransformUtils

  LINK_LIBS
  lldCommon

  DEPENDS
  intrinsics_gen
)
