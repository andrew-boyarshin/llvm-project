#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/Archive.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <utility>
#include <vector>
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

static cl::opt<std::string> Output("o", cl::desc("Output .cbc file"),
                                   cl::value_desc("file"));
static cl::opt<std::string> CrtPath("crt", cl::desc("Path to crt-cbc.bc"));
static cl::list<std::string> LibPaths("L", cl::Prefix, cl::desc("Library search path"));
static cl::list<std::string> Libs("l", cl::Prefix, cl::desc("Link a bitcode archive"));
static cl::list<std::string> Inputs(cl::Positional, cl::desc("<bitcode or archive>"));
static cl::opt<bool> NoValidate("no-native-validation", cl::init(true));

// Host libs the CBC launcher process already provides. When no bitcode
// archive is found for these names, that is expected — they are not missing
// inputs and must not be written into aotDeps.
static bool isLauncherHostLib(StringRef Name) {
  return Name == "c" || Name == "m" || Name == "pthread" || Name == "dl" ||
         Name == "rt" || Name == "gcc" || Name == "gcc_s" || Name == "resolv";
}

static std::string findArchive(StringRef Name) {
  std::string File = ("lib" + Name + ".a").str();
  for (const std::string &Dir : LibPaths) {
    SmallString<256> Path(Dir);
    sys::path::append(Path, File);
    if (sys::fs::exists(Path))
      return std::string(Path);
  }
  return "";
}

static std::unique_ptr<Module> loadIR(LLVMContext &Ctx, StringRef Path,
                                      SMDiagnostic &Err) {
  return parseIRFile(Path, Err, Ctx);
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "CBC whole-program linker\n");
  if (Output.empty() || Inputs.empty()) {
    errs() << "cbc-ld: expected -o <file> and at least one bitcode input\n";
    return 1;
  }

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  LLVMContext Ctx;
  SMDiagnostic Err;
  auto Composite = std::make_unique<Module>("cbc-ld", Ctx);
  Linker L(*Composite);

  if (!CrtPath.empty()) {
    auto Crt = loadIR(Ctx, CrtPath, Err);
    if (!Crt) {
      Err.print("cbc-ld", errs());
      return 1;
    }
    if (L.linkInModule(std::move(Crt))) {
      errs() << "cbc-ld: failed to link crt\n";
      return 1;
    }
  }
  std::vector<std::string> Archives;
  auto linkWhole = [&](StringRef Path) -> bool {
    auto M = loadIR(Ctx, Path, Err);
    if (!M) {
      Err.print("cbc-ld", errs());
      return false;
    }
    if (L.linkInModule(std::move(M))) {
      errs() << "cbc-ld: failed to link " << Path << "\n";
      return false;
    }
    return true;
  };
  for (const std::string &In : Inputs) {
    if (StringRef(In).ends_with(".a"))
      Archives.push_back(In);
    else if (!linkWhole(In))
      return 1;
  }
  std::vector<std::string> NativeLibs;
  for (const std::string &Lib : Libs) {
    std::string Path = findArchive(Lib);
    if (!Path.empty()) {
      // Bitcode archive (CBC guest code).
      Archives.push_back(Path);
    } else if (isLauncherHostLib(Lib)) {
      // Already in the launcher process; omit from aotDeps.
    } else {
      // Host shared library for the launcher to dlopen (name without lib/.so).
      NativeLibs.push_back(Lib);
    }
  }

  std::set<std::pair<std::string, uint64_t>> Extracted;
  for (int Round = 0; Round < 64; ++Round) {
    DenseSet<StringRef> Undef;
    for (Function &F : *Composite)
      if (F.isDeclaration() && !F.use_empty() && !F.isIntrinsic())
        Undef.insert(F.getName());
    for (GlobalVariable &GV : Composite->globals())
      if (GV.isDeclaration() && !GV.use_empty())
        Undef.insert(GV.getName());
    bool Progress = false;
    for (const std::string &Path : Archives) {
      ErrorOr<std::unique_ptr<MemoryBuffer>> Buf = MemoryBuffer::getFile(Path);
      if (!Buf) {
        errs() << "cbc-ld: cannot read " << Path << "\n";
        return 1;
      }
      Expected<std::unique_ptr<object::Archive>> Arch =
          object::Archive::create(Buf.get()->getMemBufferRef());
      if (!Arch) {
        errs() << "cbc-ld: not an archive: " << Path << "\n";
        consumeError(Arch.takeError());
        return 1;
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
        if (Extracted.count({Path, *Off}))
          continue;
        Needed.push_back({*Off, Name.str()});
      }
      for (const auto &Item : Needed) {
        if (!Extracted.insert({Path, Item.first}).second)
          continue;
        Expected<std::optional<object::Archive::Child>> ChildOrErr =
            (*Arch)->findSym(Item.second);
        if (!ChildOrErr || !*ChildOrErr) {
          if (!ChildOrErr)
            consumeError(ChildOrErr.takeError());
          continue;
        }
        Expected<StringRef> Member = (*ChildOrErr)->getBuffer();
        if (!Member) {
          errs() << "cbc-ld: bad archive member in " << Path << "\n";
          consumeError(Member.takeError());
          return 1;
        }
        std::unique_ptr<Module> M =
            parseIR(MemoryBufferRef(*Member, Item.second), Err, Ctx);
        if (!M) {
          Err.print("cbc-ld", errs());
          return 1;
        }
        if (L.linkInModule(std::move(M))) {
          errs() << "cbc-ld: failed to link member of " << Path
                 << " defining " << Item.second << "\n";
          return 1;
        }
        Progress = true;
      }
    }
    if (!Progress)
      break;
    if (Round == 63) {
      errs() << "cbc-ld: archive extraction did not converge\n";
      return 1;
    }
  }
  (void)NoValidate;
  for (GlobalValue &GV : Composite->global_values()) {
    if (!GV.isDeclaration() || GV.use_empty())
      continue;
    if (GV.getName().starts_with("_Z")) {
      errs() << "cbc-ld: error: native C++ symbol '" << GV.getName()
             << "' is not supported on CBC\n";
      return 1;
    }
  }
  Composite->setTargetTriple(Triple("cbc_x86_64-unknown-linux-gnu"));
  Composite->addModuleFlag(Module::Warning, "cbc-whole-program", uint32_t(1));
  if (!NativeLibs.empty()) {
    std::string Deps = NativeLibs[0];
    for (size_t I = 1; I < NativeLibs.size(); ++I) {
      Deps += ':';
      Deps += NativeLibs[I];
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

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  ModulePassManager MPM =
      PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
  MPM.run(*Composite, MAM);

  static const char *Unsupported[] = {
      "qsort", "qsort_r", "bsearch", "lfind", "lsearch", "tsearch", "tfind",
      "tdelete", "twalk", "twalk_r", "tdestroy", "ftw", "nftw", "ftw64",
      "nftw64", "pthread_create", "pthread_once", "pthread_atfork"};
  bool Failed = false;
  for (Function &F : *Composite) {
    if (!F.isDeclaration() || F.use_empty())
      continue;
    for (const char *Name : Unsupported) {
      if (F.getName() == Name) {
        errs() << "cbc-ld: error: '" << F.getName()
               << "' is not supported on CBC: its callback would be called by "
                  "native code\n";
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
            errs() << "cbc-ld: error: passing CBC function '" << AF->getName()
                   << "' to native function '" << Callee->getName()
                   << "'; native code calling CBC code is not supported\n";
            Failed = true;
          }
        }
      }
    }
  }
  if (Failed)
    return 1;

  std::string Error;
  const Target *T = TargetRegistry::lookupTarget(Composite->getTargetTriple(), Error);
  if (!T) {
    errs() << "cbc-ld: " << Error << "\n";
    return 1;
  }
  TargetOptions Options;
  auto TM = std::unique_ptr<TargetMachine>(T->createTargetMachine(
      Composite->getTargetTriple(), "", "", Options, Reloc::Static,
      CodeModel::Small, CodeGenOptLevel::Default));
  if (!TM) {
    errs() << "cbc-ld: cannot create target machine\n";
    return 1;
  }
  Composite->setDataLayout(TM->createDataLayout());

  std::error_code EC;
  ToolOutputFile Out(Output, EC, sys::fs::OF_None);
  if (EC) {
    errs() << "cbc-ld: " << EC.message() << "\n";
    return 1;
  }
  legacy::PassManager CodeGen;
  if (TM->addPassesToEmitFile(CodeGen, Out.os(), nullptr,
                              CodeGenFileType::ObjectFile)) {
    errs() << "cbc-ld: target cannot emit an object file\n";
    return 1;
  }
  CodeGen.run(*Composite);
  Out.keep();
  return 0;
}
