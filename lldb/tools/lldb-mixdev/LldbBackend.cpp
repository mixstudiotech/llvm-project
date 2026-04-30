#include "LldbBackend.h"

#include "lldb/API/SBAddress.h"
#include "lldb/API/SBAttachInfo.h"
#include "lldb/API/SBBroadcaster.h"
#include "lldb/API/SBBreakpoint.h"
#include "lldb/API/SBCommandInterpreter.h"
#include "lldb/API/SBCommandReturnObject.h"
#include "lldb/API/SBError.h"
#include "lldb/API/SBEvent.h"
#include "lldb/API/SBFileSpec.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBFunction.h"
#include "lldb/API/SBInstruction.h"
#include "lldb/API/SBInstructionList.h"
#include "lldb/API/SBLaunchInfo.h"
#include "lldb/API/SBListener.h"
#include "lldb/API/SBLineEntry.h"
#include "lldb/API/SBModule.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBSection.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBSymbol.h"
#include "lldb/API/SBTarget.h"
#include "lldb/API/SBThread.h"
#include "lldb/API/SBValue.h"
#include "lldb/API/SBValueList.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <thread>

using llvm::dtx::ns::Array;
using llvm::dtx::ns::Data;
using llvm::dtx::ns::Dict;
using llvm::dtx::ns::Object;

namespace ycode::debughost {

namespace {

void setEnv(const char *Name, const char *Value) {
#if defined(_WIN32)
  _putenv_s(Name, Value);
#else
  setenv(Name, Value, 1);
#endif
}

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

bool isSuccessObject(const Object &Obj) {
  const Dict *D = asDict(Obj);
  if (!D)
    return false;
  const Object *Success = lookup(*D, "success");
  if (!Success)
    return false;
  if (const auto *B = std::get_if<bool>(&Success->Value))
    return *B;
  return false;
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

std::string appendRelativePath(llvm::StringRef Base, llvm::StringRef Path) {
  llvm::SmallString<260> Result(Base);
  llvm::SmallString<260> Normalized(Path);
  llvm::sys::path::native(Normalized);
  llvm::StringRef Rest(Normalized);
  while (Rest.starts_with("/") || Rest.starts_with("\\"))
    Rest = Rest.drop_front();

  while (!Rest.empty()) {
    size_t Sep = Rest.find_first_of("/\\");
    llvm::StringRef Part =
        Sep == llvm::StringRef::npos ? Rest : Rest.take_front(Sep);
    if (!Part.empty())
      llvm::sys::path::append(Result, Part);
    Rest = Sep == llvm::StringRef::npos ? llvm::StringRef()
                                        : Rest.drop_front(Sep + 1);
  }
  return Result.str().str();
}

std::string manifestAttribute(llvm::StringRef Element, llvm::StringRef Name) {
  std::string Needle = (Name + "=\"").str();
  size_t Pos = Element.find(Needle);
  if (Pos == llvm::StringRef::npos)
    return "";
  Pos += Needle.size();
  size_t End = Element.find('"', Pos);
  if (End == llvm::StringRef::npos)
    return "";
  return Element.slice(Pos, End).str();
}

Object prepareManifestDeviceSupportRoot(llvm::StringRef CacheRoot) {
  Dict D;
  D["cacheRoot"] = Object(CacheRoot.str());
  D["manifest"] = boolean(false);
  D["rootfs"] = Object("");
  D["linkedFiles"] = Object(uint64_t(0));
  D["existingFiles"] = Object(uint64_t(0));
  D["failedFiles"] = Object(uint64_t(0));

  std::string ManifestPath = appendRelativePath(CacheRoot, "manifest.xml");
  auto ManifestBuffer = llvm::MemoryBuffer::getFile(ManifestPath);
  if (!ManifestBuffer)
    return Object(std::move(D));

  std::string Rootfs = appendRelativePath(CacheRoot, "lldb-root");
  std::error_code EC = llvm::sys::fs::create_directories(Rootfs);
  if (EC) {
    D["manifest"] = boolean(true);
    D["error"] = Object("create lldb-root failed: " + EC.message());
    return Object(std::move(D));
  }

  uint64_t Linked = 0;
  uint64_t Existing = 0;
  uint64_t Failed = 0;
  llvm::StringRef Text = ManifestBuffer.get()->getBuffer();
  size_t Pos = 0;
  while (true) {
    Pos = Text.find("<file ", Pos);
    if (Pos == llvm::StringRef::npos)
      break;
    size_t End = Text.find("/>", Pos);
    if (End == llvm::StringRef::npos)
      break;
    llvm::StringRef Element = Text.slice(Pos, End);
    Pos = End + 2;

    std::string RemotePath = manifestAttribute(Element, "remotePath");
    std::string LocalPath = manifestAttribute(Element, "localPath");
    if (RemotePath.empty() || LocalPath.empty())
      continue;

    std::string Source = appendRelativePath(CacheRoot, LocalPath);
    std::string Dest = appendRelativePath(Rootfs, RemotePath);
    llvm::SmallString<260> DestDir(Dest);
    llvm::sys::path::remove_filename(DestDir);
    EC = llvm::sys::fs::create_directories(DestDir);
    if (EC) {
      ++Failed;
      continue;
    }

    if (llvm::sys::fs::exists(Dest)) {
      ++Existing;
      continue;
    }

    EC = llvm::sys::fs::create_hard_link(Source, Dest);
    if (EC) {
      ++Failed;
      continue;
    }
    ++Linked;
  }

  D["manifest"] = boolean(true);
  D["rootfs"] = Object(Rootfs);
  D["linkedFiles"] = Object(Linked);
  D["existingFiles"] = Object(Existing);
  D["failedFiles"] = Object(Failed);
  return Object(std::move(D));
}

std::string archFromTriple(std::string Triple) {
  if (Triple.empty())
    return "";
  llvm::StringRef Ref(Triple);
  return Ref.split('-').first.str();
}

std::string classifySymbolSource(llvm::StringRef ModulePath,
                                 llvm::StringRef SymbolPath) {
  if (SymbolPath.empty())
    return "Missing";
  if (SymbolPath.contains(".dSYM"))
    return "Project dSYM";
  if (!ModulePath.empty() && SymbolPath == ModulePath)
    return "Object file";
  return "Explicit path";
}

std::string symbolMismatchReason(bool HasDebugInfo, bool HasSymbolFile) {
  if (HasDebugInfo)
    return "";
  if (!HasSymbolFile)
    return "no symbol file loaded";
  return "symbol file has no debug compile units";
}

uint64_t moduleSectionSize(lldb::SBSection Section) {
  if (!Section.IsValid())
    return 0;

  const size_t SubsectionCount = Section.GetNumSubSections();
  if (SubsectionCount == 0)
    return static_cast<uint64_t>(Section.GetByteSize());

  uint64_t Size = 0;
  for (size_t I = 0; I < SubsectionCount; ++I)
    Size += moduleSectionSize(Section.GetSubSectionAtIndex(I));
  return Size;
}

uint64_t moduleSize(lldb::SBModule Module) {
  uint64_t Size = 0;
  const size_t SectionCount = Module.GetNumSections();
  for (size_t I = 0; I < SectionCount; ++I)
    Size += moduleSectionSize(Module.GetSectionAtIndex(I));
  return Size;
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

Object processDiagnostics(lldb::SBProcess Process) {
  Dict D;
  const bool Valid = Process.IsValid();
  D["valid"] = boolean(Valid);
  if (!Valid)
    return Object(std::move(D));

  lldb::SBThread Selected = Process.GetSelectedThread();
  D["state"] = Object(stateName(Process.GetState()));
  D["pid"] = Object(static_cast<uint64_t>(Process.GetProcessID()));
  D["uniqueId"] = Object(static_cast<uint64_t>(Process.GetUniqueID()));
  D["threadCount"] = Object(static_cast<uint64_t>(Process.GetNumThreads()));
  D["stopId"] = Object(static_cast<uint64_t>(Process.GetStopID()));
  D["exitStatus"] = Object(static_cast<uint64_t>(Process.GetExitStatus()));
  D["exitDescription"] = Object(text(Process.GetExitDescription()));
  D["selectedThreadId"] =
      Object(Selected.IsValid() ? static_cast<uint64_t>(Selected.GetThreadID())
                                : uint64_t(0));
  D["selectedThreadIndexId"] =
      Object(Selected.IsValid() ? static_cast<uint64_t>(Selected.GetIndexID())
                                : uint64_t(0));
  D["selectedThreadStopReason"] =
      Object(Selected.IsValid() ? stopReasonName(Selected.GetStopReason()) : "");
  D["selectedThreadFrameCount"] =
      Object(Selected.IsValid() ? static_cast<uint64_t>(Selected.GetNumFrames())
                                : uint64_t(0));
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

Object frameToObject(lldb::SBTarget Target, lldb::SBFrame Frame, lldb::tid_t ThreadID,
                     uint32_t FrameIndex) {
  Dict D;
  lldb::SBAddress Address = Frame.GetPCAddress();
  if ((!Address.IsValid() ||
       (Address.IsValid() && !Address.GetModule().IsValid())) &&
      Target.IsValid()) {
    lldb::SBAddress Resolved =
        Target.ResolveLoadAddress(static_cast<lldb::addr_t>(Frame.GetPC()));
    if (Resolved.IsValid())
      Address = Resolved;
  }
  lldb::SBModule Module = Address.IsValid() ? Address.GetModule()
                                            : lldb::SBModule();
  if (!Module.IsValid())
    Module = Frame.GetModule();
  lldb::SBFunction Function = Address.IsValid() ? Address.GetFunction()
                                                : lldb::SBFunction();
  if (!Function.IsValid())
    Function = Frame.GetFunction();
  lldb::SBSymbol Symbol = Address.IsValid() ? Address.GetSymbol()
                                            : lldb::SBSymbol();
  if (!Symbol.IsValid())
    Symbol = Frame.GetSymbol();
  lldb::SBLineEntry Line = Address.IsValid() ? Address.GetLineEntry()
                                             : lldb::SBLineEntry();
  if (!Line.IsValid())
    Line = Frame.GetLineEntry();

  const char *FunctionName =
      Function.IsValid() ? Function.GetDisplayName() : nullptr;
  if (!FunctionName && Function.IsValid())
    FunctionName = Function.GetName();
  const char *SymbolName = Symbol.IsValid() ? Symbol.GetDisplayName() : nullptr;
  if (!SymbolName && Symbol.IsValid())
    SymbolName = Symbol.GetName();
  const char *Name = Frame.GetDisplayFunctionName();
  if (!Name)
    Name = FunctionName;
  if (!Name)
    Name = SymbolName;
  if (!Name)
    Name = Frame.GetFunctionName();

  std::string ModulePath = pathFromFileSpec(Module.GetFileSpec());
  std::string ModuleSymbolPath = pathFromFileSpec(Module.GetSymbolFileSpec());
  D["id"] = Object(static_cast<uint64_t>(Frame.GetFrameID()));
  D["threadId"] = Object(static_cast<uint64_t>(ThreadID));
  D["frameIndex"] = Object(static_cast<uint64_t>(FrameIndex));
  D["name"] = Object(text(Name));
  D["pc"] = Object(static_cast<uint64_t>(Frame.GetPC()));
  D["addressResolved"] = boolean(Address.IsValid());
  D["fileAddress"] =
      Object(Address.IsValid() ? static_cast<uint64_t>(Address.GetFileAddress())
                               : uint64_t(0));
  D["targetModuleCount"] =
      Object(Target.IsValid() ? static_cast<uint64_t>(Target.GetNumModules())
                              : uint64_t(0));
  D["source"] = lineEntryToObject(Line);
  D["moduleName"] = Object(text(Module.GetFileSpec().GetFilename()));
  D["modulePath"] = Object(ModulePath);
  D["moduleUuid"] = Object(text(Module.GetUUIDString()));
  D["moduleSymbolPath"] = Object(ModuleSymbolPath);
  D["symbolName"] = Object(text(SymbolName));
  D["functionName"] = Object(text(FunctionName));
  D["hasModule"] = boolean(Module.IsValid());
  D["hasSymbol"] = boolean(Symbol.IsValid());
  D["hasFunction"] = boolean(Function.IsValid());
  D["hasLineEntry"] = boolean(Line.IsValid());
  D["symbolSource"] =
      Object(classifySymbolSource(ModulePath, ModuleSymbolPath));
  return Object(std::move(D));
}

Object valueToObject(lldb::SBValue Value, uint64_t VariablesReference) {
  Dict D;
  lldb::SBError Error = Value.GetError();
  std::string TypeName = text(Value.GetDisplayTypeName());
  if (TypeName.empty())
    TypeName = text(Value.GetTypeName());
  D["name"] = Object(text(Value.GetName()));
  D["type"] = Object(TypeName);
  D["value"] = Object(text(Value.GetValue()));
  D["summary"] = Object(text(Value.GetSummary()));
  D["location"] = Object(text(Value.GetLocation()));
  D["loadAddress"] = Object(static_cast<uint64_t>(Value.GetLoadAddress()));
  D["byteSize"] = Object(static_cast<uint64_t>(Value.GetByteSize()));
  D["inScope"] = boolean(Value.IsInScope());
  D["valueDidChange"] = boolean(Value.GetValueDidChange());
  D["error"] = Object(Error.Fail() ? errorString(Error) : "");
  D["variablesReference"] = Object(VariablesReference);
  D["childCount"] = Object(static_cast<uint64_t>(Value.GetNumChildren(256)));
  return Object(std::move(D));
}

Object registerToObject(lldb::SBValue Register, const char *SetName) {
  Dict D;
  D["name"] = Object(text(Register.GetName()));
  D["set"] = Object(text(SetName));
  D["type"] = Object(text(Register.GetTypeName()));
  D["value"] = Object(text(Register.GetValue()));
  D["summary"] = Object(text(Register.GetSummary()));
  return Object(std::move(D));
}

Object moduleToObject(lldb::SBModule Module, lldb::SBTarget Target,
                      uint32_t Index) {
  lldb::SBFileSpec File = Module.GetFileSpec();
  std::string Path = pathFromFileSpec(File);
  std::string SymbolPath = pathFromFileSpec(Module.GetSymbolFileSpec());
  bool HasDebugInfo = Module.GetNumCompileUnits() > 0;
  bool HasSymbolFile = !SymbolPath.empty();
  std::string SymbolStatus = (HasDebugInfo || HasSymbolFile) ? "loaded" : "missing";
  std::string SymbolSource =
      HasDebugInfo || HasSymbolFile
          ? classifySymbolSource(Path, HasSymbolFile ? SymbolPath : Path)
          : "Missing";
  Dict D;
  D["id"] = Object(static_cast<uint64_t>(Index));
  D["name"] = Object(text(File.GetFilename()));
  D["path"] = Object(Path);
  D["symbolPath"] = Object(HasSymbolFile ? SymbolPath : "");
  D["arch"] = Object(archFromTriple(text(Module.GetTriple())));
  D["uuid"] = Object(text(Module.GetUUIDString()));
  D["symbolStatus"] = Object(SymbolStatus);
  D["symbolSource"] = Object(SymbolSource);
  D["symbolMismatchReason"] =
      Object(symbolMismatchReason(HasDebugInfo, HasSymbolFile));
  D["loadOrder"] = Object(static_cast<uint64_t>(Index + 1));

  uint64_t LoadAddress = 0;
  lldb::SBAddress Header = Module.GetObjectFileHeaderAddress();
  if (Header.IsValid())
    LoadAddress = static_cast<uint64_t>(Header.GetLoadAddress(Target));
  D["loadAddress"] = Object(LoadAddress);
  D["size"] = Object(moduleSize(Module));
  return Object(std::move(D));
}

uint64_t selectedThreadId(lldb::SBProcess Process) {
  if (!Process.IsValid())
    return 0;
  lldb::SBThread Selected = Process.GetSelectedThread();
  if (Selected.IsValid())
    return static_cast<uint64_t>(Selected.GetThreadID());
  return Process.GetNumThreads()
             ? static_cast<uint64_t>(Process.GetThreadAtIndex(0).GetThreadID())
             : uint64_t(0);
}

Object targetModuleDiagnostics(lldb::SBTarget Target) {
  Dict D;
  D["targetValid"] = boolean(Target.IsValid());
  if (!Target.IsValid())
    return Object(std::move(D));

  const uint32_t Count = Target.GetNumModules();
  D["moduleCount"] = Object(static_cast<uint64_t>(Count));
  D["triple"] = Object(text(Target.GetTriple()));
  D["arch"] = Object(text(Target.GetArchName()));
  D["executable"] = Object(pathFromFileSpec(Target.GetExecutable()));

  Array Modules;
  for (uint32_t I = 0; I < Count; ++I) {
    lldb::SBModule Module = Target.GetModuleAtIndex(I);
    if (Module.IsValid())
      Modules.emplace_back(moduleToObject(Module, Target, I));
  }
  D["modules"] = Object(std::move(Modules));
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

std::string quoteCommandArg(llvm::StringRef Value) {
  std::string Result = "\"";
  for (char C : Value) {
    if (C == '\\' || C == '"')
      Result.push_back('\\');
    Result.push_back(C);
  }
  Result.push_back('"');
  return Result;
}

Object commandResultToObject(llvm::StringRef Command,
                             lldb::SBCommandReturnObject &Result) {
  Dict D;
  D["command"] = Object(Command.str());
  D["success"] = boolean(Result.Succeeded());
  D["output"] = Object(text(Result.GetOutput()));
  D["error"] = Object(text(Result.GetError()));
  return Object(std::move(D));
}

void runLldbCommand(lldb::SBDebugger Debugger, llvm::StringRef Command,
                    Array &Results) {
  lldb::SBCommandReturnObject Result;
  Debugger.GetCommandInterpreter().HandleCommand(Command.str().c_str(), Result);
  Results.emplace_back(commandResultToObject(Command, Result));
}

std::vector<std::string> deviceSupportPathsFromRequest(const Dict &Req) {
  std::vector<std::string> Paths = getStringArray(Req, "deviceSupportPaths");
  std::string Single = getString(Req, "deviceSupportPath");
  if (!Single.empty())
    Paths.insert(Paths.begin(), Single);

  std::vector<std::string> Unique;
  for (std::string &Path : Paths) {
    if (Path.empty())
      continue;
    if (std::find(Unique.begin(), Unique.end(), Path) == Unique.end())
      Unique.push_back(std::move(Path));
  }
  return Unique;
}

Object applyDeviceSupportPaths(lldb::SBDebugger Debugger, const Dict &Req) {
  Array Results;
  Array PreparedRoots;
  std::vector<std::string> Paths = deviceSupportPathsFromRequest(Req);
  for (const std::string &Path : Paths) {
    Object Prepared = prepareManifestDeviceSupportRoot(Path);
    const Dict *PreparedDict = asDict(Prepared);
    std::string Rootfs =
        PreparedDict ? getString(*PreparedDict, "rootfs") : "";
    if (!Rootfs.empty()) {
      runLldbCommand(Debugger,
                     "target modules search-paths add " + quoteCommandArg("/") +
                         " " + quoteCommandArg(Rootfs),
                     Results);
      runLldbCommand(
          Debugger,
          "target modules search-paths add " +
              quoteCommandArg("/private/preboot/Cryptexes/OS") + " " +
              quoteCommandArg(appendRelativePath(
                  Rootfs, "/private/preboot/Cryptexes/OS")),
          Results);
      runLldbCommand(Debugger,
                     "settings append target.debug-file-search-paths " +
                         quoteCommandArg(Rootfs),
                     Results);
      runLldbCommand(Debugger,
                     "settings append target.exec-search-paths " +
                         quoteCommandArg(Rootfs),
                     Results);
    }
    PreparedRoots.emplace_back(std::move(Prepared));

    std::string Q = quoteCommandArg(Path);
    runLldbCommand(Debugger, "settings append target.debug-file-search-paths " + Q,
                   Results);
    runLldbCommand(Debugger, "settings append target.exec-search-paths " + Q,
                   Results);
    runLldbCommand(Debugger,
                   "target modules search-paths add " +
                       quoteCommandArg("/System/Library/Caches/com.apple.dyld") +
                       " " + Q,
                   Results);
    runLldbCommand(Debugger,
                   "target modules search-paths add " +
                       quoteCommandArg("/System/Library") + " " + Q,
                   Results);
    runLldbCommand(Debugger,
                   "target modules search-paths add " + quoteCommandArg("/usr/lib") +
                       " " + Q,
                   Results);
  }

  Array PathObjects;
  for (const std::string &Path : Paths)
    PathObjects.emplace_back(Path);

  Dict D;
  D["requested"] = Object(!Paths.empty());
  D["paths"] = Object(std::move(PathObjects));
  D["preparedRoots"] = Object(std::move(PreparedRoots));
  D["commands"] = Object(std::move(Results));
  return Object(std::move(D));
}

bool isMixConnectUrl(llvm::StringRef URL) {
  return URL.starts_with("ios://") || URL.starts_with("android://") ||
         URL.starts_with("mix-ios://") || URL.starts_with("mix-android://");
}

std::string lldbPlatformName(const Dict &D) {
  std::string Platform = getString(D, "platform");
  if (Platform == "ios" || Platform == "mix-ios")
    return "remote-ios";
  if (Platform == "android" || Platform == "mix-android")
    return "remote-android";

  std::string URL = getString(D, "connectUrl");
  llvm::StringRef URLRef(URL);
  if (URLRef.starts_with("ios://") || URLRef.starts_with("mix-ios://"))
    return "remote-ios";
  if (URLRef.starts_with("android://") || URLRef.starts_with("mix-android://"))
    return "remote-android";
  return Platform;
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

lldb::StateType waitForProcessStopped(lldb::SBProcess Process,
                                       uint32_t TimeoutMs) {
  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
  lldb::StateType State = Process.GetState();
  while (std::chrono::steady_clock::now() < Deadline) {
    State = Process.GetState();
    if (State == lldb::eStateStopped || State == lldb::eStateCrashed ||
        State == lldb::eStateExited || State == lldb::eStateDetached)
      return State;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return Process.GetState();
}

lldb::StateType waitForProcessNotStopped(lldb::SBProcess Process,
                                          uint32_t TimeoutMs) {
  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
  lldb::StateType State = Process.GetState();
  while (std::chrono::steady_clock::now() < Deadline) {
    State = Process.GetState();
    if (State != lldb::eStateStopped)
      return State;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return Process.GetState();
}

lldb::StateType continueIfStopped(lldb::SBProcess Process) {
  if (!Process.IsValid())
    return lldb::eStateInvalid;
  lldb::StateType State = Process.GetState();
  if (State != lldb::eStateStopped)
    return State;
  lldb::SBError Error = Process.Continue();
  if (Error.Fail())
    return Process.GetState();
  return waitForProcessNotStopped(Process, 1000);
}

bool isStableAttachState(lldb::StateType State) {
  return State == lldb::eStateStopped || State == lldb::eStateRunning ||
         State == lldb::eStateCrashed || State == lldb::eStateExited ||
         State == lldb::eStateDetached;
}

bool isUsableAttachState(lldb::StateType State) {
  return State == lldb::eStateStopped || State == lldb::eStateRunning ||
         State == lldb::eStateCrashed;
}

lldb::StateType waitForAttachState(lldb::SBListener Listener,
                                   lldb::SBProcess Process,
                                   uint32_t TimeoutSecs) {
  lldb::StateType State = Process.GetState();
  if (isStableAttachState(State))
    return State;

  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(TimeoutSecs);
  while (std::chrono::steady_clock::now() < Deadline) {
    lldb::SBEvent Event;
    if (Listener.IsValid() &&
        Listener.WaitForEventForBroadcasterWithType(
            1, Process.GetBroadcaster(),
            lldb::SBProcess::eBroadcastBitStateChanged, Event)) {
      State = lldb::SBProcess::GetStateFromEvent(Event);
      if (isStableAttachState(State))
        return State;
      continue;
    }

    State = Process.GetState();
    if (isStableAttachState(State))
      return State;
  }

  return Process.GetState();
}

Object makeAttachStateFailure(const char *Method, lldb::StateType State,
                              const Object &Request) {
  std::ostringstream Message;
  Message << "process did not reach a usable attach state"
          << " state=" << stateName(State);
  return makeFailure("execution", Method, Message.str(), Request);
}

lldb::SBFrame findFrame(lldb::SBProcess Process, const Dict &Request) {
  const uint64_t ThreadID = getUnsigned(Request, "threadId");
  const uint64_t FrameIndex =
      getUnsigned(Request, "frameIndex", getUnsigned(Request, "frameId"));
  lldb::SBThread Thread = findThread(Process, ThreadID);
  if (!Thread.IsValid())
    return lldb::SBFrame();
  Process.SetSelectedThread(Thread);
  Thread.SetSelectedFrame(static_cast<uint32_t>(FrameIndex));
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

bool canCreateTargetFromHostPath(llvm::StringRef Path) {
  if (Path.empty())
    return false;
  return llvm::sys::fs::is_regular_file(Path);
}

Object connectRemote(lldb::SBDebugger &Debugger, lldb::SBTarget &Target,
                     lldb::SBProcess &Process, const std::string &SessionId,
                     const Dict &Req, const Object &Request,
                     const char *Method, const std::string &Program) {
  std::string URL = makeMixConnectUrl(Req);
  if (URL.empty())
    return makeFailure("execution", Method, "missing mix connect URL", Request);

  lldb::SBError Error;
  std::string Platform = lldbPlatformName(Req);
  if (!Platform.empty()) {
    Error = Debugger.SetCurrentPlatform(Platform.c_str());
    if (Error.Fail())
      return makeFailure("execution", Method, errorString(Error), Request);
  }

  if (canCreateTargetFromHostPath(Program))
    Target = Debugger.CreateTarget(Program.c_str(), nullptr, nullptr, true,
                                   Error);
  else
    Target = Debugger.CreateTarget(nullptr);
  if (Error.Fail() || !Target.IsValid())
    return makeFailure("execution", Method, errorString(Error), Request);

  Object DeviceSupport = applyDeviceSupportPaths(Debugger, Req);

  if (llvm::StringRef(Method) == "attach") {
    const uint64_t Pid = getUnsigned(Req, "pid");
    if (!Pid)
      return makeFailure("execution", Method, "missing pid", Request);

    lldb::SBPlatform SelectedPlatform = Debugger.GetSelectedPlatform();
    if (!SelectedPlatform.IsValid())
      return makeFailure("execution", Method, "invalid selected platform",
                         Request);

    lldb::SBPlatformConnectOptions ConnectOptions(URL.c_str());
    Error = SelectedPlatform.ConnectRemote(ConnectOptions);
    if (Error.Fail())
      return makeFailure("execution", Method, errorString(Error), Request);

    lldb::SBAttachInfo AttachInfo(static_cast<lldb::pid_t>(Pid));
    lldb::SBListener Listener = Debugger.GetListener();
    AttachInfo.SetListener(Listener);
    AttachInfo.SetResumeCount(1);
    Error.Clear();
    Process = SelectedPlatform.Attach(AttachInfo, Debugger, Target, Error);
    if (Error.Fail() || !Process.IsValid()) {
      std::ostringstream Message;
      Message << errorString(Error) << " after platform connect"
              << " platform=" << text(SelectedPlatform.GetName())
              << " connected=" << (SelectedPlatform.IsConnected() ? "true"
                                                                  : "false");
      return makeFailure("execution", Method, Message.str(), Request);
    }
    lldb::StateType AttachState = waitForAttachState(Listener, Process, 30);
    if (!isUsableAttachState(AttachState))
      return makeAttachStateFailure(Method, AttachState, Request);
    Object AttachDiagnostics = processDiagnostics(Process);
    Object ModuleDiagnostics = targetModuleDiagnostics(Target);
    lldb::StateType RunState = continueIfStopped(Process);

    Dict D;
    D["success"] = boolean(true);
    D["implemented"] = boolean(true);
    D["service"] = Object("execution");
    D["method"] = Object(Method);
    D["sessionId"] = Object(SessionId);
    D["remote"] = boolean(true);
    D["connectUrl"] = Object(std::move(URL));
    D["attachState"] = Object(stateName(AttachState));
    D["processState"] = Object(stateName(RunState));
    D["diagnostics"] = AttachDiagnostics;
    D["moduleDiagnostics"] = ModuleDiagnostics;
    D["deviceSupport"] = DeviceSupport;
    D["process"] = processToObject(Process);
    return Object(std::move(D));
  }

  lldb::SBListener Listener = Debugger.GetListener();
  Error.Clear();
  Process = Target.ConnectRemote(Listener, URL.c_str(), "gdb-remote", Error);
  if (Error.Fail() || !Process.IsValid())
    return makeFailure("execution", Method, errorString(Error), Request);
  lldb::StateType AttachState = waitForAttachState(Listener, Process, 30);
  if (!isUsableAttachState(AttachState))
    return makeAttachStateFailure(Method, AttachState, Request);
  Object AttachDiagnostics = processDiagnostics(Process);
  Object ModuleDiagnostics = targetModuleDiagnostics(Target);
  lldb::StateType RunState = continueIfStopped(Process);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object(Method);
  D["sessionId"] = Object(SessionId);
  D["remote"] = boolean(true);
  D["connectUrl"] = Object(std::move(URL));
  D["attachState"] = Object(stateName(AttachState));
  D["processState"] = Object(stateName(RunState));
  D["diagnostics"] = AttachDiagnostics;
  D["moduleDiagnostics"] = ModuleDiagnostics;
  D["deviceSupport"] = DeviceSupport;
  D["process"] = processToObject(Process);
  return Object(std::move(D));
}

} // namespace

LldbBackend::LldbBackend() {
  setEnv("LLDB_MIXDEV_FAST_ATTACH", "1");
  setEnv("LLDB_MIXDEV_ASYNC_MODULE_LOAD", "1");
  lldb::SBError Error = lldb::SBDebugger::InitializeWithErrorHandling();
  Initialized_ = Error.Success();
}

LldbBackend::~LldbBackend() {
  for (auto &Entry : Sessions_)
    stopEventPump(Entry.second);
  for (auto &Entry : Sessions_)
    lldb::SBDebugger::Destroy(Entry.second.Debugger);
  Sessions_.clear();
  if (Initialized_)
    lldb::SBDebugger::Terminate();
}

void LldbBackend::setEventSink(DebugHostEventSink Sink) {
  std::lock_guard<std::mutex> Lock(EventSinkMutex_);
  EventSink_ = std::move(Sink);
}

void LldbBackend::startEventPump(Session &S) {
  if (!S.Process.IsValid() || !S.Events)
    return;
  auto Events = S.Events;
  if (Events->Thread.joinable())
    return;

  Events->Stop.store(false);
  {
    std::lock_guard<std::mutex> Lock(Events->Mutex);
    Events->LastState = S.Process.GetState();
    Events->LastStopID = S.Process.GetStopID();
  }

  // Cap LLDB's internal Halt timeout (default 20s) so the slow pause path
  // can't outrun the C# client's 30s CTS. With this clamp the worst case
  // is ~5s (SendAsyncInterrupt wait) + 8s (Halt) + 5s (post-stop wait) =
  // 18s, leaving headroom for the client.
  {
    lldb::SBCommandReturnObject Result;
    S.Debugger.GetCommandInterpreter().HandleCommand(
        "settings set target.process.interrupt-timeout 8", Result);
  }

  // Subscribe broadly so dyld module-load notifications and thread-state
  // updates also drain — otherwise modules/threads get stuck at 0 right
  // after attach because nothing is consuming those broadcaster queues.
  // See LldbBackend audit for context.
  lldb::SBListener Listener = S.Debugger.GetListener();
  if (Listener.IsValid()) {
    const uint32_t ProcessBits =
        lldb::SBProcess::eBroadcastBitStateChanged |
        lldb::SBProcess::eBroadcastBitInterrupt |
        lldb::SBProcess::eBroadcastBitSTDOUT |
        lldb::SBProcess::eBroadcastBitSTDERR |
        lldb::SBProcess::eBroadcastBitProfileData |
        lldb::SBProcess::eBroadcastBitStructuredData;
    Listener.StartListeningForEvents(S.Process.GetBroadcaster(), ProcessBits);
    Listener.StartListeningForEventClass(S.Debugger, "lldb.target",
                                         UINT32_MAX);
    Listener.StartListeningForEventClass(S.Debugger, "lldb.thread",
                                         UINT32_MAX);
  }

  Events->Thread = std::thread([this, &S, Events]() {
    lldb::SBListener Listener = S.Debugger.GetListener();
    // SBListener::WaitForEvent has 1-second granularity, which makes
    // session-close lag noticeable. Poll GetNextEvent (non-blocking) at
    // 50ms instead — drains all broadcasters as they come in, and
    // stopEventPump's Stop flag is observed within ~50ms.
    while (!Events->Stop.load()) {
      lldb::SBEvent Event;
      if (!Listener.IsValid() || !Listener.GetNextEvent(Event)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }

      // Only Process state-change events advance our LastState / fire
      // VS-side stopped/running notifications. Other events (module load,
      // thread, stdio, profile data) just need to be drained from the
      // listener queue so LLDB's internal pipelines keep moving.
      if (S.Process.IsValid() &&
          lldb::SBProcess::EventIsProcessEvent(Event)) {
        const uint32_t Type = Event.GetType();
        if (Type & lldb::SBProcess::eBroadcastBitStateChanged) {
          lldb::StateType State = lldb::SBProcess::GetStateFromEvent(Event);
          updateSessionState(S, State);
          emitProcessEvent(S, State, false);
        }
      } else if (lldb::SBTarget::EventIsTargetEvent(Event)) {
        const uint32_t Type = Event.GetType();
        if (Type & lldb::SBTarget::eBroadcastBitModulesLoaded)
          emitModuleLoadedEvents(S, Event);
      }
    }
  });
}

void LldbBackend::stopEventPump(Session &S) {
  if (!S.Events)
    return;
  S.Events->Stop.store(true);
  S.Events->Changed.notify_all();
  if (S.Events->Thread.joinable())
    S.Events->Thread.join();
}

void LldbBackend::updateSessionState(Session &S, lldb::StateType State) {
  if (!S.Events)
    return;
  {
    std::lock_guard<std::mutex> Lock(S.Events->Mutex);
    S.Events->LastState = State;
    if (S.Process.IsValid())
      S.Events->LastStopID = S.Process.GetStopID();
    // Bump the generation so any waiter that snapshotted before this event
    // can detect that a new state-change has been observed.
    ++S.Events->Generation;
  }
  S.Events->Changed.notify_all();
}

uint64_t LldbBackend::snapshotEventGeneration(Session &S) const {
  if (!S.Events)
    return 0;
  std::lock_guard<std::mutex> Lock(S.Events->Mutex);
  return S.Events->Generation;
}

LldbBackend::TransitionWaitResult LldbBackend::waitForSessionTransition(
    Session &S, uint64_t BaselineGen, uint32_t TimeoutMs,
    const std::function<bool(lldb::StateType, uint32_t)> &Predicate) {
  TransitionWaitResult Result;
  if (!S.Events)
  {
    if (S.Process.IsValid()) {
      Result.State = S.Process.GetState();
      Result.StopID = S.Process.GetStopID();
      Result.Observed = Predicate(Result.State, Result.StopID);
    }
    return Result;
  }

  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
  std::unique_lock<std::mutex> Lock(S.Events->Mutex);
  while (!S.Events->Stop.load()) {
    // Only consider a wait satisfied if BOTH (a) a new state-change event has
    // been consumed since the baseline, AND (b) the resulting state matches
    // the predicate. This prevents a stale historical LastState from spoofing
    // a successful pause/continue when the action hasn't actually landed yet.
    if (S.Events->Generation > BaselineGen &&
        Predicate(S.Events->LastState, S.Events->LastStopID)) {
      Result.State = S.Events->LastState;
      Result.StopID = S.Events->LastStopID;
      Result.Generation = S.Events->Generation;
      Result.Observed = true;
      return Result;
    }
    if (std::chrono::steady_clock::now() >= Deadline)
      break;
    S.Events->Changed.wait_until(Lock, Deadline);
  }
  Result.State = S.Events->LastState;
  Result.StopID = S.Events->LastStopID;
  Result.Generation = S.Events->Generation;
  return Result;
}

lldb::StateType LldbBackend::waitForSessionState(
    Session &S, uint32_t TimeoutMs,
    const std::function<bool(lldb::StateType)> &Predicate) {
  if (!S.Events)
    return S.Process.IsValid() ? S.Process.GetState() : lldb::eStateInvalid;

  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
  std::unique_lock<std::mutex> Lock(S.Events->Mutex);
  while (!S.Events->Stop.load()) {
    lldb::StateType State = S.Events->LastState;
    if (Predicate(State))
      return State;
    if (std::chrono::steady_clock::now() >= Deadline)
      break;
    S.Events->Changed.wait_until(Lock, Deadline);
  }
  return S.Events->LastState;
}

void LldbBackend::emitProcessEvent(Session &S, lldb::StateType State,
                                   bool Force) {
  if (!S.Process.IsValid())
    return;

  uint32_t StopID = S.Process.GetStopID();
  if (S.Events) {
    std::lock_guard<std::mutex> Lock(S.Events->Mutex);
    if (!Force && State == S.Events->LastEmittedState &&
        StopID == S.Events->LastEmittedStopID)
      return;
    S.Events->LastEmittedState = State;
    S.Events->LastEmittedStopID = StopID;
  }

  std::string EventName;
  switch (State) {
  case lldb::eStateStopped:
  case lldb::eStateCrashed:
    EventName = "stopped";
    break;
  case lldb::eStateExited:
    EventName = "exited";
    break;
  case lldb::eStateDetached:
    EventName = "detached";
    break;
  case lldb::eStateRunning:
    EventName = "running";
    break;
  default:
    EventName = "process.stateChanged";
    break;
  }

  Dict Event;
  Event["event"] = Object(EventName);
  Event["service"] = Object("execution");
  Event["sessionId"] = Object(S.Id);
  Event["state"] = Object(stateName(State));
  Event["stopId"] = Object(static_cast<uint64_t>(StopID));
  Event["threadId"] = Object(selectedThreadId(S.Process));
  Event["diagnostics"] = processDiagnostics(S.Process);
  Event["process"] = processToObject(S.Process);

  if (State == lldb::eStateStopped || State == lldb::eStateCrashed) {
    lldb::SBThread Thread = S.Process.GetSelectedThread();
    Event["reason"] =
        Object(Thread.IsValid() ? stopReasonName(Thread.GetStopReason()) : "");
  }

  DebugHostEventSink Sink;
  {
    std::lock_guard<std::mutex> Lock(EventSinkMutex_);
    Sink = EventSink_;
  }
  if (Sink)
    Sink(Object(std::move(Event)));
}

void LldbBackend::emitModuleLoadedEvents(Session &S,
                                         const lldb::SBEvent &Event) {
  if (!S.Target.IsValid())
    return;

  DebugHostEventSink Sink;
  {
    std::lock_guard<std::mutex> Lock(EventSinkMutex_);
    Sink = EventSink_;
  }
  if (!Sink)
    return;

  const uint32_t EventModuleCount =
      lldb::SBTarget::GetNumModulesFromEvent(Event);
  for (uint32_t I = 0; I < EventModuleCount; ++I) {
    lldb::SBModule Module = lldb::SBTarget::GetModuleAtIndexFromEvent(I, Event);
    if (!Module.IsValid())
      continue;

    uint32_t TargetIndex = 0;
    const uint32_t TargetModuleCount = S.Target.GetNumModules();
    for (; TargetIndex < TargetModuleCount; ++TargetIndex) {
      lldb::SBModule Candidate = S.Target.GetModuleAtIndex(TargetIndex);
      if (!Candidate.IsValid())
        continue;
      if (std::string(text(Candidate.GetUUIDString())) ==
              std::string(text(Module.GetUUIDString())) &&
          pathFromFileSpec(Candidate.GetFileSpec()) ==
              pathFromFileSpec(Module.GetFileSpec()))
        break;
    }
    if (TargetIndex == TargetModuleCount)
      TargetIndex = I;

    Dict ModuleEvent;
    ModuleEvent["event"] = Object("moduleLoaded");
    ModuleEvent["service"] = Object("modules");
    ModuleEvent["sessionId"] = Object(S.Id);
    ModuleEvent["moduleIndex"] = Object(static_cast<uint64_t>(TargetIndex));
    ModuleEvent["moduleCount"] =
        Object(static_cast<uint64_t>(S.Target.GetNumModules()));
    ModuleEvent["module"] = moduleToObject(Module, S.Target, TargetIndex);
    Sink(Object(std::move(ModuleEvent)));
  }
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
    std::string Platform = lldbPlatformName(*Req);
    if (!Platform.empty()) {
      lldb::SBError Error = S.Debugger.SetCurrentPlatform(Platform.c_str());
      if (Error.Fail())
        return makeFailure("session", "create", errorString(Error), Request);
    }
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

  stopEventPump(It->second);
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

  invalidateVariableRefs(*S);
  if (isRemoteMixRequest(*Req)) {
    Object Result = connectRemote(S->Debugger, S->Target, S->Process, S->Id,
                                  *Req, Request, "launch", Program);
    if (isSuccessObject(Result))
      startEventPump(*S);
    return Result;
  }

  lldb::SBError Error;
  S->Target = S->Debugger.CreateTarget(Program.c_str(), nullptr, nullptr, true,
                                       Error);
  if (Error.Fail() || !S->Target.IsValid())
    return makeFailure("execution", "launch", errorString(Error), Request);

  Object DeviceSupport = applyDeviceSupportPaths(S->Debugger, *Req);

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
  startEventPump(*S);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("launch");
  D["sessionId"] = Object(S->Id);
  D["deviceSupport"] = DeviceSupport;
  D["moduleDiagnostics"] = targetModuleDiagnostics(S->Target);
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
  invalidateVariableRefs(*S);
  if (isRemoteMixRequest(*Req)) {
    Object Result = connectRemote(S->Debugger, S->Target, S->Process, S->Id,
                                  *Req, Request, "attach", Program);
    if (isSuccessObject(Result))
      startEventPump(*S);
    return Result;
  }

  const uint64_t Pid = getUnsigned(*Req, "pid");
  if (!Pid)
    return makeFailure("execution", "attach", "missing pid", Request);

  lldb::SBError Error;
  if (canCreateTargetFromHostPath(Program))
    S->Target = S->Debugger.CreateTarget(Program.c_str(), nullptr, nullptr,
                                         true, Error);
  else
    S->Target = S->Debugger.CreateTarget(nullptr);
  if (!S->Target.IsValid())
    return makeFailure("execution", "attach", errorString(Error), Request);

  Object DeviceSupport = applyDeviceSupportPaths(S->Debugger, *Req);

  lldb::SBListener Listener = S->Debugger.GetListener();
  lldb::SBAttachInfo AttachInfo(static_cast<lldb::pid_t>(Pid));
  AttachInfo.SetListener(Listener);
  AttachInfo.SetResumeCount(1);
  Error.Clear();
  S->Process = S->Target.Attach(AttachInfo, Error);
  if (Error.Fail() || !S->Process.IsValid())
    return makeFailure("execution", "attach", errorString(Error), Request);
  lldb::StateType AttachState = waitForAttachState(Listener, S->Process, 30);
  if (!isUsableAttachState(AttachState))
    return makeAttachStateFailure("attach", AttachState, Request);
  startEventPump(*S);
  Object AttachDiagnostics = processDiagnostics(S->Process);
  Object ModuleDiagnostics = targetModuleDiagnostics(S->Target);
  lldb::StateType RunState = continueIfStopped(S->Process);

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("attach");
  D["sessionId"] = Object(S->Id);
  D["attachState"] = Object(stateName(AttachState));
  D["processState"] = Object(stateName(RunState));
  D["diagnostics"] = AttachDiagnostics;
  D["moduleDiagnostics"] = ModuleDiagnostics;
  D["deviceSupport"] = DeviceSupport;
  D["process"] = processToObject(S->Process);
  return Object(std::move(D));
}

Object LldbBackend::continueExecution(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("execution", "continue",
                       "unknown sessionId or no process", Request);
  lldb::StateType Before = S->Process.GetState();
  const uint32_t StopIDBefore = S->Process.GetStopID();
  const uint32_t ThreadsBefore = S->Process.GetNumThreads();
  Object DiagnosticsBefore = processDiagnostics(S->Process);
  // Capture baseline BEFORE issuing Continue so the wait below requires a
  // fresh state-change event consumed after this point — not a stale
  // LastState that already happened to be running.
  const uint64_t BaselineGen = snapshotEventGeneration(*S);
  lldb::SBError Error = S->Process.Continue();
  if (Error.Fail())
    return makeFailure("execution", "continue", errorString(Error), Request);
  invalidateVariableRefs(*S);
  TransitionWaitResult Wait = waitForSessionTransition(
      *S, BaselineGen, 1000, [](lldb::StateType State, uint32_t) {
        return State != lldb::eStateStopped && State != lldb::eStateCrashed;
      });
  lldb::StateType After = Wait.Observed ? Wait.State : S->Process.GetState();
  if (!Wait.Observed)
    updateSessionState(*S, After);
  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("continue");
  D["sessionId"] = Object(S->Id);
  D["stateBefore"] = Object(stateName(Before));
  D["stateAfter"] = Object(stateName(After));
  D["threadCountBefore"] = Object(static_cast<uint64_t>(ThreadsBefore));
  D["threadCountAfter"] =
      Object(static_cast<uint64_t>(S->Process.GetNumThreads()));
  D["stopIdBefore"] = Object(static_cast<uint64_t>(StopIDBefore));
  D["stopIdAfter"] = Object(static_cast<uint64_t>(S->Process.GetStopID()));
  D["diagnosticsBefore"] = DiagnosticsBefore;
  D["diagnosticsAfter"] = processDiagnostics(S->Process);
  D["process"] = processToObject(S->Process);
  return Object(std::move(D));
}

Object LldbBackend::pause(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("execution", "pause", "unknown sessionId or no process",
                       Request);
  lldb::StateType Before = S->Process.GetState();
  lldb::StateType After = Before;
  const uint32_t StopIDBefore = S->Process.GetStopID();
  bool ObservedStopTransition = false;
  // Use eStateInvalid as the "not run" sentinel so diagnostics distinguish
  // "the AsyncInterrupt path produced state X" from "we skipped that path
  // entirely because Before was already stopped".
  lldb::StateType AfterAsyncInterrupt = lldb::eStateInvalid;
  lldb::StateType AfterStop = lldb::eStateInvalid;
  std::string StopMode = "already-stopped";

  auto isStoppedState = [](lldb::StateType State) {
    return State == lldb::eStateStopped || State == lldb::eStateCrashed ||
           State == lldb::eStateExited || State == lldb::eStateDetached;
  };
  auto stoppedAfterCurrentRequest =
      [=](lldb::StateType State, uint32_t StopID) {
        return isStoppedState(State) && StopID > StopIDBefore;
      };

  const bool WasStopped = isStoppedState(Before);
  if (!WasStopped) {
    // Snapshot generation BEFORE issuing the interrupt so the subsequent
    // wait demands a freshly-consumed state-change event — otherwise a
    // stale LastState could spoof a "successful pause" without the
    // interrupt actually landing on the device.
    const uint64_t BaselineGen = snapshotEventGeneration(*S);
    S->Process.SendAsyncInterrupt();
    TransitionWaitResult Wait = waitForSessionTransition(
        *S, BaselineGen, 5000, stoppedAfterCurrentRequest);
    After = Wait.Observed ? Wait.State : S->Process.GetState();
    ObservedStopTransition =
        Wait.Observed || (isStoppedState(After) &&
                          S->Process.GetStopID() > StopIDBefore);
    AfterAsyncInterrupt = After;
    StopMode = "async-interrupt";
  }
  if (!WasStopped &&
      (!isStoppedState(After) || S->Process.GetStopID() <= StopIDBefore)) {
    // Same baseline trick around the slow Halt fallback. LLDB::Process::Halt
    // re-broadcasts the stop event on success, so our pump will see a new
    // generation; on failure we short-circuit via Error.Fail before the
    // wait runs.
    const uint64_t BaselineGen = snapshotEventGeneration(*S);
    lldb::SBError Error = S->Process.Stop();
    if (Error.Fail()) {
      std::ostringstream Message;
      Message << errorString(Error) << " before=" << stateName(Before)
              << " afterAsyncInterrupt=" << stateName(AfterAsyncInterrupt)
              << " current=" << stateName(S->Process.GetState())
              << " threads=" << S->Process.GetNumThreads()
              << " stopId=" << S->Process.GetStopID();
      return makeFailure("execution", "pause", Message.str(), Request);
    }
    TransitionWaitResult Wait = waitForSessionTransition(
        *S, BaselineGen, 5000, stoppedAfterCurrentRequest);
    After = Wait.Observed ? Wait.State : S->Process.GetState();
    ObservedStopTransition =
        Wait.Observed || (isStoppedState(After) &&
                          S->Process.GetStopID() > StopIDBefore);
    AfterStop = After;
    StopMode = "halt";
  }
  if (!WasStopped && !ObservedStopTransition) {
    std::ostringstream Message;
    Message << "pause did not observe a stop transition"
            << " before=" << stateName(Before)
            << " after=" << stateName(After)
            << " afterAsyncInterrupt=" << stateName(AfterAsyncInterrupt)
            << " afterStop=" << stateName(AfterStop)
            << " stopIdBefore=" << StopIDBefore
            << " stopIdAfter=" << S->Process.GetStopID()
            << " threads=" << S->Process.GetNumThreads();
    return makeFailure("execution", "pause", Message.str(), Request);
  }
  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object("pause");
  D["sessionId"] = Object(S->Id);
  D["stopMode"] = Object(StopMode);
  D["stateBefore"] = Object(stateName(Before));
  D["stateAfter"] = Object(stateName(After));
  D["stateAfterAsyncInterrupt"] = Object(stateName(AfterAsyncInterrupt));
  D["stateAfterStop"] = Object(stateName(AfterStop));
  D["stopIdBefore"] = Object(static_cast<uint64_t>(StopIDBefore));
  D["stopIdAfter"] = Object(static_cast<uint64_t>(S->Process.GetStopID()));
  if (!WasStopped && (After == lldb::eStateStopped ||
                      After == lldb::eStateCrashed))
    invalidateVariableRefs(*S);
  D["threadCount"] = Object(static_cast<uint64_t>(S->Process.GetNumThreads()));
  D["diagnostics"] = processDiagnostics(S->Process);
  D["process"] = processToObject(S->Process);
  return Object(std::move(D));
}

Object makeExecutionProcessResult(const std::string &SessionId,
                                  lldb::SBProcess Process,
                                  const char *Method) {
  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("execution");
  D["method"] = Object(Method);
  D["sessionId"] = Object(SessionId);
  D["process"] = processToObject(Process);
  return Object(std::move(D));
}

Object LldbBackend::stepIn(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("execution", "stepIn",
                       "unknown sessionId or no process", Request);
  const Dict *Req = asDict(Request);
  lldb::SBThread Thread = findThread(
      S->Process, Req ? getUnsigned(*Req, "threadId") : 0);
  if (!Thread.IsValid())
    return makeFailure("execution", "stepIn", "unknown thread", Request);
  invalidateVariableRefs(*S);
  Thread.StepInto(lldb::eOnlyDuringStepping);
  return makeExecutionProcessResult(S->Id, S->Process, "stepIn");
}

Object LldbBackend::stepOver(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("execution", "stepOver",
                       "unknown sessionId or no process", Request);
  const Dict *Req = asDict(Request);
  lldb::SBThread Thread = findThread(
      S->Process, Req ? getUnsigned(*Req, "threadId") : 0);
  if (!Thread.IsValid())
    return makeFailure("execution", "stepOver", "unknown thread", Request);
  lldb::SBError Error;
  Thread.StepOver(lldb::eOnlyDuringStepping, Error);
  if (Error.Fail())
    return makeFailure("execution", "stepOver", errorString(Error), Request);
  invalidateVariableRefs(*S);
  return makeExecutionProcessResult(S->Id, S->Process, "stepOver");
}

Object LldbBackend::stepOut(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("execution", "stepOut",
                       "unknown sessionId or no process", Request);
  const Dict *Req = asDict(Request);
  lldb::SBThread Thread = findThread(
      S->Process, Req ? getUnsigned(*Req, "threadId") : 0);
  if (!Thread.IsValid())
    return makeFailure("execution", "stepOut", "unknown thread", Request);
  lldb::SBError Error;
  Thread.StepOut(Error);
  if (Error.Fail())
    return makeFailure("execution", "stepOut", errorString(Error), Request);
  invalidateVariableRefs(*S);
  return makeExecutionProcessResult(S->Id, S->Process, "stepOut");
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

  auto Existing = S->SourceBreakpoints.find(Path);
  if (Existing != S->SourceBreakpoints.end()) {
    for (uint64_t Id : Existing->second)
      S->Target.BreakpointDelete(static_cast<lldb::break_id_t>(Id));
    S->SourceBreakpoints.erase(Existing);
  }

  Array Breakpoints;
  std::vector<uint64_t> CreatedIds;
  const Object *BPObj = lookup(*Req, "breakpoints");
  const Array *Requested = BPObj ? asArray(*BPObj) : nullptr;
  if (Requested) {
    for (const Object &Item : *Requested) {
      const Dict *BPReq = asDict(Item);
      if (!BPReq)
        continue;
      if (!getBool(*BPReq, "enabled", true))
        continue;
      const uint64_t Line = getUnsigned(*BPReq, "line");
      if (!Line)
        continue;
      lldb::SBBreakpoint BP =
          S->Target.BreakpointCreateByLocation(Path.c_str(),
                                               static_cast<uint32_t>(Line));
      if (BP.IsValid())
        CreatedIds.push_back(static_cast<uint64_t>(BP.GetID()));
      Dict D;
      D["id"] = Object(static_cast<uint64_t>(BP.GetID()));
      D["line"] = Object(Line);
      D["verified"] = boolean(BP.IsValid() && BP.GetNumLocations() > 0);
      D["locationCount"] = Object(static_cast<uint64_t>(BP.GetNumLocations()));
      Breakpoints.emplace_back(std::move(D));
    }
  }
  if (!CreatedIds.empty())
    S->SourceBreakpoints[Path] = std::move(CreatedIds);

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
  D["processState"] = Object(stateName(S->Process.GetState()));
  D["threadCount"] = Object(static_cast<uint64_t>(Count));
  D["diagnostics"] = processDiagnostics(S->Process);
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
  S->Process.SetSelectedThread(Thread);

  const uint64_t Start = getUnsigned(*Req, "startFrame");
  uint64_t Levels = getUnsigned(*Req, "levels",
                                std::numeric_limits<uint32_t>::max());
  const uint32_t FrameCount = Thread.GetNumFrames();
  Array Frames;
  for (uint64_t I = Start; I < FrameCount && Levels; ++I, --Levels) {
    lldb::SBFrame Frame = Thread.GetFrameAtIndex(static_cast<uint32_t>(I));
    if (Frame.IsValid()) {
      Thread.SetSelectedFrame(static_cast<uint32_t>(I));
      Frames.push_back(frameToObject(S->Target, Frame, Thread.GetThreadID(),
                                     static_cast<uint32_t>(I)));
    }
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("threads");
  D["method"] = Object("stackTrace");
  D["sessionId"] = Object(S->Id);
  D["processState"] = Object(stateName(S->Process.GetState()));
  D["threadId"] = Object(ThreadID);
  D["totalFrames"] = Object(static_cast<uint64_t>(FrameCount));
  D["diagnostics"] = processDiagnostics(S->Process);
  D["moduleDiagnostics"] = targetModuleDiagnostics(S->Target);
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
  D["threadId"] = Object(getUnsigned(*Req, "threadId"));
  D["frameIndex"] = Object(getUnsigned(*Req, "frameIndex"));
  D["arguments"] = Object(static_cast<uint64_t>(Args.size()));
  D["locals"] = Object(static_cast<uint64_t>(Locals.size()));
  D["diagnostics"] = processDiagnostics(S->Process);
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
  D["variablesReference"] = Object(Ref);
  D["requestedValues"] = Object(static_cast<uint64_t>(It->second.size()));
  D["returnedValues"] = Object(static_cast<uint64_t>(Variables.size()));
  D["variables"] = Object(std::move(Variables));
  return Object(std::move(D));
}

Object LldbBackend::registers(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Process.IsValid())
    return makeFailure("threads", "registers",
                       "unknown sessionId or no process", Request);
  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("threads", "registers",
                       "request must be a dictionary", Request);

  lldb::SBFrame Frame = findFrame(S->Process, *Req);
  if (!Frame.IsValid())
    return makeFailure("threads", "registers", "unknown frame", Request);

  Array Registers;
  lldb::SBValueList Sets = Frame.GetRegisters();
  const uint32_t SetCount = Sets.GetSize();
  for (uint32_t SetIndex = 0; SetIndex < SetCount; ++SetIndex) {
    lldb::SBValue Set = Sets.GetValueAtIndex(SetIndex);
    if (!Set.IsValid())
      continue;
    const char *SetName = Set.GetName();
    const uint32_t RegisterCount =
        std::min<uint32_t>(Set.GetNumChildren(512), 512);
    for (uint32_t RegisterIndex = 0; RegisterIndex < RegisterCount;
         ++RegisterIndex) {
      lldb::SBValue Register = Set.GetChildAtIndex(RegisterIndex);
      if (Register.IsValid())
        Registers.emplace_back(registerToObject(Register, SetName));
    }
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("threads");
  D["method"] = Object("registers");
  D["sessionId"] = Object(S->Id);
  D["registers"] = Object(std::move(Registers));
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
  D["threadId"] = Object(getUnsigned(*Req, "threadId"));
  D["frameIndex"] = Object(getUnsigned(*Req, "frameIndex"));
  D["expression"] = Object(Expression);
  D["childCount"] = Object(static_cast<uint64_t>(ChildCount));
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

Object LldbBackend::modules(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Target.IsValid())
    return makeFailure("modules", "list", "unknown sessionId or no target",
                       Request);

  Array Modules;
  const uint32_t Count = S->Target.GetNumModules();
  for (uint32_t I = 0; I < Count; ++I) {
    lldb::SBModule Module = S->Target.GetModuleAtIndex(I);
    if (Module.IsValid())
      Modules.emplace_back(moduleToObject(Module, S->Target, I));
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("modules");
  D["method"] = Object("list");
  D["sessionId"] = Object(S->Id);
  D["modules"] = Object(std::move(Modules));
  return Object(std::move(D));
}

Object LldbBackend::reloadSymbols(const Object &Request) {
  Session *S = findSession(Request);
  if (!S || !S->Target.IsValid())
    return makeFailure("modules", "reloadSymbols",
                       "unknown sessionId or no target", Request);

  const Dict *Req = asDict(Request);
  if (!Req)
    return makeFailure("modules", "reloadSymbols",
                       "request must be a dictionary", Request);

  std::string SymbolPath = getString(*Req, "symbolPath");
  if (SymbolPath.empty())
    SymbolPath = getString(*Req, "urlToSymbols");
  if (SymbolPath.empty())
    SymbolPath = getString(*Req, "path");

  Array Commands;
  if (!SymbolPath.empty())
    runLldbCommand(S->Debugger,
                   "target symbols add " + quoteCommandArg(SymbolPath),
                   Commands);

  Array Modules;
  const uint32_t Count = S->Target.GetNumModules();
  for (uint32_t I = 0; I < Count; ++I) {
    lldb::SBModule Module = S->Target.GetModuleAtIndex(I);
    if (Module.IsValid())
      Modules.emplace_back(moduleToObject(Module, S->Target, I));
  }

  Dict D;
  D["success"] = boolean(true);
  D["implemented"] = boolean(true);
  D["service"] = Object("modules");
  D["method"] = Object("reloadSymbols");
  D["sessionId"] = Object(S->Id);
  D["symbolPath"] = Object(SymbolPath);
  D["commands"] = Object(std::move(Commands));
  D["modules"] = Object(std::move(Modules));
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

void LldbBackend::invalidateVariableRefs(Session &S) {
  S.VariableRefs.clear();
  S.NextVariableRef = 1;
}

} // namespace ycode::debughost
