// Contracts of the one-owner state of a camera session (#426 stage 4, ADR-0044). Pure state: no camera, no WPD.
#include "a0/phase0/experimental/preview_session_owner.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
using namespace a0::phase0::experimental;
namespace {
int checks{}, failures{};
void Check(bool value, const char* what) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "failed: " << what << '\n';
    }
}
}
int main() {
    {   // A new session belongs to the Live worker.
        PreviewSessionOwner owner;
        Check(owner.State() == PreviewSessionState::Live && owner.MayLiveSend() && owner.AdmitsFocus(), "starts Live");
        Check(!owner.AcknowledgeHold() && !owner.Lend() && !owner.Release(), "only a hold request can be answered; nothing to lend or release");
        Check(!owner.Return(1) && !owner.Return(0) && !owner.IsLentTo(0) && !owner.IsLentTo(1), "no token matches before a lend");
    }
    {   // The whole path: hold, lend, return, release.
        PreviewSessionOwner owner;
        Check(owner.RequestHold() && owner.State() == PreviewSessionState::HoldRequested, "hold requested");
        Check(owner.MayLiveSend() && !owner.AdmitsFocus(), "the frame in flight may finish; no focus request is taken");
        Check(!owner.RequestHold(), "a second request does not stack");
        Check(!owner.Lend(), "a session that was only asked to hold cannot be lent");
        Check(owner.AcknowledgeHold() && owner.State() == PreviewSessionState::Held, "the worker answers from the boundary");
        Check(!owner.MayLiveSend() && !owner.AdmitsFocus(), "a held session is not sent to by the Live path");
        const auto token = owner.Lend();
        Check(token && *token != 0 && owner.State() == PreviewSessionState::Lent && owner.IsLentTo(*token), "lent with a token");
        Check(!owner.MayLiveSend() && !owner.AdmitsFocus(), "a lent session is not sent to by the Live path");
        Check(!owner.Lend(), "a lent session cannot be lent again");
        Check(!owner.Release(), "a lent session cannot be released");
        Check(!owner.AcknowledgeHold() && !owner.RequestHold(), "no hold request or answer while lent");
        Check(!owner.Return(*token + 1) && owner.IsLentTo(*token), "a wrong token does not return it");
        Check(owner.Return(*token) && owner.State() == PreviewSessionState::Held && !owner.IsLentTo(*token), "returned as held");
        Check(!owner.Return(*token), "a token returns the session once");
        Check(owner.Release() && owner.State() == PreviewSessionState::Live && owner.MayLiveSend() && owner.AdmitsFocus(), "released to the Live worker");
    }
    {   // A new lend invalidates the old token for good.
        PreviewSessionOwner owner;
        owner.RequestHold();owner.AcknowledgeHold();
        const auto first = owner.Lend();
        owner.Return(*first);
        const auto second = owner.Lend();
        Check(second && *second != *first, "a fresh token each lend");
        Check(!owner.IsLentTo(*first) && owner.IsLentTo(*second), "the stale token no longer matches while the session is lent again");
        Check(!owner.Return(*first), "the stale token cannot return it");
        owner.Return(*second);owner.Release();
        Check(!owner.IsLentTo(*first) && !owner.IsLentTo(*second), "no token matches after release");
    }
    {   // A withdrawn request goes back to Live; a held session can be released without ever being lent.
        PreviewSessionOwner owner;
        owner.RequestHold();
        Check(owner.Release() && owner.State() == PreviewSessionState::Live, "a request is withdrawn by Release");
        Check(!owner.AcknowledgeHold(), "a withdrawn request cannot be answered");
        owner.RequestHold();owner.AcknowledgeHold();
        Check(owner.Release() && owner.State() == PreviewSessionState::Live, "a held session is released unused");
    }
    {   // Retire ends it from any state and stops a lent token from matching.
        PreviewSessionOwner owner;
        owner.RequestHold();owner.AcknowledgeHold();
        const auto token = owner.Lend();
        owner.Retire();
        Check(owner.State() == PreviewSessionState::Retired && !owner.IsLentTo(*token) && !owner.Return(*token), "a retired session is not lent");
        Check(!owner.MayLiveSend() && !owner.AdmitsFocus() && !owner.RequestHold() && !owner.Lend() && !owner.Release(), "nothing changes a retired session");
        PreviewSessionOwner live;live.Retire();
        Check(live.State() == PreviewSessionState::Retired, "retired from Live");
    }
    {   // Two borrowers race for one held session: exactly one wins, and exactly one token matches.
        for (int round = 0; round < 200; ++round) {
            PreviewSessionOwner owner;
            owner.RequestHold();owner.AcknowledgeHold();
            std::atomic<int> winners{};
            std::atomic<bool> go{};
            std::vector<std::thread> threads;
            std::vector<PreviewSessionOwner::Token> tokens(4, 0);
            for (int i = 0; i < 4; ++i)
                threads.emplace_back([&, i] {
                    while (!go) std::this_thread::yield();
                    if (const auto token = owner.Lend()) { tokens[i] = *token; ++winners; }
                });
            go = true;
            for (auto& thread : threads) thread.join();
            int matching = 0;
            for (const auto token : tokens) if (token && owner.IsLentTo(token)) ++matching;
            Check(winners == 1 && matching == 1, "exactly one borrower wins a race");
        }
    }
    {   // The window's Release and the worker's answer race for a request: exactly one of them takes effect.
        int answered = 0, withdrawn = 0;
        for (int round = 0; round < 400; ++round) {
            PreviewSessionOwner owner;
            owner.RequestHold();
            std::atomic<bool> go{};
            bool answer = false, release = false;
            std::thread worker([&] { while (!go) std::this_thread::yield(); answer = owner.AcknowledgeHold(); });
            std::thread window([&] { while (!go) std::this_thread::yield(); release = owner.Release(); });
            go = true;
            worker.join();window.join();
            // If the worker answered first the window's Release then released the held session; both succeed in that
            // order. If the window withdrew first, the answer must fail. Either way the end state is Live or Held.
            if (answer) ++answered;
            if (!answer) ++withdrawn;
            Check(release, "the window's release never fails");
            Check(owner.State() == PreviewSessionState::Live, "the session ends with the Live worker");
        }
        Check(answered + withdrawn == 400, "every round resolved");
    }
    std::cout << "preview_session_owner_checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
