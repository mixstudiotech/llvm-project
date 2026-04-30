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

Error ServerSession::serveOne(const ServerDispatch &Dispatch) {
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

  auto ReplyObject =
      Dispatch(Header.get().ChannelCode, Selector.get().Selector, Selector.get().Args);
  if (!ReplyObject)
    return ReplyObject.error();
  if (Header.get().ExpectsReply == 0)
    return Error::success();

  std::vector<uint8_t> ReplyPayload =
      buildObjectPayload(ReplyObject.get(), PayloadFlag::Reply);
  Fragment Reply{MessageHeader::build(Header.get().ChannelCode,
                                      static_cast<uint32_t>(ReplyPayload.size()),
                                      Header.get().MessageId,
                                      Header.get().ConversationIndex + 1,
                                      false),
                 ReplyPayload};
  return writeAll(Reply.encode());
}

} // namespace llvm::dtx
