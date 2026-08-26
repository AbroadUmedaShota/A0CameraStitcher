#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace a0::phase0 {

class SdkBufferArenaCapacityExceeded final : public std::runtime_error {
public:
    SdkBufferArenaCapacityExceeded()
        : std::runtime_error("SDK buffer arena capacity was exhausted") {}
};

// Owns every payload address handed to the vendor SDK. Command buffers are
// reusable only after completion evidence; an unproven buffer is quarantined
// without allocation or failure. Session buffers (callbacks, etc.) always
// survive until ReleaseAfterSdkUnload().
class SdkBufferArena final {
public:
    static constexpr std::size_t kMaximumSlots = 256;

    class CommandLease final {
    public:
        CommandLease() noexcept = default;
        ~CommandLease() noexcept { Reset(); }
        CommandLease(const CommandLease&) = delete;
        CommandLease& operator=(const CommandLease&) = delete;

        CommandLease(CommandLease&& other) noexcept { MoveFrom(other); }
        CommandLease& operator=(CommandLease&& other) noexcept {
            if (this != &other) {
                Reset();
                MoveFrom(other);
            }
            return *this;
        }

        [[nodiscard]] void* data() const noexcept { return data_; }

        template <typename T>
        [[nodiscard]] T* as() const noexcept {
            static_assert(std::is_trivially_destructible_v<T>,
                "SDK arena payloads must be trivially destructible");
            return static_cast<T*>(data_);
        }

        // Call only when synchronous rejection or the completion callback
        // proves the SDK can no longer touch this payload.
        void ConfirmCompletion() noexcept { completion_proven_ = true; }

    private:
        friend class SdkBufferArena;
        CommandLease(SdkBufferArena& arena, std::size_t slot, void* data) noexcept
            : arena_(&arena), slot_(slot), data_(data) {}

        void Reset() noexcept {
            if (arena_ == nullptr) return;
            if (completion_proven_) arena_->ReleaseCompleted(slot_);
            else arena_->Quarantine(slot_);
            arena_ = nullptr;
            data_ = nullptr;
        }

        void MoveFrom(CommandLease& other) noexcept {
            arena_ = std::exchange(other.arena_, nullptr);
            slot_ = other.slot_;
            data_ = std::exchange(other.data_, nullptr);
            completion_proven_ = other.completion_proven_;
        }

        SdkBufferArena* arena_{nullptr};
        std::size_t slot_{};
        void* data_{nullptr};
        bool completion_proven_{false};
    };

    SdkBufferArena() = default;
    ~SdkBufferArena() noexcept { ReleaseAfterSdkUnload(); }
    SdkBufferArena(const SdkBufferArena&) = delete;
    SdkBufferArena& operator=(const SdkBufferArena&) = delete;

    template <typename T>
    [[nodiscard]] CommandLease AcquireCommand(std::size_t count = 1) {
        static_assert(std::is_trivially_destructible_v<T>,
            "SDK arena payloads must be trivially destructible");
        AssertOwnerThread();
        const auto slot = FindFreeSlot();
        T* allocation = new T[count]{};
        slots_[slot] = Slot{
            allocation,
            [](void* pointer) noexcept { delete[] static_cast<T*>(pointer); },
            SlotState::active_command,
        };
        return CommandLease(*this, slot, allocation);
    }

    template <typename T>
    [[nodiscard]] T* AcquireSession(std::size_t count = 1) {
        static_assert(std::is_trivially_destructible_v<T>,
            "SDK arena payloads must be trivially destructible");
        AssertOwnerThread();
        const auto slot = FindFreeSlot();
        T* allocation = new T[count]{};
        slots_[slot] = Slot{
            allocation,
            [](void* pointer) noexcept { delete[] static_cast<T*>(pointer); },
            SlotState::session,
        };
        return allocation;
    }

    // Must be called only after the vendor module and its transport DLL are
    // unloaded. This is the sole release path for quarantined/session buffers.
    void ReleaseAfterSdkUnload() noexcept {
        AssertOwnerThread();
        for (auto& slot : slots_) {
            if (slot.state == SlotState::free) continue;
            slot.destroy(slot.allocation);
            slot = {};
        }
#ifndef NDEBUG
        owner_thread_ = {};
#endif
    }

    void AssertOwnerThread() const noexcept {
#ifndef NDEBUG
        const auto current = std::this_thread::get_id();
        if (owner_thread_ == std::thread::id{}) owner_thread_ = current;
        assert(owner_thread_ == current && "SDK buffer arena must be used serially on one thread");
#endif
    }

private:
    enum class SlotState { free, active_command, quarantined, session };
    struct Slot {
        void* allocation{nullptr};
        void (*destroy)(void*) noexcept{nullptr};
        SlotState state{SlotState::free};
    };

    [[nodiscard]] std::size_t FindFreeSlot() const {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].state == SlotState::free) return index;
        }
        throw SdkBufferArenaCapacityExceeded();
    }

    void ReleaseCompleted(std::size_t slot_index) noexcept {
        AssertOwnerThread();
        auto& slot = slots_[slot_index];
        assert(slot.state == SlotState::active_command);
        slot.destroy(slot.allocation);
        slot = {};
    }

    // No capacity check, allocation, or destruction is allowed here. Once a
    // command becomes uncertain, retaining its pointer outranks availability.
    void Quarantine(std::size_t slot_index) noexcept {
        AssertOwnerThread();
        auto& slot = slots_[slot_index];
        assert(slot.state == SlotState::active_command);
        slot.state = SlotState::quarantined;
    }

    std::array<Slot, kMaximumSlots> slots_{};
#ifndef NDEBUG
    mutable std::thread::id owner_thread_{};
#endif
};

} // namespace a0::phase0
