#include "synthetic_pair_chart.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace a0::m2::synthetic {

namespace {

constexpr double kPaperLevel = 0.92;
constexpr double kTableLevel = 0.18;
constexpr double kFrameLevel = 0.15;
constexpr double kMinorGridLevel = 0.55;
constexpr double kMajorGridLevel = 0.30;
constexpr double kInkLevel = 0.06;
constexpr double kEdgeDarkLevel = 0.08;
constexpr double kEdgeLightLevel = 0.96;

constexpr std::array<std::array<int, 3>, 12> kPalette{{
    {227, 26, 28}, {51, 160, 44}, {31, 120, 180}, {255, 223, 0},
    {255, 127, 0}, {106, 61, 154}, {0, 174, 239}, {236, 0, 140},
    {140, 90, 60}, {242, 201, 160}, {107, 142, 35}, {112, 128, 144},
}};

constexpr std::array<double, 6> kGrayLevels{0.05, 0.2, 0.4, 0.6, 0.8, 0.95};

Rgb Gray(const double level) noexcept { return {level, level, level}; }

double Abs(const double value) noexcept { return value < 0.0 ? -value : value; }

} // namespace

namespace detail {

ChartSampler::ChartSampler(const ChartSpec& chart)
    : width_(chart.width_mm),
      height_(chart.height_mm),
      margin_(chart.margin_mm),
      grid_pitch_(chart.grid_pitch_mm),
      line_width_(chart.grid_line_width_mm),
      pitch_(chart.fiducial_pitch_mm),
      fiducial_radius_(chart.fiducial_radius_mm),
      slope_(chart.slanted_edge_slope),
      slope_norm_(1.0 / std::sqrt(1.0 + chart.slanted_edge_slope * chart.slanted_edge_slope)),
      fine_base_(chart.fine_line_base_width_mm),
      major_every_(static_cast<std::int64_t>(std::floor(chart.fiducial_pitch_mm / chart.grid_pitch_mm + 0.5))),
      columns_(static_cast<std::uint32_t>(std::floor((chart.width_mm - 2.0 * chart.margin_mm) / chart.fiducial_pitch_mm))),
      rows_(static_cast<std::uint32_t>(std::floor((chart.height_mm - 2.0 * chart.margin_mm) / chart.fiducial_pitch_mm))) {}

bool ChartSampler::OnFrame(const double x, const double y) const noexcept {
    const double half_margin = 0.5 * margin_;
    const double half_width = 1.5 * line_width_;
    const bool within_x = x >= half_margin - half_width && x <= width_ - half_margin + half_width;
    const bool within_y = y >= half_margin - half_width && y <= height_ - half_margin + half_width;
    if (!within_x || !within_y) return false;
    return Abs(x - half_margin) <= half_width || Abs(x - (width_ - half_margin)) <= half_width
        || Abs(y - half_margin) <= half_width || Abs(y - (height_ - half_margin)) <= half_width;
}

bool ChartSampler::PaintPattern(const double rx, const double ry, const std::uint32_t kind,
    const std::uint32_t column, const std::uint32_t row, Rgb& color) const noexcept {
    const double half = 0.25 * pitch_;
    switch (kind) {
    case 1:
    case 7: {
        const double backing = 0.3 * pitch_;
        if (Abs(rx) > backing || Abs(ry) > backing) return false;
        const double u = (rx + slope_ * ry) * slope_norm_;
        const double v = (ry - slope_ * rx) * slope_norm_;
        const bool inside = Abs(u) <= half && Abs(v) <= half;
        const bool dark_square = kind == 1;
        color = Gray(inside == dark_square ? kEdgeDarkLevel : kEdgeLightLevel);
        return true;
    }
    case 2: {
        if (Abs(rx) > half || Abs(ry) > half) return false;
        const auto& entry = kPalette[(column + 2U * row) % kPalette.size()];
        color = {entry[0] / 255.0, entry[1] / 255.0, entry[2] / 255.0};
        return true;
    }
    case 5: {
        if (Abs(rx) > half || Abs(ry) > half) return false;
        color = Gray(kGrayLevels[(column + row) % kGrayLevels.size()]);
        return true;
    }
    case 3:
    case 4: {
        if (Abs(rx) > half || Abs(ry) > half) return false;
        // Vertical lines (kind 3) vary their width along y; horizontal lines
        // (kind 4) vary along x. Four bands with widths of 1x..4x fine_base.
        const double across = kind == 3 ? rx : ry;
        const double along = kind == 3 ? ry : rx;
        const double band_height = half * 0.5;
        auto band = static_cast<std::int64_t>(std::floor((along + half) / band_height));
        if (band > 3) band = 3;
        if (band < 0) band = 0;
        const double width = fine_base_ * static_cast<double>(band + 1);
        const double phase = (across + half) / (2.0 * width);
        const double fraction = phase - std::floor(phase);
        color = Gray(fraction < 0.5 ? kInkLevel : kEdgeLightLevel);
        return true;
    }
    default:
        return false;
    }
}

Rgb ChartSampler::Table() noexcept { return Gray(kTableLevel); }

Rgb ChartSampler::Sample(const double x_mm, const double y_mm) const noexcept {
    if (!(x_mm >= 0.0 && x_mm < width_ && y_mm >= 0.0 && y_mm < height_)) return Gray(kTableLevel);
    if (OnFrame(x_mm, y_mm)) return Gray(kFrameLevel);

    Rgb color = Gray(kPaperLevel);
    const double lx = x_mm - margin_;
    const double ly = y_mm - margin_;
    const double extent_x = static_cast<double>(columns_) * pitch_;
    const double extent_y = static_cast<double>(rows_) * pitch_;

    // Grid lines. The major lines sit on the fiducial lattice.
    const bool in_x_range = lx >= -line_width_ && lx <= extent_x + line_width_;
    const bool in_y_range = ly >= -line_width_ && ly <= extent_y + line_width_;
    if (in_x_range && in_y_range) {
        const auto ix = static_cast<std::int64_t>(std::floor(lx / grid_pitch_ + 0.5));
        const auto iy = static_cast<std::int64_t>(std::floor(ly / grid_pitch_ + 0.5));
        const double dx = Abs(lx - static_cast<double>(ix) * grid_pitch_);
        const double dy = Abs(ly - static_cast<double>(iy) * grid_pitch_);
        const std::int64_t last_x = static_cast<std::int64_t>(columns_) * major_every_;
        const std::int64_t last_y = static_cast<std::int64_t>(rows_) * major_every_;
        bool major = false;
        bool minor = false;
        if (ix >= 0 && ix <= last_x) {
            if (ix % major_every_ == 0) major = major || dx <= line_width_;
            else minor = minor || dx <= 0.5 * line_width_;
        }
        if (iy >= 0 && iy <= last_y) {
            if (iy % major_every_ == 0) major = major || dy <= line_width_;
            else minor = minor || dy <= 0.5 * line_width_;
        }
        if (major) color = Gray(kMajorGridLevel);
        else if (minor) color = Gray(kMinorGridLevel);
    }

    // Pattern of the cell the point is in.
    if (lx >= 0.0 && ly >= 0.0 && lx < extent_x && ly < extent_y) {
        const auto column = static_cast<std::uint32_t>(std::floor(lx / pitch_));
        const auto row = static_cast<std::uint32_t>(std::floor(ly / pitch_));
        const double rx = lx - static_cast<double>(column) * pitch_ - 0.5 * pitch_;
        const double ry = ly - static_cast<double>(row) * pitch_ - 0.5 * pitch_;
        (void)PaintPattern(rx, ry, PatternKind(column, row), column, row, color);
    }

    // Ring fiducial of the nearest node.
    if (lx >= -fiducial_radius_ && lx <= extent_x + fiducial_radius_
        && ly >= -fiducial_radius_ && ly <= extent_y + fiducial_radius_) {
        const auto node_x = static_cast<std::int64_t>(std::floor(lx / pitch_ + 0.5));
        const auto node_y = static_cast<std::int64_t>(std::floor(ly / pitch_ + 0.5));
        if (node_x >= 0 && node_x <= static_cast<std::int64_t>(columns_)
            && node_y >= 0 && node_y <= static_cast<std::int64_t>(rows_)) {
            const double dx = lx - static_cast<double>(node_x) * pitch_;
            const double dy = ly - static_cast<double>(node_y) * pitch_;
            const double d2 = dx * dx + dy * dy;
            const double outer = fiducial_radius_;
            if (d2 < outer * outer) {
                const double middle = 0.6 * outer;
                const double dot = 0.25 * outer;
                if (d2 < dot * dot) color = Gray(kInkLevel);
                else if (d2 < middle * middle) color = Gray(1.0);
                else color = Gray(kInkLevel);
            }
        }
    }
    return color;
}

const char* PatternKindName(const std::uint32_t kind) noexcept {
    switch (kind) {
    case 1: return "slanted-edge-dark-on-light";
    case 2: return "color-patch";
    case 3: return "fine-lines-vertical";
    case 4: return "fine-lines-horizontal";
    case 5: return "gray-patch";
    case 7: return "slanted-edge-light-on-dark";
    default: return nullptr;
    }
}

} // namespace detail

Rgb SampleChart(const ChartSpec& chart, const Vec2 document_mm) {
    return detail::ChartSampler(chart).Sample(document_mm.x, document_mm.y);
}

std::vector<ReferencePoint> ListReferencePoints(const ChartSpec& chart) {
    const detail::ChartSampler sampler(chart);
    std::vector<ReferencePoint> points;
    points.reserve(static_cast<std::size_t>(sampler.Columns() + 1U) * (sampler.Rows() + 1U));
    for (std::uint32_t row = 0; row <= sampler.Rows(); ++row) {
        for (std::uint32_t column = 0; column <= sampler.Columns(); ++column) {
            char id[32]{};
            std::snprintf(id, sizeof(id), "F-%03u-%03u", column, row);
            points.push_back({
                id, column, row,
                {chart.margin_mm + static_cast<double>(column) * chart.fiducial_pitch_mm,
                 chart.margin_mm + static_cast<double>(row) * chart.fiducial_pitch_mm},
            });
        }
    }
    return points;
}

std::vector<ChartPattern> ListChartPatterns(const ChartSpec& chart) {
    const detail::ChartSampler sampler(chart);
    std::vector<ChartPattern> patterns;
    for (std::uint32_t row = 0; row < sampler.Rows(); ++row) {
        for (std::uint32_t column = 0; column < sampler.Columns(); ++column) {
            const char* name = detail::PatternKindName(detail::ChartSampler::PatternKind(column, row));
            if (name == nullptr) continue;
            patterns.push_back({
                name, column, row,
                {chart.margin_mm + (static_cast<double>(column) + 0.5) * chart.fiducial_pitch_mm,
                 chart.margin_mm + (static_cast<double>(row) + 0.5) * chart.fiducial_pitch_mm},
                0.25 * chart.fiducial_pitch_mm,
            });
        }
    }
    return patterns;
}

} // namespace a0::m2::synthetic
