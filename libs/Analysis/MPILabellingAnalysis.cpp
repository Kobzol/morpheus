#include "CallFinder.hpp"

#include "morpheus/Analysis/MPILabellingAnalysis.hpp"


using namespace llvm;

// -------------------------------------------------------------------------- //
// MPILabellingAnalysis

LabellingResult
MPILabellingAnalysis::run (Function &f, FunctionAnalysisManager &fam) {

  LabellingResult result;
  result.explore_function(&f);

  return result;
}

// provide definition of the analysis Key
AnalysisKey MPILabellingAnalysis::Key;


// -------------------------------------------------------------------------- //
// LabellingResult

LabellingResult::ExplorationState
LabellingResult::explore_function(const Function *f) {
  auto it = fn_labels.find(f);
  if (it != fn_labels.end()) {
    return it->getSecond();
  }

  if (f->hasName() && f->getName().startswith("MPI_")) {
    fn_labels[f] = MPI_CALL;
    return MPI_CALL;
  }

  // NOTE: store the info that the function is currently being processed.
  //       This helps to resolve recursive calls as it immediately returns
  //       PROCESSING status.

  // TODO: => TEST: make a test to simple function calling itself. => it has to end with sequential
  fn_labels[f] = PROCESSING;

  ExplorationState res_es = SEQUENTIAL;
  for (const BasicBlock &bb : *f) {
    const ExplorationState& es = explore_bb(&bb, direct_mpi_calls[f], mediate_mpi_calls[f]);
    // NOTE: basic block cannot be directly of MPI_CALL type

    if (res_es < es) {
      res_es = es;
    }

    // NOTE: continue inspecting other functions to cover all calls.
  }

  fn_labels[f] = res_es; // set the resulting status
  return res_es;
}

LabellingResult::ExplorationState LabellingResult::explore_bb(
    const BasicBlock *bb,
    std::vector<CallInst *> &direct_mpi_calls,
    std::vector<CallInst *> &mediate_mpi_calls
) {

  ExplorationState res_es = SEQUENTIAL;
  std::vector<CallInst *> call_insts = CallFinder<BasicBlock>::find_in(*bb);
  for (CallInst *call_inst : call_insts) {
    Function *called_fn = call_inst->getCalledFunction();
    if (called_fn) {
      ExplorationState es = explore_function(called_fn);

      if (es == MPI_CALL) {
        res_es = MPI_INVOLVED;
        mpi_calls[called_fn->getName()].push_back(call_inst);
        direct_mpi_calls.push_back(call_inst);
      } else if (es > MPI_CALL) {
        res_es = ExplorationState::MPI_INVOLVED_MEDIATELY;
        mediate_mpi_calls.push_back(call_inst);
      }
    } else {
      // TODO: use assert(false) here!
      errs() << "no called function for: '" << *call_inst << "'\n";
    }
  }

  return res_es;
}

CallInst * LabellingResult::get_call(StringRef name) {

  if (mpi_calls.count(name) == 0) {
    return nullptr;
  }
  return mpi_calls[name][0]; // TODO: count with more call
}