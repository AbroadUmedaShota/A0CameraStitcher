// Stub-only integration of the real WIC decoder and window message handler.
// No Start command is sent: no lease, worker, SDK or journal is created.
#include "../src/phase0/preview_commissioning_main.cpp"

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = WindowProc;
    type.hInstance = instance;
    type.lpszClassName = L"A0PreviewDisplayFailureTest";
    if (!RegisterClassW(&type)) return 2;
    App app;
    app.commissioning_allowed = true;
    const auto window = CreateWindowW(type.lpszClassName, L"Hidden preview fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 1000, 400, nullptr, nullptr, instance, &app);
    if (!window) return 2;
    auto* frame = new UiEvent{};
    frame->kind = EventKind::Frame;
    frame->pane = 0;
    frame->bytes = {0xff, 0xd8, 0x00}; // Truncated JPEG must fail real WIC decode.
    SendMessageW(window, kUiEvent, 0, reinterpret_cast<LPARAM>(frame));
    const auto deadline = GetTickCount64() + 3000;
    MSG message{};
    while (!app.completed && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
    wchar_t status[256]{};
    GetWindowTextW(app.status, status, 256);
    const bool passed = app.render_failed && app.completed && app.stopping &&
        std::wstring_view(status).find(L"未合格") != std::wstring_view::npos;
    DestroyWindow(window);
    UnregisterClassW(type.lpszClassName, instance);
    CoUninitialize();
    std::cout << "{\"displayFailurePreservedAfterClose\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
}
