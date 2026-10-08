//===--- SemaCBC.cpp - Semantic Analysis for CBC Compilation --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CBC (-fcbc) Sema diagnostics: long double ops, inline asm, and target
// builtins are warnings (-Wcbc-unsupported) in user code. System headers are
// silent. Hard errors live in CBCRejectUnsupportedIR after link O2.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Sema/Sema.h"

using namespace clang;

bool Sema::isCBCCompilation() const {
  return getLangOpts().CBC ||
         Context.getTargetInfo().getTriple().isCBC();
}

bool Sema::cbcForbidsLongDouble() const {
  if (!isCBCCompilation())
    return false;
  const TargetInfo &TI = Context.getTargetInfo();
  return TI.getLongDoubleWidth() != TI.getDoubleWidth() ||
         &TI.getLongDoubleFormat() != &TI.getDoubleFormat();
}

bool Sema::shouldDiagnoseCBCUnsupported(SourceLocation Loc) const {
  return isCBCCompilation() && Loc.isValid() &&
         !SourceMgr.isInSystemHeader(Loc);
}

bool Sema::isCBCForbiddenLongDouble(QualType T) const {
  if (!cbcForbidsLongDouble())
    return false;
  QualType C = Context.getCanonicalType(T).getUnqualifiedType();
  if (C == Context.LongDoubleTy)
    return true;
  if (const ComplexType *CT = C->getAs<ComplexType>())
    return Context.getCanonicalType(CT->getElementType()).getUnqualifiedType() ==
           Context.LongDoubleTy;
  return false;
}

bool Sema::typeInvolvesCBCForbiddenLongDouble(QualType T) const {
  if (!cbcForbidsLongDouble())
    return false;
  QualType C = Context.getCanonicalType(T).getUnqualifiedType();
  if (isCBCForbiddenLongDouble(C))
    return true;
  if (const PointerType *PT = C->getAs<PointerType>())
    return typeInvolvesCBCForbiddenLongDouble(PT->getPointeeType());
  if (const ReferenceType *RT = C->getAs<ReferenceType>())
    return typeInvolvesCBCForbiddenLongDouble(RT->getPointeeType());
  if (C->isArrayType())
    return typeInvolvesCBCForbiddenLongDouble(Context.getBaseElementType(C));
  return false;
}

bool Sema::functionTypeInvolvesCBCForbiddenLongDouble(QualType T) const {
  if (const auto *FPT = T->getAs<FunctionProtoType>()) {
    if (typeInvolvesCBCForbiddenLongDouble(FPT->getReturnType()))
      return true;
    for (QualType P : FPT->param_types())
      if (typeInvolvesCBCForbiddenLongDouble(P))
        return true;
    return false;
  }
  if (const auto *FNPT = T->getAs<FunctionNoProtoType>())
    return typeInvolvesCBCForbiddenLongDouble(FNPT->getReturnType());
  return false;
}

void Sema::DiagnoseCBCLongDouble(SourceLocation Loc) {
  if (!shouldDiagnoseCBCUnsupported(Loc) || !cbcForbidsLongDouble())
    return;
  Diag(Loc, diag::warn_cbc_long_double);
}

void Sema::DiagnoseCBCInlineAsm(SourceLocation Loc) {
  if (!shouldDiagnoseCBCUnsupported(Loc))
    return;
  Diag(Loc, diag::warn_cbc_inline_asm);
}

void Sema::DiagnoseCBCTargetBuiltin(SourceLocation Loc) {
  if (!shouldDiagnoseCBCUnsupported(Loc))
    return;
  Diag(Loc, diag::warn_cbc_target_builtin);
}

void Sema::DiagnoseCBCTargetAttr(SourceLocation Loc) {
  if (!shouldDiagnoseCBCUnsupported(Loc))
    return;
  Diag(Loc, diag::warn_cbc_target_attr);
}

void Sema::DiagnoseCBCLongDoubleInFunctionType(SourceLocation Loc,
                                               QualType T) {
  if (functionTypeInvolvesCBCForbiddenLongDouble(T))
    DiagnoseCBCLongDouble(Loc);
}
