#include "dtx/Fragment.h"

#include "dtx/KeyedArchiver.h"

namespace llvm::dtx {

bool Fragment::isComplete() const {
  return Header.FragmentId == 0 && Header.FragmentCount == 1 &&
         Header.PayloadLength == Payload.size();
}

std::vector<uint8_t> Fragment::encode() const {
  std::vector<uint8_t> Out;
  appendHeader(Out, Header);
  Out.insert(Out.end(), Payload.begin(), Payload.end());
  return Out;
}

Expected<Fragment> Fragment::decode(const uint8_t *Data, size_t Size) {
  auto HeaderOrErr = parseHeader(Data, Size);
  if (!HeaderOrErr)
    return HeaderOrErr.error();
  const MessageHeader H = HeaderOrErr.get();
  if (MessageHeaderLength + H.PayloadLength > Size)
    return Error("fragment payload extends past buffer");
  Fragment F;
  F.Header = H;
  F.Payload.assign(Data + MessageHeaderLength,
                   Data + MessageHeaderLength + H.PayloadLength);
  return F;
}

std::vector<uint8_t> buildPayload(PayloadFlag Flag,
                                  const std::vector<uint8_t> &AuxBytes,
                                  const std::vector<uint8_t> &BodyBytes) {
  PayloadHeader H;
  H.Flags = static_cast<uint32_t>(Flag);
  H.AuxiliaryLength = static_cast<uint32_t>(AuxBytes.size());
  H.TotalLength = AuxBytes.size() + BodyBytes.size();

  std::vector<uint8_t> Out;
  appendPayloadHeader(Out, H);
  Out.insert(Out.end(), AuxBytes.begin(), AuxBytes.end());
  Out.insert(Out.end(), BodyBytes.begin(), BodyBytes.end());
  return Out;
}

std::vector<uint8_t> buildSelectorPayload(const std::string &Selector,
                                          const std::vector<uint8_t> &AuxBytes) {
  std::vector<uint8_t> SelectorBytes =
      ns::KeyedArchiver::archiveRoot(ns::Object(Selector));
  return buildPayload(PayloadFlag::Selector, AuxBytes, SelectorBytes);
}

std::vector<uint8_t> buildObjectPayload(const ns::Object &Object,
                                        PayloadFlag Flag) {
  std::vector<uint8_t> Body = ns::KeyedArchiver::archiveRoot(Object);
  return buildPayload(Flag, {}, Body);
}

Expected<SelectorPayload> parseSelectorPayload(const std::vector<uint8_t> &Payload) {
  auto Header = parsePayloadHeader(Payload.data(), Payload.size());
  if (!Header)
    return Header.error();
  if (Header.get().Flags != static_cast<uint32_t>(PayloadFlag::Selector))
    return Error("payload is not a selector call");

  const size_t AuxOffset = sizeof(PayloadHeader);
  const size_t AuxLength = Header.get().AuxiliaryLength;
  if (AuxOffset + Header.get().TotalLength != Payload.size())
    return Error("selector payload length mismatch");
  if (AuxLength > Header.get().TotalLength)
    return Error("selector aux length exceeds total payload length");

  SelectorPayload Result;
  if (AuxLength > 0) {
    auto Args = AuxList::decode(Payload.data() + AuxOffset, AuxLength);
    if (!Args)
      return Args.error();
    Result.Args = Args.get();
  }

  const size_t SelectorOffset = AuxOffset + AuxLength;
  const size_t SelectorLength =
      static_cast<size_t>(Header.get().TotalLength) - AuxLength;
  auto Selector =
      ns::KeyedArchiver::unarchiveRoot(Payload.data() + SelectorOffset,
                                       SelectorLength);
  if (!Selector)
    return Selector.error();
  if (const auto *Value = std::get_if<std::string>(&Selector.get().Value)) {
    Result.Selector = *Value;
    return Result;
  }
  return Error("selector payload root is not a string");
}

Expected<ns::Object> parseObjectPayload(const std::vector<uint8_t> &Payload) {
  auto Header = parsePayloadHeader(Payload.data(), Payload.size());
  if (!Header)
    return Header.error();
  if (Header.get().Flags != static_cast<uint32_t>(PayloadFlag::Reply) &&
      Header.get().Flags != static_cast<uint32_t>(PayloadFlag::Object) &&
      Header.get().Flags != static_cast<uint32_t>(PayloadFlag::AsyncObject))
    return Error("payload is not an object reply");
  if (Header.get().AuxiliaryLength != 0)
    return Error("object payload unexpectedly contains aux bytes");
  if (sizeof(PayloadHeader) + Header.get().TotalLength != Payload.size())
    return Error("object payload length mismatch");
  if (Header.get().TotalLength == 0)
    return ns::Object(nullptr);
  return ns::KeyedArchiver::unarchiveRoot(
      Payload.data() + sizeof(PayloadHeader),
      static_cast<size_t>(Header.get().TotalLength));
}

} // namespace llvm::dtx
