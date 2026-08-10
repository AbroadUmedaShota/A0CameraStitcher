#include "a0/phase0/phase0.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace a0::phase0 {

void RecordSdkTraceEvent(SdkCommandTrace& trace, const SdkTraceEvent& event) noexcept {
    switch (event.command) {
    case SdkTraceCommand::capability_get:
        ++trace.cap_get_count;
        return;
    case SdkTraceCommand::capability_get_array:
        ++trace.cap_get_array_count;
        return;
    case SdkTraceCommand::capability_start:
        ++trace.cap_start_count;
        switch (event.capability) {
        case SdkTraceCapability::capture_start:
            ++trace.capture_start_count;
            break;
        case SdkTraceCapability::non_capture_start:
            ++trace.non_capture_start_count;
            break;
        case SdkTraceCapability::unknown_start:
            ++trace.unknown_cap_start_count;
            break;
        default:
            ++trace.unknown_cap_start_count;
            break;
        }
        return;
    case SdkTraceCommand::capability_set:
        ++trace.cap_set_count;
        switch (event.capability) {
        case SdkTraceCapability::photographic_setting:
            ++trace.photographic_setting_cap_set_count;
            break;
        case SdkTraceCapability::control_plane:
            ++trace.control_plane_cap_set_count;
            break;
        case SdkTraceCapability::storage_routing:
            ++trace.storage_routing_cap_set_count;
            break;
        case SdkTraceCapability::live_view_control:
            ++trace.live_view_control_cap_set_count;
            if (event.live_view_on) ++trace.live_view_start_count;
            break;
        default:
            ++trace.unexpected_cap_set_count;
            break;
        }
        return;
    case SdkTraceCommand::other:
        return;
    }
}

std::optional<std::string_view> ValidateSdkReadOnlyCommandTrace(
    const SdkCommandTrace& trace) noexcept {
    if (!trace.sdk_session_opened) return "session_not_opened";
    if (!trace.sdk_session_closed) return "session_not_closed";
    if (trace.cap_get_count == 0 && trace.cap_get_array_count == 0) {
        return "no_capability_reads";
    }
    if (trace.photographic_setting_cap_set_count != 0) {
        return "photographic_setting_cap_set";
    }
    if (trace.live_view_start_count != 0) return "live_view_started";
    if (trace.storage_routing_cap_set_count != 0) {
        return "storage_routing_cap_set";
    }
    if (trace.live_view_control_cap_set_count != 0) {
        return "live_view_control_cap_set";
    }
    if (trace.unexpected_cap_set_count != 0) return "unexpected_cap_set";

    const std::size_t classified_cap_starts = trace.capture_start_count +
        trace.non_capture_start_count + trace.unknown_cap_start_count;
    if (classified_cap_starts != trace.cap_start_count) {
        return "cap_start_count_inconsistent";
    }
    if (trace.capture_start_count != 0) return "capture_started";
    if (trace.unknown_cap_start_count != 0) return "unknown_cap_start";
    if (trace.non_capture_start_count != 0) return "non_capture_cap_start";

    std::size_t unclassified_cap_sets = trace.cap_set_count;
    const auto consume_cap_set_count = [&unclassified_cap_sets](std::size_t count) {
        if (count > unclassified_cap_sets) return false;
        unclassified_cap_sets -= count;
        return true;
    };
    if (!consume_cap_set_count(trace.control_plane_cap_set_count) ||
        !consume_cap_set_count(trace.photographic_setting_cap_set_count) ||
        !consume_cap_set_count(trace.storage_routing_cap_set_count) ||
        !consume_cap_set_count(trace.live_view_control_cap_set_count) ||
        !consume_cap_set_count(trace.unexpected_cap_set_count) ||
        unclassified_cap_sets != 0) {
        return "trace_count_inconsistent";
    }
    return std::nullopt;
}

std::optional<std::string_view> ValidateSdkStatusProcessRouting(
    const SdkStatusProcessRouting& routing) noexcept {
    if (!routing.sdk_status_executor_selected) return "sdk_status_executor_not_selected";
    if (routing.sdk_enumeration_count != 1) return "sdk_enumeration_count_inconsistent";
    if (routing.sdk_status_probe_count != 1) return "sdk_status_probe_count_inconsistent";
    if (routing.wpd_identity_enumeration_count > 1) return "wpd_identity_enumeration_count_inconsistent";
    if (routing.single_identity_v3_selected != (routing.wpd_identity_enumeration_count == 1)) {
        return "single_identity_v3_routing_inconsistent";
    }
    if (routing.wpd_call_count != 0) return "wpd_call_routed";
    if (routing.capture_call_count != 0) return "capture_call_routed";
    if (routing.delete_call_count != 0) return "delete_call_routed";
    return std::nullopt;
}

namespace {

std::string JsonEscape(std::string_view value) {
    std::ostringstream stream;
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': stream << "\\\\"; break;
        case '"': stream << "\\\""; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (ch < 0x20U || ch >= 0x7FU) {
                stream << "\\u00" << kHex[(ch >> 4U) & 0x0FU] << kHex[ch & 0x0FU];
            } else {
                stream << static_cast<char>(ch);
            }
        }
    }
    return stream.str();
}

std::string ControlledErrorDetail(std::string_view value) {
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 512));
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) continue;
        if (result.size() == 512) break;
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::string JsonUnescape(std::string_view value) {
    std::string result;
    const auto hex_value = [](char character) -> unsigned int {
        if (character >= '0' && character <= '9') return static_cast<unsigned int>(character - '0');
        if (character >= 'A' && character <= 'F') return static_cast<unsigned int>(character - 'A' + 10);
        if (character >= 'a' && character <= 'f') return static_cast<unsigned int>(character - 'a' + 10);
        throw std::runtime_error("local camera map contains an invalid JSON escape");
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (character != '\\') {
            result.push_back(character);
            continue;
        }
        if (++index >= value.size()) {
            throw std::runtime_error("local camera map contains a truncated JSON escape");
        }
        switch (value[index]) {
        case '\\': result.push_back('\\'); break;
        case '"': result.push_back('"'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
            if (index + 4 >= value.size()) {
                throw std::runtime_error("local camera map contains a truncated Unicode escape");
            }
            unsigned int code_unit = 0;
            for (int digit = 0; digit < 4; ++digit) {
                code_unit = (code_unit << 4U) | hex_value(value[++index]);
            }
            if (code_unit > 0xFFU) {
                throw std::runtime_error("local camera map contains an unsupported Unicode escape");
            }
            result.push_back(static_cast<char>(code_unit));
            break;
        }
        default:
            throw std::runtime_error("local camera map contains an unsupported JSON escape");
        }
    }
    return result;
}

std::string NowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << millis.count() << 'Z';
    return stream.str();
}

std::string SanitizeFileName(std::string_view value) {
    std::string result;
    for (const char ch : value) {
        result.push_back(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ? ch : '_');
    }
    return result.empty() ? "candidate" : result;
}

std::optional<std::string> EnvironmentValue(const char* name) {
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return std::nullopt;
    std::string value(buffer);
    std::free(buffer);
    return value;
}

bool IsReparsePoint(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void PrepareReparseFreeEvidenceDirectory(
    const fs::path& trusted_root,
    const fs::path& directory) {
    const fs::path absolute_root = fs::absolute(trusted_root).lexically_normal();
    const fs::path absolute_directory = fs::absolute(directory).lexically_normal();
    const fs::path relative = absolute_directory.lexically_relative(absolute_root);
    if ((!relative.empty() && std::any_of(
            relative.begin(), relative.end(), [](const fs::path& component) {
                return component == "..";
            })) || absolute_root.root_path() != absolute_directory.root_path()) {
        throw std::runtime_error("evidence directory escaped its trusted run root");
    }
    const auto validate_chain = [&](const fs::path& target) {
        fs::path current = target.root_path();
        const fs::path from_volume = target.lexically_relative(current);
        for (const auto& component : from_volume) {
            current /= component;
            if (IsReparsePoint(current)) {
                throw std::runtime_error(
                    "evidence directory path contains a reparse point");
            }
        }
    };
    validate_chain(absolute_root);
    validate_chain(absolute_directory);
    fs::create_directories(absolute_directory);
    validate_chain(absolute_directory);
    std::error_code type_error;
    if (!fs::is_directory(absolute_directory, type_error) || type_error) {
        throw std::runtime_error("evidence directory is not a local directory");
    }
}

void WriteBytesExclusive(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::error_code parent_error;
    if (!fs::is_directory(path.parent_path(), parent_error) || parent_error) {
        throw std::runtime_error("evidence parent directory was not prepared");
    }
    HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "cannot exclusively create evidence file");
    }
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) ||
                written != chunk) {
                throw std::system_error(
                    static_cast<int>(GetLastError()),
                    std::system_category(),
                    "cannot write evidence file");
            }
            offset += written;
        }
        if (!FlushFileBuffers(handle)) {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "cannot durably flush evidence file");
        }
        if (!CloseHandle(handle)) {
            handle = INVALID_HANDLE_VALUE;
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "cannot close evidence file");
        }
        handle = INVALID_HANDLE_VALUE;
    } catch (...) {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        throw;
    }
}

class ActiveGuard {
public:
    explicit ActiveGuard(bool& active) : active_(active) {
        if (active_) throw std::runtime_error("another capture transaction is active");
        active_ = true;
    }
    ~ActiveGuard() { active_ = false; }
    ActiveGuard(const ActiveGuard&) = delete;
    ActiveGuard& operator=(const ActiveGuard&) = delete;
private:
    bool& active_;
};

} // namespace

TransportError::TransportError(std::string category, std::string message)
    : std::runtime_error(std::move(message)), category_(std::move(category)) {}

const std::string& TransportError::Category() const noexcept { return category_; }

UncertainDispatchError::UncertainDispatchError(
    std::string category, std::string message, std::vector<ImageCandidate> candidates)
    : std::runtime_error(std::move(message)), category_(std::move(category)), candidates_(std::move(candidates)) {}
const std::string& UncertainDispatchError::Category() const noexcept { return category_; }
const std::vector<ImageCandidate>& UncertainDispatchError::Candidates() const noexcept { return candidates_; }

OperatorGate::OperatorGate(
    fs::path artifacts_root,
    std::string safe_name,
    std::chrono::seconds timeout,
    std::string scenario,
    std::string stage)
    : gate_directory_(std::move(artifacts_root) / "operator-gates" / safe_name),
      ready_path_(gate_directory_ / "ready.json"),
      continue_path_(gate_directory_ / "continue"),
      safe_name_(std::move(safe_name)),
      scenario_(std::move(scenario)),
      stage_(std::move(stage)),
      timeout_(timeout) {
    if (!IsSafeName(safe_name_)) {
        throw TransportError("operator_gate_invalid_name", "operator gate name must match [A-Za-z0-9_-] and be 1-64 characters");
    }
    if (timeout_ <= std::chrono::seconds::zero() || timeout_ > std::chrono::hours(1)) {
        throw TransportError("operator_gate_invalid_timeout", "operator gate timeout must be between 1 and 3600 seconds");
    }
    if ((!scenario_.empty() && !IsSafeName(scenario_)) || (!stage_.empty() && !IsSafeName(stage_))) {
        throw TransportError("operator_gate_invalid_metadata", "operator gate scenario and stage must use safe names");
    }
}

bool OperatorGate::IsSafeName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
}

const fs::path& OperatorGate::ReadyPath() const noexcept { return ready_path_; }

void OperatorGate::PublishReady(std::ostream& output, std::string_view instruction) {
    if (armed_) throw TransportError("operator_gate_reused", "operator gate cannot be reused");
    armed_ = true;
    std::error_code error;
    fs::create_directories(gate_directory_.parent_path(), error);
    if (error) throw TransportError("operator_gate_create_failed", "cannot create operator gate parent directory");
    if (!fs::create_directory(gate_directory_, error) || error) {
        throw TransportError("operator_gate_exists", "operator gate directory already exists");
    }
    const fs::path partial = gate_directory_ / "ready.json.partial";
    std::ofstream ready(partial, std::ios::binary | std::ios::out);
    if (!ready) throw TransportError("operator_gate_create_failed", "cannot create operator gate ready artifact");
    ready << "{\n"
          << "  \"schemaVersion\": \"phase0.operator-gate.v2\",\n"
          << "  \"gateName\": \"" << safe_name_ << "\",\n";
    if (!scenario_.empty()) ready << "  \"scenario\": \"" << scenario_ << "\",\n";
    if (!stage_.empty()) ready << "  \"stage\": \"" << stage_ << "\",\n";
    ready << "  \"status\": \"ready\"\n"
          << "}\n";
    ready.close();
    if (!ready) throw TransportError("operator_gate_create_failed", "cannot persist operator gate ready artifact");
    fs::rename(partial, ready_path_, error);
    if (error) throw TransportError("operator_gate_create_failed", "cannot atomically publish operator gate ready artifact");

    output << "OPERATOR GATE READY: " << instruction << '\n' << std::flush;
}

fs::path OperatorGate::AwaitContinue(std::ostream& output) {
    PublishReady(output, "create marker 'continue' at " + continue_path_.string());
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    std::error_code error;
    while (std::chrono::steady_clock::now() < deadline) {
        error.clear();
        const bool marker_exists = fs::exists(continue_path_, error);
        if (error) throw TransportError("operator_gate_wait_failed", "cannot inspect operator gate continue marker");
        if (marker_exists) {
            const bool marker_is_file = fs::is_regular_file(continue_path_, error);
            if (error) throw TransportError("operator_gate_wait_failed", "cannot inspect operator gate continue marker");
            if (marker_is_file) return continue_path_;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw TransportError("operator_gate_timeout", "operator gate continue marker was not received before timeout");
}

[[noreturn]] void OperatorGate::AwaitProcessTermination(std::ostream& output) {
    PublishReady(
        output,
        "terminate this Phase 0 process now; do not create a continue marker; CAM-B must not start");
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    std::error_code error;
    while (std::chrono::steady_clock::now() < deadline) {
        error.clear();
        if (fs::exists(continue_path_, error)) {
            throw TransportError(
                "operator_interruption_continue_marker",
                "continue marker is prohibited for a process-termination gate");
        }
        if (error) throw TransportError("operator_gate_wait_failed", "cannot inspect operator gate directory");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw TransportError("operator_gate_timeout", "process-termination gate timed out; CAM-B remains blocked");
}

IdentityMap::IdentityMap(fs::path path) : path_(std::move(path)) { Load(); }

IdentityMap::IdentityMap(
    fs::path path,
    std::optional<std::string> cam_a,
    std::optional<std::string> cam_b)
    : path_(std::move(path)), cam_a_(std::move(cam_a)), cam_b_(std::move(cam_b)) {}

void IdentityMap::Load() {
    if (!fs::exists(path_)) return;
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("cannot read local camera map");
    const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::regex a_pattern("\\\"CAM-A\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
    const std::regex b_pattern("\\\"CAM-B\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
    std::smatch match;
    if (std::regex_search(body, match, a_pattern)) cam_a_ = JsonUnescape(match[1].str());
    if (std::regex_search(body, match, b_pattern)) cam_b_ = JsonUnescape(match[1].str());
}

void IdentityMap::Save() const {
    fs::create_directories(path_.parent_path());
    const fs::path partial = path_.string() + ".partial";
    if (fs::exists(partial)) throw std::runtime_error("camera map partial exists; inspect before retrying");
    std::ofstream output(partial);
    if (!output) throw std::runtime_error("cannot write local camera map");
    output << "{\n  \"CAM-A\": " << (cam_a_ ? "\"" + JsonEscape(*cam_a_) + "\"" : "null")
           << ",\n  \"CAM-B\": " << (cam_b_ ? "\"" + JsonEscape(*cam_b_) + "\"" : "null") << "\n}\n";
    output.flush();
    if (!output) throw std::runtime_error("cannot flush local camera map");
    output.close();
    if (!fs::exists(path_)) { fs::rename(partial, path_); return; }
    const fs::path backup = path_.string() + ".previous";
    if (fs::exists(backup)) throw std::runtime_error("camera map backup exists; inspect before retrying");
    fs::rename(path_, backup);
    try {
        fs::rename(partial, path_);
        fs::remove(backup);
    } catch (...) {
        if (!fs::exists(path_) && fs::exists(backup)) fs::rename(backup, path_);
        throw;
    }
}

std::optional<std::string> IdentityMap::FindAlias(std::string_view stable_identity) const {
    if (cam_a_ && *cam_a_ == stable_identity) return "CAM-A";
    if (cam_b_ && *cam_b_ == stable_identity) return "CAM-B";
    return std::nullopt;
}

void IdentityMap::ValidateBinding(std::string_view alias, std::string_view stable_identity) const {
    if (alias != "CAM-A" && alias != "CAM-B") {
        throw std::runtime_error("camera alias must be CAM-A or CAM-B");
    }
    if (stable_identity.empty()) {
        throw std::runtime_error("camera identity must not be empty");
    }

    if (const auto existing_alias = FindAlias(stable_identity)) {
        if (*existing_alias == alias) return;
        throw std::runtime_error("camera identity is already bound to a different alias");
    }

    const auto& target = alias == "CAM-A" ? cam_a_ : cam_b_;
    if (target) {
        throw std::runtime_error("camera alias is already bound; manual review is required");
    }
}

void IdentityMap::Bind(std::string_view alias, std::string_view stable_identity) {
    ValidateBinding(alias, stable_identity);
    if (const auto existing_alias = FindAlias(stable_identity); existing_alias && *existing_alias == alias) {
        return;
    }
    auto& target = alias == "CAM-A" ? cam_a_ : cam_b_;
    target = std::string(stable_identity);
    Save();
}

const fs::path& IdentityMap::Path() const noexcept { return path_; }

AnonymousInventorySummary SummarizeInventoryReadOnly(
    const IdentityMap& map,
    const std::vector<CameraInfo>& cameras) {
    AnonymousInventorySummary summary;
    summary.cameras.reserve(cameras.size());
    for (const auto& camera : cameras) {
        AnonymousInventoryEntry entry;
        if (const auto alias = map.FindAlias(camera.stable_identity)) {
            entry.alias = *alias;
            ++summary.bound_camera_count;
        } else {
            ++summary.unbound_camera_count;
        }
        entry.model = camera.model;
        entry.firmware = camera.firmware;
        entry.shooting_mode = camera.shooting_mode;
        summary.cameras.push_back(std::move(entry));
    }
    return summary;
}

CameraInfo SelectSingleCameraForBinding(const std::vector<CameraInfo>& cameras) {
    if (cameras.size() != 1) {
        throw std::runtime_error("identity binding requires exactly one physically connected D810");
    }
    if (cameras.front().stable_identity.empty()) {
        throw std::runtime_error("enumerated camera identity is empty");
    }
    return cameras.front();
}

CrossTransportBindingSelection BindCrossTransportIdentity(
    IdentityMap& sdk_map,
    IdentityMap& wpd_map,
    std::string_view alias,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras) {
    if (sdk_map.Path() == wpd_map.Path()) {
        throw std::runtime_error("SDK and WPD identity maps must use distinct paths");
    }
    CrossTransportBindingSelection selection{
        SelectSingleCameraForBinding(sdk_cameras),
        SelectSingleCameraForBinding(wpd_cameras)};
    sdk_map.ValidateBinding(alias, selection.sdk_camera.stable_identity);
    wpd_map.ValidateBinding(alias, selection.wpd_camera.stable_identity);
    sdk_map.Bind(alias, selection.sdk_camera.stable_identity);
    wpd_map.Bind(alias, selection.wpd_camera.stable_identity);
    return selection;
}

DualIdentityVerificationSummary VerifyDualIdentityBindings(
    const IdentityMap& sdk_map,
    const IdentityMap& wpd_map,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras) {
    DualIdentityVerificationSummary result;
    result.sdk_camera_count = sdk_cameras.size();
    result.wpd_camera_count = wpd_cameras.size();
    const auto count_aliases = [](const IdentityMap& map, const std::vector<CameraInfo>& cameras,
                                  std::size_t& cam_a, std::size_t& cam_b, std::size_t& unbound) {
        for (const auto& camera : cameras) {
            const auto alias = map.FindAlias(camera.stable_identity);
            if (!alias) ++unbound;
            else if (*alias == "CAM-A") ++cam_a;
            else if (*alias == "CAM-B") ++cam_b;
        }
    };
    count_aliases(sdk_map, sdk_cameras,
        result.sdk_cam_a_count, result.sdk_cam_b_count, result.sdk_unbound_count);
    count_aliases(wpd_map, wpd_cameras,
        result.wpd_cam_a_count, result.wpd_cam_b_count, result.wpd_unbound_count);

    if (result.sdk_camera_count != 2 || result.wpd_camera_count != 2) {
        result.failure_category = "camera_count_mismatch";
    } else if (result.sdk_unbound_count != 0 || result.wpd_unbound_count != 0) {
        result.failure_category = "unbound_identity";
    } else if (result.sdk_cam_a_count != 1 || result.sdk_cam_b_count != 1 ||
               result.wpd_cam_a_count != 1 || result.wpd_cam_b_count != 1) {
        result.failure_category = "alias_cardinality_mismatch";
    } else {
        result.terminal_state = "Ready";
    }
    return result;
}

fs::path PersistDualIdentityVerificationSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    const DualIdentityVerificationSummary& summary) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    fs::create_directories(run_root);
    const fs::path final = run_root / "dual-identity-verification-summary.json";
    const fs::path partial = run_root / "dual-identity-verification-summary.json.partial";
    if (fs::exists(final) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite dual identity verification evidence");
    }
    std::ofstream output(partial, std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create dual identity verification evidence");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.dual-identity-verification-summary.v1\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"sdkCameraCount\": " << summary.sdk_camera_count << ",\n"
           << "  \"sdkCamACount\": " << summary.sdk_cam_a_count << ",\n"
           << "  \"sdkCamBCount\": " << summary.sdk_cam_b_count << ",\n"
           << "  \"sdkUnboundCount\": " << summary.sdk_unbound_count << ",\n"
           << "  \"wpdCameraCount\": " << summary.wpd_camera_count << ",\n"
           << "  \"wpdCamACount\": " << summary.wpd_cam_a_count << ",\n"
           << "  \"wpdCamBCount\": " << summary.wpd_cam_b_count << ",\n"
           << "  \"wpdUnboundCount\": " << summary.wpd_unbound_count << ",\n"
           << "  \"identityMapsChanged\": " << (summary.identity_maps_changed ? "true" : "false") << ",\n"
           << "  \"captureCommandSent\": " << (summary.capture_command_sent ? "true" : "false") << ",\n"
           << "  \"liveViewStarted\": " << (summary.live_view_started ? "true" : "false") << ",\n"
           << "  \"cameraSettingsChanged\": " << (summary.camera_settings_changed ? "true" : "false") << ",\n"
           << "  \"cardAccessPerformed\": " << (summary.card_access_performed ? "true" : "false") << ",\n"
           << "  \"realIdentifiersIncluded\": " << (summary.real_identifiers_included ? "true" : "false") << ",\n"
           << "  \"terminalState\": \"" << JsonEscape(summary.terminal_state) << "\",\n"
           << "  \"failureCategory\": \"" << JsonEscape(summary.failure_category) << "\"\n"
           << "}\n";
    output.flush();
    output.close();
    if (!output) throw std::runtime_error("cannot persist dual identity verification evidence");
    fs::rename(partial, final);
    return final;
}

DualSpoolVerificationSummary PrepareDualSpoolVerification(
    const DualIdentityVerificationSummary& identity) {
    DualSpoolVerificationSummary result;
    result.identity = identity;
    if (identity.terminal_state != "Ready") {
        result.failure_category = "dual_identity_not_ready";
        return result;
    }
    result.terminal_state = "ReadyForInspection";
    return result;
}

void FinalizeDualSpoolVerification(
    DualSpoolVerificationSummary& summary,
    std::size_t cam_a_payload_object_count,
    std::size_t cam_b_payload_object_count) {
    if (summary.terminal_state != "ReadyForInspection") {
        throw std::runtime_error("dual spool verification cannot inspect before dual identity is ready");
    }
    summary.cam_a_payload_object_count = cam_a_payload_object_count;
    summary.cam_b_payload_object_count = cam_b_payload_object_count;
    summary.wpd_sessions_closed = 2;
    summary.card_inspection_performed = true;
    if (cam_a_payload_object_count == 0 && cam_b_payload_object_count == 0) {
        summary.terminal_state = "Ready";
        summary.failure_category.clear();
    } else {
        summary.terminal_state = "Blocked";
        summary.failure_category = "spool_not_empty";
    }
}

fs::path PersistDualSpoolVerificationSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    const DualSpoolVerificationSummary& summary) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    fs::create_directories(run_root);
    const fs::path final = run_root / "dual-spool-verification-summary.json";
    const fs::path partial = run_root / "dual-spool-verification-summary.json.partial";
    if (fs::exists(final) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite dual spool verification evidence");
    }
    std::ofstream output(partial, std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create dual spool verification evidence");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.dual-spool-verification-summary.v1\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"sdkCameraCount\": " << summary.identity.sdk_camera_count << ",\n"
           << "  \"sdkCamACount\": " << summary.identity.sdk_cam_a_count << ",\n"
           << "  \"sdkCamBCount\": " << summary.identity.sdk_cam_b_count << ",\n"
           << "  \"sdkUnboundCount\": " << summary.identity.sdk_unbound_count << ",\n"
           << "  \"wpdCameraCount\": " << summary.identity.wpd_camera_count << ",\n"
           << "  \"wpdCamACount\": " << summary.identity.wpd_cam_a_count << ",\n"
           << "  \"wpdCamBCount\": " << summary.identity.wpd_cam_b_count << ",\n"
           << "  \"wpdUnboundCount\": " << summary.identity.wpd_unbound_count << ",\n"
           << "  \"camAPayloadObjectCount\": " << summary.cam_a_payload_object_count << ",\n"
           << "  \"camBPayloadObjectCount\": " << summary.cam_b_payload_object_count << ",\n"
           << "  \"wpdSessionsClosed\": " << summary.wpd_sessions_closed << ",\n"
           << "  \"readOnlyObservation\": " << (summary.read_only_observation ? "true" : "false") << ",\n"
           << "  \"cardInspectionPerformed\": " << (summary.card_inspection_performed ? "true" : "false") << ",\n"
           << "  \"captureCommandSent\": " << (summary.capture_command_sent ? "true" : "false") << ",\n"
           << "  \"cameraDeleteAttempted\": " << (summary.camera_delete_attempted ? "true" : "false") << ",\n"
           << "  \"vendorOperationExecuted\": " << (summary.vendor_operation_executed ? "true" : "false") << ",\n"
           << "  \"automaticRetry\": " << (summary.automatic_retry ? "true" : "false") << ",\n"
           << "  \"realIdentifiersIncluded\": " << (summary.real_identifiers_included ? "true" : "false") << ",\n"
           << "  \"terminalState\": \"" << JsonEscape(summary.terminal_state) << "\",\n"
           << "  \"failureCategory\": \"" << JsonEscape(summary.failure_category) << "\"\n"
           << "}\n";
    output.flush();
    output.close();
    if (!output) throw std::runtime_error("cannot persist dual spool verification evidence");
    fs::rename(partial, final);
    return final;
}

EvidenceWriter::EvidenceWriter(fs::path artifacts_root, std::string run_id, std::string sdk_version)
    : artifacts_root_(std::move(artifacts_root)), run_root_(artifacts_root_ / run_id),
      run_id_(std::move(run_id)), sdk_version_(std::move(sdk_version)) {
    PrepareReparseFreeEvidenceDirectory(run_root_, run_root_);
}

void EvidenceWriter::AppendEvent(std::string_view json_line) {
    std::ofstream output(run_root_ / "events.jsonl", std::ios::app);
    if (!output) throw std::runtime_error("cannot append Phase 0 event log");
    output << json_line << '\n';
}

void EvidenceWriter::RecordState(std::string_view transaction_id, std::string_view state, std::string_view camera_alias,
                                 std::string_view error_detail) {
    std::ostringstream event;
    event << "{\"timestamp\":\"" << NowIso8601() << "\",\"runId\":\"" << JsonEscape(run_id_)
          << "\",\"transactionId\":\"" << JsonEscape(transaction_id) << "\",\"state\":\"" << JsonEscape(state) << "\"";
    if (!camera_alias.empty()) event << ",\"cameraAlias\":\"" << JsonEscape(camera_alias) << "\"";
    if (!error_detail.empty()) event << ",\"errorDetail\":\"" << JsonEscape(error_detail) << "\"";
    event << '}';
    AppendEvent(event.str());
}

void EvidenceWriter::RecordCamera(std::string_view camera_alias, std::string_view firmware) {
    std::ostringstream event;
    event << "{\"timestamp\":\"" << NowIso8601() << "\",\"runId\":\"" << JsonEscape(run_id_)
          << "\",\"state\":\"CameraProfile\",\"cameraAlias\":\"" << JsonEscape(camera_alias)
          << "\",\"model\":\"Nikon D810\",\"firmware\":\"" << JsonEscape(firmware) << "\"}";
    AppendEvent(event.str());
}

FrameEvidence EvidenceWriter::PersistExactlyOne(std::string_view transaction_id, std::string_view camera_alias,
                                                 const std::vector<ImageCandidate>& candidates,
                                                 std::optional<std::chrono::steady_clock::time_point> transaction_deadline,
                                                 const std::function<void()>& before_atomic_rename) {
    FrameEvidence frame;
    frame.camera_alias = std::string(camera_alias);
    const auto deadline_expired = [&] {
        return transaction_deadline && std::chrono::steady_clock::now() >= *transaction_deadline;
    };
    const auto watchdog_failure = [&](const fs::path& diagnostic_path) {
        frame.path = diagnostic_path;
        frame.error_category = "transaction_watchdog";
        frame.error_detail = "capture transaction watchdog expired before canonical PC original completion";
        RecordState(transaction_id, "PcOriginalPersistWatchdogExpired", camera_alias, frame.error_detail);
        return frame;
    };
    if (candidates.size() != 1 || !candidates.front().attributable || !IsValidJpeg(candidates.front().bytes)) {
        frame.error_category = candidates.empty() ? "no_candidate" :
            (candidates.size() > 1 ? "ambiguous_candidates" :
                (!candidates.front().attributable ? "late_candidate" : "invalid_jpeg"));
        const fs::path quarantine = run_root_ / "quarantine" /
            std::string(transaction_id) / std::string(camera_alias);
        if (!candidates.empty()) {
            PrepareReparseFreeEvidenceDirectory(run_root_, quarantine);
        }
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            WriteBytesExclusive(quarantine / (std::to_string(index) + "_" + SanitizeFileName(candidate.source_name) + ".bin"), candidate.bytes);
        }
        RecordState(transaction_id, "Quarantined", camera_alias);
        return frame;
    }
    if (deadline_expired()) {
        const fs::path quarantine = run_root_ / "quarantine" /
            std::string(transaction_id) / std::string(camera_alias);
        PrepareReparseFreeEvidenceDirectory(run_root_, quarantine);
        WriteBytesExclusive(quarantine / ("watchdog_" + SanitizeFileName(candidates.front().source_name) + ".bin"),
            candidates.front().bytes);
        return watchdog_failure(quarantine);
    }
    const fs::path directory = run_root_ / std::string(transaction_id) / std::string(camera_alias);
    const fs::path partial = directory / "original.jpg.partial";
    const fs::path final = directory / "original.jpg";
    frame.bytes = candidates.front().bytes.size();
    frame.sha256 = Sha256Hex(candidates.front().bytes);
    PrepareReparseFreeEvidenceDirectory(run_root_, directory);
    WriteBytesExclusive(partial, candidates.front().bytes);
    if (deadline_expired()) return watchdog_failure(partial);
    if (fs::exists(final)) throw std::runtime_error("refusing to overwrite an original JPEG");
    if (before_atomic_rename) before_atomic_rename();
    if (deadline_expired()) return watchdog_failure(partial);
    if (!MoveFileExW(partial.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "cannot atomically publish durable original JPEG");
    }
    frame.path = final;
    if (deadline_expired()) return watchdog_failure(final);
    std::error_code size_error;
    const auto persisted_size = fs::file_size(final, size_error);
    std::ifstream persisted_stream(final, std::ios::binary);
    const std::vector<unsigned char> persisted_bytes{
        std::istreambuf_iterator<char>(persisted_stream),
        std::istreambuf_iterator<char>()};
    if (size_error || persisted_stream.bad() || persisted_size != frame.bytes ||
        persisted_bytes.size() != frame.bytes || !IsValidJpeg(persisted_bytes) ||
        Sha256Hex(persisted_bytes) != frame.sha256) {
        frame.error_category = "pc_original_verification_failed";
        RecordState(transaction_id, "PcOriginalVerificationFailed", camera_alias);
        return frame;
    }
    if (deadline_expired()) return watchdog_failure(final);
    frame.success = true;
    std::ostringstream event;
    event << "{\"timestamp\":\"" << NowIso8601() << "\",\"runId\":\"" << JsonEscape(run_id_)
          << "\",\"transactionId\":\"" << JsonEscape(transaction_id) << "\",\"state\":\"Persisted\",\"cameraAlias\":\""
          << JsonEscape(camera_alias) << "\",\"bytes\":" << frame.bytes << ",\"sha256\":\"" << frame.sha256 << "\"}";
    AppendEvent(event.str());
    return frame;
}

FrameEvidence EvidenceWriter::QuarantineUnconfirmed(
    std::string_view transaction_id, std::string_view camera_alias,
    const std::vector<ImageCandidate>& candidates, std::string_view command_detail) {
    FrameEvidence frame;
    frame.camera_alias = std::string(camera_alias);
    const fs::path quarantine = run_root_ / "quarantine" /
        std::string(transaction_id) / std::string(camera_alias);
    if (!candidates.empty()) {
        PrepareReparseFreeEvidenceDirectory(run_root_, quarantine);
    }
    frame.path = quarantine;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        const auto bytes = candidate.bytes.size();
        const auto sha256 = Sha256Hex(candidate.bytes);
        WriteBytesExclusive(quarantine / (std::to_string(index) + "_" + SanitizeFileName(candidate.source_name) + ".bin"), candidate.bytes);
        std::ostringstream event;
        event << "{\"timestamp\":\"" << NowIso8601() << "\",\"runId\":\"" << JsonEscape(run_id_)
              << "\",\"transactionId\":\"" << JsonEscape(transaction_id)
              << "\",\"state\":\"UnconfirmedCandidateQuarantined\",\"cameraAlias\":\""
              << JsonEscape(camera_alias) << "\",\"candidateIndex\":" << index
              << ",\"bytes\":" << bytes << ",\"sha256\":\"" << sha256 << "\"}";
        AppendEvent(event.str());
        if (candidates.size() == 1) {
            frame.bytes = bytes;
            frame.sha256 = sha256;
        }
    }
    RecordState(transaction_id, "UnconfirmedDispatchQuarantined", camera_alias, command_detail);
    return frame;
}

void EvidenceWriter::RecordResult(const TransactionResult& result) {
    ++transaction_count_;
    result.terminal_state == "Complete" ? ++complete_count_ : ++failed_count_;
    std::ostringstream event;
    event << "{\"timestamp\":\"" << NowIso8601() << "\",\"runId\":\"" << JsonEscape(run_id_)
          << "\",\"transactionId\":\"" << JsonEscape(result.transaction_id) << "\",\"state\":\""
          << JsonEscape(result.terminal_state) << "\",\"durationMs\":" << result.duration.count();
    if (!result.error_category.empty()) event << ",\"errorCategory\":\"" << JsonEscape(result.error_category) << "\"";
    if (!result.error_detail.empty()) event << ",\"errorDetail\":\"" << JsonEscape(result.error_detail) << "\"";
    event << '}';
    AppendEvent(event.str());
    std::ofstream summary(run_root_ / "summary.json", std::ios::trunc);
    if (!summary) throw std::runtime_error("cannot write Phase 0 summary");
    summary << "{\n  \"schemaVersion\": \"phase0.summary.v1\",\n  \"runId\": \"" << JsonEscape(run_id_)
            << "\",\n  \"cameraModel\": \"Nikon D810\",\n  \"sdkVersion\": \"" << JsonEscape(sdk_version_)
            << "\",\n  \"transactionCount\": " << transaction_count_ << ",\n  \"completeCount\": " << complete_count_
            << ",\n  \"failedCount\": " << failed_count_ << "\n}\n";
}

const std::string& EvidenceWriter::RunId() const noexcept { return run_id_; }
const fs::path& EvidenceWriter::RunRoot() const noexcept { return run_root_; }

void EvidenceWriter::GenerateRedactedReport(const fs::path& report_root) const {
    const std::array<std::pair<fs::path, fs::path>, 15> candidates{{
        {run_root_ / "summary.json", "summary.json"},
        {run_root_ / "sdk-status-summary.json", "sdk-status-summary.json"},
        {run_root_ / "wpd-status-summary.json", "wpd-status-summary.json"},
        {run_root_ / "wpd-spool-status-summary.json", "wpd-spool-status-summary.json"},
        {run_root_ / "wpd-correlation-summary.json", "wpd-correlation-summary.json"},
        {run_root_ / "events.jsonl", "transaction-events.jsonl"},
        {run_root_ / "handoff-summary.json", "handoff-summary.json"},
        {run_root_ / "live-view-summary.json", "live-view-summary.json"},
        {run_root_ / "hybrid-capture-summary.json", "hybrid-capture-summary.json"},
        {run_root_ / "hybrid-pair-summary.json", "hybrid-pair-summary.json"},
        {run_root_ / "hybrid-pair-recovery-summary.json", "hybrid-pair-recovery-summary.json"},
        {run_root_ / "hybrid-fault-summary.json", "hybrid-fault-summary.json"},
        {run_root_ / "hybrid-pair-fault-summary.json", "hybrid-pair-fault-summary.json"},
        {run_root_ / "dual-identity-verification-summary.json", "dual-identity-verification-summary.json"},
        {run_root_ / "dual-spool-verification-summary.json", "dual-spool-verification-summary.json"},
    }};
    std::vector<std::pair<fs::path, fs::path>> included;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate.first)) included.push_back(candidate);
    }
    const bool has_summary = std::ranges::any_of(included, [](const auto& item) {
        return item.second.filename() != "transaction-events.jsonl";
    });
    if (!has_summary) throw std::runtime_error("run evidence does not contain an anonymous summary");

    const fs::path destination = report_root / run_id_;
    const fs::path partial = report_root / (run_id_ + ".partial");
    if (fs::exists(destination) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite redacted report");
    }
    fs::create_directories(report_root);
    try {
        if (!fs::create_directory(partial)) {
            throw std::runtime_error("cannot create redacted report partial directory");
        }
        const fs::path report_target = partial / "report.md";
        for (const auto& item : included) {
            fs::copy_file(item.first, partial / item.second);
        }
        std::ofstream report(report_target);
        if (!report) throw std::runtime_error("cannot write redacted report");
        report << "# Phase 0 report: " << run_id_ << "\n\n- Camera model: Nikon D810\n"
               << "- Anonymous run summaries: see the included `*-summary.json` or `summary.json` files\n"
               << "- Redacted camera profiles, transaction IDs, aliases, state timestamps, sizes, hashes, and error classes, when available: see `transaction-events.jsonl`\n"
               << "- Raw images and unrestricted diagnostics: local ignored artifacts only\n"
               << "- Real camera identifiers: excluded\n";
        report.close();
        if (!report) throw std::runtime_error("cannot finalize redacted report");
        fs::rename(partial, destination);
    } catch (...) {
        std::error_code cleanup_error;
        fs::remove_all(partial, cleanup_error);
        throw;
    }
}

fs::path PersistHybridCaptureSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const HybridCaptureRunSummary& result) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path output = run_root / "hybrid-capture-summary.json";
    const fs::path partial = run_root / "hybrid-capture-summary.json.partial";
    fs::create_directories(run_root);
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write hybrid capture summary");
    stream << "{\n  \"schemaVersion\": \"phase0.hybrid-capture-summary.v3\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"cameraAlias\": \"" << JsonEscape(camera_alias) << "\",\n"
           << "  \"requested\": " << result.requested << ",\n"
           << "  \"attempted\": " << result.attempted << ",\n"
           << "  \"completed\": " << result.completed << ",\n"
           << "  \"failures\": " << result.failures << ",\n"
           << "  \"terminalState\": \"" << JsonEscape(result.terminal_state) << "\",\n"
           << "  \"lastState\": \"" << JsonEscape(result.last_state) << "\",\n"
           << "  \"pc_original_canonical\": " << (result.pc_original_canonical ? "true" : "false") << ",\n"
           << "  \"camera_card_transient\": " << (result.camera_card_transient ? "true" : "false") << ",\n"
           << "  \"spoolEmptyBeforeCount\": " << result.spool_empty_before_count << ",\n"
           << "  \"cameraCardDeleteAttemptedCount\": " << result.camera_card_delete_attempted_count << ",\n"
           << "  \"cameraCardDeleteSucceededCount\": " << result.camera_card_delete_succeeded_count << ",\n"
           << "  \"spoolEmptyAfterCount\": " << result.spool_empty_after_count << ",\n"
           << "  \"automatic_retry\": " << (result.automatic_retry ? "true" : "false") << ",\n"
           << "  \"exclusive_camera_control_confirmed\": "
           << (result.exclusive_camera_control_confirmed ? "true" : "false") << ",\n"
           << "  \"dedicatedSpoolScopeConfirmed\": "
           << (result.dedicated_spool_scope_confirmed ? "true" : "false") << ",\n"
           << "  \"exactObjectDeleteConfirmed\": "
           << (result.exact_object_delete_confirmed ? "true" : "false") << ",\n"
           << "  \"cleanupObjectIdIncluded\": false,\n"
           << "  \"vendorOperationExecuted\": false\n}\n";
    stream.close();
    if (!stream) throw std::runtime_error("cannot finalize hybrid capture summary");
    fs::rename(partial, output);
    return output;
}

fs::path PersistHybridPairSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    const HybridPairRunSummary& result) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path output = run_root / "hybrid-pair-summary.json";
    const fs::path partial = run_root / "hybrid-pair-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(output) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite a hybrid pair summary");
    }
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write hybrid pair summary");
    stream << "{\n  \"schemaVersion\": \"phase0.hybrid-pair-summary.v2\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"requestedPairs\": " << result.requested_pairs << ",\n"
           << "  \"attemptedPairs\": " << result.attempted_pairs << ",\n"
           << "  \"completedPairs\": " << result.completed_pairs << ",\n"
           << "  \"failures\": " << result.failures << ",\n"
           << "  \"camACompletedCount\": " << result.cam_a_completed_count << ",\n"
           << "  \"camBCompletedCount\": " << result.cam_b_completed_count << ",\n"
           << "  \"attemptedCameraTransactions\": " << result.attempted_camera_transactions << ",\n"
           << "  \"completedCameraTransactions\": " << result.completed_camera_transactions << ",\n"
           << "  \"spoolEmptyBeforeCount\": " << result.spool_empty_before_count << ",\n"
           << "  \"cameraCardDeleteAttemptedCount\": " << result.camera_card_delete_attempted_count << ",\n"
           << "  \"cameraCardDeleteSucceededCount\": " << result.camera_card_delete_succeeded_count << ",\n"
           << "  \"spoolEmptyAfterCount\": " << result.spool_empty_after_count << ",\n"
           << "  \"durationSampleCount\": " << result.duration_sample_count << ",\n"
           << "  \"pairDurationP50Ms\": " << result.pair_duration_p50_ms << ",\n"
           << "  \"pairDurationP95Ms\": " << result.pair_duration_p95_ms << ",\n"
           << "  \"pairDurationMaxMs\": " << result.pair_duration_max_ms << ",\n"
           << "  \"timingUsedForPhase0PassFail\": false,\n"
           << "  \"terminalState\": \"" << JsonEscape(result.terminal_state) << "\",\n"
           << "  \"lastPairState\": \"" << JsonEscape(result.last_pair_state) << "\",\n"
           << "  \"lastErrorCategory\": \"" << JsonEscape(result.last_error_category) << "\",\n"
           << "  \"lastErrorDetail\": \"" << JsonEscape(result.last_error_detail) << "\",\n"
           << "  \"captureOrder\": \"CAM-A-then-CAM-B\",\n"
           << "  \"pairWatchdogSeconds\": " << result.pair_watchdog_seconds << ",\n"
           << "  \"automaticRetry\": " << (result.automatic_retry ? "true" : "false") << ",\n"
           << "  \"exclusiveCameraControlConfirmed\": "
           << (result.exclusive_camera_control_confirmed ? "true" : "false") << ",\n"
           << "  \"dedicatedSpoolScopeConfirmed\": "
           << (result.dedicated_spool_scope_confirmed ? "true" : "false") << ",\n"
           << "  \"dualDedicatedSpoolsConfirmed\": "
           << (result.dual_dedicated_spools_confirmed ? "true" : "false") << ",\n"
           << "  \"exactObjectDeleteConfirmed\": "
           << (result.exact_object_delete_confirmed ? "true" : "false") << ",\n"
           << "  \"cameraSessionOverlapAllowed\": false,\n"
           << "  \"actualShutterSynchronizationGuaranteed\": "
           << (result.actual_shutter_synchronization_guaranteed ? "true" : "false") << ",\n"
           << "  \"realIdentifiersIncluded\": false,\n"
           << "  \"cleanupObjectIdIncluded\": false,\n"
           << "  \"vendorOperationExecuted\": false\n}\n";
    stream.close();
    if (!stream) throw std::runtime_error("cannot finalize hybrid pair summary");
    fs::rename(partial, output);
    return output;
}

HybridPairRecoveryStatus AssessHybridPairRecoveryEventLog(const fs::path& event_log) {
    HybridPairRecoveryStatus status;
    std::ifstream stream(event_log, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read hybrid pair recovery event log");

    std::string active_pair_id;
    std::string active_stage;
    std::string line;
    const auto field = [](std::string_view json, std::string_view name) -> std::string {
        const std::string prefix = "\"" + std::string(name) + "\":\"";
        const auto begin = json.find(prefix);
        if (begin == std::string_view::npos) return {};
        const auto value_begin = begin + prefix.size();
        const auto end = json.find('"', value_begin);
        if (end == std::string_view::npos) return {};
        return std::string(json.substr(value_begin, end - value_begin));
    };
    while (std::getline(stream, line)) {
        const auto state = field(line, "state");
        if (!state.starts_with("HybridPair")) continue;
        const auto pair_id = field(line, "transactionId");
        if (pair_id.empty() || !pair_id.starts_with("hybrid-pair-")) {
            status.event_sequence_consistent = false;
            continue;
        }
        if (state == "HybridPairStarted") {
            ++status.pair_started_count;
            if (!active_pair_id.empty()) status.event_sequence_consistent = false;
            active_pair_id = pair_id;
            active_stage = "CAM-A-active";
        } else if (pair_id != active_pair_id) {
            status.event_sequence_consistent = false;
        } else if (state == "HybridPairCamAComplete") {
            active_stage = "after-CAM-A-before-CAM-B";
        } else if (state == "HybridPairCamBStarting") {
            active_stage = "CAM-B-active";
        } else if (state == "HybridPairComplete") {
            ++status.pair_complete_count;
            active_pair_id.clear();
            active_stage.clear();
        } else if (state == "HybridPairFailed") {
            ++status.pair_failed_count;
            active_pair_id.clear();
            active_stage.clear();
        }
    }
    if (stream.bad()) throw std::runtime_error("cannot finish reading hybrid pair recovery event log");

    if (!active_pair_id.empty()) {
        status.interrupted_pair_detected = true;
        status.interrupted_stage = active_stage;
        status.cam_a_complete_before_interruption =
            active_stage == "after-CAM-A-before-CAM-B" || active_stage == "CAM-B-active";
    }
    if (!status.event_sequence_consistent) {
        status.terminal_state = "EvidenceInvalid";
    } else if (status.interrupted_pair_detected) {
        status.terminal_state = "Interrupted";
    } else if (status.pair_started_count == 0) {
        status.terminal_state = "NoPairEvents";
    } else {
        status.terminal_state = "Terminal";
    }
    status.recovery_requires_new_transaction =
        !status.event_sequence_consistent || status.interrupted_pair_detected || status.pair_failed_count > 0;
    return status;
}

fs::path PersistHybridPairRecoverySummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    const HybridPairRecoveryStatus& status) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path output = run_root / "hybrid-pair-recovery-summary.json";
    const fs::path partial = run_root / "hybrid-pair-recovery-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(output) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite a hybrid pair recovery summary");
    }
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write hybrid pair recovery summary");
    stream << "{\n  \"schemaVersion\": \"phase0.hybrid-pair-recovery-summary.v1\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"pairStartedCount\": " << status.pair_started_count << ",\n"
           << "  \"pairCompleteCount\": " << status.pair_complete_count << ",\n"
           << "  \"pairFailedCount\": " << status.pair_failed_count << ",\n"
           << "  \"interruptedPairDetected\": " << (status.interrupted_pair_detected ? "true" : "false") << ",\n"
           << "  \"interruptedStage\": \"" << JsonEscape(status.interrupted_stage) << "\",\n"
           << "  \"camACompleteBeforeInterruption\": "
           << (status.cam_a_complete_before_interruption ? "true" : "false") << ",\n"
           << "  \"retainCompletedOriginals\": " << (status.retain_completed_originals ? "true" : "false") << ",\n"
           << "  \"automaticRetryAllowed\": " << (status.automatic_retry_allowed ? "true" : "false") << ",\n"
           << "  \"recoveryRequiresNewTransaction\": "
           << (status.recovery_requires_new_transaction ? "true" : "false") << ",\n"
           << "  \"eventSequenceConsistent\": " << (status.event_sequence_consistent ? "true" : "false") << ",\n"
           << "  \"terminalState\": \"" << JsonEscape(status.terminal_state) << "\",\n"
           << "  \"realIdentifiersIncluded\": false\n}\n";
    stream.close();
    if (!stream) throw std::runtime_error("cannot finalize hybrid pair recovery summary");
    fs::rename(partial, output);
    return output;
}

CaptureCoordinator::CaptureCoordinator(ICameraTransport& transport, EvidenceWriter& evidence, Timeouts timeouts)
    : transport_(transport), evidence_(evidence), timeouts_(timeouts) {}

std::string CaptureCoordinator::NextTransactionId() {
    return "tx-" + std::to_string(++sequence_) + '-' + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

FrameEvidence CaptureCoordinator::CaptureOne(std::string_view transaction_id, std::string_view alias,
    std::string_view stable_identity, std::string_view capture_state, std::string_view persist_state,
    std::optional<std::chrono::steady_clock::time_point> transaction_deadline) {
    FrameEvidence frame;
    frame.camera_alias = std::string(alias);
    bool opened = false;
    const auto ensure_active = [&] {
        if (transaction_deadline && std::chrono::steady_clock::now() >= *transaction_deadline) {
            throw TransportError("transaction_watchdog", "capture transaction watchdog expired");
        }
    };
    const auto budget = [&](std::chrono::seconds configured) {
        if (!transaction_deadline) return configured;
        const auto remaining_duration = *transaction_deadline - std::chrono::steady_clock::now();
        if (remaining_duration <= std::chrono::steady_clock::duration::zero()) {
            throw TransportError("transaction_watchdog", "capture transaction watchdog expired");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(remaining_duration);
        if (remaining <= std::chrono::seconds::zero()) {
            throw TransportError("transaction_watchdog", "capture transaction watchdog expired");
        }
        return std::min(configured, remaining);
    };
    try {
        ensure_active();
        evidence_.RecordState(transaction_id, capture_state, alias);
        transport_.Open(stable_identity, budget(timeouts_.open));
        opened = true;
        ensure_active();
        const auto baseline = transport_.Baseline(budget(timeouts_.open));
        ensure_active();
        const auto capture_budget = transaction_deadline
            ? budget(timeouts_.transaction_watchdog)
            : timeouts_.image_event + timeouts_.download;
        const auto candidates = transport_.CaptureAndDownload(
            baseline, budget(timeouts_.image_event), budget(timeouts_.download), capture_budget);
        ensure_active();
        evidence_.RecordState(transaction_id, persist_state, alias);
        frame = evidence_.PersistExactlyOne(transaction_id, alias, candidates, transaction_deadline);
        ensure_active();
        opened = false;
        transport_.Close(budget(timeouts_.close));
        ensure_active();
        return frame;
    } catch (const UncertainDispatchError& error) {
        if (opened) { opened = false; try { transport_.Close(timeouts_.close); } catch (...) {} }
        frame = evidence_.QuarantineUnconfirmed(transaction_id, alias, error.Candidates(), ControlledErrorDetail(error.what()));
        frame.error_category = error.Category();
        frame.error_detail = ControlledErrorDetail(error.what());
        evidence_.RecordState(transaction_id, "UncertainDispatchFailed", alias, frame.error_detail);
        return frame;
    } catch (const TransportError& error) {
        if (opened) { opened = false; try { transport_.Close(timeouts_.close); } catch (...) {} }
        frame.error_category = error.Category();
        frame.error_detail = ControlledErrorDetail(error.what());
        evidence_.RecordState(transaction_id, "TransportError", alias, frame.error_detail);
        return frame;
    } catch (const std::exception&) {
        if (opened) { opened = false; try { transport_.Close(timeouts_.close); } catch (...) {} }
        frame.error_category = "transport_exception";
        evidence_.RecordState(transaction_id, "TransportException", alias);
        return frame;
    }
}

namespace {
bool FrameCompleted(const FrameEvidence& frame) {
    return frame.success && frame.error_category.empty();
}
} // namespace

TransactionResult CaptureCoordinator::CaptureSingle(std::string_view alias, std::string_view stable_identity) {
    ActiveGuard guard(active_);
    TransactionResult result;
    result.run_id = evidence_.RunId();
    result.transaction_id = NextTransactionId();
    const auto started = std::chrono::steady_clock::now();
    evidence_.RecordState(result.transaction_id, "Idle");
    result.frames.push_back(CaptureOne(result.transaction_id, alias, stable_identity, "CaptureA", "PersistA"));
    result.terminal_state = FrameCompleted(result.frames.front()) ? "Complete" : "FailedPartial";
    if (!FrameCompleted(result.frames.front())) result.error_category = result.frames.front().error_category;
    if (!FrameCompleted(result.frames.front())) result.error_detail = result.frames.front().error_detail;
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    evidence_.RecordResult(result);
    return result;
}

TransactionResult CaptureCoordinator::CapturePair(std::string_view cam_a_identity, std::string_view cam_b_identity) {
    ActiveGuard guard(active_);
    TransactionResult result;
    result.run_id = evidence_.RunId();
    result.transaction_id = NextTransactionId();
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeouts_.transaction_watchdog;
    evidence_.RecordState(result.transaction_id, "Idle");
    result.frames.push_back(CaptureOne(
        result.transaction_id, "CAM-A", cam_a_identity, "CaptureA", "PersistA", deadline));
    if (FrameCompleted(result.frames.back()) && std::chrono::steady_clock::now() < deadline) {
        result.frames.push_back(CaptureOne(
            result.transaction_id, "CAM-B", cam_b_identity, "CaptureB", "PersistB", deadline));
    }
    const bool watchdog_expired = std::chrono::steady_clock::now() >= deadline;
    if (result.frames.size() == 2 && FrameCompleted(result.frames[0]) && FrameCompleted(result.frames[1]) && !watchdog_expired) {
        evidence_.RecordState(result.transaction_id, "Paired");
        result.terminal_state = "Complete";
    } else {
        result.terminal_state = "FailedPartial";
        result.error_category = watchdog_expired ? "transaction_watchdog" : result.frames.back().error_category;
        result.error_detail = watchdog_expired ? ControlledErrorDetail("capture transaction watchdog expired")
            : result.frames.back().error_detail;
        if (watchdog_expired) evidence_.RecordState(result.transaction_id, "WatchdogExpired");
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    evidence_.RecordResult(result);
    return result;
}

TransactionResult ExecuteHybridCaptureOnce(
    ICameraTransport& wpd_session,
    IPostCardObservationTransport& wpd,
    ICameraTransport& sdk_session,
    ICardCaptureTransport& sdk,
    EvidenceWriter& evidence,
    std::string_view camera_alias,
    std::string_view wpd_identity,
    std::string_view sdk_identity,
    Timeouts timeouts,
    const std::function<void()>& before_wpd_recovery,
    const std::function<void()>& before_pc_original_rename,
    std::optional<std::chrono::steady_clock::time_point> transaction_deadline,
    const std::function<void(const FrameEvidence&)>& before_camera_object_delete,
    const std::function<void()>& before_sdk_capture) {
    TransactionResult result;
    result.run_id = evidence.RunId();
    result.transaction_id = "hybrid-tx-" + NewRunId().substr(4);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = transaction_deadline.value_or(started + timeouts.transaction_watchdog);
    std::string token;
    bool wpd_open = false;
    bool sdk_open = false;
    const auto fail = [&](std::string category, std::string detail) {
        result.terminal_state = "FailedPartial";
        result.error_category = std::move(category);
        result.error_detail = ControlledErrorDetail(detail);
        if (result.frames.empty()) {
            result.frames.push_back({false, std::string(camera_alias), {}, {}, 0,
                result.error_category, result.error_detail});
        }
        evidence.RecordState(result.transaction_id, "HybridFailed", camera_alias, result.error_detail);
    };
    const auto budget = [&](std::chrono::seconds configured) {
        const auto remaining_duration = deadline - std::chrono::steady_clock::now();
        if (remaining_duration <= std::chrono::steady_clock::duration::zero()) {
            throw TransportError("transaction_watchdog", "hybrid capture transaction watchdog expired");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(remaining_duration);
        if (remaining <= std::chrono::seconds::zero()) {
            throw TransportError("transaction_watchdog", "hybrid capture transaction watchdog expired");
        }
        return std::min(configured, remaining);
    };
    const auto ensure_active = [&] {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw TransportError("transaction_watchdog", "hybrid capture transaction watchdog expired");
        }
    };
    try {
        ensure_active();
        evidence.RecordState(result.transaction_id, "HybridWpdBaselineOpen", camera_alias);
        wpd_session.Open(wpd_identity, budget(timeouts.open));
        wpd_open = true;
        ensure_active();
        token = wpd.BeginPostCardObservation(budget(timeouts.open));
        ensure_active();
        result.spool_empty_before_capture = true;
        evidence.RecordState(result.transaction_id, "HybridSpoolEmptyBefore", camera_alias);
        wpd_open = false;
        wpd_session.Close(budget(timeouts.close));
        ensure_active();

        evidence.RecordState(result.transaction_id, "HybridSdkCardCapture", camera_alias);
        sdk_session.Open(sdk_identity, budget(timeouts.open));
        sdk_open = true;
        ensure_active();
        if (before_sdk_capture) {
            evidence.RecordState(
                result.transaction_id,
                "HybridSdkProfileRevalidation",
                camera_alias);
            before_sdk_capture();
            ensure_active();
        }
        sdk.CaptureToCard(
            budget(timeouts.image_event),
            budget(timeouts.image_event + timeouts.download));
        // A failed close is terminal: opening WPD afterwards could overlap a
        // still-owned SDK session.
        if (std::chrono::steady_clock::now() >= deadline) {
            sdk_open = false;
            try { sdk_session.Close(timeouts.close); } catch (...) {}
            throw TransportError("transaction_watchdog", "hybrid capture transaction watchdog expired");
        }
        sdk_open = false;
        sdk_session.Close(budget(timeouts.close));
        ensure_active();

        if (before_wpd_recovery) {
            evidence.RecordState(result.transaction_id, "HybridOperatorGateBeforeWpdRecovery", camera_alias);
            before_wpd_recovery();
            ensure_active();
            evidence.RecordState(result.transaction_id, "HybridOperatorGateContinued", camera_alias);
        }

        ensure_active();
        evidence.RecordState(result.transaction_id, "HybridWpdObserveOpen", camera_alias);
        wpd_session.Open(wpd_identity, budget(timeouts.open));
        wpd_open = true;
        ensure_active();
        const auto candidates = wpd.ObserveAndDownloadPostCardCapture(
            token,
            budget(timeouts.image_event),
            budget(timeouts.download),
            budget(timeouts.image_event + timeouts.download));
        token.clear();
        ensure_active();
        evidence.RecordState(result.transaction_id, "HybridRecoveredExactlyOneCandidate", camera_alias);
        evidence.RecordState(result.transaction_id, "HybridPersistPcOriginal", camera_alias);
        result.frames.push_back(evidence.PersistExactlyOne(
            result.transaction_id, camera_alias, candidates, deadline, before_pc_original_rename));
        ensure_active();
        if (!FrameCompleted(result.frames.front())) {
            throw TransportError(
                result.frames.front().error_category.empty()
                    ? "pc_original_persistence_failed"
                    : result.frames.front().error_category,
                "recovered JPEG did not become a verified canonical PC original");
        }
        ensure_active();
        evidence.RecordState(result.transaction_id, "HybridPcOriginalVerified", camera_alias);
        if (candidates.size() != 1 || candidates.front().cleanup_token.empty()) {
            throw TransportError(
                "cleanup_token_missing",
                "the recovered JPEG has no in-memory exact-object cleanup capability");
        }
        ensure_active();
        if (before_camera_object_delete) {
            before_camera_object_delete(result.frames.front());
            ensure_active();
        }
        result.camera_card_delete_attempted = true;
        evidence.RecordState(result.transaction_id, "HybridCameraObjectDeleteStarted", camera_alias);
        wpd.DeleteRecoveredObject(candidates.front().cleanup_token, budget(timeouts.close));
        result.camera_card_delete_succeeded = true;
        ensure_active();
        evidence.RecordState(result.transaction_id, "HybridCameraObjectDeleted", camera_alias);
        wpd.VerifyJpegSpoolEmpty(budget(timeouts.open));
        ensure_active();
        result.spool_empty_after_cleanup = true;
        evidence.RecordState(result.transaction_id, "HybridSpoolEmptyAfter", camera_alias);
        wpd_open = false;
        wpd_session.Close(budget(timeouts.close));
        ensure_active();
        result.terminal_state = "Complete";
    } catch (const TransportError& error) {
        if (sdk_open) { try { sdk_session.Close(timeouts.close); } catch (...) {} }
        if (wpd_open) { try { wpd_session.Close(timeouts.close); } catch (...) {} }
        if (!token.empty()) wpd.AbandonPostCardObservation(token);
        fail(error.Category(), error.what());
    } catch (const std::exception& error) {
        if (sdk_open) { try { sdk_session.Close(timeouts.close); } catch (...) {} }
        if (wpd_open) { try { wpd_session.Close(timeouts.close); } catch (...) {} }
        if (!token.empty()) wpd.AbandonPostCardObservation(token);
        fail("transport_exception", error.what());
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    evidence.RecordResult(result);
    return result;
}

HybridPairResult ExecuteHybridCapturePair(
    ICameraTransport& wpd_session,
    IPostCardObservationTransport& wpd,
    ICameraTransport& sdk_session,
    ICardCaptureTransport& sdk,
    EvidenceWriter& evidence,
    std::string_view cam_a_wpd_identity,
    std::string_view cam_a_sdk_identity,
    std::string_view cam_b_wpd_identity,
    std::string_view cam_b_sdk_identity,
    Timeouts timeouts,
    const std::function<void()>& before_cam_b,
    const std::function<void()>& before_cam_a_wpd_recovery,
    const std::function<void()>& before_cam_b_wpd_recovery) {
    HybridPairResult pair;
    pair.run_id = evidence.RunId();
    pair.pair_id = "hybrid-pair-" + NewRunId().substr(4);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeouts.transaction_watchdog;
    const auto finish = [&] {
        pair.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    };
    const auto fail = [&](std::string category, std::string detail, std::string_view alias) {
        pair.terminal_state = "FailedPartial";
        pair.error_category = std::move(category);
        pair.error_detail = ControlledErrorDetail(detail);
        evidence.RecordState(pair.pair_id, "HybridPairFailed", alias, pair.error_detail);
    };

    evidence.RecordState(pair.pair_id, "HybridPairStarted", "CAM-A");
    pair.cam_a = ExecuteHybridCaptureOnce(
        wpd_session, wpd, sdk_session, sdk, evidence, "CAM-A",
        cam_a_wpd_identity, cam_a_sdk_identity, timeouts,
        before_cam_a_wpd_recovery, {}, deadline);
    if (pair.cam_a.terminal_state != "Complete") {
        fail(pair.cam_a.error_category, pair.cam_a.error_detail, "CAM-A");
        finish();
        return pair;
    }
    evidence.RecordState(pair.pair_id, "HybridPairCamAComplete", "CAM-A");

    try {
        if (before_cam_b) before_cam_b();
    } catch (const std::exception& error) {
        fail("pair_boundary_exception", error.what(), "CAM-B");
        finish();
        return pair;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        fail("transaction_watchdog", "pair transaction watchdog expired before CAM-B", "CAM-B");
        finish();
        return pair;
    }

    pair.cam_b_started = true;
    evidence.RecordState(pair.pair_id, "HybridPairCamBStarting", "CAM-B");
    pair.cam_b = ExecuteHybridCaptureOnce(
        wpd_session, wpd, sdk_session, sdk, evidence, "CAM-B",
        cam_b_wpd_identity, cam_b_sdk_identity, timeouts,
        before_cam_b_wpd_recovery, {}, deadline);
    if (pair.cam_b.terminal_state != "Complete") {
        fail(pair.cam_b.error_category, pair.cam_b.error_detail, "CAM-B");
        finish();
        return pair;
    }

    pair.terminal_state = "Complete";
    evidence.RecordState(pair.pair_id, "HybridPairComplete", "CAM-B");
    finish();
    return pair;
}

HybridPairRunSummary ExecuteHybridPairRun(
    int requested_pairs,
    const std::function<HybridPairResult()>& capture_pair_once) {
    if (requested_pairs < 1) throw std::runtime_error("hybrid pair run count must be positive");
    if (!capture_pair_once) throw std::runtime_error("hybrid pair run requires a capture callback");

    HybridPairRunSummary summary;
    summary.requested_pairs = requested_pairs;
    std::vector<std::int64_t> pair_durations_ms;
    pair_durations_ms.reserve(static_cast<std::size_t>(requested_pairs));
    const auto accumulate_transaction = [&](const TransactionResult& transaction, std::string_view alias) {
        if (transaction.transaction_id.empty()) return;
        ++summary.attempted_camera_transactions;
        if (transaction.terminal_state == "Complete") {
            ++summary.completed_camera_transactions;
            if (alias == "CAM-A") ++summary.cam_a_completed_count;
            if (alias == "CAM-B") ++summary.cam_b_completed_count;
        }
        summary.spool_empty_before_count += transaction.spool_empty_before_capture ? 1 : 0;
        summary.camera_card_delete_attempted_count += transaction.camera_card_delete_attempted ? 1 : 0;
        summary.camera_card_delete_succeeded_count += transaction.camera_card_delete_succeeded ? 1 : 0;
        summary.spool_empty_after_count += transaction.spool_empty_after_cleanup ? 1 : 0;
    };

    for (int index = 0; index < requested_pairs; ++index) {
        ++summary.attempted_pairs;
        const auto pair = capture_pair_once();
        pair_durations_ms.push_back(std::max<std::int64_t>(0, pair.duration.count()));
        accumulate_transaction(pair.cam_a, "CAM-A");
        if (pair.cam_b_started) accumulate_transaction(pair.cam_b, "CAM-B");
        summary.last_pair_state = pair.terminal_state;
        summary.last_error_category = pair.error_category;
        summary.last_error_detail = pair.error_detail;
        if (pair.terminal_state == "Complete") {
            ++summary.completed_pairs;
        } else {
            ++summary.failures;
            summary.terminal_state = "FailedPartial";
            break;
        }
    }
    if (summary.completed_pairs == summary.requested_pairs && summary.failures == 0) {
        summary.terminal_state = "Complete";
    } else if (summary.terminal_state != "FailedPartial") {
        summary.terminal_state = "FailedPartial";
    }
    std::sort(pair_durations_ms.begin(), pair_durations_ms.end());
    summary.duration_sample_count = static_cast<int>(pair_durations_ms.size());
    if (!pair_durations_ms.empty()) {
        const auto nearest_rank = [&](int percentile) {
            const auto rank = (pair_durations_ms.size() * static_cast<std::size_t>(percentile) + 99U) / 100U;
            return pair_durations_ms[std::max<std::size_t>(1U, rank) - 1U];
        };
        summary.pair_duration_p50_ms = nearest_rank(50);
        summary.pair_duration_p95_ms = nearest_rank(95);
        summary.pair_duration_max_ms = pair_durations_ms.back();
    }
    return summary;
}

fs::path PersistSdkStatusSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const CameraInfo& camera,
    const SdkCameraStatus& status,
    const SdkStatusProcessRouting& routing) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path summary = run_root / "sdk-status-summary.json";
    const fs::path partial = run_root / "sdk-status-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(summary) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite an SDK status summary");
    }

    const auto validate_setting = [](const SdkCameraStatus::SettingCapability& setting) {
        const bool cap_type_valid = setting.cap_type == "enum" || setting.cap_type == "unsigned" ||
            setting.cap_type == "unsupported";
        const bool value_type_valid = setting.value_type == "unsigned" || setting.value_type == "packed-string" ||
            setting.value_type == "string" || setting.value_type == "unsupported";
        const bool probe_state_valid = setting.probe_state == "available" ||
            setting.probe_state == "not-advertised" || setting.probe_state == "get-not-supported" ||
            setting.probe_state == "unsupported-type" || setting.probe_state == "get-array-not-supported" ||
            setting.probe_state == "invalid-shape" || setting.probe_state == "read-error";
        if (!cap_type_valid || !value_type_valid || !probe_state_valid ||
            (setting.available != (setting.probe_state == "available"))) {
            throw std::runtime_error("SDK setting capability has an invalid probe state");
        }
        if ((setting.probe_state == "not-advertised" || setting.probe_state == "unsupported-type") &&
            setting.cap_type != "unsupported") {
            throw std::runtime_error("unavailable SDK setting capability has an inconsistent capability type");
        }
        if ((setting.probe_state == "get-array-not-supported" || setting.probe_state == "invalid-shape") &&
            setting.cap_type != "enum") {
            throw std::runtime_error("enum SDK setting capability has an inconsistent capability type");
        }
        if ((setting.probe_state == "get-not-supported" || setting.probe_state == "read-error") &&
            setting.cap_type != "enum" && setting.cap_type != "unsigned") {
            throw std::runtime_error("advertised SDK setting capability has an inconsistent capability type");
        }
        if (!setting.available) {
            if (setting.current_value || setting.current_index || setting.current_label ||
                !setting.numeric_values.empty() || !setting.string_values.empty()) {
                throw std::runtime_error("unavailable SDK setting capability must not include a value");
            }
            return;
        }
        if (setting.value_type == "unsigned") {
            if (!setting.current_value || setting.current_label || !setting.string_values.empty()) {
                throw std::runtime_error("available unsigned SDK setting capability has inconsistent values");
            }
            if (setting.cap_type == "unsigned" && (setting.current_index || !setting.numeric_values.empty())) {
                throw std::runtime_error("available unsigned SDK setting capability has inconsistent values");
            }
            if (setting.cap_type == "enum" && (!setting.current_index || setting.numeric_values.empty() || setting.numeric_values.size() > 256 ||
                !setting.current_index || *setting.current_index >= setting.numeric_values.size() ||
                setting.numeric_values[*setting.current_index] != *setting.current_value)) {
                throw std::runtime_error("available enum SDK setting capability has inconsistent values");
            }
            if (setting.cap_type != "unsigned" && setting.cap_type != "enum") {
                throw std::runtime_error("available unsigned SDK setting capability has an inconsistent capability type");
            }
            return;
        }
        if (setting.cap_type != "enum" || setting.value_type == "unsupported" || setting.current_value ||
            !setting.current_index || !setting.current_label || !setting.numeric_values.empty() ||
            setting.string_values.empty() || setting.string_values.size() > 256 ||
            *setting.current_index >= setting.string_values.size() ||
            setting.string_values[*setting.current_index] != *setting.current_label) {
            throw std::runtime_error("available string SDK setting capability has inconsistent values");
        }
    };
    validate_setting(status.file_type);
    validate_setting(status.compression_level);
    validate_setting(status.image_size);
    validate_setting(status.exposure_mode);
    validate_setting(status.shutter_speed);
    validate_setting(status.aperture);
    validate_setting(status.sensitivity);
    validate_setting(status.wb_mode);
    validate_setting(status.focus_mode);
    const auto command_trace_failure = ValidateSdkReadOnlyCommandTrace(status.command_trace);
    if (command_trace_failure) {
        throw std::runtime_error(
            "SDK status command trace is not read-only: " + std::string(*command_trace_failure));
    }
    const auto routing_failure = ValidateSdkStatusProcessRouting(routing);
    if (routing_failure) {
        throw std::runtime_error(
            "SDK status process routing is not read-only: " + std::string(*routing_failure));
    }

    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create SDK status summary");
    const auto write_setting = [&output](std::string_view name,
                                         const SdkCameraStatus::SettingCapability& setting,
                                         bool trailing_comma) {
        output << "  \"" << name << "\": {\"available\": "
               << (setting.available ? "true" : "false")
               << ", \"capType\": \"" << JsonEscape(setting.cap_type)
               << "\", \"probeState\": \"" << JsonEscape(setting.probe_state)
               << "\", \"valueType\": \"" << JsonEscape(setting.value_type) << "\", \"currentValue\": ";
        if (setting.current_value) output << *setting.current_value;
        else output << "null";
        output << ", \"currentIndex\": ";
        if (setting.current_index) output << *setting.current_index;
        else output << "null";
        output << ", \"currentLabel\": ";
        if (setting.current_label) output << "\"" << JsonEscape(*setting.current_label) << "\"";
        else output << "null";
        output << ", \"numericValues\": [";
        for (std::size_t index = 0; index < setting.numeric_values.size(); ++index) {
            if (index != 0) output << ", ";
            output << setting.numeric_values[index];
        }
        output << "], \"stringValues\": [";
        for (std::size_t index = 0; index < setting.string_values.size(); ++index) {
            if (index != 0) output << ", ";
            output << "\"" << JsonEscape(setting.string_values[index]) << "\"";
        }
        output << "]}" << (trailing_comma ? ",\n" : "\n");
    };
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.sdk-status-summary.v5\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"cameraAlias\": \"" << JsonEscape(camera_alias) << "\",\n"
           << "  \"model\": \"" << JsonEscape(camera.model) << "\",\n"
           << "  \"firmware\": \"" << JsonEscape(status.firmware) << "\",\n"
           << "  \"shootingMode\": \"" << JsonEscape(camera.shooting_mode) << "\",\n"
           << "  \"liveViewStatus\": \"" << JsonEscape(status.live_view_status) << "\",\n"
           << "  \"liveViewStatusAvailable\": " << (status.live_view_status_available ? "true" : "false") << ",\n"
           << "  \"liveViewSelector\": \"" << JsonEscape(status.live_view_selector) << "\",\n"
           << "  \"liveViewSelectorAvailable\": " << (status.live_view_selector_available ? "true" : "false") << ",\n"
           << "  \"liveViewProhibitMask\": ";
    if (status.live_view_prohibit_mask) output << *status.live_view_prohibit_mask;
    else output << "null";
    output << ",\n"
           << "  \"liveViewProhibitAvailable\": " << (status.live_view_prohibit_mask ? "true" : "false") << ",\n";
    write_setting("fileType", status.file_type, true);
    write_setting("compressionLevel", status.compression_level, true);
    write_setting("imageSize", status.image_size, true);
    write_setting("exposureMode", status.exposure_mode, true);
    write_setting("shutterSpeed", status.shutter_speed, true);
    write_setting("aperture", status.aperture, true);
    write_setting("sensitivity", status.sensitivity, true);
    write_setting("wbMode", status.wb_mode, true);
    write_setting("focusMode", status.focus_mode, true);
    const auto& trace = status.command_trace;
    output << "  \"commandTrace\": {\n"
           << "    \"capGetCount\": " << trace.cap_get_count << ",\n"
           << "    \"capGetArrayCount\": " << trace.cap_get_array_count << ",\n"
           << "    \"capSetCount\": " << trace.cap_set_count << ",\n"
           << "    \"controlPlaneCapSetCount\": " << trace.control_plane_cap_set_count << ",\n"
           << "    \"photographicSettingCapSetCount\": " << trace.photographic_setting_cap_set_count << ",\n"
           << "    \"storageRoutingCapSetCount\": " << trace.storage_routing_cap_set_count << ",\n"
           << "    \"liveViewControlCapSetCount\": " << trace.live_view_control_cap_set_count << ",\n"
           << "    \"unexpectedCapSetCount\": " << trace.unexpected_cap_set_count << ",\n"
           << "    \"capStartCount\": " << trace.cap_start_count << ",\n"
           << "    \"captureStartCount\": " << trace.capture_start_count << ",\n"
           << "    \"nonCaptureStartCount\": " << trace.non_capture_start_count << ",\n"
           << "    \"unknownCapStartCount\": " << trace.unknown_cap_start_count << ",\n"
           << "    \"liveViewStartCount\": " << trace.live_view_start_count << ",\n"
           << "    \"sdkSessionOpened\": " << (trace.sdk_session_opened ? "true" : "false") << ",\n"
           << "    \"sdkSessionClosed\": " << (trace.sdk_session_closed ? "true" : "false") << ",\n"
           << "    \"readOnlyContractValid\": true\n"
           << "  },\n"
           << "  \"processRoutingProof\": {\n"
           << "    \"executor\": \"sdk-status\",\n"
           << "    \"sdkStatusExecutorSelected\": " << (routing.sdk_status_executor_selected ? "true" : "false") << ",\n"
           << "    \"singleIdentityV3Selected\": " << (routing.single_identity_v3_selected ? "true" : "false") << ",\n"
           << "    \"sdkEnumerationCount\": " << routing.sdk_enumeration_count << ",\n"
           << "    \"sdkStatusProbeCount\": " << routing.sdk_status_probe_count << ",\n"
           << "    \"wpdIdentityEnumerationCount\": " << routing.wpd_identity_enumeration_count << ",\n"
           << "    \"wpdCallCount\": " << routing.wpd_call_count << ",\n"
           << "    \"captureCallCount\": " << routing.capture_call_count << ",\n"
           << "    \"deleteCallCount\": " << routing.delete_call_count << ",\n"
           << "    \"routingContractValid\": true\n"
           << "  },\n"
           << "  \"cameraSettingReadOnlyProbe\": true,\n"
           << "  \"cameraSettingWriteAttempted\": "
           << (trace.photographic_setting_cap_set_count != 0 ? "true" : "false") << ",\n"
           << "  \"sdkControlPlaneCallbackRegistrationMayUseCapSet\": true,\n"
           << "  \"cameraSettingsChanged\": false,\n"
           << "  \"liveViewStarted\": "
           << (trace.live_view_start_count != 0 ? "true" : "false") << ",\n"
           << "  \"sdkSessionClosed\": "
           << (trace.sdk_session_closed ? "true" : "false") << ",\n"
           << "  \"realIdentifiersPrinted\": false\n"
           << "}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot persist SDK status summary");
    fs::rename(partial, summary);
    return summary;
}

fs::path PersistWpdStatusSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const CameraInfo& camera,
    const WpdStatusSummary& status) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path summary = run_root / "wpd-status-summary.json";
    const fs::path partial = run_root / "wpd-status-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(summary) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite a WPD status summary");
    }

    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create WPD status summary");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.wpd-status-summary.v1\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"cameraAlias\": \"" << JsonEscape(camera_alias) << "\",\n"
           << "  \"model\": \"" << JsonEscape(camera.model) << "\",\n"
           << "  \"firmware\": \"" << JsonEscape(camera.firmware) << "\",\n"
           << "  \"shootingMode\": \"" << JsonEscape(camera.shooting_mode) << "\",\n"
           << "  \"targetValidationState\": \"" << JsonEscape(status.target_validation_state) << "\",\n"
           << "  \"commandOptionsHRESULT\": \"" << JsonEscape(status.command_options_hresult) << "\",\n"
           << "  \"optionValueHRESULT\": \"" << JsonEscape(status.option_value_hresult) << "\",\n"
           << "  \"functionalObjectCount\": " << status.functional_object_count << ",\n"
           << "  \"validObjectIdsOptionPresent\": " << (status.valid_object_ids_option_present ? "true" : "false") << ",\n"
           << "  \"validObjectIdCount\": " << status.valid_object_id_count << ",\n"
           << "  \"compatibleTargetCount\": " << status.compatible_target_count << ",\n"
           << "  \"selectedTarget\": " << (status.selected_target ? "true" : "false") << ",\n"
           << "  \"vendorOpcodeValidationState\": \"" << JsonEscape(status.vendor_opcode_validation_state) << "\",\n"
           << "  \"supportedCommandsHRESULT\": \"" << JsonEscape(status.supported_commands_hresult) << "\",\n"
           << "  \"vendorOpcodeQuerySendHRESULT\": \"" << JsonEscape(status.vendor_opcode_query_send_hresult) << "\",\n"
           << "  \"vendorOpcodeQueryCommonHRESULT\": \"" << JsonEscape(status.vendor_opcode_query_common_hresult) << "\",\n"
           << "  \"wpdStillImageCaptureCommandAdvertised\": " << (status.wpd_still_image_capture_command_advertised ? "true" : "false") << ",\n"
           << "  \"vendorOpcodeQueryAdvertised\": " << (status.vendor_opcode_query_advertised ? "true" : "false") << ",\n"
           << "  \"vendorOpcodeCollectionAvailable\": " << (status.vendor_opcode_collection_available ? "true" : "false") << ",\n"
           << "  \"vendorOpcodeItemCount\": " << status.vendor_opcode_item_count << ",\n"
           << "  \"vendorOpcodeUniqueCount\": " << status.vendor_opcode_unique_count << ",\n"
           << "  \"vendorCapture9207Advertised\": " << (status.vendor_capture_9207_advertised ? "true" : "false") << ",\n"
           << "  \"standardOpcode100eAdvertisementAvailable\": " << (status.standard_opcode_100e_advertisement_available ? "true" : "false") << ",\n"
           << "  \"standardOpcode100eAdvertisementState\": \"" << JsonEscape(status.standard_opcode_100e_advertisement_state) << "\",\n"
           << "  \"wpdRequestedAccess\": \"" << JsonEscape(status.requested_access) << "\",\n"
           << "  \"readOnlyAccess\": " << (status.read_only_access ? "true" : "false") << ",\n"
           << "  \"nonMutatingProbe\": true,\n"
           << "  \"readOnlyCommandSent\": " << (status.read_only_command_sent ? "true" : "false") << ",\n"
           << "  \"captureCommandSent\": false,\n"
           << "  \"vendorOperationExecuted\": false,\n"
           << "  \"realIdentifiersPrinted\": false\n"
           << "}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot persist WPD status summary");
    fs::rename(partial, summary);
    return summary;
}

fs::path PersistWpdSpoolStatusSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const WpdSpoolStatusSummary& status) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path summary = run_root / "wpd-spool-status-summary.json";
    const fs::path partial = run_root / "wpd-spool-status-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(summary) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite a WPD spool status summary");
    }
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create WPD spool status summary");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.wpd-spool-status-summary.v1\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"cameraAlias\": \"" << JsonEscape(camera_alias) << "\",\n"
           << "  \"payloadObjectCount\": " << status.payload_object_count << ",\n"
           << "  \"spoolState\": \""
           << (status.terminal_state == "Complete"
                   ? (status.payload_object_count == 0 ? "EMPTY" : "NON_EMPTY")
                   : "UNKNOWN")
           << "\",\n"
           << "  \"readOnlyObservation\": " << (status.read_only_observation ? "true" : "false") << ",\n"
           << "  \"captureCommandSent\": " << (status.capture_command_sent ? "true" : "false") << ",\n"
           << "  \"vendorOperationExecuted\": " << (status.vendor_operation_executed ? "true" : "false") << ",\n"
           << "  \"cameraSettingsChanged\": " << (status.camera_settings_changed ? "true" : "false") << ",\n"
           << "  \"cameraObjectDeleteAttempted\": " << (status.camera_object_delete_attempted ? "true" : "false") << ",\n"
           << "  \"wpdSessionsClosed\": " << status.wpd_sessions_closed << ",\n"
           << "  \"terminalState\": \"" << JsonEscape(status.terminal_state) << "\",\n"
           << "  \"failedStage\": \"" << JsonEscape(status.failed_stage) << "\",\n"
           << "  \"objectIdentifiersIncluded\": false,\n"
           << "  \"objectNamesIncluded\": false,\n"
           << "  \"realIdentifiersPrinted\": false\n"
           << "}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot persist WPD spool status summary");
    fs::rename(partial, summary);
    return summary;
}

fs::path PersistWpdCorrelationSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const WpdCorrelationRunSummary& status) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path summary = run_root / "wpd-correlation-summary.json";
    const fs::path partial = run_root / "wpd-correlation-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(summary) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite a WPD correlation summary");
    }
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create WPD correlation summary");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.wpd-correlation-summary.v1\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"cameraAlias\": \"" << JsonEscape(camera_alias) << "\",\n"
           << "  \"sampleCount\": " << status.sample_count << ",\n"
           << "  \"deviceDatetimeAvailableCount\": " << status.device_datetime_available_count << ",\n"
           << "  \"reopenAdvanceCount\": " << status.reopen_advance_count << ",\n"
           << "  \"reopenEqualCount\": " << status.reopen_equal_count << ",\n"
           << "  \"reopenRegressCount\": " << status.reopen_regress_count << ",\n"
           << "  \"jpegCount\": " << status.jpeg_count << ",\n"
           << "  \"datedJpegCount\": " << status.dated_jpeg_count << ",\n"
           << "  \"latestDateLessThanDeviceCount\": " << status.latest_date_less_than_device_count << ",\n"
           << "  \"latestDateEqualDeviceCount\": " << status.latest_date_equal_device_count << ",\n"
           << "  \"latestDateGreaterThanDeviceCount\": " << status.latest_date_greater_than_device_count << ",\n"
           << "  \"readOnlyObservation\": " << (status.read_only_observation ? "true" : "false") << ",\n"
           << "  \"captureCommandSent\": " << (status.capture_command_sent ? "true" : "false") << ",\n"
           << "  \"vendorOperationExecuted\": " << (status.vendor_operation_executed ? "true" : "false") << ",\n"
           << "  \"cameraSettingsChanged\": " << (status.camera_settings_changed ? "true" : "false") << ",\n"
           << "  \"cameraObjectDeleteAttempted\": " << (status.camera_object_delete_attempted ? "true" : "false") << ",\n"
           << "  \"wpdSessionsClosed\": " << status.wpd_sessions_closed << ",\n"
           << "  \"terminalState\": \"" << JsonEscape(status.terminal_state) << "\",\n"
           << "  \"failedStage\": \"" << JsonEscape(status.failed_stage) << "\",\n"
           << "  \"failedSample\": " << status.failed_sample << ",\n"
           << "  \"realIdentifiersPrinted\": false\n}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot persist WPD correlation summary");
    fs::rename(partial, summary);
    return summary;
}

fs::path PersistHybridFaultSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const HybridFaultRunSummary& status) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path summary = run_root / "hybrid-fault-summary.json";
    const fs::path partial = run_root / "hybrid-fault-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(summary) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite a hybrid fault summary");
    }
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create hybrid fault summary");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.hybrid-fault-summary.v1\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"cameraAlias\": \"" << JsonEscape(camera_alias) << "\",\n"
           << "  \"scenario\": \"" << JsonEscape(status.scenario) << "\",\n"
           << "  \"gateStage\": \"" << JsonEscape(status.gate_stage) << "\",\n"
           << "  \"transactionState\": \"" << JsonEscape(status.transaction_state) << "\",\n"
           << "  \"errorCategory\": \"" << JsonEscape(status.error_category) << "\",\n"
           << "  \"acceptanceState\": \"" << JsonEscape(status.acceptance_state) << "\",\n"
           << "  \"spoolEmptyBeforeCapture\": " << (status.spool_empty_before_capture ? "true" : "false") << ",\n"
           << "  \"pcOriginalPersisted\": " << (status.pc_original_persisted ? "true" : "false") << ",\n"
           << "  \"cameraObjectDeleteAttempted\": " << (status.camera_object_delete_attempted ? "true" : "false") << ",\n"
           << "  \"automaticRetry\": " << (status.automatic_retry ? "true" : "false") << ",\n"
           << "  \"recoveryRequiresNewTransaction\": " << (status.recovery_requires_new_transaction ? "true" : "false") << ",\n"
           << "  \"objectIdentifiersIncluded\": false,\n"
           << "  \"realIdentifiersPrinted\": false\n"
           << "}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot persist hybrid fault summary");
    fs::rename(partial, summary);
    return summary;
}

fs::path PersistHybridPairFaultSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    const HybridPairFaultRunSummary& status) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    const fs::path summary = run_root / "hybrid-pair-fault-summary.json";
    const fs::path partial = run_root / "hybrid-pair-fault-summary.json.partial";
    fs::create_directories(run_root);
    if (fs::exists(summary) || fs::exists(partial)) {
        throw std::runtime_error("refusing to overwrite a hybrid pair fault summary");
    }
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create hybrid pair fault summary");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.hybrid-pair-fault-summary.v1\",\n"
           << "  \"timestamp\": \"" << NowIso8601() << "\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"faultCameraAlias\": \"" << JsonEscape(status.fault_camera_alias) << "\",\n"
           << "  \"scenario\": \"" << JsonEscape(status.scenario) << "\",\n"
           << "  \"gateStage\": \"" << JsonEscape(status.gate_stage) << "\",\n"
           << "  \"pairState\": \"" << JsonEscape(status.pair_state) << "\",\n"
           << "  \"errorCategory\": \"" << JsonEscape(status.error_category) << "\",\n"
           << "  \"acceptanceState\": \"" << JsonEscape(status.acceptance_state) << "\",\n"
           << "  \"camBStarted\": " << (status.cam_b_started ? "true" : "false") << ",\n"
           << "  \"camAOriginalPersisted\": " << (status.cam_a_original_persisted ? "true" : "false") << ",\n"
           << "  \"camBOriginalPersisted\": " << (status.cam_b_original_persisted ? "true" : "false") << ",\n"
           << "  \"camADeleteAttempted\": " << (status.cam_a_delete_attempted ? "true" : "false") << ",\n"
           << "  \"camBDeleteAttempted\": " << (status.cam_b_delete_attempted ? "true" : "false") << ",\n"
           << "  \"automaticRetry\": " << (status.automatic_retry ? "true" : "false") << ",\n"
           << "  \"recoveryRequiresNewTransaction\": "
           << (status.recovery_requires_new_transaction ? "true" : "false") << ",\n"
           << "  \"actualShutterSynchronizationGuaranteed\": "
           << (status.actual_shutter_synchronization_guaranteed ? "true" : "false") << ",\n"
           << "  \"objectIdentifiersIncluded\": false,\n"
           << "  \"realIdentifiersPrinted\": false\n"
           << "}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot persist hybrid pair fault summary");
    fs::rename(partial, summary);
    return summary;
}

std::optional<std::string> ValidateWpdCorrelationArguments(
    std::string_view command,
    bool samples_explicit,
    bool interval_explicit,
    int samples,
    int interval_ms) noexcept {
    if (command != "wpd-correlation-status") {
        if (samples_explicit || interval_explicit) {
            return "--samples and --sample-interval-ms are valid only for wpd-correlation-status";
        }
        return std::nullopt;
    }
    if (samples < 1 || samples > 10) return "samples must be between 1 and 10";
    if (interval_ms < 0 || interval_ms > 60000) return "sample-interval-ms must be between 0 and 60000";
    return std::nullopt;
}

std::optional<std::string> ValidateHybridCaptureArguments(
    std::string_view command,
    int count,
    bool exclusive_camera_control_confirmed,
    bool dedicated_spool_scope_confirmed,
    bool exact_object_delete_confirmed,
    bool dual_dedicated_spools_confirmed) noexcept {
    const bool hybrid_command = command == "hybrid-capture-single" || command == "hybrid-capture-pair" ||
        command == "live-view-handoff" || command == "hybrid-fault-single" ||
        command == "hybrid-fault-pair" || command == "hybrid-interrupt-pair";
    if (!hybrid_command) {
        if (exclusive_camera_control_confirmed || dedicated_spool_scope_confirmed || exact_object_delete_confirmed ||
            dual_dedicated_spools_confirmed) {
            return "hybrid safety confirmations are valid only for an approved hybrid command";
        }
        return std::nullopt;
    }
    if (command == "hybrid-capture-single" && count != 1 && count != 10) {
        return "hybrid-capture-single count must be 1 or 10";
    }
    if (command == "hybrid-capture-pair" && count != 1 && count != 10 && count != 100) {
        return "hybrid-capture-pair count must be 1, 10, or 100";
    }
    if ((command == "hybrid-capture-pair" || command == "hybrid-fault-pair" ||
         command == "hybrid-interrupt-pair") &&
        !dual_dedicated_spools_confirmed) {
        return std::string(command) +
            " requires --dual-dedicated-spools-confirmed for both physical D810 bodies";
    }
    if (command != "hybrid-capture-pair" && command != "hybrid-fault-pair" &&
        command != "hybrid-interrupt-pair" &&
        dual_dedicated_spools_confirmed) {
        return "dual-dedicated-spools-confirmed is valid only for a two-body hybrid command";
    }
    if (command == "hybrid-fault-single" && count != 1) {
        return "hybrid-fault-single count must be 1";
    }
    if (command == "hybrid-fault-pair" && count != 1) {
        return "hybrid-fault-pair count must be 1";
    }
    if (command == "hybrid-interrupt-pair" && count != 1) {
        return "hybrid-interrupt-pair count must be 1";
    }
    if (command == "live-view-handoff" && count != 10) {
        return "live-view-handoff requires --count 10";
    }
    if (!exclusive_camera_control_confirmed) {
        return "hybrid operation requires --exclusive-camera-control-confirmed";
    }
    if (!dedicated_spool_scope_confirmed) {
        return "hybrid operation requires --dedicated-spool-scope-confirmed";
    }
    if (!exact_object_delete_confirmed) {
        return "hybrid operation requires --exact-object-delete-confirmed";
    }
    return std::nullopt;
}

WpdCorrelationRunSummary ExecuteWpdCorrelationSamples(
    ICorrelationObservationTransport& transport,
    std::string_view stable_identity,
    int samples,
    int interval_ms) {
    WpdCorrelationRunSummary summary;
    for (int index = 0; index < samples; ++index) {
        try {
            transport.OpenReadOnlyObservation(stable_identity, std::chrono::seconds(10));
        } catch (...) {
            summary.terminal_state = "Failed";
            summary.failed_stage = "open";
            summary.failed_sample = index + 1;
            return summary;
        }
        WpdCorrelationSample sample;
        try {
            sample = transport.ReadCorrelationSample();
        } catch (...) {
            try {
                transport.Close(std::chrono::seconds(10));
                ++summary.wpd_sessions_closed;
            } catch (...) {
                summary.terminal_state = "Failed";
                summary.failed_stage = "close_after_read_failure";
                summary.failed_sample = index + 1;
                return summary;
            }
            summary.terminal_state = "Failed";
            summary.failed_stage = "read";
            summary.failed_sample = index + 1;
            return summary;
        }
        try {
            transport.Close(std::chrono::seconds(10));
            ++summary.wpd_sessions_closed;
        } catch (...) {
            summary.terminal_state = "Failed";
            summary.failed_stage = "close";
            summary.failed_sample = index + 1;
            return summary;
        }
        ++summary.sample_count;
        summary.device_datetime_available_count += sample.device_datetime_available ? 1 : 0;
        summary.reopen_advance_count += sample.reopen_datetime_advanced ? 1 : 0;
        summary.reopen_equal_count += sample.reopen_datetime_equal ? 1 : 0;
        summary.reopen_regress_count += sample.reopen_datetime_regressed ? 1 : 0;
        summary.jpeg_count += sample.jpeg_count;
        summary.dated_jpeg_count += sample.dated_jpeg_count;
        summary.latest_date_less_than_device_count += sample.latest_date_less_than_device;
        summary.latest_date_equal_device_count += sample.latest_date_equal_device;
        summary.latest_date_greater_than_device_count += sample.latest_date_greater_than_device;
        if (index + 1 < samples) std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    summary.terminal_state = "Complete";
    return summary;
}

LiveViewProbeResult AcquireLiveViewFrames(
    ILiveViewTransport& transport,
    std::string_view stable_identity,
    int frames,
    int interval_ms,
    int duration_seconds) {
    if (stable_identity.empty()) throw std::invalid_argument("live view identity is empty");
    if (frames < 1) throw std::invalid_argument("live view frames must be positive");
    if (interval_ms < 0) throw std::invalid_argument("live view interval must not be negative");
    if (duration_seconds < 0) throw std::invalid_argument("live view duration must not be negative");

    const auto started = std::chrono::steady_clock::now();
    LiveViewProbeResult result;
    bool session_open = false;
    bool live_view_started = false;
    bool stop_attempted = false;
    bool close_attempted = false;
    try {
        transport.OpenLiveView(stable_identity, std::chrono::seconds(10));
        session_open = true;
        transport.StartLiveView(std::chrono::seconds(10));
        live_view_started = true;
        const auto duration_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
        while ((duration_seconds > 0 &&
                   (result.frames == 0 || std::chrono::steady_clock::now() < duration_deadline)) ||
               (duration_seconds == 0 && result.frames < frames)) {
            result.last_frame = transport.ReadLiveViewFrame(std::chrono::seconds(10));
            if (!IsValidJpeg(result.last_frame)) {
                throw TransportError("live_view_invalid_frame", "live view returned an invalid JPEG");
            }
            ++result.frames;
            const bool more_frames = duration_seconds > 0
                ? std::chrono::steady_clock::now() < duration_deadline
                : result.frames < frames;
            if (more_frames && interval_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
        }

        stop_attempted = true;
        transport.StopLiveView(std::chrono::seconds(10));
        live_view_started = false;
        close_attempted = true;
        transport.Close(std::chrono::seconds(10));
        session_open = false;
    } catch (...) {
        // Cleanup operations are attempted at most once. A failed stop or close is
        // terminal for this handoff and is never retried automatically.
        if (live_view_started && !stop_attempted) {
            stop_attempted = true;
            try { transport.StopLiveView(std::chrono::seconds(3)); } catch (...) {}
        }
        if (session_open && !close_attempted) {
            close_attempted = true;
            try { transport.Close(std::chrono::seconds(3)); } catch (...) {}
        }
        throw;
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

LiveViewHandoffResult ExecuteLiveViewHandoffOnce(
    ILiveViewTransport& live_view_transport,
    std::string_view live_view_identity,
    const std::function<TransactionResult()>& capture_once,
    EvidenceWriter& evidence,
    std::string_view handoff_id,
    std::string_view camera_alias,
    int frames,
    int interval_ms,
    std::chrono::milliseconds resume_delay) {
    LiveViewHandoffResult result;
    evidence.RecordState(handoff_id, "LiveViewStarting", camera_alias);
    try {
        result.before = AcquireLiveViewFrames(
            live_view_transport, live_view_identity, frames, interval_ms);
    } catch (const TransportError& error) {
        result.terminal_state = "FailedBeforeCapture";
        result.error_category = error.Category();
        result.error_detail = ControlledErrorDetail(error.what());
        evidence.RecordState(handoff_id, "LiveViewHandoffFailedBeforeCapture", camera_alias, result.error_detail);
        return result;
    } catch (const std::exception&) {
        result.terminal_state = "FailedBeforeCapture";
        result.error_category = "live_view_exception";
        evidence.RecordState(handoff_id, "LiveViewHandoffFailedBeforeCapture", camera_alias);
        return result;
    }
    evidence.RecordState(handoff_id, "LiveViewFrameReady", camera_alias);
    evidence.RecordState(handoff_id, "SdkClosed", camera_alias);

    result.wpd_capture_started = true;
    evidence.RecordState(handoff_id, "HybridCaptureStarting", camera_alias);
    try {
        result.capture = capture_once();
    } catch (const TransportError& error) {
        result.terminal_state = "CaptureFailedResumeSkipped";
        result.error_category = error.Category();
        evidence.RecordState(handoff_id, "LiveViewResumeSkippedAfterCaptureFailure", camera_alias);
        return result;
    } catch (const std::exception&) {
        result.terminal_state = "CaptureFailedResumeSkipped";
        result.error_category = "capture_exception";
        evidence.RecordState(handoff_id, "LiveViewResumeSkippedAfterCaptureFailure", camera_alias);
        return result;
    }
    if (result.capture.terminal_state != "Complete") {
        result.terminal_state = "CaptureFailedResumeSkipped";
        result.error_category = result.capture.error_category.empty()
            ? "wpd_capture_failed"
            : result.capture.error_category;
        result.error_detail = result.capture.error_detail;
        evidence.RecordState(handoff_id, "LiveViewResumeSkippedAfterCaptureFailure", camera_alias, result.error_detail);
        return result;
    }

    result.wpd_capture_complete = true;
    result.resume_attempted = true;
    evidence.RecordState(handoff_id, "LiveViewResuming", camera_alias);
    if (resume_delay > std::chrono::milliseconds::zero()) {
        std::this_thread::sleep_for(resume_delay);
    }
    try {
        result.after = AcquireLiveViewFrames(
            live_view_transport, live_view_identity, frames, interval_ms);
    } catch (const TransportError& error) {
        result.terminal_state = "CaptureCompleteLiveViewResumeFailed";
        result.error_category = error.Category();
        result.error_detail = ControlledErrorDetail(error.what());
        evidence.RecordState(handoff_id, "LiveViewResumeFailedAfterCapture", camera_alias, result.error_detail);
        return result;
    } catch (const std::exception&) {
        result.terminal_state = "CaptureCompleteLiveViewResumeFailed";
        result.error_category = "live_view_resume_exception";
        evidence.RecordState(handoff_id, "LiveViewResumeFailedAfterCapture", camera_alias);
        return result;
    }

    result.terminal_state = "Complete";
    evidence.RecordState(handoff_id, "LiveViewResumed", camera_alias);
    return result;
}

fs::path PersistLiveViewHandoffSummary(
    const fs::path& artifacts_root,
    std::string_view run_id,
    std::string_view camera_alias,
    const LiveViewHandoffRunSummary& result) {
    const fs::path run_root = artifacts_root / std::string(run_id);
    fs::create_directories(run_root);
    const fs::path summary = run_root / "handoff-summary.json";
    const fs::path partial = run_root / "handoff-summary.json.partial";
    if (fs::exists(partial)) {
        throw std::runtime_error("handoff summary partial exists; inspect before continuing");
    }
    std::ofstream output(partial, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create handoff summary");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.live-view-handoff-summary.v2\",\n"
           << "  \"runId\": \"" << JsonEscape(run_id) << "\",\n"
           << "  \"cameraAlias\": \"" << JsonEscape(camera_alias) << "\",\n"
           << "  \"requestedHandoffs\": " << result.requested << ",\n"
           << "  \"attemptedHandoffs\": " << result.attempted << ",\n"
           << "  \"completeHandoffs\": " << result.completed << ",\n"
           << "  \"failures\": " << result.failures << ",\n"
           << "  \"spoolEmptyBeforeCount\": " << result.spool_empty_before_count << ",\n"
           << "  \"cameraCardDeleteAttemptedCount\": " << result.camera_card_delete_attempted_count << ",\n"
           << "  \"cameraCardDeleteSucceededCount\": " << result.camera_card_delete_succeeded_count << ",\n"
           << "  \"spoolEmptyAfterCount\": " << result.spool_empty_after_count << ",\n"
           << "  \"terminalState\": \"" << JsonEscape(result.terminal_state) << "\",\n"
           << "  \"lastHandoffState\": \"" << JsonEscape(result.last_handoff_state) << "\",\n"
           << "  \"lastErrorCategory\": \"" << JsonEscape(result.last_error_category) << "\",\n"
           << "  \"lastErrorDetail\": \"" << JsonEscape(result.last_error_detail) << "\",\n"
           << "  \"crossTransportBinding\": \"single-connected-body-only\",\n"
           << "  \"cleanupObjectIdIncluded\": false,\n"
           << "  \"vendorOperationExecuted\": false,\n"
           << "  \"previewFramesPersisted\": false\n"
           << "}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot persist handoff summary");
    if (!MoveFileExW(
            partial.c_str(), summary.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error(
            "cannot replace handoff summary, Windows error " + std::to_string(GetLastError()));
    }
    return summary;
}

std::string NewRunId() {
    static std::atomic<unsigned long long> sequence{0};
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return "run-" + std::to_string(millis) + '-' + std::to_string(++sequence);
}

fs::path DefaultIdentityMapPath() {
    const auto local_app_data = EnvironmentValue("LOCALAPPDATA");
    if (!local_app_data || local_app_data->empty()) throw std::runtime_error("LOCALAPPDATA is unavailable");
    return fs::path(*local_app_data) / "A0CameraStitcher" / "phase0" / "camera-map.json";
}

fs::path DefaultSingleIdentityV3Path() {
    const auto path = DefaultIdentityMapPath();
    return path.parent_path() / "single-identity-v3.json";
}

void PersistSingleIdentityV3(
    const fs::path& path,
    std::string_view alias,
    const std::vector<CameraInfo>& sdk_cameras,
    const std::vector<CameraInfo>& wpd_cameras) {
    if (alias != "CAM-A") {
        throw std::runtime_error("SingleCamera identity-v3 product alias must be CAM-A");
    }
    if (sdk_cameras.size() != 1 || wpd_cameras.size() != 1 ||
        sdk_cameras.front().model != "Nikon D810" ||
        wpd_cameras.front().model != "Nikon D810") {
        throw std::runtime_error(
            "SingleCamera identity-v3 requires exactly one D810 in both SDK and WPD inventories");
    }
    const std::string& wpd_identity = wpd_cameras.front().stable_identity;
    if (wpd_identity.size() != 64 ||
        !std::all_of(wpd_identity.begin(), wpd_identity.end(), [](unsigned char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
        throw std::runtime_error("WPD identity digest is not a canonical SHA-256 value");
    }

    const fs::path absolute = fs::absolute(path).lexically_normal();
    const std::wstring native = absolute.native();
    if (!absolute.is_absolute() || native.size() < 3 || native[1] != L':' ||
        (native[2] != L'\\' && native[2] != L'/') ||
        native.find(L':', 2) != std::wstring::npos ||
        native.starts_with(L"\\\\") || native.starts_with(L"\\??\\")) {
        throw std::runtime_error("identity-v3 path must be drive-qualified local storage");
    }
    const std::wstring drive_root{native[0], L':', L'\\'};
    if (GetDriveTypeW(drive_root.c_str()) != DRIVE_FIXED) {
        throw std::runtime_error("identity-v3 path must be on a fixed local drive");
    }
    fs::path current = absolute.root_path();
    for (const auto& component : absolute.lexically_relative(current)) {
        current /= component;
        if (fs::exists(current) && IsReparsePoint(current)) {
            throw std::runtime_error("identity-v3 path chain must be reparse-free");
        }
    }
    fs::create_directories(absolute.parent_path());
    if (fs::exists(absolute)) {
        throw std::runtime_error(
            "identity-v3 already exists; explicit invalidation and review are required");
    }
    const fs::path partial = absolute.string() + ".partial";
    if (fs::exists(partial)) {
        throw std::runtime_error("identity-v3 partial exists; inspect before retrying");
    }
    const std::string body =
        "{\n  \"schemaVersion\": \"a0.camera-agent.single-identity.v3\",\n"
        "  \"cameraMode\": \"SingleCamera\",\n"
        "  \"selectedAlias\": \"CAM-A\",\n"
        "  \"wpdStableIdentitySha256\": \"" + wpd_identity + "\",\n"
        "  \"sdkSelectionPolicy\": \"exactly-one-current-session\"\n}\n";
    WriteBytesExclusive(
        partial,
        std::vector<unsigned char>(body.begin(), body.end()));
    if (!MoveFileExW(
            partial.c_str(),
            absolute.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::error_code remove_error;
        fs::remove(partial, remove_error);
        throw std::system_error(
            static_cast<int>(error), std::system_category(),
            "cannot publish SingleCamera identity-v3");
    }
}

bool IsValidJpeg(const std::vector<unsigned char>& bytes) {
    return bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[bytes.size() - 2] == 0xFF && bytes.back() == 0xD9;
}

std::vector<unsigned char> ExtractD810LiveViewJpeg(const std::vector<unsigned char>& frame_with_header) {
    constexpr std::size_t header_size = 384;
    if (frame_with_header.size() <= header_size) {
        throw TransportError("live_view_invalid_frame", "D810 live view frame is shorter than its header");
    }
    const auto payload_begin = frame_with_header.begin() + header_size;
    constexpr std::array<unsigned char, 2> soi_marker{0xFF, 0xD8};
    constexpr std::array<unsigned char, 2> eoi_marker{0xFF, 0xD9};
    const auto soi = std::search(payload_begin, frame_with_header.end(),
        soi_marker.begin(), soi_marker.end());
    const auto eoi = soi == frame_with_header.end() ? frame_with_header.end() : std::search(soi + 2, frame_with_header.end(),
        eoi_marker.begin(), eoi_marker.end());
    if (soi == frame_with_header.end() || eoi == frame_with_header.end() || eoi < soi) {
        std::ostringstream message;
        message << "D810 live view payload has no complete JPEG markers (arrayBytes="
                << frame_with_header.size() << ')';
        throw TransportError("live_view_invalid_frame", message.str());
    }
    if (soi != payload_begin) {
        std::ostringstream message;
        message << "D810 live view JPEG starts at unexpected offset "
                << std::distance(frame_with_header.begin(), soi);
        throw TransportError("live_view_invalid_frame", message.str());
    }
    std::vector<unsigned char> jpeg(soi, eoi + 2);
    if (!IsValidJpeg(jpeg)) throw TransportError("live_view_invalid_frame", "D810 live view JPEG validation failed");
    return jpeg;
}

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, hash_size = 0, written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("BCrypt open failed");
    const auto close_algorithm = [&]() { if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &written, 0) < 0) {
        close_algorithm(); throw std::runtime_error("BCrypt property failed");
    }
    std::vector<unsigned char> object(object_size), digest(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), hash_size, 0) < 0) {
        if (hash) BCryptDestroyHash(hash); close_algorithm(); throw std::runtime_error("BCrypt SHA-256 failed");
    }
    BCryptDestroyHash(hash); close_algorithm();
    std::ostringstream stream;
    for (const auto byte : digest) stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return stream.str();
}

std::optional<std::vector<std::string>> ParsePackedStringLabels(
    const std::vector<unsigned char>& bytes,
    std::size_t maximum_labels,
    std::size_t maximum_bytes) {
    if (bytes.empty() || bytes.size() > maximum_bytes) return std::nullopt;
    std::vector<std::string> labels;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto terminator = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end(), 0U);
        if (terminator == bytes.end() || terminator == bytes.begin() + static_cast<std::ptrdiff_t>(offset) ||
            labels.size() >= maximum_labels) {
            return std::nullopt;
        }
        labels.emplace_back(reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<std::size_t>(terminator - (bytes.begin() + static_cast<std::ptrdiff_t>(offset))));
        offset = static_cast<std::size_t>(terminator - bytes.begin()) + 1;
    }
    return labels;
}

} // namespace a0::phase0
