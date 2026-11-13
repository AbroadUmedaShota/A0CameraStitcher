// The words of the recalibration of camera B from one pair (#434 lane B; src/phase0/include/a0/phase0/experimental/recalibration_text.hpp):
// the guide, the names of the stages, the sentence for every reason (the twelve refusals of the computation and the codes of the job), the lines
// of the result screen with their numbers and directions, the verdict lines (checked, unchecked, not usable) and what is said after the decision.
// No camera, no file, no window. Every number below is a value of this test.
#include "a0/phase0/experimental/recalibration_text.hpp"
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
namespace rc = a0::m2::experimental::recalibration;
namespace gate = a0::m2::experimental::registration_gate;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* what, int line) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAILED (line " << line << "): " << what << '\n'; }
}
#define CHECK(expr) Check((expr), #expr, __LINE__)
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool HasLine(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines) if (Has(l, part)) return true;
    return false;
}
// Nothing the person reads names a path, a drive, a camera alias or a user.
bool Plain(const std::string& text) {
    for (const char* bad : {":\\", ":/", "\\\\", "/Users", "CAM-A", "CAM-B", "original.jpg", "仮Merge"}) if (Has(text, bad)) return false;
    return true;
}

// ---- Fixtures ----
rc::Display SampleDisplay() {
    rc::Display d;
    d.has_motion = true;
    d.motion.camera_motion_mm = {42.0, 0.3};
    d.motion.rotation_clockwise_deg = 0.3;
    d.pose.position_mm = 42.0;
    d.pose.tx_mm = 42.0;
    d.pose.tilt_deg = 0.1;
    d.pose.height_percent = 0.2;
    d.has_overlap = true;
    d.narrowest_overlap_mm = 118.0;
    d.seam_x_previous_mm = 650.0;
    d.seam_x_new_mm = 671.0;
    d.width_previous_px = 1800;
    d.width_new_px = 2220;
    d.height_previous_px = d.height_new_px = 1600;
    d.right_previous_mm = 740.0;
    d.right_new_mm = 782.0;
    d.has_new = true;
    d.has_edge_distance = true;
    d.edge_share_a = 0.12;
    d.edge_share_b = 0.09;
    return d;
}
RecalibrationGateFacts Gate(gate::GateState state) {
    RecalibrationGateFacts g;
    g.known = true;
    g.state = state;
    g.maximum_px = 0.4;
    g.limit_px = 1.0;
    g.measured_share = 0.86;
    return g;
}

void Guide() {
    const auto lines = RecalibrationGuideLines();
    CHECK(lines.size() == 5);
    CHECK(HasLine(lines, "カメラの間隔を変えた時"));
    CHECK(HasLine(lines, "ピントを合わせ"));            // the fourth condition of the paper
    CHECK(HasLine(lines, "冊子") && HasLine(lines, "罫線"));
    CHECK(HasLine(lines, "高さ") && HasLine(lines, "ズーム"));   // what not to touch
    for (const auto& l : lines) CHECK(Plain(l) && !Has(l, "B を動かした"));
}

void Stages() {
    std::set<std::string> names, codes;
    for (const auto s : {RecalibrationStage::read_condition, RecalibrationStage::decode, RecalibrationStage::wide_search, RecalibrationStage::fine_fit, RecalibrationStage::bake,
        RecalibrationStage::check, RecalibrationStage::verify, RecalibrationStage::sample, RecalibrationStage::crops}) {
        CHECK(!RecalibrationStageName(s).empty() && Plain(RecalibrationStageName(s)));
        names.insert(RecalibrationStageName(s));
        codes.insert(std::string(RecalibrationStageCode(s)));
    }
    CHECK(names.size() == 9 && codes.size() == 9);
    // The computation's own stages keep their codes and map to the job's.
    for (const auto s : {rc::Stage::wide_search, rc::Stage::fine_fit, rc::Stage::bake, rc::Stage::check, rc::Stage::verify})
        CHECK(RecalibrationStageCode(RecalibrationStageOf(s)) == rc::StageCode(s));
    CHECK(RecalibrationStageName(RecalibrationStage::wide_search) == "大まかな探索" && RecalibrationStageName(RecalibrationStage::fine_fit) == "細かい合わせ");
}

// Every one of the twelve refusals of the computation and every code of the job has its own sentence and a way to put it right.
void Sentences() {
    const rc::Refusal all[] = {rc::Refusal::no_overlap, rc::Refusal::estimate_failed, rc::Refusal::estimate_not_verified, rc::Refusal::tilt_exceeded,
        rc::Refusal::height_exceeded, rc::Refusal::profile_invalid, rc::Refusal::raster_too_large, rc::Refusal::raster_outside_sheet,
        rc::Refusal::creation_check_failed, rc::Refusal::reestimate_not_verified, rc::Refusal::reestimate_displacement, rc::Refusal::stopped};
    std::set<std::string> distinct;
    for (const auto r : all) {
        for (const auto klass : {FindingClass::no_candidate, FindingClass::rejected, FindingClass::unverified}) {
            const RecalibrationFinding f{klass, std::string(rc::RefusalCode(r)), {}};
            const auto s = RecalibrationFindingSentence(f);
            CHECK(!s.reason.empty() && !s.remedy.empty() && !Has(s.reason, "理由を特定できません") && Plain(s.reason) && Plain(s.remedy));
        }
        distinct.insert(RecalibrationFindingSentence({FindingClass::rejected, std::string(rc::RefusalCode(r)), {}}).reason);
    }
    CHECK(distinct.size() == 12);   // twelve codes, twelve different sentences (the unchecked variant of reestimate-not-verified is a thirteenth)
    const char* lane[] = {"no-v3-condition", "staging-exists", "staging-in-settings", "live-not-stopped", "decode-failed", "hash-mismatch", "size-mismatch", "profile-unreadable", "out-of-memory", "job-failed",
        "gate-undetermined", "gate-unmeasurable", "gate-not-run", "gate-fail", "sample-estimate-undetermined", "sample-estimate-out-of-envelope", "sample-skipped", "sample-failed"};
    for (const auto* code : lane) {
        const auto s = RecalibrationFindingSentence({FindingClass::rejected, code, {}});
        CHECK(!s.reason.empty() && !s.remedy.empty() && !Has(s.reason, "理由を特定できません") && Plain(s.reason) && Plain(s.remedy));
    }
    // The reasons of an estimate that did not hold sharpen the sentence (the table of the design, 2.6).
    const auto reason = [](const char* r) { return RecalibrationFindingSentence({FindingClass::no_candidate, "estimate-not-verified", r}); };
    CHECK(Has(reason("unique-blocks").reason, "模様が足りません"));
    CHECK(Has(reason("prediction-uncertain").reason, "精度が出ません"));
    CHECK(Has(reason("residual-shape").reason, "浮いている") && Has(reason("residual-shape").remedy, "平らに"));
    CHECK(Has(reason("not-converged").reason, "一つに決まりません") && Has(reason("starts-disagree").reason, "一つに決まりません"));
    CHECK(Has(reason("repeating-only").reason, "繰り返す模様"));
    CHECK(Has(reason("coarse-no-unique-blocks").reason, "同じ模様の繰り返し") && Has(reason("coarse-no-unique-blocks").remedy, "格子"));
    CHECK(Has(reason("coarse-inconsistent").reason, "大まかな位置") && Has(reason("coarse-ambiguous").reason, "大まかな位置"));
    CHECK(Has(reason("too-few-points").reason, "点が足りません") && Has(reason("no-overlap").reason, "重なり"));
    CHECK(reason("coarse-no-unique-blocks").reason != reason("unique-blocks").reason);
    CHECK(reason("unique-blocks").reason != reason("residual-shape").reason && reason("anything-else").reason != reason("unique-blocks").reason);
    // Checked and wrong, and could not be checked, are told apart.
    const auto wrong = RecalibrationFindingSentence({FindingClass::rejected, "reestimate-not-verified", {}});
    const auto unchecked = RecalibrationFindingSentence({FindingClass::unverified, "reestimate-not-verified", {}});
    CHECK(wrong.reason != unchecked.reason && Has(wrong.reason, "範囲の外") && Has(unchecked.reason, "確かな値になりませんでした"));
    CHECK(Has(RecalibrationFindingSentence({FindingClass::unverified, "gate-undetermined", {}}).reason, "確かめられませんでした"));
    CHECK(Has(RecalibrationFindingSentence({FindingClass::rejected, "gate-fail", {}}).reason, "基準を超えました"));
    // The tilt and the height send the person to the chart, not to a retake.
    CHECK(Has(RecalibrationFindingSentence({FindingClass::rejected, "tilt-exceeded", {}}).remedy, "チャート"));
    CHECK(Has(RecalibrationFindingSentence({FindingClass::rejected, "height-exceeded", {}}).remedy, "チャート"));
    // A code nobody knows gets a general sentence, never its own text.
    const auto unknown = RecalibrationFindingSentence({FindingClass::rejected, "no-such-code-xyz", {}});
    CHECK(Has(unknown.reason, "理由を特定できません") && !Has(unknown.reason, "xyz") && !Has(unknown.remedy, "xyz"));
    // The sentences speak of the cameras and the interval, not of one camera that was moved.
    for (const auto* code : lane) CHECK(!Has(RecalibrationFindingSentence({FindingClass::rejected, code, {}}).reason, "B を動かし"));
}

void ResultLines() {
    RecalibrationResultFacts facts;
    facts.display = SampleDisplay();
    facts.gate = Gate(gate::GateState::pass);
    facts.creation_checked = true;
    const auto lines = RecalibrationResultLines(facts);
    CHECK(lines.size() == 7);
    CHECK(HasLine(lines, "A に対する B の位置の変化: 図面の右へ約 42 mm、上下は下へ約 0.3 mm"));
    CHECK(HasLine(lines, "回り 約 0.3°（画面で見て時計回り）") && HasLine(lines, "傾き 約 0.1°") && HasLine(lines, "高さ +0.2 %"));
    CHECK(HasLine(lines, "重なりの幅: いちばん狭い行で約 118 mm"));
    CHECK(HasLine(lines, "継ぎ目の位置が変わります: 図面の上で右へ約 21 mm（検品の時に見る場所が変わります）"));
    CHECK(HasLine(lines, "合成の画像の大きさ: 幅 1800 → 2220 画素（右の端が約 42 mm 広がります）、高さは同じ"));
    CHECK(HasLine(lines, "継ぎ目から写真の端まで: A は幅の 12 %、B は幅の 9 %"));
    CHECK(HasLine(lines, "継ぎ目のずれ: 基準内（最大 0.4 px、基準 1 px）。測れた行 86 %"));
    CHECK(HasLine(lines, "範囲の検査: 通りました"));
    for (const auto& l : lines) CHECK(Plain(l) && !Has(l, "B を動かした"));
    // The line of B's place says up and down on its own (a vertical slip must be seen), and left and up for the other signs.
    {
        auto f = facts;
        f.display.motion.camera_motion_mm = {-12.0, -0.4};
        f.display.motion.rotation_clockwise_deg = -0.2;
        const auto l = RecalibrationResultLines(f);
        CHECK(HasLine(l, "図面の左へ約 12 mm、上下は上へ約 0.4 mm") && HasLine(l, "反時計回り"));
        auto none = facts;
        none.display.motion.camera_motion_mm = {0.0, 0.0};
        none.display.pose.position_mm = 0.0;
        none.display.motion.rotation_clockwise_deg = 0.0;
        none.display.pose.tilt_deg = 0.0;
        none.display.pose.height_percent = 0.0;
        CHECK(HasLine(RecalibrationResultLines(none), "位置はほとんど変わっていません"));
        auto only_vertical = facts;
        only_vertical.display.motion.camera_motion_mm = {0.0, 3.0};
        CHECK(HasLine(RecalibrationResultLines(only_vertical), "左右はほぼ同じ、上下は下へ約 3 mm"));
    }
    // The direction is the drawing's, from `motion`: the camera's own coordinates (pose.tx), which are reversed by a half-turn mounting, change nothing.
    {
        auto reversed = facts;
        reversed.display.pose.tx_mm = -42.0;
        reversed.display.pose.rx_deg = 0.05;
        CHECK(RecalibrationResultLines(reversed).front() == lines.front());
    }
    // The seam: unchanged, moved left; the raster: unchanged, narrower, taller.
    {
        auto f = facts;
        f.display.seam_x_new_mm = f.display.seam_x_previous_mm;
        CHECK(HasLine(RecalibrationResultLines(f), "継ぎ目の位置は変わりません"));
        f.display.seam_x_new_mm = 630.0;
        CHECK(HasLine(RecalibrationResultLines(f), "図面の上で左へ約 20 mm"));
        f.display.right_new_mm = f.display.right_previous_mm;
        f.display.width_new_px = f.display.width_previous_px;
        const auto l = RecalibrationResultLines(f);
        CHECK(HasLine(l, "幅 1800 → 1800 画素") && !HasLine(l, "広がります") && !HasLine(l, "狭まります"));
        f.display.right_new_mm = 700.0;
        f.display.height_new_px = 1700;
        const auto l2 = RecalibrationResultLines(f);
        CHECK(HasLine(l2, "狭まります") && HasLine(l2, "高さ 1600 → 1700 画素"));
    }
    // A refusal before the plan was whole (the raster went outside the sheet) leaves the "after" side half filled: the width and the right
    // edge are known, the seam and the height are zero. No line may speak of the new seam or of a height that became 0.
    {
        auto f = facts;
        f.display.has_new = false;
        f.display.seam_x_new_mm = 0.0;
        f.display.height_new_px = 0;
        const auto l = RecalibrationResultLines(f);
        CHECK(l.size() == lines.size() - 2);
        CHECK(!HasLine(l, "継ぎ目の位置") && !HasLine(l, "合成の画像の大きさ") && !HasLine(l, "画素") && !HasLine(l, "650"));
        CHECK(HasLine(l, "A に対する B の位置の変化") && HasLine(l, "継ぎ目のずれ: 基準内"));   // what is known is still shown
        const auto screen = RecalibrationScreenLines(RecalibrationVerdict::unusable, {{FindingClass::rejected, "raster-outside-sheet", {}}}, f);
        CHECK(HasLine(screen, "用紙の外に出ます") && !HasLine(screen, "継ぎ目の位置") && !HasLine(screen, "合成の画像の大きさ"));
    }
    // The gate in each of its states: what it measured is always beside the verdict.
    {
        auto f = facts;
        f.gate = Gate(gate::GateState::undetermined);
        CHECK(HasLine(RecalibrationResultLines(f), "継ぎ目のずれ: 確かめられませんでした。測れた行 86 %"));
        f.gate = Gate(gate::GateState::fail);
        CHECK(HasLine(RecalibrationResultLines(f), "継ぎ目のずれ: 基準を超えました（最大 0.4 px、基準 1 px）"));
        f.gate = Gate(gate::GateState::fail);
        f.gate.cause = gate::GateCause::unmeasurable;
        CHECK(HasLine(RecalibrationResultLines(f), "継ぎ目のずれ: 測れない所があり、確かめられませんでした。測れた行 86 %") && !HasLine(RecalibrationResultLines(f), "基準を超えました"));
        f.gate = Gate(gate::GateState::not_run);
        CHECK(HasLine(RecalibrationResultLines(f), "確かめが行われませんでした"));
        f.gate = RecalibrationGateFacts{};
        CHECK(HasLine(RecalibrationResultLines(f), "確かめていません"));
        f.gate = Gate(gate::GateState::pass);
        f.gate.maximum_px.reset();
        f.gate.measured_share = std::numeric_limits<double>::quiet_NaN();
        CHECK(HasLine(RecalibrationResultLines(f), "継ぎ目のずれ: 基準内（基準 1 px）"));
    }
    // A screen that could not compute anything still has no line that invents a number.
    {
        RecalibrationResultFacts empty;
        const auto l = RecalibrationResultLines(empty);
        CHECK(l.size() == 2 && HasLine(l, "求められませんでした") && HasLine(l, "確かめていません"));
    }
}

void Verdicts() {
    CHECK(RecalibrationVerdictLine(RecalibrationVerdict::usable) == "この校正は使えます");
    CHECK(Has(RecalibrationVerdictLine(RecalibrationVerdict::usable_unverified), "未確認の校正"));
    CHECK(!Has(RecalibrationVerdictLine(RecalibrationVerdict::usable_unverified), "確かめられませんでした"));   // what was not checked is for the reason lines
    CHECK(RecalibrationVerdictLine(RecalibrationVerdict::unusable) == "この校正は使えません");
    CHECK(RecalibrationVerdictCode(RecalibrationVerdict::usable_unverified) == "usable-unverified");
    // The screen: the verdict first, then why, then the numbers.
    RecalibrationResultFacts facts;
    facts.display = SampleDisplay();
    facts.gate = Gate(gate::GateState::undetermined);
    facts.creation_checked = true;
    const std::vector<RecalibrationFinding> findings{{FindingClass::unverified, "gate-undetermined", {}}};
    const auto lines = RecalibrationScreenLines(RecalibrationVerdict::usable_unverified, findings, facts);
    CHECK(lines.size() >= 4 && Has(lines.front(), "未確認の校正"));
    CHECK(Has(lines[1], "継ぎ目のずれを確かめられませんでした。"));
    CHECK(Has(lines[2], "A に対する B の位置の変化"));
    // The check of the estimate on the new profile was not trusted but the gate passed: the screen must not say "the seam was not checked" and
    // "the seam is within the line" together. The verdict line says only that the calibration is unchecked; the reasons say what.
    {
        RecalibrationResultFacts passed;
        passed.display = SampleDisplay();
        passed.gate = Gate(gate::GateState::pass);
        passed.creation_checked = true;
        const std::vector<RecalibrationFinding> estimate_only{{FindingClass::unverified, "reestimate-not-verified", "too-few-points"}};
        const auto both = RecalibrationScreenLines(RecalibrationVerdict::usable_unverified, estimate_only, passed);
        CHECK(Has(both.front(), "未確認の校正") && HasLine(both, "新しい校正での確かめが、確かな値になりませんでした") && HasLine(both, "継ぎ目のずれ: 基準内"));
        CHECK(!HasLine(both, "継ぎ目のずれを確かめられませんでした"));
        for (const auto& l : both) CHECK(!(Has(l, "確かめられませんでした") && Has(l, "基準内")));
        // When the gate itself could not decide, the reason line (not the verdict line) says so.
        passed.gate = Gate(gate::GateState::undetermined);
        const std::vector<RecalibrationFinding> gate_only{{FindingClass::unverified, "gate-undetermined", {}}};
        const auto gate_lines = RecalibrationScreenLines(RecalibrationVerdict::usable_unverified, gate_only, passed);
        CHECK(!Has(gate_lines.front(), "確かめられませんでした") && Has(gate_lines[1], "継ぎ目のずれを確かめられませんでした"));
    }
    // A calibration that cannot be used says why in a way the person can act on.
    const auto refused = RecalibrationScreenLines(RecalibrationVerdict::unusable, {{FindingClass::rejected, "tilt-exceeded", {}}}, RecalibrationResultFacts{});
    CHECK(refused.front() == "この校正は使えません" && HasLine(refused, "チャートでの校正が要ります"));
    for (const auto& l : lines) CHECK(Plain(l));
    for (const auto& l : refused) CHECK(Plain(l));
}

void AfterTheDecision() {
    CHECK(Has(RecalibrationConfirmedText(3, false), "校正を更新しました（改訂 3）") && !Has(RecalibrationConfirmedText(3, false), "未確認"));
    CHECK(Has(RecalibrationConfirmedText(3, true), "未確認の校正として更新しました（改訂 3）"));
    for (const char* reason : {"condition-changed", "not-usable", "not-acknowledged", "write-failed", "readback-failed"}) {
        const auto text = RecalibrationNotConfirmedText(reason);
        CHECK(!text.empty() && Plain(text));
    }
    CHECK(Has(RecalibrationNotConfirmedText("condition-changed"), "撮り直してください"));
    CHECK(Has(RecalibrationNotConfirmedText("not-acknowledged"), "警告を見たうえで"));
    CHECK(Has(RecalibrationNotConfirmedText("write-failed"), "前の校正のまま"));
    CHECK(RecalibrationNotConfirmedText("write-failed") != RecalibrationNotConfirmedText("readback-failed"));
}

void Nudge() {
    CHECK(Has(RecalibrationNudgeText(), "カメラの間隔を変えた時") && Has(RecalibrationNudgeText(), "校正をやり直す") && !Has(RecalibrationNudgeText(), "B を動かした"));
    CHECK(!RecalibrationNudgeApplies({}));
    CHECK(RecalibrationNudgeApplies({true, false, false, false}) && RecalibrationNudgeApplies({false, true, false, false}) &&
        RecalibrationNudgeApplies({false, false, true, false}) && RecalibrationNudgeApplies({false, false, false, true}));
}
}

int main() {
    try {
        Guide();
        Stages();
        Sentences();
        ResultLines();
        Verdicts();
        AfterTheDecision();
        Nudge();
    } catch (const std::exception& e) {
        std::cerr << "FAILED: an exception: " << e.what() << '\n';
        return 1;
    }
    std::cout << "recalibration text: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
