#pragma once

#include "a0/m2/synthetic_pair.hpp"

namespace a0::m2::synthetic::detail {

// Chart layout (all positions on the document plane, mm):
//   * the frame is a rectangle outline at margin / 2 from each document edge;
//   * the lattice starts at (margin, margin) with pitch fiducial_pitch and has
//     nx x ny cells, so (nx + 1) x (ny + 1) nodes;
//   * grid lines run at grid_pitch across the lattice (every
//     fiducial_pitch / grid_pitch-th line is a wider, darker major line);
//   * every node carries a ring fiducial (black ring, white ring, black dot);
//   * every cell carries one of eight patterns chosen by (3 * column + 5 * row) % 8:
//     0 and 6 empty, 1 slanted edge (dark square on light), 7 slanted edge
//     (light square on dark), 2 colour patch, 5 grey patch, 3 vertical fine
//     lines, 4 horizontal fine lines.
// Only basic arithmetic, floor and sqrt are used so that results are bit
// identical across machines; no libm transcendental functions.
class ChartSampler final {
public:
    explicit ChartSampler(const ChartSpec& chart);

    [[nodiscard]] Rgb Sample(double x_mm, double y_mm) const noexcept;
    // Colour outside the document: what Sample returns off the sheet.
    [[nodiscard]] static Rgb Table() noexcept;
    [[nodiscard]] std::uint32_t Columns() const noexcept { return columns_; }
    [[nodiscard]] std::uint32_t Rows() const noexcept { return rows_; }
    [[nodiscard]] static std::uint32_t PatternKind(std::uint32_t column, std::uint32_t row) noexcept {
        return (3U * column + 5U * row) % 8U;
    }

private:
    [[nodiscard]] bool OnFrame(double x, double y) const noexcept;
    [[nodiscard]] bool PaintPattern(double rx, double ry, std::uint32_t kind, std::uint32_t column,
        std::uint32_t row, Rgb& color) const noexcept;

    double width_;
    double height_;
    double margin_;
    double grid_pitch_;
    double line_width_;
    double pitch_;
    double fiducial_radius_;
    double slope_;
    double slope_norm_;
    double fine_base_;
    std::int64_t major_every_;
    std::uint32_t columns_;
    std::uint32_t rows_;
};

[[nodiscard]] const char* PatternKindName(std::uint32_t kind) noexcept;

} // namespace a0::m2::synthetic::detail
