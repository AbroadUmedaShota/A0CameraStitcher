#pragma once
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/experimental/fixed_composite_creation_v3.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/phase0/experimental/fixed_composite_tool_v2.hpp"
#include "a0/phase0/experimental/preview_mount_settings.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0::experimental {
// Development entry for making and revising the fixed composite condition v3 (#417, docs/design/fixed-composite-registration.md
// 11.19, 11.20; the field names are in docs/design/fixed-composite-registration-formats.md). A person (or, for a manual
// adjustment and a rollback, the operator through the review window) runs it; it never runs on its own and never overwrites,
// removes or renames a file: a new revision is a new file r<6 digits>.json in the revisions folder, and going back is writing a
// new revision (11.19.4).
//
// - The ranges, lines and limits are copied from wpd-registration-policy.json of the settings folder (11.20.2) when a revision
//   of kind created, migrated-from-v2 or values is made, and the revision carries the file's SHA-256. The other kinds carry the
//   values of the revision in force. Without the file, or with a member unset, nothing is made. No value has a default here.
// - created and migrated-from-v2 make r000001. A condition file of either older version in the settings folder stops them (a
//   person moves it away first; v2 and the revisions folder are never both in force), and so does an existing revision.
//   migrated-from-v2 reads the moved v2 file (request.v2_condition) and carries its identification of the rig (profile, raster,
//   seam, orientation) and nothing of its scale limit.
// - Every later kind reads the revision in force (the chain is verified like the capture path verifies it) and writes the next
//   number with that file's SHA-256 as parent.
// - manual-adjustment is never refused by its size (the owner's decision 9). It is refused when the base pose that follows is
//   outside the pose range of the record (design 11.19.2 "start again from the calibration"); that is the only bound.
// - rollback writes a new revision that carries the content of an earlier one.
// - saved-estimate needs the report v8 reader of the capture lane, which does not exist yet: it is refused.
// - dry_run runs every step up to and including the creation checks and writes nothing.
// - Every check of fixed_composite_creation_v3.hpp runs before a record is written.

// The registration policy file (11.20.2): a new file, not wpd-merge-policy.json (that file has fixed members and a 4096-byte ceiling).
// The reader is strict: duplicate and unknown members are refused.
inline constexpr std::string_view kRegistrationPolicySchemaV1="a0.wpd-registration-policy.v1";
inline constexpr std::string_view kRegistrationPolicyFileName="wpd-registration-policy.json";
inline constexpr std::size_t kMaximumRegistrationPolicyBytes=a0::m2::experimental::kMaximumFixedCompositeRegistrationPolicyV3Bytes;
struct RegistrationPolicyV1 {
    a0::m2::experimental::FixedCompositeConditionV3::Registration registration;
    a0::m2::experimental::FixedCompositeConditionV3::SeamGateLine seam_gate;
    a0::m2::experimental::FixedCompositeConditionV3::RevisionRules revision_rules;
};
// Throws std::invalid_argument for anything that is not exact (including an unset range, line or limit).
[[nodiscard]] inline RegistrationPolicyV1 ParseRegistrationPolicyV1(std::string_view text){
    auto values=a0::m2::experimental::ParseFixedCompositeRegistrationPolicyV3(text,kRegistrationPolicySchemaV1);
    return {std::move(values.registration),values.seam_gate,values.revision_rules};
}

struct FixedConditionV3Request {
    std::filesystem::path settings_folder;   // holds the profile copy, the policy file and the revisions folder
    a0::m2::experimental::FixedCompositeRevisionKind kind{a0::m2::experimental::FixedCompositeRevisionKind::created};
    // created: the draft rig profile and the output raster (region in micrometres of the document plane, left/top/right/bottom, and an integer DPI).
    std::filesystem::path profile;
    std::array<long long,4> region_um{};
    std::uint32_t dpi{};
    // migrated-from-v2: the v2 condition file, after a person moved it out of the settings folder's way.
    std::filesystem::path v2_condition;
    // saved_estimate: the report whose pose becomes p_b (its estimate must be verified inside the range, level G1 or G2, within criteria).
    std::filesystem::path report;
    // manual_adjustment: the operator's entry. The displacement of camera B (output pixels, x to the right and y down) that the new base pose
    // causes relative to the revision in force, measured at the seam column's middle row, and a rotation about the optical axis in degrees.
    // Never refused for its size. ※assumption: the sign is "where B goes", not "where B is seen to be wrong".
    std::optional<a0::m2::experimental::FixedCompositeRevisionChangeV3::Entered> entered;
    // rollback: the revision number whose content is written again.
    std::uint32_t restore_sequence{};
    // The design constants of layer 2's strain bound (L and s_max). Not given, a condition that enables layer 2 cannot be checked and is not made.
    std::optional<a0::m2::experimental::LayerTwoStrainDesignV3> layer_two;
    bool dry_run{};                          // every step and every check, nothing written
    // Called with "check" just before the creation checks start, which are the slow part (minutes at a real raster). Empty: not called.
    std::function<void(std::string_view)> on_stage;
};
struct FixedConditionV3MakeResult {
    bool made{};
    bool checked{};
    std::filesystem::path file;              // the revision file when made
    std::filesystem::path profile_file;      // the profile copy when made
    std::string message;                     // UTF-8, Japanese: what was done or why not
    std::vector<std::string> details;        // "key=value" facts of what was checked (no measured values of a rig)
    // What the record is (set once the checks passed, also for a dry run).
    std::uint32_t revision{};
    std::string kind;                        // revision.change.kind as written in the record
    std::string parent_sha256;               // empty for revision 1
    std::string policy_sha256;               // of the policy file the values come from (the restored one for a rollback)
    std::string condition_sha256;            // of the revision file as written; empty unless made
    // True when the base pose of the record lies beyond registration.envelope (only a manual adjustment can). The record is valid and
    // is written, but a capture and the live view do not draw with it; the message says so (set with the other facts, also for a dry run).
    bool outside_envelope{};
};

namespace fixed_tool_v3_detail {
namespace m2=a0::m2::experimental;
namespace render=a0::m2::render;
namespace cr=a0::m2::experimental::content_registration;

// Document-plane homography applied to an output sample: where the sample goes, in output pixels, relative to where it was.
inline render::Point Moved(const render::Matrix3& g,const cr::PlaneFrame& frame,double x,double y){
    const double px=frame.left_mm+(x+0.5)*frame.pitch_mm,py=frame.top_mm+(y+0.5)*frame.pitch_mm;
    const double w=g[6]*px+g[7]*py+g[8];
    return {((g[0]*px+g[1]*py+g[2])/w-px)/frame.pitch_mm,((g[3]*px+g[4]*py+g[5])/w-py)/frame.pitch_mm};
}
inline render::Matrix3 Multiply(const render::Matrix3& a,const render::Matrix3& b){
    render::Matrix3 r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double s=0;for(int k=0;k<3;++k)s+=a[3*i+k]*b[3*k+j];r[3*i+j]=s;}
    return r;
}
// The displacement at an output sample that going from pose `from` to pose `to` causes.
inline render::Point RelativeDisplacement(const cr::PoseModel& model,const cr::PlaneFrame& frame,const cr::PoseParameters& from,const cr::PoseParameters& to,double x,double y){
    return Moved(Multiply(render::Invert(model.Homography(from)),model.Homography(to)),frame,x,y);
}
inline cr::PoseParameters Added(const cr::PoseParameters& a,const cr::PoseParameters& b){
    return {a.rx_deg+b.rx_deg,a.ry_deg+b.ry_deg,a.rz_deg+b.rz_deg,a.tx_mm+b.tx_mm,a.ty_mm+b.ty_mm,a.tz_percent+b.tz_percent};
}
// The pose change (rz as entered, tx and ty found) whose displacement at the seam column's middle row is (dx, dy). Newton on two unknowns
// with a finite-difference Jacobian; the displacement is almost linear in them, so a few steps are enough. Throws when it does not converge.
inline cr::PoseParameters SolveManualChange(const cr::PoseModel& model,const cr::PlaneFrame& frame,const cr::PoseParameters& base,
    const m2::FixedCompositeRevisionChangeV3::Entered& entered,double x,double y){
    cr::PoseParameters delta{};delta.rz_deg=entered.rotation_deg;
    const auto at=[&](const cr::PoseParameters& d){return RelativeDisplacement(model,frame,base,Added(base,d),x,y);};
    constexpr double kStepMm=1e-3,kToleranceMm=1e-12;
    for(int i=0;i<12;++i){
        const auto d=at(delta);
        const double ex=entered.seam_dx_px-d.x,ey=entered.seam_dy_px-d.y;
        if(std::hypot(ex,ey)<1e-10)return delta;
        auto px=delta;px.tx_mm+=kStepMm;auto py=delta;py.ty_mm+=kStepMm;
        const auto dx=at(px),dy=at(py);
        const double a=(dx.x-d.x)/kStepMm,c=(dx.y-d.y)/kStepMm,b=(dy.x-d.x)/kStepMm,e=(dy.y-d.y)/kStepMm;   // [a b; c e] (tx, ty) = (ex, ey)
        const double det=a*e-b*c;
        if(!(std::abs(det)>kToleranceMm))throw std::invalid_argument("the seam displacement does not determine a translation");
        delta.tx_mm+=(ex*e-b*ey)/det;delta.ty_mm+=(a*ey-c*ex)/det;
        if(!std::isfinite(delta.tx_mm) || !std::isfinite(delta.ty_mm))throw std::invalid_argument("the seam displacement gives no pose");
    }
    const auto d=at(delta);
    if(std::hypot(entered.seam_dx_px-d.x,entered.seam_dy_px-d.y)>1e-6)throw std::invalid_argument("the seam displacement could not be turned into a pose");
    return delta;
}
// The largest displacement along the seam column between two poses (rows every 32, the estimator's measure).
inline double MaximumSeamDisplacement(const cr::PoseModel& model,const cr::PlaneFrame& frame,const cr::PoseParameters& from,const cr::PoseParameters& to,
    std::uint32_t seam_column,std::uint32_t height){
    double worst=0;
    for(std::uint32_t y=0;y<height;y+=32){
        const auto d=RelativeDisplacement(model,frame,from,to,static_cast<double>(seam_column),static_cast<double>(y));
        worst=std::max(worst,std::hypot(d.x,d.y));
    }
    return worst;
}
}

[[nodiscard]] inline FixedConditionV3MakeResult MakeFixedCompositeConditionV3File(const FixedConditionV3Request& request,
    const a0::m2::experimental::FixedCompositeSha256& hash,std::chrono::system_clock::time_point now){
    namespace fs=std::filesystem;namespace m2=a0::m2::experimental;namespace render=a0::m2::render;
    using fixed_tool_v2_detail::ReadSmallRegularFile;
    using Kind=m2::FixedCompositeRevisionKind;
    if(!hash)throw std::invalid_argument("a SHA-256 provider is required");
    const auto refuse=[](std::string message){FixedConditionV3MakeResult r;r.message=std::move(message);return r;};
    const auto bytes_of=[](const std::string& text){return std::vector<unsigned char>(text.begin(),text.end());};
    const auto& folder=request.settings_folder;
    std::error_code error;
    if(!fs::is_directory(folder,error))return refuse("設定のフォルダがありません。作りません");
    const auto revisions=folder/std::string(m2::kFixedCompositeRevisionFolderName);
    const auto condition_file=folder/m2::kFixedCompositeConditionName;
    const bool first_kind=request.kind==Kind::created || request.kind==Kind::migrated_from_v2;

    if(request.kind==Kind::saved_estimate)
        return refuse("推定の姿勢を基準にする改訂（saved-estimate）は、報告 v8 の読み手がまだ無いため作れません。作りません");

    // The ranges, lines and limits come from the policy file, never from the code.
    std::optional<RegistrationPolicyV1> policy;
    std::string policy_sha256;
    if(request.kind==Kind::created || request.kind==Kind::migrated_from_v2 || request.kind==Kind::values){
        const auto text=ReadSmallRegularFile(folder/std::string(kRegistrationPolicyFileName),kMaximumRegistrationPolicyBytes);
        if(!text)return refuse("方針ファイル（wpd-registration-policy.json）がないか、読めません（通常のファイルで、16 KiB 以下である必要があります）。範囲と線を写せないため作りません");
        try{policy=ParseRegistrationPolicyV1(*text);}
        catch(const std::exception& e){return refuse(std::string("方針ファイル（wpd-registration-policy.json）が正しくありません。作りません: ")+e.what());}
        policy_sha256=hash(bytes_of(*text));
    }

    // Which revisions exist.
    std::optional<m2::LoadedFixedCompositeConditionV3> active;
    try{active=m2::LoadActiveFixedCompositeRevisionV3(revisions,hash);}
    catch(const std::exception& e){return refuse(std::string("改訂の履歴が壊れているか、読めません。作りません: ")+e.what());}
    const bool condition_present=fs::symlink_status(condition_file,error).type()!=fs::file_type::not_found;
    if(first_kind){
        if(active)return refuse("改訂が既にあります。r000001 は作り直しません（前の改訂へ戻す時は rollback の改訂を書きます）");
        if(condition_present){
            bool v1=false;
            if(const auto text=ReadSmallRegularFile(condition_file,m2::kMaximumFixedCompositeConditionV2Bytes)){
                try{v1=m2::FixedCompositeConditionSchemaOf(*text)==m2::kFixedCompositeConditionSchema;}catch(const std::exception&){}
            }
            return refuse(v1?"v1 の固定の合成条件が既にあります。上書きしません。切り替える時は、人が v1 のファイルを別の名前へ移してから作り直してください"
                :"固定の合成条件が既にあります。上書きしません。v2 から切り替える時は、人が v2 のファイルを別の名前へ移し、migrated-from-v2 で作ります");
        }
    }else{
        if(condition_present)return refuse("旧版の固定の合成条件と改訂のフォルダが両方あります（出どころが 2 つ）。作りません");
        if(!active)return refuse("改訂がまだありません。先に r000001（created か migrated-from-v2）を作ってください");
    }

    m2::FixedCompositeV3Request make;
    m2::FixedCompositeRevisionChangeV3 change;change.kind=request.kind;
    std::string profile_bytes;
    std::optional<m2::MountOrientation> orientation;
    std::optional<std::string> profile_name;
    if(first_kind){
        make.sequence=1;
        if(request.kind==Kind::created){
            try{if(const auto set=LoadMountOrientationSettings(folder/kMountOrientationSettingsName))orientation=*set;else orientation=m2::MountOrientation::normal;}
            catch(const std::exception&){return refuse("取り付けの向きの設定（wpd-mount-orientation.json）を読めません。作りません");}
            const auto bytes=ReadSmallRegularFile(request.profile,m2::kMaximumFixedCompositeProfileBytes);
            if(!bytes)return refuse("rig profile を読めません（通常のファイルで、256 KiB 以下である必要があります）。作りません");
            profile_bytes=*bytes;
            const auto& r=request.region_um;
            constexpr long long kMaximumUm=1'000'000'000;
            for(const auto v:r)if(v<0 || v>kMaximumUm)return refuse("領域（マイクロメートル）が範囲外です。作りません");
            if(!(r[2]>r[0]) || !(r[3]>r[1]))return refuse("領域の右は左より、下は上より大きくしてください。作りません");
            if(request.dpi<1 || request.dpi>65535)return refuse("DPI は 1 から 65535 の整数で指定してください。作りません");
            const auto pixels=[&](long long um){return (um*static_cast<long long>(request.dpi)+25399)/25400;};
            const auto width=pixels(r[2]-r[0]),height=pixels(r[3]-r[1]);
            if(width>render::kMaximumImageDimension || height>render::kMaximumImageDimension)return refuse("出力の画素数が大きすぎます。作りません");
            make.raster=render::OutputRaster{render::DocumentRegionUm{r[0],r[1],r[2],r[3]},request.dpi,static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)};
            try{
                const auto parameters=a0::m2::RigProfileV2::Parse(profile_bytes).ValidateCalibrationForEvaluation(make.raster,render::Resampling::bilinear);
                make.seam_column=m2::FixedCompositeBandCenter(m2::FixedCompositeBandAtUnitScale(parameters));
            }catch(const std::exception& e){
                return refuse(std::string("rig profile またはその領域が使えません（校正済みの draft で、領域が用紙の中にあり、両台が重なる必要があります）。作りません: ")+e.what());
            }
            make.seam_origin=m2::FixedSeamOrigin::band_center_provisional;
            profile_name=m2::FixedCompositeProfileFileName(hash(bytes_of(profile_bytes)));
        }else{
            const auto v2_text=ReadSmallRegularFile(request.v2_condition,m2::kMaximumFixedCompositeConditionV2Bytes);
            if(!v2_text)return refuse("移した v2 の固定の合成条件を読めません。作りません");
            try{
                const auto v2=m2::ParseFixedCompositeConditionV2(*v2_text);
                const auto bytes=ReadSmallRegularFile(folder/v2.source.profile_file,m2::kMaximumFixedCompositeProfileBytes);
                if(!bytes)return refuse("v2 の条件が指す profile が設定のフォルダにありません。作りません");
                (void)m2::BindFixedCompositeProfile(v2,*bytes,hash);
                profile_bytes=*bytes;profile_name=v2.source.profile_file;orientation=v2.orientation;
                make.raster=v2.raster;make.seam_column=v2.seam.column;make.seam_origin=v2.seam.origin;
                change.v2_sha256=hash(bytes_of(*v2_text));
            }catch(const std::exception& e){return refuse(std::string("v2 の固定の合成条件を使えません。作りません: ")+e.what());}
        }
        make.parent_sha256=std::nullopt;
        make.policy_sha256=policy_sha256;
        make.registration=policy->registration;make.seam_gate=policy->seam_gate;make.revision_rules=policy->revision_rules;
        make.base_pose={};
    }else{
        const auto& current=active->condition;
        make.sequence=current.revision.sequence+1;
        make.parent_sha256=active->file_sha256;
        orientation=current.orientation;make.raster=current.raster;make.seam_column=current.seam.column;make.seam_origin=current.seam.origin;
        profile_name=current.source.profile_file;
        const auto bytes=ReadSmallRegularFile(folder/current.source.profile_file,m2::kMaximumFixedCompositeProfileBytes);
        if(!bytes)return refuse("profile を読めません。作りません");
        profile_bytes=*bytes;
        make.policy_sha256=current.source.policy_sha256;
        make.registration=current.registration;make.seam_gate=current.seam_gate;make.revision_rules=current.revision_rules;
        make.base_pose=current.base_pose;
        if(request.kind==Kind::values){
            if(policy_sha256==current.source.policy_sha256)return refuse("方針ファイルは有効な改訂が写したものと同じです。書く改訂がありません");
            make.policy_sha256=policy_sha256;
            make.registration=policy->registration;make.seam_gate=policy->seam_gate;make.revision_rules=policy->revision_rules;
        }else if(request.kind==Kind::manual_adjustment){
            if(!request.entered)return refuse("手直しの入力（継ぎ目での dx・dy と回転）がありません。作りません");
            try{
                const auto binding=m2::BindFixedCompositeProfileV3(current,profile_bytes,hash);
                const fixed_tool_v3_detail::cr::PoseModel model(binding.parameters.camera_b);
                const auto frame=fixed_tool_v3_detail::cr::FrameOfRaster(binding.parameters.raster);
                const double middle=(static_cast<double>(binding.parameters.raster.height_pixels)-1)/2;
                const auto delta=fixed_tool_v3_detail::SolveManualChange(model,frame,current.base_pose,*request.entered,static_cast<double>(current.seam.column),middle);
                make.base_pose=fixed_tool_v3_detail::Added(current.base_pose,delta);
                change.entered=*request.entered;
                change.maximum_seam_displacement_px=fixed_tool_v3_detail::MaximumSeamDisplacement(model,frame,current.base_pose,make.base_pose,current.seam.column,
                    binding.parameters.raster.height_pixels);
            }catch(const std::exception& e){return refuse(std::string("手直しを姿勢に直せません。作りません: ")+e.what());}
        }else if(request.kind==Kind::rollback){
            if(request.restore_sequence<1 || request.restore_sequence>=current.revision.sequence)
                return refuse("戻す先の番号が正しくありません（1 以上で、有効な改訂より前である必要があります）。作りません");
            const auto restored_file=revisions/m2::FixedCompositeRevisionFileName(request.restore_sequence);
            const auto text=ReadSmallRegularFile(restored_file,m2::kMaximumFixedCompositeConditionV3Bytes);
            if(!text)return refuse("戻す先の改訂を読めません。作りません");
            try{
                const auto restored=m2::ParseFixedCompositeConditionV3(*text);
                if(restored.source.profile_file_sha256!=current.source.profile_file_sha256 || restored.source.profile_file!=current.source.profile_file ||
                    restored.orientation!=current.orientation || restored.seam.column!=current.seam.column || restored.seam.origin!=current.seam.origin)
                    return refuse("戻す先の改訂は、有効な改訂と別の rig の識別を持っています。作りません");
                make.policy_sha256=restored.source.policy_sha256;
                make.registration=restored.registration;make.seam_gate=restored.seam_gate;make.revision_rules=restored.revision_rules;make.base_pose=restored.base_pose;
                change.restored_sha256=hash(bytes_of(*text));
            }catch(const std::exception& e){return refuse(std::string("戻す先の改訂を使えません。作りません: ")+e.what());}
        }
    }
    make.change=change;
    make.profile_file=*profile_name;make.profile_bytes=profile_bytes;make.orientation=*orientation;

    m2::CreationCheckDetailsV3 checked;
    m2::FixedCompositeConditionV3 condition;
    if(request.on_stage)request.on_stage("check");
    try{condition=m2::MakeFixedCompositeConditionV3(make,now,hash,m2::MakeFixedCompositeCreationCheckV3(&checked,m2::CreationCheckOptionsV3{request.layer_two}));}
    catch(const std::exception& e){
        return refuse(std::string("固定の合成条件の検査に通りませんでした。作りません（範囲・線・領域・DPI を見直してください）: ")+e.what()+
            (make.registration.parallax && !request.layer_two?"（層 2 の設計定数 L と s_max が渡されていません）":""));
    }
    FixedConditionV3MakeResult result;
    result.checked=true;
    result.revision=condition.revision.sequence;
    result.kind=std::string(m2::FixedCompositeRevisionKindText(condition.revision.change.kind));
    result.parent_sha256=condition.revision.parent_sha256.value_or(std::string());
    result.policy_sha256=condition.source.policy_sha256;
    result.details=checked.details;
    result.details.insert(result.details.begin(),"revision="+std::to_string(condition.revision.sequence)+" kind="+std::string(m2::FixedCompositeRevisionKindText(condition.revision.change.kind)));
    result.details.push_back("raster_dpi="+std::to_string(condition.raster.dpi)+" width_pixels="+std::to_string(condition.raster.width_pixels)+" height_pixels="+std::to_string(condition.raster.height_pixels));
    result.details.push_back("seam_column="+std::to_string(condition.seam.column)+" mount_orientation="+std::string(m2::MountOrientationName(condition.orientation)));
    try{m2::RequireFixedCompositeBasePoseInEnvelopeV3(condition);}
    catch(const m2::FixedCompositePoseOutsideEnvelope&){result.outside_envelope=true;result.details.push_back("base_pose_outside_envelope=1");}
    if(condition.revision.change.maximum_seam_displacement_px)
        result.details.push_back("maximum_seam_displacement_px="+fixed_tool_v2_detail::Number(*condition.revision.change.maximum_seam_displacement_px));
    if(request.dry_run){
        result.message="検査に通りました。--dry-run のため何も書いていません";
        if(result.outside_envelope)result.message+="。この手直しは範囲の外のため、書いても撮影とライブの C には使われません";
        return result;
    }
    const auto profile_target=folder/(*profile_name);
    if(first_kind){
        try{(void)m2::CopyFixedCompositeProfile(folder,profile_bytes,hash);}
        catch(const std::exception&){
            if(request.kind==Kind::created)return refuse("profile を設定のフォルダへ写せませんでした（同じ名前で別の内容のファイルがある、または書けません）。作りません");
        }
    }
    try{
        fs::create_directory(revisions,error);   // one level only; an existing folder is fine
        m2::WriteFixedCompositeConditionV3(revisions,condition);
    }catch(const std::exception&){
        return refuse("改訂を書けませんでした（同じ番号が既に存在する、または書けません）。既存のファイルは変えていません");
    }
    result.made=true;result.file=revisions/m2::FixedCompositeRevisionFileName(condition.revision.sequence);result.profile_file=profile_target;
    if(const auto written=ReadSmallRegularFile(result.file,m2::kMaximumFixedCompositeConditionV3Bytes))result.condition_sha256=hash(bytes_of(*written));
    result.message="固定の合成条件 v3 の改訂（未承認）を書きました。範囲と線は"+std::string(first_kind||request.kind==Kind::values?"方針ファイルの値を写しました":"有効な改訂の値を引き継ぎました")+
        "。既存の改訂は変えていません";
    if(result.outside_envelope)result.message+="。この手直しは範囲の外のため、撮影とライブの C には使われません。rollback で戻せます";
    return result;
}
}
