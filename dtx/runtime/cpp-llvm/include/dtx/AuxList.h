#pragma once

#include "dtx/Errors.h"
#include "dtx/NSObject.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace llvm::dtx {

enum class AuxType : uint32_t {
  ObjectOrNil = 2,
  I32 = 3,
  I64 = 4,
  U32 = 5,
  U64 = 6,
  Data = 10,
};

struct Arg {
  using Storage =
      std::variant<std::nullptr_t, int32_t, int64_t, uint32_t, uint64_t,
                   ns::Object>;
  Storage Value;

  Arg() : Value(nullptr) {}
  Arg(std::nullptr_t) : Value(nullptr) {}
  Arg(int32_t V) : Value(V) {}
  Arg(int64_t V) : Value(V) {}
  Arg(uint32_t V) : Value(V) {}
  Arg(uint64_t V) : Value(V) {}
  Arg(ns::Object V) : Value(std::move(V)) {}
};

class AuxList {
public:
  void append(Arg A) { Args_.push_back(std::move(A)); }
  const std::vector<Arg> &args() const { return Args_; }

  std::vector<uint8_t> encode() const;
  static Expected<AuxList> decode(const uint8_t *Data, size_t Size);

private:
  std::vector<Arg> Args_;
};

} // namespace llvm::dtx
