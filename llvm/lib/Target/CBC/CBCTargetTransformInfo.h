//===-- CBCTargetTransformInfo.h - CBC-specific TTI -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_CBC_CBCTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_CBC_CBCTARGETTRANSFORMINFO_H

#include "CBCTargetMachine.h"
#include "llvm/CodeGen/BasicTTIImpl.h"

namespace llvm {

class CBCTTIImpl : public BasicTTIImplBase<CBCTTIImpl> {
  typedef BasicTTIImplBase<CBCTTIImpl> BaseT;
  typedef TargetTransformInfo TTI;
  friend BaseT;

  const CBCSubtarget *ST;
  const CBCTargetLowering *TLI;

  const CBCSubtarget *getST() const { return ST; }
  const CBCTargetLowering *getTLI() const { return TLI; }

public:
  CBCTTIImpl(const CBCTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  void getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                               TTI::UnrollingPreferences &UP,
                               OptimizationRemarkEmitter *ORE) const override;
};

} // namespace llvm

#endif
