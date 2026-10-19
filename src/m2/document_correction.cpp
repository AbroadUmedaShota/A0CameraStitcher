#include "a0/m2/document_correction.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>

namespace a0::m2::correction {
namespace {
using namespace render;
using Outcome = DocumentCorrectionOutcome;

bool Finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }
void ValidatePolicy(const DocumentCorrectionPolicy& p) {
    if (p.minimum_points < 3 || p.minimum_points > kMaximumDocumentCorrectionPoints ||
        !std::isfinite(p.minimum_rms_spread_mm) || !(p.minimum_rms_spread_mm > 0) ||
        !std::isfinite(p.minimum_axis_variance_ratio) || !(p.minimum_axis_variance_ratio > 0 && p.minimum_axis_variance_ratio < 1))
        throw std::invalid_argument("document correction: invalid numerical policy");
    for (const auto limit : {p.maximum_fit_residual_output_pixels, p.maximum_displacement_output_pixels,
        p.maximum_rotation_degrees, p.maximum_scale_difference_percent})
        if (!std::isfinite(limit) || limit < 0) throw std::invalid_argument("document correction: invalid budget");
    if (p.maximum_rotation_degrees > 180) throw std::invalid_argument("document correction: rotation budget exceeds 180 degrees");
}
struct Cloud { Point mean; double extent, xx, xy, yy; };
std::optional<Cloud> Describe(std::span<const DocumentCorrespondence> matches, bool target) {
    Cloud result{{0,0},0,0,0,0};
    std::size_t count = 0;
    for (const auto& match : matches) {
        const auto p = target ? match.camera_document_mm : match.document_mm;
        ++count;
        result.mean.x += (p.x-result.mean.x)/static_cast<double>(count);
        result.mean.y += (p.y-result.mean.y)/static_cast<double>(count);
        if (!Finite(result.mean)) return {};
    }
    for (const auto& match : matches) {
        const auto p = target ? match.camera_document_mm : match.document_mm;
        result.extent = std::max({result.extent, std::abs(p.x-result.mean.x), std::abs(p.y-result.mean.y)});
    }
    if (!std::isfinite(result.extent)) return {};
    if (result.extent == 0) return result;
    for (const auto& match : matches) {
        const auto p = target ? match.camera_document_mm : match.document_mm;
        const double x = (p.x-result.mean.x)/result.extent, y = (p.y-result.mean.y)/result.extent;
        result.xx += x*x; result.xy += x*y; result.yy += y*y;
    }
    return result;
}
Outcome Geometry(const Cloud& cloud, std::size_t count, const DocumentCorrectionPolicy& policy) {
    const double trace = cloud.xx+cloud.yy;
    if (cloud.extent == 0 || !(trace > 0)) return Outcome::DegenerateGeometry;
    const double spread = cloud.extent * std::sqrt(trace/static_cast<double>(count));
    if (!std::isfinite(spread)) return Outcome::NumericalFailure;
    const double maximum = (trace+std::hypot(cloud.xx-cloud.yy,2*cloud.xy))/2;
    const double determinant = cloud.xx*cloud.yy-cloud.xy*cloud.xy;
    // det/lambdaMax avoids cancellation in (trace-discriminant)/2.
    if (!(determinant > 0) || spread < policy.minimum_rms_spread_mm ||
        (determinant/maximum)/maximum < policy.minimum_axis_variance_ratio) return Outcome::DegenerateGeometry;
    return Outcome::Estimated;
}
struct Residuals {
    double scale = 0, sum = 0, maximum = 0;
    void Add(double value) {
        maximum = std::max(maximum,value);
        if (value > scale) { const double ratio = scale/value; sum = 1+sum*ratio*ratio; scale = value; }
        else if (value > 0) { const double ratio = value/scale; sum += ratio*ratio; }
    }
    double Rms(std::size_t count) const { return scale*std::sqrt(sum/static_cast<double>(count)); }
};
} // namespace

DocumentCorrectionResult EstimateBoundedDocumentCorrection(const CameraProjection& fixed_camera,
    const OutputRaster& raster, std::span<const DocumentCorrespondence> matches, const DocumentCorrectionPolicy& policy) {
    ValidatePolicy(policy);
    ValidateCameraProjection(fixed_camera,raster);
    DocumentCorrectionResult result{Outcome::Estimated,{},{},{},{}};
    const auto fail = [&](Outcome why) { result.outcome = why; result.violations.push_back(why); return result; };
    if (matches.size() < policy.minimum_points) return fail(Outcome::InsufficientPoints);
    if (matches.size() > kMaximumDocumentCorrectionPoints) return fail(Outcome::InvalidObservations);
    std::set<std::pair<double,double>> sources, targets;
    bool identity = true;
    for (const auto& match : matches) {
        const auto p = match.document_mm, q = match.camera_document_mm;
        if (!Finite(p) || !Finite(q) || !sources.emplace(p.x,p.y).second || !targets.emplace(q.x,q.y).second)
            return fail(Outcome::InvalidObservations);
        identity = identity && p.x == q.x && p.y == q.y;
    }
    const auto source = Describe(matches,false), target = Describe(matches,true);
    if (!source || !target) return fail(Outcome::NumericalFailure);
    for (const auto* cloud : {&*source,&*target}) {
        const auto status = Geometry(*cloud,matches.size(),policy);
        if (status != Outcome::Estimated) return fail(status);
    }
    double cxx = 0, cxy = 0, cyx = 0, cyy = 0;
    for (const auto& match : matches) {
        const double x = (match.document_mm.x-source->mean.x)/source->extent;
        const double y = (match.document_mm.y-source->mean.y)/source->extent;
        const double u = (match.camera_document_mm.x-target->mean.x)/target->extent;
        const double v = (match.camera_document_mm.y-target->mean.y)/target->extent;
        cxx += x*u; cxy += x*v; cyx += y*u; cyy += y*v;
    }
    if (cxx*cyy-cxy*cyx < 0) return fail(Outcome::ReflectedCorrespondences);
    const double ratio = target->extent/source->extent, denominator = source->xx+source->yy;
    DocumentSimilarity fit{(cxx+cyy)/denominator*ratio, (cxy-cyx)/denominator*ratio,0,0};
    fit.translation_x_mm = target->mean.x-fit.a*source->mean.x+fit.b*source->mean.y;
    fit.translation_y_mm = target->mean.y-fit.b*source->mean.x-fit.a*source->mean.y;
    // Exact identical observations have an exact identity solution, including
    // when a zero budget is requested. Never snap a near-identity fit to it.
    if (identity) fit = {1,0,0,0};
    const double scale = std::hypot(fit.a,fit.b);
    if (!std::isfinite(fit.a) || !std::isfinite(fit.b) || !std::isfinite(scale) || !(scale > 0) ||
        !std::isfinite(fit.translation_x_mm) || !std::isfinite(fit.translation_y_mm)) return fail(Outcome::NumericalFailure);
    result.estimate = fit;
    // One inch is exactly 127/5 mm. Avoid an extra rounded pitch and inverse:
    // at 254 dpi, for example, pixels_per_mm is exactly 10, not 10+1 ulp.
    const double pixels_per_mm = static_cast<double>(raster.dpi)*5/127;
    Residuals residuals;
    for (const auto& match : matches) {
        const auto p = match.document_mm, q = match.camera_document_mm;
        const double dx = fit.a*p.x-fit.b*p.y+fit.translation_x_mm-q.x;
        const double dy = fit.b*p.x+fit.a*p.y+fit.translation_y_mm-q.y;
        const double error = std::hypot(dx,dy)*pixels_per_mm;
        if (!std::isfinite(error)) return fail(Outcome::NumericalFailure);
        residuals.Add(error);
    }
    double maximum_displacement = 0;
    for (const auto p : DocumentRasterCorners(raster)) {
        // Displacement is affine; its norm is convex. Its maximum on this
        // rectangle occurs at a corner, including the ceil raster extension.
        const double dx = (fit.a-1)*p.x-fit.b*p.y+fit.translation_x_mm;
        const double dy = fit.b*p.x+(fit.a-1)*p.y+fit.translation_y_mm;
        const double displacement = std::hypot(dx,dy)*pixels_per_mm;
        if (!std::isfinite(displacement)) return fail(Outcome::NumericalFailure);
        maximum_displacement = std::max(maximum_displacement,displacement);
    }
    DocumentCorrectionMetrics metrics{matches.size(),residuals.Rms(matches.size()),residuals.maximum,
        maximum_displacement,std::abs(std::atan2(fit.b,fit.a))*180/std::numbers::pi,std::abs(scale-1)*100};
    if (!std::isfinite(metrics.rotation_degrees) || !std::isfinite(metrics.scale_difference_percent)) return fail(Outcome::NumericalFailure);
    result.metrics = metrics;
    if (metrics.maximum_fit_residual_output_pixels > policy.maximum_fit_residual_output_pixels) result.violations.push_back(Outcome::FitResidualExceeded);
    if (metrics.maximum_displacement_output_pixels > policy.maximum_displacement_output_pixels) result.violations.push_back(Outcome::DisplacementExceeded);
    if (metrics.rotation_degrees > policy.maximum_rotation_degrees) result.violations.push_back(Outcome::RotationExceeded);
    if (metrics.scale_difference_percent > policy.maximum_scale_difference_percent) result.violations.push_back(Outcome::ScaleExceeded);
    if (!result.violations.empty()) { result.outcome = result.violations[0]; return result; }
    auto corrected = fixed_camera;
    if (!identity) {
        const Matrix3 transform{fit.a,-fit.b,fit.translation_x_mm,fit.b,fit.a,fit.translation_y_mm,0,0,1};
        Matrix3 composed{};
        for (std::size_t r = 0; r < 3; ++r) for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t k = 0; k < 3; ++k) composed[r*3+c] += fixed_camera.document_to_image[r*3+k]*transform[k*3+c];
            if (!std::isfinite(composed[r*3+c])) return fail(Outcome::NumericalFailure);
        }
        // Preserve the renderer's normalized, positive-denominator convention.
        // Do not flip a negative denominator into an apparently valid domain.
        const double normalization = composed[8];
        if (!(normalization > 0)) return fail(Outcome::ProjectionUnavailable);
        for (auto& value : composed) { value /= normalization; if (!std::isfinite(value)) return fail(Outcome::NumericalFailure); }
        composed[8] = 1;
        corrected.document_to_image = composed;
    }
    try { ValidateCameraProjection(corrected,raster); }
    catch (const std::invalid_argument&) { return fail(Outcome::ProjectionUnavailable); }
    result.corrected_camera = corrected;
    return result;
}
} // namespace a0::m2::correction
