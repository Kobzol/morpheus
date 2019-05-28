
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/Support/CommandLine.h"

#include "morpheus/Analysis/MPILabellingAnalysis.hpp"
#include "morpheus/Analysis/MPIScopeAnalysis.hpp"
#include "morpheus/Transforms/MPISubstituteRank.hpp"

#include <algorithm>

using namespace llvm;

// -------------------------------------------------------------------------- //
// MPISubstitueRankPass

static cl::opt<unsigned> rank_arg(
    "rank", cl::Required, cl::Hidden,
    cl::desc("An unsigned integer specifying rank of interest."));

PreservedAnalyses MPISubstituteRankPass::run (Module &m, ModuleAnalysisManager &am) {

  am.registerPass([] { return MPILabellingAnalysis(); });

  MPILabelling &mpi_labelling = am.getResult<MPILabellingAnalysis>(m);

  std::vector<Instruction *> comm_ranks = mpi_labelling.get_calls("MPI_Comm_rank");

  // replace all usages of rank by constant value
  for (Instruction *comm_rank : comm_ranks) {
    CallSite cs_comm_rank(comm_rank);
    Value *rank_val = cs_comm_rank.getArgument(1);

    IRBuilder<> builder(comm_rank);
    ConstantInt *const_rank = builder.getInt32(rank_arg);

    // NOTE: copy users to avoid iterator invalidation during replaces
    std::vector<User *> users;
    std::copy(rank_val->user_begin(), rank_val->user_end(), std::back_inserter(users));

    for (auto *user : users) {
      if (isa<LoadInst>(user)) {
        // replace all loads from rank by const value
        user->replaceAllUsesWith(const_rank);
      }
    }
  }

  // TODO (28.5.19): I do not prone the code just replace constants.
  //                 -> update the following comment
  // pruning code causes that all analyses are invalidated
  return PreservedAnalyses::none();
}
