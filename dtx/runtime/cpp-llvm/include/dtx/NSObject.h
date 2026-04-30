#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace llvm::dtx::ns {

struct Object;

using Array = std::vector<Object>;
using Dict = std::map<std::string, Object>;
using Data = std::vector<uint8_t>;

struct Object {
  using Storage = std::variant<std::nullptr_t, bool, int64_t, uint64_t, double,
                               std::string, Data, Array, Dict>;

  Storage Value;

  Object() : Value(nullptr) {}
  Object(std::nullptr_t) : Value(nullptr) {}
  Object(bool V) : Value(V) {}
  Object(int64_t V) : Value(V) {}
  Object(uint64_t V) : Value(V) {}
  Object(double V) : Value(V) {}
  Object(std::string V) : Value(std::move(V)) {}
  Object(const char *V) : Value(std::string(V)) {}
  Object(Data V) : Value(std::move(V)) {}
  Object(Array V) : Value(std::move(V)) {}
  Object(Dict V) : Value(std::move(V)) {}

  bool isNull() const { return std::holds_alternative<std::nullptr_t>(Value); }
  std::string dump() const;
};

bool operator==(const Object &LHS, const Object &RHS);
bool operator!=(const Object &LHS, const Object &RHS);

} // namespace llvm::dtx::ns
