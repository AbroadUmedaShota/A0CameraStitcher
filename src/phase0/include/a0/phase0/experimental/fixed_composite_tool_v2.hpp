#pragma once
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/phase0/experimental/preview_merge_policy.hpp"
#include "a0/phase0/experimental/preview_mount_settings.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace a0::phase0::experimental {
// Development entry for making the fixed composite condition v2 (#405 W6, design
// docs/design/fixed-composite-lens.md 3.2, 3.3, 3.9 and 8). A person runs it once per
// setup and names the calibrated DRAFT rig profile (the file the planar calibration
// of #374 writes), the settings folder, and the output raster: the region of the
// document plane in micrometres and an integer DPI. It never runs on its own, never
// overwrites a file, and has no default for the region, the DPI or the scale limit.
//
// - The scale limit is copied from wpd-merge-policy.json of the folder
//   (scaleAutoCorrectionMaxPercent). Without the file or the field nothing is made.
// - The region and the DPI are development values chosen by the person; they are not
//   an owner decision (HG-0001) and the record says so (it is unapproved). Choosing
//   the DPI at which one output pixel is about one capture pixel keeps the gate's
//   1 px comparable with v1: the details report that DPI at the middle of the region.
// - The profile is copied into the folder as wpd-fixed-composite-profile-<16 hex>.json
//   (the first 16 digits of its SHA-256) and the condition names it by SHA-256 and
//   fingerprint. A file of that name with other bytes is never replaced.
// - A condition file already in the folder, of either version, stops it. A v1 file is
//   not replaced by v2: a person moves it away first (owner decision, #405).
// - The seam column starts at the middle of the band that both cameras cover on every
//   row at scale 1 (band-center-provisional). The record is made only if the checks of
//   design 3.9 items 3 to 5 pass for the whole range of the scale limit.
// - dry_run runs every step up to and including those checks and writes nothing.
// - The mount orientation is the one in wpd-mount-orientation.json of the folder
//   (an absent file means normal, as for the live preview) and is recorded.
struct FixedConditionV2Request {
    std::filesystem::path profile;          // the draft rig profile; its folder does not matter
    std::filesystem::path settings_folder;  // receives the condition and the profile copy
    std::array<long long,4> region_um{};    // left, top, right, bottom, micrometres of the document plane
    std::uint32_t dpi{};
    bool dry_run{};
};
struct FixedConditionV2MakeResult {
    bool made{};                    // the condition (and the profile copy) were written
    bool checked{};                 // every check passed (also true for a dry run)
    std::filesystem::path file;     // the condition file when made
    std::filesystem::path profile_file; // the profile copy when made
    std::string message;            // UTF-8, Japanese: what was done or why not
    std::vector<std::string> details; // "key=value" facts of what was checked (no measured values of a rig)
};

namespace fixed_tool_v2_detail {
inline std::optional<std::string> ReadSmallRegularFile(const std::filesystem::path& file,std::size_t limit){
    std::error_code error;const auto status=std::filesystem::symlink_status(file,error);
    if(error || status.type()!=std::filesystem::file_type::regular)return {};
    std::ifstream in(file,std::ios::binary);if(!in)return {};
    std::string text;text.resize(limit+1);in.read(text.data(),static_cast<std::streamsize>(text.size()));
    if(in.bad())return {};
    text.resize(static_cast<size_t>(in.gcount()));
    if(text.size()>limit)return {};
    return text;
}
inline std::string Number(double v){
    char text[40];const auto r=std::to_chars(text,text+sizeof text,v);return std::string(text,r.ptr);
}
}

inline FixedConditionV2MakeResult MakeFixedCompositeConditionV2File(const FixedConditionV2Request& request,
    const a0::m2::experimental::FixedCompositeSha256& hash,std::chrono::system_clock::time_point now){
    namespace fs=std::filesystem;namespace m2=a0::m2::experimental;namespace render=a0::m2::render;
    using fixed_tool_v2_detail::ReadSmallRegularFile;
    if(!hash)throw std::invalid_argument("a SHA-256 provider is required");
    const auto refuse=[](std::string message){FixedConditionV2MakeResult r;r.message=std::move(message);return r;};
    const auto& folder=request.settings_folder;
    std::error_code error;
    if(!fs::is_directory(folder,error))return refuse("設定のフォルダがありません。作りません");

    // The scale limit comes from the policy file, never from the code.
    std::optional<StillMergePolicy> policy;
    try{policy=LoadMergePolicySettings(folder/kMergePolicySettingsName,hash);}
    catch(const std::exception&){return refuse("方針ファイル（wpd-merge-policy.json）を読めません。倍率の上限を写せないため作りません");}
    if(!policy)return refuse("方針ファイル（wpd-merge-policy.json）がありません。倍率の上限を写せないため作りません");
    if(!policy->scale_max_percent)return refuse("方針ファイルに倍率の上限（scaleAutoCorrectionMaxPercent）がありません。作りません");

    // Nothing is replaced: an existing condition of either version stops it.
    const auto condition_file=folder/m2::kFixedCompositeConditionName;
    if(fs::symlink_status(condition_file,error).type()!=fs::file_type::not_found){
        bool v1=false;
        if(const auto text=ReadSmallRegularFile(condition_file,m2::kMaximumFixedCompositeConditionV2Bytes)){
            try{v1=m2::FixedCompositeConditionSchemaOf(*text)==m2::kFixedCompositeConditionSchema;}catch(const std::exception&){}
        }
        return refuse(v1?"v1 の固定の合成条件が既にあります。上書きしません。v1 から切り替える時は、人が v1 のファイルを別の名前へ移してから作り直してください"
            :"固定の合成条件が既にあります。上書きしません");
    }

    m2::MountOrientation orientation=m2::MountOrientation::normal;
    try{if(const auto set=LoadMountOrientationSettings(folder/kMountOrientationSettingsName))orientation=*set;}
    catch(const std::exception&){return refuse("取り付けの向きの設定（wpd-mount-orientation.json）を読めません。作りません");}

    const auto profile_bytes=ReadSmallRegularFile(request.profile,m2::kMaximumFixedCompositeProfileBytes);
    if(!profile_bytes)return refuse("rig profile を読めません（通常のファイルで、256 KiB 以下である必要があります）。作りません");

    // The raster: region and DPI as given, pixel counts as the exact ceiling gives them.
    const auto& r=request.region_um;
    constexpr long long kMaximumUm=1'000'000'000;
    for(const auto v:r)if(v<0 || v>kMaximumUm)return refuse("領域（マイクロメートル）が範囲外です。作りません");
    if(!(r[2]>r[0]) || !(r[3]>r[1]))return refuse("領域の右は左より、下は上より大きくしてください。作りません");
    if(request.dpi<1 || request.dpi>65535)return refuse("DPI は 1 から 65535 の整数で指定してください。作りません");
    const auto pixels=[&](long long um){return (um*static_cast<long long>(request.dpi)+25399)/25400;};
    const auto width=pixels(r[2]-r[0]),height=pixels(r[3]-r[1]);
    if(width>render::kMaximumImageDimension || height>render::kMaximumImageDimension)return refuse("出力の画素数が大きすぎます。作りません");
    const render::OutputRaster raster{render::DocumentRegionUm{r[0],r[1],r[2],r[3]},request.dpi,static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)};

    // The profile must be a calibrated draft that is valid over this region; the band at
    // scale 1 gives the provisional seam column.
    std::optional<render::DocumentRenderParameters> parameters;
    m2::FixedCompositeBand band{};
    try{
        parameters.emplace(a0::m2::RigProfileV2::Parse(*profile_bytes).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear));
        band=m2::FixedCompositeBandAtUnitScale(*parameters);
    }catch(const std::exception& e){
        return refuse(std::string("rig profile またはその領域が使えません（校正済みの draft で、領域が用紙の中にあり、両台が重なる必要があります）。作りません: ")+e.what());
    }
    const auto seam_column=m2::FixedCompositeBandCenter(band);

    m2::FixedCompositeV2Request make;
    make.profile_file=m2::FixedCompositeProfileFileName(hash(std::vector<unsigned char>(profile_bytes->begin(),profile_bytes->end())));
    make.profile_bytes=*profile_bytes;make.orientation=orientation;make.raster=raster;
    make.scale_auto_correction_max_percent=*policy->scale_max_percent;
    make.seam_column=seam_column;make.seam_origin=m2::FixedSeamOrigin::band_center_provisional;
    m2::FixedCompositeConditionV2 condition;
    try{condition=m2::MakeFixedCompositeConditionV2(make,now,hash,m2::MakeFixedCompositeCreationCheck());}
    catch(const std::exception& e){
        return refuse(std::string("固定の合成条件の検査に通りませんでした。作りません（領域・DPI・倍率の上限を見直してください）: ")+e.what());
    }

    FixedConditionV2MakeResult result;
    result.checked=true;
    std::string scales;
    for(const double k:m2::FixedCompositeCheckedScales(condition.scale_auto_correction_max_percent))scales+=(scales.empty()?"":",")+fixed_tool_v2_detail::Number(k);
    if(const auto id=a0::m2::RigProfileV2::Parse(*profile_bytes).ProfileId())result.details.push_back("profile_id="+*id);
    result.details.push_back("mount_orientation="+std::string(m2::MountOrientationName(orientation)));
    result.details.push_back("output_dpi="+std::to_string(raster.dpi)+" width_pixels="+std::to_string(raster.width_pixels)+" height_pixels="+std::to_string(raster.height_pixels));
    result.details.push_back("scale_limit_percent="+fixed_tool_v2_detail::Number(condition.scale_auto_correction_max_percent)+" checked_scales="+scales);
    result.details.push_back("band_first="+std::to_string(band.first)+" band_last_exclusive="+std::to_string(band.last_exclusive)+" seam_column="+std::to_string(seam_column)+" seam_origin=band-center-provisional");
    try{
        const render::Point centre{(r[0]+r[2])/2000.0,(r[1]+r[3])/2000.0};
        const auto dpi_of=[&](const render::CameraProjection& camera){return std::round(m2::FixedCompositeCapturePixelsPerMm(camera,centre)*25.4*10)/10;};
        result.details.push_back("capture_equivalent_dpi_a="+fixed_tool_v2_detail::Number(dpi_of(parameters->camera_a))+" capture_equivalent_dpi_b="+fixed_tool_v2_detail::Number(dpi_of(parameters->camera_b))+" at_region_middle");
    }catch(const std::exception&){}

    if(request.dry_run){
        result.message="検査に通りました。--dry-run のため何も書いていません。領域と DPI は開発用の未承認の値です";
        return result;
    }
    const auto profile_target=folder/condition.source.profile_file;
    try{(void)m2::CopyFixedCompositeProfile(folder,*profile_bytes,hash);}
    catch(const std::exception&){
        return refuse("profile を設定のフォルダへ写せませんでした（同じ名前で別の内容のファイルがある、または書けません）。作りません");
    }
    try{m2::WriteFixedCompositeConditionV2(condition_file,condition);}
    catch(const std::exception&){
        return refuse("固定の合成条件を書けませんでした（既に存在する、または書けません）。profile の写しは残っていますが、条件はありません");
    }
    result.made=true;result.file=condition_file;result.profile_file=profile_target;
    result.message="固定の合成条件 v2（未承認）を作りました。倍率の上限は方針ファイルの値を写しました。写像の値は持たず、同じフォルダの profile を SHA-256 と fingerprint で参照します。領域と DPI は開発用の未承認の値です";
    return result;
}
}
