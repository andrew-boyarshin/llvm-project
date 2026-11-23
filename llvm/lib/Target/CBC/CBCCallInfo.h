//===- CBCCallInfo.h - signature / argument locations of CBC calls --------===//
//
// LowerCall appends a block of immediates to every CBC::CALL / CBC::CALL_INDIRECT
// (after the legacy "register arguments" block that AdjustInstrPostInstrSelection
// consumes). The AsmPrinter turns it into the engine's call encoding:
//
//   call.direct   uleb methodId, Location list
//   call.indirect [target:0] uleb methodId, Location list
//
// where `methodId` is a method reference of ABI kind C whose signature is the
// *lowered* signature below (see agents/13-engine-changes.md, E6 / E7).
//
//   imms = [ NSig, kind_0 .. kind_{NSig-1}, NLoc, loc_0 .. loc_{NLoc-1} ]
//
//   kind: SigInt / SigFloat for an argument, SigVarArgLabel marks the start of
//         the variadic arguments (it has no location).
//   loc:  engine `Location` index of the *source* of each argument, in
//         signature order: an integer register, VirtRegCount + float register,
//         or LocRegCount + untyped slot.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_CBC_CBCCALLINFO_H
#define LLVM_LIB_TARGET_CBC_CBCCALLINFO_H

namespace llvm {
namespace CBCCall {

enum SigKind : int { SigInt = 0, SigFloat = 1, SigVarArgLabel = 2 };

/// IReg::VIRT_COUNT / FReg::COUNT of the engine (src/cbc/isa.h): IR0 (IRZ) ..
/// IR13 are addressable as sources, IRACC is not.
constexpr unsigned VirtRegCount = 14;
constexpr unsigned FRegCount = 16;
constexpr unsigned LocRegCount = VirtRegCount + FRegCount;

constexpr unsigned intRegLoc(unsigned EncodedIReg) { return EncodedIReg; }
constexpr unsigned floatRegLoc(unsigned EncodedFReg) {
  return VirtRegCount + EncodedFReg;
}
constexpr unsigned slotLoc(unsigned Slot) { return LocRegCount + Slot; }

} // namespace CBCCall
} // namespace llvm

#endif
