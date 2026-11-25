// The live mapping and the capture's mapping apply the same surface (#434). The live C draws camera B with the pose and
// LiveAppliedSurfaceOf(the estimated plane); the capture's level draws it with the pose and
// SurfaceAppliedAcrossWidthOfLambda(the estimated plane, the capture's lambda). These tests hold the two paths together on one
// synthetic rig, reading the products' own fields (the capture's lambda, the live outcome's two planes, the live inputs, the level's
// mapping) and never making the lambda or the applied plane again here:
//  - the live outcome's surface is the capture's rule applied to the live estimate, bit for bit;
//  - estimates that differ only in the slope across the width give the same mapping on both paths, and estimates that differ in the
//    slope across the width and have a height stretch the mapping across the width by less than the line;
//  - C draws the mapping the JPEG's maker draws, at the capture's scale and at the live view's;
//  - one estimate given to both paths gives one mapping.
// Everything is synthetic: the scenes of tests/parallax_scene.hpp on its standard raster (48 dpi) and camera A, with camera B 2 percent
// higher than A. No camera, window or file.
#include "a0/m2/experimental/capture_registration.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/phase0/experimental/live_registration_runtime.hpp"
#include "parallax_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace a0::phase0::experimental;
namespace ex=a0::m2::experimental;
namespace render=a0::m2::render;
namespace cr=a0::m2::experimental::content_registration;
namespace ps=parallax_scene;
using Pose=cr::PoseParameters;

namespace {
int checks=0,failures=0;
void Check(bool ok,const std::string& what,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL line "<<line<<": "<<what<<'\n';}}
#define CHECK(x) Check((x),#x,__LINE__)
void Threw(const char* name,const std::exception& error){Check(false,std::string(name)+" threw: "+error.what(),0);}

// ---- Lines (not moved after a result) ---------------------------------------------------------------------------------------------
// The stretch across the width between two mappings, in drawing mm: the gate's 0.8 px of the product's condition raster. The synthetic
// raster is 48 dpi, where 0.8 px would be about 0.42 mm, so the line is held in drawing mm and the 48 dpi pixels are printed beside it.
constexpr double kStretchLineMm=0.113;
// One estimate through the two paths: the two mappings place every point of the raster within this (drawing mm).
constexpr double kWiringLineMm=1e-9;
// C's pixel against the JPEG maker's pixel at the same drawing point, in live pixels.
constexpr double kLiveConventionLinePx=1e-6;

// ---- Bits ----------------------------------------------------------------------------------------------------------------------------
std::uint64_t BitsOf(double v){std::uint64_t b{};std::memcpy(&b,&v,sizeof b);return b;}
bool SamePlaneBits(const ex::SurfacePlane& a,const ex::SurfacePlane& b){return BitsOf(a.h0_mm)==BitsOf(b.h0_mm) && BitsOf(a.slope_x)==BitsOf(b.slope_x) && BitsOf(a.slope_y)==BitsOf(b.slope_y);}
bool SamePoseBits(const Pose& a,const Pose& b){
    return BitsOf(a.rx_deg)==BitsOf(b.rx_deg) && BitsOf(a.ry_deg)==BitsOf(b.ry_deg) && BitsOf(a.rz_deg)==BitsOf(b.rz_deg) &&
        BitsOf(a.tx_mm)==BitsOf(b.tx_mm) && BitsOf(a.ty_mm)==BitsOf(b.ty_mm) && BitsOf(a.tz_percent)==BitsOf(b.tz_percent);
}
bool SamePoint(const render::Point& a,const render::Point& b){return BitsOf(a.x)==BitsOf(b.x) && BitsOf(a.y)==BitsOf(b.y);}
double TiltDegOf(const ex::SurfacePlane& p){return std::atan(std::hypot(p.slope_x,p.slope_y))*180/ps::kPi;}
void Print(const char* what,const Pose& p,const ex::SurfacePlane& s){
    std::cout<<"    "<<what<<": pose "<<p.rx_deg<<' '<<p.ry_deg<<' '<<p.rz_deg<<' '<<p.tx_mm<<' '<<p.ty_mm<<' '<<p.tz_percent
        <<"; plane "<<s.h0_mm<<' '<<s.slope_x<<' '<<s.slope_y<<'\n';
}

// ---- The rig -------------------------------------------------------------------------------------------------------------------------
// The standard raster and camera A, with camera B 2 percent higher than A: with the two cameras at one height the stretch of a level
// plane across the width cancels, and a rule that is wrong in the height would not show.
ps::Rig AgreementRig(){
    auto rig=ps::StandardRig();
    rig.camera_b=ps::MakeCamera(520,180,714,-0.4,0.2,-0.1);
    return rig;
}
// A pose of camera B that is not zero in any component (synthetic).
const Pose kSyntheticPose{0.15,-0.1,0.3,2.0,-1.5,0.4};

// ---- Live frames (the helpers of tests/live_registration_estimator_tests.cpp, unchanged) -----------------------------------------------
constexpr unsigned kViewWidth=ps::kRawWidth/2,kViewHeight=ps::kRawHeight/2;
constexpr double kScale=0.5;         // live pixels per raw pixel
constexpr double kNoiseSigma=2.0;    // DN

// The raw frame reduced by 2: live pixel (j, k) is the raw frame around (2j, 2k) under the kernel (1 2 1)/4 in both directions,
// so a live coordinate is the raw coordinate times 0.5 with no half-pixel shift (the convention of LiveViewOutputToCamera).
render::BgrImage Reduce(const render::BgrImage& raw){
    render::BgrImage out{raw.width/2,raw.height/2,std::vector<std::uint8_t>(static_cast<size_t>(raw.width/2)*(raw.height/2)*3)};
    const int w=static_cast<int>(raw.width),h=static_cast<int>(raw.height);
    constexpr int weight[3]{1,2,1};
    for(unsigned k=0;k<out.height;++k)for(unsigned j=0;j<out.width;++j)for(int c=0;c<3;++c){
        int sum=0;
        for(int b=-1;b<=1;++b)for(int a=-1;a<=1;++a){
            const int x=std::clamp(static_cast<int>(2*j)+a,0,w-1),y=std::clamp(static_cast<int>(2*k)+b,0,h-1);
            sum+=weight[a+1]*weight[b+1]*raw.bgr[(static_cast<size_t>(y)*raw.width+x)*3+c];
        }
        out.bgr[(static_cast<size_t>(k)*out.width+j)*3+c]=static_cast<std::uint8_t>((sum+8)/16);
    }
    return out;
}
// Noise that is about Gaussian (the sum of four uniform draws), the same for the three channels of a pixel, from a seed.
void AddNoise(render::BgrImage& image,std::uint64_t seed,double sigma){
    ps::Lcg r{seed*0x9E3779B97F4A7C15ull+1};
    for(size_t i=0;i<image.bgr.size();i+=3){
        const double n=(r.Uniform()+r.Uniform()+r.Uniform()+r.Uniform()-2.0)*std::sqrt(3.0)*sigma;
        for(int c=0;c<3;++c)image.bgr[i+c]=static_cast<std::uint8_t>(std::clamp(std::lround(image.bgr[i+c]+n),0L,255L));
    }
}
// The frame turned by 180 degrees (what OrientRenderInput does to the frames of a rig mounted upside down).
render::BgrImage Turned(const render::BgrImage& image){
    auto out=image;
    const size_t n=static_cast<size_t>(image.width)*image.height;
    for(size_t i=0;i<n;++i)for(int c=0;c<3;++c)out.bgr[i*3+c]=image.bgr[(n-1-i)*3+c];
    return out;
}
render::BgrImage Blank(){return render::BgrImage{kViewWidth,kViewHeight,std::vector<std::uint8_t>(static_cast<size_t>(kViewWidth)*kViewHeight*3,128)};}

// ---- The live side ---------------------------------------------------------------------------------------------------------------------
// The limits of the live view and the condition's ranges that tests/live_registration_estimator_tests.cpp runs with (development
// values for the synthetic rig, not product values).
cr::RegistrationLimits LiveLimits(){
    cr::RegistrationLimits l;
    l.brown_lowe_alpha=8;l.brown_lowe_beta=0.3;l.minimum_unique_blocks=3;
    l.maximum_prediction_sigma_px=0.25;l.maximum_residual_rms_px=0.3;l.maximum_residual_p95_px=0.6;l.maximum_bin_bias_px=0.25;
    l.convergence_px=0.05;l.start_agreement_px=0.25;
    return l;
}
LiveRegistrationRanges Ranges(){
    LiveRegistrationRanges r;
    r.envelope.position_mm=10;r.envelope.rotation_deg=2;r.envelope.tilt_deg=1;r.envelope.height_percent=1;
    r.surface_envelope=ex::SurfaceEnvelope{40,8};
    r.omega_margin_px=4;r.guard_px=14;r.band_half_width_px=178;
    r.apply_beyond_envelope=true;
    return r;
}
// The plan of a live C at half the raw size, as the estimator's tests build it (LiveRig there), for a given rig and mount.
struct LiveSide {
    ps::Rig rig;
    ps::SeamPlacement seam;
    bool turned{};
    std::shared_ptr<LiveRegistrationContext> context;
    std::shared_ptr<const FixedPreviewPlan> plan;
    LiveSide(const ps::Rig& r,bool turned_mount):rig(r),seam(ps::PlaceSeam(r)),turned(turned_mount){
        const auto parameters=rig.Parameters();
        const auto grid=MakeFixedPreviewGrid(rig.raster,kScale);
        const ex::RegisteredMapping fixed{Pose{},{},std::nullopt};
        context=std::make_shared<LiveRegistrationContext>(parameters,rig.raster,seam.column,grid,kScale,kScale,kViewWidth,kViewHeight,turned,fixed);
        context->live_limits=LiveLimits();
        context->table_a=BakeOutputToCamera(LiveViewOutputToCamera(parameters.camera_a,grid,kScale,kScale,kViewWidth,kViewHeight,turned),grid.width,grid.height);
        context->table_b=BakeOutputToCamera(LiveViewRegisteredOutputToCamera(*context,fixed),grid.width,grid.height);
        FixedPreviewPlan p;
        p.lens_aware=true;p.view_width=kViewWidth;p.view_height=kViewHeight;p.scale_x=p.scale_y=kScale;
        p.grid=grid;p.output_width=grid.width;p.output_height=grid.height;
        p.capture_seam_column=seam.column;
        p.scaled_seam_column=static_cast<std::uint32_t>(std::floor((seam.column+0.5)*kScale));
        p.view.seam_column=p.scaled_seam_column;
        p.settings=ScaleSeamBlendSettings(ex::kSeamBlendDesign,640.0/7360.0,640.0/7360.0);
        p.mapped=std::make_shared<const ex::MappedFixedSeamInput>(ex::MappedFixedSeamInput{grid.width,grid.height,
            TableOutputToCamera(context->table_a,grid.width,grid.height),TableOutputToCamera(context->table_b,grid.width,grid.height)});
        p.live=context;
        plan=std::make_shared<const FixedPreviewPlan>(std::move(p));
    }
};
// One full job through MakeLiveEstimator with an entrance that keeps the inputs the product prepared from the plan. `estimate` false:
// the entrance answers LiveEstimatorNotConnected and only the inputs are read.
struct LiveRun {
    LiveEstimatorInputs inputs;
    LiveEstimateOutcome outcome;
    bool recorded{};
};
LiveRun RunLive(const LiveSide& live,const render::BgrImage& a,const render::BgrImage& b,bool estimate){
    LiveRun run;
    const auto plan=live.plan;
    LiveEstimatorEntrance entrance=[&run,estimate](const LiveEstimatorInputs& in){
        run.inputs=in;run.recorded=true;
        return estimate?EstimateLiveRegistration(in,kLiveEstimatorDesignDraft):LiveEstimatorNotConnected(in);
    };
    const auto estimator=MakeLiveEstimator([plan]{return plan;},kLiveTrackerDesignDraft,std::move(entrance),Ranges());
    LiveJob job;job.kind=LiveJobKind::full;
    run.outcome=estimator(job,a,b);
    // The job and the frames the inputs pointed at do not outlive the call; the model of B lives in the plan's context.
    run.inputs.job=nullptr;run.inputs.camera_a=nullptr;run.inputs.camera_b=nullptr;run.inputs.stop=nullptr;
    return run;
}
LiveRun InputsOnly(const LiveSide& live){const auto blank=Blank();return RunLive(live,blank,blank,false);}

// ---- The capture side ------------------------------------------------------------------------------------------------------------------
// The settings of tests/capture_registration_tests.cpp (Request, Lines) on this rig, with the seam column given (the live context's).
ex::RegistrationLines Lines(){
    ex::RegistrationLines l;
    l.envelope={10,2,1,5};l.surface_envelope=ex::SurfaceEnvelope{40,8};l.apply_beyond_envelope=true;l.max_lift_mm=1.0;
    l.seam_limit_px=1;l.seam_guard_band_px=0.25;
    return l;
}
ex::CaptureRegistrationRequest Request(const render::DocumentRenderParameters& parameters,const render::BgrImage& a,const render::BgrImage& b,std::uint32_t seam){
    ex::CaptureRegistrationRequest r;
    r.camera_a=&a;r.camera_b=&b;r.parameters=&parameters;
    r.estimator.geometry={seam,8,4,40,200};
    r.estimator.envelope={10.0,2.0,1.0,1.0};
    r.estimator.limits.brown_lowe_alpha=8;r.estimator.limits.brown_lowe_beta=0.3;r.estimator.limits.minimum_unique_blocks=2;
    r.estimator.limits.maximum_prediction_sigma_px=1.0;r.estimator.limits.maximum_residual_rms_px=0.5;r.estimator.limits.maximum_residual_p95_px=1.0;
    r.estimator.limits.maximum_bin_bias_px=0.5;r.estimator.limits.convergence_px=0.02;r.estimator.limits.start_agreement_px=0.3;
    r.estimator.design=cr::kRegistrationDesignDraft;
    r.estimator.design.coarse_block_rows=256;r.estimator.design.coarse_maximum_radius_px=256;r.estimator.design.fine_point_spacing_px=32;
    r.estimator.design.fine_patch_px=48;r.estimator.design.fine_search_radius_px=16;r.estimator.design.fine_maximum_search_radius_px=64;
    r.estimator.design.fine_global_search_radius_px=36;
    r.lines=Lines();r.lines.envelope=r.estimator.envelope;
    r.blend=ex::kSeamBlendDesign;
    r.design=ex::CaptureRegistrationDesignDraft();
    for(auto* d:{&r.design.omega,&r.design.band}){d->global_search_radius_px=128;d->global_minimum_anchors=2;}
    r.cover_check=[parameters,&a,&b,seam](const ex::RegisteredMapping& m){
        const auto pair=ex::RegisteredDocumentMappedFixedSeamInput(parameters,m,seam);
        return ex::MappingCoverCheck{ex::CanRenderMappedFixedSeam(a,b,pair,seam,ex::kSeamBlendDesign),true};
    };
    r.thread_count=4;
    return r;
}
// One capture prepared on the given frames (the stored order, the raw size of the synthetic rig).
struct CaptureSide {
    render::DocumentRenderParameters parameters;
    std::uint32_t seam{};
    render::BgrImage a,b;
    ex::CaptureRegistrationRequest request;
    std::optional<ex::PreparedCaptureRegistration> prepared;
    CaptureSide(const ps::Rig& rig,std::uint32_t seam_column,render::BgrImage frame_a,render::BgrImage frame_b)
        :parameters(rig.Parameters()),seam(seam_column),a(std::move(frame_a)),b(std::move(frame_b)){
        request=Request(parameters,a,b,seam);
        prepared=ex::PrepareCaptureRegistration(request);
    }
    CaptureSide(const CaptureSide&)=delete;
    CaptureSide& operator=(const CaptureSide&)=delete;
    [[nodiscard]] const ex::CaptureRegistration& Capture() const {return prepared->capture;}
};
// The level the capture chooses for a trusted estimate of the given pose and plane: the capture's lambda, its lines and base pose, no layer
// 2, every candidate passing the cheap check.
ex::GeometryLevelDecision CaptureLevelOf(const CaptureSide& capture,const Pose& pose,const ex::SurfacePlane& estimated){
    cr::RegistrationResult e;e.state=cr::RegistrationState::verified;e.has_estimate=true;e.pose=pose;
    ex::SurfaceEstimate surface;surface.state=ex::SurfaceState::estimated;surface.plane=estimated;surface.tilt_deg=TiltDegOf(estimated);
    ex::GeometryLevelInput in;in.registration=&e;in.surface=&surface;in.lines=&capture.request.lines;
    in.base_pose=capture.request.base_pose;in.seam_column=capture.seam;
    in.cover_check=[](const ex::RegisteredMapping&){return ex::MappingCoverCheck{true,true};};
    in.seam_line_lambda=capture.Capture().seam_line_lambda;
    return ex::SelectGeometryLevel(in);
}

// ---- The ruler: two mappings on the condition's raster, by one maker --------------------------------------------------------------------
render::Point DrawingPoint(const render::OutputRaster& raster,double column,double row){
    const auto f=cr::FrameOfRaster(raster);
    return {f.left_mm+(column+0.5)*f.pitch_mm,f.top_mm+(row+0.5)*f.pitch_mm};
}
render::Matrix3 RulerOf(const LiveRegistrationContext& c,const ex::RegisteredMapping& m){
    return cr::RegisteredHomography(c.model_b,c.centre_a,c.origin_mm,m.pose,m.surface);
}
render::Point Moved(const render::Matrix3& h,render::Point x){
    render::Point out{};
    if(!render::TryTransform(h,x,out))throw std::runtime_error("the ruler cannot place a point");
    return out;
}
render::Point Difference(const render::Matrix3& one,const render::Matrix3& other,render::Point x){
    const auto p=Moved(one,x),q=Moved(other,x);
    return {p.x-q.x,p.y-q.y};
}
// Over every row: the largest |delta| at the seam column and at the last column, and the largest |delta(last) - delta(seam)| (the
// stretch across the width), where delta is where one mapping puts B's drawing point minus where the other puts it (mm).
struct Spread {double seam_mm{},last_mm{},stretch_mm{};std::uint32_t rows{};};
Spread SpreadOf(const LiveRegistrationContext& c,const ex::RegisteredMapping& one,const ex::RegisteredMapping& other){
    const auto h1=RulerOf(c,one),h2=RulerOf(c,other);
    const double last=static_cast<double>(c.raster.width_pixels)-1;
    Spread s;
    for(std::uint32_t y=0;y<c.raster.height_pixels;++y){
        const auto at_seam=Difference(h1,h2,DrawingPoint(c.raster,c.seam_column,y)),at_last=Difference(h1,h2,DrawingPoint(c.raster,last,y));
        s.seam_mm=std::max(s.seam_mm,std::hypot(at_seam.x,at_seam.y));
        s.last_mm=std::max(s.last_mm,std::hypot(at_last.x,at_last.y));
        s.stretch_mm=std::max(s.stretch_mm,std::hypot(at_last.x-at_seam.x,at_last.y-at_seam.y));
        ++s.rows;
    }
    return s;
}
// The largest |delta| over every column and every row of the raster (mm).
double WholeGapMm(const LiveRegistrationContext& c,const ex::RegisteredMapping& one,const ex::RegisteredMapping& other){
    const auto h1=RulerOf(c,one),h2=RulerOf(c,other);
    double worst=0;
    for(std::uint32_t y=0;y<c.raster.height_pixels;++y)for(std::uint32_t x=0;x<c.raster.width_pixels;++x){
        const auto d=Difference(h1,h2,DrawingPoint(c.raster,x,y));
        worst=std::max(worst,std::hypot(d.x,d.y));
    }
    return worst;
}
double PitchMm(const render::OutputRaster& raster){return cr::FrameOfRaster(raster).pitch_mm;}

// ---- Scenes ----------------------------------------------------------------------------------------------------------------------------
struct Paper {
    std::shared_ptr<ps::SceneModel> model;
    render::BgrImage raw_a,raw_b;
};
// The raw frames of a paper on the rig: the height field, B's real pose, and the paper's content moved along x by shift_x_mm.
Paper Draw(const ps::Rig& rig,const ps::SeamPlacement& seam,const ps::HeightField& field,const Pose& b_pose,double shift_x_mm){
    ps::SceneSpec spec;spec.name="agreement";spec.field=field;spec.b_pose=b_pose;
    Paper paper;
    paper.model=std::make_shared<ps::SceneModel>(rig,seam,spec);
    if(shift_x_mm!=0){
        const auto base=paper.model->sheet;
        paper.model->sheet=[base,shift_x_mm](double x,double y){return base(x-shift_x_mm,y);};
    }
    paper.raw_a=ps::RenderRaw(*paper.model,ps::Camera::a);
    paper.raw_b=ps::RenderRaw(*paper.model,ps::Camera::b);
    return paper;
}
render::BgrImage LiveFrame(const render::BgrImage& raw,std::uint64_t seed,bool turned){
    auto frame=Reduce(raw);
    AddNoise(frame,seed,kNoiseSigma);
    return turned?Turned(frame):frame;
}
render::BgrImage CaptureFrame(const render::BgrImage& raw,std::uint64_t seed){auto frame=raw;AddNoise(frame,seed,kNoiseSigma);return frame;}

// The scene of the comparisons of the estimates: the sheet of tests/capture_registration_tests.cpp (TiltedSheet: 6 mm up at the seam,
// 2 degrees across the width, 0.5 degrees along the height), B at its calibrated pose, seen by the live view (normal mount) and by the
// capture. Made once.
struct TiltedScene {
    ps::Rig rig;
    ps::SeamPlacement seam;
    Paper paper;
    LiveSide live;
    LiveRun live_run;
    std::unique_ptr<CaptureSide> capture;
    explicit TiltedScene(const ps::Rig& r)
        :rig(r),seam(ps::PlaceSeam(r)),paper(Draw(r,seam,[&]{ps::HeightField f;f.plane=ps::PlaneOver(seam,6.0,2.0,0.5);return f;}(),Pose{},0)),live(r,false){
        live_run=RunLive(live,LiveFrame(paper.raw_a,21,false),LiveFrame(paper.raw_b,22,false),true);
        capture=std::make_unique<CaptureSide>(rig,seam.column,CaptureFrame(paper.raw_a,31),CaptureFrame(paper.raw_b,32));
    }
};
const TiltedScene& Tilted(){
    static std::unique_ptr<TiltedScene> scene;
    if(!scene)scene=std::make_unique<TiltedScene>(AgreementRig());
    return *scene;
}

// ---- T26 (a): the live surface is the capture's rule on the live estimate ---------------------------------------------------------------
void LiveSurfaceIsTheCaptureRule(){
    std::cout<<"the live surface is the capture's rule on the live estimate\n";
    const auto& s=Tilted();
    const auto& o=s.live_run.outcome;
    const auto& c=s.capture->Capture();
    std::cout<<std::setprecision(17)<<"    live usable "<<o.usable<<", capture level "<<ex::GeometryLevelCode(c.level.level)<<", lambda "
        <<(c.seam_line_lambda?*c.seam_line_lambda:0.0)<<'\n';
    Print("live estimate",o.pose,o.estimated_surface);
    Print("live applied",o.pose,o.surface);
    Print("capture estimate",c.registration.pose,c.surface.plane);
    Print("capture level",c.level.mapping.pose,c.level.mapping.surface);
    // The premise: both paths estimated a plane on this scene, and the slope across the width and the height tell the planes apart.
    const bool live_estimated=o.usable && s.live_run.recorded && std::abs(o.estimated_surface.slope_x)>0.01 && std::abs(o.estimated_surface.h0_mm)>1;
    const bool capture_estimated=(c.level.level==ex::GeometryLevel::g1 || c.level.level==ex::GeometryLevel::g2) && c.seam_line_lambda.has_value();
    CHECK(live_estimated);
    CHECK(capture_estimated);
    if(!live_estimated || !capture_estimated)return;
    const auto rule=cr::SurfaceAppliedAcrossWidthOfLambda(o.estimated_surface,*c.seam_line_lambda);
    CHECK(SamePlaneBits(o.surface,rule));
    CHECK(BitsOf(o.surface.slope_x)==BitsOf(0.0) && BitsOf(o.surface.h0_mm)!=BitsOf(o.estimated_surface.h0_mm));
}

// ---- T26 (b1): the slope across the width alone does not change the mapping, on either path ----------------------------------------------
void SlopeAcrossAloneLeavesTheMapping(){
    std::cout<<"the slope across the width alone leaves the mapping\n";
    const LiveSide live(AgreementRig(),false);
    const auto run=InputsOnly(live);
    CHECK(run.recorded && !run.outcome.usable);
    const auto& capture=*Tilted().capture;
    const ex::SurfacePlane e1{0,-0.04,0},e2{0,-0.01,0};
    for(const Pose& pose:{Pose{},kSyntheticPose}){
        const auto d1=CaptureLevelOf(capture,pose,e1),d2=CaptureLevelOf(capture,pose,e2);
        CHECK(d1.level==ex::GeometryLevel::g2 && d2.level==ex::GeometryLevel::g2);
        CHECK(SamePoseBits(d1.mapping.pose,d2.mapping.pose) && SamePlaneBits(d1.mapping.surface,d2.mapping.surface));
        const auto s1=LiveAppliedSurfaceOf(run.inputs,e1),s2=LiveAppliedSurfaceOf(run.inputs,e2);
        CHECK(SamePlaneBits(s1,s2));
        const auto m1=LiveViewRegisteredOutputToCamera(*live.context,ex::RegisteredMapping{pose,s1,std::nullopt});
        const auto m2=LiveViewRegisteredOutputToCamera(*live.context,ex::RegisteredMapping{pose,s2,std::nullopt});
        bool same=true;std::uint32_t n=0;
        for(std::uint32_t y=0;y<live.plan->output_height;++y)for(std::uint32_t x=0;x<live.plan->output_width;++x){
            render::Point a{},b{};
            const bool oa=m1({double(x),double(y)},a),ob=m2({double(x),double(y)},b);
            same=same && oa==ob && (!oa || SamePoint(a,b));++n;
        }
        CHECK(same && n==live.plan->output_width*live.plan->output_height && n>0);
    }
}

// ---- T26 (b2): a slope across the width with a height stretches the mapping across the width less than the line ---------------------------
void StretchAcrossTheWidth(){
    std::cout<<"the stretch across the width of two slopes across the width\n";
    const LiveSide live(AgreementRig(),false);
    const auto run=InputsOnly(live);
    const auto& capture=*Tilted().capture;
    const auto& ruler=*live.context;
    const double pitch=PitchMm(ruler.raster);
    const std::pair<double,double> heights[]{{0,0.009},{-3,0.009},{-40,0.009}};
    for(const Pose& pose:{Pose{},kSyntheticPose})for(const auto& [h0,gy]:heights){
        const ex::SurfacePlane e1{h0,-0.04,gy},e2{h0,-0.01,gy};
        const auto d1=CaptureLevelOf(capture,pose,e1),d2=CaptureLevelOf(capture,pose,e2);
        CHECK(d1.level==ex::GeometryLevel::g2 && d2.level==ex::GeometryLevel::g2);
        const auto on_capture=SpreadOf(ruler,d1.mapping,d2.mapping);
        const auto on_live=SpreadOf(ruler,ex::RegisteredMapping{pose,LiveAppliedSurfaceOf(run.inputs,e1),std::nullopt},
            ex::RegisteredMapping{pose,LiveAppliedSurfaceOf(run.inputs,e2),std::nullopt});
        std::cout<<std::setprecision(6)<<"    pose "<<(pose.IsZero()?"zero":"synthetic")<<", h0 "<<h0<<", gy "<<gy<<": capture stretch "<<on_capture.stretch_mm
            <<" mm ("<<on_capture.stretch_mm/pitch<<" px at 48 dpi), seam "<<on_capture.seam_mm<<" mm; live stretch "<<on_live.stretch_mm<<" mm ("
            <<on_live.stretch_mm/pitch<<" px at 48 dpi), seam "<<on_live.seam_mm<<" mm; rows "<<on_capture.rows<<'\n';
        CHECK(on_capture.rows==ruler.raster.height_pixels && on_live.rows==ruler.raster.height_pixels);
        CHECK(on_capture.stretch_mm<=kStretchLineMm);
        CHECK(on_live.stretch_mm<=kStretchLineMm);
    }
}

// ---- T26 (c): C draws the mapping that the JPEG's maker draws ------------------------------------------------------------------------------
void LiveDrawsTheJpegMapping(){
    std::cout<<"C draws the JPEG's mapping\n";
    const auto rig=AgreementRig();
    const LiveSide live(rig,false);
    const auto run=InputsOnly(live);
    const auto& capture=*Tilted().capture;
    const ex::SurfacePlane estimated{-40,-0.035,0.009};
    const auto d=CaptureLevelOf(capture,kSyntheticPose,estimated);
    CHECK(d.level==ex::GeometryLevel::g2);
    const ex::RegisteredMapping jpeg_mapping=d.mapping;
    const ex::RegisteredMapping live_mapping{kSyntheticPose,LiveAppliedSurfaceOf(run.inputs,estimated),std::nullopt};
    const auto parameters=rig.Parameters();
    const cr::PoseModel model(parameters.camera_b);
    const auto jpeg=ex::RegisteredDocumentOutputToCamera(rig.raster,parameters.camera_a,parameters.camera_b,model,jpeg_mapping,live.seam.column);
    // (c1) At the capture's scale, the live context's mapping is the maker's, bit for bit.
    {
        const auto grid=MakeFixedPreviewGrid(rig.raster,1.0);
        const LiveRegistrationContext context(parameters,rig.raster,live.seam.column,grid,1.0,1.0,ps::kRawWidth,ps::kRawHeight,false,
            ex::RegisteredMapping{Pose{},{},std::nullopt});
        const auto mine=LiveViewRegisteredOutputToCamera(context,live_mapping);
        bool same=true;int n=0;
        for(std::uint32_t y=0;y<grid.height;y+=7)for(std::uint32_t x=0;x<grid.width;x+=5){
            render::Point a{},b{};
            const bool oa=mine({double(x),double(y)},a),ob=jpeg({double(x),double(y)},b);
            same=same && oa==ob && (!oa || SamePoint(a,b));++n;
        }
        std::cout<<"    capture scale: "<<n<<" points\n";
        CHECK(same && n>1000);
    }
    // (c2) At the live view's scale, C's pixel is the maker's output at the same drawing point, scaled (both mounts).
    for(const bool turned:{false,true}){
        const LiveSide view(rig,turned);
        const auto& plan=*view.plan;
        const auto mine=LiveViewRegisteredOutputToCamera(*plan.live,live_mapping);
        const double s=plan.scale_x,pitch_jpeg=25.4/rig.raster.dpi,pitch_live=pitch_jpeg/s;
        const double left_mm=rig.raster.region_um.left/1000.0,top_mm=rig.raster.region_um.top/1000.0;
        double worst=0;int n=0;
        for(std::uint32_t y=0;y<plan.output_height;y+=3)for(std::uint32_t x=0;x<plan.output_width;x+=3){
            const render::Point drawing{left_mm+(x+.5)*pitch_live,top_mm+(y+.5)*pitch_live};
            const render::Point at{(drawing.x-left_mm)/pitch_jpeg-.5,(drawing.y-top_mm)/pitch_jpeg-.5};
            render::Point raw{},seen{};
            if(!jpeg(at,raw) || !mine({double(x),double(y)},seen))continue;
            render::Point want{raw.x*s,raw.y*plan.scale_y};
            if(turned)want={(plan.view_width-1.0)-want.x,(plan.view_height-1.0)-want.y};
            worst=std::max(worst,std::hypot(want.x-seen.x,want.y-seen.y));++n;
        }
        std::cout<<std::setprecision(6)<<"    live scale, "<<(turned?"turned":"normal")<<": "<<n<<" points, largest difference "<<worst<<" live px\n";
        CHECK(n>100 && worst<kLiveConventionLinePx);
    }
}

// ---- T28 (a): one estimate through the two paths gives one mapping ------------------------------------------------------------------------
void OneEstimateOneMapping(){
    std::cout<<"one estimate through the live path and the capture path\n";
    const LiveSide live(AgreementRig(),false);
    const auto run=InputsOnly(live);
    const auto& capture=*Tilted().capture;
    const std::pair<Pose,ex::SurfacePlane> estimates[]{
        {kSyntheticPose,{-40,-0.035,0.009}},{Pose{},{-40,0.035,-0.009}},{kSyntheticPose,{6,0.0,0.009}},{kSyntheticPose,{6,-0.0,0.009}}};
    for(const auto& [pose,estimated]:estimates){
        const auto d=CaptureLevelOf(capture,pose,estimated);
        CHECK(d.level==ex::GeometryLevel::g2);
        const ex::RegisteredMapping on_live{pose,LiveAppliedSurfaceOf(run.inputs,estimated),std::nullopt};
        const double gap=WholeGapMm(*live.context,on_live,d.mapping);
        std::cout<<std::setprecision(6)<<"    estimate "<<estimated.h0_mm<<' '<<estimated.slope_x<<' '<<estimated.slope_y<<": largest gap "<<gap<<" mm over "
            <<live.context->raster.width_pixels<<" x "<<live.context->raster.height_pixels<<'\n';
        CHECK(gap<=kWiringLineMm);
    }
}
}

int main(){
    try{LiveSurfaceIsTheCaptureRule();}catch(const std::exception& e){Threw("LiveSurfaceIsTheCaptureRule",e);}
    try{SlopeAcrossAloneLeavesTheMapping();}catch(const std::exception& e){Threw("SlopeAcrossAloneLeavesTheMapping",e);}
    try{StretchAcrossTheWidth();}catch(const std::exception& e){Threw("StretchAcrossTheWidth",e);}
    try{LiveDrawsTheJpegMapping();}catch(const std::exception& e){Threw("LiveDrawsTheJpegMapping",e);}
    try{OneEstimateOneMapping();}catch(const std::exception& e){Threw("OneEstimateOneMapping",e);}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures==0?0:1;
}
