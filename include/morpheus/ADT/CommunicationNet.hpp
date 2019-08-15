
//===----------------------------------------------------------------------===//
//
// CommunicationNet
//
//===----------------------------------------------------------------------===//

#ifndef MRPH_COMM_NET_H
#define MRPH_COMM_NET_H

#include "llvm/ADT/BreadthFirstIterator.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include "morpheus/Utils.hpp"
#include "morpheus/Formats/Formatter.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

using namespace std;

namespace cn {
using namespace llvm;

enum EdgeType {
  SINGLE_HEADED,
  // just for input edges
  DOUBLE_HEADED,
  SINGLE_HEADED_RO,
  DOUBLE_HEADED_RO,
  SHUFFLE,
  SHUFFLE_RO,
};

enum EdgeCategory {
  REGULAR,
  CONTROL_FLOW,
};

template <typename T>
struct Printable {
  virtual ~Printable() = default;

  virtual void print(ostream &os, const formats::Formatter &fmt) const {
    fmt.format(os, static_cast<const T &>(*this));
  }
};


struct Identifiable {
  using ID = string;

  virtual ~Identifiable() = default;

  Identifiable() : id(generate_id()) { }
  Identifiable(const Identifiable &) = delete;
  Identifiable(Identifiable &&) = default;
  Identifiable& operator=(const Identifiable &) = delete;
  Identifiable& operator=(Identifiable &&) = default;

  ID get_id() const {
    return id;
  }

private:
  static ID generate_id();

  ID id;
};


struct Edge;

struct NetElement : public Identifiable, public Printable<NetElement> {

  virtual ~NetElement() = default;

  NetElement(string name) : name(name) { }
  NetElement(const NetElement &) = delete;
  NetElement(NetElement &&) = default;
  NetElement& operator=(const NetElement &) = delete;
  NetElement& operator=(NetElement &&) = default;

  virtual string get_element_type() const = 0;

  string name;
  vector<unique_ptr<Edge>> leads_to;

  // NOTE: non-owning pointers to edges that points to the element
  vector<Edge*> referenced_by;
};


struct Edge final : public Printable<Edge> {

  explicit Edge(NetElement &startpoint, NetElement &endpoint, string arc_expr,
                EdgeCategory category, EdgeType type)
    : startpoint(startpoint), endpoint(endpoint), arc_expr(arc_expr),
      category(category), type(type) { }
  Edge(const Edge &) = delete;
  Edge(Edge &&) = default;
  Edge& operator=(const Edge &) = delete;
  Edge& operator=(Edge &&) = default;

  inline EdgeCategory get_category() const { return category; }
  inline EdgeType     get_type()     const { return type; }

  NetElement &startpoint;
  NetElement &endpoint;
  string arc_expr;

private:
  EdgeCategory category;
  EdgeType type;
};


struct Place final : NetElement {

  explicit Place(string name, string type, string init_expr)
    : NetElement(name), type(type), init_expr(init_expr) { }
  Place(const Place &) = delete;
  Place(Place &&) = default;
  Place& operator=(const Place &) = delete;
  Place& operator=(Place &&) = default;

  string get_element_type() const {
    return "place_t";
  }

  string type;
  string init_expr;
};


using ConditionList = vector<string>;

struct Transition final : NetElement {

  explicit Transition(string name, const ConditionList guard)
    : NetElement(name), guard(guard) { }
  Transition(const Transition &) = delete;
  Transition(Transition &&) = default;
  Transition& operator=(const Transition &) = delete;
  Transition& operator=(Transition &&) = default;

  string get_element_type() const {
    return "transition_t";
  }

  ConditionList guard;
};


// -----------------------------------------------------------------------------
// Unresolved elements

class CommunicationNet;

struct IncompleteEdge final {
  NetElement *startpoint;
  NetElement *endpoint;
  string arc_expr = "";
  EdgeCategory category = REGULAR;
  EdgeType type = SINGLE_HEADED;
};

struct UnresolvedConnect final {

  using AccessCommPlaceFnTy = Place& (AddressableCN::*)();
  IncompleteEdge incomplete_edge;

  UnresolvedConnect() { }
  UnresolvedConnect(AddressableCN *acn) : acn_(acn) { }

  void close_connect(CommunicationNet &, AccessCommPlaceFnTy, string);

private:
  AddressableCN *acn_;
};

struct UnresolvedPlace final {

  using ResolveFnTy = function<void(CommunicationNet &cn, Place &, Transition &, UnresolvedConnect &)>;

  UnresolvedPlace(Place &place, const Value &mpi_rqst, ResolveFnTy resolve)
    : place(place),
      mpi_rqst(mpi_rqst),
      resolve(resolve) { }
  UnresolvedPlace(const UnresolvedPlace &) = delete;
  UnresolvedPlace(UnresolvedPlace &&) = default;
  UnresolvedPlace& operator=(const UnresolvedPlace &) = delete;
  UnresolvedPlace& operator=(UnresolvedPlace &&) = default;

  Place &place;
  const Value &mpi_rqst;
  ResolveFnTy resolve;
};

struct UnresolvedTransition final {

  UnresolvedTransition(Transition &transition, const Value &mpi_rqst)
    : transition(transition),
      mpi_rqst(mpi_rqst) { }
  UnresolvedTransition(const UnresolvedTransition &) = delete;
  UnresolvedTransition(UnresolvedTransition &&) = default;
  UnresolvedTransition& operator=(const UnresolvedTransition &) = delete;
  UnresolvedTransition& operator=(UnresolvedTransition &&) = default;

  Transition &transition;
  const Value &mpi_rqst;
  UnresolvedConnect unresolved_connect;
};


// ==========================================================
// CommunicationNet

struct AddressableCN;

class CommunicationNet : public Identifiable,
                         public Printable<CommunicationNet> {

  template <typename T>
  using Element = unique_ptr<T>;

  template <typename T>
  using Elements = vector<Element<T>>;

  template <typename T, typename... Args>
  Element<T> make_element_(Args&&... args) {
    return std::make_unique<T>(forward<Args>(args)...);
  }

public:
  // EdgePredicate is used to filter a category of edges
  template <EdgeCategory C>
  struct EdgePredicate {
    bool operator()(const Edge &e) const {
      return e.get_category() == C;
    }

    bool operator()(const Edge *e) const {
      return e->get_category() == C;
    }
  };

protected:
  // -------------------------------------------------------
  // removing methods
  using path_t = vector<const Edge *>;


  virtual bool remove(Place &p) {
    return remove_(p, places_);
  }

  virtual bool remove(Transition &t) {
    return remove_(t, transitions_);
  }

  bool remove(NetElement &elem);

  void remove_refs(const NetElement &elem) {
    for (const std::unique_ptr<Edge> &edge : elem.leads_to) {
      NetElement &pointed_elem = edge->endpoint;
      auto new_end = std::remove(pointed_elem.referenced_by.begin(),
                                 pointed_elem.referenced_by.end(),
                                 edge.get());

      auto old_end = pointed_elem.referenced_by.end();
      if (new_end != old_end) { // remove erase unspecified elements
        pointed_elem.referenced_by.erase(new_end, old_end);
      }
    }
  }

  void remove_edge(const Edge &edge) {
    NetElement &startpoint = edge.startpoint;
    NetElement &endpoint = edge.endpoint;

    { // remove the edge

      // As the edge has to be moved to be able to be passed into this function,
      // there is "invalid" pointer to it which has to be removed.
      auto it = std::find_if(startpoint.leads_to.begin(),
                             startpoint.leads_to.end(),
                             [&edge] (Element<Edge> &e) { return e.get() == &edge; });

      assert (it != startpoint.leads_to.end()
              && "Existing edge leaves invalid storage place in its starting point.");
      startpoint.leads_to.erase(it);
    }

    { // remove stored pointer to referenced edge
      auto it = std::find_if(endpoint.referenced_by.begin(),
                             endpoint.referenced_by.end(),
                             [&edge] (const Edge *e) { return e == &edge; });

      assert (it != endpoint.referenced_by.end()
              && "Existing edge is supposed to be referenced by endpoint.");
      endpoint.referenced_by.erase(it);
    }
  }

  void remove_path(path_t &path) {
    if (path.size() == 1) { // remove single edge path
      remove_edge(*path.back());
    } else if (path.size() > 1) { // remove path of consecutive elements

      // storage for nodes that are going to be removed (nodes on path)
      // exclude the first and the last nodes.
      std::vector<NetElement *> to_remove;
      to_remove.reserve(path.size() - 1);

      // remove edges
      for (size_t i = 0; i < path.size() - 1; i++) {
        const Edge *e1 = path[i];
        const Edge *e2 = path[i + 1];

        assert (&e1->endpoint == &e2->startpoint
                && "The path has to be composed of consecutive net elements.");

        // remove the first edge, the second one is needed for the following step
        to_remove.push_back(&e1->endpoint);

        remove_edge(*e1);
      }
      remove_edge(*path.back()); // remove the last remaining edge

      // remove the marked net elements (nodes)
      for (NetElement *ne : to_remove) {
        remove(*ne);
      }
    }
  }

  // -------------------------------------------------------
  // transitions of CN
  using color_t = unsigned short;
  using coloured_elem_t = pair<const NetElement *, color_t>;
  using elem_colors_t = map<const NetElement *, set<color_t>>;

  vector<path_t> backtrack_edge(const Edge &edge,
                                color_t color,
                                elem_colors_t &assigned_colors,
                                std::map<coloured_elem_t, path_t> &pbw_paths) const {

    const NetElement *startpoint = &edge.startpoint;
    const NetElement *endpoint = &edge.endpoint;
    auto pbw_path_it = pbw_paths.find({endpoint, color});

    assert(pbw_path_it != pbw_paths.end()
           && "The endpoint of the given edge has to already be among paths");
    const path_t &pbw_path = pbw_path_it->second;

    // prolong the partial path and store it to the starting point
    path_t e_pbw_path(pbw_path); // make a copy
    e_pbw_path.push_back(&edge); // prolong the path
    pbw_paths.insert({ {startpoint, color}, move(e_pbw_path) });

    // vector<const Edge *> unprocessed;
    const Edge *unprocessed_edge = nullptr;

    auto colors_it = assigned_colors.find(startpoint);
    if (colors_it == assigned_colors.end()) {
    // the element is colored for the first time

      // color the starting point
      assigned_colors.insert({startpoint, {color}});

      if (startpoint->referenced_by.size() == 1) {
        // process only further edges if there is no branching
        unprocessed_edge = startpoint->referenced_by.back();
      }
    } else {
    // the element was already colored by a color

      set<color_t> &used_colors = colors_it->second;
      used_colors.insert(color);

      if (used_colors.size() > 1) { // the inserted color might be the same one

        // two different colors means two independent parallel paths
        vector<path_t> prl_bw_paths; // parallel backward paths
        for (auto const& c : used_colors) {
          auto it = pbw_paths.find({startpoint, c});
          assert (it != pbw_paths.end());

          prl_bw_paths.push_back(it->second);
        }
        return prl_bw_paths;
      }
    }

    if (unprocessed_edge) {
      return backtrack_edge(*unprocessed_edge, color, assigned_colors, pbw_paths);
    }

    return {};
  }

  vector<path_t> backtrack_parallel_paths(const NetElement &elem) const {
    assert (elem.referenced_by.size() >= 2
            && "Backtracking of parallel paths works only on elements"
               " that are referenced at least two other nodes.");

    vector<path_t> resulting_paths;

    color_t color = 0;
    elem_colors_t assigned_colors;
    map<coloured_elem_t, path_t> pbw_paths; // partial backward paths

    for (const Edge *edge : elem.referenced_by) {
      color_t c = ++color;
      const NetElement *endpoint = &edge->endpoint;

      assigned_colors.insert({ endpoint, {c} });
      pbw_paths.insert({ {endpoint, c}, {} });

      vector<path_t> found_bw_paths = backtrack_edge(*edge, c, assigned_colors, pbw_paths);
      move(found_bw_paths.begin(),
           found_bw_paths.end(),
           back_inserter(resulting_paths));
    }
    return resulting_paths;
  }

  vector<path_t> find_parallel_paths(const vector<const NetElement *> &elements) {

    for (const NetElement *elem : elements) {
      if (elem->referenced_by.size() > 1) {
        vector<path_t> found_paths = backtrack_parallel_paths(*elem);
        if (!found_paths.empty()) {
          for (path_t &path : found_paths) {
            // reverse each path to change the backward storage
            reverse(path.begin(), path.end());
          }
          return found_paths;
        }
      }
    }

    return {};
  }

  void reduce_parallel_paths() {
    EdgePredicate<CONTROL_FLOW> is_cf;

    // put both places and transitions into one (NetElement*) vector
    vector<const NetElement*> elements;
    transform(places_.begin(), places_.end(), back_inserter(elements),
              [] (const Element<Place> &place) { return place.get(); });
    transform(transitions_.begin(), transitions_.end(), back_inserter(elements),
              [] (const Element<Transition> &transition) { return transition.get(); });

    // find and remove paths

    // NOTE: maybe it would help to topologically sort the nodes,
    //       but it is rather minor performance improvement.
    while (true) {
      path_t to_remove;
      vector<path_t> found_paths = find_parallel_paths(elements);
      for (path_t &p : found_paths) {
        if (all_of(p.begin(), p.end(), is_cf)) {
          swap(p, to_remove);
          break;
        }
      }

      if (to_remove.empty()) {
        // if no parallel edge is found break the cycle,
        // otherwise run the procedure on reduced graph
        break;

      } else {
        remove_path(to_remove);
      }
    }
  }

  bool is_collapsible(const Edge &e) const {
    EdgePredicate<CONTROL_FLOW> is_cf;

    if (e.startpoint.get_element_type() == e.endpoint.get_element_type() && is_cf(e)) {
      if (e.startpoint.get_element_type() == "place_t") {
        Place &p1 = static_cast<Place&>(e.startpoint);
        Place &p2 = static_cast<Place&>(e.endpoint);
        if (p1.type == p2.type) {
          return true;
        }
      } else {
        return true;
      }
    }
    return false;
  }

  void reconnect_to_endpoint(Edge &e) { // The endpoint of the edge remains preserved
    NetElement &startpoint = e.startpoint;
    remove_refs(startpoint); // NOTE: Remove the references stored within `referenced_by`
                             //       info at the startpoint as the startpoint is
                             //       not going to be preserved.

    for (Edge *ref_e : startpoint.referenced_by) {
      Elements<Edge> &owner_storage = ref_e->startpoint.leads_to;

      // find a unique pointer owning the reference pointer `ref_e`
      auto owner_it = std::find_if(owner_storage.begin(),
                             owner_storage.end(),
                             [ref_e](const Element<Edge> &o) { return o.get() == ref_e; });

      assert (owner_it != owner_storage.end() && "The owner has to exist!");

      Element<Edge> &edge = *owner_it;

      // create a new bypassing edge
      Element<Edge> new_edge = create_edge_(edge->startpoint, e.endpoint, edge->arc_expr,
                                            edge->get_category(), edge->get_type());

      // swap the new edge with the old one
      std::swap(edge, new_edge);
    }
  }

  void reconnect_to_startpoint(Edge &e) { // The startpoint of the edge remains preserved
    NetElement &endpoint = e.endpoint;
    for (Element<Edge> &edge : endpoint.leads_to) {
      Element<Edge> new_edge = create_edge_(e.startpoint, edge->endpoint, edge->arc_expr,
                                            edge->get_category(), edge->get_type());
      // NOTE: The new edges has to be added and the old one removed.
      //       The edges cannot be swapped as within reconnecting_topdown because there
      //       is no one-to-one matching. Single edge can be replaced by more edges.
      add_edge_(move(new_edge));
    }
    remove_edge(e);
  }

  template <typename T>
  void collapse_topdown(Elements<T> &elements,
                 CommunicationNet &tmp_cn,
                 Elements<T> CommunicationNet::*storage) {

    size_t idx = 0;
    while (idx < elements.size()) {
      Element<T> &elem = elements[idx];
      if (elem->leads_to.size() == 1) { // only one-path nodes can be collapsed
        Element<Edge> &e = elem->leads_to.back();
        if (is_collapsible(*e)) {
          reconnect_to_endpoint(*e);

          // skip the element, hence remove it
          idx++;
          continue;
        }
      }
      // takeover element
      tmp_cn.add_(move(elem), tmp_cn.*storage);
      idx++;
    }
  }

  template <typename T>
  void collapse_bottomup(Elements<T> &elements,
                         CommunicationNet &tmp_cn,
                         Elements<T> CommunicationNet::*storage) {

    size_t idx = 0;
    while (idx < elements.size()) {
      Element<T> &elem = elements[idx];
      if (elem->referenced_by.size() == 1) {
        Edge *e = elem->referenced_by.back();
        if (is_collapsible(*e)) {
          reconnect_to_startpoint(*e);

          // skip the element, and so remove it
          idx++;
          continue;
        }
      }
      tmp_cn.add_(move(elem), tmp_cn.*storage);
      idx++;
    }
  }

  // -------------------------------------------------------
  // CommunicationNet public API

public:
  virtual ~CommunicationNet() = default;

  CommunicationNet() = default;
  CommunicationNet(const CommunicationNet &) = delete;
  CommunicationNet(CommunicationNet &&) = default;
  CommunicationNet& operator=(const CommunicationNet &) = delete;
  CommunicationNet& operator=(CommunicationNet &&) = default;


  virtual void resolve_unresolved();
  virtual void collapse();
  virtual void takeover(CommunicationNet cn);

  virtual void clear() {
    places_.clear();
    transitions_.clear();
  }

  Place& add_place(string type, string init_expr, string name="") {
    return add_(make_element_<Place>(name, type, init_expr), places_);
  }

  Place& add_place(Element<Place> p) {
    return add_(move(p), places_);
  }

  Transition& add_transition(ConditionList cl, string name="") {
    return add_(make_element_<Transition>(name, cl), transitions_);
  }

  Transition& add_transition(Element<Transition> t) {
    return add_(move(t), transitions_);
  }

  template <typename Startpoint, typename Endpoint>
  inline Edge& add_edge(Startpoint &start, Endpoint &end, string ae,
                         EdgeCategory category, EdgeType type) {

    return add_edge_(create_edge_(start, end, ae, category, type));
  }

  Edge& add_input_edge(Place &src, Transition &dest, string ae="",
                       EdgeType type=SINGLE_HEADED) {
    return add_edge(src, dest, ae, REGULAR, type);
  }

  Edge& add_output_edge(Transition &src, Place &dest, string ae="") {
    return add_edge(src, dest, ae, REGULAR, SINGLE_HEADED);
  }

  template<typename Startpoint, typename Endpoint>
  Edge& add_cf_edge(Startpoint& src, Endpoint& dest) {
    return add_edge(src, dest, "", CONTROL_FLOW, SINGLE_HEADED);
  }

  UnresolvedPlace& add_unresolved_place(Place &place,
                                        const Value &mpi_rqst,
                                        UnresolvedPlace::ResolveFnTy resolve) {
    return add_(make_element_<UnresolvedPlace>(place, mpi_rqst, resolve), unresolved_places_);
  }

  UnresolvedTransition& add_unresolved_transition(Transition &transition,
                                                  const Value &mpi_rqst) {
    return add_(make_element_<UnresolvedTransition>(transition, mpi_rqst), unresolved_transitions_);
  }

  // -------------------------------------------------------
  // iterators

  iterator_range<typename Elements<Place>::iterator> places() {
    return make_range(places_.begin(), places_.end());
  }

  iterator_range<typename Elements<Place>::const_iterator> places() const {
    return make_range(places_.begin(), places_.end());
  }

  iterator_range<typename Elements<Transition>::iterator> transitions() {
    return make_range(transitions_.begin(), transitions_.end());
  }

  iterator_range<typename Elements<Transition>::const_iterator> transitions() const {
    return make_range(transitions_.begin(), transitions_.end());
  }

  iterator_range<typename Elements<UnresolvedPlace>::iterator> unresolved_places() {
    return make_range(unresolved_places_.begin(), unresolved_places_.end());
  }

  iterator_range<typename Elements<UnresolvedPlace>::const_iterator> unresolved_places() const {
    return make_range(unresolved_places_.begin(), unresolved_places_.end());
  }

  iterator_range<typename Elements<UnresolvedTransition>::iterator> unresolved_transitions() {
    return make_range(unresolved_transitions_.begin(), unresolved_transitions_.end());
  }

  iterator_range<typename Elements<UnresolvedTransition>::const_iterator>
  unresolved_transitions() const {
    return make_range(unresolved_transitions_.begin(), unresolved_transitions_.end());
  }

private:
  template <typename T>
  inline T& add_(Element<T> &&e, Elements<T> &elements) {
    elements.push_back(forward<Element<T>>(e));
    return *elements.back();
  }

  template <typename T>
  inline bool remove_(T &elem, Elements<T> &elements) {
    auto it = std::find_if(elements.begin(),
                           elements.end(),
                           [&elem] (const Element<T> &e) { return e.get() == &elem; });
    if (it != elements.end()) {
      remove_refs(**it);  // remove references to the node
      elements.erase(it); // remove the element itself
      return true;
    }
    return false;
  }

  template <typename Startpoint, typename Endpoint>
  inline Element<Edge> create_edge_(Startpoint &start, Endpoint &end, string ae,
                                    EdgeCategory category, EdgeType type) {

    Element<Edge> edge = make_element_<Edge>(start, end, ae, category, type);
    // the end element keeps the pointer to edge which is pointing to it
    end.referenced_by.push_back(edge.get());

    return edge;
  }

  inline Edge& add_edge_(Element<Edge> edge) {
    return add_(move(edge), edge->startpoint.leads_to);
  }

  template <typename T>
  inline void takeover_(Elements<T> &target, iterator_range<typename Elements<T>::iterator> src) {
    move(src.begin(), src.end(), back_inserter(target));
  }

  Elements<Place> places_;
  Elements<Transition> transitions_;

  Elements<UnresolvedPlace> unresolved_places_;
  Elements<UnresolvedTransition> unresolved_transitions_;

  friend AddressableCN; // make AddressableCN a friend class to override remove methods
};

// =============================================================================
// Addressable CN

struct AddressableCN final : public CommunicationNet {

  using Address = unsigned int;

  const Address address;
  Place &asr;
  Place &arr;
  Place &csr;
  Place &crr;
  // inner communication net used to keep away ACN elements from those of plugged nets
  CommunicationNet embedded_cn;

  ~AddressableCN() = default;

  AddressableCN(Address address)
    : address(address),
      asr(add_place("MessageToken", "", "ActiveSendRequest")),
      arr(add_place("MessageRequest", "", "ActiveReceiveRequest")),
      csr(add_place("MessageRequest", "", "CompletedSendRequest")),
      crr(add_place("MessageToken", "", "CompletedReceiveRequest")),
      embedded_cn(CommunicationNet()),
      entry_p_(&add_place("Unit", "", "ACN" + std::to_string(address) + "Entry" + get_id())),
      exit_p_(&add_place("Unit", "", "ACN" + std::to_string(address) + "Exit" + get_id())) { }

  AddressableCN(const AddressableCN &) = delete;
  AddressableCN(AddressableCN &&) = default;

  // NOTE: the print is redefined to force the formatter to use the right method
  void print(ostream &os, const formats::Formatter &fmt) const override {
    fmt.format(os, *this);
  }

  void collapse () override {
    embedded_cn.collapse();
  }

  void takeover (CommunicationNet cn) override {
    embedded_cn.takeover(move(cn));
  }

  void clear () override {
    CommunicationNet::clear();
    embedded_cn.clear();
  }

  // NOTE: the accessing methods are present in order to be able
  //       to access the references via pointer to members
  Place& get_active_send_request() { return asr; }
  Place& get_active_recv_request() { return arr; }
  Place& get_completed_send_request() { return csr; }
  Place& get_completed_recv_request() { return crr; }

  // ---------------------------------------------------------------------------
  // AddressableCN follows the interface of PluginCN in the sense that it has
  // also entry and exit places. But cannot be injected!

  Place& entry_place() { return *entry_p_; }
  Place& exit_place() { return *exit_p_; }

  void set_entry(Place &p) { entry_p_ = &p; }
  void set_exit(Place &p) { exit_p_ = &p; }

  void enclose() {
    // enclose the CN by connecting its entry & exit places
    add_cf_edge(entry_place(), exit_place());
  }

private:
  // override the protected methods to work correctly within the context of addressable CN
  bool remove(Place &p) override {
    return remove_(p);
  }

  bool remove(Transition &t) override {
    return remove_(t);
  }

  template <typename T>
  bool remove_(T &elem) {
    // first, it tries to remove the element in the addressable CN
    bool removed = CommunicationNet::remove(elem);
    if (!removed) {
      // if not successful it tries to remove it in the embedded CN
      return embedded_cn.remove(elem);
    }
    return removed;
  }

  Place *entry_p_;
  Place *exit_p_;
};


// =============================================================================
// Generic Plugin CN

class PluginCNGeneric {

public:
  ~PluginCNGeneric() = default;

  template <typename PluggableCN>
  PluginCNGeneric(PluggableCN &&net)
    : self_(std::make_unique<model<PluggableCN>>(forward<PluggableCN>(net))) { }

  PluginCNGeneric(const PluginCNGeneric &) = delete;
  PluginCNGeneric(PluginCNGeneric &&) = default;

  // ---------------------------------------------------------------------------
  // accessible methods of PluggableCNs

  void connect(AddressableCN &acn) {
    self_->connect_(acn);
  }

  template <typename PluggableCN>
  void inject_into(PluggableCN &pcn) {
    move(self_)->inject_into_(pcn);
    self_.release();
  }

  void add_cf_edge(NetElement& src, NetElement& dest) {
    self_->add_cf_edge_(src, dest);
  }

  void takeover(CommunicationNet cn) { self_->takeover_(move(cn)); }

  template <typename PluggableCN>
  void renounce_in_favor_of(PluggableCN &pcn) {
    move(self_)->renounce_in_favor_of_(pcn);
    self_.release();
  }

  Place& entry_place() { return self_->entry_place_(); }
  Place& exit_place() { return self_->exit_place_(); }

  void set_entry(Place &p) { self_->set_entry_(p); }
  void set_exit(Place &p) { self_->set_exit_(p); }

  void enclose() {self_->enclose_(); }

  void print(ostream &os, const formats::Formatter &fmt) const {
    self_->print_(os, fmt);
  }

  // ---------------------------------------------------------------------------
  // interface and model implementation of pluggable types

private:
  struct pluggable_t { // interface of pluggable CNs
    virtual ~pluggable_t() = default;

    virtual void takeover_(CommunicationNet cn) = 0;
    virtual void renounce_in_favor_of_(CommunicationNet &cn) = 0;
    virtual void renounce_in_favor_of_(PluginCNGeneric &cn) = 0;
    virtual void connect_(AddressableCN &) = 0;
    virtual void inject_into_(AddressableCN &) = 0;
    virtual void inject_into_(PluginCNGeneric &) = 0;
    virtual void add_cf_edge_(NetElement &src, NetElement &dest) = 0;
    virtual Place& entry_place_() = 0;
    virtual Place& exit_place_() = 0;
    virtual void set_entry_(Place &) = 0;
    virtual void set_exit_(Place &) = 0;
    virtual void enclose_() = 0;
    virtual void print_(ostream &os, const formats::Formatter &fmt) const = 0;
  };

  template <typename PluggableCN>
  struct model final : pluggable_t {
    model(PluggableCN &&pcn) : pcn_(forward<PluggableCN>(pcn)) { }

    void takeover_(CommunicationNet cn) override {
      pcn_.takeover(move(cn));
    }

    void renounce_in_favor_of_(CommunicationNet &cn) override {
      cn.takeover(move(pcn_));
    }

    void renounce_in_favor_of_(PluginCNGeneric &pcn) override {
      pcn.takeover(move(pcn_));
    }

    void connect_(AddressableCN &acn) override {
      pcn_.connect(acn);
    }

    void inject_into_(AddressableCN &acn) override {
      move(pcn_).inject_into(acn);
    }

    void inject_into_(PluginCNGeneric &pcn) override {
      move(pcn_).inject_into(pcn);
    }

    void add_cf_edge_(NetElement &src, NetElement &dest) override {
      pcn_.add_cf_edge(src, dest);
    }

    Place& entry_place_() override {
      return pcn_.entry_place();
    }

    Place& exit_place_() override {
      return pcn_.exit_place();
    }

    void set_entry_(Place &p) override {
      pcn_.set_entry(p);
    }

    void set_exit_(Place &p) override {
      pcn_.set_exit(p);
    }

    void enclose_() override {
      pcn_.enclose();
    }

    void print_(ostream &os, const formats::Formatter &fmt) const override {
      pcn_.print(os, fmt);
    }

    PluggableCN pcn_;
  };

  unique_ptr<pluggable_t> self_;
};


// =============================================================================
// Base Plugin CN

class PluginCNBase : public CommunicationNet {
public:
  virtual ~PluginCNBase() = default;

  PluginCNBase()
    : entry_p_(&add_place("Unit", "", "entry" + get_id())),
      exit_p_(&add_place("Unit", "", "exit" + get_id())) { }
  PluginCNBase(const PluginCNBase &) = delete;
  PluginCNBase(PluginCNBase &&) = default;

  virtual void connect(AddressableCN &acn) = 0;

  template <typename PluggableCN>
  void inject_into(PluggableCN &) & {
    assert(false && "Only an r-value reference can be injected into a PluggableCN\n");
  }

  void inject_into(AddressableCN &acn) && {
    connect(acn);
    plug_in_(acn);
  }

  void inject_into(PluginCNGeneric &pcn) && {
    plug_in_(pcn);
  }

  Place& entry_place() { return *entry_p_; }
  Place& exit_place() { return *exit_p_; }

  void set_entry(Place &p) { entry_p_ = &p; }
  void set_exit(Place &p) { exit_p_ = &p; }

  void enclose() {
    // enclose the CN by connecting its entry & exit places
    add_cf_edge(entry_place(), exit_place());
  }

private:
  template <typename PluggableCN>
  void plug_in_(PluggableCN &pcn) {
    // join entry & exit places
    pcn.add_cf_edge(pcn.entry_place(), entry_place());

    // set the new entry as the exit of injected net
    pcn.set_entry(exit_place());

    // pass over the elements into `pcn`
    pcn.takeover(move(*this));
  }

  Place *entry_p_;
  Place *exit_p_;
};


// =============================================================================
// BasicBlockCN

struct BasicBlockCN final : public PluginCNBase {

  BasicBlock const *bb;

  ~BasicBlockCN() = default;

  BasicBlockCN(BasicBlock const *bb) : bb(bb) {

    string str;
    raw_string_ostream rso(str);
    bb->printAsOperand(rso, false);
    string bb_name = " " + rso.str();

    entry_place().name += bb_name;
    exit_place().name += bb_name;
  }

  BasicBlockCN(const BasicBlockCN &) = delete;
  BasicBlockCN(BasicBlockCN &&) = default;

  void connect(AddressableCN &acn) override {
    // connect all the stored pcns separately
    for (PluginCNGeneric &pcn : stored_pcns_) {
      pcn.connect(acn);
    }
  }

  // NOTE: redefine injection function to call a different (local) `plug_in_` method.
  void inject_into(PluginCNGeneric &pcn) && {
    plug_in_(pcn);
  }

  void inject_into(AddressableCN &acn) && {
    connect(acn);
    plug_in_(acn);
  }

  // NOTE: As the BasicBlockCN is only an envelope for inner CNs
  //       it needs to keep them separate. Therefore these are added only,
  //       and not injected directly.
  void add_pcn(PluginCNGeneric pcn) {
    // join newly added pcn
    add_cf_edge(entry_place(), pcn.entry_place());

    // move the entry point to the exit point of added pcn
    set_entry(pcn.exit_place());

    // store the pcn
    stored_pcns_.push_back(move(pcn));
  }

private:
  template <typename PluggableCN>
  void plug_in_(PluggableCN &pcn) {
    for (PluginCNGeneric &stored_pcn : stored_pcns_) {
      // NOTE: all the connections has already been done, hence `acn`
      //       only takeover the elements of `pcn`. In other words,
      //       `pcn` renounces its elements in favor of the `acn`.

      move(stored_pcn).renounce_in_favor_of(pcn);
      // NOTE: it is the same as: `pcn.takeover(move(stored_pcn))`
      //       that is not directly supported with PluginCNGeneric
      //       as the actual CN is hidden inside.
    }
    pcn.takeover(move(*this));
  }

  vector<PluginCNGeneric> stored_pcns_;
};


// =============================================================================
// CFG_CN

struct CFG_CN final : public PluginCNBase {

  const Function &fn;
  const LoopInfo &loop_info;
  vector<BasicBlockCN> bb_cns;

  ~CFG_CN() = default;

  CFG_CN(const Function &fn, LoopInfo &loop_info) : fn(fn), loop_info(loop_info) {
    string str;
    raw_string_ostream rso(str);
    fn.printAsOperand(rso, false);
    string fn_name = " " + rso.str();

    entry_place().name += fn_name;
    exit_place().name += fn_name;

    // create the basic structure
    auto bfs_it = breadth_first(&fn);
    transform(bfs_it.begin(), bfs_it.end(),
              back_inserter(bb_cns),
              [] (const BasicBlock *bb) { return cn::BasicBlockCN(bb); });

    interconnect_basicblock_cns();

    expose_loops();
  }

  CFG_CN(const CFG_CN &) = delete;
  CFG_CN(CFG_CN &&) = default;

  void connect (AddressableCN &acn) {
    for (BasicBlockCN &bbcn : bb_cns) {
      bbcn.connect(acn);
    }
  }

  template<typename PluggableCN>
  void inject_into(PluggableCN &pcn) && {
    // NOTE: as the implementation is the same for both type of injections (PluginCNGeneric &
    //       AddressableCN) the method is tempalted. Moreover, `connect` is called separately
    //       within injections of particular BasicBlockCNs.

    for (BasicBlockCN &bbcn : bb_cns) {
      move(bbcn).inject_into(pcn);
    }

    add_cf_edge(pcn.entry_place(), entry_place());

    pcn.set_entry(exit_place());

    pcn.takeover(move(*this));
  }

private:
  void interconnect_basicblock_cns() {
    // TODO: think about a bit more optimal solution

    for (BasicBlockCN &bbcn : bb_cns) {
      const BasicBlock *bb = bbcn.bb;

      for (const BasicBlock *pred_bb : predecessors(bb)) {
        std::for_each(bb_cns.begin(),
                      bb_cns.end(),
                      [pred_bb, &bbcn] (BasicBlockCN &bbcn_) {
                        if (pred_bb == bbcn_.bb) {
                          bbcn_.add_cf_edge(bbcn_.exit_place(), bbcn.entry_place());
                        }
                      });
      }
    }

    // join the inner structure to entry and exit places
    add_cf_edge(entry_place(), get_entry_bb().entry_place());

    for (BasicBlockCN *exit_cn : get_exit_bbs()) {
      add_cf_edge(exit_cn->exit_place(), exit_place());
    }
  }

  BasicBlockCN& get_entry_bb() {
    // as the basic blocks are stored in bfs manner, the first one is the entry one.
    BasicBlockCN& entry_bb = bb_cns.front();
    auto it = predecessors(entry_bb.bb);
    assert(it.begin() == it.end() && "Entry basic block cannot have predecessor!");
    return entry_bb;
  }

  vector<BasicBlockCN*> get_exit_bbs() {
    // there can be more than one exit blocks
    vector<BasicBlockCN*> exit_bbs;

    auto i = bb_cns.begin();
    auto end = bb_cns.end();
    while (i != end) {
      i = find_if(i, end,
                  [] (const BasicBlockCN &bbcn) {
                    auto it = successors(bbcn.bb);
                    // return true only when no successor of the inner basic block
                    return it.begin() == it.end();
                  });

      if (i != end) {
        exit_bbs.push_back(&*i);
        i++;
      }
    }

    return exit_bbs;
  }

  void update_loop_branch(BasicBlock &loop_branch, string trigger_input_expr) {
    auto found_bbcn_it = find_if(
      bb_cns.begin(), bb_cns.end(),
      [&loop_branch] (const BasicBlockCN &bbcn) { return &loop_branch == bbcn.bb; });

    assert(found_bbcn_it != bb_cns.end() &&
           "There has to exists the corresponding BasicBlockCN for the given BasicBlock.");

    BasicBlockCN &bbcn = *found_bbcn_it;

    // change the entry place of loop branch
    // this will collapse with exit place of header loop
    Place &entry_p = bbcn.entry_place();
    std::string entry_name = entry_p.name;

    entry_p.type = "Bool";
    entry_p.name = "";

    // trigger the loop branch
    Transition &trigger_branch = bbcn.add_transition(ConditionList());
    bbcn.add_input_edge(entry_p, trigger_branch, trigger_input_expr);

    // add a new Unit place representing the new entry place
    Place &new_entry = bbcn.add_place("Unit", "", entry_name);
    bbcn.add_cf_edge(trigger_branch, new_entry);
    bbcn.set_entry(new_entry);
  }

  void expose_loops() {
    // NOTE: expose the loops in a way that loop header
    //       is represented by a Boolean place, and each
    //       branch is triggered by a transition
    for (BasicBlockCN &bbcn : bb_cns) {
      auto *loop = loop_info.getLoopFor(bbcn.bb);
      if (loop && loop->getHeader() == bbcn.bb) {

        // change the type of exit place as the loop header
        // represents the condition in the CFG structure
        Place &exit_p = bbcn.exit_place();
        exit_p.type = "Bool";
        exit_p.name = "test_loop " + bbcn.get_id();

        // Body of the loop
        auto *loop_latch = loop->getLoopLatch();
        if (loop_latch) {
          update_loop_branch(*loop_latch, "true");
        }

        // Exit branch of the loop
        auto *loop_exit = loop->getExitBlock();
        if (loop_exit) {
          update_loop_branch(*loop_exit, "false");
        }
      }
    }
  }
};

} // end of anonymous namespace

#endif // MRPH_COMM_NET_H
