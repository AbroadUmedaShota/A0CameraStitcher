// T-a/T-b/T-c: the worker-side topology counters and the shared diag helpers.
// Header-only code: no SDK, no Windows API, no child process.
#include "a0/phase0/preview_topology_diag.hpp"
#include "a0/phase0/worker_topology_counters.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace a0::phase0::experimental;
using Field = PreviewTopologyField;
using std::chrono::milliseconds;

namespace {
int failures{};
void Check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
using Clock = WorkerTopologyCounters::Clock;
const Clock::time_point t0{std::chrono::hours(1)};

// T-b static checks: the diag is the 18 numbers and nothing else, and the table
// has 18 distinct JSON keys and 18 distinct journal names.
static_assert(std::is_same_v<decltype(PreviewTopologyDiag::values), std::array<std::uint32_t, 18>>);
static_assert(sizeof(PreviewTopologyDiag) == sizeof(std::array<std::uint32_t, 18>));
static_assert(kPreviewTopologyDiagFields.size() == 18);
constexpr bool DistinctTableNames() {
    for (std::size_t i = 0; i < kPreviewTopologyDiagFields.size(); ++i)
        for (std::size_t j = i + 1; j < kPreviewTopologyDiagFields.size(); ++j)
            if (kPreviewTopologyDiagFields[i].json_key == kPreviewTopologyDiagFields[j].json_key ||
                kPreviewTopologyDiagFields[i].journal_event == kPreviewTopologyDiagFields[j].journal_event)
                return false;
    return true;
}
static_assert(DistinctTableNames());

// Independent copy of the reviewed table (design section 3.2), in order.
constexpr std::array<std::pair<std::string_view, std::string_view>, 18> kReviewedFields{{
    {"openAdd", "topo_open_add"}, {"openRemove", "topo_open_remove"},
    {"inventoryAdd", "topo_inventory_add"}, {"inventoryRemove", "topo_inventory_remove"},
    {"inventoryPumps", "topo_inventory_pumps"}, {"snapshotChildren", "topo_snapshot_children"},
    {"snapshotEventIds", "topo_snapshot_event_ids"}, {"snapshotMs", "topo_snapshot_ms"},
    {"postAddKnown", "topo_post_add_known"}, {"postAddUnknown", "topo_post_add_unknown"},
    {"postRemoveKnown", "topo_post_remove_known"}, {"postRemoveUnknown", "topo_post_remove_unknown"},
    {"postAddKnownDistinct", "topo_post_add_known_distinct"}, {"postFirstEventOp", "topo_post_first_event_op"},
    {"postFirstEventMs", "topo_post_first_event_ms"}, {"checkValidBeforePump", "topo_check_valid_before_pump"},
    {"checkSetEqual", "topo_check_set_equal"}, {"checkCurrentCount", "topo_check_current_count"},
}};

void ExpectValues(const PreviewTopologyDiag& actual, const std::array<std::uint32_t, 18>& expected, const char* message) {
    const bool same = actual.values == expected;
    Check(same, message);
    if (same) return;
    for (std::size_t index = 0; index < expected.size(); ++index)
        if (actual.values[index] != expected[index])
            std::cerr << "  " << kPreviewTopologyDiagFields[index].json_key << " actual=" << actual.values[index]
                      << " expected=" << expected[index] << '\n';
}

// T-a: phases and known/unknown classification.
void PhasesAndKnownIds() {
    WorkerTopologyCounters counters;
    counters.Reset(t0);
    counters.Observe(true, 11, t0, true);
    counters.Observe(true, 11, t0, true);
    counters.Observe(false, 99, t0, true);
    counters.BeginInventoryWait();
    counters.CountInventoryPump();
    counters.CountInventoryPump();
    counters.CountInventoryPump();
    counters.Observe(true, 11, t0, true);
    const std::array<std::uint32_t, 2> ids{12, 11};
    counters.Snapshot(2, 2, ids, t0 + milliseconds(468));
    counters.Mark(PreviewTopologyOperation::select);
    counters.Observe(true, 11, t0 + milliseconds(20418), true);
    counters.Observe(true, 11, t0 + milliseconds(20500), true);
    counters.Observe(true, 12, t0 + milliseconds(20600), true);
    counters.Observe(true, 77, t0 + milliseconds(20700), true);
    counters.Observe(false, 12, t0 + milliseconds(20800), true);
    counters.Observe(false, 55, t0 + milliseconds(20900), true);
    counters.RecordCheck(true, false, 3);
    ExpectValues(counters.Values(), {2, 1, 1, 0, 3, 2, 2, 468, 3, 1, 1, 1, 2, 2, 19950, 1, 2, 3},
                 "T-a: every field counts in its own phase and known/unknown follows the snapshot set");
}

// T-b: saturation, clamped milliseconds, serialization and no ID leak.
void SaturationAndSerialization() {
    std::uint32_t value = 4294967294U;
    SaturatingIncrement(value);
    SaturatingIncrement(value);
    Check(value == 4294967295U, "T-b: saturating increment stops at 4294967295");
    Check(SaturatingCount(static_cast<std::size_t>(5)) == 5U, "T-b: a small count is kept");
    Check(ClampTopologyMilliseconds(milliseconds(1LL << 33)) == 4294967295U, "T-b: 2^33 ms clamps to 4294967295");
    Check(ClampTopologyMilliseconds(-milliseconds(5)) == 0U, "T-b: a negative difference is 0");
    Check(ClampTopologyMilliseconds(std::chrono::microseconds(1999)) == 1U, "T-b: milliseconds are truncated");

    PreviewTopologyDiag full;
    full.values.fill(4294967295U);
    const auto text = SerializePreviewTopologyDiag(full);
    std::size_t keys{};
    for (std::size_t index = 0; index < kReviewedFields.size(); ++index) {
        const auto key = "\"" + std::string(kReviewedFields[index].first) + "\":4294967295";
        if (text.find(key) != std::string::npos) ++keys;
    }
    Check(keys == 18 && std::count(text.begin(), text.end(), ':') == 18 && text.front() == '{' && text.back() == '}',
          "T-b: serialization has the 18 keys exactly, each with a value of at most 10 digits");
    Check(text.size() <= 600, "T-b: a fully saturated diag stays within the estimated size");

    WorkerTopologyCounters counters;
    counters.Reset(t0);
    counters.BeginInventoryWait();
    const std::array<std::uint32_t, 2> ids{3735928559U, 7};
    counters.Snapshot(2, 2, ids, t0);
    counters.Mark(PreviewTopologyOperation::frame);
    counters.Observe(true, 3735928559U, t0, true);
    counters.Observe(false, 3735928558U, t0, true);
    const auto serialized = SerializePreviewTopologyDiag(counters.Values());
    Check(serialized.find("3735928559") == std::string::npos && serialized.find("3735928558") == std::string::npos,
          "T-b: no source ID appears in the serialized diag");
    Check(counters.Values().Get(Field::post_add_known) == 1 && counters.Values().Get(Field::post_remove_unknown) == 1,
          "T-b: the large ID is classified, not stored");

    for (std::size_t index = 0; index < kReviewedFields.size(); ++index) {
        Check(kPreviewTopologyDiagFields[index].json_key == kReviewedFields[index].first &&
              kPreviewTopologyDiagFields[index].journal_event == kReviewedFields[index].second,
              "T-b: the production table matches the reviewed table in order");
    }
}

// T-c: freezing, the idle phase, and the thread/idle detection of the first event.
void FreezeIdleAndThread() {
    WorkerTopologyCounters idle;
    idle.Observe(true, 1, t0, true);
    idle.CountInventoryPump();
    idle.RecordCheck(true, true, 2);
    ExpectValues(idle.Values(), {}, "T-c: nothing is counted before Reset (Idle)");

    WorkerTopologyCounters frozen;
    frozen.Reset(t0);
    frozen.Observe(true, 1, t0, true);
    frozen.BeginInventoryWait();
    const std::array<std::uint32_t, 2> ids{1, 2};
    frozen.Snapshot(2, 1, ids, t0 + milliseconds(10));
    frozen.Mark(PreviewTopologyOperation::start);
    frozen.Observe(true, 1, t0 + milliseconds(30), true);
    const auto before = frozen.Values();
    frozen.Mark(PreviewTopologyOperation::close);
    frozen.Observe(false, 2, t0 + milliseconds(40), true);
    frozen.RecordCheck(false, false, 9);
    frozen.Snapshot(9, 9, ids, t0 + milliseconds(50));
    frozen.Reset(t0 + milliseconds(60));
    frozen.CountInventoryPump();
    frozen.Mark(PreviewTopologyOperation::frame);
    Check(frozen.Values() == before, "T-c: after Mark(close) Observe/RecordCheck/Snapshot/Reset change nothing");
    Check(before.Get(Field::post_first_event_op) == 3 && before.Get(Field::post_first_event_ms) == 20,
          "T-c: the first event inside start reports op 3 and its delay");

    WorkerTopologyCounters freeze_call;
    freeze_call.Reset(t0);
    freeze_call.Freeze();
    freeze_call.Observe(true, 1, t0, true);
    freeze_call.Reset(t0);
    freeze_call.Observe(true, 1, t0, true);
    ExpectValues(freeze_call.Values(), {}, "T-c: Freeze() is final, including against Reset");

    WorkerTopologyCounters outside;
    outside.Reset(t0);
    outside.BeginInventoryWait();
    outside.Snapshot(2, 2, ids, t0);
    outside.Mark(PreviewTopologyOperation::select);
    outside.Mark(PreviewTopologyOperation::idle);
    outside.Observe(true, 1, t0 + milliseconds(5), true);
    Check(outside.Values().Get(Field::post_first_event_op) == kTopologyFirstEventOutsideCommand,
          "T-c: an event between commands (idle) reports 8");

    WorkerTopologyCounters other_thread;
    other_thread.Reset(t0);
    other_thread.BeginInventoryWait();
    other_thread.Snapshot(2, 2, ids, t0);
    other_thread.Mark(PreviewTopologyOperation::select);
    other_thread.Observe(true, 1, t0 + milliseconds(5), false);
    Check(other_thread.Values().Get(Field::post_first_event_op) == kTopologyFirstEventOutsideCommand,
          "T-c: an event off the owning thread during select reports 8");

    WorkerTopologyCounters first_only;
    first_only.Reset(t0);
    first_only.BeginInventoryWait();
    first_only.Snapshot(2, 2, ids, t0);
    first_only.Mark(PreviewTopologyOperation::frame);
    first_only.Observe(true, 2, t0 + milliseconds(7), true);
    first_only.Mark(PreviewTopologyOperation::idle);
    first_only.Observe(false, 2, t0 + milliseconds(70), false);
    first_only.Mark(PreviewTopologyOperation::resume);
    first_only.Observe(true, 5, t0 + milliseconds(700), true);
    Check(first_only.Values().Get(Field::post_first_event_op) == 4 &&
          first_only.Values().Get(Field::post_first_event_ms) == 7,
          "T-c: later events never change postFirstEventOp/postFirstEventMs");

    // Reset keeps the running command: the dispatcher marks enumerate before the
    // transport resets inside it, so an event after the snapshot of that same
    // command reports 1, not 8.
    WorkerTopologyCounters enumerate;
    enumerate.Mark(PreviewTopologyOperation::enumerate);
    enumerate.Reset(t0);
    enumerate.BeginInventoryWait();
    enumerate.Snapshot(2, 2, ids, t0);
    enumerate.Observe(true, 1, t0, true);
    Check(enumerate.Values().Get(Field::post_first_event_op) == 1, "T-c: Reset keeps the mark of the running command");

    // RecordCheck before the snapshot is ignored (0 = not checked).
    WorkerTopologyCounters early_check;
    early_check.Reset(t0);
    early_check.RecordCheck(false, false, 4);
    Check(early_check.Values().Get(Field::check_valid_before_pump) == 0 &&
          early_check.Values().Get(Field::check_set_equal) == 0 && early_check.Values().Get(Field::check_current_count) == 0,
          "T-c: 0 means no inventory check was recorded");
}

// The shared lexeme rule (design 4.3 item 6) on its own.
void Uint32Lexeme() {
    std::uint32_t value{};
    for (const auto* accepted : {"0", "1", "4294967295", "42"}) {
        Check(ParseUint32Lexeme(accepted, value), "uint32 lexeme accepted");
    }
    Check(ParseUint32Lexeme("4294967295", value) && value == 4294967295U, "uint32 maximum value round trip");
    for (const auto* rejected : {"", "-1", "1.0", "1e3", "1E3", "4294967296", "18446744073709551616", "01", "00",
                                 "+1", " 1", "12345678901"}) {
        Check(!ParseUint32Lexeme(rejected, value), "non-canonical or out-of-range lexeme rejected");
    }
    Check(TopologyOperationOf("select") == PreviewTopologyOperation::select &&
          TopologyOperationOf("close") == PreviewTopologyOperation::close && !TopologyOperationOf("capture") &&
          !TopologyOperationOf(""), "command names map to their topology codes only");
}
} // namespace

int main() {
    PhasesAndKnownIds();
    SaturationAndSerialization();
    FreezeIdleAndThread();
    Uint32Lexeme();
    std::cout << "{\"mode\":\"worker-topology-counters\",\"failures\":" << failures << "}\n";
    return failures == 0 ? 0 : 1;
}
