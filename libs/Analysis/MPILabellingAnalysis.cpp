#include "CallFinder.hpp"

#include "morpheus/Analysis/MPILabellingAnalysis.hpp"


using namespace llvm;

// -------------------------------------------------------------------------- //
// MPILabellingAnalysis

LabellingResult
MPILabellingAnalysis::run (Function &f, FunctionAnalysisManager &fam) {

  return LabellingResult();
}

// provide definition of the analysis Key
AnalysisKey MPILabellingAnalysis::Key;


// -------------------------------------------------------------------------- //
// LabellingResult

ExplorationState LabellingResult::explore_function(const Function *f) {
  auto it = fn_labels.find(f);
  if (it != fn_labels.end()) {
    return it->getSecond();
  }

  if (f->hasName() && f->getName().startswith("MPI_")) {
    fn_labels.insert({ f, ExplorationState::MPI_CALL });
    return ExplorationState::MPI_CALL;
  }

  // NOTE: store the info that the function is currently being processed.
  //       This helps to resolve recursive calls as it immediately returns
  //       PROCESSING status.

  // TODO: => TEST: make a test to simple function calling itself. => it has to end with sequential
  fn_labels.insert({ f, ExplorationState::PROCESSING });

  ExplorationState res_es = ExplorationState::SEQUENTIAL; // NOTE (10.4.19): changed from PROCESSING -> I suppose that is sequential unless otherwise
  for (const BasicBlock &bb : *f) {
    const ExplorationState& es = explore_bb(&bb);
    // NOTE: basic block cannot be directly of MPI_CALL type

    if (res_es < es) {
      res_es = es;
    }

    // TODO: count with compound MPI_INVOLVED and MPI_INVOLVED_MEDIATELY together

    // NOTE: continue inspecting other functions to cover all calls.
  }

  fn_labels[f] = res_es; // set the resulting status
  return res_es;
}

ExplorationState LabellingResult::explore_bb(const BasicBlock *bb) {

  ExplorationState res_es = ExplorationState::SEQUENTIAL;
  std::vector<CallInst *> call_insts = CallFinder<BasicBlock>::find_in(*bb);
  for (CallInst *call_inst : call_insts) {
    Function *called_fn = call_inst->getCalledFunction();
    if (called_fn) {
      const ExplorationState &es = explore_function(called_fn);

      if (es == ExplorationState::MPI_CALL) {
        res_es = ExplorationState::MPI_INVOLVED;
        map_mpi_call(called_fn->getName(), call_inst);
      } else if (es > ExplorationState::MPI_CALL) {
        res_es = ExplorationState::MPI_INVOLVED_MEDIATELY;
      }
    } else {
      // TODO: use assert(false) here!
      errs() << "no called function for: '" << *call_inst << "'\n";
    }
  }

  return res_es;
}

void LabellingResult::map_mpi_call(StringRef name, CallInst *inst) {
  if (mpi_calls.count(name) == 0) {
    mpi_calls.insert({ name, {} });
  }
  mpi_calls[name].push_back(inst);
}