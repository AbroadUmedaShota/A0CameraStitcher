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
#include "a0/phase0/sdk_pending_command.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace a0::phase0 {
namespace fs = std::filesystem;

std::string DeriveNikonSdkStableIdentity(
    std::string_view source_name,
    std::string_view source_interface) {
    constexpr std::size_t kMaximumMaidStringBytes = 255;
    const auto invalid = [](std::string_view value) {
        return value.empty() || value.size() > kMaximumMaidStringBytes ||
            value.find('\0') != std::string_view::npos;
    };
    if (invalid(source_name) || invalid(source_interface)) {
        throw std::invalid_argument("Nikon SDK source identity strings are missing or invalid");
    }

    std::vector<unsigned char> material;
    constexpr std::string_view domain = "a0camera-nikon-maid-source-identity-v2";
    material.insert(material.end(), domain.begin(), domain.end());
    material.push_back(0);
    const auto append = [&material](std::string_view value) {
        const auto length = static_cast<std::uint32_t>(value.size());
        material.push_back(static_cast<unsigned char>((length >> 24U) & 0xFFU));
        material.push_back(static_cast<unsigned char>((length >> 16U) & 0xFFU));
        material.push_back(static_cast<unsigned char>((length >> 8U) & 0xFFU));
        material.push_back(static_cast<unsigned char>(length & 0xFFU));
        material.insert(material.end(), value.begin(), value.end());
    };
    append(source_name);
    append(source_interface);
    return Sha256Hex(material);
}

void NikonCardCaptureEventWindow::ResetForSession() noexcept {
    snapshot_ = {};
}

void NikonCardCaptureEventWindow::CallbackRegistered() noexcept {
    if (!snapshot_.session_closed) snapshot_.callback_registered = true;
}

bool NikonCardCaptureEventWindow::BeginCaptureCommand() noexcept {
    if (!snapshot_.callback_registered || snapshot_.session_closed ||
        snapshot_.capture_command_started) {
        return false;
    }
    snapshot_.capture_command_started = true;
    return true;
}

bool NikonCardCaptureEventWindow::BeginEventPump() noexcept {
    if (!snapshot_.capture_command_started || snapshot_.session_closed ||
        snapshot_.event_pump_started) {
        return false;
    }
    snapshot_.event_pump_started = true;
    snapshot_.event_pump_stopped = false;
    return true;
}

void NikonCardCaptureEventWindow::CaptureCommandAccepted() noexcept {
    if (snapshot_.capture_command_started && !snapshot_.session_closed) {
        snapshot_.capture_command_accepted = true;
    }
}

void NikonCardCaptureEventWindow::Observe(NikonCardCaptureEvent event) noexcept {
    if (!snapshot_.callback_registered || !snapshot_.capture_command_started ||
        !snapshot_.event_pump_started || snapshot_.event_pump_stopped ||
        snapshot_.session_closed) {
        ++snapshot_.ignored_events;
        return;
    }
    if (event == NikonCardCaptureEvent::capture_complete) {
        ++snapshot_.capture_complete_events;
    } else if (event == NikonCardCaptureEvent::add_child_in_card) {
        ++snapshot_.add_child_in_card_events;
    }
}

void NikonCardCaptureEventWindow::EndEventPump() noexcept {
    if (snapshot_.event_pump_started) snapshot_.event_pump_stopped = true;
}

void NikonCardCaptureEventWindow::SessionClosed() noexcept {
    snapshot_.session_closed_while_pumping =
        snapshot_.event_pump_started && !snapshot_.event_pump_stopped;
    snapshot_.event_pump_stopped = snapshot_.event_pump_started;
    snapshot_.session_closed = true;
}

bool NikonCardCaptureEventWindow::CaptureCompleted() const noexcept {
    return snapshot_.capture_command_accepted &&
        snapshot_.capture_complete_events > 0;
}

NikonCardCaptureEventSnapshot NikonCardCaptureEventWindow::Snapshot() const noexcept {
    return snapshot_;
}

#if defined(A0_NIKON_SDK_AVAILABLE) && defined(_WIN32)
namespace {

constexpr auto kAsyncInterval = std::chrono::milliseconds(10);
// Grace period for abandoning a pending async command after Abort: bounded
// polling for the completion callback, roughly kAbortGraceIterations *
// kAsyncInterval (~250ms), before the buffer handed to the SDK is treated as
// unrecoverable and the session is quarantined.
constexpr int kAbortGraceIterations = 25;
constexpr auto kCandidateSettle = std::chrono::milliseconds(500);
constexpr std::size_t kMaxLiveViewArrayBytes = 16U * 1024U * 1024U;
// Mirrors wpd_transport.cpp's kMaximumJpegBytes: bounds a device-reported
// file transfer size so a malicious or malfunctioning SDK cannot force an
// unbounded allocation (ulTotalLength is a device-controlled ULONG with no
// SDK-side upper bound other than != 0).
constexpr ULONG kMaximumNikonFileDownloadBytes = 256U * 1024U * 1024U;
// Bounds camera-reported enum/array element counts (Children ids, capability
// counts, ShootingMode enum values) to the same order of magnitude already
// used for status enums below, so a malformed response cannot force an
// unbounded allocation.
constexpr ULONG kMaximumNikonEnumElements = 256;
constexpr ULONG kMaximumNikonCapabilityCount = 8192;
// Hybrid capture writes to the camera card; WPD observes the resulting object
// only after this SDK session has fully closed.
constexpr ULONG kDesiredSaveMedia = kNkMAIDSaveMedia_Card;

std::mutex g_session_mutex;
bool g_session_active = false;

std::string ResultText(NKERROR result) {
    if (result == kNkMAIDResult_DeviceBusy) return "DeviceBusy(152)";
    if (result == kNkMAIDResult_NotLiveView) return "NotLiveView(159)";
    if (result == kNkMAIDResult_BufferNotReady) return "BufferNotReady(129)";
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
        file->ulTotalLength == 0 || file->ulTotalLength > kMaximumNikonFileDownloadBytes ||
        file->ulStart > file->ulTotalLength ||
        file->ulLength > file->ulTotalLength - file->ulStart) {
        // Reject rather than throw: this callback runs on the vendor DLL's
        // stack, and a std::bad_alloc (or any exception) unwinding across
        // that frame is undefined behavior.
        state->invalid = true;
        return kNkMAIDResult_UnexpectedDataType;
    }

    if (!state->saw_file) {
        state->expected = file->ulTotalLength;
        state->file_type = file->ulFileDataType;
        // The size cap above bounds this allocation but does not guarantee
        // it succeeds (fragmentation, other pressure). resize() can throw
        // std::bad_alloc/std::length_error; std::copy below cannot throw.
        // This callback runs on the vendor DLL's stack, so an exception
        // unwinding across that frame would be undefined behavior -- reject
        // instead, using the same "invalid" pattern as the rest of this
        // function.
        try {
            state->bytes.resize(state->expected);
        } catch (...) {
            state->invalid = true;
            return kNkMAIDResult_UnexpectedError;
        }
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
        OpenSource(stable_identity, timeout, true);
    }

    void OpenLiveView(std::string_view stable_identity, std::chrono::seconds timeout) {
        OpenSource(stable_identity, timeout, false);
    }

    SdkCameraStatus ProbeSdkStatus(std::string_view stable_identity, std::chrono::seconds timeout) {
        OpenSource(stable_identity, timeout, false);
        try {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            SdkCameraStatus status;
            status.firmware = Firmware(source_, deadline, "sdk_status_failed");

            if (const auto value = TryGetCurrentValue(
                    source_, kNkMAIDCapability_LiveViewStatus, deadline, "sdk_status_failed")) {
                status.live_view_status_available = true;
                if (*value == kNkMAIDLiveViewStatus_OFF) status.live_view_status = "off";
                else if (*value == kNkMAIDLiveViewStatus_ON) status.live_view_status = "on";
            }
            if (const auto value = TryGetCurrentValue(
                    source_, kNkMAIDCapability_LiveViewSelector, deadline, "sdk_status_failed")) {
                status.live_view_selector_available = true;
                if (*value == kNkMAIDLiveViewSelector_Photo) status.live_view_selector = "photo";
                else if (*value == kNkMAIDLiveViewSelector_Movie) status.live_view_selector = "movie";
            }
            if (const auto value = TryGetCurrentValue(
                    source_, kNkMAIDCapability_LiveViewProhibit, deadline, "sdk_status_failed")) {
                status.live_view_prohibit_mask = static_cast<std::uint32_t>(*value);
            }
            status.file_type = ReadSettingCapability(source_, kNkMAIDCapability_FileType, deadline);
            status.compression_level = ReadSettingCapability(source_, kNkMAIDCapability_CompressionLevel, deadline);
            status.image_size = ReadSettingCapability(source_, kNkMAIDCapability_ImageSize, deadline);
            status.exposure_mode = ReadSettingCapability(source_, kNkMAIDCapability_ExposureMode, deadline);
            status.shutter_speed = ReadSettingCapability(source_, kNkMAIDCapability_ShutterSpeed, deadline);
            status.aperture = ReadSettingCapability(source_, kNkMAIDCapability_Aperture, deadline);
            status.sensitivity = ReadSettingCapability(source_, kNkMAIDCapability_Sensitivity, deadline);
            status.wb_mode = ReadSettingCapability(source_, kNkMAIDCapability_WBMode, deadline);
            status.focus_mode = ReadSettingCapability(source_, kNkMAIDCapability_FocusMode, deadline);

            Close(timeout);
            status.command_trace = trace_;
            if (const auto failure = ValidateSdkReadOnlyCommandTrace(status.command_trace)) {
                throw TransportError(
                    "sdk_status_command_trace_invalid",
                    "SDK status probe crossed a mutating command boundary: " + std::string(*failure));
            }
            return status;
        } catch (...) {
            try { Close(timeout); } catch (...) {}
            throw;
        }
    }

    void OpenSource(std::string_view stable_identity, std::chrono::seconds timeout, bool capture_session) {
        if (stable_identity.empty()) throw TransportError("open_failed", "camera identity is empty");
        ClaimSession();
        // sdk_session_poisoned_ reflects a previous session's abandoned
        // async command: the vendor DLL may still hold a pointer into a
        // buffer this transport already released. Merely closing the MAID
        // source/module objects does not prove that risk is gone -- the DLL
        // can still write after Close(). What actually proves it is the
        // chain that must already have run for ClaimSession() to succeed
        // here:
        //   ClaimSession() succeeded => claimed_ was false
        //     => ReleaseSession() already ran (end of Close()'s
        //        CleanupObjects() path, or end of CleanupNoThrow())
        //     => both of those call UnloadModule() before ReleaseSession()
        //     => UnloadModule() FreeLibrary()s the vendor module and USB
        //        transport DLLs and clears entry_
        //     => the vendor code that could still be writing into the old
        //        buffer no longer exists in this process
        // This is not a new assumption: it is the same safety model
        // completions_/downloads_ already rely on -- both are only cleared
        // after UnloadModule() (see #145's discussion of that existing
        // pattern). Enforce the chain rather than merely assume it, so a
        // future change that unloads the module lazily (e.g. a DLL cache)
        // instead of from Close()/CleanupNoThrow() fails loudly here
        // instead of silently making this reset unsound.
        if (entry_ != nullptr || module_handle_ != nullptr) {
            throw TransportError(
                "open_failed",
                "internal invariant violated: SDK module was not unloaded before a new session claim");
        }
        sdk_session_poisoned_ = false;
        trace_ = {};
        card_capture_events_.ResetForSession();
        try {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            OpenModule(deadline);
            const auto ids = WaitForSourceIds(deadline, "open_failed");
            std::size_t matches = 0;
            std::size_t d810_count = 0;
            std::optional<ULONG> selected_id;
            for (const ULONG id : ids) {
                MaidObject candidate;
                OpenChild(module_, candidate, id, "open_failed");
                try {
                    EnumerateCapabilities(candidate, deadline, "open_failed");
                    const ULONG type = GetUnsigned(candidate, kNkMAIDCapability_CameraType, deadline, "open_failed");
                    if (type == kNkMAIDCameraType_D810) {
                        ++d810_count;
                        if (StableIdentity(candidate, deadline, "open_failed") == stable_identity) {
                            ++matches;
                            selected_id = id;
                        }
                    }
                    CloseObjectNoThrow(candidate);
                } catch (...) {
                    CloseObjectNoThrow(candidate);
                    throw;
                }
            }
            if (require_exactly_one_d810_ && d810_count != 1) {
                throw TransportError(
                    "camera_count_mismatch",
                    "product SingleCamera SDK open requires exactly one currently connected D810");
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
            const bool source_event_callback_supported =
                Supports(source_, kNkMAIDCapability_EventProc, kNkMAIDCapOperation_Set);
            SetEventCallback(source_, deadline, "open_failed");
            if (source_event_callback_supported) card_capture_events_.CallbackRegistered();
            RunCompleted(source_, kNkMAIDCommand_EnumChildren, 0, kNkMAIDDataType_Null,
                0, deadline, "open_failed");
            if (capture_session) {
                original_save_media_ = GetUnsigned(source_, kNkMAIDCapability_SaveMedia, deadline, "save_media_mismatch");
                SetUnsigned(source_, kNkMAIDCapability_SaveMedia, kDesiredSaveMedia, deadline, "save_media_mismatch");
                const ULONG verified = GetUnsigned(source_, kNkMAIDCapability_SaveMedia, deadline, "save_media_mismatch");
                if (verified != kDesiredSaveMedia) {
                    throw TransportError("save_media_mismatch", "card capture destination did not persist");
                }
            }
            session_open_ = true;
            capture_session_ = capture_session;
            live_view_session_ = !capture_session;
            trace_.sdk_session_opened = true;
        } catch (...) {
            CleanupNoThrow();
            throw;
        }
    }

    std::string Baseline(std::chrono::seconds timeout) {
        RequireCaptureSession();
        RequireSdkSessionNotPoisoned();
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
        RequireCaptureSession();
        RequireSdkSessionNotPoisoned();
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

    void CaptureToCard(std::chrono::seconds image_event_timeout,
                       std::chrono::seconds transaction_timeout) {
        RequireCaptureSession();
        RequireSdkSessionNotPoisoned();
        const auto overall_deadline = std::chrono::steady_clock::now() + transaction_timeout;
        const auto event_deadline = std::min(std::chrono::steady_clock::now() + image_event_timeout, overall_deadline);
        capture_complete_ = false;
        add_child_in_card_ = false;
        if (!card_capture_events_.BeginCaptureCommand() ||
            !card_capture_events_.BeginEventPump()) {
            throw TransportError(
                "image_event_failed",
                "SDK source event callback was not active before the card capture command");
        }
        try {
            StartProcess(source_, kNkMAIDCapability_Capture, event_deadline, "capture_command_failed");
            card_capture_events_.CaptureCommandAccepted();
            while (std::chrono::steady_clock::now() < event_deadline) {
                Pump(source_, "image_event_failed");
                if (card_capture_events_.CaptureCompleted()) break;
                std::this_thread::sleep_for(kAsyncInterval);
            }
        } catch (...) {
            card_capture_events_.EndEventPump();
            throw;
        }
        card_capture_events_.EndEventPump();
        if (std::chrono::steady_clock::now() >= overall_deadline) {
            throw TransportError("transaction_watchdog", "card capture transaction watchdog expired");
        }
        if (!card_capture_events_.CaptureCompleted()) {
            throw TransportError("image_event_timeout", "card CaptureComplete was not observed");
        }
        // SaveMedia=Card does not require an SDK Item notification. WPD still
        // must recover exactly one post-baseline JPEG after this SDK session
        // fully closes, so CaptureComplete cannot cause ambiguous adoption.
    }

    void StartLiveView(std::chrono::seconds timeout) {
        RequireLiveViewSession();
        RequireSdkSessionNotPoisoned();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const ULONG prohibit = GetUnsigned(
            source_, kNkMAIDCapability_LiveViewProhibit, deadline, "live_view_prohibited");
        if (prohibit != 0) {
            std::ostringstream message;
            message << "D810 live view is prohibited by camera state: 0x"
                    << std::hex << std::uppercase << prohibit;
            throw TransportError("live_view_prohibited", message.str());
        }

        const ULONG current = GetUnsigned(
            source_, kNkMAIDCapability_LiveViewStatus, deadline, "live_view_start_failed");
        if (current != kNkMAIDLiveViewStatus_OFF) {
            SetUnsigned(source_, kNkMAIDCapability_LiveViewStatus,
                kNkMAIDLiveViewStatus_OFF, deadline, "live_view_recovery_failed");
            if (GetUnsigned(source_, kNkMAIDCapability_LiveViewStatus, deadline,
                    "live_view_recovery_failed") != kNkMAIDLiveViewStatus_OFF) {
                throw TransportError("live_view_recovery_failed", "existing live view state could not be stopped");
            }
        }

        SetUnsigned(source_, kNkMAIDCapability_LiveViewStatus,
            kNkMAIDLiveViewStatus_ON, deadline, "live_view_start_failed");
        live_view_started_ = true;
        live_view_stop_attempted_ = false;
        if (GetUnsigned(source_, kNkMAIDCapability_LiveViewStatus, deadline,
                "live_view_start_failed") != kNkMAIDLiveViewStatus_ON) {
            throw TransportError("live_view_start_failed", "D810 did not enter live view mode");
        }
        // D810 changes the available Source operations after entering live view.
        // Refresh the capability table before asking for the first frame.
        EnumerateCapabilities(source_, deadline, "live_view_start_failed");
    }

    std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds timeout) {
        RequireLiveViewSession();
        RequireSdkSessionNotPoisoned();
        if (!live_view_started_) {
            throw TransportError("live_view_not_started", "live view frame requested before start");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto* capability = Capability(source_, kNkMAIDCapability_GetLiveViewImage);
        if (capability == nullptr || capability->ulType != kNkMAIDCapType_Array ||
            !Supports(source_, kNkMAIDCapability_GetLiveViewImage, kNkMAIDCapOperation_Get) ||
            !Supports(source_, kNkMAIDCapability_GetLiveViewImage, kNkMAIDCapOperation_GetArray)) {
            std::ostringstream message;
            message << "D810 live view image capability is unavailable";
            if (capability != nullptr) {
                message << " (type=" << capability->ulType
                        << ", operations=0x" << std::hex << std::uppercase
                        << capability->ulOperations << ')';
            }
            throw TransportError("live_view_unavailable", message.str());
        }

        NkMAIDArray frame{};
        RunCompleted(source_, kNkMAIDCommand_CapGet, kNkMAIDCapability_GetLiveViewImage,
            kNkMAIDDataType_ArrayPtr, reinterpret_cast<NKPARAM>(&frame), deadline,
            "live_view_frame_failed");
        const std::size_t elements = static_cast<std::size_t>(frame.ulElements);
        const std::size_t physical_bytes = static_cast<std::size_t>(frame.wPhysicalBytes);
        if (elements == 0 || physical_bytes == 0 ||
            elements > std::numeric_limits<std::size_t>::max() / physical_bytes ||
            elements * physical_bytes > kMaxLiveViewArrayBytes) {
            throw TransportError("live_view_invalid_frame", "D810 returned an invalid live view array size");
        }
        std::vector<unsigned char> raw(elements * physical_bytes);
        frame.pData = raw.data();
        RunCompleted(source_, kNkMAIDCommand_CapGetArray, kNkMAIDCapability_GetLiveViewImage,
            kNkMAIDDataType_ArrayPtr, reinterpret_cast<NKPARAM>(&frame), deadline,
            "live_view_frame_failed");
        return ExtractD810LiveViewJpeg(raw);
    }

    void StopLiveView(std::chrono::seconds timeout) {
        RequireLiveViewSession();
        if (live_view_stop_attempted_) {
            throw TransportError("live_view_stop_already_attempted", "live view stop is not automatically retried");
        }
        live_view_stop_attempted_ = true;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        SetUnsigned(source_, kNkMAIDCapability_LiveViewStatus,
            kNkMAIDLiveViewStatus_OFF, deadline, "live_view_stop_failed");
        if (GetUnsigned(source_, kNkMAIDCapability_LiveViewStatus, deadline,
                "live_view_stop_failed") != kNkMAIDLiveViewStatus_OFF) {
            throw TransportError("live_view_stop_failed", "D810 did not leave live view mode");
        }
        live_view_started_ = false;
    }

    void Close(std::chrono::seconds timeout) {
        if (!claimed_) return;
        std::optional<TransportError> pending_error;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (source_.opened && live_view_session_ && live_view_started_ && !live_view_stop_attempted_) {
            try {
                live_view_stop_attempted_ = true;
                SetUnsigned(source_, kNkMAIDCapability_LiveViewStatus,
                    kNkMAIDLiveViewStatus_OFF, deadline, "live_view_stop_failed");
                if (GetUnsigned(source_, kNkMAIDCapability_LiveViewStatus, deadline,
                        "live_view_stop_failed") != kNkMAIDLiveViewStatus_OFF) {
                    throw TransportError("live_view_stop_failed", "D810 did not leave live view mode before close");
                }
                live_view_started_ = false;
            } catch (const TransportError& error) {
                pending_error.emplace(error.Category(), error.what());
            }
        }
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
        trace_.sdk_session_closed = true;
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

    // Refuses to start new SDK work on a session an abandoned async command
    // left in an unknown state (see AbandonPending): the SDK may still write
    // into a buffer this transport already released, so a second UAF window
    // must not be opened by issuing more commands.
    //
    // Deliberately NOT folded into RequireOpen(): RequireOpen() also gates
    // teardown paths like StopLiveView, and those must still be allowed to
    // run on a poisoned session. Otherwise a poisoned live view can never be
    // stopped or closed, which latches the caller's continuous-live-view
    // recovery logic into a permanently "unsafe" state and leaks its process
    // lease -- the exact failure mode fail-closed is supposed to prevent,
    // just moved from "wrote into freed memory" to "camera stuck busy
    // forever". Fail-closed means refusing new work, not refusing to stop.
    void RequireSdkSessionNotPoisoned() const {
        if (sdk_session_poisoned_) {
            throw TransportError("session_poisoned", "Nikon SDK session is poisoned after an abandoned async command");
        }
    }

    void RequireCaptureSession() const {
        RequireOpen();
        if (!capture_session_) throw TransportError("session_mode_mismatch", "Nikon session is not a capture session");
    }

    void RequireLiveViewSession() const {
        RequireOpen();
        if (!live_view_session_) throw TransportError("session_mode_mismatch", "Nikon session is not a live view session");
    }

    NKERROR Call(LPNkMAIDObject object, ULONG command, ULONG parameter, ULONG data_type,
                 NKPARAM data, LPNKFUNC completion = nullptr, NKREF reference = 0) {
        if (entry_ == nullptr) return kNkMAIDResult_MissingComponent;
        // This is the only MAID entry-point call boundary. Recording here
        // covers direct calls as well as RunCompleted and therefore cannot be
        // bypassed by a future CapStart caller. RunCompleted deliberately does
        // not record separately to avoid double counting.
        const auto event = TraceEventForCommand(command, parameter, data);
        return InvokeSdkTraceBoundary(trace_, event, [this, object, command, parameter, data_type, data, completion, reference] {
            return entry_(object, command, parameter, data_type, data, completion, reference);
        });
    }

    static bool IsPhotographicSettingCapability(ULONG capability) {
        switch (capability) {
        case kNkMAIDCapability_FileType:
        case kNkMAIDCapability_CompressionLevel:
        case kNkMAIDCapability_ImageSize:
        case kNkMAIDCapability_ExposureMode:
        case kNkMAIDCapability_ShutterSpeed:
        case kNkMAIDCapability_Aperture:
        case kNkMAIDCapability_Sensitivity:
        case kNkMAIDCapability_WBMode:
        case kNkMAIDCapability_FocusMode:
            return true;
        default:
            return false;
        }
    }

    static bool IsControlPlaneCapability(ULONG capability) {
        switch (capability) {
        case kNkMAIDCapability_ModuleMode:
        case kNkMAIDCapability_ProgressProc:
        case kNkMAIDCapability_EventProc:
        case kNkMAIDCapability_UIRequestProc:
            return true;
        default:
            return false;
        }
    }

    SdkTraceEvent TraceEventForCommand(ULONG command, ULONG parameter, NKPARAM data) noexcept {
        SdkTraceEvent event;
        if (command == kNkMAIDCommand_CapGet) {
            event.command = SdkTraceCommand::capability_get;
            return event;
        }
        if (command == kNkMAIDCommand_CapGetArray) {
            event.command = SdkTraceCommand::capability_get_array;
            return event;
        }
        if (command == kNkMAIDCommand_CapStart) {
            event.command = SdkTraceCommand::capability_start;
            if (parameter == kNkMAIDCapability_Capture) {
                event.capability = SdkTraceCapability::capture_start;
            } else if (parameter == kNkMAIDCapability_Acquire) {
                event.capability = SdkTraceCapability::non_capture_start;
            } else {
                event.capability = SdkTraceCapability::unknown_start;
            }
            return event;
        }
        if (command != kNkMAIDCommand_CapSet) return event;

        event.command = SdkTraceCommand::capability_set;
        if (IsPhotographicSettingCapability(parameter)) {
            event.capability = SdkTraceCapability::photographic_setting;
        } else if (IsControlPlaneCapability(parameter)) {
            event.capability = SdkTraceCapability::control_plane;
        } else if (parameter == kNkMAIDCapability_SaveMedia) {
            event.capability = SdkTraceCapability::storage_routing;
        } else if (parameter == kNkMAIDCapability_LiveViewStatus) {
            event.capability = SdkTraceCapability::live_view_control;
            event.live_view_on = data == static_cast<NKPARAM>(kNkMAIDLiveViewStatus_ON);
        } else {
            event.capability = SdkTraceCapability::other;
        }
        return event;
    }

    void Pump(MaidObject& object, std::string_view category) {
        const NKERROR result = Call(&object.value, kNkMAIDCommand_Async, 0, kNkMAIDDataType_Null, 0);
        if (result != kNkMAIDResult_NoError && result != kNkMAIDResult_Pending) {
            throw TransportError(std::string(category), "SDK async failed: " + ResultText(result));
        }
    }

    // Abandons a pending async command: issues Abort exactly once.
    //
    // `async_started` distinguishes two very different situations that both
    // reach this function:
    //
    //  - true (the initial Call() returned NoError or Pending, i.e. the SDK
    //    accepted the command and CompletionProc is genuinely wired up to
    //    fire later): pumps for up to kAbortGraceIterations steps waiting
    //    for `state->done`, and quarantines the session (see
    //    RequireSdkSessionNotPoisoned) if completion evidence never arrives
    //    -- the buffer handed to the SDK for this command cannot be safely
    //    released in that case (stage 2 tracks such buffers so they can be
    //    kept alive instead of freed; for now this only stops the session
    //    from accepting further commands).
    //  - false (the initial Call() was rejected synchronously, e.g. an
    //    unsupported setting during status probing): no async operation was
    //    ever queued, so CompletionProc can never fire. Polling for
    //    `state->done` here would only spend the grace period on a signal
    //    that was never coming, and treating "no evidence" as "still in
    //    flight" would quarantine the session on every routine synchronous
    //    rejection. Abort is still fired defensively in case the SDK's own
    //    "rejected" reporting is imprecise, but that alone must not
    //    quarantine the session.
    void AbandonPending(MaidObject& object, CompletionState* state, std::string_view category,
                        bool async_started) {
        if (!async_started) {
            try {
                Call(&object.value, kNkMAIDCommand_Abort, 0, kNkMAIDDataType_Null, 0);
            } catch (...) {
            }
            return;
        }
        const AbandonOutcome outcome = AbandonPendingCommand(
            [this, &object]() {
                Call(&object.value, kNkMAIDCommand_Abort, 0, kNkMAIDDataType_Null, 0);
            },
            [this, &object, category]() { Pump(object, category); },
            [state]() { return state->done.load(std::memory_order_acquire); },
            [] { std::this_thread::sleep_for(kAsyncInterval); },
            kAbortGraceIterations);
        if (outcome == AbandonOutcome::quarantined) {
            sdk_session_poisoned_ = true;
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
            // The SDK rejected the command synchronously: no async operation
            // was ever queued for it, so nothing could still write into the
            // caller's buffer later. Fire Abort defensively but do not
            // derive quarantine from missing completion evidence that was
            // never going to arrive (see AbandonPending).
            AbandonPending(object, state, category, /*async_started=*/false);
            throw TransportError(std::string(category), "SDK command failed: " + ResultText(immediate));
        }
        while (!state->done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                AbandonPending(object, state, category, /*async_started=*/true);
                throw TransportError(std::string(category), "SDK command timed out");
            }
            try {
                Pump(object, category);
            } catch (...) {
                // Pump() failing does not mean the async command itself
                // completed or was cancelled; still abandon it before
                // propagating the pump failure. Catch broadly (not just
                // TransportError): Pump()'s own message construction can
                // throw std::bad_alloc/std::length_error, and that must not
                // skip Abort either.
                AbandonPending(object, state, category, /*async_started=*/true);
                throw;
            }
            std::this_thread::sleep_for(kAsyncInterval);
        }
        if (state->result != kNkMAIDResult_NoError) {
            throw TransportError(std::string(category), "SDK completion failed: " + ResultText(state->result));
        }
        return immediate;
    }

    void EnumerateCapabilities(MaidObject& object, std::chrono::steady_clock::time_point deadline,
                               std::string_view category) {
        // The retry protocol here is: GetCapCount tells us how big a buffer
        // to allocate, then GetCapInfo either fills it or reports
        // kNkMAIDResult_BufferSize (the count grew between the two calls) and
        // we loop back to re-fetch the count. That is only legitimate if the
        // re-fetched count actually changes; a device/driver that keeps
        // reporting the same count while GetCapInfo keeps demanding a bigger
        // buffer for that same count is stuck, not making progress. Bound the
        // loop on that structural signal, not only on the wall-clock
        // deadline below (this also bounds how many entries this call can
        // add to completions_, which is otherwise unbounded per iteration).
        std::optional<ULONG> previous_count;
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw TransportError(std::string(category), "SDK capability enumeration timed out");
            }
            ULONG count = 0;
            RunCompleted(object, kNkMAIDCommand_GetCapCount, 0, kNkMAIDDataType_UnsignedPtr,
                reinterpret_cast<NKPARAM>(&count), deadline, category);
            if (count > kMaximumNikonCapabilityCount) {
                throw TransportError(std::string(category), "SDK reported an implausible capability count");
            }
            if (previous_count.has_value() && count == *previous_count) {
                throw TransportError(std::string(category),
                    "SDK capability count did not change after a buffer-size retry");
            }
            previous_count = count;
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

    std::string GetRequiredString(
        MaidObject& object,
        ULONG id,
        std::chrono::steady_clock::time_point deadline,
        std::string_view category) {
        const auto* cap = Capability(object, id);
        if (cap == nullptr || cap->ulType != kNkMAIDCapType_String ||
            !Supports(object, id, kNkMAIDCapOperation_Get)) {
            throw TransportError(std::string(category), "required SDK identity string is unavailable");
        }
        NkMAIDString value{};
        RunCompleted(object, kNkMAIDCommand_CapGet, id, kNkMAIDDataType_StringPtr,
            reinterpret_cast<NKPARAM>(&value), deadline, category);
        const char* const begin = reinterpret_cast<const char*>(value.str);
        const char* const end = begin + sizeof(value.str);
        const char* const terminator = std::find(begin, end, '\0');
        if (terminator == begin || terminator == end) {
            throw TransportError(std::string(category), "SDK identity string is empty or unterminated");
        }
        return std::string(begin, static_cast<std::size_t>(terminator - begin));
    }

    std::string StableIdentity(
        MaidObject& source,
        std::chrono::steady_clock::time_point deadline,
        std::string_view category) {
        try {
            return DeriveNikonSdkStableIdentity(
                GetRequiredString(source, kNkMAIDCapability_Name, deadline, category),
                GetRequiredString(source, kNkMAIDCapability_Interface, deadline, category));
        } catch (const std::invalid_argument&) {
            throw TransportError(std::string(category), "SDK source identity material is invalid");
        }
    }

    SdkCameraStatus ProbeOpenCaptureSessionStatus(std::chrono::seconds timeout) {
        return ReadOpenCaptureSessionStatus(timeout);
    }

    void RequireExactlyOneD810ForProductAgent() noexcept {
        require_exactly_one_d810_ = true;
    }

    SdkCameraStatus ReadOpenCaptureSessionStatus(std::chrono::seconds timeout) {
        RequireCaptureSession();
        RequireSdkSessionNotPoisoned();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        SdkCameraStatus status;
        status.firmware = Firmware(source_, deadline, "sdk_status_failed");

        if (const auto value = TryGetCurrentValue(
                source_, kNkMAIDCapability_LiveViewStatus, deadline, "sdk_status_failed")) {
            status.live_view_status_available = true;
            if (*value == kNkMAIDLiveViewStatus_OFF) status.live_view_status = "off";
            else if (*value == kNkMAIDLiveViewStatus_ON) status.live_view_status = "on";
        }
        if (const auto value = TryGetCurrentValue(
                source_, kNkMAIDCapability_LiveViewSelector, deadline, "sdk_status_failed")) {
            status.live_view_selector_available = true;
            if (*value == kNkMAIDLiveViewSelector_Photo) status.live_view_selector = "photo";
            else if (*value == kNkMAIDLiveViewSelector_Movie) status.live_view_selector = "movie";
        }
        if (const auto value = TryGetCurrentValue(
                source_, kNkMAIDCapability_LiveViewProhibit, deadline, "sdk_status_failed")) {
            status.live_view_prohibit_mask = static_cast<std::uint32_t>(*value);
        }
        status.file_type = ReadSettingCapability(source_, kNkMAIDCapability_FileType, deadline);
        status.compression_level = ReadSettingCapability(source_, kNkMAIDCapability_CompressionLevel, deadline);
        status.image_size = ReadSettingCapability(source_, kNkMAIDCapability_ImageSize, deadline);
        status.exposure_mode = ReadSettingCapability(source_, kNkMAIDCapability_ExposureMode, deadline);
        status.shutter_speed = ReadSettingCapability(source_, kNkMAIDCapability_ShutterSpeed, deadline);
        status.aperture = ReadSettingCapability(source_, kNkMAIDCapability_Aperture, deadline);
        status.sensitivity = ReadSettingCapability(source_, kNkMAIDCapability_Sensitivity, deadline);
        status.wb_mode = ReadSettingCapability(source_, kNkMAIDCapability_WBMode, deadline);
        status.focus_mode = ReadSettingCapability(source_, kNkMAIDCapability_FocusMode, deadline);
        status.command_trace = trace_;
        return status;
    }

    std::optional<ULONG> TryGetCurrentValue(
        MaidObject& object,
        ULONG id,
        std::chrono::steady_clock::time_point deadline,
        std::string_view category) {
        const auto* cap = Capability(object, id);
        if (cap == nullptr || !Supports(object, id, kNkMAIDCapOperation_Get)) {
            return std::nullopt;
        }
        if (cap->ulType == kNkMAIDCapType_Unsigned) {
            return GetUnsigned(object, id, deadline, category);
        }
        if (cap->ulType != kNkMAIDCapType_Enum ||
            !Supports(object, id, kNkMAIDCapOperation_GetArray)) {
            return std::nullopt;
        }

        NkMAIDEnum values{};
        RunCompleted(object, kNkMAIDCommand_CapGet, id,
            kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, category);
        constexpr ULONG kMaximumStatusEnumElements = 256;
        if (values.ulElements == 0 || values.ulElements > kMaximumStatusEnumElements ||
            values.wPhysicalBytes != static_cast<SWORD>(sizeof(ULONG))) {
            throw TransportError(std::string(category), "SDK status enum has an invalid shape");
        }
        std::vector<ULONG> items(values.ulElements);
        values.pData = items.data();
        RunCompleted(object, kNkMAIDCommand_CapGetArray, id,
            kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, category);
        if (values.ulValue >= items.size()) {
            throw TransportError(std::string(category), "SDK status enum has an invalid current index");
        }
        return items[values.ulValue];
    }

    SdkCameraStatus::SettingCapability ReadSettingCapability(
        MaidObject& object,
        ULONG id,
        std::chrono::steady_clock::time_point deadline) {
        SdkCameraStatus::SettingCapability result;
        const auto* cap = Capability(object, id);
        if (cap == nullptr) return result;
        if (cap->ulType == kNkMAIDCapType_Unsigned) result.cap_type = "unsigned";
        else if (cap->ulType == kNkMAIDCapType_Enum) result.cap_type = "enum";
        else {
            result.probe_state = "unsupported-type";
            return result;
        }
        if (!Supports(object, id, kNkMAIDCapOperation_Get)) {
            result.probe_state = "get-not-supported";
            return result;
        }
        try {
            if (cap->ulType == kNkMAIDCapType_Unsigned) {
                result.available = true;
                result.probe_state = "available";
                result.value_type = "unsigned";
                result.current_value = static_cast<std::uint32_t>(
                    GetUnsigned(object, id, deadline, "sdk_status_failed"));
                return result;
            }
            if (!Supports(object, id, kNkMAIDCapOperation_GetArray)) {
                result.probe_state = "get-array-not-supported";
                return result;
            }

            NkMAIDEnum values{};
            RunCompleted(object, kNkMAIDCommand_CapGet, id,
                kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, "sdk_status_failed");
            constexpr ULONG kMaximumStatusEnumElements = 256;
            // Bound untrusted packed SDK data to 64 KiB to avoid a malformed
            // device response forcing an unbounded allocation during status read.
            constexpr ULONG kMaximumPackedStringBytes = 64U * 1024U;
            if (values.ulElements == 0) {
                result.probe_state = "invalid-shape";
                return result;
            }
            if (values.ulType == kNkMAIDArrayType_Unsigned) {
                if (values.ulElements > kMaximumStatusEnumElements ||
                    values.wPhysicalBytes != static_cast<SWORD>(sizeof(ULONG))) {
                    result.probe_state = "invalid-shape";
                    return result;
                }
                std::vector<ULONG> items(values.ulElements);
                values.pData = items.data();
                RunCompleted(object, kNkMAIDCommand_CapGetArray, id,
                    kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, "sdk_status_failed");
                if (values.ulValue >= items.size()) {
                    result.probe_state = "invalid-shape";
                    return result;
                }
                result.available = true;
                result.probe_state = "available";
                result.value_type = "unsigned";
                result.current_index = static_cast<std::uint32_t>(values.ulValue);
                result.current_value = static_cast<std::uint32_t>(items[values.ulValue]);
                result.numeric_values.reserve(items.size());
                for (const ULONG item : items) result.numeric_values.push_back(static_cast<std::uint32_t>(item));
                return result;
            }
            if (values.ulType == kNkMAIDArrayType_PackedString) {
                if (values.wPhysicalBytes != 1 || values.ulElements > kMaximumPackedStringBytes) {
                    result.probe_state = "invalid-shape";
                    return result;
                }
                std::vector<unsigned char> bytes(values.ulElements);
                values.pData = bytes.data();
                RunCompleted(object, kNkMAIDCommand_CapGetArray, id,
                    kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, "sdk_status_failed");
                const auto labels = ParsePackedStringLabels(
                    bytes, kMaximumStatusEnumElements, kMaximumPackedStringBytes);
                if (!labels) {
                    result = {};
                    result.cap_type = "enum";
                    result.probe_state = "invalid-shape";
                    return result;
                }
                result.string_values = *labels;
                if (values.ulValue >= result.string_values.size()) {
                    result = {};
                    result.cap_type = "enum";
                    result.probe_state = "invalid-shape";
                    return result;
                }
                result.available = true;
                result.probe_state = "available";
                result.value_type = "packed-string";
                result.current_index = static_cast<std::uint32_t>(values.ulValue);
                result.current_label = result.string_values[values.ulValue];
                return result;
            }
            if (values.ulType != kNkMAIDArrayType_String || values.ulElements > kMaximumStatusEnumElements ||
                values.wPhysicalBytes != static_cast<SWORD>(sizeof(NkMAIDString))) {
                result.probe_state = "invalid-shape";
                return result;
            }
            std::vector<NkMAIDString> items(values.ulElements);
            values.pData = items.data();
            RunCompleted(object, kNkMAIDCommand_CapGetArray, id,
                kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, "sdk_status_failed");
            for (const NkMAIDString& item : items) {
                const char* const begin = reinterpret_cast<const char*>(item.str);
                const char* const end = begin + sizeof(item.str);
                const char* const terminator = std::find(begin, end, '\0');
                if (terminator == end) {
                    result = {};
                    result.cap_type = "enum";
                    result.probe_state = "invalid-shape";
                    return result;
                }
                result.string_values.emplace_back(begin, static_cast<std::size_t>(terminator - begin));
            }
            if (values.ulValue >= result.string_values.size()) {
                result = {};
                result.cap_type = "enum";
                result.probe_state = "invalid-shape";
                return result;
            }
            result.available = true;
            result.probe_state = "available";
            result.value_type = "string";
            result.current_index = static_cast<std::uint32_t>(values.ulValue);
            result.current_label = result.string_values[values.ulValue];
        } catch (...) {
            // Status probing is fail-closed per setting and keeps error details
            // out of anonymous evidence.
            result = {};
            if (cap->ulType == kNkMAIDCapType_Unsigned) result.cap_type = "unsigned";
            else if (cap->ulType == kNkMAIDCapType_Enum) result.cap_type = "enum";
            result.probe_state = "read-error";
        }
        return result;
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
        if (values.ulElements > kMaximumNikonEnumElements) {
            throw TransportError(std::string(category), "SDK reported an implausible child ID count");
        }
        if (values.wPhysicalBytes != static_cast<SWORD>(sizeof(ULONG))) {
            throw TransportError(std::string(category), "SDK child IDs have an unexpected width");
        }
        std::vector<ULONG> ids(values.ulElements);
        values.pData = ids.data();
        RunCompleted(object, kNkMAIDCommand_CapGetArray, kNkMAIDCapability_Children,
            kNkMAIDDataType_EnumPtr, reinterpret_cast<NKPARAM>(&values), deadline, category);
        return ids;
    }

    std::string Firmware(
        MaidObject& source,
        std::chrono::steady_clock::time_point deadline,
        std::string_view category) {
        const auto* cap = Capability(source, kNkMAIDCapability_Firmware);
        if (cap == nullptr || !Supports(source, kNkMAIDCapability_Firmware, kNkMAIDCapOperation_Get)) return "unknown";
        if (cap->ulType == kNkMAIDCapType_String) {
            NkMAIDString value{};
            RunCompleted(source, kNkMAIDCommand_CapGet, kNkMAIDCapability_Firmware,
                kNkMAIDDataType_StringPtr, reinterpret_cast<NKPARAM>(&value), deadline, category);
            const char* const begin = reinterpret_cast<const char*>(value.str);
            const char* const end = begin + sizeof(value.str);
            const char* const terminator = std::find(begin, end, '\0');
            if (terminator == begin || terminator == end) {
                return "unknown";
            }
            return std::string(begin, static_cast<std::size_t>(terminator - begin));
        }
        if (cap->ulType == kNkMAIDCapType_Unsigned) {
            std::ostringstream text;
            text << "0x" << std::hex << std::uppercase
                 << GetUnsigned(source, kNkMAIDCapability_Firmware, deadline, category);
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
            if (values.ulElements == 0 || values.ulElements > kMaximumNikonEnumElements ||
                values.wPhysicalBytes != static_cast<SWORD>(sizeof(ULONG))) return "unknown";
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
        std::set<std::string> identities;
        for (const ULONG id : WaitForSourceIds(deadline, "inventory_failed")) {
            MaidObject source;
            OpenChild(module_, source, id, "inventory_failed");
            try {
                EnumerateCapabilities(source, deadline, "inventory_failed");
                const ULONG type = GetUnsigned(source, kNkMAIDCapability_CameraType, deadline, "inventory_failed");
                if (type == kNkMAIDCameraType_D810) {
                    const auto identity = StableIdentity(source, deadline, "inventory_failed");
                    if (!identities.insert(identity).second) {
                        throw TransportError("identity_collision", "multiple D810 sources reported the same SDK identity");
                    }
                    cameras.push_back({"Nikon D810", Firmware(source, deadline, "inventory_failed"), ShootingMode(source, deadline), identity});
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
            card_capture_events_.SessionClosed();
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
        capture_session_ = false;
        live_view_session_ = false;
        live_view_started_ = false;
        live_view_stop_attempted_ = false;
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
        if (source_.opened && live_view_session_ && live_view_started_ && !live_view_stop_attempted_) {
            try {
                live_view_stop_attempted_ = true;
                SetUnsigned(source_, kNkMAIDCapability_LiveViewStatus,
                    kNkMAIDLiveViewStatus_OFF,
                    std::chrono::steady_clock::now() + std::chrono::seconds(2),
                    "live_view_stop_failed");
            } catch (...) {
                // Emergency cleanup must still close the SDK object and release the process lock.
            }
            live_view_started_ = false;
        }
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
        card_capture_events_.SessionClosed();
        CloseObjectNoThrow(module_);
        UnloadModule();
        session_open_ = false;
        capture_session_ = false;
        live_view_session_ = false;
        live_view_started_ = false;
        live_view_stop_attempted_ = false;
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
            self->card_capture_events_.Observe(NikonCardCaptureEvent::capture_complete);
            break;
        case kNkMAIDEvent_AddChildInCard:
            self->add_child_in_card_ = true;
            self->card_capture_events_.Observe(NikonCardCaptureEvent::add_child_in_card);
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
    // Set when an abandoned async command that was confirmed to have
    // actually started never produced completion evidence within the abort
    // grace period (see AbandonPending). Checked by
    // RequireSdkSessionNotPoisoned() to stop issuing new SDK work into a
    // session the SDK may still be writing into; deliberately not checked by
    // teardown paths (StopLiveView, Close), which must remain reachable even
    // on a poisoned session. Reset when a fresh session is opened
    // (OpenSource()) so it never outlives the session it describes.
    bool sdk_session_poisoned_{false};
    bool capture_session_{false};
    bool live_view_session_{false};
    bool live_view_started_{false};
    bool live_view_stop_attempted_{false};
    bool capture_complete_{false};
    bool add_child_in_card_{false};
    NikonCardCaptureEventWindow card_capture_events_;
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
    bool require_exactly_one_d810_{};
    SdkCommandTrace trace_;

    friend class NikonSdkTransport;
};

NikonSdkTransport::NikonSdkTransport() : impl_(std::make_unique<Impl>()) {}
NikonSdkTransport::~NikonSdkTransport() = default;
std::string NikonSdkTransport::SdkVersion() const { return impl_->SdkVersion(); }
std::vector<CameraInfo> NikonSdkTransport::Enumerate() { return impl_->Enumerate(); }
SdkCameraStatus NikonSdkTransport::ProbeSdkStatus(
    std::string_view stable_identity,
    std::chrono::seconds timeout) {
    return impl_->ProbeSdkStatus(stable_identity, timeout);
}
SdkCameraStatus NikonSdkTransport::ProbeOpenCaptureSessionStatus(
    std::chrono::seconds timeout) {
    return impl_->ProbeOpenCaptureSessionStatus(timeout);
}
void NikonSdkTransport::RequireExactlyOneD810ForProductAgent() {
    impl_->RequireExactlyOneD810ForProductAgent();
}
void NikonSdkTransport::Open(std::string_view stable_identity, std::chrono::seconds timeout) {
    impl_->Open(stable_identity, timeout);
}
void NikonSdkTransport::OpenLiveView(std::string_view stable_identity, std::chrono::seconds timeout) {
    impl_->OpenLiveView(stable_identity, timeout);
}
void NikonSdkTransport::StartLiveView(std::chrono::seconds timeout) { impl_->StartLiveView(timeout); }
std::vector<unsigned char> NikonSdkTransport::ReadLiveViewFrame(std::chrono::seconds timeout) {
    return impl_->ReadLiveViewFrame(timeout);
}
void NikonSdkTransport::StopLiveView(std::chrono::seconds timeout) { impl_->StopLiveView(timeout); }
std::string NikonSdkTransport::Baseline(std::chrono::seconds timeout) { return impl_->Baseline(timeout); }
std::vector<ImageCandidate> NikonSdkTransport::CaptureAndDownload(
    std::string_view baseline,
    std::chrono::seconds image_event_timeout,
    std::chrono::seconds download_timeout,
    std::chrono::seconds transaction_timeout) {
    return impl_->CaptureAndDownload(baseline, image_event_timeout, download_timeout, transaction_timeout);
}
void NikonSdkTransport::CaptureToCard(std::chrono::seconds image_event_timeout,
                                      std::chrono::seconds transaction_timeout) {
    impl_->CaptureToCard(image_event_timeout, transaction_timeout);
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
SdkCameraStatus NikonSdkTransport::ProbeSdkStatus(std::string_view, std::chrono::seconds) { ThrowGated(); }
SdkCameraStatus NikonSdkTransport::ProbeOpenCaptureSessionStatus(std::chrono::seconds) { ThrowGated(); }
void NikonSdkTransport::RequireExactlyOneD810ForProductAgent() {}
void NikonSdkTransport::Open(std::string_view, std::chrono::seconds) { ThrowGated(); }
void NikonSdkTransport::OpenLiveView(std::string_view, std::chrono::seconds) { ThrowGated(); }
void NikonSdkTransport::StartLiveView(std::chrono::seconds) { ThrowGated(); }
std::vector<unsigned char> NikonSdkTransport::ReadLiveViewFrame(std::chrono::seconds) { ThrowGated(); }
void NikonSdkTransport::StopLiveView(std::chrono::seconds) { ThrowGated(); }
std::string NikonSdkTransport::Baseline(std::chrono::seconds) { ThrowGated(); }
std::vector<ImageCandidate> NikonSdkTransport::CaptureAndDownload(
    std::string_view, std::chrono::seconds, std::chrono::seconds, std::chrono::seconds) { ThrowGated(); }
void NikonSdkTransport::CaptureToCard(std::chrono::seconds, std::chrono::seconds) { ThrowGated(); }
void NikonSdkTransport::Close(std::chrono::seconds) { ThrowGated(); }
bool NikonSdkTransport::LicensedAdapterAvailable() noexcept { return false; }

#endif

NikonSdkStatusExecutor::NikonSdkStatusExecutor() = default;
NikonSdkStatusExecutor::~NikonSdkStatusExecutor() = default;
std::string NikonSdkStatusExecutor::SdkVersion() const { return transport_.SdkVersion(); }
std::vector<CameraInfo> NikonSdkStatusExecutor::Enumerate() { return transport_.Enumerate(); }
void NikonSdkStatusExecutor::RequireExactlyOneD810ForSingleStatus() {
    transport_.RequireExactlyOneD810ForProductAgent();
}
SdkCameraStatus NikonSdkStatusExecutor::ProbeSdkStatus(
    std::string_view stable_identity,
    std::chrono::seconds timeout) {
    return transport_.ProbeSdkStatus(stable_identity, timeout);
}

} // namespace a0::phase0
