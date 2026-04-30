#include "MixDeviceBridge.h"

#if YCODE_MIXDEV_HAS_MIX_DEVICE
#include "mix_device.h"
#endif

#include <cstring>
#include <mutex>
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

Object stringOrEmpty(char *Value) {
  if (!Value)
    return Object("");
  std::string Copy(Value);
#if YCODE_MIXDEV_HAS_MIX_DEVICE
  mix_free_string(Value);
#endif
  return Object(std::move(Copy));
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
  mix_release_device(Device);

  Result["success"] = boolean(After == READY);
  Result["ready"] = boolean(After == READY);
  Result["connectUrl"] = Object(URL);
  Result["stateBefore"] = Object(prepareStateName(Before));
  Result["stateAfter"] = Object(prepareStateName(After));
  {
    std::lock_guard<std::mutex> Guard(Progress.Mutex);
    Result["progress"] = Object(std::move(Progress.Events));
  }
  Result["message"] = Object(After == READY ? "device debug tools are ready"
                                            : "device debug tools are not ready");
#else
  (void)Request;
  Result["success"] = boolean(false);
  Result["ready"] = boolean(false);
  Result["message"] = Object("mix_device SDK is not linked into this build");
#endif
  return Object(std::move(Result));
}

Object MixDeviceBridge::makeUnavailableResult(std::string Message) {
  Dict Result;
  Result["success"] = boolean(false);
  Result["message"] = Object(std::move(Message));
  Result["devices"] = Object(Array{});
  return Object(std::move(Result));
}

} // namespace ycode::debughost
