#include "dtx/AuxList.h"

#include "dtx/KeyedArchiver.h"
#include "dtx/Protocol.h"

#include <cstring>

namespace llvm::dtx {

namespace {
void appendTag(std::vector<uint8_t> &Out, AuxType Type) {
  appendLE32(Out, 10);
  appendLE32(Out, static_cast<uint32_t>(Type));
}

void appendObjectBytes(std::vector<uint8_t> &Out, const ns::Object &Obj) {
  const std::vector<uint8_t> Bytes = ns::KeyedArchiver::archiveRoot(Obj);
  appendLE32(Out, static_cast<uint32_t>(Bytes.size()));
  Out.insert(Out.end(), Bytes.begin(), Bytes.end());
}
} // namespace

std::vector<uint8_t> AuxList::encode() const {
  std::vector<uint8_t> Body;
  for (const Arg &A : Args_) {
    std::visit(
        [&](const auto &Value) {
          using T = std::decay_t<decltype(Value)>;
          if constexpr (std::is_same_v<T, std::nullptr_t>) {
            appendTag(Body, AuxType::ObjectOrNil);
            appendLE32(Body, 0);
          } else if constexpr (std::is_same_v<T, int32_t>) {
            appendTag(Body, AuxType::I32);
            appendLE32(Body, static_cast<uint32_t>(Value));
          } else if constexpr (std::is_same_v<T, int64_t>) {
            appendTag(Body, AuxType::I64);
            appendLE64(Body, static_cast<uint64_t>(Value));
          } else if constexpr (std::is_same_v<T, uint32_t>) {
            appendTag(Body, AuxType::U32);
            appendLE32(Body, Value);
          } else if constexpr (std::is_same_v<T, uint64_t>) {
            appendTag(Body, AuxType::U64);
            appendLE64(Body, Value);
          } else if constexpr (std::is_same_v<T, ns::Object>) {
            appendTag(Body, AuxType::ObjectOrNil);
            appendObjectBytes(Body, Value);
          }
        },
        A.Value);
  }

  std::vector<uint8_t> Out;
  appendLE64(Out, AuxMagic);
  appendLE64(Out, Body.size());
  Out.insert(Out.end(), Body.begin(), Body.end());
  return Out;
}

Expected<AuxList> AuxList::decode(const uint8_t *Data, size_t Size) {
  if (Size < 16)
    return Error("aux list is shorter than header");
  auto Magic = readLE64(Data, Size, 0);
  auto BodyLen = readLE64(Data, Size, 8);
  if (!Magic || !BodyLen)
    return Error("failed to decode aux header");
  if (Magic.get() != AuxMagic)
    return Error("bad aux magic");
  if (BodyLen.get() + 16 != Size)
    return Error("aux length does not match buffer size");

  AuxList Out;
  size_t Offset = 16;
  while (Offset < Size) {
    auto TypeTag = readLE32(Data, Size, Offset);
    auto Type = readLE32(Data, Size, Offset + 4);
    if (!TypeTag || !Type)
      return Error("failed to decode aux type");
    if (TypeTag.get() != 10)
      return Error("bad aux type tag");
    Offset += 8;

    switch (static_cast<AuxType>(Type.get())) {
    case AuxType::ObjectOrNil: {
      auto ObjLen = readLE32(Data, Size, Offset);
      if (!ObjLen)
        return Error("failed to decode object length");
      Offset += 4;
      if (ObjLen.get() == 0) {
        Out.append(Arg(nullptr));
        break;
      }
      if (Offset + ObjLen.get() > Size)
        return Error("object payload extends past aux buffer");
      auto Object = ns::KeyedArchiver::unarchiveRoot(Data + Offset, ObjLen.get());
      if (!Object)
        return Object.error();
      Out.append(Arg(Object.get()));
      Offset += ObjLen.get();
      break;
    }
    case AuxType::I32: {
      auto V = readLE32(Data, Size, Offset);
      if (!V)
        return Error("failed to decode i32");
      Out.append(Arg(static_cast<int32_t>(V.get())));
      Offset += 4;
      break;
    }
    case AuxType::I64: {
      auto V = readLE64(Data, Size, Offset);
      if (!V)
        return Error("failed to decode i64");
      Out.append(Arg(static_cast<int64_t>(V.get())));
      Offset += 8;
      break;
    }
    case AuxType::U32: {
      auto V = readLE32(Data, Size, Offset);
      if (!V)
        return Error("failed to decode u32");
      Out.append(Arg(V.get()));
      Offset += 4;
      break;
    }
    case AuxType::U64: {
      auto V = readLE64(Data, Size, Offset);
      if (!V)
        return Error("failed to decode u64");
      Out.append(Arg(V.get()));
      Offset += 8;
      break;
    }
    default:
      return Error("unsupported aux element type");
    }
  }
  return Out;
}

} // namespace llvm::dtx
