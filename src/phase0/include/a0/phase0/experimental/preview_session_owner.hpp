#pragma once
// Who may send to one camera's open WPD session (#426 stage 4, ADR-0044).
//
// A session has exactly one owner at a time. The Live worker owns it while it
// reads frames. To lend the session to a short transaction, the worker is first
// asked to hold (HoldRequested); it finishes the frame transfer it is in, answers
// from the boundary between two frames (Held) and sends nothing afterwards. Only
// then can the session be lent (Lent) to one borrower, which proves itself with a
// token. A stale token never matches again, so a borrower that was told to give
// the session back cannot send by mistake.
//
// State and token share one atomic word, so every change is one compare-and-swap
// and two owners can never both pass a check. This class touches no camera.
#include <atomic>
#include <cstdint>
#include <optional>

namespace a0::phase0::experimental {
enum class PreviewSessionState : unsigned { Live = 0, HoldRequested = 1, Held = 2, Lent = 3, Retired = 4 };
class PreviewSessionOwner final {
public:
    using Token = std::uint64_t;
    [[nodiscard]] PreviewSessionState State() const noexcept { return StateOf(word_.load()); }
    // The Live path may send in these two states: HoldRequested still lets the frame in flight finish.
    [[nodiscard]] bool MayLiveSend() const noexcept {
        const auto state = State();
        return state == PreviewSessionState::Live || state == PreviewSessionState::HoldRequested;
    }
    // Focus, AF-frame and capture requests are accepted only while the Live worker owns the session.
    [[nodiscard]] bool AdmitsFocus() const noexcept { return State() == PreviewSessionState::Live; }
    // Window side. Live -> HoldRequested. False when the session is in any other state.
    bool RequestHold() noexcept { return Change(PreviewSessionState::Live, PreviewSessionState::HoldRequested); }
    // Worker side, at a frame boundary. HoldRequested -> Held. False when the request was withdrawn.
    bool AcknowledgeHold() noexcept { return Change(PreviewSessionState::HoldRequested, PreviewSessionState::Held); }
    // Borrower side. Held -> Lent, with a fresh token. Empty when the session is not held.
    [[nodiscard]] std::optional<Token> Lend() noexcept {
        auto current = word_.load();
        for (;;) {
            if (StateOf(current) != PreviewSessionState::Held) return std::nullopt;
            const Token next = TokenOf(current) + 1;
            if (word_.compare_exchange_weak(current, Pack(next, PreviewSessionState::Lent))) return next;
        }
    }
    // True only while the session is lent to exactly this token.
    [[nodiscard]] bool IsLentTo(Token token) const noexcept {
        return token != 0 && word_.load() == Pack(token, PreviewSessionState::Lent);
    }
    // Borrower side. Lent -> Held, for the current token only.
    bool Return(Token token) noexcept {
        auto expected = Pack(token, PreviewSessionState::Lent);
        return token != 0 && word_.compare_exchange_strong(expected, Pack(token, PreviewSessionState::Held));
    }
    // Window side. Held -> Live, or HoldRequested -> Live (the request is withdrawn). Never from Lent.
    // One compare-and-swap loop over both source states: a worker that answers between two separate attempts
    // cannot make the release miss.
    bool Release() noexcept {
        auto current = word_.load();
        for (;;) {
            const auto state = StateOf(current);
            if (state != PreviewSessionState::Held && state != PreviewSessionState::HoldRequested) return false;
            if (word_.compare_exchange_weak(current, Pack(TokenOf(current), PreviewSessionState::Live))) return true;
        }
    }
    // Worker side, after the session is closed. From any state; a lent token stops matching.
    void Retire() noexcept {
        auto current = word_.load();
        while (!word_.compare_exchange_weak(current, Pack(TokenOf(current), PreviewSessionState::Retired))) {}
    }
private:
    static constexpr unsigned kStateBits = 3;
    static constexpr std::uint64_t Pack(Token token, PreviewSessionState state) noexcept {
        return (token << kStateBits) | static_cast<std::uint64_t>(state);
    }
    static constexpr PreviewSessionState StateOf(std::uint64_t word) noexcept {
        return static_cast<PreviewSessionState>(word & ((1u << kStateBits) - 1));
    }
    static constexpr Token TokenOf(std::uint64_t word) noexcept { return word >> kStateBits; }
    bool Change(PreviewSessionState from, PreviewSessionState to) noexcept {
        auto current = word_.load();
        while (StateOf(current) == from) {
            if (word_.compare_exchange_weak(current, Pack(TokenOf(current), to))) return true;
        }
        return false;
    }
    std::atomic<std::uint64_t> word_{Pack(0, PreviewSessionState::Live)};
};
}
