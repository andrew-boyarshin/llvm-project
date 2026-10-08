//===- CBCRejectUnsupportedIR.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hard gate for constructs that CBC bytecode cannot represent: x86_fp80 /
// fp128 operations, inline asm, and target SIMD intrinsics. Runs at the start
// of the CBC IR pipeline so CBCLowerGlobals never lays out illegal types.
// Declarations-only types are ignored; DCE before this pass drops dead bodies.
//
//===----------------------------------------------------------------------===//

#include "CBC.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "cbc-reject-unsupported-ir"
#define PASS_NAME "CBC reject unsupported IR"

namespace {

bool isForbiddenFPType(Type *Ty) {
  if (!Ty)
    return false;
  if (Ty->isX86_FP80Ty() || Ty->isFP128Ty() || Ty->isPPC_FP128Ty())
    return true;
  if (auto *ST = dyn_cast<StructType>(Ty)) {
    for (Type *E : ST->elements())
      if (isForbiddenFPType(E))
        return true;
  }
  if (auto *AT = dyn_cast<ArrayType>(Ty))
    return isForbiddenFPType(AT->getElementType());
  if (auto *VT = dyn_cast<VectorType>(Ty))
    return isForbiddenFPType(VT->getElementType());
  if (auto *FT = dyn_cast<FunctionType>(Ty)) {
    if (isForbiddenFPType(FT->getReturnType()))
      return true;
    for (Type *P : FT->params())
      if (isForbiddenFPType(P))
        return true;
  }
  return false;
}

bool typeOrOperandsForbidden(const Instruction &I) {
  if (isForbiddenFPType(I.getType()))
    return true;
  for (const Value *Op : I.operands())
    if (isForbiddenFPType(Op->getType()))
      return true;
  return false;
}

bool isTargetSIMDIntrinsic(const Function *Callee) {
  if (!Callee || !Callee->isIntrinsic())
    return false;
  StringRef Name = Callee->getName();
  return Name.starts_with("llvm.x86.") ||
         Name.starts_with("llvm.aarch64.neon.") ||
         Name.starts_with("llvm.aarch64.sve.");
}

[[noreturn]] void fail(StringRef Where, StringRef Why) {
  report_fatal_error(Twine("CBC: unsupported IR in '") + Where + "': " + Why,
                     false);
}

void checkFunction(Function &F) {
  if (F.isDeclaration())
    return;

  if (isForbiddenFPType(F.getFunctionType()))
    fail(F.getName(), "function signature uses long double / fp80 / fp128");

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->isInlineAsm())
          fail(F.getName(), "inline assembly");
        if (isTargetSIMDIntrinsic(CB->getCalledFunction()))
          fail(F.getName(), "target SIMD intrinsic");
      }
      if (auto *VA = dyn_cast<VAArgInst>(&I)) {
        if (isForbiddenFPType(VA->getType()))
          fail(F.getName(), "va_arg of long double / fp80 / fp128");
      }
      if (typeOrOperandsForbidden(I))
        fail(F.getName(), "x86_fp80 / fp128 operation");
    }
  }
}

bool constantContainsForbiddenFP(const Constant *C) {
  if (!C)
    return false;
  if (isForbiddenFPType(C->getType()))
    return true;
  if (auto *CA = dyn_cast<ConstantAggregate>(C)) {
    for (const Use &U : CA->operands())
      if (constantContainsForbiddenFP(cast<Constant>(U.get())))
        return true;
  }
  if (auto *CE = dyn_cast<ConstantExpr>(C)) {
    for (const Use &U : CE->operands())
      if (constantContainsForbiddenFP(cast<Constant>(U.get())))
        return true;
  }
  return false;
}

void checkGlobal(GlobalVariable &GV) {
  if (GV.isDeclaration())
    return;
  if (isForbiddenFPType(GV.getValueType()))
    fail(GV.getName(), "global of long double / fp80 / fp128 type");
  if (GV.hasInitializer() && constantContainsForbiddenFP(GV.getInitializer()))
    fail(GV.getName(), "global initializer uses long double / fp80 / fp128");
}

class CBCRejectUnsupportedIRLegacy : public ModulePass {
public:
  static char ID;
  CBCRejectUnsupportedIRLegacy() : ModulePass(ID) {}
  bool runOnModule(Module &M) override;
};

} // namespace

char CBCRejectUnsupportedIRLegacy::ID = 0;
INITIALIZE_PASS(CBCRejectUnsupportedIRLegacy, DEBUG_TYPE, PASS_NAME, false,
                false)

bool CBCRejectUnsupportedIRLegacy::runOnModule(Module &M) {
  if (!M.getModuleInlineAsm().empty())
    fail("<module>", "module-level inline assembly");

  for (GlobalIFunc &GI : M.ifuncs())
    fail(GI.getName(), "ifunc");

  for (GlobalVariable &GV : M.globals())
    checkGlobal(GV);

  for (Function &F : M)
    checkFunction(F);

  return false;
}

ModulePass *llvm::createCBCRejectUnsupportedIRPass() {
  return new CBCRejectUnsupportedIRLegacy();
}
