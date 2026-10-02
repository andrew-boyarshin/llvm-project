#include "CBC.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/Support/Path.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

CBCToolChain::CBCToolChain(const Driver &D, const llvm::Triple &Triple,
                           const ArgList &Args)
    : Linux(D, Triple, Args) {}

std::string CBCToolChain::getMultiarchTriple(const Driver &D,
                                             const llvm::Triple &TargetTriple,
                                             StringRef SysRoot) const {
  // Host glibc headers live in the host multiarch directory
  // (/usr/include/x86_64-linux-gnu/bits/...), not under the CBC triple.
  if (TargetTriple.isCBCHostX86_64())
    return "x86_64-linux-gnu";
  if (TargetTriple.isCBCHostAArch64())
    return "aarch64-linux-gnu";
  return Linux::getMultiarchTriple(D, TargetTriple, SysRoot);
}

LTOKind CBCToolChain::getLTOMode(const ArgList &Args,
                                 Action::OffloadKind Kind) const {
  if (Args.hasArg(options::OPT_fno_lto)) {
    getDriver().Diag(diag::err_drv_unsupported_opt)
        << "-fno-lto (separate CBC code generation is not supported)";
    return LTOK_Full;
  }
  if (!Args.hasArg(options::OPT_flto_EQ, options::OPT_fno_lto))
    return LTOK_Full;
  return Linux::getLTOMode(Args, Kind);
}

void CBCToolChain::addClangTargetOptions(const ArgList &DriverArgs,
                                         ArgStringList &CC1Args, BoundArch,
                                         Action::OffloadKind) const {
  if (getTriple().isCBCHostAArch64() && DriverArgs.hasArg(options::OPT_c) &&
      !DriverArgs.hasArg(options::OPT_E) &&
      !DriverArgs.hasArg(options::OPT_fsyntax_only)) {
    getDriver().Diag(diag::err_drv_unsupported_opt)
        << "compilation for the AArch64 flavour of CBC";
  }
  CC1Args.push_back("-femulated-tls");
  CC1Args.push_back("-fno-jump-tables");
  CC1Args.push_back("-fno-common");
}

void CBCToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                             ArgStringList &CC1Args) const {
  if (!DriverArgs.hasArg(options::OPT_nostdinc) &&
      !DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<256> Inc(getDriver().Dir);
    llvm::sys::path::append(Inc, "..", "lib", "cbc", "include");
    addSystemInclude(DriverArgs, CC1Args, Inc);
  }
  Linux::AddClangSystemIncludeArgs(DriverArgs, CC1Args);
}

ToolChain::CXXStdlibType
CBCToolChain::GetCXXStdlibType(const ArgList &Args) const {
  if (Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)) {
    StringRef Name = A->getValue();
    if (Name != "libc++") {
      getDriver().Diag(diag::err_drv_invalid_stdlib_name)
          << A->getAsString(Args);
    }
  }
  return ToolChain::CST_Libcxx;
}

void CBCToolChain::AddClangCXXStdlibIncludeArgs(const ArgList &DriverArgs,
                                                ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdincxx))
    return;
  GetCXXStdlibType(DriverArgs);
  SmallString<256> Inc(getDriver().Dir);
  llvm::sys::path::append(Inc, "..", "lib", "cbc", "include");
  llvm::sys::path::append(Inc, "c++", "v1");
  CC1Args.push_back("-internal-isystem");
  CC1Args.push_back(DriverArgs.MakeArgString(Inc));
}

void cbc::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                               const InputInfo &Output,
                               const InputInfoList &Inputs, const ArgList &Args,
                               const char *) const {
  ArgStringList CmdArgs;
  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  SmallString<256> Crt(getToolChain().getDriver().Dir);
  llvm::sys::path::append(Crt, "..", "lib", "cbc", "crt-cbc.bc");
  CmdArgs.push_back("--crt");
  CmdArgs.push_back(Args.MakeArgString(Crt));
  CmdArgs.push_back("--no-native-validation");

  AddLinkerInputs(getToolChain(), Inputs, Args, CmdArgs, JA);

  SmallString<256> CbcLib(getToolChain().getDriver().Dir);
  llvm::sys::path::append(CbcLib, "..", "lib", "cbc");
  SmallString<256> LibDir(CbcLib);
  llvm::sys::path::append(LibDir, "lib");
  CmdArgs.push_back("-L");
  CmdArgs.push_back(Args.MakeArgString(LibDir));
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

  const char *Prog =
      Args.MakeArgString(getToolChain().GetProgramPath("cbc-ld"));
  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::None(), Prog, CmdArgs, Inputs, Output));
}
