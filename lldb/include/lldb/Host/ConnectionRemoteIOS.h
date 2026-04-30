//===-- ConnectionRemoteIOS.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_HOST_CONNECTIONREMOTEIOS_H
#define LLDB_HOST_CONNECTIONREMOTEIOS_H

#include "lldb/Utility/Connection.h"
#include "lldb/lldb-forward.h"

#include <mutex>
#include <string>

namespace lldb_private {

class Status;

class ConnectionRemoteIOS : public Connection {
public:
  ConnectionRemoteIOS();
  ~ConnectionRemoteIOS() override;

  lldb::ConnectionStatus Connect(llvm::StringRef url,
                                 Status *error_ptr) override;
  lldb::ConnectionStatus Disconnect(Status *error_ptr) override;
  bool IsConnected() const override;

  size_t Read(void *dst, size_t dst_len, const Timeout<std::micro> &timeout,
              lldb::ConnectionStatus &connection_status,
              Status *error_ptr) override;
  size_t Write(const void *dst, size_t dst_len,
               lldb::ConnectionStatus &connection_status,
               Status *error_ptr) override;

  std::string GetURI() override;
  bool InterruptRead() override;
  lldb::IOObjectSP GetReadObject() override;

private:
  void *m_device = nullptr;
  void *m_connection = nullptr;
  std::string m_uri;
  mutable std::mutex m_mutex;
};

} // namespace lldb_private

#endif // LLDB_HOST_CONNECTIONREMOTEIOS_H
