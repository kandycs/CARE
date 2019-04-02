#include <llvm/IR/InstIterator.h>
#include <llvm/Support/Debug.h>

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <experimental/filesystem>

#include "RKB.h"
#include "tb.h"

#define DEBUG_TYPE "carepass"
STATISTIC(Total, "The # of kernels");
STATISTIC(Empty, "The # of empty kernels");
STATISTIC(Average, "The # of Instructions (avg)");

std::string getFilename(StringRef filename) {
  std::experimental::filesystem::path p = filename.str();
  std::string str = p.filename();
  return str;
}

/**
 * get the human readable name for the value. if value have original
 * name from source code, we will use that one, otherwise a fake one
 * will be generated.
 */
Value *getPointerOperand(Instruction *Insn) {
  if (auto LI = dyn_cast<LoadInst>(Insn)) {
    return LI->getPointerOperand();
  } else if (auto SI = dyn_cast<StoreInst>(Insn)) {
    return SI->getPointerOperand();
  } else {
    llvm_unreachable("Expecting Load/Store Instruction only");
  }
}

std::string getOrCreateValueName(CAREDIBuilder *DIB, Value *V) {
  std::string name;
  /* we are using the value of a variable */
  if (isa<LoadInst>(V)) V = dyn_cast<LoadInst>(V)->getPointerOperand();
  if (isa<StoreInst>(V)) V = dyn_cast<StoreInst>(V)->getPointerOperand();

  if (auto DbgInst = DIB->getDbgInfoIntrinsic(V)) {
    DILocalVariable *Var;
    if (isa<DbgValueInst>(DbgInst))
      Var = dyn_cast<DbgValueInst>(DbgInst)->getVariable();
    else if (isa<DbgAddrIntrinsic>(DbgInst))
      Var = dyn_cast<DbgAddrIntrinsic>(DbgInst)->getVariable();
    else if (isa<DbgDeclareInst>(DbgInst))
      Var = dyn_cast<DbgDeclareInst>(DbgInst)->getVariable();
    name = Var->getName().str();
  } else if (V->hasName()) {
    name = V->getName().str();
    std::replace(name.begin(), name.end(), '.', '_');
  } else {
    raw_string_ostream rso(name);
    V->printAsOperand(rso, false, nullptr);
    name = rso.str();
    name.erase(std::remove(name.begin(), name.end(), '%'), name.end());
    name = "LV" + name;
  }
  return name;
}

RKBuilder::RKBuilder(Instruction *MemAccInst, LivenessAnalysis &LA,
                     CAREDIBuilder *DIB, Module *M)
    : MemAccInst(MemAccInst), LA(LA), DbgInfoBuilder(DIB), CareM(M) {
  DEBUG_WITH_TYPE("RKB", dbgs() << "Retriving calculations for Instruction: "
                                << *MemAccInst << "\n");
  RK = nullptr;
  build();
}

bool RKBuilder::isLive(Value *V) {
  if (isa<Argument>(V) || isa<ConstantData>(V)) return true;
  return LA.isLiveAt(V, MemAccInst);
}

bool RKBuilder::isCallingSimpleKernel(CallInst *CI) {
  if (!CI) return false;
  auto Callee = CI->getCalledFunction();
  if (!Callee) return false;

  // we cannot expand memory allocation function
  if (isMemAlloc(Callee)) return false;

  // expand simple math kernel
  if (isMathKernel(Callee)) return true;

  if (Callee->isDeclaration()) return true;

  Function &F = *(Callee);
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
    auto Call = dyn_cast<CallInst>(&*I);
    if (!Call) continue;
    Callee = Call->getCalledFunction();
    if (!Callee || Callee->isDeclaration() || isMathKernel(Callee)) continue;
    if (isMemAlloc(Callee)) return false;
    if (!isCallingSimpleKernel(Call)) return false;
  }
  return true;
}

bool RKBuilder::isStdlibVariable(Value *V) {
  auto GV = dyn_cast<GlobalVariable>(V);
  if (!GV) return false;
  if (GV->isDeclaration()) return true;
  return false;
}

bool RKBuilder::isTerminalValue(Value *V) {
  if (isa<AllocaInst>(V) || isa<Argument>(V) || isa<PHINode>(V) ||
      isa<GlobalVariable>(V))
    return true;

  // is it load from an alloca/or global value
  if (isa<LoadInst>(V) || isa<StoreInst>(V)) {
    auto Addr = getPointerOperand(dyn_cast<Instruction>(V));
    if (auto CI = dyn_cast<CastInst>(Addr)) Addr = CI->getOperand(0);
    if (isa<AllocaInst>(Addr) || isa<GlobalValue>(Addr)) return true;
  }
  return false;
}

bool RKBuilder::isExpandable(Value *V) {
  if (Expandable.find(V) != Expandable.end()) return Expandable[V];

  Expandable[V] = true;
  if (isTerminalValue(V)) {
    Expandable[V] = false;
  } else {
    auto Insn = dyn_cast<Instruction>(V);
    if (!Insn) {
      Expandable[V] = false;
    } else if (isa<CallInst>(V) &&
               !isCallingSimpleKernel(dyn_cast<CallInst>(V))) {
      Expandable[V] = false;
    } else {
      for (auto i = 0; i < Insn->getNumOperands(); i++) {
        Value *op = Insn->getOperand(i);
        // Constant could be a ConstantData, e.g., COnstantInt, ConstantFP,
        // or a GlobalValue, e.g. Function, GlobalVariable etc.
        if (isa<Constant>(op)) continue;
        if (!isLive(op) && !isExpandable(op)) {
          Expandable[V] = false;
          break;
        }
      }
    }
  }
  return Expandable[V];
}

bool RKBuilder::isMemAlloc(Function *F) {
  auto name = F->getName();
  if (name == "_Znwm" || name == "malloc" || name == "_mm_malloc" ||
      name == "alloc")
    return true;
  return false;
}

bool RKBuilder::isMathKernel(Function *F) {
  auto name = F->getName();
  if (name == "acos" || name == "asin" || name == "atan" || name == "atan2" ||
      name == "cos" || name == "cosh" || name == "sin" || name == "sinh" ||
      name == "tanh" || name == "exp" || name == "frexp" || name == "ldexp" ||
      name == "log" || name == "log10" || name == "modf" || name == "pow" ||
      name == "sqrt" || name == "ceil" || name == "fabs" || name == "floor" ||
      name == "fmod" || name == "abs_min_int" || name == "abs_min_real")
    return true;
  return false;
}

std::string RKBuilder::getKernelName() {
  static int i = 0;
  Function *F = MemAccInst->getParent()->getParent();
  if (F)
    return "care_recover_" + F->getName().str() + "_k" + std::to_string(++i);
  else
    return "care_recover_k" + std::to_string(++i);
}

void RKBuilder::getParams(std::set<Value *> &Params) {
  std::vector<Value *> Workspace;
  Value *Addr = getPointerOperand(MemAccInst);
  Workspace.insert(Workspace.begin(), Addr);
  while (Workspace.size()) {
    Value *V = Workspace.back();
    Workspace.pop_back();

    if (isExpandable(V)) {  // Expandable Value is also an Instruction
      auto I = dyn_cast<Instruction>(V);
      for (unsigned i = 0; i < I->getNumOperands(); i++) {
        Value *Op = I->getOperand(i);
        if (isa<Constant>(Op) && !isa<GlobalVariable>(Op)) continue;
        Workspace.insert(Workspace.begin(), Op);
      }
    } else if (isStdlibVariable(V)) {
      continue;
    } else {
      Params.insert(V);
      if (!isLive(V) && !isa<GlobalValue>(V)) {
        dbgs() << "getParams meet an unlive and unexpandable value: " << *V
               << "\tMemAccInst: " << MemAccInst << "\n";
        llvm_unreachable("Unconsidered Value");
      }
    }
  }
}

void RKBuilder::getStmts(std::set<Value *> Params,
                         std::vector<Value *> &Stmts) {
  std::vector<Value *> Workspace;
  Workspace.clear();
  Value *Addr = getPointerOperand(MemAccInst);
  Workspace.push_back(Addr);
  while (Workspace.size()) {
    Value *V = Workspace.back();
    Workspace.pop_back();

    if (Params.find(V) != Params.end())
      continue;
    else {
      Stmts.push_back(V);
      auto I = dyn_cast<Instruction>(V);
      if (!I) {
        dbgs() << "Meet a non-instruction value in stmts: " << *V << "\n";
        llvm_unreachable("Error");
      }
      for (unsigned i = 0; i < I->getNumOperands(); i++) {
        Value *Op = I->getOperand(i);
        if (isa<Constant>(Op) && !isa<GlobalVariable>(Op) ||
            isStdlibVariable(Op))
          continue;
        Workspace.insert(Workspace.begin(), Op);
      }
    }
  }
  std::reverse(Stmts.begin(), Stmts.end());
}

FunctionType *RKBuilder::getFunctionType(std::set<Value *> Params) {
  std::vector<Type *> ParamTys;
  LLVMContext &Ctx = CareM->getContext();

  for (auto it = Params.begin(); it != Params.end(); it++) {
    Value *V = *it;
    Type *Ty = V->getType();
    ParamTys.push_back(Ty);
  }

  Type *RetTy = Type::getInt8PtrTy(Ctx);

  return FunctionType::get(RetTy, ParamTys, false /* VarArg */);
}

Value *RKBuilder::createInstruction(IRBuilder<> &IRB, Instruction *Insn,
                                    std::vector<Value *> Operands) {
  Value *Inst;
  Function *Callee;
  switch (Insn->getOpcode()) {
    case Instruction::AShr:
      Inst = IRB.CreateAShr(Operands[0], Operands[1]);
      break;
    case Instruction::Add:
      Inst = IRB.CreateAdd(Operands[0], Operands[1]);
      break;
    case Instruction::Sub:
      Inst = IRB.CreateSub(Operands[0], Operands[1]);
      break;
    case Instruction::Mul:
      Inst = IRB.CreateMul(Operands[0], Operands[1]);
      break;
    case Instruction::Shl:
      Inst = IRB.CreateShl(Operands[0], Operands[1]);
      break;
    case Instruction::And:
      Inst = IRB.CreateAnd(Operands[0], Operands[1]);
      break;
    case Instruction::Or:
      Inst = IRB.CreateOr(Operands[0], Operands[1]);
      break;
    case Instruction::Xor:
      Inst = IRB.CreateXor(Operands[0], Operands[1]);
      break;
    case Instruction::GetElementPtr:
      Inst = IRB.CreateGEP(
          Operands[0],
          std::vector<Value *>(Operands.begin() + 1, Operands.end()),
          Insn->getName());
      break;
    case Instruction::Load:
      Inst = IRB.CreateLoad(Operands[0], Insn->getName());
      break;
    case Instruction::BitCast:
      Inst = IRB.CreateBitCast(Operands[0], Insn->getType());
      break;
    case Instruction::Trunc:
      Inst =
          IRB.CreateTrunc(Operands[0], dyn_cast<TruncInst>(Insn)->getDestTy());
      break;
    case Instruction::ZExt:
      Inst = IRB.CreateZExt(Operands[0], dyn_cast<ZExtInst>(Insn)->getDestTy());
      break;
    case Instruction::SExt:
      Inst = IRB.CreateSExt(Operands[0], dyn_cast<SExtInst>(Insn)->getDestTy());
      break;
    case Instruction::SDiv:
      Inst = IRB.CreateSDiv(Operands[0], Operands[1]);
      break;
    case Instruction::ICmp:
      Inst = IRB.CreateICmp(dyn_cast<ICmpInst>(Insn)->getPredicate(),
                            Operands[0], Operands[1]);
      break;
    case Instruction::Select:
      Inst = IRB.CreateSelect(Operands[0], Operands[1], Operands[2]);
      break;
    case Instruction::SRem:
      Inst = IRB.CreateSRem(Operands[0], Operands[1]);
      break;
    case Instruction::ExtractValue:
      Inst = IRB.CreateExtractValue(
          Operands[0], dyn_cast<ExtractValueInst>(Insn)->getIndices(),
          Insn->getName());
      break;
    case Instruction::InsertElement:
      Inst = IRB.CreateInsertElement(Operands[0], Operands[1], Operands[2]);
      break;
    case Instruction::ExtractElement:
      Inst = IRB.CreateExtractElement(Operands[0], Operands[1]);
      break;
    case Instruction::SIToFP:
      Inst = IRB.CreateSIToFP(Operands[0],
                              dyn_cast<SIToFPInst>(Insn)->getDestTy());
      break;
    case Instruction::FDiv:
      Inst = IRB.CreateFDiv(Operands[0], Operands[1]);
      break;
    case Instruction::FMul:
      Inst = IRB.CreateFMul(Operands[0], Operands[1]);
      break;
    case Instruction::FAdd:
      Inst = IRB.CreateFAdd(Operands[0], Operands[1]);
      break;
    case Instruction::FSub:
      Inst = IRB.CreateFSub(Operands[0], Operands[1]);
      break;
    case Instruction::FCmp:
      Inst = IRB.CreateFCmp(dyn_cast<FCmpInst>(Insn)->getPredicate(),
                            Operands[0], Operands[1]);
      break;
    case Instruction::FPToSI:
      Inst = IRB.CreateFPToSI(Operands[0],
                              dyn_cast<FPToSIInst>(Insn)->getDestTy());
      break;
    case Instruction::FPToUI:
      Inst = IRB.CreateFPToUI(Operands[0],
                              dyn_cast<FPToUIInst>(Insn)->getDestTy());
      break;
    case Instruction::Call:
      Callee = dyn_cast<CallInst>(Insn)->getCalledFunction();
      Inst = IRB.CreateCall(CareM->getOrInsertFunction(
                                Callee->getName(), Callee->getFunctionType()),
                            Operands, Insn->getName());
      break;
    case Instruction::PtrToInt:
      Inst = IRB.CreatePtrToInt(Operands[0],
                                dyn_cast<PtrToIntInst>(Insn)->getDestTy());
      break;
    default:
      dbgs() << "Unsupported Instruction: " << *Insn << "\n";
      for (unsigned i = 0; i < Operands.size(); i++) {
        dbgs() << "\n\tOperand[" << i << "]: " << *Operands[i];
      }
      auto Callee = Operands.back();
      dbgs() << "\n\tCallee:" << Callee->getName()
             << ", FuncTy: " << dyn_cast<Function>(Callee) << "\n";

      dbgs() << "\n\tOperands of Original Instruction: " << *Insn << "\n";

      for (unsigned i = 0; i < Insn->getNumOperands(); i++) {
        dbgs() << "\n\tOperand[" << i << "]: " << *Insn->getOperand(i);
      }
      dbgs() << "\n";
      llvm_unreachable("Unsupported Instruction (Above)\n");
  }
  return Inst;
}

Function *RKBuilder::createRecoveryKernel(std::set<Value *> Params,
                                          std::vector<Value *> Stmts) {
  DEBUG_WITH_TYPE("RK", {
    dbgs() << "Create Recovery Kernel Function.\n    Params: ";
    int i = 0;
    for (auto it = Params.begin(); it != Params.end(); it++) {
      dbgs() << "\n\tParam[" << i++ << "]: " << **it;
    }
    dbgs() << "\n";
  });

  // create a new recovery kernel function to the CareM Module
  FunctionType *FuncTy = getFunctionType(Params);

  DEBUG_WITH_TYPE("RK", dbgs() << "Create Function Type: " << *FuncTy << "\n");
  std::string fname = getKernelName();
  Function *RK = dyn_cast<Function>(CareM->getOrInsertFunction(fname, FuncTy));

  // make sure we did the thing correctly, the number of Params is equal to
  // the number of arguments
  assert(Params.size() == RK->arg_size());

  DEBUG_WITH_TYPE("RK", dbgs() << "Init arguments and VMap with Params\n");

  // a map from value in program module to the value in Care Module
  std::map<Value *, Value *> VMap;
  std::set<Value *>::iterator pit;
  llvm::Function::arg_iterator ait;
  for (ait = RK->arg_begin(), pit = Params.begin(); ait != RK->arg_end();
       ait++, pit++) {
    ait->setName(getOrCreateValueName(DbgInfoBuilder, *pit));
    VMap[*pit] = dyn_cast<Value>(ait);
  }

  DEBUG_WITH_TYPE("RK", dbgs() << "Insert/Copying Instructions:\n");
  BasicBlock *BB = BasicBlock::Create(CareM->getContext(), "entry", RK);
  IRBuilder<> IRB(BB);
  for (Value *V : Stmts) {
    // avoid duplications, since vector is used when retrieving
    // computing instructions
    if (VMap.find(V) != VMap.end()) continue;
    DEBUG_WITH_TYPE("RK", dbgs() << "Working on inst :" << *V << "\n");

    auto Insn = dyn_cast<Instruction>(V);
    if (!Insn) {
      DEBUG_WITH_TYPE(
          "RK", dbgs() << "non-instruction value in Stmts: " << *V << "\n");
      llvm_unreachable("Meet a non-instruction Value in Stmts ");
    }

    std::vector<Value *> Operands;

    // get the operands for the instruction. For every
    // instruction, its operands should be created firstly.
    // if not, it means something is wrong
    if (auto CI = dyn_cast<CallInst>(Insn)) {
      for (unsigned i = 0; i < CI->getNumArgOperands(); i++) {
        Value *Op = CI->getArgOperand(i);
        if (isa<Constant>(Op) &&
            (isStdlibVariable(V) || !isa<GlobalVariable>(Op))) {
          Operands.push_back(Op);
        } else if (VMap.find(Op) != VMap.end()) {
          Operands.push_back(VMap[Op]);
        } else {
          dbgs() << "Unprocessed Op: " << *Op << "\n\tInstruction: " << *Insn
                 << "\n";
          llvm_unreachable("The operand has not been processed");
        }
      }
    } else
      for (unsigned i = 0; i < Insn->getNumOperands(); i++) {
        Value *Op = Insn->getOperand(i);
        // if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) {
        if (isa<Constant>(Op) &&
            (isStdlibVariable(V) || !isa<GlobalVariable>(Op))) {
          Operands.push_back(Op);
        } else if (VMap.find(Op) != VMap.end()) {
          Operands.push_back(VMap[Op]);
        } else {
          dbgs() << "Unprocessed Op: " << *Op << "\n\tInstruction: " << *Insn
                 << "\n";
          llvm_unreachable("The operand has not been processed");
        }
      }

    // create the instruction
    Value *Inst = createInstruction(IRB, Insn, Operands);

    VMap[Insn] = Inst;
  }

  Value *T;
  if (Stmts.size())
    T = Stmts.back();
  else if (Params.size() == 1)
    T = *Params.begin();
  else {
    dbgs() << "Stmts: " << Stmts.size() << "\t Params: " << Params.size()
           << "\n";
    llvm_unreachable("Zero stmts and more than 1 params");
  }

  Value *Final =
      IRB.CreateSExtOrBitCast(VMap[T], Type::getInt8PtrTy(CareM->getContext()));
  IRB.CreateRet(Final);

  DEBUG_WITH_TYPE("RK", dbgs() << "Function: \n" << *RK);
  return RK;
}

bool RKBuilder::build() {
  DEBUG_WITH_TYPE("RK", {
    dbgs() << "Build RK for " << *MemAccInst << "\n";
    dbgs() << "\t1. getParams\n";
  });
  Total++;
  getParams(Params);

  DEBUG_WITH_TYPE("RK", { dbgs() << "\t2. getStmts\n"; });

  getStmts(Params, Stmts);

  if (Stmts.size() == 0) Empty++;
  Average += Stmts.size();

  DEBUG_WITH_TYPE("RK", {
    dbgs() << "\t3. Create Kernel\n";
    dbgs() << "\tParams:\n";
    int i = 0;
    for (auto it = Params.begin(); it != Params.end(); it++) {
      dbgs() << "\t\tParam[" << i++ << "]: " << **it
             << " (Live: " << isLive(*it) << ", " << isTerminalValue(*it)
             << ", " << isExpandable(*it) << ")\n";
    }
    dbgs() << "\tStmts:\n";
    for (unsigned i = 0; i < Stmts.size(); i++) {
      dbgs() << "\t\tStmt[" << i << "]: " << *Stmts[i] << "\n";
    }
    dbgs() << "\n\n\n";
  });
  RK = createRecoveryKernel(Params, Stmts);
}

std::string RKBuilder::getRawRecord(DebugLoc loc) {
  // get a copy of human-readable recovery table
  std::string res;
  res += getFilename(loc->getScope()->getFilename()) +
         "\t\tLine: " + std::to_string(loc->getLine()) +
         "\tCol: " + std::to_string(loc->getColumn()) + "\t" +
         RK->getName().str() + "(";
  for (auto it = RK->arg_begin(); it != RK->arg_end(); it++) {
    auto name = it->getName().str();
    res += (name + ", ");
  }
  res[res.size() - 1] = ')';
  return res;
}
