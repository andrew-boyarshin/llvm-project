set(LLVM_LINK_COMPONENTS
  AllTargetsAsmParsers
  AllTargetsCodeGens
  AllTargetsDescs
  AllTargetsInfos
  Analysis
  BitReader
  Core
  Object
  IPO
  IRReader
  Linker
  MC
  Passes
  ScalarOpts
  Support
  Target
  TargetParser
  TransformUtils
  )

add_llvm_tool(cbc-ld cbc-ld.cpp)

set(CBC_LIB_DIR ${LLVM_LIBRARY_DIR}/cbc)
set(CBC_CRT_BC ${CBC_LIB_DIR}/crt-cbc.bc)
set(CBC_CATCH_STUB_BC ${CBC_LIB_DIR}/cbc_can_catch_stub.bc)
add_custom_command(
  OUTPUT ${CBC_CRT_BC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CBC_LIB_DIR}
  COMMAND ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
          --target=cbc_x86_64-unknown-linux-gnu -O2 -emit-llvm -c
          -I${CMAKE_CURRENT_SOURCE_DIR}/crt
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/crt-cbc.c -o ${CBC_CRT_BC}
  DEPENDS clang ${CMAKE_CURRENT_SOURCE_DIR}/crt/crt-cbc.c
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/unwind.h
  VERBATIM)
add_custom_command(
  OUTPUT ${CBC_CATCH_STUB_BC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CBC_LIB_DIR}
  COMMAND ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
          --target=cbc_x86_64-unknown-linux-gnu -O2 -emit-llvm -c
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/cbc_can_catch_stub.c
          -o ${CBC_CATCH_STUB_BC}
  DEPENDS clang ${CMAKE_CURRENT_SOURCE_DIR}/crt/cbc_can_catch_stub.c
  VERBATIM)
add_custom_target(crt-cbc DEPENDS ${CBC_CRT_BC} ${CBC_CATCH_STUB_BC})
add_dependencies(cbc-ld crt-cbc)
install(FILES ${CBC_CRT_BC} ${CBC_CATCH_STUB_BC} DESTINATION lib/cbc)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/crt/unwind.h DESTINATION lib/cbc/include)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/bits/floatn.h
        DESTINATION lib/cbc/include/bits)
install(FILES
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/immintrin.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/x86intrin.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/x86gprintrin.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/mmintrin.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/xmmintrin.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/emmintrin.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/pmmintrin.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/cpuid.h
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/include/arm_neon.h
        DESTINATION lib/cbc/include)
