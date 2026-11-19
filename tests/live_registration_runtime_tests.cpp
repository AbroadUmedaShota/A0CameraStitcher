// The worker of the live C's registration (#417 LC, #437 stage 1): what runs beside the window. The estimate when one was asked for, the bake of
// camera B's table whenever the position is to be replaced, the swap of the plan that C draws with, and the status that the window shows,
// with a scripted estimator. The rig is two pinhole cameras built here from known poses; no file, camera, clock or window.
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/phase0/experimental/live_registration_runtime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace a0::phase0::experimental;
namespace ex=a0::m2::experimental;
namespace render=a0::m2::render;
namespace cr=a0::m2::experimental::content_registration;
using Pose=cr::PoseParameters;
using namespace std::chrono_literals;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool ThrowsInvalid(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}
    return false;
}
#define REJECT(expr) Check(ThrowsInvalid([&]{(void)(expr);}),"rejects: " #expr)
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
constexpr double kPi=3.14159265358979323846;

using Mat=std::array<double,9>;
Mat Mul(const Mat& a,const Mat& b){
    Mat r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)r[i*3+j]+=a[i*3+k]*b[k*3+j];
    return r;
}
Mat Rx(double deg){const double c=std::cos(deg*kPi/180),s=std::sin(deg*kPi/180);return {1,0,0,0,c,-s,0,s,c};}
Mat Rz(double deg){const double c=std::cos(deg*kPi/180),s=std::sin(deg*kPi/180);return {c,-s,0,s,c,0,0,0,1};}
constexpr double kFx=6000,kFy=6000,kCx=3680,kCy=2456;
// A pinhole camera over the table at (x0, y0) mm and `height` mm up, turned by R. image_turned: mounted upside down.
render::CameraProjection Camera(const Mat& r,double x0,double y0,double height,bool image_turned){
    const std::array<double,3> t{-(r[0]*x0+r[1]*y0+r[2]*(-height)),-(r[3]*x0+r[4]*y0+r[5]*(-height)),-(r[6]*x0+r[7]*y0+r[8]*(-height))};
    Mat cam{r[0],r[1],t[0],r[3],r[4],t[1],r[6],r[7],t[2]};
    Mat k{kFx,0,kCx,0,kFy,kCy,0,0,1};
    Mat h=Mul(k,cam);
    if(image_turned)h=Mul(Mat{-1,0,2*kCx,0,-1,2*kCy,0,0,1},h);
    for(auto& v:h)v/=h[8];
    return render::CameraProjection(render::CameraIntrinsics(kFx,kFy,kCx,kCy),render::LensDistortion(0,0,0,0,0),h);
}

render::BgrImage Textured(unsigned w,unsigned h,unsigned seed){
    render::BgrImage image{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const unsigned base=60+(x*3+y*2+seed*17)%90;
        const bool bar=(x/9+y/13+seed)%5==0;
        for(unsigned c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*w+x)*3+c]=static_cast<std::uint8_t>(std::min(255u,base+(bar?70u:0u)+c*5));
    }
    return image;
}

// A condition v3 on the synthetic rig. Both cameras see the whole raster, so any seam column is inside the overlap.
ex::LoadedFixedCompositeConditionV3 MakeCondition(bool live,const Pose& base,ex::MountOrientation mount){
    const render::OutputRaster raster{render::DocumentRegionUm{450000,60000,650000,160000},254,2000,1000};
    const bool turned=mount==ex::MountOrientation::rotated_180;
    const render::DocumentRenderParameters parameters{raster,Camera(Rx(0),520,110,1100,turned),Camera(Mul(Rz(1.0),Rx(0.5)),580,110,1100,turned),
        a0::m2::StitchLayout::camera_a_left_camera_b_right,render::Resampling::bilinear};
    ex::FixedCompositeConditionV3 c;
    c.created_utc="2026-01-01T00:00:00Z";
    c.revision.sequence=1;
    c.source.profile_file="profile.json";
    c.source.profile_file_sha256=std::string(64,'a');c.source.profile_fingerprint_sha256=std::string(64,'b');c.source.policy_sha256=std::string(64,'c');
    c.orientation=mount;c.raster=raster;c.seam.column=1000;c.seam.origin=ex::FixedSeamOrigin::operator_set;
    c.base_pose=base;
    c.registration.envelope.position_mm=10;c.registration.envelope.rotation_deg=2;c.registration.envelope.tilt_deg=1;c.registration.envelope.height_percent=5;
    c.registration.geometry={8,72,256};
    c.registration.surface_envelope=ex::SurfaceEnvelope{20,4};
    c.registration.apply_beyond_envelope=true;
    c.registration.parallax=ex::FixedCompositeConditionV3::Registration::Parallax{1};
    if(live){
        ex::FixedCompositeConditionV3::Registration::Live l;
        l.limits.brown_lowe_alpha=8;l.limits.brown_lowe_beta=0.3;l.limits.minimum_unique_blocks=3;
        c.registration.live=l;
    }
    c.seam_gate.limit_px=1;c.seam_gate.guard_band_px=0.2;
    return ex::LoadedFixedCompositeConditionV3{c,std::string(64,'d'),ex::FixedCompositeProfileBinding{parameters,std::string("synthetic"),std::string(64,'e'),std::string(64,'f')}};
}


constexpr unsigned kW=400,kH=267;
const std::string kSha(64,'d');   // the sha of the condition that MakeCondition returns

LiveEstimateOutcome Estimate(const Pose& pose,const ex::SurfacePlane& surface={}){
    LiveEstimateOutcome o;
    o.usable=true;o.pose=pose;o.surface=surface;
    o.band_brightness=100;
    return o;
}
LiveEstimateOutcome Unusable(const char* reason){LiveEstimateOutcome o;o.reasons.emplace_back(reason);return o;}

// Everything one case needs: a runtime, frames, and a scripted estimator that remembers what it was asked.
struct Rig {
    bool ready{true};
    unsigned frame_width{kW},frame_height{kH};
    render::BgrImage a{Textured(kW,kH,1)},b{Textured(kW,kH,2)};
    std::deque<LiveEstimateOutcome> script;
    std::vector<LiveJob> jobs;
    std::vector<std::string> log;
    ex::LoadedFixedCompositeConditionV3 condition;
    std::unique_ptr<LiveRegistrationRuntime> runtime;
    FixedPreviewPlan plan;
    LiveFrameSource frames;
    LiveEstimator estimator;
    // `carry`: the machine of the session before. `sha`: the condition's sha; it is the condition's own unless a case says otherwise.
    explicit Rig(bool live=true,const Pose& base=Pose{},ex::MountOrientation mount=ex::MountOrientation::normal,
        std::optional<LivePlacementMachine> carry=std::nullopt,const std::string& sha=kSha)
        :condition(MakeCondition(live,base,mount)){
        LiveRegistrationRuntime::Config config;
        config.base_pose=base;config.live_enabled=live;config.mount=mount;
        config.condition_sha256=sha;config.placement=std::move(carry);
        config.log=[this](const std::string& line){log.push_back(line);};
        runtime=std::make_unique<LiveRegistrationRuntime>(config,condition.profile.parameters);
        plan=PlanFixedPreviewV3(condition,mount,kW,kH);
        frames.ready=[this]{return ready;};
        frames.snapshot=[this](render::BgrImage& x,render::BgrImage& y){
            x=frame_width==kW?a:Textured(frame_width,frame_height,1);y=frame_width==kW?b:Textured(frame_width,frame_height,2);return true;
        };
        estimator=[this](const LiveJob& job,const render::BgrImage&,const render::BgrImage&){
            jobs.push_back(job);
            if(script.empty())return Unusable("script-empty");
            auto outcome=script.front();script.pop_front();return outcome;
        };
    }
    LivePassResult Pass(){return runtime->RunPass(frames,estimator);}
    // The estimate of the first session, run and drawn.
    void PlaceFirst(const Pose& pose,const ex::SurfacePlane& surface={}){
        runtime->PublishBase(plan);
        script={Estimate(pose,surface)};
        (void)Pass();
    }
    std::string LogText() const {std::string all;for(const auto& line:log)all+=line+"\n";return all;}
    size_t Lines(const char* part) const {size_t n=0;for(const auto& line:log)if(Contains(line,part))++n;return n;}
};

bool Usable(const Rig& rig){return !rig.plan.skip && rig.plan.live;}
bool SameNine(const ex::RegisteredMapping& a,const ex::RegisteredMapping& b){return LivePlacementSameMapping(a,b);}

void Design(){
    ValidateLiveTrackerSettings(kLiveTrackerDesignDraft.tracker);   // the draft is still a valid design for the tracker, which no runtime uses now
    LiveRegistrationRuntime::Config config;
    CHECK(config.condition_sha256.empty() && !config.placement && !config.live_enabled);
}

void RigIsUsable(){
    for(const auto mount:{ex::MountOrientation::normal,ex::MountOrientation::rotated_180}){
        Rig rig(true,Pose{},mount);
        Check(Usable(rig),("the synthetic rig is planned: "+rig.plan.detail).c_str());
    }
}

// Not connected: the connection returns an estimate that cannot be used. C stays on the condition's position, the sentence says so, B's
// direction is not measured, A's is the calibration's. Nothing is baked, and nothing is asked for again.
void NotConnected(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    rig.estimator=MakeLiveEstimator([&]{return rig.runtime->BasePlan();},rig.runtime->Design(),ConnectedLiveEstimatorEntrance());
    auto status=rig.runtime->Status();
    CHECK(status.live_enabled && status.placement.activity==LivePlacementActivity::requested && status.placement.source==LivePlacementSource::fixed_condition);
    CHECK(status.state_text=="自動合成中です…");
    const auto first=rig.Pass();
    CHECK(first.job_ran && first.kind==LiveJobKind::full && first.accept==LiveAcceptKind::kept_previous && !first.rebaked);
    status=rig.runtime->Status();
    CHECK(status.placement.outcome==LivePlacementOutcomeKind::not_decided && status.placement.activity==LivePlacementActivity::idle);
    CHECK(Contains(status.state_text,"位置を決められませんでした") && Contains(status.state_text,"未接続") && Contains(status.state_text,"条件の位置"));
    CHECK(Contains(status.state_text,"自動位置合わせ"));
    CHECK(Contains(status.orientation.b_short,"B: 向きを測れていません"));
    CHECK(Contains(status.orientation.a_short,"A（校正時）: 回転なし") || Contains(status.orientation.a_short,"A（校正時）: 右回りに") || Contains(status.orientation.a_short,"A（校正時）: 左回りに"));
    CHECK(Contains(status.orientation_line,"Aに対する今の値") && Contains(status.orientation_line,"A（校正時）") && Contains(status.orientation_line,"B: 向きを測れていません"));
    CHECK(!rig.runtime->CurrentPlan() && status.rebakes==0 && !status.bake_failed);
    CHECK(Contains(rig.LogText(),"reason=session-start") && Contains(rig.LogText(),"live-estimator-unavailable"));
    // No retry: any number of passes later nothing was asked for again.
    for(int k=0;k<200;++k)CHECK(!rig.Pass().job_ran);
    CHECK(rig.Lines("C live_registration job=")==1);
    // The button asks again, for its own reason. The connection is one function: an entrance that the test supplies receives every input
    // the entrance of S2 is to get.
    bool seen=false;
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    rig.estimator=MakeLiveEstimator([&]{return rig.runtime->BasePlan();},rig.runtime->Design(),[&](const LiveEstimatorInputs& in){
        seen=true;
        CHECK(in.camera_a==&rig.a || in.camera_a->width==kW);
        CHECK(in.camera_b!=nullptr && in.camera_b->width==kW && in.camera_b->height==kH);
        CHECK(in.grid.width==rig.plan.grid.width && in.grid.height==rig.plan.grid.height && in.grid.pitch_mm==rig.plan.grid.pitch_mm &&
            in.grid.left_mm==rig.plan.grid.left_mm && in.grid.top_mm==rig.plan.grid.top_mm);
        CHECK(in.raster.width_pixels==rig.condition.condition.raster.width_pixels && in.seam_column==rig.condition.condition.seam.column);
        CHECK(in.limits.has_value() && in.limits->minimum_unique_blocks==3);
        CHECK(in.job!=nullptr && in.job->kind==LiveJobKind::full && in.job->full_reason==LiveFullReason::button && in.job->base_pose.IsZero());
        render::Point at{},fixed_b{};
        CHECK(in.camera_a_on_grid && in.camera_a_on_grid({10,10},at));
        CHECK(in.camera_b_on_grid && in.camera_b_on_grid(ex::RegisteredMapping{})({10,10},fixed_b));
        render::Point table_b{};
        CHECK(rig.plan.mapped->camera_b({10,10},table_b) && table_b.x==fixed_b.x && table_b.y==fixed_b.y);   // the fixed mapping is the plan's table
        return LiveEstimatorNotConnected(in);
    });
    (void)rig.Pass();
    CHECK(seen && rig.Lines("C live_registration job=")==2 && Contains(rig.LogText(),"reason=button"));
    // Without a plan the estimate cannot be made.
    LiveEstimator no_plan=MakeLiveEstimator([]{return std::shared_ptr<const FixedPreviewPlan>();},rig.runtime->Design(),ConnectedLiveEstimatorEntrance());
    LiveJob job;
    const auto outcome=no_plan(job,rig.a,rig.b);
    CHECK(!outcome.usable && !outcome.reasons.empty() && outcome.reasons[0]=="no-registered-plan");
}

// An estimate is run twice in a session at the most, whatever the clock and the frames do: once at the start and once for each press of
// the button. 600 passes with frames that change every time, frames that come and go and a scene that "moves" make no job of their own.
void AskedOnlyByRequest(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    const Pose first{0.3,-0.2,0.5,1.5,-1.0,0.0},second{0.2,-0.1,0.4,2.5,-1.5,0.1},third{0.25,-0.15,0.45,3.5,-2.0,0.2};
    rig.script={Estimate(first)};
    const auto run=[&](int passes,unsigned seed){
        for(int k=0;k<passes;++k){
            rig.ready=k%7!=3;
            rig.a=Textured(kW,kH,seed+static_cast<unsigned>(k)*2+1);rig.b=Textured(kW,kH,seed+static_cast<unsigned>(k)*2+2);
            (void)rig.Pass();
        }
        rig.ready=true;
    };
    run(10,100);
    CHECK(rig.jobs.size()==1 && rig.jobs[0].full_reason==LiveFullReason::session_start);
    auto status=rig.runtime->Status();
    const auto serial=status.placement.serial;
    CHECK(status.placement.source==LivePlacementSource::auto_start && SameNine(status.placement.mapping,ex::RegisteredMapping{first,{},std::nullopt}));
    run(600,200);
    status=rig.runtime->Status();
    CHECK(rig.jobs.size()==1 && status.placement.serial==serial && SameNine(status.placement.mapping,ex::RegisteredMapping{first,{},std::nullopt}) && status.rebakes==1);
    CHECK(status.placement.counters.jobs==1);
    // Each press is one job, and only a press.
    rig.script={Estimate(second)};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::busy);   // a second press while the first waits is not another job
    run(10,300);
    CHECK(rig.jobs.size()==2 && rig.jobs[1].full_reason==LiveFullReason::button);
    run(600,400);
    status=rig.runtime->Status();
    CHECK(rig.jobs.size()==2 && SameNine(status.placement.mapping,ex::RegisteredMapping{second,{},std::nullopt}) && status.placement.source==LivePlacementSource::auto_button);
    rig.script={Estimate(third)};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    run(10,500);
    run(600,600);
    status=rig.runtime->Status();
    CHECK(rig.jobs.size()==3 && SameNine(status.placement.mapping,ex::RegisteredMapping{third,{},std::nullopt}) && status.rebakes==3 && status.placement.counters.jobs==3);
    // A job that has the frames of the start is not a job of its own either: the estimator saw the frames of the pass that began it.
    for(const auto& job:rig.jobs)CHECK(job.kind==LiveJobKind::full && job.base_pose.IsZero() && job.start_pose.IsZero() && job.start_surface.IsZero());
}

// A usable estimate: the worker bakes B's table, the plan that C draws with is swapped, P is replaced after that, and the status and the
// direction follow. P is baked for every replacement, however small the move.
void PlacesAfterTheDraw(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    const Pose first{0.3,-0.2,0.5,1.5,-1.0,0.0};
    rig.script={Estimate(first)};
    const auto before=rig.runtime->Status();
    CHECK(before.placement.serial>0 && before.placement.source==LivePlacementSource::fixed_condition);
    const auto pass=rig.Pass();
    CHECK(pass.job_ran && pass.accept==LiveAcceptKind::applied && pass.rebaked && !pass.rebake_failed);
    const auto status=rig.runtime->Status();
    CHECK(status.placement.source==LivePlacementSource::auto_start && status.placement.serial==before.placement.serial+1 && status.rebakes==1);
    CHECK(status.placement.outcome==LivePlacementOutcomeKind::placed && Contains(status.state_text,"位置を確定しました（自動・開始時）") && !Contains(status.state_text,"基準内"));
    const auto current=rig.runtime->CurrentPlan();
    Check(current && current->live && current->live->baked.pose.tx_mm==1.5 && current->live->baked.pose.rx_deg==0.3,"the plan in force is baked for the estimate");
    // What is drawn is P itself, to the bit.
    CHECK(current && SameNine(current->live->baked,status.placement.mapping));
    // The base plan is untouched (a frame being drawn with it keeps its picture); the swapped plan draws B differently.
    CHECK(rig.runtime->BasePlan() && rig.runtime->BasePlan()->live->baked.pose.IsZero());
    if(current){
        const auto fixed_picture=RenderFixedPreview(rig.plan,rig.a,rig.b);
        const auto moved_picture=RenderFixedPreview(*current,rig.a,rig.b);
        CHECK(fixed_picture.image.width==moved_picture.image.width && fixed_picture.image.height==moved_picture.image.height);
        CHECK(fixed_picture.image.bgr!=moved_picture.image.bgr);
        CHECK(current->view.seam_column==rig.plan.view.seam_column && current->output_width==rig.plan.output_width);
    }
    // B's direction is now measured, and it differs from the sentence before.
    CHECK(!Contains(status.orientation.b_short,"測れていません") && status.orientation_line!=before.orientation_line);
    CHECK(Contains(status.orientation.b_short,"B: ") && status.orientation.a_short==before.orientation.a_short);
    // The log has P in the one form, once, after the bake that drew it.
    CHECK(rig.Lines("C live_placement serial=")==1);
    CHECK(Contains(rig.LogText(),"C live_placement serial="+std::to_string(status.placement.serial)+" source=auto-start "+LivePlacementFields(status.placement.mapping)+
        " condition_sha256="+kSha+" drawn=1"));

    // A move far below the line of a bake that used to be (0.5 C px = 0.9 mm here) is baked all the same: what C draws is P, not something
    // near it.
    Pose nudged=first;nudged.tx_mm+=0.001;
    rig.script={Estimate(nudged)};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto small=rig.Pass();
    CHECK(small.job_ran && small.accept==LiveAcceptKind::applied && small.rebaked);
    CHECK(rig.runtime->CurrentPlan()!=current && rig.runtime->CurrentPlan()->live->baked.pose.tx_mm==nudged.tx_mm && rig.runtime->Status().rebakes==2);
    CHECK(rig.runtime->Status().placement.source==LivePlacementSource::auto_button && Contains(rig.runtime->Status().state_text,"（自動・ボタン）"));
    // The old plan is still valid for whoever holds it.
    CHECK(current->live->baked.pose.tx_mm==1.5);
    // The same position again: P is replaced (a new serial, a new source) and nothing is baked again.
    const auto held=rig.runtime->CurrentPlan();
    const auto serial=rig.runtime->Status().placement.serial;
    rig.script={Estimate(nudged)};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto same=rig.Pass();
    CHECK(same.job_ran && same.accept==LiveAcceptKind::applied && !same.rebaked && rig.runtime->CurrentPlan()==held && rig.runtime->Status().rebakes==2);
    CHECK(rig.runtime->Status().placement.serial==serial+1 && rig.runtime->Status().placement.outcome==LivePlacementOutcomeKind::placed);
}

// The distance at the seam and over the whole of C: a lean of B moves the ends of C far more than the seam.
void SeamAndWhole(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    const ex::RegisteredMapping zero{},leaning{Pose{0.3,-0.3,0,0,0,0.3},{},std::nullopt},shifted{Pose{0,0,0,0.5,0,0},{},std::nullopt};
    // The one measure of the seam (SeamDifferencePx), in the JPEG's pixels; the whole of C is in C's pixels.
    const double seam=LiveSeamMappingDifferenceOutputPx(rig.plan,zero,leaning),whole=LiveMappingDifferencePx(rig.plan,zero,leaning,9);
    const double ratio=rig.plan.grid.pitch_mm/(25.4/rig.plan.live->raster.dpi);
    std::cerr<<"info a lean of B (0.3 degrees each way): "<<seam<<" px (the JPEG's) at the seam, "<<whole<<" px (C's) over C\n";
    CHECK(ratio>1 && seam>0 && whole*ratio>seam);
    CHECK(LiveSeamMappingDifferenceOutputPx(rig.plan,zero,zero)==0 && LiveSeamMappingDifferenceOutputPx(rig.plan,leaning,leaning)==0);
    CHECK(LiveSeamMappingDifferenceOutputPx(rig.plan,zero,shifted)>0);
    CHECK(LiveSeamMappingDifferenceOutputPx(rig.plan,zero,leaning)==LiveSeamMappingDifferenceOutputPx(rig.plan,leaning,zero));
    // The number is the one function's, not another lattice's: the same call on the context's own pieces.
    {
        const auto& c=*rig.plan.live;
        CHECK(seam==ex::SeamDifferencePx(c.model_b,c.centre_a,c.origin_mm,ex::content_registration::FrameOfRaster(c.raster),zero,leaning,c.seam_column,c.raster.height_pixels));
    }
    REJECT(LiveSeamMappingDifferenceOutputPx(FixedPreviewPlan{},zero,leaning));
    REJECT(LiveSeamMappingDifferenceOutputPx(rig.plan,zero,ex::RegisteredMapping{Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0},{},std::nullopt}));
}

// An estimate that decides nothing: P and the plan stay, the sentence says why, nothing is asked for again, and the next press is a new try.
void NotDecided(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    const Pose first{0.3,-0.2,0.5,1.5,-1.0,0.0};
    rig.PlaceFirst(first);
    const auto held=rig.runtime->CurrentPlan();
    const auto before=rig.runtime->Status();
    rig.script={Unusable("too-dark")};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    CHECK(rig.runtime->Status().state_text=="自動合成中です…");
    const auto pass=rig.Pass();
    CHECK(pass.job_ran && pass.accept==LiveAcceptKind::kept_previous && !pass.rebaked);
    auto status=rig.runtime->Status();
    CHECK(rig.runtime->CurrentPlan()==held && SameNine(status.placement.mapping,before.placement.mapping) && status.placement.serial==before.placement.serial);
    CHECK(Contains(status.state_text,"位置を決められませんでした（理由: 暗すぎます）") && Contains(status.state_text,"前の位置のままです（出どころ: 開始時の自動）"));
    CHECK(Contains(rig.LogText(),"accept=kept-previous failure=too-dark"));
    for(int k=0;k<100;++k)CHECK(!rig.Pass().job_ran);
    CHECK(rig.jobs.size()==2);
    // Another reason, another sentence; a repeating pattern is not "too few marks".
    rig.script={Unusable("coarse-ambiguous")};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    (void)rig.Pass();
    CHECK(Contains(rig.runtime->Status().state_text,"模様が繰り返して"));
    // A beyond-the-range estimate that the condition does not apply is a reason too.
    rig.script={Unusable("out-of-envelope")};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    (void)rig.Pass();
    CHECK(Contains(rig.runtime->Status().state_text,"許容の範囲の外の値でした"));
    // The press after a failure is a new try, and it places.
    rig.script={Estimate(Pose{0.2,0,0.4,2.0,0,0})};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    CHECK(rig.runtime->Status().placement.outcome==LivePlacementOutcomeKind::none);
    const auto placed=rig.Pass();
    CHECK(placed.accept==LiveAcceptKind::applied && placed.rebaked && rig.runtime->Status().placement.outcome==LivePlacementOutcomeKind::placed);
}

// A position that cannot be drawn (B pushed out of the overlap): said on screen, P and the last plan stay, and the same mapping is not baked
// again on every pass; a later estimate that can be drawn clears it.
void BakeFails(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.PlaceFirst(Pose{0,0,0.4,2.0,0.0,0.0});
    const auto held=rig.runtime->CurrentPlan();
    Check(held!=nullptr,"a plan is in force");
    const auto before=rig.runtime->Status();
    rig.script={Estimate(Pose{0,0,0.4,3000.0,0.0,0.0})};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto bad=rig.Pass();
    CHECK(bad.job_ran && bad.accept==LiveAcceptKind::applied && !bad.rebaked && bad.rebake_failed);
    auto status=rig.runtime->Status();
    CHECK(status.rebake_failures==1 && Contains(status.state_text,"位置を決められませんでした") && Contains(status.state_text,"描けませんでした") && Contains(status.state_text,"前の位置のまま"));
    CHECK(status.placement.serial==before.placement.serial && SameNine(status.placement.mapping,before.placement.mapping) && status.placement.last_failure==std::vector<std::string>{"bake-failed"});
    CHECK(rig.runtime->CurrentPlan()==held && !status.bake_failed);
    CHECK(Contains(rig.LogText(),"ok=0"));
    // The next passes have no job due and nothing wanted that is not drawn: no second bake.
    for(int k=0;k<50;++k){const auto again=rig.Pass();CHECK(!again.job_ran && !again.rebaked && !again.rebake_failed);}
    CHECK(rig.runtime->Status().rebake_failures==1);
    // A position that can be drawn clears the message.
    rig.script={Estimate(Pose{0,0,0.4,6.0,0.0,0.0})};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto good=rig.Pass();
    CHECK(good.rebaked && rig.runtime->CurrentPlan()!=held && Contains(rig.runtime->Status().state_text,"位置を確定しました（自動・ボタン）"));
}

// Frames: none ready means no job and the request waits; a job that cannot get its frames (another size, a throwing snapshot) asked nothing of
// the estimator and is asked for again; a throwing estimator is an estimate that decided nothing, with no retry.
void Frames(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    rig.script={Estimate(Pose{0,0,0.4,2.0,0.0,0.0})};
    rig.ready=false;
    for(int k=0;k<20;++k)CHECK(!rig.Pass().job_ran);
    CHECK(rig.jobs.empty() && rig.runtime->Status().placement.activity==LivePlacementActivity::requested && rig.runtime->Status().state_text=="自動合成中です…");
    rig.ready=true;rig.frame_width=100;rig.frame_height=60;
    for(int k=0;k<5;++k){const auto small=rig.Pass();CHECK(!small.job_ran);}   // the estimator is not asked, and the request stays
    CHECK(rig.jobs.empty() && rig.runtime->Status().placement.activity==LivePlacementActivity::requested);
    CHECK(rig.runtime->Status().placement.outcome==LivePlacementOutcomeKind::none && rig.runtime->Status().placement.counters.released==5);
    rig.frames.snapshot=[](render::BgrImage&,render::BgrImage&)->bool{throw std::runtime_error("no frames");};
    rig.frame_width=kW;rig.frame_height=kH;
    CHECK(!rig.Pass().job_ran && rig.jobs.empty() && rig.runtime->Status().placement.activity==LivePlacementActivity::requested);
    rig.frames.snapshot=[&](render::BgrImage& x,render::BgrImage& y){x=rig.a;y=rig.b;return true;};
    const auto ran=rig.Pass();
    CHECK(ran.job_ran && ran.accept==LiveAcceptKind::applied && rig.jobs.size()==1 && rig.runtime->Status().placement.source==LivePlacementSource::auto_start);
    // A throwing estimator: an estimate that decided nothing; P stays; no retry.
    rig.estimator=[&](const LiveJob&,const render::BgrImage&,const render::BgrImage&)->LiveEstimateOutcome{throw std::runtime_error("scripted failure");};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    const auto thrown=rig.Pass();
    CHECK(thrown.job_ran && thrown.accept==LiveAcceptKind::kept_previous);
    CHECK(Contains(rig.LogText(),"estimator-exception") && Contains(rig.LogText(),"scripted failure"));
    CHECK(Contains(rig.runtime->Status().state_text,"位置を決められませんでした（理由: 推定の処理が途中で失敗しました）"));
    for(int k=0;k<20;++k)CHECK(!rig.Pass().job_ran);
}

// A worker that is stopped while a job runs: the job did not estimate anything, so the request is asked for again and no failure is recorded.
void Stopped(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    int calls=0;
    rig.estimator=[&](const LiveJob&,const render::BgrImage&,const render::BgrImage&){
        ++calls;
        if(calls==1)return Unusable(std::string(kLiveEstimateStoppedReason).c_str());
        return Estimate(Pose{0,0,0.4,2.0,0.0,0.0});
    };
    const auto stopped=rig.runtime->RunPass(rig.frames,rig.estimator,[]{return true;});
    CHECK(stopped.job_ran && stopped.accept==LiveAcceptKind::discarded_stale && !stopped.rebaked);
    auto status=rig.runtime->Status();
    CHECK(status.placement.activity==LivePlacementActivity::requested && status.placement.outcome==LivePlacementOutcomeKind::none && status.placement.counters.not_decided==0);
    CHECK(rig.Lines("C live_registration job=")==0);
    const auto next=rig.Pass();
    CHECK(next.job_ran && next.accept==LiveAcceptKind::applied && next.rebaked && calls==2);
}

// A condition without registration.live: nothing is estimated and the button is refused, the sentence says so, A's direction still shows; a shot
// still sets the position that C is drawn with.
void Disabled(){
    Rig rig(false);
    if(!Usable(rig)){Check(false,"plan");return;}
    CHECK(!rig.runtime->Enabled());
    rig.runtime->PublishBase(rig.plan);
    rig.script={Estimate(Pose{0,0,0.4,2.0,0.0,0.0})};
    const auto pass=rig.Pass();
    CHECK(!pass.job_ran && rig.jobs.empty() && !rig.runtime->CurrentPlan());
    const auto status=rig.runtime->Status();
    CHECK(!status.live_enabled && Contains(status.state_text,"無効") && Contains(status.orientation.b_short,"測れていません") && Contains(status.orientation.a_short,"A（校正時）: "));
    CHECK(!Contains(status.orientation.a_short,"測れていません"));
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::disabled);
    for(int k=0;k<50;++k)CHECK(!rig.Pass().job_ran);
    CHECK(rig.runtime->Status().placement.activity==LivePlacementActivity::idle && rig.runtime->Status().state_text==status.state_text);
    const ex::RegisteredMapping jpeg{Pose{0.2,0,0.3,1.0,0.5,0},{},std::nullopt};
    CHECK(rig.runtime->ApplyCapture(jpeg,"G2",kSha));
    const auto shot=rig.Pass();
    CHECK(!shot.job_ran && shot.rebaked && rig.runtime->CurrentPlan() && SameNine(rig.runtime->CurrentPlan()->live->baked,jpeg));
    CHECK(Contains(rig.runtime->Status().state_text,"撮影の後の自動調整で位置を確定しました（G2）") && rig.jobs.empty());
    // The button is refused for this condition, so the sentence does not tell the person to press it.
    CHECK(!Contains(rig.runtime->Status().state_text,"自動位置合わせ"));
}

// A new plan for another frame size drops the baked plan; P stays and is baked into the new plan, and no estimate is asked for.
void Republish(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    const Pose first{0,0,0.4,2.0,0.0,0.0};
    rig.PlaceFirst(first);
    CHECK(rig.runtime->CurrentPlan()!=nullptr);
    const auto serial=rig.runtime->Status().placement.serial;
    const auto other=PlanFixedPreviewV3(rig.condition,ex::MountOrientation::normal,320,214);
    Check(!other.skip && other.live,"a plan for another size");
    if(other.skip || !other.live)return;
    rig.runtime->PublishBase(other);
    CHECK(!rig.runtime->CurrentPlan() && rig.runtime->BasePlan()->view_width==320);
    CHECK(rig.runtime->Status().placement.activity==LivePlacementActivity::idle && rig.runtime->Status().placement.serial==serial);
    rig.frame_width=320;rig.frame_height=214;
    const auto again=rig.Pass();
    CHECK(!again.job_ran && again.rebaked && rig.runtime->CurrentPlan() && rig.runtime->CurrentPlan()->view_width==320 && rig.jobs.size()==1);
    CHECK(SameNine(rig.runtime->CurrentPlan()->live->baked,ex::RegisteredMapping{first,{},std::nullopt}) && rig.runtime->Status().placement.serial==serial);
    CHECK(Contains(rig.runtime->Status().state_text,"位置を確定しました（自動・開始時）"));
    rig.runtime->ClearBase();
    CHECK(!rig.runtime->BasePlan() && !rig.runtime->CurrentPlan());
    CHECK(!rig.Pass().job_ran);
}

// The position carries to the next session; a condition that differs starts again from the condition's position and asks once.
void NextSession(){
    Rig first;
    if(!Usable(first)){Check(false,"plan");return;}
    const Pose pose{0.3,-0.2,0.5,1.5,-1.0,0.0};
    first.PlaceFirst(pose);
    const auto carried=first.runtime->PlacementMachine();
    CHECK(carried.Serial()==first.runtime->Status().placement.serial && carried.Source()==LivePlacementSource::auto_start);
    // The same condition: P is what it was, drawn into the new plan, with no estimate.
    {
        Rig next(true,Pose{},ex::MountOrientation::normal,carried);
        next.runtime->PublishBase(next.plan);
        auto status=next.runtime->Status();
        CHECK(status.placement.activity==LivePlacementActivity::idle && status.placement.serial==carried.Serial() && status.placement.source==LivePlacementSource::auto_start);
        CHECK(Contains(status.state_text,"位置を確定しました（自動・開始時）") && !Contains(status.orientation.b_short,"測れていません"));
        const auto pass=next.Pass();
        CHECK(!pass.job_ran && pass.rebaked && next.jobs.empty() && next.runtime->CurrentPlan() && SameNine(next.runtime->CurrentPlan()->live->baked,ex::RegisteredMapping{pose,{},std::nullopt}));
        for(int k=0;k<50;++k)CHECK(!next.Pass().job_ran);
        CHECK(next.Lines("C live_placement serial=")==1 && next.jobs.empty());
        // The session after that carries it on.
        const auto again=next.runtime->PlacementMachine();
        CHECK(again.Serial()==carried.Serial() && SameNine(again.Placement(),carried.Placement()));
    }
    // Another condition: the condition's position, one estimate, for that reason.
    {
        Rig other(true,Pose{0.1,0,0,0,0,0},ex::MountOrientation::normal,carried,std::string(64,'9'));
        other.runtime->PublishBase(other.plan);
        const auto status=other.runtime->Status();
        CHECK(status.placement.source==LivePlacementSource::fixed_condition && status.placement.activity==LivePlacementActivity::requested && status.placement.mapping.pose.rx_deg==0.1);
        other.script={Estimate(Pose{0.1,0,0.2,1.0,0,0})};
        const auto pass=other.Pass();
        CHECK(pass.job_ran && other.jobs.size()==1 && other.jobs[0].full_reason==LiveFullReason::condition_reloaded);
    }
    // A runtime cannot be made without the condition's sha.
    bool threw=false;
    try{Rig bad(true,Pose{},ex::MountOrientation::normal,std::nullopt,std::string());}catch(const std::invalid_argument&){threw=true;}
    CHECK(threw);
}

// What a shot does: P becomes the mapping that the JPEG was made with, bit for bit and without layer 2, the live C is drawn with it, nothing
// is asked for, and the sentence and the log say what changed.
void AfterCapture(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    const Pose live_pose{0.3,-0.2,0.5,1.5,-1.0,0.0};
    rig.PlaceFirst(live_pose);
    const auto live=rig.runtime->Status();
    const auto before_plan=rig.runtime->CurrentPlan();
    ex::RegisteredMapping jpeg{Pose{0.31,-0.12,0.52,2.25,-1.125,0.0},ex::SurfacePlane{3.5,0.0015,-0.0025},std::nullopt};
    ex::ParallaxProfile profile;
    profile.q_px={0.0,0.5,0.0};profile.row_spacing_px=10;profile.a_px=4;profile.l_px=8;
    jpeg.parallax=profile;
    CHECK(!rig.runtime->ApplyCapture(jpeg,"G1",std::string(64,'9'),0.5));   // not for this condition
    CHECK(rig.runtime->Status().placement.serial==live.placement.serial && rig.runtime->Status().placement.source==LivePlacementSource::auto_start);
    CHECK(rig.runtime->ApplyCapture(jpeg,"G1",kSha,0.5));   // layer 2's largest shift, 0.5 px, as the record's summary gives it
    auto status=rig.runtime->Status();
    CHECK(status.placement.source==LivePlacementSource::after_capture && status.placement.serial==live.placement.serial+1);
    CHECK(SameNine(status.placement.mapping,jpeg) && !status.placement.mapping.parallax);
    // The sentence: the level, how far the live C moved from where it was before the shot along the seam (in the JPEG's pixels), that the live C
    // is now at the JPEG's position, and layer 2 with its largest shift (what the live C does not show).
    const ex::RegisteredMapping jpeg_without_layer_two{jpeg.pose,jpeg.surface,std::nullopt};
    const double distance=LiveSeamMappingDifferenceOutputPx(rig.plan,ex::RegisteredMapping{live_pose,{},std::nullopt},jpeg_without_layer_two);
    CHECK(status.placement.capture && status.placement.capture->difference_px && *status.placement.capture->difference_px==distance && distance>0);
    // The live C against the JPEG's position after the shot: no distance (the number in the sentence is not that one).
    CHECK(LiveSeamMappingDifferenceOutputPx(rig.plan,status.placement.mapping,jpeg_without_layer_two)==0);
    {
        std::ostringstream moved;moved.imbue(std::locale::classic());moved<<std::fixed<<std::setprecision(1)<<distance;
        CHECK(Contains(status.state_text,"撮影の後の自動調整で位置を確定しました（G1）。撮影の前のライブの位置から継ぎ目で最大 "+moved.str()+" px（JPEGの画素）動かしました。今のライブのCはJPEGと同じ位置です"));
    }
    CHECK(!Contains(status.state_text,"違います") && !Contains(status.state_text,"ライブの位置と継ぎ目"));
    CHECK(Contains(status.state_text,"JPEGの局所の補正（最大 0.5 px）はライブのCには出ません") && Contains(status.state_text,"自動位置合わせ"));
    CHECK(status.placement.capture && status.placement.capture->layer_two_dropped && status.placement.capture->layer_two_max_px && *status.placement.capture->layer_two_max_px==0.5);
    // Not drawn yet: until the next pass the plan is the one before, and nothing is asked for.
    CHECK(rig.runtime->CurrentPlan()==before_plan && rig.jobs.size()==1);
    const auto pass=rig.Pass();
    CHECK(!pass.job_ran && pass.rebaked && rig.jobs.size()==1);
    const auto drawn=rig.runtime->CurrentPlan();
    CHECK(drawn && drawn!=before_plan && SameNine(drawn->live->baked,jpeg) && !drawn->live->baked.parallax);
    for(int k=0;k<100;++k){const auto idle=rig.Pass();CHECK(!idle.job_ran && !idle.rebaked);}
    CHECK(rig.jobs.size()==1 && rig.runtime->Status().placement.serial==live.placement.serial+1);

    // The live C's mapping IS the JPEG's: B's table of the plan in force is the maker's mapping (the one the JPEG is made with) for the mapping
    // of the shot, scaled to the live view, at every sample of C (layer 2 is the one thing the live C does not draw).
    {
        const auto& condition=rig.condition.condition;
        const auto& parameters=rig.condition.profile.parameters;
        const cr::PoseModel model(parameters.camera_b);
        const auto maker=ex::RegisteredDocumentOutputToCamera(condition.raster,parameters.camera_a,parameters.camera_b,model,
            ex::RegisteredMapping{jpeg.pose,jpeg.surface,std::nullopt},condition.seam.column);
        const double pitch=25.4/condition.raster.dpi;
        double worst=0;int n=0,moved=0;
        for(std::uint32_t y=0;y<drawn->output_height;y+=3)for(std::uint32_t x=0;x<drawn->output_width;x+=3){
            const render::Point at{(drawn->grid.left_mm+(x+.5)*drawn->grid.pitch_mm-condition.raster.region_um.left/1000.0)/pitch-.5,
                (drawn->grid.top_mm+(y+.5)*drawn->grid.pitch_mm-condition.raster.region_um.top/1000.0)/pitch-.5};
            render::Point raw{},got{},fixed_b{};
            if(!maker(at,raw) || !drawn->mapped->camera_b({double(x),double(y)},got))continue;
            const render::Point want{raw.x*drawn->scale_x,raw.y*drawn->scale_y};
            worst=std::max(worst,std::hypot(want.x-got.x,want.y-got.y));++n;
            if(rig.plan.mapped->camera_b({double(x),double(y)},fixed_b) && std::hypot(fixed_b.x-got.x,fixed_b.y-got.y)>0.5)++moved;
        }
        std::cerr<<"info the live C's table against the JPEG's maker: "<<n<<" samples, worst "<<worst<<" C px, "<<moved<<" samples moved by the shot from the condition's table\n";
        CHECK(n>100 && worst<1e-6 && moved>100);
    }

    // The log: what the shot did, once, and P in the one form after it was drawn; the JPEG's side writes the same nine numbers with the same
    // function.
    CHECK(rig.Lines("C live_placement_after_capture ")==1 && Contains(rig.LogText(),"level=G1") && Contains(rig.LogText(),"before_source=auto-start") &&
        Contains(rig.LogText(),LivePlacementFields(ex::RegisteredMapping{live_pose,{},std::nullopt},"before_")) &&
        Contains(rig.LogText(),LivePlacementLayerTwoFields(true,0.5)+" seam_difference_px="+LivePlacementNumber(distance)+" ") && Contains(rig.LogText(),"layer_two_dropped=1 layer_two_max_px=0.5 "));
    const std::string expected="C live_placement serial="+std::to_string(live.placement.serial+1)+" source=after-capture "+LivePlacementFields(ex::RegisteredMapping{jpeg.pose,jpeg.surface,std::nullopt})+
        " condition_sha256="+kSha+" drawn=1";
    CHECK(Contains(rig.LogText(),expected));
    CHECK(rig.Lines("C live_placement serial=")==2);

    // A level whose position the condition gave: the sentence says so, and the difference is the one from P before.
    const ex::RegisteredMapping condition_position{Pose{},{},std::nullopt};
    CHECK(rig.runtime->ApplyCapture(condition_position,"G4",kSha));
    (void)rig.Pass();
    status=rig.runtime->Status();
    CHECK(Contains(status.state_text,"JPEGは条件の位置で作りました。ライブのCもその位置へ移しました（撮影の前のライブの位置から継ぎ目で最大 ") && Contains(status.state_text,"px（JPEGの画素）動かしました）") &&
        !Contains(status.state_text,"局所の補正") && !Contains(status.state_text,"違います"));
    CHECK(SameNine(rig.runtime->CurrentPlan()->live->baked,condition_position) && rig.jobs.size()==1);
    CHECK(Contains(rig.LogText(),"level=G4 ") && Contains(rig.LogText(),"layer_two_dropped=0 layer_two_max_px=0 "));
    // The coarse level.
    CHECK(rig.runtime->ApplyCapture(ex::RegisteredMapping{Pose{0,0,0,0.5,0.25,0},ex::SurfacePlane{1.0,0,0},std::nullopt},"G3",kSha));
    CHECK(Contains(rig.runtime->Status().state_text,"JPEGは目安の位置（G3）で作りました。ライブのCもその位置へ移しました（撮影の前のライブの位置から継ぎ目で最大 "));
    // Layer 2 that the record gave no usable number for (not positive, not finite, or none): the sentence says C does not draw it, with no number,
    // and the log says "unknown". A layer 2 that moves nothing is not dropped.
    for(const std::optional<double> number:{std::optional<double>{},std::optional<double>{0.0},std::optional<double>{-1.0},std::optional<double>{std::numeric_limits<double>::quiet_NaN()}}){
        CHECK(rig.runtime->ApplyCapture(jpeg,"G2",kSha,number));
        const auto text=rig.runtime->Status().state_text;
        CHECK(Contains(text,"。局所の補正はライブのCには出ません") && !Contains(text,"局所の補正（") && !Contains(text,"nan"));
    }
    CHECK(Contains(rig.LogText(),"layer_two_dropped=1 layer_two_max_px=unknown "));
    {
        ex::RegisteredMapping flat=jpeg;
        flat.parallax=ex::ParallaxProfile{};flat.parallax->q_px={0.0,0.0};
        CHECK(rig.runtime->ApplyCapture(flat,"G2",kSha,0.5));
        CHECK(!Contains(rig.runtime->Status().state_text,"局所の補正") && !rig.runtime->Status().placement.capture->layer_two_max_px);
    }
    // The button after a shot is a new estimate; it replaces what the shot set.
    rig.script={Estimate(Pose{0.2,0,0.4,2.0,0,0})};
    CHECK(rig.runtime->RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
    (void)rig.Pass();
    CHECK(rig.runtime->Status().placement.source==LivePlacementSource::auto_button && !rig.runtime->Status().placement.capture);
}

// A shot before any plan exists: the distance is measured when the plan is published, and a runtime that begins from the carried machine
// draws it into its own plan, with no estimate.
void AfterCaptureCarried(){
    Rig first;
    if(!Usable(first)){Check(false,"plan");return;}
    const ex::RegisteredMapping jpeg{Pose{0.2,-0.1,0.4,2.0,-1.0,0.1},{},std::nullopt};
    // No plan yet: the shot cannot be measured, and the sentence leaves the number out.
    CHECK(first.runtime->ApplyCapture(jpeg,"G2",kSha));
    auto status=first.runtime->Status();
    CHECK(status.placement.capture && !status.placement.capture->difference_px && Contains(status.state_text,"撮影の後の自動調整で位置を確定しました（G2）。今のライブのCはJPEGと同じ位置です") &&
        !Contains(status.state_text,"動かしました") && !Contains(status.state_text,"動いていません"));
    const auto carried=first.runtime->PlacementMachine();
    Rig next(true,Pose{},ex::MountOrientation::normal,carried);
    CHECK(next.runtime->Status().placement.source==LivePlacementSource::after_capture && next.runtime->Status().placement.activity==LivePlacementActivity::idle);
    next.runtime->PublishBase(next.plan);
    status=next.runtime->Status();
    CHECK(status.placement.capture && status.placement.capture->difference_px && *status.placement.capture->difference_px>0 && Contains(status.state_text,"px（JPEGの画素）動かしました"));
    const auto pass=next.Pass();
    CHECK(!pass.job_ran && pass.rebaked && next.jobs.empty() && SameNine(next.runtime->CurrentPlan()->live->baked,jpeg));
    CHECK(Contains(next.LogText(),"C live_placement serial="+std::to_string(carried.Serial())+" source=after-capture "+LivePlacementFields(jpeg)));
}

// A position that the shot set but that cannot be drawn into this plan: the sentence says so and the plan in force stays; no retry.
void CaptureCannotBeDrawn(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    CHECK(rig.runtime->ApplyCapture(ex::RegisteredMapping{Pose{0,0,0.4,3000.0,0.0,0.0},{},std::nullopt},"G1",kSha));
    const auto bad=rig.Pass();
    CHECK(!bad.job_ran && !bad.rebaked && bad.rebake_failed);
    const auto status=rig.runtime->Status();
    CHECK(status.bake_failed && Contains(status.state_text,"撮影JPEGの位置ではCを描けませんでした") && !rig.runtime->CurrentPlan());
    for(int k=0;k<50;++k){const auto again=rig.Pass();CHECK(!again.rebaked && !again.rebake_failed);}
    CHECK(rig.runtime->Status().rebake_failures==1);
    // The next shot clears it.
    CHECK(rig.runtime->ApplyCapture(ex::RegisteredMapping{Pose{0,0,0.4,2.0,0.0,0.0},{},std::nullopt},"G2",kSha));
    CHECK(rig.Pass().rebaked && !rig.runtime->Status().bake_failed);
}

// The same calls give the same sentences and plans.
void Determinism(){
    std::vector<std::string> first,second;
    for(auto* texts:{&first,&second}){
        Rig rig;
        if(!Usable(rig)){Check(false,"plan");return;}
        rig.runtime->PublishBase(rig.plan);
        rig.script={Estimate(Pose{0.2,0,0.4,2.0,0.0,0.0}),Estimate(Pose{0.2,0,0.4,2.1,0.0,0.0}),Unusable("too-few-points")};
        for(int k=0;k<4;++k){
            if(k>0)(void)rig.runtime->RequestEstimate(LiveEstimateAsk::button);
            (void)rig.Pass();
            const auto status=rig.runtime->Status();
            texts->push_back(status.state_text+"|"+status.orientation_line+"|"+std::to_string(status.rebakes)+"|"+std::to_string(status.placement.serial));
        }
    }
    CHECK(first==second && !first.empty());
}

// The worker thread: passes until stopped; one job for the start and one for each press, however long it runs; no estimate after the stop;
// the count of workers returns to 0.
void Thread(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    std::atomic<int> calls{0};
    std::atomic<bool> halted{false};
    LiveRegistrationRuntime::Config config;
    config.base_pose=Pose{};config.live_enabled=true;config.condition_sha256=kSha;
    LiveRegistrationRuntime runtime(config,rig.condition.profile.parameters);
    runtime.PublishBase(rig.plan);
    LiveFrameSource frames;
    frames.ready=[]{return true;};
    frames.snapshot=[&](render::BgrImage& x,render::BgrImage& y){x=rig.a;y=rig.b;return true;};
    const LiveEstimator estimator=[&](const LiveJob& job,const render::BgrImage&,const render::BgrImage&){
        ++calls;
        return Estimate(Pose{0,0,0.4,2.0+(job.id%2)*3.0,0.0,0.0});
    };
    const auto wait_for=[&](auto ready){
        const auto until=std::chrono::steady_clock::now()+10000ms;
        while(!ready() && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(2ms);
        return ready();
    };
    {
        std::jthread worker([&](std::stop_token stop){runtime.Work(stop,[&]{return halted.load();},frames,estimator,5ms);});
        // Concurrent readers: the loop of C and the window. The plan they get is always a complete one of the same size.
        bool consistent=true;
        const auto read=[&](std::chrono::milliseconds span){
            const auto until=std::chrono::steady_clock::now()+span;
            while(std::chrono::steady_clock::now()<until){
                const auto status=runtime.Status();
                const auto current=runtime.CurrentPlan();
                const auto base=runtime.BasePlan();
                if(current){
                    consistent=consistent && current->output_width==base->output_width && current->output_height==base->output_height && current->mapped && current->live;
                    const auto picture=RenderFixedPreview(*current,rig.a,rig.b);
                    consistent=consistent && picture.image.width==base->output_width;
                }
                consistent=consistent && !status.state_text.empty() && !status.orientation_line.empty();
                std::this_thread::sleep_for(3ms);
            }
        };
        CHECK(wait_for([&]{return runtime.Status().placement.source==LivePlacementSource::auto_start;}));
        read(600ms);
        CHECK(calls.load()==1);   // the follow-up that used to run every half second does not
        CHECK(runtime.RequestEstimate(LiveEstimateAsk::button)==LiveAskResult::accepted);
        CHECK(wait_for([&]{return runtime.Status().placement.source==LivePlacementSource::auto_button;}));
        read(600ms);
        Check(consistent,"readers see complete plans and sentences while the worker bakes");
        CHECK(calls.load()==2 && runtime.WorkersRunning()==1);
        halted=true;
    }
    CHECK(runtime.WorkersRunning()==0);
    const int stopped=calls.load();
    std::this_thread::sleep_for(100ms);
    CHECK(calls.load()==stopped && stopped==2);
    CHECK(runtime.Status().rebakes==2 && runtime.Status().placement.counters.jobs==2);
    // request_stop alone ends it as well.
    halted=false;
    {
        std::jthread worker([&](std::stop_token stop){runtime.Work(stop,[]{return false;},frames,estimator,5ms);});
        std::this_thread::sleep_for(60ms);
        worker.request_stop();
    }
    CHECK(runtime.WorkersRunning()==0 && calls.load()==2);
}

// An exception inside a pass does not end the worker and is logged once.
void WorkerSurvives(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    std::atomic<int> asked{0};
    LiveFrameSource frames;
    frames.ready=[&]()->bool{++asked;throw std::runtime_error("ready failed");};
    frames.snapshot=[](render::BgrImage&,render::BgrImage&){return false;};
    std::atomic<bool> halted{false};
    {
        std::jthread worker([&](std::stop_token stop){rig.runtime->Work(stop,[&]{return halted.load();},frames,rig.estimator,2ms);});
        std::this_thread::sleep_for(100ms);
        CHECK(rig.runtime->WorkersRunning()==1);
        halted=true;
    }
    CHECK(asked.load()>=3 && rig.runtime->WorkersRunning()==0);
    size_t lines=0;for(const auto& line:rig.log)if(Contains(line,"pass_failure=ready failed"))++lines;
    CHECK(lines==1);
}

void Rejects(){
    bool threw=false;
    try{LiveRegistrationRuntime runtime(LiveRegistrationRuntime::Config{},MakeCondition(true,Pose{},ex::MountOrientation::normal).profile.parameters);}
    catch(const std::invalid_argument&){threw=true;}
    CHECK(threw);   // no condition sha
    Rig rig;
    threw=false;
    try{(void)rig.runtime->RunPass(rig.frames,LiveEstimator{});}catch(const std::invalid_argument&){threw=true;}
    CHECK(threw);
    threw=false;
    try{(void)MakeLiveEstimator(nullptr,kLiveTrackerDesignDraft,ConnectedLiveEstimatorEntrance());}catch(const std::invalid_argument&){threw=true;}
    CHECK(threw);
    threw=false;
    try{(void)MakeLiveEstimator([]{return std::shared_ptr<const FixedPreviewPlan>();},kLiveTrackerDesignDraft,nullptr);}catch(const std::invalid_argument&){threw=true;}
    CHECK(threw);
    threw=false;
    try{LiveRegistrationRuntime::Config config;config.condition_sha256=kSha;config.base_pose=Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0};
        LiveRegistrationRuntime runtime(config,MakeCondition(true,Pose{},ex::MountOrientation::normal).profile.parameters);}
    catch(const std::invalid_argument&){threw=true;}
    CHECK(threw);   // a base pose that is not finite
}
}

int main(){
    try{
        Design();
        RigIsUsable();
        NotConnected();
        AskedOnlyByRequest();
        PlacesAfterTheDraw();
        SeamAndWhole();
        NotDecided();
        BakeFails();
        Frames();
        Stopped();
        Disabled();
        Republish();
        NextSession();
        AfterCapture();
        AfterCaptureCarried();
        CaptureCannotBeDrawn();
        Determinism();
        Thread();
        WorkerSurvives();
        Rejects();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
