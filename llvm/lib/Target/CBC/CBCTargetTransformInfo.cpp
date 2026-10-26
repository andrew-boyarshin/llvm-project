//===-- CBCTargetTransformInfo.cpp - CBC-specific TTI ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CBCTargetTransformInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

#define DEBUG_TYPE "cbctti"

void CBCTTIImpl::getUnrollingPreferences(Loop *L, ScalarEvolution &,
                                         TTI::UnrollingPreferences &UP,
                                         OptimizationRemarkEmitter *) const {
  // CBC encodings average ~4 B/insn; O3's default Threshold=300 with a 4x
  // profitability boost fully unrolls call-heavy loops (e.g. 24x DEH_String
  // sites) into 10–40x outliers. Cap that.
  bool HasRealCall = false;
  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (const Function *F = CB->getCalledFunction()) {
        // Intrinsics (lifetime, dbg, assume, …) are not real calls.
        if (isLoweredToCall(F)) {
          HasRealCall = true;
          break;
        }
      } else {
        // Indirect / unknown callee.
        HasRealCall = true;
        break;
      }
    }
    if (HasRealCall)
      break;
  }

  if (HasRealCall) {
    UP.Threshold = 0;
    UP.PartialThreshold = 0;
    UP.FullUnrollMaxCount = 1;
    UP.Partial = false;
    UP.Runtime = false;
    UP.UpperBound = false;
    return;
  }

  // Call-free numeric kernels: milder than O3 defaults (300 / 400%).
  UP.Threshold = 150;
  UP.MaxPercentThresholdBoost = 200;
  UP.PartialThreshold = 30;
  UP.Partial = true;
  UP.Runtime = false;
  UP.UpperBound = true;
  UP.OptSizeThreshold = 0;
  UP.PartialOptSizeThreshold = 0;
  UP.BEInsns = 2;
}
