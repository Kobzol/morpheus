
#include "CallFinder.hpp"
#include "morpheus/Analysis/MPIScopeAnalysis.hpp"

#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <string>

using namespace llvm;
using namespace std;

// NOTE: Morpheus -> mor, mrp, mrh ... posible prefixes

namespace {
  template<typename IRUnitT>
  CallInst* find_call_in_by_name(const string &name, IRUnitT &unit) {
    vector<CallInst*> found_calls = CallFinder<IRUnitT>::find_in(name, unit);
    // it is used to find MPI_Init and MPI_Finalize calls,
    // these cannot be called more than once.
    assert (found_calls.size() <= 1);
    if (found_calls.size() > 0) {
      return found_calls.front();
    }
    return nullptr;
  }

  // TODO: new interface should look like: (4.3.19: does it?)
  /*
    MPIInit_Call find_mpi_init(IRUnitT &unit); // this function can internally call "find_call_in_by_name" ...
   */
}

// The analysis runs over a module and tries to find a function that covers
// the communication, either directly (MPI_Init/Finalize calls) or mediately
// (via functions that calls MPI_Initi/Finalize within)
MPIScopeAnalysis::Result
MPIScopeAnalysis::run(Module &m, ModuleAnalysisManager &am) {

  string init_f_name = "MPI_Init";
  string finalize_f_name = "MPI_Finalize";

  // ... it should be enough to have a parent for each call

  // pair<CallInst *, CallInst *> caller_callee; // NOTE: to have an info who and where the callee was called

  CallInst *mpi_init, *mpi_finalize; // The exact calls of MPI_Init/Finalize calls
  CallInst *init_f, *finalize_f;

  do {
    for (auto &f : m) {
      init_f = find_call_in_by_name(init_f_name, f);
      finalize_f = find_call_in_by_name(finalize_f_name, f);

      // Store mpi initi/finalize if found
      // NOTE: the first appearance, i.e., null mpi_init,
      //       means that init_f represents MPI_Init call.
      if (!mpi_init && init_f) {
        mpi_init = init_f;
      }

      if (!mpi_finalize && finalize_f) {
        mpi_finalize = finalize_f;
      }

      // Looking for calls within one function that define the mpi scope
      if (init_f && finalize_f) {
        return Result(&f, init_f, finalize_f, mpi_init, mpi_finalize);
      }

      // If neither init_f nor finalize_f happened,
      // then either 'init_f' or 'finalize_f' might happened.
      if (init_f) {
        StringRef f_name = f.getName();
        assert(!f_name.empty()); // NOTE: called function has to have a name (actually it is not true but for now)
        init_f_name = f_name;
      } else if (finalize_f) {
        StringRef f_name = f.getName();
        assert(!f_name.empty());
        finalize_f_name = f_name;
      }
    } // end for
  } while(init_f || finalize_f); // continue search if at least one of the init/finalize call has been found.

  return Result(); // return empty scope; MPI is not involved at all within the module
}

// provide definition of the analysis Key
AnalysisKey MPIScopeAnalysis::Key;
