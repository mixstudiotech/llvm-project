#include "DebugHostClient.h.inc"
#include "DebugHostServer.h.inc"

#include "dtx/Connection.h"
#include "dtx/Server.h"
#include "dtx/TcpTransport.h"

#include <future>
#include <iostream>
#include <thread>

class LifecycleProcessor final
    : public llvm::dtx::debughost::ILifecycleProcessor {
public:
  llvm::dtx::Expected<llvm::dtx::debughost::InitializeResult>
  initialize(const llvm::dtx::debughost::InitializeRequest &Request) override {
    if (Request.Raw != llvm::dtx::ns::Object("tcp-request"))
      return llvm::dtx::Error("unexpected initialize request object");
    llvm::dtx::debughost::InitializeResult Result;
    Result.Raw = llvm::dtx::ns::Object("tcp-initialized");
    return Result;
  }

  llvm::dtx::Expected<llvm::dtx::debughost::ShutdownResult>
  shutdown(const llvm::dtx::debughost::ShutdownRequest &Request) override {
    (void)Request;
    llvm::dtx::debughost::ShutdownResult Result;
    Result.Raw = llvm::dtx::ns::Object("tcp-shutdown");
    return Result;
  }
};

int main() {
  auto ListenerOrErr = llvm::dtx::TcpListener::listenLoopback();
  if (!ListenerOrErr) {
    std::cerr << ListenerOrErr.error().message() << "\n";
    return 1;
  }
  uint16_t Port = ListenerOrErr.get().port();
  std::promise<std::string> ServerError;
  auto ServerErrorFuture = ServerError.get_future();

  std::thread ServerThread([Listener = std::move(ListenerOrErr.get()),
                            ServerError = std::move(ServerError)]() mutable {
    auto Transport = Listener.accept();
    if (!Transport) {
      ServerError.set_value(Transport.error().message());
      return;
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
    if (Err)
      ServerError.set_value(Err.message());
    else
      ServerError.set_value("");
  });

  auto ClientTransport = llvm::dtx::TcpTransport::connectLoopback(Port);
  if (!ClientTransport) {
    std::cerr << ClientTransport.error().message() << "\n";
    ServerThread.join();
    return 1;
  }

  llvm::dtx::Connection Connection(std::move(ClientTransport.get()));
  llvm::dtx::debughost::LifecycleClient Client(
      Connection, llvm::dtx::debughost::LifecycleClient::ChannelId);
  llvm::dtx::debughost::InitializeRequest Request;
  Request.Raw = llvm::dtx::ns::Object("tcp-request");
  auto Reply = Client.initialize(Request);
  if (!Reply) {
    std::cerr << Reply.error().message() << "\n";
    ServerThread.join();
    return 1;
  }
  if (Reply.get().Raw != llvm::dtx::ns::Object("tcp-initialized")) {
    std::cerr << "unexpected tcp DebugHost reply\n";
    ServerThread.join();
    return 1;
  }

  ServerThread.join();
  const std::string ServerMessage = ServerErrorFuture.get();
  if (!ServerMessage.empty()) {
    std::cerr << ServerMessage << "\n";
    return 1;
  }
  return 0;
}
