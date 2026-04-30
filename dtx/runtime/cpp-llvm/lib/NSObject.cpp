#include "dtx/NSObject.h"

#include <sstream>

namespace llvm::dtx::ns {

namespace {
struct DumpVisitor {
  std::string operator()(std::nullptr_t) const { return "null"; }
  std::string operator()(bool V) const { return V ? "true" : "false"; }
  std::string operator()(int64_t V) const { return std::to_string(V); }
  std::string operator()(uint64_t V) const { return std::to_string(V); }
  std::string operator()(double V) const {
    std::ostringstream OS;
    OS << V;
    return OS.str();
  }
  std::string operator()(const std::string &V) const { return '"' + V + '"'; }
  std::string operator()(const Data &V) const {
    return "<data:" + std::to_string(V.size()) + ">";
  }
  std::string operator()(const Array &V) const {
    std::string Out = "[";
    for (size_t I = 0; I < V.size(); ++I) {
      if (I)
        Out += ", ";
      Out += V[I].dump();
    }
    Out += "]";
    return Out;
  }
  std::string operator()(const Dict &V) const {
    std::string Out = "{";
    bool First = true;
    for (const auto &Entry : V) {
      if (!First)
        Out += ", ";
      First = false;
      Out += Entry.first + ": " + Entry.second.dump();
    }
    Out += "}";
    return Out;
  }
};
} // namespace

std::string Object::dump() const { return std::visit(DumpVisitor{}, Value); }

bool operator==(const Object &LHS, const Object &RHS) {
  return LHS.Value == RHS.Value;
}

bool operator!=(const Object &LHS, const Object &RHS) { return !(LHS == RHS); }

} // namespace llvm::dtx::ns
