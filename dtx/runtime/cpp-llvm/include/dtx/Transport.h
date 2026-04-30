#pragma once

#include "dtx/Errors.h"

#include <cstddef>
#include <cstdint>

namespace llvm::dtx {

class Transport {
public:
  virtual ~Transport() = default;

  virtual Expected<size_t> read(uint8_t *Data, size_t Size) = 0;
  virtual Error write(const uint8_t *Data, size_t Size) = 0;
};

} // namespace llvm::dtx
