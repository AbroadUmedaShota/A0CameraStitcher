#include "a0/m2/image_patch_correction.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <type_traits>

using namespace a0::m2;
using namespace a0::m2::render;
using namespace a0::m2::correction;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; } }
template<class F> void Reject(F f, const char* why) {
    bool threw = false; try { f(); } catch (const std::invalid_argument&) { threw = true; } Check(threw,why);
}
const OutputRaster raster{{0,0,10000,8000},254,100,80};
CameraProjection Camera() { return {{100,100,64,56},{0,0,0,0,0},{10,0,8,0,10,8,0,0,1}}; }
ImagePatchPolicy Matching() { return {3,.1,4,.1,5,.9,.04,1e-8}; }
RawInversePolicy Inverse() { return {40,32,1e-9}; }
DocumentCorrectionPolicy Fit() { return {3,.01,1e-6,1e-6,20,2,2}; }
std::vector<Point> Seeds() { return {{2,2},{5,2},{8,2},{2,6},{5,6},{8,6}}; }
std::uint32_t Hash(int x, int y) {
    auto h = static_cast<std::uint32_t>(x)*0x45d9f3bu+static_cast<std::uint32_t>(y)*0x119de1f3u;
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16; return h;
}
BgrImage Texture(int dx = 0, int dy = 0, int salt = 0, bool exposure = false) {
    BgrImage image{128,112,std::vector<std::uint8_t>(128*112*3)};
    for (int y = 0; y < 112; ++y) for (int x = 0; x < 128; ++x) {
        // Multiples of four keep half-pixel bilinear oracle values away from
        // half-integer quantization boundaries, without snapping the estimate.
        const int base = 32+4*(Hash(x-dx+salt,y-dy)%32);
        for (std::size_t c = 0; c < 3; ++c) image.bgr[(static_cast<std::size_t>(y)*128+x)*3+c] =
            static_cast<std::uint8_t>(exposure ? base+base/4+8 : base);
    }
    return image;
}
ImagePatchCorrectionResult Run(const BgrImage& a, const BgrImage& b, const ImagePatchPolicy& p = Matching(),
    const DocumentCorrectionPolicy& fit = Fit(), std::span<const Point> seeds = {}) {
    const auto default_seeds = Seeds(); if (seeds.empty()) seeds = default_seeds;
    return EstimateImagePatchCorrection(a,b,Camera(),Camera(),raster,seeds,p,Inverse(),fit);
}
const CameraProjection* Corrected(const ImagePatchCorrectionResult& r) {
    if (!r.correction || !r.correction->correction || !r.correction->correction->corrected_camera) return nullptr;
    return &*r.correction->correction->corrected_camera;
}
void Inspect(const ImagePatchCorrectionResult& r) {
    for (const auto& p : r.points) {
        std::cerr << "seed=" << p.seed_document_mm.x << ',' << p.seed_document_mm.y << " reason=" << ImagePatchOutcomeCode(p.outcome);
        if (p.forward.best_score) std::cerr << " ncc=" << *p.forward.best_score;
        if (p.forward.second_score) std::cerr << " second=" << *p.forward.second_score;
        if (p.reverse) std::cerr << " reverse=" << ImagePatchOutcomeCode(p.reverse->outcome);
        std::cerr << '\n';
    }
}
void CheckTranslation(const ImagePatchCorrectionResult& r, double tx, double ty, const char* why) {
    Check(r.outcome == ImagePatchCorrectionOutcome::Estimated && Corrected(r),why);
    if (!Corrected(r)) { Inspect(r); return; }
    const auto& fit = *r.correction->correction->estimate;
    Check(std::abs(fit.a-1) < 1e-9 && std::abs(fit.b) < 1e-9 && std::abs(fit.translation_x_mm-tx) < 1e-8 &&
        std::abs(fit.translation_y_mm-ty) < 1e-8,"image content independently recovers signed document-mm translation");
    for (const auto& p : r.points) Check(p.correspondence && p.reverse && p.reverse_error_mm &&
        *p.reverse_error_mm <= Matching().maximum_reverse_error_mm && p.forward.examined_candidates == 81 && p.reverse->examined_candidates == 81,
        "both directions evaluate the full explicit grid and expose verified raw coordinates");
}
void TestMatchingAndRender() {
    const auto a = Texture(), b = Texture(3,-2), original_a = a, original_b = b;
    const auto r = Run(a,b); CheckTranslation(r,.3,-.2,"unknown image correspondences are found and connected to bounded correction");
    CheckTranslation(Run(a,Texture(-2,1)),-.2,.1,"negative X and positive Y match directions are preserved");
    CheckTranslation(Run(a,Texture(3,-2,0,true)),.3,-.2,"mean removal and normalization tolerate a positive exposure gain and offset");
    if (!Corrected(r)) return;
    const auto again = Run(a,b);
    Check(Corrected(again) && Corrected(again)->document_to_image == Corrected(r)->document_to_image,"repeat image matching and correction are deterministic");
    const auto identity = Run(a,a);
    if (identity.points[0].forward.source_intensity_stddev) {
        auto policy = Matching(); policy.minimum_intensity_stddev = *identity.points[0].forward.source_intensity_stddev;
        const auto equal = Run(a,a,policy);
        Check(equal.points[0].correspondence.has_value(),"measured source contrast equal to the budget is accepted");
        policy.minimum_intensity_stddev = std::nextafter(policy.minimum_intensity_stddev,std::numeric_limits<double>::infinity());
        const auto beyond = Run(a,a,policy);
        Check(beyond.points[0].outcome == ImagePatchOutcome::SourceTextureInsufficient && !beyond.correction,
            "one representable step above measured contrast rejects without adding an epsilon");
    } else Check(false,"source contrast is retained for policy diagnostics");
    for (const auto kernel : {Resampling::bilinear,Resampling::bicubic_catmull_rom})
        for (const auto layout : {StitchLayout::camera_a_left_camera_b_right,StitchLayout::camera_a_top_camera_b_bottom}) {
            const auto reference = RenderDocumentPair(a,a,{raster,Camera(),Camera(),layout,kernel});
            const auto corrected = RenderDocumentPair(a,b,{raster,Camera(),*Corrected(r),layout,kernel});
            const auto wrong = RenderDocumentPair(a,b,{raster,Camera(),Camera(),layout,kernel});
            std::uint64_t corrected_error = 0, wrong_error = 0; int maximum = 0;
            for (std::size_t i = 0; i < corrected.image.bgr.size(); ++i) {
                const int error = std::abs(static_cast<int>(corrected.image.bgr[i])-reference.image.bgr[i]);
                maximum = std::max(maximum,error); corrected_error += error;
                wrong_error += std::abs(static_cast<int>(wrong.image.bgr[i])-reference.image.bgr[i]);
            }
            // An unsnapped floating fit can cross an exact half-integer cubic
            // quantization boundary. Compare to the integer rounding bound;
            // determinism below still requires exact repeated output bytes.
            Check(maximum <= 1 && corrected_error*100 < wrong_error,
                "image-derived correction restores the oracle within one quantization DN and improves over 100x versus the negative control");
            Check(RenderDocumentPair(a,b,{raster,Camera(),*Corrected(r),layout,kernel}).image.bgr == corrected.image.bgr,
                "the final single-sampling image repeats byte-identically");
        }
    Check(a.bgr == original_a.bgr && b.bgr == original_b.bgr && Camera().document_to_image == Matrix3{10,0,8,0,10,8,0,0,1},
        "both source buffers and fixed camera are unchanged");
}
void TestNegatives() {
    using O = ImagePatchOutcome;
    const auto a = Texture(); auto flat = a; std::fill(flat.bgr.begin(),flat.bgr.end(),std::uint8_t{100});
    const auto check_failed = [&](const ImagePatchCorrectionResult& r, O why, const char* message) {
        Check(r.outcome == ImagePatchCorrectionOutcome::MatchingFailed && !r.correction && r.points.size() == Seeds().size() &&
            std::all_of(r.points.begin(),r.points.end(),[&](const auto& p) { return p.outcome == why && !p.correspondence; }),message);
    };
    check_failed(Run(flat,a),O::SourceTextureInsufficient,"blank source cannot certify image registration");
    check_failed(Run(a,flat),O::TargetTextureInsufficient,"blank target supplies no eligible match");
    const auto no_texture = Run(a,flat);
    Check(no_texture.points[0].forward.maximum_target_intensity_stddev && *no_texture.points[0].forward.maximum_target_intensity_stddev == 0 &&
        no_texture.points[0].forward.examined_candidates == 81 && no_texture.points[0].forward.eligible_candidates == 0,
        "contrast failure retains the observed value and complete candidate counts");
    auto stripes = a;
    for (int y = 0; y < 112; ++y) for (int x = 0; x < 128; ++x) for (int c = 0; c < 3; ++c)
        stripes.bgr[(static_cast<std::size_t>(y)*128+x)*3+c] = static_cast<std::uint8_t>(50+100*((x+y)%2));
    check_failed(Run(stripes,stripes),O::Ambiguous,"periodic pattern with multiple equal peaks is refused rather than choosing the first");
    check_failed(Run(a,Texture(4,0)),O::SearchBoundary,"a best match at the search boundary is not clamped into success");
    auto strict = Matching(); strict.minimum_correlation = 1;
    check_failed(Run(a,Texture(0,0,731),strict),O::CorrelationInsufficient,"unrelated images with low scores cannot reach correction");
    auto p = Matching(); p.minimum_best_second_gap = 2;
    check_failed(Run(a,a,p),O::Ambiguous,"a policy's peak-separation budget is enforced");
    auto seeds = Seeds(); seeds[0] = {.5,2};
    const auto clipped = Run(a,a,Matching(),Fit(),seeds);
    Check(!clipped.correction && clipped.points[0].outcome == O::SearchWindowUnavailable && clipped.points.back().correspondence,
        "one clipped search rejects the entire correction, but later seed diagnostics continue");
    seeds[0] = {.1,2};
    const auto edge = Run(a,a,Matching(),Fit(),seeds);
    Check(!edge.correction && edge.points[0].outcome == O::SourcePatchUnavailable,"out-of-domain patch samples are not filled or clipped");
    seeds = Seeds(); seeds[1] = seeds[0]; seeds[2].x = std::numeric_limits<double>::quiet_NaN(); seeds[3].y = -1;
    const auto invalid = Run(a,a,Matching(),Fit(),seeds);
    Check(!invalid.correction && invalid.points[1].outcome == O::InvalidSeed && invalid.points[2].outcome == O::InvalidSeed &&
        invalid.points[3].outcome == O::InvalidSeed && invalid.points.back().correspondence,"duplicate, NaN and outside seeds fail individually without pruning");
    auto fit = Fit(); fit.maximum_displacement_output_pixels = 1;
    const auto over = Run(a,Texture(3,-2),Matching(),fit);
    Check(over.outcome == ImagePatchCorrectionOutcome::CorrectionRejected && over.correction && over.correction->correction &&
        over.correction->correction->outcome == DocumentCorrectionOutcome::DisplacementExceeded && !Corrected(over),
        "good image matches cannot bypass the separate correction displacement bound");
    std::vector<Point> maximum(kMaximumPatchSeeds+1,{2,2});
    Check(Run(a,a,Matching(),Fit(),maximum).outcome == ImagePatchCorrectionOutcome::InvalidSeeds,"seed count hard cap is enforced before matching");
    maximum.resize(7); p = {8,.1,32,.1,5,.9,.04,1e-8};
    const auto costly = Run(a,a,p,Fit(),maximum);
    Check(costly.outcome == ImagePatchCorrectionOutcome::WorkLimitExceeded && costly.points.empty() && !costly.correction,
        "requested candidate-sample work is bounded before sampling");
    auto short_seeds = Seeds(); short_seeds.resize(2);
    const auto insufficient = Run(a,a,Matching(),Fit(),short_seeds);
    Check(insufficient.outcome == ImagePatchCorrectionOutcome::CorrectionRejected && !Corrected(insufficient),"too few seeds preserve the geometric count gate");
    for (int i = 0; i < 17; ++i) {
        auto bad = Matching();
        if (i == 0) bad.patch_radius_samples = 0;
        if (i == 1) bad.patch_radius_samples = 9;
        if (i == 2) bad.search_radius_steps = 0;
        if (i == 3) bad.search_radius_steps = 33;
        if (i == 4) bad.patch_spacing_mm = 0;
        if (i == 5) bad.patch_spacing_mm = std::numeric_limits<double>::infinity();
        if (i == 6) bad.search_step_mm = -1;
        if (i == 7) bad.search_step_mm = std::numeric_limits<double>::quiet_NaN();
        if (i == 8) bad.minimum_intensity_stddev = 0;
        if (i == 9) bad.minimum_intensity_stddev = std::numeric_limits<double>::infinity();
        if (i == 10) bad.minimum_correlation = -1.01;
        if (i == 11) bad.minimum_correlation = 1.01;
        if (i == 12) bad.minimum_best_second_gap = 0;
        if (i == 13) bad.minimum_best_second_gap = 2.01;
        if (i == 14) bad.maximum_reverse_error_mm = -1;
        if (i == 15) bad.maximum_reverse_error_mm = std::numeric_limits<double>::quiet_NaN();
        if (i == 16) bad.minimum_correlation = std::numeric_limits<double>::quiet_NaN();
        Reject([&] { (void)Run(a,a,bad); },"invalid matching policy is refused");
    }
    auto broken = a; broken.bgr.pop_back(); Reject([&] { (void)Run(broken,a); },"truncated BGR buffer rejected before reads");
    auto bad_inverse = Inverse(); bad_inverse.maximum_newton_updates = 0;
    Reject([&] { (void)EstimateImagePatchCorrection(flat,flat,Camera(),Camera(),raster,Seeds(),Matching(),bad_inverse,Fit()); },
        "invalid inverse policy cannot hide behind blank images");
}
void TestReverse() {
    auto a = Texture(), b = Texture(0,0,355);
    std::fill(a.bgr.begin(),a.bgr.end(),std::uint8_t{100});
    std::fill(b.bgr.begin(),b.bgr.end(),std::uint8_t{100});
    const std::array<int,9> pattern{31,150,75,201,64,180,110,221,43};
    const std::array<int,9> noisy{31,150,75,201,124,180,110,221,43};
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) for (int c = 0; c < 3; ++c) {
        const auto i = static_cast<std::size_t>((y+1)*3+x+1);
        a.bgr[(static_cast<std::size_t>(38+2*y)*128+38+2*x)*3+c] = static_cast<std::uint8_t>(pattern[i]);
        a.bgr[(static_cast<std::size_t>(38+2*y)*128+44+2*x)*3+c] = static_cast<std::uint8_t>(noisy[i]);
        b.bgr[(static_cast<std::size_t>(38+2*y)*128+41+2*x)*3+c] = static_cast<std::uint8_t>(noisy[i]);
    }
    const std::array<Point,3> seeds{{{3,3},{7,3},{3,6}}};
    const auto r = Run(a,b,{1,.2,4,.1,5,.8,.01,1e-8},Fit(),seeds);
    Check(r.points[0].outcome == ImagePatchOutcome::ReverseInconsistent && r.points[0].reverse_error_mm &&
        *r.points[0].reverse_error_mm > .5 && !r.points[0].correspondence && !r.correction,
        "forward match with a stronger competing reverse identity is refused");
    if (r.points[0].outcome != ImagePatchOutcome::ReverseInconsistent) Inspect(r);
}
// Independent image generator: scalar bisection for radial k1, explicit
// affine inverse, and analytic texture. No production projection or inverse.
BgrImage WarpedTexture(const CameraProjection& c, Point shift) {
    BgrImage image{128,112,std::vector<std::uint8_t>(128*112*3)};
    for (int y = 0; y < 112; ++y) for (int x = 0; x < 128; ++x) {
        const auto& k = c.intrinsics; const auto& h = c.document_to_image;
        const double xd = (x-k.cx_pixels)/k.fx_pixels, yd = (y-k.cy_pixels)/k.fy_pixels;
        const double distorted_radius = std::hypot(xd,yd);
        double low = 0, high = distorted_radius;
        for (int i = 0; i < 80; ++i) {
            const double mid = (low+high)/2;
            if (mid+c.distortion.k1*mid*mid*mid > distorted_radius) high = mid; else low = mid;
        }
        const double scale = distorted_radius == 0 ? 1 : (low+high)/2/distorted_radius;
        const double u = k.cx_pixels+k.fx_pixels*xd*scale-h[2], v = k.cy_pixels+k.fy_pixels*yd*scale-h[5];
        const double determinant = h[0]*h[4]-h[1]*h[3];
        const double px = (u*h[4]-v*h[1])/determinant-shift.x, py = (v*h[0]-u*h[3])/determinant-shift.y;
        const double value = 128+36*std::sin(19*px+2*py)+30*std::cos(13*py-3*px)+22*std::sin(11*px+17*py);
        for (int ch = 0; ch < 3; ++ch) image.bgr[(static_cast<std::size_t>(y)*128+x)*3+ch] = static_cast<std::uint8_t>(std::lround(value));
    }
    return image;
}
void TestFixedProjectionGuidance() {
    auto ca = Camera(), cb = Camera(); ca.distortion.k1 = .04;
    cb.intrinsics = {110,105,60,53}; cb.distortion.k1 = .08;
    cb.document_to_image = {10.5,.8,6,-.5,10.2,9,0,0,1};
    const auto a = WarpedTexture(ca,{0,0}), b = WarpedTexture(cb,{.2,-.1});
    const auto r = EstimateImagePatchCorrection(a,b,ca,cb,raster,Seeds(),Matching(),Inverse(),Fit());
    CheckTranslation(r,.2,-.1,"independent lens-distorted, differently scaled/sheared images match in nominal document coordinates");
    if (!Corrected(r)) return;
    Check(Corrected(r)->intrinsics.fx_pixels == cb.intrinsics.fx_pixels && Corrected(r)->distortion.k1 == cb.distortion.k1 &&
        cb.document_to_image == Matrix3{10.5,.8,6,-.5,10.2,9,0,0,1},"fixed projection guidance preserves original K/lens/H");
    const auto mismodeled = EstimateImagePatchCorrection(a,b,Camera(),Camera(),raster,Seeds(),Matching(),Inverse(),Fit());
    Check(!Corrected(mismodeled),"negative control refuses the same warped images with the wrong fixed projection");
}
void Hr(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("WIC synthetic JPEG operation failed"); }
std::vector<std::uint8_t> Jpeg(const BgrImage& image) {
    ComPtr<IWICImagingFactory> factory; Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream; Hr(CreateStreamOnHGlobal(nullptr,TRUE,&stream));
    ComPtr<IWICBitmapEncoder> encoder; Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));
    Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> options; Hr(encoder->CreateNewFrame(&frame,&options));
    PROPBAG2 property{}; property.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value{}; value.vt = VT_R4; value.fltVal = .98f; Hr(options->Write(1,&property,&value));
    Hr(frame->Initialize(options.Get())); Hr(frame->SetSize(image.width,image.height));
    auto format = GUID_WICPixelFormat24bppBGR; Hr(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("unexpected JPEG pixel format");
    Hr(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(image.bgr.size()),const_cast<BYTE*>(image.bgr.data())));
    Hr(frame->Commit()); Hr(encoder->Commit());
    STATSTG stat{}; Hr(stream->Stat(&stat,STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > 1024*1024) throw std::runtime_error("unexpected synthetic JPEG size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{}; Hr(stream->Seek(zero,STREAM_SEEK_SET,nullptr)); ULONG actual{};
    Hr(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&actual));
    if (actual != bytes.size()) throw std::runtime_error("short JPEG stream"); return bytes;
}
BgrImage Decode(const std::vector<std::uint8_t>& bytes) {
    ComPtr<IWICImagingFactory> factory; Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream; Hr(CreateStreamOnHGlobal(nullptr,TRUE,&stream)); ULONG actual{};
    Hr(stream->Write(bytes.data(),static_cast<ULONG>(bytes.size()),&actual));
    LARGE_INTEGER zero{}; Hr(stream->Seek(zero,STREAM_SEEK_SET,nullptr));
    ComPtr<IWICBitmapDecoder> decoder; Hr(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder));
    ComPtr<IWICBitmapFrameDecode> frame; Hr(decoder->GetFrame(0,&frame));
    BgrImage image; Hr(frame->GetSize(&image.width,&image.height)); image.bgr.resize(PixelCount(image.width,image.height)*3);
    ComPtr<IWICFormatConverter> converter; Hr(factory->CreateFormatConverter(&converter));
    Hr(converter->Initialize(frame.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    Hr(converter->CopyPixels(nullptr,image.width*3,static_cast<UINT>(image.bgr.size()),image.bgr.data())); return image;
}
void Write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("synthetic evidence write failed");
}
std::vector<std::uint8_t> ReadEvidence(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > 1024*1024) throw std::runtime_error("unexpected synthetic evidence size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size)); std::ifstream in(path,std::ios::binary);
    in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if (!in) throw std::runtime_error("synthetic evidence read failed"); return bytes;
}
void TestJpeg(const std::filesystem::path& evidence) {
    auto bytes_a = Jpeg(Texture()), bytes_b = Jpeg(Texture(3,-2));
    if (!evidence.empty()) {
        Write(evidence/"camera-a.jpg",bytes_a); Write(evidence/"camera-b.jpg",bytes_b);
        // In explicit evidence mode the matcher consumes JPEGs reread from disk.
        bytes_a = ReadEvidence(evidence/"camera-a.jpg"); bytes_b = ReadEvidence(evidence/"camera-b.jpg");
    }
    const auto a = Decode(bytes_a), b = Decode(bytes_b), original_a = a, original_b = b;
    const auto r = Run(a,b); CheckTranslation(r,.3,-.2,"actual compressed JPEG pixels yield image-derived correspondences");
    if (!Corrected(r)) return;
    const auto good = RenderDocumentPair(a,b,{raster,Camera(),*Corrected(r),StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear});
    const auto wrong = RenderDocumentPair(a,b,{raster,Camera(),Camera(),StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear});
    const auto reference = RenderDocumentPair(a,a,{raster,Camera(),Camera(),StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear});
    double good_error = 0, wrong_error = 0;
    for (std::size_t i = 0; i < good.image.bgr.size(); ++i) {
        good_error += std::abs(static_cast<int>(good.image.bgr[i])-reference.image.bgr[i]);
        wrong_error += std::abs(static_cast<int>(wrong.image.bgr[i])-reference.image.bgr[i]);
    }
    Check(good_error < wrong_error/10,"JPEG-derived correction reduces seam/image error by more than ten times versus uncorrected control");
    Check(a.bgr == original_a.bgr && b.bgr == original_b.bgr && Jpeg(Texture()) == bytes_a,"compressed inputs and decoded originals remain immutable");
    std::cout << "JPEG corrected MAE=" << good_error/good.image.bgr.size() << "; uncorrected MAE=" << wrong_error/good.image.bgr.size() << " DN\n";
    if (!evidence.empty() && failures == 0) {
        Write(evidence/"corrected.jpg",Jpeg(good.image)); Write(evidence/"uncorrected.jpg",Jpeg(wrong.image));
        std::ofstream report(evidence/"matching-report.json"); report.imbue(std::locale::classic()); report << std::setprecision(17);
        report << "{\n  \"schema\": \"a0.synthetic-image-patch-test.v1\",\n  \"synthetic\": true,\n  \"productApproved\": false,\n"
            << "  \"qualityAssessment\": \"not-assessed\",\n  \"width\": 128, \"height\": 112,\n"
            << "  \"policy\": {\"patchRadiusSamples\":3, \"patchSpacingMm\":0.1, \"searchRadiusSteps\":4, \"searchStepMm\":0.1,"
            << "\"minimumIntensityStddev\":5, \"minimumCorrelation\":0.9, \"minimumBestSecondGap\":0.04, \"maximumReverseErrorMm\":1e-8},\n"
            << "  \"expectedTranslationMm\": [0.3,-0.2],\n  \"estimatedSimilarity\": [";
        const auto fit = *r.correction->correction->estimate;
        report << fit.a << ',' << fit.b << ',' << fit.translation_x_mm << ',' << fit.translation_y_mm << "],\n"
            << "  \"correctedMeanAbsoluteErrorDn\": " << good_error/good.image.bgr.size() << ",\n"
            << "  \"uncorrectedMeanAbsoluteErrorDn\": " << wrong_error/good.image.bgr.size() << ",\n  \"points\": [";
        for (std::size_t i = 0; i < r.points.size(); ++i) {
            const auto& p = r.points[i]; if (i) report << ',';
            report << "\n    {\"seedMm\":[" << p.seed_document_mm.x << ',' << p.seed_document_mm.y << "],\"outcome\":\""
                << ImagePatchOutcomeCode(p.outcome) << "\",\"forwardScore\":" << *p.forward.best_score << ",\"secondScore\":" << *p.forward.second_score
                << ",\"reverseScore\":" << *p.reverse->best_score << ",\"reverseErrorMm\":" << *p.reverse_error_mm
                << ",\"rawA\":[" << p.correspondence->camera_a_raw_pixel.x << ',' << p.correspondence->camera_a_raw_pixel.y
                << "],\"rawB\":[" << p.correspondence->camera_b_raw_pixel.x << ',' << p.correspondence->camera_b_raw_pixel.y << "]}";
        }
        report << "\n  ]\n}\n"; report.flush(); if (!report) throw std::runtime_error("synthetic report write failed");
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    static_assert(!std::is_default_constructible_v<ImagePatchPolicy>);
    bool initialized = false;
    try {
        std::filesystem::path evidence;
        if (argc == 2) {
            evidence = std::filesystem::path(argv[1]);
            if (!std::filesystem::create_directory(evidence)) throw std::runtime_error("evidence requires a new directory");
        } else if (argc != 1) throw std::invalid_argument("optional argument: new synthetic evidence directory");
        Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED)); initialized = true;
        TestMatchingAndRender(); TestNegatives(); TestReverse(); TestFixedProjectionGuidance(); TestJpeg(evidence);
    } catch (const std::exception& e) { ++failures; std::cerr << e.what() << '\n'; }
    if (initialized) CoUninitialize();
    std::cout << "image_patch_correction checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
