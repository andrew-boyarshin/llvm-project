//===--- Linux.h - Linux ToolChain Implementations --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LINUX_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LINUX_H

#include "Gnu.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace toolchains {

class LLVM_LIBRARY_VISIBILITY Linux : public Generic_ELF {
public:
  Linux(const Driver &D, const llvm::Triple &Triple,
        const llvm::opt::ArgList &Args);

  bool HasNativeLLVMSupport() const override;

  std::string getMultiarchTriple(const Driver &D,
                                 const llvm::Triple &TargetTriple,
                                 StringRef SysRoot) const override;

  void
  AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const override;
  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;
  void addLibStdCxxIncludePaths(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;
  void AddCudaIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;
  void AddHIPIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                         llvm::opt::ArgStringList &CC1Args) const override;
  void AddIAMCUIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                           llvm::opt::ArgStringList &CC1Args) const override;
  void addSYCLIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;

  void addOffloadRTLibs(unsigned ActiveKinds, const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;
  RuntimeLibType GetDefaultRuntimeLibType() const override;
  unsigned GetDefaultDwarfVersion() const override;
  CXXStdlibType GetDefaultCXXStdlibType() const override;
  CXXStdlibType GetCXXStdlibType(const llvm::opt::ArgList &Args) const override;
  bool
  IsAArch64OutlineAtomicsDefault(const llvm::opt::ArgList &Args) const override;
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override;
  bool isPICDefault() const override;
  bool isPICDefaultForced() const override;
  bool IsMathErrnoDefault() const override;
  SanitizerMask
  getSupportedSanitizers(BoundArch BA,
                         Action::OffloadKind DeviceOffloadKind) const override;
  void addProfileRTLibs(const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;
  std::string ComputeEffectiveClangTriple(
      const llvm::opt::ArgList &Args, BoundArch BA = {},
      types::ID InputType = types::TY_INVALID) const override;
  std::string computeSysRoot() const override;
  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                        Action::OffloadKind DeviceOffloadKind) const override;

  LTOKind getDefaultLTOMode() const override;
  LTOKind getLTOMode(const llvm::opt::ArgList &Args,
                     Action::OffloadKind Kind = Action::OFK_None) const override;

  std::string getDynamicLinker(const llvm::opt::ArgList &Args) const override;

  void addExtraOpts(llvm::opt::ArgStringList &CmdArgs) const override;

  std::vector<std::string> ExtraOpts;

  const char *getDefaultLinker() const override;

  bool isCBCMode() const { return CBCMode; }

protected:
  Tool *buildAssembler() const override;
  Tool *buildLinker() const override;
  Tool *buildStaticLibTool() const override;

private:
  /// Set when the driver was invoked with -fcbc on a host Linux triple.
  bool CBCMode = false;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_LINUX_H
