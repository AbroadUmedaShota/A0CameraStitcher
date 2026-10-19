#include "a0/m2/experimental/evaluation_render_conditions.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using namespace a0::m2;
using namespace a0::m2::render;
using namespace a0::m2::experimental;
int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class F> void Reject(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, message);
}
const std::string hash(64, 'a');
EvaluationRenderIdentity Identity() { return {hash, std::string(64,'b'), {}, {}}; }
CameraProjection Projection() {
    return {{50,55,25,23}, {.01,-.001,0,.0002,-.0001}, {4,.2,1,.1,4,2,0,0,1}};
}
DocumentRenderParameters Parameters(StitchLayout layout, Resampling kernel) {
    auto b = Projection(); b.document_to_image[2] += 2;
    return {{{1000,2000,9000,8000},254,80,60},Projection(),b,layout,kernel};
}
OutputRaster PreviewRaster() { return {{1000,2000,9000,8000},127,40,30}; }
LiveViewMappingPair Mapping() {
    return {hash, {{{{64,64},{32,32},.5,.5,-1,-2}, {{48,56},{12,28},.25,.5,-1,-1}}},
        MappingEvidence::synthetic_fixture, true};
}
BgrImage Ramp(SourceDimensions size, int camera, double sx=1, double sy=1, double tx=0, double ty=0) {
    BgrImage image{size.width,size.height,std::vector<std::uint8_t>(size.width*size.height*3)};
    for (unsigned y=0; y<size.height; ++y) for (unsigned x=0; x<size.width; ++x)
        for (unsigned c=0; c<3; ++c) {
            const double raw_x=(x-tx)/sx, raw_y=(y-ty)/sy;
            const double value=20+10*camera+(camera+1)*raw_x+raw_y+3*c;
            image.bgr[(y*size.width+x)*3+c]=static_cast<std::uint8_t>(std::clamp(std::lround(value),0L,255L));
        }
    return image;
}
// Independent explicit Brown-Conrady polynomial; no production projection call.
Point OracleProjection(const CameraProjection& p, double x, double y) {
    const auto& h=p.document_to_image; const auto& k=p.intrinsics; const auto& d=p.distortion;
    const double z=h[6]*x+h[7]*y+1;
    const double u=((h[0]*x+h[1]*y+h[2])/z-k.cx_pixels)/k.fx_pixels;
    const double v=((h[3]*x+h[4]*y+h[5])/z-k.cy_pixels)/k.fy_pixels;
    const double r=u*u+v*v, radial=1+d.k1*r+d.k2*r*r+d.k3*r*r*r;
    return {k.fx_pixels*(u*radial+2*d.p1*u*v+d.p2*(3*u*u+v*v))+k.cx_pixels,
        k.fy_pixels*(v*radial+d.p1*(u*u+3*v*v)+2*d.p2*u*v)+k.cy_pixels};
}
void Success() {
    const auto a=Ramp({64,64},0), b=Ramp({48,56},1);
    const auto live_a=Ramp({32,32},0,.5,.5,-1,-2), live_b=Ramp({12,28},1,.25,.5,-1,-1);
    const BgrAffineTransform color{{.5,1,1.25},{-5,2,3}};
    for (const auto kernel : {Resampling::bilinear, Resampling::bicubic_catmull_rom})
        for (const auto layout : {StitchLayout::camera_a_left_camera_b_right,StitchLayout::camera_a_top_camera_b_bottom}) {
            auto parameters=Parameters(layout,kernel); auto identity=Identity();
            identity.correction=EvaluationCorrectionIdentity{std::string(64,'c'),std::string(64,'d')};
            identity.color=EvaluationCorrectionIdentity{std::string(64,'e'),std::string(64,'f')};
            EvaluationRenderConditions conditions(parameters,{64,64},{48,56},color,identity);
            parameters.camera_b.document_to_image[2]=999; identity.profile_fingerprint="changed";
            Check(conditions.Identity().profile_fingerprint==hash && conditions.Parameters().camera_b.document_to_image[2]==3,
                "conditions own projection and provenance snapshots");
            Check(conditions.Identity().color->report_sha256==std::string(64,'f'),"applied color report retained");
            const auto still=conditions.RenderStill(a,b);
            const auto direct=RenderDocumentPair(a,b,conditions.Parameters(),color);
            Check(still.image.bgr==direct.image.bgr,"saved evaluation preserves existing renderer bytes");
            const auto preview=conditions.RenderPreviewForEvaluation(live_a,live_b,PreviewRaster(),Mapping());
            Check(preview.image.width==40 && preview.image.height==30,"explicit lower-density same-crop raster");
            for (unsigned y=0; y<30; ++y) for (unsigned x=0; x<40; ++x) {
                const double dx=1+(x+.5)*.2, dy=2+(y+.5)*.2;
                const auto pa=OracleProjection(conditions.Parameters().camera_a,dx,dy);
                const auto pb=OracleProjection(conditions.Parameters().camera_b,dx,dy);
                const double weight=layout==StitchLayout::camera_a_left_camera_b_right ? x/40. : y/30.;
                for (unsigned c=0; c<3; ++c) {
                    const double va=20+pa.x+pa.y+3*c;
                    const double vb=(30+2*pb.x+pb.y+3*c)*color.gain[c]+color.offset_dn[c];
                    const auto expected=std::clamp(std::lround((1-weight)*va+weight*vb),0L,255L);
                    Check(std::abs(int(preview.image.bgr[(y*40+x)*3+c])-expected)<=1,
                        "Live geometry, half-sample, lens, color and feather agree with ramp oracle within 1 DN");
                }
            }
            // Identity mapping must be exact, including seam and edge behavior.
            auto full=Mapping(); full.cameras={{{{64,64},{64,64},1,1,0,0},{{48,56},{48,56},1,1,0,0}}};
            Check(conditions.RenderPreviewForEvaluation(a,b,conditions.Parameters().raster,full).image.bgr==still.image.bgr,
                "identity Live mapping is byte-identical to saved output");
        }
}
void Rejections() {
    const auto p=Parameters(StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear);
    const EvaluationRenderConditions conditions(p,{64,64},{48,56},{},Identity());
    auto a=Ramp({32,32},0,.5,.5,-1,-2), b=Ramp({12,28},1,.25,.5,-1,-1);
    const auto attempt=[&](LiveViewMappingPair mapping, OutputRaster raster=PreviewRaster()) {
        conditions.RenderPreviewForEvaluation(a,b,raster,mapping);
    };
    auto m=Mapping(); m.profile_fingerprint=std::string(64,'0'); Reject([&]{attempt(m);},"different profile rejected");
    m=Mapping(); m.evidence=MappingEvidence::unverified; Reject([&]{attempt(m);},"unverified mapping rejected");
    m=Mapping(); m.evidence=static_cast<MappingEvidence>(99); Reject([&]{attempt(m);},"unknown evidence rejected");
    m=Mapping(); m.normal_magnification=false; Reject([&]{attempt(m);},"zoomed mapping rejected");
    for (double value : {0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        m=Mapping(); m.cameras[0].scale_x=value; Reject([&]{attempt(m);},"invalid horizontal scale rejected");
        m=Mapping(); m.cameras[1].scale_y=value; Reject([&]{attempt(m);},"invalid vertical scale rejected");
    }
    m=Mapping(); m.cameras[0].offset_x=std::numeric_limits<double>::infinity(); Reject([&]{attempt(m);},"nonfinite offset rejected");
    m=Mapping(); m.cameras[0].scale_x=1e308; Reject([&]{attempt(m);},"finite mapping that overflows is rejected");
    m=Mapping(); m.cameras[1].still.width=49; Reject([&]{attempt(m);},"still mapping dimensions mismatch rejected");
    m=Mapping(); m.cameras[0].live.height=31; Reject([&]{attempt(m);},"Live frame dimensions mismatch rejected");
    m=Mapping(); m.cameras[0].live.width=0; Reject([&]{attempt(m);},"empty mapping dimensions rejected");
    auto crop=PreviewRaster(); crop.region_um.left=0; crop.region_um.right=8000;
    Reject([&]{attempt(Mapping(),crop);},"changed document crop rejected");
    auto bigger=p.raster; bigger.dpi=508; bigger.width_pixels=160; bigger.height_pixels=120;
    Reject([&]{attempt(Mapping(),bigger);},"preview upsampling rejected");
    auto malformed=PreviewRaster(); malformed.width_pixels=41; Reject([&]{attempt(Mapping(),malformed);},"incorrect ceil raster rejected");
    m=Mapping(); m.cameras[0].offset_x=-999; m.cameras[1].offset_x=-999;
    Reject([&]{attempt(m);},"uncovered crop fails without fallback");
    a.bgr.pop_back(); Reject([&]{attempt(Mapping());},"invalid frame buffer rejected");
    Reject([&]{conditions.RenderStill(Ramp({63,64},0),Ramp({48,56},1));},"saved source dimensions mismatch rejected");
    auto id=Identity(); id.profile_fingerprint="";
    Reject([&]{EvaluationRenderConditions bad(p,{64,64},{48,56},{},id);},"missing profile identity rejected");
    id=Identity(); id.color=EvaluationCorrectionIdentity{hash,hash};
    Reject([&]{EvaluationRenderConditions bad(p,{64,64},{48,56},{},id);},"color provenance without geometry or transform rejected");
    id=Identity(); id.profile_file_sha256=std::string(64,'A');
    Reject([&]{EvaluationRenderConditions bad(p,{64,64},{48,56},{},id);},"malformed profile file hash rejected");
    id=Identity(); id.correction=EvaluationCorrectionIdentity{hash,""};
    Reject([&]{EvaluationRenderConditions bad(p,{64,64},{48,56},{},id);},"missing correction report hash rejected");
    id=Identity(); id.correction=EvaluationCorrectionIdentity{hash,hash}; id.color=id.correction;
    Reject([&]{EvaluationRenderConditions bad(p,{64,64},{48,56},{},id);},"color provenance without transform rejected");
    Reject([&]{EvaluationRenderConditions bad(p,{0,64},{48,56},{},Identity());},"empty original source rejected");
    auto malformed_p=p; malformed_p.resampling=static_cast<Resampling>(99);
    Reject([&]{EvaluationRenderConditions bad(malformed_p,{64,64},{48,56},{},Identity());},"unknown kernel rejected");
    malformed_p=p; malformed_p.layout=static_cast<StitchLayout>(99);
    Reject([&]{EvaluationRenderConditions bad(malformed_p,{64,64},{48,56},{},Identity());},"unknown layout rejected");
    malformed_p=p; malformed_p.camera_b.document_to_image[8]=0;
    Reject([&]{EvaluationRenderConditions bad(malformed_p,{64,64},{48,56},{},Identity());},"invalid camera projection rejected");
    Reject([&]{EvaluationRenderConditions bad(p,{64,64},{48,56},BgrAffineTransform{{1,0,1},{0,0,0}},id);},"invalid evaluated color gain rejected");
    Reject([&]{EvaluationRenderConditions bad(p,{64,64},{48,56},BgrAffineTransform{{1,1,1},{0,0,0}},Identity());},"unbound color transform rejected");
}
void Coverage() {
    // CAM-A covers x=[0,8), CAM-B covers x=[4,12). Independently expect
    // A-only / overlap / B-only and a separate Live crop that removes A's left.
    const CameraProjection ca{{1,1,0,0},{0,0,0,0,0},{1,0,0,0,1,0,0,0,1}};
    auto cb=ca; cb.document_to_image[2]=-4;
    EvaluationRenderConditions c({{{0,0,12000,2000},254,120,20},ca,cb,StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear},
        {8,4},{8,4},{},Identity());
    auto a=Ramp({8,4},0), b=Ramp({8,4},1);
    LiveViewMappingPair m{hash,{{{{8,4},{8,4},1,1,0,0},{{8,4},{8,4},1,1,0,0}}},MappingEvidence::synthetic_fixture,true};
    const auto preview=c.RenderPreviewForEvaluation(a,b,{{0,0,12000,2000},127,60,10},m);
    for (unsigned x=0; x<60; ++x) {
        const double raw=(x+.5)*.2;
        const double va=20+std::min(raw,7.)+.1;
        const double vb=30+2*std::min(raw-4,7.)+.1;
        const double weight=x<20?0:x>=40?1:(x-20)/20.;
        const auto expected=std::lround(va*(1-weight)+vb*weight);
        Check(int(preview.image.bgr[x*3])==expected,"independent A-only / overlap / B-only oracle");
    }
    m.cameras[0].offset_x=-2;
    Reject([&]{c.RenderPreviewForEvaluation(a,b,{{0,0,12000,2000},127,60,10},m);},"explicit Live crop may remove coverage");
    // A point outside both original sensor domains must not become covered
    // merely because a positive Live offset moves it into padded buffers.
    auto outside=ca; outside.document_to_image[2]=-10;
    EvaluationRenderConditions padded({{{0,0,2000,2000},254,20,20},outside,outside,StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear},
        {8,4},{8,4},{},Identity());
    m.cameras[0].offset_x=12; m.cameras[1].offset_x=12;
    Reject([&]{padded.RenderPreviewForEvaluation(a,b,{{0,0,2000,2000},127,10,10},m);},"Live padding never invents original coverage");
    // The coarser ceil raster extends beyond the nominal crop. Its final
    // center is 1.1 mm, not 1.0 mm or a rescale of the original pixel index.
    EvaluationRenderConditions ceil({{{0,0,1100,1100},254,11,11},ca,ca,StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear},
        {8,4},{8,4},{},Identity());
    m.cameras[0].offset_x=0; m.cameras[1].offset_x=0;
    const auto extension=ceil.RenderPreviewForEvaluation(a,a,{{0,0,1100,1100},127,6,6},m);
    Check(extension.image.width==6 && extension.image.bgr[(5*6+5)*3]==22,"fractional ceil extent uses document sample centers");
}
}
int main() {
    static_assert(!std::is_default_constructible_v<EvaluationRenderConditions>);
    try { Success(); Rejections(); Coverage(); }
    catch (const std::exception& e) { ++failures; std::cerr << "unexpected: " << e.what() << '\n'; }
    std::cout << "evaluation_render_conditions checks=" << checks << " failures=" << failures << '\n';
    return failures?1:0;
}
