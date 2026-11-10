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
// There is no "undo", no value chosen by the operator and no other way in.
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
// Why capture is held, in the words of docs/WPD_DUAL_PREVIEW.md. Empty when it is not.
inline std::wstring D810CaptureBlockText(D810CaptureBlock block) {
    switch (block) {
    case D810CaptureBlock::None: return {};
    case D810CaptureBlock::Mismatch:
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
               L"記録のファイルを人が確かめ、退避してください。";
    }
    return L"撮影を止めています。";
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
        {"copy_covers_five_items_only", L"写すのは露出の 5 項目だけです"},
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
// "<what>:<item>:<CAM-x>" from a plan, with the item's label.
inline std::wstring D810PlanReasonText(const std::string& reason) {
    const auto first = reason.find(':');
    if (first == std::string::npos) return D810WriteReasonText(reason);
    const auto second = reason.find(':', first + 1);
    const auto item = reason.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
    const auto camera = second == std::string::npos ? std::string() : reason.substr(second + 1);
    std::wstring subject = D810WriteItemNameLabel(item);
    if (!camera.empty()) subject = D810WidenAscii(camera) + L" の" + subject;
    return subject + L": " + D810WriteReasonText(reason.substr(0, first));
}

// --- The screen ------------------------------------------------------------------------
enum class D810WriteScreenStep : std::uint8_t { Idle, ReadingForPlan, Confirming, Writing, Rereading };
// What the screen draws in place of C: the confirmation before a copy, or the
// last result. Plain lines, top to bottom.
struct D810WriteScreenView {
    std::wstring title;
    std::vector<std::wstring> lines;
    bool confirmation{};
};
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
        case D810WriteScreenStep::Writing: return L"CAM-B に書いています。終わるまでライブの再開・撮影はできません…";
        case D810WriteScreenStep::Rereading: return L"両台を読み直しています（読み取りのみ）…";
        case D810WriteScreenStep::Confirming: return L"写す内容を確かめて、「書き込む（1回だけ）」か「やめる」を押してください。";
        case D810WriteScreenStep::Idle: break;
        }
        if (close_failed_) return L"カメラとの接続を閉じられませんでした。この画面を閉じて、本体・USB接続を確認してください。";
        if (status_.enabled_rows == 0)
            return L"「A の設定を B に写す」は押せません: 書ける項目がありません。ISO感度自動制御とシャッター速度の番号（code）が本体でまだ確定しておらず、"
                   L"書く相手の本体（機種・版）も確かめていないため、表の行はすべて無効です。";
        if (!WriteAllowedByBlock(status_.block)) return D810CaptureBlockText(status_.block);
        if (!status_.write_allowed) return L"「A の設定を B に写す」は今は押せません（書き込みの表が正しくないか、カメラを別の処理が使っています）。";
        if (!screen_ready) return L"「A の設定を B に写す」は、両台のライブを止めて切断を確かめた後に押せます（撮影中・読み取り中は押せません）。";
        return L"「A の設定を B に写す」: CAM-A の露出の 5 項目を読み、CAM-B に書く内容を確定の前に表示します。";
    }

    // "A の設定を B に写す": read both (read-only), then show the plan. Sends no write.
    bool RequestCopy(bool screen_ready) {
        if (!Busy()) Refresh();
        if (!CopyOffered(screen_ready)) { Say("settings_write_ui_copy_refused=1 setting_writes=0"); return false; }
        view_.reset();
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
        Launch(D810WriteScreenStep::Writing);
        return true;
    }
    void DiscardPlan() {
        if (step_ != D810WriteScreenStep::Confirming) return;
        controller_.Discard();
        plan_.reset();
        step_ = D810WriteScreenStep::Idle;
        Finish(L"写すのをやめました。何も書いていません。", {});
        Say("settings_write_ui_plan_discarded=1 setting_writes=0");
    }
    // "設定を読み直す": read both (read-only); releases the capture block when
    // every block item reads the same on both.
    bool RequestReread(bool screen_ready) {
        if (!RereadOffered(screen_ready)) { Say("settings_write_ui_reread_refused=1"); return false; }
        view_.reset();
        Say("settings_write_ui_reread_requested=1 setting_writes=0");
        Launch(D810WriteScreenStep::Rereading);
        return true;
    }
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
        return D810CaptureBlockText(status_.block);
    }
    // A Live restart or a new stage A read: the confirmation and the last result go.
    void ClearView() {
        if (step_ == D810WriteScreenStep::Confirming) {
            controller_.Discard();
            plan_.reset();
            step_ = D810WriteScreenStep::Idle;
            Refresh();
        }
        view_.reset();
    }
    // The running operation's stop flag (window close). Only set, never cleared.
    void Stop() {
        if (stop_) stop_->store(true);
    }
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
        bool close_failed{};
        std::string error;
    };
    static bool WriteAllowedByBlock(D810CaptureBlock block) { return block == D810CaptureBlock::None || block == D810CaptureBlock::Mismatch; }
    void Say(const std::string& line) const { if (options_.log) options_.log(line); }
    void Launch(D810WriteScreenStep step) {
        step_ = step;
        auto stop = std::make_shared<std::atomic<bool>>(false);
        stop_ = stop;
        auto access = std::make_unique<D810TrackedCameraAccess>(access_ ? access_() : nullptr);
        const auto limit = step == D810WriteScreenStep::Writing ? options_.write_deadline : options_.read_deadline;
        worker_ = std::async(std::launch::async,
            [&controller = controller_, access = std::move(access), stop, limit, step, plan_id = plan_id_, log = options_.log, now = options_.now]() mutable {
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
                    if (step == D810WriteScreenStep::ReadingForPlan) outcome.read = controller.ReadForPlan(*access, options);
                    else if (step == D810WriteScreenStep::Writing) outcome.run = controller.Execute(plan_id, *access, options);
                    else outcome.reread = controller.Reread(*access, options);
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
        if (!status_.capture_allowed) lines.push_back(D810CaptureBlockText(status_.block));
        else lines.push_back(L"撮影の止めはありません。");
        status_line_ = title;
        view_ = D810WriteScreenView{std::move(title), std::move(lines), false};
    }
    std::wstring StepLine(const D810WriteStep& step) const {
        return std::wstring(D810WriteItemLabel(step.item)) + L": CAM-B の今の値 " + D810WriteValueText(step.code, step.before) + L" → 書く値 " +
            D810WriteValueText(step.code, step.value);
    }
    void AddSkipped(std::vector<std::wstring>& lines, const D810WritePlan& plan) const {
        if (plan.skipped.empty()) return;
        lines.push_back(L"外した項目（書きません。CAM-A と CAM-B の違いはそのまま残ります）:");
        for (const auto& [item, reason] : plan.skipped) lines.push_back(std::wstring(L"  ") + D810WriteItemLabel(item) + L": " + D810WriteReasonText(reason));
    }
    void Apply(const Outcome& outcome) {
        if (!outcome.error.empty()) {
            plan_.reset();
            Finish(L"設定の処理を最後まで終えられませんでした。", {D810WriteReasonText(outcome.error)});
            Say("settings_write_ui_failure reason=" + outcome.error);
            return;
        }
        if (outcome.step == D810WriteScreenStep::ReadingForPlan) ApplyRead(outcome.read);
        else if (outcome.step == D810WriteScreenStep::Writing) ApplyRun(outcome.run);
        else ApplyReread(outcome.reread);
    }
    void ApplyRead(const D810PlanRead& read) {
        if (!read.complete) {
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
            std::vector<std::wstring> lines{L"CAM-B の露出の設定は、書ける項目について CAM-A と同じです。"};
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
        view.lines.push_back(L"項目: CAM-B の今の値 → 書く値（CAM-A から読んだ値）");
        for (const auto& step : plan.steps) view.lines.push_back(L"  " + StepLine(step));
        AddSkipped(view.lines, plan);
        view.lines.push_back(L"自動では元に戻りません。書く前の値に戻す時は、本体で操作してください。");
        view.lines.push_back(L"書き込みは項目ごとに 1 回だけです。失敗しても再送しません。結果を確かめられない時は撮影を止めます。");
        view.lines.push_back(L"よければ「書き込む（1回だけ）」、やめる時は「やめる」を押してください。");
        status_line_ = L"写す内容を確かめてください（まだ何も書いていません）。";
        view_ = std::move(view);
    }
    void ApplyRun(const D810WriteRun& run) {
        plan_.reset();
        Say("settings_write_ui_result verdict=" + std::string(D810WriteVerdictToken(run.verdict)) + " sent=" + std::to_string(run.sent) +
            " reason=" + (run.reason.empty() ? std::string("none") : run.reason));
        std::vector<std::wstring> lines;
        for (const auto& outcome : run.steps) {
            std::wstring line = L"  " + StepLine(outcome.step) + L" / 結果: " + D810WriteVerdictText(outcome.verdict);
            if (outcome.readback) line += L"（読み戻し " + D810WriteValueText(outcome.step.code, *outcome.readback) + L"）";
            lines.push_back(std::move(line));
        }
        if (!run.reason.empty()) lines.push_back(L"止めた理由: " + D810WriteReasonText(run.reason));
        lines.push_back(L"再送も、前の値への書き戻しもしていません。");
        std::wstring title;
        switch (run.verdict) {
        case D810WriteVerdict::Match: title = L"CAM-B に書きました: 一致（" + std::to_wstring(run.sent) + L" 項目、読み戻した値が書いた値と同じ）"; break;
        case D810WriteVerdict::Mismatch: title = L"CAM-B への書き込み: 不一致（送った項目 " + std::to_wstring(run.sent) + L"）"; break;
        case D810WriteVerdict::Unknown: title = L"CAM-B への書き込み: 不明（結果を確かめられません。送った項目 " + std::to_wstring(run.sent) + L"）"; break;
        case D810WriteVerdict::NotSent: title = L"何も書いていません"; break;
        }
        Finish(std::move(title), std::move(lines));
    }
    void ApplyReread(const D810RereadResult& reread) {
        Say("settings_write_ui_reread complete=" + std::to_string(reread.complete) + " released=" + std::to_string(reread.check.released) +
            " reason=" + (reread.reason.empty() ? std::string("none") : reread.reason));
        if (!reread.complete) {
            Finish(L"読み直しを最後まで終えられませんでした。撮影の止めは変えていません。", {D810WriteReasonText(reread.reason)});
            return;
        }
        if (reread.reason == "record_unwritable") {
            Finish(L"読み直しの結果を記録に書けなかったため、撮影の止めは解いていません。", {D810WriteReasonText(reread.reason)});
            return;
        }
        if (reread.check.released) {
            Finish(L"読み直しました: 止めの対象の項目は CAM-A と CAM-B で同じです。", {});
            return;
        }
        std::vector<std::wstring> lines{L"違う項目（または読めない項目）:"};
        for (const auto& difference : reread.check.differences) {
            const auto colon = difference.find(':');
            std::wstring line = L"  " + D810WriteItemNameLabel(difference.substr(0, colon));
            if (colon != std::string::npos) line += difference.substr(colon + 1) == "unreadable" ? L"（読めない）" : L"（本体の別の中身に頼る値で比べられない）";
            lines.push_back(std::move(line));
        }
        Finish(L"読み直しました: CAM-A と CAM-B で違う項目があります。", std::move(lines));
    }

    D810SettingsWriteController& controller_;
    AccessFactory access_;
    Options options_;
    D810WriteStatus status_;
    D810WriteScreenStep step_{D810WriteScreenStep::Idle};
    std::optional<D810WritePlan> plan_;
    std::uint64_t plan_id_{};
    std::optional<D810WriteScreenView> view_;
    std::wstring status_line_;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::future<Outcome> worker_;
    bool close_failed_{};
};
} // namespace a0::phase0::experimental
