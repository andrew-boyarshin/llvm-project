#include "CBCFrameLowering.h"
#include "CBCTargetMachine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

bool CBCFrameLowering::hasFPImpl(const MachineFunction &) const { return false; }

void CBCFrameLowering::emitPrologue(MachineFunction &,
                                    MachineBasicBlock &) const {}

void CBCFrameLowering::emitEpilogue(MachineFunction &,
                                    MachineBasicBlock &) const {}

void CBCFrameLowering::processFunctionBeforeFrameFinalized(
    MachineFunction &MF, RegScavenger *) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  auto *Info = MF.getInfo<CBCMachineFunctionInfo>();
  unsigned Slot = Info->MaxOutgoingSlots;
  for (int I = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd(); I != E;
       ++I) {
    if (MFI.isDeadObjectIndex(I) || MFI.isFixedObjectIndex(I))
      continue;
    if (!MFI.isSpillSlotObjectIndex(I)) {
      Align A = MFI.getObjectAlign(I);
      if (A.value() > 16) {
        MFI.setObjectSize(I, MFI.getObjectSize(I) + static_cast<int64_t>(A.value()) - 1);
        MFI.setObjectAlignment(I, Align(16));
      }
      continue;
    }
    uint64_t Bytes = static_cast<uint64_t>(MFI.getObjectSize(I));
    unsigned Count = static_cast<unsigned>((Bytes + 7) / 8);
    if (Count == 0)
      Count = 1;
    if (Slot + Count > 65535)
      report_fatal_error("CBC function needs more than 65535 untyped slots");
    MFI.setStackID(I, TargetStackID::NoAlloc);
    MFI.setObjectOffset(I, Slot);
    Slot += Count;
  }
}
