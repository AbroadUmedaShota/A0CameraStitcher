// SdkBufferArena contracts (GitHub Issue #145, stages 2 and 3).
//
// These tests use only the public, header-only ownership API. They do not
// load the Nikon SDK or interact with a camera.

#include "a0/phase0/sdk_buffer_arena.hpp"

#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace a0::phase0;

static_assert(std::is_nothrow_destructible_v<SdkBufferArena::CommandLease>);
static_assert(std::is_nothrow_move_constructible_v<SdkBufferArena::CommandLease>);
static_assert(std::is_nothrow_move_assignable_v<SdkBufferArena::CommandLease>);

namespace {

int failures = 0;

void Check(const bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Action>
bool ThrowsCapacityExceeded(Action&& action) {
    try {
        std::forward<Action>(action)();
    } catch (const SdkBufferArenaCapacityExceeded&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void ConfirmedCommandsReleaseTheirSlots() {
    SdkBufferArena arena;
    bool all_acquired = true;
    try {
        for (std::size_t index = 0; index < SdkBufferArena::kMaximumSlots * 3; ++index) {
            auto lease = arena.AcquireCommand<unsigned long>();
            *lease.as<unsigned long>() = static_cast<unsigned long>(index);
            lease.ConfirmCompletion();
        }
    } catch (...) {
        all_acquired = false;
    }
    Check(all_acquired,
        "confirmed command leases release their slots and do not exhaust the arena across repeated acquisitions");
}

void UnconfirmedCommandsQuarantineUnconditionally() {
    SdkBufferArena arena;
    for (std::size_t index = 0; index < SdkBufferArena::kMaximumSlots; ++index) {
        auto lease = arena.AcquireCommand<unsigned char>();
        *lease.as<unsigned char>() = static_cast<unsigned char>(index);
    }

    Check(ThrowsCapacityExceeded([&] { (void)arena.AcquireCommand<unsigned char>(); }),
        "destroying unconfirmed leases quarantines every slot and the next acquisition fails closed");
}

void QuarantinedSlotsAreReusableOnlyAfterSdkUnloadRelease() {
    SdkBufferArena arena;
    for (std::size_t index = 0; index < SdkBufferArena::kMaximumSlots; ++index) {
        auto lease = arena.AcquireCommand<unsigned char>();
        *lease.as<unsigned char>() = static_cast<unsigned char>(index);
    }
    Check(ThrowsCapacityExceeded([&] { (void)arena.AcquireCommand<unsigned char>(); }),
        "quarantined slots remain unavailable before ReleaseAfterSdkUnload");

    arena.ReleaseAfterSdkUnload();

    bool acquired_after_release = false;
    try {
        auto lease = arena.AcquireCommand<unsigned char>();
        lease.ConfirmCompletion();
        acquired_after_release = true;
    } catch (...) {
    }
    Check(acquired_after_release,
        "ReleaseAfterSdkUnload makes quarantined slots available to a later session");
}

void SessionAllocationsRemainUntilSdkUnloadRelease() {
    SdkBufferArena arena;
    unsigned long* first = nullptr;
    for (std::size_t index = 0; index < SdkBufferArena::kMaximumSlots; ++index) {
        auto* value = arena.AcquireSession<unsigned long>();
        *value = static_cast<unsigned long>(index + 1);
        if (index == 0) first = value;
    }
    Check(first != nullptr && *first == 1,
        "session allocations remain valid while the SDK session is loaded");
    Check(ThrowsCapacityExceeded([&] { (void)arena.AcquireSession<unsigned long>(); }),
        "session allocations consume the bounded arena and excess acquisition fails closed");

    arena.ReleaseAfterSdkUnload();

    bool acquired_after_release = false;
    try {
        auto* value = arena.AcquireSession<unsigned long>();
        *value = 42;
        acquired_after_release = (*value == 42);
    } catch (...) {
    }
    Check(acquired_after_release,
        "ReleaseAfterSdkUnload makes session allocation slots available to a later session");
}

void MovingALeaseDoesNotReleaseItsSlotTwice() {
    SdkBufferArena arena;
    {
        auto original = arena.AcquireCommand<unsigned long>();
        auto moved = std::move(original);
        moved.ConfirmCompletion();
    }

    bool all_slots_available = true;
    std::vector<SdkBufferArena::CommandLease> leases;
    leases.reserve(SdkBufferArena::kMaximumSlots);
    try {
        for (std::size_t index = 0; index < SdkBufferArena::kMaximumSlots; ++index) {
            leases.push_back(arena.AcquireCommand<unsigned long>());
        }
    } catch (...) {
        all_slots_available = false;
    }
    for (auto& lease : leases) lease.ConfirmCompletion();
    Check(all_slots_available,
        "moving a command lease transfers sole ownership without losing or releasing a slot twice");
}

} // namespace

int main() {
    ConfirmedCommandsReleaseTheirSlots();
    UnconfirmedCommandsQuarantineUnconditionally();
    QuarantinedSlotsAreReusableOnlyAfterSdkUnloadRelease();
    SessionAllocationsRemainUntilSdkUnloadRelease();
    MovingALeaseDoesNotReleaseItsSlotTwice();

    if (failures != 0) {
        std::cerr << failures << " SdkBufferArena contract failures\n";
        return 1;
    }
    std::cout << "SdkBufferArena contracts passed\n";
    return 0;
}
