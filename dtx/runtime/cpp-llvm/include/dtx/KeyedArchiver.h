#pragma once

#include "dtx/Errors.h"
#include "dtx/NSObject.h"

#include <cstdint>
#include <vector>

namespace llvm::dtx::ns {

// MVP object archive used by the DTX aux layer.
//
// The public API intentionally mirrors the future NSKeyedArchive layer. The
// current byte format is a deterministic project-local binary object encoding;
// it keeps object framing isolated so it can be replaced by full
// NSKeyedArchiver/bplist compatibility without touching AuxList or codegen.
class KeyedArchiver {
public:
  static std::vector<uint8_t> archiveRoot(const Object &Root);
  static llvm::dtx::Expected<Object> unarchiveRoot(const uint8_t *Data,
                                                   size_t Size);
};

} // namespace llvm::dtx::ns
