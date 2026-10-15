#include "a0/m2/raw_document_correction.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace a0::m2::correction {
namespace {
using namespace render;
using Outcome = RawDocumentOutcome;
bool Finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }
void ValidatePolicy(const RawInversePolicy& p) {
    if (p.maximum_newton_updates < 1 || p.maximum_newton_updates > 1000 ||
        p.maximum_backtracking_steps < 1 || p.maximum_backtracking_steps > 64 ||
        !std::isfinite(p.maximum_round_trip_error_pixels) || !(p.maximum_round_trip_error_pixels > 0))
        throw std::invalid_argument("raw document inverse: invalid numerical policy");
}
struct LensSample { Point distorted; double xx, xy, yy; };
LensSample Evaluate(const LensDistortion& d, Point p) {
    const double x = p.x, y = p.y, t = x*x+y*y;
    const double radial = 1+d.k1*t+d.k2*t*t+d.k3*t*t*t;
    const double derivative = 2*(d.k1+2*d.k2*t+3*d.k3*t*t);
    return {{x*radial+2*d.p1*x*y+d.p2*(t+2*x*x),
             y*radial+d.p1*(t+2*y*y)+2*d.p2*x*y},
        radial+x*x*derivative+2*d.p1*y+6*d.p2*x,
        x*y*derivative+2*d.p1*x+2*d.p2*y,
        radial+y*y*derivative+6*d.p1*y+2*d.p2*x};
}
struct Domain { std::array<Point,4> corners; double radius; bool lens_available; };
// Outward rounding of nonnegative bounds (including accumulated sums).
double Up(double x) { return std::nextafter(x,std::numeric_limits<double>::infinity()); }
double Down(double x) { return std::nextafter(x,-std::numeric_limits<double>::infinity()); }
struct Interval { double low, high; };
Interval Exact(double x) { return {x,x}; }
Interval Add(Interval a, Interval b) { return {Down(a.low+b.low),Up(a.high+b.high)}; }
Interval Multiply(Interval a, Interval b) {
    const std::array<double,4> products{a.low*b.low,a.low*b.high,a.high*b.low,a.high*b.high};
    for (const double value : products) if (!std::isfinite(value)) {
        const double nan = std::numeric_limits<double>::quiet_NaN(); return {nan,nan};
    }
    return {Down(*std::min_element(products.begin(),products.end())),Up(*std::max_element(products.begin(),products.end()))};
}
Interval DividePositive(Interval a, Interval b) {
    return Multiply(a,{Down(1/b.high),Up(1/b.low)});
}
bool Valid(Interval a) { return std::isfinite(a.low) && std::isfinite(a.high) && a.low <= a.high; }
Domain Prepare(const CameraProjection& camera, const OutputRaster& raster, RawImageExtent image,
    const RawInversePolicy& policy) {
    ValidatePolicy(policy);
    (void)PixelCount(image.width,image.height);
    ValidateCameraProjection(camera,raster);
    Domain result{DocumentRasterCorners(raster),0,false};
    const auto& h = camera.document_to_image;
    const auto& k = camera.intrinsics;
    for (const auto p : result.corners) {
        const auto row = [&](std::size_t r) {
            return Add(Add(Multiply(Exact(h[r*3]),Exact(p.x)),Multiply(Exact(h[r*3+1]),Exact(p.y))),Exact(h[r*3+2]));
        };
        const auto w = row(2);
        if (!Valid(w) || !(w.low > 0)) return result;
        const auto x = DividePositive(Add(DividePositive(row(0),w),Exact(-k.cx_pixels)),Exact(k.fx_pixels));
        const auto y = DividePositive(Add(DividePositive(row(1),w),Exact(-k.cy_pixels)),Exact(k.fy_pixels));
        if (!Valid(x) || !Valid(y)) return result;
        const double mx = std::max(std::abs(x.low),std::abs(x.high)), my = std::max(std::abs(y.low),std::abs(y.high));
        // Enclose all basic arithmetic, not just the final hypot rounding.
        const double radius = Up(std::sqrt(Up(Up(mx*mx)+Up(my*my))));
        result.radius = std::max(result.radius,radius);
    }
    // For radial Jacobian eigenvalues, bound all negative coefficients of
    // 1+3*k1*t+5*k2*t^2+7*k3*t^3. This also lower-bounds radial(t).
    // Tangential Jacobian spectral norm <= 6*hypot(p1,p2)*R on the disk.
    // A positive lower bound makes the symmetric Jacobian positive definite
    // throughout a convex disk, hence the lens map is injective there.
    const auto& d = camera.distortion;
    const double t = Up(result.radius*result.radius);
    const double c1 = Up(3*std::max(0.0,-d.k1));
    const double c2 = Up(5*std::max(0.0,-d.k2));
    const double c3 = Up(7*std::max(0.0,-d.k3));
    const double radial_loss = Up(Up(Up(Up(c3*t)+c2)*t)+c1);
    const double tangential_loss = Up(Up(6*Up(std::hypot(d.p1,d.p2)))*result.radius);
    const double loss = Up(Up(radial_loss*t)+tangential_loss);
    result.lens_available = std::isfinite(result.radius) && std::isfinite(loss) && loss < 1;
    return result;
}
bool InDisk(Point p, const Domain& domain) { return Finite(p) && Up(std::hypot(p.x,p.y)) <= domain.radius; }
double Error(const CameraIntrinsics& k, Point projected, Point target) {
    return std::hypot((projected.x-target.x)*k.fx_pixels,(projected.y-target.y)*k.fy_pixels);
}
std::optional<Point> UndoHomography(const CameraProjection& camera, Point normalized) {
    const auto& k = camera.intrinsics;
    const auto& h = camera.document_to_image;
    const double u = k.fx_pixels*normalized.x+k.cx_pixels, v = k.fy_pixels*normalized.y+k.cy_pixels;
    // Intersect two homogeneous lines, scaled separately. No legacy absolute
    // determinant epsilon (which rejects legitimate small document scales).
    std::array<double,3> a{h[0]-u*h[6],h[1]-u*h[7],h[2]-u};
    std::array<double,3> b{h[3]-v*h[6],h[4]-v*h[7],h[5]-v};
    for (auto* row : {&a,&b}) {
        const double scale = std::max({std::abs((*row)[0]),std::abs((*row)[1]),std::abs((*row)[2])});
        if (!std::isfinite(scale) || !(scale > 0)) return {};
        for (auto& value : *row) value /= scale;
    }
    const double w = a[0]*b[1]-a[1]*b[0];
    if (!std::isfinite(w) || w == 0) return {};
    const Point result{(a[1]*b[2]-a[2]*b[1])/w,(a[2]*b[0]-a[0]*b[2])/w};
    if (!Finite(result)) return {};
    return result;
}
RawDocumentResult Map(const CameraProjection& camera, RawImageExtent image, Point raw,
    const RawInversePolicy& policy, const Domain& domain) {
    RawDocumentResult result{Outcome::InvalidRawPixel,{},{},0};
    const auto fail = [&](Outcome why) { result.outcome = why; return result; };
    if (!Finite(raw) || raw.x < 0 || raw.y < 0 || raw.x >= image.width || raw.y >= image.height) return result;
    if (!domain.lens_available) return fail(Outcome::LensDomainUnavailable);
    const auto& k = camera.intrinsics;
    const Point target{(raw.x-k.cx_pixels)/k.fx_pixels,(raw.y-k.cy_pixels)/k.fy_pixels};
    if (!Finite(target)) return fail(Outcome::NumericalFailure);
    Point current{0,0}; // Always inside the certified disk; never choose an outside root.
    for (std::uint32_t update = 0; update <= policy.maximum_newton_updates; ++update) {
        result.newton_updates = update;
        const auto sample = Evaluate(camera.distortion,current);
        const double error = Error(k,sample.distorted,target);
        const double determinant = sample.xx*sample.yy-sample.xy*sample.xy;
        if (!std::isfinite(error) || !std::isfinite(determinant) || !(sample.xx > 0) || !(determinant > 0))
            return fail(Outcome::NumericalFailure);
        if (error <= policy.maximum_round_trip_error_pixels) {
            const auto document = UndoHomography(camera,current);
            if (!document) return fail(Outcome::NumericalFailure);
            if (document->x < domain.corners[0].x || document->x > domain.corners[2].x ||
                document->y < domain.corners[0].y || document->y > domain.corners[2].y)
                return fail(Outcome::OutsideDocumentDomain);
            Point check{};
            if (!TryProjectDocument(camera,*document,check)) return fail(Outcome::NumericalFailure);
            const double round_trip = std::hypot(check.x-raw.x,check.y-raw.y);
            if (!std::isfinite(round_trip)) return fail(Outcome::NumericalFailure);
            result.round_trip_error_pixels = round_trip;
            if (round_trip > policy.maximum_round_trip_error_pixels) return fail(Outcome::RoundTripExceeded);
            result.outcome = Outcome::Mapped; result.document_mm = document;
            return result;
        }
        if (update == policy.maximum_newton_updates) break;
        const double dx = sample.distorted.x-target.x, dy = sample.distorted.y-target.y;
        const Point step{(sample.yy*dx-sample.xy*dy)/determinant,(sample.xx*dy-sample.xy*dx)/determinant};
        if (!Finite(step)) return fail(Outcome::NumericalFailure);
        bool advanced = false;
        double fraction = 1;
        for (std::uint32_t backtrack = 0; backtrack < policy.maximum_backtracking_steps; ++backtrack) {
            const Point candidate{current.x-fraction*step.x,current.y-fraction*step.y};
            if (InDisk(candidate,domain)) {
                const double candidate_error = Error(k,Evaluate(camera.distortion,candidate).distorted,target);
                if (std::isfinite(candidate_error) && candidate_error < error) {
                    current = candidate; advanced = true; break;
                }
            }
            fraction *= .5;
        }
        if (!advanced) return fail(Outcome::UndistortionNonconvergence);
    }
    return fail(Outcome::UndistortionNonconvergence);
}
} // namespace

RawDocumentResult MapRawPixelToDocument(const CameraProjection& camera, const OutputRaster& raster,
    RawImageExtent image, Point raw, const RawInversePolicy& policy) {
    return Map(camera,image,raw,policy,Prepare(camera,raster,image,policy));
}
RawPairDocumentCorrectionResult EstimateRawPairDocumentCorrection(const CameraProjection& camera_a,
    const CameraProjection& camera_b, const OutputRaster& raster, RawImageExtent image_a, RawImageExtent image_b,
    std::span<const RawPixelCorrespondence> matches, const RawInversePolicy& inverse_policy,
    const DocumentCorrectionPolicy& correction_policy) {
    const auto domain_a = Prepare(camera_a,raster,image_a,inverse_policy);
    const auto domain_b = Prepare(camera_b,raster,image_b,inverse_policy);
    // Validate the entire correction policy even when point mapping fails.
    const auto count_rejection = EstimateBoundedDocumentCorrection(camera_b,raster,{},correction_policy);
    RawPairDocumentCorrectionResult result{RawPairCorrectionOutcome::InvalidCorrespondences,{}, {}};
    if (matches.size() > kMaximumDocumentCorrectionPoints) return result;
    if (matches.size() < correction_policy.minimum_points) {
        result.outcome = RawPairCorrectionOutcome::CorrectionRejected; result.correction = count_rejection; return result;
    }
    bool complete = true;
    std::vector<DocumentCorrespondence> document_matches;
    document_matches.reserve(matches.size()); result.points.reserve(matches.size());
    for (const auto& match : matches) {
        auto a = Map(camera_a,image_a,match.camera_a_raw_pixel,inverse_policy,domain_a);
        auto b = Map(camera_b,image_b,match.camera_b_raw_pixel,inverse_policy,domain_b);
        if (a.document_mm && b.document_mm) document_matches.emplace_back(*a.document_mm,*b.document_mm);
        else complete = false;
        result.points.push_back({a,b});
    }
    if (!complete) { result.outcome = RawPairCorrectionOutcome::PointMappingFailed; return result; }
    result.correction = EstimateBoundedDocumentCorrection(camera_b,raster,document_matches,correction_policy);
    result.outcome = result.correction->corrected_camera ? RawPairCorrectionOutcome::Estimated : RawPairCorrectionOutcome::CorrectionRejected;
    return result;
}
} // namespace a0::m2::correction
