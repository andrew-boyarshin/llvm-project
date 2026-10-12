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
  bool external = false;
};
State &state() {
  static State S;
  return S;
}

void store(ArrayRef<const char *> argv, StringRef tmpObjectToRemove,
           bool external) {
  State &S = state();
  S.storage.clear();
  S.argv.clear();
  for (const char *A : argv)
    S.storage.emplace_back(A);
  S.argv.reserve(S.storage.size());
  for (const std::string &Str : S.storage)
    S.argv.push_back(Str.c_str());
  S.tmpObject = tmpObjectToRemove.str();
  S.external = external;
  S.pending = true;
}
} // namespace

void lld::setDeferredNativeLink(ArrayRef<const char *> argv,
                                StringRef tmpObjectToRemove) {
  store(argv, tmpObjectToRemove, /*external=*/false);
}

void lld::setDeferredExternalLink(ArrayRef<const char *> argv,
                                  StringRef tmpObjectToRemove) {
  store(argv, tmpObjectToRemove, /*external=*/true);
}

bool lld::takeDeferredNativeLink(SmallVectorImpl<const char *> &OutArgv,
                                 std::string &TmpObjectToRemove,
                                 bool &External) {
  State &S = state();
  if (!S.pending)
    return false;
  S.pending = false;
  TmpObjectToRemove = S.tmpObject;
  External = S.external;
  OutArgv.assign(S.argv.begin(), S.argv.end());
  return true;
}
