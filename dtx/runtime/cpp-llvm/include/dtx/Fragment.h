#pragma once

#include "dtx/AuxList.h"
#include "dtx/NSObject.h"
#include "dtx/Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace llvm::dtx {

struct Fragment {
  MessageHeader Header;
  std::vector<uint8_t> Payload;

  bool isComplete() const;
  std::vector<uint8_t> encode() const;
  static Expected<Fragment> decode(const uint8_t *Data, size_t Size);
};

struct SelectorPayload {
  std::string Selector;
  AuxList Args;
};

std::vector<uint8_t> buildPayload(PayloadFlag Flag,
                                  const std::vector<uint8_t> &AuxBytes,
                                  const std::vector<uint8_t> &BodyBytes = {});
std::vector<uint8_t> buildSelectorPayload(const std::string &Selector,
                                          const std::vector<uint8_t> &AuxBytes);
std::vector<uint8_t> buildObjectPayload(const ns::Object &Object,
                                        PayloadFlag Flag = PayloadFlag::Reply);
Expected<SelectorPayload> parseSelectorPayload(const std::vector<uint8_t> &Payload);
Expected<ns::Object> parseObjectPayload(const std::vector<uint8_t> &Payload);

} // namespace llvm::dtx
