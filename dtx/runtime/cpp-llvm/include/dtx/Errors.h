#pragma once

#include <string>

namespace llvm::dtx {

class Error {
public:
  Error() = default;
  explicit Error(std::string Message) : Message_(std::move(Message)) {}

  explicit operator bool() const { return !Message_.empty(); }
  const std::string &message() const { return Message_; }

  static Error success() { return Error(); }

private:
  std::string Message_;
};

template <typename T> class Expected {
public:
  Expected(T Value) : HasValue_(true), Value_(std::move(Value)) {}
  Expected(Error Err) : HasValue_(false), Err_(std::move(Err)) {}

  explicit operator bool() const { return HasValue_; }
  T &get() { return Value_; }
  const T &get() const { return Value_; }
  const Error &error() const { return Err_; }

private:
  bool HasValue_ = false;
  T Value_{};
  Error Err_;
};

} // namespace llvm::dtx
