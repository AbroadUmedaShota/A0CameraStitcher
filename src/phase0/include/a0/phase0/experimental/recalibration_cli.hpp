#pragma once
// The development command line of the recalibration of camera B from one pair (#434). Built into StillReview only when the CMake option
// A0_BUILD_WPD_RECALIBRATION is ON (still_review_main.cpp includes this header under A0_ENABLE_WPD_RECALIBRATION); a build without the
// option has no entrance.
//
//   StillReview --recalibrate-from-pair --camera-a <jpg> --camera-b <jpg> --settings-folder <folder> --staging <new folder>
//               [--seam center|keep] [--raster follow-b|keep] [--unverified confirm|refuse] [--pair-id <id>] [--measured-at <UTC>]
//
// It reads the settings folder and the two originals, runs the job (the same code the preview runs), and writes its record into the new
// staging folder. It never writes into the settings folder, never confirms, and never touches a camera. The output has no path and no
// machine identifier: facts as key=value, the findings, then the lines of the screen.
//
// Exit code: 0 the calibration is usable (checked); 3 it is usable but unchecked (a person would have to confirm it after the warning);
// 1 it is not usable, or the job could not run; 2 wrong arguments.
#include "a0/phase0/experimental/recalibration_job.hpp"
#include <chrono>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0::experimental {
inline constexpr std::wstring_view kRecalibrationCommandName=L"--recalibrate-from-pair";
inline constexpr std::string_view kRecalibrationUsage=
    "usage: StillReview --recalibrate-from-pair --camera-a <jpg> --camera-b <jpg> --settings-folder <folder> --staging <new folder>\n"
    "                   [--seam center|keep] [--raster follow-b|keep] [--unverified confirm|refuse] [--pair-id <id>] [--measured-at <UTC>]\n";

struct RecalibrationCliParse {
    std::optional<RecalibrationJobRequest> request;   // without the hash, the clock, the stop and the computation (the caller sets them)
    std::string error;                                // ASCII: why the arguments were refused; empty when accepted
};

// `args` are the arguments after the command name. Every flag is given once with a value, and every flag but the optional ones is needed.
[[nodiscard]] inline RecalibrationCliParse ParseRecalibrationArguments(const std::vector<std::wstring>& args){
    namespace m2e=a0::m2::experimental;
    namespace rc=a0::m2::experimental::recalibration;
    RecalibrationCliParse out;
    const auto refuse=[&](std::string why){out.request.reset();out.error=std::move(why);return out;};
    RecalibrationJobRequest request;
    bool have_a=false,have_b=false,have_settings=false,have_staging=false,have_seam=false,have_raster=false,have_policy=false,have_pair=false,have_time=false;
    const auto ascii=[](const std::wstring& text)->std::optional<std::string>{
        std::string s;
        for(const wchar_t c:text){if(c<0x20 || c>0x7e)return std::nullopt;s.push_back(static_cast<char>(c));}
        return s;
    };
    for(std::size_t i=0;i<args.size();++i){
        const std::wstring_view flag(args[i]);
        if(i+1>=args.size())return refuse("a flag has no value");
        const std::wstring& value=args[++i];
        const auto once=[&](bool& seen){if(seen)return false;seen=true;return true;};
        if(value.empty())return refuse("a value is empty");
        if(flag==L"--camera-a"){if(!once(have_a))return refuse("--camera-a is given twice");request.original_a=std::filesystem::path(value);}
        else if(flag==L"--camera-b"){if(!once(have_b))return refuse("--camera-b is given twice");request.original_b=std::filesystem::path(value);}
        else if(flag==L"--settings-folder"){if(!once(have_settings))return refuse("--settings-folder is given twice");request.settings_folder=std::filesystem::path(value);}
        else if(flag==L"--staging"){if(!once(have_staging))return refuse("--staging is given twice");request.staging_folder=std::filesystem::path(value);}
        else if(flag==L"--seam"){
            if(!once(have_seam))return refuse("--seam is given twice");
            if(value==L"center")request.seam=rc::SeamPlacement::band_center;
            else if(value==L"keep")request.seam=rc::SeamPlacement::keep_previous;
            else return refuse("--seam is center or keep");
        }else if(flag==L"--raster"){
            if(!once(have_raster))return refuse("--raster is given twice");
            if(value==L"follow-b")request.raster=rc::RasterPlacement::follow_camera_b;
            else if(value==L"keep")request.raster=rc::RasterPlacement::keep;
            else return refuse("--raster is follow-b or keep");
        }else if(flag==L"--unverified"){
            if(!once(have_policy))return refuse("--unverified is given twice");
            if(value==L"confirm")request.unverified_policy=UnverifiedPolicy::confirm_with_warning;
            else if(value==L"refuse")request.unverified_policy=UnverifiedPolicy::refuse_to_confirm;
            else return refuse("--unverified is confirm or refuse");
        }else if(flag==L"--pair-id"){
            if(!once(have_pair))return refuse("--pair-id is given twice");
            const auto text=ascii(value);
            if(!text || !m2e::fixed_composite_detail::IsPairId(*text))return refuse("--pair-id is not a pair identity");
            request.pair_id=*text;
        }else if(flag==L"--measured-at"){
            if(!once(have_time))return refuse("--measured-at is given twice");
            const auto text=ascii(value);
            if(!text || !m2e::fixed_composite_detail::IsUtcTimestamp(*text))return refuse("--measured-at is not YYYY-MM-DDTHH:MM:SSZ");
            request.measured_at_utc=*text;
        }else return refuse("a flag is unknown");
    }
    if(!have_a || !have_b || !have_settings || !have_staging)return refuse("--camera-a, --camera-b, --settings-folder and --staging are needed");
    out.request=std::move(request);
    return out;
}

namespace recalibration_cli_detail {
inline std::string Number(double value){return recalibration_job_detail::Num(value);}
}

// Runs the command (`args` are the arguments after the command name) and writes what it found to `out`. Returns the exit code.
// `computation` is the real one unless a test passes another.
[[nodiscard]] inline int RunRecalibrationCommand(const std::vector<std::wstring>& args,std::ostream& out,const a0::m2::experimental::FixedCompositeSha256& hash,
    std::chrono::system_clock::time_point now,const RecalibrationComputation& computation=RecalibrationComputation{}){
    using recalibration_cli_detail::Number;
    const auto parsed=ParseRecalibrationArguments(args);
    if(!parsed.request){
        out<<"recalibration_done=0 camera_operations=0 settings_written=0\nerror="<<parsed.error<<'\n'<<kRecalibrationUsage<<std::flush;
        return 2;
    }
    auto request=*parsed.request;
    request.hash=hash;request.now=now;request.computation=computation;
    request.live_view_stopped=true;   // no live view exists in this process
    if(request.measured_at_utc.empty())request.measured_at_utc=std::format("{:%Y-%m-%dT%H:%M:%SZ}",std::chrono::floor<std::chrono::seconds>(now));
    request.on_stage=[&out](RecalibrationStage stage,double seconds){out<<"stage name="<<RecalibrationStageCode(stage)<<" at_s="<<Number(seconds)<<'\n'<<std::flush;};
    RecalibrationJobOutcome outcome;
    try{outcome=RunRecalibrationJob(request);}
    catch(const std::exception& error){
        out<<"recalibration_done=0 camera_operations=0 settings_written=0\nerror="<<ErrorSummary(error)<<'\n'<<std::flush;
        return 1;
    }
    const auto& d=outcome.facts.display;
    const auto& g=outcome.facts.gate;
    out<<"recalibration_done=1 verdict="<<RecalibrationVerdictCode(outcome.verdict)<<" unverified="<<(outcome.unverified()?1:0)<<" confirmed=0 camera_operations=0 settings_written=0\n";
    for(const auto& f:outcome.findings)out<<"finding class="<<FindingClassCode(f.klass)<<" code="<<f.code<<(f.reason.empty()?std::string{}:" reason="+f.reason)<<'\n';
    if(d.has_motion)
        out<<"motion right_mm="<<Number(d.motion.camera_motion_mm.x)<<" down_mm="<<Number(d.motion.camera_motion_mm.y)<<" rotation_clockwise_deg="<<Number(d.motion.rotation_clockwise_deg)
           <<" tilt_deg="<<Number(d.pose.tilt_deg)<<" height_percent="<<Number(d.pose.height_percent)<<'\n';
    if(d.has_overlap)out<<"overlap narrowest_mm="<<Number(d.narrowest_overlap_mm)<<'\n';
    if(d.has_new)
        out<<"seam previous_mm="<<Number(d.seam_x_previous_mm)<<" new_mm="<<Number(d.seam_x_new_mm)<<" raster_width_px="<<d.width_previous_px<<"->"<<d.width_new_px
           <<" raster_height_px="<<d.height_previous_px<<"->"<<d.height_new_px<<'\n';
    if(d.has_edge_distance)out<<"edge_share a="<<Number(d.edge_share_a)<<" b="<<Number(d.edge_share_b)<<'\n';
    if(g.known)
        out<<"gate state="<<a0::m2::experimental::registration_gate::GateStateCode(g.state)<<" cause="<<a0::m2::experimental::registration_gate::GateCauseCode(g.cause)
           <<" maximum_px="<<(g.maximum_px?Number(*g.maximum_px):std::string("none"))<<" limit_px="<<Number(g.limit_px)<<" measured_row_share="<<Number(g.measured_share)<<'\n';
    out<<"check_seconds="<<Number(outcome.seconds)<<'\n';
    for(const auto& line:outcome.log_lines)out<<"log "<<line<<'\n';
    for(const auto& line:outcome.screen_lines)out<<"screen "<<line<<'\n';
    out<<std::flush;
    return outcome.verdict==RecalibrationVerdict::usable?0:outcome.verdict==RecalibrationVerdict::usable_unverified?3:1;
}
}
