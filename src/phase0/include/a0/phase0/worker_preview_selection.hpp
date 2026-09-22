#pragma once
#include "a0/phase0/phase0.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0 {
// Worker/module-local candidates, NOT CAM-A/B or physical identity. Construction
// performs no Source Open. A token cannot be reused in another module generation.
class WorkerPreviewSelection final {
public:
    WorkerPreviewSelection(std::string generation, std::vector<std::uint32_t> inventory)
        : inventory_(std::move(inventory)) {
        std::sort(inventory_.begin(), inventory_.end());
        if (generation.empty() || inventory_.size() != 2 || inventory_[0] == inventory_[1])
            throw TransportError("worker_inventory_not_pair", "exactly two distinct raw sources are required");
        for (std::size_t i = 0; i < inventory_.size(); ++i)
            tokens_.push_back("preview-candidate-" + generation + "-" + std::to_string(i));
    }
    const std::vector<std::string>& Tokens() const noexcept { return tokens_; }
    void Invalidate() noexcept { valid_ = false; }
    bool Opened() const noexcept { return valid_ && opened_; }
    // A child add/remove is not an identity proof.  In particular, an Add for
    // an already-enumerated ID could be a disconnect/reconnect which happened
    // between polls.  Consume this selection on every topology event rather
    // than attempting to recover or reinterpret it.
    void ObserveTopology(bool /*added*/, std::uint32_t /*id*/) noexcept { Invalidate(); }
    void CheckInventory(std::vector<std::uint32_t> current) {
        std::sort(current.begin(), current.end());
        if (!valid_ || current != inventory_) Reject("worker_selection_invalidated");
    }
    template<class Open>
    void OpenSelected(std::string_view token, std::vector<std::uint32_t> current, Open&& open) {
        CheckInventory(std::move(current));
        const auto found = std::find(tokens_.begin(), tokens_.end(), token);
        if (opened_ || found == tokens_.end()) Reject("worker_candidate_unavailable");
        opened_ = true; // Consumed before SDK entry; no retry after unknown completion.
        try { open(inventory_[static_cast<std::size_t>(found - tokens_.begin())]); }
        catch (...) { Invalidate(); throw; }
    }
private:
    [[noreturn]] void Reject(const char* reason) {
        Invalidate(); throw TransportError(reason, "worker-local preview selection rejected");
    }
    std::vector<std::uint32_t> inventory_;
    std::vector<std::string> tokens_;
    bool valid_{true}, opened_{};
};
} // namespace a0::phase0
