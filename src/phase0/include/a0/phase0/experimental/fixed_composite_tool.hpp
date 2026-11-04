#pragma once
#include "a0/m2/experimental/fixed_composite_condition.hpp"
#include "a0/m2/experimental/fixed_scale_correction.hpp"
#include "a0/phase0/experimental/preview_merge_policy.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace a0::phase0::experimental {
// Development entry for making the fixed composite condition (#400, #399).
// A person runs it once per setup and names the saved review report that holds
// the mapping to fix. It never runs on its own, never overwrites an existing
// condition and never reads a default folder: the settings folder is given.
// The scale limit is copied from wpd-merge-policy.json of that folder; without
// the file or its scaleAutoCorrectionMaxPercent nothing is made. From then on
// the condition's value is the one that decides; changing the limit means
// making the condition again.
struct FixedConditionMakeResult {
    bool made{};
    std::filesystem::path file; // the condition file when made
    std::string message;        // UTF-8, Japanese: what was done or why not
};
// hash: SHA-256 lower hex of the bytes read, as LoadMergePolicySettings takes it.
template<class Hash>
FixedConditionMakeResult MakeFixedCompositeConditionFile(const std::filesystem::path& review_report,const std::filesystem::path& settings_folder,
    Hash hash,std::chrono::system_clock::time_point now){
    namespace fs=std::filesystem;namespace m2=a0::m2::experimental;
    const auto refuse=[](std::string message){return FixedConditionMakeResult{false,{},std::move(message)};};
    std::error_code error;
    if(!fs::is_directory(settings_folder,error))return refuse("設定のフォルダがありません。作りません");
    std::optional<StillMergePolicy> policy;
    try{policy=LoadMergePolicySettings(settings_folder/kMergePolicySettingsName,hash);}
    catch(const std::exception&){return refuse("方針ファイル（wpd-merge-policy.json）を読めません。倍率の上限を写せないため作りません");}
    if(!policy)return refuse("方針ファイル（wpd-merge-policy.json）がありません。倍率の上限を写せないため作りません");
    if(!policy->scale_max_percent)return refuse("方針ファイルに倍率の上限（scaleAutoCorrectionMaxPercent）がありません。作りません");
    const auto file=settings_folder/m2::kFixedCompositeConditionName;
    if(fs::symlink_status(file,error).type()!=fs::file_type::not_found)return refuse("固定の合成条件が既にあります。上書きしません");
    m2::SavedStillReview review;
    try{review=m2::LoadSavedStillReview(review_report);}
    catch(const std::exception&){return refuse("保存済みの報告を読めません。作りません");}
    if(review.fixed_composite)return refuse("固定の合成条件で作った報告からは、固定の合成条件を作れません。設置の時の報告を指定してください");
    try{
        const auto condition=m2::MakeFixedCompositeCondition(review,now,*policy->scale_max_percent);
        m2::WriteFixedCompositeCondition(file,condition);
    }catch(const std::exception&){return refuse("固定の合成条件を書けませんでした（報告の内容が合わない、または既に存在します）");}
    return {true,file,"固定の合成条件（未承認）を作りました。倍率の上限は方針ファイルの値を写しました。以後はこの記録の値で合成します"};
}
}
