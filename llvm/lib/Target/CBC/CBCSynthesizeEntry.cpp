#include "CBC.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include <algorithm>

using namespace llvm;

#define DEBUG_TYPE "cbc-synthesize-entry"
#define PASS_NAME "CBC synthesize entry"

namespace {
class CBCSynthesizeEntryLegacy : public ModulePass {
public:
  static char ID;
  CBCSynthesizeEntryLegacy() : ModulePass(ID) {}
  bool runOnModule(Module &M) override;
};

Function *declare(Module &M, StringRef Name, FunctionType *Ty) {
  if (Function *F = M.getFunction(Name))
    return F;
  return Function::Create(Ty, Function::ExternalLinkage, Name, &M);
}
} // namespace

char CBCSynthesizeEntryLegacy::ID = 0;
INITIALIZE_PASS(CBCSynthesizeEntryLegacy, DEBUG_TYPE, PASS_NAME, false, false)

bool CBCSynthesizeEntryLegacy::runOnModule(Module &M) {
  Function *Host = M.getFunction("__cbc_check_host");
  bool RuntimePresent = Host && !Host->isDeclaration();
  bool WholeProgram = M.getModuleFlag("cbc-whole-program") != nullptr;
  if (!RuntimePresent && !WholeProgram)
    return false;
  Function *Main = M.getFunction("main");
  if (!Main || Main->isDeclaration() || M.getFunction("__cbc_entry"))
    return false;
  Main->setName("__cbc_main");

  LLVMContext &Ctx = M.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Void = Type::getVoidTy(Ctx);
  Function *Check = declare(M, "__cbc_check_host", FunctionType::get(Void, false));
  Function *Fcb = declare(M, "__cbc_fcb", FunctionType::get(I64, false));
  Function *Entry = Function::Create(FunctionType::get(I64, false),
                                     Function::ExternalLinkage, "__cbc_entry", &M);
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Entry);
  IRBuilder<> B(BB);
  B.CreateCall(Fcb);
  B.CreateCall(Check);
  if (Function *Img = M.getFunction("__cbc_image_init"))
    B.CreateCall(Img);
  if (GlobalVariable *Ctors = M.getGlobalVariable("llvm.global_ctors")) {
    if (Ctors->hasInitializer()) {
      if (auto *Arr = dyn_cast<ConstantArray>(Ctors->getInitializer())) {
        struct Item {
          int Pri;
          Function *F;
        };
        SmallVector<Item, 8> Items;
        for (Value *Op : Arr->operands()) {
          auto *CS = dyn_cast<ConstantStruct>(Op);
          if (!CS || CS->getNumOperands() < 2)
            continue;
          auto *PriC = dyn_cast<ConstantInt>(CS->getOperand(0));
          Value *FnV = CS->getOperand(1)->stripPointerCasts();
          auto *Fn = dyn_cast<Function>(FnV);
          if (!PriC || !Fn || Fn->isDeclaration())
            continue;
          Items.push_back({(int)PriC->getSExtValue(), Fn});
        }
        llvm::stable_sort(Items, [](const Item &A, const Item &B) {
          return A.Pri < B.Pri;
        });
        for (const Item &I : Items)
          B.CreateCall(I.F);
      }
    }
  }
  if (GlobalVariable *Dtors = M.getGlobalVariable("llvm.global_dtors"))
    Dtors->eraseFromParent();
  if (GlobalVariable *Ctors = M.getGlobalVariable("llvm.global_ctors"))
    Ctors->eraseFromParent();
  SmallVector<Value *, 3> Args;
  for (Argument &A : Main->args())
    Args.push_back(Constant::getNullValue(A.getType()));
  Value *R = B.CreateCall(Main, Args, Main->getReturnType()->isVoidTy() ? "" : "rc");
  Function *Handlers =
      declare(M, "__cbc_exit_handlers", FunctionType::get(Void, false));
  B.CreateCall(Handlers);
  if (Main->getReturnType()->isVoidTy())
    B.CreateRet(B.getInt64(0));
  else if (Main->getReturnType()->isIntegerTy(32))
    B.CreateRet(B.CreateSExt(R, I64));
  else
    B.CreateRet(R);
  return true;
}

ModulePass *llvm::createCBCSynthesizeEntryPass() {
  return new CBCSynthesizeEntryLegacy();
}
