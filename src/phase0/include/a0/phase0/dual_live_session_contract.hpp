#pragma once

// Offline contract only. This file cannot acquire a hardware lease, open SDK /
// WPD, authenticate an OS process, or establish physical camera identity.
#include "a0/phase0/dual_live_worker_poc.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::phase0::experimental {
enum class SessionStage { Collecting, SimulatedReady, Previewing, Quiescing, Closed, Invalid };
enum class LeaseObservation { HeldByController, Missing, Abandoned };
enum class Operation { SimulatedPreview, RealSdk, Wpd, Capture };

// Source numbers never leave the worker-local selection object. Even the same
// numeric Source in two workers can mean different cameras (and vice versa).
class WorkerLocalSelection final {
public:
    WorkerLocalSelection(std::string instance, std::string epoch,
                         std::vector<std::uint32_t> inventory)
        : instance_(std::move(instance)), epoch_(std::move(epoch)), inventory_(std::move(inventory)) {}

    bool Select(std::string_view instance, std::string_view epoch, std::uint32_t source) {
        if (!valid_ || selected_ || instance != instance_ || epoch != epoch_ ||
            instance_.empty() || epoch_.empty()) return Reject();
        selected_ = SelectAssignedSourceBeforeOpen(inventory_, source);
        return selected_.has_value() || Reject();
    }
    // Callback is the fake transport's observable Open seam, not an SDK call.
    template<class Open>
    bool OpenSelected(std::string_view instance, std::string_view epoch, Open&& open) {
        if (!valid_ || opened_ || !selected_ || instance != instance_ || epoch != epoch_) return Reject();
        opened_ = true; // An ambiguous/throwing Open must never be retried.
        try { if (!open(*selected_)) return Reject(); }
        catch (...) { Reject(); throw; }
        return true;
    }
    void Invalidate() noexcept { valid_ = false; selected_.reset(); }
private:
    bool Reject() noexcept { Invalidate(); return false; }
    std::string instance_, epoch_;
    std::vector<std::uint32_t> inventory_;
    std::optional<std::uint32_t> selected_;
    bool valid_ = true, opened_ = false;
};

// Synthetic receipt supplied by a fake camera/operator, never real SDK proof.
// fake_body is an independently supplied fixture identity, NOT a Source number.
struct SimulatedBindingReceipt {
    std::string alias, instance, epoch, receipt, fake_body;
};
struct PreviewGrant {
    std::string controller, epoch, alias, instance, receipt, capability;
    std::uint64_t issued_at_ms{};
    std::uint64_t deadline_ms{};
    bool operator==(const PreviewGrant&) const = default;
};

class DualLiveSessionContract final {
public:
    DualLiveSessionContract(std::string controller, std::string epoch)
        : controller_(std::move(controller)), epoch_(std::move(epoch)) {
        if (controller_.empty() || epoch_.empty()) Invalidate("invalid_session");
    }
    SessionStage Stage() const noexcept { return stage_; }
    std::string_view Failure() const noexcept { return failure_; }
    bool HardwareAllowed() const noexcept { return false; }
    bool AllCloseReceiptsObserved() const noexcept { return stage_ == SessionStage::Closed; }

    // Observation is injected by the simulator. Real integration must derive
    // it from the controller that still owns HardwareProcessLease, never IPC.
    bool ObserveLease(LeaseObservation observation) {
        if (Terminal()) return false;
        if (observation != LeaseObservation::HeldByController)
            return Invalidate(observation == LeaseObservation::Abandoned ? "lease_abandoned" : "lease_lost");
        lease_observed_ = true;
        return true;
    }
    bool Bind(const SimulatedBindingReceipt& receipt) {
        if (stage_ != SessionStage::Collecting || !lease_observed_) return Invalidate("binding_state");
        const auto slot = Slot(receipt.alias);
        if (!slot || receipt.epoch != epoch_ || receipt.instance.empty() || receipt.receipt.empty() ||
            receipt.fake_body.empty() || bindings_[*slot]) return Invalidate("invalid_binding");
        const auto other = 1U - *slot;
        if (bindings_[other] && (bindings_[other]->instance == receipt.instance ||
            bindings_[other]->receipt == receipt.receipt || bindings_[other]->fake_body == receipt.fake_body))
            return Invalidate("duplicate_binding");
        bindings_[*slot] = receipt;
        if (bindings_[0] && bindings_[1]) stage_ = SessionStage::SimulatedReady;
        return true;
    }
    std::optional<PreviewGrant> Grant(std::string_view alias, std::string capability,
                                      std::uint64_t now_ms, std::uint64_t deadline_ms) {
        if (!ObserveTime(now_ms)) return std::nullopt;
        const auto slot = Slot(alias);
        if (!slot || !lease_observed_ || (stage_ != SessionStage::SimulatedReady && stage_ != SessionStage::Previewing) ||
            !bindings_[*slot] || grants_[*slot] || capability.empty() || deadline_ms <= now_ms ||
            (grants_[1U - *slot] && grants_[1U - *slot]->capability == capability)) {
            Invalidate("invalid_grant"); return std::nullopt;
        }
        const auto& binding = *bindings_[*slot];
        PreviewGrant grant{controller_, epoch_, binding.alias, binding.instance, binding.receipt,
                           std::move(capability), now_ms, deadline_ms};
        grants_[*slot] = grant;
        stage_ = SessionStage::Previewing;
        return grant;
    }
    bool Authorize(const PreviewGrant& grant, Operation operation, std::uint64_t sequence,
                   std::uint64_t now_ms, LeaseObservation lease) {
        if (operation != Operation::SimulatedPreview) return Invalidate("hardware_forbidden");
        if (!ObserveLease(lease)) return false;
        if (!ObserveTime(now_ms)) return false;
        const auto slot = Slot(grant.alias);
        if (stage_ != SessionStage::Previewing || !slot || !grants_[*slot] ||
            *grants_[*slot] != grant || now_ms < grant.issued_at_ms || now_ms >= grant.deadline_ms ||
            sequence == 0 || sequence <= sequence_[*slot]) return Invalidate("invalid_authority");
        sequence_[*slot] = sequence;
        return true;
    }
    bool BeginQuiesce() {
        if (stage_ != SessionStage::Previewing || !grants_[0] || !grants_[1]) return Invalidate("quiesce_state");
        stage_ = SessionStage::Quiescing;
        return true;
    }
    bool ConfirmClosed(const PreviewGrant& grant, bool preview_stopped, bool source_closed, bool module_closed) {
        const auto slot = Slot(grant.alias);
        if (stage_ != SessionStage::Quiescing || !slot || !grants_[*slot] || *grants_[*slot] != grant ||
            closed_[*slot] || !preview_stopped || !source_closed || !module_closed) return Invalidate("close_unconfirmed");
        closed_[*slot] = true;
        if (closed_[0] && closed_[1]) {
            stage_ = SessionStage::Closed;
            bindings_ = {}; // Old assignment is never reused after module close.
            grants_ = {};
        }
        return true;
    }
    void WorkerExited(std::string_view /*instance*/) { Invalidate("worker_exit_not_close_proof"); }
    void TopologyChanged() { Invalidate("topology_changed"); }
    void ControllerExited() { Invalidate("controller_exited"); }

private:
    bool ObserveTime(std::uint64_t now_ms) {
        if (Terminal()) return false;
        if (last_time_ms_ && now_ms < *last_time_ms_) return Invalidate("clock_regressed");
        last_time_ms_ = now_ms;
        return true;
    }
    static std::optional<std::size_t> Slot(std::string_view alias) {
        if (alias == "CAM-A") return 0;
        if (alias == "CAM-B") return 1;
        return std::nullopt;
    }
    bool Terminal() const noexcept { return stage_ == SessionStage::Invalid || stage_ == SessionStage::Closed; }
    bool Invalidate(std::string_view reason) {
        // Completed shutdown is immutable; late notifications cannot erase it.
        if (stage_ == SessionStage::Closed) return false;
        if (failure_.empty()) failure_ = reason;
        stage_ = SessionStage::Invalid;
        lease_observed_ = false;
        grants_ = {}; bindings_ = {};
        return false;
    }
    std::string controller_, epoch_, failure_;
    SessionStage stage_ = SessionStage::Collecting;
    bool lease_observed_ = false;
    std::array<std::optional<SimulatedBindingReceipt>, 2> bindings_;
    std::array<std::optional<PreviewGrant>, 2> grants_;
    std::array<std::uint64_t, 2> sequence_{};
    std::array<bool, 2> closed_{};
    std::optional<std::uint64_t> last_time_ms_;
};
} // namespace a0::phase0::experimental
