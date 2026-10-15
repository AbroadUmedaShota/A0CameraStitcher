#include "a0/m2/image_patch_seed_selection.hpp"
#include "image_patch_detail.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace a0::m2::correction {
namespace {
using namespace render;
using namespace patch_detail;
void ValidateSelection(const PatchSeedSelectionPolicy& p) {
    if (p.grid_columns < 1 || p.grid_columns > kMaximumSeedGridSide || p.grid_rows < 1 || p.grid_rows > kMaximumSeedGridSide ||
        p.maximum_seeds < 3 || p.maximum_seeds > kMaximumPatchSeeds ||
        !std::isfinite(p.minimum_spacing_mm) || !(p.minimum_spacing_mm > 0) ||
        !std::isfinite(p.minimum_minor_gradient_eigenvalue) || !(p.minimum_minor_gradient_eigenvalue > 0) ||
        !std::isfinite(p.minimum_gradient_axis_ratio) || !(p.minimum_gradient_axis_ratio > 0 && p.minimum_gradient_axis_ratio < 1))
        throw std::invalid_argument("patch seed selection: invalid explicit policy");
}
// Small outward interval enclosure of the SAME forward model, including basic
// arithmetic rounding. Nonfinite/uncertain bounds are ineligible, never clipped.
double Up(double x) { return std::nextafter(x,std::numeric_limits<double>::infinity()); }
double Down(double x) { return std::nextafter(x,-std::numeric_limits<double>::infinity()); }
struct I { double low, high; };
I Exact(double x) { return {x,x}; }
I Bad() { const double n = std::numeric_limits<double>::quiet_NaN(); return {n,n}; }
bool Valid(I a) { return std::isfinite(a.low) && std::isfinite(a.high) && a.low <= a.high; }
I Add(I a, I b) { if (!Valid(a) || !Valid(b)) return Bad(); return {Down(a.low+b.low),Up(a.high+b.high)}; }
I Mul(I a, I b) {
    if (!Valid(a) || !Valid(b)) return Bad();
    const std::array<double,4> v{a.low*b.low,a.low*b.high,a.high*b.low,a.high*b.high};
    for (const auto x : v) if (!std::isfinite(x)) return Bad();
    return {Down(*std::min_element(v.begin(),v.end())),Up(*std::max_element(v.begin(),v.end()))};
}
I Square(I a) {
    if (!Valid(a)) return Bad();
    const double lo = std::min(std::abs(a.low),std::abs(a.high)), hi = std::max(std::abs(a.low),std::abs(a.high));
    return {a.low <= 0 && a.high >= 0 ? 0 : Down(lo*lo),Up(hi*hi)};
}
I Divide(I a, I b) {
    if (!Valid(a) || !Valid(b) || !(b.low > 0)) return Bad();
    return Mul(a,{Down(1/b.high),Up(1/b.low)});
}
bool Covered(const BgrImage& image, const CameraProjection& c, Point seed,
    const std::array<Point,4>& domain, const ImagePatchPolicy& p, int searches) {
    const double search = Up(p.search_radius_steps*p.search_step_mm), patch = Up(p.patch_radius_samples*p.patch_spacing_mm);
    I x = Exact(seed.x), y = Exact(seed.y);
    // Enclose nested p+forwardOffset+reverseOffset+patchOffset operations,
    // rather than assuming a single combined margin has identical rounding.
    for (int i = 0; i < searches; ++i) { x = Add(x,{-search,search}); y = Add(y,{-search,search}); }
    x = Add(x,{-patch,patch}); y = Add(y,{-patch,patch});
    if (!Valid(x) || !Valid(y) || x.low < domain[0].x || x.high > domain[2].x || y.low < domain[0].y || y.high > domain[2].y) return false;
    const auto& h = c.document_to_image; const auto& k = c.intrinsics; const auto& d = c.distortion;
    const auto row = [&](std::size_t r) { return Add(Add(Mul(Exact(h[r*3]),x),Mul(Exact(h[r*3+1]),y)),Exact(h[r*3+2])); };
    const I w = row(2);
    const I nx = Divide(Add(Divide(row(0),w),Exact(-k.cx_pixels)),Exact(k.fx_pixels));
    const I ny = Divide(Add(Divide(row(1),w),Exact(-k.cy_pixels)),Exact(k.fy_pixels));
    const I r2 = Add(Square(nx),Square(ny));
    const I radial = Add(Add(Add(Exact(1),Mul(Exact(d.k1),r2)),Mul(Mul(Exact(d.k2),r2),r2)),Mul(Mul(Mul(Exact(d.k3),r2),r2),r2));
    const I xd = Add(Add(Mul(nx,radial),Mul(Mul(Mul(Exact(2),Exact(d.p1)),nx),ny)),Mul(Exact(d.p2),Add(r2,Mul(Exact(2),Square(nx)))));
    const I yd = Add(Add(Mul(ny,radial),Mul(Exact(d.p1),Add(r2,Mul(Exact(2),Square(ny))))),Mul(Mul(Mul(Exact(2),Exact(d.p2)),nx),ny));
    const I raw_x = Add(Mul(Exact(k.fx_pixels),xd),Exact(k.cx_pixels)), raw_y = Add(Mul(Exact(k.fy_pixels),yd),Exact(k.cy_pixels));
    return Valid(raw_x) && Valid(raw_y) && raw_x.low >= 0 && raw_x.high < image.width && raw_y.low >= 0 && raw_y.high < image.height;
}
PatchSeedTexture Texture(std::vector<double>& values, std::uint32_t radius) {
    const std::size_t side = 2*radius+1;
    double xx = 0, xy = 0, yy = 0;
    for (std::size_t y = 0; y+1 < side; ++y) for (std::size_t x = 0; x+1 < side; ++x) {
        const double gx = values[y*side+x+1]-values[y*side+x], gy = values[(y+1)*side+x]-values[y*side+x];
        xx += gx*gx; xy += gx*gy; yy += gy*gy;
    }
    const double count = static_cast<double>((side-1)*(side-1)); xx /= count; xy /= count; yy /= count;
    const double major = (xx+yy+std::hypot(xx-yy,2*xy))/2;
    const double minor = major > 0 ? (xx*yy-xy*xy)/major : 0;
    return {Center(values)/std::sqrt(static_cast<double>(values.size())),minor,major,major > 0 ? minor/major : 0};
}
} // namespace

PatchSeedSelectionResult SelectImagePatchSeeds(const BgrImage& a, const BgrImage& b,
    const CameraProjection& ca, const CameraProjection& cb, const OutputRaster& raster,
    const PatchSeedSelectionPolicy& policy, const ImagePatchPolicy& matching, const DocumentCorrectionPolicy& correction) {
    Validate(a); Validate(b); Validate(matching); ValidateSelection(policy);
    ValidateCameraProjection(ca,raster);
    (void)EstimateBoundedDocumentCorrection(cb,raster,{},correction); // validate all geometry policy fields
    const auto domain = DocumentRasterCorners(raster);
    PatchSeedSelectionResult result{PatchSeedSelectionOutcome::InsufficientSeeds,DocumentCorrectionOutcome::InsufficientPoints,{}, {}};
    std::vector<std::size_t> eligible;
    std::set<std::pair<double,double>> positions;
    std::vector<double> values;
    for (std::uint32_t row = 0; row < policy.grid_rows; ++row) for (std::uint32_t column = 0; column < policy.grid_columns; ++column) {
        const Point point{domain[0].x+(column+.5)*(domain[2].x-domain[0].x)/policy.grid_columns,
            domain[0].y+(row+.5)*(domain[2].y-domain[0].y)/policy.grid_rows};
        PatchSeedCandidate candidate{column,row,point,PatchSeedOutcome::NumericalFailure,{}};
        if (!InDomain(point,domain)) candidate.outcome = PatchSeedOutcome::NumericalFailure;
        else if (!positions.emplace(point.x,point.y).second) candidate.outcome = PatchSeedOutcome::DuplicatePosition;
        else if (!Covered(a,ca,point,domain,matching,2)) candidate.outcome = PatchSeedOutcome::CoverageAUnavailable;
        else if (!Covered(b,cb,point,domain,matching,1)) candidate.outcome = PatchSeedOutcome::CoverageBUnavailable;
        else if (!Patch(a,ca,point,domain,matching,values)) candidate.outcome = PatchSeedOutcome::SourcePatchUnavailable;
        else {
            candidate.texture = Texture(values,matching.patch_radius_samples);
            const auto t = *candidate.texture;
            if (!std::isfinite(t.intensity_stddev) || !std::isfinite(t.minor_gradient_eigenvalue) || !std::isfinite(t.gradient_axis_ratio)) candidate.outcome = PatchSeedOutcome::NumericalFailure;
            else if (t.intensity_stddev < matching.minimum_intensity_stddev) candidate.outcome = PatchSeedOutcome::LowContrast;
            else if (t.gradient_axis_ratio < policy.minimum_gradient_axis_ratio) candidate.outcome = PatchSeedOutcome::OneDimensionalTexture;
            else if (t.minor_gradient_eigenvalue < policy.minimum_minor_gradient_eigenvalue) candidate.outcome = PatchSeedOutcome::WeakTexture;
            else { candidate.outcome = PatchSeedOutcome::SelectionLimit; eligible.push_back(result.candidates.size()); }
        }
        result.candidates.push_back(candidate);
    }
    std::sort(eligible.begin(),eligible.end(),[&](std::size_t x, std::size_t y) {
        const double sx = result.candidates[x].texture->minor_gradient_eigenvalue, sy = result.candidates[y].texture->minor_gradient_eigenvalue;
        return sx != sy ? sx > sy : x < y;
    });
    for (const auto index : eligible) {
        auto& candidate = result.candidates[index];
        if (result.selected_indices.size() == policy.maximum_seeds) continue;
        bool separated = true;
        for (const auto selected : result.selected_indices) {
            const auto point = result.candidates[selected].document_mm;
            if (std::hypot(candidate.document_mm.x-point.x,candidate.document_mm.y-point.y) < policy.minimum_spacing_mm) { separated = false; break; }
        }
        if (!separated) candidate.outcome = PatchSeedOutcome::SpacingExcluded;
        else { candidate.outcome = PatchSeedOutcome::Selected; result.selected_indices.push_back(index); }
    }
    std::vector<DocumentCorrespondence> identity;
    for (const auto index : result.selected_indices) { const auto p = result.candidates[index].document_mm; identity.emplace_back(p,p); }
    const auto geometry = EstimateBoundedDocumentCorrection(cb,raster,identity,correction);
    result.geometry_outcome = geometry.outcome; // never expose its identity warp as a correction
    result.outcome = geometry.outcome == DocumentCorrectionOutcome::Estimated ? PatchSeedSelectionOutcome::Selected :
        geometry.outcome == DocumentCorrectionOutcome::InsufficientPoints ? PatchSeedSelectionOutcome::InsufficientSeeds : PatchSeedSelectionOutcome::GeometryRejected;
    return result;
}

AutoImagePatchCorrectionResult EstimateAutoImagePatchCorrection(const BgrImage& a, const BgrImage& b,
    const CameraProjection& ca, const CameraProjection& cb, const OutputRaster& raster,
    const PatchSeedSelectionPolicy& selection, const ImagePatchPolicy& matching, const RawInversePolicy& inverse,
    const DocumentCorrectionPolicy& correction) {
    (void)EstimateRawPairDocumentCorrection(ca,cb,raster,{a.width,a.height},{b.width,b.height},{},inverse,correction);
    AutoImagePatchCorrectionResult result{AutoImagePatchOutcome::SelectionRejected,
        SelectImagePatchSeeds(a,b,ca,cb,raster,selection,matching,correction),{}};
    if (result.selection.outcome != PatchSeedSelectionOutcome::Selected) return result;
    std::vector<Point> seeds;
    for (const auto index : result.selection.selected_indices) seeds.push_back(result.selection.candidates[index].document_mm);
    result.correction = EstimateImagePatchCorrection(a,b,ca,cb,raster,seeds,matching,inverse,correction);
    result.outcome = result.correction->outcome == ImagePatchCorrectionOutcome::Estimated ? AutoImagePatchOutcome::Estimated : AutoImagePatchOutcome::MatchingRejected;
    return result;
}
} // namespace a0::m2::correction
