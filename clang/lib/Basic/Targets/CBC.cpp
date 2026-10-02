#include "CBC.h"
#include "clang/Basic/MacroBuilder.h"

using namespace clang;
using namespace clang::targets;

CBCTargetInfo::CBCTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
    : TargetInfo(Triple) {
  LongWidth = LongAlign = PointerWidth = PointerAlign = 64;
  IntMaxType = SignedLong;
  Int64Type = SignedLong;
  SizeType = UnsignedLong;
  PtrDiffType = SignedLong;
  IntPtrType = SignedLong;
  LongDoubleWidth = LongDoubleAlign = 64;
  LongDoubleFormat = &llvm::APFloat::IEEEdouble();
  DoubleAlign = 64;
  LongLongAlign = 64;
  SuitableAlign = 128;
  MaxAtomicPromoteWidth = MaxAtomicInlineWidth = 64;
  HasUnalignedAccess = true;
  WCharType = SignedInt;
  WIntType = SignedInt;
  resetDataLayout();
}

void CBCTargetInfo::getTargetDefines(const LangOptions &,
                                     MacroBuilder &Builder) const {
  Builder.defineMacro("__CBC__");
  Builder.defineMacro("__cbc__");
  // Host identity macros. glibc's bits/ headers (word size, gnu/stubs-*.h)
  // select the ABI from these, not from the CBC triple.
  if (getTriple().isCBCHostX86_64()) {
    Builder.defineMacro("__cbc_x86_64__");
    Builder.defineMacro("__x86_64__");
    Builder.defineMacro("__x86_64");
    Builder.defineMacro("__amd64__");
    Builder.defineMacro("__amd64");
  }
  if (getTriple().isCBCHostAArch64()) {
    Builder.defineMacro("__cbc_aarch64__");
    Builder.defineMacro("__aarch64__");
    Builder.defineMacro("__AARCH64EL__");
    Builder.defineMacro("__ARM_64BIT_STATE");
    Builder.defineMacro("__ARM_PCS_AAPCS64");
    Builder.defineMacro("__ARM_ARCH", "8");
    Builder.defineMacro("__ARM_ARCH_PROFILE", "'A'");
  }
  Builder.defineMacro("__SIZEOF_LONG_DOUBLE__", "8");
}
