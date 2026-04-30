#include "DebugHostServer.h.inc"

#include "DebugHostServices.h"
#include "MixDeviceBridge.h"

#include "dtx/Server.h"
#include "dtx/TcpTransport.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <mutex>

namespace {

llvm::cl::OptionCategory YCodeDebugHostCategory("YCode DebugHost options");
llvm::cl::opt<bool> PrintPort(
    "print-port",
    llvm::cl::desc("Print PORT=<port> after binding the TCP listener"),
    llvm::cl::init(true), llvm::cl::cat(YCodeDebugHostCategory));
llvm::cl::opt<bool> ServeOne(
    "serve-one",
    llvm::cl::desc("Serve one DTX request and exit"),
    llvm::cl::init(false), llvm::cl::cat(YCodeDebugHostCategory));

int runTcpServer() {
  auto Listener = llvm::dtx::TcpListener::listenLoopback();
  if (!Listener) {
    llvm::errs() << Listener.error().message() << "\n";
    return 1;
  }

  if (PrintPort) {
    llvm::outs() << "PORT=" << Listener.get().port() << "\n";
    llvm::outs().flush();
  }

  ycode::debughost::MixDeviceBridge Bridge;
  ycode::debughost::DebugHostServices Services(Bridge);
  llvm::dtx::debughost::DebugHostServer GeneratedServer;
  Services.registerWith(GeneratedServer);

  do {
    auto Transport = Listener.get().accept();
    if (!Transport) {
      llvm::errs() << Transport.error().message() << "\n";
      return 1;
    }

    llvm::dtx::ServerSession Session(*Transport.get());
    struct EventSinkContext {
      llvm::dtx::ServerSession *Session = nullptr;
      std::mutex Mutex;
      bool Active = true;
    };
    auto SinkContext = std::make_shared<EventSinkContext>();
    SinkContext->Session = &Session;
    Services.setEventSink([SinkContext](const llvm::dtx::ns::Object &Event) {
      std::lock_guard<std::mutex> Lock(SinkContext->Mutex);
      if (!SinkContext->Active || !SinkContext->Session)
        return;
      llvm::dtx::Error Err = SinkContext->Session->sendObject(0, Event);
      if (Err)
        llvm::errs() << Err.message() << "\n";
    });
    struct EventSinkReset {
      ycode::debughost::DebugHostServices &Services;
      std::shared_ptr<EventSinkContext> Context;
      ~EventSinkReset() {
        {
          std::lock_guard<std::mutex> Lock(Context->Mutex);
          Context->Active = false;
          Context->Session = nullptr;
        }
        Services.setEventSink(nullptr);
      }
    } Reset{Services, SinkContext};
    do {
      llvm::dtx::Error Err = Session.serveOne(
          [&](uint32_t Channel, const std::string &Selector,
              const llvm::dtx::AuxList &Args) {
            return GeneratedServer.dispatch(Channel, Selector, Args);
          });
      if (Err) {
        llvm::errs() << Err.message() << "\n";
        return 1;
      }
    } while (!ServeOne && !Services.shouldStop());
  } while (!ServeOne && !Services.shouldStop());

  return 0;
}

} // namespace

int main(int argc, char **argv) {
  llvm::InitLLVM X(argc, argv);
  llvm::cl::HideUnrelatedOptions(YCodeDebugHostCategory);
  llvm::cl::ParseCommandLineOptions(argc, argv, "YCode DebugHost\n");
  return runTcpServer();
}
