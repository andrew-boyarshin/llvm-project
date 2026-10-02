#ifndef LLVM_LIB_TARGET_CBC_CBCFILEWRITER_H
#define LLVM_LIB_TARGET_CBC_CBCFILEWRITER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace llvm {

struct CBCCompiledMethod {
  std::string Name;
  std::vector<uint8_t> Code;
  /// Byte offsets of state points (the instruction after each call).
  std::vector<uint32_t> StatePoints;
  /// 'i' or 'f' per parameter. Non-float results use the I64 term, matching calls.
  std::string Params;
  bool RetFloat = false;
  unsigned UntypedSlots = 0;
  uint32_t UntypedMemSize = 0;
  uint8_t UsesAlloca = 0;
  /// Outgoing untyped slots used as stack arguments (slots 0..N-1).
  unsigned MaxCalleeStackArgs = 0;
  uint8_t IMask = 0;
  uint8_t FMask = 0;
  /// Term ids of TUPLE slots. Only `__cbc_image_init` uses these, for InitString.
  std::vector<uint32_t> TypedSlots;
  /// AsmPrinter sets this so the file writer can prepend the InitString prologue
  /// once the string-pool offsets of the image blobs are known.
  bool NeedsImagePrologue = false;
  struct ExRegion {
    uint32_t Start = 0, End = 0, Target = 0;
  };
  std::vector<ExRegion> Regions;
};

struct CBCNativeCallee {
  std::string Name;
  bool Native = true;
  /// Argument classes in call order: 'i' integer register, 'f' float register.
  std::string Params;
  bool RetFloat = false;
};

struct CBCImageInfo {
  uint32_t SizeBytes = 0;
  /// Non-zero image prefix. Empty skips the memcpy.
  std::string Blob;
  /// Little-endian u32 file offsets for IMAGE_ABS64 slots. Empty skips apply.
  std::string Relocs;
};

/// Write a whole-program `.cbc` whose entry method is `main`.
/// AotDeps is a ':'-separated list of native library names (without lib/ .so).
std::string buildCBCFile(ArrayRef<CBCCompiledMethod> Methods,
                         ArrayRef<CBCNativeCallee> Natives = {},
                         CBCImageInfo Image = {},
                         StringRef AotDeps = {});

} // namespace llvm

#endif
