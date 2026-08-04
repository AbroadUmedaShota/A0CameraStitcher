#include "a0/phase0/phase0.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace a0::phase0 {
namespace {

std::string JsonEscape(std::string_view value) {
    std::ostringstream stream;
    for (const char ch : value) {
        switch (ch) {
        case '\\': stream << "\\\\"; break;
        case '"': stream << "\\\""; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default: stream << ch; break;
        }
    }
    return stream.str();
}

std::string JsonUnescape(std::string_view value) {
    std::string result;
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped && ch == '\\') { escaped = true; continue; }
        if (escaped) {
            switch (ch) {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: result.push_back(ch); break;
            }
            escaped = false;
        } else {
            result.push_back(ch);
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

void WriteBytesExclusive(const fs::path& path, const std::vector<unsigned char>& bytes) {
    if (fs::exists(path)) throw std::runtime_error("refusing to overwrite evidence: " + path.string());
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::out);
    if (!output) throw std::runtime_error("cannot create evidence file: " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) throw std::runtime_error("cannot persist evidence file: " + path.string());
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

IdentityMap::IdentityMap(fs::path path) : path_(std::move(path)) { Load(); }

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

std::string IdentityMap::AssignNext(std::string_view stable_identity) {
    if (const auto existing = FindAlias(stable_identity)) return *existing;
    if (!cam_a_) { cam_a_ = std::string(stable_identity); Save(); return "CAM-A"; }
    if (!cam_b_) { cam_b_ = std::string(stable_identity); Save(); return "CAM-B"; }
    throw std::runtime_error("CAM-A and CAM-B are already assigned; manual review required");
}

const fs::path& IdentityMap::Path() const noexcept { return path_; }

EvidenceWriter::EvidenceWriter(fs::path artifacts_root, std::string run_id, std::string sdk_version)
    : artifacts_root_(std::move(artifacts_root)), run_root_(artifacts_root_ / run_id),
      run_id_(std::move(run_id)), sdk_version_(std::move(sdk_version)) {
    fs::create_directories(run_root_);
}

void EvidenceWriter::AppendEvent(std::string_view json_line) {
    std::ofstream output(run_root_ / "events.jsonl", std::ios::app);
    if (!output) throw std::runtime_error("cannot append Phase 0 event log");
    output << json_line << '\n';
}

void EvidenceWriter::RecordState(std::string_view transaction_id, std::string_view state, std::string_view camera_alias) {
    std::ostringstream event;
    event << "{\"timestamp\":\"" << NowIso8601() << "\",\"runId\":\"" << JsonEscape(run_id_)
          << "\",\"transactionId\":\"" << JsonEscape(transaction_id) << "\",\"state\":\"" << JsonEscape(state) << "\"";
    if (!camera_alias.empty()) event << ",\"cameraAlias\":\"" << JsonEscape(camera_alias) << "\"";
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
                                                 const std::vector<ImageCandidate>& candidates) {
    FrameEvidence frame;
    frame.camera_alias = std::string(camera_alias);
    if (candidates.size() != 1 || !candidates.front().attributable || !IsValidJpeg(candidates.front().bytes)) {
        frame.error_category = candidates.empty() ? "no_candidate" :
            (candidates.size() > 1 ? "ambiguous_candidates" :
                (!candidates.front().attributable ? "late_candidate" : "invalid_jpeg"));
        const fs::path quarantine = artifacts_root_ / "quarantine" / run_id_ /
            std::string(transaction_id) / std::string(camera_alias);
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            WriteBytesExclusive(quarantine / (std::to_string(index) + "_" + SanitizeFileName(candidate.source_name) + ".bin"), candidate.bytes);
        }
        RecordState(transaction_id, "Quarantined", camera_alias);
        return frame;
    }
    const fs::path directory = run_root_ / std::string(transaction_id) / std::string(camera_alias);
    const fs::path partial = directory / "original.jpg.partial";
    const fs::path final = directory / "original.jpg";
    frame.bytes = candidates.front().bytes.size();
    frame.sha256 = Sha256Hex(candidates.front().bytes);
    WriteBytesExclusive(partial, candidates.front().bytes);
    if (fs::exists(final)) throw std::runtime_error("refusing to overwrite an original JPEG");
    fs::rename(partial, final);
    frame.success = true;
    frame.path = final;
    std::ostringstream event;
    event << "{\"timestamp\":\"" << NowIso8601() << "\",\"runId\":\"" << JsonEscape(run_id_)
          << "\",\"transactionId\":\"" << JsonEscape(transaction_id) << "\",\"state\":\"Persisted\",\"cameraAlias\":\""
          << JsonEscape(camera_alias) << "\",\"bytes\":" << frame.bytes << ",\"sha256\":\"" << frame.sha256 << "\"}";
    AppendEvent(event.str());
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
    const fs::path source = run_root_ / "summary.json";
    const fs::path events_source = run_root_ / "events.jsonl";
    if (!fs::exists(source) || !fs::exists(events_source)) throw std::runtime_error("run evidence does not exist");
    const fs::path destination = report_root / run_id_;
    fs::create_directories(destination);
    const fs::path summary_target = destination / "summary.json";
    const fs::path events_target = destination / "transaction-events.jsonl";
    const fs::path report_target = destination / "report.md";
    if (fs::exists(summary_target) || fs::exists(events_target) || fs::exists(report_target)) {
        throw std::runtime_error("refusing to overwrite redacted report");
    }
    fs::copy_file(source, summary_target);
    fs::copy_file(events_source, events_target);
    std::ofstream report(report_target);
    if (!report) throw std::runtime_error("cannot write redacted report");
    report << "# Phase 0 report: " << run_id_ << "\n\n- Camera model: Nikon D810\n"
           << "- SDK version and counts: see `summary.json`\n"
           << "- Redacted camera profiles, transaction IDs, aliases, state timestamps, sizes, hashes, and error classes: see `transaction-events.jsonl`\n"
           << "- Raw images and unrestricted diagnostics: local ignored artifacts only\n"
           << "- Real camera identifiers: excluded\n";
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
    const auto budget = [&](std::chrono::seconds configured) {
        if (!transaction_deadline) return configured;
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            *transaction_deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::seconds::zero()) {
            throw TransportError("transaction_watchdog", "capture transaction watchdog expired");
        }
        return std::min(configured, remaining);
    };
    try {
        evidence_.RecordState(transaction_id, capture_state, alias);
        transport_.Open(stable_identity, budget(timeouts_.open));
        opened = true;
        const auto baseline = transport_.Baseline(budget(timeouts_.open));
        const auto capture_budget = transaction_deadline
            ? budget(timeouts_.pair_watchdog)
            : timeouts_.image_event + timeouts_.download;
        const auto candidates = transport_.CaptureAndDownload(
            baseline, budget(timeouts_.image_event), budget(timeouts_.download), capture_budget);
        evidence_.RecordState(transaction_id, persist_state, alias);
        frame = evidence_.PersistExactlyOne(transaction_id, alias, candidates);
        opened = false;
        transport_.Close(budget(timeouts_.close));
        return frame;
    } catch (const TransportError& error) {
        if (opened) { opened = false; try { transport_.Close(timeouts_.close); } catch (...) {} }
        frame.error_category = error.Category();
        evidence_.RecordState(transaction_id, "TransportError", alias);
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
    const auto deadline = started + timeouts_.pair_watchdog;
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
        if (watchdog_expired) evidence_.RecordState(result.transaction_id, "WatchdogExpired");
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    evidence_.RecordResult(result);
    return result;
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

bool IsValidJpeg(const std::vector<unsigned char>& bytes) {
    return bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[bytes.size() - 2] == 0xFF && bytes.back() == 0xD9;
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

} // namespace a0::phase0
