//===- DeferredNativeLink.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stash a second link invocation across CommonLinkerContext::destroy so CBC
// wrap can emit a host DSO (-shared, re-enter ld.lld) or host PIE (spawn host
// clang) after the CBC Ctx is gone.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_DEFERREDNATIVELINK_H
#define LLD_COMMON_DEFERREDNATIVELINK_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace lld {

/// Record argv for a native ld.lld link to run after the current Ctx is
/// destroyed. Copies all strings; safe to call while a Ctx is live.
void setDeferredNativeLink(llvm::ArrayRef<const char *> argv,
                           llvm::StringRef tmpObjectToRemove);

/// Record argv for an external process (host clang PIE link) after Ctx is
/// gone. argv[0] must be an absolute path to the executable.
void setDeferredExternalLink(llvm::ArrayRef<const char *> argv,
                             llvm::StringRef tmpObjectToRemove);

/// If a deferred link is pending, fills OutArgv / TmpObjectToRemove /
/// External and clears the pending state. Returns false if nothing pending.
bool takeDeferredNativeLink(llvm::SmallVectorImpl<const char *> &OutArgv,
                            std::string &TmpObjectToRemove, bool &External);

} // namespace lld

#endif
