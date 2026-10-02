#include "CBCSubtarget.h"
#include "llvm/Analysis/LibcallLoweringInfo.h"
#include "llvm/IR/RuntimeLibcalls.h"

#define GET_SUBTARGETINFO_CTOR
#include "CBCGenSubtargetInfo.inc"

using namespace llvm;

CBCSubtarget::CBCSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                           const TargetMachine &TM)
    : CBCGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS), TT(TT), InstrInfo(*this),
      FrameLowering(), TLInfo(TM, *this) {}

void CBCSubtarget::initLibcallLoweringInfo(LibcallLoweringInfo &Info) const {
  // The CBC triple is not in the default runtime-libcall set, so float
  // libcalls (ceil, and the other libm ops left as libcalls) have no impl.
  // Host libm provides the usual names.
  static const struct {
    RTLIB::Libcall Op;
    RTLIB::LibcallImpl Impl;
  } Libm[] = {
      {RTLIB::CEIL_F64, RTLIB::impl_ceil},
      {RTLIB::CEIL_F32, RTLIB::impl_ceilf},
      {RTLIB::FLOOR_F64, RTLIB::impl_floor},
      {RTLIB::FLOOR_F32, RTLIB::impl_floorf},
      {RTLIB::TRUNC_F64, RTLIB::impl_trunc},
      {RTLIB::TRUNC_F32, RTLIB::impl_truncf},
      {RTLIB::ROUND_F64, RTLIB::impl_round},
      {RTLIB::ROUND_F32, RTLIB::impl_roundf},
      {RTLIB::NEARBYINT_F64, RTLIB::impl_nearbyint},
      {RTLIB::NEARBYINT_F32, RTLIB::impl_nearbyintf},
      {RTLIB::RINT_F64, RTLIB::impl_rint},
      {RTLIB::RINT_F32, RTLIB::impl_rintf},
      {RTLIB::REM_F64, RTLIB::impl_fmod},
      {RTLIB::REM_F32, RTLIB::impl_fmodf},
      {RTLIB::FMIN_F64, RTLIB::impl_fmin},
      {RTLIB::FMIN_F32, RTLIB::impl_fminf},
      {RTLIB::FMAX_F64, RTLIB::impl_fmax},
      {RTLIB::FMAX_F32, RTLIB::impl_fmaxf},
      {RTLIB::FMA_F64, RTLIB::impl_fma},
      {RTLIB::FMA_F32, RTLIB::impl_fmaf},
      {RTLIB::FPTOSINT_F32_I128, RTLIB::impl___fixsfti},
      {RTLIB::FPTOSINT_F64_I128, RTLIB::impl___fixdfti},
  };
  for (const auto &LC : Libm)
    Info.setLibcallImpl(LC.Op, LC.Impl);
}
