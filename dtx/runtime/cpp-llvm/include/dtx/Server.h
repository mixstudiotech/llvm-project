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

// Captures everything serveOne needs to send a reply for a request, decoupled
// from the read step. Lets callers run dispatch on a worker thread while the
// main loop reads the next request — the single-threaded dispatch in plain
// serveOne() blocks all subsequent RPCs whenever one handler is slow.
struct ServerRequest {
  uint32_t ChannelCode = 0;
  uint32_t MessageId = 0;
  uint32_t ConversationIndex = 0;
  bool ExpectsReply = false;
  std::string Selector;
  AuxList Args;
};

class ServerSession {
public:
  explicit ServerSession(Transport &Transport) : Transport_(Transport) {}

  // Synchronous helper: read one request, dispatch on the calling thread,
  // send the reply. Convenient for single-threaded servers and tests.
  Error serveOne(const ServerDispatch &Dispatch);

  // Read one request off the wire. Does NOT dispatch. Caller is responsible
  // for invoking the appropriate handler and (if ExpectsReply) calling
  // sendReply with the resulting Object on the same ServerRequest.
  Expected<ServerRequest> readRequest();

  // Send the reply for a previously-read request. Thread-safe; serializes
  // writes via WriteMutex_ so concurrent worker threads don't interleave.
  Error sendReply(const ServerRequest &Request, const ns::Object &Reply);

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
