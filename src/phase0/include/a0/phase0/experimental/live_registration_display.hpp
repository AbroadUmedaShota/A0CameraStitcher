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
    // A shift too small to read as a number ("最大 0.0 px") is not given as one.
    if (!note.layer_two_max_px || !std::isfinite(*note.layer_two_max_px) || *note.layer_two_max_px < 0.05) return "局所の補正はライブのCには出ません";
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
    // The JPEG was made with the position as it was (the capture's own estimate decided nothing, or the position was set by hand): the live C
    // did not move to anything.
    if (note.level == kLiveCaptureConfirmedLevel) return "ライブで決めていた位置で JPEG を作りました（撮影の後の自動調整では位置を決められませんでした）。ライブの位置は変えていません";
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

// ---- Hand steps (#437 stage 2) ------------------------------------------------------------------------------------------------------
// What a press of a hand button does, as a person sees it on C: the picture of B moves right or left, down or up, or turns clockwise or
// counter-clockwise (the arrows of the buttons say the same). The counts of the machine (HandCounts) are the displacement that the manual
// adjustment of the condition enters; what a positive count does to B's picture on C (the constants below: -1 is to the left, up and
// counter-clockwise) is turned into the buttons' and the sentences' directions once, here, and nowhere else. A smoke draws B's picture
// before and after each button and holds the constants to what is seen.
enum class LiveHandButton { left, right, up, down, turn_counter_clockwise, turn_clockwise };
inline constexpr int kLiveHandSeamDxScreenSign = -1;     // screen right = sign * counts along the seam
inline constexpr int kLiveHandSeamDyScreenSign = -1;     // screen down = sign * counts across it
inline constexpr int kLiveHandRotationScreenSign = -1;   // screen clockwise = sign * counts about the axis
// The counts of one press: 12 px and 4 px along and across the seam, 0.2 and 0.05 degrees about the axis (`fine` picks the small step).
[[nodiscard]] inline a0::m2::experimental::HandCounts LiveHandCountsOf(LiveHandButton button, bool fine) {
    namespace ex = a0::m2::experimental;
    const std::int32_t seam = fine ? ex::kHandFineSeamCounts : ex::kHandCoarseSeamCounts;
    const std::int32_t turn = fine ? ex::kHandFineRotationCounts : ex::kHandCoarseRotationCounts;
    switch (button) {
    case LiveHandButton::right: return {seam * kLiveHandSeamDxScreenSign, 0, 0};
    case LiveHandButton::left: return {-seam * kLiveHandSeamDxScreenSign, 0, 0};
    case LiveHandButton::down: return {0, seam * kLiveHandSeamDyScreenSign, 0};
    case LiveHandButton::up: return {0, -seam * kLiveHandSeamDyScreenSign, 0};
    case LiveHandButton::turn_clockwise: return {0, 0, turn * kLiveHandRotationScreenSign};
    case LiveHandButton::turn_counter_clockwise: return {0, 0, -turn * kLiveHandRotationScreenSign};
    }
    return {};
}
// The amounts entered so far as the picture of B moves on C: to the right, downward, clockwise. Pixels are the JPEG's, degrees are degrees.
struct LiveHandScreenMove { double right_px{}, down_px{}, clockwise_deg{}; };
[[nodiscard]] inline LiveHandScreenMove LiveHandScreenMoveOf(const a0::m2::experimental::HandCounts& counts) {
    const auto values = a0::m2::experimental::HandValuesOf(counts);
    return {values.seam_dx_px * kLiveHandSeamDxScreenSign, values.seam_dy_px * kLiveHandSeamDyScreenSign, values.rotation_deg * kLiveHandRotationScreenSign};
}
namespace live_hand_detail {
inline std::string Fixed(double value, int digits) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(digits) << value;
    return s.str();
}
inline std::string Amount(double value, const char* positive, const char* negative, const char* unit, int digits) {
    if (value == 0.0) return std::string("0 ") + unit;
    return std::string(value > 0 ? positive : negative) + " " + Fixed(std::abs(value), digits) + " " + unit;
}
}
// "左右 右へ 12 px・上下 上へ 4 px・回り 時計回りに 0.05°": the sum of the amounts, a zero as 0.
[[nodiscard]] inline std::string LiveHandMovesText(const a0::m2::experimental::HandCounts& counts) {
    using namespace live_hand_detail;
    const auto move = LiveHandScreenMoveOf(counts);
    return "左右 " + Amount(move.right_px, "右へ", "左へ", "px", 0) + "・上下 " + Amount(move.down_px, "下へ", "上へ", "px", 0) + "・回り " +
        (move.clockwise_deg == 0.0 ? std::string("0°") : std::string(move.clockwise_deg > 0 ? "時計回りに " : "反時計回りに ") + Fixed(std::abs(move.clockwise_deg), 2) + "°");
}
// One live pixel of C is this many pixels of the JPEG (the grid's pitch over the raster's pitch); the hand steps are counted in the JPEG's.
[[nodiscard]] inline std::string LiveHandScaleText(double live_pixel_in_output_px) {
    if (!std::isfinite(live_pixel_in_output_px) || !(live_pixel_in_output_px > 0)) return {};
    return "ライブの 1 画素は撮影の約 " + live_hand_detail::Fixed(live_pixel_in_output_px, 1) +
        " 画素に当たります。細かく合わせる時は、撮影してから「拡大確認・再Merge」で見てください";
}
// Why a press changed nothing, in words that say what to do next. `placement_beyond_envelope`: P itself is outside the pose envelope (an
// estimate was applied beyond it), so a step that leaves it outside is refused for that reason. `bake_failed`: the last bake of P failed.
// An accepted press has no refusal text (empty).
[[nodiscard]] inline std::string LiveHandRefusalText(const LiveHandResult& result, bool placement_beyond_envelope = false, bool bake_failed = false) {
    const std::string kept = "位置は変えていません";
    switch (result.outcome) {
    case LiveHandOutcome::accepted: return {};
    case LiveHandOutcome::refused_not_configured: return "ライブが始まっていないので、位置を動かせません";
    case LiveHandOutcome::refused_capturing: return "撮影中は位置を動かせません。撮影が終わると動かせます";
    case LiveHandOutcome::refused_stopped: return "ライブを止めている間は位置を動かせません。「ライブ再開」を押してから動かしてください";
    case LiveHandOutcome::refused_not_drawn:
        return bake_failed ? "ライブの合成がまだ確定した位置で描けていないので、位置を動かせません。「自動調整」を押すか、ライブを止めて再開してください"
                           : "位置を画面に描いている途中です。描き終わるまで少し待ってから押してください";
    case LiveHandOutcome::refused_zero: return "動かす量が 0 です。" + kept;
    case LiveHandOutcome::refused_range: return "これ以上は動かせません（入れた量が大きすぎます）。" + kept;
    case LiveHandOutcome::refused_envelope: {
        const std::string components = LiveExceededComponentsText(result.exceeded);
        if (placement_beyond_envelope)
            return "今の位置は範囲の外で、この押し方では範囲の内に入りません。範囲の外の位置を直すのは「自動調整」か校正の取り直しです。" + kept;
        return "範囲の端です。これ以上は動かせません" + (components.empty() ? std::string() : "（超える項目: " + components + "）") +
            "。設置が動いた時は、校正の取り直しが要ります。" + kept;
    }
    case LiveHandOutcome::refused_cover:
        if (!result.seam_covered) return "この位置では継ぎ目が重なりの外に出るため、動かせません。" + kept;
        if (!result.output_covered) return "この位置では出力の端が覆われないため、動かせません。" + kept;
        return "この位置では合成が成り立たないため、動かせません。" + kept;
    case LiveHandOutcome::refused_unsolvable: return "この量では位置を計算できないため、動かせません。向きや刻みを変えて試してください。" + kept;
    case LiveHandOutcome::refused_error: return "位置の確かめでエラーが起きたため、動かせません（詳細はログ）。" + kept;
    case LiveHandOutcome::refused_stale: return "押している間に位置が変わったので、反映しませんでした。もう一度押してください";
    }
    return {};
}
// The state row while a hand press is being checked (the check of a full-size position takes a moment), and what the presses that came
// meanwhile are going to do.
[[nodiscard]] inline std::string LiveHandBusyText(unsigned queued_presses) {
    std::string text = "押したことを受け付けました。位置を確かめています…";
    if (queued_presses) text += "（続けて押した " + std::to_string(queued_presses) + " 回分は、確かめが終わってからまとめて入れます）";
    return text;
}
// While no check runs and a run of waiting presses waits for the screen to be drawn with the position before them: what the state row says.
[[nodiscard]] inline std::string LiveHandWaitingText(unsigned queued_presses) {
    return "押したことを受け付けました。前の位置を描き終えてから、まとめて入れます（" + std::to_string(queued_presses) + " 回分）";
}
// A check refused the press: what was waiting behind it goes too, and the position stays as it was.
[[nodiscard]] inline std::string LiveHandQueueDroppedText(unsigned queued_presses) {
    return queued_presses ? "続けて押した " + std::to_string(queued_presses) + " 回分も反映していません" : std::string();
}
// The notice after a refusal, and the sentence about the presses that waited behind it, as two sentences.
[[nodiscard]] inline std::string LiveHandRefusalWithQueueText(const std::string& refusal, unsigned queued_presses) {
    const auto queue = LiveHandQueueDroppedText(queued_presses);
    return queue.empty() ? refusal : refusal + "。" + queue;
}
[[nodiscard]] inline std::string LiveHandDroppedByCaptureText() {
    return "撮影を頼んだので、確かめの途中だった手の押下は捨てました。位置は撮影の前のままです";
}
// Presses that waited to be checked were dropped without a check refusing them (Live stopped, the position was not drawn in time, a new session began):
// how many, and where the position stays. When Live stopped, what to do next is said too.
[[nodiscard]] inline std::string LiveHandQueueLostText(unsigned presses, bool live_stopped = false) {
    std::string text = "押した " + std::to_string(presses) + " 回分は反映していません。この分では位置を変えていません";
    if (live_stopped) text += "。「ライブ再開」を押してから、もう一度押してください";
    return text;
}
// A pair capture was asked for while presses were not in the position yet (a check ran, they waited, or their position was not drawn): the capture is
// taken with the position that is drawn, and they are not in it. Said when it is asked for, and in the row of the capture's result. One camera's capture
// does not hold the position (a check that runs goes on and its press is entered): its result row says the first sentence only.
[[nodiscard]] inline std::string LiveCaptureUnenteredAskText(unsigned presses) {
    return "（撮影の直前に押した手の押下 " + std::to_string(presses) + " 回分は、入れずに撮ります）";
}
[[nodiscard]] inline std::string LiveCaptureUnenteredResultText(unsigned presses, bool pair = true) {
    std::string text = "撮影の直前に押した手の押下 " + std::to_string(presses) + " 回分は入っていません";
    if (pair) text += "。JPEG とライブは、撮影の前の位置のままです";
    return text;
}
// The button of the automatic estimate while a hand check runs or presses wait: it is not accepted (what was pressed goes in first).
[[nodiscard]] inline std::string LiveEstimateRefusedByHandText() {
    return "手で動かしている途中です。動き終わってから押してください";
}

// How a press of a hand button is taken (the window's thread). One check runs at a time, and a check is made against the picture that is on the screen
// (the machine refuses one made against a picture that is not drawn with P), so a press is checked at once only when nothing is in the way:
//  - start: nothing runs, nothing waits, and the screen is drawn with P (or Live is stopped, a capture holds P, the bake of P failed, or the picture is
//    not drawn with a P that the window did not get from a hand press: the machine refuses it and the sentence says why);
//  - queue: a check runs or a run waits, or the position that a hand press of the window was given is not on the screen yet (it waits to be drawn, or it
//    is P but the picture was not drawn with it). The press is added to the ones that wait, and their sum is checked once the screen is drawn with P;
//  - refuse_capturing: a capture holds P while a check still runs: the press is refused here (it cannot be started and would only be dropped later).
// A picture that is not drawn with P for any other reason (an estimate's position, a shot's, a session that began, a table that was laid again) is
// refused by the machine with "drawing now": a press made against the picture before it must not be added on top of a position that it did not see.
enum class LiveHandPressRoute { start, queue, refuse_capturing };
struct LiveHandPressContext {
    bool busy{false};            // a check runs, or a run waits to be started
    bool hand_wanted{false};     // an accepted position waits to be drawn
    bool drawn{false};           // the picture on the screen was drawn with a table baked for P
    bool hand_undrawn{false};    // a check of the window was accepted and the screen was not drawn with its position yet (the window's mark)
    bool bake_failed{false};     // the last bake of P failed (a wait would not end)
    bool capturing{false};       // a pair capture holds P
    bool live_running{false};    // Live runs and is not stopping
};
[[nodiscard]] inline LiveHandPressRoute RouteLiveHandPress(const LiveHandPressContext& context) {
    if (context.busy) return context.capturing ? LiveHandPressRoute::refuse_capturing : LiveHandPressRoute::queue;
    if (!context.live_running || context.capturing || context.bake_failed) return LiveHandPressRoute::start;
    if (context.hand_wanted || (!context.drawn && context.hand_undrawn)) return LiveHandPressRoute::queue;
    return LiveHandPressRoute::start;
}
[[nodiscard]] inline std::string_view LiveHandPressRouteCode(LiveHandPressRoute route) {
    switch (route) {
    case LiveHandPressRoute::start: return "start";
    case LiveHandPressRoute::queue: return "queue";
    case LiveHandPressRoute::refuse_capturing: return "refuse-capturing";
    }
    return {};
}
// A check that was refused because something moved while it ran: is its amount checked again (with what waits) or is it refused with the sentence?
// Stale (P or the hand position that waits changed): again. Not drawn: again only when the check began with the screen drawn with P and P is the same
// one now (the table was laid again), never when it began against a picture that was not drawn with P (an estimate's or a shot's position came in
// between) or when P is another one (what the press saw is gone).
struct LiveHandRetryContext {
    LiveHandOutcome outcome{LiveHandOutcome::accepted};
    bool bake_failed{false};
    bool began_drawn{false};           // the screen was drawn with P when the check began
    std::uint64_t serial_at_begin{0};  // P's serial when it began
    std::uint64_t serial_now{0};
};
[[nodiscard]] inline bool LiveHandCheckIsEnteredAgain(const LiveHandRetryContext& context) {
    if (context.outcome == LiveHandOutcome::refused_stale) return true;
    if (context.outcome == LiveHandOutcome::refused_not_drawn)
        return !context.bake_failed && context.began_drawn && context.serial_at_begin == context.serial_now;
    return false;
}
[[nodiscard]] inline std::string LiveHandWithdrewEstimateText() {
    return "自動調整の途中だったので、その結果は使いません。手で直した位置を優先します";
}
[[nodiscard]] inline std::string LiveHandBecameConditionText() {
    return "手で動かした結果が条件の位置に戻ったので、手で直した印は消えます";
}
// The button of the automatic estimate while a capture holds the position.
[[nodiscard]] inline std::string LiveEstimateRefusedText(LiveAskResult result) {
    switch (result) {
    case LiveAskResult::accepted: return {};
    case LiveAskResult::busy: return "自動合成中です…";
    case LiveAskResult::disabled: return "この固定の合成条件では、映像からの自動調整は無効です";
    case LiveAskResult::capturing: return "撮影中は「自動調整」を押せません。撮影が終わってから押してください";
    }
    return {};
}
// A pair capture that was not started because the position is not drawn yet (or no session began), and what to do.
[[nodiscard]] inline std::string LiveCaptureRefusalText(LiveCaptureAdmission admission, bool bake_failed = false) {
    switch (admission) {
    case LiveCaptureAdmission::accepted: return {};
    case LiveCaptureAdmission::not_configured: return "ライブが始まっていないので、両台撮影を始められません";
    case LiveCaptureAdmission::already_capturing: return "前の撮影の結果を待っています。終わるまで、もう一度押さないでください";
    case LiveCaptureAdmission::not_drawn:
        return bake_failed ? "ライブの合成がまだ確定した位置で描けていないので、撮影できません。「自動調整」を押すか、ライブを止めて再開してください"
                           : "ライブの合成がまだ確定した位置で描けていないので、撮影できません。少し待ってからもう一度押してください";
    }
    return {};
}

// The screen is still drawn with the condition's table (a session just began, or a shot just ended) and the table of the position is not baked yet.
[[nodiscard]] inline std::string LiveNotDrawnYetText() {
    return "ライブの合成は、まだ確定した位置で描けていません。描き終わるまで、撮影と手の操作は少し待ってください";
}

// What the state row says of a position that a person set by hand: the amounts, what the shot did with it, and the way back to an estimate.
[[nodiscard]] inline std::string LiveManualStateText(const LivePlacementView& view) {
    std::string text = "手で直しました（Bの絵の動き " + (view.hand_entered ? LiveHandMovesText(*view.hand_entered) : std::string("合計は不明")) + "、撮影の画素。合計）。";
    if (view.capture && view.capture->level == kLiveCaptureConfirmedLevel)
        text += "手で直した位置で JPEG を作りました。ライブは手で直した位置のままです";
    else if (view.capture && view.capture->level == "G4")
        text += "手で直した位置では継ぎ目が重なりの外に出るか出力の端が覆われないため、JPEG は条件の位置で作りました。ライブは手で直した位置のままです";
    else
        text += "この位置で撮影します";
    if (view.live_enabled) text += "。自動で測り直すには「自動調整」を押してください（手で直した位置は置き換わります）";
    return text;
}

// One sentence about what the live C is drawn with, and what the last estimate came to. The picture is a guide: no sentence calls it within a
// criterion, and none says that a composite cannot be saved. `bake_failed`: the position in force could not be drawn (the table of B could not
// be made for it), so C keeps the last mapping that could.
namespace live_state_detail {
inline std::string Body(const LivePlacementView& view, bool bake_failed) {
    // The button is refused when the condition has no registration.live, so it is named only when it works.
    const std::string hint = view.live_enabled ? "。紙を替えた時は「自動調整」を押してください" : std::string();
    if (view.capturing) return "撮影中は位置を動かせません。撮影が終わると動かせます";
    if (view.activity != LivePlacementActivity::idle) return "自動合成中です…";
    if (view.hand_wanted && !bake_failed) return "手で直した位置を描いています…";
    if (bake_failed) return view.source == LivePlacementSource::after_capture ? "撮影JPEGの位置ではCを描けませんでした。直前に描けた位置のまま表示しています"
                          : view.source == LivePlacementSource::manual      ? "手で直した位置ではCを描けませんでした。直前に描けた位置のまま表示しています"
                                                                              : "推定した位置ではCを描けませんでした。直前に描けた位置のまま表示しています";
    if (view.outcome == LivePlacementOutcomeKind::not_decided)
        return "位置を決められませんでした（理由: " + LiveEstimateReasonText(view.last_failure) + "）。前の位置のままです（出どころ: " + LivePlacementSourceText(view.source) +
            "）。模様のある所で「自動調整」を押してください";
    if (view.source == LivePlacementSource::manual) return LiveManualStateText(view);
    if (view.source == LivePlacementSource::after_capture && view.capture) return LivePlacementCaptureText(*view.capture) + hint;
    // A shot that used the position as it was (level confirmed) leaves the position and its source; the shot is still said.
    if (view.capture && view.capture->level == kLiveCaptureConfirmedLevel)
        return "ライブで決めていた位置で JPEG を作りました（撮影の後の自動調整では位置を決められませんでした）。ライブの位置は変えていません" + hint;
    if (view.hand_returned)
        return "手で動かした量が 0 に戻ったので、手で直す前の位置に戻しました。撮影の後の自動調整で位置が変わることがあります" + hint;
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
}
[[nodiscard]] inline std::string LivePlacementStateText(const LivePlacementView& view, bool bake_failed = false) {
    std::string text = live_state_detail::Body(view, bake_failed);
    // A condition that changed after a hand step: the hand-set position is gone and, when the condition has a live estimate, one estimate is asked
    // for (said in front of the rest).
    if (view.hand_discarded_by_condition)
        text = std::string("固定の合成条件が変わったので、手で直した位置を捨て、新しい条件の位置に戻しました。") + (view.live_enabled ? "自動調整を 1 回行います。" : "") + text;
    return text;
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
