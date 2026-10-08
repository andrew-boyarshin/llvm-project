//===-- CBCABI.h - CBC module-flag key --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGETPARSER_CBCABI_H
#define LLVM_TARGETPARSER_CBCABI_H

#include "llvm/ADT/StringRef.h"

namespace llvm {

/// Module flag key whose presence marks CBC bitcode (compiled with -fcbc).
/// Value is an integer 1; ABI identity is the module target triple.
inline constexpr StringRef CBCModuleFlagKey = "CBC";

} // namespace llvm

#endif // LLVM_TARGETPARSER_CBCABI_H
