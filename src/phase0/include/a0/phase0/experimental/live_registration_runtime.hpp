#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/render.hpp"
#include "a0/phase0/experimental/fixed_composite_preview.hpp"
#include "a0/phase0/experimental/live_placement.hpp"
#include "a0/phase0/experimental/live_registration_display.hpp"
#include "a0/phase0/experimental/live_registration_tracker.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <locale>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace a0::phase0::experimental {
// The part of the live C's registration that runs beside the window (#417 LC; docs/design/fixed-composite-registration.md 4 "the live
// C", 11.9, addendum 11.21; #437 stage 1). The machine (LivePlacementMachine, live_placement.hpp) holds the one confirmed position P that
// C is drawn with and says what to run: one estimate when the first session begins, one when the button is pressed. This file runs it on
// one worker thread of its own, bakes camera B's table again whenever P is to be replaced, and keeps the plan that the 80 ms loop of C draws
// with and the sentences that the window shows. No estimate runs unless one was asked for: there is no follow-up, no change detection, no
// holding time and no retry.
//
// Who calls what:
//  - the loop of C (merge worker): PublishBase when it has chosen the plan for a frame size, CurrentPlan on every pass. It never
//    estimates and never bakes; it only reads a shared_ptr under a mutex;
//  - the worker (Work / RunPass): the only caller of the machine's BeginJob / Accept, of the estimator and of RebakePreviewB. A new plan
//    is swapped in under the mutex when the bake is done, so a frame being drawn keeps the plan it started with (the two faces of B's
//    table). P is replaced only after the bake that draws it has passed;
//  - the window (UI thread): Status once per paint, RequestEstimate when the button is pressed, ApplyCapture when a shot ended with a JPEG.
// Every member is safe to call from any of those threads. The estimator and the bake run with no lock held.

// ---- Design constants of the live view: a DRAFT -------------------------------------------------------------------------------
//
// The values that decided how the live C followed the paper (#417). They are one proposal, NOT decided by measurement and NOT part of the
// condition (the condition v3 carries none of them). Since #437 stage 1 the runtime reads none of the lines of the follow-up, of holding and
// of the replacement of an estimate, nor the lattice of difference_samples_per_axis (LiveRegistrationTracker, which still reads them, is not
// used by the runtime; the struct is kept so that the estimator's connection and the tracker's own tests keep their shape). Units: seconds;
// C pixels (the pixels of C's grid); a fraction; the brightness unit of A's band.
struct LiveTrackerDesign {
    LiveTrackerSettings tracker;
    unsigned difference_samples_per_axis{};   // the lattice that LiveMappingDifferencePx measures on (samples per axis)
};
inline constexpr LiveTrackerDesign kLiveTrackerDesignDraft{
    {0.5,    // follow_interval_seconds: from the end of one job to the start of the next follow-up
     3.0,    // hold_seconds: how long the last verified estimate is kept when no estimate can be made
     1.0,    // full_retry_seconds: the pause before a full estimate is tried again after it failed
     0.5,    // follow_change_limit_px: the follow-up's change of the seam column's displacement that counts as a change
     0.6,    // inlier_fraction_floor: matches kept by the follow-up
     20.0,   // brightness_change_limit: change of A's band brightness from the last full estimate (the paper was replaced)
     0.5,    // bake_threshold_px (tau_bake): B's table is baked again only when the wanted mapping is further than this from the baked one
     0.5,    // adopt_threshold_px: an estimate replaces the one in force only when it is further than this from it along the seam. Two
             //   estimates of a still scene differ by a fraction of a pixel at the seam (with a rare tail past the follow-up's limit) and by
             //   several pixels at the ends of C, where the directions that the seam does not constrain count. The line is the follow-up's
             //   own limit: a follow-up never replaces the estimate in force (it confirms it or asks for a full estimate), and a full estimate
             //   does when it is as far as the limit says a change is. Chosen on synthetic scenes (tests/live_registration_estimator_tests.cpp)
     1.0},   // adopt_sigma_factor: ... or further than its own uncertainty, when that is larger (the condition's line for the uncertainty is the cap)
    9};      // difference_samples_per_axis: 9 x 9 lattice

// ---- The entrance of the estimate: the one place to connect it -----------------------------------------------------------------
//
// What the estimator of the live C is given. The caller prepares every member from the plan and the job, so the function that
// connects the estimate (below) only has to read them. The members are, in the order of the entrance that S2 is to provide:
//  1. the two oriented, reduced live frames (the size of the plan's view);
//  2. C's grid: the frame in document mm (left, top, pitch) and the width and height in C pixels;
//  3. the mappings on that grid: A's (the plan's table) and a maker of B's mapping for a pose and a surface;
//  4. the condition's raster and seam column (they place the surface's origin) and the limits in C pixels (registration.live);
//  5. the design constants;
//  6. the job: the flag (follow or full), the pose and surface to start from (the estimate in force for a follow-up, the base pose
//     for a full estimate) and the base pose p_b.
//
// The estimate also needs what the condition says about the registration besides the limits of the live view: the ranges
// (registration.envelope, surfaceEnvelope), the geometry in the capture's pixels and applyBeyondEnvelope. The plan does not carry
// them, so whoever makes the estimator passes them (MakeLiveEstimator). Without them the estimate is not connected.
struct LiveRegistrationRanges {
    a0::m2::experimental::content_registration::RegistrationEnvelope envelope;
    std::optional<a0::m2::experimental::SurfaceEnvelope> surface_envelope;   // absent: the pose only (six components)
    std::uint32_t omega_margin_px{},guard_px{},band_half_width_px{};         // the capture's pixels (registration.geometry)
    bool apply_beyond_envelope{false};
};
[[nodiscard]] inline LiveRegistrationRanges LiveRegistrationRangesOf(const a0::m2::experimental::FixedCompositeConditionV3::Registration& registration){
    return {registration.envelope,registration.surface_envelope,registration.geometry.omega_margin_px,registration.geometry.guard_px,
        registration.geometry.band_half_width_px,registration.apply_beyond_envelope.value_or(false)};
}
struct LiveEstimatorInputs {
    const a0::m2::render::BgrImage* camera_a{};
    const a0::m2::render::BgrImage* camera_b{};
    FixedPreviewGrid grid;
    a0::m2::render::OutputToCamera camera_a_on_grid;
    std::function<a0::m2::render::OutputToCamera(const a0::m2::experimental::RegisteredMapping&)> camera_b_on_grid;
    a0::m2::render::OutputRaster raster{a0::m2::render::DocumentRegionUm{0,0,0,0},0,0,0};
    std::uint32_t seam_column{};
    std::optional<a0::m2::experimental::content_registration::RegistrationLimits> limits;
    LiveTrackerDesign design;
    const LiveJob* job{};
    // On C's own grid: the seam column in use and the blend width (the plan's), and the model behind camera_b_on_grid (B's
    // calibrated projection, A's calibrated centre and the surface's origin; they live as long as the plan the call was made from).
    std::uint32_t grid_seam_column{};
    std::uint32_t grid_mixing_width_px{};
    const a0::m2::experimental::content_registration::PoseModel* model_b{};
    a0::m2::experimental::content_registration::CameraCentre centre_a{};
    a0::m2::render::Point surface_origin_mm{};
    std::optional<LiveRegistrationRanges> ranges;
    // The worker's stop (the job's): asked between the estimate's steps; true ends the job early. May be empty.
    std::function<bool()> stop;
};
using LiveEstimatorEntrance=std::function<LiveEstimateOutcome(const LiveEstimatorInputs&)>;

// An estimate that cannot be used because the estimate is not connected (the window words it "not connected"). The live C is then
// drawn with the condition's fixed mapping.
[[nodiscard]] inline LiveEstimateOutcome LiveEstimatorNotConnected(const LiveEstimatorInputs&){
    LiveEstimateOutcome outcome;
    outcome.usable=false;
    outcome.reasons.emplace_back(kLiveEstimatorUnavailableReason);
    outcome.detail="the entrance of the live estimate is not connected";
    return outcome;
}

// ---- The estimate of the live C --------------------------------------------------------------------------------------------------
//
// content_registration's estimate on a grid (EstimateRegistrationOnGrid), on C's grid with C's own mappings. A full job runs every
// stage; a follow-up runs the fine stage and the fit once from the estimate in force. The design constants are a DRAFT chosen on
// synthetic frames of the live view's coarseness (kRegistrationDesignLiveDraft).
//
// Two threads at most: the two sides of the band are measured side by side, each on one thread. The window's loop of C (every
// 80 ms) and the two camera threads run at the same time, and the estimate must not starve them on a four-core machine.
struct LiveEstimatorDesign {
    a0::m2::experimental::content_registration::GridRegistrationDesign grid;
    unsigned thread_count{};
};
inline constexpr LiveEstimatorDesign kLiveEstimatorDesignDraft{a0::m2::experimental::content_registration::kRegistrationDesignLiveDraft,2};

// Reasons that the estimate of the live C adds to the estimator's own: the job was stopped by the worker; the estimate is trusted
// but outside a range that the condition does not apply; the settings made from the condition were refused (a band that does not
// reach past the guard at this size, a mapping that cannot be evaluated).
inline constexpr std::string_view kLiveEstimateStoppedReason="stopped";
inline constexpr std::string_view kLiveEstimateBeyondEnvelopeReason="out-of-envelope";
inline constexpr std::string_view kLiveEstimateRefusedReason="estimate-refused";

// A message for a log line: kept when it cannot hold a path (a file system error carries the full path, with the user's name),
// replaced by a note otherwise.
[[nodiscard]] inline std::string LiveLogSafeText(std::string_view text){
    if(text.find('\\')!=std::string_view::npos || text.find('/')!=std::string_view::npos || text.size()>200)return "(message withheld: it may hold a path)";
    return std::string(text);
}

// What the estimate was made with and what it measured, for one log line (numbers and codes only).
[[nodiscard]] inline std::string LiveEstimateDetail(const a0::m2::experimental::content_registration::GridRegistrationResult& result,
    const a0::m2::experimental::content_registration::RegistrationGeometry& geometry){
    const auto& r=result.registration;const auto& d=r.diagnostics;
    // The residual that V5 judges: across the baseline when a surface is estimated, the whole residual otherwise.
    const bool surface=r.surface.state!=a0::m2::experimental::SurfaceState::disabled;
    const auto& residual=surface?d.residual_across:d.residual;
    std::ostringstream s;s.imbue(std::locale::classic());
    s<<"estimate_state="<<a0::m2::experimental::content_registration::RegistrationStateCode(r.state)<<" seam="<<geometry.seam_column
        <<" omega_half="<<geometry.omega_half_width_px()<<" guard="<<geometry.guard_px<<" band_half="<<geometry.band_half_width_px
        <<" brightness_a="<<result.band_brightness_a<<" brightness_b="<<result.band_brightness_b<<" anchors="<<d.coarse_vote.unique_supporters
        <<" blocks="<<d.consistent_blocks<<" points="<<d.points<<" inliers="<<d.inliers<<" sigma_px="<<d.maximum_prediction_sigma_px
        <<" rms_px="<<residual.rms<<" p95_px="<<residual.p95<<" bias_px="<<(surface?d.maximum_bin_bias_across_px:d.maximum_bin_bias_px)
        <<" outer="<<d.outer_iterations<<" change_px="<<d.last_change_px<<" second_start="
        <<a0::m2::experimental::content_registration::SecondStartCode(d.second_start)<<" second_start_px="<<d.second_start_difference_px;
    // The estimate itself and the components that are outside the condition's range (value / limit, in mm, degrees or percent).
    if(r.has_estimate){
        s<<" est_rx_deg="<<r.pose.rx_deg<<" est_ry_deg="<<r.pose.ry_deg<<" est_rz_deg="<<r.pose.rz_deg<<" est_tx_mm="<<r.pose.tx_mm
            <<" est_ty_mm="<<r.pose.ty_mm<<" est_tz_percent="<<r.pose.tz_percent;
        if(r.surface.state==a0::m2::experimental::SurfaceState::estimated)
            s<<" est_h0_mm="<<r.surface.plane.h0_mm<<" est_slope_x="<<r.surface.plane.slope_x<<" est_slope_y="<<r.surface.plane.slope_y;
    }
    s<<" exceeded=";
    bool any=false;
    const auto list=[&](const std::vector<a0::m2::experimental::content_registration::ComponentCheck>& checks){
        for(const auto& c:checks)if(c.exceeded){
            s<<(any?",":"")<<LiveLogSafeText(a0::m2::experimental::content_registration::EnvelopeComponentCode(c.component))<<':'<<c.value<<'/'<<c.limit;
            any=true;
        }
    };
    list(r.components);list(r.surface_components);
    if(!any)s<<"none";
    return s.str();
}

// One job on the inputs. Never throws for what the frames show: an estimate that cannot be trusted is an outcome that is not
// usable, with the estimator's reasons, and the tracker then holds the last estimate or goes back to the fixed mapping. A full
// estimate is usable when it is verified, or trusted but outside a range that the condition applies anyway
// (applyBeyondEnvelope); a follow-up when V1 and V3 pass.
[[nodiscard]] inline LiveEstimateOutcome EstimateLiveRegistration(const LiveEstimatorInputs& in,const LiveEstimatorDesign& design){
    namespace cr=a0::m2::experimental::content_registration;
    LiveEstimateOutcome outcome;
    const auto refuse=[&](std::string_view reason,std::string detail){
        outcome=LiveEstimateOutcome{};
        outcome.reasons.emplace_back(reason);outcome.detail=std::move(detail);
        return outcome;
    };
    if(!in.ranges)return refuse(kLiveEstimatorUnavailableReason,"the ranges of the condition did not reach the live estimate");
    if(!in.limits)return refuse(kLiveRegistrationDisabledReason,"the condition has no limits of the live view");
    if(!in.camera_a || !in.camera_b || !in.job || !in.camera_a_on_grid || !in.camera_b_on_grid || !in.model_b || !in.raster.dpi || !(in.grid.pitch_mm>0))
        return refuse(kLiveEstimateRefusedReason,"an input of the live estimate is missing");
    if(in.stop && in.stop())return refuse(kLiveEstimateStoppedReason,{});
    const auto& job=*in.job;
    try{
        cr::RegistrationGrid grid;
        grid.width=in.grid.width;grid.height=in.grid.height;
        grid.frame={in.grid.left_mm,in.grid.top_mm,in.grid.pitch_mm};
        grid.camera_a=in.camera_a_on_grid;
        grid.camera_b=[&in](const cr::PoseParameters& pose,const a0::m2::experimental::SurfacePlane& surface){
            return in.camera_b_on_grid(a0::m2::experimental::RegisteredMapping{pose,surface,std::nullopt});
        };
        grid.model_b=in.model_b;grid.centre_a=in.centre_a;grid.surface_origin_mm=in.surface_origin_mm;

        cr::GridRegistrationSettings settings;
        // One pixel of C is 1/s pixels of the capture's raster.
        const double scale=25.4/static_cast<double>(in.raster.dpi)/in.grid.pitch_mm;
        settings.estimator.geometry=cr::ScaleRegistrationGeometry(in.grid_seam_column,in.grid_mixing_width_px,in.ranges->omega_margin_px,
            in.ranges->guard_px,in.ranges->band_half_width_px,scale,design.grid.measurer.patch_px);
        settings.estimator.envelope=in.ranges->envelope;
        settings.estimator.surface_envelope=in.ranges->surface_envelope;
        settings.estimator.limits=*in.limits;
        settings.estimator.design=design.grid.estimator;
        settings.estimator.base_pose=job.base_pose;
        settings.measurer=design.grid.measurer;
        settings.minimum_band_brightness=design.grid.minimum_band_brightness;

        cr::GridRegistrationRequest request;
        if(job.kind==LiveJobKind::follow){request.job=cr::GridJob::follow;request.start_pose=job.start_pose;request.start_surface=job.start_surface;}
        cr::RegistrationControl control;
        control.stop=in.stop;
        const auto result=cr::EstimateRegistrationOnGrid(*in.camera_a,*in.camera_b,grid,settings,request,design.thread_count,control);
        const auto& r=result.registration;
        outcome.band_brightness=result.band_brightness_a;
        outcome.detail=LiveEstimateDetail(result,settings.estimator.geometry);
        if(result.stopped){outcome.reasons.emplace_back(kLiveEstimateStoppedReason);return outcome;}
        outcome.reasons=r.reasons;
        outcome.beyond_envelope=r.state==cr::RegistrationState::out_of_envelope;
        outcome.prediction_sigma_px=r.diagnostics.maximum_prediction_sigma_px;
        if(outcome.beyond_envelope)
            for(const auto& check:r.components)if(check.exceeded)outcome.exceeded.emplace_back(cr::EnvelopeComponentCode(check.component));
        if(outcome.beyond_envelope)
            for(const auto& check:r.surface_components)if(check.exceeded)outcome.exceeded.emplace_back(cr::EnvelopeComponentCode(check.component));
        const bool applied=r.state==cr::RegistrationState::verified || (outcome.beyond_envelope && in.ranges->apply_beyond_envelope);
        if(outcome.beyond_envelope && !applied)outcome.reasons.emplace_back(kLiveEstimateBeyondEnvelopeReason);
        if(r.has_estimate){
            outcome.pose=r.pose;
            if(r.surface.state==a0::m2::experimental::SurfaceState::estimated)outcome.surface=r.surface.plane;
        }
        if(job.kind==LiveJobKind::follow){outcome.change_px=result.change_px;outcome.inlier_fraction=result.inlier_fraction;}
        outcome.usable=applied && r.has_estimate;
        return outcome;
    }catch(const std::invalid_argument& error){
        return refuse(kLiveEstimateRefusedReason,LiveLogSafeText(error.what()));
    }
}

// The connection: the estimate of the live C with the draft design.
[[nodiscard]] inline LiveEstimatorEntrance ConnectedLiveEstimatorEntrance(){
    return [](const LiveEstimatorInputs& inputs){return EstimateLiveRegistration(inputs,kLiveEstimatorDesignDraft);};
}

using LivePlanSource=std::function<std::shared_ptr<const FixedPreviewPlan>()>;
// The estimator that the tracker runs: it prepares the inputs from the plan in force and calls the entrance. A plan without a
// registered context gives an estimate that cannot be used (reason "no-registered-plan"). `ranges` is what the condition says
// about the registration (LiveRegistrationRangesOf); without it the connected entrance answers "not connected".
[[nodiscard]] inline LiveEstimator MakeLiveEstimator(LivePlanSource plan_source,const LiveTrackerDesign& design,LiveEstimatorEntrance entrance,
    std::optional<LiveRegistrationRanges> ranges=std::nullopt){
    if(!plan_source || !entrance)throw std::invalid_argument("live estimator: no plan source or entrance");
    return [plan_source=std::move(plan_source),design,entrance=std::move(entrance),ranges=std::move(ranges)](const LiveJob& job,
        const a0::m2::render::BgrImage& a,const a0::m2::render::BgrImage& b){
        const auto plan=plan_source();
        if(!plan || !plan->live || !plan->mapped){
            LiveEstimateOutcome outcome;
            outcome.reasons.emplace_back("no-registered-plan");
            return outcome;
        }
        LiveEstimatorInputs inputs;
        inputs.camera_a=&a;inputs.camera_b=&b;
        inputs.grid=plan->grid;
        inputs.camera_a_on_grid=plan->mapped->camera_a;
        const std::shared_ptr<const LiveRegistrationContext> context=plan->live;
        inputs.camera_b_on_grid=[context](const a0::m2::experimental::RegisteredMapping& mapping){return LiveViewRegisteredOutputToCamera(*context,mapping);};
        inputs.raster=context->raster;inputs.seam_column=context->seam_column;inputs.limits=context->live_limits;
        inputs.design=design;inputs.job=&job;
        inputs.grid_seam_column=plan->view.seam_column;inputs.grid_mixing_width_px=plan->settings.width_pixels;
        inputs.model_b=&context->model_b;inputs.centre_a=context->centre_a;inputs.surface_origin_mm=context->origin_mm;
        inputs.ranges=ranges;
        inputs.stop=job.stop;
        return entrance(inputs);
    };
}

// The largest distance, in C pixels, between where `a` and `b` put the same pixel of the seam in the document: a lattice of rows
// down C's grid by the seam column and the columns a blend width either side of it (the pixels that C's blend can show), where camera B
// shows the sample. LiveMappingDifferencePx measures the same thing over all of C, and there the directions that the seam does not
// constrain count as much as the ones it does. +infinity where a mapping cannot place a sample or B shows none of the lattice.
// Throws what LiveDocumentHomography throws.
[[nodiscard]] inline double LiveSeamMappingDifferencePx(const FixedPreviewPlan& plan,const a0::m2::experimental::RegisteredMapping& a,
    const a0::m2::experimental::RegisteredMapping& b,unsigned rows){
    if(!plan.live || !plan.live->table_b)throw std::invalid_argument("no registered plan");
    if(rows<2)throw std::invalid_argument("at least 2 rows");
    const auto& context=*plan.live;
    const auto ha=LiveDocumentHomography(context,a),hb=LiveDocumentHomography(context,b);
    const auto& grid=context.grid;
    const std::int64_t seam=plan.view.seam_column,reach=std::max<std::int64_t>(1,plan.settings.width_pixels);
    double largest=0;
    bool any=false;
    for(unsigned j=0;j<rows;++j){
        const auto y=static_cast<std::uint32_t>(std::llround(static_cast<double>(j)*static_cast<double>(grid.height-1)/static_cast<double>(rows-1)));
        for(const std::int64_t offset:{-reach,std::int64_t{0},reach}){
            const std::int64_t column=seam+offset;
            if(column<0 || column>=static_cast<std::int64_t>(grid.width))continue;
            const auto x=static_cast<std::uint32_t>(column);
            if(std::isnan((*context.table_b)[static_cast<size_t>(y)*grid.width+x].x))continue;
            const a0::m2::render::Point document{grid.left_mm+(x+.5)*grid.pitch_mm,grid.top_mm+(y+.5)*grid.pitch_mm};
            a0::m2::render::Point pa=document,pb=document;
            if((ha && !a0::m2::render::TryTransform(*ha,document,pa)) || (hb && !a0::m2::render::TryTransform(*hb,document,pb)))
                return std::numeric_limits<double>::infinity();
            largest=std::max(largest,std::hypot(pa.x-pb.x,pa.y-pb.y)/grid.pitch_mm);
            any=true;
        }
    }
    return any?largest:std::numeric_limits<double>::infinity();
}
inline constexpr unsigned kLiveSeamDifferenceRows=17;

// The same distance in pixels of the JPEG's raster (the condition's), which is what a person reads: C's pixels times C's pitch over the
// raster's pitch. +infinity where the mapping cannot place a sample. Throws what LiveSeamMappingDifferencePx throws.
[[nodiscard]] inline double LiveSeamMappingDifferenceOutputPx(const FixedPreviewPlan& plan,const a0::m2::experimental::RegisteredMapping& a,
    const a0::m2::experimental::RegisteredMapping& b,unsigned rows){
    const double in_c=LiveSeamMappingDifferencePx(plan,a,b,rows);
    if(!plan.live || !plan.live->raster.dpi)throw std::invalid_argument("no registered plan");
    return in_c*plan.live->grid.pitch_mm/(25.4/static_cast<double>(plan.live->raster.dpi));
}

// ---- What the window shows ------------------------------------------------------------------------------------------------------
struct LiveStatus {
    bool live_enabled{false};              // the condition has registration.live
    LivePlacementView placement;           // P, where it came from, what the last estimate came to
    bool bake_failed{false};               // P could not be drawn; C keeps the last mapping that could
    std::string state_text;                // LivePlacementStateText (UTF-8, one line)
    LiveOrientationTexts orientation;
    std::string orientation_line;          // LiveOrientationRowText (UTF-8, one line)
    std::uint64_t rebakes{},rebake_failures{};
};

// What the worker reads of the frames.
struct LiveFrameSource {
    std::function<bool()> ready;           // cheap: a fresh pair of frames exists
    std::function<bool(a0::m2::render::BgrImage&,a0::m2::render::BgrImage&)> snapshot;   // the oriented, reduced frames (the size of C's view)
};

struct LivePassResult {
    bool job_ran{false};                   // the estimator was called
    std::optional<LiveJobKind> kind;
    LiveAcceptKind accept{LiveAcceptKind::discarded_stale};   // applied: a position waits to be drawn; kept_previous: nothing was decided
    bool rebaked{false};
    bool rebake_failed{false};
};

[[nodiscard]] inline std::string_view LiveAcceptKindCode(LiveAcceptKind kind){
    switch(kind){
    case LiveAcceptKind::applied:return "applied";
    case LiveAcceptKind::kept_previous:return "kept-previous";
    case LiveAcceptKind::discarded_stale:return "discarded-stale";
    }
    return {};
}

class LiveRegistrationRuntime {
public:
    struct Config {
        LiveTrackerDesign design{kLiveTrackerDesignDraft};   // the estimator's connection reads it; the lines of the follow-up and of holding are not used
        a0::m2::experimental::content_registration::PoseParameters base_pose;   // p_b of the condition
        bool live_enabled{false};                                               // the condition has registration.live
        a0::m2::experimental::MountOrientation mount{a0::m2::experimental::MountOrientation::normal};
        std::string condition_sha256;                                           // the file that this session was read from (not empty)
        std::optional<LivePlacementMachine> placement;                          // the machine of the sessions before: P is carried across them
        std::function<void(const std::string&)> log;                            // one line per event; may be empty
    };
    // `profile` is the condition's verified profile: A's calibrated direction and B's moved direction come from it.
    LiveRegistrationRuntime(Config config,a0::m2::render::DocumentRenderParameters profile)
        :config_(std::move(config)),profile_(std::move(profile)){
        if(config_.placement)machine_=*config_.placement;
        config_.placement.reset();
        machine_.BeginSession(config_.condition_sha256,config_.base_pose,config_.live_enabled);
        orientation_a_=CameraOrientationOfCalibratedA(profile_.camera_a,config_.mount);
        std::lock_guard lock(mutex_);
        PublishStatusLocked();
    }
    LiveRegistrationRuntime(const LiveRegistrationRuntime&)=delete;
    LiveRegistrationRuntime& operator=(const LiveRegistrationRuntime&)=delete;

    [[nodiscard]] const LiveTrackerDesign& Design() const {return config_.design;}
    [[nodiscard]] bool Enabled() const {return config_.live_enabled;}

    // The button: one estimate is asked for (refused while one is asked for or running, and when the condition has no live estimate).
    LiveAskResult RequestEstimate(LiveEstimateAsk ask){
        std::lock_guard lock(mutex_);
        const auto result=machine_.RequestEstimate(ask);
        PublishStatusLocked();
        return result;
    }
    // A capture ended with a JPEG made with `mapping` (the mapping of the geometry level it chose) from the condition of `condition_sha256`:
    // P becomes that mapping, with layer 2 left out. False (nothing changed) when the condition is not the session's.
    bool ApplyCapture(const a0::m2::experimental::RegisteredMapping& mapping,const std::string& level,const std::string& condition_sha256){
        std::optional<std::string> line;
        bool applied=false;
        {
            std::lock_guard lock(mutex_);
            applied=machine_.ApplyCapture(mapping,level,condition_sha256);
            if(applied){
                bake_failed_=false;failed_wanted_serial_.reset();
                MeasureCaptureLocked();
                line=CaptureLineLocked();
            }
            PublishStatusLocked();
        }
        if(line)Log(*line);
        return applied;
    }
    // The machine as it is now: the window keeps it across the sessions (the runtime of the next session starts from it).
    [[nodiscard]] LivePlacementMachine PlacementMachine() const {std::lock_guard lock(mutex_);return machine_;}

    // The loop of C has chosen the plan (the fixed mapping of the condition) for the frame size. A plan chosen again for another
    // size drops the plan baked for the old one; P is baked into the new one by the worker.
    void PublishBase(const FixedPreviewPlan& plan){
        std::lock_guard lock(mutex_);
        base_=std::make_shared<const FixedPreviewPlan>(plan);
        ++base_serial_;current_.reset();
        bake_failed_=false;failed_wanted_serial_.reset();
        MeasureCaptureLocked();
        PublishStatusLocked();
    }
    // C is not drawn with a registered plan (no condition v3 plan for this size).
    void ClearBase(){
        std::lock_guard lock(mutex_);
        base_.reset();current_.reset();++base_serial_;
        bake_failed_=false;failed_wanted_serial_.reset();
        PublishStatusLocked();
    }
    [[nodiscard]] std::shared_ptr<const FixedPreviewPlan> BasePlan() const {std::lock_guard lock(mutex_);return base_;}
    // The plan with B's table baked for P; null until the first bake (draw with the base plan).
    [[nodiscard]] std::shared_ptr<const FixedPreviewPlan> CurrentPlan() const {std::lock_guard lock(mutex_);return current_;}
    [[nodiscard]] LiveStatus Status() const {std::lock_guard lock(mutex_);return status_;}
    [[nodiscard]] int WorkersRunning() const {return workers_running_.load();}

    // One pass of the worker: the estimate when one is asked for, then the bake when P is to be replaced or has not been drawn, then the
    // status. `stop` is the worker's stop: the job carries it to the estimator, and after it answers true nothing is baked. A job that
    // was stopped, or that had no frames, did not estimate anything: it is asked for again (not counted as an estimate that decided nothing).
    LivePassResult RunPass(const LiveFrameSource& frames,const LiveEstimator& estimator,const std::function<bool()>& stop={}){
        if(!estimator)throw std::invalid_argument("live runtime: no estimator");
        LivePassResult result;
        std::shared_ptr<const FixedPreviewPlan> base;
        bool asked=false;
        {
            std::lock_guard lock(mutex_);
            base=base_;
            asked=machine_.Activity()==LivePlacementActivity::requested;
            if(!base || !base->live){PublishStatusLocked();return result;}
        }
        std::optional<LiveJob> job;
        if(asked && frames.ready && frames.ready()){
            std::lock_guard lock(mutex_);
            job=machine_.BeginJob();
        }
        if(job){
            job->stop=stop;
            a0::m2::render::BgrImage a,b;
            bool got=false;
            try{
                got=frames.snapshot && frames.snapshot(a,b) && a.width==base->view_width && a.height==base->view_height &&
                    b.width==base->view_width && b.height==base->view_height;
            }catch(...){got=false;}
            if(!got){
                std::lock_guard lock(mutex_);
                machine_.Release(*job);   // nothing was estimated
            }else{
                result.job_ran=true;result.kind=job->kind;
                LiveEstimateOutcome outcome;
                // Whatever the estimator throws is an outcome that is not usable: the machine must get an answer for the job.
                try{outcome=estimator(*job,a,b);}
                catch(const std::exception& error){
                    outcome=LiveEstimateOutcome{};
                    outcome.reasons.emplace_back("estimator-exception");
                    outcome.detail=LiveLogSafeText(error.what());
                }catch(...){
                    outcome=LiveEstimateOutcome{};
                    outcome.reasons.emplace_back("estimator-exception");
                    outcome.detail="an exception of an unknown kind";
                }
                const bool interrupted=std::find(outcome.reasons.begin(),outcome.reasons.end(),std::string(kLiveEstimateStoppedReason))!=outcome.reasons.end();
                std::optional<std::string> line;
                {
                    std::lock_guard lock(mutex_);
                    if(interrupted){machine_.Release(*job);result.accept=LiveAcceptKind::discarded_stale;}
                    else{
                        const auto accepted=machine_.Accept(*job,outcome);
                        result.accept=accepted==LivePlacementMachine::AcceptKind::pending_draw?LiveAcceptKind::applied:
                            accepted==LivePlacementMachine::AcceptKind::not_decided?LiveAcceptKind::kept_previous:LiveAcceptKind::discarded_stale;
                        line=JobLineLocked(*job,result.accept,outcome);
                    }
                }
                if(line)Log(*line);   // with no lock held: a log that blocks must not stop Status() or CurrentPlan()
            }
        }
        if(!(stop && stop()))Rebake(result,stop);
        std::lock_guard lock(mutex_);
        PublishStatusLocked();
        return result;
    }

    // The worker: passes at a fixed period until stopped. `halted` is the window's own stop (checked every period, and by the
    // estimator between its steps). An exception of a pass, of any kind, is logged once per message and does not end the worker.
    void Work(const std::stop_token& stop,const std::function<bool()>& halted,const LiveFrameSource& frames,const LiveEstimator& estimator,
        std::chrono::milliseconds period){
        struct Running {
            std::atomic<int>& count;
            explicit Running(std::atomic<int>& value):count(value){++count;}
            ~Running(){--count;}
        } running(workers_running_);
        const std::function<bool()> stopped=[&stop,&halted]{return stop.stop_requested() || (halted && halted());};
        std::string last_error;
        const auto failed=[&](std::string message){
            if(last_error==message)return;
            last_error=std::move(message);
            Log("C live_registration pass_failure="+last_error);
        };
        while(!stopped()){
            try{(void)RunPass(frames,estimator,stopped);}
            catch(const std::exception& error){failed(LiveLogSafeText(error.what()));}
            catch(...){failed("an exception of an unknown kind");}
            std::this_thread::sleep_for(period);
        }
    }

private:
    using Plan=FixedPreviewPlan;
    // Never called with mutex_ held.
    void Log(const std::string& line) const {if(config_.log)config_.log(line);}

    // Bakes camera B's table again whenever the mapping that is wanted (a position that waits to be drawn, else P) is not the one the plan in
    // use was baked for; there is no distance below which a bake is skipped, so what C draws is P itself. The bake runs with no lock held; the
    // result is taken only if the base plan is still the one it was made for and the wanted mapping is still the one that was baked (a shot or
    // a new condition may have changed it meanwhile). A position that waits becomes P when it has been drawn, and is dropped when it could not be.
    void Rebake(LivePassResult& result,const std::function<bool()>& stop){
        std::shared_ptr<const Plan> from;
        std::uint64_t serial{},wanted_serial{};
        a0::m2::experimental::RegisteredMapping wanted;
        std::optional<std::string> line;
        {
            std::lock_guard lock(mutex_);
            if(!base_ || !base_->live)return;
            serial=base_serial_;from=current_?current_:base_;
            wanted_serial=machine_.WantedSerial();wanted=machine_.Wanted();
            if(failed_wanted_serial_==wanted_serial)return;   // the same mapping failed already; wait for another
            if(LivePlacementSameMapping(from->live->baked,wanted)){
                // Already drawn (P itself, or a position that came out the same as P).
                if(machine_.HasPending())machine_.Committed();
                bake_failed_=false;failed_wanted_serial_.reset();
                line=PlacementLineLocked(*from);
                if(!line)return;
            }
        }
        if(line){Log(*line);return;}
        if(stop && stop())return;
        const auto started=std::chrono::steady_clock::now();
        auto baked=RebakePreviewB(*from,wanted);
        const auto milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        std::ostringstream text;text.imbue(std::locale::classic());
        std::optional<std::string> placement_line;
        {
            std::lock_guard lock(mutex_);
            if(base_serial_!=serial)return;   // another base plan was published meanwhile; P is baked into it by the next pass
            if(machine_.WantedSerial()!=wanted_serial)return;   // the wanted mapping changed meanwhile; the next pass bakes it
            text<<"C live_rebake wanted_serial="<<wanted_serial<<" ms="<<milliseconds;
            if(baked.ok){
                current_=std::make_shared<const Plan>(std::move(baked.plan));
                if(machine_.HasPending())machine_.Committed();
                bake_failed_=false;failed_wanted_serial_.reset();
                ++rebakes_;result.rebaked=true;
                text<<" ok=1";
                placement_line=PlacementLineLocked(*current_);
            }else{
                ++rebake_failures_;result.rebake_failed=true;
                if(machine_.HasPending())machine_.DrawFailed("bake-failed");
                else{bake_failed_=true;failed_wanted_serial_=wanted_serial;}
                text<<" ok=0 detail="<<LiveLogSafeText(baked.detail);
            }
        }
        Log(text.str());
        if(placement_line)Log(*placement_line);
    }

    // The line of a job: what asked for it, what it came to, what the estimator measured (outcome.detail). Written for every job (there are
    // two a session at the most, so no line is dropped).
    [[nodiscard]] std::optional<std::string> JobLineLocked(const LiveJob& job,LiveAcceptKind accept,const LiveEstimateOutcome& outcome){
        std::ostringstream line;line.imbue(std::locale::classic());
        line<<"C live_registration job="<<LiveJobKindCode(job.kind);
        if(job.full_reason)line<<" reason="<<LiveFullReasonCode(*job.full_reason);
        line<<" accept="<<LiveAcceptKindCode(accept);
        if(accept!=LiveAcceptKind::applied && !outcome.reasons.empty()){
            line<<" failure=";
            for(size_t i=0;i<outcome.reasons.size();++i)line<<(i?",":"")<<outcome.reasons[i];
        }
        if(!outcome.detail.empty())line<<" detail="<<outcome.detail;
        line<<" quality_accepted=0";
        return line.str();
    }

    // P as the live C draws it, in the one form of LivePlacementFields (the capture's log writes the JPEG's position with the same function).
    // Written once for each P, after the plan in use was baked for it. nullopt when it has been written, or when the plan is not baked for P.
    [[nodiscard]] std::optional<std::string> PlacementLineLocked(const Plan& drawn){
        if(!LivePlacementSameMapping(drawn.live->baked,machine_.Placement()))return std::nullopt;
        if(logged_serial_==machine_.Serial())return std::nullopt;
        logged_serial_=machine_.Serial();
        std::ostringstream line;line.imbue(std::locale::classic());
        line<<"C live_placement serial="<<machine_.Serial()<<" source="<<LivePlacementSourceCode(machine_.Source())<<' '<<LivePlacementFields(machine_.Placement())
            <<" condition_sha256="<<machine_.ConditionSha256()<<" drawn=1 quality_accepted=0";
        return line.str();
    }
    // What a shot did, once: P before the shot and the JPEG's mapping, the level, and how far apart they are (when it was measured).
    [[nodiscard]] std::optional<std::string> CaptureLineLocked() const {
        const auto& note=machine_.Capture();
        if(!note)return std::nullopt;
        std::ostringstream line;line.imbue(std::locale::classic());
        line<<"C live_placement_after_capture serial="<<machine_.Serial()<<" level="<<note->level<<" before_source="<<LivePlacementSourceCode(note->before_source)
            <<' '<<LivePlacementFields(note->before,"before_")<<" layer_two_dropped="<<(note->layer_two_dropped?1:0);
        if(note->difference_px)line<<" seam_difference_px="<<*note->difference_px;
        line<<" quality_accepted=0";
        return line.str();
    }
    // Measures the distance between P before the shot and the JPEG's mapping on the plan in use, once a registered plan exists.
    void MeasureCaptureLocked(){
        if(!machine_.CaptureDifferencePending() || !base_ || !base_->live)return;
        try{
            const double pixels=LiveSeamMappingDifferenceOutputPx(*base_,machine_.Capture()->before,machine_.Placement(),kLiveSeamDifferenceRows);
            machine_.SetCaptureDifference(pixels);
        }catch(const std::exception&){}   // not measured: the sentence leaves the number out
    }

    void PublishStatusLocked(){
        LiveStatus next;
        next.live_enabled=config_.live_enabled;
        next.placement=machine_.View();
        const auto& view=next.placement;
        // B's direction is shown for a position that was measured or set by a shot; for the condition's position it is not measured.
        if(view.source!=LivePlacementSource::fixed_condition){
            if(orientation_serial_!=view.serial){
                orientation_b_=CameraOrientationOfBFromPose(profile_,view.mapping.pose);
                orientation_serial_=view.serial;
            }
        }else{orientation_b_.reset();orientation_serial_.reset();}
        next.bake_failed=bake_failed_;
        next.state_text=LivePlacementStateText(view,bake_failed_);
        next.orientation=LiveOrientationTextsOf(orientation_a_,orientation_b_);
        next.orientation_line=LiveOrientationRowText(next.orientation);
        next.rebakes=rebakes_;next.rebake_failures=rebake_failures_;
        status_=std::move(next);
    }

    mutable std::mutex mutex_;
    Config config_;
    a0::m2::render::DocumentRenderParameters profile_;
    LivePlacementMachine machine_;
    std::shared_ptr<const Plan> base_,current_;
    std::uint64_t base_serial_{};
    bool bake_failed_{false};
    std::optional<std::uint64_t> failed_wanted_serial_;
    std::optional<std::uint64_t> logged_serial_;
    std::optional<CameraOrientation> orientation_a_,orientation_b_;
    std::optional<std::uint64_t> orientation_serial_;
    LiveStatus status_;
    std::uint64_t rebakes_{},rebake_failures_{};
    std::atomic<int> workers_running_{0};
};
}
