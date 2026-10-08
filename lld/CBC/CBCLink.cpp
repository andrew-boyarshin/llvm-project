//===- CBCLink.cpp --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Mechanical move of llvm/tools/cbc-ld/cbc-ld.cpp emit pipeline.
// Archive extraction is the cbc-ld 64-round loop, not ELF lazy symbols.
//
//===----------------------------------------------------------------------===//

#include "lld/CBC/CBCLink.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/Archive.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/CBCABI.h"
#include "llvm/TargetParser/Triple.h"
#include "CBCFileWriter.h"
#include <set>
#include <utility>
#include <vector>

using namespace llvm;
using namespace lld;

namespace {

const Target *findCBCTarget() {
  for (const Target &T : TargetRegistry::targets())
    if (StringRef(T.getName()) == "cbc")
      return &T;
  return nullptr;
}

/// Require !"CBC" on \p M. Returns false after reporting an error.
bool checkCBCModuleFlag(Module &M, StringRef Ident) {
  if (!M.getModuleFlag(CBCModuleFlagKey)) {
    error("'" + Ident + "' was not compiled with -fcbc (missing CBC module flag)");
    return false;
  }
  return true;
}

} // namespace

bool cbc::linkToMemory(const CBCLinkRequest &request, CBCLinkResult &out) {
  out = CBCLinkResult();
  out.wrap = request.wrap;
  for (StringRef D : request.aotDeps)
    out.aotDeps.push_back(D.str());

  if (request.wholeModules.empty()) {
    error("no bitcode input");
    return false;
  }

  LLVMContext Ctx;
  SMDiagnostic Err;
  auto Composite = std::make_unique<Module>("lld-cbc", Ctx);
  Linker L(*Composite);

  Triple ExpectedTriple = request.triple;
  bool SawCBC = false;
  for (MemoryBufferRef MB : request.wholeModules) {
    std::unique_ptr<Module> M = parseIR(MB, Err, Ctx);
    if (!M) {
      Err.print("ld.lld", errs());
      error("failed to parse bitcode '" + MB.getBufferIdentifier() + "'");
      return false;
    }
    if (!checkCBCModuleFlag(*M, MB.getBufferIdentifier()))
      return false;
    SawCBC = true;
    Triple MT(M->getTargetTriple());
    if (ExpectedTriple.getTriple().empty())
      ExpectedTriple = MT;
    else if (MT != ExpectedTriple) {
      error("bitcode triple mismatch: '" + MB.getBufferIdentifier() + "' is " +
            MT.str() + ", expected " + ExpectedTriple.str());
      return false;
    }
    if (L.linkInModule(std::move(M))) {
      error("failed to link bitcode '" + MB.getBufferIdentifier() + "'");
      return false;
    }
  }
  if (ExpectedTriple.getTriple().empty())
    ExpectedTriple = request.triple;
  // Seed the composite so an empty-dest IRMover path cannot drop the flag.
  if (SawCBC && !Composite->getModuleFlag(CBCModuleFlagKey))
    Composite->addModuleFlag(Module::Error, CBCModuleFlagKey, uint32_t(1));

  // Lazy bitcode archives: cbc-ld 64-round loop (not ELF lazy BitcodeFile).
  std::set<std::pair<std::string, uint64_t>> Extracted;
  for (int Round = 0; Round < 64; ++Round) {
    // Use owned strings: linkInModule can erase declarations and invalidate
    // StringRefs into the composite module. Treat available_externally as still
    // needing a real archive definition (isDeclarationForLinker).
    std::vector<std::string> UndefStorage;
    for (Function &F : *Composite)
      if (F.isDeclarationForLinker() && !F.use_empty() && !F.isIntrinsic())
        UndefStorage.emplace_back(F.getName().str());
    for (GlobalVariable &GV : Composite->globals())
      if (GV.isDeclarationForLinker() && !GV.use_empty())
        UndefStorage.emplace_back(GV.getName().str());
    DenseSet<StringRef> Undef;
    for (const std::string &Name : UndefStorage)
      Undef.insert(Name);
    bool Progress = false;
    for (StringRef Path : request.lazyArchives) {
      ErrorOr<std::unique_ptr<MemoryBuffer>> Buf = MemoryBuffer::getFile(Path);
      if (!Buf) {
        error("cannot read " + Path);
        return false;
      }
      Expected<std::unique_ptr<object::Archive>> Arch =
          object::Archive::create(Buf.get()->getMemBufferRef());
      if (!Arch) {
        error("not an archive: " + Path);
        consumeError(Arch.takeError());
        return false;
      }
      std::vector<std::pair<uint64_t, std::string>> Needed;
      for (object::Archive::Symbol Sym : (*Arch)->symbols()) {
        StringRef Name = Sym.getName();
        if (!Undef.contains(Name))
          continue;
        Expected<object::Archive::Child> Child = Sym.getMember();
        if (!Child) {
          consumeError(Child.takeError());
          continue;
        }
        Expected<uint64_t> Off = Child->getChildOffset();
        if (!Off) {
          consumeError(Off.takeError());
          continue;
        }
        if (Extracted.count({Path.str(), *Off}))
          continue;
        Needed.push_back({*Off, Name.str()});
      }
      for (const auto &Item : Needed) {
        Expected<std::optional<object::Archive::Child>> ChildOrErr =
            (*Arch)->findSym(Item.second);
        if (!ChildOrErr || !*ChildOrErr) {
          if (!ChildOrErr)
            consumeError(ChildOrErr.takeError());
          continue;
        }
        if (!Extracted.insert({Path.str(), Item.first}).second)
          continue;
        Expected<StringRef> Member = (*ChildOrErr)->getBuffer();
        if (!Member) {
          error("bad archive member in " + Path);
          consumeError(Member.takeError());
          return false;
        }
        std::unique_ptr<Module> M =
            parseIR(MemoryBufferRef(*Member, Item.second), Err, Ctx);
        if (!M) {
          Err.print("ld.lld", errs());
          error("failed to parse archive member defining " + Item.second);
          return false;
        }
        if (!checkCBCModuleFlag(*M, Path.str() + "(" + Item.second + ")"))
          return false;
        if (L.linkInModule(std::move(M))) {
          error("failed to link member of " + Path + " defining " +
                Item.second);
          return false;
        }
        Progress = true;
      }
    }
    if (!Progress)
      break;
    if (Round == 63) {
      error("archive extraction did not converge");
      return false;
    }
  }

  for (GlobalValue &GV : Composite->global_values()) {
    if (!GV.isDeclaration() || GV.use_empty())
      continue;
    if (GV.getName().starts_with("_Z")) {
      error("native C++ symbol '" + GV.getName() +
            "' is not supported on CBC");
      return false;
    }
  }

  Composite->setTargetTriple(ExpectedTriple);
  if (SawCBC && !Composite->getModuleFlag(CBCModuleFlagKey))
    Composite->addModuleFlag(Module::Error, CBCModuleFlagKey, uint32_t(1));
  Composite->addModuleFlag(Module::Warning, "cbc-whole-program", uint32_t(1));
  if (request.wrap)
    Composite->addModuleFlag(Module::Warning, "cbc-wrap", uint32_t(1));
  if (!request.aotDeps.empty()) {
    std::string Deps = request.aotDeps[0].str();
    for (size_t I = 1; I < request.aotDeps.size(); ++I) {
      Deps += ':';
      Deps += request.aotDeps[I];
    }
    Composite->getOrInsertNamedMetadata("cbc.aotDeps")
        ->addOperand(MDNode::get(Ctx, MDString::get(Ctx, Deps)));
  }

  // crt is compiled as C, so clang marks it nounwind. C++ throw must stay
  // may-unwind or the optimizer deletes the landing pad.
  for (const char *Name : {"__cbc_raise", "__cbc_nullcheck",
                           "_Unwind_RaiseException", "_Unwind_Resume",
                           "_Unwind_Resume_or_Rethrow"}) {
    Function *F = Composite->getFunction(Name);
    if (!F)
      continue;
    F->removeFnAttr(Attribute::NoUnwind);
    for (User *U : F->users())
      if (auto *CB = dyn_cast<CallBase>(U))
        CB->removeFnAttr(Attribute::NoUnwind);
  }

  // Construct CBC TM before O2 so TargetIRAnalysis uses CBC TTI (no vector
  // registers), not the default TTI that claims 8 vector regs.
  const Target *T = findCBCTarget();
  if (!T) {
    error("CBC target is not registered");
    return false;
  }
  TargetOptions Options;
  auto TM = std::unique_ptr<TargetMachine>(T->createTargetMachine(
      ExpectedTriple, "", "", Options, Reloc::Static, CodeModel::Small,
      CodeGenOptLevel::Default));
  if (!TM) {
    error("cannot create CBC target machine");
    return false;
  }
  Composite->setDataLayout(TM->createDataLayout());

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB(TM.get());
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  ModulePassManager MPM =
      PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
  MPM.run(*Composite, MAM);

  // Native shared-library wrap: N2C stubs make CBC callbacks legal for
  // qsort/pthread_create/etc. Keep pthread_atfork (fork, not attach) and the
  // existing signal/sigaction async crt guard. Deprecated .cbc emit keeps
  // today's checks.
  bool Failed = false;
  if (!request.wrap) {
    static const char *Unsupported[] = {
        "qsort", "qsort_r", "bsearch", "lfind", "lsearch", "tsearch", "tfind",
        "tdelete", "twalk", "twalk_r", "tdestroy", "ftw", "nftw", "ftw64",
        "nftw64", "pthread_create", "pthread_once", "pthread_atfork"};
    for (Function &F : *Composite) {
      if (!F.isDeclaration() || F.use_empty())
        continue;
      for (const char *Name : Unsupported) {
        if (F.getName() == Name) {
          error("'" + F.getName() +
                "' is not supported on CBC: its callback would be called by "
                "native code");
          Failed = true;
        }
      }
    }
    for (Function &F : *Composite) {
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          auto *CB = dyn_cast<CallBase>(&I);
          if (!CB)
            continue;
          const Function *Callee = CB->getCalledFunction();
          if (!Callee || !Callee->isDeclaration())
            continue;
          for (const Use &U : CB->args()) {
            const Value *V = U->stripPointerCasts();
            const auto *AF = dyn_cast<Function>(V);
            if (AF && !AF->isDeclaration()) {
              error("passing CBC function '" + AF->getName() +
                    "' to native function '" + Callee->getName() +
                    "'; native code calling CBC code is not supported");
              Failed = true;
            }
          }
        }
      }
    }
  } else {
    // Shared-library link: still reject pthread_atfork (fork, not attach).
    for (Function &F : *Composite) {
      if (!F.isDeclaration() || F.use_empty())
        continue;
      if (F.getName() == "pthread_atfork") {
        error("'pthread_atfork' is not supported on CBC: fork handlers are "
              "not N2C-attached");
        Failed = true;
      }
    }
  }
  if (Failed)
    return false;

  // Before codegen: SynthesizeEntry (in the emit pipeline) renames main→__cbc_main.
  if (Function *Main = Composite->getFunction("main"))
    out.hasMain = !Main->isDeclaration();
  else if (Function *Renamed = Composite->getFunction("__cbc_main"))
    out.hasMain = !Renamed->isDeclaration();

  SmallVector<char, 0> Buf;
  raw_svector_ostream OS(Buf);
  legacy::PassManager CodeGen;
  if (TM->addPassesToEmitFile(CodeGen, OS, nullptr,
                              CodeGenFileType::ObjectFile)) {
    error("CBC target cannot emit an object file");
    return false;
  }
  CodeGen.run(*Composite);
  if (errorCount())
    return false;

  out.bytes = std::move(Buf);
  out.triple = ExpectedTriple;

  // Copy method-offset exports from CBC codegen (index 0 == __cbc_lib_start).
  SmallVector<llvm::CBCExport, 0> CodegenExports =
      CBCTakeExportsFromTargetMachine(*TM);
  out.exports.clear();
  for (const llvm::CBCExport &E : CodegenExports) {
    // N2C stubs (wrap only) hardcode cpStackSize=0, so stubbed exports' Params
    // must fit the host register file. Container .cbc emit has no N2C stubs.
    // MaxCalleeStackArgs is outgoing call stack slots, not this prototype.
    bool NeedsN2CStub = request.wrap && (E.DefaultVis || E.AddressTaken);
    if (NeedsN2CStub && E.VarArg) {
      error("CBC native shared library: varargs export '" + E.Name +
            "' is not supported");
      return false;
    }
    unsigned Ints = 0, Floats = 0;
    for (char C : E.Params) {
      if (C == 'f')
        ++Floats;
      else
        ++Ints;
    }
    // SysV: 6 int + 8 SSE; AAPCS: 8 int + 8 SIMD. Use the looser int limit.
    if (NeedsN2CStub && (Ints > 8 || Floats > 8)) {
      error("CBC native shared library: export '" + E.Name +
            "' does not fit the host register file");
      return false;
    }
    cbc::CBCExport OutE;
    OutE.name = E.Name;
    OutE.offset = E.Offset;
    OutE.defaultVis = E.DefaultVis;
    OutE.addressTaken = E.AddressTaken;
    OutE.varArg = E.VarArg;
    OutE.params = E.Params;
    OutE.retFloat = E.RetFloat;
    OutE.maxCalleeStackArgs = E.MaxCalleeStackArgs;
    out.exports.push_back(std::move(OutE));
  }
  return errorCount() == 0;
}

bool cbc::link(const CBCLinkRequest &request) {
  CBCLinkResult Result;
  if (!linkToMemory(request, Result))
    return false;
  if (request.outputPath.empty()) {
    error("CBC link requires an output path");
    return false;
  }
  std::error_code EC;
  raw_fd_ostream Out(request.outputPath, EC, sys::fs::OF_None);
  if (EC) {
    error(EC.message());
    return false;
  }
  Out.write(Result.bytes.data(), Result.bytes.size());
  return !Out.has_error() && errorCount() == 0;
}
