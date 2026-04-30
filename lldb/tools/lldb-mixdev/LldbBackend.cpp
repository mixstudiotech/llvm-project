#include "LldbBackend.h"

#include "lldb/API/SBAddress.h"
#include "lldb/API/SBAttachInfo.h"
#include "lldb/API/SBBreakpoint.h"
#include "lldb/API/SBError.h"
#include "lldb/API/SBFileSpec.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBInstruction.h"
#include "lldb/API/SBInstructionList.h"
#include "lldb/API/SBLaunchInfo.h"
#include "lldb/API/SBListener.h"
#include "lldb/API/SBLineEntry.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBTarget.h"
#include "lldb/API/SBThread.h"
#include "lldb/API/SBValue.h"
#include "lldb/API/SBValueList.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>

using llvm::dtx::ns::Array;
using llvm::dtx::ns::Data;
using llvm::dtx::ns::Dict;
using llvm::dtx::ns::Object;

namespace ycode::debughost {

namespace {

Object boolean(bool Value) { return Object(Value); }

std::string text(const char *Value) { return Value ? Value : ""; }

const Dict *asDict(const Object &Obj) {
  return std::get_if<Dict>(&Obj.Value);
}

const Array *asArray(const Object &Obj) {
  return std::get_if<Array>(&Obj.Value);
}

const Object *lookup(const Dict &D, const std::string &Key) {
  auto It = D.find(Key);
  if (It == D.end())
    return nullptr;
  return &It->second;
}

std::string getString(const Dict &D, const std::string &Key,
                      std::string Default = "") {
  const Object *Value = lookup(D, Key);
  if (!Value)
    return Default;
  if (const auto *S = std::get_if<std::string>(&Value->Value))
    return *S;
  return Default;
}

bool getBool(const Dict &D, const std::string &Key, bool Default = false) {
  const Object *Value = lookup(D, Key);
  if (!Value)
    return Default;
  if (const auto *B = std::get_if<bool>(&Value->Value))
    return *B;
  return Default;
}

uint64_t parseUnsignedString(const std::string &Text, uint64_t Default = 0) {
  if (Text.empty())
    return Default;
  char *End = nullptr;
  const int Base = Text.rfind("0x", 0) == 0 || Text.rfind("0X", 0) == 0 ? 16 : 10;
  uint64_t Value = std::strtoull(Text.c_str(), &End, Base);
  return End && *End == '\0' ? Value : Default;
}

uint64_t getUnsigned(const Dict &D, const std::string &Key,
                     uint64_t Default = 0) {
  const Object *Value = lookup(D, Key);
  if (!Value)
    return Default;
  if (const auto *U = std::get_if<uint64_t>(&Value->Value))
    return *U;
  if (const auto *I = std::get_if<int64_t>(&Value->Value))
    return *I >= 0 ? static_cast<uint64_t>(*I) : Default;
  if (const auto *S = std::get_if<std::string>(&Value->Value))
    return parseUnsignedString(*S, Default);
  return Default;
}

std::string getSessionId(const Object &Request) {
  const Dict *D = asDict(Request);
  return D ? getString(*D, "sessionId") : "";
}

Object makeStatus(std::string Service, std::string Method, bool Success,
                  std::string Message, const Object &Request = Object()) {
  Dict D;
  D["success"] = boolean(Success);
  D["implemented"] = boolean(true);
  D["service"] = Object(std::move(Service));
  D["method"] = Object(std::move(Method));
  D["message"] = Object(std::move(Message));
  if (!Request.isNull())
    D["request"] = Request;
  return Object(std::move(D));
}

Object makeFailure(std::string Service, std::string Method, std::string Message,
                   const Object &Request = Object()) {
  return makeStatus(std::move(Service), std::move(Method), false,
                    std::move(Message), Request);
}

std::string errorString(const lldb::SBError &Error) {
  if (const char *Message = Error.GetCString())
    return Message;
  return "LLDB operation failed";
}

std::string stateName(lldb::StateType State) {
  return text(lldb::SBDebugger::StateAsCString(State));
}

std::string stopReasonName(lldb::StopReason Reason) {
  switch (Reason) {
  case lldb::eStopReasonInvalid:
    return "invalid";
  case lldb::eStopReasonNone:
    return "none";
  case lldb::eStopReasonTrace:
    return "trace";
  case lldb::eStopReasonBreakpoint:
    return "breakpoint";
  case lldb::eStopReasonWatchpoint:
    return "watchpoint";
  case lldb::eStopReasonSignal:
    return "signal";
  case lldb::eStopReasonException:
    return "exception";
  case lldb::eStopReasonExec:
    return "exec";
  case lldb::eStopReasonPlanComplete:
    return "planComplete";
  case lldb::eStopReasonThreadExiting:
    return "threadExiting";
  case lldb::eStopReasonInstrumentation:
    return "instrumentation";
  case lldb::eStopReasonProcessorTrace:
    return "processorTrace";
  case lldb::eStopReasonFork:
    return "fork";
  case lldb::eStopReasonVFork:
    return "vfork";
  case lldb::eStopReasonVForkDone:
    return "vforkDone";
  case lldb::eStopReasonInterrupt:
    return "interrupt";
  case lldb::eStopReasonHistoryBoundary:
    return "historyBoundary";
  }
  return "unknown";
}

std::string pathFromFileSpec(lldb::SBFileSpec File) {
  char Buffer[4096] = {};
  if (File.IsValid() && File.GetPath(Buffer, sizeof(Buffer)) > 0)
    return Buffer;
  return "";
}

Object lineEntryToObject(lldb::SBLineEntry Line) {
  Dict D;
  if (!Line.IsValid())
    return Object(std::move(D));
  D["path"] = Object(pathFromFileSpec(Line.GetFileSpec()));
  D["line"] = Object(static_cast<uint64_t>(Line.GetLine()));
  D["column"] = Object(static_cast<uint64_t>(Line.GetColumn()));
  return Object(std::move(D));
}

Object processToObject(lldb::SBProcess Process) {
  Dict D;
  const bool Valid = Process.IsValid();
  D["valid"] = boolean(Valid);
  if (Valid) {
    D["pid"] = Object(static_cast<uint64_t>(Process.GetProcessID()));
    D["state"] = Object(stateName(Process.GetState()));
  }
  return Object(std::move(D));
}

Object threadToObject(lldb::SBThread Thread) {
  Dict D;
  D["id"] = Object(static_cast<uint64_t>(Thread.GetThreadID()));
  D["indexId"] = Object(static_cast<uint64_t>(Thread.GetIndexID()));
  D["name"] = Object(text(Thread.GetName()));
  D["queueName"] = Object(text(Thread.GetQueueName()));
  D["stopReason"] = Object(stopReasonName(Thread.GetStopReason()));
  D["frameCount"] = Object(static_cast<uint64_t>(Thread.GetNumFrames()));
  return Object(std::move(D));
}

Object frameToObject(lldb::SBFrame Frame, lldb::tid_t ThreadID,
                     uint32_t FrameIndex) {
  Dict D;
  const char *Name = Frame.GetDisplayFunctionName();
  if (!Name)
    Name = Frame.GetFunctionName();
  D["id"] = Object(static_cast<uint64_t>(Frame.GetFrameID()));
  D["threadId"] = Object(static_cast<uint64_t>(ThreadID));
  D["frameIndex"] = Object(static_cast<uint64_t>(FrameIndex));
  D["name"] = Object(text(Name));
  D["pc"] = Object(static_cast<uint64_t>(Frame.GetPC()));
  D["source"] = lineEntryToObject(Frame.GetLineEntry());
  return Object(std::move(D));
}

Object valueToObject(lldb::SBValue Value, uint64_t VariablesReference) {
  Dict D;
  D["name"] = Object(text(Value.GetName()));
  D["type"] = Object(text(Value.GetTypeName()));
  D["value"] = Object(text(Value.GetValue()));
  D["summary"] = Object(text(Value.GetSummary()));
  D["variablesReference"] = Object(VariablesReference);
  D["childCount"] = Object(static_cast<uint64_t>(Value.GetNumChildren(256)));
  return Object(std::move(D));
}

std::vector<lldb::SBValue> listValues(lldb::SBValueList Values) {
  std::vector<lldb::SBValue> Result;
  const uint32_t Count = Values.GetSize();
  Result.reserve(Count);
  for (uint32_t I = 0; I < Count; ++I) {
    lldb::SBValue V = Values.GetValueAtIndex(I);
    if (V.IsValid())
      Result.push_back(V);
  }
  return Result;
}

std::vector<std::string> getStringArray(const Dict &D, const std::string &Key) {
  std::vector<std::string> Result;
  const Object *Obj = lookup(D, Key);
  if (!Obj)
    return Result;
  const Array *A = asArray(*Obj);
  if (!A)
    return Result;
  for (const Object &Item : *A) {
    if (const auto *S = std::get_if<std::string>(&Item.Value))
      Result.push_back(*S);
  }
  return Result;
}

bool isMixConnectUrl(llvm::StringRef URL) {
  return URL.starts_with("ios://") || URL.starts_with("android://") ||
         URL.starts_with("mix-ios://") || URL.starts_with("mix-android://");
}

std::string makeMixConnectUrl(const Dict &D) {
  std::string URL = getString(D, "connectUrl");
  if (!URL.empty())
    return URL;

  std::string Platform = getString(D, "platform");
  std::string DeviceID = getString(D, "deviceId");
  if (DeviceID.empty())
    DeviceID = getString(D, "id");
  if (Platform.empty() || DeviceID.empty())
    return "";

  if (Platform != "ios" && Platform != "android" && Platform != "mix-ios" &&
      Platform != "mix-android")
    return "";

  URL = Platform + "://" + DeviceID;
  uint64_t Port = getUnsigned(D, "debugPort", getUnsigned(D, "port"));
  if (Port)
    URL += ":" + std::to_string(Port);
  return URL;
}

bool isRemoteMixRequest(const Dict &D) {
  std::string URL = makeMixConnectUrl(D);
  return !URL.empty() && isMixConnectUrl(URL);
}

lldb::SBThread findThread(lldb::SBProcess Process, uint64_t ThreadID) {
  if (!Process.IsValid())
    return lldb::SBThread();
  if (ThreadID)
    return Process.GetThreadByID(static_cast<lldb::tid_t>(ThreadID));
  return Process.GetNumThreads() ? Process.GetThreadAtIndex(0) : lldb::SBThread();
}

lldb::SBFrame findFrame(lldb::SBProcess Process, const Dict &Request) {
  const uint64_t ThreadID = getUnsigned(Request, "threadId");
  const uint64_t FrameIndex =
      getUnsigned(Request, "frameIndex", getUnsigned(Request, "frameId"));
  lldb::SBThread Thread = findThread(Process, ThreadID);
  if (!Thread.IsValid())
    return lldb::SBFrame();
  return Thread.GetFrameAtIndex(static_cast<uint32_t>(FrameIndex));
}

std::string sourcePathFromRequest(const Dict &Request) {
  std::string Path = getString(Request, "path");
  if (!Path.empty())
    return Path;
  const Object *SourceObj = lookup(Request, "source");
  if (!SourceObj)
    return "";
  const Dict *Source = asDict(*SourceObj);
  if (!Source)
    return "";
  Path = getString(*Source, "path");
  if (!Path.empty())
    return Path;
  return getString(*Source, "sourceReference");
}

Object connectRemote(lldb::SBDebugger &Debugger, lldb::SBTarget &Target,
                     lldb::SBProcess &Process, const std::string &SessionId,
                     const Dict &Req, const Object &Request,
                     const char *Method, const std::string &Program) {
  std::string URL = makeMixConnectUrl(Req);
  if (URL.empty())
    return makeFailure("execution", Method, "missing mix connect URL", Request);

  lldb::SBError Error;
  if (!Program.empty())
    Target = Debugger.CreateTarget(Program.c_str(), nullptr, nullptr, true,
                                   Error);
  else
    Target = Debugger.CreateTarget(nullptr);
  if (Error.Fail() || !Target.IsValid())
    return makeFailure("execution", Method, errorString(Error), Request);

  lldb::SBListener Listener = Debugger.GetListener();
  Error.Clear();
  Process = Target.ConnectRemote(Listener, URL.c_str(), "gdb-remote", Error);
  if (Error.Fail() || !Process.IsValid())
    return makeFailure("execution", Method, errorString(Error), Request);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object(Method);
  D["sessionId"] = Object(SessionId);
  D["remote"] = boolean(true);
  D["connectUrl"] = Object(std::move(URL));
  D["process"] = processToObject(Process);
  return Object(std::move(D));
}

} // namespace

LldbBackend::LldbBackend() {
  lldb::SBError Error = lldb::SBDebugger::InitializeWithErrorHandling();
  Initialized_ = Error.Success();
}

LldbBackend::~LldbBackend() {
  for (auto &Entry : Sessions_)
    lldb::SBDebugger::Destroy(Entry.second.Debugger);
  Sessions_.clear();
  if (Initialized_)
    lldb::SBDebugger::Terminate();
}

Object LldbBackend::makeCapabilities() const {
  Dict D;
  D["supportsLldb"] = boolean(Initialized_);
  D["supportsSession"] = boolean(Initialized_);
  D["supportsLocalLaunch"] = boolean(Initialized_);
  D["supportsLocalAttach"] = boolean(Initialized_);
  D["supportsSourceBreakpoints"] = boolean(Initialized_);
  D["supportsThreads"] = boolean(Initialized_);
  D["supportsStackTrace"] = boolean(Initialized_);
  D["supportsEvaluate"] = boolean(Initialized_);
  D["supportsReadMemory"] = boolean(Initialized_);
  D["supportsDisassemble"] = boolean(Initialized_);
  D["lldbVersion"] = Object(text(lldb::SBDebugger::GetVersionString()));
  return Object(std::move(D));
}

Object LldbBackend::createSession(const Object &Request) {
  if (!Initialized_)
    return makeFailure("session", "create", "LLDB failed to initialize",
                       Request);

  Session S;
  S.Id = "session-" + std::to_string(NextSessionId_++);
  S.Debugger = lldb::SBDebugger::Create(false);
  if (!S.Debugger.IsValid())
    return makeFailure("session", "create", "SBDebugger::Create failed",
                       Request);
  S.Debugger.SetAsync(true);

  const Dict *Req = asDict(Request);
  if (Req) {
    std::string Platform = getString(*Req, "platform");
    if (!Platform.empty())
      S.Debugger.SetCurrentPlatform(Platform.c_str());
  }

  std::string Id = S.Id;
  Sessions_.emplace(Id, S);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("session");
  D["method"] = Object("create");
  D["sessionId"] = Object(Id);
  D["backend"] = Object("lldb");
  D["capabilities"] = makeCapabilities();
  return Object(std::move(D));
}

Object LldbBackend::closeSession(const Object &Request) {
  std::string Id = getSessionId(Request);
  if (Id.empty())
    return makeFailure("session", "close", "missing sessionId", Request);

  auto It = Sessions_.find(Id);
  if (It == Sessions_.end())
    return makeFailure("session", "close", "unknown sessionId", Request);

  lldb::SBDebugger::Destroy(It->second.Debugger);
  Sessions_.erase(It);
  return makeStatus("session", "close", true, "session closed", Request);
}

Object LldbBackend::launch(const Object &Request) {
  Session *S = findSession(Request);
  if (!S)
    return makeFailure("execution", "launch", "unknown or missing sessionId",
                       Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("execution", "launch", "request must be a dictionary",
                       Request);

  std::string Program = getString(*Req, "executablePath");
  if (Program.empty())
    Program = getString(*Req, "program");
  if (Program.empty())
    return makeFailure("execution", "launch",
                       "missing executablePath or program", Request);

  if (isRemoteMixRequest(*Req))
    return connectRemote(S->Debugger, S->Target, S->Process, S->Id, *Req,
                         Request, "launch", Program);

  lldb::SBError Error;
  S->Target = S->Debugger.CreateTarget(Program.c_str(), nullptr, nullptr, true,
                                       Error);
  if (Error.Fail() || !S->Target.IsValid())
    return makeFailure("execution", "launch", errorString(Error), Request);

  std::vector<std::string> ArgStorage = getStringArray(*Req, "args");
  std::vector<const char *> Args;
  Args.reserve(ArgStorage.size() + 2);
  Args.push_back(Program.c_str());
  for (const std::string &Arg : ArgStorage)
    Args.push_back(Arg.c_str());
  Args.push_back(nullptr);

  lldb::SBLaunchInfo LaunchInfo(Args.data());
  std::string WorkingDirectory = getString(*Req, "workingDirectory");
  if (!WorkingDirectory.empty())
    LaunchInfo.SetWorkingDirectory(WorkingDirectory.c_str());
  if (getBool(*Req, "stopAtEntry"))
    LaunchInfo.SetLaunchFlags(LaunchInfo.GetLaunchFlags() |
                              lldb::eLaunchFlagStopAtEntry);

  Error.Clear();
  S->Process = S->Target.Launch(LaunchInfo, Error);
  if (Error.Fail() || !S->Process.IsValid())
    return makeFailure("execution", "launch", errorString(Error), Request);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("launch");
  D["sessionId"] = Object(S->Id);
  D["process"] = processToObject(S->Process);
  return Object(std::move(D));
}

Object LldbBackend::attach(const Object &Request) {
  Session *S = findSession(Request);
  if (!S)
    return makeFailure("execution", "attach", "unknown or missing sessionId",
                       Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("execution", "attach", "request must be a dictionary",
                       Request);

  std::string Program = getString(*Req, "executablePath");
  if (isRemoteMixRequest(*Req))
    return connectRemote(S->Debugger, S->Target, S->Process, S->Id, *Req,
                         Request, "attach", Program);

  const uint64_t Pid = getUnsigned(*Req, "pid");
  if (!Pid)
    return makeFailure("execution", "attach", "missing pid", Request);

  lldb::SBError Error;
  if (!Program.empty())
    S->Target = S->Debugger.CreateTarget(Program.c_str(), nullptr, nullptr,
                                         true, Error);
  else
    S->Target = S->Debugger.CreateTarget(nullptr);
  if (!S->Target.IsValid())
    return makeFailure("execution", "attach", errorString(Error), Request);

  lldb::SBAttachInfo AttachInfo(static_cast<lldb::pid_t>(Pid));
  Error.Clear();
  S->Process = S->Target.Attach(AttachInfo, Error);
  if (Error.Fail() || !S->Process.IsValid())
    return makeFailure("execution", "attach", errorString(Error), Request);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("attach");
  D["sessionId"] = Object(S->Id);
  D["process"] = processToObject(S->Process);
  return Object(std::move(D));
}

Object LldbBackend::continueExecution(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("execution", "continue",
                       "unknown sessionId or no process", Request);
  lldb::SBError Error = S->Process.Continue();
  if (Error.Fail())
    return makeFailure("execution", "continue", errorString(Error), Request);
  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("continue");
  D["sessionId"] = Object(S->Id);
  D["process"] = processToObject(S->Process);
  return Object(std::move(D));
}

Object LldbBackend::pause(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("execution", "pause", "unknown sessionId or no process",
                       Request);
  lldb::SBError Error = S->Process.Stop();
  if (Error.Fail())
    return makeFailure("execution", "pause", errorString(Error), Request);
  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("pause");
  D["sessionId"] = Object(S->Id);
  D["process"] = processToObject(S->Process);
  return Object(std::move(D));
}

Object LldbBackend::setSourceBreakpoints(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Target.IsValid())
    return makeFailure("breakpoints", "setSource",
                       "unknown sessionId or no target", Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("breakpoints", "setSource",
                       "request must be a dictionary", Request);

  std::string Path = sourcePathFromRequest(*Req);
  if (Path.empty())
    return makeFailure("breakpoints", "setSource", "missing source path",
                       Request);

  Array Breakpoints;
  const Object *BPObj = lookup(*Req, "breakpoints");
  const Array *Requested = BPObj ? asArray(*BPObj) : nullptr;
  if (Requested) {
    for (const Object &Item : *Requested) {
      const Dict *BPReq = asDict(Item);
      if (!BPReq)
        continue;
      const uint64_t Line = getUnsigned(*BPReq, "line");
      if (!Line)
        continue;
      lldb::SBBreakpoint BP =
          S->Target.BreakpointCreateByLocation(Path.c_str(),
                                               static_cast<uint32_t>(Line));
      Dict D;
      D["id"] = Object(static_cast<uint64_t>(BP.GetID()));
      D["line"] = Object(Line);
      D["verified"] = boolean(BP.IsValid() && BP.GetNumLocations() > 0);
      D["locationCount"] = Object(static_cast<uint64_t>(BP.GetNumLocations()));
      Breakpoints.emplace_back(std::move(D));
    }
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("breakpoints");
  D["method"] = Object("setSource");
  D["sessionId"] = Object(S->Id);
  D["breakpoints"] = Object(std::move(Breakpoints));
  return Object(std::move(D));
}

Object LldbBackend::threads(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("threads", "list", "unknown sessionId or no process",
                       Request);

  Array Threads;
  const uint32_t Count = S->Process.GetNumThreads();
  for (uint32_t I = 0; I < Count; ++I) {
    lldb::SBThread Thread = S->Process.GetThreadAtIndex(I);
    if (Thread.IsValid())
      Threads.push_back(threadToObject(Thread));
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("threads");
  D["method"] = Object("list");
  D["sessionId"] = Object(S->Id);
  D["threads"] = Object(std::move(Threads));
  return Object(std::move(D));
}

Object LldbBackend::stackTrace(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("threads", "stackTrace",
                       "unknown sessionId or no process", Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("threads", "stackTrace",
                       "request must be a dictionary", Request);

  const uint64_t ThreadID = getUnsigned(*Req, "threadId");
  lldb::SBThread Thread = findThread(S->Process, ThreadID);
  if (!Thread.IsValid())
    return makeFailure("threads", "stackTrace", "thread not found", Request);

  const uint64_t Start = getUnsigned(*Req, "startFrame");
  uint64_t Levels = getUnsigned(*Req, "levels",
                                std::numeric_limits<uint32_t>::max());
  const uint32_t FrameCount = Thread.GetNumFrames();
  Array Frames;
  for (uint64_t I = Start; I < FrameCount && Levels; ++I, --Levels) {
    lldb::SBFrame Frame = Thread.GetFrameAtIndex(static_cast<uint32_t>(I));
    if (Frame.IsValid())
      Frames.push_back(frameToObject(Frame, Thread.GetThreadID(),
                                     static_cast<uint32_t>(I)));
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("threads");
  D["method"] = Object("stackTrace");
  D["sessionId"] = Object(S->Id);
  D["totalFrames"] = Object(static_cast<uint64_t>(FrameCount));
  D["stackFrames"] = Object(std::move(Frames));
  return Object(std::move(D));
}

Object LldbBackend::scopes(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("threads", "scopes", "unknown sessionId or no process",
                       Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("threads", "scopes", "request must be a dictionary",
                       Request);
  lldb::SBFrame Frame = findFrame(S->Process, *Req);
  if (!Frame.IsValid())
    return makeFailure("threads", "scopes", "frame not found", Request);

  std::vector<lldb::SBValue> Args =
      listValues(Frame.GetVariables(true, false, false, true));
  std::vector<lldb::SBValue> Locals =
      listValues(Frame.GetVariables(false, true, false, true));

  Array Scopes;
  Dict ArgsScope;
  ArgsScope["name"] = Object("Arguments");
  ArgsScope["variablesReference"] = Object(addVariableRef(*S, std::move(Args)));
  ArgsScope["expensive"] = boolean(false);
  Scopes.emplace_back(std::move(ArgsScope));

  Dict LocalsScope;
  LocalsScope["name"] = Object("Locals");
  LocalsScope["variablesReference"] =
      Object(addVariableRef(*S, std::move(Locals)));
  LocalsScope["expensive"] = boolean(false);
  Scopes.emplace_back(std::move(LocalsScope));

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("threads");
  D["method"] = Object("scopes");
  D["sessionId"] = Object(S->Id);
  D["scopes"] = Object(std::move(Scopes));
  return Object(std::move(D));
}

Object LldbBackend::variables(const Object &Request) {
  Session *S = findSession(Request);
  if (!S)
    return makeFailure("threads", "variables", "unknown or missing sessionId",
                       Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("threads", "variables", "request must be a dictionary",
                       Request);

  const uint64_t Ref = getUnsigned(*Req, "variablesReference");
  auto It = S->VariableRefs.find(Ref);
  if (It == S->VariableRefs.end())
    return makeFailure("threads", "variables", "unknown variablesReference",
                       Request);

  Array Variables;
  for (lldb::SBValue Value : It->second) {
    std::vector<lldb::SBValue> Children;
    const uint32_t ChildCount = std::min<uint32_t>(Value.GetNumChildren(256), 256);
    Children.reserve(ChildCount);
    for (uint32_t I = 0; I < ChildCount; ++I) {
      lldb::SBValue Child = Value.GetChildAtIndex(I);
      if (Child.IsValid())
        Children.push_back(Child);
    }
    const uint64_t ChildRef =
        Children.empty() ? 0 : addVariableRef(*S, std::move(Children));
    Variables.push_back(valueToObject(Value, ChildRef));
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("threads");
  D["method"] = Object("variables");
  D["sessionId"] = Object(S->Id);
  D["variables"] = Object(std::move(Variables));
  return Object(std::move(D));
}

Object LldbBackend::evaluate(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("expressions", "evaluate",
                       "unknown sessionId or no process", Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("expressions", "evaluate",
                       "request must be a dictionary", Request);
  std::string Expression = getString(*Req, "expression");
  if (Expression.empty())
    return makeFailure("expressions", "evaluate", "missing expression",
                       Request);

  lldb::SBFrame Frame = findFrame(S->Process, *Req);
  if (!Frame.IsValid())
    return makeFailure("expressions", "evaluate", "frame not found", Request);

  lldb::SBValue Value = Frame.EvaluateExpression(Expression.c_str());
  if (!Value.IsValid())
    return makeFailure("expressions", "evaluate", "expression failed",
                       Request);

  std::vector<lldb::SBValue> Children;
  const uint32_t ChildCount = std::min<uint32_t>(Value.GetNumChildren(256), 256);
  for (uint32_t I = 0; I < ChildCount; ++I) {
    lldb::SBValue Child = Value.GetChildAtIndex(I);
    if (Child.IsValid())
      Children.push_back(Child);
  }
  const uint64_t Ref =
      Children.empty() ? 0 : addVariableRef(*S, std::move(Children));

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("expressions");
  D["method"] = Object("evaluate");
  D["sessionId"] = Object(S->Id);
  D["result"] = valueToObject(Value, Ref);
  return Object(std::move(D));
}

Object LldbBackend::readMemory(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("memory", "read", "unknown sessionId or no process",
                       Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("memory", "read", "request must be a dictionary",
                       Request);
  const uint64_t Address = getUnsigned(*Req, "address");
  const uint64_t Count = getUnsigned(*Req, "count", getUnsigned(*Req, "length"));
  if (!Address || !Count)
    return makeFailure("memory", "read", "missing address or count", Request);

  Data Bytes(static_cast<size_t>(Count));
  lldb::SBError Error;
  const size_t Read =
      S->Process.ReadMemory(static_cast<lldb::addr_t>(Address), Bytes.data(),
                            Bytes.size(), Error);
  if (Error.Fail())
    return makeFailure("memory", "read", errorString(Error), Request);
  Bytes.resize(Read);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("memory");
  D["method"] = Object("read");
  D["sessionId"] = Object(S->Id);
  D["address"] = Object(Address);
  D["bytesRead"] = Object(static_cast<uint64_t>(Read));
  D["data"] = Object(std::move(Bytes));
  return Object(std::move(D));
}

Object LldbBackend::disassemble(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Target.IsValid())
    return makeFailure("memory", "disassemble",
                       "unknown sessionId or no target", Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("memory", "disassemble",
                       "request must be a dictionary", Request);
  const uint64_t Address = getUnsigned(*Req, "address");
  const uint64_t Count = getUnsigned(*Req, "count", 32);
  if (!Address)
    return makeFailure("memory", "disassemble", "missing address", Request);

  lldb::SBAddress SBAddr =
      S->Target.ResolveLoadAddress(static_cast<lldb::addr_t>(Address));
  if (!SBAddr.IsValid())
    return makeFailure("memory", "disassemble", "address does not resolve",
                       Request);

  lldb::SBInstructionList List =
      S->Target.ReadInstructions(SBAddr, static_cast<uint32_t>(Count));
  Array Instructions;
  const size_t Size = List.GetSize();
  for (size_t I = 0; I < Size; ++I) {
    lldb::SBInstruction Inst =
        List.GetInstructionAtIndex(static_cast<uint32_t>(I));
    if (!Inst.IsValid())
      continue;
    Dict D;
    lldb::SBAddress InstAddr = Inst.GetAddress();
    D["address"] =
        Object(static_cast<uint64_t>(InstAddr.GetLoadAddress(S->Target)));
    D["mnemonic"] = Object(text(Inst.GetMnemonic(S->Target)));
    D["operands"] = Object(text(Inst.GetOperands(S->Target)));
    D["comment"] = Object(text(Inst.GetComment(S->Target)));
    D["byteSize"] = Object(static_cast<uint64_t>(Inst.GetByteSize()));
    D["doesBranch"] = boolean(Inst.DoesBranch());
    Instructions.emplace_back(std::move(D));
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("memory");
  D["method"] = Object("disassemble");
  D["sessionId"] = Object(S->Id);
  D["instructions"] = Object(std::move(Instructions));
  return Object(std::move(D));
}

LldbBackend::Session *LldbBackend::findSession(const Object &Request) {
  std::string Id = getSessionId(Request);
  if (Id.empty())
    return nullptr;
  auto It = Sessions_.find(Id);
  return It == Sessions_.end() ? nullptr : &It->second;
}

const LldbBackend::Session *
LldbBackend::findSession(const Object &Request) const {
  std::string Id = getSessionId(Request);
  if (Id.empty())
    return nullptr;
  auto It = Sessions_.find(Id);
  return It == Sessions_.end() ? nullptr : &It->second;
}

uint64_t LldbBackend::addVariableRef(Session &S,
                                     std::vector<lldb::SBValue> Values) {
  const uint64_t Ref = S.NextVariableRef++;
  S.VariableRefs.emplace(Ref, std::move(Values));
  return Ref;
}

} // namespace ycode::debughost
