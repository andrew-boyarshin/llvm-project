# CBC crt, per flavour. Directory name "cbc.<host-triple>" (the Android API level is
# not part of it; see clang ToolChains/CBC.h and agents/28-aarch64-flavour-plan.md).
#
#   CBC_FLAVORS  host triples to build the crt for (default: x86_64 Linux only).
#                aarch64-unknown-linux-android needs the AArch64 CBC backend
#                (not implemented yet, agents/28 P1) and ANDROID_NDK_ROOT.
set(CBC_FLAVORS x86_64-unknown-linux-gnu CACHE STRING
    "CBC flavours (host triples) to build the crt for")
set(CBC_ANDROID_API 23 CACHE STRING "Android API level of the Android CBC flavour")
set(ANDROID_NDK_ROOT "" CACHE PATH "Android NDK, needed for aarch64-unknown-linux-android")

set(CBC_CRT_TARGETS)

# cbc_add_flavor(<host-triple> <compile-target> <dir-name> <with-main> <with-x86-headers> [clang flags...])
function(cbc_add_flavor HOST_TRIPLE COMPILE_TARGET TARGET_DIR WITH_MAIN WITH_X86_HEADERS)
  set(EXTRA_FLAGS ${ARGN})
  set(LIB_DIR ${LLVM_LIBRARY_DIR}/${TARGET_DIR})
  set(NATIVE_DIR ${LIB_DIR}/native)
  set(CRT_BC ${LIB_DIR}/crt-cbc.bc)
  set(STUB_BC ${LIB_DIR}/cbc_can_catch_stub.bc)
  set(MAIN_O ${NATIVE_DIR}/cbc-main.o)
  set(MAIN_A ${NATIVE_DIR}/libcbc-main.a)
  set(CRT ${CMAKE_CURRENT_SOURCE_DIR}/crt)
  set(CLANG ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang)
  add_custom_command(
    OUTPUT ${CRT_BC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIB_DIR}
    COMMAND ${CLANG} --target=${COMPILE_TARGET} -fcbc -O2 -emit-llvm -c ${EXTRA_FLAGS}
            -I${CRT} ${CRT}/crt-cbc.c -o ${CRT_BC}
    DEPENDS clang ${CRT}/crt-cbc.c ${CRT}/unwind.h
    VERBATIM)
  add_custom_command(
    OUTPUT ${STUB_BC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIB_DIR}
    COMMAND ${CLANG} --target=${COMPILE_TARGET} -fcbc -O2 -emit-llvm -c ${EXTRA_FLAGS}
            ${CRT}/cbc_can_catch_stub.c -o ${STUB_BC}
    DEPENDS clang ${CRT}/cbc_can_catch_stub.c
    VERBATIM)
  set(OUTPUTS ${CRT_BC} ${STUB_BC})
  if(WITH_MAIN)
    # Host-triple main for CBC PIE executables (no -fcbc). Not built for Android:
    # Android is shared-library only (agents/28 D3).
    add_custom_command(
      OUTPUT ${MAIN_A}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${NATIVE_DIR}
      COMMAND ${CLANG} --target=${COMPILE_TARGET} -O2 -c ${EXTRA_FLAGS}
              -I${CRT} ${CRT}/cbc-main.c -o ${MAIN_O}
      COMMAND ${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-ar rcs ${MAIN_A} ${MAIN_O}
      DEPENDS clang llvm-ar ${CRT}/cbc-main.c ${CRT}/cj-interface.h
      VERBATIM)
    list(APPEND OUTPUTS ${MAIN_A})
  endif()
  string(REPLACE "." "_" TARGET_NAME crt-${TARGET_DIR})
  add_custom_target(${TARGET_NAME} ALL DEPENDS ${OUTPUTS})
  set(CBC_CRT_TARGETS ${CBC_CRT_TARGETS} ${TARGET_NAME} PARENT_SCOPE)

  install(FILES ${CRT_BC} ${STUB_BC} DESTINATION lib/${TARGET_DIR})
  if(WITH_MAIN)
    install(FILES ${MAIN_A} DESTINATION lib/${TARGET_DIR}/native)
  endif()
  install(FILES ${CRT}/unwind.h DESTINATION include/${TARGET_DIR})
  if(WITH_X86_HEADERS)
    install(FILES ${CRT}/include/bits/floatn.h DESTINATION include/${TARGET_DIR}/bits)
    install(FILES
              ${CRT}/include/immintrin.h
              ${CRT}/include/x86intrin.h
              ${CRT}/include/x86gprintrin.h
              ${CRT}/include/mmintrin.h
              ${CRT}/include/xmmintrin.h
              ${CRT}/include/emmintrin.h
              ${CRT}/include/pmmintrin.h
              ${CRT}/include/cpuid.h
              ${CRT}/include/arm_neon.h
            DESTINATION include/${TARGET_DIR})
  endif()
endfunction()

foreach(FLAVOR ${CBC_FLAVORS})
  if(FLAVOR STREQUAL "x86_64-unknown-linux-gnu")
    cbc_add_flavor(${FLAVOR} ${FLAVOR} cbc.${FLAVOR} TRUE TRUE)
  elseif(FLAVOR STREQUAL "aarch64-unknown-linux-android")
    if(NOT ANDROID_NDK_ROOT)
      message(FATAL_ERROR "CBC flavour ${FLAVOR} needs -DANDROID_NDK_ROOT=<ndk>")
    endif()
    set(NDK_SYSROOT ${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/sysroot)
    # Headers beyond unwind.h (per-flavour crt/include split) come later, agents/28 W3.1.
    cbc_add_flavor(${FLAVOR} aarch64-linux-android${CBC_ANDROID_API}
                   cbc.${FLAVOR} FALSE FALSE --sysroot=${NDK_SYSROOT})
  else()
    message(FATAL_ERROR "Unknown CBC flavour '${FLAVOR}' "
            "(x86_64-unknown-linux-gnu | aarch64-unknown-linux-android)")
  endif()
endforeach()

add_custom_target(crt-cbc ALL DEPENDS ${CBC_CRT_TARGETS})
if(TARGET lld)
  add_dependencies(lld crt-cbc)
endif()
