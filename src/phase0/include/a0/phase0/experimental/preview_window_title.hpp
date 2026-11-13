#pragma once
// The caption of the development preview's window and the label of the pair-capture button (#432). Both follow the calibration that the
// Live view read when the session started (fixed_composite_preview.hpp: FixedPreviewSource), so a window that shows a calibrated C no
// longer calls itself "uncalibrated". Header only, no window handle: the preview sets the texts, the tests read them.
//
//   a condition v3 (a revision)   校正あり・改訂 N
//   a condition v2 or v1          校正あり・v2 / 校正あり・v1
//   no condition file             校正なし・仮Merge
//   a file that cannot be read    校正を読めません
//
// The marks of the build (a probe build, a build with the settings write) are compile-time constants of this header, so every place that
// makes the caption, the first one and each one that follows the Live start or a condition read again, carries them with the state.
//
// `note` is placed after the state, inside the parentheses. A build that has more to say about the calibration passes it; the default
// build passes nothing.
#include "a0/phase0/experimental/fixed_composite_preview.hpp"
#include <string>
#include <string_view>

namespace a0::phase0::experimental {

[[nodiscard]] inline std::wstring PreviewCalibrationState(const FixedPreviewSource& source,std::wstring_view note={}){
    std::wstring state;
    if(source.loaded_v3)state=L"校正あり・改訂 "+std::to_wstring(source.loaded_v3->condition.revision.sequence);
    else if(source.loaded_v2)state=L"校正あり・v2";
    else if(source.loaded)state=L"校正あり・v1";
    else if(!source.Present())return L"校正なし・仮Merge";
    else return L"校正を読めません";
    state.append(note);
    return state;
}

#if defined(A0_ENABLE_WPD_LIVE_CAPTURE_PROBE)
inline constexpr bool kPreviewProbeBuild=true;
#else
inline constexpr bool kPreviewProbeBuild=false;
#endif
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
inline constexpr bool kPreviewSettingsWriteBuild=true;
#else
inline constexpr bool kPreviewSettingsWriteBuild=false;
#endif
inline constexpr const wchar_t* kPreviewProbeMark=L"[探りbuild: ライブを開いたまま1回撮る] ";
inline constexpr const wchar_t* kPreviewSettingsWriteMark=L" ／ 設定の書き込みあり（開発用・#416 段B）";

// The whole caption for a state text (the part inside the parentheses).
[[nodiscard]] inline std::wstring PreviewTitleFrame(std::wstring_view state){
    std::wstring title=L"A0 ";
    if(kPreviewProbeBuild)title+=kPreviewProbeMark;
    title+=L"A/B Live + C（";title.append(state);title+=L"） / 撮影・PC保存";
    if(kPreviewSettingsWriteBuild)title+=kPreviewSettingsWriteMark;
    return title;
}
[[nodiscard]] inline std::wstring PreviewWindowTitle(const FixedPreviewSource& source,std::wstring_view note={}){
    return PreviewTitleFrame(PreviewCalibrationState(source,note));
}
// True when the caption carries exactly the marks that this build has (and none that it does not have).
[[nodiscard]] inline bool PreviewTitleMarksMatch(std::wstring_view title){
    const auto has=[&](const wchar_t* part){return title.find(part)!=std::wstring_view::npos;};
    return has(kPreviewProbeMark)==kPreviewProbeBuild && has(kPreviewSettingsWriteMark)==kPreviewSettingsWriteBuild;
}

// A composite that is made with a calibration is not a "provisional" one.
[[nodiscard]] inline bool PreviewCalibrated(const FixedPreviewSource& source){
    return source.loaded_v3.has_value() || source.loaded_v2.has_value() || source.loaded.has_value();
}

// The heading over the saved composite that the review button shows: a composite made with a calibration is not called uncalibrated.
[[nodiscard]] inline const wchar_t* PreviewReviewHeading(bool calibrated){
    return calibrated?L"撮影JPEGの合成結果（静止画／校正あり・確認用）":L"撮影JPEGの合成結果（静止画／未校正・確認用）";
}

[[nodiscard]] inline const wchar_t* PairButtonLabel(const FixedPreviewSource& source){
    return PreviewCalibrated(source)?L"両台撮影 → 合成を保存":L"両台撮影 → 仮Merge保存";
}

}
