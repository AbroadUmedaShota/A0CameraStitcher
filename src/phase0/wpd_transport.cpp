#include "a0/phase0/wpd_transport.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <portabledeviceapi.h>
#include <portabledevice.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace a0::phase0 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaximumJpegBytes = 256U * 1024U * 1024U;
constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr auto kCandidateSettle = std::chrono::milliseconds(750);

std::string HResultText(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return text.str();
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

std::string Sha256(std::wstring_view value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD bytes = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size), &bytes, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
            sizeof(hash_size), &bytes, 0) != 0) {
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw TransportError("inventory_failed", "WPD identity hash initialization failed");
    }
    std::vector<unsigned char> object(object_size);
    std::vector<unsigned char> digest(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) != 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(value.data())),
            static_cast<ULONG>(value.size() * sizeof(wchar_t)), 0) != 0 ||
        BCryptFinishHash(hash, digest.data(), hash_size, 0) != 0) {
        if (hash != nullptr) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw TransportError("inventory_failed", "WPD identity hash failed");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
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

ComPtr<IPortableDevice> OpenDevice(std::wstring_view pnp_id, std::string_view category) {
    ComPtr<IPortableDevice> device;
    Check(CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&device)), category, "create WPD device");
    auto client = NewValues(category);
    Check(client->SetStringValue(WPD_CLIENT_NAME, L"A0CameraStitcher Phase0"), category, "set WPD client name");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 0), category, "set WPD client major version");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 1), category, "set WPD client minor version");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0), category, "set WPD client revision");
    const std::wstring id(pnp_id);
    Check(device->Open(id.c_str(), client.Get()), category, "open WPD device");
    return device;
}

bool SupportsCapture(IPortableDevice* device) {
    ComPtr<IPortableDeviceCapabilities> capabilities;
    if (FAILED(device->Capabilities(&capabilities))) return false;
    ComPtr<IPortableDeviceKeyCollection> commands;
    if (FAILED(capabilities->GetSupportedCommands(&commands))) return false;
    DWORD count = 0;
    if (FAILED(commands->GetCount(&count))) return false;
    for (DWORD index = 0; index < count; ++index) {
        PROPERTYKEY command{};
        if (SUCCEEDED(commands->GetAt(index, &command)) &&
            IsEqualPropertyKey(command, WPD_COMMAND_STILL_IMAGE_CAPTURE_INITIATE)) return true;
    }
    return false;
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
};

} // namespace

class WpdTransport::Impl {
public:
    Impl() {
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
            try {
                auto device = OpenDevice(id, "inventory_failed");
                if (!SupportsCapture(device.Get())) {
                    device->Close();
                    continue;
                }
                device->Close();
                DeviceRecord record{id, Sha256(id), WideToUtf8(friendly)};
                devices_.emplace(record.stable_identity, record);
                cameras.push_back({"Nikon D810", "unknown", "S", record.stable_identity});
            } catch (const TransportError&) {
                // A non-capture WPD projection of the same physical camera is ignored.
            }
        }
        return cameras;
    }

    void Open(std::string_view stable_identity) {
        if (device_) throw TransportError("session_busy", "WPD session is already open");
        if (devices_.empty()) Enumerate();
        const auto found = devices_.find(std::string(stable_identity));
        if (found == devices_.end()) throw TransportError("open_failed", "requested WPD D810 is unavailable");
        device_ = OpenDevice(found->second.pnp_id, "open_failed");
        if (!SupportsCapture(device_.Get())) {
            CloseNoThrow();
            throw TransportError("open_failed", "D810 WPD capture command is unavailable");
        }
        Check(device_->Content(&content_), "open_failed", "open WPD content");
        Check(content_->Properties(&properties_), "open_failed", "open WPD properties");
        Check(content_->Transfer(&resources_), "open_failed", "open WPD resources");
    }

    std::string Baseline(std::chrono::seconds) {
        RequireOpen();
        baseline_.clear();
        for (const auto& object : JpegObjects("baseline_failed")) baseline_.insert(object.object_id);
        baseline_token_ = "wpd-baseline-" + std::to_string(++baseline_sequence_);
        return baseline_token_;
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
        ComPtr<IPortableDeviceValues> result;
        Check(device_->SendCommand(0, command.Get(), &result), "capture_command_failed", "send WPD capture command");
        HRESULT command_result = S_OK;
        if (SUCCEEDED(result->GetErrorValue(WPD_PROPERTY_COMMON_HRESULT, &command_result)) && FAILED(command_result)) {
            Check(command_result, "capture_command_failed", "WPD capture command result");
        }

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
        if (candidates.empty()) throw TransportError("image_event_timeout", "WPD did not publish a new JPEG object");

        const auto download_deadline = std::min(std::chrono::steady_clock::now() + download_timeout, overall_deadline);
        std::vector<ImageCandidate> images;
        images.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            images.push_back({candidate.name, Download(candidate.object_id, download_deadline), true});
        }
        baseline_token_.clear();
        return images;
    }

    void Close() {
        if (!device_) return;
        const HRESULT result = device_->Close();
        resources_.Reset();
        properties_.Reset();
        content_.Reset();
        device_.Reset();
        baseline_.clear();
        baseline_token_.clear();
        Check(result, "close_failed", "close WPD device");
    }

private:
    void RequireOpen() const {
        if (!device_ || !content_ || !properties_ || !resources_) {
            throw TransportError("session_not_open", "WPD D810 session is not open");
        }
    }

    std::vector<ObjectRecord> JpegObjects(std::string_view category) {
        std::vector<ObjectRecord> images;
        std::vector<std::wstring> pending{WPD_DEVICE_OBJECT_ID};
        std::set<std::wstring> visited;
        auto keys = NewKeys(category);
        Check(keys->Add(WPD_OBJECT_CONTENT_TYPE), category, "add WPD content type key");
        Check(keys->Add(WPD_OBJECT_ORIGINAL_FILE_NAME), category, "add WPD file name key");
        Check(keys->Add(WPD_OBJECT_NAME), category, "add WPD name key");

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
                    if (FAILED(properties_->GetValues(id.c_str(), keys.Get(), &values))) continue;
                    GUID content_type{};
                    if (FAILED(values->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &content_type)) ||
                        !IsEqualGUID(content_type, WPD_CONTENT_TYPE_IMAGE)) continue;
                    PWSTR name_value = nullptr;
                    if (FAILED(values->GetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, &name_value))) {
                        values->GetStringValue(WPD_OBJECT_NAME, &name_value);
                    }
                    const std::wstring name = name_value == nullptr ? L"captured.jpg" : std::wstring(name_value);
                    if (name_value != nullptr) CoTaskMemFree(name_value);
                    std::wstring extension = name;
                    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) { return std::towlower(ch); });
                    if (!extension.ends_with(L".jpg") && !extension.ends_with(L".jpeg")) continue;
                    images.push_back({std::move(id), WideToUtf8(name)});
                }
                if (next == S_FALSE || fetched == 0) break;
            }
        }
        return images;
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
            if (read != 0) {
                if (output.size() + read > kMaximumJpegBytes) {
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
    }

    bool owns_com_{false};
    std::map<std::string, DeviceRecord> devices_;
    ComPtr<IPortableDevice> device_;
    ComPtr<IPortableDeviceContent> content_;
    ComPtr<IPortableDeviceProperties> properties_;
    ComPtr<IPortableDeviceResources> resources_;
    std::set<std::wstring> baseline_;
    std::string baseline_token_;
    unsigned long long baseline_sequence_{0};
};

WpdTransport::WpdTransport() : impl_(std::make_unique<Impl>()) {}
WpdTransport::~WpdTransport() = default;
std::string WpdTransport::SdkVersion() const { return "windows-wpd-1"; }
std::vector<CameraInfo> WpdTransport::Enumerate() { return impl_->Enumerate(); }
void WpdTransport::Open(std::string_view stable_identity, std::chrono::seconds) { impl_->Open(stable_identity); }
std::string WpdTransport::Baseline(std::chrono::seconds timeout) { return impl_->Baseline(timeout); }
std::vector<ImageCandidate> WpdTransport::CaptureAndDownload(
    std::string_view baseline,
    std::chrono::seconds image_event_timeout,
    std::chrono::seconds download_timeout,
    std::chrono::seconds transaction_timeout) {
    return impl_->CaptureAndDownload(baseline, image_event_timeout, download_timeout, transaction_timeout);
}
void WpdTransport::Close(std::chrono::seconds) { impl_->Close(); }

} // namespace a0::phase0
