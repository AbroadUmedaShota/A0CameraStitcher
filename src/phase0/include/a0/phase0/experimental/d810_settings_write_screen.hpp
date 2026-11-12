#pragma once
// The screen of the camera settings write (#416 stage B, ADR-0038): which
// controller call each button makes, what the operator is shown before and
// after a copy, and the admission of a capture. No window code: the development
// preview draws it (src/phase0/experimental/wpd_settings_write_ui.hpp), and the
// contract tests drive it with their tables and synthetic cameras.
//
// Threads: one thread (the screen's) calls every member. One worker thread at a
// time runs one controller call (the read for a plan, Execute or Reread);
// while it runs nothing else calls the controller, and its result comes back
// through a future (D810SettingsWriteController is not thread-safe and relies
// on this). The controller's Confirm is called from ConfirmShownPlan only, and
// the screen calls that from its confirm button only.
//
// There is no "undo" and no other way in. The values an operator picks in the panel are a draft: they reach a camera only as a
// plan that the confirm button confirms (D810SettingsWriteController::PlanChosen, then Confirm and Execute).
#if !defined(A0_ENABLE_WPD_SETTINGS_WRITE)
#error "d810_settings_write_screen.hpp is only for targets built with A0_BUILD_WPD_SETTINGS_WRITE"
#endif
#include "a0/phase0/experimental/wpd_settings_write.hpp"
#include <future>
#include <string_view>

namespace a0::phase0::experimental {
// --- Words on the screen ---------------------------------------------------------------
inline std::wstring D810WidenAscii(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char c : text) out.push_back(static_cast<unsigned char>(c) < 0x80 ? static_cast<wchar_t>(c) : L'?');
    return out;
}
inline const wchar_t* D810WriteItemLabel(D810WriteItem item) {
    switch (item) {
    case D810WriteItem::ExposureMode: return L"露出モード";
    case D810WriteItem::IsoAuto: return L"ISO感度自動制御";
    case D810WriteItem::Iso: return L"ISO感度";
    case D810WriteItem::ShutterSpeed: return L"シャッター速度";
    case D810WriteItem::Aperture: return L"絞り";
    case D810WriteItem::WhiteBalance: return L"ホワイトバランス";
    case D810WriteItem::Metering: return L"測光";
    case D810WriteItem::ExposureCompensation: return L"露出補正";
    }
    return L"";
}
// The label of an item name of the tokens ("aperture", "iso_auto", ...), or the name.
inline std::wstring D810WriteItemNameLabel(std::string_view name) {
    for (const auto& spec : kD810WriteItems)
        if (name == spec.name) return D810WriteItemLabel(spec.item);
    return D810WidenAscii(name);
}
inline std::wstring D810WriteValueText(std::uint16_t code, std::int64_t value) {
    const auto* spec = FindD810Setting(code);
    return D810WidenAscii(spec ? FormatD810Setting(*spec, value) : D810Hex(static_cast<std::uint64_t>(value), 4));
}
inline const wchar_t* D810WriteVerdictText(D810WriteVerdict verdict) {
    switch (verdict) {
    case D810WriteVerdict::NotSent: return L"送っていない";
    case D810WriteVerdict::Match: return L"一致";
    case D810WriteVerdict::Mismatch: return L"不一致";
    case D810WriteVerdict::Unknown: return L"不明";
    }
    return L"不明";
}
inline std::wstring D810CaptureBlockBaseText(D810CaptureBlock block, bool chosen_history) {
    switch (block) {
    case D810CaptureBlock::None: return {};
    case D810CaptureBlock::Mismatch:
        if (chosen_history)
            return L"撮影を止めています（不一致）: 書いた値が、読み戻しで確かめられていません。"
                   L"ライブを止めて「設定を読み直す」を押すと、各台が書いた値のまま読めた時に撮影できます。";
        return L"撮影を止めています（不一致）: 設定の書き込みのあと、CAM-B の露出の設定が CAM-A と合っていません。"
               L"ライブを止めて「設定を読み直す」を押し、両台が同じと確かめると撮影できます。";
    case D810CaptureBlock::Unknown:
        return L"撮影を止めています（不明）: 設定の書き込みの結果を確かめられていません。"
               L"ライブを止めて「設定を読み直す」を押してください。";
    case D810CaptureBlock::Unconfirmed:
        return L"撮影を止めています（未確認）: 前回の設定の書き込みが終わったことを確かめられていません。"
               L"ライブを止めて「設定を読み直す」を押してください。";
    case D810CaptureBlock::Unreadable:
        return L"撮影を止めています: 設定の書き込みの記録を読めません。この画面からは解けません。"
               L"記録のファイルを人が確かめてください（消さない・動かさないでください）。";
    }
    return L"撮影を止めています。";
}
// What to do when part of the record cannot be read: the cameras are compared (the rule of a copy) whatever the history holds.
inline constexpr const wchar_t* kD810RecordDamagedAdvice = L"操作の記録の一部を読めない、または計画と合わないので、両台の止めの対象の項目を本体で同じにしてから読み直してください。";
// Why capture is held, in the words of docs/WPD_DUAL_PREVIEW.md. Empty when it is not. `chosen_history`: the record holds
// a plan of picked values that is read back against the values written (not a difference of the two cameras).
// `history_damaged`: part of the record cannot be read; the cameras are then compared as for a copy, and the advice says so.
inline std::wstring D810CaptureBlockText(D810CaptureBlock block, bool chosen_history = false, bool history_damaged = false) {
    auto text = D810CaptureBlockBaseText(block, chosen_history);
    if (history_damaged && !text.empty() && block != D810CaptureBlock::Unreadable) text += std::wstring(L" ") + kD810RecordDamagedAdvice;
    return text;
}
// Operator text for one ASCII token of a plan, a run or a reread. The token is
// kept in brackets for the log reader; tokens hold fixed words, item names and
// camera aliases only.
inline std::wstring D810WriteReasonText(std::string_view token) {
    struct Entry { std::string_view prefix; const wchar_t* text; };
    static constexpr Entry kEntries[] = {
        {"code_unconfirmed", L"項目の番号（code）が本体でまだ確かめられていないため、書けません"},
        {"row_disabled", L"表で無効の項目です（書きません）"},
        {"copy_disabled", L"「写す」は表で無効です"},
        {"copy_targets_cam_b_only", L"写す相手は CAM-B だけです"},
        {"copy_item_out_of_scope", L"「写す」の対象の項目ではありません"},
        {"left_out_by_operator", L"外した項目です"},
        {"invalid_table", L"書き込みの表が正しくないため、書けません"},
        {"reread_first", L"前回の書き込みが確かめられていません。先に「設定を読み直す」を押してください"},
        {"a_not_manual", L"CAM-A が M（マニュアル露出）ではありません。A の本体で M にしてから、もう一度押してください"},
        {"a_iso_auto_not_off", L"CAM-A の ISO感度自動制御が OFF ではありません。A の本体で OFF にしてから、もう一度押してください"},
        {"a_shutter_unreadable", L"CAM-A のシャッター速度を読めませんでした"},
        {"a_shutter_bulb_or_time", L"CAM-A のシャッター速度が Bulb / Time のため写せません"},
        {"a_shutter_exposure_zero", L"CAM-A のシャッター速度の値が正しくありません"},
        {"a_shutter_exposure_too_long", L"CAM-A のシャッター速度が 1 秒より長いため写せません"},
        {"exposure_mode_not_manual_set_on_body", L"CAM-B が M ではなく、露出モードは書けません。B の本体で M にしてください"},
        {"iso_auto_not_off", L"CAM-B の ISO感度自動制御が OFF ではなく、書けません。B の本体で OFF にしてください"},
        {"unreadable", L"読めない項目があります"},
        {"value_not_advertised", L"CAM-B がその値を受け付けると答えていません"},
        {"not_writable", L"CAM-B のその項目は書けない状態です"},
        {"type_mismatch", L"本体が答えた型が表と違います"},
        {"value_content_dependent", L"本体に登録した内容に頼る値のため写せません。色が揃わない事があります。B の本体で合わせてください"},
        {"value_not_listed", L"この画面で写せる値の一覧にない値です。CAM-B の値は変えません"},
        {"value_", L"その値は書けません"},
        {"plan_not_confirmed", L"確定した計画がありません"},
        {"invalid_options", L"内部の設定が足りないため、何も送っていません"},
        {"cameras_busy", L"カメラを別の処理が使っています"},
        {"record_unwritable", L"書き込みの記録をディスクに書けませんでした"},
        {"reread_incomplete", L"カメラを最後まで読めませんでした"},
        {"read_incomplete", L"カメラを最後まで読めませんでした"},
        {"body_not_verified", L"書く相手の本体の機種・版が表と違います（本体が未確認）"},
        {"plan_changed", L"確定のあとに本体の値が変わりました。もう一度「写す」から作り直してください"},
        {"operation_latched", L"前の項目が失敗したため、残りは送っていません"},
        {"open_failed", L"カメラを開けませんでした（登録と合わない、またはつながっていません）"},
        {"close_failed", L"カメラとの接続を閉じられませんでした"},
        {"device_not_ready", L"カメラの準備ができませんでした"},
        {"interrupted", L"停止の指示か期限で止めました"},
        {"deadline_margin", L"残り時間が足りないため、次の書き込みを送りませんでした"},
        {"live_state_unreadable", L"ライブビューの状態を読めませんでした"},
        {"live_view_on", L"CAM-B のライブビューが動いています"},
        {"desc_unreadable", L"項目を読み直せませんでした"},
        {"changed_before_write", L"書く直前に CAM-B の値が変わっていました"},
        {"refused_", L"送る前に止めました"},
        {"result_unknown", L"書いた結果を確かめられませんでした（不明）"},
        {"readback_mismatch", L"読み戻した値が書いた値と違いました（不一致）"},
        {"settings_differ", L"CAM-A と CAM-B で違う項目があります"},
        {"com_init_failed", L"処理を始められませんでした"},
        {"exception", L"処理の途中で失敗しました"},
    };
    for (const auto& entry : kEntries)
        if (token.substr(0, entry.prefix.size()) == entry.prefix) return std::wstring(entry.text) + L"（" + D810WidenAscii(token) + L"）";
    return L"理由の符号: " + D810WidenAscii(token);
}
// The notice of a plan of picked values that has a white balance step: only the kind is written (the fine adjustment, the type of Automatic and
// the type of Fluorescent are other properties), so the colour need not match whatever the kinds are. It is added to the line that names the columns,
// not to the item line (the item line of the widest values already fills the window, and the line count of a confirmation is at its limit).
inline const wchar_t* D810ChosenWhiteBalanceNote() { return L"（WB は種類だけを書きます。色まで揃うとは限りません）"; }
// One step of a plan or a result: the item, the value now on CAM-B and the value to write.
inline std::wstring D810WriteStepLine(const D810WriteStep& step) {
    std::wstring line = std::wstring(D810WriteItemLabel(step.item)) + L": CAM-B の今の値 " + D810WriteValueText(step.code, step.before) + L" → 書く値 " +
        D810WriteValueText(step.code, step.value);
    // The kind only: the fine adjustment of each kind, the type of Automatic and the type of Fluorescent are other
    // properties and are not copied. The notice is on the step's own line (the line count of a confirmation is unchanged).
    if (step.item == D810WriteItem::WhiteBalance) line += L"（種類だけを写します。微調整・自動の型・蛍光灯の型は写りません）";
    return line;
}
// One item a plan leaves out, with the reason.
inline std::wstring D810WriteSkippedLine(D810WriteItem item, std::string_view reason) {
    return std::wstring(L"  ") + D810WriteItemLabel(item) + L": " + D810WriteReasonText(reason);
}
// The reasons of a plan of picked values (and of its run) whose words do not name CAM-B: the picks go to either camera.
// Everything else is told as for a copy.
inline const wchar_t* D810ChosenReasonText(std::string_view what) {
    struct Entry { std::string_view token; const wchar_t* text; };
    static constexpr Entry kEntries[] = {
        {"chosen_values_disabled", L"この版では、選んだ値を書くことはできません"},
        {"no_choice", L"書く値が選ばれていません"},
        {"plan_changed", L"確定のあとに、本体の値が変わりました（書く台、または「A と同じ」の元にした CAM-A）。もう一度「N 件を書き込む…」から作り直してください"},
        {"read_not_current", L"書く前の読み取りが古いか、最後まで終わっていないため、計画を作りませんでした。もう一度「N 件を書き込む…」を押してください"},
        {"item_not_choosable", L"この項目は、選んで書く対象ではありません"},
        {"same_as_a_on_cam_a", L"CAM-A を「A と同じ」にはできません"},
        {"same_as_a_not_offered", L"CAM-A の値を、この台は受け付けると答えていません"},
        {"duplicate_choice", L"同じ項目を 2 回選んでいます"},
        {"value_not_named", L"名前を確かめていない値は選べません"},
        {"value_out_of_band", L"選べる範囲の外の値です"},
        {"value_not_advertised", L"この台が、その値を受け付けると答えていません"},
        {"not_writable", L"この台のその項目は、書けない状態です"},
        {"live_view_on", L"書く相手の台のライブビューが動いています"},
        {"changed_before_write", L"書く直前に、書く相手の台の値が変わっていました"},
        {"plan_order", L"計画の台の順が正しくないため、何も送っていません"},
        {"plan_step_code", L"計画の項目の番号が正しくないため、何も送っていません"},
    };
    for (const auto& entry : kEntries)
        if (entry.token == what) return entry.text;
    return nullptr;
}
// "<what>:<item>:<CAM-x>" from a plan, with the item's label. `chosen`: the plan is one of picked values.
inline std::wstring D810PlanReasonText(const std::string& reason, bool chosen = false) {
    const auto first = reason.find(':');
    const auto what = reason.substr(0, first);
    const auto* chosen_text = chosen ? D810ChosenReasonText(what) : nullptr;
    const auto told = [&] { return chosen_text ? std::wstring(chosen_text) + L"（" + D810WidenAscii(what) + L"）" : D810WriteReasonText(what); };
    if (first == std::string::npos) return told();
    const auto second = reason.find(':', first + 1);
    const auto item = reason.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
    const auto camera = second == std::string::npos ? std::string() : reason.substr(second + 1);
    // The two preconditions name the camera in the place of the item ("...:CAM-A"), and the advice is for that body.
    if (chosen && second == std::string::npos && (what == "exposure_mode_not_manual_set_on_body" || what == "iso_auto_not_off")) {
        const auto body = D810WidenAscii(item);
        return what == "iso_auto_not_off" ? body + L": " + body + L" の ISO感度自動制御が OFF ではないため、書けません。" + body + L" の本体で OFF にしてください（iso_auto_not_off）"
                                          : body + L": " + body + L" が M ではないため、露出モードは書けません。" + body + L" の本体で M にしてください（exposure_mode_not_manual_set_on_body）";
    }
    std::wstring subject = D810WriteItemNameLabel(item);
    if (!camera.empty()) subject = D810WidenAscii(camera) + L" の" + subject;
    return subject + L": " + told();
}

// --- The settings panel: words and rows ----------------------------------------------------
inline const wchar_t* D810ChoiceItemLabel(D810ChoiceItem item) { return D810WriteItemLabel(D810ChoiceWriteItem(item)); }
// Why a cell of the panel cannot be picked, in a few words (the cell's note): the tokens are those of
// D810PanelCell::locked_reason.
inline std::wstring D810PanelLockText(const char* reason) {
    struct Entry { std::string_view token; const wchar_t* text; };
    static constexpr Entry kEntries[] = {
        {"unreadable", L"読めません"},
        {"not_writable", L"書けない状態です"},
        {"chosen_values_disabled", L"この版では選べません"},
        {"row_disabled", L"表で無効の項目です"},
        {"code_unconfirmed", L"番号が未確認です"},
        {"body_not_verified", L"本体が未確認です"},
        {"exposure_mode_not_manual", L"本体を M に"},
        {"iso_auto_not_off", L"自動制御を OFF に"},
        {"no_options", L"選べる値がありません"},
    };
    if (!reason) return {};
    for (const auto& entry : kEntries)
        if (entry.token == reason) return entry.text;
    return D810WidenAscii(reason);
}
// One row of the panel as it is drawn: a heading, a row that is only read, or a row with one drop-down per camera.
struct D810PanelRowView {
    std::wstring label;
    bool choosable{};
    D810ChoiceItem item{};
    // Read-only rows: the value of each camera ("M", "OFF", "読めません"). Choosable rows draw the drop-downs instead.
    std::array<std::wstring, 2> value;
    // The word beside each camera's cell: why it cannot be picked, or that a change is planned. Empty when neither.
    std::array<std::wstring, 2> note;
    // "=" the two cameras read the same, "≠" they differ, "?" one of them is not readable.
    std::wstring difference;
};

// --- The screen ------------------------------------------------------------------------
enum class D810WriteScreenStep : std::uint8_t { Idle, ReadingForPlan, Confirming, Writing, Rereading, ReadingForPanel, ReadingForChosen };
// What the screen draws in place of C: the confirmation before a copy, or the
// last result. Plain lines, top to bottom.
struct D810WriteScreenView {
    std::wstring title;
    std::vector<std::wstring> lines;
    bool confirmation{};
};
// How many lines of a view fit under its title in a window whose client area is `client_bottom` high (22 px a
// line, from 30 px under the title to 84 px above the bottom, the title at the middle of the window plus 88 px).
// The preview draws this many and offers the confirm button only when every line of a confirmation fits.
constexpr std::size_t D810WriteViewVisibleLines(int client_bottom) {
    const int first = client_bottom / 2 + 88 + 30, bottom = client_bottom - 84;
    return bottom < first ? 0 : static_cast<std::size_t>((bottom - first) / 22);
}
inline bool D810WriteViewFits(const D810WriteScreenView& view, int client_bottom) { return view.lines.size() <= D810WriteViewVisibleLines(client_bottom); }
// The most lines a confirmation can have: the three opening lines, one line per step, the skipped heading with one
// line per skipped item, and the closing line. Every item the copy plan walks (all but the chosen-value item) is
// either a step or a skipped one, so that is 5 + (the number of those items) at most.
constexpr std::size_t D810WriteViewMaxLines() {
    std::size_t items = 0;
    for (const auto& spec : kD810WriteItems)
        if (spec.scope != D810ItemScope::Adr41) ++items;
    return 5 + items;
}
// The smallest client height at which `lines` lines are visible (the inverse of D810WriteViewVisibleLines).
constexpr int D810WriteViewMinClientBottom(std::size_t lines) {
    for (int bottom = 0; bottom < 8000; ++bottom)
        if (D810WriteViewVisibleLines(bottom) >= lines) return bottom;
    return 8000;
}
// The window's smallest client area when a row is enabled: the height shows the longest confirmation, the width holds
// the button row and the notice lines in the default font (a line still cut at this width is not drawn whole).
inline constexpr int kD810WriteMinClientWidth = 1240;
inline constexpr int kD810WriteMinClientHeight = D810WriteViewMinClientBottom(D810WriteViewMaxLines());
// The confirm button: its left edge, size and distance from the bottom of the client area (the preview lays it out
// with these). It is pressable only when it lies inside the client area.
inline constexpr int kD810ConfirmButtonLeft = 432, kD810ConfirmButtonWidth = 200, kD810ConfirmButtonHeight = 28, kD810ButtonRowGap = 40;
constexpr bool D810ConfirmButtonInside(int client_width, int client_bottom) {
    return client_bottom >= kD810ButtonRowGap && client_width >= kD810ConfirmButtonLeft + kD810ConfirmButtonWidth;
}
// Whether the confirm button may be pressed: the one decision both the button state and its handler ask. A
// confirmation is presented; the last present of the window drew exactly this one whole (`drawn_serial` is the serial
// of the view that present drew completely, 0 when none, so a view of an earlier serial, a window too small to draw
// and a failed present all leave it different); every line still fits the height of the window as it is now; and the
// button is inside the window. Anything else is not pressable (fail closed).
inline bool D810ConfirmPressable(const D810WriteScreenView* view, std::uint64_t view_serial, std::uint64_t drawn_serial,
    int client_width, int client_bottom) {
    if (!view || !view->confirmation || view_serial == 0 || drawn_serial != view_serial) return false;
    return D810WriteViewFits(*view, client_bottom) && D810ConfirmButtonInside(client_width, client_bottom);
}
// --- Words and lines of a plan of picked values (#416, ADR-0041) -----------------------------------------------------
inline std::wstring D810WidenUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return D810WidenAscii(text);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}
// A value as the drop-down shows it, for the items that can be picked; otherwise the plain value text.
inline std::wstring D810ChoiceValueText(D810WriteItem item, std::uint16_t code, std::int64_t value) {
    if (const auto choice = D810ChoiceItemOf(item)) return D810WidenUtf8(D810ChoiceLabel(*choice, value));
    return D810WriteValueText(code, value);
}
// The value a camera reads now: the drop-down's label when the application could pick it back, otherwise the value
// as the camera reports it (a white balance Preset, for example), which the application cannot set again.
inline std::wstring D810ChosenBeforeText(const D810WriteStep& step, const D810SettingsSnapshot& snapshot, bool& restorable) {
    const auto choice = D810ChoiceItemOf(step.item);
    const auto* reading = AvailableD810Reading(snapshot, step.code);
    restorable = choice && reading && D810ChoosableRefusal(*choice, reading->desc, step.before) == nullptr;
    return restorable ? D810ChoiceValueText(step.item, step.code, step.before) : D810WriteValueText(step.code, step.before);
}
// The items of a plan or a run in the order its steps first name them (CAM-B's items, then CAM-A's others).
template <class Steps, class Item>
inline std::vector<D810WriteItem> D810ItemsInStepOrder(const Steps& steps, Item item_of) {
    std::vector<D810WriteItem> order;
    for (const auto& step : steps) {
        const auto item = item_of(step);
        if (std::find(order.begin(), order.end(), item) == order.end()) order.push_back(item);
    }
    return order;
}
// The confirmation of a plan of picked values: the notices of ADR-0038 decision 5 first, the order the cameras are
// written in, then one line per item with each camera's value now and the value to write, in the order the plan's
// steps name them (CAM-B's steps come first in a plan, and the lines follow them; they are not sorted again).
// `model`: the panel's model, to say that a camera's value changed after the panel was read.
inline D810WriteScreenView BuildD810ChosenConfirmation(const D810WritePlan& plan, const std::array<D810SettingsSnapshot, 2>& snapshots, const D810PanelModel* model) {
    D810WriteScreenView view;
    view.confirmation = true;
    view.title = L"選んだ値を書く前の確認（まだ何も書いていません）";
    view.lines.push_back(L"自動では元に戻りません。書く前の値に戻す時は、本体で操作してください。");
    view.lines.push_back(L"書き込みは項目ごとに 1 回だけです。失敗しても再送しません。結果を確かめられない時は撮影を止めます。");
    view.lines.push_back(L"書く順: CAM-B の項目を先に、CAM-A の項目を最後に書きます。どれかが失敗したら、残りは送りません。");
    const bool has_white_balance = std::any_of(plan.steps.begin(), plan.steps.end(), [](const D810WriteStep& step) { return step.item == D810WriteItem::WhiteBalance; });
    view.lines.push_back(std::wstring(L"項目: CAM-B の今の値 → 書く値 ／ CAM-A の今の値 → 書く値") + (has_white_balance ? D810ChosenWhiteBalanceNote() : L""));
    const auto order = D810ItemsInStepOrder(plan.steps, [](const D810WriteStep& step) { return step.item; });
    bool a_written = false, changed = false;
    for (const auto item : order) {
        std::array<const D810WriteStep*, 2> by_camera{};
        for (const auto& step : plan.steps)
            if (step.item == item) by_camera[static_cast<std::size_t>(step.camera)] = &step;
        std::wstring line = std::wstring(L"  ") + D810WriteItemLabel(item) + L":";
        bool restorable_all = true;
        for (const auto camera : {D810Camera::B, D810Camera::A}) {
            const auto* step = by_camera[static_cast<std::size_t>(camera)];
            line += camera == D810Camera::B ? L" CAM-B " : L" ／ CAM-A ";
            if (!step) { line += L"変えない"; continue; }
            bool restorable = true;
            line += D810ChosenBeforeText(*step, snapshots[static_cast<std::size_t>(camera)], restorable) + L" → " + D810ChoiceValueText(step->item, step->code, step->value);
            restorable_all = restorable_all && restorable;
            a_written = a_written || camera == D810Camera::A;
            if (model)
                if (const auto choice = D810ChoiceItemOf(step->item)) {
                    const auto& cell = model->At(camera, *choice);
                    changed = changed || !cell.readable || cell.current != step->before;
                }
        }
        if (!restorable_all) line += L"（元の値は本体でしか戻せません）";
        view.lines.push_back(std::move(line));
    }
    if (a_written) view.lines.push_back(L"CAM-A は合成と校正の基準のカメラです。承認済みの撮影の設定と違う値になると、撮影の前の照合で止まります。");
    if (changed) view.lines.push_back(L"設定の画面を開いた後に、本体の値が変わった項目があります。");
    view.lines.push_back(L"よければ「書き込む（1回だけ）」、やめる時は「やめる」を押してください。");
    return view;
}
// One symbol of the difference list of a release by the values written ("<item>:<CAM-x>:<why>", "nothing_planned",
// "record_unreadable"), as a line of the result. A symbol this screen does not know is shown as it is.
inline std::wstring D810ChosenDifferenceText(const std::string& difference) {
    if (difference == "record_unreadable") return L"  操作の記録を読めないので、解けません。記録のファイルは消さない・動かさないで、人が確かめてください";
    if (difference == "nothing_planned") return L"  書く予定だった値が記録に残っていません。本体で合わせてから、もう一度押してください";
    const auto first = difference.find(':');
    const auto second = first == std::string::npos ? std::string::npos : difference.find(':', first + 1);
    if (second == std::string::npos) return L"  差の符号: " + D810WidenAscii(difference);
    const auto item = difference.substr(0, first);
    const auto camera = D810WidenAscii(difference.substr(first + 1, second - first - 1));
    const auto why = difference.substr(second + 1);
    if (item == "exposure_mode" && why == "not_manual") return L"  " + camera + L" の露出モードが M ではありません";
    if (item == "exposure_mode" && why == "unreadable") return L"  " + camera + L" の露出モードを読めませんでした";
    if (item == "iso_auto" && why == "not_off") return L"  " + camera + L" の ISO 自動が OFF ではありません";
    if (item == "iso_auto" && why == "unreadable") return L"  " + camera + L" の ISO 自動を読めませんでした";
    const auto head = L"  " + D810WriteItemNameLabel(item) + L": " + camera;
    if (why == "unreadable") return head + L" は読めません";
    if (why == "content_dependent") return head + L" の値は、本体の別の中身に頼るため確かめられません";
    if (why == "not_planned_value") return head + L" は、書いた値になっていません";
    return L"  差の符号: " + D810WidenAscii(difference);
}
// The result of a run of picked values: one line per item, each camera's outcome.
inline std::vector<std::wstring> D810ChosenRunLines(const D810WriteRun& run) {
    std::vector<std::wstring> lines;
    const auto order = D810ItemsInStepOrder(run.steps, [](const D810StepOutcome& outcome) { return outcome.step.item; });
    for (const auto item : order) {
        std::array<const D810StepOutcome*, 2> by_camera{};
        for (const auto& outcome : run.steps)
            if (outcome.step.item == item) by_camera[static_cast<std::size_t>(outcome.step.camera)] = &outcome;
        std::wstring line = std::wstring(L"  ") + D810WriteItemLabel(item) + L":";
        for (const auto camera : {D810Camera::B, D810Camera::A}) {
            const auto* outcome = by_camera[static_cast<std::size_t>(camera)];
            line += camera == D810Camera::B ? L" CAM-B " : L" ／ CAM-A ";
            if (!outcome) { line += L"変えない"; continue; }
            if (!outcome->sent) { line += L"書いていません"; continue; }
            line += D810WriteVerdictText(outcome->verdict);
            if (outcome->readback) line += L"（読み戻し " + D810ChoiceValueText(outcome->step.item, outcome->step.code, *outcome->readback) + L"）";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}
// Closes are counted here: a camera that could not be closed holds the screen
// (no further read, write or Live restart) as stage A does.
class D810TrackedCameraAccess final: public D810SettingsCameraAccess {
public:
    explicit D810TrackedCameraAccess(std::unique_ptr<D810SettingsCameraAccess> inner): inner_(std::move(inner)) {}
    D810OpenedCamera Open(D810Camera camera) override { return inner_ ? inner_->Open(camera) : D810OpenedCamera{}; }
    bool Close(D810Camera camera) override {
        const bool closed = inner_ && inner_->Close(camera);
        close_failed_ = close_failed_ || !closed;
        return closed;
    }
    bool CloseFailed() const { return close_failed_; }
private:
    std::unique_ptr<D810SettingsCameraAccess> inner_;
    bool close_failed_{};
};

class D810SettingsWriteScreen {
public:
    // Makes the access for one operation (with the cameras registered now), on the screen's thread.
    using AccessFactory = std::function<std::unique_ptr<D810SettingsCameraAccess>()>;
    struct Options {
        // ADR-0038 decision 10 (both provisional). The time the operator takes to
        // confirm is in neither: no session is open meanwhile.
        std::chrono::milliseconds read_deadline{30000};
        std::chrono::milliseconds write_deadline{60000};
        std::function<void(const std::string&)> log;
        std::function<std::string()> now;
    };
    D810SettingsWriteScreen(D810SettingsWriteController& controller, AccessFactory access, Options options):
        controller_(controller), access_(std::move(access)), options_(std::move(options)) {
        Refresh();
    }
    D810SettingsWriteScreen(const D810SettingsWriteScreen&) = delete;
    D810SettingsWriteScreen& operator=(const D810SettingsWriteScreen&) = delete;
    ~D810SettingsWriteScreen() {
        Stop();
        if (worker_.valid()) worker_.wait();
    }

    // The controller's status, read again (the record is read from the disk).
    // Not while a worker runs.
    void Refresh() {
        if (!Busy()) status_ = controller_.Status();
    }
    const D810WriteStatus& Status() const { return status_; }
    D810WriteScreenStep Step() const { return step_; }
    bool Busy() const { return worker_.valid(); }
    bool CloseFailed() const { return close_failed_; }
    const D810WriteScreenView* View() const { return view_ ? &*view_ : nullptr; }
    // Changes whenever the view is replaced or cleared (never 0 once a view has existed). The preview remembers the
    // serial of the confirmation it drew whole; a confirmation it has not drawn is not pressable.
    std::uint64_t ViewSerial() const { return view_serial_; }
    // The confirmation is up and the confirm button may be offered: whatever else the preview shows, it is drawn first.
    bool ConfirmationShown() const { return step_ == D810WriteScreenStep::Confirming && view_ && view_->confirmation; }
    // The last event in one line, for the preview's status line. Empty when none.
    const std::wstring& StatusLine() const { return status_line_; }

    // `screen_ready`: the preview's own condition for the read button (both Live
    // sessions stopped and closed, nothing else running, no failed Close).
    bool CopyOffered(bool screen_ready) const {
        return screen_ready && !Busy() && !close_failed_ && step_ != D810WriteScreenStep::Confirming && status_.write_allowed;
    }
    bool RereadOffered(bool screen_ready) const {
        return screen_ready && !Busy() && !close_failed_ && step_ != D810WriteScreenStep::Confirming;
    }
    bool ConfirmOffered(bool screen_ready) const { return screen_ready && !Busy() && step_ == D810WriteScreenStep::Confirming && plan_.has_value(); }
    // Why "copy" cannot be pressed now, or what it does.
    std::wstring OfferText(bool screen_ready) const {
        switch (step_) {
        case D810WriteScreenStep::ReadingForPlan: return L"両台の設定を読んでいます（読み取りのみ。A を閉じてから B を開きます）…";
        case D810WriteScreenStep::Writing:
            if (run_source_ == D810PlanSource::Chosen) return L"選んだ値を書いています（CAM-B を先に、CAM-A を最後に）。終わるまでライブの再開・撮影はできません…";
            return L"CAM-B に書いています。終わるまでライブの再開・撮影はできません…";
        case D810WriteScreenStep::ReadingForPanel: return L"両台の設定を読んでいます（読み取りのみ。A を閉じてから B を開きます）…";
        case D810WriteScreenStep::ReadingForChosen: return L"書く前に、両台の設定をもう一度読んでいます（読み取りのみ）…";
        case D810WriteScreenStep::Rereading: return L"両台を読み直しています（読み取りのみ）…";
        case D810WriteScreenStep::Confirming:
            if (plan_ && plan_->source == D810PlanSource::Chosen) return L"自動では元に戻りません。書く内容を確かめて、「書き込む（1回だけ）」か「やめる」を押してください。";
            return L"自動では元に戻りません。写す内容を確かめて、「書き込む（1回だけ）」か「やめる」を押してください。";
        case D810WriteScreenStep::Idle: break;
        }
        if (close_failed_) return L"カメラとの接続を閉じられませんでした。この画面を閉じて、本体・USB接続を確認してください。";
        if (status_.enabled_rows == 0)
            return L"「A の設定を B に写す」は押せません: 書ける項目がありません。ISO感度自動制御とシャッター速度の番号（code）が本体でまだ確定しておらず、"
                   L"書く相手の本体（機種・版）も確かめていないため、表の行はすべて無効です。";
        if (!WriteAllowedByBlock(status_.block)) return BlockText();
        if (!status_.write_allowed) return L"「A の設定を B に写す」は今は押せません（書き込みの表が正しくないか、カメラを別の処理が使っています）。";
        if (!screen_ready) return L"「A の設定を B に写す」は、両台のライブを止めて切断を確かめた後に押せます（撮影中・読み取り中は押せません）。";
        // What is read and what can be written are different: both bodies' exposure items and white balance and
        // metering are read; only the items the table enables for CAM-B are written, and they are named from it.
        std::wstring writable;
        for (const auto item : status_.writable_items) {
            if (!writable.empty()) writable += L"・";
            writable += D810WriteItemLabel(item);
        }
        if (writable.empty()) writable = L"（なし）";
        return L"「A の設定を B に写す」: 両台の露出の 5 項目とホワイトバランス・測光を読みます。CAM-B に書くのは " + writable +
               L" だけで、ほかは読むだけです。書く内容は確定の前に表示します。";
    }

    // "A の設定を B に写す": read both (read-only), then show the plan. Sends no write.
    bool RequestCopy(bool screen_ready) {
        if (!Busy()) Refresh();
        if (!CopyOffered(screen_ready)) { Say("settings_write_ui_copy_refused=1 setting_writes=0"); return false; }
        ResetView();
        Say("settings_write_ui_copy_requested=1 setting_writes=0");
        Launch(D810WriteScreenStep::ReadingForPlan);
        return true;
    }
    // The confirm button, and nothing else, confirms the plan shown and runs it once.
    bool ConfirmShownPlan(bool screen_ready) {
        if (!ConfirmOffered(screen_ready)) { Say("settings_write_ui_confirm_refused=1 setting_writes=0"); return false; }
        if (const auto refusal = controller_.Confirm(plan_->id)) {
            plan_.reset();
            Finish(L"確定できませんでした。何も書いていません: " + D810WriteReasonText(refusal), {});
            return false;
        }
        Say("settings_write_ui_confirmed=1 steps=" + std::to_string(plan_->steps.size()));
        plan_id_ = plan_->id;
        run_source_ = plan_->source;
        Launch(D810WriteScreenStep::Writing);
        return true;
    }
    void DiscardPlan() {
        if (step_ != D810WriteScreenStep::Confirming) return;
        const bool run_source_chosen_plan = plan_ && plan_->source == D810PlanSource::Chosen;
        controller_.Discard();
        plan_.reset();
        step_ = D810WriteScreenStep::Idle;
        Finish(run_source_chosen_plan ? L"書くのをやめました。何も書いていません。" : L"写すのをやめました。何も書いていません。", {});
        Say("settings_write_ui_plan_discarded=1 setting_writes=0");
    }
    // "設定を読み直す": read both (read-only); releases the capture block when
    // every block item reads the same on both.
    bool RequestReread(bool screen_ready) {
        if (!RereadOffered(screen_ready)) { Say("settings_write_ui_reread_refused=1"); return false; }
        ResetView();
        Say("settings_write_ui_reread_requested=1 setting_writes=0");
        Launch(D810WriteScreenStep::Rereading);
        return true;
    }

    // --- The settings panel (#416, ADR-0041) -------------------------------------------------------------------
    // A grid of the two cameras' values with a drop-down per cell. Opening it reads both cameras (read-only) and
    // keeps the values as a model; the operator's picks are a draft that sends nothing. The panel replaces the Live
    // area of the preview, so it is only opened while both Live sessions are stopped and closed (`screen_ready`).
    bool PanelOpen() const { return panel_open_; }
    // The cameras were written after the model was read: its values are old until the next read.
    bool PanelStale() const { return panel_stale_; }
    const D810PanelModel* Panel() const { return model_ ? &*model_ : nullptr; }
    // Changes when the model is replaced or the panel opens or closes (the preview rebuilds its drop-downs), and
    // when the draft changes (the preview shows the picks).
    std::uint64_t PanelSerial() const { return panel_serial_; }
    std::uint64_t DraftSerial() const { return draft_serial_; }
    bool PanelOpenOffered(bool screen_ready) const { return screen_ready && !Busy() && !close_failed_ && !panel_open_ && step_ == D810WriteScreenStep::Idle; }
    bool PanelCloseOffered() const { return panel_open_ && !Busy() && step_ == D810WriteScreenStep::Idle; }
    // Picks and cancels are taken while nothing runs, nothing is being confirmed and the values are not old.
    bool PanelEditable() const { return panel_open_ && model_ && !panel_stale_ && !Busy() && step_ == D810WriteScreenStep::Idle; }
    bool RequestPanelOpen(bool screen_ready) {
        if (!Busy()) Refresh();
        if (!PanelOpenOffered(screen_ready)) { Say("settings_write_ui_panel_refused=1 setting_writes=0"); return false; }
        ResetView();
        Say("settings_write_ui_panel_requested=1 setting_writes=0");
        Launch(D810WriteScreenStep::ReadingForPanel);
        return true;
    }
    // "設定を読み直す" with the panel open: the reread that releases a capture block, then a new read for the panel.
    // The draft is dropped when the new model replaces the old one.
    bool RequestPanelReread(bool screen_ready) {
        if (!panel_open_ || !RereadOffered(screen_ready)) { Say("settings_write_ui_reread_refused=1"); return false; }
        ResetView();
        Say("settings_write_ui_reread_requested=1 setting_writes=0");
        Launch(D810WriteScreenStep::Rereading, true);
        return true;
    }
    // Closes the panel. Returns the number of changes that were not written and are dropped (also said on the
    // status line). 0 when the panel cannot be closed now (a read or a write runs, or a confirmation is up).
    std::size_t ClosePanel() {
        if (!PanelCloseOffered()) return 0;
        const auto dropped = DropPanel();
        ResetView();
        status_line_ = dropped ? L"書いていない変更 " + std::to_wstring(dropped) + L" 件を捨てて、設定の画面を閉じました。" : L"設定の画面を閉じました。";
        Say("settings_write_ui_panel_closed=1 discarded=" + std::to_string(dropped) + " setting_writes=0");
        return dropped;
    }
    // The last thing the panel did with the cameras ended: a read or a reread that did not finish, or a run that was stopped before
    // it sent, is not followed by starting the cameras again by itself.
    bool PanelCamerasWereFine() const { return panel_cameras_ok_; }
    // A pick from a drop-down: only the draft changes, nothing is sent. Picking CAM-A's value takes away CAM-B's "same as CAM-A"
    // of the item when CAM-B does not offer the new value (the note says so; CAM-B keeps what it read).
    bool PanelPick(D810Camera camera, D810ChoiceItem item, std::int64_t value) {
        if (!PanelEditable() || !draft_.Pick(*model_, camera, item, value)) return false;
        panel_notice_.clear();
        if (camera == D810Camera::A) {
            const auto& follow = draft_.Get(D810Camera::B, item);
            if (follow && follow->source == D810ChoiceSource::SameAsA && !draft_.SameAsAOffered(*model_, item)) {
                draft_.Cancel(D810Camera::B, item);
                panel_notice_ = std::wstring(D810ChoiceItemLabel(item)) + L": CAM-B はその値を受け付けないため、「A と同じ」を外しました。";
            }
        }
        ++draft_serial_;
        return true;
    }
    // CAM-B follows CAM-A in this item.
    bool PanelPickSameAsA(D810ChoiceItem item) {
        if (!PanelEditable() || !draft_.PickSameAsA(*model_, item)) return false;
        panel_notice_.clear();
        ++draft_serial_;
        return true;
    }
    // "変更を取り消す": every pick is dropped. Returns the number of changes dropped.
    std::size_t PanelCancelAll() {
        if (!PanelEditable()) return 0;
        const auto dropped = draft_.Discard(*model_);
        panel_notice_.clear();
        ++draft_serial_;
        return dropped;
    }
    std::size_t PanelEntryCount() const { return draft_.EntryCount(); }
    // Whether the draft holds an entry for the cell (a pick or a follow), whether or not it changes anything.
    bool PanelDraftHas(D810Camera camera, D810ChoiceItem item) const { return draft_.Get(camera, item).has_value(); }
    // The number of changes the picks stand for ("N 件を書き込む…").
    std::size_t PanelDirty() const { return model_ ? draft_.Dirty(*model_) : 0; }
    std::vector<D810PlannedWrite> PanelPlanned() const { return model_ ? draft_.PlannedWrites(*model_) : std::vector<D810PlannedWrite>{}; }
    // What a drop-down shows: the value picked (for CAM-B "same as CAM-A", CAM-A's), else the value read.
    std::optional<std::int64_t> PanelShownValue(D810Camera camera, D810ChoiceItem item) const {
        if (!model_) return std::nullopt;
        if (const auto& entry = draft_.Get(camera, item)) {
            if (entry->source == D810ChoiceSource::Picked) return entry->value;
            return draft_.EffectiveAValue(*model_, item);
        }
        const auto& cell = model_->At(camera, item);
        if (cell.readable) return cell.current;
        return std::nullopt;
    }
    bool PanelSameAsAOffered(D810ChoiceItem item) const {
        return PanelEditable() && model_->At(D810Camera::B, item).Editable() && draft_.SameAsAOffered(*model_, item);
    }
    // The panel's rows, top to bottom: the two read-only rows, then the five that can be picked.
    std::vector<D810PanelRowView> PanelRows() const {
        std::vector<D810PanelRowView> rows;
        if (!model_) return rows;
        const auto read_only = [&](D810WriteItem item, D810SettingId id, std::uint16_t code, std::int64_t wanted, const wchar_t* advice) {
            D810PanelRowView row;
            row.label = D810WriteItemLabel(item);
            bool all = true;
            std::array<std::int64_t, 2> seen{};
            for (const auto camera : {D810Camera::A, D810Camera::B}) {
                const auto index = static_cast<std::size_t>(camera);
                const auto* reading = FindD810Reading(panel_snapshots_[index], id);
                if (!reading || !reading->available) { row.value[index] = L"読めません"; all = false; continue; }
                seen[index] = reading->desc.current_value;
                row.value[index] = D810WriteValueText(code, reading->desc.current_value);
                if (reading->desc.current_value != wanted) row.note[index] = advice;
            }
            row.difference = !all ? L"?" : seen[0] == seen[1] ? L"=" : L"≠";
            rows.push_back(std::move(row));
        };
        read_only(D810WriteItem::ExposureMode, D810SettingId::ExposureMode, kD810ExposureModeCode, kD810ExposureModeManual, L"本体を M に");
        read_only(D810WriteItem::IsoAuto, D810SettingId::IsoAuto, controller_.Table().Code(D810WriteItem::IsoAuto), kD810IsoAutoOff, L"本体で OFF に");
        const auto planned = PanelPlanned();
        for (const auto item : kD810ChoiceItems) {
            D810PanelRowView row;
            row.label = D810ChoiceItemLabel(item);
            row.choosable = true;
            row.item = item;
            const auto& a = model_->At(D810Camera::A, item);
            const auto& b = model_->At(D810Camera::B, item);
            row.difference = !a.readable || !b.readable ? L"?" : a.current == b.current ? L"=" : L"≠";
            for (const auto camera : {D810Camera::A, D810Camera::B}) {
                const auto index = static_cast<std::size_t>(camera);
                const auto& cell = model_->At(camera, item);
                const bool pending = std::any_of(planned.begin(), planned.end(), [&](const auto& write) { return write.camera == camera && write.item == item; });
                if (!cell.Editable()) row.note[index] = D810PanelLockText(cell.locked_reason);
                else if (pending) {
                    const auto& entry = draft_.Get(camera, item);
                    row.note[index] = entry && entry->source == D810ChoiceSource::SameAsA ? L"＊変更予定（A と同じ）" : L"＊変更予定";
                }
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }
    // The line under the panel: what the operator can do now.
    std::wstring PanelSummaryText() const {
        const auto core = PanelSummaryCore();
        return panel_notice_.empty() || core.empty() ? core : panel_notice_ + L" " + core;
    }
    std::wstring PanelSummaryCore() const {
        if (!panel_open_ || !model_) return {};
        if (panel_stale_) return L"書き込みのあと、または書き込みの途中で止まったあとです。「設定を読み直す」を押して、最新の値を読んでください。";
        std::size_t editable = 0;
        for (const auto camera : {D810Camera::A, D810Camera::B})
            for (const auto item : kD810ChoiceItems) editable += model_->At(camera, item).Editable();
        if (editable == 0) return L"この版では、値を選んで書くことはできません（各セルの右に理由を出しています。読むだけです）。";
        const auto dirty = PanelDirty();
        if (dirty == 0) return L"値を選ぶと「変更予定」になります。選んだだけでは、カメラには何も送りません。";
        return L"変更予定 " + std::to_wstring(dirty) + L" 件（まだ何も書いていません）。";
    }
    // "N 件を書き込む…" may be pressed: the panel's values are current, there is at least one change, nothing runs
    // and no confirmation is up, the preview is ready, and the table lets a plan be made and run (a block that is a
    // known mismatch lets one through, as for a copy). A cell the product table keeps closed cannot be picked, so a
    // change exists only where the table opens a row.
    bool ChosenOffered(bool screen_ready) const {
        return screen_ready && !Busy() && !close_failed_ && PanelEditable() && status_.write_allowed && PanelDirty() > 0;
    }
    // Why "N 件を書き込む…" cannot be pressed, or what it does.
    std::wstring ChosenOfferText(bool screen_ready) const {
        if (!panel_open_) return {};
        if (Busy() || step_ != D810WriteScreenStep::Idle) return OfferText(screen_ready);
        if (panel_stale_) return L"書き込みのあと、または書き込みの途中で止まったあとです。「設定を読み直す」を押して、最新の値を読んでください。";
        if (close_failed_) return OfferText(screen_ready);
        if (PanelDirty() == 0) return L"変更予定がありません。値を選ぶと、書き込めるようになります。";
        if (!WriteAllowedByBlock(status_.block)) return BlockText();
        if (!status_.write_allowed) return L"今は書き込めません（書き込みの表が正しくないか、カメラを別の処理が使っています）。";
        if (!screen_ready) return L"書き込みは、両台のライブを止めて切断を確かめた後に押せます（撮影中・読み取り中は押せません）。";
        return L"「" + std::to_wstring(PanelDirty()) + L" 件を書き込む…」を押すと、書く前に両台をもう一度読んで、確認の画面を出します。まだ何も書きません。";
    }
    // "N 件を書き込む…": read both cameras again (read-only), make the plan of the picks from that read and show it
    // for confirmation. Sends no write.
    bool RequestChosen(bool screen_ready) {
        if (!Busy()) Refresh();
        if (!ChosenOffered(screen_ready)) { Say("settings_write_ui_chosen_refused=1 setting_writes=0"); return false; }
        choices_ = draft_.Choices(); // a copy: the worker and the plan never see the draft itself
        ResetView();
        Say("settings_write_ui_chosen_requested=1 changes=" + std::to_string(PanelDirty()) + " setting_writes=0");
        Launch(D810WriteScreenStep::ReadingForChosen);
        return true;
    }
    // Whether the confirmation that is up (or the run that is going) is one of picked values.
    bool RunIsChosen() const { return run_source_ == D810PlanSource::Chosen; }
    // Takes a finished worker's result. True when something changed.
    bool Poll() {
        if (!worker_.valid() || worker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        auto outcome = worker_.get();
        stop_.reset();
        close_failed_ = close_failed_ || outcome.close_failed;
        Apply(outcome);
        return true;
    }
    // A capture start point (the buttons and RequestCapture): the record is read
    // again now. False while a worker runs or while capture is held.
    bool AdmitCapture() {
        if (Busy()) return false;
        Refresh();
        return status_.capture_allowed;
    }
    std::wstring CaptureRefusalText() const {
        if (Busy()) return L"設定の書き込み・読み直しの途中です。終わるまで撮影はできません。";
        return BlockText();
    }
    // A Live restart or a new stage A read: the confirmation and the last result go.
    void ClearView() {
        if (step_ == D810WriteScreenStep::Confirming) {
            controller_.Discard();
            plan_.reset();
            step_ = D810WriteScreenStep::Idle;
            Refresh();
        }
        // The Live area is the preview's again: the panel (and the picks in it) goes with the view.
        if (!Busy()) DropPanel();
        ResetView();
    }
    // The running operation's stop flag (window close). Only set, never cleared.
    void Stop() {
        if (stop_) stop_->store(true);
    }
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
    // Test builds only (the target that also has the table-taking controller): take a read made without a camera
    // as the worker's result, so a confirmation can be put up for the window's own code to draw.
    void TakePlanReadForTest(const D810PlanRead& read) { ApplyRead(read); }
    // The same for the panel: a read made without a camera becomes the panel's model.
    void TakePanelReadForTest(const D810PlanRead& read) { ApplyPanelRead(read); }
    // The read made for "N 件を書き込む…" without a camera, from the picks now in the draft: the plan and its confirmation.
    void TakeChosenReadForTest(const D810PlanRead& read) {
        auto taken = read;
        controller_.AcceptReadForTest(taken); // the read the plan is made from is the one the controller holds
        choices_ = draft_.Choices();
        ApplyChosenRead(taken);
    }
    // A finished run of a plan of `source` as the worker would hand it back.
    void TakeRunForTest(const D810WriteRun& run, D810PlanSource source) { run_source_ = source; ApplyRun(run); }
#endif
    // Waits for the worker (its cameras are closed when it returns) and takes its result.
    void Wait() {
        if (!worker_.valid()) return;
        worker_.wait();
        Poll();
    }

private:
    struct Outcome {
        D810WriteScreenStep step{};
        D810PlanRead read;
        D810WriteRun run;
        D810RereadResult reread;
        // A reread with the panel open reads both cameras once more for the panel (`read` holds that read).
        bool panel_read{};
        bool close_failed{};
        std::string error;
    };
    std::wstring BlockText() const { return D810CaptureBlockText(status_.block, status_.chosen_history, status_.history_damaged); }
    static bool WriteAllowedByBlock(D810CaptureBlock block) { return block == D810CaptureBlock::None || block == D810CaptureBlock::Mismatch; }
    void Say(const std::string& line) const { if (options_.log) options_.log(line); }
    void ResetView() { view_.reset(); ++view_serial_; }
    void SetView(D810WriteScreenView view) { view_ = std::move(view); ++view_serial_; }
    void Launch(D810WriteScreenStep step, bool panel_read = false) {
        step_ = step;
        auto stop = std::make_shared<std::atomic<bool>>(false);
        stop_ = stop;
        auto access = std::make_unique<D810TrackedCameraAccess>(access_ ? access_() : nullptr);
        const auto write_limit = plan_ ? D810WriteDeadlineFor(*plan_, options_.write_deadline) : options_.write_deadline;
        // A reread and a read for the panel one after the other have two reads' time.
        const auto read_limit = panel_read ? options_.read_deadline * 2 : options_.read_deadline;
        const auto limit = step == D810WriteScreenStep::Writing ? write_limit : read_limit;
        worker_ = std::async(std::launch::async,
            [&controller = controller_, access = std::move(access), stop, limit, step, panel_read, plan_id = plan_id_, log = options_.log, now = options_.now]() mutable {
                Outcome outcome;
                outcome.step = step;
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(com)) {
                    outcome.error = "com_init_failed";
                    return outcome;
                }
                try {
                    const auto deadline = std::chrono::steady_clock::now() + limit;
                    D810WriteRunOptions options;
                    options.may_continue = [stop, deadline] { return !stop->load() && std::chrono::steady_clock::now() < deadline; };
                    options.may_cleanup = [] { return true; };
                    options.remaining = [deadline] {
                        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                    };
                    options.log = log;
                    options.now = now;
                    if (step == D810WriteScreenStep::ReadingForPlan || step == D810WriteScreenStep::ReadingForPanel || step == D810WriteScreenStep::ReadingForChosen)
                        outcome.read = controller.ReadForPlan(*access, options);
                    else if (step == D810WriteScreenStep::Writing) outcome.run = controller.Execute(plan_id, *access, options);
                    else {
                        outcome.reread = controller.Reread(*access, options);
                        if (panel_read && outcome.reread.complete) {
                            outcome.read = controller.ReadForPlan(*access, options);
                            outcome.panel_read = true;
                        }
                    }
                } catch (...) {
                    outcome.error = "exception";
                }
                outcome.close_failed = access->CloseFailed();
                access.reset();
                CoUninitialize();
                return outcome;
            });
    }
    void Finish(std::wstring title, std::vector<std::wstring> lines) {
        step_ = D810WriteScreenStep::Idle;
        Refresh();
        if (!status_.capture_allowed) lines.push_back(BlockText());
        else lines.push_back(L"撮影の止めはありません。");
        status_line_ = title;
        SetView(D810WriteScreenView{std::move(title), std::move(lines), false});
    }
    static std::wstring StepLine(const D810WriteStep& step) { return D810WriteStepLine(step); }
    void AddSkipped(std::vector<std::wstring>& lines, const D810WritePlan& plan) const {
        if (plan.skipped.empty()) return;
        lines.push_back(L"外した項目（書きません。CAM-A と CAM-B で違う、または比べられない項目は、そのまま残ります）:");
        for (const auto& [item, reason] : plan.skipped) lines.push_back(D810WriteSkippedLine(item, reason));
    }
    void Apply(const Outcome& outcome) {
        if (!outcome.error.empty()) {
            plan_.reset();
            panel_cameras_ok_ = false;
            std::vector<std::wstring> lines{D810WriteReasonText(outcome.error)};
            if (outcome.step == D810WriteScreenStep::Writing) {
                // The run may have sent before it failed: no plan stays up, and the panel does not say that nothing was written.
                controller_.Discard();
                const auto dropped = MarkPanelOld();
                lines.push_back(L"書き込みの途中で止まった可能性があります。書いたかもしれないので、「設定を読み直す」で確かめてください。");
                if (dropped) lines.push_back(L"設定の画面で選んでいた変更 " + std::to_wstring(dropped) + L" 件の下書きは捨てました。");
            }
            Finish(L"設定の処理を最後まで終えられませんでした。", std::move(lines));
            Say("settings_write_ui_failure reason=" + outcome.error);
            return;
        }
        if (outcome.step == D810WriteScreenStep::ReadingForPlan) ApplyRead(outcome.read);
        else if (outcome.step == D810WriteScreenStep::ReadingForPanel) ApplyPanelRead(outcome.read);
        else if (outcome.step == D810WriteScreenStep::ReadingForChosen) ApplyChosenRead(outcome.read);
        else if (outcome.step == D810WriteScreenStep::Writing) ApplyRun(outcome.run);
        else ApplyReread(outcome.reread, outcome.panel_read ? &outcome.read : nullptr);
    }
    // The panel's model from a read: the picks of the model it replaces are dropped first (the count is returned).
    std::size_t ReplaceModel(const D810PlanRead& read) {
        const std::size_t dropped = model_ ? draft_.Discard(*model_) : 0; // before the new model: a draft of another read counts 0
        ++generation_;
        model_ = BuildD810PanelModel(controller_.Table(), read.snapshots[0], read.verified[0], read.snapshots[1], read.verified[1], generation_);
        draft_ = D810PanelDraft(generation_);
        panel_snapshots_ = read.snapshots;
        panel_stale_ = false;
        panel_cameras_ok_ = true;
        panel_notice_.clear();
        ++panel_serial_;
        ++draft_serial_;
        return dropped;
    }
    std::size_t DropPanel() {
        const std::size_t dropped = model_ ? draft_.Discard(*model_) : 0;
        model_.reset();
        panel_snapshots_ = {};
        panel_open_ = false;
        panel_stale_ = false;
        panel_cameras_ok_ = true;
        panel_notice_.clear();
        ++panel_serial_;
        ++draft_serial_;
        return dropped;
    }
    // The read made for "N 件を書き込む…": the controller makes the plan of the picks (`choices_`) from the last read it made
    // (this read is only its number; the flags of the bodies are the controller's own), then the confirmation.
    void ApplyChosenRead(const D810PlanRead& read) {
        if (!read.complete) {
            panel_cameras_ok_ = false;
            Finish(L"書く前の読み取りを最後まで終えられませんでした。何も書いていません。", {D810WriteReasonText(read.reason)});
            Say("settings_write_ui_chosen_read_incomplete=1 reason=" + read.reason);
            return;
        }
        panel_cameras_ok_ = true; // both cameras answered
        const auto result = controller_.PlanChosen(read, choices_);
        if (!result.Ok()) {
            std::vector<std::wstring> lines;
            for (const auto& reason : result.reasons) lines.push_back(D810PlanReasonText(reason, true));
            Finish(L"書けません。何も書いていません。", std::move(lines));
            Say("settings_write_ui_plan_refused=1 reasons=" + std::to_string(result.reasons.size()));
            return;
        }
        const auto& plan = *result.plan;
        Say("settings_write_ui_plan steps=" + std::to_string(plan.steps.size()) + " source=chosen");
        if (plan.steps.empty()) {
            controller_.Discard();
            Finish(L"書くものはありません。何も書いていません。", {L"選んだ値は、どちらのカメラも今の値と同じです。"});
            return;
        }
        plan_ = plan;
        step_ = D810WriteScreenStep::Confirming;
        Refresh();
        auto view = BuildD810ChosenConfirmation(plan, read.snapshots, model_ ? &*model_ : nullptr);
        status_line_ = L"書く内容を確かめてください（まだ何も書いていません）。";
        SetView(std::move(view));
    }
    void ApplyPanelRead(const D810PlanRead& read) {
        if (!read.complete) {
            Finish(L"両台の設定を読めませんでした。設定の画面は開きません。何も書いていません。", {D810WriteReasonText(read.reason), L"ライブは止まっています。「ライブ再開」で再開できます。"});
            Say("settings_write_ui_panel_read_incomplete=1 reason=" + read.reason);
            return;
        }
        ReplaceModel(read);
        panel_open_ = true;
        step_ = D810WriteScreenStep::Idle;
        Refresh();
        status_line_ = L"両台の設定を読みました（読み取りのみ）。値を選ぶだけでは、カメラには何も送りません。";
        if (!status_.capture_allowed) status_line_ += L" " + BlockText();
        Say("settings_write_ui_panel_opened=1 setting_writes=0");
    }
    void ApplyRead(const D810PlanRead& read) {
        if (!read.complete) {
            panel_cameras_ok_ = false; // a read of the copy that did not finish, with the panel open, also leaves the cameras not as the panel read them
            Finish(L"両台の設定を読めませんでした。何も書いていません。", {D810WriteReasonText(read.reason)});
            Say("settings_write_ui_plan_read_incomplete=1 reason=" + read.reason);
            return;
        }
        const auto result = controller_.PlanCopy(read.snapshots[0], read.snapshots[1]);
        if (!result.Ok()) {
            std::vector<std::wstring> lines;
            for (const auto& reason : result.reasons) lines.push_back(D810PlanReasonText(reason));
            Finish(L"写せません。何も書いていません。", std::move(lines));
            Say("settings_write_ui_plan_refused=1 reasons=" + std::to_string(result.reasons.size()));
            return;
        }
        const auto& plan = *result.plan;
        Say("settings_write_ui_plan steps=" + std::to_string(plan.steps.size()) + " skipped=" + std::to_string(plan.skipped.size()));
        if (plan.steps.empty()) {
            std::vector<std::wstring> lines{L"CAM-B の設定は、書ける項目について CAM-A と同じです。"};
            AddSkipped(lines, plan);
            controller_.Discard();
            Finish(L"書くものはありません。何も書いていません。", std::move(lines));
            return;
        }
        plan_ = plan;
        step_ = D810WriteScreenStep::Confirming;
        Refresh();
        D810WriteScreenView view;
        view.confirmation = true;
        view.title = L"CAM-A の設定を CAM-B に写す前の確認（まだ何も書いていません）";
        // The two notices of ADR-0038 decision 5 come first: a window too small for every line cuts the end.
        view.lines.push_back(L"自動では元に戻りません。書く前の値に戻す時は、本体で操作してください。");
        view.lines.push_back(L"書き込みは項目ごとに 1 回だけです。失敗しても再送しません。結果を確かめられない時は撮影を止めます。");
        view.lines.push_back(L"項目: CAM-B の今の値 → 書く値（CAM-A から読んだ値）");
        for (const auto& step : plan.steps) view.lines.push_back(L"  " + StepLine(step));
        AddSkipped(view.lines, plan);
        view.lines.push_back(L"よければ「書き込む（1回だけ）」、やめる時は「やめる」を押してください。");
        status_line_ = L"写す内容を確かめてください（まだ何も書いていません）。";
        SetView(std::move(view));
    }
    // The cameras may have been written: the panel's values are old and its picks are used up (or, after a copy, dropped).
    // Returns the number of changes the picks stood for.
    std::size_t MarkPanelOld() {
        if (!panel_open_ || !model_) return 0;
        const auto dropped = draft_.Discard(*model_);
        panel_stale_ = true;
        ++panel_serial_;
        ++draft_serial_;
        return dropped;
    }
    void ApplyRun(const D810WriteRun& run) {
        plan_.reset();
        const std::size_t dropped_picks = MarkPanelOld();
        // A run that was stopped before it sent anything for a reason (a camera that did not open, a body that changed) leaves the cameras not as they were found.
        panel_cameras_ok_ = !(run.sent == 0 && !run.reason.empty());
        Say("settings_write_ui_result verdict=" + std::string(D810WriteVerdictToken(run.verdict)) + " sent=" + std::to_string(run.sent) +
            " reason=" + (run.reason.empty() ? std::string("none") : run.reason));
        const bool chosen = run_source_ == D810PlanSource::Chosen;
        std::vector<std::wstring> lines;
        if (chosen) {
            lines = D810ChosenRunLines(run);
        } else {
            for (const auto& outcome : run.steps) {
                std::wstring line = L"  " + StepLine(outcome.step) + L" / 結果: " + D810WriteVerdictText(outcome.verdict);
                if (outcome.readback) line += L"（読み戻し " + D810WriteValueText(outcome.step.code, *outcome.readback) + L"）";
                lines.push_back(std::move(line));
            }
        }
        if (!run.reason.empty()) {
            const auto* chosen_text = chosen ? D810ChosenReasonText(run.reason.substr(0, run.reason.find(':'))) : nullptr;
            lines.push_back(L"止めた理由: " + (chosen_text ? std::wstring(chosen_text) + L"（" + D810WidenAscii(run.reason) + L"）" : D810WriteReasonText(run.reason)));
        }
        lines.push_back(L"再送も、前の値への書き戻しもしていません。");
        if (dropped_picks && !chosen) lines.push_back(L"設定の画面で選んでいた、書いていない変更 " + std::to_wstring(dropped_picks) + L" 件は捨てました。");
        std::wstring title;
        const std::wstring who = chosen ? L"選んだ値" : L"CAM-B";
        // A picked-value run counts steps (an item on a camera), so its title says both the items and the steps.
        std::wstring counted = std::to_wstring(run.sent) + L" 件";
        if (chosen) {
            std::vector<D810WriteItem> sent_items;
            for (const auto& outcome : run.steps)
                if (outcome.sent && std::find(sent_items.begin(), sent_items.end(), outcome.step.item) == sent_items.end()) sent_items.push_back(outcome.step.item);
            counted = std::to_wstring(sent_items.size()) + L" 項目・" + counted;
        }
        switch (run.verdict) {
        case D810WriteVerdict::Match: title = (chosen ? L"選んだ値を書きました" : L"CAM-B に書きました") + std::wstring(L": 一致（") + (chosen ? counted : std::to_wstring(run.sent) + L" 項目") + L"、読み戻した値が書いた値と同じ）"; break;
        case D810WriteVerdict::Mismatch: title = who + (chosen ? L"の書き込み" : L"への書き込み") + L": 不一致（送った" + (chosen ? L" " + counted : L"項目 " + std::to_wstring(run.sent)) + L"）"; break;
        case D810WriteVerdict::Unknown: title = who + (chosen ? L"の書き込み" : L"への書き込み") + L": 不明（結果を確かめられません。送った" + (chosen ? L" " + counted : L"項目 " + std::to_wstring(run.sent)) + L"）"; break;
        case D810WriteVerdict::NotSent: title = L"何も書いていません"; break;
        }
        Finish(std::move(title), std::move(lines));
    }
    // `panel_read`: the read for the panel that followed the reread (null when the panel is not open).
    void ApplyReread(const D810RereadResult& reread, const D810PlanRead* panel_read = nullptr) {
        Say("settings_write_ui_reread complete=" + std::to_string(reread.complete) + " released=" + std::to_string(reread.check.released) +
            " reason=" + (reread.reason.empty() ? std::string("none") : reread.reason));
        // What the panel did on the way, said under the result.
        std::vector<std::wstring> panel_lines;
        std::size_t dropped_picks = 0;
        if (panel_read) {
            if (panel_read->complete) {
                if (const auto dropped = ReplaceModel(*panel_read)) {
                    dropped_picks = dropped;
                    panel_lines.push_back(L"書いていない変更 " + std::to_wstring(dropped) + L" 件を捨てました。設定の画面は、読み直した値になりました。");
                }
            } else {
                panel_cameras_ok_ = false;
                panel_lines.push_back(L"設定の画面の値は読み直せていません（前の値のままです）。" + D810WriteReasonText(panel_read->reason));
            }
        }
        const auto finish = [&](std::wstring title, std::vector<std::wstring> lines) {
            for (const auto& line : panel_lines) lines.push_back(line);
            Finish(std::move(title), std::move(lines));
            // The status line says how many picks the new read dropped.
            if (dropped_picks) status_line_ += L"（書いていない変更 " + std::to_wstring(dropped_picks) + L" 件を捨てました）";
        };
        if (!reread.complete) {
            panel_cameras_ok_ = false;
            finish(L"読み直しを最後まで終えられませんでした。撮影の止めは変えていません。", {D810WriteReasonText(reread.reason)});
            return;
        }
        if (reread.reason == "record_unwritable") {
            finish(L"読み直しの結果を記録に書けなかったため、撮影の止めは解いていません。", {D810WriteReasonText(reread.reason)});
            return;
        }
        if (reread.check.released) {
            finish(reread.by_planned_values ? L"読み直しました: 書いた値は、各台でそのまま読めました。" : L"読み直しました: 止めの対象の項目は CAM-A と CAM-B で同じです。", {});
            return;
        }
        if (reread.by_planned_values) {
            // The record holds a plan of picked values: the cameras are not compared; each is checked against what was written
            // (D810ChosenDifferenceText tells each symbol of the check.) The controller takes this rule only for a history that holds a
            // plan of picked values and no damaged part of the record (chosen_unresolved and not history_damaged); anything else is
            // compared camera against camera, below.
            std::vector<std::wstring> lines{L"書いた値と違う項目（または読めない項目）:"};
            for (const auto& difference : reread.check.differences) lines.push_back(D810ChosenDifferenceText(difference));
            finish(L"読み直しました: 書いた値と違う項目があります。", std::move(lines));
            return;
        }
        std::vector<std::wstring> lines{L"違う項目（または読めない項目）:"};
        for (const auto& difference : reread.check.differences) {
            // A symbol of the planned values (two colons) does not belong to this rule; if one comes, it is told by its own symbol, not as an item name.
            if (difference == "record_unreadable" || difference.find(':') != difference.rfind(':')) { lines.push_back(D810ChosenDifferenceText(difference)); continue; }
            const auto colon = difference.find(':');
            std::wstring line = L"  " + D810WriteItemNameLabel(difference.substr(0, colon));
            if (colon != std::string::npos) line += difference.substr(colon + 1) == "unreadable" ? L"（読めない）" : L"（本体の別の中身に頼る値で比べられない）";
            lines.push_back(std::move(line));
        }
        if (reread.history_damaged) lines.push_back(std::wstring(L"  ") + kD810RecordDamagedAdvice);
        const bool record_unreadable = reread.check.differences.size() == 1 && reread.check.differences[0] == "record_unreadable";
        finish(record_unreadable ? L"読み直しました: 操作の記録を読めないため、止めは解いていません。" : L"読み直しました: CAM-A と CAM-B で違う項目があります。", std::move(lines));
    }

    D810SettingsWriteController& controller_;
    AccessFactory access_;
    Options options_;
    D810WriteStatus status_;
    D810WriteScreenStep step_{D810WriteScreenStep::Idle};
    std::optional<D810WritePlan> plan_;
    std::uint64_t plan_id_{};
    std::optional<D810WriteScreenView> view_;
    std::uint64_t view_serial_{};
    std::wstring status_line_;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::future<Outcome> worker_;
    bool close_failed_{};
    // The panel: the model of the last read, the picks on it and the read it came from.
    bool panel_open_{}, panel_stale_{};
    std::optional<D810PanelModel> model_;
    D810PanelDraft draft_;
    std::array<D810SettingsSnapshot, 2> panel_snapshots_;
    std::uint64_t generation_{}, panel_serial_{}, draft_serial_{};
    // The source of the plan the last confirmation ran.
    D810PlanSource run_source_{D810PlanSource::CopyAToB};
    // The picks the plan in the making is built from (a copy of the draft taken when "N 件を書き込む…" was pressed).
    std::vector<D810CameraChoice> choices_;
    // The last thing the panel did with the cameras ended (a read or a reread that did not finish, or a run stopped before it
    // sent, did not).
    bool panel_cameras_ok_{true};
    // A note on the line under the panel about the last pick (a follow that had to be removed).
    std::wstring panel_notice_;
};
} // namespace a0::phase0::experimental
