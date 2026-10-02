#include "CBC.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "cbc-lower-sjlj"
#define PASS_NAME "CBC lower setjmp/longjmp"

namespace {
class CBCLowerSjLjLegacy : public ModulePass {
public:
  static char ID;
  CBCLowerSjLjLegacy() : ModulePass(ID) {}
  bool runOnModule(Module &M) override;
};

bool isSetjmpName(StringRef Name) {
  return Name == "setjmp" || Name == "_setjmp" || Name == "sigsetjmp" ||
         Name == "__sigsetjmp";
}

bool isRuntimeHelper(StringRef Name) {
  return Name == "__cbc_setjmp" || Name == "__cbc_sjlj_test" ||
         Name == "__cbc_sjlj_value" || Name == "__cbc_continue_unwinding" ||
         Name == "__cbc_raise" || Name == "__cbc_fcb" ||
         Name == "__cbc_check_host" ||
         Name == "__cbc_image_init" || Name == "__cbc_image_base";
}

Function *declare(Module &M, StringRef Name, FunctionType *Ty) {
  if (Function *F = M.getFunction(Name))
    return F;
  return Function::Create(Ty, Function::ExternalLinkage, Name, &M);
}

bool lowerFunction(Function &F) {
  SmallVector<CallInst *, 4> Sites;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *Callee = CI->getCalledFunction();
      if (Callee && isSetjmpName(Callee->getName()))
        Sites.push_back(CI);
    }
  }
  if (Sites.empty())
    return false;

  for (BasicBlock &BB : F) {
    if (BB.isEHPad())
      report_fatal_error("CBC setjmp and C++ catch in the same function is not supported yet: " +
                         F.getName());
  }

  Module &M = *F.getParent();
  for (Function &SF : M) {
    if (!isSetjmpName(SF.getName()))
      continue;
    for (User *U : SF.users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledFunction() != &SF)
        report_fatal_error("taking the address of 'setjmp' is not supported on CBC");
    }
  }

  LLVMContext &Ctx = M.getContext();
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Ptr = PointerType::getUnqual(Ctx);
  Type *Void = Type::getVoidTy(Ctx);
  Function *SetjmpFn = declare(
      M, "__cbc_setjmp", FunctionType::get(Void, {Ptr, I64, I32, I64}, false));
  // stacksave/stackrestore are overloaded on the pointer type. Declaring them
  // with an empty overload list asserts in DecodeFixedType.
  Function *StackSave =
      Intrinsic::getOrInsertDeclaration(&M, Intrinsic::stacksave, {Ptr});
  Function *StackRestore =
      Intrinsic::getOrInsertDeclaration(&M, Intrinsic::stackrestore, {Ptr});
  Function *TestFn =
      declare(M, "__cbc_sjlj_test", FunctionType::get(I32, {I64}, false));
  Function *ValueFn =
      declare(M, "__cbc_sjlj_value", FunctionType::get(I32, false));
  Function *ContFn = declare(M, "__cbc_continue_unwinding",
                             FunctionType::get(Void, false));
  ContFn->setDoesNotReturn();

  if (!F.hasPersonalityFn()) {
    FunctionType *PTy = FunctionType::get(I32, true);
    Function *Pers = declare(M, "__gcc_personality_v0", PTy);
    F.setPersonalityFn(Pers);
  }

  IRBuilder<> Entry(&*F.getEntryBlock().getFirstNonPHIIt());
  AllocaInst *TokA = Entry.CreateAlloca(Type::getInt8Ty(Ctx), nullptr, "sjlj.tok");
  Value *Tok = Entry.CreatePtrToInt(TokA, I64, "sjlj.tok.i");

  struct Site {
    PHINode *Phi;
    BasicBlock *Cont;
    Value *Buf = nullptr;
    BasicBlock *ValBB = nullptr;
    Value *Val = nullptr;
  };
  SmallVector<Site, 4> Rewritten;
  for (unsigned Index = 0; Index < Sites.size(); ++Index) {
    CallInst *CI = Sites[Index];
    BasicBlock *BB = CI->getParent();
    BasicBlock *Cont = BB->splitBasicBlock(std::next(CI->getIterator()),
                                           "sjlj.after");
    IRBuilder<> B(CI);
    Value *Buf = CI->getArgOperand(0);
    Value *Shadow = B.CreateCall(StackSave, {}, "sjlj.shadow");
    if (Shadow->getType() != I64)
      Shadow = B.CreatePtrToInt(Shadow, I64);
    B.CreateCall(SetjmpFn, {Buf, Tok, B.getInt32(Index), Shadow});
    PHINode *Phi = PHINode::Create(CI->getType(), 2, "sjlj.r", Cont->begin());
    Phi->addIncoming(ConstantInt::get(CI->getType(), 0), BB);
    CI->replaceAllUsesWith(Phi);
    CI->eraseFromParent();
    Rewritten.push_back({Phi, Cont, Buf});
  }

  BasicBlock *LPad = BasicBlock::Create(Ctx, "sjlj.lpad", &F);
  BasicBlock *NotOurs = BasicBlock::Create(Ctx, "sjlj.notours", &F);
  SmallPtrSet<BasicBlock *, 8> Skip;
  Skip.insert(LPad);
  Skip.insert(NotOurs);

  IRBuilder<> LB(LPad);
  LandingPadInst *LP = LB.CreateLandingPad(StructType::get(Ctx, {Ptr, I32}), 0);
  LP->setCleanup(true);
  Value *Which = LB.CreateCall(TestFn, {Tok}, "sjlj.which");
  for (unsigned Index = 0; Index < Rewritten.size(); ++Index) {
    BasicBlock *ValBB = BasicBlock::Create(Ctx, "sjlj.val", &F);
    Skip.insert(ValBB);
    BasicBlock *Next = Index + 1 == Rewritten.size()
                           ? NotOurs
                           : BasicBlock::Create(Ctx, "sjlj.next", &F);
    if (Next != NotOurs)
      Skip.insert(Next);
    LB.CreateCondBr(LB.CreateICmpEQ(Which, ConstantInt::get(I32, Index + 1)),
                    ValBB, Next);
    IRBuilder<> VB(ValBB);
    Value *Slot = VB.CreateConstInBoundsGEP1_64(
        Type::getInt8Ty(Ctx), Rewritten[Index].Buf, 24);
    Value *Saved = VB.CreateLoad(I64, Slot, "sjlj.shadow.reload");
    Value *RestoreArg = Saved;
    if (StackRestore->getFunctionType()->getParamType(0) != I64)
      RestoreArg = VB.CreateIntToPtr(
          Saved, StackRestore->getFunctionType()->getParamType(0));
    VB.CreateCall(StackRestore, {RestoreArg});
    Value *Val = VB.CreateCall(ValueFn, {}, "sjlj.v");
    if (Val->getType() != Rewritten[Index].Phi->getType())
      Val = VB.CreateIntCast(Val, Rewritten[Index].Phi->getType(), true);
    VB.CreateBr(Rewritten[Index].Cont);
    Rewritten[Index].Phi->addIncoming(Val, ValBB);
    Rewritten[Index].ValBB = ValBB;
    Rewritten[Index].Val = Val;
    if (Next != NotOurs)
      LB.SetInsertPoint(Next);
  }
  IRBuilder<> NB(NotOurs);
  NB.CreateCall(ContFn);
  NB.CreateUnreachable();

  SmallVector<CallInst *, 8> ToInvoke;
  for (BasicBlock &BB : F) {
    if (Skip.contains(&BB))
      continue;
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI || CI->isInlineAsm())
        continue;
      Function *Callee = CI->getCalledFunction();
      if (Callee && (Callee->isIntrinsic() || isRuntimeHelper(Callee->getName())))
        continue;
      ToInvoke.push_back(CI);
    }
  }
  for (CallInst *CI : ToInvoke) {
    BasicBlock *BB = CI->getParent();
    BasicBlock *Cont =
        BB->splitBasicBlock(std::next(CI->getIterator()), "sjlj.cont");
    BB->getTerminator()->eraseFromParent();
    SmallVector<Value *, 8> Args(CI->args());
    IRBuilder<> B(CI);
    InvokeInst *II = B.CreateInvoke(CI->getFunctionType(), CI->getCalledOperand(),
                                    Cont, LPad, Args, CI->getName());
    II->setCallingConv(CI->getCallingConv());
    CI->replaceAllUsesWith(II);
    CI->eraseFromParent();
  }

  // Splitting a block retargets successor PHIs. Rebuild setjmp PHIs from the
  // predecessors that exist now so every edge has an incoming value.
  for (Site &S : Rewritten) {
    SmallVector<BasicBlock *, 4> Preds(predecessors(S.Cont));
    while (S.Phi->getNumIncomingValues())
      S.Phi->removeIncomingValue(S.Phi->getNumIncomingValues() - 1, false);
    for (BasicBlock *Pred : Preds) {
      Value *Inc = Pred == S.ValBB ? S.Val : ConstantInt::get(S.Phi->getType(), 0);
      S.Phi->addIncoming(Inc, Pred);
    }
  }

  std::string Err;
  raw_string_ostream OS(Err);
  if (verifyFunction(F, &OS))
    report_fatal_error(Twine("CBC setjmp lowering produced invalid IR: ") + Err);
  return true;
}
} // namespace

char CBCLowerSjLjLegacy::ID = 0;
INITIALIZE_PASS(CBCLowerSjLjLegacy, DEBUG_TYPE, PASS_NAME, false, false)

bool CBCLowerSjLjLegacy::runOnModule(Module &M) {
  bool Changed = false;
  for (Function &F : M) {
    if (!F.isDeclaration())
      Changed |= lowerFunction(F);
  }
  return Changed;
}

ModulePass *llvm::createCBCLowerSjLjPass() {
  return new CBCLowerSjLjLegacy();
}
