#include "a0/phase0/wpd_transport.hpp"

#include <windows.h>
#include <portabledeviceapi.h>
#include <portabledevice.h>
#include <WpdMtpExtensions.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace a0::phase0 {

std::string DeriveWpdStableIdentity(std::string_view device_serial_utf8) {
    constexpr std::size_t kMaximumSerialBytes = 512;
    if (device_serial_utf8.empty() || device_serial_utf8.size() > kMaximumSerialBytes ||
        device_serial_utf8.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("WPD device serial is missing or invalid");
    }
    std::vector<unsigned char> material;
    constexpr std::string_view domain = "a0camera-wpd-device-serial-identity-v2";
    material.insert(material.end(), domain.begin(), domain.end());
    material.push_back(0);
    const auto length = static_cast<std::uint32_t>(device_serial_utf8.size());
    material.push_back(static_cast<unsigned char>((length >> 24U) & 0xFFU));
    material.push_back(static_cast<unsigned char>((length >> 16U) & 0xFFU));
    material.push_back(static_cast<unsigned char>((length >> 8U) & 0xFFU));
    material.push_back(static_cast<unsigned char>(length & 0xFFU));
    material.insert(material.end(), device_serial_utf8.begin(), device_serial_utf8.end());
    return Sha256Hex(material);
}

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaximumJpegBytes = 256U * 1024U * 1024U;
constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr auto kCandidateSettle = std::chrono::milliseconds(750);

std::string HResultText(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8)
         << static_cast<unsigned long>(result);
    return text.str();
}

std::string DriverErrorText(DWORD result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << result;
    return text.str();
}

std::string CommandTargetPolicyName(WpdCommandTargetPolicy policy) {
    switch (policy) {
    case WpdCommandTargetPolicy::functional:
        return "functional";
    case WpdCommandTargetPolicy::omit:
        return "omit";
    }
    return "unknown";
}

std::string StatusAccessName(WpdStatusAccess access) {
    return access == WpdStatusAccess::read_only ? "read-only" : "read-write";
}

void Check(HRESULT result, std::string_view category, std::string_view operation) {
    if (FAILED(result)) {
        throw TransportError(std::string(category), std::string(operation) + " failed: " + HResultText(result));
    }
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "unknown";
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), size, nullptr, nullptr);
    return output;
}

ComPtr<IPortableDeviceValues> NewValues(std::string_view category) {
    ComPtr<IPortableDeviceValues> values;
    Check(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&values)), category, "create WPD values");
    return values;
}

ComPtr<IPortableDeviceKeyCollection> NewKeys(std::string_view category) {
    ComPtr<IPortableDeviceKeyCollection> keys;
    Check(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&keys)), category, "create WPD key collection");
    return keys;
}

ComPtr<IPortableDevicePropVariantCollection> NewPropVariantCollection(std::string_view category) {
    ComPtr<IPortableDevicePropVariantCollection> values;
    Check(CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&values)), category, "create WPD PROPVARIANT collection");
    return values;
}

ComPtr<IPortableDevice> OpenDevice(
    std::wstring_view pnp_id,
    std::string_view category,
    DWORD desired_access = GENERIC_READ | GENERIC_WRITE) {
    ComPtr<IPortableDevice> device;
    Check(CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&device)), category, "create WPD device");
    auto client = NewValues(category);
    Check(client->SetStringValue(WPD_CLIENT_NAME, L"A0CameraStitcher Phase0"), category, "set WPD client name");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 0), category, "set WPD client major version");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 1), category, "set WPD client minor version");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0), category, "set WPD client revision");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS, desired_access),
        category, "set WPD client desired access");
    Check(client->SetUnsignedIntegerValue(
        WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION),
        category, "set WPD client security quality of service");
    const std::wstring id(pnp_id);
    Check(device->Open(id.c_str(), client.Get()), category, "open WPD device");
    return device;
}

struct SupportedWpdCommands {
    HRESULT hresult{E_FAIL};
    bool still_image_capture{};
    bool vendor_opcode_query{};
};

SupportedWpdCommands GetSupportedWpdCommands(IPortableDevice* device) {
    SupportedWpdCommands result;
    ComPtr<IPortableDeviceCapabilities> capabilities;
    if (device == nullptr) return result;
    result.hresult = device->Capabilities(&capabilities);
    if (result.hresult != S_OK || !capabilities) {
        if (result.hresult == S_OK) result.hresult = E_POINTER;
        return result;
    }
    ComPtr<IPortableDeviceKeyCollection> commands;
    result.hresult = capabilities->GetSupportedCommands(&commands);
    if (result.hresult != S_OK || !commands) {
        if (result.hresult == S_OK) result.hresult = E_POINTER;
        return result;
    }
    DWORD count = 0;
    result.hresult = commands->GetCount(&count);
    if (result.hresult != S_OK) return result;
    for (DWORD index = 0; index < count; ++index) {
        PROPERTYKEY command{};
        const HRESULT get_result = commands->GetAt(index, &command);
        if (get_result != S_OK) {
            result.hresult = get_result;
            return result;
        }
        result.still_image_capture = result.still_image_capture ||
            IsEqualPropertyKey(command, WPD_COMMAND_STILL_IMAGE_CAPTURE_INITIATE);
        result.vendor_opcode_query = result.vendor_opcode_query ||
            IsEqualPropertyKey(command, WPD_COMMAND_MTP_EXT_GET_SUPPORTED_VENDOR_OPCODES);
    }
    return result;
}

std::string DeviceFirmware(IPortableDevice* device) {
    ComPtr<IPortableDeviceContent> content;
    ComPtr<IPortableDeviceProperties> properties;
    ComPtr<IPortableDeviceKeyCollection> keys;
    ComPtr<IPortableDeviceValues> values;
    if (device == nullptr || FAILED(device->Content(&content)) ||
        FAILED(content->Properties(&properties)) ||
        FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&keys))) ||
        FAILED(keys->Add(WPD_DEVICE_FIRMWARE_VERSION)) ||
        FAILED(properties->GetValues(WPD_DEVICE_OBJECT_ID, keys.Get(), &values))) {
        return "unknown";
    }
    PWSTR firmware = nullptr;
    if (FAILED(values->GetStringValue(WPD_DEVICE_FIRMWARE_VERSION, &firmware)) || firmware == nullptr) {
        if (firmware != nullptr) CoTaskMemFree(firmware);
        return "unknown";
    }
    const std::string result = WideToUtf8(firmware);
    CoTaskMemFree(firmware);
    return result.empty() ? "unknown" : result;
}

std::string DeviceStableIdentity(IPortableDevice* device) {
    ComPtr<IPortableDeviceContent> content;
    ComPtr<IPortableDeviceProperties> properties;
    auto keys = NewKeys("inventory_failed");
    if (device == nullptr || FAILED(device->Content(&content)) ||
        FAILED(content->Properties(&properties)) ||
        FAILED(keys->Add(WPD_DEVICE_SERIAL_NUMBER))) {
        throw TransportError("inventory_failed", "WPD device serial property is unavailable");
    }
    ComPtr<IPortableDeviceValues> values;
    Check(properties->GetValues(WPD_DEVICE_OBJECT_ID, keys.Get(), &values),
        "inventory_failed", "read WPD device identity properties");
    PWSTR serial = nullptr;
    if (!values || FAILED(values->GetStringValue(WPD_DEVICE_SERIAL_NUMBER, &serial)) ||
        serial == nullptr || serial[0] == L'\0') {
        if (serial != nullptr) CoTaskMemFree(serial);
        throw TransportError("inventory_failed", "WPD device serial is missing");
    }
    const std::string serial_utf8 = WideToUtf8(serial);
    CoTaskMemFree(serial);
    try {
        return DeriveWpdStableIdentity(serial_utf8);
    } catch (const std::invalid_argument&) {
        throw TransportError("inventory_failed", "WPD device serial is invalid");
    }
}

struct CaptureTargetProbe {
    WpdCaptureTargetDiagnostic diagnostic{"not_probed", "0x00000000", "0x00000000"};
    std::wstring selected_target;
};

CaptureTargetProbe ProbeStillImageCaptureTarget(IPortableDevice* device) {
    CaptureTargetProbe probe;
    ComPtr<IPortableDeviceCapabilities> capabilities;
    const HRESULT capabilities_result = device->Capabilities(&capabilities);
    if (FAILED(capabilities_result)) {
        probe.diagnostic.validation_state = "capabilities_query_failed";
        probe.diagnostic.command_options_hresult = HResultText(capabilities_result);
        return probe;
    }

    ComPtr<IPortableDevicePropVariantCollection> functional_objects;
    const HRESULT functional_result = capabilities->GetFunctionalObjects(
        WPD_FUNCTIONAL_CATEGORY_STILL_IMAGE_CAPTURE, &functional_objects);
    if (FAILED(functional_result)) {
        probe.diagnostic.validation_state = "functional_objects_query_failed";
        probe.diagnostic.command_options_hresult = HResultText(functional_result);
        return probe;
    }
    DWORD functional_count = 0;
    const HRESULT functional_count_result = functional_objects->GetCount(&functional_count);
    if (FAILED(functional_count_result)) {
        probe.diagnostic.validation_state = "functional_objects_count_failed";
        probe.diagnostic.command_options_hresult = HResultText(functional_count_result);
        return probe;
    }
    std::set<std::wstring> functional_ids;
    for (DWORD index = 0; index < functional_count; ++index) {
        PROPVARIANT item{};
        PropVariantInit(&item);
        const HRESULT item_result = functional_objects->GetAt(index, &item);
        if (FAILED(item_result)) {
            PropVariantClear(&item);
            probe.diagnostic.validation_state = "functional_object_read_failed";
            probe.diagnostic.command_options_hresult = HResultText(item_result);
            return probe;
        }
        if (item.vt != VT_LPWSTR || item.pwszVal == nullptr || item.pwszVal[0] == L'\0') {
            PropVariantClear(&item);
            probe.diagnostic.validation_state = "functional_object_malformed";
            return probe;
        }
        functional_ids.emplace(item.pwszVal);
        PropVariantClear(&item);
    }
    probe.diagnostic.functional_object_count = functional_ids.size();
    if (functional_ids.empty()) {
        probe.diagnostic.validation_state = "functional_object_zero";
        return probe;
    }
    if (functional_ids.size() != 1) {
        probe.diagnostic.validation_state = "functional_object_multiple";
        return probe;
    }

    ComPtr<IPortableDeviceValues> command_options;
    const HRESULT command_options_result = capabilities->GetCommandOptions(
        WPD_COMMAND_STILL_IMAGE_CAPTURE_INITIATE, &command_options);
    probe.diagnostic.command_options_hresult = HResultText(command_options_result);
    if (FAILED(command_options_result)) {
        probe.diagnostic.validation_state = "command_options_query_failed";
        return probe;
    }

    DWORD option_count = 0;
    const HRESULT option_count_result = command_options->GetCount(&option_count);
    if (FAILED(option_count_result)) {
        probe.diagnostic.validation_state = "command_options_count_failed";
        probe.diagnostic.option_value_hresult = HResultText(option_count_result);
        return probe;
    }
    bool valid_ids_option_present = false;
    for (DWORD index = 0; index < option_count; ++index) {
        PROPERTYKEY key{};
        PROPVARIANT value{};
        PropVariantInit(&value);
        const HRESULT option_result = command_options->GetAt(index, &key, &value);
        PropVariantClear(&value);
        if (FAILED(option_result)) {
            probe.diagnostic.validation_state = "command_options_read_failed";
            probe.diagnostic.option_value_hresult = HResultText(option_result);
            return probe;
        }
        if (IsEqualPropertyKey(key, WPD_OPTION_VALID_OBJECT_IDS)) {
            valid_ids_option_present = true;
            break;
        }
    }
    probe.diagnostic.valid_object_ids_option_present = valid_ids_option_present;
    if (!valid_ids_option_present) {
        if (functional_ids.size() != 1) {
            probe.diagnostic.validation_state = functional_ids.empty()
                ? "functional_object_zero" : "functional_object_multiple";
            return probe;
        }
        probe.selected_target = *functional_ids.begin();
        probe.diagnostic.compatible_target_count = 1;
        probe.diagnostic.selected_target = true;
        probe.diagnostic.validation_state = "selected_without_valid_object_ids_option";
        return probe;
    }

    ComPtr<IPortableDevicePropVariantCollection> valid_ids;
    const HRESULT valid_ids_result = command_options->GetIPortableDevicePropVariantCollectionValue(
        WPD_OPTION_VALID_OBJECT_IDS, &valid_ids);
    probe.diagnostic.option_value_hresult = HResultText(valid_ids_result);
    if (FAILED(valid_ids_result) || !valid_ids) {
        probe.diagnostic.validation_state = "valid_object_ids_query_failed";
        return probe;
    }
    DWORD valid_count = 0;
    const HRESULT valid_count_result = valid_ids->GetCount(&valid_count);
    if (FAILED(valid_count_result)) {
        probe.diagnostic.validation_state = "valid_object_ids_count_failed";
        probe.diagnostic.option_value_hresult = HResultText(valid_count_result);
        return probe;
    }
    std::set<std::wstring> option_ids;
    for (DWORD index = 0; index < valid_count; ++index) {
        PROPVARIANT item{};
        PropVariantInit(&item);
        const HRESULT item_result = valid_ids->GetAt(index, &item);
        if (FAILED(item_result)) {
            PropVariantClear(&item);
            probe.diagnostic.validation_state = "valid_object_id_read_failed";
            probe.diagnostic.option_value_hresult = HResultText(item_result);
            return probe;
        }
        if (item.vt != VT_LPWSTR || item.pwszVal == nullptr || item.pwszVal[0] == L'\0') {
            PropVariantClear(&item);
            probe.diagnostic.validation_state = "valid_object_id_malformed";
            return probe;
        }
        option_ids.emplace(item.pwszVal);
        PropVariantClear(&item);
    }
    probe.diagnostic.valid_object_id_count = option_ids.size();
    if (option_ids.empty()) {
        probe.diagnostic.validation_state = "valid_object_id_zero";
        return probe;
    }
    std::set<std::wstring> compatible_ids;
    std::set_intersection(functional_ids.begin(), functional_ids.end(), option_ids.begin(), option_ids.end(),
        std::inserter(compatible_ids, compatible_ids.end()));
    probe.diagnostic.compatible_target_count = compatible_ids.size();
    if (compatible_ids.empty()) {
        probe.diagnostic.validation_state = "compatible_target_zero";
        return probe;
    }
    if (compatible_ids.size() != 1) {
        probe.diagnostic.validation_state = "compatible_target_multiple";
        return probe;
    }
    probe.selected_target = *compatible_ids.begin();
    probe.diagnostic.selected_target = true;
    probe.diagnostic.validation_state = "selected_from_valid_object_ids_option";
    return probe;
}

std::wstring DeviceFriendlyName(IPortableDeviceManager* manager, const wchar_t* id) {
    DWORD characters = 0;
    manager->GetDeviceFriendlyName(id, nullptr, &characters);
    if (characters == 0) return {};
    std::vector<wchar_t> buffer(characters);
    if (FAILED(manager->GetDeviceFriendlyName(id, buffer.data(), &characters))) return {};
    return std::wstring(buffer.data());
}

bool ContainsD810(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return std::towupper(ch); });
    return value.find(L"D810") != std::wstring::npos;
}

struct DeviceRecord {
    std::wstring pnp_id;
    std::string stable_identity;
    std::string friendly_name;
};

struct ObjectRecord {
    std::wstring object_id;
    std::string name;
    std::optional<double> created_on_device;
};

} // namespace

bool WpdContentTypeCountsAsSpoolPayload(const GUID& content_type) noexcept {
    return !IsEqualGUID(content_type, WPD_CONTENT_TYPE_FOLDER) &&
        !IsEqualGUID(content_type, WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);
}

WpdVendorOpcodeCollectionSummary SummarizeWpdVendorOpcodes(
    const std::vector<std::optional<std::uint32_t>>& opcodes) {
    WpdVendorOpcodeCollectionSummary summary;
    summary.item_count = opcodes.size();
    std::set<std::uint32_t> unique;
    for (const auto opcode : opcodes) {
        if (!opcode) {
            summary.malformed = true;
            continue;
        }
        unique.insert(*opcode);
    }
    summary.unique_count = unique.size();
    if (summary.malformed) return summary;
    summary.available = true;
    summary.vendor_capture_9207_advertised = unique.contains(0x9207U);
    return summary;
}

bool WpdObjectDateIsAttributable(
    std::optional<double> object_created,
    std::optional<double> baseline_device_time) noexcept {
    return object_created && baseline_device_time &&
        std::isfinite(*object_created) && std::isfinite(*baseline_device_time) &&
        *object_created >= *baseline_device_time;
}

bool WpdDeviceClockAdvanced(
    std::optional<double> initial_device_time,
    std::optional<double> cutoff_device_time) noexcept {
    return initial_device_time && cutoff_device_time &&
        std::isfinite(*initial_device_time) && std::isfinite(*cutoff_device_time) &&
        *cutoff_device_time > *initial_device_time;
}

void ValidateWpdStreamReadLength(
    std::size_t reported_bytes,
    std::size_t requested_bytes) {
    if (reported_bytes > requested_bytes) {
        throw TransportError(
            "download_failed",
            "WPD JPEG stream returned more bytes than requested");
    }
}

class WpdTransport::Impl {
public:
    explicit Impl(WpdCommandTargetPolicy command_target_policy, WpdTransport::BeforeCommandCallback before_command)
        : command_target_policy_(command_target_policy), before_command_(std::move(before_command)) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
            Check(initialized, "wpd_initialize_failed", "initialize COM");
        }
        owns_com_ = SUCCEEDED(initialized);
    }

    ~Impl() {
        CloseNoThrow();
        if (owns_com_) CoUninitialize();
    }

    std::string SdkVersion() const {
        return "windows-wpd-1;access=read-write;qos=impersonation;command-target=" +
            CommandTargetPolicyName(command_target_policy_);
    }

    std::vector<CameraInfo> Enumerate() {
        devices_.clear();
        ComPtr<IPortableDeviceManager> manager;
        Check(CoCreateInstance(CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&manager)), "inventory_failed", "create WPD manager");
        Check(manager->RefreshDeviceList(), "inventory_failed", "refresh WPD devices");
        DWORD count = 0;
        Check(manager->GetDevices(nullptr, &count), "inventory_failed", "count WPD devices");
        std::vector<PWSTR> ids(count, nullptr);
        if (count != 0) Check(manager->GetDevices(ids.data(), &count), "inventory_failed", "enumerate WPD devices");

        std::vector<CameraInfo> cameras;
        for (DWORD index = 0; index < count; ++index) {
            std::wstring id = ids[index] == nullptr ? L"" : ids[index];
            if (ids[index] != nullptr) CoTaskMemFree(ids[index]);
            const std::wstring friendly = DeviceFriendlyName(manager.Get(), id.c_str());
            if (!ContainsD810(friendly)) continue;
            // Inventory only reads capabilities, firmware, and the standard
            // device serial property. Capture opens a separate read/write
            // session later, after target validation.
            auto device = OpenDevice(id, "inventory_failed", GENERIC_READ);
            std::string firmware;
            std::string identity;
            try {
                if (!GetSupportedWpdCommands(device.Get()).still_image_capture) {
                    device->Close();
                    continue;
                }
                firmware = DeviceFirmware(device.Get());
                identity = DeviceStableIdentity(device.Get());
                device->Close();
            } catch (...) {
                device->Close();
                throw;
            }
            DeviceRecord record{id, identity, WideToUtf8(friendly)};
            if (!devices_.emplace(record.stable_identity, record).second) {
                throw TransportError("identity_collision", "multiple D810 devices reported the same WPD serial identity");
            }
            cameras.push_back({"Nikon D810", firmware, "S", record.stable_identity});
        }
        return cameras;
    }

    void RequireExactlyOneD810ForProductAgent() noexcept {
        require_exactly_one_d810_ = true;
    }

    void Open(std::string_view stable_identity) {
        if (device_) throw TransportError("session_busy", "WPD session is already open");
        if (require_exactly_one_d810_) {
            if (Enumerate().size() != 1) {
                throw TransportError(
                    "camera_count_mismatch",
                    "product SingleCamera WPD open requires exactly one currently connected D810");
            }
        } else if (devices_.empty()) {
            Enumerate();
        }
        const auto found = devices_.find(std::string(stable_identity));
        if (found == devices_.end()) throw TransportError("open_failed", "requested WPD D810 is unavailable");
        device_ = OpenDevice(found->second.pnp_id, "open_failed");
        try {
            const auto target_probe = ProbeStillImageCaptureTarget(device_.Get());
            if (!target_probe.diagnostic.selected_target) {
                throw TransportError("capture_target_validation_failed",
                    "WPD capture target validation failed; state=" + target_probe.diagnostic.validation_state +
                    "; command-options HRESULT=" + target_probe.diagnostic.command_options_hresult +
                    "; option-value HRESULT=" + target_probe.diagnostic.option_value_hresult +
                    "; functional-count=" + std::to_string(target_probe.diagnostic.functional_object_count) +
                    "; valid-object-id-count=" + std::to_string(target_probe.diagnostic.valid_object_id_count) +
                    "; compatible-count=" + std::to_string(target_probe.diagnostic.compatible_target_count));
            }
            capture_target_id_ = target_probe.selected_target;
            Check(device_->Content(&content_), "open_failed", "open WPD content");
            Check(content_->Properties(&properties_), "open_failed", "open WPD properties");
            Check(content_->Transfer(&resources_), "open_failed", "open WPD resources");
            active_identity_ = std::string(stable_identity);
        } catch (...) {
            CloseNoThrow();
            throw;
        }
    }

    void OpenReadOnlyObservation(std::string_view stable_identity) {
        if (device_) throw TransportError("session_busy", "WPD session is already open");
        if (devices_.empty()) Enumerate();
        const auto found = devices_.find(std::string(stable_identity));
        if (found == devices_.end()) throw TransportError("open_failed", "requested WPD D810 is unavailable");
        device_ = OpenDevice(found->second.pnp_id, "correlation_open_failed", GENERIC_READ);
        try {
            Check(device_->Content(&content_), "correlation_open_failed", "open WPD content");
            Check(content_->Properties(&properties_), "correlation_open_failed", "open WPD properties");
            active_identity_ = std::string(stable_identity);
        } catch (...) {
            CloseNoThrow();
            throw;
        }
    }

    std::size_t InspectSpoolPayloadCount(std::string_view stable_identity) {
        if (device_) throw TransportError("session_busy", "WPD session is already open");
        if (require_exactly_one_d810_) {
            if (Enumerate().size() != 1) {
                throw TransportError(
                    "camera_count_mismatch",
                    "product SingleCamera spool inspection requires exactly one currently connected D810");
            }
        } else if (devices_.empty()) {
            Enumerate();
        }
        const auto found = devices_.find(std::string(stable_identity));
        if (found == devices_.end()) {
            throw TransportError("spool_status_open_failed", "requested WPD D810 is unavailable");
        }
        device_ = OpenDevice(found->second.pnp_id, "spool_status_open_failed", GENERIC_READ);
        try {
            Check(device_->Content(&content_), "spool_status_open_failed", "open WPD content");
            Check(content_->Properties(&properties_), "spool_status_open_failed", "open WPD properties");
            active_identity_ = std::string(stable_identity);
            const auto payload_count = SpoolObjects("spool_status_read_failed").size();
            Close();
            return payload_count;
        } catch (...) {
            CloseNoThrow();
            throw;
        }
    }

    WpdCorrelationSample ReadCorrelationSample() {
        if (!device_ || !content_ || !properties_) {
            throw TransportError("session_not_open", "WPD read-only observation session is not open");
        }
        const auto device_time = DeviceDateTime();
        const auto images = JpegObjects("correlation_read_failed");
        WpdCorrelationSample sample;
        sample.device_datetime_available = device_time.has_value();
        if (device_time && correlation_previous_device_time_) {
            sample.reopen_datetime_advanced = *device_time > *correlation_previous_device_time_;
            sample.reopen_datetime_equal = *device_time == *correlation_previous_device_time_;
            sample.reopen_datetime_regressed = *device_time < *correlation_previous_device_time_;
        }
        correlation_previous_device_time_ = device_time;
        sample.jpeg_count = images.size();
        std::optional<double> latest;
        for (const auto& image : images) {
            if (!image.created_on_device) continue;
            ++sample.dated_jpeg_count;
            if (!latest || *image.created_on_device > *latest) latest = image.created_on_device;
        }
        sample.latest_jpeg_date_available = latest.has_value();
        if (latest && device_time) {
            if (*latest < *device_time) ++sample.latest_date_less_than_device;
            else if (*latest == *device_time) ++sample.latest_date_equal_device;
            else ++sample.latest_date_greater_than_device;
        }
        return sample;
    }

    WpdCaptureTargetDiagnostic ProbeCaptureTarget(std::string_view stable_identity) {
        if (device_) throw TransportError("session_busy", "WPD session is already open");
        if (devices_.empty()) Enumerate();
        const auto found = devices_.find(std::string(stable_identity));
        if (found == devices_.end()) throw TransportError("open_failed", "requested WPD D810 is unavailable");
        auto device = OpenDevice(found->second.pnp_id, "wpd_status_open_failed", GENERIC_READ);
        CaptureTargetProbe probe;
        try {
            probe = ProbeStillImageCaptureTarget(device.Get());
        } catch (...) {
            device->Close();
            throw;
        }
        const HRESULT close_result = device->Close();
        Check(close_result, "wpd_status_close_failed", "close WPD device");
        return probe.diagnostic;
    }

    WpdVendorOpcodeDiagnostic ProbeVendorOpcodes(
        std::string_view stable_identity,
        WpdStatusAccess access) {
        if (device_) throw TransportError("session_busy", "WPD session is already open");
        if (devices_.empty()) Enumerate();
        const auto found = devices_.find(std::string(stable_identity));
        if (found == devices_.end()) throw TransportError("open_failed", "requested WPD D810 is unavailable");

        const DWORD desired_access = access == WpdStatusAccess::read_only
            ? GENERIC_READ
            : (GENERIC_READ | GENERIC_WRITE);
        auto device = OpenDevice(found->second.pnp_id, "wpd_status_open_failed", desired_access);
        WpdVendorOpcodeDiagnostic diagnostic;
        diagnostic.requested_access = StatusAccessName(access);
        diagnostic.read_only_access = access == WpdStatusAccess::read_only;
        try {
            const auto supported = GetSupportedWpdCommands(device.Get());
            diagnostic.supported_commands_hresult = HResultText(supported.hresult);
            if (supported.hresult != S_OK) {
                diagnostic.validation_state = "supported_commands_query_failed";
            } else {
                diagnostic.wpd_still_image_capture_command_advertised = supported.still_image_capture;
                diagnostic.vendor_opcode_query_advertised = supported.vendor_opcode_query;
                if (!supported.vendor_opcode_query) {
                    diagnostic.validation_state = "vendor_opcode_query_not_advertised";
                } else {
                    auto command = NewValues("wpd_vendor_opcode_query_failed");
                    Check(command->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,
                        WPD_COMMAND_MTP_EXT_GET_SUPPORTED_VENDOR_OPCODES.fmtid),
                        "wpd_vendor_opcode_query_failed", "set vendor opcode query category");
                    Check(command->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,
                        WPD_COMMAND_MTP_EXT_GET_SUPPORTED_VENDOR_OPCODES.pid),
                        "wpd_vendor_opcode_query_failed", "set vendor opcode query command");

                    // This is the sole SendCommand in this read-only diagnostic.
                    // No vendor operation command or capture command is constructed or executed.
                    diagnostic.read_only_command_sent = true;
                    ComPtr<IPortableDeviceValues> result;
                    const HRESULT send_result = device->SendCommand(0, command.Get(), &result);
                    diagnostic.query_send_hresult = HResultText(send_result);
                    if (send_result != S_OK || !result) {
                        diagnostic.validation_state = send_result != S_OK
                            ? "vendor_opcode_query_send_failed" : "vendor_opcode_query_result_unavailable";
                    } else {
                        HRESULT common_result = E_FAIL;
                        const HRESULT common_read_result = result->GetErrorValue(
                            WPD_PROPERTY_COMMON_HRESULT, &common_result);
                        diagnostic.query_common_hresult = common_read_result != S_OK
                            ? HResultText(common_read_result) : HResultText(common_result);
                        if (common_read_result != S_OK) {
                            diagnostic.validation_state = "vendor_opcode_query_common_hresult_unavailable";
                        } else if (common_result != S_OK) {
                            diagnostic.validation_state = "vendor_opcode_query_common_failed";
                        } else {
                            ComPtr<IPortableDevicePropVariantCollection> values;
                            const HRESULT collection_result = result->GetIPortableDevicePropVariantCollectionValue(
                                WPD_PROPERTY_MTP_EXT_VENDOR_OPERATION_CODES, &values);
                            if (collection_result != S_OK || !values) {
                                diagnostic.validation_state = "vendor_opcode_collection_unavailable";
                            } else {
                                DWORD count = 0;
                                const HRESULT count_result = values->GetCount(&count);
                                if (count_result != S_OK) {
                                    diagnostic.validation_state = "vendor_opcode_collection_count_failed";
                                } else {
                                    std::vector<std::optional<std::uint32_t>> opcodes;
                                    opcodes.reserve(count);
                                    for (DWORD index = 0; index < count; ++index) {
                                        PROPVARIANT value{};
                                        PropVariantInit(&value);
                                        const HRESULT item_result = values->GetAt(index, &value);
                                        if (item_result != S_OK || value.vt != VT_UI4) {
                                            opcodes.emplace_back(std::nullopt);
                                        } else {
                                            opcodes.emplace_back(value.ulVal);
                                        }
                                        PropVariantClear(&value);
                                    }
                                    const auto summary = SummarizeWpdVendorOpcodes(opcodes);
                                    diagnostic.vendor_opcode_item_count = summary.item_count;
                                    diagnostic.vendor_opcode_unique_count = summary.unique_count;
                                    diagnostic.vendor_opcode_collection_available = summary.available;
                                    diagnostic.vendor_capture_9207_advertised = summary.vendor_capture_9207_advertised;
                                    diagnostic.validation_state = summary.malformed
                                        ? "vendor_opcode_collection_malformed" : "vendor_opcode_query_complete";
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {
            device->Close();
            throw;
        }
        const HRESULT close_result = device->Close();
        Check(close_result, "wpd_status_close_failed", "close WPD device");
        return diagnostic;
    }

    std::string Baseline(std::chrono::seconds) {
        RequireOpen();
        baseline_.clear();
        for (const auto& object : JpegObjects("baseline_failed")) baseline_.insert(object.object_id);
        baseline_token_ = "wpd-baseline-" + std::to_string(++baseline_sequence_);
        observation_not_before_.reset();
        observation_identity_.clear();
        return baseline_token_;
    }

    std::string BeginPostCardObservation(std::chrono::seconds timeout) {
        RequireOpen();
        baseline_.clear();
        baseline_token_.clear();
        observation_not_before_.reset();
        observation_identity_.clear();
        cleanup_handles_.clear();
        try {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::size_t previous_count = static_cast<std::size_t>(-1);
            std::optional<std::chrono::steady_clock::time_point> stable_since;
            std::vector<ObjectRecord> snapshot;
            while (std::chrono::steady_clock::now() < deadline) {
                snapshot = SpoolObjects("spool_preflight_failed");
                if (snapshot.size() != previous_count) {
                    previous_count = snapshot.size();
                    stable_since = std::chrono::steady_clock::now();
                }
                if (stable_since && std::chrono::steady_clock::now() - *stable_since >= kCandidateSettle) break;
                std::this_thread::sleep_for(kPollInterval);
            }
            if (!stable_since || std::chrono::steady_clock::now() - *stable_since < kCandidateSettle) {
                throw TransportError("spool_preflight_timeout", "WPD spool did not stabilize before SDK capture");
            }
            if (!snapshot.empty()) {
                throw TransportError("spool_not_empty",
                    "dedicated WPD spool contains camera payload objects; object-count=" +
                    std::to_string(snapshot.size()));
            }
            observation_identity_ = active_identity_;
            baseline_token_ = "wpd-card-observation-" + std::to_string(++baseline_sequence_);
            return baseline_token_;
        } catch (...) {
            baseline_.clear();
            baseline_token_.clear();
            observation_not_before_.reset();
            observation_identity_.clear();
            throw;
        }
    }

    std::vector<ImageCandidate> ObserveAndDownloadPostCardCapture(
        std::string_view token,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) {
        RequireOpen();
        if (token.empty() || token != baseline_token_) {
            throw TransportError("baseline_mismatch", "WPD post-card observation token is not current");
        }
        if (active_identity_ != observation_identity_) {
            baseline_token_.clear();
            baseline_.clear();
            observation_not_before_.reset();
            observation_identity_.clear();
            throw TransportError("identity_mismatch", "WPD post-card observation reopened a different camera");
        }
        struct ConsumeToken final {
            std::string& token;
            std::optional<double>& not_before;
            std::string& identity;
            ~ConsumeToken() {
                token.clear();
                not_before.reset();
                identity.clear();
            }
        } consume{baseline_token_, observation_not_before_, observation_identity_};
        const auto deadline = std::chrono::steady_clock::now() + transaction_timeout;
        return ObservePostBaseline(image_event_timeout, download_timeout, deadline, false, true);
    }

    void DeleteRecoveredObject(std::string_view cleanup_token, std::chrono::seconds) {
        RequireOpen();
        const auto handle = cleanup_handles_.find(std::string(cleanup_token));
        if (cleanup_token.empty() || handle == cleanup_handles_.end()) {
            throw TransportError("cleanup_token_invalid", "exact-object cleanup capability is absent or expired");
        }
        const std::wstring object_id = handle->second;
        cleanup_handles_.erase(handle);

        const auto current = SpoolObjects("spool_changed_before_delete");
        if (current.size() != 1 || current.front().object_id != object_id) {
            throw TransportError("spool_changed_before_delete",
                "dedicated spool changed after recovery; delete was not attempted; object-count=" +
                std::to_string(current.size()));
        }

        auto object_ids = NewPropVariantCollection("exact_object_delete_failed");
        PROPVARIANT value{};
        PropVariantInit(&value);
        value.vt = VT_LPWSTR;
        const std::size_t bytes = (object_id.size() + 1) * sizeof(wchar_t);
        value.pwszVal = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
        if (value.pwszVal == nullptr) {
            throw TransportError("exact_object_delete_failed", "allocate exact WPD object ID failed");
        }
        std::memcpy(value.pwszVal, object_id.c_str(), bytes);
        const HRESULT add_result = object_ids->Add(&value);
        PropVariantClear(&value);
        Check(add_result, "exact_object_delete_failed", "add exact WPD object ID");

        ComPtr<IPortableDevicePropVariantCollection> results;
        const HRESULT delete_result = content_->Delete(
            PORTABLE_DEVICE_DELETE_NO_RECURSION, object_ids.Get(), &results);
        if (delete_result != S_OK) {
            throw TransportError("exact_object_delete_failed",
                "delete exact WPD object failed: " + HResultText(delete_result));
        }
        if (!results) {
            throw TransportError("exact_object_delete_result_invalid", "WPD delete returned no per-object result");
        }
        DWORD result_count = 0;
        Check(results->GetCount(&result_count), "exact_object_delete_result_invalid", "count WPD delete results");
        if (result_count != 1) {
            throw TransportError("exact_object_delete_result_invalid",
                "WPD delete result count was not exactly one");
        }
        PROPVARIANT item{};
        PropVariantInit(&item);
        const HRESULT item_result = results->GetAt(0, &item);
        const bool succeeded = item_result == S_OK && item.vt == VT_ERROR && item.scode == S_OK;
        const HRESULT object_result = item.vt == VT_ERROR ? item.scode : E_UNEXPECTED;
        PropVariantClear(&item);
        if (!succeeded) {
            throw TransportError("exact_object_delete_result_failed",
                "WPD exact-object delete result failed: " +
                HResultText(item_result == S_OK ? object_result : item_result));
        }
    }

    void VerifyJpegSpoolEmpty(std::chrono::seconds timeout) {
        RequireOpen();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::size_t last_count = static_cast<std::size_t>(-1);
        std::optional<std::chrono::steady_clock::time_point> stable_since;
        std::vector<ObjectRecord> objects;
        while (std::chrono::steady_clock::now() < deadline) {
            objects = SpoolObjects("spool_verify_failed");
            if (objects.size() != last_count) {
                last_count = objects.size();
                stable_since = std::chrono::steady_clock::now();
            }
            if (stable_since && std::chrono::steady_clock::now() - *stable_since >= kCandidateSettle) break;
            std::this_thread::sleep_for(kPollInterval);
        }
        if (!stable_since || std::chrono::steady_clock::now() - *stable_since < kCandidateSettle) {
            throw TransportError("spool_verify_timeout", "WPD spool did not stabilize after cleanup");
        }
        if (!objects.empty()) {
            throw TransportError("spool_not_empty_after_cleanup",
                "dedicated WPD spool contains payload after exact-object cleanup; object-count=" +
                std::to_string(objects.size()));
        }
        cleanup_handles_.clear();
    }

    void AbandonPostCardObservation(std::string_view token) noexcept {
        if (token == baseline_token_) {
            baseline_token_.clear();
            baseline_.clear();
            observation_not_before_.reset();
            observation_identity_.clear();
        }
    }

    std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) {
        RequireOpen();
        if (baseline.empty() || baseline != baseline_token_) {
            throw TransportError("baseline_mismatch", "WPD capture baseline token is not current");
        }
        const auto overall_deadline = std::chrono::steady_clock::now() + transaction_timeout;
        auto command = NewValues("capture_command_failed");
        Check(command->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,
            WPD_COMMAND_STILL_IMAGE_CAPTURE_INITIATE.fmtid), "capture_command_failed", "set WPD capture category");
        Check(command->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,
            WPD_COMMAND_STILL_IMAGE_CAPTURE_INITIATE.pid), "capture_command_failed", "set WPD capture command");
        if (command_target_policy_ == WpdCommandTargetPolicy::functional) {
            const HRESULT set_target = command->SetStringValue(
                WPD_PROPERTY_COMMON_COMMAND_TARGET, capture_target_id_.c_str());
            if (FAILED(set_target)) {
                throw TransportError("capture_command_failed",
                    "set WPD capture command target failed: " + HResultText(set_target) +
                    "; command target policy=" + CommandTargetPolicyName(command_target_policy_));
            }
        }
        if (before_command_) before_command_();
        ComPtr<IPortableDeviceValues> result;
        const HRESULT send_result = device_->SendCommand(0, command.Get(), &result);
        if (FAILED(send_result)) {
            throw TransportError("capture_command_failed",
                "WPD capture command SendCommand HRESULT=" + HResultText(send_result) +
                "; command target policy=" + CommandTargetPolicyName(command_target_policy_));
        }
        const auto command_prefix = "WPD capture command uncertain after SendCommand HRESULT=" + HResultText(send_result) +
            "; command target policy=" + CommandTargetPolicyName(command_target_policy_);
        const auto observe_uncertain = [&](std::string detail) -> void {
            try {
                const auto candidates = ObservePostBaseline(
                    image_event_timeout, download_timeout, overall_deadline, true, false);
                const auto category = candidates.empty() ? "uncertain_dispatch_no_object" :
                    (candidates.size() > 1 ? "uncertain_dispatch_ambiguous" : "uncertain_dispatch");
                throw UncertainDispatchError(category, std::move(detail), candidates);
            } catch (const UncertainDispatchError&) {
                throw;
            } catch (const TransportError& error) {
                throw UncertainDispatchError("uncertain_dispatch_observation_failed",
                    detail + "; post-baseline observation failed: " + error.what(), {});
            }
        };
        if (!result) {
            const auto detail = command_prefix + "; command result is unavailable";
            if (send_result == S_OK) observe_uncertain(detail);
            throw TransportError("capture_command_failed", detail);
        }
        HRESULT command_result = S_OK;
        const HRESULT read_command_result =
            result->GetErrorValue(WPD_PROPERTY_COMMON_HRESULT, &command_result);
        DWORD driver_error = 0;
        const bool has_driver_error = SUCCEEDED(
            result->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_DRIVER_ERROR_CODE, &driver_error));
        if (FAILED(read_command_result)) {
            std::string detail = command_prefix + "; result-read HRESULT=" + HResultText(read_command_result);
            if (has_driver_error) detail += "; driver error=" + DriverErrorText(driver_error);
            if (send_result == S_OK) observe_uncertain(detail);
            throw TransportError("capture_command_failed", std::move(detail));
        }
        if (FAILED(command_result)) {
            std::string detail = command_prefix + "; command HRESULT=" + HResultText(command_result);
            if (has_driver_error) detail += "; driver error=" + DriverErrorText(driver_error);
            if (send_result == S_OK) observe_uncertain(detail);
            throw TransportError("capture_command_failed", std::move(detail));
        }

        return ObservePostBaseline(image_event_timeout, download_timeout, overall_deadline, false, false);
    }

    std::vector<ImageCandidate> ObservePostBaseline(
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::steady_clock::time_point overall_deadline,
        bool allow_empty,
        bool issue_cleanup_token) {
        const auto event_deadline = std::min(std::chrono::steady_clock::now() + image_event_timeout, overall_deadline);
        std::vector<ObjectRecord> candidates;
        std::optional<std::chrono::steady_clock::time_point> stable_since;
        std::size_t previous_count = 0;
        while (std::chrono::steady_clock::now() < event_deadline) {
            candidates.clear();
            for (const auto& object : JpegObjects("image_event_failed")) {
                if (!baseline_.contains(object.object_id)) candidates.push_back(object);
            }
            if (candidates.size() != previous_count) {
                previous_count = candidates.size();
                stable_since = candidates.empty() ? std::nullopt : std::optional{std::chrono::steady_clock::now()};
            }
            if (stable_since && std::chrono::steady_clock::now() - *stable_since >= kCandidateSettle) break;
            std::this_thread::sleep_for(kPollInterval);
        }
        if (candidates.empty()) {
            if (allow_empty) return {};
            throw TransportError("image_event_timeout", "WPD did not publish a new JPEG object");
        }

        const auto download_deadline = std::min(std::chrono::steady_clock::now() + download_timeout, overall_deadline);
        std::vector<ImageCandidate> images;
        images.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            std::string cleanup_token;
            if (issue_cleanup_token && candidates.size() == 1) {
                cleanup_token = "cleanup-capability-" + std::to_string(++cleanup_token_sequence_);
                cleanup_handles_.emplace(cleanup_token, candidate.object_id);
            }
            images.push_back({candidate.name, Download(candidate.object_id, download_deadline), true,
                std::move(cleanup_token)});
        }
        baseline_token_.clear();
        return images;
    }

    void Close() {
        if (!device_) {
            capture_target_id_.clear();
            cleanup_handles_.clear();
            return;
        }
        const HRESULT result = device_->Close();
        resources_.Reset();
        properties_.Reset();
        content_.Reset();
        device_.Reset();
        // A post-card observation token intentionally survives the close/reopen
        // handoff.  It is consumed by observation, including error paths.
        if (baseline_token_.empty()) baseline_.clear();
        cleanup_handles_.clear();
        capture_target_id_.clear();
        Check(result, "close_failed", "close WPD device");
    }

private:
    void RequireOpen() const {
        if (!device_ || !content_ || !properties_ || !resources_ || capture_target_id_.empty()) {
            throw TransportError("session_not_open", "WPD D810 session is not open");
        }
    }

    std::vector<ObjectRecord> ContentObjects(std::string_view category, bool jpeg_only) {
        std::vector<ObjectRecord> objects;
        std::vector<std::wstring> pending{WPD_DEVICE_OBJECT_ID};
        std::set<std::wstring> visited;
        auto keys = NewKeys(category);
        Check(keys->Add(WPD_OBJECT_CONTENT_TYPE), category, "add WPD content type key");
        Check(keys->Add(WPD_OBJECT_ORIGINAL_FILE_NAME), category, "add WPD file name key");
        Check(keys->Add(WPD_OBJECT_NAME), category, "add WPD name key");
        auto date_keys = NewKeys(category);
        Check(date_keys->Add(WPD_OBJECT_DATE_CREATED), category, "add WPD object creation date key");

        while (!pending.empty()) {
            const std::wstring parent = std::move(pending.back());
            pending.pop_back();
            if (!visited.insert(parent).second) continue;
            ComPtr<IEnumPortableDeviceObjectIDs> enumerator;
            Check(content_->EnumObjects(0, parent.c_str(), nullptr, &enumerator), category, "enumerate WPD objects");
            for (;;) {
                std::array<PWSTR, 16> ids{};
                DWORD fetched = 0;
                const HRESULT next = enumerator->Next(static_cast<ULONG>(ids.size()), ids.data(), &fetched);
                if (FAILED(next)) Check(next, category, "read WPD object IDs");
                for (DWORD index = 0; index < fetched; ++index) {
                    std::wstring id(ids[index]);
                    CoTaskMemFree(ids[index]);
                    pending.push_back(id);
                    ComPtr<IPortableDeviceValues> values;
                    Check(properties_->GetValues(id.c_str(), keys.Get(), &values),
                        category, "read WPD object properties");
                    if (!values) {
                        throw TransportError(std::string(category), "WPD object properties were unavailable");
                    }
                    GUID content_type{};
                    Check(values->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &content_type),
                        category, "read WPD object content type");
                    if (!WpdContentTypeCountsAsSpoolPayload(content_type)) continue;
                    if (jpeg_only && !IsEqualGUID(content_type, WPD_CONTENT_TYPE_IMAGE)) continue;
                    PWSTR name_value = nullptr;
                    if (FAILED(values->GetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, &name_value))) {
                        values->GetStringValue(WPD_OBJECT_NAME, &name_value);
                    }
                    const std::wstring name = name_value == nullptr
                        ? (jpeg_only ? L"captured.jpg" : L"camera-object")
                        : std::wstring(name_value);
                    if (name_value != nullptr) CoTaskMemFree(name_value);
                    std::wstring extension = name;
                    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) { return std::towlower(ch); });
                    if (jpeg_only && !extension.ends_with(L".jpg") && !extension.ends_with(L".jpeg")) continue;
                    std::optional<double> created_on_device;
                    ComPtr<IPortableDeviceValues> date_values;
                    if (properties_->GetValues(id.c_str(), date_keys.Get(), &date_values) == S_OK && date_values) {
                        PROPVARIANT created{};
                        PropVariantInit(&created);
                        if (date_values->GetValue(WPD_OBJECT_DATE_CREATED, &created) == S_OK &&
                            created.vt == VT_DATE && std::isfinite(created.date)) {
                            created_on_device = created.date;
                        }
                        PropVariantClear(&created);
                    }
                    objects.push_back({std::move(id), WideToUtf8(name), created_on_device});
                }
                if (next == S_FALSE || fetched == 0) break;
            }
        }
        return objects;
    }

    std::vector<ObjectRecord> JpegObjects(std::string_view category) {
        return ContentObjects(category, true);
    }

    std::vector<ObjectRecord> SpoolObjects(std::string_view category) {
        return ContentObjects(category, false);
    }

    std::optional<double> DeviceDateTime() {
        auto keys = NewKeys("capture_correlation_unavailable");
        Check(keys->Add(WPD_DEVICE_DATETIME),
            "capture_correlation_unavailable", "add WPD device datetime key");
        ComPtr<IPortableDeviceValues> values;
        Check(properties_->GetValues(WPD_DEVICE_OBJECT_ID, keys.Get(), &values),
            "capture_correlation_unavailable", "read WPD device datetime");
        PROPVARIANT value{};
        PropVariantInit(&value);
        const HRESULT result = values->GetValue(WPD_DEVICE_DATETIME, &value);
        std::optional<double> date;
        if (result == S_OK && value.vt == VT_DATE && std::isfinite(value.date)) date = value.date;
        PropVariantClear(&value);
        return date;
    }

    std::vector<unsigned char> Download(
        const std::wstring& object_id,
        std::chrono::steady_clock::time_point deadline) {
        DWORD optimal = 0;
        ComPtr<IStream> stream;
        Check(resources_->GetStream(object_id.c_str(), WPD_RESOURCE_DEFAULT, STGM_READ,
            &optimal, &stream), "download_failed", "open WPD JPEG stream");
        const ULONG chunk_size = std::clamp<ULONG>(optimal, 64U * 1024U, 4U * 1024U * 1024U);
        std::vector<unsigned char> output;
        std::vector<unsigned char> buffer(chunk_size);
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw TransportError("download_timeout", "WPD JPEG transfer timed out");
            }
            ULONG read = 0;
            const HRESULT result = stream->Read(buffer.data(), chunk_size, &read);
            if (FAILED(result)) Check(result, "download_failed", "read WPD JPEG stream");
            ValidateWpdStreamReadLength(read, chunk_size);
            if (read != 0) {
                if (read > kMaximumJpegBytes - output.size()) {
                    throw TransportError("invalid_jpeg", "WPD JPEG exceeds the safety size limit");
                }
                output.insert(output.end(), buffer.begin(), buffer.begin() + read);
            }
            if (result == S_FALSE || read == 0) break;
        }
        return output;
    }

    void CloseNoThrow() noexcept {
        if (device_) device_->Close();
        resources_.Reset();
        properties_.Reset();
        content_.Reset();
        device_.Reset();
        baseline_.clear();
        baseline_token_.clear();
        observation_not_before_.reset();
        observation_identity_.clear();
        cleanup_handles_.clear();
        capture_target_id_.clear();
    }

    bool owns_com_{false};
    WpdCommandTargetPolicy command_target_policy_;
    WpdTransport::BeforeCommandCallback before_command_;
    std::map<std::string, DeviceRecord> devices_;
    bool require_exactly_one_d810_{};
    ComPtr<IPortableDevice> device_;
    ComPtr<IPortableDeviceContent> content_;
    ComPtr<IPortableDeviceProperties> properties_;
    ComPtr<IPortableDeviceResources> resources_;
    std::wstring capture_target_id_;
    std::set<std::wstring> baseline_;
    std::string baseline_token_;
    std::string active_identity_;
    std::string observation_identity_;
    std::optional<double> correlation_previous_device_time_;
    std::optional<double> observation_not_before_;
    std::map<std::string, std::wstring> cleanup_handles_;
    unsigned long long baseline_sequence_{0};
    unsigned long long cleanup_token_sequence_{0};
};

WpdTransport::WpdTransport(WpdCommandTargetPolicy command_target_policy, BeforeCommandCallback before_command)
    : impl_(std::make_unique<Impl>(command_target_policy, std::move(before_command))) {}
WpdTransport::~WpdTransport() = default;
std::string WpdTransport::SdkVersion() const {
    return impl_->SdkVersion();
}
std::vector<CameraInfo> WpdTransport::Enumerate() { return impl_->Enumerate(); }
void WpdTransport::RequireExactlyOneD810ForProductAgent() {
    impl_->RequireExactlyOneD810ForProductAgent();
}
WpdCaptureTargetDiagnostic WpdTransport::ProbeCaptureTarget(std::string_view stable_identity) {
    return impl_->ProbeCaptureTarget(stable_identity);
}
WpdVendorOpcodeDiagnostic WpdTransport::ProbeVendorOpcodes(
    std::string_view stable_identity,
    WpdStatusAccess access) {
    return impl_->ProbeVendorOpcodes(stable_identity, access);
}
std::size_t WpdTransport::InspectSpoolPayloadCount(
    std::string_view stable_identity,
    std::chrono::seconds) {
    return impl_->InspectSpoolPayloadCount(stable_identity);
}
void WpdTransport::OpenReadOnlyObservation(std::string_view stable_identity, std::chrono::seconds) {
    impl_->OpenReadOnlyObservation(stable_identity);
}
WpdCorrelationSample WpdTransport::ReadCorrelationSample() { return impl_->ReadCorrelationSample(); }
void WpdTransport::Open(std::string_view stable_identity, std::chrono::seconds) { impl_->Open(stable_identity); }
std::string WpdTransport::Baseline(std::chrono::seconds timeout) { return impl_->Baseline(timeout); }
std::vector<ImageCandidate> WpdTransport::CaptureAndDownload(
    std::string_view baseline,
    std::chrono::seconds image_event_timeout,
    std::chrono::seconds download_timeout,
    std::chrono::seconds transaction_timeout) {
    return impl_->CaptureAndDownload(baseline, image_event_timeout, download_timeout, transaction_timeout);
}
std::string WpdTransport::BeginPostCardObservation(std::chrono::seconds timeout) {
    return impl_->BeginPostCardObservation(timeout);
}
std::vector<ImageCandidate> WpdTransport::ObserveAndDownloadPostCardCapture(
    std::string_view token,
    std::chrono::seconds image_event_timeout,
    std::chrono::seconds download_timeout,
    std::chrono::seconds transaction_timeout) {
    return impl_->ObserveAndDownloadPostCardCapture(token, image_event_timeout, download_timeout, transaction_timeout);
}
void WpdTransport::AbandonPostCardObservation(std::string_view token) noexcept {
    impl_->AbandonPostCardObservation(token);
}
void WpdTransport::DeleteRecoveredObject(std::string_view cleanup_token, std::chrono::seconds timeout) {
    impl_->DeleteRecoveredObject(cleanup_token, timeout);
}
void WpdTransport::VerifyJpegSpoolEmpty(std::chrono::seconds timeout) {
    impl_->VerifyJpegSpoolEmpty(timeout);
}
void WpdTransport::Close(std::chrono::seconds) { impl_->Close(); }

} // namespace a0::phase0
