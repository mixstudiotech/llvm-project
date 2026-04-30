#include "dtx/Connection.h"

#include "dtx/Fragment.h"
#include "dtx/Protocol.h"

namespace llvm::dtx {

Connection::Connection(std::unique_ptr<Transport> Transport)
    : Transport_(std::move(Transport)) {}

Expected<std::vector<uint8_t>> Connection::readExact(size_t Size) {
  if (!Transport_)
    return Error("connection has no transport");
  std::vector<uint8_t> Bytes(Size);
  size_t Offset = 0;
  while (Offset < Size) {
    auto Count = Transport_->read(Bytes.data() + Offset, Size - Offset);
    if (!Count)
      return Count.error();
    if (Count.get() == 0)
      return Error("transport reached eof");
    Offset += Count.get();
  }
  return Bytes;
}

Error Connection::writeAll(const std::vector<uint8_t> &Bytes) {
  if (!Transport_)
    return Error("connection has no transport");
  return Transport_->write(Bytes.data(), Bytes.size());
}

Expected<ns::Object> Connection::call(uint32_t Channel,
                                      const std::string &Selector,
                                      const AuxList &Args, bool ExpectsReply) {
  const std::vector<uint8_t> AuxBytes = Args.encode();
  std::vector<uint8_t> Payload = buildSelectorPayload(Selector, AuxBytes);
  const uint32_t MessageId = ++NextMessageId_;
  Fragment Request{MessageHeader::build(Channel, static_cast<uint32_t>(Payload.size()),
                                        MessageId, 0, ExpectsReply),
                   Payload};
  Error WriteError = writeAll(Request.encode());
  if (WriteError)
    return WriteError;

  if (!ExpectsReply)
    return ns::Object(nullptr);

  auto HeaderBytes = readExact(MessageHeaderLength);
  if (!HeaderBytes)
    return HeaderBytes.error();
  auto Header = parseHeader(HeaderBytes.get().data(), HeaderBytes.get().size());
  if (!Header)
    return Header.error();
  auto PayloadBytes = readExact(Header.get().PayloadLength);
  if (!PayloadBytes)
    return PayloadBytes.error();

  return parseObjectPayload(PayloadBytes.get());
}

} // namespace llvm::dtx
