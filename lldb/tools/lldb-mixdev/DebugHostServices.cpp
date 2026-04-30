#include "DebugHostServices.h"

using llvm::dtx::Expected;
using llvm::dtx::ns::Array;
using llvm::dtx::ns::Dict;
using llvm::dtx::ns::Object;

namespace ycode::debughost {

namespace {

Object boolean(bool Value) { return Object(Value); }

Object makeStatus(std::string Service, std::string Method, bool Success,
                  std::string Message, const Object &Request = Object()) {
  Dict D;
  D["success"] = boolean(Success);
  D["implemented"] = boolean(Success);
  D["service"] = Object(std::move(Service));
  D["method"] = Object(std::move(Method));
  D["message"] = Object(std::move(Message));
  if (!Request.isNull())
    D["request"] = Request;
  return Object(std::move(D));
}

} // namespace

LifecycleProcessor::LifecycleProcessor(MixDeviceBridge &Bridge,
                                       LldbBackend &Backend)
    : Bridge_(Bridge), Backend_(Backend) {}

Expected<llvm::dtx::debughost::InitializeResult>
LifecycleProcessor::initialize(
    const llvm::dtx::debughost::InitializeRequest &Request) {
  (void)Request;
  llvm::dtx::debughost::InitializeResult Result;
  Dict D;
  D["mixDevice"] = Bridge_.makeCapabilities();
  D["lldb"] = Backend_.makeCapabilities();
  D["supportsDebugHost"] = boolean(true);
  Result.Raw = Object(std::move(D));
  return Result;
}

Expected<llvm::dtx::debughost::ShutdownResult>
LifecycleProcessor::shutdown(
    const llvm::dtx::debughost::ShutdownRequest &Request) {
  (void)Request;
  llvm::dtx::debughost::ShutdownResult Result;
  Result.Raw = makeStatus("lifecycle", "shutdown", true,
                          "YCode.DebugHost shutdown requested");
  ShouldStop_ = true;
  return Result;
}

SessionProcessor::SessionProcessor(LldbBackend &Backend) : Backend_(Backend) {}

Expected<llvm::dtx::debughost::SessionCreateResult>
SessionProcessor::create(
    const llvm::dtx::debughost::SessionCreateRequest &Request) {
  llvm::dtx::debughost::SessionCreateResult Result;
  Result.Raw = Backend_.createSession(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::SessionCloseResult>
SessionProcessor::close(
    const llvm::dtx::debughost::SessionCloseRequest &Request) {
  llvm::dtx::debughost::SessionCloseResult Result;
  Result.Raw = Backend_.closeSession(Request.Raw);
  return Result;
}

ExecutionProcessor::ExecutionProcessor(LldbBackend &Backend)
    : Backend_(Backend) {}

Expected<llvm::dtx::debughost::LaunchResult>
ExecutionProcessor::launch(
    const llvm::dtx::debughost::LaunchRequest &Request) {
  llvm::dtx::debughost::LaunchResult Result;
  Result.Raw = Backend_.launch(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::AttachResult>
ExecutionProcessor::attach(
    const llvm::dtx::debughost::AttachRequest &Request) {
  llvm::dtx::debughost::AttachResult Result;
  Result.Raw = Backend_.attach(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::ContinueResult>
ExecutionProcessor::continueExecution(
    const llvm::dtx::debughost::ContinueRequest &Request) {
  llvm::dtx::debughost::ContinueResult Result;
  Result.Raw = Backend_.continueExecution(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::PauseResult>
ExecutionProcessor::pause(
    const llvm::dtx::debughost::PauseRequest &Request) {
  llvm::dtx::debughost::PauseResult Result;
  Result.Raw = Backend_.pause(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::StepInResult>
ExecutionProcessor::stepIn(
    const llvm::dtx::debughost::StepInRequest &Request) {
  llvm::dtx::debughost::StepInResult Result;
  Result.Raw = Backend_.stepIn(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::StepOverResult>
ExecutionProcessor::stepOver(
    const llvm::dtx::debughost::StepOverRequest &Request) {
  llvm::dtx::debughost::StepOverResult Result;
  Result.Raw = Backend_.stepOver(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::StepOutResult>
ExecutionProcessor::stepOut(
    const llvm::dtx::debughost::StepOutRequest &Request) {
  llvm::dtx::debughost::StepOutResult Result;
  Result.Raw = Backend_.stepOut(Request.Raw);
  return Result;
}

BreakpointsProcessor::BreakpointsProcessor(LldbBackend &Backend)
    : Backend_(Backend) {}

Expected<llvm::dtx::debughost::SetSourceBreakpointsResult>
BreakpointsProcessor::setSource(
    const llvm::dtx::debughost::SetSourceBreakpointsRequest &Request) {
  llvm::dtx::debughost::SetSourceBreakpointsResult Result;
  Result.Raw = Backend_.setSourceBreakpoints(Request.Raw);
  return Result;
}

ThreadsProcessor::ThreadsProcessor(LldbBackend &Backend) : Backend_(Backend) {}

Expected<llvm::dtx::debughost::ThreadsResult>
ThreadsProcessor::list(const llvm::dtx::debughost::ThreadsRequest &Request) {
  llvm::dtx::debughost::ThreadsResult Result;
  Result.Raw = Backend_.threads(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::StackTraceResult>
ThreadsProcessor::stackTrace(
    const llvm::dtx::debughost::StackTraceRequest &Request) {
  llvm::dtx::debughost::StackTraceResult Result;
  Result.Raw = Backend_.stackTrace(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::ScopesResult>
ThreadsProcessor::scopes(const llvm::dtx::debughost::ScopesRequest &Request) {
  llvm::dtx::debughost::ScopesResult Result;
  Result.Raw = Backend_.scopes(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::VariablesResult>
ThreadsProcessor::variables(
    const llvm::dtx::debughost::VariablesRequest &Request) {
  llvm::dtx::debughost::VariablesResult Result;
  Result.Raw = Backend_.variables(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::RegistersResult>
ThreadsProcessor::registers(
    const llvm::dtx::debughost::RegistersRequest &Request) {
  llvm::dtx::debughost::RegistersResult Result;
  Result.Raw = Backend_.registers(Request.Raw);
  return Result;
}

ExpressionsProcessor::ExpressionsProcessor(LldbBackend &Backend)
    : Backend_(Backend) {}

Expected<llvm::dtx::debughost::EvaluateResult>
ExpressionsProcessor::evaluate(
    const llvm::dtx::debughost::EvaluateRequest &Request) {
  llvm::dtx::debughost::EvaluateResult Result;
  Result.Raw = Backend_.evaluate(Request.Raw);
  return Result;
}

MemoryProcessor::MemoryProcessor(LldbBackend &Backend) : Backend_(Backend) {}

Expected<llvm::dtx::debughost::ReadMemoryResult>
MemoryProcessor::read(const llvm::dtx::debughost::ReadMemoryRequest &Request) {
  llvm::dtx::debughost::ReadMemoryResult Result;
  Result.Raw = Backend_.readMemory(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::DisassembleResult>
MemoryProcessor::disassemble(
    const llvm::dtx::debughost::DisassembleRequest &Request) {
  llvm::dtx::debughost::DisassembleResult Result;
  Result.Raw = Backend_.disassemble(Request.Raw);
  return Result;
}

ModulesProcessor::ModulesProcessor(LldbBackend &Backend) : Backend_(Backend) {}

Expected<llvm::dtx::debughost::ModulesResult>
ModulesProcessor::list(
    const llvm::dtx::debughost::ModulesRequest &Request) {
  llvm::dtx::debughost::ModulesResult Result;
  Result.Raw = Backend_.modules(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::ReloadSymbolsResult>
ModulesProcessor::reloadSymbols(
    const llvm::dtx::debughost::ReloadSymbolsRequest &Request) {
  llvm::dtx::debughost::ReloadSymbolsResult Result;
  Result.Raw = Backend_.reloadSymbols(Request.Raw);
  return Result;
}

DeviceProcessor::DeviceProcessor(MixDeviceBridge &Bridge) : Bridge_(Bridge) {}

Expected<llvm::dtx::debughost::DeviceListResult>
DeviceProcessor::list(const llvm::dtx::debughost::DeviceListRequest &Request) {
  (void)Request;
  llvm::dtx::debughost::DeviceListResult Result;
  Result.Raw = Bridge_.listDevices();
  return Result;
}

Expected<llvm::dtx::debughost::DevicePrepareDebugResult>
DeviceProcessor::prepareDebug(
    const llvm::dtx::debughost::DevicePrepareDebugRequest &Request) {
  llvm::dtx::debughost::DevicePrepareDebugResult Result;
  Result.Raw = Bridge_.prepareDebug(Request.Raw);
  return Result;
}

DeviceSymbolsProcessor::DeviceSymbolsProcessor(MixDeviceBridge &Bridge)
    : Bridge_(Bridge) {}

Expected<llvm::dtx::debughost::DeviceSymbolsStatusResult>
DeviceSymbolsProcessor::status(
    const llvm::dtx::debughost::DeviceSymbolsStatusRequest &Request) {
  llvm::dtx::debughost::DeviceSymbolsStatusResult Result;
  Result.Raw = Bridge_.deviceSymbolsStatus(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::DeviceSymbolsValidateResult>
DeviceSymbolsProcessor::validate(
    const llvm::dtx::debughost::DeviceSymbolsValidateRequest &Request) {
  llvm::dtx::debughost::DeviceSymbolsValidateResult Result;
  Result.Raw = Bridge_.deviceSymbolsValidate(Request.Raw);
  return Result;
}

Expected<llvm::dtx::debughost::DeviceSymbolsPrefetchResult>
DeviceSymbolsProcessor::prefetch(
    const llvm::dtx::debughost::DeviceSymbolsPrefetchRequest &Request) {
  llvm::dtx::debughost::DeviceSymbolsPrefetchResult Result;
  Result.Raw = Bridge_.deviceSymbolsPrefetch(Request.Raw, EventSink_);
  return Result;
}

DebugHostServices::DebugHostServices(MixDeviceBridge &Bridge)
    : Lifecycle_(Bridge, Backend_), Session_(Backend_), Execution_(Backend_),
      Breakpoints_(Backend_), Threads_(Backend_), Expressions_(Backend_),
      Memory_(Backend_), Modules_(Backend_), Device_(Bridge),
      DeviceSymbols_(Bridge) {}

void DebugHostServices::registerWith(
    llvm::dtx::debughost::DebugHostServer &Server) {
  Server.setLifecycleProcessor(&Lifecycle_);
  Server.setSessionProcessor(&Session_);
  Server.setExecutionProcessor(&Execution_);
  Server.setBreakpointsProcessor(&Breakpoints_);
  Server.setThreadsProcessor(&Threads_);
  Server.setExpressionsProcessor(&Expressions_);
  Server.setMemoryProcessor(&Memory_);
  Server.setModulesProcessor(&Modules_);
  Server.setDeviceProcessor(&Device_);
  Server.setDeviceSymbolsProcessor(&DeviceSymbols_);
}

void DebugHostServices::setEventSink(DebugHostEventSink Sink) {
  Backend_.setEventSink(Sink);
  DeviceSymbols_.setEventSink(std::move(Sink));
}

} // namespace ycode::debughost
