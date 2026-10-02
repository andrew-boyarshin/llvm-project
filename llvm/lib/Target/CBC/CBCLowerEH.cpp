#include "CBC.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/ConstantFold.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

using namespace llvm;

#define DEBUG_TYPE "cbc-lower-eh"
#define PASS_NAME "CBC lower C++ exceptions"

namespace {
class CBCLowerEHLegacy : public ModulePass {
public:
  static char ID;
  CBCLowerEHLegacy() : ModulePass(ID) {}
  bool runOnModule(Module &M) override;
};

Function *declare(Module &M, StringRef Name, FunctionType *Ty) {
  if (Function *F = M.getFunction(Name))
    return F;
  return Function::Create(Ty, Function::ExternalLinkage, Name, &M);
}

bool lowerFunction(Function &F) {
  SmallVector<LandingPadInst *, 4> Pads;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *LP = dyn_cast<LandingPadInst>(&I))
        Pads.push_back(LP);
  if (Pads.empty())
    return false;

  Module &M = *F.getParent();
  LLVMContext &Ctx = M.getContext();
  Type *Ptr = PointerType::getUnqual(Ctx);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *Void = Type::getVoidTy(Ctx);

  Function *Landing =
      declare(M, "__cbc_eh_landing", FunctionType::get(Ptr, false));
  Function *Select = declare(
      M, "__cbc_eh_select", FunctionType::get(I32, {Ptr, Ptr}, false));
  Function *ResumeFn =
      declare(M, "__cbc_eh_resume", FunctionType::get(Void, {Ptr}, false));
  ResumeFn->setDoesNotReturn();

  DenseMap<Value *, unsigned> TypeIds;
  unsigned NextId = 1;
  auto idFor = [&](Value *V) -> unsigned {
    auto It = TypeIds.find(V);
    if (It != TypeIds.end())
      return It->second;
    unsigned Id = NextId++;
    TypeIds[V] = Id;
    return Id;
  };

  for (LandingPadInst *LP : Pads) {
    for (unsigned C = 0, N = LP->getNumClauses(); C < N; ++C) {
      if (!LP->isCatch(C))
        continue;
      Value *Clause = LP->getClause(C);
      if (isa<ConstantPointerNull>(Clause))
        continue;
      idFor(Clause);
    }
  }

  SmallVector<CallInst *, 4> TypeIdCalls;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->isIntrinsic() &&
          Callee->getIntrinsicID() == Intrinsic::eh_typeid_for)
        TypeIdCalls.push_back(CI);
    }
  }
  for (CallInst *CI : TypeIdCalls) {
    Value *Arg = CI->getArgOperand(0);
    unsigned Id = TypeIds.lookup(Arg);
    if (!Id)
      Id = idFor(Arg);
    CI->replaceAllUsesWith(ConstantInt::get(I32, Id));
    CI->eraseFromParent();
  }

  StructType *ClauseTy =
      StructType::get(Ctx, {I32, I32, Ptr});
  DenseMap<LandingPadInst *, Value *> ExnForPad;
  DenseMap<LandingPadInst *, Value *> SelForPad;

  for (LandingPadInst *LP : Pads) {
    SmallVector<Constant *, 8> Rows;
    unsigned CatchAllId = 0;
    for (unsigned C = 0, N = LP->getNumClauses(); C < N; ++C) {
      if (LP->isFilter(C)) {
        Constant *Filter = LP->getClause(C);
        if (!Filter->getType()->isPointerTy()) {
          Filter = ConstantFoldGetElementPtr(
              Filter->getType(), Filter, std::nullopt,
              ArrayRef<Value *>{ConstantInt::get(I32, 0),
                                ConstantInt::get(I32, 0)});
        }
        Rows.push_back(ConstantStruct::get(
            ClauseTy, {ConstantInt::get(I32, 1), ConstantInt::get(I32, -1),
                       Filter}));
        continue;
      }
      Value *Clause = LP->getClause(C);
      unsigned Kind;
      unsigned Id;
      Constant *TInfo;
      if (isa<ConstantPointerNull>(Clause)) {
        Kind = 2;
        if (!CatchAllId)
          CatchAllId = NextId++;
        Id = CatchAllId;
        TInfo = ConstantPointerNull::get(cast<PointerType>(Ptr));
      } else {
        Kind = 0;
        Id = TypeIds.lookup(Clause);
        TInfo = cast<Constant>(Clause);
      }
      Rows.push_back(ConstantStruct::get(
          ClauseTy, {ConstantInt::get(I32, Kind), ConstantInt::get(I32, Id),
                     TInfo}));
    }
    unsigned Count = Rows.size();
    if (Rows.empty()) {
      Rows.push_back(ConstantStruct::get(
          ClauseTy,
          {ConstantInt::get(I32, 0), ConstantInt::get(I32, 0),
           ConstantPointerNull::get(cast<PointerType>(Ptr))}));
    }
    ArrayType *ArrTy = ArrayType::get(ClauseTy, Rows.size());
    StructType *TableTy = StructType::get(Ctx, {I32, I32, ArrTy});
    Constant *Table = ConstantStruct::get(
        TableTy,
        {ConstantInt::get(I32, Count),
         ConstantInt::get(I32, LP->isCleanup() ? 1 : 0),
         ConstantArray::get(ArrTy, Rows)});
    GlobalVariable *GV = new GlobalVariable(
        M, TableTy, true, GlobalValue::PrivateLinkage, Table, "cbc.lpad.clauses");

    IRBuilder<> B(LP->getNextNode());
    Value *Exn = B.CreateCall(Landing, {}, "cbc.exn");
    Value *Sel = B.CreateCall(Select, {Exn, GV}, "cbc.sel");
    ExnForPad[LP] = Exn;
    SelForPad[LP] = Sel;

    SmallVector<ExtractValueInst *, 4> Extracts;
    for (User *U : LP->users())
      if (auto *EV = dyn_cast<ExtractValueInst>(U))
        Extracts.push_back(EV);
    for (ExtractValueInst *EV : Extracts) {
      unsigned Idx = EV->getIndices()[0];
      EV->replaceAllUsesWith(Idx == 0 ? Exn : Sel);
      EV->eraseFromParent();
    }
  }

  // A catch dispatcher often phis the whole landingpad aggregate. Those
  // extracts still read the personality registers, which this lowering does
  // not fill. Split the phi into the values __cbc_eh_select actually returned.
  SmallVector<PHINode *, 4> PadPhis;
  SmallPtrSet<PHINode *, 4> SeenPhi;
  for (LandingPadInst *LP : Pads)
    for (User *U : LP->users())
      if (auto *PN = dyn_cast<PHINode>(U))
        if (SeenPhi.insert(PN).second)
          PadPhis.push_back(PN);
  for (PHINode *PN : PadPhis) {
    if (!PN->getType()->isStructTy() || PN->getType()->getStructNumElements() < 2)
      continue;
    IRBuilder<> B(PN);
    auto *ExnPhi = B.CreatePHI(Ptr, PN->getNumIncomingValues(), "cbc.exn.phi");
    auto *SelPhi = B.CreatePHI(I32, PN->getNumIncomingValues(), "cbc.sel.phi");
    for (unsigned I = 0, N = PN->getNumIncomingValues(); I != N; ++I) {
      BasicBlock *IB = PN->getIncomingBlock(I);
      Value *In = PN->getIncomingValue(I);
      Value *ExnIn = nullptr;
      Value *SelIn = nullptr;
      if (auto *LP = dyn_cast<LandingPadInst>(In)) {
        ExnIn = ExnForPad.lookup(LP);
        SelIn = SelForPad.lookup(LP);
      }
      if (!ExnIn || !SelIn) {
        ExnIn = Constant::getNullValue(Ptr);
        SelIn = ConstantInt::get(I32, 0);
      }
      ExnPhi->addIncoming(ExnIn, IB);
      SelPhi->addIncoming(SelIn, IB);
    }
    SmallVector<ExtractValueInst *, 4> Extracts;
    for (User *U : PN->users())
      if (auto *EV = dyn_cast<ExtractValueInst>(U))
        Extracts.push_back(EV);
    for (ExtractValueInst *EV : Extracts) {
      unsigned Idx = EV->getIndices()[0];
      EV->replaceAllUsesWith(Idx == 0 ? static_cast<Value *>(ExnPhi)
                                      : static_cast<Value *>(SelPhi));
      EV->eraseFromParent();
    }
    if (!PN->use_empty()) {
      // Insertvalue is not a PHI. Building it with IRBuilder(PN) places it in
      // the PHI section and leaves later PHIs for SelectionDAG to visit
      // (llvm_unreachable in visitPHI).
      BasicBlock *BB = PN->getParent();
      IRBuilder<> AfterPhis(BB, BB->getFirstInsertionPt());
      Value *Agg = UndefValue::get(PN->getType());
      Agg = AfterPhis.CreateInsertValue(Agg, ExnPhi, 0);
      Agg = AfterPhis.CreateInsertValue(Agg, SelPhi, 1);
      PN->replaceAllUsesWith(Agg);
    }
    PN->eraseFromParent();
  }

  SmallVector<ResumeInst *, 4> Resumes;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *RI = dyn_cast<ResumeInst>(&I))
        Resumes.push_back(RI);
  for (ResumeInst *RI : Resumes) {
    Value *Agg = RI->getValue();
    Value *Exn = nullptr;
    if (auto *LP = dyn_cast<LandingPadInst>(Agg))
      Exn = ExnForPad.lookup(LP);
    IRBuilder<> B(RI);
    if (!Exn)
      Exn = B.CreateExtractValue(Agg, 0, "cbc.resume.exn");
    B.CreateCall(ResumeFn, {Exn});
    B.CreateUnreachable();
    RI->eraseFromParent();
  }
  return true;
}
} // namespace

char CBCLowerEHLegacy::ID = 0;
INITIALIZE_PASS(CBCLowerEHLegacy, DEBUG_TYPE, PASS_NAME, false, false)

bool CBCLowerEHLegacy::runOnModule(Module &M) {
  bool Changed = false;
  for (Function &F : M)
    if (!F.isDeclaration())
      Changed |= lowerFunction(F);
  return Changed;
}

ModulePass *llvm::createCBCLowerEHPass() { return new CBCLowerEHLegacy(); }
