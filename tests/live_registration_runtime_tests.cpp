// The worker of the live C's registration (#417 LC): what runs beside the window. A job when one is due, the bake of camera B's table
// when the mapping in force has moved, the swap of the plan that C draws with, and the status that the window shows, with a scripted
// estimator and a clock that the test moves. The rig is two pinhole cameras built here from known poses; no file, camera or window.
#include "a0/phase0/experimental/live_registration_runtime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
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

LiveEstimateOutcome Estimate(const Pose& pose,bool follow,double change_px=0.1,double brightness=100){
    LiveEstimateOutcome o;
    o.usable=true;o.pose=pose;
    if(follow){o.change_px=change_px;o.inlier_fraction=0.9;}
    o.band_brightness=brightness;
    return o;
}
LiveEstimateOutcome Unusable(const char* reason){LiveEstimateOutcome o;o.reasons.emplace_back(reason);return o;}

// Everything one case needs: a runtime on a moved clock, frames, and a scripted estimator that remembers what it was asked.
struct Rig {
    double now{0};
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
    // `policy`: the draft design's significance line for replacing the estimate in force. Off by default: the scripted outcomes of most cases
    // move between poses with the change of a small step and expect every estimate that is taken to be drawn; the policy has its own case.
    explicit Rig(bool live=true,const Pose& base=Pose{},ex::MountOrientation mount=ex::MountOrientation::normal,bool policy=false)
        :condition(MakeCondition(live,base,mount)){
        LiveRegistrationRuntime::Config config;
        if(!policy){config.design.tracker.adopt_threshold_px=0;config.design.tracker.adopt_sigma_factor=0;}
        config.base_pose=base;config.live_enabled=live;config.mount=mount;
        config.clock=[this]{return now;};
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
    LivePassResult Pass(double at){now=at;return runtime->RunPass(frames,estimator);}
    std::string LogText() const {std::string all;for(const auto& line:log)all+=line+"\n";return all;}
};

bool Usable(const Rig& rig){return !rig.plan.skip && rig.plan.live;}

void Design(){
    const auto& d=kLiveTrackerDesignDraft;
    CHECK(d.tracker.follow_interval_seconds==0.5 && d.tracker.hold_seconds==3.0 && d.tracker.full_retry_seconds==1.0);
    CHECK(d.tracker.follow_change_limit_px==0.5 && d.tracker.inlier_fraction_floor==0.6 && d.tracker.brightness_change_limit==20.0 && d.tracker.bake_threshold_px==0.5);
    CHECK(d.tracker.adopt_threshold_px==d.tracker.follow_change_limit_px && d.tracker.adopt_sigma_factor==1.0);
    CHECK(d.difference_samples_per_axis==9);
    ValidateLiveTrackerSettings(d.tracker);   // throws if a value is unset or out of range
    LiveRegistrationRuntime::Config config;
    CHECK(config.design.tracker.hold_seconds==d.tracker.hold_seconds && config.design.difference_samples_per_axis==9);
}

void RigIsUsable(){
    for(const auto mount:{ex::MountOrientation::normal,ex::MountOrientation::rotated_180}){
        Rig rig(true,Pose{},mount);
        Check(Usable(rig),("the synthetic rig is planned: "+rig.plan.detail).c_str());
    }
}

// Not connected: the connection returns an estimate that cannot be used. C stays on the fixed mapping, the sentence says so, B's direction
// is not measured, A's is the calibration's. Nothing is baked and the retry waits for its interval.
void NotConnected(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    rig.estimator=MakeLiveEstimator([&]{return rig.runtime->BasePlan();},rig.runtime->Design(),ConnectedLiveEstimatorEntrance());
    auto status=rig.runtime->Status();
    CHECK(status.live_enabled && status.view.state==LiveRegistrationState::fixed && Contains(status.state_text,"待って"));
    const auto first=rig.Pass(0);
    CHECK(first.job_ran && first.kind==LiveJobKind::full && first.accept==LiveAcceptKind::kept_previous && !first.rebaked);
    status=rig.runtime->Status();
    CHECK(status.view.state==LiveRegistrationState::fixed && status.view.fixed_reason=="estimate-failed");
    CHECK(Contains(status.state_text,"未接続") && Contains(status.state_text,"固定の位置"));
    CHECK(Contains(status.orientation.b_short,"B: 向きを測れていません"));
    CHECK(Contains(status.orientation.a_short,"A（校正時）: 回転なし") || Contains(status.orientation.a_short,"A（校正時）: 右回りに") || Contains(status.orientation.a_short,"A（校正時）: 左回りに"));
    CHECK(Contains(status.orientation_line,"Aに対する今の値") && Contains(status.orientation_line,"A（校正時）") && Contains(status.orientation_line,"B: 向きを測れていません"));
    CHECK(!rig.runtime->CurrentPlan() && status.rebakes==0 && !status.bake_failed);
    CHECK(Contains(rig.LogText(),"reason=session-start") && Contains(rig.LogText(),"live-estimator-unavailable"));
    // The retry waits for full_retry_seconds (1 s): no job at 0.4, one at 1.1.
    CHECK(!rig.Pass(0.4).job_ran);
    const auto second=rig.Pass(1.1);
    CHECK(second.job_ran && second.kind==LiveJobKind::full);
    // The same result is one line of the log, not one per retry.
    size_t lines=0;for(const auto& line:rig.log)if(Contains(line,"C live_registration job="))++lines;
    CHECK(lines==1);
    // The connection is one function: an entrance that the test supplies receives every input the entrance of S2 is to get.
    bool seen=false;
    rig.runtime->RequestFull(LiveFullReason::after_capture);
    rig.estimator=MakeLiveEstimator([&]{return rig.runtime->BasePlan();},rig.runtime->Design(),[&](const LiveEstimatorInputs& in){
        seen=true;
        CHECK(in.camera_a==&rig.a || in.camera_a->width==kW);
        CHECK(in.camera_b!=nullptr && in.camera_b->width==kW && in.camera_b->height==kH);
        CHECK(in.grid.width==rig.plan.grid.width && in.grid.height==rig.plan.grid.height && in.grid.pitch_mm==rig.plan.grid.pitch_mm &&
            in.grid.left_mm==rig.plan.grid.left_mm && in.grid.top_mm==rig.plan.grid.top_mm);
        CHECK(in.raster.width_pixels==rig.condition.condition.raster.width_pixels && in.seam_column==rig.condition.condition.seam.column);
        CHECK(in.limits.has_value() && in.limits->minimum_unique_blocks==3);
        CHECK(in.design.tracker.bake_threshold_px==rig.runtime->Design().tracker.bake_threshold_px && in.design.difference_samples_per_axis==9);
        CHECK(in.job!=nullptr && in.job->kind==LiveJobKind::full && in.job->full_reason==LiveFullReason::after_capture && in.job->base_pose.IsZero());
        render::Point at{},fixed_b{};
        CHECK(in.camera_a_on_grid && in.camera_a_on_grid({10,10},at));
        CHECK(in.camera_b_on_grid && in.camera_b_on_grid(ex::RegisteredMapping{})({10,10},fixed_b));
        render::Point table_b{};
        CHECK(rig.plan.mapped->camera_b({10,10},table_b) && table_b.x==fixed_b.x && table_b.y==fixed_b.y);   // the fixed mapping is the plan's table
        return LiveEstimatorNotConnected(in);
    });
    rig.Pass(3);
    CHECK(seen);
    // Without a plan the estimate cannot be made.
    LiveEstimator no_plan=MakeLiveEstimator([]{return std::shared_ptr<const FixedPreviewPlan>();},rig.runtime->Design(),ConnectedLiveEstimatorEntrance());
    LiveJob job;
    const auto outcome=no_plan(job,rig.a,rig.b);
    CHECK(!outcome.usable && !outcome.reasons.empty() && outcome.reasons[0]=="no-registered-plan");
}

// A usable estimate: the worker bakes B's table, the plan that C draws with is swapped, and the status and the direction follow.
void Follows(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    const Pose first{0.3,-0.2,0.5,1.5,-1.0,0.0};
    rig.script={Estimate(first,false)};
    const auto before=rig.runtime->Status();
    const auto pass=rig.Pass(0);
    CHECK(pass.job_ran && pass.accept==LiveAcceptKind::applied && pass.rebaked && !pass.rebake_failed);
    const auto status=rig.runtime->Status();
    CHECK(status.view.state==LiveRegistrationState::tracking && Contains(status.state_text,"目安") && status.rebakes==1);
    const auto current=rig.runtime->CurrentPlan();
    Check(current && current->live && current->live->baked.pose.tx_mm==1.5 && current->live->baked.pose.rx_deg==0.3,"the plan in force is baked for the estimate");
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

    // Under tau_bake (0.5 C px = 0.9 mm here): applied, not baked; the plan in force is the same object.
    Pose nudged=first;nudged.tx_mm+=0.1;
    rig.script={Estimate(nudged,true,0.05)};
    const auto small=rig.Pass(0.6);
    CHECK(small.job_ran && small.kind==LiveJobKind::follow && small.accept==LiveAcceptKind::applied && !small.rebaked);
    CHECK(rig.runtime->CurrentPlan()==current && rig.runtime->Status().rebakes==1);
    // Over it: baked again, a new object; the old plan is still valid for whoever holds it.
    Pose far=first;far.tx_mm+=6.0;
    rig.script={Estimate(far,true,0.4)};
    const auto large=rig.Pass(1.2);
    CHECK(large.job_ran && large.accept==LiveAcceptKind::applied && large.rebaked);
    CHECK(rig.runtime->CurrentPlan()!=current && rig.runtime->CurrentPlan()->live->baked.pose.tx_mm==far.tx_mm && current->live->baked.pose.tx_mm==1.5);
    CHECK(rig.runtime->Status().rebakes==2);
}

// The distance at the seam and over the whole of C: a lean of B moves the ends of C far more than the seam.
void SeamAndWhole(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    const ex::RegisteredMapping zero{},leaning{Pose{0.3,-0.3,0,0,0,0.3},{},std::nullopt},shifted{Pose{0,0,0,0.5,0,0},{},std::nullopt};
    const double seam=LiveSeamMappingDifferencePx(rig.plan,zero,leaning,kLiveSeamDifferenceRows),whole=LiveMappingDifferencePx(rig.plan,zero,leaning,9);
    std::cerr<<"info a lean of B (0.3 degrees each way): "<<seam<<" px at the seam, "<<whole<<" px over C\n";
    CHECK(seam>0 && whole>seam);
    // A shift of the baseline is the same at the seam as anywhere, to within the lattice.
    CHECK(LiveSeamMappingDifferencePx(rig.plan,zero,zero,kLiveSeamDifferenceRows)==0 && LiveSeamMappingDifferencePx(rig.plan,leaning,leaning,5)==0);
    CHECK(LiveSeamMappingDifferencePx(rig.plan,zero,shifted,kLiveSeamDifferenceRows)>0);
    CHECK(LiveSeamMappingDifferencePx(rig.plan,zero,leaning,kLiveSeamDifferenceRows)==LiveSeamMappingDifferencePx(rig.plan,leaning,zero,kLiveSeamDifferenceRows));
    REJECT(LiveSeamMappingDifferencePx(rig.plan,zero,leaning,1));
    REJECT(LiveSeamMappingDifferencePx(FixedPreviewPlan{},zero,leaning,kLiveSeamDifferenceRows));
    REJECT(LiveSeamMappingDifferencePx(rig.plan,zero,ex::RegisteredMapping{Pose{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0},{},std::nullopt},kLiveSeamDifferenceRows));
}

// The draft design's policy through the worker: an estimate that is not further than the line from the one in force is taken as a
// confirmation (nothing is drawn again, whatever it did to the ends of C), and one that is further replaces it and is baked.
void Policy(){
    Rig rig(true,Pose{},ex::MountOrientation::normal,true);
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    const Pose first{0.3,-0.2,0.5,1.5,-1.0,0.0};
    rig.script={Estimate(first,false)};
    CHECK(rig.Pass(0).rebaked && rig.runtime->Status().rebakes==1);
    const auto current=rig.runtime->CurrentPlan();
    const auto serial=rig.runtime->Status().view.serial;
    // An estimate that would move C by more than tau_bake if it were drawn, while the follow-up measured 0.2 px at the seam (the scripted
    // change is the follow-up's own measure; the stand-in does not compute it).
    Pose scattered=first;scattered.tx_mm+=1.0;
    const double over_c=LiveMappingDifferencePx(rig.plan,ex::RegisteredMapping{first,{},std::nullopt},ex::RegisteredMapping{scattered,{},std::nullopt},9);
    CHECK(over_c>kLiveTrackerDesignDraft.tracker.bake_threshold_px);
    rig.script={Estimate(scattered,true,0.2)};
    const auto confirmed=rig.Pass(0.6);
    CHECK(confirmed.job_ran && confirmed.kind==LiveJobKind::follow && confirmed.accept==LiveAcceptKind::applied && !confirmed.rebaked);
    const auto status=rig.runtime->Status();
    CHECK(rig.runtime->CurrentPlan()==current && status.rebakes==1 && status.view.serial==serial && status.view.state==LiveRegistrationState::tracking);
    CHECK(status.view.counters.held==1 && status.view.mapping.pose.rx_deg==first.rx_deg);
    // The log says so: the estimate that was taken, how far it was from the one in force, the line, and what is drawn (the old estimate).
    CHECK(Contains(rig.LogText(),"replaced=0 seam_diff_px=0.2 adopt_line_px=0.5"));
    CHECK(Contains(rig.LogText(),"drawn_rx_deg=0.3 ") && Contains(rig.LogText(),"drawn_tx_mm=1.5 "));
    // A change over the follow-up's limit asks for a full estimate; a full estimate that is not further than the line from the estimate in
    // force confirms it too (the seam distance is measured on the lattice of the seam), and one that is further replaces it.
    rig.script={Estimate(scattered,true,0.7),Estimate(first,false)};
    const auto detected=rig.Pass(1.2);
    CHECK(detected.accept==LiveAcceptKind::kept_previous && !detected.rebaked);
    const auto full=rig.Pass(1.2);
    CHECK(full.job_ran && full.kind==LiveJobKind::full && full.accept==LiveAcceptKind::applied);
    CHECK(Contains(rig.LogText(),"job=full reason=change-detected accept=applied") && Contains(rig.LogText(),"replaced=0 seam_diff_px=0 adopt_line_px=0.5"));
    CHECK(!full.rebaked && rig.runtime->CurrentPlan()==current && rig.runtime->Status().view.mapping.pose.tx_mm==first.tx_mm);
    Pose moved=first;moved.tx_mm+=2.0;
    rig.script={Estimate(moved,true,0.7),Estimate(moved,false)};
    (void)rig.Pass(1.8);
    const auto replaced=rig.Pass(1.8);
    CHECK(replaced.job_ran && replaced.kind==LiveJobKind::full && replaced.accept==LiveAcceptKind::applied && replaced.rebaked);
    CHECK(rig.runtime->Status().view.mapping.pose.tx_mm==moved.tx_mm && rig.runtime->CurrentPlan()!=current && Contains(rig.LogText(),"replaced=1"));
}

// A follow-up that shows a change: a full estimate at once; the old estimate is held on screen meanwhile.
void Change(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    const Pose first{0,0,0.4,2.0,0.0,0.0};
    rig.script={Estimate(first,false),Estimate(first,true,3.0),Estimate(Pose{0,0,0.4,4.0,0.0,0.0},false)};
    rig.Pass(0);
    const auto changed=rig.Pass(0.6);
    CHECK(changed.job_ran && changed.kind==LiveJobKind::follow && changed.accept==LiveAcceptKind::kept_previous && !changed.rebaked);
    auto status=rig.runtime->Status();
    CHECK(status.view.state==LiveRegistrationState::holding && status.view.full_pending && Contains(status.state_text,"直前の推定"));
    CHECK(rig.runtime->CurrentPlan() && rig.runtime->CurrentPlan()->live->baked.pose.tx_mm==2.0);
    CHECK(Contains(rig.LogText(),"change-over-limit"));
    const auto full=rig.Pass(0.6);
    CHECK(full.job_ran && full.kind==LiveJobKind::full && rig.jobs.back().full_reason==LiveFullReason::change_detected && full.accept==LiveAcceptKind::applied && full.rebaked);
    status=rig.runtime->Status();
    CHECK(status.view.state==LiveRegistrationState::tracking && rig.runtime->CurrentPlan()->live->baked.pose.tx_mm==4.0);
    // Follow-ups start from the estimate in force.
    rig.script={Estimate(Pose{0,0,0.4,4.0,0.0,0.0},true,0.1)};
    rig.Pass(1.2);
    CHECK(rig.jobs.back().kind==LiveJobKind::follow && rig.jobs.back().start_pose.tx_mm==4.0);
}

// No estimate can be made: the last one is held for hold_seconds (3 s), then C goes back to the fixed mapping and B's direction is not
// measured again.
void HoldAndExpire(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    const Pose first{0,0,0.4,2.0,0.0,0.0};
    rig.script={Estimate(first,false),Unusable("too-few-points"),Unusable("too-few-points")};
    rig.Pass(0);
    rig.Pass(0.6);   // a follow-up that is not usable: a change; full requested at once
    auto status=rig.runtime->Status();
    CHECK(status.view.state==LiveRegistrationState::holding && rig.runtime->CurrentPlan()->live->baked.pose.tx_mm==2.0);
    rig.Pass(1.0);   // the full estimate fails too: held
    status=rig.runtime->Status();
    CHECK(status.view.state==LiveRegistrationState::holding && !Contains(status.orientation.b_short,"測れていません"));
    // 3.5 s after the last verified estimate (0 s): no frames are ready, so no job is begun and the holding time alone ends the estimate
    rig.ready=false;
    const auto expired=rig.Pass(3.5);
    status=rig.runtime->Status();
    CHECK(status.view.state==LiveRegistrationState::fixed && status.view.fixed_reason=="hold-expired" && Contains(status.state_text,"保てる時間"));
    CHECK(expired.rebaked && !expired.job_ran);
    CHECK(rig.runtime->CurrentPlan() && rig.runtime->CurrentPlan()->live->baked.pose.IsZero());
    CHECK(Contains(status.orientation.b_short,"B: 向きを測れていません"));
    CHECK(status.view.full_pending && status.view.full_reason==LiveFullReason::hold_expired);
    // With frames again a full estimate is tried for that reason.
    rig.ready=true;
    const auto retry=rig.Pass(3.6);
    CHECK(retry.job_ran && retry.kind==LiveJobKind::full && rig.jobs.back().full_reason==LiveFullReason::hold_expired);
}

// An estimate that cannot be drawn (B pushed out of the overlap): said on screen, the last plan stays, and the same mapping is not baked
// again on every pass; a later estimate that can be drawn clears it.
void BakeFails(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    const Pose good{0,0,0.4,2.0,0.0,0.0};
    rig.script={Estimate(good,false)};
    rig.Pass(0);
    const auto held=rig.runtime->CurrentPlan();
    Check(held!=nullptr,"a plan is in force");
    rig.script={Estimate(Pose{0,0,0.4,3000.0,0.0,0.0},true,0.2)};
    const auto bad=rig.Pass(0.6);
    CHECK(bad.job_ran && bad.accept==LiveAcceptKind::applied && !bad.rebaked && bad.rebake_failed);
    auto status=rig.runtime->Status();
    CHECK(status.bake_failed && status.rebake_failures==1 && Contains(status.state_text,"描けませんでした") && Contains(status.state_text,"直前"));
    CHECK(rig.runtime->CurrentPlan()==held);
    CHECK(Contains(rig.LogText(),"ok=0"));
    // The next pass has no job due and the same mapping: no second bake.
    const auto again=rig.Pass(0.7);
    CHECK(!again.job_ran && !again.rebaked && !again.rebake_failed && rig.runtime->Status().rebake_failures==1);
    // A mapping that can be drawn clears the message.
    rig.script={Estimate(Pose{0,0,0.4,6.0,0.0,0.0},true,0.2)};
    const auto good_again=rig.Pass(1.2);
    CHECK(good_again.rebaked && !rig.runtime->Status().bake_failed && rig.runtime->CurrentPlan()!=held);
}

// Frames: none ready means no job; frames of another size, a throwing estimator and a throwing snapshot are estimates that cannot be used.
void Frames(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    rig.script={Estimate(Pose{0,0,0.4,2.0,0.0,0.0},false)};
    rig.ready=false;
    CHECK(!rig.Pass(0).job_ran && rig.jobs.empty());
    rig.ready=true;rig.frame_width=100;rig.frame_height=60;
    const auto small=rig.Pass(0.1);
    CHECK(small.job_ran && small.accept==LiveAcceptKind::kept_previous && rig.jobs.empty());   // the estimator was not asked
    CHECK(Contains(rig.LogText(),"frames-unavailable"));
    rig.frame_width=kW;rig.frame_height=kH;
    rig.estimator=[](const LiveJob&,const render::BgrImage&,const render::BgrImage&)->LiveEstimateOutcome{throw std::runtime_error("scripted failure");};
    const auto thrown=rig.Pass(1.2);
    CHECK(thrown.job_ran && thrown.accept==LiveAcceptKind::kept_previous);
    CHECK(Contains(rig.LogText(),"estimator-exception") && Contains(rig.LogText(),"scripted failure"));
    rig.frames.snapshot=[](render::BgrImage&,render::BgrImage&)->bool{throw std::runtime_error("no frames");};
    const auto no_frames=rig.Pass(2.5);
    CHECK(no_frames.job_ran && no_frames.accept==LiveAcceptKind::kept_previous);
    // The tracker does not keep a job in flight after any of these: the next one is given out.
    rig.frames.snapshot=[&](render::BgrImage& x,render::BgrImage& y){x=rig.a;y=rig.b;return true;};
    rig.estimator=[&](const LiveJob& job,const render::BgrImage&,const render::BgrImage&){rig.jobs.push_back(job);return Estimate(Pose{0,0,0.4,2.0,0.0,0.0},false);};
    CHECK(rig.Pass(4).accept==LiveAcceptKind::applied && rig.runtime->Status().view.state==LiveRegistrationState::tracking);
}

// A condition without registration.live: nothing is estimated, the sentence says so, A's direction still shows.
void Disabled(){
    Rig rig(false);
    if(!Usable(rig)){Check(false,"plan");return;}
    CHECK(!rig.runtime->Enabled());
    rig.runtime->PublishBase(rig.plan);
    rig.script={Estimate(Pose{0,0,0.4,2.0,0.0,0.0},false)};
    const auto pass=rig.Pass(0);
    CHECK(!pass.job_ran && rig.jobs.empty() && !rig.runtime->CurrentPlan());
    const auto status=rig.runtime->Status();
    CHECK(!status.live_enabled && Contains(status.state_text,"無効") && Contains(status.orientation.b_short,"測れていません") && Contains(status.orientation.a_short,"A（校正時）: "));
    CHECK(!Contains(status.orientation.a_short,"測れていません"));
    rig.runtime->Restart(LiveFullReason::after_capture);
    rig.runtime->RequestFull(LiveFullReason::after_capture);
    CHECK(!rig.runtime->Status().live_enabled);
}

// A new plan for another frame size drops the baked plan and asks for a full estimate; restart and request name their reasons.
void Republish(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    rig.script={Estimate(Pose{0,0,0.4,2.0,0.0,0.0},false)};
    rig.Pass(0);
    CHECK(rig.runtime->CurrentPlan()!=nullptr);
    const auto other=PlanFixedPreviewV3(rig.condition,ex::MountOrientation::normal,320,214);
    Check(!other.skip && other.live,"a plan for another size");
    if(other.skip || !other.live)return;
    rig.runtime->PublishBase(other);
    CHECK(!rig.runtime->CurrentPlan() && rig.runtime->BasePlan()->view_width==320);
    CHECK(rig.runtime->Status().view.full_pending);
    // A job begun for the old size is not given a result for the new one: frames of the old size are refused.
    rig.script={Estimate(Pose{0,0,0.4,2.0,0.0,0.0},false)};
    const auto pass=rig.Pass(0.1);
    CHECK(pass.job_ran && pass.accept==LiveAcceptKind::kept_previous);
    rig.runtime->ClearBase();
    CHECK(!rig.runtime->BasePlan() && !rig.runtime->CurrentPlan());
    CHECK(!rig.Pass(5).job_ran);
    // Restart and RequestFull name why a full estimate is wanted.
    rig.runtime->PublishBase(rig.plan);
    rig.runtime->Restart(LiveFullReason::condition_reloaded);
    CHECK(rig.runtime->Status().view.full_reason==LiveFullReason::condition_reloaded);
    rig.runtime->RequestFull(LiveFullReason::after_capture);
    CHECK(rig.runtime->Status().view.full_reason==LiveFullReason::after_capture && rig.runtime->Status().view.state==LiveRegistrationState::fixed);
}

// The same calls give the same sentences and plans.
void Determinism(){
    std::vector<std::string> first,second;
    for(auto* texts:{&first,&second}){
        Rig rig;
        if(!Usable(rig)){Check(false,"plan");return;}
        rig.runtime->PublishBase(rig.plan);
        rig.script={Estimate(Pose{0.2,0,0.4,2.0,0.0,0.0},false),Estimate(Pose{0.2,0,0.4,2.1,0.0,0.0},true,0.1),Unusable("too-few-points")};
        for(const double at:{0.0,0.6,1.2,1.8}){
            rig.Pass(at);
            const auto status=rig.runtime->Status();
            texts->push_back(status.state_text+"|"+status.orientation_line+"|"+std::to_string(status.rebakes));
        }
    }
    CHECK(first==second && !first.empty());
}

// The worker thread: passes until stopped, no estimate after the stop, the count of workers returns to 0.
void Thread(){
    Rig rig;
    if(!Usable(rig)){Check(false,"plan");return;}
    rig.runtime->PublishBase(rig.plan);
    std::atomic<int> calls{0};
    std::atomic<bool> halted{false};
    const auto origin=std::chrono::steady_clock::now();
    LiveRegistrationRuntime::Config config;
    config.base_pose=Pose{};config.live_enabled=true;
    config.clock=[origin]{return std::chrono::duration<double>(std::chrono::steady_clock::now()-origin).count();};
    LiveRegistrationRuntime runtime(config,rig.condition.profile.parameters);
    runtime.PublishBase(rig.plan);
    LiveFrameSource frames;
    frames.ready=[]{return true;};
    frames.snapshot=[&](render::BgrImage& x,render::BgrImage& y){x=rig.a;y=rig.b;return true;};
    const LiveEstimator estimator=[&](const LiveJob& job,const render::BgrImage&,const render::BgrImage&){
        ++calls;
        return Estimate(Pose{0,0,0.4,2.0+(job.id%2)*3.0,0.0,0.0},job.kind==LiveJobKind::follow,0.1);
    };
    {
        std::jthread worker([&](std::stop_token stop){runtime.Work(stop,[&]{return halted.load();},frames,estimator,5ms);});
        // Concurrent readers: the loop of C and the window. The plan they get is always a complete one of the same size.
        bool consistent=true;
        const auto until=std::chrono::steady_clock::now()+1500ms;
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
        Check(consistent,"readers see complete plans and sentences while the worker bakes");
        CHECK(runtime.WorkersRunning()==1);
        halted=true;
    }
    CHECK(runtime.WorkersRunning()==0);
    const int stopped=calls.load();
    std::this_thread::sleep_for(100ms);
    CHECK(calls.load()==stopped && stopped>=2);
    CHECK(runtime.Status().rebakes>=1 && runtime.Status().view.state==LiveRegistrationState::tracking);
    // request_stop alone ends it as well.
    halted=false;
    {
        std::jthread worker([&](std::stop_token stop){runtime.Work(stop,[]{return false;},frames,estimator,5ms);});
        std::this_thread::sleep_for(60ms);
        worker.request_stop();
    }
    CHECK(runtime.WorkersRunning()==0);
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
    CHECK(threw);   // no clock
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
}
}

int main(){
    try{
        Design();
        RigIsUsable();
        NotConnected();
        Follows();
        SeamAndWhole();
        Policy();
        Change();
        HoldAndExpire();
        BakeFails();
        Frames();
        Disabled();
        Republish();
        Determinism();
        Thread();
        WorkerSurvives();
        Rejects();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
