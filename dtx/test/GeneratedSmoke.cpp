#include "DebugHostClient.h.inc"
#include "DebugHostServer.h.inc"
#include "DebugHostService.h.inc"

#include <iostream>

class MockConnection final : public llvm::dtx::Connection {
public:
  llvm::dtx::Expected<llvm::dtx::ns::Object>
  call(uint32_t Channel, const std::string &Selector, const llvm::dtx::AuxList &Args,
       bool ExpectsReply) override {
    if (Channel != 7 || Selector != "initialize:" || !ExpectsReply)
      return llvm::dtx::Error("unexpected generated client call");
    if (Args.args().size() != 1)
      return llvm::dtx::Error("generated client did not encode one request arg");
    return llvm::dtx::ns::Object("ok");
  }
};

class LifecycleProcessor final
    : public llvm::dtx::debughost::ILifecycleProcessor {
public:
  llvm::dtx::Expected<llvm::dtx::debughost::InitializeResult>
  initialize(const llvm::dtx::debughost::InitializeRequest &request) override {
    (void)request;
    llvm::dtx::debughost::InitializeResult Result;
    Result.Raw = llvm::dtx::ns::Object("initialized");
    return Result;
  }

  llvm::dtx::Expected<llvm::dtx::debughost::ShutdownResult>
  shutdown(const llvm::dtx::debughost::ShutdownRequest &request) override {
    (void)request;
    llvm::dtx::debughost::ShutdownResult Result;
    Result.Raw = llvm::dtx::ns::Object("shutdown");
    return Result;
  }
};

int main() {
  MockConnection Connection;
  llvm::dtx::debughost::LifecycleClient Client(Connection, 7);
  llvm::dtx::debughost::InitializeRequest Request;
  Request.Raw = llvm::dtx::ns::Object("request");
  auto Reply = Client.initialize(Request);
  if (!Reply) {
    std::cerr << Reply.error().message() << "\n";
    return 1;
  }

  LifecycleProcessor Processor;
  auto DirectReply = Processor.initialize(Request);
  if (!DirectReply) {
    std::cerr << DirectReply.error().message() << "\n";
    return 1;
  }

  llvm::dtx::AuxList DispatchArgs;
  DispatchArgs.append(llvm::dtx::Arg(Request.toNSObject()));
  auto DispatchedReply = llvm::dtx::debughost::LifecycleServiceDesc::dispatch(
      Processor, "initialize:", DispatchArgs);
  if (!DispatchedReply ||
      DispatchedReply.get() != llvm::dtx::ns::Object("initialized")) {
    std::cerr << "generated dispatch failed\n";
    return 1;
  }

  llvm::dtx::debughost::DebugHostServer Server;
  Server.setLifecycleProcessor(&Processor);
  auto ServerReply = Server.dispatch(
      llvm::dtx::debughost::LifecycleClient::ChannelId, "initialize:",
      DispatchArgs);
  if (!ServerReply ||
      ServerReply.get() != llvm::dtx::ns::Object("initialized")) {
    std::cerr << "generated server dispatch failed\n";
    return 1;
  }

  if (std::string(llvm::dtx::debughost::LifecycleServiceDesc::Channel) !=
      "org.llvm.debughost.lifecycle") {
    std::cerr << "bad generated service channel\n";
    return 1;
  }
  return 0;
}
