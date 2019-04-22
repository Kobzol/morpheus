
//===----------------------------------------------------------------------===//
//
// ParentPointerNode
//
//===----------------------------------------------------------------------===//

#ifndef MRPH_PPNODE
#define MRPH_PPNODE

#include <memory>
#include "llvm/Support/raw_ostream.h"

template<typename T>
class PPNode {
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &, const PPNode<T> &);

public:
  T data;
  std::shared_ptr<PPNode<T>> parent;

  explicit PPNode(const T &data) : data(data) { }
  PPNode(PPNode &&node) = default;
  PPNode() = default;
  PPNode(const PPNode &node) = delete;

  template<typename... ArgsT>
  static std::shared_ptr<PPNode<T>> create(ArgsT&&... args) {
    return std::make_shared<PPNode<T>>(std::forward<ArgsT>(args)...);
  }
};

namespace llvm {

  template<typename T>
  raw_ostream &operator<< (raw_ostream &out, const PPNode<T> &node) {
    out << "Node(" << node.data << ")";
    if (node.parent) {
      out << " -> " << *node.parent;
    } else {
      out << ";";
    }
    return out;
  }
}
#endif // MMRPH_PPNODE
