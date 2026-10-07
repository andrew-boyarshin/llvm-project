//===- DeferredNativeLink.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/DeferredNativeLink.h"
#include <vector>

using namespace llvm;
using namespace lld;

namespace {
struct State {
  std::vector<std::string> storage;
  std::vector<const char *> argv;
  std::string tmpObject;
  bool pending = false;
};
State &state() {
  static State S;
  return S;
}
} // namespace

void lld::setDeferredNativeLink(ArrayRef<const char *> argv,
                                StringRef tmpObjectToRemove) {
  State &S = state();
  S.storage.clear();
  S.argv.clear();
  for (const char *A : argv)
    S.storage.emplace_back(A);
  S.argv.reserve(S.storage.size());
  for (const std::string &Str : S.storage)
    S.argv.push_back(Str.c_str());
  S.tmpObject = tmpObjectToRemove.str();
  S.pending = true;
}

bool lld::takeDeferredNativeLink(SmallVectorImpl<const char *> &OutArgv,
                                 std::string &TmpObjectToRemove) {
  State &S = state();
  if (!S.pending)
    return false;
  S.pending = false;
  TmpObjectToRemove = S.tmpObject;
  OutArgv.assign(S.argv.begin(), S.argv.end());
  return true;
}
