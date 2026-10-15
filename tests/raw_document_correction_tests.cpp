#include "a0/m2/raw_document_correction.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace a0::m2;
using namespace a0::m2::render;
using namespace a0::m2::correction;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; } }
template<class F> void Reject(F f, const char* why) {
    bool rejected = false; try { f(); } catch (const std::invalid_argument&) { rejected = true; } Check(rejected,why);
}
const OutputRaster small{{0,0,2000,1000},254,20,10};
const OutputRaster full{{0,0,841000,594000},1,34,24};
const RawImageExtent extent{7360,4912};
RawInversePolicy Inverse() { return {40,32,1e-9}; }
DocumentCorrectionPolicy Fit() { return {3,.001,1e-8,1e-6,1000,180,200}; }
CameraProjection Simple() { return {{1,1,0,0},{0,0,0,0,0},{10,0,5.25,0,10,5.25,0,0,1}}; }
CameraProjection A() { return {{6200,6100,3679.5,2455.5},{-.04,.006,-.0002,.0003,-.0002},
    {3.1,.2,1500,-.1,2.9,2000,.0001,-.00007,1}}; }
CameraProjection B() { return {{6000,6250,3650,2400},{.02,-.003,.0001,-.0002,.0004},
    {3.4,-.15,1800,.12,3.0,1700,-.00008,.00006,1}}; }
// Independent forward oracle. No inverse or production projection called.
Point Oracle(const CameraProjection& camera, Point p) {
    const auto& h = camera.document_to_image; const auto& k = camera.intrinsics; const auto& d = camera.distortion;
    const double w = h[6]*p.x+h[7]*p.y+1;
    const double x = ((h[0]*p.x+h[1]*p.y+h[2])/w-k.cx_pixels)/k.fx_pixels;
    const double y = ((h[3]*p.x+h[4]*p.y+h[5])/w-k.cy_pixels)/k.fy_pixels;
    const double t = x*x+y*y;
    return {k.cx_pixels+k.fx_pixels*(x+d.k1*x*t+d.k2*x*t*t+d.k3*x*t*t*t+2*d.p1*x*y+d.p2*(3*x*x+y*y)),
            k.cy_pixels+k.fy_pixels*(y+d.k1*y*t+d.k2*y*t*t+d.k3*y*t*t*t+d.p1*(x*x+3*y*y)+2*d.p2*x*y)};
}
Point Similarity(Point p) {
    const double a = 1.002*std::cos(.006), b = 1.002*std::sin(.006);
    return {a*p.x-b*p.y+1.5,b*p.x+a*p.y-1};
}
std::vector<RawPixelCorrespondence> SmallMatches() {
    std::vector<RawPixelCorrespondence> matches;
    for (const auto p : std::array<Point,4>{{{.2,.2},{1.2,.2},{.2,.6},{1.2,.6}}})
        matches.emplace_back(Oracle(Simple(),p),Oracle(Simple(),{p.x+.3,p.y+.2}));
    return matches;
}
void TestInverse() {
    double max_mm = 0, max_pixels = 0;
    for (const auto camera : {A(),B()}) for (int y = 0; y < 9; ++y) for (int x = 0; x < 13; ++x) {
        const Point p{10+65.0*x,10+70.0*y}; const auto raw = Oracle(camera,p);
        const auto mapped = MapRawPixelToDocument(camera,full,extent,raw,Inverse());
        Check(mapped.outcome == RawDocumentOutcome::Mapped && mapped.document_mm && mapped.round_trip_error_pixels,
            "independent distorted/projective points invert within the full ceil raster");
        if (!mapped.document_mm || !mapped.round_trip_error_pixels) continue;
        max_mm = std::max(max_mm,std::hypot(mapped.document_mm->x-p.x,mapped.document_mm->y-p.y));
        max_pixels = std::max(max_pixels,*mapped.round_trip_error_pixels);
    }
    Check(max_mm < 1e-8 && max_pixels <= Inverse().maximum_round_trip_error_pixels,
        "independent truth and actual production round trip both meet explicit tolerances");
    std::cout << "inverse oracle maximum=" << max_mm << " mm; round trip=" << max_pixels << " px\n";
    for (const auto p : DocumentRasterCorners(small)) {
        const auto mapped = MapRawPixelToDocument(Simple(),small,extent,Oracle(Simple(),p),Inverse());
        Check(mapped.document_mm && mapped.document_mm->x == p.x && mapped.document_mm->y == p.y,
            "exactly representable closed document domain includes all four boundary corners");
    }
    const OutputRaster fractional{{0,0,1001,1},254,11,1};
    Check(MapRawPixelToDocument(Simple(),fractional,extent,Oracle(Simple(),{1.05,.05}),Inverse()).document_mm.has_value(),
        "ceil extension is part of the allowed inverse domain");
    auto tiny = Simple(); tiny.document_to_image = {1e-14,0,0,0,1e-14,0,0,0,1};
    // A much tighter explicit numerical tolerance is required for this scale.
    const auto mapped = MapRawPixelToDocument(tiny,small,extent,{1e-14,5e-15},{4,4,1e-30});
    Check(mapped.document_mm && std::abs(mapped.document_mm->x-1) < 1e-14 && std::abs(mapped.document_mm->y-.5) < 1e-14,
        "small nonsingular H is not rejected by the legacy absolute determinant cutoff");
    const auto no_lens = Simple();
    const auto exact = MapRawPixelToDocument(no_lens,small,extent,{15.25,10.25},{1,1,1e-30});
    Check(exact.document_mm && exact.round_trip_error_pixels && *exact.round_trip_error_pixels == 0 && exact.newton_updates == 1,
        "zero round-trip error at a representable point needs only one Newton update");
}
void TestPair() {
    const auto camera_a = A(), camera_b = B();
    std::vector<RawPixelCorrespondence> matches;
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 9; ++x) {
        const Point p{20+100.0*x,20+90.0*y}; matches.emplace_back(Oracle(camera_a,p),Oracle(camera_b,Similarity(p)));
    }
    const auto original = matches;
    const auto result = EstimateRawPairDocumentCorrection(camera_a,camera_b,full,extent,extent,matches,Inverse(),Fit());
    Check(result.outcome == RawPairCorrectionOutcome::Estimated && result.correction && result.correction->corrected_camera,
        "all raw correspondences feed the bounded four-parameter correction");
    Check(result.points.size() == matches.size(),"both per-camera diagnostics retain the input order and full point count");
    if (!result.correction || !result.correction->estimate || !result.correction->corrected_camera) return;
    const auto& fit = *result.correction->estimate;
    Check(std::abs(fit.a-1.002*std::cos(.006)) < 1e-10 && std::abs(fit.b-1.002*std::sin(.006)) < 1e-10 &&
        std::abs(fit.translation_x_mm-1.5) < 1e-8 && std::abs(fit.translation_y_mm+1) < 1e-8,
        "raw A-to-B inputs recover independently known scale/rotation/translation with correct direction");
    double maximum = 0;
    for (int y = 0; y < 9; ++y) for (int x = 0; x < 13; ++x) {
        const Point p{10+65.0*x,10+70.0*y}; Point actual{};
        Check(TryProjectDocument(*result.correction->corrected_camera,p,actual),"corrected B projects the dense output grid");
        const auto truth = Oracle(camera_b,Similarity(p)); maximum = std::max(maximum,std::hypot(actual.x-truth.x,actual.y-truth.y));
    }
    Check(maximum < 1e-7,"raw-to-document-to-H*T matches an independent dense camera-pixel oracle");
    const auto again = EstimateRawPairDocumentCorrection(camera_a,camera_b,full,extent,extent,matches,Inverse(),Fit());
    Check(again.correction && again.correction->corrected_camera && again.correction->corrected_camera->document_to_image ==
        result.correction->corrected_camera->document_to_image,"repeated raw correction is deterministic");
    bool unchanged = camera_a.document_to_image == A().document_to_image && camera_b.document_to_image == B().document_to_image;
    for (std::size_t i = 0; i < matches.size(); ++i) unchanged = unchanged && matches[i].camera_a_raw_pixel.x == original[i].camera_a_raw_pixel.x &&
        matches[i].camera_a_raw_pixel.y == original[i].camera_a_raw_pixel.y && matches[i].camera_b_raw_pixel.x == original[i].camera_b_raw_pixel.x &&
        matches[i].camera_b_raw_pixel.y == original[i].camera_b_raw_pixel.y;
    const auto& corrected = *result.correction->corrected_camera;
    Check(unchanged && corrected.intrinsics.fx_pixels == camera_b.intrinsics.fx_pixels && corrected.distortion.k3 == camera_b.distortion.k3 &&
        corrected.distortion.p2 == camera_b.distortion.p2,"raw matches, fixed camera A/B, K and lens are immutable");
    std::cout << "raw pair projection oracle maximum=" << maximum << " px\n";
}
void TestFailures() {
    using O = RawDocumentOutcome;
    const auto expect = [&](Point p, O outcome, const char* why) {
        const auto r = MapRawPixelToDocument(Simple(),small,extent,p,Inverse()); Check(r.outcome == outcome && !r.document_mm,why);
    };
    expect({-1,2},O::InvalidRawPixel,"negative stored-order pixel rejected");
    expect({7360,2},O::InvalidRawPixel,"width boundary is exclusive");
    expect({2,4912},O::InvalidRawPixel,"height boundary is exclusive");
    expect({std::numeric_limits<double>::quiet_NaN(),2},O::InvalidRawPixel,"NaN cannot be a pixel");
    expect({2,std::numeric_limits<double>::infinity()},O::InvalidRawPixel,"infinite raw pixel rejected");
    expect(Oracle(Simple(),{-.125,.5}),O::OutsideDocumentDomain,"in-frame observation outside the declared document domain rejected");
    // This point is inside the certified disk, but outside the document rectangle.
    expect(Oracle(Simple(),{1,1.125}),O::OutsideDocumentDomain,"lens disk is not substituted for the document rectangle");
    auto tangential = Simple(); tangential.distortion.p2 = 1;
    const auto ambiguous = MapRawPixelToDocument(tangential,small,extent,{10,10},Inverse());
    Check(ambiguous.outcome == O::LensDomainUnavailable && !ambiguous.document_mm,
        "radially valid but tangentially folded lens cannot choose an arbitrary inverse root");
    auto uncertified = A(); uncertified.distortion.k1 = -10; uncertified.distortion.k2 = 1000;
    const auto conservative = MapRawPixelToDocument(uncertified,full,extent,Oracle(uncertified,{400,200}),Inverse());
    Check(conservative.outcome == O::LensDomainUnavailable && !conservative.document_mm,
        "conservative injectivity bound may reject a valid lens rather than guessing uniqueness");
    auto overflow = Simple(); overflow.distortion.p2 = 1e308;
    const auto unavailable = MapRawPixelToDocument(overflow,small,extent,{10,10},Inverse());
    Check(unavailable.outcome == O::LensDomainUnavailable && !unavailable.document_mm,
        "overflow in the injectivity bound cannot make an apparently valid domain");
    const CameraProjection extreme{{1e-308,1e308,0,0},{0,0,0,0,0},{1e-308,0,0,0,1e308,0,0,0,1}};
    const auto numerical = MapRawPixelToDocument(extreme,small,extent,{1000,1},Inverse());
    Check(numerical.outcome == O::NumericalFailure && !numerical.document_mm,"unrepresentable raw normalization fails with no point");
    const auto slow = MapRawPixelToDocument(A(),full,extent,Oracle(A(),{700,500}),{1,32,1e-12});
    Check(slow.outcome == O::UndistortionNonconvergence && !slow.document_mm && slow.newton_updates == 1,
        "explicit Newton update limit is enforced, no approximate fallback point");
    const auto limited = MapRawPixelToDocument(B(),full,extent,Oracle(B(),{20,20}),{1,1,1e-12});
    Check(!limited.document_mm,"bounded line search cannot create success after exhaustion");
    auto matches = SmallMatches(); matches[1].camera_b_raw_pixel.x = -1; matches[2].camera_a_raw_pixel.y = -1;
    const auto partial = EstimateRawPairDocumentCorrection(Simple(),Simple(),small,extent,extent,matches,Inverse(),Fit());
    Check(partial.outcome == RawPairCorrectionOutcome::PointMappingFailed && !partial.correction && partial.points.size() == 4 &&
        partial.points[1][0].document_mm && !partial.points[1][1].document_mm && !partial.points[2][0].document_mm && partial.points[2][1].document_mm &&
        partial.points[3][0].document_mm && partial.points[3][1].document_mm,"both sides and later points diagnosed, failed points never pruned into a candidate");
    auto policy = Fit(); policy.maximum_displacement_output_pixels = 1;
    const auto over = EstimateRawPairDocumentCorrection(Simple(),Simple(),small,extent,extent,SmallMatches(),Inverse(),policy);
    Check(over.outcome == RawPairCorrectionOutcome::CorrectionRejected && over.correction && over.correction->estimate && !over.correction->corrected_camera &&
        over.correction->outcome == DocumentCorrectionOutcome::DisplacementExceeded,"raw bridge preserves rejection and original over-limit diagnostics");
    matches = SmallMatches(); matches[1] = matches[0];
    const auto duplicate = EstimateRawPairDocumentCorrection(Simple(),Simple(),small,extent,extent,matches,Inverse(),Fit());
    Check(duplicate.outcome == RawPairCorrectionOutcome::CorrectionRejected && duplicate.correction && !duplicate.correction->corrected_camera,
        "raw duplicate coordinates are not turned into independent landmarks");
    const auto short_pair = EstimateRawPairDocumentCorrection(Simple(),Simple(),small,extent,extent,{},Inverse(),Fit());
    Check(short_pair.correction && short_pair.correction->outcome == DocumentCorrectionOutcome::InsufficientPoints && !short_pair.correction->corrected_camera,
        "insufficient pair retains the existing count rejection");
    matches = std::vector<RawPixelCorrespondence>(kMaximumDocumentCorrectionPoints+1,SmallMatches()[0]);
    const auto large = EstimateRawPairDocumentCorrection(Simple(),Simple(),small,extent,extent,matches,Inverse(),Fit());
    Check(large.outcome == RawPairCorrectionOutcome::InvalidCorrespondences && large.points.empty() && !large.correction,
        "bounded correspondence count prevents unbounded inverse work");
    matches.clear();
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const auto raw = Oracle(Simple(),{.125+x/64.0,.125+y/128.0}); matches.emplace_back(raw,raw);
    }
    const auto maximum = EstimateRawPairDocumentCorrection(Simple(),Simple(),small,extent,extent,matches,Inverse(),Fit());
    Check(maximum.outcome == RawPairCorrectionOutcome::Estimated && maximum.correction && maximum.correction->corrected_camera &&
        maximum.points.size() == kMaximumDocumentCorrectionPoints && maximum.correction->metrics &&
        maximum.correction->metrics->point_count == kMaximumDocumentCorrectionPoints,"exact resource boundary evaluates and retains every raw pair");
    for (int i = 0; i < 8; ++i) {
        auto invalid = Inverse();
        if (i == 0) invalid.maximum_newton_updates = 0;
        if (i == 1) invalid.maximum_newton_updates = 1001;
        if (i == 2) invalid.maximum_backtracking_steps = 0;
        if (i == 3) invalid.maximum_backtracking_steps = 65;
        if (i == 4) invalid.maximum_round_trip_error_pixels = 0;
        if (i == 5) invalid.maximum_round_trip_error_pixels = -1;
        if (i == 6) invalid.maximum_round_trip_error_pixels = std::numeric_limits<double>::infinity();
        if (i == 7) invalid.maximum_round_trip_error_pixels = std::numeric_limits<double>::quiet_NaN();
        Reject([&] { (void)MapRawPixelToDocument(Simple(),small,extent,{10,10},invalid); },"invalid numerical inverse policy throws");
    }
    Reject([&] { (void)MapRawPixelToDocument(Simple(),small,{0,24},{10,10},Inverse()); },"zero image dimension rejected before mapping");
    Reject([&] { (void)MapRawPixelToDocument(Simple(),small,{32769,24},{10,10},Inverse()); },"oversize image rejected");
    auto broken = Simple(); broken.document_to_image[8] = 2;
    Reject([&] { (void)MapRawPixelToDocument(broken,small,extent,{10,10},Inverse()); },"invalid fixed projection still throws");
    auto bad_fit = Fit(); bad_fit.maximum_rotation_degrees = 181;
    Reject([&] { (void)EstimateRawPairDocumentCorrection(Simple(),Simple(),small,extent,extent,{},Inverse(),bad_fit); },
        "correction policy validated even without observations");
    auto folded = Simple(); folded.distortion.k1 = -1;
    Reject([&] { (void)MapRawPixelToDocument(folded,small,extent,{10,10},Inverse()); },"radial fold remains an invalid fixed projection");
}
void TestRender() {
    BgrImage image_a{40,24,std::vector<std::uint8_t>(40*24*3)}, image_b = image_a;
    for (std::size_t y = 0; y < 24; ++y) for (std::size_t x = 0; x < 40; ++x) for (std::size_t c = 0; c < 3; ++c) {
        image_a.bgr[(y*40+x)*3+c] = static_cast<std::uint8_t>(80+x+2*y+c);
        image_b.bgr[(y*40+x)*3+c] = static_cast<std::uint8_t>(73+x+2*y+c);
    }
    const auto original_a = image_a.bgr, original_b = image_b.bgr;
    const auto result = EstimateRawPairDocumentCorrection(Simple(),Simple(),small,{40,24},{40,24},SmallMatches(),Inverse(),Fit());
    Check(result.correction && result.correction->corrected_camera,"raw pixel bridge creates a renderer-ready B projection");
    if (!result.correction || !result.correction->corrected_camera) return;
    for (const auto kernel : {Resampling::bilinear,Resampling::bicubic_catmull_rom})
        for (const auto layout : {StitchLayout::camera_a_left_camera_b_right,StitchLayout::camera_a_top_camera_b_bottom}) {
            const DocumentRenderParameters parameters{small,Simple(),*result.correction->corrected_camera,layout,kernel};
            const auto rendered = RenderDocumentPair(image_a,image_b,parameters);
            bool expected = rendered.seam_navigation.available;
            for (std::size_t y = 0; y < 10; ++y) for (std::size_t x = 0; x < 20; ++x) for (std::size_t c = 0; c < 3; ++c)
                expected = expected && rendered.image.bgr[(y*20+x)*3+c] == 97+x+2*y+c;
            Check(expected,"raw correspondences restore independently known pixels through the actual single-pass renderer");
            Check(RenderDocumentPair(image_a,image_b,parameters).image.bgr == rendered.image.bgr,"corrected pixels repeat byte identically");
            Check(RenderDocumentPair(image_a,image_b,{small,Simple(),Simple(),layout,kernel}).image.bgr != rendered.image.bgr,
                "uncorrected control detects the image shift");
        }
    Check(image_a.bgr == original_a && image_b.bgr == original_b,"both source image buffers preserved");
}
} // namespace
int main() {
    static_assert(!std::is_default_constructible_v<RawInversePolicy>);
    static_assert(!std::is_default_constructible_v<RawImageExtent>);
    try { TestInverse(); TestPair(); TestFailures(); TestRender(); }
    catch (const std::exception& e) { ++failures; std::cerr << e.what() << '\n'; }
    std::cout << "raw_document_correction checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
