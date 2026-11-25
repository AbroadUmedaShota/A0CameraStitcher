// The estimate of the live C (#417 LC; docs/design/fixed-composite-registration.md addendum 11.21): content_registration's estimate
// on C's grid, connected to the worker of the live registration. Everything is synthetic: the scenes of tests/parallax_scene.hpp
// (paper, two cameras, a pose of camera B and a height of the paper), reduced to the coarseness of a live view. No camera, window
// or file; no real image.
//
// The coarseness. The synthetic rig's raw frame is 1200 x 800 px over about 540 x 360 mm. It is reduced by 2 to 600 x 400 px, about
// 0.9 mm of paper per pixel, which is what a 640 x 424 live view of a frame that covers about 600 mm gives. C's grid is the
// condition's region at the pitch of that frame (about 1.06 mm per pixel, 727 x 321 pixels). Noise of 2 DN is added to every frame.
//
// The target. C is a guide drawn at its own resolution. A seam that is off by less than half a pixel of C is not visible in C, so the
// target of a full estimate is: the largest error along the seam column below 0.5 px of C (about 0.5 mm of paper). It says nothing about
// the capture, whose estimate is made again on the captured images.
//
// Since #437 stage 1 the estimate runs when it is asked for (the start of a session, the button) and nowhere else: the scenes of the
// follow-up, of the steady scene and of the fall back to the fixed mapping that tested the old policy are gone; the estimator's own follow
// job is still tested here, on the estimator itself.
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
#include <cstring>
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
    render::BgrImage a,b;
    std::vector<std::string> log;
    std::unique_ptr<LiveRegistrationRuntime> runtime;
    LiveFrameSource frames;
    LiveEstimator estimator;
    explicit Session(LiveRig& rig,const LiveRegistrationRanges& ranges=Ranges(),const LiveEstimatorDesign& design=kLiveEstimatorDesignDraft):live(rig){
        LiveRegistrationRuntime::Config config;
        config.base_pose=live.base;config.live_enabled=true;
        config.mount=live.turned?ex::MountOrientation::rotated_180:ex::MountOrientation::normal;
        config.condition_sha256=std::string(64,'d');
        config.log=[this](const std::string& line){log.push_back(line);};
        runtime=std::make_unique<LiveRegistrationRuntime>(config,live.rig.Parameters());
        runtime->PublishBase(live.plan);
        frames.ready=[]{return true;};
        frames.snapshot=[this](render::BgrImage& x,render::BgrImage& y){x=a;y=b;return true;};
        estimator=MakeLiveEstimator([this]{return runtime->BasePlan();},runtime->Design(),
            [design](const LiveEstimatorInputs& in){return EstimateLiveRegistration(in,design);},ranges);
    }
    void Show(const LiveScene& scene){a=scene.a;b=scene.b;}
    // One pass of the worker. The first pass of a session runs the estimate of the start; every other job is one that the test asked for.
    LivePassResult Pass(){return runtime->RunPass(frames,estimator);}
    std::string LogText() const {std::string all;for(const auto& line:log)all+=line+"\n";return all;}
};

void Report(const char* name,const SeamError& before,const SeamError& after,const LivePlacementView& view){
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
        const auto pass=session.Pass();
        const auto status=session.runtime->Status();
        const auto after=SeamErrorOf(live,*scene.model,status.placement.mapping);
        Report(c.name,before,after,status.placement);
        Check(pass.job_ran && pass.kind==LiveJobKind::full && pass.accept==LiveAcceptKind::applied,(std::string(c.name)+": the full estimate is taken ("+session.LogText()+")").c_str());
        Check(status.placement.source==LivePlacementSource::auto_start && Contains(status.state_text,"位置を確定しました（自動・開始時") && !Contains(status.state_text,"基準内"),
            (std::string(c.name)+": C says that the position is confirmed, and not that it is within a criterion").c_str());
        // The line of the log carries what the estimate measured, for whoever sets the limits of the live view on a rig.
        Check(Contains(session.LogText(),"job=full") && Contains(session.LogText(),"estimate_state=") && Contains(session.LogText(),"sigma_px=") &&
            Contains(session.LogText(),"rms_px=") && Contains(session.LogText(),"bias_px="),(std::string(c.name)+": the log line has the measured values").c_str());
        // ... and the estimate itself (the six components and the paper) and the components outside the range; and, once the bake that drew it
        // has passed, the position in the one form that the capture's log writes too.
        Check(Contains(session.LogText(),"est_rx_deg=") && Contains(session.LogText(),"est_ry_deg=") && Contains(session.LogText(),"est_rz_deg=") &&
            Contains(session.LogText(),"est_tx_mm=") && Contains(session.LogText(),"est_ty_mm=") && Contains(session.LogText(),"est_tz_percent=") &&
            Contains(session.LogText(),"est_h0_mm=") && Contains(session.LogText(),"est_slope_x=") && Contains(session.LogText(),"est_slope_y=") &&
            Contains(session.LogText(),"exceeded=") && Contains(session.LogText(),"C live_placement serial=") && Contains(session.LogText(),"source=auto-start") &&
            Contains(session.LogText(),LivePlacementFields(status.placement.mapping)) && Contains(session.LogText(),"drawn=1"),
            (std::string(c.name)+": the log has the estimate, the range and the position that C is drawn with").c_str());
        Check(before.rows>=40 && before.maximum>c.moved_px,(std::string(c.name)+": the scene moves the seam").c_str());
        // The lean of B's lens is the scene's to within a quarter of a degree (the band sees it far less well than the turn).
        Check(std::abs(status.placement.mapping.pose.rx_deg-c.b.rx_deg)<=0.25 && std::abs(status.placement.mapping.pose.ry_deg-c.b.ry_deg)<=0.25,
            (std::string(c.name)+": B's tilt is the scene's within 0.25 degrees").c_str());
        Check(after.maximum<=kSeamTargetPx,(std::string(c.name)+": the seam is within the target after the estimate").c_str());
        // The table that C draws with is the estimate's.
        Check(pass.rebaked && session.runtime->CurrentPlan() && session.runtime->CurrentPlan()->live->baked.pose.rz_deg==status.placement.mapping.pose.rz_deg,
            (std::string(c.name)+": B's table is baked for the estimate").c_str());
        // The neighbourhood of the seam (the columns that the blend of C can show), not only the column itself.
        Check(SeamErrorOf(live,*scene.model,status.placement.mapping,-8).maximum<=kSeamTargetPx && SeamErrorOf(live,*scene.model,status.placement.mapping,8).maximum<=kSeamTargetPx,
            (std::string(c.name)+": eight columns either side of the seam are within the target").c_str());
        // B's direction is shown, and its turn is the scene's (the turn about the axis is what the band sees best).
        Check(!Contains(status.orientation.b_short,"測れていません") && Contains(status.orientation_line,"B: "),(std::string(c.name)+": B's direction is shown").c_str());
        Check(std::abs(status.placement.mapping.pose.rz_deg-c.b.rz_deg)<=0.15,(std::string(c.name)+": B's turn is the scene's within 0.15 degrees").c_str());
    }
}

// ---- 2. C does not follow by itself: B moves, and nothing changes until the button is pressed ----------------------------------------
void HoldsUntilPressed(){
    LiveRig live;
    const Pose start{0.0,0.0,0.4,2.0,-1.0,0.0},moved{0.1,-0.1,0.7,4.0,-1.0,0.0};
    const auto before=MakeLiveScene(live,"waves",kWaves,{},start,910);
    const auto after=MakeLiveScene(live,"waves",kWaves,{},moved,911);
    Session session(live);
    session.Show(before);
    const auto first=session.Pass();
    Check(first.job_ran && first.accept==LiveAcceptKind::applied && first.rebaked,("the estimate of the start is taken ("+session.LogText()+")").c_str());
    const auto placed=session.runtime->Status();
    CHECK(SeamErrorOf(live,*before.model,placed.placement.mapping).maximum<=kSeamTargetPx);
    // B moves. However long the worker runs afterwards nothing is estimated, nothing is baked and P stays: the seam is off, and C says
    // nothing about it.
    session.Show(after);
    const auto plan=session.runtime->CurrentPlan();
    for(int k=0;k<400;++k){const auto pass=session.Pass();CHECK(!pass.job_ran && !pass.rebaked);}
    auto status=session.runtime->Status();
    CHECK(session.runtime->CurrentPlan()==plan && status.placement.serial==placed.placement.serial && LivePlacementSameMapping(status.placement.mapping,placed.placement.mapping));
    CHECK(status.state_text==placed.state_text && status.rebakes==placed.rebakes);
    const double off=SeamErrorOf(live,*after.model,status.placement.mapping).maximum;
    std::cerr<<std::fixed<<std::setprecision(3)<<"info B moved: the seam is off by "<<off<<" px of C with nothing following it\n";
    Check(off>kSeamTargetPx,"the move shows at the seam");
    // The button: one estimate, and the seam is right again.
    CHECK(session.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto pressed=session.Pass();
    status=session.runtime->Status();
    Check(pressed.job_ran && pressed.kind==LiveJobKind::full && pressed.accept==LiveAcceptKind::applied && pressed.rebaked,("the press runs one estimate ("+session.LogText()+")").c_str());
    CHECK(status.placement.source==LivePlacementSource::auto_button && Contains(status.state_text,"位置を確定しました（自動・ボタン"));
    CHECK(SeamErrorOf(live,*after.model,status.placement.mapping).maximum<=kSeamTargetPx);
    CHECK(session.runtime->CurrentPlan()->live->baked.pose.tx_mm==status.placement.mapping.pose.tx_mm);
    // A press on frames that carry nothing to estimate from: P stays, the sentence says why, and nothing is asked for again.
    session.a=Flat(200);session.b=Flat(190);
    const auto kept=session.runtime->CurrentPlan();
    const auto serial=status.placement.serial;
    CHECK(session.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto nothing=session.Pass();
    status=session.runtime->Status();
    CHECK(nothing.job_ran && nothing.accept==LiveAcceptKind::kept_previous && !nothing.rebaked && session.runtime->CurrentPlan()==kept && status.placement.serial==serial);
    CHECK(Contains(status.state_text,"位置を決められませんでした") && Contains(status.state_text,"前の位置のままです（出どころ: ボタンの自動）"));
    CHECK(SeamErrorOf(live,*after.model,status.placement.mapping).maximum<=kSeamTargetPx);   // the position that was right is still the one in force
    for(int k=0;k<100;++k)CHECK(!session.Pass().job_ran);
}

// ---- 3. Nothing to estimate from: no estimate is taken, and the screen does not say that C is registered -----------------------------
void ExpectNoEstimate(LiveRig& live,const char* name,const render::BgrImage& a,const render::BgrImage& b,const char* reason,
    const LiveEstimatorDesign& design=kLiveEstimatorDesignDraft){
    Session session(live,Ranges(),design);
    session.a=a;session.b=b;
    const auto pass=session.Pass();
    const auto status=session.runtime->Status();
    std::cerr<<"info "<<name<<": "<<Join(status.placement.last_failure)<<'\n';
    Check(pass.job_ran && pass.accept==LiveAcceptKind::kept_previous && !pass.rebaked,(std::string(name)+": no estimate is taken").c_str());
    Check(status.placement.source==LivePlacementSource::fixed_condition && status.placement.mapping.pose.IsZero() && status.placement.mapping.surface.IsZero() && !session.runtime->CurrentPlan(),
        (std::string(name)+": C stays on the condition's position").c_str());
    Check(Contains(status.state_text,"位置を決められませんでした") && !Contains(status.state_text,"位置を確定しました") && Contains(status.orientation.b_short,"測れていません"),
        (std::string(name)+": the rows say that no position was decided").c_str());
    if(reason)Check(Has(status.placement.last_failure,reason),(std::string(name)+": the reason is "+reason+", got "+Join(status.placement.last_failure)).c_str());
    // Nothing is asked for again by itself; and when it is asked for, on the same kind of frames, a wrong estimate must not slip through
    // either.
    for(int k=1;k<=3;++k){
        CHECK(!session.Pass().job_ran);
        CHECK(session.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
        const auto retry=session.Pass();
        Check(retry.job_ran && retry.accept==LiveAcceptKind::kept_previous,(std::string(name)+": no estimate is taken when it is asked for again").c_str());
    }
    CHECK(session.runtime->Status().placement.source==LivePlacementSource::fixed_condition);
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
        const auto pass=session.Pass();
        const auto status=session.runtime->Status();
        const auto after=SeamErrorOf(live,*scene.model,status.placement.mapping);
        Report("cells and text",before,after,status.placement);
        // Either the true place or no estimate; never an estimate that is off.
        if(pass.accept==LiveAcceptKind::applied)CHECK(after.maximum<=kSeamTargetPx);
        else CHECK(status.placement.source==LivePlacementSource::fixed_condition);
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
    // Through the worker: a stopped job estimated nothing (it is asked for again, and no failure is recorded), nothing is baked, and the next
    // pass with no stop estimates.
    const auto stopped=session.runtime->RunPass(session.frames,session.estimator,[]{return true;});
    CHECK(stopped.job_ran && stopped.accept==LiveAcceptKind::discarded_stale && !stopped.rebaked);
    CHECK(session.runtime->Status().placement.activity==LivePlacementActivity::requested && session.runtime->Status().placement.outcome==LivePlacementOutcomeKind::none);
    const auto next=session.Pass();
    CHECK(next.job_ran && next.accept==LiveAcceptKind::applied && next.rebaked);
    // The real worker thread: asked to stop while a job runs, it is joined quickly.
    {
        Session threaded(live);
        threaded.Show(scene);
        std::atomic<int> jobs{0};
        LiveRegistrationRuntime::Config config;
        config.live_enabled=true;config.condition_sha256=std::string(64,'d');
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
    job.kind=LiveJobKind::follow;job.start_pose=one.pose;job.start_surface=one.estimated_surface;
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
        const auto pass=turned_session.Pass();
        const auto view=turned_session.runtime->Status().placement;
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
        const auto pass=pose_only.Pass();
        const auto view=pose_only.runtime->Status().placement;
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
        config.live_enabled=true;config.condition_sha256=std::string(64,'d');
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
        CHECK(applies.Pass().accept==LiveAcceptKind::applied && Contains(applies.runtime->Status().state_text,"範囲の外"));
        Check(Contains(applies.runtime->Status().state_text,"範囲を超えた量: 回転）") && Contains(applies.LogText(),"exceeded=rotation:"),
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
    job.kind=LiveJobKind::follow;job.start_pose=full.pose;job.start_surface=full.estimated_surface;
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
    // The follow job is not on the live path any more (nothing asks for it); its time is reported for whoever brings it back.
    (void)follow_wall;
}

// ---- 9. The surface that the live mapping applies is the estimated one turned about the seam line ------------------------------------
// The real estimate on the scene of "mixed, paper 5 mm up" (FullEstimates), through the connection the runtime uses. The entrance is
// wrapped so that the inputs the product made and the outcome it returned are seen: the outcome carries the estimated plane and the
// applied one, the applied one is the live mouth's answer for those inputs, and what a pair capture is then given is that applied one.
// The rule itself is tested elsewhere (T24, the runtime's rows); this checks the wiring.
bool SameBits(double a,double b){std::uint64_t x{},y{};std::memcpy(&x,&a,sizeof x);std::memcpy(&y,&b,sizeof y);return x==y;}
bool SameSurfaceBits(const ex::SurfacePlane& a,const ex::SurfacePlane& b){return SameBits(a.h0_mm,b.h0_mm) && SameBits(a.slope_x,b.slope_x) && SameBits(a.slope_y,b.slope_y);}
void AppliedSurfaceOfTheEstimate(){
    LiveRig live;
    ps::HeightField field;
    field.plane=ps::PlaneOver(live.seam,5.0,0,0);
    const auto scene=MakeLiveScene(live,"waves-up",kWaves,field,Pose{0.2,0.1,-0.6,3.0,-2.0,0.4},107);
    Session session(live);
    session.Show(scene);
    std::optional<LiveEstimateOutcome> seen;
    std::optional<ex::SurfacePlane> mouth;
    session.estimator=MakeLiveEstimator([&session]{return session.runtime->BasePlan();},session.runtime->Design(),[&](const LiveEstimatorInputs& in){
        auto outcome=EstimateLiveRegistration(in,kLiveEstimatorDesignDraft);
        seen=outcome;
        mouth=LiveAppliedSurfaceOf(in,outcome.estimated_surface);
        return outcome;
    },Ranges());
    const auto pass=session.Pass();
    Check(pass.job_ran && pass.accept==LiveAcceptKind::applied && pass.rebaked,("the estimate is taken and drawn ("+session.LogText()+")").c_str());
    CHECK(seen.has_value() && mouth.has_value());
    if(!seen || !mouth)return;
    const auto& o=*seen;
    std::cerr<<std::setprecision(17)<<"info applied surface: estimated h0 "<<o.estimated_surface.h0_mm<<" gx "<<o.estimated_surface.slope_x<<" gy "<<o.estimated_surface.slope_y
        <<"; applied h0 "<<o.surface.h0_mm<<" gx "<<o.surface.slope_x<<" gy "<<o.surface.slope_y<<'\n'<<std::fixed<<std::setprecision(3);
    // The scene estimates a surface with a slope across the width; otherwise the rows below say nothing.
    CHECK(o.usable && o.estimated_surface.slope_x!=0);
    CHECK(SameSurfaceBits(o.surface,*mouth));
    CHECK(o.surface.slope_x==0 && !std::signbit(o.surface.slope_x));
    CHECK(!SameSurfaceBits(o.surface,o.estimated_surface));
    // What C is drawn with and what a pair capture is given is the applied surface, bit for bit.
    const auto status=session.runtime->Status();
    CHECK(SameSurfaceBits(status.placement.mapping.surface,o.surface));
    const auto begin=session.runtime->BeginCapture(session.runtime->CurrentPlan());
    CHECK(begin.admission==LiveCaptureAdmission::accepted && begin.grant.has_value());
    if(begin.grant)CHECK(SameSurfaceBits(begin.grant->mapping.surface,o.surface) && begin.grant->mapping.pose.tx_mm==o.pose.tx_mm);
    session.runtime->EndCapture();
}
}

int main(){
    const double started=WallSeconds();
    try{
        {
        Connection();
        FullEstimates();
        HoldsUntilPressed();
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
    try{AppliedSurfaceOfTheEstimate();}
    catch(const std::exception& error){std::cerr<<"FAILED: exception in AppliedSurfaceOfTheEstimate "<<error.what()<<'\n';++failures;}
    std::cerr<<std::fixed<<std::setprecision(1)<<"timing whole run: "<<WallSeconds()-started<<" s\n";
    std::cout<<"live_registration_estimator: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
