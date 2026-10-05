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
        if (error.Category() != category) std::cerr << "  actual: " << error.Category() << '\n';
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
    Rejects([&] { changed.CheckInventory({71, 97}); }, "worker_selection_inventory_changed",
            "inventory substitution invalidates selection as inventory_changed");
    Rejects([&] { changed.CheckInventory({71, 83}); }, "worker_selection_invalidated",
            "a rejected selection cannot recover after inventory returns and is not reclassified");

    auto removed_readded = Pair();
    removed_readded.ObserveTopology(false, 71);
    removed_readded.ObserveTopology(true, 71);
    Rejects([&] { removed_readded.OpenSelected(removed_readded.Tokens().front(), {71, 83}, [](std::uint32_t) {}); },
            "worker_selection_topology_event", "remove/re-add cannot reuse selection");

    auto duplicate_add = Pair();
    duplicate_add.ObserveTopology(true, 71);
    Rejects([&] { duplicate_add.OpenSelected(duplicate_add.Tokens().front(), {71, 83}, [](std::uint32_t) {}); },
            "worker_selection_topology_event", "duplicate existing Add invalidates selection");

    // A changed set wins over an earlier topology event.
    auto event_then_changed = Pair();
    event_then_changed.ObserveTopology(true, 71);
    Rejects([&] { event_then_changed.CheckInventory({71, 97}); }, "worker_selection_inventory_changed",
            "set mismatch takes precedence over a topology event");
    Rejects([&] { event_then_changed.CheckInventory({71, 83}); }, "worker_selection_invalidated",
            "after that rejection the topology event is not reported again");

    // An external invalidation followed by a topology event stays an external one.
    auto external_first = Pair();
    external_first.Invalidate();
    external_first.ObserveTopology(true, 71);
    Rejects([&] { external_first.CheckInventory({71, 83}); }, "worker_selection_invalidated",
            "the first invalidation cause wins");

    // Valid() and SameInventory() only read.
    auto probe = Pair();
    for (int repeat = 0; repeat < 3; ++repeat) {
        Check(probe.Valid() && probe.SameInventory({83, 71}) && !probe.SameInventory({71, 97}) &&
              !probe.SameInventory({71}), "Valid/SameInventory report the state");
    }
    probe.CheckInventory({71, 83});
    std::uint32_t probe_opened{};
    probe.OpenSelected(probe.Tokens().front(), {83, 71}, [&](std::uint32_t id) { probe_opened = id; });
    Check(probe.Opened() && probe_opened == 71, "Valid/SameInventory change no state: the selection still opens");
    probe.ObserveTopology(false, 83);
    Check(!probe.Valid() && probe.SameInventory({71, 83}), "Valid follows invalidation; SameInventory ignores it");

    // Inside Suspend's close() and Resume's open(), an event keeps the
    // pre-split category: those checks are not the inventory check.
    auto close_event = Pair();
    close_event.OpenSelected(close_event.Tokens().front(), {71, 83}, [](auto) {});
    Rejects([&] { close_event.Suspend({71, 83}, [&] { close_event.ObserveTopology(true, 71); }); },
            "worker_selection_invalidated", "an event inside Suspend close() stays worker_selection_invalidated");
    auto open_event = Pair();
    const auto open_token = open_event.Tokens().front();
    open_event.OpenSelected(open_token, {71, 83}, [](auto) {});
    open_event.Suspend({71, 83}, [] {});
    Rejects([&] { open_event.Resume(open_token, {71, 83}, [&](auto) { open_event.ObserveTopology(false, 83); }); },
            "worker_selection_invalidated", "an event inside Resume open() stays worker_selection_invalidated");

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

    auto handoff = Pair();
    const auto chosen = handoff.Tokens().at(1);
    std::vector<std::uint32_t> opened;
    unsigned closed{};
    handoff.OpenSelected(chosen, {71,83}, [&](auto id) { opened.push_back(id); });
    handoff.Suspend({71,83}, [&] { ++closed; });
    Check(!handoff.Opened() && closed == 1, "suspended selection has no open source");
    handoff.Resume(chosen, {71,83}, [&](auto id) { opened.push_back(id); });
    Check(handoff.Opened() && opened == std::vector<std::uint32_t>{83,83},
          "handoff opens only the same worker-local source");
    Rejects([&] { handoff.Suspend({71,83}, [] {}); }, "worker_handoff_unavailable",
            "a second handoff is not a retry path");

    for (int fault = 0; fault < 5; ++fault) {
        auto s = Pair();
        const auto token = s.Tokens().front();
        s.OpenSelected(token, {71,83}, [](auto) {});
        if (fault == 0) {
            Rejects([&] { s.Suspend({71,83}, [] { throw TransportError("injected", "close"); }); },
                    "injected", "close failure invalidates handoff");
        } else {
            s.Suspend({71,83}, [] {});
            if (fault == 1) s.ObserveTopology(true, 71);
            if (fault == 2) {
                Rejects([&] { s.Resume(s.Tokens().back(), {71,83}, [](auto) {}); },
                        "worker_handoff_unavailable", "handoff cannot switch camera");
            }
            if (fault == 3) {
                Rejects([&] { s.Resume(token, {71,97}, [](auto) {}); },
                        "worker_selection_inventory_changed", "changed inventory prevents resume");
            }
            if (fault == 4) {
                Rejects([&] { s.Resume(token, {71,83}, [](auto) { throw TransportError("injected", "open"); }); },
                        "injected", "resume failure invalidates without retry");
            }
        }
        unsigned retries{};
        // fault 1 is the only topology event; every other fault invalidated
        // the selection some other way, or was already rejected.
        Rejects([&] { s.Resume(token, {71,83}, [&](auto) { ++retries; }); },
                fault == 1 ? "worker_selection_topology_event" : "worker_selection_invalidated",
                "failed handoff cannot recover");
        Check(retries == 0 && !s.Opened(), "terminal selection never calls SDK again");
    }

    std::cout << "{\"mode\":\"simulation\",\"failures\":" << failures << "}\n";
    return failures == 0 ? 0 : 1;
}
