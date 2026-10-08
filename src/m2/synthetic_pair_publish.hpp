#pragma once

#include <filesystem>
#include <vector>

namespace a0::m2::synthetic::detail {

struct PublishItem {
    std::filesystem::path partial;
    std::filesystem::path destination;
};

// Moves every partial file to its destination, in order, never replacing an
// existing file. When one move fails, the destinations already created are
// deleted again, every remaining partial file is deleted, and std::runtime_error
// is thrown; a destination that existed before the call is left alone.
void PublishAll(const std::vector<PublishItem>& items);

} // namespace a0::m2::synthetic::detail
