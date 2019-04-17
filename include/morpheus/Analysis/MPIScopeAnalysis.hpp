
//===----------------------------------------------------------------------===//
//
// MPIScopeAnalysis
//
// Description of the file ... basic idea to provide a scope of elements
// that are involved in MPI either directly or indirectly
//
//===----------------------------------------------------------------------===//

#ifndef MR_MPI_SCOPE_H
#define MR_MPI_SCOPE_H

#include "llvm/ADT/ilist_iterator.h"
#include "llvm/ADT/simple_ilist.h"
#include "llvm/IR/PassManager.h"

#include <vector>
#include <string>
#include <iterator>

namespace llvm {

  class ScopeIterator
    : public std::iterator<std::forward_iterator_tag, Instruction> { // TODO: the iterator has to be self contained

    // Note: I "cannot use" defined iterators, just because I'm going throw
    // more Basicblocks. In fact, I'm writing something like flat iterator.

    using OptionsT = typename ilist_detail::compute_node_options<Instruction>::type;
    ilist_sentinel<OptionsT> Sentinel;

    using iterator = ilist_iterator<OptionsT, false, false>;
    using const_iterator = ilist_iterator<OptionsT, false, true>;

    Instruction *begin_inst;
    Instruction *end_inst;

    iterator iter;

  public:

    explicit ScopeIterator()
      : begin_inst(nullptr), end_inst(nullptr),
        iter(iterator(Sentinel)) { }

    ~ScopeIterator() = default;

    explicit ScopeIterator(Instruction *begin, Instruction *end)
      : begin_inst(begin), end_inst(end),
        iter(iterator(Sentinel)) { }

    ScopeIterator(const ScopeIterator &sci) = default;

    ScopeIterator& operator=(const ScopeIterator& sci) = default;

    ScopeIterator begin() {
      iter = iterator(begin_inst);
      return *this;
    }

    ScopeIterator end() {
      iter = iterator(end_inst);
      return *this;
    }

    ScopeIterator& operator++() {
      BasicBlock *parent_bb = (*iter).getParent();
      Instruction *term_inst = parent_bb->getTerminator();
      if (&*iter != term_inst) {
        iter++;
      } else {
        BasicBlock *next_bb = parent_bb->getNextNode();
        iter = next_bb->begin();
      }
      return *this;

      // auto *terminator = current->getParent()->getTerminator();
      // if (&*current == terminator) {
      //   auto *p = current->getParent();
      //   auto *node = p->getNextNode();
      //   errs() << "Parent: " << p << "\n";
      //   errs() << "Node: " << node << "\n";

      //   if (!p || !node) { // TODO: is it possible to have nullptr parent?
      //     errs() << ">>>error<<< " << p << " -- " << node << "\n";
      //     ScopeIterator end_it = end();
      //     return end_it;
      //   }
      //   current = node->begin();
      //   errs() << "Curent: " << *current << "\n";
      //   // errs() << "Parrent: " << *current << "\n";
      //   // ScopeIterator end_it = end();
      //   // return end_it;
      //   // then go up and continue from there.
      //   errs() << "\n";
      // }
      // return *this;
    }

    ScopeIterator operator++(int) {
      ScopeIterator tmp = *this;
      ++*this;
      return tmp;
    }

    Instruction& operator*() const {
      return *iter;
    }

    Instruction* operator->() const {
      return &operator*();
    }

    bool operator==(const ScopeIterator &other) const {
      return (begin_inst == other.begin_inst &&
              end_inst == other.end_inst &&
              &*iter == &*(other.iter));
    }

    bool operator!=(const ScopeIterator &other) const {
      return !operator==(other);
    }
  }; // ScopeIterator


  // TODO: does it make sense to implement ilist_node_with_parent (as same as basic block?)
  class MPIScope {

    Module &analyzed_m;
    std::unique_ptr<CallGraph> cg;
    std::unique_ptr<MPILabeling> labelling;

  public:
    using iterator = ScopeIterator;

    explicit MPIScope(Module &m);
    MPIScope(const MPIScope &scope);
    MPIScope(MPIScope &&scope) = default;

    // TODO: review
    ScopeIterator begin() {
      return ScopeIterator(init_call, finalize_call).begin();
    }

    ScopeIterator end()   {
      return ScopeIterator(init_call, finalize_call).end();
    }

    // ScopeIterator::const_iterator begin() const { iter.begin(); }
    // ScopeIterator::const_iterator end() const { iter.end(); }

    bool isValid();

    // NOTE:
    // Well, in the end I need a kind of navigation trough the scope. Therefore, a set of queries needs to be defined.

    // TODO: define it as an iterator over "unfolded" scope
  }; // MPIScope


  class MPIScopeAnalysis : public AnalysisInfoMixin<MPIScopeAnalysis> {
    static AnalysisKey Key;
    friend AnalysisInfoMixin<MPIScopeAnalysis>;

  public:

    using Result = MPIScopeResult;

    MPIScopeResult run (Module &, ModuleAnalysisManager &);
  }; // MPIScopeAnalysis
} // llvm

#endif // MR_MPI_SCOPE_H
