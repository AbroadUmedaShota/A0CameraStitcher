// The sentences about the registration (#417: docs/design/fixed-composite-registration.md 11.6, 11.7, 11.17, 11.24 and the
// owner's requests in the issue): the examples of the design as exact text, every code of a warning, of how to put it right, of
// a level and of the gate's state and cause, the words that are never used, the direction words, and that the same physical
// movement gives the same sentence for either mount orientation. Camera, file and clock free.
#include "a0/phase0/experimental/fixed_composite_registration_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
namespace ex = a0::m2::experimental;
namespace cr = a0::m2::experimental::content_registration;
namespace rg = a0::m2::experimental::registration_gate;
namespace render = a0::m2::render;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const std::string& what) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}
#define CHECK(expr) Check((expr), #expr)
void CheckText(const std::string& actual, const std::string& expected, const std::string& what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::cerr << "FAILED: " << what << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
    }
}
bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Words that no sentence uses: the word for "passed", sentences that say the composite cannot be written or saved, and
// anything that tells to press or flatten the paper (decision D-d), or to use a weight, tape or a board.
const std::array<const char*, 9> kForbidden{"押さえ", "平ら", "合格", "書き出せません", "保存できません", "重し", "テープ", "透明な板", "押し付け"};
// A code name (kebab-case), a file path, or a camera identifier must not appear in a sentence.
bool LooksLikeInternalCode(const std::string& text) {
    for (std::size_t i = 1; i + 1 < text.size(); ++i)
        if (text[i] == '-' && std::islower(static_cast<unsigned char>(text[i - 1])) && std::islower(static_cast<unsigned char>(text[i + 1]))) return true;
    for (const char* part : {"\\", "/", "CAM-", ".json", ".jpg", "rotated", "_", "nan", "inf"})
        if (Contains(text, part)) return true;
    return false;
}
void CheckSentence(const std::string& text, const std::string& what) {
    Check(!text.empty(), "not empty: " + what);
    for (const char* word : kForbidden) Check(!Contains(text, word), std::string("no '") + word + "': " + what + " -> " + text);
    Check(!LooksLikeInternalCode(text), "no internal code: " + what + " -> " + text);
}

constexpr double kHalfRoot2 = 0.70710678118654752440;

ex::RegistrationWarning W(ex::WarningCode code, ex::WarningClass cls, double value, std::optional<double> limit, ex::WarningUnit unit,
    ex::Remedy remedy = ex::Remedy::none) {
    ex::RegistrationWarning w;
    w.code = code;
    w.warning_class = cls;
    w.value = value;
    w.limit = limit;
    w.unit = unit;
    w.remedy = remedy;
    return w;
}
using Code = ex::WarningCode;
using Cls = ex::WarningClass;
using Unit = ex::WarningUnit;
using Rem = ex::Remedy;

RegistrationTextContext RichContext() {
    RegistrationTextContext c;
    c.output_frame.camera_motion_mm = {0, -3};
    c.output_frame.lens_direction = {kHalfRoot2, -kHalfRoot2};
    c.output_frame.lens_tilt_deg = 1.2;
    c.output_frame.rotation_clockwise_deg = 2.5;
    c.output_frame.surface_high_side = {1, 0};
    c.output_frame.baseline_equivalent_motion_mm = {6, 0};
    c.surface.state = ex::SurfaceState::estimated;
    c.surface.plane.h0_mm = 4;
    c.surface.baseline_equivalent_mm = 6;
    c.pitch_mm = 0.5;
    cr::PoseDisplay display;
    display.height_percent = 1.5;
    display.height_mm = 15;
    c.pose_display = display;
    c.pivot_depth_mm = 1000;
    ex::parallax_correction::ParallaxSummary summary;
    summary.max_shift_mm = 0.8;
    c.parallax_summary = summary;
    return c;
}

// The pose in the drawing's frame for a camera whose own x and y axes point the way `mount` says: the raw vectors are
// those the camera sees, the display is what the estimator would give for them. A 180-degree mount turns every vector over
// (the camera's right is the drawing's left); a turn about the axis seen on the screen keeps its sense.
ex::OutputFrameDisplay DisplayFor(bool rotated_180, render::Point raw_motion, double raw_rotation_clockwise, render::Point raw_lens_direction, double tilt) {
    const double s = rotated_180 ? -1.0 : 1.0;
    ex::OutputFrameDisplay d;
    d.camera_motion_mm = {s * raw_motion.x, s * raw_motion.y};
    d.rotation_clockwise_deg = raw_rotation_clockwise;
    d.lens_direction = {s * raw_lens_direction.x, s * raw_lens_direction.y};
    d.lens_tilt_deg = tilt;
    return d;
}
}   // namespace

int main() {
    // ---- the direction words (11.24 item 8): eight, from the drawing as seen on the screen ----
    {
        struct Case {double x, y; const char* word;};
        const Case cases[] = {{1, 0, "右"}, {1, 1, "右下"}, {0, 1, "下"}, {-1, 1, "左下"}, {-1, 0, "左"}, {-1, -1, "左上"}, {0, -1, "上"}, {1, -1, "右上"},
            {1, 0.3, "右"}, {1, 0.5, "右下"}, {0.3, -1, "上"}, {-0.5, -1, "左上"}};
        for (const auto& c : cases) CheckText(DrawingDirectionWord({c.x, c.y}), c.word, std::string("direction ") + c.word);
        CHECK(DrawingDirectionWord({0, 0}).empty());
        CHECK(DrawingDirectionWord({std::nan(""), 1}).empty());
        // y grows downward in the drawing: a negative y is above.
        CHECK(DrawingDirectionWord({0, -3}) == "上");
        CHECK(DrawingDirectionWord({0, 3}) == "下");
    }

    // ---- the design's examples, exact (11.7, 11.24) ----
    const auto rich = RichContext();
    CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm, Rem::check_camera_mount), rich),
        "B が図面の上へ約 3 mm ずれています（許容 10 mm）", "camera-position");
    CheckText(RegistrationWarningText(W(Code::camera_rotation, Cls::exceeded, 2.5, 2, Unit::deg, Rem::check_camera_mount), rich),
        "B が画面で見て時計回りに約 2.5° 回っています（許容 2°）", "camera-rotation");
    CheckText(RegistrationWarningText(W(Code::camera_tilt, Cls::exceeded, 1.2, 1, Unit::deg, Rem::check_camera_mount), rich),
        "B のレンズが図面の右上へ約 1.2° 傾いています（許容 1°）", "camera-tilt");
    CheckText(RegistrationWarningText(W(Code::camera_height, Cls::exceeded, 1.5, 1, Unit::percent, Rem::check_camera_mount_or_focus), rich),
        "B が受け入れた設置の時より約 15 mm 高い位置にあります（許容 10 mm）。ピントが合わせ直された可能性もあります", "camera-height");
    {
        auto w = W(Code::surface_height, Cls::exceeded, 4, 3, Unit::mm, Rem::place_drawing_again);
        w.interpretations = {"surface-raised", "camera-moved-along-baseline"};
        CheckText(RegistrationWarningText(w, rich),
            "紙面が台より約 4 mm 高いか、B が図面の右（A から離れる向き）へ約 6 mm ずれています（撮影 1 組からは区別できません）。冊子など厚い物なら前者です",
            "surface-height both readings");
        w.interpretations = {};
        CheckText(RegistrationWarningText(w, rich),
            "紙面が台より約 4 mm 高いか、B が図面の右（A から離れる向き）へ約 6 mm ずれています（撮影 1 組からは区別できません）。冊子など厚い物なら前者です",
            "surface-height: no reading named gives both");
        w.interpretations = {"surface-raised"};
        CheckText(RegistrationWarningText(w, rich), "紙面が台より約 4 mm 高くなっています。冊子など厚い物はこの状態になります", "surface-height raised only");
        w.interpretations = {"camera-moved-along-baseline"};
        CheckText(RegistrationWarningText(w, rich), "B が図面の右（A から離れる向き）へ約 6 mm ずれています", "surface-height moved only");
        auto left = rich;
        left.output_frame.baseline_equivalent_motion_mm = {-6, 0};
        w.interpretations = {"camera-moved-along-baseline"};
        CheckText(RegistrationWarningText(w, left), "B が図面の左（A に近づく向き）へ約 6 mm ずれています", "surface-height moved to the left");
        auto low = rich;
        low.surface.plane.h0_mm = -4;
        w.interpretations = {};
        Check(Contains(RegistrationWarningText(w, low), "紙面が台より約 4 mm 低いか"), "surface-height lower than the table");
        Check(!Contains(RegistrationWarningText(w, low), "冊子"), "no thick-book remark for a lower surface");
    }
    CheckText(RegistrationWarningText(W(Code::surface_tilt, Cls::exceeded, 6, 5, Unit::deg, Rem::place_drawing_again), rich),
        "紙面の右側が高く、約 6° 傾いています。冊子ののどの近くでは寸法も変わります", "surface-tilt");
    {
        auto c = rich;
        c.output_frame.surface_high_side = {-kHalfRoot2, -kHalfRoot2};
        CheckText(RegistrationWarningText(W(Code::surface_tilt, Cls::exceeded, 6, 5, Unit::deg), c),
            "紙面の左上側が高く、約 6° 傾いています。冊子ののどの近くでは寸法も変わります", "surface-tilt, upper left");
    }
    {
        auto w = W(Code::seam_lift, Cls::notice, 1.4, 1, Unit::px);
        w.rows_mm = ex::RowSpan{120, 180};
        w.applied = true;
        CheckText(RegistrationWarningText(w, rich), "継ぎ目の上から約 120〜180 mm の所で、紙が約 0.8 mm 浮いています。補正しました（最大 1.4 px）", "seam-lift");
        w.applied = false;
        Check(Contains(RegistrationWarningText(w, rich), "補正していません"), "seam-lift not applied says so");
    }
    {
        auto w = W(Code::seam_lift_limit, Cls::exceeded, 1.4, 1, Unit::mm, Rem::shift_drawing_horizontal);
        w.rows_mm = ex::RowSpan{200, 200};
        w.remedy_amount_mm = 50;
        CheckText(RegistrationWarningText(w, rich),
            "継ぎ目の上から約 200 mm の所の紙の浮き（約 1.4 mm）が線を超えています。補正しきれない分が残っています。図面を左右に約 5 cm ずらして置き直すと、継ぎ目から離せることがあります",
            "seam-lift-limit shift");
        w.remedy = Rem::place_drawing_again;
        w.remedy_amount_mm.reset();
        CheckText(RegistrationWarningText(w, rich),
            "継ぎ目の上から約 200 mm の所の紙の浮き（約 1.4 mm）が線を超えています。補正しきれない分が残っています。図面を置き直すと、継ぎ目から離せることがあります",
            "seam-lift-limit place again");
    }
    {
        auto w = W(Code::fold_near_seam, Cls::notice, 40, std::nullopt, Unit::mm, Rem::shift_drawing_horizontal);
        w.rows_mm = ex::RowSpan{300, 300};
        w.remedy_amount_mm = 80;
        CheckText(RegistrationWarningText(w, rich), "継ぎ目の近くに折り目があります（上から約 300 mm、継ぎ目から右へ約 40 mm）。図面を左右に約 8 cm ずらして置き直してください",
            "fold-near-seam right");
        w.value = -40;
        CheckText(RegistrationWarningText(w, rich), "継ぎ目の近くに折り目があります（上から約 300 mm、継ぎ目から左へ約 40 mm）。図面を左右に約 8 cm ずらして置き直してください",
            "fold-near-seam left");
    }
    CheckText(RegistrationWarningText(W(Code::gutter_in_band, Cls::notice, 25, std::nullopt, Unit::mm, Rem::shift_drawing_horizontal), rich),
        "綴じ目（のど）が重なりの帯にあります（継ぎ目から右へ約 25 mm）。継ぎ目から離すように置き直してください", "gutter-in-band");
    {
        auto w = W(Code::seam_displacement, Cls::exceeded, 2.5, 1, Unit::px, Rem::retake);
        w.rows_mm = ex::RowSpan{150, 150};
        CheckText(RegistrationWarningText(w, rich), "継ぎ目のずれが最大 2.5 px（上から約 150 mm の所）で、基準の 1 px を超えています", "seam-displacement");
    }
    {
        auto w = W(Code::seam_dx_unmeasured, Cls::unverified, 0.6, std::nullopt, Unit::percent, Rem::review_visually);
        w.rows_mm = ex::RowSpan{100, 140};
        CheckText(RegistrationWarningText(w, rich), "継ぎ目の上から約 100〜140 mm で、左右のずれを測れていません（横の線しかありません）", "seam-dx-unmeasured");
    }
    CheckText(RegistrationWarningText(W(Code::seam_wide_search, Cls::notice, 18, std::nullopt, Unit::px), rich),
        "継ぎ目で左右に約 18 px（約 9 mm）ずれています（広い探索による目安）", "seam-wide-search");
    CheckText(RegistrationWarningText(W(Code::placement_unverified, Cls::unverified, 0, std::nullopt, Unit::px, Rem::review_visually), rich),
        "図面の配置を確かめられませんでした（目印になる模様が足りません）", "placement-unverified");
    {
        ex::GeometryLevelDecision level;
        level.level = ex::GeometryLevel::g3;
        CheckText(GeometryLevelText(level), "位置合わせを G3（一様なずれだけ）で行いました。理由: 上の段に必要な推定が確かでないか、使えなかったため", "geometry level G3");
        level.reasons = {"seam-outside-overlap-at-level"};
        CheckText(GeometryLevelText(level), "位置合わせを G3（一様なずれだけ）で行いました。理由: 継ぎ目が重なりの外に出るため", "geometry level G3 with a reason");
        level.reasons = {"seam-outside-overlap-at-level", "output-uncovered-at-level", "output-uncovered-at-level", "a-code-nobody-knows"};
        CheckText(GeometryLevelText(level),
            "位置合わせを G3（一様なずれだけ）で行いました。理由: 継ぎ目が重なりの外に出るため、出力の端にどちらのカメラも写していない所が残るため", "reasons joined, repeated and unknown dropped");
        level.reasons = {"a-code-nobody-knows"};
        Check(!Contains(GeometryLevelText(level), "a-code"), "an unknown reason code is not printed");
        level.level = ex::GeometryLevel::g1;
        level.reasons.clear();
        CheckText(GeometryLevelText(level), "位置合わせを G1（姿勢・紙面・紙の浮きを補正）で行いました", "geometry level G1");
        level.beyond_envelope_applied = true;
        CheckText(GeometryLevelText(level), "位置合わせを G1（姿勢・紙面・紙の浮きを補正）で行いました。許容の範囲を超えた量も、推定が確かなので当てています", "geometry level G1 beyond");
        auto c = rich;
        level.beyond_envelope_applied = false;
        level.level = ex::GeometryLevel::g3;
        c.geometry_level = &level;
        CheckText(RegistrationWarningText(W(Code::geometry_level, Cls::notice, 3, std::nullopt, Unit::rows), c), GeometryLevelText(level), "geometry-level warning uses the decision");
        // A stage that threw (#417): the level's sentence says which.
        {
            auto failed = W(Code::geometry_level, Cls::notice, 0, std::nullopt, Unit::rows);
            failed.interpretations = {"estimate-failed"};
            CheckText(RegistrationWarningText(failed, c), GeometryLevelText(level) + "（B の姿勢の推定が途中で失敗しました）", "geometry level, the estimate threw");
            failed.interpretations = {"layer-two-failed"};
            CheckText(RegistrationWarningText(failed, c), GeometryLevelText(level) + "（紙の浮きの補正が途中で失敗しました）", "geometry level, layer 2 threw");
            auto gate = W(Code::gate_not_run, Cls::unverified, 0, std::nullopt, Unit::rows);
            CheckText(RegistrationWarningText(gate, c), "時間が足りず、継ぎ目のずれの測定を行っていません", "gate not run for the time");
            gate.interpretations = {"gate-failed"};
            CheckText(RegistrationWarningText(gate, c), "継ぎ目のずれの測定が途中で失敗したため、ずれを判定できませんでした", "gate not run, it threw");
        }
        for (const auto l : {ex::GeometryLevel::g1, ex::GeometryLevel::g2, ex::GeometryLevel::g3, ex::GeometryLevel::g4}) {
            level.level = l;
            for (const auto& reasons : std::vector<std::vector<std::string>>{{}, {"seam-outside-overlap-at-level"}, {"output-uncovered-at-level"}, {"x-y"}}) {
                level.reasons = reasons;
                CheckSentence(GeometryLevelText(level), "level text");
            }
            CheckSentence(GeometryLevelName(l), "level name");
        }
    }

    // ---- the pose as a line (the owner's example in the issue) ----
    {
        cr::RegistrationResult r;
        r.state = cr::RegistrationState::verified;
        r.has_estimate = true;
        r.components = {{cr::EnvelopeComponent::position, 2, 5, false}, {cr::EnvelopeComponent::rotation, 0.8, 2, false},
            {cr::EnvelopeComponent::tilt, 0.4, 1, false}, {cr::EnvelopeComponent::height, 0.3, 1, false}};
        auto c = rich;
        c.pivot_depth_mm = 1000;
        c.pose_display->height_percent = 0.3;
        CheckText(RegistrationPoseSummaryText(r, c), "B の傾き 0.4°（許容 1°）、回転 0.8°（許容 2°）、高さ +3 mm（許容 ±10 mm）、位置 2 mm（許容 5 mm）", "pose summary");
        c.pose_display->height_percent = -0.3;
        Check(Contains(RegistrationPoseSummaryText(r, c), "高さ -3 mm（許容 ±10 mm）"), "pose summary, lower");
        c.pose_display.reset();
        c.pivot_depth_mm.reset();
        CheckText(RegistrationPoseSummaryText(r, c), "B の傾き 0.4°（許容 1°）、回転 0.8°（許容 2°）、高さ 0.3 %（許容 ±1 %）、位置 2 mm（許容 5 mm）", "pose summary without the depth");
        // A value equal to its limit is within it: nothing is named as exceeded. One over is.
        r.components[0] = {cr::EnvelopeComponent::position, 5, 5, false};
        r.components[1] = {cr::EnvelopeComponent::rotation, 2, 2, false};
        Check(!Contains(RegistrationPoseSummaryText(r, c), "超えている"), "pose summary: equal to the limit is not exceeded");
        r.state = cr::RegistrationState::out_of_envelope;
        r.components[1] = {cr::EnvelopeComponent::rotation, 2.5, 2, true};
        r.components[0] = {cr::EnvelopeComponent::position, 5.5, 5, true};
        Check(Contains(RegistrationPoseSummaryText(r, c), "。許容を超えている項目: 回転、位置"), "pose summary names the exceeded");
        // Not estimated: the reason in plain words, for every reason code of the estimator and an unknown one.
        for (const char* reason : {"no-overlap", "coarse-no-unique-blocks", "coarse-inconsistent", "too-few-points", "fit-failed", "brown-lowe", "unique-blocks",
                 "prediction-uncertain", "residual-shape", "not-converged", "starts-disagree", "time-budget", "estimate-failed", "something-new"}) {
            cr::RegistrationResult u;
            u.state = cr::RegistrationState::undetermined;
            u.reasons = {reason};
            CheckSentence(RegistrationPoseSummaryText(u, c), std::string("pose summary, not estimated: ") + reason);
            u.has_estimate = true;
            CheckSentence(RegistrationPoseSummaryText(u, c), std::string("pose summary, untrusted: ") + reason);
        }
        cr::RegistrationResult none;
        CheckSentence(RegistrationPoseSummaryText(none, c), "pose summary, no reason");
    }

    // ---- the camera's direction before the shot (the owner's request) ----
    {
        const CameraOrientation b{0.8, {0, 1}, 0.4};
        CheckText(CameraOrientationShortText(OrientationSubject::camera_b_against_a, b), "B: 右回りに 0.8°、図面の下へ 0.4° 傾き", "B short");
        CheckText(CameraOrientationShortText(OrientationSubject::camera_b_against_a, CameraOrientation{-0.8, {kHalfRoot2, -kHalfRoot2}, 1.2}),
            "B: 左回りに 0.8°、図面の右上へ 1.2° 傾き", "B short, counterclockwise and upper right");
        CheckText(CameraOrientationShortText(OrientationSubject::camera_a_at_calibration, CameraOrientation{0.3, {-1, 0}, 0.2}),
            "A（校正時）: 右回りに 0.3°、図面の左へ 0.2° 傾き", "A short");
        CheckText(CameraOrientationShortText(OrientationSubject::camera_a_at_calibration, CameraOrientation{0.02, {0, 0}, 0.01}),
            "A（校正時）: 回転なし、傾きなし", "A short, nothing");
        CheckText(CameraOrientationShortText(OrientationSubject::camera_b_against_a, std::nullopt), "B: 向きを測れていません", "B short, not measured");
        CheckText(CameraOrientationShortText(OrientationSubject::camera_b_against_a, CameraOrientation{std::nan(""), {0, 1}, 0.4}), "B: 向きを測れていません", "B short, not a number");
        CheckText(CameraOrientationDetailText(OrientationSubject::camera_b_against_a, b),
            "B のカメラの向き（A に対する今の値）\n回転: 画面で見て時計回りに 0.8° 回っています。直すには、画面で見て反時計回りに 0.8° 回してください\n"
            "傾き: レンズが図面の下へ 0.4° 傾いています。直すには、レンズを図面の上へ 0.4° 向け直してください", "B detail");
        CheckText(CameraOrientationDetailText(OrientationSubject::camera_a_at_calibration, CameraOrientation{-0.3, {-1, 0}, 0.2}),
            "A のカメラの向き（校正の時の値）\n回転: 画面で見て反時計回りに 0.3° 回っています。直すには、画面で見て時計回りに 0.3° 回してください\n"
            "傾き: レンズが図面の左へ 0.2° 傾いています。直すには、レンズを図面の右へ 0.2° 向け直してください\n校正の後にカメラが動いた分は、映像からは分かりません", "A detail");
        CheckText(CameraOrientationDetailText(OrientationSubject::camera_b_against_a, CameraOrientation{}), "B のカメラの向き（A に対する今の値）\n回転: ありません\n傾き: ありません", "B detail, nothing");
        CheckSentence(CameraOrientationDetailText(OrientationSubject::camera_b_against_a, std::nullopt), "detail, not measured");
        // The report's display gives the same.
        ex::OutputFrameDisplay d;
        d.rotation_clockwise_deg = 0.8;
        d.lens_direction = {0, 1};
        d.lens_tilt_deg = 0.4;
        CheckText(CameraOrientationShortText(OrientationSubject::camera_b_against_a, CameraOrientationOf(d)), "B: 右回りに 0.8°、図面の下へ 0.4° 傾き", "from the display");
    }

    // ---- directions of the fix are the opposite of how the camera moved ----
    {
        auto c = rich;
        CheckText(RegistrationPoseFixText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), c), "B のカメラを図面の下へ約 3 mm 動かしてください", "fix position");
        c.output_frame.camera_motion_mm = {0, 3};
        CheckText(RegistrationPoseFixText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), c), "B のカメラを図面の上へ約 3 mm 動かしてください", "fix position, moved down");
        CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), c), "B が図面の下へ約 3 mm ずれています（許容 10 mm）", "position, moved down");
        c.output_frame.camera_motion_mm = {-3, 0};
        CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), c), "B が図面の左へ約 3 mm ずれています（許容 10 mm）", "position, moved left");
        c.output_frame.camera_motion_mm = {3, 0};
        CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), c), "B が図面の右へ約 3 mm ずれています（許容 10 mm）", "position, moved right");
        // The part along the baseline goes to surface-height: only the part across it is told.
        c.output_frame.camera_motion_mm = {5, -3};
        c.baseline = render::Point{1, 0};
        CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), c), "B が図面の上へ約 3 mm ずれています（許容 10 mm）", "position across the baseline");
        c.baseline.reset();
        CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), c), "B が図面の右上へ約 3 mm ずれています（許容 10 mm）", "position without a baseline");
        c = rich;
        c.output_frame.rotation_clockwise_deg = -2.5;
        CheckText(RegistrationWarningText(W(Code::camera_rotation, Cls::exceeded, 2.5, 2, Unit::deg), c), "B が画面で見て反時計回りに約 2.5° 回っています（許容 2°）", "rotation counterclockwise");
        CheckText(RegistrationPoseFixText(W(Code::camera_rotation, Cls::exceeded, 2.5, 2, Unit::deg), c), "B のカメラを画面で見て時計回りに約 2.5° 回してください", "fix rotation");
        c.output_frame.lens_direction = {-kHalfRoot2, kHalfRoot2};
        CheckText(RegistrationWarningText(W(Code::camera_tilt, Cls::exceeded, 1.2, 1, Unit::deg), c), "B のレンズが図面の左下へ約 1.2° 傾いています（許容 1°）", "tilt lower left");
        CheckText(RegistrationPoseFixText(W(Code::camera_tilt, Cls::exceeded, 1.2, 1, Unit::deg), c), "B のレンズを図面の右上へ約 1.2° 向け直してください", "fix tilt");
        c = rich;
        c.pose_display->height_percent = -1.5;
        c.pose_display->height_mm = -15;
        CheckText(RegistrationWarningText(W(Code::camera_height, Cls::exceeded, 1.5, 1, Unit::percent), c),
            "B が受け入れた設置の時より約 15 mm 低い位置にあります（許容 10 mm）。ピントが合わせ直された可能性もあります", "height lower");
        CheckText(RegistrationPoseFixText(W(Code::camera_height, Cls::exceeded, 1.5, 1, Unit::percent), c), "B のカメラを約 15 mm 上げてください", "fix height, lower");
        CheckText(RegistrationPoseFixText(W(Code::camera_height, Cls::exceeded, 1.5, 1, Unit::percent), rich), "B のカメラを約 15 mm 下げてください", "fix height, higher");
        // Without the extras the sentences are plainer, not missing.
        const RegistrationTextContext bare;
        CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), bare), "B の位置が約 3 mm ずれています（許容 10 mm）", "position without a direction");
        CheckText(RegistrationWarningText(W(Code::camera_height, Cls::exceeded, 1.5, 1, Unit::percent), bare),
            "B の高さが、受け入れた設置の時と約 1.5 % 違います（許容 1 %）。ピントが合わせ直された可能性もあります", "height without mm");
        CHECK(RegistrationPoseFixText(W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm), bare).empty());
        // A pixel value is turned into mm with the pitch.
        CheckText(RegistrationWarningText(W(Code::camera_position, Cls::exceeded, 6, 20, Unit::px), rich), "B が図面の上へ約 3 mm ずれています（許容 10 mm）", "position in px");
    }

    // ---- the same physical movement gives the same sentence for either mount ----
    {
        // The camera moved up in the drawing by 3 mm, turned clockwise on the screen by 2.5 degrees and tilted its lens to the upper right.
        // Seen by the camera (raw), a 180-degree mount sees every direction turned over.
        const render::Point up{0, -3};
        const render::Point toward_upper_right{kHalfRoot2, -kHalfRoot2};
        auto normal = RichContext();
        auto turned = RichContext();
        normal.output_frame = DisplayFor(false, up, 2.5, toward_upper_right, 1.2);
        turned.output_frame = DisplayFor(true, {0, 3}, 2.5, {-kHalfRoot2, kHalfRoot2}, 1.2);
        for (const auto code : {Code::camera_position, Code::camera_rotation, Code::camera_tilt}) {
            const auto w = W(code, Cls::exceeded, code == Code::camera_position ? 3 : code == Code::camera_rotation ? 2.5 : 1.2, 1, code == Code::camera_position ? Unit::mm : Unit::deg);
            CheckText(RegistrationWarningLine(w, normal), RegistrationWarningLine(w, turned), "same sentence for either mount: " + std::string(ex::WarningCodeText(code)));
            CheckText(RegistrationPoseFixText(w, normal), RegistrationPoseFixText(w, turned), "same fix for either mount: " + std::string(ex::WarningCodeText(code)));
        }
        CheckText(CameraOrientationShortText(OrientationSubject::camera_b_against_a, CameraOrientationOf(normal.output_frame)),
            CameraOrientationShortText(OrientationSubject::camera_b_against_a, CameraOrientationOf(turned.output_frame)), "same orientation line for either mount");
        // And a different movement is a different sentence (the direction is read, not ignored).
        auto other = RichContext();
        other.output_frame = DisplayFor(false, {0, 3}, -2.5, {-kHalfRoot2, kHalfRoot2}, 1.2);
        const auto position = W(Code::camera_position, Cls::exceeded, 3, 10, Unit::mm);
        Check(RegistrationWarningText(position, normal) != RegistrationWarningText(position, other), "another direction gives another sentence (position)");
        const auto tilt = W(Code::camera_tilt, Cls::exceeded, 1.2, 1, Unit::deg);
        Check(RegistrationWarningText(tilt, normal) != RegistrationWarningText(tilt, other), "another direction gives another sentence (tilt)");
        const auto rotation = W(Code::camera_rotation, Cls::exceeded, 2.5, 2, Unit::deg);
        Check(RegistrationWarningText(rotation, normal) != RegistrationWarningText(rotation, other), "another sense gives another sentence (rotation)");
    }

    // ---- a value equal to its limit is within it: no sentence says it exceeds ----
    {
        auto w = W(Code::seam_displacement, Cls::exceeded, 1, 1, Unit::px, Rem::retake);
        auto text = RegistrationWarningText(w, rich);
        CheckText(text, "継ぎ目のずれが最大 1 px で、基準の 1 px 以内です", "seam-displacement at the line");
        Check(!Contains(text, "超えています"), "seam-displacement at the line does not say it exceeds");
        w.value = 0.5;
        Check(!Contains(RegistrationWarningText(w, rich), "超えています"), "seam-displacement under the line");
        w.value = 1.004;
        CheckText(RegistrationWarningText(w, rich), "継ぎ目のずれが最大 1.004 px で、基準の 1 px を超えています", "just over the line reads differently");
        auto lift = W(Code::seam_lift_limit, Cls::exceeded, 1, 1, Unit::mm, Rem::shift_drawing_horizontal);
        lift.remedy_amount_mm = 50;
        text = RegistrationWarningText(lift, rich);
        Check(!Contains(text, "超えています"), "seam-lift-limit at the line does not say it exceeds");
        Check(Contains(text, "線の範囲内です"), "seam-lift-limit at the line says it is within");
        Check(!Contains(RegistrationWarningLine(lift, rich), "【基準外】"), "an exceeded warning at its limit is not labelled as outside the criteria");
        Check(EffectiveWarningClass(lift) == Cls::notice, "effective class at the limit");
        lift.value = 1.1;
        Check(EffectiveWarningClass(lift) == Cls::exceeded, "effective class over the limit");
        Check(Contains(RegistrationWarningLine(lift, rich), "【基準外】"), "label over the limit");
        // The pose components do not claim to exceed either; they only state the amount and the allowance.
        for (const auto code : {Code::camera_position, Code::camera_rotation, Code::camera_tilt, Code::camera_height}) {
            const auto p = W(code, Cls::exceeded, 1, 1, code == Code::camera_position ? Unit::mm : code == Code::camera_height ? Unit::percent : Unit::deg);
            Check(!Contains(RegistrationWarningText(p, rich), "超えて"), "pose component at the limit does not say it exceeds");
        }
    }

    // ---- the gate ----
    {
        rg::RegistrationGateResult g;
        g.state = rg::GateState::pass;
        g.after.maximum_px = 0.8;
        g.rows.dx_measured_share = 1.0;
        g.maxima.two_directional_px = 0.5;
        g.maxima.one_directional_px = 0.8;
        g.maxima.one_directional_normal = render::Point{0, 1};
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレ: 基準内（最大 0.8 px）。二方向の点の最大 0.5 px、一方向の点の最大 0.8 px（上下の成分）", "pass with both kinds of points");
        g.maxima.one_directional_px.reset();
        g.maxima.one_directional_normal.reset();
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレ: 基準内（最大 0.8 px）。二方向の点の最大 0.5 px", "pass with two-directional points only");
        g.maxima.two_directional_px.reset();
        g.maxima.one_directional_px = 0.8;
        g.maxima.one_directional_normal = render::Point{0, 1};
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレ: 基準内（最大 0.8 px）。一方向の点だけ（左右のずれは測れていません）。測れたのは上下の成分です", "pass with one-directional points only");
        g.rows.dx_measured_share = 0.8;
        g.rows.unmeasured = {{100, 200}};
        Check(Contains(RegistrationGateStateText(g), "左右のずれは未測定の行があります（測れた行は 80 %）"), "unmeasured rows are said");
        g.rows.unmeasured.clear();
        g.rows.dx_measured_share = 1.0;
        g.state = rg::GateState::undetermined;
        g.cause = rg::GateCause::none;
        g.maxima = {};
        g.findings = {{"global-placement-unverified", "review-visually"}};
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレを判定できませんでした。図面の配置を確かめられませんでした", "undetermined with a reason");
        g.findings = {{"something-new", "x-y"}};
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレを判定できませんでした", "undetermined with an unknown reason");
        g.state = rg::GateState::not_run;
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレを判定できませんでした（測定を行っていません）", "not run");
        g.state = rg::GateState::fail;
        g.cause = rg::GateCause::unmeasurable;
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレを測れない所があります", "fail, unmeasurable");
        g.cause = rg::GateCause::displacement;
        g.after.maximum_px = 2.5;
        g.seam_gate = a0::m2::seam_gate::SeamGateResult{};
        g.seam_gate->limits.limit_px = 1;
        g.seam_gate->limits.guard_band_px = 0.2;
        g.rows.dx_measured_share = 1.0;
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレが基準を超えています（最大 2.5 px、基準 1 px）", "fail, displacement");
        g.after.maximum_px = 0.9;
        CheckText(RegistrationGateStateText(g), "継ぎ目のズレが基準を超えています（最大 0.9 px。測定の不確かさを含めると基準 1 px を超えます）", "fail by the uncertainty");

        // The guidance by the cause: a paper that stands up never sends the person to calibrate again.
        g.state = rg::GateState::fail;
        g.cause = rg::GateCause::lift;
        Check(!Contains(RegistrationGateGuidanceText(g), "校正"), "lift: no 'calibrate again'");
        Check(Contains(RegistrationGateGuidanceText(g), "置き直"), "lift: put the drawing again");
        g.cause = rg::GateCause::pose;
        Check(Contains(RegistrationGateGuidanceText(g), "校正をやり直してください"), "pose: recalibrate if it does not go");
        g.cause = rg::GateCause::displacement;
        CheckText(RegistrationGateGuidanceText(g), "撮り直してください", "displacement: retake");
        g.cause = rg::GateCause::unmeasurable;
        Check(Contains(RegistrationGateGuidanceText(g), "ずらして"), "unmeasurable: shift the drawing");

        // Every state with every cause, every kind of maxima and rows: a sentence, always, and none of the words.
        const std::array<rg::GateState, 4> states{rg::GateState::pass, rg::GateState::fail, rg::GateState::undetermined, rg::GateState::not_run};
        const std::array<rg::GateCause, 5> causes{rg::GateCause::none, rg::GateCause::lift, rg::GateCause::pose, rg::GateCause::displacement, rg::GateCause::unmeasurable};
        const std::vector<std::string> reasons{"", "seam-misalignment", "predicted-misalignment", "unmeasurable-content", "no-measurable-block", "unpredicted-empty-block",
            "global-placement-mismatch", "global-placement-unverified", "transition-outside-band", "brand-new-reason"};
        std::size_t count = 0;
        for (const auto state : states)
            for (const auto cause : causes)
                for (int kinds = 0; kinds < 4; ++kinds)
                    for (int rows = 0; rows < 3; ++rows)
                        for (int with_gate = 0; with_gate < 2; ++with_gate)
                            for (const auto& reason : reasons) {
                                rg::RegistrationGateResult x;
                                x.state = state;
                                x.cause = cause;
                                if (kinds & 1) x.maxima.two_directional_px = 0.4;
                                if (kinds & 2) {
                                    x.maxima.one_directional_px = 1.6;
                                    x.maxima.one_directional_normal = render::Point{kHalfRoot2, kHalfRoot2};
                                }
                                if (with_gate) {
                                    x.seam_gate = a0::m2::seam_gate::SeamGateResult{};
                                    x.seam_gate->limits.limit_px = 1;
                                    x.seam_gate->limits.guard_band_px = 0.2;
                                    x.seam_gate->maximum_px = 1.6;
                                }
                                x.rows.dx_measured_share = rows == 0 ? 1.0 : rows == 1 ? 0.5 : std::nan("");
                                if (rows == 1) x.rows.unmeasured = {{10, 20}};
                                if (!reason.empty()) x.findings = {{reason, "x-y"}};
                                const auto label = "gate " + std::string(rg::GateStateCode(state)) + "/" + std::string(rg::GateCauseCode(cause)) + "/" + reason;
                                CheckSentence(RegistrationGateStateText(x), label);
                                CheckSentence(RegistrationGateGuidanceText(x), label + " guidance");
                                ++count;
                            }
        Check(count == 4 * 5 * 4 * 3 * 2 * reasons.size(), "the gate sweep ran every combination");
        // "passed" is never the word, on any state.
        for (const auto state : states) {
            rg::RegistrationGateResult x;
            x.state = state;
            Check(!Contains(RegistrationGateStateText(x), "合格"), "no word for passed");
        }
    }

    // ---- the assessment, the stages of the time budget, the nine remedies ----
    {
        CheckText(RegistrationAssessmentText(ex::AssessmentState::within_criteria), "位置合わせの確認: 基準内", "assessment within");
        CheckText(RegistrationAssessmentText(ex::AssessmentState::outside_criteria), "位置合わせの確認: 基準外の項目があります", "assessment outside");
        CheckText(RegistrationAssessmentText(ex::AssessmentState::not_judged), "位置合わせの確認: 判定できなかった項目があります", "assessment not judged");
        for (const auto s : {ex::AssessmentState::within_criteria, ex::AssessmentState::outside_criteria, ex::AssessmentState::not_judged})
            CheckSentence(RegistrationAssessmentText(s), "assessment");
        for (const auto& e : ex::kReviewStageTexts) CheckSentence(ReviewStageName(e.first), std::string("stage ") + std::string(e.second));
        ex::TimeBudgetRecord record;
        CheckText(TimeBudgetText(record), "時間の都合で省いた処理はありません", "time budget, nothing skipped");
        record.skipped = {{ex::ReviewStage::parallax, "time-budget"}, {ex::ReviewStage::gate, "time-budget"}};
        CheckText(TimeBudgetText(record), "時間が足りなかったため、次の処理を省きました: 紙の浮きの補正、継ぎ目のズレの測定", "time budget, skipped");
        // A stage that threw is not a stage dropped for the time (#417).
        record.skipped = {{ex::ReviewStage::parallax, "stage-failed"}, {ex::ReviewStage::gate, "time-budget"}};
        CheckText(TimeBudgetText(record), "時間が足りなかったため、次の処理を省きました: 継ぎ目のズレの測定", "time budget, one stage threw");
        record.skipped = {{ex::ReviewStage::gate, "stage-failed"}};
        CheckText(TimeBudgetText(record), "時間の都合で省いた処理はありません", "time budget, only a stage that threw");
        for (const auto& e : ex::kRemedyTexts) {
            for (const auto amount : {std::optional<double>{}, std::optional<double>{50}, std::optional<double>{0}, std::optional<double>{std::nan("")}}) {
                const auto text = RemedyGuidanceText(e.first, amount);
                CheckSentence(text, std::string("remedy ") + std::string(e.second));
            }
        }
        CheckText(RemedyGuidanceText(Rem::shift_drawing_horizontal, 50), "図面を左右に約 5 cm ずらして置き直してください", "shift with the amount");
        CheckText(RemedyGuidanceText(Rem::shift_drawing_horizontal), "図面を左右にずらして置き直してください", "shift without the amount");
        CheckText(RemedyGuidanceText(Rem::none), "特に対応は要りません", "none");
        // Nothing about pressing, flattening, weights or boards in any of them.
        for (const auto& e : ex::kRemedyTexts) CheckSentence(RemedyGuidanceText(e.first, 80), "remedy words");
    }

    // ---- every code of a warning, every class, every unit, with and without the details ----
    {
        std::vector<RegistrationTextContext> contexts{RegistrationTextContext{}, rich};
        auto level = ex::GeometryLevelDecision{};
        level.level = ex::GeometryLevel::g2;
        contexts.push_back(rich);
        contexts.back().geometry_level = &level;
        std::size_t texts = 0;
        for (const auto& e : ex::kWarningCodeTexts) {
            for (const auto cls : {Cls::exceeded, Cls::unverified, Cls::notice})
                for (const auto& unit_entry : ex::kWarningUnitTexts)
                    for (const auto limit : {std::optional<double>{}, std::optional<double>{1.0}})
                        for (const double value : {0.0, 2.5, std::nan("")})
                            for (const auto& remedy_entry : ex::kRemedyTexts)
                                for (int detail = 0; detail < 3; ++detail) {
                                    auto w = W(e.first, cls, value, limit, unit_entry.first, remedy_entry.first);
                                    if (detail >= 1) {
                                        w.rows_mm = ex::RowSpan{120, 180};
                                        w.rows_px = ex::RowSpan{1000, 1500};
                                        w.applied = true;
                                        w.interpretations = {"surface-raised", "unknown-reading"};
                                    }
                                    if (detail == 2) {
                                        w.remedy_amount_mm = 80;
                                        w.interpretations = {"camera-moved-along-baseline"};
                                        w.rows_mm = ex::RowSpan{180, 120};
                                    }
                                    for (const auto& context : contexts) {
                                        const auto name = std::string(e.second);
                                        const auto line = RegistrationWarningLine(w, context);
                                        ++texts;
                                        if (line.empty()) CheckSentence(line, "warning " + name);
                                        for (const char* word : kForbidden)
                                            if (Contains(line, word)) Check(false, "forbidden word '" + std::string(word) + "' in " + name + ": " + line);
                                        if (LooksLikeInternalCode(line)) Check(false, "internal code in " + name + ": " + line);
                                    }
                                }
        }
        Check(texts > 0, "swept the warnings");
        std::cerr << "warning sentences swept: " << texts << '\n';
        ++checks;
        // Each of the 21 codes has a sentence of its own that is not the generic one.
        std::vector<std::string> seen;
        for (const auto& e : ex::kWarningCodeTexts) {
            const auto text = RegistrationWarningText(W(e.first, Cls::notice, 1.5, std::nullopt, Unit::mm), rich);
            CheckSentence(text, "code " + std::string(e.second));
            Check(text != "確認が必要な項目があります", "a sentence of its own: " + std::string(e.second));
            Check(std::find(seen.begin(), seen.end(), text) == seen.end(), "distinct sentences: " + std::string(e.second));
            seen.push_back(text);
        }
        Check(ex::kWarningCodeTexts.size() == 21, "21 codes");
        // The label by class: exceeded / unverified / notice.
        CheckText(WarningClassLabel(Cls::exceeded), "基準外", "label exceeded");
        CheckText(WarningClassLabel(Cls::unverified), "確かめられていません", "label unverified");
        CheckText(WarningClassLabel(Cls::notice), "お知らせ", "label notice");
        CheckText(RegistrationWarningLine(W(Code::placement_unverified, Cls::unverified, 0, std::nullopt, Unit::px, Rem::review_visually), rich),
            "【確かめられていません】図面の配置を確かめられませんでした（目印になる模様が足りません）。合成画像を目で見て確かめてください", "line, unverified");
        CheckText(RegistrationWarningLine(W(Code::camera_position, Cls::exceeded, 12, 10, Unit::mm, Rem::check_camera_mount), rich),
            "【基準外】B が図面の上へ約 12 mm ずれています（許容 10 mm）。B のカメラを図面の下へ約 12 mm 動かしてください", "line, camera position");
        CheckText(RegistrationWarningLine(W(Code::geometry_level, Cls::notice, 3, std::nullopt, Unit::rows), RegistrationTextContext{}),
            "【お知らせ】位置合わせは、推定が確かでない部分を当てない段で行いました", "line, notice with nothing to do");
        {
            auto fold = W(Code::fold_near_seam, Cls::notice, 40, std::nullopt, Unit::mm, Rem::shift_drawing_horizontal);
            fold.rows_mm = ex::RowSpan{300, 300};
            fold.remedy_amount_mm = 80;
            // The sentence already says how to put it right: it is not said twice.
            CheckText(RegistrationWarningLine(fold, rich), "【お知らせ】" + RegistrationWarningText(fold, rich), "line, fold: the remedy is inside the sentence");
        }
    }

    // ---- the layer-1 notice (a pixel value) and the unmeasured rows ----
    {
        auto w = W(Code::surface_height, Cls::notice, 2.2, 1, Unit::px);
        w.applied = true;
        CheckText(RegistrationWarningText(w, rich), "紙面が台より約 4 mm 高いため、補正しなければ継ぎ目で約 2.2 px ずれていました（基準 1 px）。位置合わせで補正しました", "surface-height notice");
        CheckText(RegistrationWarningText(w, RegistrationTextContext{}), "紙面の高さのため、補正しなければ継ぎ目で約 2.2 px ずれていました（基準 1 px）。位置合わせで補正しました", "surface-height notice without the surface");
    }

    // ---- not a number or infinite: a sentence, never "nan" ----
    {
        const double bad[] = {std::nan(""), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        for (const double v : bad)
            for (const auto& e : ex::kWarningCodeTexts) {
                const auto text = RegistrationWarningLine(W(e.first, Cls::exceeded, v, v, Unit::mm), rich);
                CheckSentence(text, "not a number: " + std::string(e.second));
            }
    }

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
