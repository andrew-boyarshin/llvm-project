#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_CBC_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_CBC_H

#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/TargetParser/Triple.h"
#include <string>

namespace clang {
namespace driver {
namespace tools {
namespace cbc {
/// Name of the per-flavor install directory component: "cbc.<host-triple>",
/// e.g. "cbc.x86_64-unknown-linux-gnu". Used as
///   <prefix>/lib/<name>                       crt, libc++, libc++abi, native/
///   <prefix>/lib/clang/<ver>/lib/<name>       compiler-rt builtins
///   <prefix>/include/<name>[/c++/v1]          CBC-only headers, __config_site
/// The "cbc." prefix keeps these apart from native per-target runtime dirs.
std::string getTargetDirName(const llvm::Triple &T);

/// CBC image linker: invokes ld.lld --cbc (not the host ELF linker).
class LLVM_LIBRARY_VISIBILITY Linker : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("cbc::Linker", "ld.lld", TC) {}
  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};
} // namespace cbc
} // namespace tools
} // namespace driver
} // namespace clang

#endif
