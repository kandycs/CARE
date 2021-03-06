#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>

#include <experimental/filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>

#include "CarePass.h"

#include "tb.h"

using namespace llvm;

bool CarePass::isLoadFromAlloca(Value *V) {
  auto LI = dyn_cast<LoadInst>(V);
  if (!LI) return false;
  auto addr = LI->getPointerOperand();
  if (isa<AllocaInst>(addr)) return true;
  return false;
}

bool CarePass::isStoreToAlloca(Value *V) {
  auto SI = dyn_cast<StoreInst>(V);
  if (!SI) return false;
  auto addr = SI->getPointerOperand();
  if (isa<AllocaInst>(addr)) return true;
  return false;
}

bool CarePass::isMemAccInst(Instruction *Insn) {
  if (!isa<StoreInst>(Insn) && !isa<LoadInst>(Insn)) return false;
  auto Addr = getPointerOperand(Insn);
  if (isa<LoadInst>(Addr)) return true;
  if (isa<CastInst>(Addr)) Addr = dyn_cast<CastInst>(Addr)->getOperand(0);
  if (isa<CallInst>(Addr) && isCallingSimpleKernel(dyn_cast<CallInst>(Addr)))
    return true;
  else if (!isa<GetElementPtrInst>(Addr))
    return false;
  return true;
}

bool CarePass::isMemAlloc(CallInst *CI) {
  auto Callee = CI->getCalledFunction();
  if (!Callee) return false;
  auto fname = Callee->getName();
  if (fname == "_Znwm" || fname == "malloc" || fname == "_mm_malloc" ||
      fname == "alloc")
    return true;
}

bool CarePass::isCallingSimpleKernel(CallInst *CI) {
  if (isMemAlloc(CI)) return false;
  if (isMath(CI)) return true;
  auto Callee = CI->getCalledFunction();
  if (!Callee) return false;
  if (Callee->isDeclaration()) return true;
  Function &F = *(Callee);
  auto &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
  if (!LI.empty()) return false;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
    auto CI = dyn_cast<CallInst>(&*I);
    if (!CI) continue;
    if (!isCallingSimpleKernel(CI)) return false;
  }
  return true;
}

bool CarePass::isMath(CallInst *CI) {
  auto Callee = CI->getCalledFunction();
  if (!Callee) {
    dbgs() << "Callee is not a function\n";
    return false;
  }
  auto func = Callee->getName();

  // GTC-P math kernels
  if (func == "abs_min_int" || func == "abs_min_real") return true;

  if (func == "acos" || func == "asin" || func == "atan" || func == "atan2" ||
      func == "cos" || func == "cosh" || func == "sin" || func == "sinh" ||
      func == "tanh" || func == "exp" || func == "frexp" || func == "ldexp" ||
      func == "log" || func == "log10" || func == "modf" || func == "pow" ||
      func == "sqrt" || func == "ceil" || func == "fabs" || func == "floor" ||
      func == "fmod")
    return true;
  return false;
}

Value *CarePass::getPointerOperand(Instruction *Insn) {
  if (auto LI = dyn_cast<LoadInst>(Insn)) {
    return LI->getPointerOperand();
  } else if (auto SI = dyn_cast<StoreInst>(Insn)) {
    return SI->getPointerOperand();
  } else {
    llvm_unreachable("Expecting Load/Store Instruction only");
  }
}

/**
 * initialize(). * simiple initialization routine.
 * It will create a DIBuilder for the original module
 * which is used to create dbg data. It will also create
 * a new module for care which is to store recovery routines
 */
void CarePass::initialize(Module &M) {
  // if the module has no debug data (by checking whether the
  // module dwarf version information), a DIBuilder will be created.
  hasDebugInfo = M.getDwarfVersion() ? true : false;
  if (hasDebugInfo) {
    resolveConflictDbgInfo(M);
    getDbgInfo(M);
  } else
    DbgInfoBuilder = new CAREDIBuilder(M);
  CareM = new Module("Care", M.getContext());
  CareM->setTargetTriple(M.getTargetTriple());
  CareM->setDataLayout(M.getDataLayout());
}

std::string CarePass::getOrCreateValueName(Value *V) {
  std::string name;
  if (isLoadFromAlloca(V)) V = dyn_cast<LoadInst>(V)->getPointerOperand();
  if (isStoreToAlloca(V)) V = dyn_cast<StoreInst>(V)->getPointerOperand();
  if (DbgValueMap.find(V) != DbgValueMap.end()) {
    auto DbgInst = DbgValueMap[V];
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

std::string CarePass::getFilename(StringRef filename) {
  std::experimental::filesystem::path p = filename.str();
  std::string str = p.filename();
  return str;
}

std::string CarePass::getKey(DebugLoc loc) {
  MHASH mhd = mhash_init(MHASH_MD5);
  assert(mhd != MHASH_FAILED);

  int lineno = loc.getLine();
  int colno = loc.getCol();
  auto scope = dyn_cast<DIScope>(loc.getScope());
  std::string str = getFilename(scope->getFilename());
  str += "/" + std::to_string(lineno) + "/" + std::to_string(colno);

  unsigned char buf[16];
  mhash(mhd, str.c_str(), str.size());
  mhash_deinit(mhd, buf);
  char key[32];
  for (int i = 0; i < 16; i++) sprintf(&key[2 * i], "%02x", buf[i]);

  std::string result(key);

  DEBUG_WITH_TYPE("RTB", {
    dbgs() << "getKey for File: " << getFilename(scope->getFilename())
           << ", Line: " << lineno << ", Col: " << colno
           << "\n\tMD5 CStr: " << result << "\n\tMD5 Char: ";
    for (int i = 0; i < 32; i++) fprintf(stdout, "%c", key[i]);
    dbgs() << "\n";
  });

  return result;
}

void CarePass::resolveConflictDbgInfo(Module &M) {
  std::map<DebugLoc, std::vector<Instruction *>> DbgLocMap;
  Module::FunctionListType &funcs = M.getFunctionList();
  for (Module::FunctionListType::iterator it = funcs.begin(), end = funcs.end();
       it != end; it++) {
    Function &F = *it;
    if (F.isDeclaration() || F.isIntrinsic()) continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
      Instruction *Insn = &*I;
      if (!isa<LoadInst>(Insn) && !isa<StoreInst>(Insn)) continue;
      // if (!isMemAccInst(Insn)) continue;
      DebugLoc DbgLoc = Insn->getDebugLoc();
      if (!DbgLoc) continue;
      if (DbgLocMap.find(DbgLoc) == DbgLocMap.end())
        DbgLocMap[DbgLoc] = std::vector<Instruction *>();
      DbgLocMap[DbgLoc].push_back(Insn);
    }
  }

  for (auto mit = DbgLocMap.begin(); mit != DbgLocMap.end(); mit++) {
    if (mit->second.size() == 1) continue;
    // dbgs() << "DbgLoc: " << *(mit->first) << "(line: " <<
    // mit->first->getLine()
    //<< ", Col:" << mit->first->getColumn() << ")\n";
    for (int i = 1; i < mit->second.size(); i++) {
      // dbgs() << "\t" << *(mit->second[i]) << "\n";
      unsigned line = mit->first->getLine();
      unsigned col = mit->first->getColumn() + i + 100;
      DebugLoc loc = DebugLoc::get(line, col, mit->first->getScope(),
                                   mit->first->getInlinedAt());

      mit->second[i]->setDebugLoc(loc);
    }
    // dbgs() << "\n";
  }
}

void CarePass::getDbgInfo(Module &M) {
  Module::FunctionListType &funcs = M.getFunctionList();
  for (Module::FunctionListType::iterator it = funcs.begin(), end = funcs.end();
       it != end; it++) {
    Function &F = *it;

    if (F.isDeclaration() || F.isIntrinsic()) continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
      if (auto DbgInst = dyn_cast<DbgInfoIntrinsic>(&*I)) {
        DEBUG_WITH_TYPE("DBG", {
          llvm::dbgs() << "Instruction: " << *DbgInst << "\t Ops: ";
          for (unsigned i = 0; i < DbgInst->getNumArgOperands(); i++)
            llvm::dbgs() << "\n\t" << *(DbgInst->getArgOperand(i));
          llvm::dbgs() << "\n\n";
        });

        if (!isa<DbgDeclareInst>(&*I) && !isa<DbgValueInst>(&*I) &&
            !isa<DbgAddrIntrinsic>(&*I)) {
          dbgs() << "Unprocessed DbgInfoIntrinsic: " << *I << "\n";
          llvm_unreachable("Unprocessed DbgInfoIntrinsic\n");
        }

        Value *V = DbgInst->getVariableLocation();
        if (V) {  // in some casres
          DbgValueMap[V] = DbgInst;
          // if the value is simply a cast, the original operand refers to the
          // same object
          if (auto Cast = dyn_cast<BitCastInst>(V))
            DbgValueMap[Cast->getOperand(0)] = DbgInst;
        }
      }
    }
  }

  DEBUG_WITH_TYPE("DBG", {
    llvm::dbgs() << "Debug Local Variables: ";
    for (auto item : DbgValueMap)
      llvm::dbgs() << "\n\tValue : " << *item.first << "(" << item.first << ")"
                   << "\tdbg.value: " << *item.second;
    llvm::dbgs() << "\n\n";
  });
}

DebugLoc CarePass::getNearbyDebugLoc(Instruction *Insn) {
  DebugLoc Prev, Next;
  Instruction *P = Insn;
  while (P = P->getPrevNode()) {
    // dbgs() << "Prev: " << *P << "\n";
    if (isMemAccInst(P)) break;
    Prev = P->getDebugLoc();
    if (Prev) break;
  }

  Instruction *N = Insn;
  while (N = N->getNextNode()) {
    // dbgs() << "Next: " << *N << "\n";
    if (isMemAccInst(N)) break;
    Next = N->getDebugLoc();
    if (Next) break;
  }

  if (Prev && Next) {
    // dbgs() << "Prev DebugLoc: " << *Prev << "\n";
    // dbgs() << "Next DebugLoc: " << *Next << "\n";
    int line = (Prev->getLine() + Next->getLine()) / 2;
    int col = (Prev->getColumn() + Next->getColumn()) / 2;
    return DebugLoc::get(line, col, Prev->getScope());
  } else if (Prev) {
    // dbgs() << "Prev DebugLoc: " << *Prev << "\n";
    return Prev;
  } else if (Next) {
    // dbgs() << "Next DebugLoc: " << *Next << "\n";
    return Next;
  }
  return nullptr;
}

bool CarePass::runOnModule(Module &M) {
  initialize(M);

  // the recovery table holding the recovery kernel for MemAccInst
  pb::Table *rtb = care_tb_create();

  Module::FunctionListType &funcs = M.getFunctionList();
  for (Module::FunctionListType::iterator it = funcs.begin(), end = funcs.end();
       it != end; it++) {
    Function &F = *it;

    if (F.isDeclaration() || F.isIntrinsic()) continue;

    // if (F.getName() != "_ZNKSt4lessIiEclERKiS2_") continue;

    dbgs() << "Working on Function: " << F.getName() << "!\n";

    DISubprogram *DIFunc = nullptr;
    std::set<Value *> Variables;
    if (!hasDebugInfo) DIFunc = DbgInfoBuilder->createDIFunction(F);

    curr = &F;

    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
      if (!isMemAccInst(&*I)) continue;

      // dbgs() << "Working on Instruction: " << *I << "\n";

      Function *kernel;
      std::set<Value *> params;
      std::tie(kernel, params) = buildRecoveryKernel(&*I);

      if (!kernel) continue;

      DEBUG_WITH_TYPE("RK", dbgs() << "created a recovery kernel: "
                                   << "\n\tname: " << kernel->getName()
                                   << "\n\tFunctionType: "
                                   << *kernel->getFunctionType() << "\n");

      Variables.insert(params.begin(), params.end());
      // get the debug info for the memory access instruction.
      // if it does not exist, create one
      DebugLoc loc;
      if (hasDebugInfo) {  // if debug flag is enabled
        loc = I->getDebugLoc();
        // Debug data could be missed for some instruction even when compiled
        // with -g flag. Especially after some code optimizations.
        if (!loc) {
          DEBUG_WITH_TYPE(
              "DBGLOC", dbgs() << "\n\nNo Debug Data for: " << *I
                               << "\n\tSearching for its nearby instructions.");
          loc = getNearbyDebugLoc(&*I);
          if (!loc) {
            DIFunc = I->getFunction()->getSubprogram();
            static int Fline = 0;
            static int Fcol = 0;
            loc = DebugLoc::get(++Fline, ++Fcol, DIFunc);
            DEBUG_WITH_TYPE("DBGLOC",
                            dbgs() << "\n\nNo Debug Data for: " << *I
                                   << "\n\tand so for its nearby instructions."
                                   << "\n\tCreated a fake one: " << *loc
                                   << "\n\tFunction Type: " << *DIFunc);
          }
          DEBUG_WITH_TYPE("DBGLOC", dbgs() << "\n\n");
          I->setDebugLoc(loc);
        }
      } else  // if debug flag is not enabled
        loc = DbgInfoBuilder->setDIDebugLoc(&*I, DIFunc);

      // build the recovery kernel table
      std::string key = getKey(loc);
      std::vector<std::string> pnames;
      for (auto it = params.begin(); it != params.end(); it++)
        pnames.push_back(getOrCreateValueName(*it));

      DEBUG_WITH_TYPE("RTB", {
        dbgs() << "Create RTB Entry: "
               << "\n\tKey: " << key << "\n\tKernel: " << kernel->getName()
               << "\n\tParams: ";
        for (unsigned i = 0; i < pnames.size(); i++) dbgs() << pnames[i] << " ";
      });
      care_tb_add_record(rtb, key, kernel, pnames);

      rktable += getFilename(loc->getScope()->getFilename()) +
                 "\t\tLine: " + std::to_string(loc->getLine()) +
                 "\tCol: " + std::to_string(loc->getColumn()) + "\t" +
                 kernel->getName().str() + "(";
      for (auto i = 0; i < pnames.size() - 1; i++)
        rktable += (pnames[i] + ", ");
      rktable += (pnames[pnames.size() - 1] + ")\n");

      // promote the debug data to all of its binary users
      // since in x86 architecture, the binary operation could
      // be merged with a memort access instruction
      for (auto U : I->users()) {
        if (auto Insn = dyn_cast<BinaryOperator>(U)) Insn->setDebugLoc(loc);
        if (auto Insn = dyn_cast<CastInst>(U)) Insn->setDebugLoc(loc);
        if (auto Insn = dyn_cast<CmpInst>(U)) Insn->setDebugLoc(loc);
      }
    }

    // Create DILocalVariable for referenced values
    if (!hasDebugInfo) {
      for (auto it = Variables.begin(); it != Variables.end(); it++) {
        std::string VName = getOrCreateValueName(*it);
        DbgInfoBuilder->createDIVariable(*it, VName, DIFunc);
      }
    }
  }

  if (!hasDebugInfo) DbgInfoBuilder->finalize();

  // save CareM to .bc file
  dbgs() << "Module Name: " << M.getName() << "\n";
#if 0
  std::string program = M.getName().ltrim("./").split('.').first.str();
  std::string FileName = "lib" + program + "_care.bc";
#else
  std::experimental::filesystem::path p =
      M.getName().split('/').second.split('.').first.str();
  std::string program = p.filename();
  std::string FileName = "lib" + program + "_care.bc";
#endif
  std::error_code EC;

  dbgs() << "Writing CareM module to " << FileName << "\n";
  llvm::raw_fd_ostream OS(FileName, EC, llvm::sys::fs::F_None);
  WriteBitcodeToFile(CareM, OS);
  OS.flush();

  // save recovery table
  FileName = program + "_care.tb";
  care_tb_save(FileName, rtb);

  FileName = program + "_care_raw.tb";
  std::ofstream rkraw;
  rkraw.open(FileName);
  rkraw << rktable;
  rkraw.close();

  DEBUG_WITH_TYPE("RTB", {
    dbgs() << "Recovery Table (" << FileName << "): \n";
    care_tb_print(rtb);
  });

  care_tb_release(rtb);
  return true;
}

std::pair<Function *, std::set<Value *>> CarePass::buildRecoveryKernel(
    Instruction *Insn) {
  std::vector<Value *> Stmts;
  std::set<Value *> Params;
  DEBUG_WITH_TYPE(
      "RK", dbgs() << "\n\n\nBuild recovery kernel for: " << *Insn << "\n");

  Type *RetTy = getParamsAndStmts(Insn, Params, Stmts);
  Function *RK = createFunction(RetTy, Params, Stmts);
  return std::make_pair(RK, Params);
}

Type *CarePass::getParamsAndStmts(Instruction *I, std::set<Value *> &Params,
                                  std::vector<Value *> &Stmts) {
  std::vector<Value *> Workspace;

  DEBUG_WITH_TYPE("info", dbgs() << "Retriving calculations for Instruction: "
                                 << *I << "\n");

  Value *Addr = getPointerOperand(I);

  Workspace.insert(Workspace.begin(), Addr);
  while (Workspace.size()) {
    Value *V = Workspace.back();
    Workspace.pop_back();

    if (isa<Argument>(V) || isa<PHINode>(V) || isa<GlobalValue>(V) ||
        isa<AllocaInst>(V)) {
      Params.insert(V);
    } else if (isLoadFromAlloca(V) || isStoreToAlloca(V)) {
      Params.insert(V);
    } else if (auto CI = dyn_cast<CallInst>(V)) {
      if (!isCallingSimpleKernel(CI)) {
        Params.insert(V);
      } else {
        Stmts.push_back(V);

        for (unsigned i = 0; i < CI->getNumArgOperands(); i++) {
          Value *Op = CI->getArgOperand(i);
          if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) continue;
          // dbgs() << "Put " << *Op << "into stmts.\n";
          Workspace.insert(Workspace.begin(), Op);
        }
      }
    } else if (auto I = dyn_cast<Instruction>(V)) {
      Stmts.push_back(V);
      DEBUG_WITH_TYPE("Inst", dbgs() << "\n\nWorking on: " << *V);
      for (unsigned i = 0; i < I->getNumOperands(); i++) {
        Value *Op = I->getOperand(i);
        if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) continue;
        Workspace.insert(Workspace.begin(), Op);
        DEBUG_WITH_TYPE("Inst", dbgs() << "\n\tAppend: " << *Op);
      }
      DEBUG_WITH_TYPE("Inst", dbgs() << "\n");
    } else {
      dbgs() << "Meet an Unknown Value: " << *V << ": \n";
      llvm_unreachable("Unconsidered Value");
    }
  }
  std::reverse(Stmts.begin(), Stmts.end());

  DEBUG_WITH_TYPE("params", {
    int i = 0;
    dbgs() << "Params:\n";
    for (auto it = Params.begin(); it != Params.end(); it++) {
      dbgs() << "\tParam[" << i++ << "]: " << **it << " (" << *it << ")\n";
    }
  });

  DEBUG_WITH_TYPE("stmt", dbgs() << "Body:\n");
  DEBUG_WITH_TYPE("stmt", for (unsigned i = 0; i < Stmts.size(); i++) {
    dbgs() << "\tStmt[" << i << "]: " << *Stmts[i] << "\n";
  });
  DEBUG_WITH_TYPE("info", dbgs() << "\n\n");

  return Addr->getType();
}

Function *CarePass::createFunction(Type *RetTy, std::set<Value *> Params,
                                   std::vector<Value *> Stmts) {
  DEBUG_WITH_TYPE("RK", {
    dbgs() << "Create Recovery Kernel Function.\n    Params: ";
    int i = 0;
    for (auto it = Params.begin(); it != Params.end(); it++) {
      dbgs() << "\n\tParam[" << i++ << "]: " << **it;
    }
    dbgs() << "\n";
  });

  // a map from value in program module to the value in Care Module
  std::map<Value *, Value *> VMap;

  // create a new recovery kernel function to the CareM Module
  FunctionType *FuncTy = getFunctionType(Params);

  DEBUG_WITH_TYPE("RK", dbgs() << "Create Function Type: " << *FuncTy << "\n");

  std::string fname = getFunctionName();
  Function *RK = dyn_cast<Function>(CareM->getOrInsertFunction(fname, FuncTy));

  DEBUG_WITH_TYPE("RK", dbgs() << "Init arguments and VMap with Params\n");

  // make sure we did the thing correctly, the number of Params is equal to
  // the number of arguments
  assert(Params.size() == RK->arg_size());

  // set the argument name based on related parameter name
  // and create a map from original values to function arguments
  std::set<Value *>::iterator pit;
  llvm::Function::arg_iterator ait;
  for (ait = RK->arg_begin(), pit = Params.begin(); ait != RK->arg_end();
       ait++, pit++) {
    ait->setName(getOrCreateValueName(*pit));
    VMap[(*pit)] = dyn_cast<Value>(ait);
  }

  DEBUG_WITH_TYPE("RK", dbgs() << "Insert/Copying Instructions:\n");

  BasicBlock *BB = BasicBlock::Create(CareM->getContext(), "entry", RK);
  IRBuilder<> IRB(BB);
  for (Value *V : Stmts) {
    // avoid duplications, since vector is used when retrieving
    // computing instructions
    DEBUG_WITH_TYPE("RK", dbgs() << "Working on inst :" << *V << "\n");
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
    DEBUG_WITH_TYPE("RK", dbgs() << "Preparing Operands:\n");
    if (auto CI = dyn_cast<CallInst>(Insn)) {
      for (unsigned i = 0; i < CI->getNumArgOperands(); i++) {
        Value *Op = CI->getArgOperand(i);
        if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) {
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
        DEBUG_WITH_TYPE("RK", dbgs() << "Resolve Operand: " << *Op << "\n");
        // if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) {
        if (isa<Constant>(Op)) {
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
    DEBUG_WITH_TYPE("RK", dbgs() << "Create/Copy Inst: " << *Insn << "\n");

    Value *Inst = createInstruction(IRB, Insn, Operands);

    DEBUG_WITH_TYPE("RK",
                    dbgs() << "New Instruction: " << *Inst << " is created.\n");

    VMap[Insn] = Inst;
  }
  DEBUG_WITH_TYPE("RK", dbgs() << "Finalize the kernel.\n");

  Value *T;
  if (Stmts.size())
    T = Stmts.back();
  else if (Params.size() == 1)
    T = *Params.begin();
  else
    llvm_unreachable("Zero stmts and more than 1 params");

  DEBUG_WITH_TYPE("RK", dbgs() << "Terminal Value:  " << *T << "\n");

  DEBUG_WITH_TYPE("RK", {
    Type *STy = T->getType();
    dbgs() << "Final Type Conv Inst: " << *T << "\tType: " << *STy << "\n";
  });

  Value *Final =
      IRB.CreateSExtOrBitCast(VMap[T], Type::getInt8PtrTy(CareM->getContext()));
  IRB.CreateRet(Final);

  DEBUG_WITH_TYPE("RK", dbgs() << "Function: \n" << *RK);
  return RK;
}

FunctionType *CarePass::getFunctionType(std::set<Value *> Params) {
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

Value *CarePass::createInstruction(IRBuilder<> &IRB, Instruction *Insn,
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

char CarePass::ID = 0;

static RegisterPass<CarePass> X("carepass", "carepass", false, false);
