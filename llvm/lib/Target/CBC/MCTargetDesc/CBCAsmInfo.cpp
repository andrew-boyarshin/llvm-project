#include "CBCAsmInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

CBCAsmInfo::CBCAsmInfo(const Triple &, const MCTargetOptions &Options)
    : MCAsmInfo(Options) {
  CommentString = "//";
  InternalSymbolPrefix = ".L";
  CodePointerSize = 8;
  // Keep invoke instructions through instruction selection. WinEHPrepare
  // ignores the GNU personality, so it does not rewrite these landing pads.
  ExceptionsType = ExceptionHandling::Wasm;
}
