#pragma once
// The pair capture's composite with the condition of a setup, and what is said about the registered composite after the shot
// (#417 J8w; docs/design/fixed-composite-registration.md 11.9, addendum 11.25). Header only, no window and no camera, so that the
// development preview and its tests run the same code.
//
//  - ComposePairWithSelection: the branch of the pair capture that turns the selected condition (v1, v2 or the revision in force of
//    v3) into one StillReviewRequest and calls the review writer. The v1 and v2 branches are the ones SavePairReview always had.
//    A condition that cannot be applied is a skip with its reason, never an error: the originals are kept either way.
//  - DescribeRegistrationReview: the lines for the status row and for the log of a v8 review: the level (G1 to G4), the pose of B
//    and the paper surface, the gate and its largest displacement, every warning (the sentences of
//    fixed_composite_registration_text.hpp), the time of each stage. The sentences are made there and nowhere else. No camera
//    identifier and no path is written.
#include "a0/m2/experimental/capture_registration.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/experimental/fixed_scale_correction.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include "a0/phase0/experimental/fixed_composite_registration_text.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace a0::phase0::experimental {

// ---- The composite ----
struct PairCompositeOutcome {
    std::optional<a0::m2::experimental::StillReviewResult> review;
    // Why there is no review: the selection's own reason when there is no usable condition, or the writer's refusal. Set when
    // `review` is empty.
    std::optional<a0::m2::experimental::FixedCompositeSkip> skip;
    std::string detail;   // the writer's message for a refusal (English, for logs)
};

// `request` has everything except the condition. `selection` is the result of SelectFixedComposite for the setup, which the caller
// has already logged when it was not usable. The condition and its profile are owned by `selection`, which must outlive the call.
// v1: the similarity composite (report v6); v2: the lens-aware one (v7, with the scale limit compared with the policy's); v3: the
// registered one (v8). Only FixedCompositeUnavailable is turned into a skip; every other failure is the caller's (the deadline).
[[nodiscard]] inline PairCompositeOutcome ComposePairWithSelection(const a0::m2::experimental::FixedCompositeSelection& selection,
    a0::m2::experimental::StillReviewRequest request,const std::optional<a0::m2::experimental::StillMergePolicy>& merge_policy){
    namespace m2e=a0::m2::experimental;
    PairCompositeOutcome outcome;
    const m2e::LoadedFixedCompositeCondition* condition=selection.loaded?std::get_if<m2e::LoadedFixedCompositeCondition>(&*selection.loaded):nullptr;
    const m2e::LoadedFixedCompositeConditionV2* condition_v2=selection.loaded?std::get_if<m2e::LoadedFixedCompositeConditionV2>(&*selection.loaded):nullptr;
    const m2e::LoadedFixedCompositeConditionV3* condition_v3=selection.loaded?std::get_if<m2e::LoadedFixedCompositeConditionV3>(&*selection.loaded):nullptr;
    const auto policy_scale=merge_policy?merge_policy->scale_max_percent:std::nullopt;
    const auto policy_check=merge_policy?m2e::FixedPolicyCheck::policy_without_scale_limit:m2e::FixedPolicyCheck::policy_absent;
    if(condition){
        request.fixed_composite=m2e::StillReviewRequest::FixedComposite{&condition->condition,condition->file_sha256,
            m2e::ComparePolicyLimit(condition->condition.scale_auto_correction_max_percent,policy_scale,policy_check)};
    }else if(condition_v2){
        request.fixed_composite_lens=m2e::StillReviewRequest::FixedCompositeLens{&condition_v2->condition,&condition_v2->profile,condition_v2->file_sha256,
            m2e::ComparePolicyLimit(condition_v2->condition.scale_auto_correction_max_percent,policy_scale,policy_check)};
    }else if(condition_v3){
        // The registered composite takes the deadline of the request as the time left for its stages (PlanReviewStages decides
        // which stages the remaining time holds; the 180-second window of the pair capture is not extended). 0 threads: the
        // hardware's count; the composite does not depend on it.
        request.fixed_composite_registration=m2e::StillReviewRequest::FixedCompositeRegistration{&condition_v3->condition,&condition_v3->profile,condition_v3->file_sha256,0};
    }else{
        outcome.skip=selection.reason;
        return outcome;
    }
    try{outcome.review=m2e::WriteUncalibratedStillReview(request);}
    catch(const m2e::FixedCompositeUnavailable& unavailable){outcome.skip=unavailable.Skip();outcome.detail=unavailable.what();}
    return outcome;
}

// ---- A composite that threw after both originals were saved ----
// Every failure other than FixedCompositeUnavailable (the 180-second window, a size mismatch, the gate, memory) leaves the originals
// on disk. The window turns this into "originals saved, no composite" the way a skip does, with the reason in a few words.

// The exception's own text can hold a path (std::filesystem::filesystem_error does): the screen and the log get this instead.
// Anything that starts like an absolute path (drive letter or UNC) is replaced; the length is capped and control characters are spaces.
[[nodiscard]] inline std::string RedactPaths(std::string_view text){
    std::string out;
    const auto is_drive_start=[&](std::size_t i){
        return i+2<text.size() && ((text[i]>='A' && text[i]<='Z') || (text[i]>='a' && text[i]<='z')) && text[i+1]==':' && (text[i+2]=='\\' || text[i+2]=='/') &&
            (i==0 || !((text[i-1]>='A' && text[i-1]<='Z') || (text[i-1]>='a' && text[i-1]<='z') || (text[i-1]>='0' && text[i-1]<='9')));
    };
    for(std::size_t i=0;i<text.size();){
        const bool unc=i+1<text.size() && text[i]=='\\' && text[i+1]=='\\';
        if(unc || is_drive_start(i)){
            // A path in quotes ends at the closing quote (it can hold spaces); any other ends at white space.
            const char quote=i>0 && (text[i-1]=='"' || text[i-1]=='\'')?text[i-1]:'\0';
            while(i<text.size() && (quote?text[i]!=quote:!(text[i]==' ' || text[i]=='\t' || text[i]=='\r' || text[i]=='\n')))++i;
            out+="<path>";
        }else{out+=static_cast<unsigned char>(text[i])<0x20?' ':text[i];++i;}
        if(out.size()>=200){out.resize(200);out+="...";break;}
    }
    return out;
}
// A summary of an exception with no path in it. A filesystem_error is reduced to the system's own message and code.
[[nodiscard]] inline std::string ErrorSummary(const std::exception& error){
    if(const auto* file=dynamic_cast<const std::filesystem::filesystem_error*>(&error))
        return "filesystem error: "+RedactPaths(file->code().message())+" (code "+std::to_string(file->code().value())+")";
    return RedactPaths(error.what());
}
// `deadline_passed`: the caller's own clock against the deadline it gave the composite, read when the exception is caught. The
// exceptions of the 180-second window are plain runtime_errors, so the clock says it, not the text.
[[nodiscard]] inline CompositeFailureReason ClassifyCompositeFailure(const std::exception& error,bool deadline_passed){
    if(dynamic_cast<const std::bad_alloc*>(&error))return CompositeFailureReason::out_of_memory;
    return deadline_passed?CompositeFailureReason::time_limit:CompositeFailureReason::other;
}

// What was left in a review folder after the composite failed.
enum class ReportlessCleanup {no_composite, removed, complete_kept, not_removed};
[[nodiscard]] inline std::string_view ReportlessCleanupCode(ReportlessCleanup cleanup){
    switch(cleanup){
    case ReportlessCleanup::no_composite: return "none";
    case ReportlessCleanup::removed: return "removed";
    case ReportlessCleanup::complete_kept: return "complete_kept";
    case ReportlessCleanup::not_removed: return "not_removed";
    }
    return "not_removed";
}
// A composite JPEG without its report looks like a finished one. After a failure this removes that one file, and nothing else:
// only `uncalibrated-review.jpg` in `review_directory`, only when the folder is a plain directory and the file a plain file (a
// symbolic link or junction is never followed), and only when `uncalibrated-review.json` is absent. The originals are not in this
// folder and are never touched; a `.partial` file is left as it is. A complete pair (JPEG and report) is kept.
[[nodiscard]] inline ReportlessCleanup RemoveReportlessComposite(const std::filesystem::path& review_directory){
    namespace fs=std::filesystem;
    std::error_code ec;
    // Absent is not an error; any other error means the state is unknown, and then nothing is removed.
    const auto missing=[](const fs::file_status& status,const std::error_code& error){return status.type()==fs::file_type::not_found || error==std::errc::no_such_file_or_directory;};
    const auto folder=fs::symlink_status(review_directory,ec);
    if(missing(folder,ec))return ReportlessCleanup::no_composite;
    if(ec || !fs::is_directory(folder))return ReportlessCleanup::not_removed;   // a link, a junction or a file: not followed, not touched
    const auto jpeg_path=review_directory/L"uncalibrated-review.jpg",report_path=review_directory/L"uncalibrated-review.json";
    ec.clear();
    const auto jpeg=fs::symlink_status(jpeg_path,ec);
    if(missing(jpeg,ec))return ReportlessCleanup::no_composite;
    if(ec)return ReportlessCleanup::not_removed;
    const auto report=fs::symlink_status(report_path,ec);
    if(!missing(report,ec))return ec?ReportlessCleanup::not_removed:ReportlessCleanup::complete_kept;
    if(!fs::is_regular_file(jpeg))return ReportlessCleanup::not_removed;
    ec.clear();
    return fs::remove(jpeg_path,ec) && !ec?ReportlessCleanup::removed:ReportlessCleanup::not_removed;
}

// ---- What is said after the shot (v8 only) ----
struct RegistrationReviewLines {
    // The status row, one sentence per line (UTF-8, LF between lines when joined by the caller).
    std::vector<std::string> status;
    // Every warning of the report, in the report's order: the label, the sentence and how to put it right.
    std::vector<std::string> warnings;
    // Log lines of key=value pairs (ASCII), the warnings with their sentences, and the time of each stage.
    std::vector<std::string> log;
};

namespace pair_review_detail {
namespace ex = a0::m2::experimental;
inline std::string Number(double value,int digits=3){
    if(!std::isfinite(value))return "nan";
    std::ostringstream s;s.imbue(std::locale::classic());s<<std::fixed<<std::setprecision(digits)<<value;
    return s.str();
}
inline std::string SurfaceStateCode(ex::SurfaceState state){
    return state==ex::SurfaceState::estimated?"estimated":state==ex::SurfaceState::not_estimated?"not-estimated":"disabled";
}
// The paper surface as a person reads it: the height at the seam's middle and the inclination.
inline std::string SurfaceText(const ex::SurfaceEstimate& surface){
    using namespace registration_text_detail;
    switch(surface.state){
    case ex::SurfaceState::estimated:
        return "紙面の高さ "+Q(surface.plane.h0_mm,Unit::mm)+"、傾き "+Q(surface.tilt_deg,Unit::deg);
    case ex::SurfaceState::not_estimated: return "紙面は推定できませんでした";
    case ex::SurfaceState::disabled: return "紙面は推定していません（条件に紙面の範囲がありません）";
    }
    return {};
}
inline RegistrationTextContext ContextOf(const ex::SavedFixedCompositeRegistration& record){
    RegistrationTextContext context;
    const auto& capture=record.registration;
    if(capture.registration.display.output_frame)context.output_frame=*capture.registration.display.output_frame;
    context.surface=capture.surface;
    context.pitch_mm=record.raster.dpi?25.4/record.raster.dpi:0.0;
    context.pose_display=capture.registration.display;
    if(registration_text_detail::HasDirection(capture.parallax.baseline))context.baseline=capture.parallax.baseline;
    context.parallax_summary=capture.parallax.summary;
    context.geometry_level=&capture.level;
    return context;
}
}

// `review` must hold a v8 record (fixed_composite_registration); an empty result otherwise. The context points into `review`.
[[nodiscard]] inline RegistrationReviewLines DescribeRegistrationReview(const a0::m2::experimental::StillReviewResult& review){
    using namespace pair_review_detail;
    RegistrationReviewLines lines;
    if(!review.fixed_composite_registration)return lines;
    const auto& record=*review.fixed_composite_registration;
    const auto& capture=record.registration;
    const auto context=ContextOf(record);

    // The status row: the level and why; the pose of B and the surface; the gate and its largest displacement; the warnings and the
    // assessment; the stages the time budget dropped.
    lines.status.push_back(GeometryLevelText(capture.level));
    lines.status.push_back(RegistrationPoseSummaryText(capture.registration,context)+"。"+SurfaceText(capture.surface));
    lines.status.push_back(RegistrationGateStateText(capture.gate));
    unsigned exceeded=0,unverified=0,notices=0;
    for(const auto& warning:record.warnings){
        const auto effective=EffectiveWarningClass(warning);
        if(effective==ex::WarningClass::exceeded)++exceeded;else if(effective==ex::WarningClass::unverified)++unverified;else ++notices;
        lines.warnings.push_back(RegistrationWarningLine(warning,context));
    }
    {
        std::string text=RegistrationAssessmentText(record.assessment.state);
        text+="。警告 "+std::to_string(record.warnings.size())+" 件（基準外 "+std::to_string(exceeded)+"、確かめられていません "+std::to_string(unverified)+"、お知らせ "+std::to_string(notices)+"）";
        // The most serious one is first in the report's order of codes only by chance: name the first exceeded one, else the first.
        const ex::RegistrationWarning* shown=nullptr;
        for(const auto& warning:record.warnings)if(EffectiveWarningClass(warning)==ex::WarningClass::exceeded){shown=&warning;break;}
        if(!shown && !record.warnings.empty())shown=&record.warnings.front();
        if(shown)text+="。"+RegistrationWarningLine(*shown,context);
        if(record.warnings.size()>1)text+="（全件は記録とログにあります）";
        lines.status.push_back(std::move(text));
    }
    // Stages dropped for the time; a stage that threw is in the warnings of the level and the gate.
    if(std::any_of(capture.time_budget.skipped.begin(),capture.time_budget.skipped.end(),[](const ex::SkippedStage& s){return s.reason=="time-budget";}))
        lines.status.push_back(TimeBudgetText(capture.time_budget));

    // The log.
    std::ostringstream head;head.imbue(std::locale::classic());
    head<<"pair_review_registration level="<<ex::GeometryLevelCode(capture.level.level)
        <<" estimate="<<ex::content_registration::RegistrationStateCode(capture.registration.state)
        <<" surface="<<SurfaceStateCode(capture.surface.state)
        <<" layer2="<<ex::parallax_correction::ParallaxStateCode(capture.parallax.state)
        <<" applied_pose="<<(capture.level.applied.pose?1:0)<<" applied_surface="<<(capture.level.applied.surface?1:0)<<" applied_parallax="<<(capture.level.applied.parallax?1:0)
        <<" beyond_envelope_applied="<<(capture.level.beyond_envelope_applied?1:0)
        <<" gate="<<ex::registration_gate::GateStateCode(capture.gate.state)<<" gate_cause="<<ex::registration_gate::GateCauseCode(capture.gate.cause);
    if(const auto maximum=registration_text_detail::GateMaximum(capture.gate))head<<" gate_maximum_px="<<Number(*maximum);else head<<" gate_maximum_px=none";
    head<<" assessment="<<ex::AssessmentStateText(record.assessment.state)<<" warnings="<<record.warnings.size()<<" exceeded="<<exceeded<<" unverified="<<unverified<<" notices="<<notices
        <<" condition_sequence="<<record.condition_sequence<<" seam_column="<<record.seam_column<<" raster="<<record.raster.width_pixels<<"x"<<record.raster.height_pixels
        <<" geometry_hash="<<capture.geometry_hash<<" quality_accepted=0";
    lines.log.push_back(head.str());

    {
        std::ostringstream pose;pose.imbue(std::locale::classic());
        pose<<"pair_review_registration_pose";
        for(const auto& c:capture.registration.components)
            pose<<" "<<ex::content_registration::EnvelopeComponentCode(c.component)<<"="<<Number(c.value)<<"/"<<Number(c.limit)<<(c.exceeded?"!":"");
        pose<<" surface_height_mm="<<(capture.surface.state==ex::SurfaceState::estimated?Number(capture.surface.plane.h0_mm):std::string("none"))
            <<" surface_tilt_deg="<<(capture.surface.state==ex::SurfaceState::estimated?Number(capture.surface.tilt_deg):std::string("none"))
            <<" text="<<RegistrationPoseSummaryText(capture.registration,context)<<"。"<<SurfaceText(capture.surface);
        lines.log.push_back(pose.str());
    }
    lines.log.push_back("pair_review_registration_gate text="+RegistrationGateStateText(capture.gate));
    for(std::size_t i=0;i<record.warnings.size();++i){
        const auto& warning=record.warnings[i];
        std::ostringstream line;line.imbue(std::locale::classic());
        line<<"pair_review_registration_warning index="<<(i+1)<<" class="<<ex::WarningClassCode(EffectiveWarningClass(warning))<<" code="<<ex::WarningCodeText(warning.code)
            <<" applied="<<(warning.applied?1:0)<<" text="<<lines.warnings[i];
        lines.log.push_back(line.str());
    }
    {
        // Milliseconds are for the log only; the report records which stages ran and which were dropped, not how long they took.
        std::ostringstream time;time.imbue(std::locale::classic());
        time<<"pair_review_registration_ms estimate_ms="<<Number(review.alignment_milliseconds,1)<<" composite_ms="<<Number(review.blend_milliseconds,1);
        if(review.registration_milliseconds){
            const auto& m=*review.registration_milliseconds;
            for(std::size_t i=0;i<m.stage.size();++i)if(m.stage[i])time<<" "<<ex::kReviewStageTexts[i].second<<"_ms="<<Number(*m.stage[i],1);
            if(m.composite)time<<" composite_stage_ms="<<Number(*m.composite,1);
        }
        time<<" planned=";
        for(std::size_t i=0;i<capture.time_budget.planned.size();++i)time<<(i?",":"")<<ex::ReviewStageText(capture.time_budget.planned[i]);
        time<<" skipped=";
        if(capture.time_budget.skipped.empty())time<<"none";
        for(std::size_t i=0;i<capture.time_budget.skipped.size();++i)time<<(i?",":"")<<ex::ReviewStageText(capture.time_budget.skipped[i].stage)<<(capture.time_budget.skipped[i].reason=="time-budget"?"":":"+capture.time_budget.skipped[i].reason);
        lines.log.push_back(time.str());
    }
    return lines;
}
}
