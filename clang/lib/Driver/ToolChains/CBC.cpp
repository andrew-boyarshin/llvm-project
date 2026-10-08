#include "CBC.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include <optional>

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

void cbc::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                               const InputInfo &Output,
                               const InputInfoList &Inputs, const ArgList &Args,
                               const char *) const {
  ArgStringList CmdArgs;
  // --cbc must be first so ELF option parsing selects CBC mode.
  CmdArgs.push_back("--cbc");
  if (Args.hasArg(options::OPT_shared))
    CmdArgs.push_back("-shared");
  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  SmallString<256> Crt(getToolChain().getDriver().Dir);
  llvm::sys::path::append(Crt, "..", "lib", "cbc", "crt-cbc.bc");
  CmdArgs.push_back("--crt");
  CmdArgs.push_back(Args.MakeArgString(Crt));
  CmdArgs.push_back("--no-native-validation");

  SmallString<256> CbcLib(getToolChain().getDriver().Dir);
  llvm::sys::path::append(CbcLib, "..", "lib", "cbc");
  SmallString<256> LibDir(CbcLib);
  llvm::sys::path::append(LibDir, "lib");
  // -L before -l: ELF lld resolves libraries in argv order. CBC lib first
  // so -lc++ finds bitcode libc++.a before any host libc++.so.
  CmdArgs.push_back("-L");
  CmdArgs.push_back(Args.MakeArgString(LibDir));
  // Wrap (-shared) links -lcbcengine / -l:libcangjie-runtime.so; those live
  // under CANGJIE_HOME. Inject before user -L so the SDK wins by default.
  if (Args.hasArg(options::OPT_shared)) {
    std::optional<std::string> CjHome =
        llvm::sys::Process::GetEnv("CANGJIE_HOME");
    if (!CjHome || CjHome->empty()) {
      getToolChain().getDriver().Diag(diag::err_drv_cbc_missing_cangjie_home);
      return;
    }
    SmallString<256> ToolsLib(*CjHome);
    llvm::sys::path::append(ToolsLib, "tools", "lib");
    CmdArgs.push_back("-L");
    CmdArgs.push_back(Args.MakeArgString(ToolsLib));
    const char *RuntimeSubdir = nullptr;
    const llvm::Triple &TT = getToolChain().getTriple();
    if (TT.getArch() == llvm::Triple::x86_64 || TT.isCBCHostX86_64())
      RuntimeSubdir = "linux_x86_64_cjnative";
    else if (TT.getArch() == llvm::Triple::aarch64 || TT.isCBCHostAArch64())
      RuntimeSubdir = "linux_aarch64_cjnative";
    if (RuntimeSubdir) {
      SmallString<256> RuntimeLib(*CjHome);
      llvm::sys::path::append(RuntimeLib, "runtime", "lib", RuntimeSubdir);
      CmdArgs.push_back("-L");
      CmdArgs.push_back(Args.MakeArgString(RuntimeLib));
    }
  }
  Args.addAllArgs(CmdArgs, {options::OPT_L});
  getToolChain().AddFilePathLibArgs(Args, CmdArgs);

  AddLinkerInputs(getToolChain(), Inputs, Args, CmdArgs, JA);

  if (getToolChain().ShouldLinkCXXStdlib(Args)) {
    CmdArgs.push_back("-lc++");
    CmdArgs.push_back("-lc++abi");
  } else {
    SmallString<256> Stub(CbcLib);
    llvm::sys::path::append(Stub, "cbc_can_catch_stub.bc");
    CmdArgs.push_back(Args.MakeArgString(Stub));
  }
  SmallString<256> Builtins(LibDir);
  llvm::sys::path::append(Builtins, "libclang_rt.builtins-cbc.a");
  CmdArgs.push_back(Args.MakeArgString(Builtins));

  // Do not call gnutools::Linker::ConstructJob (crt1.o, -dynamic-linker, -lc).
  const char *Prog = Args.MakeArgString(getToolChain().GetLinkerPath());
  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::None(), Prog, CmdArgs, Inputs, Output));
}
