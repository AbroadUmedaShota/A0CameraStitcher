#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/phase0/experimental/fixed_composite_registration_text.hpp"
#include "a0/phase0/experimental/live_registration_tracker.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
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

// The state row when the registration of the live C could not be set up for the session (C is the condition's fixed mapping).
[[nodiscard]] inline std::string LiveRegistrationStartFailedText() {
    return "映像からの合わせ込みを始められませんでした（Cは条件の固定の位置で表示しています）";
}

// The row under the title of C: what the cameras face. B is the value against A now; A is the value at calibration.
[[nodiscard]] inline std::string LiveOrientationRowText(const LiveOrientationTexts& texts) {
    return "カメラの向き（Bは、Aに対する今の値）　" + texts.line;
}
}
