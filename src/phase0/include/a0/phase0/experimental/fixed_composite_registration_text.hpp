#pragma once
#include "a0/m2/experimental/capture_registration.hpp"
#include "a0/m2/experimental/registration_gate.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace a0::phase0::experimental {
// The sentences that the review window and the logs show about the registration (#417, docs/design/fixed-composite-registration.md
// 11.6 items 2 and 6, 11.7, 11.17.1, 11.24). All of them are made here, from the codes and values of the report, and nowhere
// else: nothing about the sentences is stored in the report. UTF-8 text, Japanese, plain words (no code names, no paths, no
// camera identifiers). Line breaks inside a text are LF.
//
// Rules the bodies keep:
//  - the numbers are the report's, not rounded in the report. A sentence rounds for reading and says "約" where it does. The
//    rounding is in one place (registration_text_detail::Decimals): degrees 1 digit, mm 1, px 2, percent 2, cm 1, trailing
//    zeros dropped. When a sentence shows a value and the limit it was compared with, and the value is greater, the digits are
//    raised until the two read differently;
//  - a limit is warned about only when the value is greater than the limit (a value equal to the limit is within it). A text
//    never says that a value "exceeds" when it does not, and the label "exceeded" is dropped for such a warning;
//  - directions are those of the drawing as seen on the screen (right, left, above, below, and the four in between) taken from
//    OutputFrameDisplay; the report's raw pose is never read here, so the same physical movement gives the same sentence for
//    either mount orientation (11.24). A translation is told as the way the camera moved; the way to put it right is the
//    opposite way;
//  - the word for "passed" is not used for the gate or the composite, and no sentence says the composite cannot be written or
//    saved (11.6 item 6); no sentence tells to press or flatten the paper (decision D-d);
//  - no camera identifier and no path appears in a sentence; a code the reader of a sentence does not know is never printed (an
//    unknown code gets a general sentence).
//
// What the values of a warning mean for its sentence (the writer of the warnings, J8, fills them this way; *assumption* where
// the design does not say):
//  camera-position       value/limit: the translation across the baseline, mm. Direction: OutputFrameDisplay::camera_motion_mm
//  camera-rotation       value/limit: |rz|, deg. Direction: OutputFrameDisplay::rotation_clockwise_deg
//  camera-tilt           value/limit: the tilt, deg. Direction: OutputFrameDisplay::lens_direction
//  camera-height         value/limit: |tz| in percent (or mm). The sign and mm come from RegistrationTextContext::pose_display
//  surface-height        exceeded: value/limit are |h0| and its range in mm; notice: value/limit are the shift layer 1 made at
//                        the seam and the gate line, px
//  surface-tilt          value: the inclination, deg. Direction: OutputFrameDisplay::surface_high_side
//  output-edge-uncovered value: pixels (unit pixels) or the width (unit px)
//  seam-lift             value: the largest shift layer 2 made (px); rows_mm the rows; the height comes from parallax_summary
//  seam-lift-limit       value/limit: |dh| and maxLiftMm, mm; rows_mm the rows
//  fold-near-seam, gutter-in-band
//                        value: the distance from the seam, SIGNED (positive: to the right of the seam in the drawing), mm
//                        (or px); rows_mm: the line's row; remedy_amount_mm: the N of "N cm"
//  seam-displacement     value: the largest displacement plus the measurement uncertainty U, px (what is compared with the
//                        line); limit: the line L; rows_mm: the row of the largest
//  seam-dx-unmeasured    rows_mm: the rows where the left-right displacement was not measured
//  seam-unmeasurable     rows_mm: the rows
//  seam-wide-search      value: the offset along the baseline, px (or mm)
//  geometry-level        the level and its reasons come from RegistrationTextContext::geometry_level

// What the sentence of a warning needs besides the warning itself: the drawing-frame values behind the pose and the surface.
struct RegistrationTextContext {
    a0::m2::experimental::OutputFrameDisplay output_frame;
    a0::m2::experimental::SurfaceEstimate surface;
    double pitch_mm{};                  // output pixel pitch, to turn px into mm where a sentence gives both
    // Optional extras. A sentence that needs one and does not have it uses a plainer sentence.
    std::optional<a0::m2::experimental::content_registration::PoseDisplay> pose_display;   // p-hat against the accepted setup: the sign and mm of the height
    std::optional<double> pivot_depth_mm;                  // to turn a height in percent into mm (otherwise taken from pose_display)
    std::optional<a0::m2::render::Point> baseline;         // e_b: the camera-position sentence leaves out the motion along it (it goes to surface-height)
    std::optional<a0::m2::experimental::parallax_correction::ParallaxSummary> parallax_summary;   // the height for seam-lift
    const a0::m2::experimental::GeometryLevelDecision* geometry_level{};   // for the geometry-level warning; the caller keeps it alive
};

namespace registration_text_detail {
namespace ex = a0::m2::experimental;
namespace render = a0::m2::render;

enum class Unit {deg, mm, px, percent, cm, rows, pixels};

// The one place that decides how many digits a unit is read with.
inline int Decimals(Unit unit) {
    switch (unit) {
    case Unit::deg: return 1;
    case Unit::mm: return 1;
    case Unit::px: return 2;
    case Unit::percent: return 2;
    case Unit::cm: return 1;
    case Unit::rows: return 0;
    case Unit::pixels: return 0;
    }
    return 1;
}
inline std::string_view Suffix(Unit unit) {
    switch (unit) {
    case Unit::deg: return "°";
    case Unit::mm: return " mm";
    case Unit::px: return " px";
    case Unit::percent: return " %";
    case Unit::cm: return " cm";
    case Unit::rows: return " 行";
    case Unit::pixels: return " 画素";
    }
    return {};
}
inline Unit UnitOf(ex::WarningUnit unit) {
    switch (unit) {
    case ex::WarningUnit::px: return Unit::px;
    case ex::WarningUnit::mm: return Unit::mm;
    case ex::WarningUnit::deg: return Unit::deg;
    case ex::WarningUnit::percent: return Unit::percent;
    case ex::WarningUnit::rows: return Unit::rows;
    case ex::WarningUnit::pixels: return Unit::pixels;
    }
    return Unit::px;
}

// A number with its digits, trailing zeros dropped. A value that is not a number reads "不明".
inline std::string Fixed(double value, int decimals) {
    if (!std::isfinite(value)) return "不明";
    char buffer[400];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, decimals);
    if (result.ec != std::errc{}) return "不明";
    std::string text(buffer, result.ptr);
    if (decimals > 0) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text == "-0" || text.empty()) text = "0";
    return text;
}
// "2.5°", "10 mm". extra raises the digits.
inline std::string Q(double value, Unit unit, int extra = 0) {
    const auto text = Fixed(value, Decimals(unit) + extra);
    return text == "不明" ? text : text + std::string(Suffix(unit));
}
inline std::string Approx(double value, Unit unit) { return "約 " + Q(std::abs(value), unit); }
// The value and the limit it is compared with, read with enough digits to differ when the value is greater.
inline std::pair<std::string, std::string> AgainstLimit(double value, double limit, Unit unit) {
    for (int extra = 0; extra <= 3; ++extra) {
        auto a = Q(value, unit, extra);
        auto b = Q(limit, unit, extra);
        if (a != b || !(value > limit)) return {a, b};
    }
    return {Q(value, unit, 3), Q(limit, unit, 3)};
}
// "Y mm" or "Y1〜Y2 mm" (no "約").
inline std::string MmRange(const ex::RowSpan& rows) {
    const double low = std::min(rows.first, rows.last), high = std::max(rows.first, rows.last);
    const auto a = Fixed(low, Decimals(Unit::mm)), b = Fixed(high, Decimals(Unit::mm));
    if (a == "不明" || b == "不明") return "不明";
    if (a == b) return a + std::string(Suffix(Unit::mm));
    return a + "〜" + b + std::string(Suffix(Unit::mm));
}
inline std::optional<double> ToMm(double value, ex::WarningUnit unit, double pitch_mm) {
    if (!std::isfinite(value)) return std::nullopt;
    if (unit == ex::WarningUnit::mm) return value;
    if (unit == ex::WarningUnit::px && std::isfinite(pitch_mm) && pitch_mm > 0) return value * pitch_mm;
    return std::nullopt;
}
inline std::optional<double> ToPx(double value, ex::WarningUnit unit, double pitch_mm) {
    if (!std::isfinite(value)) return std::nullopt;
    if (unit == ex::WarningUnit::px) return value;
    if (unit == ex::WarningUnit::mm && std::isfinite(pitch_mm) && pitch_mm > 0) return value / pitch_mm;
    return std::nullopt;
}

// ---- Directions (11.24 item 8) ----
inline bool HasDirection(const render::Point& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::hypot(v.x, v.y) > 1e-12;
}
inline render::Point Opposite(const render::Point& v) { return {-v.x, -v.y}; }
// The eight words of a vector in the drawing as seen on the screen: x to the right, y downward. Empty for no direction.
inline std::string DirectionWord(const render::Point& v) {
    if (!HasDirection(v)) return {};
    constexpr std::array<const char*, 8> words{"右", "右下", "下", "左下", "左", "左上", "上", "右上"};
    constexpr double kPi = 3.14159265358979323846;
    const double angle = std::atan2(v.y, v.x) * 180.0 / kPi;   // (-180, 180]; 0 right, 90 down
    const int sector = static_cast<int>(std::floor((angle + 22.5) / 45.0));
    return words[static_cast<std::size_t>(((sector % 8) + 8) % 8)];
}
// "図面の右上"
inline std::string InDrawing(const render::Point& v) {
    const auto word = DirectionWord(v);
    return word.empty() ? word : "図面の" + word;
}
// The axis a unit normal measures along: the left-right component, the up-down component, or one of the diagonals.
inline std::string AxisName(const render::Point& normal) {
    if (!HasDirection(normal)) return {};
    constexpr double kPi = 3.14159265358979323846;
    double angle = std::atan2(normal.y, normal.x) * 180.0 / kPi;
    if (angle < 0) angle += 180.0;
    if (angle >= 180.0) angle -= 180.0;
    if (angle < 22.5 || angle >= 157.5) return "左右の成分";
    if (angle < 67.5) return "右下〜左上の斜めの成分";
    if (angle < 112.5) return "上下の成分";
    return "右上〜左下の斜めの成分";
}
inline render::Point WithoutComponent(const render::Point& v, const render::Point& along) {
    const double n = std::hypot(along.x, along.y);
    if (!(n > 1e-12) || !std::isfinite(n)) return v;
    const render::Point e{along.x / n, along.y / n};
    const double dot = v.x * e.x + v.y * e.y;
    return {v.x - dot * e.x, v.y - dot * e.y};
}
// The way of turning on the screen; empty for no turn.
inline std::string ClockwiseWord(double rotation_clockwise_deg) {
    if (!std::isfinite(rotation_clockwise_deg) || rotation_clockwise_deg == 0) return {};
    return rotation_clockwise_deg > 0 ? "時計回り" : "反時計回り";
}

inline bool Exceeds(const ex::RegistrationWarning& w) { return w.limit.has_value() && w.value > *w.limit; }
inline std::string LimitSuffix(const ex::RegistrationWarning& w) {
    if (!w.limit) return {};
    return "（許容 " + Q(*w.limit, UnitOf(w.unit)) + "）";
}
inline bool Has(const std::vector<std::string>& list, std::string_view code) {
    return std::find(list.begin(), list.end(), code) != list.end();
}
inline bool PositiveAmount(const std::optional<double>& amount) { return amount && std::isfinite(*amount) && *amount > 0; }
// mm per percent of height.
inline std::optional<double> MmPerPercent(const RegistrationTextContext& c) {
    if (c.pivot_depth_mm && std::isfinite(*c.pivot_depth_mm) && *c.pivot_depth_mm > 0) return *c.pivot_depth_mm / 100.0;
    if (c.pose_display && c.pose_display->height_percent != 0 && std::isfinite(c.pose_display->height_mm) &&
        std::isfinite(c.pose_display->height_percent))
        return std::abs(c.pose_display->height_mm / c.pose_display->height_percent);
    return std::nullopt;
}
// The height of a camera-height warning in mm and its limit in mm, when they can be told.
struct HeightMm {std::optional<double> value, limit;};
inline HeightMm HeightInMm(const ex::RegistrationWarning& w, const RegistrationTextContext& c) {
    HeightMm out;
    if (w.unit == ex::WarningUnit::mm) {
        out.value = std::abs(w.value);
        out.limit = w.limit;
    } else if (w.unit == ex::WarningUnit::percent) {
        if (const auto per = MmPerPercent(c)) {
            out.value = std::abs(w.value) * *per;
            if (w.limit) out.limit = *w.limit * *per;
        }
    }
    if (out.value && !std::isfinite(*out.value)) out.value.reset();
    return out;
}
}   // namespace registration_text_detail

// The word of a vector's direction in the drawing as seen on the screen ("右", "右下", "下", "左下", "左", "左上", "上", "右上");
// empty for a vector that has no direction.
[[nodiscard]] inline std::string DrawingDirectionWord(const a0::m2::render::Point& vector) {
    return registration_text_detail::DirectionWord(vector);
}

// ---- Geometry level (11.7, 11.18) ----
[[nodiscard]] inline std::string GeometryLevelName(a0::m2::experimental::GeometryLevel level) {
    using L = a0::m2::experimental::GeometryLevel;
    switch (level) {
    case L::g1: return "G1（姿勢・紙面・紙の浮きを補正）";
    case L::g2: return "G2（姿勢と紙面を補正）";
    case L::g3: return "G3（一様なずれだけ）";
    case L::g4: return "G4（固定の合成条件のまま）";
    }
    return "不明な段";
}
// The geometry level and the reason it is below the best one ("位置合わせを G3（一様なずれだけ）で行いました。理由: …").
[[nodiscard]] inline std::string GeometryLevelText(const a0::m2::experimental::GeometryLevelDecision& level) {
    using namespace registration_text_detail;
    std::vector<std::string> reasons;
    for (const auto& code : level.reasons) {
        std::string text;
        if (code == "seam-outside-overlap-at-level") text = "継ぎ目が重なりの外に出るため";
        else if (code == "output-uncovered-at-level") text = "出力の端にどちらのカメラも写していない所が残るため";
        if (!text.empty() && std::find(reasons.begin(), reasons.end(), text) == reasons.end()) reasons.push_back(text);
    }
    std::string text = "位置合わせを " + GeometryLevelName(level.level) + "で行いました";
    if (!reasons.empty()) {
        text += "。理由: ";
        for (std::size_t i = 0; i < reasons.size(); ++i) text += (i ? "、" : "") + reasons[i];
    } else if (level.level != a0::m2::experimental::GeometryLevel::g1) {
        text += "。理由: 上の段に必要な推定が確かでないか、使えなかったため";
    }
    if (level.beyond_envelope_applied) text += "。許容の範囲を超えた量も、推定が確かなので当てています";
    return text;
}

// ---- Pose components (11.24) ----
[[nodiscard]] inline std::string RegistrationWarningText(const a0::m2::experimental::RegistrationWarning& warning,
    const RegistrationTextContext& context);

// The one-line text of the pose components that exceeded (camera-position, -rotation, -tilt, -height), by component. For any
// other code it gives the warning's own sentence.
[[nodiscard]] inline std::string RegistrationPoseComponentText(const a0::m2::experimental::RegistrationWarning& warning,
    const RegistrationTextContext& context) {
    using namespace registration_text_detail;
    using C = ex::WarningCode;
    const auto unit = UnitOf(warning.unit);
    switch (warning.code) {
    case C::camera_position: {
        const auto mm = ToMm(warning.value, warning.unit, context.pitch_mm);
        const auto amount = mm ? Approx(*mm, Unit::mm) : Approx(warning.value, unit);
        std::string limit = LimitSuffix(warning);
        if (mm && warning.limit)
            if (const auto limit_mm = ToMm(*warning.limit, warning.unit, context.pitch_mm)) limit = "（許容 " + Q(*limit_mm, Unit::mm) + "）";
        auto motion = context.output_frame.camera_motion_mm;
        if (context.baseline) motion = WithoutComponent(motion, *context.baseline);
        const auto where = InDrawing(motion);
        return where.empty() ? "B の位置が" + amount + " ずれています" + limit : "B が" + where + "へ" + amount + " ずれています" + limit;
    }
    case C::camera_rotation: {
        const auto way = ClockwiseWord(context.output_frame.rotation_clockwise_deg);
        const auto amount = Approx(warning.value, unit);
        return way.empty() ? "B が" + amount + " 回っています" + LimitSuffix(warning)
                           : "B が画面で見て" + way + "に" + amount + " 回っています" + LimitSuffix(warning);
    }
    case C::camera_tilt: {
        const auto where = InDrawing(context.output_frame.lens_direction);
        const auto amount = Approx(warning.value, unit);
        return where.empty() ? "B のレンズが" + amount + " 傾いています" + LimitSuffix(warning)
                             : "B のレンズが" + where + "へ" + amount + " 傾いています" + LimitSuffix(warning);
    }
    case C::camera_height: {
        const auto height = HeightInMm(warning, context);
        const std::string focus = "。ピントが合わせ直された可能性もあります";
        if (height.value) {
            std::string way = "ずれた高さにあります";
            if (context.pose_display && context.pose_display->height_percent != 0)
                way = context.pose_display->height_percent > 0 ? "高い位置にあります" : "低い位置にあります";
            const auto limit = warning.limit ? "（許容 " + (height.limit ? Q(*height.limit, Unit::mm) : Q(*warning.limit, unit)) + "）" : std::string{};
            return "B が受け入れた設置の時より" + Approx(*height.value, Unit::mm) + " " + way + limit + focus;
        }
        return "B の高さが、受け入れた設置の時と" + Approx(warning.value, unit) + " 違います" + LimitSuffix(warning) + focus;
    }
    default: return RegistrationWarningText(warning, context);
    }
}

// The way to put a pose component right, told as the way to move the camera (the opposite of how it moved): "B のカメラを図面の
// 下へ約 3 mm 動かしてください". Empty for a code that is not a pose component or when the report has no direction to tell.
[[nodiscard]] inline std::string RegistrationPoseFixText(const a0::m2::experimental::RegistrationWarning& warning,
    const RegistrationTextContext& context) {
    using namespace registration_text_detail;
    using C = ex::WarningCode;
    const auto unit = UnitOf(warning.unit);
    switch (warning.code) {
    case C::camera_position: {
        auto motion = context.output_frame.camera_motion_mm;
        if (context.baseline) motion = WithoutComponent(motion, *context.baseline);
        const auto where = InDrawing(Opposite(motion));
        if (where.empty()) return {};
        const auto mm = ToMm(warning.value, warning.unit, context.pitch_mm);
        return "B のカメラを" + where + "へ" + (mm ? Approx(*mm, Unit::mm) : Approx(warning.value, unit)) + " 動かしてください";
    }
    case C::camera_rotation: {
        const auto way = ClockwiseWord(-context.output_frame.rotation_clockwise_deg);
        if (way.empty()) return {};
        return "B のカメラを画面で見て" + way + "に" + Approx(warning.value, unit) + " 回してください";
    }
    case C::camera_tilt: {
        const auto where = InDrawing(Opposite(context.output_frame.lens_direction));
        if (where.empty()) return {};
        return "B のレンズを" + where + "へ" + Approx(warning.value, unit) + " 向け直してください";
    }
    case C::camera_height: {
        if (!(context.pose_display && context.pose_display->height_percent != 0)) return {};
        const auto height = HeightInMm(warning, context);
        if (!height.value) return {};
        return std::string("B のカメラを") + Approx(*height.value, Unit::mm) + (context.pose_display->height_percent > 0 ? " 下げてください" : " 上げてください");
    }
    default: return {};
    }
}

// ---- How to put it right: the nine codes (11.17.2) ----
// `none` says that nothing is needed. amount_mm is the N of "約 N cm" for shift-drawing-horizontal.
[[nodiscard]] inline std::string RemedyGuidanceText(a0::m2::experimental::Remedy remedy, std::optional<double> amount_mm = std::nullopt) {
    using namespace registration_text_detail;
    using R = ex::Remedy;
    switch (remedy) {
    case R::shift_drawing_horizontal:
        if (PositiveAmount(amount_mm)) return "図面を左右に約 " + Q(*amount_mm / 10.0, Unit::cm) + " ずらして置き直してください";
        return "図面を左右にずらして置き直してください";
    case R::place_drawing_again: return "図面を置き直して、撮り直してください";
    case R::check_camera_mount: return "カメラの取り付けを確かめてください";
    case R::check_camera_mount_or_focus: return "カメラの高さと、ピントが合わせ直されていないかを確かめてください";
    case R::retake: return "撮り直してください";
    case R::retake_then_recalibrate: return "撮り直してください。直らなければ校正をやり直してください";
    case R::review_visually: return "合成画像を目で見て確かめてください";
    case R::contact_technician: return "設定の問題かもしれません。担当者に連絡してください";
    case R::none: return "特に対応は要りません";
    }
    return "合成画像を目で見て確かめてください";
}

// ---- Warnings ----
// One sentence per warning, by its code (the codes of capture_registration.hpp): what was found, with the values, and where the
// design's example puts it, how to put it right. For surface-height it gives the two readings the warning names in
// `interpretations` (both when it names neither).
[[nodiscard]] inline std::string RegistrationWarningText(const a0::m2::experimental::RegistrationWarning& warning,
    const RegistrationTextContext& context) {
    using namespace registration_text_detail;
    using C = ex::WarningCode;
    const auto unit = UnitOf(warning.unit);
    const auto rows_mm_text = [&]() -> std::string { return warning.rows_mm ? "約 " + MmRange(*warning.rows_mm) : std::string{}; };
    const auto first_row_mm_text = [&]() -> std::string {
        return warning.rows_mm ? "約 " + MmRange(ex::RowSpan{warning.rows_mm->first, warning.rows_mm->first}) : std::string{};
    };
    const auto amount_cm = [&]() -> std::string {
        return PositiveAmount(warning.remedy_amount_mm) ? "約 " + Q(*warning.remedy_amount_mm / 10.0, Unit::cm) + " " : std::string{};
    };
    switch (warning.code) {
    case C::camera_position:
    case C::camera_rotation:
    case C::camera_tilt:
    case C::camera_height: return RegistrationPoseComponentText(warning, context);
    case C::surface_height: {
        if (warning.unit == ex::WarningUnit::px) {
            // Within the range, but layer 1 moved the seam by more than the gate line (a notice).
            const bool known = context.surface.state == ex::SurfaceState::estimated && std::isfinite(context.surface.plane.h0_mm);
            const auto because = known ? "紙面が台より約 " + Q(std::abs(context.surface.plane.h0_mm), Unit::mm) +
                (context.surface.plane.h0_mm < 0 ? " 低いため、" : " 高いため、") : std::string("紙面の高さのため、");
            return because + "補正しなければ継ぎ目で約 " + Q(std::abs(warning.value), Unit::px) + " ずれていました" +
                (warning.limit ? "（基準 " + Q(*warning.limit, Unit::px) + "）" : std::string{}) +
                (warning.applied ? "。位置合わせで補正しました" : "。位置合わせには当てていません");
        }
        const bool raised = Has(warning.interpretations, "surface-raised");
        const bool moved = Has(warning.interpretations, "camera-moved-along-baseline");
        const bool both = (raised && moved) || (!raised && !moved);
        const double height = warning.unit == ex::WarningUnit::mm ? std::abs(warning.value) : std::abs(context.surface.plane.h0_mm);
        const bool lower = context.surface.state == ex::SurfaceState::estimated && context.surface.plane.h0_mm < 0;
        const std::string stem = lower ? "低" : "高";
        const auto raised_head = "紙面が台より約 " + Q(height, Unit::mm) + " " + stem;
        const auto motion = context.output_frame.baseline_equivalent_motion_mm;
        double amount = std::hypot(motion.x, motion.y);
        if (!(amount > 0) || !std::isfinite(amount)) amount = std::abs(context.surface.baseline_equivalent_mm);
        std::string where = InDrawing(motion);
        const auto word = DirectionWord(motion);
        if (!where.empty() && std::abs(motion.x) > 1e-12 && word != "上" && word != "下")
            where += motion.x > 0 ? "（A から離れる向き）" : "（A に近づく向き）";
        const auto moved_text = where.empty() ? "B が左右に約 " + Q(amount, Unit::mm) + " ずれています"
                                              : "B が" + where + "へ約 " + Q(amount, Unit::mm) + " ずれています";
        if (both)
            return raised_head + "いか、" + moved_text + "（撮影 1 組からは区別できません）" + (lower ? "" : "。冊子など厚い物なら前者です");
        if (raised) return raised_head + "くなっています" + (lower ? "" : "。冊子など厚い物はこの状態になります");
        return moved_text;
    }
    case C::surface_tilt: {
        const auto word = DirectionWord(context.output_frame.surface_high_side);
        const auto amount = Approx(warning.value, unit);
        return word.empty() ? "紙面が" + amount + " 傾いています。冊子ののどの近くでは寸法も変わります"
                            : "紙面の" + word + "側が高く、" + amount + " 傾いています。冊子ののどの近くでは寸法も変わります";
    }
    case C::output_edge_uncovered:
        if (warning.unit == ex::WarningUnit::pixels)
            return "位置合わせの結果、出力の外周に、どちらのカメラも写していない所が約 " + Fixed(std::abs(warning.value), 0) + " 画素分あります";
        return "位置合わせの結果、出力の外周に、どちらのカメラも写していない所が幅 " + Approx(warning.value, unit) + " ほどあります";
    case C::seam_lift: {
        const auto where = warning.rows_mm ? "継ぎ目の上から" + rows_mm_text() + " の所で、" : std::string("継ぎ目の一部で、");
        const auto height = context.parallax_summary && std::isfinite(context.parallax_summary->max_shift_mm)
            ? "紙が約 " + Q(std::abs(context.parallax_summary->max_shift_mm), Unit::mm) + " 浮いています" : std::string("紙が浮いています");
        return where + height + (warning.applied ? "。補正しました" : "。補正していません") + "（最大 " + Q(std::abs(warning.value), unit) + "）";
    }
    case C::seam_lift_limit: {
        const auto where = warning.rows_mm ? "継ぎ目の上から" + rows_mm_text() + " の所の" : std::string{};
        const auto lift = where + "紙の浮き（約 " + Q(std::abs(warning.value), unit) + "）";
        if (warning.limit && !Exceeds(warning)) return lift + "は線の範囲内です";
        std::string tail;
        if (warning.remedy == ex::Remedy::shift_drawing_horizontal) tail = "。図面を左右に" + amount_cm() + "ずらして置き直すと、継ぎ目から離せることがあります";
        else if (warning.remedy == ex::Remedy::place_drawing_again) tail = "。図面を置き直すと、継ぎ目から離せることがあります";
        return lift + "が線を超えています。補正しきれない分が残っています" + tail;
    }
    case C::fold_near_seam: {
        const auto distance = ToMm(warning.value, warning.unit, context.pitch_mm);
        std::string detail;
        if (warning.rows_mm) detail = "上から" + first_row_mm_text();
        if (distance) {
            detail += detail.empty() ? "" : "、";
            detail += *distance == 0 ? std::string("継ぎ目の上") : std::string(*distance > 0 ? "継ぎ目から右へ" : "継ぎ目から左へ") + "約 " + Q(std::abs(*distance), Unit::mm);
        }
        return "継ぎ目の近くに折り目があります" + (detail.empty() ? std::string{} : "（" + detail + "）") +
            "。図面を左右に" + amount_cm() + "ずらして置き直してください";
    }
    case C::gutter_in_band: {
        const auto distance = ToMm(warning.value, warning.unit, context.pitch_mm);
        std::string detail;
        if (distance)
            detail = *distance == 0 ? std::string("継ぎ目の上") : std::string(*distance > 0 ? "継ぎ目から右へ" : "継ぎ目から左へ") + "約 " + Q(std::abs(*distance), Unit::mm);
        const auto shift = PositiveAmount(warning.remedy_amount_mm) ? "図面を左右に" + amount_cm() + "ずらして、" : std::string{};
        return "綴じ目（のど）が重なりの帯にあります" + (detail.empty() ? std::string{} : "（" + detail + "）") +
            "。" + shift + "継ぎ目から離すように置き直してください";
    }
    case C::seam_displacement: {
        const auto where = warning.rows_mm ? "（上から" + first_row_mm_text() + " の所）" : std::string{};
        if (!warning.limit) return "継ぎ目のずれが最大 " + Q(std::abs(warning.value), Unit::px) + (where.empty() ? " " : where) + "です";
        const auto shown = AgainstLimit(warning.value, *warning.limit, Unit::px);
        return "継ぎ目のずれが最大 " + shown.first + (where.empty() ? " " : where) + "で、基準の " + shown.second + (Exceeds(warning) ? " を超えています" : " 以内です");
    }
    case C::seam_dx_unmeasured:
        return (warning.rows_mm ? "継ぎ目の上から" + rows_mm_text() + " で、" : std::string("継ぎ目の一部で、")) +
            "左右のずれを測れていません（横の線しかありません）";
    case C::seam_unmeasurable:
        return warning.rows_mm ? "継ぎ目の上から" + rows_mm_text() + " の辺りに、ずれを測れない所があります（模様が足りないか、測定が安定しません）"
                               : std::string("継ぎ目に、ずれを測れない所があります（模様が足りないか、測定が安定しません）");
    case C::placement_unverified: return "図面の配置を確かめられませんでした（目印になる模様が足りません）";
    case C::placement_mismatch: return "図面の配置が、周りの模様から見た位置と食い違っています";
    case C::gate_configuration: return "継ぎ目のずれを確かめる設定に問題があり、判定できませんでした";
    case C::gate_not_run:
        if (Has(warning.interpretations, "gate-failed")) return "継ぎ目のずれの測定が途中で失敗したため、ずれを判定できませんでした";
        return "時間が足りず、継ぎ目のずれの測定を行っていません";
    case C::seam_wide_search: {
        const auto px = ToPx(warning.value, warning.unit, context.pitch_mm);
        const auto mm = ToMm(warning.value, warning.unit, context.pitch_mm);
        if (px && mm)
            return "継ぎ目で左右に約 " + Q(std::abs(*px), Unit::px) + "（約 " + Q(std::abs(*mm), Unit::mm) + "）ずれています（広い探索による目安）";
        return "継ぎ目で左右に約 " + Q(std::abs(warning.value), unit) + " ずれています（広い探索による目安）";
    }
    case C::geometry_level: {
        // A stage that threw is why the level is lower.
        std::string why;
        if (Has(warning.interpretations, "estimate-failed")) why = "（B の姿勢の推定が途中で失敗しました）";
        else if (Has(warning.interpretations, "layer-two-failed")) why = "（紙の浮きの補正が途中で失敗しました）";
        if (context.geometry_level) return GeometryLevelText(*context.geometry_level) + why;
        return "位置合わせは、推定が確かでない部分を当てない段で行いました" + why;
    }
    case C::time_budget: return "時間が足りなかったため、一部の処理を省きました";
    }
    return "確認が必要な項目があります";
}

// The label of a warning's class (基準を超えた／確かめられなかった／お知らせ).
[[nodiscard]] inline std::string WarningClassLabel(a0::m2::experimental::WarningClass warning_class) {
    switch (warning_class) {
    case a0::m2::experimental::WarningClass::exceeded: return "基準外";
    case a0::m2::experimental::WarningClass::unverified: return "確かめられていません";
    case a0::m2::experimental::WarningClass::notice: return "お知らせ";
    }
    return "お知らせ";
}
// The class a warning is shown with: an exceeded warning whose value is not greater than its limit is only a notice.
[[nodiscard]] inline a0::m2::experimental::WarningClass EffectiveWarningClass(const a0::m2::experimental::RegistrationWarning& warning) {
    using K = a0::m2::experimental::WarningClass;
    if (warning.warning_class == K::exceeded && warning.limit && !(warning.value > *warning.limit)) return K::notice;
    return warning.warning_class;
}

// How to put a warning right, for the line under it: the direction to move the camera for a pose component, otherwise the
// sentence of its code of how. Empty when nothing is needed, or when the warning's own sentence already says it
// (fold-near-seam, gutter-in-band, and seam-lift-limit with its excess).
[[nodiscard]] inline std::string RegistrationWarningGuidance(const a0::m2::experimental::RegistrationWarning& warning,
    const RegistrationTextContext& context) {
    using namespace registration_text_detail;
    using C = ex::WarningCode;
    using R = ex::Remedy;
    if (warning.code == C::fold_near_seam || warning.code == C::gutter_in_band) return {};
    if (warning.code == C::seam_lift_limit && Exceeds(warning) &&
        (warning.remedy == R::shift_drawing_horizontal || warning.remedy == R::place_drawing_again)) return {};
    if (auto fix = RegistrationPoseFixText(warning, context); !fix.empty()) return fix;
    if (warning.remedy == R::none) return {};
    return RemedyGuidanceText(warning.remedy, warning.remedy_amount_mm);
}
// The label, the sentence and how to put it right, in one line: "【基準外】B が図面の上へ約 3 mm ずれています（許容 10 mm）。…".
[[nodiscard]] inline std::string RegistrationWarningLine(const a0::m2::experimental::RegistrationWarning& warning,
    const RegistrationTextContext& context) {
    auto text = "【" + WarningClassLabel(EffectiveWarningClass(warning)) + "】" + RegistrationWarningText(warning, context);
    if (const auto guidance = RegistrationWarningGuidance(warning, context); !guidance.empty()) text += "。" + guidance;
    return text;
}

// ---- The estimated pose as a line (the owner's request in #417: a level in place of the unusable one) ----
// "B の傾き 0.4°（許容 1°）、回転 0.8°（許容 2°）、高さ +3 mm（許容 ±10 mm）、位置 2 mm（許容 5 mm）". The sizes are those against
// the accepted setup (the components' values) and are not signed except the height; when a pose could not be estimated the
// sentence says so with the first reason in plain words. Components that exceeded are named after the line.
[[nodiscard]] inline std::string RegistrationPoseSummaryText(const a0::m2::experimental::content_registration::RegistrationResult& result,
    const RegistrationTextContext& context) {
    using namespace registration_text_detail;
    namespace cr = a0::m2::experimental::content_registration;
    const bool usable = result.has_estimate && (result.state == cr::RegistrationState::verified || result.state == cr::RegistrationState::out_of_envelope);
    if (!usable) {
        const std::string reason = result.reasons.empty() ? std::string{} : result.reasons.front();
        std::string why;
        if (reason == "no-overlap") why = "二台の重なりが足りません";
        else if (reason == "coarse-no-unique-blocks" || reason == "unique-blocks") why = "位置を決める手がかりになる模様が足りません";
        else if (reason == "coarse-inconsistent") why = "大まかな位置が一致しませんでした";
        else if (reason == "too-few-points") why = "測れた点が足りません";
        else if (reason == "fit-failed" || reason == "not-converged") why = "推定が収束しませんでした";
        else if (reason == "brown-lowe") why = "一致した点が少なく、推定が確かではありません";
        else if (reason == "prediction-uncertain") why = "継ぎ目の位置の推定が不確かでした";
        else if (reason == "residual-shape") why = "残りのずれの形が偏っていて、推定が確かではありません";
        else if (reason == "starts-disagree") why = "別の出発点から違う結果になり、推定が確かではありません";
        else if (reason == "time-budget") why = "時間が足りませんでした";
        else if (reason == "estimate-failed") why = "推定の処理が途中で失敗しました";
        else why = "推定が確かではありません";
        return result.has_estimate ? "B の姿勢の推定は確かではありません（理由: " + why + "）" : "B の姿勢を推定できませんでした（理由: " + why + "）";
    }
    const auto find = [&](cr::EnvelopeComponent component) -> const cr::ComponentCheck* {
        for (const auto& c : result.components) if (c.component == component) return &c;
        return nullptr;
    };
    std::vector<std::string> parts;
    std::vector<std::string> exceeded;
    const auto note = [&](const cr::ComponentCheck& c, const char* name) {
        if (c.value > c.limit) exceeded.emplace_back(name);
    };
    if (const auto* c = find(cr::EnvelopeComponent::tilt)) {
        parts.push_back("傾き " + Q(c->value, Unit::deg) + "（許容 " + Q(c->limit, Unit::deg) + "）");
        note(*c, "傾き");
    }
    if (const auto* c = find(cr::EnvelopeComponent::rotation)) {
        parts.push_back("回転 " + Q(c->value, Unit::deg) + "（許容 " + Q(c->limit, Unit::deg) + "）");
        note(*c, "回転");
    }
    if (const auto* c = find(cr::EnvelopeComponent::height)) {
        const auto per_percent = MmPerPercent(context);
        std::string sign;
        if (context.pose_display && std::isfinite(context.pose_display->height_percent)) {
            const double shown = per_percent ? c->value * *per_percent : c->value;
            const bool zero = Fixed(shown, per_percent ? Decimals(Unit::mm) : Decimals(Unit::percent)) == "0";
            if (!zero) sign = context.pose_display->height_percent > 0 ? "+" : context.pose_display->height_percent < 0 ? "-" : "";
        }
        if (per_percent)
            parts.push_back("高さ " + sign + Q(c->value * *per_percent, Unit::mm) + "（許容 ±" + Q(c->limit * *per_percent, Unit::mm) + "）");
        else
            parts.push_back("高さ " + sign + Q(c->value, Unit::percent) + "（許容 ±" + Q(c->limit, Unit::percent) + "）");
        note(*c, "高さ");
    }
    if (const auto* c = find(cr::EnvelopeComponent::position)) {
        parts.push_back("位置 " + Q(c->value, Unit::mm) + "（許容 " + Q(c->limit, Unit::mm) + "）");
        note(*c, "位置");
    }
    if (parts.empty()) return "B の姿勢を推定しましたが、許容と比べる値がありません";
    std::string text = "B の" + parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i) text += "、" + parts[i];
    if (!exceeded.empty()) {
        text += "。許容を超えている項目: ";
        for (std::size_t i = 0; i < exceeded.size(); ++i) text += (i ? "、" : "") + exceeded[i];
    }
    return text;
}

// ---- The camera's direction, for the screen before the shot (the owner's request in #417) ----
// B is shown as the value against A now, A as the value at calibration (what moved A after calibration is not seen from the
// picture). The values are those of the report / the live estimate; nothing is computed here.
struct CameraOrientation {
    double rotation_clockwise_deg{};                 // signed; positive is clockwise as seen on the screen
    a0::m2::render::Point lens_direction{};          // unit vector: the way the lens faces over the drawing; 0 for no tilt
    double lens_tilt_deg{};
};
[[nodiscard]] inline CameraOrientation CameraOrientationOf(const a0::m2::experimental::OutputFrameDisplay& display) {
    return {display.rotation_clockwise_deg, display.lens_direction, display.lens_tilt_deg};
}
enum class OrientationSubject {camera_a_at_calibration, camera_b_against_a};

// One short line: "B: 右回りに 0.8°、図面の右上へ 0.4° 傾き", "A（校正時）: 回転なし、傾きなし". An absent value reads "向きを測れていません".
[[nodiscard]] inline std::string CameraOrientationShortText(OrientationSubject subject, const std::optional<CameraOrientation>& orientation) {
    using namespace registration_text_detail;
    const std::string label = subject == OrientationSubject::camera_a_at_calibration ? "A（校正時）: " : "B: ";
    if (!orientation || !std::isfinite(orientation->rotation_clockwise_deg) || !std::isfinite(orientation->lens_tilt_deg))
        return label + "向きを測れていません";
    std::string rotation = "回転なし";
    if (Fixed(std::abs(orientation->rotation_clockwise_deg), Decimals(Unit::deg)) != "0")
        rotation = std::string(orientation->rotation_clockwise_deg > 0 ? "右回りに " : "左回りに ") + Q(std::abs(orientation->rotation_clockwise_deg), Unit::deg);
    std::string tilt = "傾きなし";
    if (Fixed(std::abs(orientation->lens_tilt_deg), Decimals(Unit::deg)) != "0") {
        const auto where = InDrawing(orientation->lens_direction);
        tilt = (where.empty() ? std::string{} : where + "へ ") + Q(std::abs(orientation->lens_tilt_deg), Unit::deg) + " 傾き";
    }
    return label + rotation + "、" + tilt;
}
// The detailed form (LF lines): what the camera does and the way to put it right.
[[nodiscard]] inline std::string CameraOrientationDetailText(OrientationSubject subject, const std::optional<CameraOrientation>& orientation) {
    using namespace registration_text_detail;
    const bool a = subject == OrientationSubject::camera_a_at_calibration;
    std::string text = a ? "A のカメラの向き（校正の時の値）" : "B のカメラの向き（A に対する今の値）";
    if (!orientation || !std::isfinite(orientation->rotation_clockwise_deg) || !std::isfinite(orientation->lens_tilt_deg))
        return text + "\n向きを測れていません";
    text += "\n回転: ";
    if (Fixed(std::abs(orientation->rotation_clockwise_deg), Decimals(Unit::deg)) == "0") {
        text += "ありません";
    } else {
        const bool clockwise = orientation->rotation_clockwise_deg > 0;
        const auto amount = Q(std::abs(orientation->rotation_clockwise_deg), Unit::deg);
        text += std::string("画面で見て") + (clockwise ? "時計回り" : "反時計回り") + "に " + amount + " 回っています。直すには、画面で見て" +
            (clockwise ? "反時計回り" : "時計回り") + "に " + amount + " 回してください";
    }
    text += "\n傾き: ";
    if (Fixed(std::abs(orientation->lens_tilt_deg), Decimals(Unit::deg)) == "0") {
        text += "ありません";
    } else {
        const auto amount = Q(std::abs(orientation->lens_tilt_deg), Unit::deg);
        const auto where = InDrawing(orientation->lens_direction);
        const auto back = InDrawing(Opposite(orientation->lens_direction));
        if (where.empty()) text += "レンズが " + amount + " 傾いています";
        else text += "レンズが" + where + "へ " + amount + " 傾いています。直すには、レンズを" + back + "へ " + amount + " 向け直してください";
    }
    if (a) text += "\n校正の後にカメラが動いた分は、映像からは分かりません";
    return text;
}

// ---- The gate (11.6, 11.22) ----
namespace registration_text_detail {
inline std::optional<double> GateMaximum(const ex::registration_gate::RegistrationGateResult& gate) {
    if (gate.after.maximum_px && std::isfinite(*gate.after.maximum_px)) return gate.after.maximum_px;
    if (gate.seam_gate && gate.seam_gate->maximum_px && std::isfinite(*gate.seam_gate->maximum_px)) return gate.seam_gate->maximum_px;
    std::optional<double> best;
    for (const auto& v : {gate.maxima.two_directional_px, gate.maxima.one_directional_px})
        if (v && std::isfinite(*v) && (!best || *v > *best)) best = v;
    return best;
}
inline std::string GateFindingText(const std::string& reason) {
    if (reason == "seam-misalignment") return "継ぎ目のずれが基準を超えています";
    if (reason == "predicted-misalignment") return "中身の無い所の予測が、ずれの基準を超えています";
    if (reason == "unmeasurable-content" || reason == "no-measurable-block" || reason == "unpredicted-empty-block") return "ずれを測れない所があります";
    if (reason == "global-placement-mismatch") return "図面の配置が、周りの模様から見た位置と食い違っています";
    if (reason == "global-placement-unverified") return "図面の配置を確かめられませんでした";
    if (reason == "transition-outside-band") return "継ぎ目の確認の設定に問題があります";
    return {};
}
// The reasons of the gate for the registered composite itself (registration_gate.hpp GateFinding, design 11.30). They come
// before the seam gate's own reasons, which the gate may have decided again.
inline std::string OwnGateFindingText(const std::string& reason) {
    if (reason == "unconfirmed-content" || reason == "one-sided-content" || reason == "nothing-measured" || reason == "content-unmeasured")
        return "ずれを測れない所があります";
    if (reason == "dx-unmeasured") return "左右のずれを測れた行がありません（横の線しかありません）";
    if (reason == "placement-unverified") return "図面の配置を確かめられませんでした";
    if (reason == "transition-outside-band") return "継ぎ目の確認の設定に問題があります";
    return {};
}
inline bool DxUnmeasured(const ex::registration_gate::RegistrationGateResult& gate) {
    using S = ex::registration_gate::GateState;
    if (gate.state == S::not_run) return false;
    return !gate.rows.unmeasured.empty() || (std::isfinite(gate.rows.dx_measured_share) && gate.rows.dx_measured_share < 1.0);
}
}   // namespace registration_text_detail

// The gate's state with the maximum split by kind of point; with no two-directional point the text says that only one
// direction was measured and the left-right displacement was not; with rows where the left-right displacement was not
// measured it says so. The names follow docs/M2_SEAM_QUALITY_GATE.md section 9 and never use the word for "passed".
[[nodiscard]] inline std::string RegistrationGateStateText(const a0::m2::experimental::registration_gate::RegistrationGateResult& gate) {
    using namespace registration_text_detail;
    using S = ex::registration_gate::GateState;
    using Cause = ex::registration_gate::GateCause;
    const auto maximum = GateMaximum(gate);
    std::string text;
    switch (gate.state) {
    case S::pass:
        text = "継ぎ目のズレ: 基準内" + (maximum ? "（最大 " + Q(*maximum, Unit::px) + "）" : std::string{});
        break;
    case S::fail:
        if (gate.cause == Cause::unmeasurable) {
            text = "継ぎ目のズレを測れない所があります";
        } else {
            const std::optional<double> line = gate.seam_gate && std::isfinite(gate.seam_gate->limits.limit_px) ? std::optional<double>(gate.seam_gate->limits.limit_px) : std::nullopt;
            text = "継ぎ目のズレが基準を超えています";
            if (maximum && line) {
                if (*maximum > *line) {
                    const auto shown = AgainstLimit(*maximum, *line, Unit::px);
                    text += "（最大 " + shown.first + "、基準 " + shown.second + "）";
                } else if (std::isfinite(gate.seam_gate->limits.guard_band_px) && *maximum + gate.seam_gate->limits.guard_band_px > *line) {
                    text += "（最大 " + Q(*maximum, Unit::px) + "。測定の不確かさを含めると基準 " + Q(*line, Unit::px) + " を超えます）";
                } else {
                    text += "（最大 " + Q(*maximum, Unit::px) + "）";
                }
            } else if (maximum) {
                text += "（最大 " + Q(*maximum, Unit::px) + "）";
            }
        }
        break;
    case S::undetermined: {
        text = "継ぎ目のズレを判定できませんでした";
        std::string why;
        for (const auto& finding : gate.gate_findings) if (why.empty()) why = OwnGateFindingText(finding.reason);
        for (const auto& finding : gate.findings) if (why.empty() && gate.gate_findings.empty()) why = GateFindingText(finding.reason);
        if (!why.empty()) text += "。" + why;
        break;
    }
    case S::not_run: text = "継ぎ目のズレを判定できませんでした（測定を行っていません）"; break;
    }
    if (gate.state != S::not_run) {
        const auto& m = gate.maxima;
        const bool two = m.two_directional_px && std::isfinite(*m.two_directional_px);
        const bool one = m.one_directional_px && std::isfinite(*m.one_directional_px);
        if (two && one) {
            const auto axis = m.one_directional_normal ? AxisName(*m.one_directional_normal) : std::string{};
            text += "。二方向の点の最大 " + Q(*m.two_directional_px, Unit::px) + "、一方向の点の最大 " + Q(*m.one_directional_px, Unit::px) +
                (axis.empty() ? std::string{} : "（" + axis + "）");
        } else if (two) {
            text += "。二方向の点の最大 " + Q(*m.two_directional_px, Unit::px);
        } else if (one) {
            const auto axis = m.one_directional_normal ? AxisName(*m.one_directional_normal) : std::string{};
            text += "。一方向の点だけ（左右のずれは測れていません）";
            if (!axis.empty()) text += "。測れたのは" + axis + "です";
        }
        if (DxUnmeasured(gate)) {
            text += "。左右のずれは未測定の行があります";
            if (std::isfinite(gate.rows.dx_measured_share) && gate.rows.dx_measured_share >= 0 && gate.rows.dx_measured_share <= 1)
                text += "（測れた行は " + Fixed(gate.rows.dx_measured_share * 100.0, 0) + " %）";
        }
    }
    return text;
}
// How to put it right after the gate, by the state and the cause (11.22.3). A paper that stands up or a high surface never
// sends the person to calibrate again.
[[nodiscard]] inline std::string RegistrationGateGuidanceText(const a0::m2::experimental::registration_gate::RegistrationGateResult& gate) {
    using namespace registration_text_detail;
    using S = ex::registration_gate::GateState;
    using Cause = ex::registration_gate::GateCause;
    switch (gate.state) {
    case S::pass:
        // Rows that measured lines along the baseline only are information (design 11.30): the visible displacement there was
        // measured, so a pass asks for nothing.
        return "特に対応は要りません";
    case S::not_run: return "合成画像を目で見て、継ぎ目のずれを確かめてください";
    case S::undetermined:
        for (const auto& finding : gate.findings) {
            if (finding.reason == "transition-outside-band") return RemedyGuidanceText(ex::Remedy::contact_technician);
            if (finding.reason == "global-placement-mismatch") return RemedyGuidanceText(ex::Remedy::retake);
        }
        return "合成画像を目で見て、継ぎ目のずれを確かめてください";
    case S::fail:
        switch (gate.cause) {
        case Cause::lift:
            return "紙の浮きか、紙面が高いことが原因と考えられます。図面を置き直して撮り直すか、継ぎ目から離れるように図面を左右にずらして置いてください";
        case Cause::pose: return RemedyGuidanceText(ex::Remedy::retake_then_recalibrate);
        case Cause::displacement: return RemedyGuidanceText(ex::Remedy::retake);
        case Cause::unmeasurable: return "図面を左右にずらして撮り直すか、合成画像を目で見て確かめてください";
        case Cause::none: return "原因を特定できませんでした。合成画像を目で見て確かめてください";
        }
        break;
    }
    return "合成画像を目で見て確かめてください";
}

// The assessment as a heading for the list of warnings (not "合格").
[[nodiscard]] inline std::string RegistrationAssessmentText(a0::m2::experimental::AssessmentState state) {
    using A = a0::m2::experimental::AssessmentState;
    switch (state) {
    case A::within_criteria: return "位置合わせの確認: 基準内";
    case A::outside_criteria: return "位置合わせの確認: 基準外の項目があります";
    case A::not_judged: return "位置合わせの確認: 判定できなかった項目があります";
    }
    return "位置合わせの確認: 判定できませんでした";
}

// ---- The time budget (11.25) ----
// The stage a time budget dropped, in plain words.
[[nodiscard]] inline std::string ReviewStageName(a0::m2::experimental::ReviewStage stage) {
    using S = a0::m2::experimental::ReviewStage;
    switch (stage) {
    case S::wide_search_diagnostic: return "広い範囲での探し直し";
    case S::band_measurement: return "帯全体の測定";
    case S::parallax: return "紙の浮きの補正";
    case S::fold_refit: return "折り目を除いた当て直し";
    case S::fine_and_fit: return "細かい位置合わせ";
    case S::coarse: return "大まかな位置合わせ";
    case S::gate: return "継ぎ目のズレの測定";
    }
    return "一部の処理";
}
[[nodiscard]] inline std::string TimeBudgetText(const a0::m2::experimental::TimeBudgetRecord& record) {
    // Only the stages dropped for the time; a stage that threw (stage-failed) is said by the warnings of the level and the gate.
    std::string names;
    for (const auto& s : record.skipped)
        if (s.reason == "time-budget") names += (names.empty() ? "" : "、") + ReviewStageName(s.stage);
    if (names.empty()) return "時間の都合で省いた処理はありません";
    return "時間が足りなかったため、次の処理を省きました: " + names;
}

// ---- A composite that could not be made, with the originals already saved ----
// Why the composite step threw after both originals were on disk. The originals are kept in every case.
enum class CompositeFailureReason {time_limit, out_of_memory, other};
[[nodiscard]] inline std::string_view CompositeFailureCode(CompositeFailureReason reason) {
    switch (reason) {
    case CompositeFailureReason::time_limit: return "time_limit";
    case CompositeFailureReason::out_of_memory: return "out_of_memory";
    case CompositeFailureReason::other: return "error";
    }
    return "error";
}
// One sentence for the status row. It names the reason in a few words and never carries the exception's own text (which can hold paths).
[[nodiscard]] inline std::string CompositeFailureText(CompositeFailureReason reason) {
    switch (reason) {
    case CompositeFailureReason::time_limit: return "撮影から180秒以内に合成が終わりませんでした";
    case CompositeFailureReason::out_of_memory: return "合成に必要なメモリを確保できませんでした";
    case CompositeFailureReason::other: return "合成の処理でエラーが起きました（詳細はログ）";
    }
    return "合成の処理でエラーが起きました（詳細はログ）";
}
}
