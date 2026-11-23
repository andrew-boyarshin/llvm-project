//===- CBCLowerByVal.cpp - internal convention for byval parameters --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The engine places the arguments of every ClangCBC call by the host C rules
// and cannot see a `byval` aggregate in a lowered signature, so the backend
// rejects `byval` parameters (CBCISelLowering). That is correct for calls that
// cross the native boundary, but it also rejects code that never leaves CBC:
// libc++ passes small helper structs (`__exception_guard_exceptions`, ...) by
// value to its own hidden functions.
//
// This pass gives such functions an internal convention. A function that is
// defined in the program, takes `byval` parameters, and is only ever called
// directly is rewritten to take a plain pointer instead; every call site copies
// the aggregate into a fresh entry-block alloca and passes its address. The
// callee still receives a private copy, which is all `byval` promises. With
// opaque pointers the function type does not change; the pass only rewrites the
// parameter attributes and the call sites.
//
// Left alone (and therefore still rejected later, with a clear message):
//  * functions whose address is taken (vtables, callbacks, aliases),
//  * variadic functions, and `inalloca` / `preallocated` / `musttail` uses,
//  * default-visibility functions of a native shared library (`cbc-wrap`),
//    which native code may call by name,
//  * declarations: the callee is outside the program and keeps the C ABI.
//
//===----------------------------------------------------------------------===//

#include "CBC.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

using namespace llvm;

#define DEBUG_TYPE "cbc-lower-byval"
#define PASS_NAME "CBC lower byval parameters"

namespace {

bool hasByValParam(const Function &F) {
  for (const Argument &A : F.args())
    if (A.hasByValAttr())
      return true;
  return false;
}

/// True if \p F may be rewritten: all of its uses are direct calls of exactly
/// its own type, and nothing outside the program can call it.
bool isEligible(const Function &F, bool IsNativeLibrary) {
  if (F.isDeclaration() || F.isVarArg() || F.isIntrinsic())
    return false;
  if (IsNativeLibrary && !F.hasLocalLinkage() && F.hasDefaultVisibility())
    return false;
  for (const Argument &A : F.args())
    if (A.hasInAllocaAttr() || A.hasPreallocatedAttr())
      return false;
  for (const Use &U : F.uses()) {
    const auto *CB = dyn_cast<CallBase>(U.getUser());
    if (!CB || !CB->isCallee(&U) || CB->getFunctionType() != F.getFunctionType())
      return false;
    if (CB->isMustTailCall())
      return false;
  }
  return true;
}

/// Copy of the aggregate for one byval argument, placed in the entry block of
/// the caller so loops do not grow the stack.
AllocaInst *createCopyAlloca(Function &Caller, Type *Ty, Align A) {
  BasicBlock &Entry = Caller.getEntryBlock();
  IRBuilder<> B(&Entry, Entry.getFirstInsertionPt());
  AllocaInst *AI = B.CreateAlloca(Ty, nullptr, "byval.copy");
  AI->setAlignment(A);
  return AI;
}

using ByValParams = SmallVector<std::pair<unsigned, std::pair<Type *, Align>>, 4>;

/// What `byval` guaranteed, restated for a plain pointer: a private,
/// dereferenceable, aligned object.
AttributeList plainPointerAttrs(LLVMContext &Ctx, const DataLayout &DL,
                                AttributeList Attrs, const ByValParams &ByVal) {
  for (const auto &[No, TyAl] : ByVal) {
    auto [Ty, Al] = TyAl;
    Attrs = Attrs.removeParamAttribute(Ctx, No, Attribute::ByVal);
    Attrs = Attrs.removeParamAttribute(Ctx, No, Attribute::Alignment);
    Attrs = Attrs.addParamAttribute(Ctx, No, Attribute::NoAlias);
    Attrs = Attrs.addParamAttribute(Ctx, No, Attribute::NonNull);
    Attrs = Attrs.addParamAttribute(Ctx, No, Attribute::getWithAlignment(Ctx, Al));
    Attrs = Attrs.addDereferenceableParamAttr(Ctx, No, DL.getTypeAllocSize(Ty));
  }
  return Attrs;
}

bool lowerFunction(Function &F) {
  Module &M = *F.getParent();
  const DataLayout &DL = M.getDataLayout();
  LLVMContext &Ctx = M.getContext();

  // With opaque pointers a `byval` parameter and a plain pointer have the same
  // type, so the function and its calls keep their types; only the attributes
  // and the arguments at the call sites change.
  SmallVector<CallBase *, 8> Calls;
  for (Use &U : F.uses())
    Calls.push_back(cast<CallBase>(U.getUser()));

  // New attribute list: drop `byval`, state what it guaranteed instead.
  AttributeList Attrs = F.getAttributes();
  ByValParams ByVal;
  for (const Argument &A : F.args()) {
    if (!A.hasByValAttr())
      continue;
    Type *Ty = A.getParamByValType();
    Align Al = A.getParamAlign().value_or(DL.getABITypeAlign(Ty));
    ByVal.push_back({A.getArgNo(), {Ty, Al}});
  }
  F.setAttributes(plainPointerAttrs(Ctx, DL, Attrs, ByVal));

  for (CallBase *CB : Calls) {
    Function &Caller = *CB->getFunction();
    IRBuilder<> B(CB);
    for (auto &[No, TyAl] : ByVal) {
      auto [Ty, Al] = TyAl;
      Value *Src = CB->getArgOperand(No);
      Align SrcAlign = CB->getParamAlign(No).value_or(Al);
      AllocaInst *Copy = createCopyAlloca(Caller, Ty, Al);
      B.CreateMemCpy(Copy, Al, Src, SrcAlign, DL.getTypeAllocSize(Ty));
      CB->setArgOperand(No, Copy);
    }
    CB->setAttributes(plainPointerAttrs(Ctx, DL, CB->getAttributes(), ByVal));
  }
  return true;
}

class CBCLowerByValLegacy : public ModulePass {
public:
  static char ID;
  CBCLowerByValLegacy() : ModulePass(ID) {}
  bool runOnModule(Module &M) override;
};

} // namespace

char CBCLowerByValLegacy::ID = 0;
INITIALIZE_PASS(CBCLowerByValLegacy, DEBUG_TYPE, PASS_NAME, false, false)

bool CBCLowerByValLegacy::runOnModule(Module &M) {
  bool IsNativeLibrary = M.getModuleFlag("cbc-wrap") != nullptr;
  SmallVector<Function *, 16> Work;
  for (Function &F : M)
    if (hasByValParam(F) && isEligible(F, IsNativeLibrary))
      Work.push_back(&F);
  bool Changed = false;
  for (Function *F : Work)
    Changed |= lowerFunction(*F);
  return Changed;
}

ModulePass *llvm::createCBCLowerByValPass() { return new CBCLowerByValLegacy(); }
