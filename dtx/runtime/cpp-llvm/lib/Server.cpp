#include "dtx/Server.h"

namespace llvm::dtx {

Expected<std::vector<uint8_t>> ServerSession::readExact(size_t Size) {
  std::vector<uint8_t> Bytes(Size);
  size_t Offset = 0;
  while (Offset < Size) {
    auto Count = Transport_.read(Bytes.data() + Offset, Size - Offset);
    if (!Count)
      return Count.error();
    if (Count.get() == 0)
      return Error("transport reached eof");
    Offset += Count.get();
  }
  return Bytes;
}

Error ServerSession::writeAll(const std::vector<uint8_t> &Bytes) {
  return Transport_.write(Bytes.data(), Bytes.size());
}

Error ServerSession::sendObject(uint32_t Channel, const ns::Object &Object,
                                PayloadFlag Flag) {
  std::lock_guard<std::mutex> Lock(WriteMutex_);
  std::vector<uint8_t> Payload = buildObjectPayload(Object, Flag);
  Fragment Message{MessageHeader::build(Channel,
                                        static_cast<uint32_t>(Payload.size()),
                                        ++NextOutboundMessageId_, 0, false),
                   Payload};
  return writeAll(Message.encode());
}

Expected<ServerRequest> ServerSession::readRequest() {
  auto HeaderBytes = readExact(MessageHeaderLength);
  if (!HeaderBytes)
    return HeaderBytes.error();
  auto Header = parseHeader(HeaderBytes.get().data(), HeaderBytes.get().size());
  if (!Header)
    return Header.error();
  auto PayloadBytes = readExact(Header.get().PayloadLength);
  if (!PayloadBytes)
    return PayloadBytes.error();
  auto Selector = parseSelectorPayload(PayloadBytes.get());
  if (!Selector)
    return Selector.error();

  ServerRequest Req;
  Req.ChannelCode = Header.get().ChannelCode;
  Req.MessageId = Header.get().MessageId;
  Req.ConversationIndex = Header.get().ConversationIndex;
  Req.ExpectsReply = Header.get().ExpectsReply != 0;
  Req.Selector = std::move(Selector.get().Selector);
  Req.Args = std::move(Selector.get().Args);
  return Req;
}

Error ServerSession::sendReply(const ServerRequest &Request,
                               const ns::Object &Reply) {
  if (!Request.ExpectsReply)
    return Error::success();
  std::vector<uint8_t> ReplyPayload =
      buildObjectPayload(Reply, PayloadFlag::Reply);
  Fragment ReplyFrag{
      MessageHeader::build(Request.ChannelCode,
                           static_cast<uint32_t>(ReplyPayload.size()),
                           Request.MessageId, Request.ConversationIndex + 1,
                           false),
      ReplyPayload};
  std::lock_guard<std::mutex> Lock(WriteMutex_);
  return writeAll(ReplyFrag.encode());
}

Error ServerSession::serveOne(const ServerDispatch &Dispatch) {
  auto Req = readRequest();
  if (!Req)
    return Req.error();

  auto ReplyObject = Dispatch(Req.get().ChannelCode, Req.get().Selector,
                              Req.get().Args);
  if (!ReplyObject)
    return ReplyObject.error();
  return sendReply(Req.get(), ReplyObject.get());
}

} // namespace llvm::dtx
