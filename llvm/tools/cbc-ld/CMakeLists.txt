set(CBC_LIB_DIR ${LLVM_LIBRARY_DIR}/cbc)
set(CBC_NATIVE_DIR ${CBC_LIB_DIR}/native)
set(CBC_CRT_BC ${CBC_LIB_DIR}/crt-cbc.bc)
set(CBC_CATCH_STUB_BC ${CBC_LIB_DIR}/cbc_can_catch_stub.bc)
set(CBC_MAIN_O ${CBC_NATIVE_DIR}/cbc-main.o)
set(CBC_MAIN_A ${CBC_NATIVE_DIR}/libcbc-main.a)
add_custom_command(
  OUTPUT ${CBC_CRT_BC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CBC_LIB_DIR}
  COMMAND ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
          --target=x86_64-unknown-linux-gnu -fcbc -O2 -emit-llvm -c
          -I${CMAKE_CURRENT_SOURCE_DIR}/crt
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/crt-cbc.c -o ${CBC_CRT_BC}
  DEPENDS clang ${CMAKE_CURRENT_SOURCE_DIR}/crt/crt-cbc.c
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/unwind.h
  VERBATIM)
add_custom_command(
  OUTPUT ${CBC_CATCH_STUB_BC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CBC_LIB_DIR}
  COMMAND ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
          --target=x86_64-unknown-linux-gnu -fcbc -O2 -emit-llvm -c
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/cbc_can_catch_stub.c
          -o ${CBC_CATCH_STUB_BC}
  DEPENDS clang ${CMAKE_CURRENT_SOURCE_DIR}/crt/cbc_can_catch_stub.c
  VERBATIM)
# Host-triple main for CBC PIE executables (no -fcbc).
add_custom_command(
  OUTPUT ${CBC_MAIN_A}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CBC_NATIVE_DIR}
  COMMAND ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
          --target=x86_64-unknown-linux-gnu -O2 -c
          -I${CMAKE_CURRENT_SOURCE_DIR}/crt
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/cbc-main.c -o ${CBC_MAIN_O}
  COMMAND ${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-ar rcs ${CBC_MAIN_A} ${CBC_MAIN_O}
  DEPENDS clang llvm-ar ${CMAKE_CURRENT_SOURCE_DIR}/crt/cbc-main.c
          ${CMAKE_CURRENT_SOURCE_DIR}/crt/cj-interface.h
  VERBATIM)
add_custom_target(crt-cbc ALL DEPENDS ${CBC_CRT_BC} ${CBC_CATCH_STUB_BC}
                                    ${CBC_MAIN_A})
if(TARGET lld)
  add_dependencies(lld crt-cbc)
endif()
install(FILES ${CBC_CRT_BC} ${CBC_CATCH_STUB_BC} DESTINATION lib/cbc)
install(FILES ${CBC_MAIN_A} DESTINATION lib/cbc/native)
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
