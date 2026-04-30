#pragma once

#include "DebugHostServer.h.inc"

#include "LldbBackend.h"
#include "MixDeviceBridge.h"

namespace ycode::debughost {

class LifecycleProcessor final
    : public llvm::dtx::debughost::ILifecycleProcessor {
public:
  LifecycleProcessor(MixDeviceBridge &Bridge, LldbBackend &Backend);

  llvm::dtx::Expected<llvm::dtx::debughost::InitializeResult>
  initialize(const llvm::dtx::debughost::InitializeRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::ShutdownResult>
  shutdown(const llvm::dtx::debughost::ShutdownRequest &Request) override;

  bool shouldStop() const { return ShouldStop_; }

private:
  MixDeviceBridge &Bridge_;
  LldbBackend &Backend_;
  bool ShouldStop_ = false;
};

class SessionProcessor final : public llvm::dtx::debughost::ISessionProcessor {
public:
  explicit SessionProcessor(LldbBackend &Backend);

  llvm::dtx::Expected<llvm::dtx::debughost::SessionCreateResult>
  create(const llvm::dtx::debughost::SessionCreateRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::SessionCloseResult>
  close(const llvm::dtx::debughost::SessionCloseRequest &Request) override;

private:
  LldbBackend &Backend_;
};

class ExecutionProcessor final
    : public llvm::dtx::debughost::IExecutionProcessor {
public:
  explicit ExecutionProcessor(LldbBackend &Backend);

  llvm::dtx::Expected<llvm::dtx::debughost::LaunchResult>
  launch(const llvm::dtx::debughost::LaunchRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::AttachResult>
  attach(const llvm::dtx::debughost::AttachRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::ContinueResult>
  continueExecution(
      const llvm::dtx::debughost::ContinueRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::PauseResult>
  pause(const llvm::dtx::debughost::PauseRequest &Request) override;

private:
  LldbBackend &Backend_;
};

class BreakpointsProcessor final
    : public llvm::dtx::debughost::IBreakpointsProcessor {
public:
  explicit BreakpointsProcessor(LldbBackend &Backend);

  llvm::dtx::Expected<llvm::dtx::debughost::SetSourceBreakpointsResult>
  setSource(
      const llvm::dtx::debughost::SetSourceBreakpointsRequest &Request) override;

private:
  LldbBackend &Backend_;
};

class ThreadsProcessor final : public llvm::dtx::debughost::IThreadsProcessor {
public:
  explicit ThreadsProcessor(LldbBackend &Backend);

  llvm::dtx::Expected<llvm::dtx::debughost::ThreadsResult>
  list(const llvm::dtx::debughost::ThreadsRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::StackTraceResult>
  stackTrace(const llvm::dtx::debughost::StackTraceRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::ScopesResult>
  scopes(const llvm::dtx::debughost::ScopesRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::VariablesResult>
  variables(const llvm::dtx::debughost::VariablesRequest &Request) override;

private:
  LldbBackend &Backend_;
};

class ExpressionsProcessor final
    : public llvm::dtx::debughost::IExpressionsProcessor {
public:
  explicit ExpressionsProcessor(LldbBackend &Backend);

  llvm::dtx::Expected<llvm::dtx::debughost::EvaluateResult>
  evaluate(const llvm::dtx::debughost::EvaluateRequest &Request) override;

private:
  LldbBackend &Backend_;
};

class MemoryProcessor final : public llvm::dtx::debughost::IMemoryProcessor {
public:
  explicit MemoryProcessor(LldbBackend &Backend);

  llvm::dtx::Expected<llvm::dtx::debughost::ReadMemoryResult>
  read(const llvm::dtx::debughost::ReadMemoryRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::DisassembleResult>
  disassemble(
      const llvm::dtx::debughost::DisassembleRequest &Request) override;

private:
  LldbBackend &Backend_;
};

class DeviceProcessor final : public llvm::dtx::debughost::IDeviceProcessor {
public:
  explicit DeviceProcessor(MixDeviceBridge &Bridge);

  llvm::dtx::Expected<llvm::dtx::debughost::DeviceListResult>
  list(const llvm::dtx::debughost::DeviceListRequest &Request) override;

  llvm::dtx::Expected<llvm::dtx::debughost::DevicePrepareDebugResult>
  prepareDebug(
      const llvm::dtx::debughost::DevicePrepareDebugRequest &Request) override;

private:
  MixDeviceBridge &Bridge_;
};

class DebugHostServices {
public:
  explicit DebugHostServices(MixDeviceBridge &Bridge);

  void registerWith(llvm::dtx::debughost::DebugHostServer &Server);
  bool shouldStop() const { return Lifecycle_.shouldStop(); }

private:
  LldbBackend Backend_;
  LifecycleProcessor Lifecycle_;
  SessionProcessor Session_;
  ExecutionProcessor Execution_;
  BreakpointsProcessor Breakpoints_;
  ThreadsProcessor Threads_;
  ExpressionsProcessor Expressions_;
  MemoryProcessor Memory_;
  DeviceProcessor Device_;
};

} // namespace ycode::debughost
