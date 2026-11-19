#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/phase0/experimental/fixed_composite_registration_text.hpp"
#include "a0/phase0/experimental/live_placement.hpp"
#include "a0/phase0/experimental/live_registration_tracker.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace a0::phase0::experimental {
// What the screen before the shot shows about the cameras and about the live C (#417 LC; the owner's request: "how much the
// images of camera A and B are tilted should be visible on the screen before shooting, so the cameras can be adjusted by looking
// at it"; docs/design/fixed-composite-registration.md addendum 11.24). Values and sentences only; no window is touched.
//
// What is shown is the direction of the camera: the turn about its own axis (as seen on the screen, clockwise or not) and the
// tilt of the lens (how far, and towards which side of the drawing). B is the value the live estimate gives now, against A
// (RegistrationResult::orientation.b_from_a, which TXT turns into a sentence); A is the value at calibration, because what
// moved A after the calibration cannot be seen from the picture. The range of the registration that C could still correct is
// not shown.

// The values come from the estimator's own definition of a camera's direction (content_registration::OrientationOf, which also
// fills RegistrationResult::orientation: A at calibration, B as estimated, B against A). Nothing is computed twice here; the two
// functions below only fit those values to TXT's CameraOrientation and to the mounting:
//  - turn: positive when clockwise as seen on the screen (from x towards y), the sign TXT's sentences use;
//  - tilt direction: the way the lens faces over the drawing (a unit vector, x right and y down; 0 for no tilt);
//  - the mounting: the stored frames are never turned, so a rig mounted turned by 180 degrees has the image upside down in its
//    calibrated projection and OrientationOf reports a half turn for it. That half turn is the mounting and not a setting to
//    correct, so it is removed once for A (design 11.24 item 2); B is shown against A, in which it cancels.
// Not computed again for the oriented frame: nothing is flipped twice.
namespace live_orientation_detail {
inline double WrapDegrees(double degrees) {
    degrees = std::fmod(degrees, 360.0);
    if (degrees > 180.0) degrees -= 360.0;
    if (degrees <= -180.0) degrees += 360.0;
    return degrees;
}
inline bool Finite(const a0::m2::render::Point& p) { return std::isfinite(p.x) && std::isfinite(p.y); }
}

// A at calibration, for a rig with the given mounting. nullopt where a value is not finite.
[[nodiscard]] inline std::optional<CameraOrientation> CameraOrientationOfCalibratedA(
    const a0::m2::experimental::content_registration::CameraOrientation& a, a0::m2::experimental::MountOrientation mount) {
    using namespace live_orientation_detail;
    if (!std::isfinite(a.rotation_clockwise_deg) || !std::isfinite(a.tilt_deg) || !Finite(a.lens_direction)) return std::nullopt;
    const double turn = mount == a0::m2::experimental::MountOrientation::rotated_180 ? a.rotation_clockwise_deg - 180.0 : a.rotation_clockwise_deg;
    return CameraOrientation{WrapDegrees(turn), a.lens_direction, a.tilt_deg};
}
// A at calibration straight from the calibrated projection of the profile (before any estimate exists).
[[nodiscard]] inline std::optional<CameraOrientation> CameraOrientationOfCalibratedA(const a0::m2::render::CameraProjection& camera_a,
    a0::m2::experimental::MountOrientation mount) {
    try {
        return CameraOrientationOfCalibratedA(a0::m2::experimental::content_registration::OrientationOf(camera_a), mount);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
// B against A, from the estimator's relative orientation (B's turn less A's, the angle between the optical axes and the side
// that B's lens leans to from A's).
[[nodiscard]] inline std::optional<CameraOrientation> CameraOrientationOfBAgainstA(
    const a0::m2::experimental::content_registration::RelativeOrientation& relative) {
    using namespace live_orientation_detail;
    if (!std::isfinite(relative.rotation_clockwise_deg) || !std::isfinite(relative.tilt_deg) || !Finite(relative.tilt_direction)) return std::nullopt;
    return CameraOrientation{relative.rotation_clockwise_deg, relative.tilt_direction, relative.tilt_deg};
}

// B against A for B moved by `pose` (p-hat, against the calibrated pose of the profile): the same construction as the estimator's
// RegistrationResult::orientation (B's projection with the homography of the pose, OrientationOf, RelativeOrientationOf). nullopt
// when the pose is not finite or the profile's B cannot be decomposed.
[[nodiscard]] inline std::optional<CameraOrientation> CameraOrientationOfBFromPose(const a0::m2::render::DocumentRenderParameters& profile,
    const a0::m2::experimental::content_registration::PoseParameters& pose) {
    namespace cr = a0::m2::experimental::content_registration;
    try {
        const cr::PoseModel model(profile.camera_b);
        auto moved = profile.camera_b;
        moved.document_to_image = model.CameraHomography(pose);
        return CameraOrientationOfBAgainstA(cr::RelativeOrientationOf(cr::OrientationOf(profile.camera_a), cr::OrientationOf(moved)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// B's direction against the accepted setup (p-hat less p_b, the values of OutputFrameDisplay), for a screen that wants the
// movement since the setup and not the direction against A. nullopt when there is no estimate or a value is not finite.
[[nodiscard]] inline std::optional<CameraOrientation> CameraOrientationOfEstimate(const std::optional<a0::m2::experimental::OutputFrameDisplay>& display) {
    if (!display) return std::nullopt;
    if (!std::isfinite(display->rotation_clockwise_deg) || !std::isfinite(display->lens_tilt_deg) || !live_orientation_detail::Finite(display->lens_direction))
        return std::nullopt;
    return CameraOrientationOf(*display);
}

// The sentences of the two cameras. `line` is the one line for the status row of the window; the details are for a dialog or the
// log. All of them come from TXT's CameraOrientationShortText / CameraOrientationDetailText.
struct LiveOrientationTexts {
    std::string a_short, b_short, a_detail, b_detail;
    std::string line;
};
[[nodiscard]] inline LiveOrientationTexts LiveOrientationTextsOf(const std::optional<CameraOrientation>& a, const std::optional<CameraOrientation>& b) {
    LiveOrientationTexts texts;
    texts.a_short = CameraOrientationShortText(OrientationSubject::camera_a_at_calibration, a);
    texts.b_short = CameraOrientationShortText(OrientationSubject::camera_b_against_a, b);
    texts.a_detail = CameraOrientationDetailText(OrientationSubject::camera_a_at_calibration, a);
    texts.b_detail = CameraOrientationDetailText(OrientationSubject::camera_b_against_a, b);
    texts.line = texts.a_short + " ／ " + texts.b_short;
    return texts;
}

// The code that the connection of the estimator returns while the entrance of the estimate does not exist yet (the live C is then
// drawn with the condition's fixed mapping; LiveRegistrationStateText names it).
inline constexpr std::string_view kLiveEstimatorUnavailableReason = "live-estimator-unavailable";

// The components of an estimate that are outside the condition's range, as a person reads them (codes of EnvelopeComponentCode):
// "位置・回転". A code that is not known is left out; empty when none is.
[[nodiscard]] inline std::string LiveExceededComponentsText(const std::vector<std::string>& codes) {
    std::string text;
    for (const char* code : {"position", "rotation", "tilt", "height", "surface-height", "surface-tilt"}) {
        if (std::find(codes.begin(), codes.end(), std::string(code)) == codes.end()) continue;
        const std::string_view name = std::string_view(code) == "position" ? "位置"
            : std::string_view(code) == "rotation" ? "回転"
            : std::string_view(code) == "tilt" ? "傾き"
            : std::string_view(code) == "height" ? "高さ"
            : std::string_view(code) == "surface-height" ? "紙面の高さ" : "紙面の傾き";
        text += (text.empty() ? "" : "・") + std::string(name);
    }
    return text;
}

// One sentence about what the live C is drawn with. The picture is a guide: no sentence calls it within a criterion, and none says
// that a composite cannot be saved. `live_enabled` is false when the condition has no registration.live (LiveRegistrationDisabledReason).
// `bake_failed`: the estimate in force could not be drawn (the table of B could not be made for it), so C keeps the last mapping
// that could be drawn.
[[nodiscard]] inline std::string LiveRegistrationStateText(const LiveRegistrationView& view, bool live_enabled, bool bake_failed = false) {
    if (!live_enabled) return "Cは固定の写像で描いています（映像からの合わせ込みは条件で無効です）";
    if (bake_failed && view.state != LiveRegistrationState::fixed) return "推定した位置ではCを描けませんでした。直前に描けた位置のまま表示しています";
    switch (view.state) {
    case LiveRegistrationState::tracking:
    {
        if (!view.beyond_envelope) return "Cは映像から姿勢と紙面を合わせた目安です";
        const std::string components = LiveExceededComponentsText(view.exceeded_components);
        return "Cは映像から姿勢と紙面を合わせた目安です（許容の範囲の外の値で描いています" + (components.empty() ? std::string() : "。範囲を超えた量: " + components) + "）";
    }
    case LiveRegistrationState::holding:
        return "映像から推定できないため、直前の推定を保って描いています";
    case LiveRegistrationState::fixed:
        break;
    }
    if (view.fixed_reason == "estimate-failed" &&
        std::find(view.last_failure.begin(), view.last_failure.end(), std::string(kLiveEstimatorUnavailableReason)) != view.last_failure.end())
        return "映像からの推定は未接続です（Cは条件の固定の位置で表示しています）";
    if (view.fixed_reason == "hold-expired") return "推定を保てる時間を過ぎたため、Cは固定の写像で描いています";
    if (view.fixed_reason == "estimate-failed") return "映像から推定できないため、Cは固定の写像で描いています";
    return "推定の完了を待っています（それまでCは固定の写像で描きます）";
}

// ---- The state row of the live C under the position that is confirmed once (#437 stage 1) -------------------------------------------
// The reason of an estimate that decided no position, in a few words (the first code of the estimator's own list).
[[nodiscard]] inline std::string LiveEstimateReasonText(const std::vector<std::string>& reasons) {
    const std::string reason = reasons.empty() ? std::string() : reasons.front();
    if (reason == "too-dark") return "暗すぎます";
    if (reason == "no-overlap") return "二台の重なりが足りません";
    if (reason == "coarse-no-unique-blocks" || reason == "unique-blocks" || reason == "too-few-points") return "模様が足りません";
    if (reason == "repeating-only" || reason == "coarse-ambiguous") return "模様が繰り返していて、位置が一つに決まりません";
    if (reason == "coarse-inconsistent") return "大まかな位置が一致しませんでした";
    if (reason == "out-of-envelope") return "許容の範囲の外の値でした";
    if (reason == kLiveEstimatorUnavailableReason) return "映像からの推定は未接続です";
    if (reason == "bake-failed") return "推定した位置ではCを描けませんでした";
    if (reason == kLivePositionSeamOutsideOverlapReason) return "継ぎ目が重なりの外に出ます";
    if (reason == kLivePositionOutputUncoveredReason) return "出力の端が覆われません";
    if (reason == "estimate-refused" || reason == "estimator-exception") return "推定の処理が途中で失敗しました";
    return "推定が確かではありません";
}
[[nodiscard]] inline std::string LivePlacementSourceText(LivePlacementSource source) {
    switch (source) {
    case LivePlacementSource::fixed_condition: return "条件の位置";
    case LivePlacementSource::auto_start: return "開始時の自動";
    case LivePlacementSource::auto_button: return "ボタンの自動";
    case LivePlacementSource::after_capture: return "撮影の後の位置";
    case LivePlacementSource::manual: return "手で直した位置";
    }
    return {};
}
// How far the live C moved at the shot: the distance between the position it was drawn with before the shot and the one it is drawn with
// now, along the seam (the largest of the lattice's samples, in pixels of the JPEG's raster). It is not a difference between the live C and
// the JPEG: after the shot the live C is drawn with the JPEG's own position. Empty difference: nothing is said.
[[nodiscard]] inline std::string LivePlacementMovedText(const std::optional<double>& difference_px) {
    if (!difference_px || !std::isfinite(*difference_px)) return {};
    if (*difference_px < 0.05) return "撮影の前のライブの位置から動いていません";
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "撮影の前のライブの位置から継ぎ目で最大 " << std::fixed << std::setprecision(1) << *difference_px << " px（JPEGの画素）動かしました";
    return s.str();
}
// The layer 2 that the JPEG has and the live C does not draw: its largest shift when the record gave one (output px, the JPEG's pixels).
[[nodiscard]] inline std::string LivePlacementLayerTwoText(const LivePlacementCaptureNote& note) {
    if (!note.layer_two_dropped) return {};
    if (!note.layer_two_max_px || !std::isfinite(*note.layer_two_max_px)) return "局所の補正はライブのCには出ません";
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "JPEGの局所の補正（最大 " << std::fixed << std::setprecision(1) << *note.layer_two_max_px << " px）はライブのCには出ません";
    return s.str();
}
// What the shot did to the position (after_capture): the JPEG's own estimate decided it (G1, G2), or it did not and the JPEG was made with a
// position that the condition or the coarse stage gave (G3, G4). Either way the live C is drawn with the JPEG's position from now on, so it
// is at the same place as the JPEG; what the sentence says moved is the live C, and what stays different is layer 2.
[[nodiscard]] inline std::string LivePlacementCaptureText(const LivePlacementCaptureNote& note) {
    const auto moved = LivePlacementMovedText(note.difference_px);
    const auto layer_two = LivePlacementLayerTwoText(note);
    std::string text;
    if (note.level == "G1" || note.level == "G2") {
        text = "撮影の後の自動調整で位置を確定しました（" + note.level + "）";
        if (!moved.empty()) text += "。" + moved;
        text += "。今のライブのCはJPEGと同じ位置です";
    } else {
        text = std::string("撮影の後の自動調整では確かな位置が出ませんでした。JPEGは") + (note.level == "G3" ? "目安の位置（G3）" : "条件の位置") + "で作りました。ライブのCもその位置へ移しました";
        if (!moved.empty()) text += "（" + moved + "）";
    }
    if (!layer_two.empty()) text += "。" + layer_two;
    return text;
}

// One sentence about what the live C is drawn with, and what the last estimate came to. The picture is a guide: no sentence calls it within a
// criterion, and none says that a composite cannot be saved. `bake_failed`: the position in force could not be drawn (the table of B could not
// be made for it), so C keeps the last mapping that could.
[[nodiscard]] inline std::string LivePlacementStateText(const LivePlacementView& view, bool bake_failed = false) {
    // The button is refused when the condition has no registration.live, so it is named only when it works.
    const std::string hint = view.live_enabled ? "。紙を替えた時は「自動位置合わせ」を押してください" : std::string();
    if (view.activity != LivePlacementActivity::idle) return "自動合成中です…";
    if (bake_failed) return view.source == LivePlacementSource::after_capture ? "撮影JPEGの位置ではCを描けませんでした。直前に描けた位置のまま表示しています"
                                                                              : "推定した位置ではCを描けませんでした。直前に描けた位置のまま表示しています";
    if (view.outcome == LivePlacementOutcomeKind::not_decided)
        return "位置を決められませんでした（理由: " + LiveEstimateReasonText(view.last_failure) + "）。前の位置のままです（出どころ: " + LivePlacementSourceText(view.source) +
            "）。模様のある所で「自動位置合わせ」を押してください";
    if (view.source == LivePlacementSource::after_capture && view.capture) return LivePlacementCaptureText(*view.capture) + hint;
    if (!view.live_enabled && view.source == LivePlacementSource::fixed_condition) return "Cは固定の写像で描いています（映像からの合わせ込みは条件で無効です）";
    if (view.outcome == LivePlacementOutcomeKind::placed) {
        const bool button = view.outcome_ask && *view.outcome_ask == LiveEstimateAsk::button;
        std::string text = std::string("位置を確定しました（自動・") + (button ? "ボタン" : "開始時");
        if (view.beyond_envelope) {
            const std::string components = LiveExceededComponentsText(view.exceeded_components);
            text += "。許容の範囲の外の値で描いています" + (components.empty() ? std::string() : "。範囲を超えた量: " + components);
        }
        return text + "）" + hint;
    }
    if (view.source == LivePlacementSource::after_capture) return "Cは撮影の後の位置で描いています" + hint;
    return "Cは条件の位置で描いています" + hint;
}

// The state row when the registration of the live C could not be set up for the session (C is the condition's fixed mapping).
[[nodiscard]] inline std::string LiveRegistrationStartFailedText() {
    return "映像からの合わせ込みを始められませんでした（Cは条件の固定の位置で表示しています）";
}

// The row under the title of C: what the cameras face. B is the value against A now; A is the value at calibration.
[[nodiscard]] inline std::string LiveOrientationRowText(const LiveOrientationTexts& texts) {
    return "カメラの向き（Bは、Aに対する今の値）　" + texts.line;
}
}
