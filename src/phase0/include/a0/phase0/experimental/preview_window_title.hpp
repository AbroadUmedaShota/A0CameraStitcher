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

[[nodiscard]] inline std::wstring PreviewWindowTitle(const FixedPreviewSource& source,std::wstring_view note={}){
    return L"A0 A/B Live + C（"+PreviewCalibrationState(source,note)+L"） / 撮影・PC保存";
}

// A composite that is made with a calibration is not a "provisional" one.
[[nodiscard]] inline bool PreviewCalibrated(const FixedPreviewSource& source){
    return source.loaded_v3.has_value() || source.loaded_v2.has_value() || source.loaded.has_value();
}

[[nodiscard]] inline const wchar_t* PairButtonLabel(const FixedPreviewSource& source){
    return PreviewCalibrated(source)?L"両台撮影 → 合成を保存":L"両台撮影 → 仮Merge保存";
}

}
