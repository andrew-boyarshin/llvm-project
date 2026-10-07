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

void callSortedCtors(IRBuilder<> &B, Module &M) {
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
}

void eraseCtorDtorLists(Module &M) {
  if (GlobalVariable *Dtors = M.getGlobalVariable("llvm.global_dtors"))
    Dtors->eraseFromParent();
  if (GlobalVariable *Ctors = M.getGlobalVariable("llvm.global_ctors"))
    Ctors->eraseFromParent();
}

/// Build argc/argv via __cbc_args for the accepted main shapes.
void appendMainArgs(IRBuilder<> &B, Module &M, Function *Main,
                    SmallVectorImpl<Value *> &Args) {
  unsigned N = Main->arg_size();
  if (N == 0)
    return;
  LLVMContext &Ctx = M.getContext();
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *Ptr = PointerType::getUnqual(Ctx);
  Function *ArgsFn =
      declare(M, "__cbc_args", FunctionType::get(I32, {Ptr}, false));
  AllocaInst *Slot = B.CreateAlloca(Ptr, nullptr, "argv.slot");
  Value *Argc = B.CreateCall(ArgsFn, {Slot}, "argc");
  Value *Argv = B.CreateLoad(Ptr, Slot, "argv");
  Args.push_back(Argc);
  if (N >= 2)
    Args.push_back(Argv);
  if (N >= 3) {
    for (unsigned I = 2; I < N; ++I)
      Args.push_back(Constant::getNullValue(Main->getArg(I)->getType()));
  }
}

void finishEntryReturn(IRBuilder<> &B, Function *Main, Value *R, Type *I64) {
  if (Main->getReturnType()->isVoidTy())
    B.CreateRet(B.getInt64(0));
  else if (Main->getReturnType()->isIntegerTy(32))
    B.CreateRet(B.CreateSExt(R, I64));
  else
    B.CreateRet(R);
}

Function *createLibStart(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Void = Type::getVoidTy(Ctx);
  Function *Check =
      declare(M, "__cbc_check_host", FunctionType::get(Void, false));
  Function *Fcb = declare(M, "__cbc_fcb", FunctionType::get(I64, false));
  Function *LibStart = Function::Create(FunctionType::get(I64, false),
                                        Function::ExternalLinkage,
                                        "__cbc_lib_start", &M);
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", LibStart);
  IRBuilder<> B(BB);
  B.CreateCall(Fcb);
  B.CreateCall(Check);
  if (Function *Img = M.getFunction("__cbc_image_init"))
    B.CreateCall(Img);
  callSortedCtors(B, M);
  eraseCtorDtorLists(M);
  B.CreateRet(B.getInt64(0));
  return LibStart;
}

Function *createFullEntry(Module &M, Function *Main) {
  LLVMContext &Ctx = M.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Void = Type::getVoidTy(Ctx);
  Function *Check =
      declare(M, "__cbc_check_host", FunctionType::get(Void, false));
  Function *Fcb = declare(M, "__cbc_fcb", FunctionType::get(I64, false));
  Function *Entry = Function::Create(FunctionType::get(I64, false),
                                     Function::ExternalLinkage, "__cbc_entry",
                                     &M);
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Entry);
  IRBuilder<> B(BB);
  B.CreateCall(Fcb);
  B.CreateCall(Check);
  if (Function *Img = M.getFunction("__cbc_image_init"))
    B.CreateCall(Img);
  callSortedCtors(B, M);
  eraseCtorDtorLists(M);
  SmallVector<Value *, 3> Args;
  appendMainArgs(B, M, Main, Args);
  Value *R = B.CreateCall(Main, Args,
                          Main->getReturnType()->isVoidTy() ? "" : "rc");
  Function *Handlers =
      declare(M, "__cbc_exit_handlers", FunctionType::get(Void, false));
  B.CreateCall(Handlers);
  finishEntryReturn(B, Main, R, I64);
  return Entry;
}

Function *createWrapEntry(Module &M, Function *Main) {
  LLVMContext &Ctx = M.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Void = Type::getVoidTy(Ctx);
  Function *Entry = Function::Create(FunctionType::get(I64, false),
                                     Function::ExternalLinkage, "__cbc_entry",
                                     &M);
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Entry);
  IRBuilder<> B(BB);
  SmallVector<Value *, 3> Args;
  appendMainArgs(B, M, Main, Args);
  Value *R = B.CreateCall(Main, Args,
                          Main->getReturnType()->isVoidTy() ? "" : "rc");
  Function *Handlers =
      declare(M, "__cbc_exit_handlers", FunctionType::get(Void, false));
  B.CreateCall(Handlers);
  finishEntryReturn(B, Main, R, I64);
  return Entry;
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

  bool Wrap = M.getModuleFlag("cbc-wrap") != nullptr;
  if (M.getFunction("__cbc_entry") || M.getFunction("__cbc_lib_start"))
    return false;

  Function *Main = M.getFunction("main");
  bool HasMain = Main && !Main->isDeclaration();

  if (!Wrap) {
    if (!HasMain)
      return false;
    Main->setName("__cbc_main");
    createFullEntry(M, Main);
    return true;
  }

  // Wrap: always emit __cbc_lib_start (probes, image init, ctors).
  createLibStart(M);
  if (!HasMain)
    return true;

  // Wrap + main: slim Entry.main — args, main, exit handlers only.
  Main->setName("__cbc_main");
  createWrapEntry(M, Main);
  return true;
}

ModulePass *llvm::createCBCSynthesizeEntryPass() {
  return new CBCSynthesizeEntryLegacy();
}
