#include "MixDeviceBridge.h"

#if YCODE_MIXDEV_HAS_MIX_DEVICE
#include "mix_device.h"
#endif

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

using llvm::dtx::ns::Array;
using llvm::dtx::ns::Dict;
using llvm::dtx::ns::Object;

namespace ycode::debughost {

namespace {

Object boolean(bool Value) { return Object(Value); }

const Dict *asDict(const Object &Obj) {
  return std::get_if<Dict>(&Obj.Value);
}

const Object *lookup(const Dict &D, const std::string &Key) {
  auto It = D.find(Key);
  return It == D.end() ? nullptr : &It->second;
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

Object stringOrEmpty(char *Value) {
  if (!Value)
    return Object("");
  std::string Copy(Value);
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  mix_free_string(Value);
#endif
  return Object(std::move(Copy));
}

std::string takeString(char *Value) {
  if (!Value)
    return "";
  std::string Copy(Value);
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  mix_free_string(Value);
#endif
  return Copy;
}

std::string platformName(
#if YCODE_MIXDEV_HAS_MIX_DEVICE
    mix_device_platform Platform
#else
    int Platform
#endif
) {
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  switch (Platform) {
  case mix_device_platform_ios:
    return "ios";
  case mix_device_platform_android:
    return "android";
  case mix_device_platform_unsupported:
    return "unsupported";
  }
#endif
  (void)Platform;
  return "unknown";
}

#if YCODE_MIXDEV_HAS_MIX_DEVICE
struct RemoteSymbolFile {
  int32_t Index = 0;
  std::string RemotePath;
  uint64_t Size = 0;
  std::string LocalName;
};

std::string prepareStateName(uint32_t State) {
  switch (State) {
  case READY:
    return "ready";
  case UNPREPARED:
    return "unprepared";
  case DOWNLOADING:
    return "downloading";
  case MOUNTING:
    return "mounting";
  case QUERYING:
    return "querying";
  default:
    return "unknown";
  }
}

std::string makeDeviceUrl(const Dict &Request) {
  std::string URL = getString(Request, "connectUrl");
  if (!URL.empty())
    return URL;

  std::string Platform = getString(Request, "platform");
  if (Platform.empty())
    Platform = "ios";
  std::string DeviceID = getString(Request, "deviceId");
  if (DeviceID.empty())
    DeviceID = getString(Request, "id");
  if (DeviceID.empty())
    return "";
  return Platform + "://" + DeviceID;
}

std::string xmlEscape(llvm::StringRef Value) {
  std::string Result;
  Result.reserve(Value.size());
  for (char C : Value) {
    switch (C) {
    case '&':
      Result += "&amp;";
      break;
    case '<':
      Result += "&lt;";
      break;
    case '>':
      Result += "&gt;";
      break;
    case '"':
      Result += "&quot;";
      break;
    default:
      Result += C;
      break;
    }
  }
  return Result;
}

std::string sanitizeFileName(llvm::StringRef Value) {
  if (Value.empty())
    return "unknown";
  std::string Result;
  Result.reserve(Value.size());
  for (char C : Value) {
    switch (C) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
      Result += '_';
      break;
    default:
      Result += C;
      break;
    }
  }
  return Result.empty() ? "unknown" : Result;
}

std::string localSymbolName(int32_t Index, llvm::StringRef RemotePath) {
  std::string Name = llvm::sys::path::filename(RemotePath).str();
  if (Name.empty())
    Name = "symbol";
  std::ostringstream Stream;
  Stream << std::setw(4) << std::setfill('0') << Index << "-"
         << sanitizeFileName(Name);
  return Stream.str();
}

std::string toHex(const std::array<uint8_t, 32> &Bytes) {
  static const char Hex[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Bytes.size() * 2);
  for (uint8_t Byte : Bytes) {
    Result.push_back(Hex[(Byte >> 4) & 0xf]);
    Result.push_back(Hex[Byte & 0xf]);
  }
  return Result;
}

std::string computeSymbolSignature(std::vector<RemoteSymbolFile> Files) {
  std::sort(Files.begin(), Files.end(),
            [](const RemoteSymbolFile &A, const RemoteSymbolFile &B) {
              return A.RemotePath < B.RemotePath;
            });
  std::string Text;
  for (const RemoteSymbolFile &File : Files) {
    Text += File.RemotePath;
    Text += ":";
    Text += std::to_string(File.Size);
    Text += "\n";
  }
  llvm::SHA256 Hash;
  Hash.update(Text);
  return toHex(Hash.final());
}

std::string shortSignature(llvm::StringRef Signature) {
  if (Signature.empty())
    return "nosig";
  return Signature.take_front(12).str();
}

std::string deviceSupportRoot() {
  llvm::SmallString<260> Root;
  if (auto LocalAppData = llvm::sys::Process::GetEnv("LOCALAPPDATA")) {
    Root = *LocalAppData;
  } else if (auto UserProfile = llvm::sys::Process::GetEnv("USERPROFILE")) {
    Root = *UserProfile;
    llvm::sys::path::append(Root, "AppData", "Local");
  } else {
    Root = ".";
  }
  llvm::sys::path::append(Root, "Mix", "DeviceSupport");
  return Root.str().str();
}

std::string appendPath(llvm::StringRef Base, llvm::StringRef A) {
  llvm::SmallString<260> Path(Base);
  llvm::sys::path::append(Path, A);
  return Path.str().str();
}

std::string appendPath(llvm::StringRef Base, llvm::StringRef A,
                       llvm::StringRef B) {
  llvm::SmallString<260> Path(Base);
  llvm::sys::path::append(Path, A, B);
  return Path.str().str();
}

std::string cacheRootFor(mix_device_t *Device, llvm::StringRef Signature) {
  std::string OSVersion = takeString(mix_device_get_os_ver(Device));
  std::string Model = takeString(mix_device_get_model(Device));
  std::string Key = sanitizeFileName(platformName(mix_device_get_platform(Device)) +
                                     "-" + OSVersion + "-" + Model);
  return appendPath(deviceSupportRoot(), Key + "-" + shortSignature(Signature));
}

bool fileExistsWithSize(llvm::StringRef Path, uint64_t ExpectedSize) {
  uint64_t Size = 0;
  if (llvm::sys::fs::file_size(Path, Size))
    return false;
  if (ExpectedSize == 0)
    return Size > 0;
  return Size == ExpectedSize;
}

std::string readFileOrEmpty(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  if (!Buffer)
    return "";
  return Buffer.get()->getBuffer().str();
}

bool manifestHas(llvm::StringRef Text, llvm::StringRef Name,
                 llvm::StringRef Value) {
  std::string Needle = (Name + "=\"" + Value + "\"").str();
  return Text.contains(Needle);
}

Object symbolStatusObject(std::string Method, std::string State, std::string Message,
                          std::string CacheRoot, std::string Signature,
                          const std::vector<RemoteSymbolFile> &Files,
                          bool ReusedOnly = false, uint64_t Downloaded = 0,
                          uint64_t Reused = 0) {
  uint64_t Bytes = 0;
  for (const RemoteSymbolFile &File : Files)
    Bytes += File.Size;

  Dict D;
  D["success"] = boolean(State != "failed");
  D["implemented"] = boolean(true);
  D["service"] = Object("deviceSymbols");
  D["method"] = Object(std::move(Method));
  D["state"] = Object(std::move(State));
  D["message"] = Object(std::move(Message));
  D["cacheRoot"] = Object(std::move(CacheRoot));
  D["signature"] = Object(std::move(Signature));
  D["fileCount"] = Object(static_cast<uint64_t>(Files.size()));
  D["cacheBytes"] = Object(Bytes);
  D["reusedOnly"] = boolean(ReusedOnly);
  D["downloadedFiles"] = Object(Downloaded);
  D["reusedFiles"] = Object(Reused);
  return Object(std::move(D));
}

void emitDeviceSymbolsEvent(const DebugHostEventSink &EventSink,
                            llvm::StringRef EventName,
                            const Object &Payload) {
  if (!EventSink)
    return;

  Dict Event;
  Event["event"] = Object(EventName.str());
  Event["service"] = Object("deviceSymbols");
  if (const Dict *PayloadDict = asDict(Payload)) {
    for (const auto &Entry : *PayloadDict)
      Event[Entry.first] = Entry.second;
  } else {
    Event["payload"] = Payload;
  }
  EventSink(Object(std::move(Event)));
}

void emitDeviceSymbolsProgress(const DebugHostEventSink &EventSink,
                               const RemoteSymbolFile &File,
                               uint64_t FileIndex, uint64_t FileCount,
                               uint64_t Received, uint64_t Total,
                               uint64_t AggregateReceived,
                               uint64_t AggregateTotal) {
  if (!EventSink)
    return;

  Dict D;
  D["method"] = Object("prefetch");
  D["state"] = Object("refreshing");
  D["currentFile"] = Object(File.RemotePath);
  D["fileIndex"] = Object(FileIndex);
  D["fileCount"] = Object(FileCount);
  D["received"] = Object(Received);
  D["total"] = Object(Total);
  D["aggregateReceived"] = Object(AggregateReceived);
  D["aggregateTotal"] = Object(AggregateTotal);
  emitDeviceSymbolsEvent(EventSink, "deviceSymbols.progress", Object(std::move(D)));
}

Object evaluateSymbolCache(std::string Method, mix_device_t *Device,
                           std::string CacheRoot,
                           std::string Signature,
                           const std::vector<RemoteSymbolFile> &Files,
                           bool StaleWhenMissing) {
  std::string ManifestPath = appendPath(CacheRoot, "manifest.xml");
  if (!llvm::sys::fs::exists(ManifestPath))
    return symbolStatusObject(std::move(Method),
                              StaleWhenMissing ? "stale" : "notFetched",
                              "No local cache.", std::move(CacheRoot),
                              std::move(Signature), Files);

  std::string Manifest = readFileOrEmpty(ManifestPath);
  if (Manifest.empty())
    return symbolStatusObject(std::move(Method), "failed",
                              "Manifest is empty or unreadable.",
                              std::move(CacheRoot), std::move(Signature),
                              Files);
  if (!manifestHas(Manifest, "state", "complete"))
    return symbolStatusObject(std::move(Method), "partial",
                              "Previous prefetch did not complete.",
                              std::move(CacheRoot), std::move(Signature),
                              Files);
  if (!manifestHas(Manifest, "remoteFileSetSignature", Signature))
    return symbolStatusObject(std::move(Method), "stale",
                              "Remote file set changed.",
                              std::move(CacheRoot), std::move(Signature),
                              Files);

  std::string FilesRoot = appendPath(CacheRoot, "files");
  for (const RemoteSymbolFile &File : Files) {
    if (!fileExistsWithSize(appendPath(FilesRoot, File.LocalName), File.Size))
      return symbolStatusObject(std::move(Method), "partial",
                                "Local file missing or truncated.",
                                std::move(CacheRoot), std::move(Signature),
                                Files);
  }

  (void)Device;
  return symbolStatusObject(std::move(Method), "upToDate",
                            "System symbols are up to date.",
                            std::move(CacheRoot), std::move(Signature), Files);
}

void writeSymbolManifest(mix_device_t *Device, llvm::StringRef CacheRoot,
                         llvm::StringRef Signature,
                         const std::vector<RemoteSymbolFile> &Files,
                         llvm::StringRef State, uint64_t Downloaded,
                         uint64_t Reused) {
  llvm::sys::fs::create_directories(CacheRoot);
  std::string Tmp = appendPath(CacheRoot, "manifest.xml.tmp");
  std::error_code EC;
  llvm::raw_fd_ostream OS(Tmp, EC, llvm::sys::fs::OF_Text);
  if (EC)
    return;

  OS << "<deviceSymbols state=\"" << State << "\" platform=\""
     << xmlEscape(platformName(mix_device_get_platform(Device)))
     << "\" productVersion=\"" << xmlEscape(takeString(mix_device_get_os_ver(Device)))
     << "\" model=\"" << xmlEscape(takeString(mix_device_get_model(Device)))
     << "\" remoteFileCount=\"" << Files.size()
     << "\" remoteFileSetSignature=\"" << xmlEscape(Signature)
     << "\" downloadedFiles=\"" << Downloaded << "\" reusedFiles=\"" << Reused
     << "\">\n";
  OS << "  <lastSeenDevices><device udid=\""
     << xmlEscape(takeString(mix_device_get_uid(Device))) << "\" name=\""
     << xmlEscape(takeString(mix_device_get_name(Device))) << "\" /></lastSeenDevices>\n";
  OS << "  <files>\n";
  for (const RemoteSymbolFile &File : Files) {
    OS << "    <file index=\"" << File.Index << "\" remotePath=\""
       << xmlEscape(File.RemotePath) << "\" size=\"" << File.Size
       << "\" localPath=\"files/" << xmlEscape(File.LocalName) << "\" />\n";
  }
  OS << "  </files>\n</deviceSymbols>\n";
  OS.close();

  std::string Final = appendPath(CacheRoot, "manifest.xml");
  llvm::sys::fs::remove(Final);
  llvm::sys::fs::rename(Tmp, Final);
}

bool listSymbolFiles(FetchSymbol *Fetch, std::vector<RemoteSymbolFile> &Files) {
  int64_t Count = mix_fetch_symbol_list(
      Fetch,
      [](const char *Path, uint64_t Size, void *User) {
        auto *Files = static_cast<std::vector<RemoteSymbolFile> *>(User);
        RemoteSymbolFile File;
        File.Index = static_cast<int32_t>(Files->size());
        File.RemotePath = Path ? Path : "";
        File.Size = Size;
        File.LocalName = localSymbolName(File.Index, File.RemotePath);
        Files->push_back(std::move(File));
      },
      &Files);
  return Count >= 0;
}

Object withConnectedDevice(
    const Object &Request,
    llvm::function_ref<Object(mix_device_t *, const Dict &)> Body) {
  const Dict *Req = asDict(Request);
  if (!Req) {
    Dict D;
    D["success"] = boolean(false);
    D["implemented"] = boolean(true);
    D["service"] = Object("deviceSymbols");
    D["state"] = Object("failed");
    D["message"] = Object("request must be a dictionary");
    return Object(std::move(D));
  }

  std::string URL = makeDeviceUrl(*Req);
  if (URL.empty()) {
    Dict D;
    D["success"] = boolean(false);
    D["implemented"] = boolean(true);
    D["service"] = Object("deviceSymbols");
    D["state"] = Object("failed");
    D["message"] = Object("missing deviceId or connectUrl");
    return Object(std::move(D));
  }

  struct ConnectState {
    std::string Error;
  } Connect;
  mix_device_t *Device = nullptr;
  mix_connect_device(
      URL.c_str(), &Device,
      [](const char *Error, void *User) {
        auto *State = static_cast<ConnectState *>(User);
        if (Error)
          State->Error = Error;
      },
      &Connect);

  if (!Device || !Connect.Error.empty()) {
    Dict D;
    D["success"] = boolean(false);
    D["implemented"] = boolean(true);
    D["service"] = Object("deviceSymbols");
    D["state"] = Object("failed");
    D["connectUrl"] = Object(URL);
    D["message"] = Object(Connect.Error.empty() ? "mix_connect_device failed"
                                                : Connect.Error);
    if (Device)
      mix_release_device(Device);
    return Object(std::move(D));
  }

  Object Result = Body(Device, *Req);
  mix_release_device(Device);
  return Result;
}

Object featuresToObject(const mix_features &Features) {
  Dict D;
  D["supportLog"] = boolean(Features.support_log != 0);
  D["supportStat"] = boolean(Features.support_stat != 0);
  D["supportInstruments"] = boolean(Features.support_instruments != 0);
  D["supportPerfetto"] = boolean(Features.support_perfetto != 0);
  D["supportFileIo"] = boolean(Features.support_file_io != 0);
  D["supportScreenshot"] = boolean(Features.support_screenshot != 0);
  D["supportScreenrecord"] = boolean(Features.support_screenrecord != 0);
  D["supportDebug"] = boolean(Features.support_debug != 0);
  D["supportSocketForward"] = boolean(Features.support_socket_forward != 0);
  D["supportSocketReverse"] = boolean(Features.support_socket_reverse != 0);
  D["supportAppList"] = boolean(Features.support_app_list != 0);
  D["supportAppNotification"] = boolean(Features.support_app_notification != 0);
  D["supportShell"] = boolean(Features.support_shell != 0);
  D["supportAppInstall"] = boolean(Features.support_app_install != 0);
  D["supportImageMount"] = boolean(Features.support_image_mount != 0);
  D["supportSimulateLocation"] = boolean(Features.support_simulate_location != 0);
  return Object(std::move(D));
}

Object deviceToObject(mix_device_t *Device) {
  Dict D;
  D["id"] = stringOrEmpty(mix_device_get_uid(Device));
  D["name"] = stringOrEmpty(mix_device_get_name(Device));
  D["brand"] = stringOrEmpty(mix_device_get_brand(Device));
  D["model"] = stringOrEmpty(mix_device_get_model(Device));
  D["osVersion"] = stringOrEmpty(mix_device_get_os_ver(Device));
  D["platform"] = Object(platformName(mix_device_get_platform(Device)));
  D["supportsX86"] = boolean(mix_device_support_abi(Device, mix_device_abi_x86));
  D["supportsX64"] = boolean(mix_device_support_abi(Device, mix_device_abi_x64));
  D["supportsArm"] = boolean(mix_device_support_abi(Device, mix_device_abi_arm));
  D["supportsArm64"] = boolean(mix_device_support_abi(Device, mix_device_abi_arm64));

  mix_features Features;
  std::memset(&Features, 0, sizeof(Features));
  mix_device_get_features(Device, &Features, sizeof(Features));
  D["features"] = featuresToObject(Features);
  D["debugAvailable"] = boolean(Features.support_debug != 0);
  return Object(std::move(D));
}
#endif

} // namespace

Object MixDeviceBridge::makeCapabilities() const {
  Dict D;
  D["hostName"] = Object("YCode.DebugHost");
  D["hostVersion"] = Object("0.1.0");
  D["protocolVersion"] = Object("0.1");
  D["dtxRuntime"] = boolean(true);
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  D["mixDevice"] = Object(mix_version());
  D["supportsDeviceList"] = boolean(true);
  D["supportsDevicePrepareDebug"] = boolean(true);
#else
  D["mixDevice"] = Object("unavailable");
  D["supportsDeviceList"] = boolean(false);
  D["supportsDevicePrepareDebug"] = boolean(false);
#endif
  return Object(std::move(D));
}

Object MixDeviceBridge::listDevices() const {
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  mix_devices_t *Devices = nullptr;
  mix_list_devices(&Devices);
  Array Items;
  const size_t Count = Devices ? mix_devices_count(Devices) : 0;
  Items.reserve(Count);
  for (size_t I = 0; I < Count; ++I) {
    mix_device_t *Device = mix_device_at(Devices, I);
    if (Device)
      Items.push_back(deviceToObject(Device));
  }
  if (Devices)
    mix_release_devices(Devices);

  Dict Result;
  Result["success"] = boolean(true);
  Result["devices"] = Object(std::move(Items));
  return Object(std::move(Result));
#else
  return makeUnavailableResult("mix_device SDK is not linked into this build");
#endif
}

Object MixDeviceBridge::prepareDebug(const Object &Request) const {
  Dict Result;
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  const Dict *Req = asDict(Request);
  if (!Req) {
    Result["success"] = boolean(false);
    Result["ready"] = boolean(false);
    Result["message"] = Object("request must be a dictionary");
    return Object(std::move(Result));
  }

  std::string URL = makeDeviceUrl(*Req);
  if (URL.empty()) {
    Result["success"] = boolean(false);
    Result["ready"] = boolean(false);
    Result["message"] = Object("missing deviceId or connectUrl");
    return Object(std::move(Result));
  }

  struct ConnectState {
    std::string Error;
  } Connect;
  mix_device_t *Device = nullptr;
  mix_connect_device(
      URL.c_str(), &Device,
      [](const char *Error, void *User) {
        auto *State = static_cast<ConnectState *>(User);
        if (Error)
          State->Error = Error;
      },
      &Connect);

  if (!Device || !Connect.Error.empty()) {
    Result["success"] = boolean(false);
    Result["ready"] = boolean(false);
    Result["connectUrl"] = Object(URL);
    Result["message"] =
        Object(Connect.Error.empty() ? "mix_connect_device failed"
                                     : Connect.Error);
    if (Device)
      mix_release_device(Device);
    return Object(std::move(Result));
  }

  const bool HasToolsBefore = mix_device_has_developer_image(Device);
  if (HasToolsBefore) {
    Result["success"] = boolean(true);
    Result["ready"] = boolean(true);
    Result["connectUrl"] = Object(URL);
    Result["stateBefore"] = Object("ready");
    Result["stateAfter"] = Object("ready");
    Result["hasDeveloperImageBefore"] = boolean(true);
    Result["hasDeveloperImageAfter"] = boolean(true);
    Result["progress"] = Object(Array{});
    Result["message"] = Object("device debug tools are already ready");
    mix_release_device(Device);
    return Object(std::move(Result));
  }

  struct PrepareProgress {
    std::mutex Mutex;
    Array Events;
  } Progress;

  uint32_t Before = mix_device_get_prepare_state(Device);
  mix_device_prepare_tools(
      Device,
      [](uint32_t Stage, uint32_t Value, void *User) {
        auto *Progress = static_cast<PrepareProgress *>(User);
        Dict E;
        E["stage"] = Object(static_cast<uint64_t>(Stage));
        E["stageName"] = Object(prepareStateName(Stage));
        E["progress"] = Object(static_cast<uint64_t>(Value));
        std::lock_guard<std::mutex> Guard(Progress->Mutex);
        Progress->Events.emplace_back(std::move(E));
      },
      &Progress);
  uint32_t After = mix_device_get_prepare_state(Device);
  const bool HasToolsAfter = mix_device_has_developer_image(Device);
  mix_release_device(Device);

  const bool Ready = After == READY || HasToolsAfter;
  Result["success"] = boolean(Ready);
  Result["ready"] = boolean(Ready);
  Result["connectUrl"] = Object(URL);
  Result["stateBefore"] = Object(prepareStateName(Before));
  Result["stateAfter"] = Object(prepareStateName(After));
  Result["hasDeveloperImageBefore"] = boolean(HasToolsBefore);
  Result["hasDeveloperImageAfter"] = boolean(HasToolsAfter);
  {
    std::lock_guard<std::mutex> Guard(Progress.Mutex);
    Result["progress"] = Object(std::move(Progress.Events));
  }
  Result["message"] = Object(Ready ? "device debug tools are ready"
                                   : "device debug tools are not ready");
#else
  (void)Request;
  Result["success"] = boolean(false);
  Result["ready"] = boolean(false);
  Result["message"] = Object("mix_device SDK is not linked into this build");
#endif
  return Object(std::move(Result));
}

Object MixDeviceBridge::deviceSymbolsStatus(const Object &Request) const {
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  return withConnectedDevice(Request, [](mix_device_t *Device, const Dict &Req) {
    (void)Req;
    if (mix_device_get_platform(Device) != mix_device_platform_ios)
      return symbolStatusObject("status", "failed",
                                "Only iOS devices support system symbol prefetch.",
                                "", "", {});

    FetchSymbol *Fetch = mix_create_fetch_symbol(Device);
    if (!Fetch)
      return symbolStatusObject(
          "status", "failed",
          "remote symbol service unavailable or developer image missing.", "",
          "", {});

    std::vector<RemoteSymbolFile> Files;
    bool Listed = listSymbolFiles(Fetch, Files);
    mix_release_fetch_symbol(Fetch);
    if (!Listed || Files.empty())
      return symbolStatusObject("status", "failed",
                                "failed to list remote symbol files.", "", "",
                                Files);

    std::string Signature = computeSymbolSignature(Files);
    std::string CacheRoot = cacheRootFor(Device, Signature);
    return evaluateSymbolCache("status", Device, std::move(CacheRoot),
                               std::move(Signature), Files,
                               /*StaleWhenMissing=*/false);
  });
#else
  (void)Request;
  return makeUnavailableResult("mix_device SDK is not linked into this build");
#endif
}

Object MixDeviceBridge::deviceSymbolsValidate(const Object &Request) const {
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  return withConnectedDevice(Request, [](mix_device_t *Device, const Dict &Req) {
    (void)Req;
    if (mix_device_get_platform(Device) != mix_device_platform_ios)
      return symbolStatusObject("validate", "failed",
                                "Only iOS devices support system symbol prefetch.",
                                "", "", {});

    FetchSymbol *Fetch = mix_create_fetch_symbol(Device);
    if (!Fetch)
      return symbolStatusObject(
          "validate", "failed",
          "remote symbol service unavailable or developer image missing.", "",
          "", {});

    std::vector<RemoteSymbolFile> Files;
    bool Listed = listSymbolFiles(Fetch, Files);
    mix_release_fetch_symbol(Fetch);
    if (!Listed || Files.empty())
      return symbolStatusObject("validate", "failed",
                                "failed to list remote symbol files.", "", "",
                                Files);

    std::string Signature = computeSymbolSignature(Files);
    std::string CacheRoot = cacheRootFor(Device, Signature);
    return evaluateSymbolCache("validate", Device, std::move(CacheRoot),
                               std::move(Signature), Files,
                               /*StaleWhenMissing=*/true);
  });
#else
  (void)Request;
  return makeUnavailableResult("mix_device SDK is not linked into this build");
#endif
}

Object MixDeviceBridge::deviceSymbolsPrefetch(
    const Object &Request, const DebugHostEventSink &EventSink) const {
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  return withConnectedDevice(Request, [&](mix_device_t *Device, const Dict &Req) {
    auto Fail = [&](std::string Message, std::string CacheRoot,
                    std::string Signature,
                    const std::vector<RemoteSymbolFile> &Files,
                    uint64_t Downloaded = 0, uint64_t Reused = 0) {
      Object Result = symbolStatusObject("prefetch", "failed", std::move(Message),
                                         std::move(CacheRoot),
                                         std::move(Signature), Files, false,
                                         Downloaded, Reused);
      emitDeviceSymbolsEvent(EventSink, "deviceSymbols.failed", Result);
      return Result;
    };

    if (mix_device_get_platform(Device) != mix_device_platform_ios)
      return Fail("Only iOS devices support system symbol prefetch.", "", "",
                  {});

    const bool Force = getBool(Req, "force", false);
    FetchSymbol *Fetch = mix_create_fetch_symbol(Device);
    if (!Fetch)
      return Fail("remote symbol service unavailable or developer image missing.",
                  "", "", {});

    std::vector<RemoteSymbolFile> Files;
    if (!listSymbolFiles(Fetch, Files) || Files.empty()) {
      mix_release_fetch_symbol(Fetch);
      return Fail("failed to list remote symbol files.", "", "", Files);
    }

    std::string Signature = computeSymbolSignature(Files);
    std::string CacheRoot = cacheRootFor(Device, Signature);
    Object Current = evaluateSymbolCache("prefetch", Device, CacheRoot,
                                         Signature, Files,
                                         /*StaleWhenMissing=*/true);
    if (!Force) {
      if (const Dict *CurrentDict = asDict(Current)) {
        if (getString(*CurrentDict, "state") == "upToDate") {
          mix_release_fetch_symbol(Fetch);
          Object Result = symbolStatusObject(
              "prefetch", "upToDate", "System symbols already up to date.",
              std::move(CacheRoot), std::move(Signature), Files,
              /*ReusedOnly=*/true, /*Downloaded=*/0, /*Reused=*/Files.size());
          emitDeviceSymbolsEvent(EventSink, "deviceSymbols.completed", Result);
          return Result;
        }
      }
    }

    std::error_code EC = llvm::sys::fs::create_directories(CacheRoot);
    if (EC) {
      mix_release_fetch_symbol(Fetch);
      return Fail("cache permission error: " + EC.message(),
                  std::move(CacheRoot), std::move(Signature), Files);
    }
    std::string FilesRoot = appendPath(CacheRoot, "files");
    EC = llvm::sys::fs::create_directories(FilesRoot);
    if (EC) {
      mix_release_fetch_symbol(Fetch);
      return Fail("cache permission error: " + EC.message(),
                  std::move(CacheRoot), std::move(Signature), Files);
    }

    writeSymbolManifest(Device, CacheRoot, Signature, Files, "incomplete", 0,
                        0);

    uint64_t Downloaded = 0;
    uint64_t Reused = 0;
    uint64_t AggregateTotal = 0;
    for (const RemoteSymbolFile &File : Files)
      AggregateTotal += File.Size;
    uint64_t AggregateDone = 0;
    for (const RemoteSymbolFile &File : Files) {
      std::string Output = appendPath(FilesRoot, File.LocalName);
      if (!Force && fileExistsWithSize(Output, File.Size)) {
        ++Reused;
        AggregateDone += File.Size;
        emitDeviceSymbolsProgress(EventSink, File, File.Index + 1, Files.size(),
                                  File.Size, File.Size, AggregateDone,
                                  AggregateTotal);
        continue;
      }

      std::string Temp = Output + ".download";
      llvm::sys::fs::remove(Temp);
      struct ProgressState {
        const DebugHostEventSink *Sink;
        const RemoteSymbolFile *File;
        uint64_t FileCount;
        uint64_t AggregateBase;
        uint64_t AggregateTotal;
      } Progress{&EventSink, &File, static_cast<uint64_t>(Files.size()),
                 AggregateDone, AggregateTotal};
      int64_t Written = mix_fetch_symbol_download(
          Fetch, File.Index, Temp.c_str(),
          [](uint64_t Received, uint64_t Total, void *User) {
            auto *Progress = static_cast<ProgressState *>(User);
            emitDeviceSymbolsProgress(
                *Progress->Sink, *Progress->File, Progress->File->Index + 1,
                Progress->FileCount, Received, Total,
                Progress->AggregateBase + Received, Progress->AggregateTotal);
          },
          &Progress);
      if (Written < 0) {
        mix_release_fetch_symbol(Fetch);
        return Fail("download failed: " + File.RemotePath, std::move(CacheRoot),
                    std::move(Signature), Files, Downloaded, Reused);
      }

      llvm::sys::fs::remove(Output);
      EC = llvm::sys::fs::rename(Temp, Output);
      if (EC) {
        mix_release_fetch_symbol(Fetch);
        return Fail("cache rename failed: " + EC.message(),
                    std::move(CacheRoot), std::move(Signature), Files,
                    Downloaded, Reused);
      }
      ++Downloaded;
      AggregateDone += static_cast<uint64_t>(Written);
      emitDeviceSymbolsProgress(EventSink, File, File.Index + 1, Files.size(),
                                static_cast<uint64_t>(Written), File.Size,
                                AggregateDone, AggregateTotal);
    }

    writeSymbolManifest(Device, CacheRoot, Signature, Files, "complete",
                        Downloaded, Reused);
    mix_release_fetch_symbol(Fetch);
    Object Result = symbolStatusObject(
        "prefetch", "upToDate",
        Downloaded == 0 ? "System symbols already up to date."
                        : "System symbols prefetched.",
        std::move(CacheRoot), std::move(Signature), Files,
        /*ReusedOnly=*/Downloaded == 0, Downloaded, Reused);
    emitDeviceSymbolsEvent(EventSink, "deviceSymbols.completed", Result);
    return Result;
  });
#else
  (void)Request;
  return makeUnavailableResult("mix_device SDK is not linked into this build");
#endif
}

Object MixDeviceBridge::makeUnavailableResult(std::string Message) {
  Dict Result;
  Result["success"] = boolean(false);
  Result["message"] = Object(std::move(Message));
  Result["devices"] = Object(Array{});
  return Object(std::move(Result));
}

} // namespace ycode::debughost
