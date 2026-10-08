//===- CBCWrap.cpp --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Host-triple wrap of a finished .cbc: blob + N2C stubs + DSO ctor that calls
// engine_load_buffer. Native -shared link is deferred until after the CBC
// CommonLinkerContext is destroyed (see lld/Common/DriverDispatcher.cpp).
//
//===----------------------------------------------------------------------===//

#include "lld/CBC/CBCLink.h"
#include "lld/Common/DeferredNativeLink.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <string>

using namespace llvm;
using namespace lld;

namespace {

Triple hostTripleFromCBC(const Triple &CBC) {
  if (CBC.getArch() != Triple::cbc) {
    if (CBC.getArch() == Triple::UnknownArch)
      return Triple(sys::getDefaultTargetTriple());
    return CBC;
  }
  // Rebuild a host triple from CBC subarch + remaining components.
  StringRef Arch =
      CBC.getSubArch() == Triple::CBCSubArch_aarch64 ? "aarch64" : "x86_64";
  return Triple(Arch, CBC.getVendorName(), CBC.getOSName(),
                CBC.getEnvironmentName());
}

bool isAlwaysHiddenExport(StringRef Name) {
  return Name == "main" || Name == "__cbc_main" || Name == "__cbc_entry" ||
         Name == "__cbc_lib_start";
}

FunctionType *exportFunctionType(LLVMContext &Ctx, const cbc::CBCExport &E) {
  Type *Ret = E.retFloat ? Type::getDoubleTy(Ctx) : Type::getVoidTy(Ctx);
  // Non-float CBC returns are i64 in the file writer; void only for empty
  // ret when we lack a better signal. Prefer i64 for non-float (matches
  // MethodSigs) so declarations stay ABI-plausible.
  if (!E.retFloat)
    Ret = Type::getInt64Ty(Ctx);
  SmallVector<Type *, 8> Params;
  for (char C : E.params)
    Params.push_back(C == 'f' ? Type::getDoubleTy(Ctx)
                              : Type::getInt64Ty(Ctx));
  return FunctionType::get(Ret, Params, /*isVarArg=*/false);
}

void appendX86Stub(raw_ostream &OS, StringRef Name, unsigned Index,
                   bool Hidden) {
  OS << "  .text\n";
  OS << "  .p2align 4, 0x90\n";
  OS << "  .globl " << Name << "\n";
  if (Hidden)
    OS << "  .hidden " << Name << "\n";
  OS << "  .type " << Name << ",@function\n";
  OS << Name << ":\n";
  OS << "  movq (%rsp), %r11\n";
  OS << "  subq $16, %rsp\n";
  OS << "  movq %r11, (%rsp)\n";
  OS << "  leaq .Lenter_" << Index << "(%rip), %r11\n";
  OS << "  movq %r11, 16(%rsp)\n";
  OS << "  movq $0, 8(%rsp)\n";
  OS << "  jmp CJ_MCC_N2CStub@PLT\n";
  OS << ".Lenter_" << Index << ":\n";
  OS << "  movq __cbc_exported_handles+" << (8u * Index) << "(%rip), %rax\n";
  OS << "  jmp *8(%rax)\n";
  OS << "  .size " << Name << ", .-" << Name << "\n";
}

void appendAArch64Stub(raw_ostream &OS, StringRef Name, unsigned Index,
                       bool Hidden) {
  OS << "  .text\n";
  OS << "  .align 2\n";
  OS << "  .globl " << Name << "\n";
  if (Hidden)
    OS << "  .hidden " << Name << "\n";
  OS << "  .type " << Name << ",%function\n";
  OS << Name << ":\n";
  // Plant [sp]=enter, [sp+8]=cpStackSize=0; keep x0-x7 / x30 for the caller.
  OS << "  adr x9, .Lenter_" << Index << "\n";
  OS << "  mov x10, #0\n";
  OS << "  stp x9, x10, [sp, #-16]!\n";
  OS << "  adrp x16, :got:CJ_MCC_N2CStub\n";
  OS << "  ldr x16, [x16, :got_lo12:CJ_MCC_N2CStub]\n";
  OS << "  br x16\n";
  OS << ".Lenter_" << Index << ":\n";
  OS << "  adrp x9, __cbc_exported_handles\n";
  OS << "  add x9, x9, :lo12:__cbc_exported_handles\n";
  OS << "  ldr x9, [x9, #" << (8u * Index) << "]\n";
  OS << "  ldr x10, [x9, #8]\n";
  OS << "  br x10\n";
  OS << "  .size " << Name << ", .-" << Name << "\n";
}

std::string buildStubModuleAsm(const Triple &Host,
                               ArrayRef<cbc::CBCExport> Exports) {
  std::string Asm;
  raw_string_ostream OS(Asm);
  bool IsAArch64 = Host.isAArch64();
  for (unsigned I = 0, E = Exports.size(); I != E; ++I) {
    const cbc::CBCExport &Ex = Exports[I];
    bool Hidden =
        isAlwaysHiddenExport(Ex.name) || !(Ex.defaultVis || Ex.addressTaken);
    if (IsAArch64)
      appendAArch64Stub(OS, Ex.name, I, Hidden);
    else
      appendX86Stub(OS, Ex.name, I, Hidden);
  }
  OS.flush();
  return Asm;
}

std::unique_ptr<Module> buildWrapModule(LLVMContext &Ctx,
                                        const cbc::CBCLinkResult &Result) {
  auto M = std::make_unique<Module>("cbc-wrap", Ctx);
  Triple Host = hostTripleFromCBC(Result.triple);
  M->setTargetTriple(Host);

  if (Result.exports.empty()) {
    error("CBC wrap: no method exports (missing __cbc_lib_start?)");
    return nullptr;
  }

  Constant *BlobInit = ConstantDataArray::get(
      Ctx, ArrayRef<uint8_t>(
               reinterpret_cast<const uint8_t *>(Result.bytes.data()),
               Result.bytes.size()));
  auto *Blob = new GlobalVariable(*M, BlobInit->getType(), /*isConstant=*/true,
                                  GlobalValue::PrivateLinkage, BlobInit,
                                  "__cbc_blob");
  Blob->setAlignment(Align(16));
  Blob->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  Type *I8Ptr = PointerType::getUnqual(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *Void = Type::getVoidTy(Ctx);

  // ExportedInfo: { ptr, i32, i32 } — 16 bytes on LP64.
  StructType *ExportedInfoTy =
      StructType::create(Ctx, {I8Ptr, I32, I32}, "ExportedInfo");

  const unsigned N = Result.exports.size();
  ArrayType *HandlesTy = ArrayType::get(I8Ptr, N);
  ArrayType *InfosTy = ArrayType::get(ExportedInfoTy, N);

  auto *Handles = new GlobalVariable(
      *M, HandlesTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
      Constant::getNullValue(HandlesTy), "__cbc_exported_handles");
  Handles->setAlignment(Align(8));

  SmallVector<Constant *, 8> InfoElts;
  for (unsigned I = 0; I < N; ++I) {
    const cbc::CBCExport &E = Result.exports[I];
    Function *Decl = Function::Create(exportFunctionType(Ctx, E),
                                      GlobalValue::ExternalLinkage, E.name, *M);
    bool Hidden =
        isAlwaysHiddenExport(E.name) || !(E.defaultVis || E.addressTaken);
    Decl->setVisibility(Hidden ? GlobalValue::HiddenVisibility
                               : GlobalValue::DefaultVisibility);
    // Body is module asm; keep as a declaration for IR references.
    InfoElts.push_back(ConstantStruct::get(
        ExportedInfoTy,
        {Decl, ConstantInt::get(I32, E.offset), ConstantInt::get(I32, 0)}));
  }
  auto *Infos = new GlobalVariable(*M, InfosTy, /*isConstant=*/true,
                                   GlobalValue::InternalLinkage,
                                   ConstantArray::get(InfosTy, InfoElts),
                                   "__cbc_exported_infos");
  Infos->setAlignment(Align(8));

  M->appendModuleInlineAsm(buildStubModuleAsm(Host, Result.exports));

  StructType *DlInfoTy =
      StructType::create(Ctx, {I8Ptr, I8Ptr, I8Ptr, I8Ptr}, "struct.Dl_info");

  FunctionCallee Dladdr = M->getOrInsertFunction(
      "dladdr",
      FunctionType::get(I32, {I8Ptr, PointerType::getUnqual(Ctx)}, false));
  FunctionCallee Realpath = M->getOrInsertFunction(
      "realpath", FunctionType::get(I8Ptr, {I8Ptr, I8Ptr}, false));
  FunctionCallee LoadBuffer = M->getOrInsertFunction(
      "engine_load_buffer",
      FunctionType::get(I32, {I8Ptr, I64, I8Ptr, I32, I8Ptr, I8Ptr}, false));

  Function *InitFn = Function::Create(FunctionType::get(Void, false),
                                      GlobalValue::InternalLinkage,
                                      "__cbc_lib_init", *M);
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", InitFn);
  IRBuilder<> B(BB);

  AllocaInst *Info = B.CreateAlloca(DlInfoTy, nullptr, "info");
  Value *InitPtr = InitFn;
  B.CreateCall(Dladdr, {InitPtr, Info});
  Value *FnamePtr = B.CreateStructGEP(DlInfoTy, Info, 0);
  Value *Fname = B.CreateLoad(I8Ptr, FnamePtr, "dli_fname");
  Value *Resolved = B.CreateCall(
      Realpath,
      {Fname, ConstantPointerNull::get(cast<PointerType>(I8Ptr))}, "resolved");
  Value *IsNull = B.CreateICmpEQ(
      Resolved, ConstantPointerNull::get(cast<PointerType>(I8Ptr)));
  Value *Path = B.CreateSelect(IsNull, Fname, Resolved, "load.name");

  Value *BlobPtr = Blob;
  Value *Len = ConstantInt::get(I64, Result.bytes.size());
  Value *Count = ConstantInt::get(I32, N);
  B.CreateCall(LoadBuffer, {BlobPtr, Len, Path, Count, Infos, Handles});
  B.CreateRetVoid();

  // Emit into .init_array directly. Host codegen of llvm.global_ctors into a
  // bare .o (no crt begin/end) left an empty .ctors and no DT_INIT_ARRAY, so
  // dlopen never ran the wrap ctor.
  auto *InitArrEnt = new GlobalVariable(
      *M, PointerType::getUnqual(Ctx), /*isConstant=*/false,
      GlobalValue::InternalLinkage, InitFn, "__cbc_lib_init.ptr");
  InitArrEnt->setSection(".init_array");
  InitArrEnt->setAlignment(Align(8));

  return M;
}

bool codegenWrapObject(Module &M, const Triple &Host,
                       SmallVectorImpl<char> &OutObj) {
  std::string Err;
  const Target *T = TargetRegistry::lookupTarget(Host, Err);
  if (!T) {
    error("CBC wrap: cannot find host target: " + Err);
    return false;
  }
  TargetOptions Options;
  auto TM = std::unique_ptr<TargetMachine>(T->createTargetMachine(
      Host, "generic", "", Options, Reloc::PIC_, CodeModel::Small,
      CodeGenOptLevel::Default));
  if (!TM) {
    error("CBC wrap: cannot create host TargetMachine");
    return false;
  }
  M.setDataLayout(TM->createDataLayout());
  raw_svector_ostream OS(OutObj);
  legacy::PassManager PM;
  if (TM->addPassesToEmitFile(PM, OS, nullptr, CodeGenFileType::ObjectFile)) {
    error("CBC wrap: host target cannot emit object file");
    return false;
  }
  PM.run(M);
  return errorCount() == 0;
}

} // namespace

namespace lld {
namespace cbc {

bool emitSharedWrap(StringRef outputPath, const CBCLinkResult &result,
                    ArrayRef<StringRef> hostSearchPaths) {
  if (result.bytes.empty()) {
    error("CBC wrap: empty CBC payload");
    return false;
  }

  LLVMContext Ctx;
  std::unique_ptr<Module> M = buildWrapModule(Ctx, result);
  if (!M)
    return false;
  Triple Host = hostTripleFromCBC(result.triple);

  SmallVector<char, 0> ObjBuf;
  if (!codegenWrapObject(*M, Host, ObjBuf))
    return false;

  SmallString<256> TmpPath;
  int FD = -1;
  if (std::error_code EC =
          sys::fs::createTemporaryFile("cbc-wrap", "o", FD, TmpPath)) {
    error("CBC wrap: cannot create temp object: " + EC.message());
    return false;
  }
  {
    raw_fd_ostream FOS(FD, /*shouldClose=*/true);
    FOS.write(ObjBuf.data(), ObjBuf.size());
  }

  SmallVector<std::string, 32> Storage;
  auto push = [&](Twine S) { Storage.emplace_back(S.str()); };
  push("ld.lld");
  push("-shared");
  push("-o");
  push(outputPath);
  push(TmpPath);
  // -L from the CBC link (user -L and toolchain FilePaths).
  for (StringRef Dir : hostSearchPaths) {
    if (Dir.empty())
      continue;
    push("-L");
    push(Dir);
  }
  push("-lcbcengine");
  push("-l:libcangjie-runtime.so");
  push("-ldl");
  for (const std::string &Dep : result.aotDeps) {
    if (StringRef(Dep).starts_with("lib") && StringRef(Dep).contains(".so"))
      push(Twine("-l:") + Dep);
    else {
      push("-l");
      push(Dep);
    }
  }

  SmallVector<const char *, 32> Argv;
  for (const std::string &S : Storage)
    Argv.push_back(S.c_str());
  setDeferredNativeLink(Argv, TmpPath);
  return true;
}

} // namespace cbc
} // namespace lld
