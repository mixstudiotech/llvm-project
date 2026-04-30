#pragma once

#include "dtx/NSObject.h"

#include <functional>
#include <string>

namespace ycode::debughost {

using DebugHostEventSink =
    std::function<void(const llvm::dtx::ns::Object &Event)>;

class MixDeviceBridge {
public:
  llvm::dtx::ns::Object makeCapabilities() const;
  llvm::dtx::ns::Object listDevices() const;
  llvm::dtx::ns::Object prepareDebug(const llvm::dtx::ns::Object &Request) const;
  llvm::dtx::ns::Object deviceSymbolsStatus(
      const llvm::dtx::ns::Object &Request) const;
  llvm::dtx::ns::Object deviceSymbolsValidate(
      const llvm::dtx::ns::Object &Request) const;
  llvm::dtx::ns::Object deviceSymbolsPrefetch(
      const llvm::dtx::ns::Object &Request,
      const DebugHostEventSink &EventSink = nullptr) const;

private:
  static llvm::dtx::ns::Object makeUnavailableResult(std::string Message);
};

} // namespace ycode::debughost
