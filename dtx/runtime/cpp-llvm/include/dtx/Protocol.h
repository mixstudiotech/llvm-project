#pragma once

#include "dtx/Errors.h"

#include <cstdint>
#include <vector>

namespace llvm::dtx {

constexpr uint32_t MessageMagic = 0x1F3D5B79;
constexpr uint32_t MessageHeaderLength = 0x20;
constexpr uint64_t AuxMagic = 0x01F0;

enum class PayloadFlag : uint32_t {
  Empty = 0x00,
  AsyncObject = 0x01,
  Selector = 0x02,
  Reply = 0x03,
  Object = 0x04,
  Null = 0x05,
};

struct MessageHeader {
  uint32_t Magic = MessageMagic;
  uint32_t HeaderLength = MessageHeaderLength;
  uint16_t FragmentId = 0;
  uint16_t FragmentCount = 1;
  uint32_t PayloadLength = 0;
  uint32_t MessageId = 0;
  uint32_t ConversationIndex = 0;
  uint32_t ChannelCode = 0;
  uint32_t ExpectsReply = 0;

  static MessageHeader build(uint32_t ChannelCode, uint32_t PayloadLength,
                             uint32_t MessageId, uint32_t ConversationIndex,
                             bool ExpectsReply);
};

static_assert(sizeof(MessageHeader) == MessageHeaderLength,
              "DTX message header must stay wire-compatible");

struct PayloadHeader {
  uint32_t Flags = 0;
  uint32_t AuxiliaryLength = 0;
  uint64_t TotalLength = 0;
};

static_assert(sizeof(PayloadHeader) == 16,
              "DTX payload header must stay wire-compatible");

void appendLE16(std::vector<uint8_t> &Out, uint16_t Value);
void appendLE32(std::vector<uint8_t> &Out, uint32_t Value);
void appendLE64(std::vector<uint8_t> &Out, uint64_t Value);

Expected<uint16_t> readLE16(const uint8_t *Data, size_t Size, size_t Offset);
Expected<uint32_t> readLE32(const uint8_t *Data, size_t Size, size_t Offset);
Expected<uint64_t> readLE64(const uint8_t *Data, size_t Size, size_t Offset);

void appendHeader(std::vector<uint8_t> &Out, const MessageHeader &Header);
Expected<MessageHeader> parseHeader(const uint8_t *Data, size_t Size);

void appendPayloadHeader(std::vector<uint8_t> &Out, const PayloadHeader &Header);
Expected<PayloadHeader> parsePayloadHeader(const uint8_t *Data, size_t Size,
                                           size_t Offset = 0);

} // namespace llvm::dtx
