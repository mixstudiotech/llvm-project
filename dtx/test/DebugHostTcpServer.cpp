#include "DebugHostServer.h.inc"

#include "dtx/Server.h"
#include "dtx/TcpTransport.h"

#include <iostream>

class LifecycleProcessor final
    : public llvm::dtx::debughost::ILifecycleProcessor {
public:
  llvm::dtx::Expected<llvm::dtx::debughost::InitializeResult>
  initialize(const llvm::dtx::debughost::InitializeRequest &Request) override {
    llvm::dtx::debughost::InitializeResult Result;
    if (Request.Raw == llvm::dtx::ns::Object("csharp-request"))
      Result.Raw = llvm::dtx::ns::Object("csharp-initialized");
    else if (Request.Raw == llvm::dtx::ns::Object("kotlin-request"))
      Result.Raw = llvm::dtx::ns::Object("kotlin-initialized");
    else
      return llvm::dtx::Error("unexpected initialize request object");
    return Result;
  }

  llvm::dtx::Expected<llvm::dtx::debughost::ShutdownResult>
  shutdown(const llvm::dtx::debughost::ShutdownRequest &Request) override {
    (void)Request;
    llvm::dtx::debughost::ShutdownResult Result;
    Result.Raw = llvm::dtx::ns::Object("csharp-shutdown");
    return Result;
  }
};

int main() {
  auto Listener = llvm::dtx::TcpListener::listenLoopback();
  if (!Listener) {
    std::cerr << Listener.error().message() << "\n";
    return 1;
  }

  std::cout << "PORT=" << Listener.get().port() << "\n";
  std::cout.flush();

  auto Transport = Listener.get().accept();
  if (!Transport) {
    std::cerr << Transport.error().message() << "\n";
    return 1;
  }

  LifecycleProcessor Lifecycle;
  llvm::dtx::debughost::DebugHostServer GeneratedServer;
  GeneratedServer.setLifecycleProcessor(&Lifecycle);

  llvm::dtx::ServerSession Session(*Transport.get());
  llvm::dtx::Error Err = Session.serveOne(
      [&](uint32_t Channel, const std::string &Selector,
          const llvm::dtx::AuxList &Args) {
        return GeneratedServer.dispatch(Channel, Selector, Args);
      });
  if (Err) {
    std::cerr << Err.message() << "\n";
    return 1;
  }
  return 0;
}
