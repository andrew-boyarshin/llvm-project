#include "CBC.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Pass.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

using namespace llvm;

#define DEBUG_TYPE "cbc-lower-globals"
#define PASS_NAME "CBC lower globals into one data image"

namespace {
struct Placed {
  GlobalVariable *GV;
  uint64_t Offset = 0;
  uint64_t Size = 0;
  Align Alignment;
  bool Initialized = false;
};

enum class RelocKind { ImageAbs, FnPtr, NativeData };

struct ImageReloc {
  uint64_t Slot = 0;
  GlobalValue *Target = nullptr;
  int64_t Addend = 0;
  RelocKind Kind = RelocKind::ImageAbs;
};

class CBCLowerGlobalsLegacy : public ModulePass {
public:
  static char ID;
  CBCLowerGlobalsLegacy() : ModulePass(ID) {}
  bool runOnModule(Module &M) override;
};

static void writeBytes(uint8_t *Dst, uint64_t Value, unsigned Bytes) {
  for (unsigned I = 0; I < Bytes; ++I)
    Dst[I] = (Value >> (8 * I)) & 0xFF;
}

static void badInitializer(const Constant *C) {
  std::string Msg;
  raw_string_ostream OS(Msg);
  OS << "CBC global initializer is not supported: ";
  C->printAsOperand(OS, false);
  report_fatal_error(Twine(Msg));
}

static void writeConstant(uint8_t *Image, uint64_t At, Constant *C,
                          const DataLayout &DL,
                          const DenseMap<const GlobalVariable *, uint64_t> &Off,
                          SmallVectorImpl<ImageReloc> &Relocs);

static void writePointer(uint8_t *Image, uint64_t At, Constant *C,
                         const DataLayout &DL,
                         const DenseMap<const GlobalVariable *, uint64_t> &Off,
                         SmallVectorImpl<ImageReloc> &Relocs) {
  int64_t Addend = 0;
  Constant *Cur = C;
  while (auto *CE = dyn_cast<ConstantExpr>(Cur)) {
    unsigned Op = CE->getOpcode();
    if (Op == Instruction::BitCast || Op == Instruction::AddrSpaceCast ||
        Op == Instruction::IntToPtr) {
      Cur = cast<Constant>(CE->getOperand(0));
      continue;
    }
    if (Op == Instruction::GetElementPtr) {
      auto *GEP = cast<GEPOperator>(CE);
      APInt Delta(DL.getIndexSizeInBits(GEP->getPointerAddressSpace()), 0);
      if (!GEP->accumulateConstantOffset(DL, Delta))
        badInitializer(C);
      Addend += Delta.getSExtValue();
      Cur = cast<Constant>(GEP->getPointerOperand());
      continue;
    }
    badInitializer(C);
  }
  if (auto *GA = dyn_cast<GlobalAlias>(Cur))
    Cur = GA->getAliasee();
  if (auto *CI = dyn_cast<ConstantInt>(Cur)) {
    writeBytes(Image + At, CI->getZExtValue() + static_cast<uint64_t>(Addend), 8);
    return;
  }
  if (Cur->isNullValue()) {
    writeBytes(Image + At, static_cast<uint64_t>(Addend), 8);
    return;
  }
  if (auto *GV = dyn_cast<GlobalVariable>(Cur)) {
    if (GV->isDeclaration()) {
      Relocs.push_back({At, GV, Addend, RelocKind::NativeData});
      return;
    }
    auto It = Off.find(GV);
    if (It == Off.end())
      badInitializer(C);
    writeBytes(Image + At, It->second + static_cast<uint64_t>(Addend), 8);
    Relocs.push_back({At, nullptr, 0, RelocKind::ImageAbs});
    return;
  }
  if (auto *F = dyn_cast<Function>(Cur)) {
    if (Addend)
      badInitializer(C);
    Relocs.push_back({At, F, 0, RelocKind::FnPtr});
    return;
  }
  badInitializer(C);
}

static void writeConstant(uint8_t *Image, uint64_t At, Constant *C,
                          const DataLayout &DL,
                          const DenseMap<const GlobalVariable *, uint64_t> &Off,
                          SmallVectorImpl<ImageReloc> &Relocs) {
  if (C->getType()->isPointerTy()) {
    writePointer(Image, At, C, DL, Off, Relocs);
    return;
  }
  if (auto *CDS = dyn_cast<ConstantDataSequential>(C)) {
    StringRef Raw = CDS->getRawDataValues();
    memcpy(Image + At, Raw.data(), Raw.size());
    return;
  }
  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    unsigned Bytes = (CI->getType()->getIntegerBitWidth() + 7) / 8;
    APInt Bits = CI->getValue().zext(Bytes * 8);
    for (unsigned I = 0; I < Bytes; ++I)
      Image[At + I] = Bits.extractBits(8, I * 8).getZExtValue();
    return;
  }
  if (auto *CF = dyn_cast<ConstantFP>(C)) {
    APInt Bits = CF->getValueAPF().bitcastToAPInt();
    unsigned Bytes = Bits.getBitWidth() / 8;
    for (unsigned I = 0; I < Bytes; ++I)
      Image[At + I] = Bits.extractBits(8, I * 8).getZExtValue();
    return;
  }
  if (auto *CS = dyn_cast<ConstantStruct>(C)) {
    const StructLayout *SL = DL.getStructLayout(CS->getType());
    for (unsigned I = 0, E = CS->getNumOperands(); I != E; ++I)
      writeConstant(Image, At + SL->getElementOffset(I),
                    cast<Constant>(CS->getOperand(I)), DL, Off, Relocs);
    return;
  }
  if (auto *CA = dyn_cast<ConstantArray>(C)) {
    Type *Elem = CA->getType()->getElementType();
    uint64_t Stride = DL.getTypeAllocSize(Elem);
    for (unsigned I = 0, E = CA->getNumOperands(); I != E; ++I)
      writeConstant(Image, At + I * Stride, cast<Constant>(CA->getOperand(I)),
                    DL, Off, Relocs);
    return;
  }
  if (auto *CV = dyn_cast<ConstantVector>(C)) {
    Type *Elem = CV->getType()->getElementType();
    uint64_t Stride = DL.getTypeAllocSize(Elem);
    for (unsigned I = 0, E = CV->getNumOperands(); I != E; ++I)
      writeConstant(Image, At + I * Stride, cast<Constant>(CV->getOperand(I)),
                    DL, Off, Relocs);
    return;
  }
  if (C->isNullValue() || isa<UndefValue>(C) || isa<PoisonValue>(C))
    return;
  badInitializer(C);
}

static std::string symbolKey(StringRef Name) {
  std::string Key;
  for (unsigned char C : Name)
    Key.push_back((C >= '0' && C <= '9') || (C >= 'A' && C <= 'Z') ||
                          (C >= 'a' && C <= 'z') || C == '_'
                      ? static_cast<char>(C)
                      : '_');
  if (Key.empty())
    Key = "sym";
  return Key;
}

static Function *dlsymHelper(Module &M, StringRef Name, GlobalVariable *Cache) {
  LLVMContext &Ctx = M.getContext();
  Type *Ptr = PointerType::get(Ctx, 0);
  Type *I8 = Type::getInt8Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  Function *F = Function::Create(FunctionType::get(Ptr, false),
                                 Function::InternalLinkage,
                                 "__cbc_dlsym_" + symbolKey(Name), &M);
  F->addFnAttr(Attribute::NoInline);
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *Miss = BasicBlock::Create(Ctx, "miss", F);
  BasicBlock *Done = BasicBlock::Create(Ctx, "done", F);
  IRBuilder<> B(Entry);
  ArrayType *NameTy = ArrayType::get(I8, Name.size() + 1);
  AllocaInst *Buf = B.CreateAlloca(NameTy, nullptr, "sym");
  Value *Cached = B.CreatePtrToInt(B.CreateLoad(Ptr, Cache), I64);
  B.CreateCondBr(B.CreateICmpEQ(Cached, B.getInt64(0)), Miss, Done);
  B.SetInsertPoint(Miss);
  for (size_t I = 0; I <= Name.size(); ++I) {
    Value *Slot = B.CreateInBoundsGEP(
        NameTy, Buf, {B.getInt32(0), B.getInt32(static_cast<unsigned>(I))});
    B.CreateStore(B.getInt8(I == Name.size() ? 0 : Name[I]), Slot);
  }
  FunctionCallee Dlsym = M.getOrInsertFunction(
      "dlsym", FunctionType::get(Ptr, {Ptr, Ptr}, false));
  Value *Found = B.CreateCall(Dlsym, {ConstantPointerNull::get(cast<PointerType>(Ptr)), Buf});
  B.CreateStore(Found, Cache);
  Value *FoundI = B.CreatePtrToInt(Found, I64);
  B.CreateBr(Done);
  B.SetInsertPoint(Done);
  PHINode *VI = B.CreatePHI(I64, 2);
  VI->addIncoming(Cached, Entry);
  VI->addIncoming(FoundI, Miss);
  B.CreateRet(B.CreateIntToPtr(VI, Ptr));
  return F;
}
} // namespace

char CBCLowerGlobalsLegacy::ID = 0;
INITIALIZE_PASS(CBCLowerGlobalsLegacy, DEBUG_TYPE, PASS_NAME, false, false)

bool CBCLowerGlobalsLegacy::runOnModule(Module &M) {
  const DataLayout &DL = M.getDataLayout();
  LLVMContext &Ctx = M.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);

  // Empty inline asm is used as a compiler barrier or as a tied register
  // copy (GCC torture). CBC has no native asm — expand to copies / delete.
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : llvm::make_early_inc_range(BB)) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI || !CI->isInlineAsm())
          continue;
        auto *IA = cast<InlineAsm>(CI->getCalledOperand());
        if (!IA->getAsmString().empty())
          report_fatal_error("CBC does not support non-empty inline assembly");
        if (CI->getType()->isVoidTy()) {
          CI->eraseFromParent();
          continue;
        }
        // Empty asm does not change its output. The result is the input tied
        // to that output ("=r,0"(x), or "=r,r,0"(&other, x) when another
        // input comes first). Falling back to argument 0 covers a single tie.
        if (CI->arg_empty())
          report_fatal_error("CBC empty inline asm produced a value with no input");
        unsigned ArgIdx = 0;
        SmallVector<StringRef, 8> Parts;
        IA->getConstraintString().split(Parts, ',');
        unsigned NumOut = 0;
        for (StringRef P : Parts) {
          P = P.trim();
          if (P.empty() || P.starts_with("~"))
            break;
          if (P.contains('=') || P.contains('+'))
            ++NumOut;
          else
            break;
        }
        unsigned InIdx = 0;
        unsigned PartIdx = 0;
        for (StringRef P : Parts) {
          P = P.trim();
          if (P.empty() || P.starts_with("~"))
            break;
          if (PartIdx++ < NumOut)
            continue;
          if (P == "0") {
            ArgIdx = InIdx;
            break;
          }
          ++InIdx;
        }
        if (ArgIdx >= CI->arg_size())
          ArgIdx = 0;
        Value *In = CI->getArgOperand(ArgIdx);
        if (In->getType() != CI->getType())
          In = CastInst::CreateBitOrPointerCast(In, CI->getType(), "",
                                                CI->getIterator());
        CI->replaceAllUsesWith(In);
        CI->eraseFromParent();
      }
    }
  }

  // Flatten aliases while preserving GEP/bitcast aliasees (not just the
  // base GlobalObject). RAUW all aliases before erasing so alias chains and
  // ConstantExpr users are updated safely.
  SmallVector<GlobalAlias *, 8> Aliases;
  for (GlobalAlias &A : M.aliases())
    Aliases.push_back(&A);
  for (GlobalAlias *A : Aliases) {
    Constant *Aliasee = A->getAliasee();
    if (!Aliasee)
      report_fatal_error("CBC alias has no aliasee");
    A->replaceAllUsesWith(Aliasee);
  }
  for (GlobalAlias *A : Aliases)
    A->eraseFromParent();

  uint64_t NextBlockCookie = 1;
  DenseMap<const BasicBlock *, uint64_t> BlockCookies;
  SmallVector<BlockAddress *, 8> BlockAddrs;
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      if (BlockAddress *BA = BlockAddress::lookup(&BB))
        if (!BA->use_empty())
          BlockAddrs.push_back(BA);
    }
  }
  for (BlockAddress *BA : BlockAddrs) {
    const BasicBlock *BB = BA->getBasicBlock();
    uint64_t &Id = BlockCookies[BB];
    if (!Id)
      Id = NextBlockCookie++;
    Constant *Cookie = ConstantExpr::getIntToPtr(
        ConstantInt::get(I64, Id), BA->getType());
    BA->replaceAllUsesWith(Cookie);
  }

  DenseMap<GlobalVariable *, Function *> NativeAddr;
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.isDeclaration() || GV.use_empty() || GV.getName().starts_with("llvm."))
      continue;
    GlobalVariable *Cache = new GlobalVariable(
        M, GV.getValueType(), false, GlobalValue::InternalLinkage,
        Constant::getNullValue(GV.getValueType()),
        "__cbc_dlcache_" + symbolKey(GV.getName()));
    NativeAddr[&GV] = dlsymHelper(M, GV.getName(), Cache);
  }

  SmallVector<Placed, 8> Items;
  for (GlobalVariable &GV : M.globals()) {
    if (GV.isDeclaration() || GV.getName().starts_with("llvm."))
      continue;
    if (!GV.hasInitializer())
      continue;
    Placed P;
    P.GV = &GV;
    P.Size = DL.getTypeAllocSize(GV.getValueType());
    P.Alignment = GV.getAlign().valueOrOne();
    if (P.Alignment < DL.getABITypeAlign(GV.getValueType()))
      P.Alignment = DL.getABITypeAlign(GV.getValueType());
    P.Initialized = !GV.getInitializer()->isNullValue();
    Items.push_back(P);
  }
  if (Items.empty())
    return false;

  llvm::stable_sort(Items, [](const Placed &A, const Placed &B) {
    if (A.Initialized != B.Initialized)
      return A.Initialized > B.Initialized;
    if (A.Alignment != B.Alignment)
      return A.Alignment > B.Alignment;
    return A.Size > B.Size;
  });

  uint64_t Cursor = 0;
  uint64_t InitEnd = 0;
  DenseMap<const GlobalVariable *, uint64_t> Off;
  for (Placed &P : Items) {
    Cursor = alignTo(Cursor, P.Alignment);
    P.Offset = Cursor;
    Off[P.GV] = Cursor;
    Cursor += P.Size;
    if (P.Initialized)
      InitEnd = Cursor;
  }
  uint64_t ImageSize = std::max<uint64_t>(alignTo(Cursor, Align(8)), 8);
  uint64_t BlobBytes = alignTo(InitEnd, Align(8));
  std::vector<uint8_t> Blob(BlobBytes, 0);
  SmallVector<ImageReloc, 8> Relocs;
  for (const Placed &P : Items) {
    if (!P.Initialized)
      continue;
    writeConstant(Blob.data(), P.Offset, P.GV->getInitializer(), DL, Off, Relocs);
  }

  Type *I8 = Type::getInt8Ty(Ctx);
  Type *Ptr = PointerType::get(Ctx, 0);
  FunctionType *BaseTy = FunctionType::get(I64, false);
  Function *BaseFn = M.getFunction("__cbc_image_base");
  if (!BaseFn)
    BaseFn = Function::Create(BaseTy, Function::ExternalLinkage,
                              "__cbc_image_base", &M);

  // IMAGE_ABS offsets travel in reloc blob R. Function and native pointers stay
  // straight-line stores. A method's literal table holds 4095 entries, and each
  // out-of-range displacement plus each ld.fnptr consumes one, so split at 1000.
  constexpr unsigned FnPtrCap = 1000;
  SmallVector<ImageReloc, 8> PtrRelocs;
  std::string RelocBytes;
  for (const ImageReloc &R : Relocs) {
    if (R.Kind == RelocKind::ImageAbs) {
      if (R.Slot > UINT32_MAX)
        report_fatal_error("CBC image relocation offset does not fit in 32 bits");
      uint32_t Off = static_cast<uint32_t>(R.Slot);
      for (int Byte = 0; Byte < 4; ++Byte)
        RelocBytes.push_back(static_cast<char>((Off >> (8 * Byte)) & 0xFF));
      continue;
    }
    PtrRelocs.push_back(R);
  }
  auto emitPointerStores = [&](Function *Fn, ArrayRef<ImageReloc> Rs) {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Fn);
    IRBuilder<> B(BB);
    Value *Base = B.CreateCall(BaseFn, {}, "image");
    Value *ImagePtr = B.CreateIntToPtr(Base, Ptr, "image.p");
    for (const ImageReloc &R : Rs) {
      Value *Slot = B.CreateGEP(I8, ImagePtr, B.getInt64(R.Slot));
      Value *Addr = R.Kind == RelocKind::FnPtr
                        ? static_cast<Value *>(R.Target)
                        : B.CreateCall(
                              NativeAddr.lookup(cast<GlobalVariable>(R.Target)));
      if (R.Addend)
        Addr = B.CreateGEP(I8, Addr, B.getInt64(R.Addend));
      B.CreateStore(Addr, Slot);
    }
    if (Fn->getReturnType()->isVoidTy())
      B.CreateRetVoid();
    else
      B.CreateRet(ConstantInt::get(I64, 0));
  };
  if (!Blob.empty() || !Relocs.empty()) {
    Function *InitFn =
        Function::Create(FunctionType::get(I64, false), Function::InternalLinkage,
                         "__cbc_image_init", &M);
    InitFn->addFnAttr(Attribute::NoInline);
    if (PtrRelocs.size() > FnPtrCap) {
      SmallVector<Function *, 4> Chunks;
      FunctionType *ChunkTy = FunctionType::get(Type::getVoidTy(Ctx), false);
      for (unsigned Start = 0; Start < PtrRelocs.size(); Start += FnPtrCap) {
        unsigned N = std::min<unsigned>(FnPtrCap, PtrRelocs.size() - Start);
        Function *Chunk = Function::Create(
            ChunkTy, Function::InternalLinkage,
            "__cbc_image_init." + std::to_string(Chunks.size() + 1), &M);
        Chunk->addFnAttr(Attribute::NoInline);
        emitPointerStores(Chunk, ArrayRef<ImageReloc>(PtrRelocs).slice(Start, N));
        Chunks.push_back(Chunk);
      }
      BasicBlock *BB = BasicBlock::Create(Ctx, "entry", InitFn);
      IRBuilder<> B(BB);
      for (Function *Chunk : Chunks)
        B.CreateCall(Chunk);
      B.CreateRet(ConstantInt::get(I64, 0));
    } else if (!PtrRelocs.empty()) {
      emitPointerStores(InitFn, PtrRelocs);
    } else {
      BasicBlock *BB = BasicBlock::Create(Ctx, "entry", InitFn);
      IRBuilder<> B(BB);
      B.CreateRet(ConstantInt::get(I64, 0));
    }
  }

  for (const Placed &P : Items)
    P.GV->setInitializer(Constant::getNullValue(P.GV->getValueType()));

  SmallVector<Constant *, 8> Consts;
  for (const Placed &P : Items)
    Consts.push_back(P.GV);

  // Landingpad clauses must stay constants, and the landingpad must stay the
  // first non-PHI. Materializing an image address in front of the pad makes
  // SelectionDAG's findUnwindDestinations spin. CBCLowerEH already recorded
  // the real typeinfo in cbc.lpad.clauses before this pass.
  auto usesPlaced = [&](const Value *V) {
    SmallVector<const Value *, 8> Stack{V};
    DenseSet<const Value *> Seen;
    while (!Stack.empty()) {
      const Value *Cur = Stack.pop_back_val();
      if (!Seen.insert(Cur).second)
        continue;
      if (const auto *GV = dyn_cast<GlobalVariable>(Cur))
        if (Off.contains(GV))
          return true;
      if (const auto *U = dyn_cast<User>(Cur))
        for (const Use &Op : U->operands())
          Stack.push_back(Op.get());
    }
    return false;
  };
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      auto It = BB.getFirstNonPHIIt();
      if (It == BB.end())
        continue;
      auto *LP = dyn_cast<LandingPadInst>(&*It);
      if (!LP)
        continue;
      for (unsigned C = 0, N = LP->getNumClauses(); C < N; ++C) {
        if (!usesPlaced(LP->getClause(C)))
          continue;
        LP->setOperand(C, Constant::getNullValue(LP->getClause(C)->getType()));
      }
    }
  }
  convertUsersOfConstantsToInstructions(Consts);

  DenseMap<Function *, Value *> BasePtr;
  auto basePtrIn = [&](Function &F) -> Value * {
    auto It = BasePtr.find(&F);
    if (It != BasePtr.end())
      return It->second;
    IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
    Value *Base = B.CreateCall(BaseFn, {}, "image");
    Value *ImagePtr = B.CreateIntToPtr(Base, Ptr, "image.p");
    BasePtr[&F] = ImagePtr;
    return ImagePtr;
  };

  for (const Placed &P : Items) {
    SmallVector<User *, 8> Users(P.GV->users());
    for (User *U : Users) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I)
        report_fatal_error("CBC global is used outside a function");
      Value *ImagePtr = basePtrIn(*I->getFunction());
      if (auto *PN = dyn_cast<PHINode>(I)) {
        for (unsigned Op = 0; Op < PN->getNumIncomingValues(); ++Op) {
          if (PN->getIncomingValue(Op) != P.GV)
            continue;
          IRBuilder<> B(PN->getIncomingBlock(Op)->getTerminator());
          Value *Ptr = ImagePtr;
          if (P.Offset)
            Ptr = B.CreateGEP(I8, ImagePtr, B.getInt64(P.Offset));
          PN->setIncomingValue(Op, Ptr);
        }
        continue;
      }
      if (P.Offset) {
        IRBuilder<> B(I);
        ImagePtr = B.CreateGEP(I8, ImagePtr, B.getInt64(P.Offset));
      }
      I->replaceUsesOfWith(P.GV, ImagePtr);
    }
    P.GV->eraseFromParent();
  }

  for (auto &NA : NativeAddr) {
    GlobalVariable *GV = NA.first;
    Function *Helper = NA.second;
    SmallVector<Constant *, 1> One{GV};
    convertUsersOfConstantsToInstructions(One);
    SmallVector<User *, 8> Users(GV->users());
    for (User *U : Users) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I)
        report_fatal_error("CBC native data is used outside a function");
      IRBuilder<> B(I);
      I->replaceUsesOfWith(GV, B.CreateCall(Helper));
    }
    GV->eraseFromParent();
  }

  NamedMDNode *MD = M.getOrInsertNamedMetadata("cbc.image");
  MD->addOperand(MDNode::get(
      Ctx, {ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt32Ty(Ctx), ImageSize)),
            MDString::get(Ctx, StringRef(reinterpret_cast<const char *>(Blob.data()),
                                         Blob.size())),
            MDString::get(Ctx, RelocBytes)}));
  return true;
}

ModulePass *llvm::createCBCLowerGlobalsPass() {
  return new CBCLowerGlobalsLegacy();
}
