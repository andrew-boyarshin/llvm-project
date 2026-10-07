//===- CBCLink.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared CBC emit pipeline. Called by ELF ld.lld --cbc (and later Mach-O).
// Does not include lld/ELF headers. Archive extraction uses the cbc-ld
// 64-round loop, not ELF lazy BitcodeFile symbols.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CBC_CBCLINK_H
#define LLD_CBC_CBCLINK_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/TargetParser/Triple.h"

namespace lld {
namespace cbc {

struct CBCLinkRequest {
  // crt first (via ELF addFile), then positional / --whole-archive bitcode.
  llvm::ArrayRef<llvm::MemoryBufferRef> wholeModules;
  // Bitcode .a paths for the 64-round lazy extraction loop.
  llvm::ArrayRef<llvm::StringRef> lazyArchives;
  // Opaque dlopen tokens (SONAMEs, then unresolved -l stems).
  llvm::ArrayRef<llvm::StringRef> aotDeps;
  llvm::StringRef outputPath;
  // From the first bitcode module; do not hardcode a CBC arch triple.
  llvm::Triple triple;
};

/// IR-link, O2, policy checks, CBC codegen. Returns true on success.
bool link(const CBCLinkRequest &request);

} // namespace cbc
} // namespace lld

#endif
