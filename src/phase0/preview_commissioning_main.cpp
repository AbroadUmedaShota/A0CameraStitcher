// Experimental Phase 0 commissioning display.  This is deliberately a
// separate Win32 executable: it is not the .NET product UI and it has no
// capture, WPD, settings, card, or image-saving controls.
#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/preview_run_journal.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using a0::phase0::experimental::ObservedPreviewBody;
using a0::phase0::experimental::PreviewWorkerOwner;
using a0::phase0::experimental::FormatPreviewWorkerFailure;

constexpr UINT kUiEvent = WM_APP + 41;
constexpr INT_PTR kStart = 101;
constexpr INT_PTR kPreview = 102;
constexpr INT_PTR kConfirmA = 103;
constexpr INT_PTR kConfirmB = 104;
constexpr INT_PTR kStartBoth = 105;
constexpr INT_PTR kClose = 106;
constexpr INT_PTR kCandidates = 107;
constexpr INT_PTR kStatus = 108;

enum class Command { Start, Preview, ConfirmA, ConfirmB, StartBoth, Close, RenderFailed, Quit };
enum class EventKind { Status, Candidates, Frame, PaneStopped, Closed, Quarantined };

struct CommandItem { Command command; std::size_t candidate{}; };
struct UiEvent {
    EventKind kind;
    std::wstring text;
    std::size_t worker{};
    int pane{-1};
    std::array<std::string, 2> candidates{};
    std::vector<unsigned char> bytes;
    std::wstring label;
    ULONGLONG receipt_tick{};
};

std::wstring ErrorText() {
    // Do not reflect SDK exception details into the display: they can include
    // device paths or proprietary runtime details.  The safe state is visible.
    return L"実験用プレビュー制御に失敗しました。安全な終了確認を開始します。";
}

class ControlThread final {
public:
    explicit ControlThread(HWND window) : window_(window), thread_([this] { Run(); }) {}
    ~ControlThread() { Request(Command::Quit); if (thread_.joinable()) thread_.join(); }
    ControlThread(const ControlThread&) = delete;
    ControlThread& operator=(const ControlThread&) = delete;

    void Request(Command command, std::size_t candidate = 0) {
        if (command == Command::Close || command == Command::RenderFailed || command == Command::Quit) stop_requested_ = true;
        {
            std::lock_guard lock(mutex_);
            commands_.push({command, candidate});
        }
        condition_.notify_one();
    }

private:
    void BeginJournal() {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("journal location unavailable");
        const auto directory = std::filesystem::path(executable.data()).parent_path() / L"logs";
        std::filesystem::create_directories(directory);
        const auto file = directory / (L"preview-run-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L".jsonl");
        journal_ = std::make_unique<a0::phase0::experimental::PreviewRunJournal>(file);
        journal_->Record("run_started"); // Must persist before any worker/SDK startup.
    }
    void Record(std::string_view event, std::uint64_t value = 0) {
        if (!journal_) throw std::runtime_error("journal unavailable");
        journal_->Record(event, value);
    }
    bool RecordShutdown(std::string_view event) noexcept {
        if (!journal_) return false;
        try { journal_->Record(event); return true; } catch (...) { return false; }
    }
    void Post(std::unique_ptr<UiEvent> event) {
        auto* raw = event.release();
        if (!PostMessageW(window_, kUiEvent, 0, reinterpret_cast<LPARAM>(raw))) {
            // The UI stays alive while an owner exists.  If this is a process
            // teardown race, retaining shutdown uncertainty is safer than a retry.
            delete raw;
        }
    }
    void Status(std::wstring text) { Post(std::make_unique<UiEvent>(UiEvent{EventKind::Status, std::move(text)})); }
    bool CloseOnce(PreviewWorkerOwner*& owner) {
        if (construction_failed_) {
            RecordShutdown("startup_quarantined");
            Post(std::make_unique<UiEvent>(UiEvent{EventKind::Quarantined,
                L"ワーカー構築中に失敗しました。部分起動・隔離記録の有無が未確認のため、正常終了とは扱いません。"}));
            return false;
        }
        if (!owner) {
            RecordShutdown("no_active_owner");
            Post(std::make_unique<UiEvent>(UiEvent{EventKind::Closed, L"開始前に終了しました。"}));
            return true;
        }
        const bool closed = owner->Close(); // Owner caches result; this never resends an ambiguous close.
        if (closed) {
            delete owner;
            owner = nullptr;
            const bool recorded = RecordShutdown("both_workers_close_verified");
            Post(std::make_unique<UiEvent>(UiEvent{EventKind::Closed, recorded
                ? L"停止済み。終了確認を記録しました。プレビュー画像は保存していません。"
                : L"停止済み。ただし試験記録に失敗したため、実機試験の合格とは扱いません。"}));
        } else {
            RecordShutdown("close_unconfirmed");
            const auto failure = owner->FirstFailure();
            if (failure) {
                RecordShutdown(failure->response_validated ? "worker_response_validated" :
                               failure->response_received ? "worker_response_invalid" : "worker_response_missing");
                if (failure->ack_write_completed) RecordShutdown("worker_ack_write_completed");
            }
            Post(std::make_unique<UiEvent>(UiEvent{EventKind::Quarantined,
                L"終了確認が取れません。隔離を維持しています。アプリを閉じず、実機状態を確認してください。" +
                    (failure ? FormatPreviewWorkerFailure(*failure) : L" 失敗段階は未取得です。")}));
        }
        return closed;
    }
    void FailAndClose(PreviewWorkerOwner*& owner) {
        RecordShutdown("operation_failed");
        Status(ErrorText());
        CloseOnce(owner);
    }
    void Run() {
        // This thread is the sole lifetime owner of PreviewWorkerOwner and its lease.
        PreviewWorkerOwner* owner = nullptr;
        bool terminal = false;
        while (!terminal) {
            CommandItem item{};
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return !commands_.empty(); });
                item = commands_.front();
                commands_.pop();
            }
            if (item.command == Command::Quit) {
                if (!owner || CloseOnce(owner)) terminal = true;
                continue;
            }
            if (terminal) continue;
            try {
                if (item.command == Command::RenderFailed) {
                    RecordShutdown("display_failed");
                    CloseOnce(owner);
                    continue;
                }
                if (stop_requested_ && item.command != Command::Close) {
                    CloseOnce(owner);
                    continue;
                }
                if (owner && item.command != Command::Close &&
                    GetTickCount64() >= deadline_) {
                    throw std::runtime_error("experimental worker lifetime expired");
                }
                switch (item.command) {
                case Command::Start: {
                    if (start_attempted_) throw std::runtime_error("session cannot be restarted");
                    start_attempted_ = true;
                    BeginJournal();
                    Status(L"二つの実験用ワーカーを開始し、SDK候補列挙を開始しています。撮影はしません。");
                    deadline_ = GetTickCount64() + 60000; // conservative, starts before bootstrap.
                    try { owner = new PreviewWorkerOwner(); }
                    catch (const a0::phase0::experimental::PreviewWorkerStartupError& error) {
                        construction_failed_ = error.WorkersMayExist();
                        if (!construction_failed_) {
                            RecordShutdown("startup_no_workers");
                            stop_requested_ = true;
                            Post(std::make_unique<UiEvent>(UiEvent{EventKind::Closed,
                                L"ワーカー生成前に失敗しました。この画面は閉じられます。隔離記録があれば維持します。"}));
                            break;
                        }
                        throw;
                    }
                    catch (...) { construction_failed_ = true; throw; }
                    if (stop_requested_) { CloseOnce(owner); break; }
                    const auto candidates = owner->Enumerate(0);
                    Record("worker_a_enumerated", candidates.size());
                    candidates_[0] = candidates;
                    auto event = std::make_unique<UiEvent>();
                    event->kind = EventKind::Candidates;
                    event->worker = 0;
                    event->candidates = candidates;
                    event->text = L"ワーカー1の候補を選び、プレビューを表示してください。";
                    Post(std::move(event));
                    break;
                }
                case Command::Preview: {
                    if (!owner || item.candidate > 1) throw std::runtime_error("preview is not ready");
                    // The worker is determined by the commissioning stage; the UI
                    // sends a candidate ordinal only and never interprets opaque IDs.
                    const auto worker = active_worker_;
                    const auto bytes = owner->Preview(worker, candidates_[worker][item.candidate]);
                    Record(worker == 0 ? "worker_a_observed_bytes" : "worker_b_observed_bytes", bytes.size());
                    auto event = std::make_unique<UiEvent>();
                    event->kind = EventKind::Frame;
                    event->pane = static_cast<int>(worker);
                    event->bytes = std::move(bytes);
                    event->label = L"ワーカー" + std::to_wstring(worker + 1) + L"・候補" + std::to_wstring(item.candidate) + L"（観測中）";
                    event->receipt_tick = GetTickCount64();
                    event->text = L"表示された実機を CAM-A または CAM-B として確認してください（SDK個体証明ではありません）。";
                    Post(std::move(event));
                    break;
                }
                case Command::ConfirmA:
                case Command::ConfirmB: {
                    if (!owner) throw std::runtime_error("confirmation is not ready");
                    const auto body = item.command == Command::ConfirmA ? ObservedPreviewBody::CameraA : ObservedPreviewBody::CameraB;
                    const auto worker = active_worker_;
                    owner->ConfirmAndSuspend(worker, body);
                    Record(body == ObservedPreviewBody::CameraA ? "operator_confirmed_cam_a" : "operator_confirmed_cam_b", worker);
                    if (stop_requested_) { CloseOnce(owner); break; }
                    auto stopped = std::make_unique<UiEvent>();
                    stopped->kind = EventKind::PaneStopped;
                    stopped->pane = static_cast<int>(worker);
                    stopped->label = body == ObservedPreviewBody::CameraA
                        ? L"CAM-A（観測により割り当て）"
                        : L"CAM-B（観測により割り当て）";
                    stopped->text = L"選択したワーカーのライブビューを停止し、ソースを閉じました。";
                    Post(std::move(stopped));
                    if (worker == 0) {
                        active_worker_ = 1;
                        candidates_[1] = owner->Enumerate(1);
                        Record("worker_b_enumerated", candidates_[1].size());
                        auto event = std::make_unique<UiEvent>();
                        event->kind = EventKind::Candidates;
                        event->worker = 1;
                        event->candidates = candidates_[1];
                        event->text = L"ワーカー2の候補を選び、プレビューを表示してください。";
                        Post(std::move(event));
                    } else {
                        Post(std::make_unique<UiEvent>(UiEvent{EventKind::Status,
                            L"二台を別の実機として確認しました。左右同時プレビューを開始できます。"}));
                        Post(std::make_unique<UiEvent>(UiEvent{EventKind::Candidates,
                            L"開始準備完了", 2}));
                    }
                    break;
                }
                case Command::StartBoth: {
                    if (!owner) throw std::runtime_error("both previews are not ready");
                    owner->StartBoth();
                    Record("both_live_views_started");
                    // A bounded, one-shot feasibility sample.  This is not a retry loop,
                    // synchronization claim, capture, or persisted preview artifact.
                    for (int pair = 1; pair <= 3; ++pair) {
                        if (stop_requested_) break;
                        if (GetTickCount64() >= deadline_)
                            throw std::runtime_error("experimental worker lifetime expired");
                        auto left = owner->Read(ObservedPreviewBody::CameraA);
                        const auto left_received = GetTickCount64();
                        Record("cam_a_frame_bytes", left.size());
                        if (stop_requested_) break;
                        auto right = owner->Read(ObservedPreviewBody::CameraB);
                        const auto right_received = GetTickCount64();
                        Record("cam_b_frame_bytes", right.size());
                        Record("frame_pair_received", pair);
                        auto left_event = std::make_unique<UiEvent>();
                        left_event->kind = EventKind::Frame;
                        left_event->pane = 0;
                        left_event->bytes = std::move(left);
                        left_event->label = L"CAM-A（観測により割り当て）";
                        left_event->receipt_tick = left_received;
                        left_event->text = L"同時プレビュー確認 " + std::to_wstring(pair) + L"/3";
                        Post(std::move(left_event));
                        auto right_event = std::make_unique<UiEvent>();
                        right_event->kind = EventKind::Frame;
                        right_event->pane = 1;
                        right_event->bytes = std::move(right);
                        right_event->label = L"CAM-B（観測により割り当て）";
                        right_event->receipt_tick = right_received;
                        right_event->text = L"同時プレビュー確認 " + std::to_wstring(pair) + L"/3";
                        Post(std::move(right_event));
                    }
                    Status(stop_requested_ ? L"停止要求を受け付けました。終了を確認しています。" : L"3組のプレビューを取得しました。終了を確認しています。");
                    CloseOnce(owner);
                    break;
                }
                case Command::Close:
                    CloseOnce(owner);
                    break;
                case Command::Quit:
                case Command::RenderFailed:
                    break;
                }
            } catch (...) { FailAndClose(owner); }
        }
    }

    HWND window_{};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<CommandItem> commands_;
    std::thread thread_;
    std::unique_ptr<a0::phase0::experimental::PreviewRunJournal> journal_;
    std::array<std::array<std::string, 2>, 2> candidates_{};
    std::size_t active_worker_{};
    ULONGLONG deadline_{};
    bool start_attempted_{}, construction_failed_{};
    std::atomic<bool> stop_requested_{};
};

struct App {
    bool commissioning_allowed{};
    bool ui_only{};
    std::unique_ptr<ControlThread> controller;
    HBITMAP panes[2]{};
    bool stopped[2]{true, true};
    ULONGLONG receipt_ticks[2]{};
    std::wstring pane_labels[2]{L"CAM-A（観測後に割り当て）", L"CAM-B（観測後に割り当て）"};
    HWND status{};
    HWND combo{};
    int active_worker{};
    bool close_pending{};
    bool completed{};
    bool render_failed{};
    bool concurrent{};
    bool stopping{};
    ULONGLONG deadline{};
    std::wstring last_status;
};

void SetText(HWND control, std::wstring_view value) {
    SetWindowTextW(control, std::wstring(value).c_str());
}
void SetStatus(App& app, std::wstring value) {
    app.last_status = std::move(value);
    SetText(app.status, app.last_status);
}
void EnableControls(HWND window, bool start, bool preview, bool confirm, bool both, bool close) {
    EnableWindow(GetDlgItem(window, kStart), start);
    EnableWindow(GetDlgItem(window, kPreview), preview);
    EnableWindow(GetDlgItem(window, kConfirmA), confirm);
    EnableWindow(GetDlgItem(window, kConfirmB), confirm);
    EnableWindow(GetDlgItem(window, kStartBoth), both);
    EnableWindow(GetDlgItem(window, kClose), close);
}

HBITMAP DecodeJpeg(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return nullptr;
    IWICImagingFactory* factory{};
    IWICStream* stream{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICFormatConverter* converter{};
    HBITMAP result{};
    UINT width{}, height{};
    BITMAPINFO info{};
    void* pixels{};
    UINT stride{};
    std::uint64_t total{};
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        goto done;
    }
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height || width > 8192 || height > 8192) goto done;
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    result = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    stride = width * 4U;
    total = static_cast<std::uint64_t>(stride) * height;
    if (!result || total > MAXDWORD || FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(total),
                                                                     static_cast<BYTE*>(pixels)))) {
        if (result) DeleteObject(result);
        result = nullptr;
    }
done:
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return result;
}

void DrawPane(HDC hdc, const RECT& rect, HBITMAP bitmap, bool stopped, ULONGLONG receipt_tick, std::wstring_view title) {
    FillRect(hdc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, stopped ? RGB(220, 180, 80) : RGB(100, 230, 150));
    TextOutW(hdc, rect.left + 12, rect.top + 10, title.data(), static_cast<int>(title.size()));
    const bool stale = !stopped && receipt_tick && GetTickCount64() - receipt_tick > 5000;
    const auto label = stopped ? L"更新停止・最後の表示のみ（保存なし）" : (stale ? L"映像が古い状態（受信から5秒超）" : L"プレビュー受信（実験用）");
    TextOutW(hdc, rect.left + 12, rect.top + 32, label, static_cast<int>(wcslen(label)));
    const auto receipt = receipt_tick ? L"受信 tick: " + std::to_wstring(receipt_tick) + L" ms" : L"受信時刻: なし";
    TextOutW(hdc, rect.left + 12, rect.top + 50, receipt.c_str(), static_cast<int>(receipt.size()));
    if (!bitmap) return;
    BITMAP info{};
    GetObjectW(bitmap, sizeof(info), &info);
    const auto available_width = std::max(1L, (rect.right - rect.left) - 24);
    const auto available_height = std::max(1L, (rect.bottom - rect.top) - 84);
    const double factor = std::min(static_cast<double>(available_width) / info.bmWidth,
                                   static_cast<double>(available_height) / info.bmHeight);
    const auto width = static_cast<int>(info.bmWidth * factor);
    const auto height = static_cast<int>(info.bmHeight * factor);
    HDC source = CreateCompatibleDC(hdc);
    const auto old = SelectObject(source, bitmap);
    SetStretchBltMode(hdc, HALFTONE);
    StretchBlt(hdc, rect.left + 12 + (available_width - width) / 2, rect.top + 78 + (available_height - height) / 2,
              width, height, source, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
    SelectObject(source, old);
    DeleteDC(source);
}

void Layout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    MoveWindow(GetDlgItem(window, kStart), 12, 12, 118, 28, TRUE);
    MoveWindow(GetDlgItem(window, kCandidates), 140, 12, 150, 300, TRUE);
    MoveWindow(GetDlgItem(window, kPreview), 302, 12, 105, 28, TRUE);
    MoveWindow(GetDlgItem(window, kConfirmA), 419, 12, 105, 28, TRUE);
    MoveWindow(GetDlgItem(window, kConfirmB), 536, 12, 105, 28, TRUE);
    MoveWindow(GetDlgItem(window, kStartBoth), 653, 12, 130, 28, TRUE);
    MoveWindow(GetDlgItem(window, kClose), 795, 12, 100, 28, TRUE);
    MoveWindow(GetDlgItem(window, kStatus), 12, 48, width - 24, 38, TRUE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        CreateWindowW(L"BUTTON", L"開始", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                      reinterpret_cast<HMENU>(kStart), nullptr, nullptr);
        app->combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                   0, 0, 0, 0, window, reinterpret_cast<HMENU>(kCandidates), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"候補を表示", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                      reinterpret_cast<HMENU>(kPreview), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"CAM-A に確認", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                      reinterpret_cast<HMENU>(kConfirmA), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"CAM-B に確認", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                      reinterpret_cast<HMENU>(kConfirmB), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"左右を開始（3組）", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                      reinterpret_cast<HMENU>(kStartBoth), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"安全に停止", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                      reinterpret_cast<HMENU>(kClose), nullptr, nullptr);
        app->status = CreateWindowW(L"STATIC", L"実験用の確認画面です。原画像の保存・撮影・設定変更は行いません。",
                                    WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                                    reinterpret_cast<HMENU>(kStatus), nullptr, nullptr);
        Layout(window);
        EnableControls(window, app->commissioning_allowed, false, false, false, false);
        if (app->commissioning_allowed) app->controller = std::make_unique<ControlThread>(window);
        if (app->ui_only) SetStatus(*app, L"UI-only: SDK、リース、ワーカーは開始しません。");
        return 0;
    }
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_COMMAND:
        if (!app || HIWORD(wparam) != BN_CLICKED) break;
        if (!app->controller || !app->commissioning_allowed || app->stopping || app->completed ||
            LOWORD(wparam) < kStart || LOWORD(wparam) > kClose ||
            !IsWindowEnabled(GetDlgItem(window, LOWORD(wparam)))) return 0;
        switch (LOWORD(wparam)) {
        case kStart:
            app->deadline = GetTickCount64() + 60000;
            SetTimer(window, 1, 1000, nullptr);
            EnableControls(window, false, false, false, false, true);
            SetStatus(*app, L"開始を要求しました。ワーカー起動とSDK候補列挙を待っています。寿命は最大60秒です。");
            app->controller->Request(Command::Start);
            return 0;
        case kPreview: {
            const auto selected = SendMessageW(app->combo, CB_GETCURSEL, 0, 0);
            if (selected == CB_ERR) return 0;
            EnableControls(window, false, false, false, false, true);
            app->controller->Request(Command::Preview, static_cast<std::size_t>(selected));
            return 0;
        }
        case kConfirmA:
            EnableControls(window, false, false, false, false, true);
            app->controller->Request(Command::ConfirmA);
            return 0;
        case kConfirmB:
            EnableControls(window, false, false, false, false, true);
            app->controller->Request(Command::ConfirmB);
            return 0;
        case kStartBoth:
            EnableControls(window, false, false, false, false, true);
            app->concurrent = true;
            // Old commissioning images remain stopped until fresh alias frames arrive.
            app->stopped[0] = app->stopped[1] = true;
            InvalidateRect(window, nullptr, FALSE);
            app->controller->Request(Command::StartBoth);
            return 0;
        case kClose:
            app->stopping = true;
            EnableControls(window, false, false, false, false, false);
            SetStatus(*app, L"安全な終了確認を要求しました。確認完了または隔離状態までこの画面を保持します。");
            app->controller->Request(Command::Close);
            return 0;
        }
        break;
    case kUiEvent: {
        std::unique_ptr<UiEvent> event(reinterpret_cast<UiEvent*>(lparam));
        if (!app) return 0;
        if (app->stopping && event->kind != EventKind::Closed && event->kind != EventKind::Quarantined) return 0;
        if (event->kind == EventKind::Status) SetStatus(*app, event->text);
        if (event->kind == EventKind::Candidates) {
            SetStatus(*app, event->text);
            if (event->worker < 2) {
                app->active_worker = static_cast<int>(event->worker);
                SendMessageW(app->combo, CB_RESETCONTENT, 0, 0);
                SendMessageW(app->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"候補 0（不透明な SDK 候補）"));
                SendMessageW(app->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"候補 1（不透明な SDK 候補）"));
                SendMessageW(app->combo, CB_SETCURSEL, 0, 0);
                EnableControls(window, false, true, false, false, true);
            } else {
                EnableControls(window, false, false, false, true, true);
            }
        }
        if (event->kind == EventKind::Frame) {
            if (event->pane < 0 || event->pane > 1) return 0;
            auto bitmap = DecodeJpeg(event->bytes);
            if (!bitmap) {
                app->stopping = true;
                app->render_failed = true;
                SetStatus(*app, L"プレビューJPEGを表示できません。終了確認を開始します。");
                EnableControls(window, false, false, false, false, false);
                app->controller->Request(Command::RenderFailed);
            } else {
                if (app->panes[event->pane]) DeleteObject(app->panes[event->pane]);
                app->panes[event->pane] = bitmap;
                app->stopped[event->pane] = false;
                app->receipt_ticks[event->pane] = event->receipt_tick;
                app->pane_labels[event->pane] = event->label;
                SetStatus(*app, event->text);
                InvalidateRect(window, nullptr, FALSE);
                if (!app->concurrent && app->active_worker == event->pane) EnableControls(window, false, false, true, false, true);
            }
        }
        if (event->kind == EventKind::PaneStopped && event->pane >= 0 && event->pane < 2) {
            app->stopped[event->pane] = true;
            app->pane_labels[event->pane] = event->label;
            SetStatus(*app, event->text);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (event->kind == EventKind::Closed) {
            app->stopped[0] = app->stopped[1] = true;
            app->completed = true;
            KillTimer(window, 1);
            SetStatus(*app, app->render_failed
                ? L"停止済み。プレビュー表示に失敗したため、実機試験は未合格です。"
                : event->text);
            EnableControls(window, false, false, false, false, false);
            InvalidateRect(window, nullptr, FALSE);
            if (app->close_pending) DestroyWindow(window);
        }
        if (event->kind == EventKind::Quarantined) {
            app->stopping = true;
            app->stopped[0] = app->stopped[1] = true;
            KillTimer(window, 1);
            SetStatus(*app, event->text);
            EnableControls(window, false, false, false, false, false);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        if (app && wparam == 1 && app->deadline) {
            const auto now = GetTickCount64();
            const auto remaining = now < app->deadline ? static_cast<unsigned long long>((app->deadline - now + 999) / 1000) : 0;
            const auto suffix = L"  ワーカー寿命の残り目安: " + std::to_wstring(remaining) + L"秒（延長・自動再起動なし）";
            SetText(app->status, app->last_status + suffix);
            if (!remaining) {
                app->stopping = true;
                KillTimer(window, 1);
                EnableControls(window, false, false, false, false, false);
                SetStatus(*app, L"60秒の実験用ワーカー寿命が終了しました。安全な終了確認を開始します。");
                app->controller->Request(Command::Close);
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int top = 96;
        const int middle = client.right / 2;
        DrawPane(hdc, {0, top, middle - 2, client.bottom}, app ? app->panes[0] : nullptr,
                 !app || app->stopped[0], app ? app->receipt_ticks[0] : 0,
                 app ? app->pane_labels[0] : L"CAM-A（観測後に割り当て）");
        DrawPane(hdc, {middle + 2, top, client.right, client.bottom}, app ? app->panes[1] : nullptr,
                 !app || app->stopped[1], app ? app->receipt_ticks[1] : 0,
                 app ? app->pane_labels[1] : L"CAM-B（観測後に割り当て）");
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        if (app && app->controller) {
            if (app->completed) { DestroyWindow(window); return 0; }
            app->close_pending = true;
            app->stopping = true;
            EnableControls(window, false, false, false, false, false);
            SetStatus(*app, L"安全な終了確認を要求しました。確認が終わるまで画面を閉じません。");
            app->controller->Request(Command::Close);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (app) {
            app->controller.reset();
            for (auto bitmap : app->panes) if (bitmap) DeleteObject(bitmap);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int RunWindow(HINSTANCE instance, bool commissioning_allowed, bool ui_only) {
    App app{};
    app.commissioning_allowed = commissioning_allowed;
    app.ui_only = ui_only;
    const wchar_t class_name[] = L"A0PreviewCommissioningExperimental";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    window_class.lpfnWndProc = WindowProc;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
    if (!RegisterClassW(&window_class)) return 2;
    HWND window = CreateWindowExW(0, class_name, L"A0 Camera Stitcher - 二台ライブビュー確認（実験用）",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 760,
        nullptr, nullptr, instance, &app);
    if (!window) return 2;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) return 2;
    int argc{};
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { CoUninitialize(); return 2; }
    bool describe = false, ui_only = argc == 1, commissioning = false, invalid = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument(argv[i]);
        if (argument == L"--describe") describe = true;
        else if (argument == L"--ui-only") ui_only = true;
        else if (argument == L"--commission-preview-only") commissioning = true;
        else invalid = true;
    }
    LocalFree(argv);
    const int modes = static_cast<int>(describe) + static_cast<int>(ui_only) + static_cast<int>(commissioning);
    if (describe && modes == 1 && !invalid && argc == 2) {
        std::wcout << L"{\"schema\":\"a0.preview-commissioning.describe.v1\",\"app\":\"A0CameraStitcher.PreviewCommissioning\",\"mode\":\"describe\",\"experimental\":true,\"machineOperation\":\"partial\",\"requiresExplicit\":\"--commission-preview-only\",\"operations\":[\"preview only\"],\"prohibited\":[\"capture\",\"settings\",\"WPD\",\"card\",\"save\"]}\n";
        CoUninitialize();
        return 0;
    }
    if (invalid || modes != 1 || (argc != 2 && !(argc == 1 && ui_only)) || (!ui_only && !commissioning)) {
        std::wcerr << L"usage: --describe | --ui-only | --commission-preview-only\n";
        CoUninitialize();
        return 2;
    }
    const int result = RunWindow(instance, commissioning, ui_only);
    CoUninitialize();
    return result;
}
