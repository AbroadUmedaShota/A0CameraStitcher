#pragma once

#include "a0/phase0/preview_topology_diag.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace a0::phase0::experimental {

// Parenthesized so a Windows max() macro cannot expand here.
inline constexpr std::uint32_t kTopologyValueMax = (std::numeric_limits<std::uint32_t>::max)();

// Counts, repetitions and sizes never wrap: they stop at 4294967295.
constexpr void SaturatingIncrement(std::uint32_t& value) noexcept {
    if (value != kTopologyValueMax) ++value;
}
[[nodiscard]] constexpr std::uint32_t SaturatingCount(std::size_t value) noexcept {
    return value >= kTopologyValueMax ? kTopologyValueMax : static_cast<std::uint32_t>(value);
}
// Whole milliseconds, truncated; a negative difference is 0 and anything past
// the uint32 range is 4294967295.
[[nodiscard]] constexpr std::uint32_t ClampTopologyMilliseconds(std::chrono::steady_clock::duration elapsed) noexcept {
    if (elapsed <= std::chrono::steady_clock::duration::zero()) return 0;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return milliseconds >= static_cast<std::int64_t>(kTopologyValueMax) ? kTopologyValueMax
                                                                        : static_cast<std::uint32_t>(milliseconds);
}

// Worker-side state machine behind PreviewTopologyDiag. Fed only from the
// module event callback, the inventory wait and the inventory checks of the SDK
// transport, and told the running command by the dispatcher.
//
//   Idle --Reset--> Opening --BeginInventoryWait--> InventoryWait --Snapshot--> PostSnapshot
//   any phase --Freeze / Mark(close)--> Frozen (nothing changes afterwards, Reset included)
//
// Every member is noexcept, allocates nothing and never logs, so Observe can run
// inside the SDK callback. Source IDs are kept only as the (at most two)
// snapshot IDs needed to tell known from unknown; Values() returns numbers only.
// Not synchronized: the transport calls it from its owning thread, the same
// assumption its module_sources_ bookkeeping already makes.
class WorkerTopologyCounters final {
public:
    using Clock = std::chrono::steady_clock;

    void Reset(Clock::time_point now) noexcept {
        if (phase_ == Phase::frozen) return;
        values_ = {};
        known_ = {};
        known_count_ = 0;
        known_add_mask_ = 0;
        first_event_seen_ = false;
        reset_time_ = now;
        snapshot_time_ = now;
        // current_op_ is kept: the dispatcher marks enumerate before the
        // transport resets the counters inside that command.
        phase_ = Phase::opening;
    }

    void BeginInventoryWait() noexcept {
        if (phase_ == Phase::opening) phase_ = Phase::inventory_wait;
    }

    void CountInventoryPump() noexcept {
        if (phase_ == Phase::inventory_wait) SaturatingIncrement(At(PreviewTopologyField::inventory_pumps));
    }

    // `ids` is the inventory the selection is built from. At most the two
    // smallest distinct IDs are kept; a selection needs exactly two, so with any
    // other count the selection fails right after this call.
    void Snapshot(std::uint32_t children, std::uint32_t event_ids, std::span<const std::uint32_t> ids,
                  Clock::time_point now) noexcept {
        if (phase_ != Phase::opening && phase_ != Phase::inventory_wait) return;
        At(PreviewTopologyField::snapshot_children) = children;
        At(PreviewTopologyField::snapshot_event_ids) = event_ids;
        At(PreviewTopologyField::snapshot_ms) = ClampTopologyMilliseconds(now - reset_time_);
        known_count_ = 0;
        for (const auto id : ids) Remember(id);
        snapshot_time_ = now;
        phase_ = Phase::post_snapshot;
    }

    void Mark(PreviewTopologyOperation operation) noexcept {
        if (phase_ == Phase::frozen) return;
        if (operation == PreviewTopologyOperation::close) {
            Freeze();
            return;
        }
        current_op_ = operation;
    }

    void Observe(bool added, std::uint32_t id, Clock::time_point now, bool on_owner_thread) noexcept {
        switch (phase_) {
        case Phase::opening:
            SaturatingIncrement(At(added ? PreviewTopologyField::open_add : PreviewTopologyField::open_remove));
            return;
        case Phase::inventory_wait:
            SaturatingIncrement(At(added ? PreviewTopologyField::inventory_add : PreviewTopologyField::inventory_remove));
            return;
        case Phase::post_snapshot:
            ObservePostSnapshot(added, id, now, on_owner_thread);
            return;
        case Phase::idle:
        case Phase::frozen:
            return;
        }
    }

    // One inventory check: whether the selection was still valid before the
    // check pumped the module, whether the current set equals the snapshot,
    // and the size of the current set. The last check wins.
    void RecordCheck(bool valid_before_pump, bool set_equal, std::size_t current_count) noexcept {
        if (phase_ != Phase::post_snapshot) return;
        At(PreviewTopologyField::check_valid_before_pump) = valid_before_pump ? 1U : 2U;
        At(PreviewTopologyField::check_set_equal) = set_equal ? 1U : 2U;
        At(PreviewTopologyField::check_current_count) = SaturatingCount(current_count);
    }

    void Freeze() noexcept { phase_ = Phase::frozen; }

    [[nodiscard]] PreviewTopologyDiag Values() const noexcept { return values_; }

private:
    enum class Phase { idle, opening, inventory_wait, post_snapshot, frozen };

    std::uint32_t& At(PreviewTopologyField field) noexcept { return values_.values[static_cast<std::size_t>(field)]; }

    void Remember(std::uint32_t id) noexcept {
        for (std::size_t index = 0; index < known_count_; ++index)
            if (known_[index] == id) return;
        if (known_count_ < known_.size()) {
            known_[known_count_++] = id;
        } else if (id < known_[1]) {
            known_[1] = id;
        } else {
            return;
        }
        if (known_count_ == 2 && known_[0] > known_[1]) std::swap(known_[0], known_[1]);
    }

    [[nodiscard]] int KnownIndex(std::uint32_t id) const noexcept {
        for (std::size_t index = 0; index < known_count_; ++index)
            if (known_[index] == id) return static_cast<int>(index);
        return -1;
    }

    void ObservePostSnapshot(bool added, std::uint32_t id, Clock::time_point now, bool on_owner_thread) noexcept {
        const int known = KnownIndex(id);
        if (added && known >= 0) {
            SaturatingIncrement(At(PreviewTopologyField::post_add_known));
            known_add_mask_ |= 1U << known;
            At(PreviewTopologyField::post_add_known_distinct) = static_cast<std::uint32_t>(std::popcount(known_add_mask_));
        } else if (added) {
            SaturatingIncrement(At(PreviewTopologyField::post_add_unknown));
        } else {
            SaturatingIncrement(At(known >= 0 ? PreviewTopologyField::post_remove_known
                                              : PreviewTopologyField::post_remove_unknown));
        }
        if (first_event_seen_) return;
        first_event_seen_ = true;
        At(PreviewTopologyField::post_first_event_op) =
            !on_owner_thread || current_op_ == PreviewTopologyOperation::idle
                ? kTopologyFirstEventOutsideCommand : static_cast<std::uint32_t>(current_op_);
        At(PreviewTopologyField::post_first_event_ms) = ClampTopologyMilliseconds(now - snapshot_time_);
    }

    PreviewTopologyDiag values_{};
    std::array<std::uint32_t, 2> known_{};
    std::size_t known_count_{};
    unsigned known_add_mask_{};
    bool first_event_seen_{};
    Phase phase_{Phase::idle};
    PreviewTopologyOperation current_op_{PreviewTopologyOperation::idle};
    Clock::time_point reset_time_{};
    Clock::time_point snapshot_time_{};
};

} // namespace a0::phase0::experimental
