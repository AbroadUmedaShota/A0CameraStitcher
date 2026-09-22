#pragma once

#include "a0/phase0/phase0.hpp"
#include <cstdint>
#include <vector>

namespace a0::phase0 {
// Stage-one real-worker preflight. Raw SDK inventory only: no source may be
// opened for identification. Never choose by ordinal from multiple bodies.
template<class Open>
void OpenOnlyWorkerSource(const std::vector<std::uint32_t>& sources, Open&& open) {
    if (sources.size() != 1) {
        throw TransportError("worker_requires_single_source",
            "single-worker preview requires exactly one raw SDK source before any source Open");
    }
    open(sources.front()); // One attempt; no retry or fallback candidate.
}
} // namespace a0::phase0
