#include "DebugHostServer.h.inc"

#include "DebugHostServices.h"
#include "MixDeviceBridge.h"

#include "dtx/Server.h"
#include "dtx/TcpTransport.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

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

    // Tracks worker threads spawned per RPC so the main loop can wait for
    // all in-flight handlers to finish before tearing down the session
    // (otherwise their captured references to Session/GeneratedServer
    // would dangle).
    struct InflightTracker {
      std::mutex Mutex;
      std::condition_variable Drained;
      size_t Count = 0;
      void inc() {
        std::lock_guard<std::mutex> L(Mutex);
        ++Count;
      }
      void dec() {
        std::unique_lock<std::mutex> L(Mutex);
        if (--Count == 0)
          Drained.notify_all();
      }
      void waitDrain() {
        std::unique_lock<std::mutex> L(Mutex);
        Drained.wait(L, [&] { return Count == 0; });
      }
    };
    InflightTracker Tracker;

    struct SessionTeardown {
      ycode::debughost::DebugHostServices &Services;
      std::shared_ptr<EventSinkContext> Context;
      InflightTracker &Tracker;
      ~SessionTeardown() {
        // 1) Wait for every dispatched handler to finish its sendReply so no
        //    background thread is still touching Session.
        Tracker.waitDrain();
        // 2) Detach the event sink so the pump (lldb event thread) stops
        //    writing to the about-to-die Session.
        {
          std::lock_guard<std::mutex> Lock(Context->Mutex);
          Context->Active = false;
          Context->Session = nullptr;
        }
        Services.setEventSink(nullptr);
      }
    } Teardown{Services, SinkContext, Tracker};

    do {
      auto ReqOrErr = Session.readRequest();
      if (!ReqOrErr) {
        llvm::errs() << ReqOrErr.error().message() << "\n";
        return 1;
      }
      auto Req = std::make_shared<llvm::dtx::ServerRequest>(
          std::move(ReqOrErr.get()));

      // Dispatch on a detached worker thread so a slow handler (e.g. pause
      // waiting on a remote interrupt round-trip) can't block the main loop
      // from reading the next RPC. Replies serialize through ServerSession's
      // WriteMutex_, so concurrent workers writing replies + the event sink
      // pushing async events are all safe.
      Tracker.inc();
      std::thread([&Session, &GeneratedServer, &Tracker, Req]() {
        auto ReplyObject = GeneratedServer.dispatch(
            Req->ChannelCode, Req->Selector, Req->Args);
        if (!ReplyObject) {
          llvm::errs() << "[lldb-mixdev] dispatch failed: "
                       << ReplyObject.error().message() << "\n";
        } else if (Req->ExpectsReply) {
          llvm::dtx::Error Err = Session.sendReply(*Req, ReplyObject.get());
          if (Err)
            llvm::errs() << "[lldb-mixdev] sendReply failed: " << Err.message()
                         << "\n";
        }
        Tracker.dec();
      }).detach();

      if (ServeOne)
        Tracker.waitDrain();
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
