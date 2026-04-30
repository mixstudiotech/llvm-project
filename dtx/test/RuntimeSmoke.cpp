#include "dtx/AuxList.h"
#include "dtx/Connection.h"
#include "dtx/Fragment.h"
#include "dtx/KeyedArchiver.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <variant>

namespace {
class MemoryTransport final : public llvm::dtx::Transport {
public:
  explicit MemoryTransport(std::vector<uint8_t> ReadBytes)
      : ReadBytes_(std::move(ReadBytes)) {}

  llvm::dtx::Expected<size_t> read(uint8_t *Data, size_t Size) override {
    const size_t Remaining = ReadBytes_.size() - ReadOffset_;
    const size_t Count = std::min(Size, Remaining);
    std::copy(ReadBytes_.begin() + ReadOffset_,
              ReadBytes_.begin() + ReadOffset_ + Count, Data);
    ReadOffset_ += Count;
    return Count;
  }

  llvm::dtx::Error write(const uint8_t *Data, size_t Size) override {
    Written.insert(Written.end(), Data, Data + Size);
    return llvm::dtx::Error::success();
  }

  std::vector<uint8_t> Written;

private:
  std::vector<uint8_t> ReadBytes_;
  size_t ReadOffset_ = 0;
};
} // namespace

int main() {
  using namespace llvm::dtx;

  AuxList Aux;
  Aux.append(Arg(int32_t(-7)));
  Aux.append(Arg(uint64_t(42)));
  ns::Dict Dict;
  Dict.emplace("message", ns::Object("hello"));
  Dict.emplace("answer", ns::Object(uint64_t(42)));
  Dict.emplace("ok", ns::Object(true));
  ns::Object Object(Dict);
  Aux.append(Arg(Object));
  const std::vector<uint8_t> AuxBytes = Aux.encode();

  auto DecodedAux = AuxList::decode(AuxBytes.data(), AuxBytes.size());
  if (!DecodedAux) {
    std::cerr << DecodedAux.error().message() << "\n";
    return 1;
  }
  if (DecodedAux.get().args().size() != 3) {
    std::cerr << "wrong aux count\n";
    return 1;
  }
  const Arg &DecodedObjectArg = DecodedAux.get().args()[2];
  if (!std::holds_alternative<ns::Object>(DecodedObjectArg.Value) ||
      std::get<ns::Object>(DecodedObjectArg.Value) != Object) {
    std::cerr << "object aux roundtrip mismatch\n";
    return 1;
  }

  const std::vector<uint8_t> Archive = ns::KeyedArchiver::archiveRoot(Object);
  auto Unarchived = ns::KeyedArchiver::unarchiveRoot(Archive.data(), Archive.size());
  if (!Unarchived || Unarchived.get() != Object) {
    std::cerr << "object archive roundtrip mismatch\n";
    return 1;
  }

  std::vector<uint8_t> Payload =
      buildPayload(PayloadFlag::Selector, AuxBytes, {});
  MessageHeader Header =
      MessageHeader::build(/*ChannelCode=*/1, static_cast<uint32_t>(Payload.size()),
                           /*MessageId=*/9, /*ConversationIndex=*/0,
                           /*ExpectsReply=*/true);
  Fragment F{Header, Payload};
  if (!F.isComplete()) {
    std::cerr << "fragment is not complete\n";
    return 1;
  }

  const std::vector<uint8_t> Bytes = F.encode();
  auto DecodedFragment = Fragment::decode(Bytes.data(), Bytes.size());
  if (!DecodedFragment) {
    std::cerr << DecodedFragment.error().message() << "\n";
    return 1;
  }
  if (DecodedFragment.get().Header.Magic != MessageMagic ||
      DecodedFragment.get().Header.HeaderLength != MessageHeaderLength ||
      DecodedFragment.get().Header.MessageId != 9 ||
      DecodedFragment.get().Payload.size() != Payload.size()) {
    std::cerr << "fragment roundtrip mismatch\n";
    return 1;
  }

  auto PayloadHeader = parsePayloadHeader(DecodedFragment.get().Payload.data(),
                                          DecodedFragment.get().Payload.size());
  if (!PayloadHeader) {
    std::cerr << PayloadHeader.error().message() << "\n";
    return 1;
  }
  if (PayloadHeader.get().Flags != static_cast<uint32_t>(PayloadFlag::Selector) ||
      PayloadHeader.get().AuxiliaryLength != AuxBytes.size()) {
    std::cerr << "payload header mismatch\n";
    return 1;
  }

  std::vector<uint8_t> ReplyPayload =
      buildObjectPayload(ns::Object("reply"), PayloadFlag::Reply);
  Fragment Reply{MessageHeader::build(/*ChannelCode=*/3,
                                      static_cast<uint32_t>(ReplyPayload.size()),
                                      /*MessageId=*/1,
                                      /*ConversationIndex=*/1,
                                      /*ExpectsReply=*/false),
                 ReplyPayload};
  auto Transport = std::make_unique<MemoryTransport>(Reply.encode());
  MemoryTransport *TransportPtr = Transport.get();
  Connection Conn(std::move(Transport));
  AuxList CallArgs;
  CallArgs.append(Arg(ns::Object("arg")));
  auto CallReply = Conn.call(3, "initialize:", CallArgs, true);
  if (!CallReply) {
    std::cerr << "connection call failed: " << CallReply.error().message() << "\n";
    return 1;
  }
  if (CallReply.get() != ns::Object("reply")) {
    std::cerr << "connection call reply mismatch: " << CallReply.get().dump() << "\n";
    return 1;
  }
  auto Request = Fragment::decode(TransportPtr->Written.data(),
                                  TransportPtr->Written.size());
  if (!Request || Request.get().Header.ChannelCode != 3 ||
      Request.get().Header.ExpectsReply != 1) {
    std::cerr << "connection request frame mismatch\n";
    return 1;
  }

  auto RequestPayloadHeader =
      parsePayloadHeader(Request.get().Payload.data(), Request.get().Payload.size());
  if (!RequestPayloadHeader ||
      RequestPayloadHeader.get().Flags != static_cast<uint32_t>(PayloadFlag::Selector)) {
    std::cerr << "connection request payload mismatch\n";
    return 1;
  }
  return 0;
}
