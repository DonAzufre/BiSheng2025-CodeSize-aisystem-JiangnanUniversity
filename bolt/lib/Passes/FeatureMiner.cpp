//===--- Passes/FeatureMiner.cpp ------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// A very simple feature extractor based on Calder's paper
// Evidence-based static branch prediction using machine learning
// https://dl.acm.org/doi/10.1145/239912.239923
//===----------------------------------------------------------------------===//

#include "bolt/Passes/FeatureMiner.h"
#include "bolt/Passes/DataflowInfoManager.h"
#include "llvm/Support/CommandLine.h"
//include "llvm/Support/Options.h" dhf-modify notsure
#include "llvm/Support/CommandLine.h"
#undef DEBUG_TYPE
#define DEBUG_TYPE "bolt-feature-miner"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory InferenceCategory;

cl::opt<bool> VespaUseDFS(
  "vespa-dfs",
  cl::desc("use DFS ordering when using -gen-features option"),
  cl::init(false),
  cl::ReallyHidden,
  cl::ZeroOrMore,
  cl::cat(InferenceCategory));

cl::opt<bool> IncludeValidProfile(
  "beetle-valid-profile-info",
  cl::desc("include valid profile information."),
  cl::init(false),
  cl::ReallyHidden,
  cl::ZeroOrMore,
  cl::cat(InferenceCategory));

} // namespace opts

namespace llvm {
namespace bolt {

class BinaryFunction;

int8_t FeatureMiner::getProcedureType(BinaryFunction &Function,
                                      BinaryContext &BC) {
  int8_t ProcedureType = 1;
  for (auto &BB : Function) {
    for (auto &Inst : BB) {
      if (BC.MIB->isCall(Inst)) {
        ProcedureType = 0; // non-leaf type
        if (const auto *CalleeSymbol = BC.MIB->getTargetSymbol(Inst)) {
          const auto *Callee = BC.getFunctionForSymbol(CalleeSymbol);
          if (Callee &&
              Callee->getFunctionNumber() == Function.getFunctionNumber()) {
            return 2; // call self type
          }
        }
      }
    }
  }
  return ProcedureType; // leaf type
}

void FeatureMiner::addSuccessorInfo(DominatorAnalysis<false> &DA,
                                    DominatorAnalysis<true> &PDA,
                                    BFIPtr const &BFI, BinaryFunction &Function,
                                    BinaryContext &BC, MCInst &Inst,
                                    BinaryBasicBlock &BB, bool SuccType) {

  BinaryBasicBlock *Successor = BB.getConditionalSuccessor(SuccType);

  if (!Successor)
    return;

  unsigned NumLoads{0};
  unsigned NumStores{0};
  unsigned NumCallsExit{0};
  unsigned NumCalls{0};
  unsigned NumCallsInvoke{0};
  unsigned NumTailCalls{0};
  unsigned NumIndirectCalls{0};

  for (auto &Inst : BB) {
    if (BC.MIB->isLoad(Inst)) {
      ++NumLoads;
    } else if (BC.MIB->isStore(Inst)) {
      ++NumStores;
    } else if (BC.MIB->isCall(Inst)) {
      ++NumCalls;

      // if (BC.MIB->isIndirectCall(Inst))
      //   ++NumIndirectCalls;

      // if (BC.MIB->isInvoke(Inst))
      //   ++NumCallsInvoke;

      // if (BC.MIB->isTailCall(Inst))
      //   ++NumTailCalls;

      // if (const auto *CalleeSymbol = BC.MIB->getTargetSymbol(Inst)) {
      //   StringRef CalleeName = CalleeSymbol->getName();
      //   if (CalleeName == "__cxa_throw@PLT" ||
      //       CalleeName == "_Unwind_Resume@PLT" ||
      //       CalleeName == "__cxa_rethrow@PLT" || CalleeName == "exit@PLT" ||
      //       CalleeName == "abort@PLT")
      //     ++NumCallsExit;
      // }
    }
  }

  BBIPtr SuccBBInfo = std::make_unique<struct BasicBlockInfo>();

  // Check if the successor basic block is a loop header and store it.
  SuccBBInfo->LoopHeader = SBI->isLoopHeader(Successor);

  SuccBBInfo->BasicBlockSize = Successor->size();

  // Check if the edge getting to the successor basic block is a loop
  // exit edge and store it.
  SuccBBInfo->Exit = SBI->isExitEdge(&BB, Successor);

  // Check if the edge getting to the successor basic block is a loop
  // back edge and store it.
  SuccBBInfo->Backedge = SBI->isBackEdge(&BB, Successor);

  MCInst *SuccInst = Successor->getTerminatorBefore(nullptr);
  // Store information about the branch type ending sucessor basic block
  SuccBBInfo->EndOpcode = (SuccInst && BC.MIA->isBranch(*SuccInst))
                              ? SuccInst->getOpcode()
                              : 0; // 0 = NOTHING
  if (SuccBBInfo->EndOpcode != 0)
    SuccBBInfo->EndOpcodeStr = BC.MII->getName(SuccInst->getOpcode());
  else
    SuccBBInfo->EndOpcodeStr = "NOTHING";

  // Check if the successor basic block contains
  // a procedure call and store it.
  SuccBBInfo->Call = (NumCalls > 0) ? 1  // Contains a call instruction
                                    : 0; // Does not contain a call instruction

  SuccBBInfo->NumStores = NumStores;
  SuccBBInfo->NumLoads = NumLoads;
  SuccBBInfo->NumCallsExit = NumCallsExit;
  SuccBBInfo->NumCalls = NumCalls;

  SuccBBInfo->NumCallsInvoke = NumCallsInvoke;
  SuccBBInfo->NumIndirectCalls = NumIndirectCalls;
  SuccBBInfo->NumTailCalls = NumTailCalls;

  auto InstSucc = Successor->getLastNonPseudoInstr();
  if (InstSucc) {
    // Check if the source basic block dominates its
    // target basic block and store it.
    SuccBBInfo->BranchDominates = (DA.doesADominateB(Inst, *InstSucc) == true)
                                      ? 1  // Dominates
                                      : 0; // Does not dominate

    // Check if the target basic block postdominates
    // the source basic block and store it.
    SuccBBInfo->BranchPostdominates =
        (PDA.doesADominateB(*InstSucc, Inst) == true)
            ? 1  // Postdominates
            : 0; // Does not postdominate
  }

  /// The follwoing information is used as an identifier only for
  /// the purpose of matching the inferred probabilities with the branches
  /// in the binary.
  SuccBBInfo->FromFunName = Function.getPrintName();
  SuccBBInfo->FromBb =  BB.getInputOffset();
  BinaryFunction *ToFun = Successor->getFunction();
  SuccBBInfo->ToFunName = ToFun->getPrintName();
  SuccBBInfo->ToBb = Successor->getInputOffset();

  auto Offset = BC.MIB->tryGetAnnotationAs<uint32_t>(Inst, "Offset");
  if (Offset) {
    uint32_t TargetOffset = Successor->getInputOffset();
    uint32_t BranchOffset = Offset.get();
    BFI->BranchOffset = BranchOffset;
    if (BranchOffset != UINT32_MAX && TargetOffset != UINT32_MAX) {
      int64_t Delta = TargetOffset - BranchOffset;
      BFI->DeltaTaken = std::abs(Delta);
    }
  }

  if (SuccType) {
    BFI->TrueSuccessor = std::move(SuccBBInfo);

    // Check if the taken branch is a forward
    // or a backwards branch and store it.
    BFI->Direction = (Function.isForwardBranch(&BB, Successor) == true)
                         ? 1  // Forward branch
                         : 0; // Backwards branch

    auto TakenBranchInfo = BB.getTakenBranchInfo();
    BFI->Count = TakenBranchInfo.Count;
    BFI->MissPredicted = TakenBranchInfo.MispredictedCount;
  } else {
    BFI->FalseSuccessor = std::move(SuccBBInfo);

    auto FallthroughBranchInfo = BB.getFallthroughBranchInfo();
    BFI->FallthroughCount = FallthroughBranchInfo.Count;
    BFI->FallthroughMissPredicted = FallthroughBranchInfo.MispredictedCount;
  }
}


void FeatureMiner::extractFeatures(BinaryFunction &Function, BinaryContext &BC,
                                   raw_ostream &Printer) {
  int8_t ProcedureType = getProcedureType(Function, BC);
  //dhf-modify not sure
  //auto Info = DataflowInfoManager(BC, Function，nullptr, nullptr);
  auto Info = DataflowInfoManager(Function, nullptr, nullptr, 0);

  auto &DA = Info.getDominatorAnalysis();
  auto &PDA = Info.getPostDominatorAnalysis();
  const BinaryLoopInfo &LoopsInfo = Function.getLoopInfo();
  bool Simple = Function.isSimple();

  //dhf-modify not sure
  //const auto &Order = opts::VespaUseDFS ? Function.dfs() : Function.getLayout();
  llvm::SmallVector<llvm::bolt::BinaryBasicBlock *> Order;
  if (opts::VespaUseDFS) {
    Order = Function.dfs();
  } else {
    auto LayoutRange = Function.getLayout().blocks();
    Order.append(LayoutRange.begin(), LayoutRange.end());
  }

  for (auto *BBA : Order) {

    auto &BB = *BBA;
    unsigned NumOuterLoops{0};
    unsigned TotalLoops{0};
    unsigned MaximumLoopDepth{0};
    unsigned LoopDepth{0};
    unsigned LoopNumExitEdges{0};
    unsigned LoopNumExitBlocks{0};
    unsigned LoopNumExitingBlocks{0};
    unsigned LoopNumLatches{0};
    unsigned LoopNumBlocks{0};
    unsigned LoopNumBackEdges{0};

    bool LocalExitingBlock{false};
    bool LocalLatchBlock{false};
    bool LocalLoopHeader{false};

    BinaryLoop *Loop = LoopsInfo.getLoopFor(&BB);
    if (Loop) {
      SmallVector<BinaryBasicBlock *, 1> ExitingBlocks;
      Loop->getExitingBlocks(ExitingBlocks);

      SmallVector<BinaryBasicBlock *, 1> ExitBlocks;
      Loop->getExitBlocks(ExitBlocks);

      SmallVector<BinaryLoop::Edge, 1> ExitEdges;
      Loop->getExitEdges(ExitEdges);

      SmallVector<BinaryBasicBlock *, 1> Latches;
      Loop->getLoopLatches(Latches);

      NumOuterLoops = LoopsInfo.OuterLoops;
      TotalLoops = LoopsInfo.TotalLoops;
      MaximumLoopDepth = LoopsInfo.MaximumDepth;
      LoopDepth = Loop->getLoopDepth();
      LoopNumExitEdges = ExitEdges.size();
      LoopNumExitBlocks = ExitBlocks.size();
      LoopNumExitingBlocks = ExitingBlocks.size();
      LoopNumLatches = Latches.size();
      LoopNumBlocks = Loop->getNumBlocks();
      LoopNumBackEdges = Loop->getNumBackEdges();

      LocalExitingBlock = Loop->isLoopExiting(&BB);
      LocalLatchBlock = Loop->isLoopLatch(&BB);
      LocalLoopHeader = ((Loop->getHeader() == (&BB)) ? 1 : 0);
    }

    unsigned NumLoads{0};
    unsigned NumStores{0};
    unsigned NumCallsExit{0};
    unsigned NumCalls{0};
    unsigned NumCallsInvoke{0};
    unsigned NumTailCalls{0};
    unsigned NumIndirectCalls{0};
    unsigned NumSelfCalls{0};

    for (auto &Inst : BB) {
      if (BC.MIB->isLoad(Inst)) {
        ++NumLoads;
      } else if (BC.MIB->isStore(Inst)) {
        // llvm::errs() << "isStore Opcode================================"<<Inst.getOpcode()<<"\n";
        ++NumStores;
      } else if (BC.MIB->isCall(Inst)) {
        // llvm::errs() << "isCall Opcode================================"<<Inst.getOpcode()<<"\n";
        ++NumCalls;

        // if (BC.MIB->isIndirectCall(Inst))
        //   ++NumIndirectCalls;

        // if (BC.MIB->isInvoke(Inst))
        //   ++NumCallsInvoke;

        // if (BC.MIB->isTailCall(Inst))
        //   ++NumTailCalls;

        // if (const auto *CalleeSymbol = BC.MIB->getTargetSymbol(Inst)) {
        //   StringRef CalleeName = CalleeSymbol->getName();
        //   if (CalleeName == "__cxa_throw@PLT" ||
        //       CalleeName == "_Unwind_Resume@PLT" ||
        //       CalleeName == "__cxa_rethrow@PLT" || CalleeName == "exit@PLT" ||
        //       CalleeName == "abort@PLT")
        //     ++NumCallsExit;
        //   else if (CalleeName == Function.getPrintName()) {
        //     ++NumSelfCalls;
        //   }
        // }
      }
    }
    // llvm::errs() << "NumLoads================================"<<NumLoads<<"\n";
    // llvm::errs() << "NumStores==============================="<<NumStores<<"\n";
    // llvm::errs() << "NumCalls================================"<<NumCalls<<"\n";
    // llvm::errs() << "NumIndirectCalls========================"<<NumIndirectCalls<<"\n";
    // llvm::errs() << "NumCallsInvoke=========================="<<NumCallsInvoke<<"\n";
    // llvm::errs() << "NumTailCalls============================"<<NumTailCalls<<"\n\n";
    int Index = -2;
    bool LoopHeader = SBI->isLoopHeader(&BB);
    // llvm::errs() << "到达两个for (auto &Inst : BB)中间\n";
    for (auto &Inst : BB) {
      // llvm::errs() << "第二个for (auto &Inst : BB)开始\n";
      ++Index;

      if (!BC.MIA->isConditionalBranch(Inst)){
        // llvm::errs() << "!BC.MIA->isConditionalBranch(Inst) , continue\n";
          continue;
      }
      // llvm::errs() << "识别到条件分支指令\n";
      BFIPtr BFI = std::make_unique<struct BranchFeaturesInfo>();

      BFI->Simple = Simple;
      BFI->NumOuterLoops = NumOuterLoops;
      BFI->TotalLoops = TotalLoops;
      BFI->MaximumLoopDepth = MaximumLoopDepth;
      BFI->LoopDepth = LoopDepth;
      BFI->LoopNumExitEdges = LoopNumExitEdges;
      BFI->LoopNumExitBlocks = LoopNumExitBlocks;
      BFI->LoopNumExitingBlocks = LoopNumExitingBlocks;
      BFI->LoopNumLatches = LoopNumLatches;
      BFI->LoopNumBlocks = LoopNumBlocks;
      BFI->LoopNumBackEdges = LoopNumBackEdges;

      BFI->LocalExitingBlock = LocalExitingBlock;
      BFI->LocalLatchBlock = LocalLatchBlock;
      BFI->LocalLoopHeader = LocalLoopHeader;

      BFI->Call = ((NumCalls > 0) ? 1 : 0);
      BFI->NumCalls = NumCalls;

      BFI->BasicBlockSize = BB.size();
      BFI->NumBasicBlocks = Function.size();
      BFI->NumSelfCalls = NumSelfCalls;

      BFI->NumLoads = NumLoads;
      BFI->NumStores = NumStores;
      BFI->NumCallsExit = NumCallsExit;

      BFI->NumCallsInvoke = NumCallsInvoke;
      BFI->NumIndirectCalls = NumIndirectCalls;
      BFI->NumTailCalls = NumTailCalls;

      // Check if branch's basic block is a loop header and store it.
      BFI->LoopHeader = LoopHeader;

      // Adding taken successor info.
      addSuccessorInfo(DA, PDA, BFI, Function, BC, Inst, BB, true);

      // Adding fall through successor info.
      addSuccessorInfo(DA, PDA, BFI, Function, BC, Inst, BB, false);

      // Holds the branch opcode info.
      BFI->Opcode = Inst.getOpcode();
      BFI->OpcodeStr = BC.MII->getName(Inst.getOpcode());

      // Holds the branch's procedure type.
      BFI->ProcedureType = ProcedureType;

      BFI->CmpOpcode = 0;
      if (Index > -1) {
        auto Cmp = BB.begin() + Index;

        if (BC.MII->get((*Cmp).getOpcode()).isCompare()) {
          // Holding the branch comparison opcode info.
          BFI->CmpOpcode = (*Cmp).getOpcode();

          BFI->CmpOpcodeStr = BC.MII->getName((*Cmp).getOpcode());

          auto getOperandType = [&](const MCOperand &Operand) -> int32_t {
            if (Operand.isReg())
              return 0;
            else if (Operand.isImm())
              return 1;
          //dhf-modify not sure
          //  else if (Operand.isFPImm())
            else if (Operand.isSFPImm())
              return 2;
            else if (Operand.isExpr())
              return 3;
            else
              return -1;
          };

          const auto InstInfo = BC.MII->get((*Cmp).getOpcode());
          unsigned NumDefs = InstInfo.getNumDefs();
          int32_t NumPrimeOperands =
              MCPlus::getNumPrimeOperands(*Cmp) - NumDefs;
          switch (NumPrimeOperands) {
          case 6: {
            int32_t RBType = getOperandType((*Cmp).getOperand(NumDefs));
            int32_t RAType = getOperandType((*Cmp).getOperand(NumDefs + 1));

            if (RBType == 0 && RAType == 0) {
              BFI->OperandRBType = RBType;
              BFI->OperandRAType = RAType;
            } else if (RBType == 0 && (RAType == 1 || RAType == 2)) {
              RAType = getOperandType((*Cmp).getOperand(NumPrimeOperands - 1));

              if (RAType != 1 && RAType != 2) {
                RAType = -1;
              }

              BFI->OperandRBType = RBType;
              BFI->OperandRAType = RAType;
            } else {
              BFI->OperandRAType = -1;
              BFI->OperandRBType = -1;
            }
            break;
          }
          case 2:
            BFI->OperandRBType = getOperandType((*Cmp).getOperand(NumDefs));
            BFI->OperandRAType = getOperandType((*Cmp).getOperand(NumDefs + 1));
            break;
          case 3:
            BFI->OperandRBType = getOperandType((*Cmp).getOperand(NumDefs));
            BFI->OperandRAType = getOperandType((*Cmp).getOperand(NumDefs + 2));
            break;
          case 1:
            BFI->OperandRAType = getOperandType((*Cmp).getOperand(NumDefs));
            break;
          default:
            BFI->OperandRAType = -1;
            BFI->OperandRBType = -1;
            break;
          }

        } else {
          Index -= 1;
          for (int Idx = Index; Idx > -1; Idx--) {
            auto Cmp = BB.begin() + Idx;
            if (BC.MII->get((*Cmp).getOpcode()).isCompare()) {
              // Holding the branch comparison opcode info.
              BFI->CmpOpcode = (*Cmp).getOpcode();
              BFI->CmpOpcodeStr = BC.MII->getName((*Cmp).getOpcode());
              break;
            }
          }
        }
      }
      // llvm::errs() << "第二个for (auto &Inst : BB)中间\n";
      //========================================================================

      auto &FalseSuccessor = BFI->FalseSuccessor;
      auto &TrueSuccessor = BFI->TrueSuccessor;

      if (!FalseSuccessor && !TrueSuccessor){
        // llvm::errs() << "!FalseSuccessor && !TrueSuccessor , continue\n";
          continue;
      }
      // llvm::errs() << "if (!FalseSuccessor && !TrueSuccessor)没判定成功，程序继续\n";
      int64_t BranchOffset = 
          (BFI->BranchOffset.has_value())
            ? static_cast<int64_t>(*(BFI->BranchOffset))
            : 24;
      if(BranchOffset == -1){
        llvm::errs() << "BranchOffset == -1 , continue\n";
          continue;
      }
      // llvm::errs() << "if(BranchOffset == -1)没判定成功，程序继续\n";
      int16_t ProcedureType = (BFI->ProcedureType.has_value())
                                  ? static_cast<int16_t>(*(BFI->ProcedureType))
                                  : -1;

      int16_t Direction = (BFI->Direction.has_value())
                              ? static_cast<bool>(*(BFI->Direction))
                              : -1;

      int16_t LoopHeader = (BFI->LoopHeader.has_value())
                               ? static_cast<bool>(*(BFI->LoopHeader))
                               : -1;

      int32_t Opcode =
          (BFI->Opcode.has_value()) ? static_cast<int32_t>(*(BFI->Opcode)) : -1;

      int32_t CmpOpcode = (BFI->CmpOpcode.has_value())
                              ? static_cast<int32_t>(*(BFI->CmpOpcode))
                              : -1;

      int64_t Count =
          (BFI->Count.has_value()) ? static_cast<int64_t>(*(BFI->Count)) : -1;

      int64_t MissPredicted = (BFI->MissPredicted.has_value())
                                  ? static_cast<int64_t>(*(BFI->MissPredicted))
                                  : -1;

      int64_t FallthroughCount =
          (BFI->FallthroughCount.has_value())
              ? static_cast<int64_t>(*(BFI->FallthroughCount))
              : -1;

      int64_t FallthroughMissPredicted =
          (BFI->FallthroughMissPredicted.has_value())
              ? static_cast<int64_t>(*(BFI->FallthroughMissPredicted))
              : -1;

      int64_t NumOuterLoops = (BFI->NumOuterLoops.has_value())
                                  ? static_cast<int64_t>(*(BFI->NumOuterLoops))
                                  : -1;
      int64_t TotalLoops = (BFI->TotalLoops.has_value())
                               ? static_cast<int64_t>(*(BFI->TotalLoops))
                               : -1;
      int64_t MaximumLoopDepth =
          (BFI->MaximumLoopDepth.has_value())
              ? static_cast<int64_t>(*(BFI->MaximumLoopDepth))
              : -1;
      int64_t LoopDepth = (BFI->LoopDepth.has_value())
                              ? static_cast<int64_t>(*(BFI->LoopDepth))
                              : -1;
      int64_t LoopNumExitEdges =
          (BFI->LoopNumExitEdges.has_value())
              ? static_cast<int64_t>(*(BFI->LoopNumExitEdges))
              : -1;
      int64_t LoopNumExitBlocks =
          (BFI->LoopNumExitBlocks.has_value())
              ? static_cast<int64_t>(*(BFI->LoopNumExitBlocks))
              : -1;
      int64_t LoopNumExitingBlocks =
          (BFI->LoopNumExitingBlocks.has_value())
              ? static_cast<int64_t>(*(BFI->LoopNumExitingBlocks))
              : -1;
      int64_t LoopNumLatches =
          (BFI->LoopNumLatches.has_value())
              ? static_cast<int64_t>(*(BFI->LoopNumLatches))
              : -1;
      int64_t LoopNumBlocks = (BFI->LoopNumBlocks.has_value())
                                  ? static_cast<int64_t>(*(BFI->LoopNumBlocks))
                                  : -1;
      int64_t LoopNumBackEdges =
          (BFI->LoopNumBackEdges.has_value())
              ? static_cast<int64_t>(*(BFI->LoopNumBackEdges))
              : -1;

      int64_t LocalExitingBlock =
          (BFI->LocalExitingBlock.has_value())
              ? static_cast<bool>(*(BFI->LocalExitingBlock))
              : -1;

      int64_t LocalLatchBlock = (BFI->LocalLatchBlock.has_value())
                                    ? static_cast<bool>(*(BFI->LocalLatchBlock))
                                    : -1;

      int64_t LocalLoopHeader = (BFI->LocalLoopHeader.has_value())
                                    ? static_cast<bool>(*(BFI->LocalLoopHeader))
                                    : -1;

      int64_t Call =
          (BFI->Call.has_value()) ? static_cast<bool>(*(BFI->Call)) : -1;

      int64_t DeltaTaken = (BFI->DeltaTaken.has_value())
                             ? static_cast<int64_t>(*(BFI->DeltaTaken))
                             : -1;

      int64_t NumLoads = (BFI->NumLoads.has_value())
                             ? static_cast<int64_t>(*(BFI->NumLoads))
                             : -1;

      int64_t NumStores = (BFI->NumStores.has_value())
                              ? static_cast<int64_t>(*(BFI->NumStores))
                              : -1;

      int64_t BasicBlockSize =
          (BFI->BasicBlockSize.has_value())
              ? static_cast<int64_t>(*(BFI->BasicBlockSize))
              : -1;

      int64_t NumBasicBlocks =
          (BFI->NumBasicBlocks.has_value())
              ? static_cast<int64_t>(*(BFI->NumBasicBlocks))
              : -1;

      int64_t NumCalls = (BFI->NumCalls.has_value())
                             ? static_cast<int64_t>(*(BFI->NumCalls))
                             : -1;

      int64_t NumSelfCalls = (BFI->NumSelfCalls.has_value())
                                 ? static_cast<int64_t>(*(BFI->NumSelfCalls))
                                 : -1;

      int64_t NumCallsExit = (BFI->NumCallsExit.has_value())
                                 ? static_cast<int64_t>(*(BFI->NumCallsExit))
                                 : -1;

      int64_t OperandRAType = (BFI->OperandRAType.has_value())
                                  ? static_cast<int32_t>(*(BFI->OperandRAType))
                                  : -1;

      int64_t OperandRBType = (BFI->OperandRBType.has_value())
                                  ? static_cast<int32_t>(*(BFI->OperandRBType))
                                  : -1;

      int64_t NumCallsInvoke =
          (BFI->NumCallsInvoke.has_value())
              ? static_cast<int64_t>(*(BFI->NumCallsInvoke))
              : -1;

      int64_t NumIndirectCalls =
          (BFI->NumIndirectCalls.has_value())
              ? static_cast<int64_t>(*(BFI->NumIndirectCalls))
              : -1;

      int64_t NumTailCalls = (BFI->NumTailCalls.has_value())
                                 ? static_cast<int64_t>(*(BFI->NumTailCalls))
                                 : -1;
      // llvm::errs() << "成功走到打印特征\n";
      Printer << BFI->Simple << "," << Opcode << "," << BFI->OpcodeStr << ","
              << Direction << "," << CmpOpcode << "," << BFI->CmpOpcodeStr
              << "," << LoopHeader << "," << ProcedureType << "," << Count
              << "," << MissPredicted << "," << FallthroughCount << ","
              << FallthroughMissPredicted << "," << NumOuterLoops << ","
              << NumCallsExit << "," << TotalLoops << "," << MaximumLoopDepth
              << "," << LoopDepth << "," << LoopNumExitEdges << ","
              << LoopNumExitBlocks << "," << LoopNumExitingBlocks << ","
              << LoopNumLatches << "," << LoopNumBlocks << ","
              << LoopNumBackEdges << "," << LocalExitingBlock << ","
              << LocalLatchBlock << "," << LocalLoopHeader << "," << Call << ","
              << DeltaTaken << "," << NumLoads << "," << NumStores << ","
              << NumCalls << "," << OperandRAType << "," << OperandRBType << ","
              << BasicBlockSize << "," << NumBasicBlocks << ","
              << NumCallsInvoke << "," << NumIndirectCalls << ","
              << NumTailCalls << "," << NumSelfCalls;

      if (FalseSuccessor && TrueSuccessor) {
        dumpSuccessorFeatures(Printer, TrueSuccessor);
        dumpSuccessorFeatures(Printer, FalseSuccessor);

        FalseSuccessor.reset();
        TrueSuccessor.reset();
      }
      BFI.reset();

      std::string BranchOffsetStr =  (BranchOffset == -1) ? "None" : Twine::utohexstr(BranchOffset).str();

      uint64_t fun_exec = Function.getExecutionCount();
      fun_exec = (fun_exec != UINT64_MAX) ? fun_exec : 0;
      Printer << "," << Twine::utohexstr(Function.getAddress()) << ","
              << fun_exec << "," << Function.getFunctionNumber() << ","
              << Function.getOneName() << "," << Function.getPrintName()
              << "," << BranchOffsetStr
              << "\n";

      //========================================================================

      // this->BranchesInfoSet.push_back(std::move(BFI));
      // llvm::errs() << "第二个for (auto &Inst : BB)正常结束\n";
    }
  }
}

void FeatureMiner::dumpSuccessorFeatures(raw_ostream &Printer,
                                         BBIPtr &Successor) {
  int16_t BranchDominates =
      (Successor->BranchDominates.has_value())
          ? static_cast<bool>(*(Successor->BranchDominates))
          : -1;

  int16_t BranchPostdominates =
      (Successor->BranchPostdominates.has_value())
          ? static_cast<bool>(*(Successor->BranchPostdominates))
          : -1;

  int16_t LoopHeader = (Successor->LoopHeader.has_value())
                           ? static_cast<bool>(*(Successor->LoopHeader))
                           : -1;

  int16_t Backedge = (Successor->Backedge.has_value())
                         ? static_cast<bool>(*(Successor->Backedge))
                         : -1;

  int16_t Exit =
      (Successor->Exit.has_value()) ? static_cast<bool>(*(Successor->Exit)) : -1;

  int16_t Call =
      (Successor->Call.has_value()) ? static_cast<bool>(*(Successor->Call)) : -1;

  int32_t EndOpcode = (Successor->EndOpcode.has_value())
                          ? static_cast<int32_t>(*(Successor->EndOpcode))
                          : -1;

  int64_t NumLoads = (Successor->NumLoads.has_value())
                         ? static_cast<int64_t>(*(Successor->NumLoads))
                         : -1;

  int64_t NumStores = (Successor->NumStores.has_value())
                          ? static_cast<int64_t>(*(Successor->NumStores))
                          : -1;

  int64_t BasicBlockSize =
      (Successor->BasicBlockSize.has_value())
          ? static_cast<int64_t>(*(Successor->BasicBlockSize))
          : -1;

  int64_t NumCalls = (Successor->NumCalls.has_value())
                         ? static_cast<int64_t>(*(Successor->NumCalls))
                         : -1;

  int64_t NumCallsExit = (Successor->NumCallsExit.has_value())
                             ? static_cast<int64_t>(*(Successor->NumCallsExit))
                             : -1;

  int64_t NumCallsInvoke =
      (Successor->NumCallsInvoke.has_value())
          ? static_cast<int64_t>(*(Successor->NumCallsInvoke))
          : -1;

  int64_t NumIndirectCalls =
      (Successor->NumIndirectCalls.has_value())
          ? static_cast<int64_t>(*(Successor->NumIndirectCalls))
          : -1;

  int64_t NumTailCalls = (Successor->NumTailCalls.has_value())
                             ? static_cast<int64_t>(*(Successor->NumTailCalls))
                             : -1;

  Printer << "," << BranchDominates << "," << BranchPostdominates << ","
          << EndOpcode << "," << Successor->EndOpcodeStr << "," << LoopHeader
          << "," << Backedge << "," << Exit << "," << Call << ","
          << Successor->FromFunName << ","
          << Twine::utohexstr(Successor->FromBb) << "," << Successor->ToFunName
          << "," << Twine::utohexstr(Successor->ToBb) << "," << NumLoads << ","
          << NumStores << "," << BasicBlockSize << "," << NumCalls << ","
          << NumCallsExit << "," << NumIndirectCalls << "," << NumCallsInvoke
          << "," << NumTailCalls;
}

void FeatureMiner::dumpFeatures(raw_ostream &Printer, uint64_t FunctionAddress,
                                uint64_t FunctionFrequency) {

  for (auto const &BFI : BranchesInfoSet) {
    auto &FalseSuccessor = BFI->FalseSuccessor;
    auto &TrueSuccessor = BFI->TrueSuccessor;

    if (!FalseSuccessor && !TrueSuccessor)
      continue;

    int16_t ProcedureType = (BFI->ProcedureType.has_value())
                                ? static_cast<int16_t>(*(BFI->ProcedureType))
                                : -1;

    int16_t Direction =
        (BFI->Direction.has_value()) ? static_cast<bool>(*(BFI->Direction)) : -1;

    int16_t LoopHeader = (BFI->LoopHeader.has_value())
                             ? static_cast<bool>(*(BFI->LoopHeader))
                             : -1;

    int32_t Opcode =
        (BFI->Opcode.has_value()) ? static_cast<int32_t>(*(BFI->Opcode)) : -1;

    int32_t CmpOpcode = (BFI->CmpOpcode.has_value())
                            ? static_cast<int32_t>(*(BFI->CmpOpcode))
                            : -1;

    int64_t Count =
        (BFI->Count.has_value()) ? static_cast<int64_t>(*(BFI->Count)) : -1;

    int64_t MissPredicted = (BFI->MissPredicted.has_value())
                                ? static_cast<int64_t>(*(BFI->MissPredicted))
                                : -1;

    int64_t FallthroughCount =
        (BFI->FallthroughCount.has_value())
            ? static_cast<int64_t>(*(BFI->FallthroughCount))
            : -1;

    int64_t FallthroughMissPredicted =
        (BFI->FallthroughMissPredicted.has_value())
            ? static_cast<int64_t>(*(BFI->FallthroughMissPredicted))
            : -1;

    int64_t NumOuterLoops = (BFI->NumOuterLoops.has_value())
                                ? static_cast<int64_t>(*(BFI->NumOuterLoops))
                                : -1;
    int64_t TotalLoops = (BFI->TotalLoops.has_value())
                             ? static_cast<int64_t>(*(BFI->TotalLoops))
                             : -1;
    int64_t MaximumLoopDepth =
        (BFI->MaximumLoopDepth.has_value())
            ? static_cast<int64_t>(*(BFI->MaximumLoopDepth))
            : -1;
    int64_t LoopDepth = (BFI->LoopDepth.has_value())
                            ? static_cast<int64_t>(*(BFI->LoopDepth))
                            : -1;
    int64_t LoopNumExitEdges =
        (BFI->LoopNumExitEdges.has_value())
            ? static_cast<int64_t>(*(BFI->LoopNumExitEdges))
            : -1;
    int64_t LoopNumExitBlocks =
        (BFI->LoopNumExitBlocks.has_value())
            ? static_cast<int64_t>(*(BFI->LoopNumExitBlocks))
            : -1;
    int64_t LoopNumExitingBlocks =
        (BFI->LoopNumExitingBlocks.has_value())
            ? static_cast<int64_t>(*(BFI->LoopNumExitingBlocks))
            : -1;
    int64_t LoopNumLatches = (BFI->LoopNumLatches.has_value())
                                 ? static_cast<int64_t>(*(BFI->LoopNumLatches))
                                 : -1;
    int64_t LoopNumBlocks = (BFI->LoopNumBlocks.has_value())
                                ? static_cast<int64_t>(*(BFI->LoopNumBlocks))
                                : -1;
    int64_t LoopNumBackEdges =
        (BFI->LoopNumBackEdges.has_value())
            ? static_cast<int64_t>(*(BFI->LoopNumBackEdges))
            : -1;

    int64_t LocalExitingBlock =
        (BFI->LocalExitingBlock.has_value())
            ? static_cast<bool>(*(BFI->LocalExitingBlock))
            : -1;

    int64_t LocalLatchBlock = (BFI->LocalLatchBlock.has_value())
                                  ? static_cast<bool>(*(BFI->LocalLatchBlock))
                                  : -1;

    int64_t LocalLoopHeader = (BFI->LocalLoopHeader.has_value())
                                  ? static_cast<bool>(*(BFI->LocalLoopHeader))
                                  : -1;

    int64_t Call =
        (BFI->Call.has_value()) ? static_cast<bool>(*(BFI->Call)) : -1;

    int64_t DeltaTaken = (BFI->DeltaTaken.has_value())
                             ? static_cast<int64_t>(*(BFI->DeltaTaken))
                             : -1;

    int64_t NumLoads = (BFI->NumLoads.has_value())
                           ? static_cast<int64_t>(*(BFI->NumLoads))
                           : -1;

    int64_t NumStores = (BFI->NumStores.has_value())
                            ? static_cast<int64_t>(*(BFI->NumStores))
                            : -1;

    int64_t BasicBlockSize = (BFI->BasicBlockSize.has_value())
                                 ? static_cast<int64_t>(*(BFI->BasicBlockSize))
                                 : -1;

    int64_t BranchOffset = (BFI->BranchOffset.has_value())
                              ? static_cast<int64_t>(*(BFI->BranchOffset)): -1;

    int64_t NumBasicBlocks = (BFI->NumBasicBlocks.has_value())
                                 ? static_cast<int64_t>(*(BFI->NumBasicBlocks))
                                 : -1;

    int64_t NumCalls = (BFI->NumCalls.has_value())
                           ? static_cast<int64_t>(*(BFI->NumCalls))
                           : -1;

    int64_t NumSelfCalls = (BFI->NumSelfCalls.has_value())
                               ? static_cast<int64_t>(*(BFI->NumSelfCalls))
                               : -1;

    int64_t NumCallsExit = (BFI->NumCallsExit.has_value())
                               ? static_cast<int64_t>(*(BFI->NumCallsExit))
                               : -1;

    int64_t OperandRAType = (BFI->OperandRAType.has_value())
                                ? static_cast<int32_t>(*(BFI->OperandRAType))
                                : -1;

    int64_t OperandRBType = (BFI->OperandRBType.has_value())
                                ? static_cast<int32_t>(*(BFI->OperandRBType))
                                : -1;

    int64_t NumCallsInvoke = (BFI->NumCallsInvoke.has_value())
                                 ? static_cast<int64_t>(*(BFI->NumCallsInvoke))
                                 : -1;

    int64_t NumIndirectCalls =
        (BFI->NumIndirectCalls.has_value())
            ? static_cast<int64_t>(*(BFI->NumIndirectCalls))
            : -1;

    int64_t NumTailCalls = (BFI->NumTailCalls.has_value())
                               ? static_cast<int64_t>(*(BFI->NumTailCalls))
                               : -1;

    Printer << BFI->Simple << "," << Opcode << "," << BFI->OpcodeStr << ","
            << Direction << "," << CmpOpcode << "," << BFI->CmpOpcodeStr << ","
            << LoopHeader << "," << ProcedureType << "," << Count << ","
            << MissPredicted << "," << FallthroughCount << ","
            << FallthroughMissPredicted << "," << NumOuterLoops << ","
            << NumCallsExit << "," << TotalLoops << "," << MaximumLoopDepth
            << "," << LoopDepth << "," << LoopNumExitEdges << ","
            << LoopNumExitBlocks << "," << LoopNumExitingBlocks << ","
            << LoopNumLatches << "," << LoopNumBlocks << "," << LoopNumBackEdges
            << "," << LocalExitingBlock << "," << LocalLatchBlock << ","
            << LocalLoopHeader << "," << Call << "," << DeltaTaken << ","
            << NumLoads << "," << NumStores << "," << NumCalls << ","
            << OperandRAType << "," << OperandRBType << "," << BasicBlockSize
            << "," << NumBasicBlocks << "," << NumCallsInvoke << ","
            << NumIndirectCalls << "," << NumTailCalls << "," << NumSelfCalls;

    if (FalseSuccessor && TrueSuccessor) {
      dumpSuccessorFeatures(Printer, TrueSuccessor);
      dumpSuccessorFeatures(Printer, FalseSuccessor);
    }

    Printer << "," << Twine::utohexstr(FunctionAddress) << ","
            << FunctionFrequency << "\n";
  }
  BranchesInfoSet.clear();
}

void FeatureMiner::runOnFunctions(BinaryContext &BC) {
  auto FileName = "features_new.csv";
  outs() << "BOLT-INFO: Starting feature miner pass\n";

  std::error_code EC;
  raw_fd_ostream Printer(FileName, EC);
  if (EC) {
    errs() << "BOLT-WARNING: " << EC.message() << ", unable to open "
           << FileName << " for output.\n";
    return;
  }

  auto FILENAME = "profile_data_regular.fdata";
  raw_fd_ostream Printer2(FILENAME, EC);
  if (EC) {
    errs() << "BOLT-WARNING: " << EC.message() << ", unable to open "
           << FILENAME << " for output.\n";
    return;
  }

  // CSV file header
  Printer << "FUN_TYPE,OPCODE,OPCODE_STR,DIRECTION,CMP_OPCODE,CMP_OPCODE_STR,"
             "LOOP_HEADER,PROCEDURE_TYPE,"
             "COUNT_TAKEN,MISS_TAKEN,COUNT_NOT_TAKEN,MISS_NOT_TAKEN,"
             "NUM_OUTER_LOOPS,NUM_CALLS_EXIT,TOTAL_LOOPS,MAXIMUM_LOOP_DEPTH,"
             "LOOP_DEPTH,LOOP_NUM_EXIT_EDGES,LOOP_NUM_EXIT_BLOCKS,"
             "LOOP_NUM_EXITING_BLOCKS,LOOP_NUM_LATCHES,LOOP_NUM_BLOCKS,"
             "LOOP_NUM_BAKCEDGES,LOCAL_EXITING_BLOCK,LOCAL_LATCH_BLOCK,"
             "LOCAL_LOOP_HEADER,CALL,DELTA_TAKEN,NUM_LOADS,NUM_STORES,"
             "NUM_CALLS,OPERAND_RA_TYPE,OPERAND_RB_TYPE,BASIC_BLOCK_SIZE,"
             "NUM_BASIC_BLOCKS,NUM_CALLS_INVOKE,NUM_INDIRECT_CALLS,"
             "NUM_TAIL_CALLS,NUM_SELF_CALLS,TS_DOMINATES,TS_POSTDOMINATES,"
             "TS_END_OPCODE,TS_END_OPCODE_STR,TS_LOOP_HEADER,TS_BACKEDGE,TS_"
             "EXIT,TS_CALL,"
             "TS_FROM_FUN_NAME,TS_FROM_BB,TS_TO_FUN_NAME,TS_TO_BB,TS_NUM_LOADS,"
             "TS_NUM_STORES,TS_BASIC_BLOCK_SIZE,TS_NUM_CALLS,TS_NUM_CALLS_EXIT,"
             "TS_NUM_INDIRECT_CALL,TS_NUM_CALLS_INVOKE,TS_NUM_TAIL_CALLS,"
             "FS_DOMINATES,FS_POSTDOMINATES,FS_END_OPCODE,FS_END_OPCODE_STR,FS_"
             "LOOP_HEADER,"
             "FS_BACKEDGE,FS_EXIT,FS_CALL,FS_FROM_FUN_NAME,FS_FROM_BB,"
             "FS_TO_FUN_NAME,FS_TO_BB,FS_NUM_LOADS,FS_NUM_STORES,"
             "FS_BASIC_BLOCK_SIZE,FS_NUM_CALLS,FS_NUM_CALLS_EXIT,"
             "FS_NUM_INDIRECT_CALL,FS_NUM_CALLS_INVOKE,FS_NUM_TAIL_CALLS,"
             "FUN_ENTRY_ADDRESS,FUN_ENTRY_FREQUENCY"
             ",FUN_UNIQUE_NUMBER,FUN_ONE_NAME,FUN_PRINT_NAME,"
             "BRANCH_ADDRESS\n";

  auto &BFs = BC.getBinaryFunctions();
  SBI = std::make_unique<StaticBranchInfo>();
  size_t FuncCounter = 0;

  for (auto &BFI : BFs) {
    BinaryFunction &Function = BFI.second;
    ++FuncCounter;

    // outs() << "[Miner] === Function #" << FuncCounter
          //  << ": " << Function.getPrintName() << "\n";

    if (Function.empty()) {
      // outs() << "[Miner]   Skipped: function is empty.\n";
      continue;
    }

    if (Function.hasValidProfile() && opts::IncludeValidProfile) {
      // outs() << "[Miner]   Skipped: already has valid profile.\n";
      continue;
    }

    if (!Function.isLoopFree()) {
      // outs() << "[Miner]   Loop info present. Getting loops...\n";
      const BinaryLoopInfo &LoopsInfo = Function.getLoopInfo();
      SBI->findLoopEdgesInfo(LoopsInfo);
      // outs() << "[Miner]   Loop info processed.\n";
    }

    // outs() << "[Miner]   >> extractFeatures START\n";
        std::string funcName = Function.getPrintName();
        // if (funcName.compare(0, strlen("__init_libc"), "__init_libc") == 0) {
        //   outs() << "[Miner] Skipping problematic function: " << funcName << "\n";
        //   continue;
        // }
        // if (funcName.rfind("libc_", 0) == 0 || funcName.rfind("__init_", 0) == 0) {
        //   outs() << "[Miner] Skipping libc-related function: " << funcName << "\n";
        //   continue;
        // }

        // if (
        //     funcName.rfind("__init_", 0) == 0 ||
        //     funcName.rfind("__libc_", 0) == 0 ||
        //     funcName.rfind("libc_", 0) == 0 ||
        //     funcName.rfind("__copy_tls", 0) == 0 ||
        //     funcName.rfind("__stack_chk", 0) == 0 ||
        //     funcName.find("crt") != std::string::npos ||
        //     funcName.find("start") != std::string::npos ||
        //     funcName.find("init") != std::string::npos
        // ) {
        //     outs() << "[Miner] Skipping libc/boot-related function: " << funcName << "\n";
        //     continue;
        // }
    extractFeatures(Function, BC, Printer);
    // outs() << "[Miner]   << extractFeatures END\n";
    
    SBI->clear();

    // outs() << "[Miner]   >> dumpProfileData START\n";
    dumpProfileData(Function, Printer2);
    // outs() << "[Miner]   << dumpProfileData END\n";

    // outs() << "[Miner] Done with: " << Function.getPrintName() << "\n";
  }
  outs() << "BOLT-INFO: Dumping two-way conditional branches' features at " << FileName << "\n";
}


void FeatureMiner::dumpProfileData(BinaryFunction &Function,
                                   raw_ostream &Printer) {

  BinaryContext &BC = Function.getBinaryContext();

  std::string FromFunName = Function.getPrintName();
  for (auto &BB : Function) {
    auto LastInst = BB.getLastNonPseudoInstr();

    for (auto &Inst : BB) {
      if (!BC.MIB->isCall(Inst) && !BC.MIB->isBranch(Inst) &&
          LastInst != (&Inst))
        continue;

      auto Offset = BC.MIB->tryGetAnnotationAs<uint32_t>(Inst, "Offset");

      if (!Offset)
        continue;

      uint64_t TakenFreqEdge = 0;
      auto FromBb = Offset.get();
      std::string ToFunName;
      uint32_t ToBb;

      if (BC.MIB->isCall(Inst)) {
        auto *CalleeSymbol = BC.MIB->getTargetSymbol(Inst);
        if (!CalleeSymbol)
          continue;

        ToFunName = CalleeSymbol->getName();
        ToBb = 0;

        if (BC.MIB->getConditionalTailCall(Inst)) {

          if (BC.MIB->hasAnnotation(Inst, "CTCTakenCount")) {
            auto CountAnnt =
                BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, "CTCTakenCount");
            if (CountAnnt) {
              TakenFreqEdge = (*CountAnnt);
            }
          }
        } else {
          if (BC.MIB->hasAnnotation(Inst, "Count")) {
            auto CountAnnt =
                BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, "Count");
            if (CountAnnt) {
              TakenFreqEdge = (*CountAnnt);
            }
          }
        }

        if (TakenFreqEdge > 0)
          Printer << "1 " << FromFunName << " " << Twine::utohexstr(FromBb)
                  << " 1 " << ToFunName << " " << Twine::utohexstr(ToBb) << " "
                  << 0 << " " << TakenFreqEdge << "\n";
      } else {
        for (BinaryBasicBlock *SuccBB : BB.successors()) {
          TakenFreqEdge = BB.getBranchInfo(*SuccBB).Count;
          BinaryFunction *ToFun = SuccBB->getFunction();
          ToFunName = ToFun->getPrintName();
          ToBb = SuccBB->getInputOffset();

          if (TakenFreqEdge > 0)
            Printer << "1 " << FromFunName << " " << Twine::utohexstr(FromBb)
                    << " 1 " << ToFunName << " " << Twine::utohexstr(ToBb)
                    << " " << 0 << " " << TakenFreqEdge << "\n";
        }
      }
    }
  }
}

} // namespace bolt
} // namespace llvm
