#pragma once

#include "dtx/AuxList.h"
#include "dtx/NSObject.h"
#include "dtx/Transport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm::dtx {

class Connection {
public:
  Connection() = default;
  explicit Connection(std::unique_ptr<Transport> Transport);
  virtual ~Connection() = default;

  virtual Expected<ns::Object> call(uint32_t Channel, const std::string &Selector,
                                    const AuxList &Args,
                                    bool ExpectsReply = true);

private:
  Expected<std::vector<uint8_t>> readExact(size_t Size);
  Error writeAll(const std::vector<uint8_t> &Bytes);

  std::unique_ptr<Transport> Transport_;
  uint32_t NextMessageId_ = 0;
};

} // namespace llvm::dtx
