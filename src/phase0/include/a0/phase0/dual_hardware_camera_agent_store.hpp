#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::phase0 {

inline constexpr std::string_view kDualHardwarePairJournalSchema =
    "a0.camera-agent.hardware-dual.pair-journal.v1";
inline constexpr std::string_view kDualHardwarePairJournalSchemaV2 =
    "a0.camera-agent.hardware-dual.pair-journal.v2";
inline constexpr std::string_view kDualHardwarePairJournalActiveDirectory =
    "active";
inline constexpr std::string_view kDualHardwarePairJournalFileName =
    "pair-journal.json";

enum class DualHardwarePairJournalState {
    reserved,
    dispatching,
    succeeded,
    failed,
    failed_partial,
    watchdog_expired,
};

struct DualHardwarePairJournalRecord {
    std::string transaction_id;
    DualHardwarePairJournalState state{DualHardwarePairJournalState::reserved};
    int automatic_retry_count{};
    std::string terminal_result_json;
};

class DualHardwarePairJournalStoreError final : public std::runtime_error {
public:
    DualHardwarePairJournalStoreError(std::string code, std::string message);

    [[nodiscard]] const std::string& Code() const noexcept;

private:
    std::string code_;
};

// Validates that `path` is a fail-closed-safe fixed local filesystem path:
// absolute and drive-qualified (e.g. "C:\..."), not a UNC (\\) or
// NT-namespace (\??\) path, free of "."/".." traversal components, resolves
// onto a DRIVE_FIXED volume (never removable/network/optical), and has no
// reparse point anywhere along its existing path chain -- including the
// leaf itself, so a symlink or junction masquerading as a plain file or
// directory is rejected even though std::filesystem's own type queries
// would otherwise follow it. Returns the normalized path on success and
// throws DualHardwarePairJournalStoreError (code "InvalidStoreRoot") on
// failure, with `subject` substituted into the message so callers outside
// this store (for example a Named Pipe host validating a
// --approved-capture-profile or --dual-identity-proof argument) get an
// accurate error instead of one that talks about a "pair journal root".
//
// This is the exact path-shape contract DualHardwarePairJournalStore's own
// constructor applies to its root (see ValidateFixedLocalRoot in
// dual_hardware_camera_agent_store.cpp); it is exposed here so other Dual
// hardware v2 entry points can apply an equally strict check without
// reimplementing it. It does not require the path to already exist, and it
// does not constrain whether an existing path is a file or a directory --
// that is the caller's concern.
[[nodiscard]] std::filesystem::path ValidateDualHardwareFixedLocalPath(
    const std::filesystem::path& path,
    std::string_view subject);

// Durable AR-08a-2A boundary only. This store owns one Reserved pair journal;
// it does not dispatch, capture, retry, or mutate a completed camera product.
class DualHardwarePairJournalStore final {
public:
    explicit DualHardwarePairJournalStore(std::filesystem::path root);

    [[nodiscard]] DualHardwarePairJournalRecord Reserve(
        std::string_view transaction_id);
    [[nodiscard]] std::optional<DualHardwarePairJournalRecord> Query(
        std::string_view transaction_id) const;
    [[nodiscard]] DualHardwarePairJournalRecord BeginDispatch(
        std::string_view transaction_id);
    [[nodiscard]] DualHardwarePairJournalRecord CompleteTerminal(
        std::string_view transaction_id,
        DualHardwarePairJournalState terminal_state,
        std::string_view terminal_result_json);

private:
    std::filesystem::path root_;
};

} // namespace a0::phase0
