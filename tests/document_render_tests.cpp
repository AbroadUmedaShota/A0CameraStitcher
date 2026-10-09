#include "a0/m2/document_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace a0::m2::render;
using a0::m2::StitchLayout;
constexpr auto horizontal = StitchLayout::camera_a_left_camera_b_right;
constexpr auto vertical = StitchLayout::camera_a_top_camera_b_bottom;
constexpr auto linear = Resampling::bilinear;
constexpr auto cubic = Resampling::bicubic_catmull_rom;
static_assert(!std::is_default_constructible_v<CameraIntrinsics>);
static_assert(!std::is_default_constructible_v<LensDistortion>);
static_assert(!std::is_default_constructible_v<CameraProjection>);
static_assert(!std::is_default_constructible_v<DocumentRegionUm>);
static_assert(!std::is_default_constructible_v<OutputRaster>);
static_assert(!std::is_default_constructible_v<DocumentRenderParameters>);
static_assert(!std::is_default_constructible_v<MappedRenderParameters>);
int failures = 0, checks = 0;
void Check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class F> void Reject(F action, const std::string& message) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, message);
}
CameraProjection Projection(Matrix3 h = {1,0,0,0,1,0,0,0,1}) {
    return {CameraIntrinsics{1,1,0,0}, LensDistortion{0,0,0,0,0}, h};
}
BgrImage Image(std::uint32_t width, std::uint32_t height, int constant = -1) {
    BgrImage result{width, height, std::vector<std::uint8_t>(width * height * 3)};
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                result.bgr[(y * width + x) * 3 + c] = static_cast<std::uint8_t>(constant < 0 ? 20 + 8*x + 4*y + c : constant);
    return result;
}
OutputToCamera Identity() { return [](Point p, Point& raw) { raw = p; return true; }; }
OutputToCamera Missing() { return [](Point, Point&) { return false; }; }

// Algebraically independent oracle: dot products and explicit polynomial terms.
// On MSVC long double has binary64 precision; independence, not extra precision,
// is the reason to retain this implementation outside the production renderer.
std::array<long double,2> Oracle(const CameraProjection& camera, Point point) {
    const long double column[]{point.x, point.y, 1};
    long double projected[3]{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) projected[row] += camera.document_to_image[row*3+col] * column[col];
    const auto& k = camera.intrinsics; const auto& d = camera.distortion;
    const long double x = (projected[0]/projected[2]-k.cx_pixels)/k.fx_pixels;
    const long double y = (projected[1]/projected[2]-k.cy_pixels)/k.fy_pixels;
    const long double rr = x*x+y*y;
    const long double radial_x = x + d.k1*x*rr + d.k2*x*rr*rr + d.k3*x*rr*rr*rr;
    const long double radial_y = y + d.k1*y*rr + d.k2*y*rr*rr + d.k3*y*rr*rr*rr;
    return {k.cx_pixels+k.fx_pixels*(radial_x+2*d.p1*x*y+d.p2*(3*x*x+y*y)),
            k.cy_pixels+k.fy_pixels*(radial_y+d.p1*(x*x+3*y*y)+2*d.p2*x*y)};
}
void TestProjectionOracle() {
    const std::array cameras{
        CameraProjection{{10000,11000,3679.5,2455.5},{-.04,.008,-.0007,.0009,-.0011},
            {0,7.27,100,-7.27,0,4800,.00005,-.00003,1}},
        CameraProjection{{9800,10400,3650,2400},{-.03,.006,-.0004,-.0012,.0008},
            {0,7.27,125,-7.27,0,8733.07,-.00004,.00006,1}}
    };
    std::vector<double> errors;
    for (const auto& camera : cameras) for (int y = 0; y <= 8; ++y) for (int x = 0; x <= 12; ++x) {
        const Point document{double(x)*90, double(y)*90};
        const auto expected = Oracle(camera, document); Point actual{};
        Check(TryProjectDocument(camera, document, actual), "finite forward projection");
        const double error = std::hypot(actual.x-static_cast<double>(expected[0]), actual.y-static_cast<double>(expected[1]));
        errors.push_back(error); Check(error < 1e-8, "five-coefficient/projective projection agrees with independent oracle");
    }
    std::sort(errors.begin(), errors.end());
    const double rank = .95*(errors.size()-1);
    const auto index = static_cast<std::size_t>(std::floor(rank));
    const double p95 = errors[index]+(rank-index)*(errors[index+1]-errors[index]);
    std::cout << std::setprecision(17) << "projection_oracle_camera_pixels count=" << errors.size()
        << " p95_linear_r7=" << p95 << " max=" << errors.back() << '\n';
    Point ignored{};
    Check(!TryProjectDocument(cameras[0], {std::numeric_limits<double>::infinity(),0}, ignored), "nonfinite document point has no projection");
    Check(!TryProjectDocument(Projection({1,0,0,0,1,0,-1,0,1}), {1,0}, ignored), "zero projective denominator has no projection");
}
void TestSamplingAndDocumentRaster() {
    const auto ramp = Image(8,8); std::array<double,3> sample{};
    Check(SampleBicubic(ramp,3.25,2.75,sample), "interior bicubic sample");
    for (int c=0;c<3;++c) Check(std::abs(sample[c]-(57.0+c))<1e-10, "Catmull-Rom reproduces affine ramps");
    Check(SampleBicubic(ramp,3,2,sample) && sample[0]==52, "bicubic cardinal integer sample");
    const auto solid = Image(8,8,77);
    for (const auto p : {Point{0,0},Point{.25,.75},Point{7.9,7.9}}) {
        Check(SampleBicubic(solid,p.x,p.y,sample), "clamped support remains covered");
        for (double value : sample) Check(std::abs(value-77)<1e-10, "bicubic preserves constant at image edges");
    }
    Check(!SampleBicubic(solid,-.001,0,sample) && !SampleBicubic(solid,8,0,sample), "bicubic coverage is half-open");
    auto step=Image(8,8,0);
    for (std::uint32_t y=0;y<8;++y) for (std::uint32_t x=0;x<4;++x) for (int c=0;c<3;++c) step.bgr[(y*8+x)*3+c]=255;
    Check(SampleBicubic(step,2.5,3,sample) && sample[0]==270.9375, "Catmull-Rom half-sample weights keep the known 255*17/16 overshoot");
    const auto clipped=RenderMappedPair(step,step,{1,1,[](Point,Point& p){p={2.5,3};return true;},Missing(),horizontal,cubic});
    Check(clipped.image.bgr[0]==255, "bicubic overshoot is clamped only at final output quantization");
    const OutputRaster raster{{0,0,400,400},254,4,4}; // p=0.1 mm, centers map to 2.5..5.5.
    const auto a = Projection({10,0,2,0,10,2,0,0,1}), b = Projection({10,0,100,0,10,100,0,0,1});
    const auto original = ramp.bgr;
    for (auto kernel : {linear,cubic}) {
        const DocumentRenderParameters parameters{raster,a,b,horizontal,kernel};
        const auto output = RenderDocumentPair(ramp,ramp,parameters);
        Check(output.image.width==4 && output.image.height==4, "document raster keeps declared dimensions");
        for (std::uint32_t y=0;y<4;++y) for (std::uint32_t x=0;x<4;++x) for (std::uint32_t c=0;c<3;++c)
            Check(output.image.bgr[(y*4+x)*3+c]==50+8*x+4*y+c, "actual document render samples the expected affine BGR value");
        Check(!output.seam_navigation.available, "single-camera coverage has no overlap navigation");
        Check(output.image.bgr==RenderDocumentPair(ramp,ramp,parameters).image.bgr, "direct document repeat byte identity for both kernels");
        const CameraProjection distorted{{8,9,4,4},{.6,.02,.008,.03,-.04},{10,.2,2,.1,10,2,.002,-.003,1}};
        const auto projected=RenderDocumentPair(ramp,ramp,{raster,distorted,b,horizontal,kernel});
        for (std::uint32_t y=0;y<4;++y) for (std::uint32_t x=0;x<4;++x) {
            const Point mm{(double(x)+.5)/10,(double(y)+.5)/10};
            const auto raw=Oracle(distorted,mm);
            for (int c=0;c<3;++c) {
                const int expected=static_cast<int>(std::floor(20+8*raw[0]+4*raw[1]+c+.5L));
                Check(projected.image.bgr[(y*4+x)*3+c]==expected, "actual document sampling applies full lens/projective mapping before affine BGR interpolation");
            }
        }
    }
    Check(ramp.bgr==original, "document render preserves both source buffers");
}
void TestLegacyCompatibility() {
    const auto a = Image(16,8), b = Image(16,8,137);
    for (auto layout : {horizontal,vertical}) for (bool translated : {false,true}) {
        const double dx = translated && layout==horizontal ? 12 : 0, dy = translated && layout==vertical ? 5 : 0;
        const std::uint32_t crop = translated ? 1 : 0;
        const PairRenderParameters old{{1,0,dx,0,1,dy,0,0,1},layout,{crop,crop,crop,crop}};
        const auto expected = RenderPair(a,b,old);
        const auto pa=Projection({1,0,double(crop),0,1,double(crop),0,0,1});
        const auto pb=Projection({1,0,crop-dx,0,1,crop-dy,0,0,1});
        // Exercise the zero-lens projection itself on a pixel-space lattice,
        // including the exact unit map, without the mm/DPI rounding stage.
        const MappedRenderParameters mapped{translated ? (layout==horizontal ? 26u : 14u) : 16u,
            translated ? (layout==horizontal ? 6u : 11u) : 8u,
            [pa](Point p,Point& raw){ return TryProjectDocument(pa,p,raw); },
            [pb](Point p,Point& raw){ return TryProjectDocument(pb,p,raw); },layout,linear};
        const auto actual = RenderMappedPair(a,b,mapped);
        Check(actual.image.bgr==expected.image.bgr, "integer pixel-space bridge is byte-identical to legacy in both layouts");
        Check(actual.seam_navigation.available==expected.seam_navigation.available &&
            actual.seam_navigation.x==expected.seam_navigation.x && actual.seam_navigation.y==expected.seam_navigation.y,
            "legacy bridge preserves overlap navigation");
    }
}
void TestCoverage() {
    const auto a=Image(8,8,20), b=Image(8,8,220); const auto a_before=a.bgr, b_before=b.bgr;
    for (auto layout : {horizontal,vertical}) for (auto kernel : {linear,cubic}) {
        auto at = [layout](Point p){ return static_cast<int>(layout==horizontal ? p.x : p.y); };
        const MappedRenderParameters runs{layout==horizontal ? 8u : 1u,layout==horizontal ? 1u : 8u,
            [](Point,Point& raw){raw={0,0};return true;},
            [at](Point p,Point& raw){raw={0,0};const int n=at(p);return n<2 || (n>=4 && n<7);},layout,kernel};
        const auto output=RenderMappedPair(a,b,runs);
        const std::array<int,8> expected{20,120,20,20,20,87,153,20};
        for (std::size_t n=0;n<expected.size();++n) for (int c=0;c<3;++c)
            Check(output.image.bgr[n*3+c]==expected[n], "each disjoint overlap run has its own half-open feather interval");
        Check(output.seam_navigation.available, "multiple overlap runs produce navigation");
        Check(output.image.bgr==RenderMappedPair(a,b,runs).image.bgr, "mapped repeat bytes for both layouts/kernels");
        const MappedRenderParameters split{runs.width,runs.height,
            [at](Point p,Point& raw){raw={0,0};return at(p)<3;},
            [at](Point p,Point& raw){raw={0,0};return at(p)>=3;},layout,kernel};
        const auto joined=RenderMappedPair(a,b,split);
        Check(!joined.seam_navigation.available, "disjoint complete coverage has no seam navigation");
        for (int n=0;n<8;++n) Check(joined.image.bgr[n*3]==(n<3 ? 20 : 220), "non-overlapping pixels use the sole camera");
    }
    Reject([&]{RenderMappedPair(a,b,{2,2,Missing(),Missing(),horizontal,linear});}, "uncovered sample fails");
    Reject([&]{RenderMappedPair(a,b,{2,2,[](Point,Point& p){p={std::numeric_limits<double>::quiet_NaN(),0};return true;},Missing(),horizontal,linear});}, "nonfinite callback coordinates fail closed");
    for (auto kernel : {linear,cubic}) {
        Reject([&]{RenderMappedPair(a,b,{2,2,
            [](Point,Point& p){p={std::numeric_limits<double>::quiet_NaN(),0};return true;},Identity(),horizontal,kernel});},
            "CAM-B coverage cannot conceal CAM-A callback NaN");
        Reject([&]{RenderMappedPair(a,b,{2,2,Identity(),
            [](Point,Point& p){p={0,std::numeric_limits<double>::infinity()};return true;},horizontal,kernel});},
            "CAM-A coverage cannot conceal CAM-B callback infinity");
        auto overflow=Projection({10,0,2,0,10,2,0,0,1});
        overflow.distortion.p1=std::numeric_limits<double>::max();
        const OutputRaster small{{0,0,400,400},254,4,4};
        const auto normal=Projection({10,0,2,0,10,2,0,0,1});
        Reject([&]{RenderDocumentPair(a,b,{small,overflow,normal,horizontal,kernel});},
            "finite projection coefficients overflowing raw pixels cannot fall back to the other camera");
    }
    std::size_t calls=0;
    Reject([&]{RenderMappedPair(a,b,{2,2,[&](Point,Point& p){p={0,0};return ++calls<=4;},Missing(),horizontal,linear});}, "callback changing coverage across traversals is rejected");
    Check(a.bgr==a_before && b.bgr==b_before, "success and failure preserve both source buffers");
}
void TestValidation() {
    const OutputRaster raster{{0,0,1600,1200},254,16,12};
    ValidateOutputRaster(raster); ValidateCameraProjection(Projection(),raster);
    ValidateRadialMonotonicity({0,0,0,0,0},100);
    Check(true,"finite identity, exact raster and zero-distortion validation");
    for (auto invalid : {
        OutputRaster{{0,0,1600,1200},0,16,12}, OutputRaster{{0,0,1600,1200},254,17,12},
        OutputRaster{{1600,0,0,1200},254,16,12}, OutputRaster{{0,0,100,100},100,0,1},
        OutputRaster{{0,0,3276900,100},254,32769,1}, OutputRaster{{0,0,2000000,2000000},254,20000,20000},
        OutputRaster{{0,0,std::numeric_limits<std::int64_t>::max(),100},65535,1,1}})
        Reject([&]{ValidateOutputRaster(invalid);}, "invalid raster ordering, arithmetic, dimensions or overflow");
    ValidateOutputRaster({{0,0,101,201},100,1,1}); // ceil(101*100/25400)=1; height also 1.
    Check(true,"integer rational ceil dimensions are accepted");
    for (auto invalid : {
        Projection({0,0,0,0,0,0,0,0,1}), Projection({1,0,0,0,1,0,0,0,2}),
        Projection({1,0,0,0,1,0,-1,0,1}), Projection({std::numeric_limits<double>::infinity(),0,0,0,1,0,0,0,1})})
        Reject([&]{ValidateCameraProjection(invalid,raster);}, "singular, unnormalized, horizon-crossing or nonfinite homography");
    auto bad=Projection(); bad.intrinsics.fx_pixels=0;
    Reject([&]{ValidateCameraProjection(bad,raster);}, "intrinsic focal length must be positive");
    bad=Projection(); bad.distortion.p2=std::numeric_limits<double>::quiet_NaN();
    Reject([&]{ValidateCameraProjection(bad,raster);}, "all five lens coefficients must be finite");
    Reject([]{ValidateRadialMonotonicity({-2,1.2,0,0,0},1);}, "radial derivative 1-6t+6t^2 has positive endpoints but a negative interior valley");
    Reject([]{ValidateRadialMonotonicity({-2,1.2,1.0/7,0,0},1);},
        "cubic radial derivative 1-6t+6t^2+t^3 has positive endpoints but a negative interior valley");
    Reject([]{ValidateRadialMonotonicity({-1.0/3,0,0,0,0},1);}, "radial derivative reaching zero is invalid");
    Reject([]{ValidateRadialMonotonicity({0,0,0,0,0},-1);}, "radius domain cannot be negative");
    const OutputRaster tiny{{0,0,100,100},100,1,1};
    // Declared right=.1mm gives w=.2>0; actual sample X=.127mm gives w=-.016.
    Reject([&]{ValidateCameraProjection(Projection({1,0,0,0,1,0,-8,0,1}),tiny);}, "ceil raster samples outside declared region cannot cross a projective horizon");
    auto malformed=Image(2,2); malformed.bgr.pop_back();
    Reject([&]{RenderMappedPair(malformed,Image(2,2),{2,2,Identity(),Missing(),horizontal,linear});}, "malformed BGR length is refused before sampling");
    Reject([&]{RenderMappedPair(Image(2,2),Image(2,2),{2,2,Identity(),Missing(),horizontal,static_cast<Resampling>(99)});}, "unsupported interpolation cannot silently become a supported kernel");
}
} // namespace
int main() {
    try { TestProjectionOracle(); TestSamplingAndDocumentRaster(); TestLegacyCompatibility(); TestCoverage(); TestValidation(); }
    catch (const std::exception& error) { ++failures; std::cerr<<"Unexpected test exception: "<<error.what()<<'\n'; }
    std::cout<<"document_render checks="<<checks<<" failures="<<failures<<'\n';
    return failures==0 ? 0 : 1;
}
