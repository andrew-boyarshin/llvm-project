//===- DeferredNativeLink.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stash a second ld.lld invocation across CommonLinkerContext::destroy so CBC
// wrap can run a fresh ELF -shared link after the CBC Ctx is gone.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_DEFERREDNATIVELINK_H
#define LLD_COMMON_DEFERREDNATIVELINK_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace lld {

/// Record argv for a native link to run after the current Ctx is destroyed.
/// Copies all strings; safe to call while a Ctx is live.
void setDeferredNativeLink(llvm::ArrayRef<const char *> argv,
                           llvm::StringRef tmpObjectToRemove);

/// If set, fills OutArgv / TmpObjectToRemove and clears the pending state.
bool takeDeferredNativeLink(llvm::SmallVectorImpl<const char *> &OutArgv,
                            std::string &TmpObjectToRemove);

} // namespace lld

#endif
