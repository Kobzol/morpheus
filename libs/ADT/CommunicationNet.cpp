
#include "morpheus/ADT/CommunicationNet.hpp"
#include "morpheus/Formats/PlainText.hpp"

#include <algorithm>
#include <sstream>

namespace cn {

  using namespace llvm;

  Identifiable::ID Identifiable::generate_id() {
    static unsigned int id = 0;
    return std::to_string(++id);
  }

  // ---------------------------------------------------------------------------
  // CommunicationNet

  // # protected
  bool CommunicationNet::remove(NetElement &elem) {
    if (elem.get_element_type() == "place_t") {
      return remove(static_cast<Place &>(elem));
    } else if (elem.get_element_type() == "transition_t") {
      return remove(static_cast<Transition &>(elem));
    } else {
      assert(false && "Unknown type of the net element.");
    }
    return false;
  }

  // + public methods
  void CommunicationNet::resolve_unresolved() {
    std::vector<UnresolvedPlace *> to_remove;

    // match unresolved places with unresolved transitions
    for (auto up_it = unresolved_places_.begin();
         up_it != unresolved_places_.end();
         up_it++) {

      auto &up = *up_it;
      auto matched_ut_it = std::find_if(
        unresolved_transitions_.begin(), unresolved_transitions_.end(),
        [&up](const auto &ut) { return &up->mpi_rqst == &ut->mpi_rqst; });
      if (matched_ut_it != unresolved_transitions_.end()) {
        auto &ut = *matched_ut_it;
        up->resolve(*this, up->place, ut->transition, ut->unresolved_connect);
        unresolved_transitions_.erase(matched_ut_it);
        to_remove.push_back(up.get());
      }
    }

    // remove resolved places
    auto remove_from_it = unresolved_places_.end();
    while (!to_remove.empty()) {
      UnresolvedPlace *up_rm = to_remove.back();
      to_remove.pop_back();
      remove_from_it = std::remove_if(unresolved_places_.begin(), remove_from_it,
                                      [up_rm](const auto &up) { return up.get() == up_rm; });
    }
    unresolved_places_.erase(remove_from_it, unresolved_places_.end());

    for (const auto &up : unresolved_places()) {
      errs() << "up: " << up->place.name << "\n";
      errs() << "  - " << up->mpi_rqst << "\n";
    }

    for (const auto &ut : unresolved_transitions()) {
      errs() << "ut: " << ut->transition.name << "\n";
      errs() << "  - " << ut->mpi_rqst << "\n";
    }
  }

  void CommunicationNet::collapse() {
    CommunicationNet tmp_cn;
    collapse_topdown(places_, tmp_cn, &CommunicationNet::places_);
    collapse_topdown(transitions_, tmp_cn, &CommunicationNet::transitions_);
    std::swap(tmp_cn, *this);

    tmp_cn.clear();
    collapse_bottomup(places_, tmp_cn, &CommunicationNet::places_);
    collapse_bottomup(transitions_, tmp_cn, &CommunicationNet::transitions_);
    std::swap(tmp_cn, *this);

    reduce_parallel_paths();
  }

  void CommunicationNet::takeover(CommunicationNet cn) {
    takeover_(places_, cn.places());
    takeover_(transitions_, cn.transitions());
    takeover_(unresolved_places_, cn.unresolved_places());
    takeover_(unresolved_transitions_, cn.unresolved_transitions());
  }

} // end of communication net (cn) namespace

