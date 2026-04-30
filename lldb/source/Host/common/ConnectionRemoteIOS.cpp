//===-- ConnectionRemoteIOS.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/ConnectionRemoteIOS.h"

#include "lldb/Utility/Status.h"
#include "lldb/Utility/Timeout.h"
#include "lldb/Utility/UriParser.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <limits>
#include <optional>

#if LLDB_ENABLE_MIX_DEVICE
#include "mix_device.h"
#endif

using namespace lldb;
using namespace lldb_private;

namespace {

void setError(Status *Error, const char *Message) {
  if (!Error || !Message)
    return;
  *Error = Status::FromErrorString(Message);
}

#if LLDB_ENABLE_MIX_DEVICE
ConnectionStatus toConnectionStatus(mix_lldb_connection_status Status) {
  switch (Status) {
  case mix_lldb_connection_status_success:
    return eConnectionStatusSuccess;
  case mix_lldb_connection_status_end_of_file:
    return eConnectionStatusEndOfFile;
  case mix_lldb_connection_status_error:
    return eConnectionStatusError;
  case mix_lldb_connection_status_timed_out:
    return eConnectionStatusTimedOut;
  case mix_lldb_connection_status_no_connection:
    return eConnectionStatusNoConnection;
  case mix_lldb_connection_status_lost_connection:
    return eConnectionStatusLostConnection;
  case mix_lldb_connection_status_interrupted:
    return eConnectionStatusInterrupted;
  }
  return eConnectionStatusError;
}

mix_device_t *asDevice(void *Ptr) { return static_cast<mix_device_t *>(Ptr); }

mix_lldb_debugger *asDebugger(void *Ptr) {
  return static_cast<mix_lldb_debugger *>(Ptr);
}

struct CallbackResult {
  Status *Error = nullptr;
  ConnectionStatus Status = eConnectionStatusSuccess;
};

void errorCallback(const char *Error, void *UserData) {
  auto *Result = static_cast<CallbackResult *>(UserData);
  if (!Error) {
    Result->Status = eConnectionStatusSuccess;
    return;
  }
  Result->Status = eConnectionStatusError;
  setError(Result->Error, Error);
}

void connectionStatusCallback(mix_lldb_connection_status Status,
                              const char *Error, void *UserData) {
  auto *Result = static_cast<CallbackResult *>(UserData);
  Result->Status = toConnectionStatus(Status);
  if (Result->Status != eConnectionStatusSuccess)
    setError(Result->Error, Error);
}

std::string makeMixDeviceUrl(const URI &Parsed) {
  std::string Scheme = Parsed.scheme.str();
  if (Scheme == "mix-ios")
    Scheme = "ios";
  else if (Scheme == "mix-android")
    Scheme = "android";

  std::string Url = Scheme + "://" + Parsed.hostname.str();
  return Url;
}

std::string makeDebugPort(const URI &Parsed) {
  if (Parsed.port)
    return std::to_string(*Parsed.port);
  llvm::StringRef Path = Parsed.path;
  if (Path.consume_front("/") && !Path.empty())
    return Path.str();
  return "";
}
#endif

} // namespace

ConnectionRemoteIOS::ConnectionRemoteIOS() = default;

ConnectionRemoteIOS::~ConnectionRemoteIOS() {
  std::lock_guard<std::mutex> Guard(m_mutex);
#if LLDB_ENABLE_MIX_DEVICE
  if (m_connection) {
    mix_lldb_connection_free(asDebugger(m_connection));
    m_connection = nullptr;
  }
  if (m_device) {
    mix_release_device(asDevice(m_device));
    m_device = nullptr;
  }
#endif
}

ConnectionStatus ConnectionRemoteIOS::Connect(llvm::StringRef Url,
                                              Status *Error) {
#if !LLDB_ENABLE_MIX_DEVICE
  setError(Error, "mix_device SDK is not linked into this LLDB build");
  return eConnectionStatusError;
#else
  std::optional<URI> Parsed = URI::Parse(Url);
  if (!Parsed) {
    setError(Error, "invalid mix device URL");
    return eConnectionStatusError;
  }

  llvm::StringRef Scheme = Parsed->scheme;
  if (Scheme != "ios" && Scheme != "android" && Scheme != "mix-ios" &&
      Scheme != "mix-android") {
    setError(Error, "unsupported mix device URL scheme");
    return eConnectionStatusError;
  }

  std::lock_guard<std::mutex> Guard(m_mutex);
  if (m_connection) {
    mix_lldb_connection_free(asDebugger(m_connection));
    m_connection = nullptr;
  }
  if (m_device) {
    mix_release_device(asDevice(m_device));
    m_device = nullptr;
  }

  CallbackResult Result{Error, eConnectionStatusError};
  mix_device_t *Device = nullptr;
  std::string DeviceUrl = makeMixDeviceUrl(*Parsed);
  mix_connect_device(DeviceUrl.c_str(), &Device, errorCallback, &Result);
  if (Result.Status != eConnectionStatusSuccess || !Device)
    return Result.Status;

  std::string DebugPort = makeDebugPort(*Parsed);
  mix_lldb_debugger *Connection = mix_device_create_lldb_connection(
      Device, DebugPort.c_str(), connectionStatusCallback, &Result);
  if (Result.Status != eConnectionStatusSuccess || !Connection) {
    mix_release_device(Device);
    if (Result.Status == eConnectionStatusSuccess) {
      Result.Status = eConnectionStatusError;
      setError(Error, "mix_device failed to create an LLDB connection");
    }
    return Result.Status;
  }

  m_device = Device;
  m_connection = Connection;
  m_uri = Url.str();
  return eConnectionStatusSuccess;
#endif
}

ConnectionStatus ConnectionRemoteIOS::Disconnect(Status *Error) {
#if !LLDB_ENABLE_MIX_DEVICE
  setError(Error, "mix_device SDK is not linked into this LLDB build");
  return eConnectionStatusNoConnection;
#else
  std::lock_guard<std::mutex> Guard(m_mutex);
  if (!m_connection)
    return eConnectionStatusNoConnection;

  CallbackResult Result{Error, eConnectionStatusNoConnection};
  mix_lldb_connection_disconnect(asDebugger(m_connection),
                                 connectionStatusCallback, &Result);
  mix_lldb_connection_free(asDebugger(m_connection));
  m_connection = nullptr;
  if (m_device) {
    mix_release_device(asDevice(m_device));
    m_device = nullptr;
  }
  m_uri.clear();
  return Result.Status;
#endif
}

bool ConnectionRemoteIOS::IsConnected() const {
#if !LLDB_ENABLE_MIX_DEVICE
  return false;
#else
  std::lock_guard<std::mutex> Guard(m_mutex);
  return m_connection &&
         mix_lldb_connection_is_connected(asDebugger(m_connection));
#endif
}

size_t ConnectionRemoteIOS::Read(void *Dst, size_t DstLen,
                                 const Timeout<std::micro> &Timeout,
                                 ConnectionStatus &ConnStatus, Status *Error) {
#if !LLDB_ENABLE_MIX_DEVICE
  ConnStatus = eConnectionStatusError;
  setError(Error, "mix_device SDK is not linked into this LLDB build");
  return 0;
#else
  std::lock_guard<std::mutex> Guard(m_mutex);
  if (!m_connection) {
    ConnStatus = eConnectionStatusNoConnection;
    return 0;
  }

  CallbackResult Result{Error, eConnectionStatusSuccess};
  uint64_t TimeoutUS = std::numeric_limits<uint64_t>::max();
  if (Timeout)
    TimeoutUS = static_cast<uint64_t>(Timeout->count());
  size_t Bytes =
      mix_lldb_connection_read(asDebugger(m_connection), Dst, DstLen,
                               TimeoutUS, connectionStatusCallback, &Result);
  ConnStatus = Result.Status;
  return Bytes;
#endif
}

size_t ConnectionRemoteIOS::Write(const void *Src, size_t SrcLen,
                                  ConnectionStatus &ConnStatus, Status *Error) {
#if !LLDB_ENABLE_MIX_DEVICE
  ConnStatus = eConnectionStatusError;
  setError(Error, "mix_device SDK is not linked into this LLDB build");
  return 0;
#else
  std::lock_guard<std::mutex> Guard(m_mutex);
  if (!m_connection) {
    ConnStatus = eConnectionStatusNoConnection;
    return 0;
  }

  CallbackResult Result{Error, eConnectionStatusSuccess};
  size_t Bytes =
      mix_lldb_connection_write(asDebugger(m_connection), Src, SrcLen,
                                connectionStatusCallback, &Result);
  ConnStatus = Result.Status;
  return Bytes;
#endif
}

std::string ConnectionRemoteIOS::GetURI() { return m_uri; }

bool ConnectionRemoteIOS::InterruptRead() {
#if !LLDB_ENABLE_MIX_DEVICE
  return false;
#else
  std::lock_guard<std::mutex> Guard(m_mutex);
  return m_connection &&
         mix_lldb_connection_interrupt_read(asDebugger(m_connection));
#endif
}

lldb::IOObjectSP ConnectionRemoteIOS::GetReadObject() {
  return lldb::IOObjectSP();
}
