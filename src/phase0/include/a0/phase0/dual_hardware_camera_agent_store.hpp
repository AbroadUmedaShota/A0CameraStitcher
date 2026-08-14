#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::phase0 {

inline constexpr std::string_view kDualHardwarePairJournalSchema =
    "a0.camera-agent.hardware-dual.pair-journal.v1";
inline constexpr std::string_view kDualHardwarePairJournalActiveDirectory =
    "active";
inline constexpr std::string_view kDualHardwarePairJournalFileName =
    "pair-journal.json";

enum class DualHardwarePairJournalState {
    reserved,
};

struct DualHardwarePairJournalRecord {
    std::string transaction_id;
    DualHardwarePairJournalState state{DualHardwarePairJournalState::reserved};
    int automatic_retry_count{};
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

private:
    std::filesystem::path root_;
};

} // namespace a0::phase0
