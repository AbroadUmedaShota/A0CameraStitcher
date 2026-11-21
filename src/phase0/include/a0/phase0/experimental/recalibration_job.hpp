#pragma once
// The job and the confirmation of the recalibration of camera B from one pair (#434; docs/design/recalibration-from-pair.md 2.2 to 2.8).
// Header only, no window and no camera: the preview and the development command line run the same code.
//
//  RunRecalibrationJob   reads the condition in force (read only), decodes the two originals, asks the computation of
//                        fixed_composite_recalibration.hpp for a proposal, composes a sample with the candidate (the same composite
//                        a capture makes, with the candidate as an in-memory selection), classifies what was found, and writes a
//                        record into a NEW staging folder. It never writes into the settings folder.
//  ConfirmRecalibration  the only function that writes into the settings folder: the profile copy first, then the revision, with the
//                        checks of the design (2.8), from the candidate that is in memory.
//
// The live view must be stopped while the job runs (it takes the cores and about 217 MB for the two originals): the job asks the caller
// to say so (`live_view_stopped`) and refuses otherwise. The caller (the preview) owns that; nothing here touches a camera or a session.
//
// What is checked and what is not (the owner's decision on Q3): findings are of three classes (recalibration_text.hpp).
//   no_candidate / rejected  the calibration is not offered, whatever the policy.
//   unverified               the check could not be made because there was too little to measure (too few marks), and nothing was
//                            found wrong. With the policy `confirm_with_warning` the verdict is `usable_unverified` and the person may
//                            confirm after seeing the warning; with `refuse_to_confirm` it is `unusable`. The policy is the constant
//                            kRecalibrationUnverifiedPolicy, one place. An estimate that is undetermined counts as this only when ALL of
//                            its reasons are of the "too few marks" family (content_registration::AssessEvidenceShortage, the one place
//                            that decides it); one other reason, or one that is not known, makes it `rejected`. The gate is told apart by
//                            the codes of its findings (ClassifySample), not by its state. A gate that did not run, or whose own setting
//                            is inconsistent (gate-undetermined), is a check that did not work: it is `rejected`.
// Nothing about "unchecked" is stored in the condition (its format is not changed): the mark is in the record of the staging folder
// (recalibration-report.json, and confirmation.json after a confirmation) and in the log.
//
// The SHA-256 provider is a parameter (StillReview does not link the camera core, which holds the other one); nothing here uses
// a0_phase0_core. No path and no machine identifier is written to the screen lines, the log lines or the report.
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_profile_common.hpp"
#include "a0/m2/experimental/fixed_composite_recalibration.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/experimental/parallax_correction.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/optical_planner.hpp"
#include "a0/phase0/experimental/pair_review_registration.hpp"
#include "a0/phase0/experimental/recalibration_text.hpp"
#include "a0/phase0/experimental/seam_crops_export.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace a0::phase0::experimental {

// ---- The owner's decisions, in one place ----
// Q1 where the seam goes, Q2 how big the composite is, Q3 what to do with a calibration that could not be checked. Each is a default
// of the request; a caller (or a test) may pass the other value.
enum class UnverifiedPolicy {confirm_with_warning,refuse_to_confirm};
// What the gate's `unconfirmed-content` (a point over the line that a second measurement could not confirm) and `one-sided-content`
// (content in one camera only) mean. Both are "measured, and it looked wrong", not "too little to measure": the default refuses them
// (`reject`). The owner's answer is awaited (Q1 of the ruling); `allow_with_warning` makes them `unverified`, a calibration the person may
// confirm after the warning. kRecalibrationSuspectContentPolicy is the only place that says which; the tests run both values.
enum class SuspectContentPolicy {reject,allow_with_warning};
inline constexpr a0::m2::experimental::recalibration::SeamPlacement kRecalibrationSeamPlacement=a0::m2::experimental::recalibration::SeamPlacement::band_center;
inline constexpr a0::m2::experimental::recalibration::RasterPlacement kRecalibrationRasterPlacement=a0::m2::experimental::recalibration::RasterPlacement::follow_camera_b;
inline constexpr UnverifiedPolicy kRecalibrationUnverifiedPolicy=UnverifiedPolicy::confirm_with_warning;
inline constexpr SuspectContentPolicy kRecalibrationSuspectContentPolicy=SuspectContentPolicy::reject;
// The time the sample composite may take (the stages it holds are all run). Not a line of acceptance; the 180-second window of a pair
// capture is for the shutter and is not used here (the design, 2.4).
inline constexpr std::chrono::seconds kRecalibrationSampleWindow{900};

// ---- Pure decisions (no files, no images) ----
// How each refusal of the computation counts (docs/design/recalibration-from-pair.md, section 5):
//   no_candidate  no-overlap, band-uncovered, range-collapsed, estimate-failed, estimate-not-verified, stopped: no position was found that
//                 holds, or the raster that follows it cannot be made; nothing to confirm. Only reestimate-not-verified can become
//                 `unverified`, so none of these can be confirmed, with a warning or without
//   rejected      tilt-exceeded, height-exceeded, creation-check-failed, raster-too-large, raster-outside-sheet, profile-invalid,
//                 reestimate-displacement; reestimate-not-verified when the estimator on the new profile found the pose out of range;
//                 reestimate-doubtful (it replaces the code reestimate-not-verified) when that estimator was undetermined for a reason
//                 that is not "too few marks"
//   unverified    reestimate-not-verified when that estimator was undetermined and every one of its reasons is "too few marks"
// `reason` of an estimate-not-verified finding is the estimator's first reason code; of a reestimate-doubtful finding, the first reason
// that is not "too few marks"; of a no-overlap finding, the reason of the wide search; of a band-uncovered or range-collapsed finding,
// the lower-level reason that came with the refusal (Proposal::refusal_reason). `limits` are the condition's (alpha and beta of the estimator's first check, and the minimum number of unique blocks).
[[nodiscard]] inline std::vector<RecalibrationFinding> ClassifyProposal(const a0::m2::experimental::recalibration::Proposal& proposal,
    const a0::m2::experimental::content_registration::RegistrationLimits& limits){
    namespace rc=a0::m2::experimental::recalibration;
    namespace cr=a0::m2::experimental::content_registration;
    std::vector<RecalibrationFinding> findings;
    for(const auto refusal:proposal.refusals){
        RecalibrationFinding finding;
        finding.code=std::string(rc::RefusalCode(refusal));
        switch(refusal){
        case rc::Refusal::no_overlap: case rc::Refusal::band_uncovered: case rc::Refusal::range_collapsed:
            finding.klass=FindingClass::no_candidate;
            finding.reason=rc::RefusalReasonOf(proposal,refusal);
            break;
        case rc::Refusal::estimate_failed: case rc::Refusal::stopped:
            finding.klass=FindingClass::no_candidate;break;
        case rc::Refusal::estimate_not_verified:
            finding.klass=FindingClass::no_candidate;
            if(!proposal.estimate.reasons.empty())finding.reason=proposal.estimate.reasons.front();
            break;
        case rc::Refusal::reestimate_not_verified:{
            const auto& check=proposal.verification;
            finding.klass=FindingClass::rejected;
            if(check.state==cr::RegistrationState::undetermined){
                const auto assessed=cr::AssessEvidenceShortage(check.reasons,check.evidence,limits);
                if(assessed.shortage_only){finding.klass=FindingClass::unverified;finding.reason=check.reasons.front();}
                else{finding.code="reestimate-doubtful";finding.reason=assessed.doubt;}
            }else if(!check.reasons.empty())finding.reason=check.reasons.front();
            break;
        }
        default:
            finding.klass=FindingClass::rejected;break;
        }
        findings.push_back(std::move(finding));
    }
    return findings;
}

// A refusal list that is only "the check could not be made for want of marks": the candidate may be rebuilt from the pose and offered as
// unchecked. The class is the one ClassifyProposal gives (one decision, not a second copy of it).
[[nodiscard]] inline bool OnlyUnverifiedCheck(const a0::m2::experimental::recalibration::Proposal& proposal,
    const a0::m2::experimental::content_registration::RegistrationLimits& limits){
    namespace rc=a0::m2::experimental::recalibration;
    return !proposal.ready && proposal.refusals.size()==1 && proposal.refusals.front()==rc::Refusal::reestimate_not_verified &&
        proposal.verification.performed && proposal.has_pose && !proposal.estimate_record_text.empty() &&
        ClassifyProposal(proposal,limits).front().klass==FindingClass::unverified;
}

// What the sample composite said, reduced to what the decision needs.
struct RecalibrationSampleFacts {
    bool composed{};                                              // a v8 record exists
    bool threw{};                                                 // the writer threw (time, memory, ...)
    bool out_of_memory{};
    std::optional<a0::m2::experimental::FixedCompositeSkip> skip; // the writer refused before creating anything
    a0::m2::experimental::content_registration::RegistrationState estimate{a0::m2::experimental::content_registration::RegistrationState::undetermined};
    // The sample's estimate: its reasons, and the counts of its fit (what AssessEvidenceShortage needs, copied from the diagnostics in memory).
    std::vector<std::string> estimate_reasons;
    a0::m2::experimental::content_registration::EvidenceCounts estimate_evidence;
    a0::m2::experimental::registration_gate::GateState gate{a0::m2::experimental::registration_gate::GateState::not_run};
    a0::m2::experimental::registration_gate::GateCause gate_cause{a0::m2::experimental::registration_gate::GateCause::none};
    // The codes of the gate's own findings (the `reason` of each entry of RegistrationGateResult::gate_findings).
    std::vector<std::string> gate_findings;
    // Why the gate did not run (the reason of its entry in the time budget's skipped stages: time-budget or stage-failed); empty when it
    // ran or when no entry is found. Only carried to the finding, which tells the person what to do next.
    std::string gate_skipped_reason;
};

// One finding of the gate (the codes of registration_gate.hpp's GateFinding): how it counts, and the code that carries its sentence.
//   too little to measure (unverified)   nothing-measured, content-unmeasured, dx-unmeasured, placement-unverified
//   measured and it looked wrong         unconfirmed-content, one-sided-content: by `policy` (kRecalibrationSuspectContentPolicy)
//   a problem of the composite itself    transition-outside-band, and a code that is not known here (never confirmed)
//   the check itself is broken           gate-undetermined (the setting of the seam measurement is inconsistent): never confirmed
[[nodiscard]] inline RecalibrationFinding ClassifyGateFinding(std::string_view symbol,SuspectContentPolicy policy){
    const auto suspect=policy==SuspectContentPolicy::allow_with_warning?FindingClass::unverified:FindingClass::rejected;
    if(symbol=="nothing-measured")return {FindingClass::unverified,"gate-nothing-measured",{}};
    if(symbol=="content-unmeasured")return {FindingClass::unverified,"gate-content-unmeasured",{}};
    if(symbol=="dx-unmeasured")return {FindingClass::unverified,"gate-dx-unmeasured",{}};
    if(symbol=="placement-unverified")return {FindingClass::unverified,"gate-placement-unverified",{}};
    if(symbol=="gate-undetermined")return {FindingClass::rejected,"gate-undetermined",{}};
    if(symbol=="unconfirmed-content")return {suspect,"gate-unconfirmed-content",{}};
    if(symbol=="one-sided-content")return {suspect,"gate-one-sided-content",{}};
    if(symbol=="transition-outside-band")return {FindingClass::rejected,"gate-transition-outside-band",{}};
    return {FindingClass::rejected,"gate-unknown",std::string(symbol)};
}

// The gate and the estimate of the sample. The gate says `fail` for two different things: an excess that it measured (cause lift, pose,
// displacement: checked and wrong, rejected) and a seam that it could not measure (cause unmeasurable). The second, and `undetermined`,
// are told apart by the codes of the gate's findings (ClassifyGateFinding), not by the state: a paper with few marks ends in "too little
// to measure", the case the owner wants a person to be able to decide; a finding of another kind is never confirmed. A gate that is not
// `pass` and gives no finding to explain it is not trusted either (`gate-unknown`), nor is a state that this function does not know. A
// gate that did not run (`not_run`) is never confirmed, whatever the cause (the time ran short, or the stage failed): that is a check that
// did not work, not too little to measure; the cause is carried in the finding's reason, and only the sentence differs. The estimate is
// told apart by all of its reasons.
[[nodiscard]] inline std::vector<RecalibrationFinding> ClassifySample(const RecalibrationSampleFacts& facts,
    const a0::m2::experimental::content_registration::RegistrationLimits& limits,
    SuspectContentPolicy policy=kRecalibrationSuspectContentPolicy){
    namespace cr=a0::m2::experimental::content_registration;
    namespace gate=a0::m2::experimental::registration_gate;
    std::vector<RecalibrationFinding> findings;
    const auto add=[&](FindingClass klass,const char* code,std::string reason={}){findings.push_back({klass,code,std::move(reason)});};
    if(facts.out_of_memory){add(FindingClass::rejected,"out-of-memory");return findings;}
    if(facts.threw){add(FindingClass::rejected,"sample-failed");return findings;}
    if(facts.skip){add(FindingClass::rejected,"sample-skipped");return findings;}
    if(!facts.composed){add(FindingClass::rejected,"sample-failed");return findings;}
    if(facts.estimate==cr::RegistrationState::undetermined){
        const auto assessed=cr::AssessEvidenceShortage(facts.estimate_reasons,facts.estimate_evidence,limits);
        if(assessed.shortage_only)add(FindingClass::unverified,"sample-estimate-undetermined",facts.estimate_reasons.front());
        else add(FindingClass::rejected,"sample-estimate-doubtful",assessed.doubt);
    }
    else if(facts.estimate==cr::RegistrationState::out_of_envelope)add(FindingClass::rejected,"sample-estimate-out-of-envelope");
    const bool unmeasurable=facts.gate==gate::GateState::fail && facts.gate_cause==gate::GateCause::unmeasurable;
    if(facts.gate==gate::GateState::pass){}
    else if(facts.gate==gate::GateState::not_run)add(FindingClass::rejected,"gate-not-run",facts.gate_skipped_reason);
    else if(facts.gate==gate::GateState::fail && !unmeasurable)add(FindingClass::rejected,"gate-fail");
    else if(facts.gate==gate::GateState::undetermined || unmeasurable){
        std::vector<std::string> seen;
        for(const auto& symbol:facts.gate_findings){
            if(std::find(seen.begin(),seen.end(),symbol)!=seen.end())continue;
            seen.push_back(symbol);
            findings.push_back(ClassifyGateFinding(symbol,policy));
        }
        if(seen.empty())add(FindingClass::rejected,"gate-unknown");
    }
    else add(FindingClass::rejected,"gate-unknown");   // a state that is not known here is not trusted: only `pass` passes
    return findings;
}

// The verdict from the findings. A calibration without a candidate is never usable.
[[nodiscard]] inline RecalibrationVerdict DecideVerdict(const std::vector<RecalibrationFinding>& findings,UnverifiedPolicy policy,bool has_candidate){
    if(!has_candidate)return RecalibrationVerdict::unusable;
    bool unverified=false;
    for(const auto& finding:findings){
        if(finding.klass!=FindingClass::unverified)return RecalibrationVerdict::unusable;
        unverified=true;
    }
    if(!unverified)return RecalibrationVerdict::usable;
    return policy==UnverifiedPolicy::confirm_with_warning?RecalibrationVerdict::usable_unverified:RecalibrationVerdict::unusable;
}

// ---- The request and the outcome ----
// The computation, replaceable so that the rest can be tried in seconds (the tests use a fake that returns a ready proposal at once).
struct RecalibrationComputation {
    std::function<a0::m2::experimental::recalibration::Proposal(const a0::m2::experimental::recalibration::Inputs&)> propose=
        [](const a0::m2::experimental::recalibration::Inputs& in){return a0::m2::experimental::recalibration::ProposeRecalibration(in);};
    std::function<a0::m2::experimental::recalibration::Proposal(const a0::m2::experimental::recalibration::Inputs&,
        const a0::m2::experimental::content_registration::PoseParameters&,const a0::m2::experimental::recalibration::ProfileFit&,std::string)> from_pose=
        [](const a0::m2::experimental::recalibration::Inputs& in,const a0::m2::experimental::content_registration::PoseParameters& pose,
            const a0::m2::experimental::recalibration::ProfileFit& fit,std::string record){
            return a0::m2::experimental::recalibration::ProposeFromPose(in,pose,fit,std::move(record));};
};

struct RecalibrationJobRequest {
    // The two originals of the pair (JPEG files), in the stored pixel order. Their SHA-256: empty means "take the file's own".
    std::filesystem::path original_a,original_b;
    std::string sha_a,sha_b;
    // The identity of the pair (pair-..., as the capture's run) and the time of the shot (UTC, YYYY-MM-DDTHH:MM:SSZ). Empty pair_id:
    // one is made from the two hashes. measured_at_utc is required.
    std::string pair_id,measured_at_utc;
    std::filesystem::path settings_folder;   // read only
    std::filesystem::path staging_folder;    // a new folder (in the capture's run folder); an existing one is refused
    a0::m2::experimental::recalibration::SeamPlacement seam=kRecalibrationSeamPlacement;
    a0::m2::experimental::recalibration::RasterPlacement raster=kRecalibrationRasterPlacement;
    UnverifiedPolicy unverified_policy=kRecalibrationUnverifiedPolicy;
    SuspectContentPolicy suspect_content_policy=kRecalibrationSuspectContentPolicy;
    a0::m2::experimental::FixedCompositeSha256 hash;
    // The caller says that the live view is stopped for the length of the job. Refused otherwise.
    bool live_view_stopped{false};
    std::stop_token stop;
    unsigned thread_count{0};                // 0: the hardware's count
    std::chrono::seconds sample_window=kRecalibrationSampleWindow;
    std::chrono::system_clock::time_point now{};   // the time of the new revision (the caller's clock)
    // Called when a stage begins, with the seconds since the job began (the screen shows the name and the seconds).
    std::function<void(RecalibrationStage,double)> on_stage;
    RecalibrationComputation computation;
};

struct RecalibrationJobOutcome {
    RecalibrationVerdict verdict{RecalibrationVerdict::unusable};
    // True when the person may confirm (usable, or usable_unverified); an unchecked calibration needs the acknowledgement.
    bool ready{};
    [[nodiscard]] bool unverified() const {return verdict==RecalibrationVerdict::usable_unverified;}
    std::vector<RecalibrationFinding> findings;
    std::vector<std::string> screen_lines,log_lines;
    RecalibrationResultFacts facts;
    // What the wide search found (set when it ran), and the raster that the fine stage ran on (set when that stage was started): for the
    // report and the command line, whether or not a candidate came out.
    std::optional<a0::m2::experimental::recalibration::WideStart> wide;
    std::optional<a0::m2::render::OutputRaster> fine_fit_raster;
    // The sample, for the screen: the thumbnail of the composite and the three crops of the seam (one sheet).
    a0::m2::render::BgrImage thumbnail;
    std::optional<SeamCropsExported> seam_crops;
    // What a confirmation writes, from memory (nothing is read back from the staging folder).
    std::optional<a0::m2::experimental::recalibration::Candidate> candidate;
    std::string parent_sha256,pair_id;
    std::uint32_t sequence{};
    std::filesystem::path staging_folder;
    double seconds{};
};

// ---- Small file helpers ----
namespace recalibration_job_detail {
namespace fs=std::filesystem;
namespace m2e=a0::m2::experimental;
namespace rc=a0::m2::experimental::recalibration;
namespace cr=a0::m2::experimental::content_registration;
namespace render=a0::m2::render;

inline constexpr std::uintmax_t kMaximumOriginalBytes=256ull*1024*1024;

inline std::optional<std::vector<std::uint8_t>> ReadPlainFile(const fs::path& path,std::uintmax_t limit){
    std::error_code ec;
    const auto status=fs::symlink_status(path,ec);
    if(ec || status.type()!=fs::file_type::regular)return std::nullopt;
    const auto size=fs::file_size(path,ec);
    if(ec || size>limit)return std::nullopt;
    std::ifstream in(path,std::ios::binary);
    if(!in)return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(static_cast<std::size_t>(in.gcount())!=bytes.size())return std::nullopt;
    return bytes;
}
inline std::string Sha(const m2e::FixedCompositeSha256& hash,const std::vector<std::uint8_t>& bytes){
    return hash(std::vector<unsigned char>(bytes.begin(),bytes.end()));
}
inline std::string Sha(const m2e::FixedCompositeSha256& hash,const std::string& text){
    return hash(std::vector<unsigned char>(text.begin(),text.end()));
}
inline void WriteNew(const fs::path& path,const std::string& text){
    seam_crops_detail::WriteNewFileDurably(path,std::vector<std::uint8_t>(text.begin(),text.end()));
}

// The JPEG as BGR in the stored pixel order (the registered composite works on that order; nothing is rotated). One frame, a JPEG
// container, no more than render::kMaximumDecodedPixels. Returns an empty image on any failure.
inline render::BgrImage DecodeJpegBytes(const std::vector<std::uint8_t>& bytes){
    using Microsoft::WRL::ComPtr;
    seam_crops_detail::Apartment apartment;
    ComPtr<IWICImagingFactory> factory;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return {};
    ComPtr<IWICStream> stream;
    if(FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),static_cast<DWORD>(bytes.size()))))return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if(FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder)))return {};
    GUID container{};
    if(FAILED(decoder->GetContainerFormat(&container)) || !IsEqualGUID(container,GUID_ContainerFormatJpeg))return {};
    UINT frames=0;
    if(FAILED(decoder->GetFrameCount(&frames)) || frames!=1)return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    if(FAILED(decoder->GetFrame(0,&frame)))return {};
    UINT width=0,height=0;
    if(FAILED(frame->GetSize(&width,&height)) || !width || !height)return {};
    if(width>render::kMaximumImageDimension || height>render::kMaximumImageDimension || static_cast<std::uint64_t>(width)*height>render::kMaximumDecodedPixels)return {};
    ComPtr<IWICFormatConverter> converter;
    if(FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom)))return {};
    render::BgrImage image{width,height,std::vector<std::uint8_t>(static_cast<std::size_t>(width)*height*3)};
    if(FAILED(converter->CopyPixels(nullptr,width*3u,static_cast<UINT>(image.bgr.size()),image.bgr.data())))return {};
    return image;
}

// ---- A small JSON writer for the report (values are ASCII codes and numbers: no sentence and no path goes in) ----
inline std::string Esc(std::string_view text){
    std::string out="\"";
    for(const char c:text){
        if(c=='"' || c=='\\'){out+='\\';out+=c;}
        else if(static_cast<unsigned char>(c)<0x20 || static_cast<unsigned char>(c)>0x7e)out+='?';
        else out+=c;
    }
    return out+"\"";
}
// Fixed notation with up to 6 decimals (no exponent: the JSON reader of the tests takes plain numbers only), zeros trimmed.
inline std::string Num(double value){
    if(!std::isfinite(value))return "null";
    char buffer[400];
    const auto r=std::to_chars(buffer,buffer+sizeof buffer,value,std::chars_format::fixed,6);
    if(r.ec!=std::errc{})return "null";
    std::string text(buffer,r.ptr);
    if(text.find('.')!=std::string::npos){
        while(!text.empty() && text.back()=='0')text.pop_back();
        if(!text.empty() && text.back()=='.')text.pop_back();
    }
    return text=="-0" || text.empty()?"0":text;
}
inline std::string Num(const std::optional<double>& value){return value?Num(*value):"null";}
inline std::string Bool(bool value){return value?"true":"false";}

// The outer iterations of the check's estimate: the count that the estimate carried, or null (the log: "-") when it was not passed on (the
// default of EvidenceCounts). The default is not shown as a number.
inline bool OuterIterationsCarried(const rc::Verification& v){return v.evidence.outer_iterations!=cr::EvidenceCounts{}.outer_iterations;}
inline std::string OuterIterationsJson(const rc::Verification& v){return OuterIterationsCarried(v)?std::to_string(v.evidence.outer_iterations):std::string("null");}
// The change of the pose's displacement in each outer iteration, the numbers with `separator` between them.
inline std::string ChangeHistoryText(const rc::Verification& v,const char* separator){
    std::string text;
    for(std::size_t i=0;i<v.change_history_px.size();++i)text+=(i?std::string(separator):std::string{})+Num(v.change_history_px[i]);
    return text;
}

struct ReportInput {
    const RecalibrationJobOutcome* outcome{};
    std::string policy;
    std::string estimate_state;
    std::vector<std::string> estimate_reasons;
    const rc::Verification* verification{};
    std::string original_a_sha,original_b_sha;
    std::optional<std::string> level;
    // Whether the sample's capture applied a registration beyond the condition's ranges (the level's `beyond_envelope_applied`); empty when the sample was not composed.
    std::optional<bool> sample_beyond_envelope_applied;
};
inline std::string ReportJson(const ReportInput& in){
    const auto& o=*in.outcome;
    const auto& d=o.facts.display;
    std::ostringstream s;
    s<<"{\"schema\":\"a0.pair-recalibration-report.v1\",\"pairId\":"<<Esc(o.pair_id)<<",\"referenceCamera\":"<<Esc(rc::kReferenceCamera)
     <<",\"targetCamera\":"<<Esc(rc::kTargetCamera)<<",\"verdict\":"<<Esc(RecalibrationVerdictCode(o.verdict))
     <<",\"unverified\":"<<Bool(o.unverified())<<",\"unverifiedPolicy\":"<<Esc(in.policy)<<",\"findings\":[";
    for(std::size_t i=0;i<o.findings.size();++i){
        const auto& f=o.findings[i];
        s<<(i?",":"")<<"{\"class\":"<<Esc(FindingClassCode(f.klass))<<",\"code\":"<<Esc(f.code)<<",\"reason\":"<<Esc(f.reason)<<"}";
    }
    s<<"],\"originalSha256\":{"<<Esc(rc::kReferenceCamera)<<":"<<Esc(in.original_a_sha)<<","<<Esc(rc::kTargetCamera)<<":"<<Esc(in.original_b_sha)<<"}"
     <<",\"parent\":{\"revisionSha256\":"<<Esc(o.parent_sha256)<<"}";
    if(o.candidate){
        const auto& c=*o.candidate;
        s<<",\"candidate\":{\"sequence\":"<<c.condition.revision.sequence<<",\"revisionSha256\":"<<Esc(c.sha256)<<",\"profileFileSha256\":"<<Esc(c.condition.source.profile_file_sha256)
         <<",\"estimateRecordSha256\":"<<Esc(c.condition.revision.change.report_sha256.value_or(std::string{}))
         <<",\"seamColumn\":"<<c.condition.seam.column<<",\"widthPixels\":"<<c.condition.raster.width_pixels<<",\"heightPixels\":"<<c.condition.raster.height_pixels<<"}";
    }
    // The wide search and the raster of the fine stage: null when that stage did not run. The pose and the residuals of the wide search are
    // null when it found no pose; the pose is the target camera's against the reference camera.
    s<<",\"wideSearch\":";
    if(o.wide){
        const auto& w=*o.wide;
        s<<"{\"determined\":"<<Bool(w.determined)<<",\"reason\":"<<Esc(w.reason)<<",\"tilesAgreed\":"<<w.tiles_agreed<<",\"correspondences\":"<<w.correspondences;
        if(w.determined)
            s<<",\"txMm\":"<<Num(w.pose.tx_mm)<<",\"tyMm\":"<<Num(w.pose.ty_mm)<<",\"rzDeg\":"<<Num(w.pose.rz_deg)
             <<",\"residualRmsMm\":"<<Num(w.residual_rms_mm)<<",\"residualMaxMm\":"<<Num(w.residual_max_mm)<<"}";
        else s<<",\"txMm\":null,\"tyMm\":null,\"rzDeg\":null,\"residualRmsMm\":null,\"residualMaxMm\":null}";
    }else s<<"null";
    s<<",\"fineFitRaster\":";
    if(o.fine_fit_raster){
        const auto& r=*o.fine_fit_raster;
        s<<"{\"widthPx\":"<<r.width_pixels<<",\"heightPx\":"<<r.height_pixels<<",\"topMm\":"<<Num(static_cast<double>(r.region_um.top)/1000.0)
         <<",\"bottomMm\":"<<Num(static_cast<double>(r.region_um.bottom)/1000.0)<<",\"rightMm\":"<<Num(static_cast<double>(r.region_um.right)/1000.0)<<"}";
    }else s<<"null";
    s<<",\"estimate\":{\"state\":"<<Esc(in.estimate_state)<<",\"reasons\":[";
    for(std::size_t i=0;i<in.estimate_reasons.size();++i)s<<(i?",":"")<<Esc(in.estimate_reasons[i]);
    s<<"]}";
    if(in.verification){
        const auto& v=*in.verification;
        s<<",\"verification\":{\"performed\":"<<Bool(v.performed)<<",\"state\":"<<Esc(cr::RegistrationStateCode(v.state))<<",\"inliers\":"<<v.inliers<<",\"points\":"<<v.points<<",\"seamDisplacementPx\":"<<Num(v.seam_displacement_px)
         <<",\"limitPx\":"<<Num(v.limit_px)<<",\"passed\":"<<Bool(v.passed)<<",\"diagnostics\":{\"outerIterations\":"<<OuterIterationsJson(v)
         <<",\"converged\":"<<Bool(v.converged)<<",\"lastChangePx\":"<<Num(v.last_change_px)<<",\"changeHistoryPx\":"<<"["<<ChangeHistoryText(v,",")<<"]"
         <<",\"secondStart\":"<<Esc(cr::SecondStartCode(v.second_start))<<",\"secondStartDifferencePx\":"<<Num(v.second_start_difference_px)
         <<",\"fitWindow\":{\"grown\":"<<Bool(v.fit_window.grown)<<",\"leftPx\":"<<Num(v.fit_window.left_px)<<",\"rightPx\":"<<Num(v.fit_window.right_px)
         <<",\"leftAvailablePx\":"<<Num(v.fit_window.left_available_px)<<",\"rightAvailablePx\":"<<Num(v.fit_window.right_available_px)
         <<",\"leftCut\":"<<Bool(v.fit_window.left_cut)<<",\"rightCut\":"<<Bool(v.fit_window.right_cut)
         <<",\"corePoints\":"<<v.fit_window.core_points<<",\"refusedPoints\":"<<v.fit_window.refused_points<<"}}}";
    }
    const auto& g=o.facts.gate;
    s<<",\"sample\":{\"gateKnown\":"<<Bool(g.known);
    if(g.known)
        s<<",\"gateState\":"<<Esc(m2e::registration_gate::GateStateCode(g.state))<<",\"gateCause\":"<<Esc(m2e::registration_gate::GateCauseCode(g.cause))
         <<",\"gateMaximumPx\":"<<Num(g.maximum_px)<<",\"gateLimitPx\":"<<Num(g.limit_px)<<",\"measuredRowShare\":"<<Num(g.measured_share);
    if(in.level)s<<",\"level\":"<<Esc(*in.level);
    s<<",\"beyondEnvelopeApplied\":"<<(in.sample_beyond_envelope_applied?Bool(*in.sample_beyond_envelope_applied):std::string("null"));
    s<<"}";
    s<<",\"display\":{\"hasMotion\":"<<Bool(d.has_motion)<<",\"motionRightMm\":"<<Num(d.motion.camera_motion_mm.x)<<",\"motionDownMm\":"<<Num(d.motion.camera_motion_mm.y)
     <<",\"rotationClockwiseDeg\":"<<Num(d.motion.rotation_clockwise_deg)<<",\"tiltDeg\":"<<Num(d.pose.tilt_deg)<<",\"heightPercent\":"<<Num(d.pose.height_percent)
     <<",\"hasStartMotion\":"<<Bool(d.has_start_motion)
     <<",\"startMotionRightMm\":"<<(d.has_start_motion?Num(d.start_motion.camera_motion_mm.x):std::string("null"))
     <<",\"startMotionDownMm\":"<<(d.has_start_motion?Num(d.start_motion.camera_motion_mm.y):std::string("null"))
     <<",\"startRotationClockwiseDeg\":"<<(d.has_start_motion?Num(d.start_motion.rotation_clockwise_deg):std::string("null"))
     <<",\"hasOverlap\":"<<Bool(d.has_overlap)<<",\"narrowestOverlapMm\":"<<Num(d.narrowest_overlap_mm)
     <<",\"hasNew\":"<<Bool(d.has_new)<<",\"seamXPreviousMm\":"<<Num(d.seam_x_previous_mm)<<",\"seamXNewMm\":"<<(d.has_new?Num(d.seam_x_new_mm):std::string("null"))
     <<",\"widthPreviousPx\":"<<d.width_previous_px<<",\"widthNewPx\":"<<(d.has_new?std::to_string(d.width_new_px):std::string("null"))
     <<",\"heightPreviousPx\":"<<d.height_previous_px<<",\"heightNewPx\":"<<(d.has_new?std::to_string(d.height_new_px):std::string("null"))
     <<",\"rightPreviousMm\":"<<Num(d.right_previous_mm)<<",\"rightNewMm\":"<<(d.has_new?Num(d.right_new_mm):std::string("null"))
     <<",\"topPreviousMm\":"<<Num(d.top_previous_mm)<<",\"bottomPreviousMm\":"<<Num(d.bottom_previous_mm)
     <<",\"topNewMm\":"<<(d.has_new?Num(d.top_new_mm):std::string("null"))<<",\"bottomNewMm\":"<<(d.has_new?Num(d.bottom_new_mm):std::string("null"))
     <<",\"hasEdgeDistance\":"<<Bool(d.has_edge_distance)<<",\"edgeShare\":{"<<Esc(rc::kReferenceCamera)<<":"<<Num(d.edge_share_a)<<","<<Esc(rc::kTargetCamera)<<":"<<Num(d.edge_share_b)<<"}";
    // The rows that the target camera does not see (band-uncovered), named by the role: it is a value of the target camera only.
    s<<",\"targetCoverShortfall\":";
    if(d.target_cover_shortfall.present){
        const auto& f=d.target_cover_shortfall;
        s<<"{\"pose\":"<<Esc(f.at_estimate?"estimate":"start")<<",\"rowsTop\":"<<f.rows_top<<",\"rowsBottom\":"<<f.rows_bottom
         <<",\"topMm\":"<<Num(f.top_mm)<<",\"bottomMm\":"<<Num(f.bottom_mm)<<"}";
    }else s<<"null";
    s<<"}";
    s<<"}";
    return s.str();
}

// True when `path` is `folder` or lies below it, compared on the real places (".." and links resolved as far as the paths exist) and by
// whole path elements, so a sibling whose name starts the same way does not count.
inline bool IsInsideFolder(const fs::path& path,const fs::path& folder){
    std::error_code ec;
    auto inner=fs::weakly_canonical(path,ec);
    if(ec)inner=fs::absolute(path,ec).lexically_normal();
    auto outer=fs::weakly_canonical(folder,ec);
    if(ec)outer=fs::absolute(folder,ec).lexically_normal();
    if(inner.empty() || outer.empty())return false;
    const auto relative=inner.lexically_relative(outer);
    return !relative.empty() && *relative.begin()!=fs::path("..");
}

inline bool IsCanonicalOriginal(const fs::path& file,const wchar_t* alias){
    return file.filename()==L"original.jpg" && file.parent_path().filename()==alias;
}
}

[[nodiscard]] inline std::string_view UnverifiedPolicyCode(UnverifiedPolicy policy){
    return policy==UnverifiedPolicy::confirm_with_warning?"confirm-with-warning":"refuse-to-confirm";
}

// ---- The job ----
// Never writes into the settings folder. Programming errors (no hash provider, an empty path, a time that is not a UTC time) throw
// std::invalid_argument; everything else, including every failure inside the computation and the sample (also memory), ends as an
// outcome whose verdict is `unusable` and whose findings say why, and nothing is changed anywhere but the staging folder.
[[nodiscard]] inline RecalibrationJobOutcome RunRecalibrationJob(const RecalibrationJobRequest& request){
    using namespace recalibration_job_detail;
    using Clock=std::chrono::steady_clock;
    RecalibrationJobOutcome out;
    out.pair_id=request.pair_id;out.staging_folder=request.staging_folder;
    if(!request.hash)throw std::invalid_argument("no SHA-256 provider");
    if(request.original_a.empty() || request.original_b.empty() || request.settings_folder.empty() || request.staging_folder.empty())
        throw std::invalid_argument("the originals, the settings folder and the staging folder are required");
    if(!m2e::fixed_composite_detail::IsUtcTimestamp(request.measured_at_utc))throw std::invalid_argument("the time of the shot is not a UTC time");
    const auto began=Clock::now();
    const auto seconds=[&]{return std::chrono::duration<double>(Clock::now()-began).count();};
    const auto stage=[&](RecalibrationStage s){
        const double at=seconds();
        if(request.on_stage)request.on_stage(s,at);
        out.log_lines.push_back("recalibration_stage name="+std::string(RecalibrationStageCode(s))+" at_s="+Num(at));
    };
    const auto find=[&](FindingClass klass,const char* code,std::string reason={}){out.findings.push_back({klass,code,std::move(reason)});};
    // A stop asked for is honoured at every step that follows a long one: whatever was found so far is dropped and the outcome says "stopped".
    const auto stop_asked=[&]{
        if(!request.stop.stop_requested())return false;
        out.findings.clear();out.candidate.reset();out.facts=RecalibrationResultFacts{};out.thumbnail=render::BgrImage{};out.seam_crops.reset();
        out.wide.reset();out.fine_fit_raster.reset();
        find(FindingClass::no_candidate,"stopped");
        return true;
    };

    bool staging_created=false;
    ReportInput report;
    report.outcome=&out;report.policy=std::string(UnverifiedPolicyCode(request.unverified_policy));
    report.estimate_state=std::string(cr::RegistrationStateCode(cr::RegistrationState::undetermined));
    rc::Verification verification;
    std::array<std::string,2> shas{request.sha_a,request.sha_b};

    const auto finish=[&]()->RecalibrationJobOutcome{
        out.verdict=DecideVerdict(out.findings,request.unverified_policy,out.candidate.has_value());
        out.ready=out.verdict!=RecalibrationVerdict::unusable;
        out.seconds=seconds();
        out.screen_lines=RecalibrationScreenLines(out.verdict,out.findings,out.facts);
        out.log_lines.push_back("recalibration_verdict verdict="+std::string(RecalibrationVerdictCode(out.verdict))+" unverified="+(out.unverified()?"1":"0")+
            " policy="+report.policy+" candidate="+(out.candidate?"1":"0")+" total_s="+Num(out.seconds)+" camera_operations=0 settings_written=0");
        // The raster that the candidate has, when it is not the one in force: width, height and the top and bottom edges, before and after.
        if(const auto& d=out.facts.display;d.has_new && (d.width_new_px!=d.width_previous_px || d.height_new_px!=d.height_previous_px ||
            d.top_new_mm!=d.top_previous_mm || d.bottom_new_mm!=d.bottom_previous_mm))
            out.log_lines.push_back("recalibration_raster width_px="+std::to_string(d.width_previous_px)+"->"+std::to_string(d.width_new_px)+
                " height_px="+std::to_string(d.height_previous_px)+"->"+std::to_string(d.height_new_px)+
                " top_mm="+Num(d.top_previous_mm)+"->"+Num(d.top_new_mm)+" bottom_mm="+Num(d.bottom_previous_mm)+"->"+Num(d.bottom_new_mm));
        if(verification.performed)
            out.log_lines.push_back("recalibration_verification state="+std::string(cr::RegistrationStateCode(verification.state))+" passed="+(verification.passed?"1":"0")+
                " seam_displacement_px="+Num(verification.seam_displacement_px)+" limit_px="+Num(verification.limit_px)+
                " outer_iterations="+(OuterIterationsCarried(verification)?std::to_string(verification.evidence.outer_iterations):std::string("-"))+
                " converged="+(verification.converged?"1":"0")+" last_change_px="+Num(verification.last_change_px)+" change_history_px="+ChangeHistoryText(verification,";")+
                " second_start="+std::string(cr::SecondStartCode(verification.second_start))+" second_start_difference_px="+Num(verification.second_start_difference_px)+
                " fit_window_grown="+(verification.fit_window.grown?"1":"0"));
        for(const auto& f:out.findings)
            out.log_lines.push_back("recalibration_finding class="+std::string(FindingClassCode(f.klass))+" code="+f.code+(f.reason.empty()?std::string{}:" reason="+f.reason));
        if(staging_created){
            report.original_a_sha=shas[0];report.original_b_sha=shas[1];
            report.verification=verification.performed?&verification:nullptr;
            try{WriteNew(request.staging_folder/"recalibration-report.json",ReportJson(report)+"\n");}
            catch(const std::exception&){out.log_lines.push_back("recalibration_report_written=0");}
        }
        return std::move(out);
    };

    if(!request.live_view_stopped){find(FindingClass::no_candidate,"live-not-stopped");return finish();}
    // The staging folder is never inside the settings folder: a record folder made among the revisions would make the reader refuse the
    // whole row of revisions. Nothing is made.
    if(IsInsideFolder(request.staging_folder,request.settings_folder)){find(FindingClass::no_candidate,"staging-in-settings");return finish();}
    {
        std::error_code ec;
        // A path that is not there is reported by some libraries as "not found" with an error code set: the type decides, not the code.
        const auto status=fs::symlink_status(request.staging_folder,ec);
        if(status.type()!=fs::file_type::not_found){find(FindingClass::no_candidate,"staging-exists");return finish();}
    }
    try{
        // 1. The condition in force: read only. A v1 or v2 file, a damaged chain or no revision is "no condition (v3)". A hand adjustment
        // beyond the envelope (pose_outside_envelope) is not: the chain, the profile and the single source were all checked before the range
        // was applied, so the revision in force is read again without the range and the calibration may start from it (a recalibrated
        // revision has base pose 0, which ends that state).
        stage(RecalibrationStage::read_condition);
        const auto selection=m2e::SelectFixedComposite(request.settings_folder/m2e::kFixedCompositeConditionName,request.hash);
        const auto* active=selection.loaded?std::get_if<m2e::LoadedFixedCompositeConditionV3>(&*selection.loaded):nullptr;
        std::optional<m2e::LoadedFixedCompositeConditionV3> beyond_range;
        if(!selection.loaded && selection.reason==m2e::FixedCompositeSkip::pose_outside_envelope){
            try{beyond_range=m2e::LoadActiveFixedCompositeRevisionV3(request.settings_folder/std::string(m2e::kFixedCompositeRevisionFolderName),request.hash);}
            catch(const std::exception&){out.log_lines.push_back("recalibration_condition_unreadable_beyond_range=1");}
            if(beyond_range){active=&*beyond_range;out.log_lines.push_back("recalibration_condition_outside_envelope=1");}
        }
        if(!active){
            find(FindingClass::no_candidate,"no-v3-condition");
            out.log_lines.push_back("recalibration_condition_unusable reason="+(selection.loaded?std::string("not-v3"):std::string(m2e::FixedCompositeSkipCode(selection.reason))));
            return finish();
        }
        const auto& limits=active->condition.registration.limits;
        out.parent_sha256=active->file_sha256;
        const auto profile_file=request.settings_folder/active->condition.source.profile_file;
        const auto profile_bytes=ReadPlainFile(profile_file,m2e::kMaximumFixedCompositeProfileBytes);
        if(!profile_bytes || Sha(request.hash,*profile_bytes)!=active->condition.source.profile_file_sha256){find(FindingClass::no_candidate,"profile-unreadable");return finish();}

        // The staging folder is made now: from here on there is a record, whatever happens.
        // A folder that appeared since the check above is not ours: it is refused and not written into. Any other failure ends as job-failed.
        std::error_code made_ec;
        const bool made=fs::create_directory(request.staging_folder,made_ec);
        if(made_ec)throw fs::filesystem_error("the staging folder could not be made",request.staging_folder,made_ec);
        if(!made){find(FindingClass::no_candidate,"staging-exists");return finish();}
        staging_created=true;

        // 2. The two originals: read, hashed, decoded in the stored pixel order (the registered composite does not rotate them).
        stage(RecalibrationStage::decode);
        const std::array<fs::path,2> originals{request.original_a,request.original_b};
        std::array<render::BgrImage,2> images;
        for(std::size_t i=0;i<2;++i){
            auto bytes=ReadPlainFile(originals[i],kMaximumOriginalBytes);
            if(!bytes){find(FindingClass::no_candidate,"decode-failed");return finish();}
            const auto actual=Sha(request.hash,*bytes);
            if(shas[i].empty())shas[i]=actual;
            else if(shas[i]!=actual){find(FindingClass::no_candidate,"hash-mismatch");return finish();}
            images[i]=DecodeJpegBytes(*bytes);
            if(images[i].bgr.empty()){find(FindingClass::no_candidate,"decode-failed");return finish();}
        }
        if(images[0].width!=images[1].width || images[0].height!=images[1].height ||
            images[0].width!=a0::m2::kD810SensorWidthPixels || images[0].height!=a0::m2::kD810SensorHeightPixels){find(FindingClass::no_candidate,"size-mismatch");return finish();}
        if(out.pair_id.empty())out.pair_id="recal-"+shas[0].substr(0,12)+"-"+shas[1].substr(0,12);
        if(request.stop.stop_requested()){find(FindingClass::no_candidate,"stopped");return finish();}

        // 3. The computation. The images are let go as soon as it returns (the sample decodes them again).
        const auto stop=request.stop;
        rc::Inputs in;
        in.camera_a=&images[0];in.camera_b=&images[1];
        in.active=active->condition;in.active_sha256=active->file_sha256;in.profile_bytes=std::string(profile_bytes->begin(),profile_bytes->end());
        in.seam=request.seam;in.raster=request.raster;
        in.layer_two=m2e::LayerTwoStrainDesignV3{m2e::parallax_correction::kParallaxDesignDraft.weight_falloff_px,m2e::parallax_correction::kParallaxDesignDraft.maximum_strain};
        in.pair_id=out.pair_id;in.original_a_sha256=shas[0];in.original_b_sha256=shas[1];in.measured_at_utc=request.measured_at_utc;
        in.hash=request.hash;in.design=cr::kRegistrationDesignDraft;
        in.thread_count=request.thread_count?std::clamp(request.thread_count,1u,256u):std::clamp(std::thread::hardware_concurrency(),1u,256u);
        in.control.stop=[stop]{return stop.stop_requested();};
        in.now=request.now;
        in.progress=[&](rc::Stage s){stage(RecalibrationStageOf(s));};
        auto proposal=request.computation.propose(in);
        images[0]=render::BgrImage{};images[1]=render::BgrImage{};in.camera_a=nullptr;in.camera_b=nullptr;
        if(stop_asked())return finish();   // the computation is done; the rebuild, the sample and the crops are not started

        out.facts.display=proposal.display;
        if(proposal.wide_performed)out.wide=proposal.wide;
        out.fine_fit_raster=proposal.fine_raster;
        report.estimate_state=std::string(cr::RegistrationStateCode(proposal.estimate.state));report.estimate_reasons=proposal.estimate.reasons;
        verification=proposal.verification;
        out.findings=ClassifyProposal(proposal,limits);
        std::optional<rc::Candidate> candidate=proposal.candidate;
        const bool only_unverified=OnlyUnverifiedCheck(proposal,limits);
        if(proposal.ready && !candidate){find(FindingClass::rejected,"profile-invalid");}
        else if(only_unverified){
            // The check on the new profile could not be made; nothing found wrong. The computation let the candidate go, so it is made
            // again from the pose it found (the same pure steps as before, no images).
            const auto rebuilt=request.computation.from_pose(in,proposal.pose,proposal.fit,proposal.estimate_record_text);
            if(rebuilt.candidate && rebuilt.refusals.empty()){candidate=rebuilt.candidate;out.facts.display=rebuilt.display;}
            else for(auto& f:ClassifyProposal(rebuilt,limits))out.findings.push_back(std::move(f));
        }
        if(!proposal.ready && !only_unverified)candidate.reset();
        if(stop_asked())return finish();   // before the record and the sample
        if(!candidate){
            if(out.findings.empty())find(FindingClass::rejected,"profile-invalid");   // a proposal that is not ready and says nothing is not offered
            return finish();
        }

        // 4. What a confirmation would write is kept in memory; the staging folder gets a copy for the record.
        out.candidate=std::move(candidate);
        out.sequence=out.candidate->condition.revision.sequence;
        out.facts.creation_checked=true;
        WriteNew(request.staging_folder/"estimate-record.json",proposal.estimate_record_text);
        WriteNew(request.staging_folder/"candidate-profile.json",out.candidate->profile_bytes);
        WriteNew(request.staging_folder/"candidate-revision.json",out.candidate->text);

        // 5. The sample: the composite that a capture makes, with the candidate as the selection (nothing is written to the settings).
        stage(RecalibrationStage::sample);
        RecalibrationSampleFacts sample;
        PairCompositeOutcome composed;
        try{
            fs::path a=fs::absolute(request.original_a),b=fs::absolute(request.original_b);
            if(!IsCanonicalOriginal(a,L"CAM-A") || !IsCanonicalOriginal(b,L"CAM-B")){
                // The review writer takes originals named original.jpg in folders CAM-A and CAM-B; a copy goes beside the record.
                const auto copies=request.staging_folder/"sample-originals";
                fs::create_directories(copies/"CAM-A");fs::create_directories(copies/"CAM-B");
                fs::copy_file(a,copies/"CAM-A"/"original.jpg");fs::copy_file(b,copies/"CAM-B"/"original.jpg");
                a=copies/"CAM-A"/"original.jpg";b=copies/"CAM-B"/"original.jpg";
            }
            const auto selected=rc::SelectionOfCandidate(*out.candidate);
            // The registered composite does not use the live view's grid; the values are the ones a pair capture records.
            m2e::PreviewReviewGeometry geometry{460,307,229,3,11,out.candidate->condition.orientation};
            m2e::StillReviewRequest review{a,b,request.staging_folder/"review",shas[0],shas[1],out.pair_id,geometry,Clock::now()+request.sample_window};
            composed=ComposePairWithSelection(selected,std::move(review),std::nullopt);
        }catch(const std::bad_alloc&){sample.out_of_memory=true;}
        catch(const std::exception& error){sample.threw=true;out.log_lines.push_back("recalibration_sample_failure: "+ErrorSummary(error));}
        sample.skip=composed.skip;
        if(composed.review && composed.review->fixed_composite_registration){
            const auto& record=*composed.review->fixed_composite_registration;
            const auto& capture=record.registration;
            sample.composed=true;sample.estimate=capture.registration.state;sample.gate=capture.gate.state;sample.gate_cause=capture.gate.cause;
            sample.estimate_reasons=capture.registration.reasons;
            sample.estimate_evidence=cr::EvidenceCountsOf(capture.registration.diagnostics);
            for(const auto& gate_finding:capture.gate.gate_findings)sample.gate_findings.push_back(gate_finding.reason);
            for(const auto& skipped:capture.time_budget.skipped)
                if(skipped.stage==m2e::ReviewStage::gate)sample.gate_skipped_reason=skipped.reason;
            auto& gate_facts=out.facts.gate;
            gate_facts.known=true;gate_facts.state=capture.gate.state;gate_facts.cause=capture.gate.cause;
            gate_facts.maximum_px=registration_text_detail::GateMaximum(capture.gate);
            gate_facts.limit_px=out.candidate->condition.seam_gate.limit_px;
            gate_facts.measured_share=capture.gate.rows.rows_with_content?
                static_cast<double>(capture.gate.rows.rows_measured)/static_cast<double>(capture.gate.rows.rows_with_content):std::numeric_limits<double>::quiet_NaN();
            report.level=std::string(m2e::GeometryLevelCode(capture.level.level));
            report.sample_beyond_envelope_applied=capture.level.beyond_envelope_applied;
            out.thumbnail=composed.review->thumbnail;
            for(auto& line:DescribeRegistrationReview(*composed.review).log)out.log_lines.push_back(std::move(line));
        }
        for(auto& f:ClassifySample(sample,out.candidate->condition.registration.limits,request.suspect_content_policy))out.findings.push_back(std::move(f));

        // 6. The seam at native size, for the screen (a failure here loses the crops, not the result).
        if(stop_asked())return finish();   // before the crops (the sample is in the staging folder, nothing of it is offered)
        if(composed.review){
            stage(RecalibrationStage::crops);
            try{out.seam_crops=ExportSeamCrops(request.staging_folder/"review",request.staging_folder/"seam-crops.png");}
            catch(const std::exception& error){out.log_lines.push_back("recalibration_crops_failure: "+ErrorSummary(error));}
        }
        return finish();
    }catch(const std::bad_alloc&){
        find(FindingClass::rejected,"out-of-memory");
    }catch(const std::exception& error){
        find(FindingClass::rejected,"job-failed");
        out.log_lines.push_back("recalibration_job_failure: "+ErrorSummary(error));
    }
    return finish();
}

// ---- The confirmation ----
struct RecalibrationConfirmOptions {
    // The person saw the warning of an unchecked calibration and decided to use it. Required for `usable_unverified`.
    bool acknowledge_unverified{false};
};
struct RecalibrationConfirmResult {
    bool written{};
    bool unverified{};                 // what was written (or would have been) is an unchecked calibration
    std::string reason;                // ASCII code of what stopped it; empty when written
    std::string message;               // UTF-8 Japanese, for the screen
    std::string revision_sha256;
    std::uint32_t sequence{};
    std::vector<std::string> log_lines;
};

// The only writer of the settings folder, in the order of the design (2.8): (1) the revision in force must be the candidate's parent,
// (2) the profile copy, (3) the revision, (4) read it back. What is written is the candidate in memory; the staging folder is not read.
// Nothing is removed on a failure: a profile copy that stays is named by its content and does no harm, a revision that was not made
// leaves the earlier one in force. A second call with the same outcome stops at (1): the revision in force is no longer the parent.
[[nodiscard]] inline RecalibrationConfirmResult ConfirmRecalibration(const RecalibrationJobOutcome& outcome,const std::filesystem::path& settings_folder,
    const a0::m2::experimental::FixedCompositeSha256& hash,const RecalibrationConfirmOptions& options={}){
    using namespace recalibration_job_detail;
    if(!hash)throw std::invalid_argument("no SHA-256 provider");
    RecalibrationConfirmResult result;
    result.unverified=outcome.unverified();
    const auto stopped=[&](const char* reason){
        result.written=false;result.reason=reason;result.message=RecalibrationNotConfirmedText(reason);
        result.log_lines.push_back("recalibration_confirmed=0 reason="+std::string(reason));
        return result;
    };
    if(!outcome.ready || !outcome.candidate || outcome.verdict==RecalibrationVerdict::unusable)return stopped("not-usable");
    if(outcome.unverified() && !options.acknowledge_unverified)return stopped("not-acknowledged");
    const auto& candidate=*outcome.candidate;
    const auto& parent=candidate.condition.revision.parent_sha256;
    if(!parent || *parent!=outcome.parent_sha256)return stopped("not-usable");
    const auto revisions=settings_folder/std::string(m2e::kFixedCompositeRevisionFolderName);

    // (1) The revision in force is the one the candidate was made from.
    try{
        const auto active=m2e::LoadActiveFixedCompositeRevisionV3(revisions,hash);
        if(!active || active->file_sha256!=*parent || active->condition.revision.sequence+1!=candidate.condition.revision.sequence)return stopped("condition-changed");
    }catch(const std::exception&){return stopped("condition-changed");}

    // What is written is what was checked: the text and the profile are the ones the candidate's hashes name.
    try{
        if(m2e::FormatFixedCompositeConditionV3(candidate.condition)+"\n"!=candidate.text || Sha(hash,candidate.text)!=candidate.sha256 ||
            Sha(hash,candidate.profile_bytes)!=candidate.condition.source.profile_file_sha256)return stopped("write-failed");
    }catch(const std::exception&){return stopped("write-failed");}

    // (2) The profile copy (a new file named by its content; the same bytes already there count as done; other bytes are refused).
    try{
        const auto name=m2e::CopyFixedCompositeProfile(settings_folder,candidate.profile_bytes,hash);
        if(name!=candidate.profile_name || name!=candidate.condition.source.profile_file)return stopped("write-failed");
    }catch(const std::exception&){return stopped("write-failed");}

    // (3) The revision (an exclusive partial, read back, a rename that does not replace).
    try{m2e::WriteFixedCompositeConditionV3(revisions,candidate.condition);}
    catch(const std::exception&){return stopped("write-failed");}

    // (4) Read back: the revision in force is the candidate.
    try{
        const auto after=m2e::LoadActiveFixedCompositeRevisionV3(revisions,hash);
        if(!after || after->file_sha256!=candidate.sha256 || after->condition.revision.sequence!=candidate.condition.revision.sequence)return stopped("readback-failed");
    }catch(const std::exception&){return stopped("readback-failed");}

    result.written=true;result.sequence=candidate.condition.revision.sequence;result.revision_sha256=candidate.sha256;
    result.message=RecalibrationConfirmedText(result.sequence,result.unverified);
    result.log_lines.push_back("recalibration_confirmed=1 sequence="+std::to_string(result.sequence)+" unverified="+(result.unverified?"1":"0")+
        " acknowledged="+(options.acknowledge_unverified?"1":"0")+" revision_sha256="+result.revision_sha256);
    // The mark of an unchecked calibration is kept beside the report, in the staging folder (the condition's format has no place for it).
    if(!outcome.staging_folder.empty()){
        try{
            std::ostringstream s;
            s<<"{\"schema\":\"a0.pair-recalibration-confirmation.v1\",\"pairId\":"<<Esc(outcome.pair_id)<<",\"referenceCamera\":"<<Esc(rc::kReferenceCamera)<<",\"targetCamera\":"<<Esc(rc::kTargetCamera)
             <<",\"sequence\":"<<result.sequence
             <<",\"revisionSha256\":"<<Esc(result.revision_sha256)<<",\"parentSha256\":"<<Esc(*parent)<<",\"verdict\":"<<Esc(RecalibrationVerdictCode(outcome.verdict))
             <<",\"unverified\":"<<Bool(result.unverified)<<",\"acknowledged\":"<<Bool(options.acknowledge_unverified)<<"}\n";
            WriteNew(outcome.staging_folder/"confirmation.json",s.str());
        }catch(const std::exception&){result.log_lines.push_back("recalibration_confirmation_record_written=0");}
    }
    return result;
}
}
