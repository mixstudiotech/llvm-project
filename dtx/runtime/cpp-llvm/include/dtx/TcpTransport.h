#pragma once

#include "dtx/Transport.h"

#include <cstdint>
#include <memory>

namespace llvm::dtx {

class TcpTransport final : public Transport {
public:
  TcpTransport() = default;
  explicit TcpTransport(uintptr_t Socket);
  ~TcpTransport() override;

  TcpTransport(const TcpTransport &) = delete;
  TcpTransport &operator=(const TcpTransport &) = delete;
  TcpTransport(TcpTransport &&Other) noexcept;
  TcpTransport &operator=(TcpTransport &&Other) noexcept;

  static Expected<std::unique_ptr<TcpTransport>> connectLoopback(uint16_t Port);

  Expected<size_t> read(uint8_t *Data, size_t Size) override;
  Error write(const uint8_t *Data, size_t Size) override;

private:
  void close();
  uintptr_t Socket_ = 0;
};

class TcpListener final {
public:
  TcpListener() = default;
  explicit TcpListener(uintptr_t Socket, uint16_t Port);
  ~TcpListener();

  TcpListener(const TcpListener &) = delete;
  TcpListener &operator=(const TcpListener &) = delete;
  TcpListener(TcpListener &&Other) noexcept;
  TcpListener &operator=(TcpListener &&Other) noexcept;

  static Expected<TcpListener> listenLoopback(uint16_t Port = 0);

  uint16_t port() const { return Port_; }
  Expected<std::unique_ptr<TcpTransport>> accept();

private:
  void close();
  uintptr_t Socket_ = 0;
  uint16_t Port_ = 0;
};

} // namespace llvm::dtx
