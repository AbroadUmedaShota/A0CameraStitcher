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
