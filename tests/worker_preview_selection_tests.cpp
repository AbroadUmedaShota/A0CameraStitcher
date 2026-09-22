#include "a0/phase0/worker_preview_selection.hpp"

#include <iostream>
#include <string>
#include <vector>

using a0::phase0::TransportError;
using a0::phase0::WorkerPreviewSelection;

namespace {
int failures{};

void Check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}

template <class Action>
void Rejects(Action&& action, const char* category, const char* message) {
    try {
        action();
        Check(false, message);
    } catch (const TransportError& error) {
        Check(error.Category() == category, message);
    }
}

WorkerPreviewSelection Pair(std::string generation = "generation-1") {
    return WorkerPreviewSelection(std::move(generation), {83, 71});
}
} // namespace

int main() {
    for (const auto& inventory : std::vector<std::vector<std::uint32_t>>{
             {}, {71}, {71, 71}, {71, 83, 97}}) {
        Rejects([&] { WorkerPreviewSelection rejected("generation", inventory); },
                "worker_inventory_not_pair", "only exactly two distinct inventory entries are accepted");
    }

    auto selection = Pair();
    const auto tokens = selection.Tokens();
    Check(tokens.size() == 2 && tokens[0] != tokens[1], "pair yields two distinct opaque tokens");
    Rejects([&] { selection.OpenSelected("wrong", {71, 83}, [](std::uint32_t) {}); },
            "worker_candidate_unavailable", "wrong token is rejected");
    Check(!selection.Opened(), "wrong token cannot open a source");

    auto old_selection = Pair("old");
    auto new_selection = Pair("new");
    Rejects([&] { new_selection.OpenSelected(old_selection.Tokens().front(), {71, 83}, [](std::uint32_t) {}); },
            "worker_candidate_unavailable", "stale generation token is rejected");

    auto changed = Pair();
    Rejects([&] { changed.CheckInventory({71, 97}); }, "worker_selection_invalidated",
            "inventory substitution invalidates selection");
    Rejects([&] { changed.CheckInventory({71, 83}); }, "worker_selection_invalidated",
            "invalidated selection cannot recover after inventory returns");

    auto removed_readded = Pair();
    removed_readded.ObserveTopology(false, 71);
    removed_readded.ObserveTopology(true, 71);
    Rejects([&] { removed_readded.OpenSelected(removed_readded.Tokens().front(), {71, 83}, [](std::uint32_t) {}); },
            "worker_selection_invalidated", "remove/re-add cannot reuse selection");

    auto duplicate_add = Pair();
    duplicate_add.ObserveTopology(true, 71);
    Rejects([&] { duplicate_add.OpenSelected(duplicate_add.Tokens().front(), {71, 83}, [](std::uint32_t) {}); },
            "worker_selection_invalidated", "duplicate existing Add invalidates selection");

    auto exceptional = Pair();
    unsigned callback_count{};
    Rejects([&] {
        exceptional.OpenSelected(exceptional.Tokens().front(), {71, 83}, [&](std::uint32_t) {
            ++callback_count;
            throw TransportError("injected", "simulated SDK exception");
        });
    }, "injected", "callback exception is propagated");
    Check(callback_count == 1 && !exceptional.Opened(), "SDK exception consumes and invalidates selection");
    Rejects([&] { exceptional.OpenSelected(exceptional.Tokens().front(), {71, 83}, [](std::uint32_t) {}); },
            "worker_selection_invalidated", "failed callback is never retried");

    auto accepted = Pair();
    std::uint32_t selected{};
    accepted.OpenSelected(accepted.Tokens().at(1), {71, 83}, [&](std::uint32_t id) { selected = id; });
    Check(accepted.Opened() && selected == 83, "token opens only its worker-local raw candidate");
    Rejects([&] { accepted.OpenSelected(accepted.Tokens().at(0), {71, 83}, [](std::uint32_t) {}); },
            "worker_candidate_unavailable", "only one candidate can be opened per selection");

    std::cout << "{\"mode\":\"simulation\",\"failures\":" << failures << "}\n";
    return failures == 0 ? 0 : 1;
}
