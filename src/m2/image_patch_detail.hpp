#pragma once

// Shared private sampling contract for matching and pre-match seed selection.
#include "a0/m2/image_patch_correction.hpp"
#include <cmath>
#include <stdexcept>

namespace a0::m2::correction::patch_detail {
using namespace render;
inline void Validate(const BgrImage& image) {
    if (image.bgr.size() != PixelCount(image.width,image.height)*3)
        throw std::invalid_argument("patch matching: BGR buffer differs from dimensions");
}
inline void Validate(const ImagePatchPolicy& p) {
    if (p.patch_radius_samples < 1 || p.patch_radius_samples > 8 || p.search_radius_steps < 1 || p.search_radius_steps > 32 ||
        !std::isfinite(p.patch_spacing_mm) || !(p.patch_spacing_mm > 0) ||
        !std::isfinite(p.search_step_mm) || !(p.search_step_mm > 0) ||
        !std::isfinite(p.minimum_intensity_stddev) || !(p.minimum_intensity_stddev > 0) ||
        !std::isfinite(p.minimum_correlation) || p.minimum_correlation < -1 || p.minimum_correlation > 1 ||
        !std::isfinite(p.minimum_best_second_gap) || !(p.minimum_best_second_gap > 0) || p.minimum_best_second_gap > 2 ||
        !std::isfinite(p.maximum_reverse_error_mm) || p.maximum_reverse_error_mm < 0)
        throw std::invalid_argument("patch matching: invalid explicit policy");
}
inline bool InDomain(Point p, const std::array<Point,4>& corners) {
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= corners[0].x && p.x <= corners[2].x &&
        p.y >= corners[0].y && p.y <= corners[2].y;
}
inline void Validate(const PatchRefinementPolicy& p) {
    if (p.levels < 1 || p.levels > 8) throw std::invalid_argument("patch matching: invalid refinement levels");
}
inline bool Patch(const BgrImage& image, const CameraProjection& camera, Point center,
    const std::array<Point,4>& domain, const ImagePatchPolicy& policy, std::vector<double>& values) {
    values.clear();
    const auto radius = static_cast<int>(policy.patch_radius_samples);
    for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
        const Point p{center.x+x*policy.patch_spacing_mm,center.y+y*policy.patch_spacing_mm};
        Point raw{}; std::array<double,3> bgr{};
        if (!InDomain(p,domain) || !TryProjectDocument(camera,p,raw) || !SampleBilinear(image,raw.x,raw.y,bgr)) return false;
        values.push_back((bgr[0]+bgr[1]+bgr[2])/3);
    }
    return true;
}
inline double Center(std::vector<double>& values) {
    double sum = 0; for (const double v : values) sum += v;
    const double mean = sum/static_cast<double>(values.size());
    double square_sum = 0;
    for (auto& value : values) { value -= mean; square_sum += value*value; }
    return std::sqrt(square_sum);
}
} // namespace a0::m2::correction::patch_detail
