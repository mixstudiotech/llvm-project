#pragma once

#include "dtx/Fragment.h"
#include "dtx/NSObject.h"
#include "dtx/Transport.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace llvm::dtx {

using ServerDispatch =
    std::function<Expected<ns::Object>(uint32_t Channel, const std::string &Selector,
                                       const AuxList &Args)>;

class ServerSession {
public:
  explicit ServerSession(Transport &Transport) : Transport_(Transport) {}

  Error serveOne(const ServerDispatch &Dispatch);
  Error sendObject(uint32_t Channel, const ns::Object &Object,
                   PayloadFlag Flag = PayloadFlag::AsyncObject);

private:
  Expected<std::vector<uint8_t>> readExact(size_t Size);
  Error writeAll(const std::vector<uint8_t> &Bytes);

  Transport &Transport_;
  uint32_t NextOutboundMessageId_ = 0x80000000u;
  std::mutex WriteMutex_;
};

} // namespace llvm::dtx
