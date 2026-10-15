#include "a0/m2/document_correction.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace a0::m2;
using namespace a0::m2::render;
using namespace a0::m2::correction;
namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* message) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
template<class F> void Reject(F f, const char* message) { bool threw = false; try { f(); } catch (const std::invalid_argument&) { threw = true; } Check(threw,message); }
void Expect(const DocumentCorrectionResult& value, DocumentCorrectionOutcome outcome, const char* message) {
    Check(value.outcome == outcome && !value.corrected_camera && !value.violations.empty(),message);
}
CameraProjection Camera() { return {{1,1,0,0}, {0,0,0,0,0}, {10,0,5.25,0,10,5.25,0,0,1}}; }
const OutputRaster raster{{0,0,2000,1000}, 254, 20, 10};
DocumentCorrectionPolicy Policy() { return {3, .001, 1e-8, 1e-6, 1000, 180, 200}; }
std::vector<DocumentCorrespondence> Matches(double a, double b, double tx, double ty) {
    std::vector<DocumentCorrespondence> result;
    for (const auto p : std::array<Point,4>{{{0,0},{2,0},{0,1},{2,1}}})
        result.emplace_back(p, Point{a*p.x-b*p.y+tx, b*p.x+a*p.y+ty});
    return result;
}
void TestFit() {
    const auto result = EstimateBoundedDocumentCorrection(Camera(), raster, Matches(1,0,.3,.2), Policy());
    Check(result.outcome == DocumentCorrectionOutcome::Estimated && result.corrected_camera.has_value(), "known translation reaches the renderer seam");
    if (result.estimate) {
        Check(std::abs(result.estimate->translation_x_mm-.3) < 1e-12 && std::abs(result.estimate->translation_y_mm-.2) < 1e-12,
            "translation is signed output-document to camera-document mm");
    }
    for (const double angle : {-.3, .17, 1.0}) {
        const double a = 1.02*std::cos(angle), b = 1.02*std::sin(angle);
        const auto fitted = EstimateBoundedDocumentCorrection(Camera(),raster,Matches(a,b,.2,-.1),Policy());
        Check(fitted.corrected_camera && fitted.estimate && fitted.metrics, "rotation/scale/translation produces a complete candidate");
        if (!fitted.estimate || !fitted.metrics) continue;
        Check(std::abs(fitted.estimate->a-a) < 1e-12 && std::abs(fitted.estimate->b-b) < 1e-12 &&
            std::abs(fitted.estimate->translation_x_mm-.2) < 1e-12 && std::abs(fitted.estimate->translation_y_mm+.1) < 1e-12,
            "independent known similarity recovers all four signed parameters");
        Check(fitted.metrics->maximum_fit_residual_output_pixels < 1e-11 &&
            std::abs(fitted.metrics->scale_difference_percent-2) < 1e-11 &&
            std::abs(fitted.metrics->rotation_degrees-std::abs(angle)*180/std::numbers::pi) < 1e-11,
            "metrics have declared output-pixel, degree and percent units");
    }
    for (const double magnitude : {1e-160,1.0,1e160}) {
        auto points = Matches(1,0,0,0);
        for (auto& p : points) { p.document_mm.x *= magnitude; p.document_mm.y *= magnitude; p.camera_document_mm = p.document_mm; }
        auto policy = Policy(); policy.minimum_rms_spread_mm = magnitude/100;
        policy.maximum_fit_residual_output_pixels = 0; policy.maximum_displacement_output_pixels = 0;
        policy.maximum_rotation_degrees = 0; policy.maximum_scale_difference_percent = 0;
        const auto same = EstimateBoundedDocumentCorrection(Camera(),raster,points,policy);
        Check(same.corrected_camera && same.metrics && same.metrics->maximum_displacement_output_pixels == 0 &&
            same.corrected_camera->document_to_image == Camera().document_to_image,
            "normalized fit preserves exact identity at zero budget across numerical scales");
    }
    std::vector<DocumentCorrespondence> maximum_points;
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        maximum_points.emplace_back(Point{static_cast<double>(x),static_cast<double>(y)},Point{static_cast<double>(x),static_cast<double>(y)});
    auto maximum_policy = Policy(); maximum_policy.minimum_points = kMaximumDocumentCorrectionPoints;
    const auto maximum = EstimateBoundedDocumentCorrection(Camera(),raster,maximum_points,maximum_policy);
    Check(maximum.corrected_camera && maximum.metrics && maximum.metrics->point_count == kMaximumDocumentCorrectionPoints,
        "the exact resource boundary is accepted without dropping points");
}
void TestBudgets() {
    using O = DocumentCorrectionOutcome;
    auto policy = Policy();
    policy.maximum_displacement_output_pixels = 6.25;
    const auto at = EstimateBoundedDocumentCorrection(Camera(),raster,Matches(1,0,.375,.5),policy);
    Check(at.corrected_camera && at.metrics && at.metrics->maximum_displacement_output_pixels == 6.25, "known 6.25px translation at exact budget is accepted");
    policy.maximum_displacement_output_pixels = std::nextafter(6.25,0.0);
    const auto over = EstimateBoundedDocumentCorrection(Camera(),raster,Matches(1,0,.375,.5),policy);
    Expect(over,O::DisplacementExceeded,"one representable step below measured displacement rejects, without clamp");
    Check(over.estimate && over.metrics && over.estimate->translation_y_mm == .5, "rejection retains the original estimate and metrics");
    policy = Policy(); policy.maximum_rotation_degrees = 90;
    Check(EstimateBoundedDocumentCorrection(Camera(),raster,Matches(0,1,0,0),policy).corrected_camera.has_value(), "90 degree boundary accepted");
    policy.maximum_rotation_degrees = std::nextafter(90.0,0.0);
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,Matches(0,1,0,0),policy),O::RotationExceeded,"rotation boundary does not gain an epsilon");
    policy = Policy(); policy.maximum_scale_difference_percent = 50;
    Check(EstimateBoundedDocumentCorrection(Camera(),raster,Matches(1.5,0,0,0),policy).corrected_camera.has_value(), "50 percent boundary accepted");
    policy.maximum_scale_difference_percent = std::nextafter(50.0,0.0);
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,Matches(1.5,0,0,0),policy),O::ScaleExceeded,"scale boundary is enforced");
    policy = Policy(); policy.maximum_displacement_output_pixels = 1; policy.maximum_rotation_degrees = 1; policy.maximum_scale_difference_percent = 1;
    const auto several = EstimateBoundedDocumentCorrection(Camera(),raster,Matches(0,1.5,8,0),policy);
    Check(!several.corrected_camera && several.violations.size() == 3, "all simultaneous budget violations are reported");
    auto noisy = Matches(1,0,0,0); noisy.back().camera_document_mm.x += .1;
    const auto outlier = EstimateBoundedDocumentCorrection(Camera(),raster,noisy,Policy());
    Expect(outlier,O::FitResidualExceeded,"a single inconsistent point is rejected rather than pruned");
    Check(outlier.metrics && outlier.metrics->point_count == noisy.size() && outlier.metrics->maximum_fit_residual_output_pixels > .1,
        "fit residual retains every correspondence");
    if (outlier.metrics) {
        policy = Policy(); policy.maximum_fit_residual_output_pixels = outlier.metrics->maximum_fit_residual_output_pixels;
        Check(EstimateBoundedDocumentCorrection(Camera(),raster,noisy,policy).corrected_camera.has_value(), "fit-residual equality is inclusive");
        policy.maximum_fit_residual_output_pixels = std::nextafter(policy.maximum_fit_residual_output_pixels,0.0);
        Expect(EstimateBoundedDocumentCorrection(Camera(),raster,noisy,policy),O::FitResidualExceeded,"fit-residual limit has no silent tolerance");
    }
    const OutputRaster fractional{{0,0,1001,1},254,11,1};
    policy = Policy(); policy.maximum_displacement_output_pixels = 5.3;
    const auto extended = EstimateBoundedDocumentCorrection(Camera(),fractional,Matches(1.5,0,0,0),policy);
    Expect(extended,O::DisplacementExceeded,"whole ceil raster is bounded, not just requested region or fit points");
    Check(extended.metrics && std::abs(extended.metrics->maximum_displacement_output_pixels-std::sqrt(1.22)*5) < 1e-12,
        "maximum displacement matches independent convex-corner calculation");
    const auto corners = DocumentRasterCorners(fractional);
    Check(std::abs(corners[2].x-1.1) < 1e-15 && std::abs(corners[2].y-.1) < 1e-15, "shared raster domain includes integer-ceiling extension");
}
void TestFailures() {
    using O = DocumentCorrectionOutcome;
    auto points = Matches(1,0,0,0);
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,std::span<const DocumentCorrespondence>(points).first(2),Policy()),O::InsufficientPoints,"too few points cannot produce a projection");
    auto bad = points; bad[1].document_mm.x = std::numeric_limits<double>::quiet_NaN();
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,Policy()),O::InvalidObservations,"nonfinite points rejected");
    bad = points; bad[1].document_mm = bad[0].document_mm;
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,Policy()),O::InvalidObservations,"duplicate source points rejected");
    bad = points; bad[1].camera_document_mm = bad[0].camera_document_mm;
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,Policy()),O::InvalidObservations,"duplicate targets rejected");
    std::vector<DocumentCorrespondence> line{{{0,0},{1,0}},{{1,0},{2,0}},{{2,0},{3,0}}};
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,line,Policy()),O::DegenerateGeometry,"collinear landmarks cannot certify a 2D fit");
    bad = points; for (auto& p : bad) { p.document_mm.y *= 1e-10; p.camera_document_mm = p.document_mm; }
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,Policy()),O::DegenerateGeometry,"near-collinear identity is still rejected");
    bad = points; for (auto& p : bad) { p.document_mm.x *= 1e-6; p.document_mm.y *= 1e-6; p.camera_document_mm = p.document_mm; }
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,Policy()),O::DegenerateGeometry,"insufficient absolute spread is explicit");
    bad = points; for (auto& p : bad) p.camera_document_mm.x = -p.document_mm.x;
    auto policy = Policy(); policy.maximum_fit_residual_output_pixels = 1e6;
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,policy),O::ReflectedCorrespondences,"reflection is refused even under a loose residual budget");
    bad.clear(); for (const auto p : std::array<Point,4>{{{1e308,1e308},{-1e308,1e308},{1e308,-1e308},{-1e308,-1e308}}}) bad.emplace_back(p,p);
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,Policy()),O::NumericalFailure,"unrepresentable centered coordinates fail closed");
    bad = std::vector<DocumentCorrespondence>(kMaximumDocumentCorrectionPoints+1,points[0]);
    Expect(EstimateBoundedDocumentCorrection(Camera(),raster,bad,Policy()),O::InvalidObservations,"resource cap enforced before unbounded work");
    for (int i = 0; i < 9; ++i) {
        auto invalid = Policy();
        if (i == 0) invalid.minimum_points = 2;
        if (i == 1) invalid.minimum_points = kMaximumDocumentCorrectionPoints+1;
        if (i == 2) invalid.minimum_rms_spread_mm = 0;
        if (i == 3) invalid.minimum_axis_variance_ratio = 1;
        if (i == 4) invalid.maximum_fit_residual_output_pixels = -1;
        if (i == 5) invalid.maximum_displacement_output_pixels = std::numeric_limits<double>::infinity();
        if (i == 6) invalid.maximum_rotation_degrees = 181;
        if (i == 7) invalid.maximum_scale_difference_percent = std::numeric_limits<double>::quiet_NaN();
        if (i == 8) invalid.minimum_axis_variance_ratio = 0;
        Reject([&] { (void)EstimateBoundedDocumentCorrection(Camera(),raster,points,invalid); }, "malformed policy throws instead of supplying defaults");
    }
    auto invalidCamera = Camera(); invalidCamera.intrinsics.fx_pixels = 0;
    Reject([&] { (void)EstimateBoundedDocumentCorrection(invalidCamera,raster,points,Policy()); }, "invalid fixed projection rejected before estimation");
    const OutputRaster invalidRaster{{0,0,2000,1000},254,21,10};
    Reject([&] { (void)EstimateBoundedDocumentCorrection(Camera(),invalidRaster,points,Policy()); }, "inconsistent raster rejected");
    const CameraProjection projective{{1,1,0,0},{0,0,0,0,0},{1,0,0,0,1,0,-.2,0,1}};
    for (const double tx : {3.0,5.0,6.0})
        Expect(EstimateBoundedDocumentCorrection(projective,raster,Matches(1,0,tx,0),Policy()),O::ProjectionUnavailable,"corrected denominator crosses zero or cannot normalize");
    const OutputRaster fractional{{0,0,2010,1000},254,21,10};
    Expect(EstimateBoundedDocumentCorrection(projective,fractional,Matches(1,0,2.95,0),Policy()),O::ProjectionUnavailable,"corrected projection is checked beyond the requested region to ceil edges");
    const CameraProjection folding{{1,1,0,0},{-.01,0,0,0,0},{1,0,0,0,1,0,0,0,1}};
    Expect(EstimateBoundedDocumentCorrection(folding,raster,Matches(3,0,0,0),Policy()),O::ProjectionUnavailable,"lens validity is checked over the corrected domain");
}
// Independent direct pinhole/distortion oracle, fed the KNOWN generating
// similarity rather than the fitted result or its composed matrix.
Point Oracle(const CameraProjection& camera, Point p) {
    const auto& h = camera.document_to_image;
    const double w = h[6]*p.x+h[7]*p.y+1;
    const double xn = ((h[0]*p.x+h[1]*p.y+h[2])/w-camera.intrinsics.cx_pixels)/camera.intrinsics.fx_pixels;
    const double yn = ((h[3]*p.x+h[4]*p.y+h[5])/w-camera.intrinsics.cy_pixels)/camera.intrinsics.fy_pixels;
    const auto& d = camera.distortion; const double r2 = xn*xn+yn*yn;
    return {camera.intrinsics.cx_pixels+camera.intrinsics.fx_pixels*(xn+d.k1*xn*r2+d.k2*xn*r2*r2+d.k3*xn*r2*r2*r2+2*d.p1*xn*yn+d.p2*(3*xn*xn+yn*yn)),
        camera.intrinsics.cy_pixels+camera.intrinsics.fy_pixels*(yn+d.k1*yn*r2+d.k2*yn*r2*r2+d.k3*yn*r2*r2*r2+d.p1*(xn*xn+3*yn*yn)+2*d.p2*xn*yn)};
}
void TestProjection() {
    const CameraProjection base{{6200,6100,3679.5,2455.5},{-.04,.006,0,.0003,-.0002},
        {3.1,.2,1500,-.1,2.9,2000,.0001,-.00007,1}};
    const double a = 1.02*std::cos(.17), b = 1.02*std::sin(.17);
    const auto result = EstimateBoundedDocumentCorrection(base,raster,Matches(a,b,.2,-.1),Policy());
    Check(result.corrected_camera.has_value(),"projective baseline and distortion accept a bounded similarity");
    if (!result.corrected_camera) return;
    double maximum = 0;
    for (int y = 0; y <= 10; ++y) for (int x = 0; x <= 20; ++x) {
        const Point p{x/10.0,y/10.0}; const auto expected = Oracle(base,{a*p.x-b*p.y+.2,b*p.x+a*p.y-.1});
        Point actual;
        Check(TryProjectDocument(*result.corrected_camera,p,actual),"corrected production projection exists");
        maximum = std::max(maximum,std::hypot(actual.x-expected.x,actual.y-expected.y));
    }
    Check(maximum < 1e-9,"composed H*T and unchanged distortion match the independent oracle");
    const auto unchanged = Oracle(base,{1,1}); const auto correct = Oracle(base,{a-b+.2,b+a-.1});
    Check(std::hypot((a*unchanged.x-b*unchanged.y+.2)-correct.x,(b*unchanged.x+a*unchanged.y-.1)-correct.y) > 1,
        "fixture detects wrong left-composition in camera pixel coordinates");
    std::cout << "projection oracle maximum=" << maximum << " px\n";
}
void TestRender() {
    BgrImage imageA{40,24,std::vector<std::uint8_t>(40*24*3)}, imageB = imageA;
    for (std::size_t y = 0; y < 24; ++y) for (std::size_t x = 0; x < 40; ++x) for (std::size_t c = 0; c < 3; ++c) {
        imageA.bgr[(y*40+x)*3+c] = static_cast<std::uint8_t>(80+x+2*y+c);
        imageB.bgr[(y*40+x)*3+c] = static_cast<std::uint8_t>(73+x+2*y+c);
    }
    const auto originalA = imageA.bgr, originalB = imageB.bgr;
    const auto camera = Camera(); const auto points = Matches(1,0,.3,.2);
    const auto result = EstimateBoundedDocumentCorrection(camera,raster,points,Policy());
    if (!result.corrected_camera) { Check(false,"render fixture has a valid correction"); return; }
    const auto again = EstimateBoundedDocumentCorrection(camera,raster,points,Policy());
    Check(again.corrected_camera && again.corrected_camera->document_to_image == result.corrected_camera->document_to_image,"repeated estimation is deterministic");
    for (const auto kernel : {Resampling::bilinear,Resampling::bicubic_catmull_rom})
        for (const auto layout : {StitchLayout::camera_a_left_camera_b_right,StitchLayout::camera_a_top_camera_b_bottom}) {
            const DocumentRenderParameters parameters{raster,camera,*result.corrected_camera,layout,kernel};
            const auto rendered = RenderDocumentPair(imageA,imageB,parameters);
            bool expected = rendered.seam_navigation.available;
            for (std::size_t y = 0; y < 10; ++y) for (std::size_t x = 0; x < 20; ++x) for (std::size_t c = 0; c < 3; ++c)
                expected = expected && rendered.image.bgr[(y*20+x)*3+c] == 97+x+2*y+c;
            Check(expected,"single-pass renderer restores independently known pixels across the seam");
            Check(RenderDocumentPair(imageA,imageB,parameters).image.bgr == rendered.image.bgr,"repeated corrected render is byte identical");
            Check(RenderDocumentPair(imageA,imageB,{raster,camera,camera,layout,kernel}).image.bgr != rendered.image.bgr,"negative control exposes the uncorrected shift");
        }
    Check(imageA.bgr == originalA && imageB.bgr == originalB && camera.document_to_image == Camera().document_to_image,
        "both original buffers and fixed baseline remain unchanged");
}
}
int main() {
    static_assert(!std::is_default_constructible_v<DocumentCorrectionPolicy>);
    try { TestFit(); TestBudgets(); TestFailures(); TestProjection(); TestRender(); } catch (const std::exception& e) { ++failures; std::cerr << e.what() << '\n'; }
    std::cout << "document_correction checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
