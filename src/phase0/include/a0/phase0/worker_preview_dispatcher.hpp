#pragma once
#include "a0/common/protocol_json.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/preview_worker_timing.hpp"
#include <charconv>
#include <functional>

namespace a0::phase0 { class NikonSdkTransport; }

namespace a0::phase0::experimental {
// No hardware CLI exposes this dispatcher yet. Its host must hold the parent's
// process handle and the controller must arm/register before granting commands.
//
// `authority` is the serving authority (live parent, valid delegation, serving
// lifetime not over); OnIdle closes the session only when it is lost.
// `operation_window` is the operation deadline: once it returns false every
// command except close is rejected with worker_authority_expired. An empty
// operation_window leaves `authority` as the only check.
template<class Transport> class WorkerPreviewDispatcher final {
public:
    WorkerPreviewDispatcher(Transport& transport, std::string epoch, std::string capability,
                            std::uint32_t parent_pid, std::uint32_t worker_pid, std::function<bool()> authority,
                            std::function<bool()> operation_window = {})
        : transport_(transport), epoch_(std::move(epoch)), capability_(std::move(capability)),
          parent_pid_(parent_pid), worker_pid_(worker_pid), authority_(std::move(authority)),
          operation_window_(std::move(operation_window)) {
        if (epoch_.empty() || capability_.empty() || !parent_pid_ || !worker_pid_ || !authority_)
            throw TransportError("worker_authority_missing", "worker context required");
    }
    bool PeerAllowed(std::uint32_t pid) const { return pid == parent_pid_; }
    bool ShouldStop() const { return terminal_; }
    bool SafeToExit() const { return closed_ && !handoff_close_unconfirmed_ && (!start_attempted_ || stopped_); }
    bool Failed() const { return failed_; }
    bool Completed() const { return explicit_close_ && !failed_ && SafeToExit(); }
    void TransportFailed() { failed_ = true; CloseForShutdown(); }
    void OnIdle() { if (!terminal_ && !authority_()) { failed_ = true; CloseForShutdown(); } }
    void CloseForShutdown() noexcept {
        terminal_ = true;
        if (close_attempted_) return;
        close_attempted_ = true;
        if (!sdk_attempted_) { source_closed_ = module_closed_ = claim_released_ = closed_ = true; return; }
        if (started_ && !stop_attempted_) {
            stop_attempted_ = true;
            try { transport_.StopLiveView(preview_worker_timing::kStopLiveViewTimeout); stopped_ = true; }
            catch (...) {}
        }
        try {
            transport_.Close(preview_worker_timing::kCloseTimeout);
            const auto state = transport_.InspectDualSessionExitState();
            source_closed_ = !state.source_open;
            module_closed_ = !state.module_retained;
            claim_released_ = !state.process_claim_retained;
            closed_ = state.FullyEnded();
        } catch (...) {}
    }
    std::string Handle(std::string_view wire) {
        namespace json = a0::common::protocol_json;
        namespace timing = preview_worker_timing;
        try {
            if (terminal_) Fail("worker_terminal");
            const auto request = json::BasicJsonParser<Failure>(wire).Parse();
            if (request.kind != json::JsonKind::object || request.object.size() != 6) Fail("worker_envelope");
            auto string = [&](const char* key) -> const std::string& {
                return json::RequireFieldWith<Failure>(request, key, json::JsonKind::string).string;
            };
            if (string("schema") != "a0.preview-worker.v1" || string("epoch") != epoch_ ||
                string("capability") != capability_) Fail("worker_authority");
            const auto& lexeme = json::RequireFieldWith<Failure>(request, "sequence", json::JsonKind::number).string;
            std::uint64_t sequence{};
            const auto parsed = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), sequence);
            if (parsed.ec != std::errc{} || parsed.ptr != lexeme.data() + lexeme.size() ||
                sequence == 0 || sequence != sequence_ + 1) Fail("worker_sequence");
            sequence_ = sequence;
            const auto& operation = string("operation");
            const auto& candidate = string("candidate");
            if (operation != "select" && operation != "resume" && !candidate.empty()) Fail("worker_candidate");
            // Authenticated close remains legal after the operation deadline,
            // for as long as the host still serves (serving lifetime).
            if (operation == "close") {
                explicit_close_ = true; CloseForShutdown();
                return Reply(SafeToExit() ? "closed" : "quarantined", Receipt());
            }
            if (!OperationAllowed()) Fail("worker_authority_expired");
            std::string payload = "null";
            if (operation == "enumerate" && stage_ == Stage::Fresh) {
                sdk_attempted_ = true;
                const auto candidates = transport_.BeginWorkerPreviewSelection(timing::kSdkOperationTimeout);
                if (candidates.size() != 2 || candidates[0] == candidates[1]) Fail("worker_inventory");
                payload = "[\"" + json::JsonEscape(candidates[0]) + "\",\"" + json::JsonEscape(candidates[1]) + "\"]";
                stage_ = Stage::Enumerated;
            } else if (operation == "select" && stage_ == Stage::Enumerated) {
                transport_.OpenWorkerPreviewCandidate(candidate, timing::kSdkOperationTimeout);
                stage_ = Stage::Selected;
            } else if (operation == "start" && (stage_ == Stage::Selected || stage_ == Stage::Resumed)) {
                start_attempted_ = true; stopped_ = false; stop_attempted_ = false;
                transport_.StartSelectedWorkerLiveView(timing::kSdkOperationTimeout);
                started_ = true; stage_ = Stage::Live;
            } else if (operation == "frame" && stage_ == Stage::Live) {
                const auto frame = transport_.ReadLiveViewFrame(timing::kFrameTimeout);
                if (frame.empty() || frame.size() > 256U * 1024U) Fail("worker_frame_size");
                // Hex is bounded to 512 KiB, below the shared pipe's 1 MiB limit.
                static constexpr char digits[] = "0123456789abcdef";
                payload = "\"";
                payload.reserve(frame.size() * 2 + 2);
                for (auto byte : frame) { payload += digits[byte >> 4]; payload += digits[byte & 15]; }
                payload += '"';
            } else if (operation == "suspend" && stage_ == Stage::Live && !handoff_) {
                stop_attempted_ = true;
                transport_.StopLiveView(timing::kStopLiveViewTimeout); stopped_ = true; started_ = false;
                if (!OperationAllowed()) Fail("worker_authority_expired");
                handoff_close_unconfirmed_ = true;
                transport_.SuspendSelectedWorkerPreview(timing::kSdkOperationTimeout);
                handoff_close_unconfirmed_ = false;
                handoff_ = true; stage_ = Stage::Suspended;
            } else if (operation == "resume" && stage_ == Stage::Suspended) {
                transport_.ResumeSelectedWorkerPreview(candidate, timing::kSdkOperationTimeout);
                stage_ = Stage::Resumed;
            } else Fail("worker_operation_unavailable");
            if (!OperationAllowed()) Fail("worker_authority_expired");
            return Reply("ok", payload);
        } catch (const TransportError& error) {
            failed_ = true;
            CloseForShutdown();
            return Reply("failed", "{\"error\":\"" + json::JsonEscape(error.Category()) + "\",\"close\":" + Receipt() + "}");
        } catch (...) {
            failed_ = true;
            CloseForShutdown();
            return Reply("failed", "{\"error\":\"worker_unexpected\",\"close\":" + Receipt() + "}");
        }
    }
private:
    enum class Stage { Fresh, Enumerated, Selected, Live, Suspended, Resumed };
    struct Failure {
        [[noreturn]] static void Fail(std::string_view category, std::string_view message) {
            throw TransportError(std::string(category), std::string(message));
        }
    };
    [[noreturn]] static void Fail(const char* category) { throw TransportError(category, "worker command rejected"); }
    bool OperationAllowed() const { return authority_() && (!operation_window_ || operation_window_()); }
    static std::string Boolean(bool value) { return value ? "true" : "false"; }
    std::string Receipt() const {
        return "{\"liveViewOff\":" + Boolean(!start_attempted_ || stopped_) +
            ",\"sourceClosed\":" + Boolean(source_closed_) + ",\"moduleClosed\":" + Boolean(module_closed_) +
            ",\"processClaimReleased\":" + Boolean(claim_released_) + ",\"safeToExit\":" + Boolean(SafeToExit()) + "}";
    }
    std::string Reply(const char* status, const std::string& payload) const {
        return "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"" + a0::common::protocol_json::JsonEscape(epoch_) +
            "\",\"workerPid\":" + std::to_string(worker_pid_) + ",\"sequence\":" + std::to_string(sequence_) +
            ",\"status\":\"" + status + "\",\"payload\":" + payload + "}";
    }
    Transport& transport_;
    std::string epoch_, capability_;
    std::uint32_t parent_pid_{}, worker_pid_{};
    std::function<bool()> authority_;
    std::function<bool()> operation_window_;
    std::uint64_t sequence_{};
    Stage stage_{Stage::Fresh};
    bool sdk_attempted_{}, start_attempted_{}, started_{}, stop_attempted_{}, stopped_{}, handoff_{};
    bool terminal_{}, close_attempted_{}, closed_{}, source_closed_{}, module_closed_{}, claim_released_{};
    bool failed_{}, handoff_close_unconfirmed_{}, explicit_close_{};
};
struct WorkerPreviewHostResult { int ipc_exit_code; bool safe_to_exit; };
// Internal integration seam only, no public executable entrypoint. Caller must
// retain/quarantine the worker process if safe_to_exit is false.
// operation_deadline: commands other than close are rejected after it.
// serving_lifetime: close is accepted until it; then the host closes itself.
// Requires 0 < operation_deadline < serving_lifetime <= kPreviewSessionLimit.
WorkerPreviewHostResult RunWorkerPreviewNamedPipeServer(
    std::string_view pipe_name, NikonSdkTransport& transport, std::string epoch,
    std::string capability, void* inherited_parent_process, std::chrono::milliseconds operation_deadline,
    std::chrono::milliseconds serving_lifetime, std::function<bool()> delegation_authority);
} // namespace a0::phase0::experimental
