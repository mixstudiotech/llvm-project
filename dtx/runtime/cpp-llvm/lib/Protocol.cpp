#include "dtx/Protocol.h"

#include <cstring>

namespace llvm::dtx {

MessageHeader MessageHeader::build(uint32_t ChannelCode, uint32_t PayloadLength,
                                   uint32_t MessageId,
                                   uint32_t ConversationIndex,
                                   bool ExpectsReply) {
  MessageHeader H;
  H.ChannelCode = ChannelCode;
  H.PayloadLength = PayloadLength;
  H.MessageId = MessageId;
  H.ConversationIndex = ConversationIndex;
  H.ExpectsReply = ExpectsReply ? 1 : 0;
  return H;
}

void appendLE16(std::vector<uint8_t> &Out, uint16_t Value) {
  Out.push_back(static_cast<uint8_t>(Value));
  Out.push_back(static_cast<uint8_t>(Value >> 8));
}

void appendLE32(std::vector<uint8_t> &Out, uint32_t Value) {
  for (unsigned I = 0; I < 4; ++I)
    Out.push_back(static_cast<uint8_t>(Value >> (I * 8)));
}

void appendLE64(std::vector<uint8_t> &Out, uint64_t Value) {
  for (unsigned I = 0; I < 8; ++I)
    Out.push_back(static_cast<uint8_t>(Value >> (I * 8)));
}

Expected<uint16_t> readLE16(const uint8_t *Data, size_t Size, size_t Offset) {
  if (Offset + 2 > Size)
    return Error("unexpected eof while reading u16");
  return static_cast<uint16_t>(Data[Offset]) |
         static_cast<uint16_t>(Data[Offset + 1] << 8);
}

Expected<uint32_t> readLE32(const uint8_t *Data, size_t Size, size_t Offset) {
  if (Offset + 4 > Size)
    return Error("unexpected eof while reading u32");
  return static_cast<uint32_t>(Data[Offset]) |
         (static_cast<uint32_t>(Data[Offset + 1]) << 8) |
         (static_cast<uint32_t>(Data[Offset + 2]) << 16) |
         (static_cast<uint32_t>(Data[Offset + 3]) << 24);
}

Expected<uint64_t> readLE64(const uint8_t *Data, size_t Size, size_t Offset) {
  if (Offset + 8 > Size)
    return Error("unexpected eof while reading u64");
  uint64_t Value = 0;
  for (unsigned I = 0; I < 8; ++I)
    Value |= static_cast<uint64_t>(Data[Offset + I]) << (I * 8);
  return Value;
}

void appendHeader(std::vector<uint8_t> &Out, const MessageHeader &Header) {
  appendLE32(Out, Header.Magic);
  appendLE32(Out, Header.HeaderLength);
  appendLE16(Out, Header.FragmentId);
  appendLE16(Out, Header.FragmentCount);
  appendLE32(Out, Header.PayloadLength);
  appendLE32(Out, Header.MessageId);
  appendLE32(Out, Header.ConversationIndex);
  appendLE32(Out, Header.ChannelCode);
  appendLE32(Out, Header.ExpectsReply);
}

Expected<MessageHeader> parseHeader(const uint8_t *Data, size_t Size) {
  if (Size < MessageHeaderLength)
    return Error("DTX header is shorter than 0x20 bytes");
  MessageHeader H;
  auto Magic = readLE32(Data, Size, 0);
  auto HeaderLength = readLE32(Data, Size, 4);
  auto FragmentId = readLE16(Data, Size, 8);
  auto FragmentCount = readLE16(Data, Size, 10);
  auto PayloadLength = readLE32(Data, Size, 12);
  auto MessageId = readLE32(Data, Size, 16);
  auto ConversationIndex = readLE32(Data, Size, 20);
  auto ChannelCode = readLE32(Data, Size, 24);
  auto ExpectsReply = readLE32(Data, Size, 28);
  if (!Magic || !HeaderLength || !FragmentId || !FragmentCount ||
      !PayloadLength || !MessageId || !ConversationIndex || !ChannelCode ||
      !ExpectsReply)
    return Error("failed to parse DTX header");
  H.Magic = Magic.get();
  H.HeaderLength = HeaderLength.get();
  H.FragmentId = FragmentId.get();
  H.FragmentCount = FragmentCount.get();
  H.PayloadLength = PayloadLength.get();
  H.MessageId = MessageId.get();
  H.ConversationIndex = ConversationIndex.get();
  H.ChannelCode = ChannelCode.get();
  H.ExpectsReply = ExpectsReply.get();
  if (H.Magic != MessageMagic)
    return Error("bad DTX message magic");
  if (H.HeaderLength != MessageHeaderLength)
    return Error("bad DTX message header length");
  return H;
}

void appendPayloadHeader(std::vector<uint8_t> &Out,
                         const PayloadHeader &Header) {
  appendLE32(Out, Header.Flags);
  appendLE32(Out, Header.AuxiliaryLength);
  appendLE64(Out, Header.TotalLength);
}

Expected<PayloadHeader> parsePayloadHeader(const uint8_t *Data, size_t Size,
                                           size_t Offset) {
  if (Offset + sizeof(PayloadHeader) > Size)
    return Error("DTX payload header is shorter than 16 bytes");
  PayloadHeader H;
  auto Flags = readLE32(Data, Size, Offset);
  auto AuxLen = readLE32(Data, Size, Offset + 4);
  auto TotalLen = readLE64(Data, Size, Offset + 8);
  if (!Flags || !AuxLen || !TotalLen)
    return Error("failed to parse DTX payload header");
  H.Flags = Flags.get();
  H.AuxiliaryLength = AuxLen.get();
  H.TotalLength = TotalLen.get();
  return H;
}

} // namespace llvm::dtx
