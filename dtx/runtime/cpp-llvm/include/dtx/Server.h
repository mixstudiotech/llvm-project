#pragma once

#include "dtx/Fragment.h"
#include "dtx/Transport.h"

#include <functional>
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

private:
  Expected<std::vector<uint8_t>> readExact(size_t Size);
  Error writeAll(const std::vector<uint8_t> &Bytes);

  Transport &Transport_;
};

} // namespace llvm::dtx
