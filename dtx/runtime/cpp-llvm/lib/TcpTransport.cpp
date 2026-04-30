#include "dtx/TcpTransport.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Ws2tcpip.h>
using SocketType = SOCKET;
static constexpr SocketType InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketType = int;
static constexpr SocketType InvalidSocket = -1;
#endif

namespace llvm::dtx {

namespace {

SocketType toSocket(uintptr_t Value) { return static_cast<SocketType>(Value); }
uintptr_t fromSocket(SocketType Value) { return static_cast<uintptr_t>(Value); }

Error ensureSocketsReady() {
#ifdef _WIN32
  static bool Ready = [] {
    WSADATA Data;
    return WSAStartup(MAKEWORD(2, 2), &Data) == 0;
  }();
  if (!Ready)
    return Error("WSAStartup failed");
#endif
  return Error::success();
}

std::string socketError(const char *Operation) {
#ifdef _WIN32
  return std::string(Operation) + " failed with WSA error " +
         std::to_string(WSAGetLastError());
#else
  return std::string(Operation) + " failed: " + std::strerror(errno);
#endif
}

void closeSocket(SocketType Socket) {
  if (Socket == InvalidSocket)
    return;
#ifdef _WIN32
  closesocket(Socket);
#else
  close(Socket);
#endif
}

} // namespace

TcpTransport::TcpTransport(uintptr_t Socket) : Socket_(Socket) {}

TcpTransport::~TcpTransport() { close(); }

TcpTransport::TcpTransport(TcpTransport &&Other) noexcept
    : Socket_(Other.Socket_) {
  Other.Socket_ = 0;
}

TcpTransport &TcpTransport::operator=(TcpTransport &&Other) noexcept {
  if (this != &Other) {
    close();
    Socket_ = Other.Socket_;
    Other.Socket_ = 0;
  }
  return *this;
}

Expected<std::unique_ptr<TcpTransport>>
TcpTransport::connectLoopback(uint16_t Port) {
  Error SocketReady = ensureSocketsReady();
  if (SocketReady)
    return SocketReady;

  SocketType Socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (Socket == InvalidSocket)
    return Error(socketError("socket"));

  sockaddr_in Addr{};
  Addr.sin_family = AF_INET;
  Addr.sin_port = htons(Port);
  Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(Socket, reinterpret_cast<sockaddr *>(&Addr), sizeof(Addr)) != 0) {
    std::string Message = socketError("connect");
    closeSocket(Socket);
    return Error(Message);
  }
  return std::make_unique<TcpTransport>(fromSocket(Socket));
}

Expected<size_t> TcpTransport::read(uint8_t *Data, size_t Size) {
  SocketType Socket = toSocket(Socket_);
  if (Socket == InvalidSocket || Socket_ == 0)
    return Error("tcp transport is closed");
  int Result = ::recv(Socket, reinterpret_cast<char *>(Data), static_cast<int>(Size), 0);
  if (Result < 0)
    return Error(socketError("recv"));
  return static_cast<size_t>(Result);
}

Error TcpTransport::write(const uint8_t *Data, size_t Size) {
  SocketType Socket = toSocket(Socket_);
  if (Socket == InvalidSocket || Socket_ == 0)
    return Error("tcp transport is closed");
  size_t Offset = 0;
  while (Offset < Size) {
    int Sent = ::send(Socket, reinterpret_cast<const char *>(Data + Offset),
                      static_cast<int>(Size - Offset), 0);
    if (Sent <= 0)
      return Error(socketError("send"));
    Offset += static_cast<size_t>(Sent);
  }
  return Error::success();
}

void TcpTransport::close() {
  SocketType Socket = toSocket(Socket_);
  if (Socket_ != 0 && Socket != InvalidSocket) {
    closeSocket(Socket);
    Socket_ = 0;
  }
}

TcpListener::TcpListener(uintptr_t Socket, uint16_t Port)
    : Socket_(Socket), Port_(Port) {}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener &&Other) noexcept
    : Socket_(Other.Socket_), Port_(Other.Port_) {
  Other.Socket_ = 0;
  Other.Port_ = 0;
}

TcpListener &TcpListener::operator=(TcpListener &&Other) noexcept {
  if (this != &Other) {
    close();
    Socket_ = Other.Socket_;
    Port_ = Other.Port_;
    Other.Socket_ = 0;
    Other.Port_ = 0;
  }
  return *this;
}

Expected<TcpListener> TcpListener::listenLoopback(uint16_t Port) {
  Error SocketReady = ensureSocketsReady();
  if (SocketReady)
    return SocketReady;

  SocketType Socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (Socket == InvalidSocket)
    return Error(socketError("socket"));

  int Reuse = 1;
  setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&Reuse),
             sizeof(Reuse));

  sockaddr_in Addr{};
  Addr.sin_family = AF_INET;
  Addr.sin_port = htons(Port);
  Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(Socket, reinterpret_cast<sockaddr *>(&Addr), sizeof(Addr)) != 0) {
    std::string Message = socketError("bind");
    closeSocket(Socket);
    return Error(Message);
  }
  if (::listen(Socket, 1) != 0) {
    std::string Message = socketError("listen");
    closeSocket(Socket);
    return Error(Message);
  }

  sockaddr_in Bound{};
#ifdef _WIN32
  int BoundSize = sizeof(Bound);
#else
  socklen_t BoundSize = sizeof(Bound);
#endif
  if (::getsockname(Socket, reinterpret_cast<sockaddr *>(&Bound), &BoundSize) != 0) {
    std::string Message = socketError("getsockname");
    closeSocket(Socket);
    return Error(Message);
  }
  return TcpListener(fromSocket(Socket), ntohs(Bound.sin_port));
}

Expected<std::unique_ptr<TcpTransport>> TcpListener::accept() {
  SocketType Socket = toSocket(Socket_);
  if (Socket == InvalidSocket || Socket_ == 0)
    return Error("tcp listener is closed");
  SocketType Client = ::accept(Socket, nullptr, nullptr);
  if (Client == InvalidSocket)
    return Error(socketError("accept"));
  return std::make_unique<TcpTransport>(fromSocket(Client));
}

void TcpListener::close() {
  SocketType Socket = toSocket(Socket_);
  if (Socket_ != 0 && Socket != InvalidSocket) {
    closeSocket(Socket);
    Socket_ = 0;
  }
}

} // namespace llvm::dtx
