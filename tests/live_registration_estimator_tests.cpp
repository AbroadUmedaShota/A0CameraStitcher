// The estimate of the live C (#417 LC; docs/design/fixed-composite-registration.md addendum 11.21): content_registration's estimate
// on C's grid, connected to the worker of the live registration. Everything is synthetic: the scenes of tests/parallax_scene.hpp
// (paper, two cameras, a pose of camera B and a height of the paper), reduced to the coarseness of a live view. No camera, window
// or file; no real image.
//
// The coarseness. The synthetic rig's raw frame is 1200 x 800 px over about 540 x 360 mm. It is reduced by 2 to 600 x 400 px, about
// 0.9 mm of paper per pixel, which is what a 640 x 424 live view of a frame that covers about 600 mm gives. C's grid is the
// condition's region at the pitch of that frame (about 1.06 mm per pixel, 727 x 321 pixels). Noise of 2 DN is added to every frame.
//
// The target. C is a guide drawn at its own resolution, and B's table is baked again only when the mapping moves by more than
// tau_bake = 0.5 px of C. A seam that is off by less than half a pixel of C is not visible in C, so the target of a full estimate
// is: the largest error along the seam column below 0.5 px of C (about 0.5 mm of paper). It says nothing about the capture, whose
// estimate is made again on the captured images.
#include "a0/phase0/experimental/live_registration_runtime.hpp"
#include "parallax_scene.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
namespace ex=a0::m2::experimental;
namespace render=a0::m2::render;
namespace cr=a0::m2::experimental::content_registration;
namespace ps=parallax_scene;
using Pose=cr::PoseParameters;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
bool Has(const std::vector<std::string>& codes,std::string_view code){return std::find(codes.begin(),codes.end(),std::string(code))!=codes.end();}
std::string Join(const std::vector<std::string>& codes){std::string s;for(const auto& c:codes)s+=(s.empty()?"":",")+c;return s;}

double WallSeconds(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
// Processor time of this process (kernel and user), seconds.
double CpuSeconds(){
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return 0;
    const auto ticks=[](const FILETIME& t){return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return static_cast<double>(ticks(kernel)+ticks(user))*1e-7;
}

// ---- Live frames ------------------------------------------------------------------------------------------------------------------
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
render::BgrImage Flat(unsigned char value){
    return render::BgrImage{kViewWidth,kViewHeight,std::vector<std::uint8_t>(static_cast<size_t>(kViewWidth)*kViewHeight*3,value)};
}
render::BgrImage Noise(unsigned char mean,double sigma,std::uint64_t seed){auto image=Flat(mean);AddNoise(image,seed,sigma);return image;}
// Noise with the blocks of a compressed frame: every 8 x 8 block has a level of its own.
render::BgrImage BlockNoise(unsigned char mean,double block_sigma,double sigma,std::uint64_t seed){
    auto image=Flat(mean);
    ps::Lcg r{seed*77+5};
    for(unsigned by=0;by<kViewHeight;by+=8)for(unsigned bx=0;bx<kViewWidth;bx+=8){
        const double level=(r.Uniform()+r.Uniform()+r.Uniform()+r.Uniform()-2.0)*std::sqrt(3.0)*block_sigma;
        for(unsigned y=by;y<std::min(by+8,kViewHeight);++y)for(unsigned x=bx;x<std::min(bx+8,kViewWidth);++x)
            for(int c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*kViewWidth+x)*3+c]=static_cast<std::uint8_t>(std::clamp(std::lround(mean+level),0L,255L));
    }
    AddNoise(image,seed+1,sigma);
    return image;
}
// The frame turned by 180 degrees (what OrientRenderInput does to the frames of a rig mounted upside down).
render::BgrImage Turned(const render::BgrImage& image){
    auto out=image;
    const size_t n=static_cast<size_t>(image.width)*image.height;
    for(size_t i=0;i<n;++i)for(int c=0;c<3;++c)out.bgr[i*3+c]=image.bgr[(n-1-i)*3+c];
    return out;
}

// ---- The rig of the live C ----------------------------------------------------------------------------------------------------------
// The limits of the live view (registration.live.limits, pixels of C) that these tests run with: development values, a proposal.
cr::RegistrationLimits LiveLimits(){
    cr::RegistrationLimits l;
    l.brown_lowe_alpha=8;l.brown_lowe_beta=0.3;l.minimum_unique_blocks=3;
    l.maximum_prediction_sigma_px=0.25;l.maximum_residual_rms_px=0.3;l.maximum_residual_p95_px=0.6;l.maximum_bin_bias_px=0.25;
    l.convergence_px=0.05;l.start_agreement_px=0.25;
    return l;
}
// The condition's ranges (examples for the synthetic rig, not product values). The geometry is in the pixels of the synthetic
// capture raster, chosen so that C's values are of the size a capture geometry has through a 640 px live view: guard 7 px
// (widened to clear half a patch), band half width 89 px.
LiveRegistrationRanges Ranges(bool surface=true){
    LiveRegistrationRanges r;
    r.envelope.position_mm=10;r.envelope.rotation_deg=2;r.envelope.tilt_deg=1;r.envelope.height_percent=1;
    if(surface)r.surface_envelope=ex::SurfaceEnvelope{40,8};
    r.omega_margin_px=4;r.guard_px=14;r.band_half_width_px=178;
    r.apply_beyond_envelope=true;
    return r;
}

struct LiveRig {
    ps::Rig rig{ps::StandardRig()};
    ps::SeamPlacement seam{ps::PlaceSeam(rig)};
    bool turned{};
    Pose base;
    std::shared_ptr<LiveRegistrationContext> context;
    FixedPreviewPlan plan;
    explicit LiveRig(bool turned_mount=false,const Pose& base_pose={}):turned(turned_mount),base(base_pose){
        const auto parameters=rig.Parameters();
        const auto grid=MakeFixedPreviewGrid(rig.raster,kScale);
        const ex::RegisteredMapping fixed{base,{},std::nullopt};
        context=std::make_shared<LiveRegistrationContext>(parameters,rig.raster,seam.column,grid,kScale,kScale,kViewWidth,kViewHeight,turned,fixed);
        context->live_limits=LiveLimits();
        context->table_a=BakeOutputToCamera(LiveViewOutputToCamera(parameters.camera_a,grid,kScale,kScale,kViewWidth,kViewHeight,turned),grid.width,grid.height);
        context->table_b=BakeOutputToCamera(LiveViewRegisteredOutputToCamera(*context,fixed),grid.width,grid.height);
        plan.lens_aware=true;plan.view_width=kViewWidth;plan.view_height=kViewHeight;plan.scale_x=plan.scale_y=kScale;
        plan.grid=grid;plan.output_width=grid.width;plan.output_height=grid.height;
        plan.capture_seam_column=seam.column;
        plan.scaled_seam_column=static_cast<std::uint32_t>(std::floor((seam.column+0.5)*kScale));
        plan.view.seam_column=plan.scaled_seam_column;
        // The blend of a C seen through a 640 px live view: one pixel wide.
        plan.settings=ScaleSeamBlendSettings(ex::kSeamBlendDesign,640.0/7360.0,640.0/7360.0);
        plan.mapped=std::make_shared<const ex::MappedFixedSeamInput>(ex::MappedFixedSeamInput{grid.width,grid.height,
            TableOutputToCamera(context->table_a,grid.width,grid.height),TableOutputToCamera(context->table_b,grid.width,grid.height)});
        plan.live=context;
    }
};

// ---- Scenes at the live view's coarseness -------------------------------------------------------------------------------------------
struct LiveScene {
    std::shared_ptr<ps::SceneModel> model;
    render::BgrImage a,b;   // the oriented, reduced frames
};
// The reduced frame of camera A is the same for every pose of B: it is rendered once per pattern and height field.
std::map<std::string,render::BgrImage>& ReducedA(){static std::map<std::string,render::BgrImage> cache;return cache;}
LiveScene MakeLiveScene(const LiveRig& live,const std::string& paper_key,const ps::PatternSpec& pattern,const ps::HeightField& field,const Pose& b_pose,
    std::uint64_t noise_seed,double noise_sigma=kNoiseSigma){
    ps::SceneSpec spec;
    spec.name=paper_key;spec.pattern=pattern;spec.field=field;spec.b_pose=b_pose;
    LiveScene scene;
    scene.model=std::make_shared<ps::SceneModel>(live.rig,live.seam,spec);
    auto& cache=ReducedA();
    auto found=cache.find(paper_key);
    if(found==cache.end())found=cache.emplace(paper_key,Reduce(ps::RenderRaw(*scene.model,ps::Camera::a))).first;
    scene.a=found->second;
    scene.b=Reduce(ps::RenderRaw(*scene.model,ps::Camera::b));
    AddNoise(scene.a,noise_seed*2+11,noise_sigma);AddNoise(scene.b,noise_seed*2+12,noise_sigma);
    if(live.turned){scene.a=Turned(scene.a);scene.b=Turned(scene.b);}
    return scene;
}

// How far off C's seam is when B is drawn with `mapping`: along C's seam column, the distance (pixels of C) between where the
// mapping puts B's document point and where it has to be for B to show the paper point that A shows there. The truth is the
// scene's own geometry (the paper point on A's ray, the moved camera B that sees it), not an estimate.
struct SeamError {double maximum{},rms{};int rows{};};
SeamError SeamErrorOf(const LiveRig& live,const ps::SceneModel& model,const ex::RegisteredMapping& mapping,int column_offset=0){
    const auto h=LiveDocumentHomography(*live.context,mapping);
    const auto& grid=live.plan.grid;
    SeamError error;
    double squares=0;
    const double x=static_cast<double>(live.plan.view.seam_column)+column_offset;
    for(std::uint32_t y=0;y<grid.height;y+=4){
        const render::Point document{grid.left_mm+(x+0.5)*grid.pitch_mm,grid.top_mm+(y+0.5)*grid.pitch_mm};
        render::Point raw_a{},raw_b{},wanted{};
        if(!render::TryProjectDocument(model.rig.camera_a,document,raw_a) || !ps::InsideFrame(raw_a))continue;
        const auto surface=ps::SolveSurface(model.spec.field,model.geometry_a,document);
        if(!render::TryProjectDocument(model.camera_b_true,ps::TablePointOfSurface(model.geometry_b_true,surface),raw_b) || !ps::InsideFrame(raw_b))continue;
        if(!model.b_calibrated_to_table(raw_b.x,raw_b.y,wanted))continue;
        render::Point placed=document;
        if(h && !render::TryTransform(*h,document,placed))continue;
        const double e=std::hypot(wanted.x-placed.x,wanted.y-placed.y)/grid.pitch_mm;
        error.maximum=std::max(error.maximum,e);squares+=e*e;++error.rows;
    }
    if(error.rows)error.rms=std::sqrt(squares/error.rows);
    return error;
}

// ---- The worker with the connected estimate ----------------------------------------------------------------------------------------
struct Session {
    LiveRig& live;
    double now{0};
    render::BgrImage a,b;
    std::vector<std::string> log;
    std::unique_ptr<LiveRegistrationRuntime> runtime;
    LiveFrameSource frames;
    LiveEstimator estimator;
    explicit Session(LiveRig& rig,const LiveRegistrationRanges& ranges=Ranges(),const LiveEstimatorDesign& design=kLiveEstimatorDesignDraft,
        const LiveTrackerDesign& tracker_design=kLiveTrackerDesignDraft):live(rig){
        LiveRegistrationRuntime::Config config;
        config.design=tracker_design;
        config.base_pose=live.base;config.live_enabled=true;
        config.mount=live.turned?ex::MountOrientation::rotated_180:ex::MountOrientation::normal;
        config.clock=[this]{return now;};
        config.log=[this](const std::string& line){log.push_back(line);};
        runtime=std::make_unique<LiveRegistrationRuntime>(config,live.rig.Parameters());
        runtime->PublishBase(live.plan);
        frames.ready=[]{return true;};
        frames.snapshot=[this](render::BgrImage& x,render::BgrImage& y){x=a;y=b;return true;};
        estimator=MakeLiveEstimator([this]{return runtime->BasePlan();},runtime->Design(),
            [design](const LiveEstimatorInputs& in){return EstimateLiveRegistration(in,design);},ranges);
    }
    void Show(const LiveScene& scene){a=scene.a;b=scene.b;}
    LivePassResult Pass(double at){now=at;return runtime->RunPass(frames,estimator);}
    std::string LogText() const {std::string all;for(const auto& line:log)all+=line+"\n";return all;}
};

void Report(const char* name,const SeamError& before,const SeamError& after,const LiveRegistrationView& view){
    const auto& p=view.mapping.pose;const auto& s=view.mapping.surface;
    std::cerr<<std::fixed<<std::setprecision(3)<<"info "<<name<<": seam error before max "<<before.maximum<<" rms "<<before.rms<<" px, after max "<<after.maximum
        <<" rms "<<after.rms<<" px ("<<after.rows<<" rows); pose rx "<<p.rx_deg<<" ry "<<p.ry_deg<<" rz "<<p.rz_deg<<" tx "<<p.tx_mm<<" ty "<<p.ty_mm
        <<" tz "<<p.tz_percent<<"; surface h0 "<<s.h0_mm<<" gx "<<s.slope_x<<" gy "<<s.slope_y<<'\n';
}

constexpr double kSeamTargetPx=0.5;

const ps::PatternSpec kWaves{};

// ---- 1. A full estimate puts the seam of C right when camera B has moved --------------------------------------------------------------
void FullEstimates(){
    LiveRig live;
    // moved_px: how far the scene has to move the seam for the case to mean something. A tilt of the lens turns about the point
    // that the lens looks at, so it moves the seam far less than a translation or a turn does.
    struct Case {const char* name;Pose b;double h0_mm;double moved_px;};
    const Case cases[]{
        {"baseline +4 mm",{0,0,0,4.0,0,0},0,1.0},
        {"baseline -5 mm",{0,0,0,-5.0,0,0},0,1.0},
        {"across the baseline 3 mm",{0,0,0,0,3.0,0},0,1.0},
        {"rotation 0.8 deg",{0,0,0.8,0,0,0},0,1.0},
        {"tilt",{0.3,-0.2,0,0,0,0},0,0.3},
        {"tilt 0.8 deg",{0.8,0.5,0,0,0,0},0,0.6},
        {"mixed, paper 5 mm up",{0.2,0.1,-0.6,3.0,-2.0,0.4},5.0,1.0},
    };
    std::uint64_t seed=100;
    for(const auto& c:cases){
        ps::HeightField field;
        if(c.h0_mm!=0)field.plane=ps::PlaneOver(live.seam,c.h0_mm,0,0);
        const auto scene=MakeLiveScene(live,c.h0_mm!=0?"waves-up":"waves",kWaves,field,c.b,++seed);
        Session session(live);
        session.Show(scene);
        const auto before=SeamErrorOf(live,*scene.model,ex::RegisteredMapping{});
        const auto pass=session.Pass(0);
        const auto status=session.runtime->Status();
        const auto after=SeamErrorOf(live,*scene.model,status.view.mapping);
        Report(c.name,before,after,status.view);
        Check(pass.job_ran && pass.kind==LiveJobKind::full && pass.accept==LiveAcceptKind::applied,(std::string(c.name)+": the full estimate is taken ("+session.LogText()+")").c_str());
        Check(status.view.state==LiveRegistrationState::tracking && Contains(status.state_text,"目安"),(std::string(c.name)+": C says it is a guide").c_str());
        // The line of the log carries what the estimate measured, for whoever sets the limits of the live view on a rig.
        Check(Contains(session.LogText(),"job=full") && Contains(session.LogText(),"estimate_state=") && Contains(session.LogText(),"sigma_px=") &&
            Contains(session.LogText(),"rms_px=") && Contains(session.LogText(),"bias_px="),(std::string(c.name)+": the log line has the measured values").c_str());
        // ... and the estimate itself (the six components and the paper), the components outside the range, whether it replaced the estimate in force
        // and what C is drawn with.
        Check(Contains(session.LogText(),"est_rx_deg=") && Contains(session.LogText(),"est_ry_deg=") && Contains(session.LogText(),"est_rz_deg=") &&
            Contains(session.LogText(),"est_tx_mm=") && Contains(session.LogText(),"est_ty_mm=") && Contains(session.LogText(),"est_tz_percent=") &&
            Contains(session.LogText(),"est_h0_mm=") && Contains(session.LogText(),"est_slope_x=") && Contains(session.LogText(),"est_slope_y=") &&
            Contains(session.LogText(),"exceeded=") && Contains(session.LogText(),"replaced=1 seam_diff_px=") && Contains(session.LogText(),"adopt_line_px=") &&
            Contains(session.LogText(),"drawn_rx_deg=") && Contains(session.LogText(),"drawn_h0_mm=") && Contains(session.LogText(),"drawn_slope_y="),
            (std::string(c.name)+": the log line has the estimate, the range and what C is drawn with").c_str());
        Check(before.rows>=40 && before.maximum>c.moved_px,(std::string(c.name)+": the scene moves the seam").c_str());
        // The lean of B's lens is the scene's to within a quarter of a degree (the band sees it far less well than the turn).
        Check(std::abs(status.view.mapping.pose.rx_deg-c.b.rx_deg)<=0.25 && std::abs(status.view.mapping.pose.ry_deg-c.b.ry_deg)<=0.25,
            (std::string(c.name)+": B's tilt is the scene's within 0.25 degrees").c_str());
        Check(after.maximum<=kSeamTargetPx,(std::string(c.name)+": the seam is within the target after the estimate").c_str());
        // The table that C draws with is the estimate's.
        Check(pass.rebaked && session.runtime->CurrentPlan() && session.runtime->CurrentPlan()->live->baked.pose.rz_deg==status.view.mapping.pose.rz_deg,
            (std::string(c.name)+": B's table is baked for the estimate").c_str());
        // The neighbourhood of the seam (the columns that the blend of C can show), not only the column itself.
        Check(SeamErrorOf(live,*scene.model,status.view.mapping,-8).maximum<=kSeamTargetPx && SeamErrorOf(live,*scene.model,status.view.mapping,8).maximum<=kSeamTargetPx,
            (std::string(c.name)+": eight columns either side of the seam are within the target").c_str());
        // B's direction is shown, and its turn is the scene's (the turn about the axis is what the band sees best).
        Check(!Contains(status.orientation.b_short,"測れていません") && Contains(status.orientation_line,"B: "),(std::string(c.name)+": B's direction is shown").c_str());
        Check(std::abs(status.view.mapping.pose.rz_deg-c.b.rz_deg)<=0.15,(std::string(c.name)+": B's turn is the scene's within 0.15 degrees").c_str());
    }
}

// ---- 2. The follow-up keeps up with a slow drift, a jump asks for a full estimate, and a still scene is not baked again and again ----
void FollowsAndJumps(){
    LiveRig live;
    Session session(live);
    const Pose start{0,0,0.3,2.0,1.0,0};
    std::uint64_t seed=200;
    auto scene=MakeLiveScene(live,"waves",kWaves,{},start,++seed);
    session.Show(scene);
    double t=0;
    CHECK(session.Pass(t).accept==LiveAcceptKind::applied);
    CHECK(SeamErrorOf(live,*scene.model,session.runtime->Status().view.mapping).maximum<=kSeamTargetPx);
    // A slow drift: each step moves the seam by about a fifth of a pixel of C. A follow-up that finds less than its limit (0.5 px) confirms
    // the estimate in force and changes nothing on screen; once the drift is over the limit the follow-up detects a change, the full estimate
    // that it asks for runs at once and is taken. So the seam is off by the limit and a step at the most, and within the target at the end.
    Pose pose=start;
    double worst=0;
    int confirmed=0,corrected=0;
    for(int k=0;k<5;++k){
        pose.tx_mm+=0.15;pose.rz_deg+=0.01;pose.ty_mm-=0.1;
        scene=MakeLiveScene(live,"waves",kWaves,{},pose,++seed);
        session.Show(scene);
        t+=0.6;
        auto pass=session.Pass(t);
        Check(pass.job_ran && pass.kind==LiveJobKind::follow,("drift step: the follow-up runs ("+session.LogText()+")").c_str());
        if(pass.accept==LiveAcceptKind::applied)++confirmed;
        else if(session.runtime->Status().view.full_pending){
            pass=session.Pass(t);
            Check(pass.job_ran && pass.kind==LiveJobKind::full && pass.accept==LiveAcceptKind::applied,("drift step: the full estimate is taken ("+session.LogText()+")").c_str());
            ++corrected;
        }
        const auto view=session.runtime->Status().view;
        const auto error=SeamErrorOf(live,*scene.model,view.mapping);
        worst=std::max(worst,error.maximum);
        CHECK(view.state==LiveRegistrationState::tracking && error.maximum<=kSeamTargetPx+0.25);
    }
    std::cerr<<std::fixed<<std::setprecision(3)<<"info drift: largest seam error over 5 steps "<<worst<<" px ("<<confirmed<<" follow-ups confirmed the estimate, "<<corrected<<" changes corrected by a full estimate)\n";
    CHECK(confirmed>=1 && corrected>=1 && SeamErrorOf(live,*scene.model,session.runtime->Status().view.mapping).maximum<=kSeamTargetPx);
    const auto follows_before=session.runtime->Status().view.counters.follow_jobs;
    // A jump: the follow-up is not taken (what it found is beyond its line, or it found nothing), the estimate before stays on
    // screen, and the next job is a full estimate, which is taken.
    const Pose kept=session.runtime->Status().view.mapping.pose;
    pose.tx_mm+=5.0;pose.rz_deg-=0.7;
    scene=MakeLiveScene(live,"waves",kWaves,{},pose,++seed);
    session.Show(scene);
    t+=0.6;
    const auto jumped=session.Pass(t);
    {
        const auto view=session.runtime->Status().view;
        Check(jumped.job_ran && jumped.kind==LiveJobKind::follow && jumped.accept==LiveAcceptKind::kept_previous,("jump: the follow-up is not taken ("+session.LogText()+")").c_str());
        CHECK(view.full_pending && view.full_reason==LiveFullReason::change_detected && view.state==LiveRegistrationState::holding);
        CHECK(view.mapping.pose.tx_mm==kept.tx_mm && view.mapping.pose.rz_deg==kept.rz_deg);
        CHECK(Contains(session.runtime->Status().state_text,"直前の推定"));
        std::cerr<<"info jump: the follow-up reported "<<Join(view.last_failure)<<'\n';
    }
    const auto full=session.Pass(t);   // at once
    {
        const auto view=session.runtime->Status().view;
        const auto error=SeamErrorOf(live,*scene.model,view.mapping);
        Check(full.job_ran && full.kind==LiveJobKind::full && full.accept==LiveAcceptKind::applied,("jump: the full estimate is taken ("+session.LogText()+")").c_str());
        CHECK(view.state==LiveRegistrationState::tracking && error.maximum<=kSeamTargetPx);
        CHECK(view.counters.follow_jobs==follows_before+1);
        std::cerr<<std::fixed<<std::setprecision(3)<<"info jump: seam error after the full estimate "<<error.maximum<<" px\n";
    }
    // A still scene (the same paper and pose, new noise in every frame): the follow-ups are taken, and the scatter of the estimate
    // does not bake B's table again and again.
    const auto rebakes_before=session.runtime->Status().rebakes;
    double still_worst=0,largest_step=0;
    ex::RegisteredMapping previous=session.runtime->Status().view.mapping;
    constexpr int kStill=10;
    for(int k=0;k<kStill;++k){
        auto again=scene;
        again.a=ReducedA().at("waves");AddNoise(again.a,7000+k,kNoiseSigma);
        again.b=Reduce(ps::RenderRaw(*scene.model,ps::Camera::b));AddNoise(again.b,8000+k,kNoiseSigma);
        session.Show(again);
        t+=0.6;
        const auto pass=session.Pass(t);
        const auto view=session.runtime->Status().view;
        CHECK(pass.job_ran && pass.kind==LiveJobKind::follow && pass.accept==LiveAcceptKind::applied);
        still_worst=std::max(still_worst,SeamErrorOf(live,*scene.model,view.mapping).maximum);
        largest_step=std::max(largest_step,LiveMappingDifferencePx(live.plan,previous,view.mapping,9));
        previous=view.mapping;
    }
    const auto still_rebakes=session.runtime->Status().rebakes-rebakes_before;
    std::cerr<<std::fixed<<std::setprecision(3)<<"info still scene: "<<kStill<<" follow-ups, "<<still_rebakes<<" rebakes, largest seam error "<<still_worst
        <<" px, largest move of the mapping between two follow-ups over C "<<largest_step<<" px\n";
    CHECK(still_worst<=kSeamTargetPx);
    CHECK(still_rebakes<=2);
}

// ---- 2b. A still scene with the scatter of a real rig: C is not drawn again and again, and a real move is followed -------------------------
// The symptom on a rig, with nothing moved: C baked again after nearly every follow-up, each time with the mapping several pixels away from
// the one before over the whole of C, while the follow-up reported a fraction of a pixel at the seam. The seam constrains some directions
// of the estimate (the baseline, the turn) and hardly the others (the lean of B, its height), so the scatter of the estimate is small at
// the seam and several times larger at the ends of C.
//
// Two scenes. The first is a stand-in for the estimator that answers the truth with a scatter of that kind (the draws are made in the poorly
// constrained directions, so the seam moves a fraction of a pixel and the ends several pixels); it runs the real tracker, difference
// functions and bakes, and takes no time. The second runs the estimator itself on synthetic frames that are flat (contrast 0.3) and noisy
// (6 DN), the most that the estimator accepts before it refuses the frame: its scatter is smaller than a rig's, and it shows the same effect
// at that size. ※The prediction sigma of these frames stays far below a rig's; the sizes are not those of a measurement.
constexpr double kFlatContrast=0.3,kFlatNoise=6.0;   // the synthetic frames of the second scene
constexpr double kMoveNoise=10.0;                    // DN, the frames of the move
// The condition's limits with the line for the prediction sigma relaxed to 0.6 px, as a rig needs; the rest are the development values.
cr::RegistrationLimits RigLimits(){auto l=LiveLimits();l.maximum_prediction_sigma_px=0.6;return l;}
// Times of the jobs on an idle four-core machine (the full estimate, the follow-up and the bake of B's table), in seconds; the clock of the
// session advances by them, so that the time to recover from a move includes the work that follows it. ※assumption: the figures of the
// estimate's report for a machine that is not busy.
constexpr double kFullSeconds=0.45,kFollowSeconds=0.1,kBakeSeconds=0.25;

// The contrast of a frame about its own mean: a rig's frames are dim and flat next to the synthetic ones.
void Dim(render::BgrImage& image,double contrast){
    double sum=0;
    for(const auto v:image.bgr)sum+=v;
    const double mean=sum/static_cast<double>(image.bgr.size());
    for(auto& v:image.bgr)v=static_cast<std::uint8_t>(std::clamp(std::lround(mean+contrast*(v-mean)),0L,255L));
}
struct Clean {LiveScene scene;render::BgrImage a,b;};
Clean MakeClean(const LiveRig& live,const Pose& pose,std::uint64_t seed,double contrast=1.0){
    Clean c;
    c.scene=MakeLiveScene(live,"waves",kWaves,{},pose,seed,0.0);
    c.a=c.scene.a;c.b=c.scene.b;
    if(contrast!=1.0){Dim(c.a,contrast);Dim(c.b,contrast);}
    return c;
}
void ShowNoisy(Session& session,const Clean& clean,std::uint64_t seed,double sigma){
    session.a=clean.a;session.b=clean.b;
    AddNoise(session.a,seed*2+1,sigma);AddNoise(session.b,seed*2+2,sigma);
}
// One pass of the worker with the cost of the jobs on the session's clock.
LivePassResult TimedPass(Session& session,const LiveEstimator& estimator){
    const auto result=session.runtime->RunPass(session.frames,estimator);
    if(result.job_ran)session.now+=result.kind==LiveJobKind::full?kFullSeconds:kFollowSeconds;
    if(result.rebaked)session.now+=kBakeSeconds;
    if(!result.job_ran && !result.rebaked)session.now+=0.05;
    return result;
}
LivePassResult TimedPass(Session& session){return TimedPass(session,session.estimator);}
LiveTrackerDesign LegacyTracker(){auto d=kLiveTrackerDesignDraft;d.tracker.adopt_threshold_px=0;d.tracker.adopt_sigma_factor=0;return d;}

struct SteadyResult {
    int follows{},fulls{},rebakes{},over_limit{},held{},summaries{};
    double worst_error{0},largest_step{0},largest_change{0};
    LiveRegistrationState state{};
};
SteadyResult Summarise(Session& session,const LiveTrackerCounters& before,std::uint64_t rebakes_before,LiveRegistrationState state){
    const auto status=session.runtime->Status();
    SteadyResult out;
    out.rebakes=static_cast<int>(status.rebakes-rebakes_before);
    out.follows=static_cast<int>(status.view.counters.follow_jobs-before.follow_jobs);
    out.fulls=static_cast<int>(status.view.counters.full_jobs-before.full_jobs);
    out.over_limit=static_cast<int>(status.view.counters.changes_detected-before.changes_detected);
    out.held=static_cast<int>(status.view.counters.held-before.held);
    out.state=state;
    return out;
}
void ReportSteady(const char* name,const SteadyResult& r,double seconds){
    std::cerr<<std::fixed<<std::setprecision(3)<<"info "<<name<<": "<<seconds<<" s of a still scene, "<<r.follows<<" follow-ups and "<<r.fulls<<" full estimates ("<<r.over_limit
        <<" changes detected, "<<r.held<<" estimates taken as confirmations), "<<r.rebakes<<" rebakes, largest move of C between two passes "<<r.largest_step
        <<" px, largest change a follow-up reported "<<r.largest_change<<" px, worst seam error "<<r.worst_error<<" px\n";
}

// ---- the first scene: the truth and a scatter ----
struct Scatter {double tilt_deg,height_percent,shift_mm,turn_deg,sigma_px;};
// The scatter of a rig, ※assumption: sizes found so that the distance at the seam is a fraction of a pixel and C moves by several pixels over the whole.
constexpr Scatter kRigScatter{0.05,0.1,0.016,0.004,0.3};
LiveEstimator ScatteredEstimator(const LiveRig& live,const Pose& truth,const Scatter& scatter,std::uint64_t seed){
    auto draws=std::make_shared<ps::Lcg>(ps::Lcg{seed*0x9E3779B97F4A7C15ull+3});
    return [&live,truth,scatter,draws](const LiveJob& job,const render::BgrImage&,const render::BgrImage&){
        const auto g=[&]{return (draws->Uniform()+draws->Uniform()+draws->Uniform()+draws->Uniform()-2.0)*std::sqrt(3.0);};
        LiveEstimateOutcome o;
        o.usable=true;o.pose=truth;
        o.pose.rx_deg+=g()*scatter.tilt_deg;o.pose.ry_deg+=g()*scatter.tilt_deg;o.pose.tz_percent+=g()*scatter.height_percent;
        o.pose.tx_mm+=g()*scatter.shift_mm;o.pose.ty_mm+=g()*scatter.shift_mm;o.pose.rz_deg+=g()*scatter.turn_deg;
        o.band_brightness=100;o.prediction_sigma_px=scatter.sigma_px;
        if(job.kind==LiveJobKind::follow){
            o.change_px=LiveSeamMappingDifferencePx(live.plan,ex::RegisteredMapping{job.start_pose,job.start_surface,std::nullopt},
                ex::RegisteredMapping{o.pose,o.surface,std::nullopt},kLiveSeamDifferenceRows);
            o.inlier_fraction=0.9;
        }
        return o;
    };
}
SteadyResult RunScattered(LiveRig& live,const LiveTrackerDesign& tracker,const Pose& truth,const Scatter& scatter,double seconds,std::uint64_t seed){
    Session session(live,Ranges(),kLiveEstimatorDesignDraft,tracker);
    session.a=Flat(128);session.b=Flat(128);
    const auto estimator=ScatteredEstimator(live,truth,scatter,seed);
    for(int k=0;k<6 && session.runtime->Status().view.state!=LiveRegistrationState::tracking;++k)(void)TimedPass(session,estimator);
    const double begin=session.now;
    const auto before=session.runtime->Status().view.counters;
    const auto rebakes0=session.runtime->Status().rebakes;
    const ex::RegisteredMapping truth_mapping{truth,{},std::nullopt};
    ex::RegisteredMapping previous=session.runtime->Status().view.mapping;
    double worst=0,step=0;
    while(session.now-begin<seconds){
        (void)TimedPass(session,estimator);
        const auto view=session.runtime->Status().view;
        worst=std::max(worst,LiveSeamMappingDifferencePx(live.plan,truth_mapping,view.mapping,kLiveSeamDifferenceRows));
        step=std::max(step,LiveMappingDifferencePx(live.plan,previous,view.mapping,9));
        previous=view.mapping;
    }
    auto out=Summarise(session,before,rebakes0,session.runtime->Status().view.state);
    out.worst_error=worst;out.largest_step=step;
    for(const auto& line:session.log)if(Contains(line,"C live_registration_summary ") && Contains(line,"held=") && Contains(line,"rebakes=") && Contains(line,"largest_follow_change_px="))++out.summaries;
    return out;
}
void SteadyUnderRigScatter(){
    LiveRig live;
    live.context->live_limits=RigLimits();
    const Pose truth{0.1,-0.1,0.4,2.0,-1.0,0};
    // The size of the scatter, as the first estimate measures it: from the draws themselves.
    {
        const auto estimator=ScatteredEstimator(live,truth,kRigScatter,5);
        LiveJob job;job.kind=LiveJobKind::follow;job.start_pose=truth;
        double seam=0,whole=0,seam_sum=0;
        const int n=200;
        for(int k=0;k<n;++k){
            const auto o=estimator(job,Flat(0),Flat(0));
            seam=std::max(seam,o.change_px);seam_sum+=o.change_px;
            whole=std::max(whole,LiveMappingDifferencePx(live.plan,ex::RegisteredMapping{truth,{},std::nullopt},ex::RegisteredMapping{o.pose,o.surface,std::nullopt},9));
        }
        std::cerr<<std::fixed<<std::setprecision(3)<<"info the scatter of the draws: "<<n<<" draws, distance from the truth at the seam mean "<<seam_sum/n<<" max "<<seam
            <<" px, over the whole of C max "<<whole<<" px\n";
        Check(seam<=0.6 && seam_sum/n>=0.1 && whole>=3*kLiveTrackerDesignDraft.tracker.bake_threshold_px,"the draws scatter at the seam by under a pixel and over the whole of C by more than the line of the bake");
    }
    const auto legacy=RunScattered(live,LegacyTracker(),truth,kRigScatter,60,1);
    const auto steady=RunScattered(live,kLiveTrackerDesignDraft,truth,kRigScatter,60,1);
    ReportSteady("scatter, every estimate replaces the one in force",legacy,60);
    ReportSteady("scatter, the significance line",steady,60);
    // (a) Without the line C is baked again for most follow-ups; with it, a handful of times at most, and when it is it moves by a step of
    // the size of a real move.
    Check(legacy.rebakes>=20,"without the line the scatter bakes B's table again and again");
    Check(steady.rebakes<=3,"with the line a still scene is baked at most a few times");
    Check(steady.worst_error<=kSeamTargetPx,"the seam stays within the target");
    Check(steady.state==LiveRegistrationState::tracking && steady.follows>=40,"and C stays on the estimate");
    // The log carries a summary of the still scene every 30 s, since the job line is written only when something differs from the line before.
    Check(steady.summaries>=2 && steady.summaries<=4,"the log has a summary line every 30 seconds");
}

// ---- the second scene: the estimator on flat, noisy frames ----
SteadyResult RunFlatFrames(LiveRig& live,const LiveTrackerDesign& tracker,const Clean& clean,double seconds,std::uint64_t seed){
    Session session(live,Ranges(),kLiveEstimatorDesignDraft,tracker);
    ShowNoisy(session,clean,seed,kFlatNoise);
    for(int k=0;k<6 && session.runtime->Status().view.state!=LiveRegistrationState::tracking;++k){ShowNoisy(session,clean,seed+1+k,kFlatNoise);(void)TimedPass(session);}
    const double begin=session.now;
    const auto before=session.runtime->Status().view.counters;
    const auto rebakes0=session.runtime->Status().rebakes;
    ex::RegisteredMapping previous=session.runtime->Status().view.mapping;
    double worst=0,step=0;
    for(int pass=0;session.now-begin<seconds;++pass){
        ShowNoisy(session,clean,seed+100+pass,kFlatNoise);
        (void)TimedPass(session);
        const auto view=session.runtime->Status().view;
        worst=std::max(worst,SeamErrorOf(live,*clean.scene.model,view.mapping).maximum);
        step=std::max(step,LiveMappingDifferencePx(live.plan,previous,view.mapping,9));
        previous=view.mapping;
    }
    auto out=Summarise(session,before,rebakes0,session.runtime->Status().view.state);
    out.worst_error=worst;out.largest_step=step;
    for(const auto& line:session.log){
        const auto at=line.find("change_px=");
        if(at!=std::string::npos && line.find("job=follow")!=std::string::npos)out.largest_change=std::max(out.largest_change,std::atof(line.c_str()+at+10));
    }
    return out;
}
void SteadyOnFlatFrames(){
    LiveRig live;
    live.context->live_limits=RigLimits();
    const Pose pose{0.1,-0.1,0.4,2.0,-1.0,0};
    const auto clean=MakeClean(live,pose,900,kFlatContrast);
    const auto legacy=RunFlatFrames(live,LegacyTracker(),clean,30,1000);
    const auto steady=RunFlatFrames(live,kLiveTrackerDesignDraft,clean,30,1000);
    ReportSteady("flat noisy frames, every estimate replaces the one in force",legacy,30);
    ReportSteady("flat noisy frames, the significance line",steady,30);
    Check(legacy.rebakes>=10,"flat frames, without the line: the scatter bakes B's table again and again");
    Check(steady.rebakes<=3,"flat frames, with the line: a still scene is baked at most a few times");
    Check(steady.worst_error<=kSeamTargetPx,"flat frames: the seam stays within the target");
    Check(steady.state==LiveRegistrationState::tracking && steady.follows>=40,"flat frames: C stays on the estimate");
}

// A move of camera B while the scene is otherwise still: the seam of C is within the target again within kRecoverSeconds of the move.
constexpr double kRecoverSeconds=2.0;
void FollowsAMoveOfB(){
    LiveRig live;
    live.context->live_limits=RigLimits();
    const Pose start{0.1,-0.1,0.4,2.0,-1.0,0};
    struct Move {const char* name;Pose to;};
    const Move moves[]{{"B moved 2 mm along the baseline",{0.1,-0.1,0.4,4.0,-1.0,0}},{"B turned 0.3 degrees",{0.1,-0.1,0.7,2.0,-1.0,0}},
        {"B moved 2 mm and turned 0.3 degrees",{0.1,-0.1,0.7,0.0,-1.0,0}}};
    const auto before=MakeClean(live,start,910);
    std::uint64_t seed=2000;
    for(const auto& move:moves){
        const auto after=MakeClean(live,move.to,911);
        Session session(live);
        ShowNoisy(session,before,seed,kMoveNoise);
        for(int k=0;k<8 && session.runtime->Status().view.state!=LiveRegistrationState::tracking;++k){ShowNoisy(session,before,++seed,kMoveNoise);(void)TimedPass(session);}
        Check(session.runtime->Status().view.state==LiveRegistrationState::tracking,(std::string(move.name)+": the first estimate is taken").c_str());
        // Still for 8 s, then the move.
        const double settle=session.now+8;
        while(session.now<settle){ShowNoisy(session,before,++seed,kMoveNoise);(void)TimedPass(session);}
        const auto drawn=[&]{const auto plan=session.runtime->CurrentPlan();return plan?plan->live->baked:session.runtime->BasePlan()->live->baked;};
        const double error_before_move=SeamErrorOf(live,*before.scene.model,drawn()).maximum;
        const double moved_px=SeamErrorOf(live,*after.scene.model,drawn()).maximum;
        const double move_time=session.now;
        double recovered=-1;
        int passes=0;
        while(session.now-move_time<6 && recovered<0){
            ShowNoisy(session,after,++seed,kMoveNoise);
            (void)TimedPass(session);++passes;
            if(SeamErrorOf(live,*after.scene.model,drawn()).maximum<=kSeamTargetPx)recovered=session.now-move_time;
        }
        std::cerr<<std::fixed<<std::setprecision(3)<<"info "<<move.name<<": seam error before the move "<<error_before_move<<" px, right after it "<<moved_px
            <<" px, back within "<<kSeamTargetPx<<" px after "<<recovered<<" s (clock of idle-machine job times, "<<passes<<" passes)\n";
        Check(error_before_move<=kSeamTargetPx,(std::string(move.name)+": the seam was right before the move").c_str());
        Check(moved_px>kSeamTargetPx,(std::string(move.name)+": the move shows at the seam").c_str());
        Check(recovered>=0 && recovered<=kRecoverSeconds,(std::string(move.name)+": the seam is back within the target in time").c_str());
    }
}

// Frames that carry nothing to estimate from, after C has been tracking: the estimate in force is kept for the holding time and then C goes
// back to the condition's fixed mapping, as it did before the policy of the significance line.
void FallsBackToFixed(){
    LiveRig live;
    live.context->live_limits=RigLimits();
    const auto clean=MakeClean(live,Pose{0.1,-0.1,0.4,2.0,-1.0,0},910);
    struct Case {const char* name;render::BgrImage a,b;};
    const Case cases[]{{"dark",Noise(4,3.0,31),Noise(4,3.0,32)},{"plain paper",Flat(200),Flat(190)},{"camera B black",clean.a,Flat(0)},
        {"camera A black",Flat(0),clean.b}};
    for(const auto& c:cases){
        Session session(live);
        std::uint64_t seed=3000;
        for(int k=0;k<8 && session.runtime->Status().view.state!=LiveRegistrationState::tracking;++k){ShowNoisy(session,clean,++seed,kMoveNoise);(void)TimedPass(session);}
        Check(session.runtime->Status().view.state==LiveRegistrationState::tracking,(std::string(c.name)+": C was tracking").c_str());
        session.a=c.a;session.b=c.b;
        const double start=session.now;
        bool held=false;
        while(session.now-start<5.0){
            (void)TimedPass(session);
            if(session.runtime->Status().view.state==LiveRegistrationState::holding)held=true;
        }
        const auto status=session.runtime->Status();
        Check(held,(std::string(c.name)+": the estimate in force is kept for a while").c_str());
        Check(status.view.state==LiveRegistrationState::fixed && status.view.mapping.pose.IsZero() && status.view.mapping.surface.IsZero(),
            (std::string(c.name)+": C is back on the fixed mapping").c_str());
        Check(Contains(status.state_text,"固定の写像") && !Contains(status.state_text,"目安"),(std::string(c.name)+": and says so").c_str());
    }
}

// ---- 3. Nothing to estimate from: no estimate is taken, and the screen does not say that C is registered -----------------------------
void ExpectNoEstimate(LiveRig& live,const char* name,const render::BgrImage& a,const render::BgrImage& b,const char* reason,
    const LiveEstimatorDesign& design=kLiveEstimatorDesignDraft){
    Session session(live,Ranges(),design);
    session.a=a;session.b=b;
    const auto pass=session.Pass(0);
    const auto status=session.runtime->Status();
    std::cerr<<"info "<<name<<": "<<Join(status.view.last_failure)<<'\n';
    Check(pass.job_ran && pass.accept==LiveAcceptKind::kept_previous && !pass.rebaked,(std::string(name)+": no estimate is taken").c_str());
    Check(status.view.state==LiveRegistrationState::fixed && status.view.mapping.pose.IsZero() && status.view.mapping.surface.IsZero() && !session.runtime->CurrentPlan(),
        (std::string(name)+": C stays on the fixed mapping").c_str());
    Check(Contains(status.state_text,"推定できない") && !Contains(status.state_text,"目安") && Contains(status.orientation.b_short,"測れていません"),
        (std::string(name)+": the rows say that nothing is estimated").c_str());
    if(reason)Check(Has(status.view.last_failure,reason),(std::string(name)+": the reason is "+reason+", got "+Join(status.view.last_failure)).c_str());
    // And again on later frames: a wrong estimate must not slip through on a retry either.
    for(int k=1;k<=3;++k){
        const auto retry=session.Pass(1.1*k);
        Check(!retry.job_ran || retry.accept==LiveAcceptKind::kept_previous,(std::string(name)+": no estimate is taken on a retry").c_str());
    }
    CHECK(session.runtime->Status().view.state==LiveRegistrationState::fixed);
}
void NothingToEstimateFrom(){
    LiveRig live;
    // The line of darkness turned off, to show that the estimator itself refuses noise.
    auto no_line=kLiveEstimatorDesignDraft;
    no_line.grid.minimum_band_brightness=0;
    ExpectNoEstimate(live,"plain paper",Flat(200),Flat(190),nullptr);
    ExpectNoEstimate(live,"plain paper with noise",Noise(200,kNoiseSigma,1),Noise(190,kNoiseSigma,2),nullptr);
    // A live view at night: black with noise.
    for(std::uint64_t seed=0;seed<4;++seed){
        ExpectNoEstimate(live,"dark with noise",Noise(4,3.0,10+seed),Noise(4,3.0,20+seed),"too-dark");
        ExpectNoEstimate(live,"dark with noise, no line of darkness",Noise(4,3.0,10+seed),Noise(4,3.0,20+seed),nullptr,no_line);
        ExpectNoEstimate(live,"dark with strong noise, no line of darkness",Noise(10,8.0,30+seed),Noise(10,8.0,40+seed),nullptr,no_line);
        ExpectNoEstimate(live,"grey with strong noise",Noise(90,10.0,50+seed),Noise(90,10.0,60+seed),nullptr);
        ExpectNoEstimate(live,"grey with the blocks of a compressed frame",BlockNoise(90,6.0,3.0,70+seed),BlockNoise(90,6.0,3.0,80+seed),nullptr);
    }
    // One camera only has a picture.
    const auto scene=MakeLiveScene(live,"waves",kWaves,{},Pose{0,0,0.4,3.0,0,0},300);
    ExpectNoEstimate(live,"camera B black",scene.a,Flat(0),"too-dark");
    ExpectNoEstimate(live,"camera A black",Flat(0),scene.b,"too-dark");
    ExpectNoEstimate(live,"camera B black, no line of darkness",scene.a,Flat(0),nullptr,no_line);
    ExpectNoEstimate(live,"camera B noise only",scene.a,Noise(120,6.0,91),nullptr);
    // The same content in A with a camera B that shows another paper: nothing agrees.
    const auto other=MakeLiveScene(live,"waves-other",ps::PatternSpec{ps::PatternKind::two_directional,99},{},Pose{},301);
    ExpectNoEstimate(live,"camera B shows another paper",scene.a,other.b,nullptr);
}

// ---- 4. A pattern that repeats: no estimate one period off ---------------------------------------------------------------------------
void RepeatingPattern(){
    LiveRig live;
    // Cells of 8 mm and nothing else; B moved by one cell along the baseline. The edge energy and the colour repeat, so there is
    // no anchor: no estimate (and not "no movement", which the cells would agree with).
    ps::PatternSpec cells;
    cells.kind=ps::PatternKind::periodic_grid;cells.period_mm=8.0;
    const Pose one_period{0,0,0,8.0,0,0};
    {
        const auto scene=MakeLiveScene(live,"cells",cells,{},one_period,400);
        ExpectNoEstimate(live,"cells only, one period off",scene.a,scene.b,nullptr);
    }
    // The same cells in bands between bands of text: the text is the anchor, and the estimate is the true place, one period away,
    // not the nearest one.
    ps::PatternSpec mixed;
    mixed.kind=ps::PatternKind::table_and_text;mixed.period_mm=8.0;mixed.band_mm=30.0;
    for(const auto& b:{one_period,Pose{0,0,0,0,0,0},Pose{0,0,0.5,-4.0,2.0,0}}){
        const auto scene=MakeLiveScene(live,"cells-and-text",mixed,{},b,401);
        Session session(live);
        session.Show(scene);
        const auto before=SeamErrorOf(live,*scene.model,ex::RegisteredMapping{});
        const auto pass=session.Pass(0);
        const auto status=session.runtime->Status();
        const auto after=SeamErrorOf(live,*scene.model,status.view.mapping);
        Report("cells and text",before,after,status.view);
        // Either the true place or no estimate; never an estimate that is off.
        if(pass.accept==LiveAcceptKind::applied)CHECK(after.maximum<=kSeamTargetPx);
        else CHECK(status.view.state==LiveRegistrationState::fixed);
        Check(pass.accept==LiveAcceptKind::applied,("cells and text: the text anchors the estimate ("+session.LogText()+")").c_str());
    }
}

// ---- 5. The stop ends a job quickly ---------------------------------------------------------------------------------------------------
void StopsQuickly(){
    LiveRig live;
    const auto scene=MakeLiveScene(live,"waves",kWaves,{},Pose{0.2,0,0.5,3.0,-1.0,0},500);
    Session session(live);
    session.Show(scene);
    LiveJob job;
    job.kind=LiveJobKind::full;job.full_reason=LiveFullReason::session_start;
    // Asked before the job: nothing is estimated.
    job.stop=[]{return true;};
    double started=WallSeconds();
    auto outcome=session.estimator(job,scene.a,scene.b);
    CHECK(!outcome.usable && Has(outcome.reasons,kLiveEstimateStoppedReason));
    std::cerr<<std::fixed<<std::setprecision(3)<<"timing stop before the job: "<<(WallSeconds()-started)*1000<<" ms\n";
    // Asked while the job runs: the n-th question answers true. The job ends within one step from there, whichever step it is.
    int asked_total=0;
    {
        std::atomic<int> asked{0};
        job.stop=[&]{++asked;return false;};
        outcome=session.estimator(job,scene.a,scene.b);
        asked_total=asked;
        CHECK(outcome.usable && asked_total>=4);
    }
    double slowest=0;
    for(int n=1;n<=asked_total;++n){
        std::atomic<int> asked{0};
        std::atomic<double> stop_time{0};
        job.stop=[&]{if(++asked>=n){double expected=0;stop_time.compare_exchange_strong(expected,WallSeconds());return true;}return false;};
        outcome=session.estimator(job,scene.a,scene.b);
        const double waited=WallSeconds()-stop_time.load();
        slowest=std::max(slowest,waited);
        Check(!outcome.usable && Has(outcome.reasons,kLiveEstimateStoppedReason),("stopped at question "+std::to_string(n)+": the job says so, got "+Join(outcome.reasons)).c_str());
    }
    std::cerr<<std::fixed<<std::setprecision(3)<<"timing stop: "<<asked_total<<" questions in a full job; slowest return after the stop "<<slowest*1000<<" ms\n";
    CHECK(slowest<2.0);   // one measurement of the band at most; a busy machine is allowed for
    // Through the worker: a stopped job is handed to the tracker (nothing stays in flight), nothing is baked, and the next pass
    // with no stop estimates.
    const auto stopped=session.runtime->RunPass(session.frames,session.estimator,[]{return true;});
    CHECK(stopped.job_ran && stopped.accept==LiveAcceptKind::kept_previous && !stopped.rebaked);
    CHECK(session.runtime->Status().view.state==LiveRegistrationState::fixed && Has(session.runtime->Status().view.last_failure,kLiveEstimateStoppedReason));
    const auto next=session.Pass(1.1);
    CHECK(next.job_ran && next.accept==LiveAcceptKind::applied && next.rebaked);
    // The real worker thread: asked to stop while a job runs, it is joined quickly.
    {
        Session threaded(live);
        threaded.Show(scene);
        std::atomic<int> jobs{0};
        const auto origin=std::chrono::steady_clock::now();
        LiveRegistrationRuntime::Config config;
        config.live_enabled=true;
        config.clock=[origin]{return std::chrono::duration<double>(std::chrono::steady_clock::now()-origin).count();};
        LiveRegistrationRuntime runtime(config,live.rig.Parameters());
        runtime.PublishBase(live.plan);
        const LiveEstimator estimator=MakeLiveEstimator([&]{return runtime.BasePlan();},runtime.Design(),ConnectedLiveEstimatorEntrance(),Ranges());
        const LiveEstimator counting=[&](const LiveJob& j,const render::BgrImage& x,const render::BgrImage& y){++jobs;return estimator(j,x,y);};
        std::jthread worker([&](std::stop_token token){runtime.Work(token,{},threaded.frames,counting,std::chrono::milliseconds(5));});
        while(jobs.load()==0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // inside the first job
        const double asked=WallSeconds();
        worker.request_stop();worker.join();
        const double joined=WallSeconds()-asked;
        std::cerr<<std::fixed<<std::setprecision(3)<<"timing stop: the worker is joined "<<joined*1000<<" ms after the request\n";
        CHECK(joined<2.0 && runtime.WorkersRunning()==0);
    }
}

// ---- 6. The same frames give the same estimate, on one thread or two; the mounting turned by 180 degrees; the pose alone ---------------
void DeterministicAndMounted(){
    LiveRig live;
    const Pose b{0.1,-0.1,0.6,-3.0,2.0,0};
    const auto scene=MakeLiveScene(live,"waves",kWaves,{},b,600);
    Session session(live);
    LiveJob job;
    job.kind=LiveJobKind::full;
    const auto one=session.estimator(job,scene.a,scene.b),two=session.estimator(job,scene.a,scene.b);
    auto single=kLiveEstimatorDesignDraft;
    single.thread_count=1;
    Session single_session(live,Ranges(),single);
    const auto three=single_session.estimator(job,scene.a,scene.b);
    const auto same=[](const LiveEstimateOutcome& x,const LiveEstimateOutcome& y){
        return x.usable==y.usable && x.pose.rx_deg==y.pose.rx_deg && x.pose.ry_deg==y.pose.ry_deg && x.pose.rz_deg==y.pose.rz_deg && x.pose.tx_mm==y.pose.tx_mm &&
            x.pose.ty_mm==y.pose.ty_mm && x.pose.tz_percent==y.pose.tz_percent && x.surface.h0_mm==y.surface.h0_mm && x.surface.slope_x==y.surface.slope_x &&
            x.surface.slope_y==y.surface.slope_y && x.reasons==y.reasons;
    };
    CHECK(one.usable && same(one,two) && same(one,three));
    // A follow-up from the estimate, on the same frames, moves by less than its line and keeps its points.
    job.kind=LiveJobKind::follow;job.start_pose=one.pose;job.start_surface=one.surface;
    const auto follow=session.estimator(job,scene.a,scene.b);
    std::cerr<<std::fixed<<std::setprecision(3)<<"info follow on the same frames: change "<<follow.change_px<<" px, inlier fraction "<<follow.inlier_fraction
        <<", brightness "<<follow.band_brightness<<'\n';
    CHECK(follow.usable && follow.change_px<=kLiveTrackerDesignDraft.tracker.follow_change_limit_px && follow.inlier_fraction>=kLiveTrackerDesignDraft.tracker.inlier_fraction_floor);
    CHECK(std::isfinite(follow.band_brightness) && std::abs(follow.band_brightness-one.band_brightness)<1e-9 && one.band_brightness>100 && one.band_brightness<200);

    // The rig mounted upside down: the frames are turned, the mapping carries the flip, the pose is the same physical movement.
    {
        LiveRig turned(true);
        const auto turned_scene=MakeLiveScene(turned,"waves",kWaves,{},b,600);
        Session turned_session(turned);
        turned_session.Show(turned_scene);
        const auto before=SeamErrorOf(turned,*turned_scene.model,ex::RegisteredMapping{});
        const auto pass=turned_session.Pass(0);
        const auto view=turned_session.runtime->Status().view;
        const auto after=SeamErrorOf(turned,*turned_scene.model,view.mapping);
        Report("mounted upside down",before,after,view);
        Check(pass.accept==LiveAcceptKind::applied && after.maximum<=kSeamTargetPx,("mounted upside down: the seam is within the target ("+turned_session.LogText()+")").c_str());
        CHECK(std::abs(view.mapping.pose.rz_deg-b.rz_deg)<=0.15);
    }
    // A condition without a surface envelope: the pose alone (six components).
    {
        Session pose_only(live,Ranges(false));
        pose_only.Show(scene);
        const auto before=SeamErrorOf(live,*scene.model,ex::RegisteredMapping{});
        const auto pass=pose_only.Pass(0);
        const auto view=pose_only.runtime->Status().view;
        const auto after=SeamErrorOf(live,*scene.model,view.mapping);
        Report("pose only",before,after,view);
        Check(pass.accept==LiveAcceptKind::applied && after.maximum<=kSeamTargetPx && view.mapping.surface.IsZero(),("pose only: the seam is within the target ("+pose_only.LogText()+")").c_str());
    }
}

// ---- 7. What the connection is given, and what it refuses -----------------------------------------------------------------------------
void Connection(){
    // The geometry of a capture condition on C's grid (addendum 11.21): margin and guard up, band down, the guard clear of half a patch.
    {
        const auto g=cr::ScaleRegistrationGeometry(500,1,16,72,1024,640.0/7360.0,16);
        CHECK(g.seam_column==500 && g.mixing_width_px==1 && g.omega_margin_px==2 && g.guard_px==9 && g.band_half_width_px==89);
        CHECK(g.omega_half_width_px()==3 && g.exclusion_half_width_px()==12);
        const auto wide=cr::ScaleRegistrationGeometry(500,4,16,72,1024,0.5,16);
        CHECK(wide.omega_margin_px==8 && wide.guard_px==36 && wide.band_half_width_px==512);
        bool threw=false;
        try{(void)cr::ScaleRegistrationGeometry(500,1,16,72,1024,0.0,16);}catch(const std::invalid_argument&){threw=true;}
        CHECK(threw);
    }
    {
        ex::FixedCompositeConditionV3::Registration registration;
        registration.envelope.position_mm=10;registration.envelope.rotation_deg=2;registration.envelope.tilt_deg=1;registration.envelope.height_percent=1;
        registration.geometry={16,72,1024};registration.surface_envelope=ex::SurfaceEnvelope{40,8};registration.apply_beyond_envelope=true;
        const auto ranges=LiveRegistrationRangesOf(registration);
        CHECK(ranges.envelope.position_mm==10 && ranges.surface_envelope && ranges.surface_envelope->height_mm==40 && ranges.omega_margin_px==16 &&
            ranges.guard_px==72 && ranges.band_half_width_px==1024 && ranges.apply_beyond_envelope);
        registration.apply_beyond_envelope.reset();
        CHECK(!LiveRegistrationRangesOf(registration).apply_beyond_envelope);
    }
    // The draft design is one that the estimator and the measurer accept, and two threads.
    {
        const auto& d=kLiveEstimatorDesignDraft;
        CHECK(d.thread_count==2 && d.grid.measurer.patch_px==16 && d.grid.measurer.point_spacing_px==16 && d.grid.estimator.fine_patch_px==16);
        CHECK(d.grid.minimum_band_brightness==16.0);
        a0::m2::seam_gate::ValidateSeamGateDesign(d.grid.measurer);
    }
    CHECK(LiveLogSafeText("the band must reach past Omega and the guard")=="the band must reach past Omega and the guard");
    CHECK(!Contains(LiveLogSafeText("cannot open C:\\Users\\someone\\file.json"),"someone") && !Contains(LiveLogSafeText("/home/someone/file"),"someone"));

    LiveRig live;
    const auto scene=MakeLiveScene(live,"waves",kWaves,{},Pose{0,0,0.4,2.0,0,0},700);
    LiveJob job;
    job.kind=LiveJobKind::full;
    // Without the condition's ranges the estimate is not connected, and says so with the code that the window words.
    {
        LiveRegistrationRuntime::Config config;
        config.live_enabled=true;config.clock=[]{return 0.0;};
        LiveRegistrationRuntime runtime(config,live.rig.Parameters());
        runtime.PublishBase(live.plan);
        const auto estimator=MakeLiveEstimator([&]{return runtime.BasePlan();},runtime.Design(),ConnectedLiveEstimatorEntrance());
        const auto outcome=estimator(job,scene.a,scene.b);
        CHECK(!outcome.usable && Has(outcome.reasons,kLiveEstimatorUnavailableReason));
    }
    // A band that does not reach past the guard on C's grid is refused with a reason, not thrown.
    {
        auto narrow=Ranges();
        narrow.band_half_width_px=20;
        Session session(live,narrow);
        const auto outcome=session.estimator(job,scene.a,scene.b);
        CHECK(!outcome.usable && Has(outcome.reasons,kLiveEstimateRefusedReason) && !outcome.detail.empty());
    }
    // An estimate outside a range is applied only when the condition says so; otherwise it is not usable and says why.
    {
        const auto beyond=MakeLiveScene(live,"waves",kWaves,{},Pose{0,0,2.6,1.0,0,0},701);   // 2.6 degrees, the range is 2
        Session applies(live);
        auto outcome=applies.estimator(job,beyond.a,beyond.b);
        std::cerr<<"info beyond the range: "<<(outcome.usable?"usable":"not usable")<<" "<<Join(outcome.reasons)<<" rz "<<outcome.pose.rz_deg<<'\n';
        CHECK(outcome.usable && outcome.beyond_envelope && std::abs(outcome.pose.rz_deg-2.6)<=0.15);
        // ... and the estimate says which component it is, with the value and the limit in its log line.
        CHECK(Has(outcome.exceeded,"rotation") && outcome.exceeded.size()==1);
        CHECK(Contains(outcome.detail,"exceeded=rotation:2.") && Contains(outcome.detail,"/2"));
        auto ranges=Ranges();
        ranges.apply_beyond_envelope=false;
        Session refuses(live,ranges);
        outcome=refuses.estimator(job,beyond.a,beyond.b);
        CHECK(!outcome.usable && outcome.beyond_envelope && Has(outcome.reasons,kLiveEstimateBeyondEnvelopeReason));
        applies.Show(beyond);
        CHECK(applies.Pass(0).accept==LiveAcceptKind::applied && Contains(applies.runtime->Status().state_text,"範囲の外"));
        Check(Contains(applies.runtime->Status().state_text,"範囲を超えた量: 回転）") && Contains(applies.LogText(),"drawn_exceeded=rotation"),
            ("the screen and the log name the component that is outside the range: "+applies.runtime->Status().state_text).c_str());
    }
}

// ---- 8. How long a job takes -------------------------------------------------------------------------------------------------------
void Timing(){
    LiveRig live;
    const auto scene=MakeLiveScene(live,"waves",kWaves,{},Pose{0.2,0,0.5,3.0,-1.0,0},800);
    Session session(live);
    LiveJob job;
    job.kind=LiveJobKind::full;
    constexpr int kRuns=5;
    LiveEstimateOutcome full;
    double wall=WallSeconds(),cpu=CpuSeconds(),slowest=0;
    for(int k=0;k<kRuns;++k){const double t=WallSeconds();full=session.estimator(job,scene.a,scene.b);slowest=std::max(slowest,WallSeconds()-t);}
    std::cerr<<std::fixed<<std::setprecision(1)<<"timing full estimate: wall "<<(WallSeconds()-wall)/kRuns*1000<<" ms (slowest "<<slowest*1000<<" ms), processor "
        <<(CpuSeconds()-cpu)/kRuns*1000<<" ms, mean of "<<kRuns<<" on a grid of "<<live.plan.grid.width<<" x "<<live.plan.grid.height<<" with "<<kLiveEstimatorDesignDraft.thread_count<<" threads\n";
    CHECK(full.usable);
    job.kind=LiveJobKind::follow;job.start_pose=full.pose;job.start_surface=full.surface;
    LiveEstimateOutcome follow;
    wall=WallSeconds();cpu=CpuSeconds();slowest=0;
    for(int k=0;k<kRuns;++k){const double t=WallSeconds();follow=session.estimator(job,scene.a,scene.b);slowest=std::max(slowest,WallSeconds()-t);}
    const double follow_wall=(WallSeconds()-wall)/kRuns;
    std::cerr<<std::fixed<<std::setprecision(1)<<"timing follow-up: wall "<<follow_wall*1000<<" ms (slowest "<<slowest*1000<<" ms), processor "<<(CpuSeconds()-cpu)/kRuns*1000
        <<" ms, mean of "<<kRuns<<'\n';
    CHECK(follow.usable);
    // The bake of B's table for the estimate.
    wall=WallSeconds();cpu=CpuSeconds();
    const auto baked=RebakePreviewB(live.plan,ex::RegisteredMapping{full.pose,full.surface,std::nullopt});
    std::cerr<<std::fixed<<std::setprecision(1)<<"timing bake of B's table: wall "<<(WallSeconds()-wall)*1000<<" ms, processor "<<(CpuSeconds()-cpu)*1000<<" ms\n";
    Check(baked.ok,("the estimate can be baked: "+baked.detail).c_str());
    // The follow-up has to fit in its interval with room for the bake (a busy machine is allowed for: twice the interval).
    CHECK(follow_wall<2*kLiveTrackerDesignDraft.tracker.follow_interval_seconds);
}
}

int main(int argc,char** argv){
    const double started=WallSeconds();
    try{
        if(argc>1 && std::string(argv[1])=="--steady"){SteadyUnderRigScatter();SteadyOnFlatFrames();FollowsAMoveOfB();FallsBackToFixed();}   // the sections of the still scene, for tuning
        else{
        Connection();
        FullEstimates();
        FollowsAndJumps();
        SteadyUnderRigScatter();
        SteadyOnFlatFrames();
        FollowsAMoveOfB();
        FallsBackToFixed();
        NothingToEstimateFrom();
        RepeatingPattern();
        DeterministicAndMounted();
        StopsQuickly();
        Timing();
        }
    }catch(const std::exception& error){
        std::cerr<<"FAILED: exception "<<error.what()<<'\n';
        ++failures;
    }
    std::cerr<<std::fixed<<std::setprecision(1)<<"timing whole run: "<<WallSeconds()-started<<" s\n";
    std::cout<<"live_registration_estimator: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
