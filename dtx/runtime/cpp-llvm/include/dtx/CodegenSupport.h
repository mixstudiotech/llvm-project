#pragma once

#include "dtx/AuxList.h"
#include "dtx/Connection.h"
#include "dtx/Errors.h"
#include "dtx/NSObject.h"

#include <cstdint>
#include <string>

namespace llvm::dtx {

class ArgWriter {
public:
  void write(int32_t V) { Aux_.append(Arg(V)); }
  void write(int64_t V) { Aux_.append(Arg(V)); }
  void write(uint32_t V) { Aux_.append(Arg(V)); }
  void write(uint64_t V) { Aux_.append(Arg(V)); }
  void write(const std::string &V) { Aux_.append(Arg(ns::Object(V))); }
  void write(const ns::Object &V) { Aux_.append(Arg(V)); }

  const AuxList &aux() const { return Aux_; }

private:
  AuxList Aux_;
};

class ArgReader {
public:
  explicit ArgReader(const AuxList &Aux) : Args_(Aux.args()) {}

  size_t size() const { return Args_.size(); }

  Expected<ns::Object> readObject(size_t Index) const {
    if (Index >= Args_.size())
      return Error("missing DTX argument");
    if (const auto *Object = std::get_if<ns::Object>(&Args_[Index].Value))
      return *Object;
    return Error("DTX argument is not an object");
  }

private:
  const std::vector<Arg> &Args_;
};

} // namespace llvm::dtx
