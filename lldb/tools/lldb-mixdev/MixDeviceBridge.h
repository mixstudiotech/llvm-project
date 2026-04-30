#pragma once

#include "dtx/NSObject.h"

#include <string>

namespace ycode::debughost {

class MixDeviceBridge {
public:
  llvm::dtx::ns::Object makeCapabilities() const;
  llvm::dtx::ns::Object listDevices() const;
  llvm::dtx::ns::Object prepareDebug(const llvm::dtx::ns::Object &Request) const;

private:
  static llvm::dtx::ns::Object makeUnavailableResult(std::string Message);
};

} // namespace ycode::debughost
