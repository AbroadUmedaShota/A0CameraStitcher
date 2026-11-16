#pragma once
// The words of the recalibration of camera B from one pair (#434; docs/design/recalibration-from-pair.md): the guide before the shot,
// the names of the stages, the lines of the result screen, the sentence for each reason a calibration is not made or is not checked,
// and what is said after the person decides. Header only, no window and no camera. The sentences are made here and nowhere else.
//
// Rules the sentences keep (the same as fixed_composite_registration_text.hpp):
//  - UTF-8 Japanese, plain words; no code names, no paths, no camera identifiers;
//  - numbers are read with the one rounding of registration_text_detail (degrees 1 digit, mm 1, px 2, percent 2);
//  - directions are the drawing's as seen on the screen (right, left, above, below), taken from OutputFrameDisplay, so the same
//    physical movement gives the same sentence for either mount orientation;
//  - the sentences do not say that B moved: the cameras may both have been moved. They speak of the interval between the cameras and
//    of the place of B against A;
//  - "checked" and "not checked" are told apart. A calibration that was checked and found not to fit is never offered; one that could
//    not be checked is offered with a warning, and the screen calls it "未確認の校正" (an unchecked calibration).
#include "a0/m2/experimental/fixed_composite_recalibration.hpp"
#include "a0/m2/experimental/registration_gate.hpp"
#include "a0/phase0/experimental/fixed_composite_registration_text.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::phase0::experimental {

// ---- The stages of one run ----
enum class RecalibrationStage {read_condition,decode,wide_search,fine_fit,bake,check,verify,sample,crops};
[[nodiscard]] inline std::string_view RecalibrationStageCode(RecalibrationStage stage){
    switch(stage){
    case RecalibrationStage::read_condition: return "read-condition";
    case RecalibrationStage::decode: return "decode";
    case RecalibrationStage::wide_search: return "wide-search";
    case RecalibrationStage::fine_fit: return "fine-fit";
    case RecalibrationStage::bake: return "bake";
    case RecalibrationStage::check: return "check";
    case RecalibrationStage::verify: return "verify";
    case RecalibrationStage::sample: return "sample";
    case RecalibrationStage::crops: return "crops";
    }
    return "unknown";
}
[[nodiscard]] inline std::string RecalibrationStageName(RecalibrationStage stage){
    switch(stage){
    case RecalibrationStage::read_condition: return "今の校正を読んでいます";
    case RecalibrationStage::decode: return "撮影した画像を読んでいます";
    case RecalibrationStage::wide_search: return "大まかな探索";
    case RecalibrationStage::fine_fit: return "細かい合わせ";
    case RecalibrationStage::bake: return "新しい校正の組み立て";
    case RecalibrationStage::check: return "範囲の検査";
    case RecalibrationStage::verify: return "新しい校正での確かめ";
    case RecalibrationStage::sample: return "見本の合成";
    case RecalibrationStage::crops: return "継ぎ目の切り出し";
    }
    return "処理中";
}
[[nodiscard]] inline RecalibrationStage RecalibrationStageOf(a0::m2::experimental::recalibration::Stage stage){
    namespace rc=a0::m2::experimental::recalibration;
    switch(stage){
    case rc::Stage::wide_search: return RecalibrationStage::wide_search;
    case rc::Stage::fine_fit: return RecalibrationStage::fine_fit;
    case rc::Stage::bake: return RecalibrationStage::bake;
    case rc::Stage::check: return RecalibrationStage::check;
    case rc::Stage::verify: return RecalibrationStage::verify;
    }
    return RecalibrationStage::wide_search;
}

// ---- What was found, and how it counts ----
//  no_candidate  nothing could be proposed (no overlap, no position that holds, a stop, a file that cannot be read)
//  rejected      a calibration was made and then checked, and the check says it does not fit (or the check itself refused it)
//  unverified    a calibration was made and the check could not be made (the marks were too few to measure). Only this class can be
//                confirmed, with a warning, and only when the policy allows it.
enum class FindingClass {no_candidate,rejected,unverified};
[[nodiscard]] inline std::string_view FindingClassCode(FindingClass klass){
    return klass==FindingClass::no_candidate?"no-candidate":klass==FindingClass::rejected?"rejected":"unverified";
}
struct RecalibrationFinding {
    FindingClass klass{FindingClass::no_candidate};
    std::string code;     // the refusal code of the computation, or one of the codes of this lane (ASCII)
    std::string reason;   // an estimator reason code that sharpens the sentence (unique-blocks, residual-shape, ...); may be empty
};

// What the screen is told about the whole run.
//  usable             checked everywhere it was checked: it may be used
//  usable_unverified  not checked somewhere, nothing found wrong: it may be used after the person has seen the warning ("未確認の校正")
//  unusable           not made, or checked and found not to fit: it is not offered
enum class RecalibrationVerdict {usable,usable_unverified,unusable};
[[nodiscard]] inline std::string_view RecalibrationVerdictCode(RecalibrationVerdict verdict){
    return verdict==RecalibrationVerdict::usable?"usable":verdict==RecalibrationVerdict::usable_unverified?"usable-unverified":"unusable";
}

// ---- The guide before the shot ----
// When to use the button, how to move the cameras, what not to touch, and the four conditions of the paper. The cameras may have been
// moved one at a time or both together, so the words speak of the interval between them.
[[nodiscard]] inline std::vector<std::string> RecalibrationGuideLines(){
    return {
        "この校正は、カメラの間隔を変えた時だけ行います。撮影のたびには要りません",
        "カメラを動かす時は、平行に動かしてください。レンズの高さ、ピントリング、ズームは触らないでください",
        "校正の時は、いつも図面を置く面に、線や模様の多い薄い紙を 1 枚、平らに置きます。紙を置いた面が、新しい基準の面になります。次の 4 点を守ってください",
        "1. 平らに置いた薄い紙を使います。冊子・厚紙・板に貼った物は使えません（紙の高さが B の位置として焼き込まれます）",
        "2. 左右のカメラの両方に写る帯（継ぎ目のあたり）に、上から下まで文字・線・図形がある紙にします",
        "3. 罫線や格子だけの紙は使えません（繰り返す模様は位置が一つに決まりません）",
        "4. 撮る前に、両方のカメラでピントを合わせます（ピントが合っていないと継ぎ目のずれを測れません）",
    };
}

// ---- The sentence for each finding ----
struct RecalibrationSentence {std::string reason,remedy;};

// What an estimator's reason code says, for the estimate of the pair, of the check on the new profile and of the sample alike (the reason
// codes of content_registration.hpp). The doubtful ones (the placement may be wrong) and the "too few marks" ones are told by the same table.
[[nodiscard]] inline RecalibrationSentence RecalibrationEstimateReasonSentence(const std::string& r){
    if(r=="unique-blocks")return {"模様が足りません","文字や図形が上から下まである紙で撮り直してください"};
    if(r=="prediction-uncertain")return {"精度が出ません","模様の多い紙で撮り直してください"};
    if(r=="residual-shape")return {"紙が浮いているか、平らでないようです","机に直に平らに置いて撮り直してください"};
    if(r=="not-converged" || r=="starts-disagree")return {"位置が一つに決まりません","紙を少し動かして撮り直してください"};
    if(r=="repeating-only")return {"繰り返す模様だけで、位置が一つに決まりません","文字や図形がある紙で撮り直してください"};
    // The coarse stage found no block whose place is single: too few marks, or a pattern that repeats (a grid, a ruled sheet).
    if(r=="coarse-no-unique-blocks")return {"位置が一つに決まる模様がありません（模様が足りないか、同じ模様の繰り返しです）","文字や図形が上から下まである紙で撮り直してください。罫線や格子だけの紙は使えません"};
    if(r=="coarse-inconsistent" || r=="coarse-ambiguous")return {"大まかな位置が一つに決まりません","紙を少し動かして、模様の多い紙で撮り直してください"};
    if(r=="too-few-points")return {"位置を測れる点が足りません","文字や図形が多い紙で撮り直してください"};
    if(r=="no-overlap")return {"左右の重なりに、位置を測る中身がありません","継ぎ目のあたりに文字や線がある紙を置いて撮り直してください"};
    if(r=="brown-lowe")return {"位置を測った点の多くが外れていて、確かな値になりません","紙を替えて撮り直してください"};
    if(r=="fit-failed")return {"位置の当てはめが成り立ちませんでした","紙を替えて撮り直してください"};
    return {"位置を確かに決められませんでした","模様の多い紙で撮り直してください"};
}

[[nodiscard]] inline RecalibrationSentence RecalibrationFindingSentence(const RecalibrationFinding& finding){
    const auto& c=finding.code;
    const auto& r=finding.reason;
    // Lane-level codes (before or after the computation).
    if(c=="no-v3-condition")return {"校正の元になる条件（v3）がありません","技術担当の作業が要ります"};
    if(c=="staging-exists")return {"校正用のフォルダが既にあります","新しいフォルダを指定してください（何も書いていません）"};
    if(c=="staging-in-settings")return {"校正用のフォルダが、設定のフォルダの中にあります","設定のフォルダの外に新しいフォルダを指定してください（何も書いていません）"};
    if(c=="live-not-stopped")return {"ライブが止まっていません","ライブを止めてからやり直してください"};
    if(c=="decode-failed")return {"撮影した画像を読めませんでした","撮り直してください"};
    if(c=="hash-mismatch")return {"画像が撮影の記録と一致しません","撮り直してください"};
    if(c=="size-mismatch")return {"画像の大きさが、校正が想定する大きさと違います","撮り直してください"};
    if(c=="profile-unreadable")return {"今の校正が指すファイルを読めません","技術担当の作業が要ります"};
    if(c=="out-of-memory")return {"メモリが足りませんでした","他のアプリを閉じてから、撮り直してください。前の校正のままです"};
    if(c=="job-failed")return {"校正の計算が途中で失敗しました","撮り直してください。続く時は技術担当へ知らせてください。前の校正のままです"};
    // The computation's refusals.
    if(c=="no-overlap")return {"左右の重なりが見つかりません","継ぎ目のあたりに文字や線がある紙を置いて撮り直してください。カメラの間が広すぎて重なりが無い事もあります"};
    if(c=="estimate-failed")return {"位置の計算が成り立ちませんでした","紙を替えて撮り直してください"};
    if(c=="estimate-not-verified")return RecalibrationEstimateReasonSentence(r);
    if(c=="tilt-exceeded")return {"カメラの向きが変わったようです（傾きが許容を超えています）","チャートでの校正が要ります。技術担当へ知らせてください"};
    if(c=="height-exceeded")return {"カメラの高さ、ピント、ズームのどれかが変わったようです","チャートでの校正が要ります。高さとピントリングとズームは触らないでください"};
    if(c=="creation-check-failed")return {"カメラの間隔が広すぎて、撮るたびの自動の合わせ込みの範囲では継ぎ目を保てません","間隔を少し狭めてください"};
    if(c=="raster-too-large")return {"合成の画像が大きすぎます","間隔を少し狭めてください"};
    if(c=="raster-outside-sheet")return {"合成の範囲が、校正が想定する用紙の外に出ます","間隔を少し狭めてください"};
    if(c=="profile-invalid")return {"新しい校正を組み立てられませんでした","撮り直してください。続く時は技術担当へ知らせてください"};
    if(c=="reestimate-displacement")return {"新しい校正で確かめたところ、継ぎ目のずれが基準を超えました","紙を替えて撮り直してください"};
    if(c=="reestimate-not-verified"){
        if(finding.klass==FindingClass::unverified)return {"新しい校正での確かめが、確かな値になりませんでした","紙を替えて撮り直すと確かめられます"};
        return {"新しい校正で確かめたところ、位置が範囲の外になりました","撮り直してください"};
    }
    // The check on the new profile found a sign that the placement does not fit (not only too few marks): the calibration is not offered.
    if(c=="reestimate-doubtful"){
        const auto s=RecalibrationEstimateReasonSentence(r);
        return {"新しい校正で確かめたところ、合っていない疑いが見つかりました（"+s.reason+"）",s.remedy};
    }
    if(c=="stopped")return {"中止しました","何も書いていません。前の校正のままです"};
    // The sample and its gate. The first group is "too little to measure": nothing was found wrong. The second is a sign that it does not fit.
    // The setting that measures the seam is inconsistent: a fault of the check, not of the paper. It is not offered.
    if(c=="gate-undetermined")return {"継ぎ目のずれを測る設定に食い違いがありました","技術担当へ知らせてください"};
    if(c=="gate-nothing-measured")return {"継ぎ目の上に測れる所が無く、継ぎ目のずれを確かめられませんでした","継ぎ目の線の上に文字や線が多い紙で撮り直すと確かめられます"};
    if(c=="gate-content-unmeasured")return {"継ぎ目の上の文字や線の多くを測れず、継ぎ目のずれを確かめきれませんでした","継ぎ目の線の上に文字や線が多い紙で撮り直すと確かめられます"};
    if(c=="gate-dx-unmeasured")return {"継ぎ目の左右のずれを測れる線（縦線）が無く、確かめきれませんでした","縦線や文字がある紙で撮り直すと確かめられます"};
    if(c=="gate-placement-unverified")return {"左右の写真の位置が合っているかを、模様では確かめきれませんでした","模様の多い紙で撮り直すと確かめられます"};
    // The check did not work (the reason is the time budget's or the stage's): it is not offered. A time-out is cured by pressing again (the button of the result screen).
    if(c=="gate-not-run"){
        if(r=="time-budget")return {"継ぎ目のずれの確かめが時間内に終わりませんでした","もう一度「撮り直す」を押してください"};
        return {"継ぎ目のずれの確かめの処理が止まりました","技術担当へ知らせてください"};
    }
    if(c=="gate-fail")return {"継ぎ目のずれが基準を超えました","紙を替えて撮り直してください"};
    if(c=="gate-unconfirmed-content")return {"基準を超えた所があり、測り直しでは確かめられませんでした","紙を平らに置き直して撮り直してください"};
    if(c=="gate-one-sided-content")return {"片方のカメラにだけ写っている所があります（紙が浮いているか、位置が合っていません）","紙を平らに置き直して撮り直してください"};
    if(c=="gate-transition-outside-band")return {"継ぎ目の混ぜ合わせの幅が、想定の範囲の外です","技術担当へ知らせてください"};
    if(c=="gate-unknown")return {"継ぎ目の確かめで、想定していない結果が出ました","技術担当へ知らせてください"};
    if(c=="sample-estimate-undetermined")return {"見本の合成で、位置の推定が確かになりませんでした","紙を替えて撮り直すと確かめられます"};
    if(c=="sample-estimate-doubtful"){
        const auto s=RecalibrationEstimateReasonSentence(r);
        // The gate may have passed: the line of the seam says "within the line" beside this sentence, so it says that this does not save it.
        return {"見本の合成で、位置の推定に食い違いの印が見つかりました（"+s.reason+"）。継ぎ目のずれが基準内でも、この校正は使えません",s.remedy};
    }
    if(c=="sample-estimate-out-of-envelope")return {"見本の合成で、位置が範囲の外になりました","撮り直してください"};
    if(c=="sample-skipped")return {"見本を合成できませんでした（今の条件に合いません）","撮り直してください。続く時は技術担当へ知らせてください"};
    if(c=="sample-failed")return {"見本の合成に失敗しました（時間かメモリが足りなかった可能性があります）","撮り直してください"};
    return {"理由を特定できませんでした","撮り直してください"};
}

// ---- The result screen ----
struct RecalibrationGateFacts {
    bool known{};
    a0::m2::experimental::registration_gate::GateState state{a0::m2::experimental::registration_gate::GateState::not_run};
    a0::m2::experimental::registration_gate::GateCause cause{a0::m2::experimental::registration_gate::GateCause::none};
    std::optional<double> maximum_px;
    double limit_px{std::numeric_limits<double>::quiet_NaN()};
    double measured_share{std::numeric_limits<double>::quiet_NaN()};   // measured rows / rows with content, 0..1
};
struct RecalibrationResultFacts {
    a0::m2::experimental::recalibration::Display display;
    RecalibrationGateFacts gate;
    bool creation_checked{};
};

namespace recalibration_text_detail {
using registration_text_detail::Q;
using registration_text_detail::Approx;
using registration_text_detail::Unit;
inline bool Present(double mm){return std::isfinite(mm) && std::abs(mm)>=0.05;}
// "+0.2 %" / "-0.2 %"
inline std::string Signed(double value,Unit unit){
    const auto text=Q(std::abs(value),unit);
    if(text=="不明")return text;
    return std::string(value<0?"-":"+")+text;
}

// B's place against A, as the drawing shows it.
inline std::string MotionLine(const a0::m2::experimental::recalibration::Display& d){
    if(!d.has_motion)return "A に対する B の位置: 求められませんでした";
    const auto& m=d.motion.camera_motion_mm;
    std::string text="A に対する B の位置の変化: ";
    if(!Present(d.pose.position_mm) && !Present(m.x) && !Present(m.y)){
        text+="位置はほとんど変わっていません";
    }else{
        // Left-right and up-down are told apart: the person compares each with the way the cameras were moved.
        text+=Present(m.x)?"図面の"+std::string(m.x>0?"右":"左")+"へ"+Approx(m.x,Unit::mm):std::string("左右はほぼ同じ");
        text+="、";
        text+=Present(m.y)?"上下は"+std::string(m.y>0?"下":"上")+"へ"+Approx(m.y,Unit::mm):std::string("上下はほぼ同じ");
    }
    const double turn=d.motion.rotation_clockwise_deg;
    if(std::isfinite(turn) && std::abs(turn)>=0.05)text+="、回り "+Approx(turn,Unit::deg)+"（画面で見て"+registration_text_detail::ClockwiseWord(turn)+"）";
    if(std::isfinite(d.pose.tilt_deg) && d.pose.tilt_deg>=0.05)text+="、傾き "+Approx(d.pose.tilt_deg,Unit::deg);
    if(std::isfinite(d.pose.height_percent) && std::abs(d.pose.height_percent)>=0.005)text+="、高さ "+Signed(d.pose.height_percent,Unit::percent);
    return text;
}
inline std::string SeamLine(const a0::m2::experimental::recalibration::Display& d){
    if(!d.has_new)return {};   // the new side is not whole (a refusal before the plan was made): there is no new seam to speak of
    const double delta=d.seam_x_new_mm-d.seam_x_previous_mm;
    if(!std::isfinite(delta))return {};
    if(!Present(delta))return "継ぎ目の位置は変わりません";
    return "継ぎ目の位置が変わります: 図面の上で"+std::string(delta>0?"右":"左")+"へ"+Approx(delta,Unit::mm)+"（検品の時に見る場所が変わります）";
}
inline std::string RasterLine(const a0::m2::experimental::recalibration::Display& d){
    if(!d.has_new)return {};
    const double grown=d.right_new_mm-d.right_previous_mm;
    std::string text="合成の画像の大きさ: 幅 "+std::to_string(d.width_previous_px)+" → "+std::to_string(d.width_new_px)+" 画素";
    if(Present(grown))text+="（右の端が"+Approx(grown,Unit::mm)+(grown>0?" 広がります":" 狭まります")+"）";
    text+=d.height_previous_px==d.height_new_px?"、高さは同じ":"、高さ "+std::to_string(d.height_previous_px)+" → "+std::to_string(d.height_new_px)+" 画素";
    return text;
}
inline std::string GateLine(const RecalibrationGateFacts& g){
    using S=a0::m2::experimental::registration_gate::GateState;
    if(!g.known)return "継ぎ目のずれ: 見本を合成できなかったため、確かめていません";
    const std::string rows=std::isfinite(g.measured_share)?"。測れた行 "+Q(g.measured_share*100.0,Unit::percent):std::string{};
    const std::string maximum=g.maximum_px?"最大 "+Q(*g.maximum_px,Unit::px):std::string{};
    const std::string line=std::isfinite(g.limit_px)?"基準 "+Q(g.limit_px,Unit::px):std::string{};
    std::string inside=maximum;
    if(!maximum.empty() && !line.empty())inside+="、";
    inside+=line;
    // Only what was measured is said here. Whether the seam was checked, and whether the calibration can be used, is the finding's sentence
    // (the class comes from the findings, and this line is not a second copy of it).
    const std::string measured=maximum.empty()?std::string("測れた値はありません"):"測れた所の"+maximum+(line.empty()?std::string{}:"（"+line+"）");
    switch(g.state){
    case S::pass: return "継ぎ目のずれ: 基準内"+(inside.empty()?std::string{}:"（"+inside+"）")+rows;
    case S::fail:
        // The gate calls a seam that it could not measure a failure too: only a seam over the line is said to be over it.
        if(g.cause==a0::m2::experimental::registration_gate::GateCause::unmeasurable)return "継ぎ目のずれ: "+measured+rows;
        return "継ぎ目のずれ: 基準を超えました"+(inside.empty()?std::string{}:"（"+inside+"）")+rows;
    case S::undetermined: return "継ぎ目のずれ: "+measured+rows;
    case S::not_run: return "継ぎ目のずれ: 測っていません";
    }
    return {};
}
}

// The numbers of the result screen, one line each (2.3 of the design), in the order the person reads them. A line that has nothing
// to say is left out.
[[nodiscard]] inline std::vector<std::string> RecalibrationResultLines(const RecalibrationResultFacts& facts){
    using namespace recalibration_text_detail;
    const auto& d=facts.display;
    std::vector<std::string> lines;
    const auto add=[&](std::string line){if(!line.empty())lines.push_back(std::move(line));};
    // The hand adjustment in force was beyond the range (the reader says "roll back, or calibrate again"; this is the second): the changes
    // below are counted from the profile's own placement, not from that adjustment.
    if(d.previous_outside_envelope)add("前の手直しは範囲の外だったため、校正の位置からの変化を出します");
    add(MotionLine(d));
    if(d.has_overlap)add("重なりの幅: いちばん狭い行で"+Approx(d.narrowest_overlap_mm,Unit::mm));
    add(SeamLine(d));
    add(RasterLine(d));
    if(d.has_edge_distance)
        add("継ぎ目から写真の端まで: A は幅の "+Q(d.edge_share_a*100.0,Unit::percent)+"、B は幅の "+Q(d.edge_share_b*100.0,Unit::percent)+
            "（端に近いほど、レンズの校正が確かでない所です）");
    add(GateLine(facts.gate));
    if(facts.creation_checked)add("範囲の検査: 通りました（撮るたびの自動の合わせ込みが、今の範囲で効きます）");
    return lines;
}

// The first line of the screen and the lines of reasons under it.
[[nodiscard]] inline std::string RecalibrationVerdictLine(RecalibrationVerdict verdict){
    switch(verdict){
    case RecalibrationVerdict::usable: return "この校正は使えます";
    case RecalibrationVerdict::usable_unverified:
        // What could not be checked is told by the lines of reasons below; this line must not name it (the gate may have passed).
        return "未確認の校正です。数と見本を見て、使うかどうかを決めてください";
    case RecalibrationVerdict::unusable: return "この校正は使えません";
    }
    return {};
}
[[nodiscard]] inline std::vector<std::string> RecalibrationFindingLines(const std::vector<RecalibrationFinding>& findings){
    std::vector<std::string> lines;
    for(const auto& finding:findings){
        const auto sentence=RecalibrationFindingSentence(finding);
        lines.push_back(sentence.reason+"。"+sentence.remedy);
    }
    return lines;
}
// Everything the screen shows as text, top to bottom: the verdict, why, then the numbers.
[[nodiscard]] inline std::vector<std::string> RecalibrationScreenLines(RecalibrationVerdict verdict,const std::vector<RecalibrationFinding>& findings,
    const RecalibrationResultFacts& facts){
    std::vector<std::string> lines{RecalibrationVerdictLine(verdict)};
    for(auto& line:RecalibrationFindingLines(findings))lines.push_back(std::move(line));
    for(auto& line:RecalibrationResultLines(facts))lines.push_back(std::move(line));
    return lines;
}

// ---- After the person decides ----
[[nodiscard]] inline std::string RecalibrationConfirmedText(std::uint32_t sequence,bool unverified){
    return std::string(unverified?"未確認の校正として更新しました":"校正を更新しました")+"（改訂 "+std::to_string(sequence)+"）。ライブを開始し直すと、新しい校正を読みます";
}
// `reason` is the code of what stopped the confirmation (ASCII).
[[nodiscard]] inline std::string RecalibrationNotConfirmedText(std::string_view reason){
    if(reason=="condition-changed")return "条件が変わっています。撮り直してください（前の校正のままです）";
    if(reason=="not-usable")return "この校正は使えない結果です。前の校正のままです";
    if(reason=="not-acknowledged")return "確かめられていない校正です。警告を見たうえで使うと決めた時だけ更新できます。前の校正のままです";
    if(reason=="readback-failed")return "書いた後の確認で、新しい校正が有効になっていませんでした。技術担当へ知らせてください";
    return "書けませんでした。前の校正のまま使います";
}

// ---- A hint on the status row (2.9) ----
// For the screen's status row and the C area when the cameras seem to have moved a lot. The words say "the interval between the cameras"
// because either camera may have been moved. The condition (the build has the button) is the caller's.
struct RecalibrationNudgeFacts {
    bool seam_outside_overlap{};          // the composite was not made: FixedCompositeSkip::seam_outside_overlap
    bool position_exceeded{};             // the registration warnings hold a position that exceeded its range
    bool undetermined_at_level_g4{};      // the estimate is undetermined and the level fell to G4
    bool gate_cause_pose{};               // the gate failed with the cause `pose`
};
[[nodiscard]] inline bool RecalibrationNudgeApplies(const RecalibrationNudgeFacts& f){
    return f.seam_outside_overlap || f.position_exceeded || f.undetermined_at_level_g4 || f.gate_cause_pose;
}
[[nodiscard]] inline std::string RecalibrationNudgeText(){
    return "カメラの位置が大きく変わったようです。カメラの間隔を変えた時は「校正をやり直す」を押してください";
}
}
