#pragma once

#include "a0/common/protocol_json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace a0::phase0::experimental {

// Shared contract between the preview worker and its parent: the fixed module
// topology counters every a0.preview-worker.v2 reply carries as "diag". Only
// counts, durations and fixed codes travel here; source IDs never leave the
// worker process. The table is the single source for the JSON keys and the
// journal event names. Adding a field changes the reply envelope and therefore
// requires a new schema version, this table, the static_assert below and the
// independent key sets of the tests to change together.
inline constexpr std::size_t kPreviewTopologyDiagFieldCount = 18;

// Schema of both the request and the reply envelope of the parent/worker pipe.
// v2 added "diag" to every reply; a v1 peer is rejected before any SDK call.
inline constexpr std::string_view kPreviewWorkerSchema = "a0.preview-worker.v2";

struct PreviewTopologyDiagField {
    std::string_view json_key;
    std::string_view journal_event;
};

inline constexpr std::array<PreviewTopologyDiagField, kPreviewTopologyDiagFieldCount> kPreviewTopologyDiagFields{{
    {"openAdd", "topo_open_add"},
    {"openRemove", "topo_open_remove"},
    {"inventoryAdd", "topo_inventory_add"},
    {"inventoryRemove", "topo_inventory_remove"},
    {"inventoryPumps", "topo_inventory_pumps"},
    {"snapshotChildren", "topo_snapshot_children"},
    {"snapshotEventIds", "topo_snapshot_event_ids"},
    {"snapshotMs", "topo_snapshot_ms"},
    {"postAddKnown", "topo_post_add_known"},
    {"postAddUnknown", "topo_post_add_unknown"},
    {"postRemoveKnown", "topo_post_remove_known"},
    {"postRemoveUnknown", "topo_post_remove_unknown"},
    {"postAddKnownDistinct", "topo_post_add_known_distinct"},
    {"postFirstEventOp", "topo_post_first_event_op"},
    {"postFirstEventMs", "topo_post_first_event_ms"},
    {"checkValidBeforePump", "topo_check_valid_before_pump"},
    {"checkSetEqual", "topo_check_set_equal"},
    {"checkCurrentCount", "topo_check_current_count"},
}};

// Position of each field in kPreviewTopologyDiagFields and PreviewTopologyDiag::values.
enum class PreviewTopologyField : std::size_t {
    open_add,
    open_remove,
    inventory_add,
    inventory_remove,
    inventory_pumps,
    snapshot_children,
    snapshot_event_ids,
    snapshot_ms,
    post_add_known,
    post_add_unknown,
    post_remove_known,
    post_remove_unknown,
    post_add_known_distinct,
    post_first_event_op,
    post_first_event_ms,
    check_valid_before_pump,
    check_set_equal,
    check_current_count,
    count_,
};
static_assert(static_cast<std::size_t>(PreviewTopologyField::count_) == kPreviewTopologyDiagFieldCount,
              "every topology field has a table entry");

struct PreviewTopologyDiag {
    std::array<std::uint32_t, kPreviewTopologyDiagFieldCount> values{};

    [[nodiscard]] constexpr std::uint32_t Get(PreviewTopologyField field) const noexcept {
        return values[static_cast<std::size_t>(field)];
    }
    [[nodiscard]] friend constexpr bool operator==(const PreviewTopologyDiag&, const PreviewTopologyDiag&) = default;
};
static_assert(sizeof(PreviewTopologyDiag) == sizeof(std::array<std::uint32_t, kPreviewTopologyDiagFieldCount>),
              "PreviewTopologyDiag carries the 18 numbers and nothing else");

// The dispatcher command that was running when a topology event arrived
// (postFirstEventOp). close is never reported: it freezes the counters.
enum class PreviewTopologyOperation : std::uint32_t {
    idle = 0, enumerate = 1, select = 2, start = 3, frame = 4, suspend = 5, resume = 6, close = 7,
};
// postFirstEventOp when the first post-snapshot event arrived while no command
// was running, or on a thread other than the transport's owning thread.
inline constexpr std::uint32_t kTopologyFirstEventOutsideCommand = 8;

[[nodiscard]] inline std::optional<PreviewTopologyOperation> TopologyOperationOf(std::string_view operation) noexcept {
    if (operation == "enumerate") return PreviewTopologyOperation::enumerate;
    if (operation == "select") return PreviewTopologyOperation::select;
    if (operation == "start") return PreviewTopologyOperation::start;
    if (operation == "frame") return PreviewTopologyOperation::frame;
    if (operation == "suspend") return PreviewTopologyOperation::suspend;
    if (operation == "resume") return PreviewTopologyOperation::resume;
    if (operation == "close") return PreviewTopologyOperation::close;
    return std::nullopt;
}

// {"openAdd":0,...} with the keys in table order.
[[nodiscard]] inline std::string SerializePreviewTopologyDiag(const PreviewTopologyDiag& diag) {
    std::string result = "{";
    for (std::size_t index = 0; index < kPreviewTopologyDiagFieldCount; ++index) {
        if (index) result += ',';
        result += '"';
        result += kPreviewTopologyDiagFields[index].json_key;
        result += "\":";
        result += std::to_string(diag.values[index]);
    }
    result += '}';
    return result;
}

// A non-negative decimal integer in uint32 range, judged on the raw JSON number
// lexeme: 1..10 digits, no leading zero except "0" itself, at most 4294967295.
// The JSON parser accepts signs, fractions and exponents as number lexemes, so
// the check is on the text, not on a converted value.
[[nodiscard]] inline bool ParseUint32Lexeme(std::string_view lexeme, std::uint32_t& out) noexcept {
    if (lexeme.empty() || lexeme.size() > 10) return false;
    if (lexeme.size() > 1 && lexeme.front() == '0') return false;
    std::uint64_t value{};
    for (const char character : lexeme) {
        if (character < '0' || character > '9') return false;
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
    }
    if (value > 4294967295ULL) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

// Strict shape check of a reply's "diag": an object with exactly the 18 table
// keys, each a number whose lexeme passes ParseUint32Lexeme. Any deviation is
// reported through Failure::Fail. The values are not range-checked here: the
// parent journals them and never decides anything from them.
template <class Failure>
[[nodiscard]] PreviewTopologyDiag ParsePreviewTopologyDiag(const a0::common::protocol_json::JsonValue& diag) {
    namespace json = a0::common::protocol_json;
    if (diag.kind != json::JsonKind::object || diag.object.size() != kPreviewTopologyDiagFieldCount)
        Failure::Fail("worker_reply_invalid", "worker reply diag shape rejected");
    PreviewTopologyDiag result;
    for (std::size_t index = 0; index < kPreviewTopologyDiagFieldCount; ++index) {
        const auto found = diag.object.find(std::string(kPreviewTopologyDiagFields[index].json_key));
        if (found == diag.object.end() || found->second.kind != json::JsonKind::number ||
            !ParseUint32Lexeme(found->second.string, result.values[index]))
            Failure::Fail("worker_reply_invalid", "worker reply diag value rejected");
    }
    return result;
}

} // namespace a0::phase0::experimental
