#include "dtx/KeyedArchiver.h"

#include "dtx/Protocol.h"

#include <cstring>

namespace llvm::dtx::ns {

namespace {

constexpr uint8_t Magic[] = {'D', 'T', 'X', 'K', 'A', 'R', '1', '\0'};

enum class Tag : uint8_t {
  Null = 0,
  Bool = 1,
  Int = 2,
  UInt = 3,
  Double = 4,
  String = 5,
  Data = 6,
  Array = 7,
  Dict = 8,
};

class Writer {
public:
  void writeObject(const Object &Obj) {
    std::visit(
        [&](const auto &Value) {
          using T = std::decay_t<decltype(Value)>;
          if constexpr (std::is_same_v<T, std::nullptr_t>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::Null));
          } else if constexpr (std::is_same_v<T, bool>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::Bool));
            Bytes.push_back(Value ? 1 : 0);
          } else if constexpr (std::is_same_v<T, int64_t>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::Int));
            llvm::dtx::appendLE64(Bytes, static_cast<uint64_t>(Value));
          } else if constexpr (std::is_same_v<T, uint64_t>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::UInt));
            llvm::dtx::appendLE64(Bytes, Value);
          } else if constexpr (std::is_same_v<T, double>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::Double));
            uint64_t Raw = 0;
            static_assert(sizeof(Raw) == sizeof(Value),
                          "double must be 64-bit IEEE storage here");
            std::memcpy(&Raw, &Value, sizeof(Raw));
            llvm::dtx::appendLE64(Bytes, Raw);
          } else if constexpr (std::is_same_v<T, std::string>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::String));
            writeBytes(reinterpret_cast<const uint8_t *>(Value.data()),
                       Value.size());
          } else if constexpr (std::is_same_v<T, Data>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::Data));
            writeBytes(Value.data(), Value.size());
          } else if constexpr (std::is_same_v<T, Array>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::Array));
            llvm::dtx::appendLE32(Bytes, static_cast<uint32_t>(Value.size()));
            for (const Object &Element : Value)
              writeObject(Element);
          } else if constexpr (std::is_same_v<T, Dict>) {
            Bytes.push_back(static_cast<uint8_t>(Tag::Dict));
            llvm::dtx::appendLE32(Bytes, static_cast<uint32_t>(Value.size()));
            for (const auto &Entry : Value) {
              writeBytes(reinterpret_cast<const uint8_t *>(Entry.first.data()),
                         Entry.first.size());
              writeObject(Entry.second);
            }
          }
        },
        Obj.Value);
  }

  std::vector<uint8_t> take() { return std::move(Bytes); }

private:
  void writeBytes(const uint8_t *Data, size_t Size) {
    llvm::dtx::appendLE32(Bytes, static_cast<uint32_t>(Size));
    Bytes.insert(Bytes.end(), Data, Data + Size);
  }

  std::vector<uint8_t> Bytes;
};

class Reader {
public:
  Reader(const uint8_t *Data, size_t Size) : Data(Data), Size(Size) {}

  llvm::dtx::Expected<Object> readObject() {
    auto TagByte = readByte();
    if (!TagByte)
      return TagByte.error();

    switch (static_cast<Tag>(TagByte.get())) {
    case Tag::Null:
      return Object(nullptr);
    case Tag::Bool: {
      auto Value = readByte();
      if (!Value)
        return Value.error();
      return Object(Value.get() != 0);
    }
    case Tag::Int: {
      auto Value = readU64();
      if (!Value)
        return Value.error();
      return Object(static_cast<int64_t>(Value.get()));
    }
    case Tag::UInt: {
      auto Value = readU64();
      if (!Value)
        return Value.error();
      return Object(Value.get());
    }
    case Tag::Double: {
      auto Raw = readU64();
      if (!Raw)
        return Raw.error();
      double Value = 0;
      uint64_t RawValue = Raw.get();
      std::memcpy(&Value, &RawValue, sizeof(Value));
      return Object(Value);
    }
    case Tag::String: {
      auto Bytes = readBytes();
      if (!Bytes)
        return Bytes.error();
      return Object(std::string(reinterpret_cast<const char *>(Bytes.get().data()),
                                Bytes.get().size()));
    }
    case Tag::Data: {
      auto Bytes = readBytes();
      if (!Bytes)
        return Bytes.error();
      return Object(Bytes.get());
    }
    case Tag::Array: {
      auto Count = readU32();
      if (!Count)
        return Count.error();
      Array Values;
      Values.reserve(Count.get());
      for (uint32_t I = 0; I < Count.get(); ++I) {
        auto Element = readObject();
        if (!Element)
          return Element.error();
        Values.push_back(Element.get());
      }
      return Object(std::move(Values));
    }
    case Tag::Dict: {
      auto Count = readU32();
      if (!Count)
        return Count.error();
      Dict Values;
      for (uint32_t I = 0; I < Count.get(); ++I) {
        auto KeyBytes = readBytes();
        if (!KeyBytes)
          return KeyBytes.error();
        std::string Key(reinterpret_cast<const char *>(KeyBytes.get().data()),
                        KeyBytes.get().size());
        auto Value = readObject();
        if (!Value)
          return Value.error();
        Values.emplace(std::move(Key), Value.get());
      }
      return Object(std::move(Values));
    }
    }
    return llvm::dtx::Error("unknown object archive tag");
  }

  bool consumed() const { return Offset == Size; }

private:
  llvm::dtx::Expected<uint8_t> readByte() {
    if (Offset + 1 > Size)
      return llvm::dtx::Error("unexpected eof while reading object tag");
    return Data[Offset++];
  }

  llvm::dtx::Expected<uint32_t> readU32() {
    auto Value = llvm::dtx::readLE32(Data, Size, Offset);
    if (!Value)
      return Value.error();
    Offset += 4;
    return Value.get();
  }

  llvm::dtx::Expected<uint64_t> readU64() {
    auto Value = llvm::dtx::readLE64(Data, Size, Offset);
    if (!Value)
      return Value.error();
    Offset += 8;
    return Value.get();
  }

  llvm::dtx::Expected<std::vector<uint8_t>> readBytes() {
    auto Length = readU32();
    if (!Length)
      return Length.error();
    if (Offset + Length.get() > Size)
      return llvm::dtx::Error("object byte payload extends past archive");
    std::vector<uint8_t> Bytes(Data + Offset, Data + Offset + Length.get());
    Offset += Length.get();
    return Bytes;
  }

  const uint8_t *Data = nullptr;
  size_t Size = 0;
  size_t Offset = 0;
};

} // namespace

std::vector<uint8_t> KeyedArchiver::archiveRoot(const Object &Root) {
  std::vector<uint8_t> Out(std::begin(Magic), std::end(Magic));
  Writer W;
  W.writeObject(Root);
  std::vector<uint8_t> Body = W.take();
  Out.insert(Out.end(), Body.begin(), Body.end());
  return Out;
}

llvm::dtx::Expected<Object> KeyedArchiver::unarchiveRoot(const uint8_t *Data,
                                                         size_t Size) {
  if (Size < sizeof(Magic))
    return llvm::dtx::Error("object archive is shorter than magic");
  if (std::memcmp(Data, Magic, sizeof(Magic)) != 0)
    return llvm::dtx::Error("bad object archive magic");

  Reader R(Data + sizeof(Magic), Size - sizeof(Magic));
  auto Root = R.readObject();
  if (!Root)
    return Root.error();
  if (!R.consumed())
    return llvm::dtx::Error("trailing bytes after object archive root");
  return Root.get();
}

} // namespace llvm::dtx::ns
