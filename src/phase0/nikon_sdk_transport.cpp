#if defined(A0_NIKON_SDK_AVAILABLE) && defined(_WIN32)
// The D810 headers use TCHAR aliases for byte-oriented MAID strings and are
// compatible with the SDK sample's multibyte build, not a Unicode TCHAR build.
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif
#include <Maid3.h>
#include <Maid3d1.h>
#endif

#include "a0/phase0/nikon_sdk_transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace a0::phase0 {
namespace fs = std::filesystem;

#if defined(A0_NIKON_SDK_AVAILABLE) && defined(_WIN32)
namespace {

constexpr auto kAsyncInterval = std::chrono::milliseconds(10);
constexpr auto kCandidateSettle = std::chrono::milliseconds(500);
// Phase 0 is a PC-capture path: the camera publishes a transient SDRAM Item,
// which is acquired and durably persisted by EvidenceWriter.
constexpr ULONG kDesiredSaveMedia = kNkMAIDSaveMedia_SDRAM;

std::mutex g_session_mutex;
bool g_session_active = false;

std::string IdentityForSource(ULONG source_id) {
    const std::string material = "nikon-d810-source:" + std::to_string(source_id);
    return Sha256Hex(std::vector<unsigned char>(material.begin(), material.end()));
}

std::string ResultText(NKERROR result) {
    return std::to_string(static_cast<long long>(result));
}

struct MaidObject {
    NkMAIDObject value{};
    std::vector<NkMAIDCapInfo> capabilities;
    bool opened{false};
};

struct CompletionState {
    std::atomic<bool> done{false};
    NKERROR result{kNkMAIDResult_UnexpectedError};
};

struct DownloadState {
    std::vector<unsigned char> bytes;
    std::size_t expected{0};
    std::size_t next_offset{0};
    ULONG file_type{kNkMAIDFileDataType_NotSpecified};
    bool saw_file{false};
    bool removed{false};
    bool invalid{false};
};

void CALLBACK CompletionProc(
    LPNkMAIDObject,
    ULONG,
    ULONG,
    ULONG,
    NKPARAM,
    NKREF reference,
    NKERROR result) {
    auto* state = reinterpret_cast<CompletionState*>(reference);
    if (state == nullptr) return;
    state->result = result;
    state->done.store(true, std::memory_order_release);
}

NKERROR CALLBACK DataProc(NKREF reference, LPVOID raw_info, LPVOID raw_data) {
    auto* state = reinterpret_cast<DownloadState*>(reference);
    auto* info = static_cast<NkMAIDDataInfo*>(raw_info);
    if (state == nullptr || info == nullptr || raw_data == nullptr ||
        (info->ulType & kNkMAIDDataObjType_File) == 0) {
        if (state != nullptr) state->invalid = true;
        return kNkMAIDResult_UnexpectedDataType;
    }

    auto* file = static_cast<NkMAIDFileInfo*>(raw_info);
    if (file->fDiskFile || file->ulFileDataType != kNkMAIDFileDataType_JPEG ||
        file->ulTotalLength == 0 || file->ulStart > file->ulTotalLength ||
        file->ulLength > file->ulTotalLength - file->ulStart) {
        state->invalid = true;
        return kNkMAIDResult_UnexpectedDataType;
    }

    if (!state->saw_file) {
        state->expected = file->ulTotalLength;
        state->file_type = file->ulFileDataType;
        state->bytes.resize(state->expected);
        state->saw_file = true;
    }
    if (state->expected != file->ulTotalLength || state->bytes.size() != state->expected ||
        file->ulLength == 0 || file->ulStart != state->next_offset) {
        state->invalid = true;
        return kNkMAIDResult_UnexpectedError;
    }

    const auto* begin = static_cast<const unsigned char*>(raw_data);
    std::copy(begin, begin + file->ulLength, state->bytes.begin() + file->ulStart);
    state->next_offset += file->ulLength;
    state->removed = state->removed || file->fRemoveObject != FALSE;
    return kNkMAIDResult_NoError;
}

} // namespace

class NikonSdkTransport::Impl {
public:
    Impl() = default;
    ~Impl() { CleanupNoThrow(); }

    std::string SdkVersion() const {
        return sdk_version_.empty() ? "D810-Remote-SDK-local" : sdk_version_;
    }

    std::vector<CameraInfo> Enumerate() {
        ClaimSession();
        try {
            OpenModule(std::chrono::steady_clock::now() + std::chrono::seconds(10));
            const auto cameras = EnumerateD810s(std::chrono::steady_clock::now() + std::chrono::seconds(10));
            CleanupNoThrow();
            return cameras;
        } catch (...) {
            CleanupNoThrow();
            throw;
        }
    }

    void Open(std::string_view stable_identity, std::chrono::seconds timeout) {
        if (stable_identity.empty()) throw TransportError("open_failed", "camera identity is empty");
        ClaimSession();
        try {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            OpenModule(deadline);
            const auto ids = WaitForSourceIds(deadline, "open_failed");
            std::size_t matches = 0;
            std::optional<ULONG> selected_id;
            for (const ULONG id : ids) {
                MaidObject candidate;
                OpenChild(module_, candidate, id, "open_failed");
                try {
                    EnumerateCapabilities(candidate, deadline, "open_failed");
                    const ULONG type = GetUnsigned(candidate, kNkMAIDCapability_CameraType, deadline, "open_failed");
                    if (type == kNkMAIDCameraType_D810 && IdentityForSource(id) == stable_identity) {
                        ++matches;
                        selected_id = id;
                    }
                    CloseObjectNoThrow(candidate);
                } catch (...) {
                    CloseObjectNoThrow(candidate);
                    throw;
                }
            }
            if (matches != 1 || !selected_id) {
                throw TransportError("open_failed", "requested D810 was not uniquely available");
            }
            // MAID object storage must remain at a stable address for the whole
            // session; identify with temporary objects, then open into the
            // long-lived member used by every later command and callback.
            OpenChild(module_, source_, *selected_id, "open_failed");
            source_id_ = *selected_id;
            EnumerateCapabilities(source_, deadline, "open_failed");
            SetProgressCallback(source_, deadline, "open_failed");
            SetEventCallback(source_, deadline, "open_failed");
            RunCompleted(source_, kNkMAIDCommand_EnumChildren, 0, kNkMAIDDataType_Null,
                0, deadline, "open_failed");
            original_save_media_ = GetUnsigned(source_, kNkMAIDCapability_SaveMedia, deadline, "save_media_mismatch");
            SetUnsigned(source_, kNkMAIDCapability_SaveMedia, kDesiredSaveMedia, deadline, "save_media_mismatch");
            const ULONG verified = GetUnsigned(source_, kNkMAIDCapability_SaveMedia, deadline, "save_media_mismatch");
            if (verified != kDesiredSaveMedia) {
                throw TransportError("save_media_mismatch", "SDRAM capture destination did not persist");
            }
            session_open_ = true;
        } catch (...) {
            CleanupNoThrow();
            throw;
        }
    }

    std::string Baseline(std::chrono::seconds timeout) {
        RequireOpen();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto children = Children(source_, deadline, "baseline_failed");
        baseline_.clear();
        baseline_.insert(children.begin(), children.end());
        added_items_.clear();
        late_items_.clear();
        removed_items_.clear();
        capture_complete_ = false;
        add_child_in_card_ = false;
        baseline_token_ = "baseline-" + std::to_string(++baseline_sequence_);
        return baseline_token_;
    }

    std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) {
        RequireOpen();
        if (baseline != baseline_token_ || baseline_token_.empty()) {
            throw TransportError("baseline_mismatch", "capture baseline token is not current");
        }

        const auto overall_deadline = std::chrono::steady_clock::now() + transaction_timeout;
        const auto event_deadline = std::min(
            std::chrono::steady_clock::now() + image_event_timeout,
            overall_deadline);
        StartProcess(source_, kNkMAIDCapability_Capture, event_deadline, "capture_command_failed");

        std::optional<std::chrono::steady_clock::time_point> candidates_stable_since;
        std::size_t observed_candidate_count = 0;
        while (std::chrono::steady_clock::now() < event_deadline) {
            Pump(source_, "image_event_failed");
            ReconcileChildren(event_deadline);
            const std::size_t candidate_count = CandidateIds().size();
            if (candidate_count != observed_candidate_count) {
                observed_candidate_count = candidate_count;
                candidates_stable_since = candidate_count == 0
                    ? std::nullopt
                    : std::optional{std::chrono::steady_clock::now()};
            }
            if (capture_complete_ && candidates_stable_since &&
                std::chrono::steady_clock::now() - *candidates_stable_since >= kCandidateSettle) {
                break;
            }
            std::this_thread::sleep_for(kAsyncInterval);
        }

        ReconcileChildren(event_deadline);
        if (std::chrono::steady_clock::now() >= overall_deadline) {
            throw TransportError("transaction_watchdog", "capture transaction watchdog expired");
        }
        if (!capture_complete_) {
            throw TransportError("image_event_timeout", "card CaptureComplete was not observed");
        }
        const auto ids = CandidateIds();
        if (ids.empty()) {
            throw TransportError(
                add_child_in_card_ ? "image_on_card_only" : "image_event_timeout",
                add_child_in_card_
                    ? "capture completed on card but no SDRAM image item arrived"
                    : "no post-baseline image item arrived");
        }
        if (std::any_of(ids.begin(), ids.end(), [this](ULONG id) { return removed_items_.contains(id); })) {
            throw TransportError("ambiguous_image_event", "a post-baseline image item disappeared before attribution");
        }

        std::vector<ImageCandidate> candidates;
        candidates.reserve(ids.size());
        const auto download_deadline = std::min(
            std::chrono::steady_clock::now() + download_timeout,
            overall_deadline);
        for (const ULONG id : ids) {
            if (std::chrono::steady_clock::now() >= overall_deadline) {
                throw TransportError("transaction_watchdog", "capture transaction watchdog expired");
            }
            auto candidate = AcquireCandidate(id, download_deadline);
            candidate.attributable = added_items_.contains(id) && !late_items_.contains(id);
            candidates.push_back(std::move(candidate));
        }
        baseline_token_.clear();
        return candidates;
    }

    void Close(std::chrono::seconds timeout) {
        if (!claimed_) return;
        std::optional<TransportError> pending_error;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (source_.opened && original_save_media_) {
            try {
                SetUnsigned(source_, kNkMAIDCapability_SaveMedia, *original_save_media_, deadline, "save_media_restore_failed");
                const ULONG restored = GetUnsigned(source_, kNkMAIDCapability_SaveMedia, deadline, "save_media_restore_failed");
                if (restored != *original_save_media_) {
                    throw TransportError("save_media_restore_failed", "original SaveMedia value was not restored");
                }
            } catch (const TransportError& error) {
                pending_error.emplace(error.Category(), error.what());
            }
            // Restoration is attempted exactly once during an explicit close.
            original_save_media_.reset();
        }
        try {
            CleanupObjects(deadline);
        } catch (const TransportError& error) {
            if (!pending_error) pending_error.emplace(error.Category(), error.what());
            CleanupNoThrow();
        }
        ReleaseSession();
        if (pending_error) throw *pending_error;
    }

private:
    void ClaimSession() {
        std::lock_guard lock(g_session_mutex);
        if (claimed_ || g_session_active) throw TransportError("session_busy", "another Nikon SDK session is active");
        g_session_active = true;
        claimed_ = true;
    }

    void ReleaseSession() noexcept {
        std::lock_guard lock(g_session_mutex);
        if (claimed_) g_session_active = false;
        claimed_ = false;
    }

    void RequireOpen() const {
        if (!session_open_ || !source_.opened) throw TransportError("session_not_open", "Nikon source is not open");
    }

    NKERROR Call(LPNkMAIDObject object, ULONG command, ULONG parameter, ULONG data_type,
                 NKPARAM data, LPNKFUNC completion = nullptr, NKREF reference = 0) const {
        if (entry_ == nullptr) return kNkMAIDResult_MissingComponent;
        return entry_(object, command, parameter, data_type, data, completion, reference);
    }

    void Pump(MaidObject& object, std::string_view category) {
        const NKERROR result = Call(&object.value, kNkMAIDCommand_Async, 0, kNkMAIDDataType_Null, 0);
        if (result != kNkMAIDResult_NoError && result != kNkMAIDResult_Pending) {
            throw TransportError(std::string(category), "SDK async failed: " + ResultText(result));
        }
    }

    NKERROR RunCompleted(MaidObject& object, ULONG command, ULONG parameter, ULONG data_type,
                         NKPARAM data, std::chrono::steady_clock::time_point deadline,
                         std::string_view category) {
        auto completion = std::make_unique<CompletionState>();
        CompletionState* state = completion.get();
        completions_.push_back(std::move(completion));
        const NKERROR immediate = Call(&object.value, command, parameter, data_type, data,
            reinterpret_cast<LPNKFUNC>(&CompletionProc), reinterpret_cast<NKREF>(state));
        if (immediate == kNkMAIDResult_BufferSize) return immediate;
        if (immediate != kNkMAIDResult_NoError && immediate != kNkMAIDResult_Pending) {
            throw TransportError(std::string(category), "SDK command failed: " + ResultText(immediate));
        }
        while (!state->done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                Call(&object.value, kNkMAIDCommand_Abort, 0, kNkMAIDDataType_Null, 0);
                throw TransportError(std::string(category), "SDK command timed out");
            }
            Pump(object, category);
            std::this_thread::sleep_for(kAsyncInterval);
        }
        if (state->result != kNkMAIDResult_NoError) {
            throw TransportError(std::string(category), "SDK completion failed: " + ResultText(state->result));
        }
        return immediate;
    }

    void EnumerateCapabilities(MaidObject& object, std::chrono::steady_clock::time_point deadline,
                               std::string_view category) {
        for (;;) {
            ULONG count = 0;
            RunCompleted(object, kNkMAIDCommand_GetCapCount, 0, kNkMAIDDataType_UnsignedPtr,
                reinterpret_cast<NKPARAM>(&count), deadline, category);
            object.capabilities.assign(count, NkMAIDCapInfo{});
            const NKERROR result = RunCompleted(object, kNkMAIDCommand_GetCapInfo, count,
                kNkMAIDDataType_CapInfoPtr, reinterpret_cast<NKPARAM>(object.capabilities.data()), deadline, category);
            if (result != kNkMAIDResult_BufferSize) return;
        }
    }

    const NkMAIDCapInfo* Capability(const MaidObject& object, ULONG id) const {
        const auto found = std::find_if(object.capabilities.begin(), object.capabilities.end(),
            [id](const NkMAIDCapInfo& cap) { return cap.ulID == id; });
        return found == object.capabilities.end() ? nullptr : &*found;
    }

    bool Supports(const MaidObject& object, ULONG id, ULONG operation) const {
        const auto* cap = Capability(object, id);
        return cap != nullptr && (cap->ulOperations & operation) != 0;
    }

    ULONG GetUnsigned(MaidObject& object, ULONG id, std::chrono::steady_clock::time_point deadline,
                      std::string_view category) {
        const auto* cap = Capability(object, id);
        if (cap == nullptr || cap->ulType != kNkMAIDCapType_Unsigned ||
            !Supports(object, id, kNkMAIDCapOperation_Get)) {
            throw TransportError(std::string(category), "required unsigned SDK capability is unavailable");
        }
        ULONG value = 0;
        RunCompleted(object, kNkMAIDCommand_CapGet, id, kNkMAIDDataType_UnsignedPtr,
            reinterpret_cast<NKPARAM>(&value), deadline, category);
        return value;
    }

    void SetUnsigned(MaidObject& object, ULONG id, ULONG value,
                     std::chrono::steady_clock::time_point deadline, std::string_view category) {
        const auto* cap = Capability(object, id);
        if (cap == nullptr || cap->ulType != kNkMAIDCapType_Unsigned ||
            !Supports(object, id, kNkMAIDCapOperation_Set)) {
            throw TransportError(std::string(category), "required writable SDK capability is unavailable");
        }
        RunCompleted(object, kNkMAIDCommand_CapSet, id, kNkMAIDDataType_Unsigned,
            static_cast<NKPARAM>(value), deadline, category);
    }

    std::vector<ULONG> Children(MaidObject& object, std::chrono::steady_clock::time_point deadline,
                                std::string_view category) {
        const auto* cap = Capability(object, kNkMAIDCapability_Children);
        if (cap == nullptr || cap->ulType != kNkMAIDCapType_Enum ||
            !Supports(object, kNkMAIDCapability_Children, kNkMAIDCapOperation_Get) ||
            !Supports(object, kNkMAIDCapability_Children, kNkMAIDCapOperation_GetArray)) {
            throw TransportError(std::string(category), "SDK Children capability is unavailable");
        }
        NkMAIDEnum values{};
        RunCompleted(object, kNkMAIDCommand_CapGet, kNkMAIDCapability_Children,
            kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, category);
        if (values.ulElements == 0) return {};
        if (values.wPhysicalBytes != static_cast<SWORD>(sizeof(ULONG))) {
            throw TransportError(std::string(category), "SDK child IDs have an unexpected width");
        }
        std::vector<ULONG> ids(values.ulElements);
        values.pData = ids.data();
        RunCompleted(object, kNkMAIDCommand_CapGetArray, kNkMAIDCapability_Children,
            kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, category);
        return ids;
    }

    std::string Firmware(MaidObject& source, std::chrono::steady_clock::time_point deadline) {
        const auto* cap = Capability(source, kNkMAIDCapability_Firmware);
        if (cap == nullptr || !Supports(source, kNkMAIDCapability_Firmware, kNkMAIDCapOperation_Get)) return "unknown";
        if (cap->ulType == kNkMAIDCapType_String) {
            NkMAIDString value{};
            RunCompleted(source, kNkMAIDCommand_CapGet, kNkMAIDCapability_Firmware,
                kNkMAIDDataType_StringPtr, reinterpret_cast<NKPARAM>(&value), deadline, "inventory_failed");
            return reinterpret_cast<const char*>(value.str);
        }
        if (cap->ulType == kNkMAIDCapType_Unsigned) {
            std::ostringstream text;
            text << "0x" << std::hex << std::uppercase
                 << GetUnsigned(source, kNkMAIDCapability_Firmware, deadline, "inventory_failed");
            return text.str();
        }
        return "unknown";
    }

    std::string ShootingMode(MaidObject& source, std::chrono::steady_clock::time_point deadline) {
        const auto* cap = Capability(source, kNkMAIDCapability_ShootingMode);
        if (cap == nullptr || !Supports(source, kNkMAIDCapability_ShootingMode, kNkMAIDCapOperation_Get)) {
            return "unknown";
        }
        ULONG value = eNkMAIDShootingMode_Unknown;
        if (cap->ulType == kNkMAIDCapType_Unsigned) {
            value = GetUnsigned(source, kNkMAIDCapability_ShootingMode, deadline, "inventory_failed");
        } else if (cap->ulType == kNkMAIDCapType_Enum) {
            NkMAIDEnum values{};
            RunCompleted(source, kNkMAIDCommand_CapGet, kNkMAIDCapability_ShootingMode,
                kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, "inventory_failed");
            if (values.ulElements == 0 || values.wPhysicalBytes != static_cast<SWORD>(sizeof(ULONG))) return "unknown";
            std::vector<ULONG> items(values.ulElements);
            values.pData = items.data();
            RunCompleted(source, kNkMAIDCommand_CapGetArray, kNkMAIDCapability_ShootingMode,
                kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, "inventory_failed");
            if (values.ulValue >= items.size()) return "unknown";
            value = items[values.ulValue];
        } else {
            return "unknown";
        }
        switch (value) {
        case eNkMAIDShootingMode_S: return "S";
        case eNkMAIDShootingMode_C: return "CL";
        case eNkMAIDShootingMode_CH: return "CH";
        case eNkMAIDShootingMode_SelfTimer: return "SelfTimer";
        case eNkMAIDShootingMode_MirrorUp: return "MUP";
        case eNkMAIDShootingMode_RemoteTimer_Instant: return "RemoteInstant";
        case eNkMAIDShootingMode_RemoteTimer_2sec: return "Remote2s";
        case eNkMAIDShootingMode_LiveView: return "LiveView";
        case eNkMAIDShootingMode_Quiet: return "Quiet";
        case eNkMAIDShootingMode_RemoteCtrl: return "Remote";
        case eNkMAIDShootingMode_QuietC: return "QuietContinuous";
        default: return "unknown";
        }
    }

    void SetEventCallback(MaidObject& object, std::chrono::steady_clock::time_point deadline,
                          std::string_view category) {
        if (!Supports(object, kNkMAIDCapability_EventProc, kNkMAIDCapOperation_Set)) return;
        NkMAIDCallback callback{};
        callback.pProc = object.value.ulType == kNkMAIDObjectType_Module
            ? reinterpret_cast<LPNKFUNC>(&ModuleEventProc)
            : reinterpret_cast<LPNKFUNC>(&SourceEventProc);
        callback.refProc = reinterpret_cast<NKREF>(this);
        RunCompleted(object, kNkMAIDCommand_CapSet, kNkMAIDCapability_EventProc,
            kNkMAIDDataType_CallbackPtr, reinterpret_cast<NKPARAM>(&callback), deadline, category);
    }

    void SetProgressCallback(MaidObject& object, std::chrono::steady_clock::time_point deadline,
                             std::string_view category) {
        if (!Supports(object, kNkMAIDCapability_ProgressProc, kNkMAIDCapOperation_Set)) return;
        NkMAIDCallback callback{};
        callback.pProc = reinterpret_cast<LPNKFUNC>(&ProgressProc);
        callback.refProc = reinterpret_cast<NKREF>(this);
        RunCompleted(object, kNkMAIDCommand_CapSet, kNkMAIDCapability_ProgressProc,
            kNkMAIDDataType_CallbackPtr, reinterpret_cast<NKPARAM>(&callback), deadline, category);
    }

    void SetUiCallback(MaidObject& object, std::chrono::steady_clock::time_point deadline) {
        if (!Supports(object, kNkMAIDCapability_UIRequestProc, kNkMAIDCapOperation_Set)) return;
        NkMAIDCallback callback{};
        callback.pProc = reinterpret_cast<LPNKFUNC>(&UiRequestProc);
        callback.refProc = reinterpret_cast<NKREF>(this);
        RunCompleted(object, kNkMAIDCommand_CapSet, kNkMAIDCapability_UIRequestProc,
            kNkMAIDDataType_CallbackPtr, reinterpret_cast<NKPARAM>(&callback), deadline, "open_failed");
    }

    void StartProcess(MaidObject& object, ULONG capability,
                      std::chrono::steady_clock::time_point deadline, std::string_view category) {
        const auto* cap = Capability(object, capability);
        if (cap == nullptr || cap->ulType != kNkMAIDCapType_Process ||
            !Supports(object, capability, kNkMAIDCapOperation_Start)) {
            throw TransportError(std::string(category), "required SDK process capability is unavailable");
        }
        RunCompleted(object, kNkMAIDCommand_CapStart, capability, kNkMAIDDataType_Null,
            0, deadline, category);
    }

    void OpenModule(std::chrono::steady_clock::time_point deadline) {
        const fs::path module_path = fs::path(A0_NIKON_SDK_MODULE_PATH);
        const std::wstring directory = module_path.parent_path().wstring();
        dll_directory_ = AddDllDirectory(directory.c_str());
        if (dll_directory_ == nullptr) throw TransportError("sdk_load_failed", "SDK DLL directory could not be registered");
        // Type0014 loads the USB transport at runtime rather than through its
        // import table.  Preload it from the licensed SDK directory so camera
        // discovery never depends on the process working directory.
        const fs::path ptp_path = module_path.parent_path() / L"NkdPTP.dll";
        ptp_handle_ = LoadLibraryExW(ptp_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (ptp_handle_ == nullptr) throw TransportError("sdk_load_failed", "Nikon USB transport could not be loaded");
        module_handle_ = LoadLibraryExW(module_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (module_handle_ == nullptr) throw TransportError("sdk_load_failed", "Type0014 module could not be loaded");
        entry_ = reinterpret_cast<LPMAIDEntryPointProc>(GetProcAddress(module_handle_, "MAIDEntryPoint"));
        if (entry_ == nullptr) throw TransportError("sdk_load_failed", "MAIDEntryPoint was not found");

        module_.value.refClient = reinterpret_cast<NKREF>(this);
        const NKERROR opened = Call(nullptr, kNkMAIDCommand_Open, 0, kNkMAIDDataType_ObjectPtr,
            reinterpret_cast<NKPARAM>(&module_.value));
        if (opened != kNkMAIDResult_NoError) {
            throw TransportError("sdk_load_failed", "SDK module open failed: " + ResultText(opened));
        }
        module_.opened = true;
        EnumerateCapabilities(module_, deadline, "sdk_load_failed");
        SetProgressCallback(module_, deadline, "sdk_load_failed");
        SetEventCallback(module_, deadline, "sdk_load_failed");
        SetUiCallback(module_, deadline);
        if (Supports(module_, kNkMAIDCapability_ModuleMode, kNkMAIDCapOperation_Set)) {
            SetUnsigned(module_, kNkMAIDCapability_ModuleMode, kNkMAIDModuleMode_Controller,
                deadline, "sdk_load_failed");
        }
        // Ask MAID to publish the currently attached source objects.  Reading
        // the Children capability alone is not sufficient on the D810 module:
        // EnumChildren emits the AddChild events that make the source visible.
        RunCompleted(module_, kNkMAIDCommand_EnumChildren, 0, kNkMAIDDataType_Null,
            0, deadline, "sdk_load_failed");
        if (Supports(module_, kNkMAIDCapability_Version, kNkMAIDCapOperation_Get)) {
            std::ostringstream version;
            version << "0x" << std::hex << std::uppercase
                    << GetUnsigned(module_, kNkMAIDCapability_Version, deadline, "sdk_load_failed");
            sdk_version_ = version.str();
        }
    }

    std::vector<CameraInfo> EnumerateD810s(std::chrono::steady_clock::time_point deadline) {
        std::vector<CameraInfo> cameras;
        for (const ULONG id : WaitForSourceIds(deadline, "inventory_failed")) {
            MaidObject source;
            OpenChild(module_, source, id, "inventory_failed");
            try {
                EnumerateCapabilities(source, deadline, "inventory_failed");
                const ULONG type = GetUnsigned(source, kNkMAIDCapability_CameraType, deadline, "inventory_failed");
                if (type == kNkMAIDCameraType_D810) {
                    cameras.push_back({"Nikon D810", Firmware(source, deadline), ShootingMode(source, deadline), IdentityForSource(id)});
                }
            } catch (...) {
                CloseObjectNoThrow(source);
                throw;
            }
            CloseObjectNoThrow(source);
        }
        return cameras;
    }

    std::vector<ULONG> WaitForSourceIds(
        std::chrono::steady_clock::time_point deadline,
        std::string_view category) {
        std::vector<ULONG> ids;
        while (std::chrono::steady_clock::now() < deadline) {
            Pump(module_, category);
            ids = Children(module_, deadline, category);
            ids.insert(ids.end(), module_sources_.begin(), module_sources_.end());
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            if (!ids.empty()) return ids;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return ids;
    }

    void OpenChild(MaidObject& parent, MaidObject& child, ULONG id, std::string_view category) {
        child.value.refClient = reinterpret_cast<NKREF>(this);
        const NKERROR result = Call(&parent.value, kNkMAIDCommand_Open, id,
            kNkMAIDDataType_ObjectPtr, reinterpret_cast<NKPARAM>(&child.value));
        if (result != kNkMAIDResult_NoError) {
            throw TransportError(std::string(category), "SDK child open failed: " + ResultText(result));
        }
        child.opened = true;
    }

    void ReconcileChildren(std::chrono::steady_clock::time_point deadline) {
        try {
            // MAID permits clients to force publication of all current child
            // Items.  D810 can complete capture without delivering the
            // spontaneous AddChild event, so reconcile both the event stream
            // and the Children capability.
            RunCompleted(source_, kNkMAIDCommand_EnumChildren, 0, kNkMAIDDataType_Null,
                0, deadline, "image_event_failed");
            for (const ULONG id : Children(source_, deadline, "image_event_failed")) {
                ObserveCandidate(id);
            }
        } catch (const TransportError&) {
            // Capture completion may temporarily make Children busy; callback events remain authoritative.
        }
    }

    std::vector<ULONG> CandidateIds() const {
        std::vector<ULONG> ids(added_items_.begin(), added_items_.end());
        ids.insert(ids.end(), late_items_.begin(), late_items_.end());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    }

    void ObserveCandidate(ULONG id) {
        if (baseline_.contains(id) || added_items_.contains(id) || late_items_.contains(id)) return;
        // D810 can publish the SDRAM Item after CaptureComplete.  An item is
        // attributable while the current baseline token and event window are
        // active; only items observed outside that window are late.
        if (baseline_token_.empty()) late_items_.insert(id);
        else added_items_.insert(id);
    }

    ImageCandidate AcquireCandidate(ULONG id, std::chrono::steady_clock::time_point deadline) {
        MaidObject item;
        MaidObject data;
        OpenChild(source_, item, id, "download_failed");
        try {
            EnumerateCapabilities(item, deadline, "download_failed");
            const ULONG data_types = GetUnsigned(item, kNkMAIDCapability_DataTypes, deadline, "download_failed");
            if ((data_types & kNkMAIDDataObjType_Image) == 0) {
                throw TransportError("invalid_jpeg", "captured item does not expose image data");
            }
            OpenChild(item, data, kNkMAIDDataObjType_Image, "download_failed");
            EnumerateCapabilities(data, deadline, "download_failed");
            if (!Supports(data, kNkMAIDCapability_DataProc, kNkMAIDCapOperation_Set)) {
                throw TransportError("download_failed", "image DataProc cannot be set");
            }

            auto download = std::make_unique<DownloadState>();
            DownloadState* download_state = download.get();
            downloads_.push_back(std::move(download));
            NkMAIDCallback callback{};
            callback.pProc = reinterpret_cast<LPNKFUNC>(&DataProc);
            callback.refProc = reinterpret_cast<NKREF>(download_state);
            RunCompleted(data, kNkMAIDCommand_CapSet, kNkMAIDCapability_DataProc,
                kNkMAIDDataType_CallbackPtr, reinterpret_cast<NKPARAM>(&callback), deadline, "download_failed");
            StartProcess(data, kNkMAIDCapability_Acquire, deadline, "download_timeout");
            try {
                RunCompleted(data, kNkMAIDCommand_CapSet, kNkMAIDCapability_DataProc,
                    kNkMAIDDataType_Null, 0, deadline, "download_failed");
            } catch (...) {
                CloseObjectNoThrow(data);
                CloseObjectNoThrow(item);
                throw;
            }
            if (download_state->invalid || !download_state->saw_file ||
                download_state->file_type != kNkMAIDFileDataType_JPEG ||
                download_state->bytes.size() != download_state->expected ||
                download_state->next_offset != download_state->expected ||
                !IsValidJpeg(download_state->bytes)) {
                throw TransportError("invalid_jpeg", "SDK acquisition did not produce a complete JPEG");
            }
            ImageCandidate candidate{"sdk-item-" + std::to_string(id) + ".jpg", download_state->bytes};
            CloseObjectNoThrow(data);
            CloseObjectNoThrow(item);
            return candidate;
        } catch (...) {
            CloseObjectNoThrow(data);
            CloseObjectNoThrow(item);
            throw;
        }
    }

    void CleanupObjects(std::chrono::steady_clock::time_point) {
        if (source_.opened) {
            const NKERROR result = Call(&source_.value, kNkMAIDCommand_Close, 0, kNkMAIDDataType_Null, 0);
            source_.opened = false;
            if (result != kNkMAIDResult_NoError && result != kNkMAIDResult_ZombieObject) {
                throw TransportError("close_failed", "SDK source close failed: " + ResultText(result));
            }
        }
        if (module_.opened) {
            const NKERROR result = Call(&module_.value, kNkMAIDCommand_Close, 0, kNkMAIDDataType_Null, 0);
            module_.opened = false;
            if (result != kNkMAIDResult_NoError) {
                throw TransportError("close_failed", "SDK module close failed: " + ResultText(result));
            }
        }
        UnloadModule();
        session_open_ = false;
        original_save_media_.reset();
    }

    void CloseObjectNoThrow(MaidObject& object) noexcept {
        if (!object.opened || entry_ == nullptr) return;
        Call(&object.value, kNkMAIDCommand_Close, 0, kNkMAIDDataType_Null, 0);
        object.opened = false;
    }

    void UnloadModule() noexcept {
        entry_ = nullptr;
        if (module_handle_ != nullptr) {
            FreeLibrary(module_handle_);
            module_handle_ = nullptr;
        }
        if (ptp_handle_ != nullptr) {
            FreeLibrary(ptp_handle_);
            ptp_handle_ = nullptr;
        }
        if (dll_directory_ != nullptr) {
            RemoveDllDirectory(dll_directory_);
            dll_directory_ = nullptr;
        }
        completions_.clear();
        downloads_.clear();
    }

    void CleanupNoThrow() noexcept {
        if (source_.opened && original_save_media_) {
            try {
                SetUnsigned(source_, kNkMAIDCapability_SaveMedia, *original_save_media_,
                    std::chrono::steady_clock::now() + std::chrono::seconds(2),
                    "save_media_restore_failed");
            } catch (...) {
                // Emergency cleanup cannot surface an error (destructor/open rollback).
            }
            original_save_media_.reset();
        }
        CloseObjectNoThrow(source_);
        CloseObjectNoThrow(module_);
        UnloadModule();
        session_open_ = false;
        original_save_media_.reset();
        module_sources_.clear();
        baseline_.clear();
        added_items_.clear();
        late_items_.clear();
        removed_items_.clear();
        baseline_token_.clear();
        ReleaseSession();
    }

    static void CALLBACK ModuleEventProc(NKREF reference, ULONG event, NKPARAM data) {
        auto* self = reinterpret_cast<Impl*>(reference);
        if (self == nullptr) return;
        const ULONG id = static_cast<ULONG>(data);
        if (event == kNkMAIDEvent_AddChild) self->module_sources_.insert(id);
        if (event == kNkMAIDEvent_RemoveChild) self->module_sources_.erase(id);
    }

    static void CALLBACK ProgressProc(ULONG, ULONG, NKREF, ULONG, ULONG) {}

    static void CALLBACK SourceEventProc(NKREF reference, ULONG event, NKPARAM data) {
        auto* self = reinterpret_cast<Impl*>(reference);
        if (self == nullptr) return;
        const ULONG id = static_cast<ULONG>(data);
        switch (event) {
        case kNkMAIDEvent_AddChild:
            self->ObserveCandidate(id);
            break;
        case kNkMAIDEvent_RemoveChild:
            self->removed_items_.insert(id);
            break;
        case kNkMAIDEvent_CaptureComplete:
            // The D810 SDK sample treats the event itself as completion and
            // does not assign a contract to its data parameter.
            self->capture_complete_ = true;
            break;
        case kNkMAIDEvent_AddChildInCard:
            self->add_child_in_card_ = true;
            break;
        default:
            break;
        }
    }

    static ULONG CALLBACK UiRequestProc(NKREF, LPNkMAIDUIRequestInfo request) {
        if (request == nullptr) return kNkMAIDUIRequestResult_None;
        switch (request->ulType) {
        case kNkMAIDUIRequestType_Ok: return kNkMAIDUIRequestResult_Ok;
        case kNkMAIDUIRequestType_OkCancel:
        case kNkMAIDUIRequestType_YesNoCancel: return kNkMAIDUIRequestResult_Cancel;
        case kNkMAIDUIRequestType_YesNo: return kNkMAIDUIRequestResult_No;
        default: return kNkMAIDUIRequestResult_None;
        }
    }

    HMODULE module_handle_{nullptr};
    HMODULE ptp_handle_{nullptr};
    DLL_DIRECTORY_COOKIE dll_directory_{nullptr};
    LPMAIDEntryPointProc entry_{nullptr};
    MaidObject module_;
    MaidObject source_;
    ULONG source_id_{0};
    bool claimed_{false};
    bool session_open_{false};
    bool capture_complete_{false};
    bool add_child_in_card_{false};
    std::optional<ULONG> original_save_media_;
    std::set<ULONG> baseline_;
    std::set<ULONG> module_sources_;
    std::set<ULONG> added_items_;
    std::set<ULONG> late_items_;
    std::set<ULONG> removed_items_;
    std::string baseline_token_;
    unsigned long long baseline_sequence_{0};
    std::string sdk_version_;
    std::vector<std::unique_ptr<CompletionState>> completions_;
    std::vector<std::unique_ptr<DownloadState>> downloads_;
};

NikonSdkTransport::NikonSdkTransport() : impl_(std::make_unique<Impl>()) {}
NikonSdkTransport::~NikonSdkTransport() = default;
std::string NikonSdkTransport::SdkVersion() const { return impl_->SdkVersion(); }
std::vector<CameraInfo> NikonSdkTransport::Enumerate() { return impl_->Enumerate(); }
void NikonSdkTransport::Open(std::string_view stable_identity, std::chrono::seconds timeout) {
    impl_->Open(stable_identity, timeout);
}
std::string NikonSdkTransport::Baseline(std::chrono::seconds timeout) { return impl_->Baseline(timeout); }
std::vector<ImageCandidate> NikonSdkTransport::CaptureAndDownload(
    std::string_view baseline,
    std::chrono::seconds image_event_timeout,
    std::chrono::seconds download_timeout,
    std::chrono::seconds transaction_timeout) {
    return impl_->CaptureAndDownload(baseline, image_event_timeout, download_timeout, transaction_timeout);
}
void NikonSdkTransport::Close(std::chrono::seconds timeout) { impl_->Close(timeout); }
bool NikonSdkTransport::LicensedAdapterAvailable() noexcept {
    try {
        return fs::exists(fs::path(A0_NIKON_SDK_MODULE_PATH));
    } catch (...) {
        return false;
    }
}

#else

class NikonSdkTransport::Impl {};

namespace {
[[noreturn]] void ThrowGated() {
    throw TransportError(
        "licensed_adapter_unavailable",
        "Nikon D810 SDK adapter is gated. Configure a complete ignored local SDK root.");
}
} // namespace

NikonSdkTransport::NikonSdkTransport() : impl_(std::make_unique<Impl>()) {}
NikonSdkTransport::~NikonSdkTransport() = default;
std::string NikonSdkTransport::SdkVersion() const { return "not-linked-license-gated"; }
std::vector<CameraInfo> NikonSdkTransport::Enumerate() { ThrowGated(); }
void NikonSdkTransport::Open(std::string_view, std::chrono::seconds) { ThrowGated(); }
std::string NikonSdkTransport::Baseline(std::chrono::seconds) { ThrowGated(); }
std::vector<ImageCandidate> NikonSdkTransport::CaptureAndDownload(
    std::string_view, std::chrono::seconds, std::chrono::seconds, std::chrono::seconds) { ThrowGated(); }
void NikonSdkTransport::Close(std::chrono::seconds) { ThrowGated(); }
bool NikonSdkTransport::LicensedAdapterAvailable() noexcept { return false; }

#endif

} // namespace a0::phase0
