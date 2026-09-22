#pragma once
#include "a0/common/protocol_json.hpp"
#include "a0/phase0/phase0.hpp"
#include <array>
#include <functional>

namespace a0::phase0::experimental {
// The alias is the operator's observation of a physical body, NOT a Source ID,
// candidate ordinal, or a claim of SDK/WPD capture binding.
enum class ObservedPreviewBody { CameraA, CameraB };
class PreviewCommissioning final {
public:
    using Value = a0::common::protocol_json::JsonValue;
    using Send = std::function<Value(std::size_t, std::string_view, std::string_view)>;
    explicit PreviewCommissioning(Send send) : send_(std::move(send)) {}
    std::array<std::string, 2> Enumerate(std::size_t worker) {
        return Guard([&] {
            Require(worker < 2 && stage_[worker] == Stage::Fresh);
            // No second worker may open while the first source is still live.
            Require(worker == 0 || stage_[0] == Stage::Suspended);
            const auto value = send_(worker, "enumerate", "");
            Require(value.kind == Kind::array && value.array.size() == 2);
            for (std::size_t i = 0; i < 2; ++i) {
                Require(value.array[i].kind == Kind::string && !value.array[i].string.empty() && value.array[i].string.size() <= 256);
                candidates_[worker][i] = value.array[i].string;
            }
            Require(candidates_[worker][0] != candidates_[worker][1]);
            stage_[worker] = Stage::Enumerated;
            return candidates_[worker];
        });
    }
    std::vector<unsigned char> Preview(std::size_t worker, std::string_view candidate) {
        return Guard([&] {
            Require(worker < 2 && stage_[worker] == Stage::Enumerated);
            Require(candidate == candidates_[worker][0] || candidate == candidates_[worker][1]);
            selected_[worker] = candidate;
            Empty(send_(worker, "select", candidate));
            Empty(send_(worker, "start", ""));
            auto bytes = Frame(send_(worker, "frame", ""));
            stage_[worker] = Stage::Observed;
            return bytes;
        });
    }
    void ConfirmAndSuspend(std::size_t worker, ObservedPreviewBody body) {
        Guard([&] {
            Require(worker < 2 && stage_[worker] == Stage::Observed);
            Require(body == ObservedPreviewBody::CameraA || body == ObservedPreviewBody::CameraB);
            Require(worker == 0 || body != bodies_[0]);
            Empty(send_(worker, "suspend", ""));
            bodies_[worker] = body;
            stage_[worker] = Stage::Suspended;
        });
    }
    void StartBoth() {
        Guard([&] {
            Require(stage_[0] == Stage::Suspended && stage_[1] == Stage::Suspended && bodies_[0] != bodies_[1]);
            for (std::size_t i = 0; i < 2; ++i) {
                Empty(send_(i, "resume", selected_[i]));
                Empty(send_(i, "start", ""));
                stage_[i] = Stage::Concurrent;
            }
        });
    }
    std::vector<unsigned char> Read(ObservedPreviewBody body) {
        return Guard([&] {
            Require(stage_[0] == Stage::Concurrent && stage_[1] == Stage::Concurrent);
            Require(body == ObservedPreviewBody::CameraA || body == ObservedPreviewBody::CameraB);
            return Frame(send_(bodies_[0] == body ? 0 : 1, "frame", ""));
        });
    }
    void End() noexcept { terminal_ = true; }
private:
    using Kind = a0::common::protocol_json::JsonKind;
    enum class Stage { Fresh, Enumerated, Observed, Suspended, Concurrent };
    static void Require(bool ok) { if (!ok) throw TransportError("preview_commissioning_rejected", "preview workflow rejected"); }
    static void Empty(const Value& value) { Require(value.kind == Kind::null_value); }
    static std::vector<unsigned char> Frame(const Value& value) {
        Require(value.kind == Kind::string && !value.string.empty() && value.string.size() <= 512U * 1024U && value.string.size() % 2 == 0);
        auto nibble = [](char c) -> unsigned char {
            Require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            return static_cast<unsigned char>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        std::vector<unsigned char> result;
        result.reserve(value.string.size() / 2);
        for (std::size_t i = 0; i < value.string.size(); i += 2)
            result.push_back(static_cast<unsigned char>((nibble(value.string[i]) << 4) | nibble(value.string[i + 1])));
        return result; // Preview bytes only; display/decode is the caller's responsibility.
    }
    template<class Action> auto Guard(Action action) -> decltype(action()) {
        try { Require(!terminal_); return action(); }
        catch (...) { terminal_ = true; throw; }
    }
    Send send_;
    std::array<Stage, 2> stage_{};
    std::array<std::array<std::string, 2>, 2> candidates_;
    std::array<std::string, 2> selected_;
    std::array<ObservedPreviewBody, 2> bodies_{};
    bool terminal_{};
};
} // namespace a0::phase0::experimental
