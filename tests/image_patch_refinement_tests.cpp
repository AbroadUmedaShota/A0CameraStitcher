#include "a0/m2/image_patch_correction.hpp"
#include "a0/m2/image_patch_seed_selection.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <iomanip>
#include <stdexcept>
#include <type_traits>

using namespace a0::m2;
using namespace a0::m2::render;
using namespace a0::m2::correction;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; } }
const OutputRaster raster{{0,0,10000,8000},254,100,80};
CameraProjection Camera() { return {{100,100,64,56},{0,0,0,0,0},{10,0,8,0,10,8,0,0,1}}; }
ImagePatchPolicy Matching() { return {8,.1,4,.1,3,.98,.0001,.02}; }
RawInversePolicy Inverse() { return {40,32,1e-9}; }
DocumentCorrectionPolicy Fit() { return {3,.01,1e-6,.3,20,2,2}; }
std::vector<Point> Seeds() { return {{2,2},{5,2},{8,2},{2,6},{5,6},{8,6}}; }
// Independent continuous scene, sampled directly into both cameras. Neither
// production sampler nor projection/warp is used to generate the oracle.
BgrImage Scene(double dx = 0, double dy = 0, double a = 1, double b = 0) {
    BgrImage image{128,112,std::vector<std::uint8_t>(128*112*3)};
    for (int y = 0; y < 112; ++y) for (int x = 0; x < 128; ++x) {
        const double qx = x-dx-64, qy = y-dy-56, determinant = a*a+b*b;
        const double u = (a*qx+b*qy)/determinant+64, v = (-b*qx+a*qy)/determinant+56;
        const double value = 128+30*std::sin(.31*u+.13*v)+26*std::cos(.19*v-.07*u)+22*std::sin(.21*u-.23*v);
        for (int c = 0; c < 3; ++c) image.bgr[(static_cast<std::size_t>(y)*128+x)*3+c] = static_cast<std::uint8_t>(std::lround(value));
    }
    return image;
}
ImagePatchCorrectionResult Run(const BgrImage& a, const BgrImage& b, const ImagePatchPolicy& p = Matching(),
    const DocumentCorrectionPolicy& fit = Fit(), std::span<const Point> seeds = {}, std::uint32_t levels = 7) {
    const auto defaults = Seeds(); if (seeds.empty()) seeds = defaults;
    return EstimateImagePatchCorrection(a,b,Camera(),Camera(),raster,seeds,p,PatchRefinementPolicy{levels},Inverse(),fit);
}
const CameraProjection* Corrected(const ImagePatchCorrectionResult& r) {
    return r.correction && r.correction->correction && r.correction->correction->corrected_camera ?
        &*r.correction->correction->corrected_camera : nullptr;
}
double CorrespondenceError(const ImagePatchCorrectionResult& r, double dx, double dy) {
    double error = 0;
    for (const auto& p : r.points) {
        if (!p.correspondence) return std::numeric_limits<double>::infinity();
        const auto a = p.correspondence->camera_a_raw_pixel, b = p.correspondence->camera_b_raw_pixel;
        error = std::max(error,std::hypot(b.x-a.x-dx,b.y-a.y-dy));
    }
    return error;
}
double Mae(const BgrImage& a, const BgrImage& b) {
    double sum = 0; for (std::size_t i = 0; i < a.bgr.size(); ++i) sum += std::abs(int(a.bgr[i])-b.bgr[i]);
    return sum/a.bgr.size();
}
void FractionalShift() {
    const auto a = Scene(), b = Scene(2.35,-1.65), original_a = a, original_b = b;
    for (const auto shift : {Point{2.35,-1.65},Point{-2.35,1.65},Point{.35,.65},Point{-.65,-.35},Point{0,0}}) {
        const auto shifted = Scene(shift.x,shift.y);
        const auto coarse = EstimateImagePatchCorrection(a,shifted,Camera(),Camera(),raster,Seeds(),Matching(),Inverse(),Fit());
        const auto result = Run(a,shifted);
        const double error = CorrespondenceError(result,shift.x,shift.y);
        std::cout << "shift=" << shift.x << ',' << shift.y << " maximum error=" << error << "; coarse="
            << CorrespondenceError(coarse,shift.x,shift.y) << " raw pixels\n";
        Check(result.outcome == ImagePatchCorrectionOutcome::Estimated && Corrected(result) && error < .12,
            "fractional displacement is recovered to within 0.12 raw pixels");
        if (shift.x != 0) Check(CorrespondenceError(coarse,shift.x,shift.y) > .4,"coarse-only negative control exposes quantization error");
        if (!Corrected(result)) continue;
        const auto& fitted = *result.correction->correction->estimate;
        Check(std::hypot(fitted.translation_x_mm*10-shift.x,fitted.translation_y_mm*10-shift.y) < .12 &&
            std::abs(fitted.a-1) < .002 && std::abs(fitted.b) < .002,"bounded similarity recovers the independent translation oracle");
        const auto repeated = Run(a,shifted);
        Check(Corrected(repeated) && Corrected(repeated)->document_to_image == Corrected(result)->document_to_image,
            "repeated refined fits are bitwise deterministic");
        Check(Corrected(result)->intrinsics.fx_pixels == Camera().intrinsics.fx_pixels && Corrected(result)->distortion.k1 == 0,
            "fixed intrinsics and distortion are preserved");
        for (const auto& p : result.points) {
            Check(p.forward.refinement.size() == 7 && p.reverse && p.reverse->refinement.size() == 7 &&
                p.forward.examined_candidates == 81 && p.reverse->examined_candidates == 81,"both directions retain coarse diagnostics and all requested levels");
            double step = Matching().search_step_mm, score = *p.forward.best_score;
            for (const auto& level : p.forward.refinement) {
                step /= 2;
                Check(level.outcome == ImagePatchOutcome::Matched && level.examined_candidates == 9 && level.eligible_candidates == 9 &&
                    level.step_mm == step && level.best_score && *level.best_score >= score && level.second_score &&
                    *level.best_score > *level.second_score,"each full half-step grid has a strict unique peak and never reduces correlation");
                score = *level.best_score;
            }
            const auto final = *p.forward.refinement.back().best_document_mm;
            Check(std::hypot(p.correspondence->camera_b_raw_pixel.x-(10*final.x+8),
                p.correspondence->camera_b_raw_pixel.y-(10*final.y+8)) < 1e-10,
                "raw correspondence uses the final refined position rather than the retained coarse point");
        }
    }
    const auto result = Run(a,b);
    if (!Corrected(result)) return;
    for (const auto kernel : {Resampling::bilinear,Resampling::bicubic_catmull_rom})
        for (const auto layout : {StitchLayout::camera_a_left_camera_b_right,StitchLayout::camera_a_top_camera_b_bottom}) {
            const auto reference = RenderDocumentPair(a,a,{raster,Camera(),Camera(),layout,kernel});
            const auto wrong = RenderDocumentPair(a,b,{raster,Camera(),Camera(),layout,kernel});
            const auto good = RenderDocumentPair(a,b,{raster,Camera(),*Corrected(result),layout,kernel});
            std::cout << "render MAE=" << Mae(good.image,reference.image) << "; uncorrected=" << Mae(wrong.image,reference.image) << " DN\n";
            Check(Mae(good.image,reference.image) < .5 && Mae(good.image,reference.image)*10 < Mae(wrong.image,reference.image),
                "one final sampling of originals improves synthetic error by over ten times in both layouts/kernels");
            Check(RenderDocumentPair(a,b,{raster,Camera(),*Corrected(result),layout,kernel}).image.bgr == good.image.bgr,
                "refined final render repeats byte-identically");
        }
    Check(a.bgr == original_a.bgr && b.bgr == original_b.bgr,"both source images remain immutable");
    for (const auto levels : {1u,8u}) {
        const auto r = Run(a,b,Matching(),Fit(),{},levels);
        Check(!r.points.empty() && r.points[0].forward.refinement.size() == levels,"both refinement-level bounds execute explicitly");
    }
}
void SmallSimilarity() {
    for (const auto sign : {1.,-1.}) {
        // Known affine relation in RAW coordinates, independently inverted by
        // Scene(). Translate about (64,56), which is (5.6,4.8) document mm.
        const double angle = sign*.0025, scale = 1+sign*.001;
        const double a = scale*std::cos(angle), b = scale*std::sin(angle), dx = .35, dy = -.45;
        const auto result = Run(Scene(),Scene(dx,dy,a,b));
        double error = 0;
        for (const auto& p : result.points) {
            if (!p.correspondence) { error = std::numeric_limits<double>::infinity(); break; }
            const auto raw = p.correspondence->camera_a_raw_pixel, found = p.correspondence->camera_b_raw_pixel;
            error = std::max(error,std::hypot(found.x-(a*(raw.x-64)-b*(raw.y-56)+64+dx),
                found.y-(b*(raw.x-64)+a*(raw.y-56)+56+dy)));
        }
        std::cout << "similarity sign=" << sign << " maximum correspondence error=" << error << " raw pixels\n";
        Check(Corrected(result) && error < .12,"small rotation/scale with fractional translation reaches the independent raw oracle");
        if (!Corrected(result)) continue;
        const auto& fit = *result.correction->correction->estimate;
        const double tx = (-56*a+48*b+56+dx)/10, ty = (-56*b-48*a+48+dy)/10;
        double maximum = 0;
        for (const auto corner : {Point{0,0},Point{10,0},Point{0,8},Point{10,8}})
            maximum = std::max(maximum,10*std::hypot((fit.a-a)*corner.x-(fit.b-b)*corner.y+fit.translation_x_mm-tx,
                (fit.b-b)*corner.x+(fit.a-a)*corner.y+fit.translation_y_mm-ty));
        Check(maximum < .12,"fitted bounded similarity agrees with the known transform over the full raster");
    }
}
void Rejections() {
    const auto a = Scene(), b = Scene(2.35,-1.65);
    const auto successful = Run(a,b);
    double maximum_reverse = 0;
    for (const auto& p : successful.points) if (p.reverse_error_mm) maximum_reverse = std::max(maximum_reverse,*p.reverse_error_mm);
    Check(maximum_reverse > 0,"fractional fixture has a measurable nonzero reverse error");
    auto matching = Matching(); matching.maximum_reverse_error_mm = maximum_reverse;
    Check(Corrected(Run(a,b,matching)),"reverse-error budget equality is accepted without epsilon");
    matching.maximum_reverse_error_mm = std::nextafter(maximum_reverse,0.);
    const auto reverse = Run(a,b,matching);
    Check(reverse.outcome == ImagePatchCorrectionOutcome::MatchingFailed && !reverse.correction &&
        std::any_of(reverse.points.begin(),reverse.points.end(),[](const auto& p) { return p.outcome == ImagePatchOutcome::ReverseInconsistent && !p.correspondence; }),
        "one representable step below measured reverse error rejects without pruning");
    auto fit = Fit(); fit.maximum_displacement_output_pixels = .1;
    const auto excessive = Run(a,b,Matching(),fit);
    Check(excessive.outcome == ImagePatchCorrectionOutcome::CorrectionRejected && excessive.correction &&
        excessive.correction->correction && excessive.correction->correction->estimate && !Corrected(excessive),
        "refined correspondence still obeys whole-raster correction limits and retains rejected estimates");
    auto seeds = Seeds(); seeds[0] = {.9,2};
    const auto unavailable = Run(a,b,Matching(),Fit(),seeds);
    Check(unavailable.outcome == ImagePatchCorrectionOutcome::MatchingFailed && !unavailable.correction &&
        unavailable.points[0].outcome == ImagePatchOutcome::SearchWindowUnavailable && unavailable.points[0].forward.refinement.empty(),
        "a clipped coarse search is never repaired by local refinement or seed pruning");
    seeds = Seeds(); seeds[1] = seeds[0];
    const auto duplicate = Run(a,b,Matching(),Fit(),seeds);
    Check(duplicate.points[1].outcome == ImagePatchOutcome::InvalidSeed && !duplicate.correction,"duplicate seeds still reject");
    auto flat = a; std::fill(flat.bgr.begin(),flat.bgr.end(),std::uint8_t{100});
    const auto texture = Run(flat,flat);
    Check(!texture.correction && texture.points[0].outcome == ImagePatchOutcome::SourceTextureInsufficient &&
        texture.points[0].forward.refinement.empty(),"texture rejection precedes refinement");
    const auto boundary = Run(a,Scene(4,0));
    Check(!boundary.correction && boundary.points[0].outcome == ImagePatchOutcome::SearchBoundary &&
        boundary.points[0].forward.refinement.empty(),"a coarse boundary maximum cannot become successful after refinement");
    matching = Matching(); matching.minimum_best_second_gap = 2;
    const auto ambiguous = Run(a,b,matching);
    Check(!ambiguous.correction && ambiguous.points[0].outcome == ImagePatchOutcome::Ambiguous &&
        ambiguous.points[0].forward.refinement.empty(),"coarse peak separation remains mandatory");
    for (const auto levels : {0u,9u,std::numeric_limits<std::uint32_t>::max()}) {
        bool threw = false; try { (void)Run(a,b,Matching(),Fit(),{},levels); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw,"invalid refinement levels throw instead of defaulting/clamping");
    }
    // 181*2*17^2*(9^2+9*8) exceeds the public work cap; coarse-only
    // 181*2*17^2*9^2 does not. Refusal must precede any seed evaluation.
    const std::vector<Point> many(181,{2,2});
    const auto work = Run(a,b,Matching(),Fit(),many,8);
    Check(work.outcome == ImagePatchCorrectionOutcome::WorkLimitExceeded && work.points.empty() && !work.correction,
        "all extra candidates count toward the work cap before sampling");
    const auto coarse = EstimateImagePatchCorrection(a,b,Camera(),Camera(),raster,many,Matching(),Inverse(),Fit());
    Check(coarse.outcome != ImagePatchCorrectionOutcome::WorkLimitExceeded && coarse.points.size() == many.size(),
        "the unchanged coarse overload has its original work accounting");
}
void LocalAmbiguity() {
    auto a = Scene(); const int center = 38;
    const std::array<int,9> values{31,150,75,201,64,180,110,221,43};
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
        for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) for (int c = 0; c < 3; ++c)
            a.bgr[(static_cast<std::size_t>(center+6*y+oy)*128+center+6*x+ox)*3+c] = static_cast<std::uint8_t>(values[(y+1)*3+x+1]);
    const std::array<Point,3> seeds{{{3,3},{7,3},{3,6}}};
    const ImagePatchPolicy p{1,.6,2,.2,3,.9,.0001,.02};
    const auto coarse = EstimateImagePatchCorrection(a,a,Camera(),Camera(),raster,seeds,p,Inverse(),Fit());
    const auto refined = Run(a,a,p,Fit(),seeds);
    Check(coarse.points[0].correspondence.has_value(),"local plateau fixture has a valid unique coarse peak");
    Check(refined.points[0].outcome == ImagePatchOutcome::Ambiguous && !refined.points[0].correspondence &&
        refined.points[0].forward.refinement.size() == 1 && refined.points[0].forward.refinement[0].examined_candidates == 9 &&
        !refined.correction,"a tied local maximum is rejected with all local diagnostics retained");
}
void AutomaticSelection() {
    const auto a = Scene(), b = Scene(2.35,-1.65);
    const PatchSeedSelectionPolicy selection{5,4,6,1,.01,.001};
    const auto chosen = SelectImagePatchSeeds(a,b,Camera(),Camera(),raster,selection,Matching(),Fit());
    const auto r = EstimateAutoImagePatchCorrection(a,b,Camera(),Camera(),raster,selection,Matching(),PatchRefinementPolicy{7},Inverse(),Fit());
    Check(r.selection.selected_indices == chosen.selected_indices && r.selection.candidates.size() == chosen.candidates.size(),
        "automatic selection is frozen before refinement and is identical to the independent selection call");
    Check(r.outcome == AutoImagePatchOutcome::Estimated && r.correction && Corrected(*r.correction) &&
        CorrespondenceError(*r.correction,2.35,-1.65) < .12,"automatic seeds connect to refined bounded correction");
    auto matching = Matching(); matching.maximum_reverse_error_mm = 0;
    const auto rejected = EstimateAutoImagePatchCorrection(a,b,Camera(),Camera(),raster,selection,matching,PatchRefinementPolicy{7},Inverse(),Fit());
    Check(rejected.outcome == AutoImagePatchOutcome::MatchingRejected && rejected.selection.selected_indices == r.selection.selected_indices,
        "a selected match failure never triggers reselection");
    auto flat = a; std::fill(flat.bgr.begin(),flat.bgr.end(),std::uint8_t{100});
    bool threw = false;
    try { (void)EstimateAutoImagePatchCorrection(flat,flat,Camera(),Camera(),raster,selection,Matching(),PatchRefinementPolicy{0},Inverse(),Fit()); }
    catch (const std::invalid_argument&) { threw = true; }
    Check(threw,"auto overload validates refinement policy before any selection early return");
}
}
int main() {
    static_assert(!std::is_default_constructible_v<PatchRefinementPolicy>);
    std::cout << std::setprecision(17);
    FractionalShift(); SmallSimilarity(); Rejections(); LocalAmbiguity(); AutomaticSelection();
    std::cout << "image_patch_refinement checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
