#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_CBC_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_CBC_H

#include "Linux.h"

namespace clang {
namespace driver {
namespace tools {
namespace cbc {
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

namespace toolchains {
class LLVM_LIBRARY_VISIBILITY CBCToolChain : public Linux {
public:
  CBCToolChain(const Driver &D, const llvm::Triple &Triple,
               const llvm::opt::ArgList &Args);
  LTOKind getDefaultLTOMode() const override { return LTOK_Full; }
  LTOKind getLTOMode(const llvm::opt::ArgList &Args,
                     Action::OffloadKind Kind) const override;
  void addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                             llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                             Action::OffloadKind) const override;
  void AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                 llvm::opt::ArgStringList &CC1Args) const override;
  void AddClangCXXStdlibIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                    llvm::opt::ArgStringList &CC1Args) const override;
  CXXStdlibType GetCXXStdlibType(const llvm::opt::ArgList &Args) const override;
  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &) const override { return false; }
  bool isPICDefaultForced() const override { return false; }
  std::string getMultiarchTriple(const Driver &D,
                                 const llvm::Triple &TargetTriple,
                                 StringRef SysRoot) const override;
  const char *getDefaultLinker() const override { return "ld.lld"; }

protected:
  Tool *buildLinker() const override { return new tools::cbc::Linker(*this); }
};
} // namespace toolchains
} // namespace driver
} // namespace clang

#endif
