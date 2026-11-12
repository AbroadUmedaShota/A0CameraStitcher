// What the product table (kD810ProductWriteTable) opens at the stage this commit stands for (#416, ADR-0041).
//
// Written out here and not read from the table: a row opened early, or by mistake, makes a test fail. The commit that opens
// the next stage changes this file and the product table together, and nothing else about what the tests expect of the
// stage. Stage T2: CAM-B can be written with picked values; CAM-A has its ISO row, and the table's entry for CAM-A's firmware version is CAM-B's, by reference.
#pragma once

#include "a0/phase0/experimental/d810_settings_write.hpp"

#include <cstddef>
#include <initializer_list>
#include <cstdint>

namespace a0::phase0::experimental::product_stage {

constexpr std::uint32_t ItemBits(std::initializer_list<D810WriteItem> items) {
    std::uint32_t bits = 0;
    for (const auto item : items) bits |= D810ItemBit(item);
    return bits;
}
constexpr std::size_t BitCount(std::uint32_t bits) {
    std::size_t count = 0;
    for (; bits; bits >>= 1) count += bits & 1u;
    return count;
}

// The items whose row is enabled, per camera.
inline constexpr std::uint32_t kCamBItems = ItemBits({D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture, D810WriteItem::WhiteBalance, D810WriteItem::Metering});
inline constexpr std::uint32_t kCamAItems = ItemBits({D810WriteItem::Iso});
// The table names a firmware version for CAM-A (and it is CAM-B's, by reference).
inline constexpr bool kCamAVersionNamed = true;
// The enabled rows the first log line reports (`enabled_rows=`).
inline constexpr std::size_t kEnabledRows = BitCount(kCamBItems) + BitCount(kCamAItems);
static_assert(kEnabledRows == 6, "stage T2: the five CAM-B rows and CAM-A's ISO");

// True when the product table lets a plan of picked values write `item` to `camera`.
constexpr bool Opens(D810Camera camera, D810WriteItem item) { return ((camera == D810Camera::A ? kCamAItems : kCamBItems) & D810ItemBit(item)) != 0; }

} // namespace a0::phase0::experimental::product_stage
