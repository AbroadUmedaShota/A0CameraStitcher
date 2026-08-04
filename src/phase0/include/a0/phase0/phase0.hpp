#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0 {

class TransportError final : public std::runtime_error {
public:
    TransportError(std::string category, std::string message);
    [[nodiscard]] const std::string& Category() const noexcept;

private:
    std::string category_;
};

struct Timeouts {
    std::chrono::seconds open{10};
    std::chrono::seconds image_event{15};
    std::chrono::seconds download{60};
    std::chrono::seconds close{10};
    std::chrono::seconds pair_watchdog{180};
};

struct CameraInfo {
    std::string model;
    std::string firmware;
    std::string shooting_mode;
    std::string stable_identity;
};

struct ImageCandidate {
    std::string source_name;
    std::vector<unsigned char> bytes;
    bool attributable{true};
};

class ICameraTransport {
public:
    virtual ~ICameraTransport() = default;
    [[nodiscard]] virtual std::string SdkVersion() const = 0;
    [[nodiscard]] virtual std::vector<CameraInfo> Enumerate() = 0;
    virtual void Open(std::string_view stable_identity, std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::string Baseline(std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) = 0;
    virtual void Close(std::chrono::seconds timeout) = 0;
};

class IdentityMap {
public:
    explicit IdentityMap(std::filesystem::path path);
    [[nodiscard]] std::optional<std::string> FindAlias(std::string_view stable_identity) const;
    [[nodiscard]] std::string AssignNext(std::string_view stable_identity);
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

private:
    void Load();
    void Save() const;
    std::filesystem::path path_;
    std::optional<std::string> cam_a_;
    std::optional<std::string> cam_b_;
};

struct FrameEvidence {
    bool success{false};
    std::string camera_alias;
    std::filesystem::path path;
    std::string sha256;
    std::uintmax_t bytes{0};
    std::string error_category;
};

struct TransactionResult {
    std::string run_id;
    std::string transaction_id;
    std::string terminal_state;
    std::vector<FrameEvidence> frames;
    std::string error_category;
    std::chrono::milliseconds duration{0};
};

class EvidenceWriter {
public:
    EvidenceWriter(std::filesystem::path artifacts_root, std::string run_id, std::string sdk_version);
    [[nodiscard]] FrameEvidence PersistExactlyOne(
        std::string_view transaction_id,
        std::string_view camera_alias,
        const std::vector<ImageCandidate>& candidates);
    void RecordCamera(std::string_view camera_alias, std::string_view firmware);
    void RecordState(std::string_view transaction_id, std::string_view state, std::string_view camera_alias = {});
    void RecordResult(const TransactionResult& result);
    [[nodiscard]] const std::string& RunId() const noexcept;
    [[nodiscard]] const std::filesystem::path& RunRoot() const noexcept;
    void GenerateRedactedReport(const std::filesystem::path& report_root) const;

private:
    void AppendEvent(std::string_view json_line);
    std::filesystem::path artifacts_root_;
    std::filesystem::path run_root_;
    std::string run_id_;
    std::string sdk_version_;
    std::size_t transaction_count_{0};
    std::size_t complete_count_{0};
    std::size_t failed_count_{0};
};

class CaptureCoordinator {
public:
    CaptureCoordinator(ICameraTransport& transport, EvidenceWriter& evidence, Timeouts timeouts = {});
    [[nodiscard]] TransactionResult CaptureSingle(std::string_view alias, std::string_view stable_identity);
    [[nodiscard]] TransactionResult CapturePair(std::string_view cam_a_identity, std::string_view cam_b_identity);

private:
    [[nodiscard]] FrameEvidence CaptureOne(
        std::string_view transaction_id,
        std::string_view alias,
        std::string_view stable_identity,
        std::string_view capture_state,
        std::string_view persist_state,
        std::optional<std::chrono::steady_clock::time_point> transaction_deadline = std::nullopt);
    [[nodiscard]] std::string NextTransactionId();
    ICameraTransport& transport_;
    EvidenceWriter& evidence_;
    Timeouts timeouts_;
    bool active_{false};
    unsigned long long sequence_{0};
};

[[nodiscard]] std::string NewRunId();
[[nodiscard]] std::filesystem::path DefaultIdentityMapPath();
[[nodiscard]] bool IsValidJpeg(const std::vector<unsigned char>& bytes);
[[nodiscard]] std::string Sha256Hex(const std::vector<unsigned char>& bytes);

} // namespace a0::phase0
