// Only configured with the SDK stub: no test process can open a physical camera.
#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/common/protocol_json.hpp"
#include <Windows.h>
#include <filesystem>
#include <iostream>
#include <charconv>
#include <future>
#include <cstring>
using namespace a0::phase0;
using namespace a0::phase0::experimental;
namespace fs = std::filesystem;
namespace json = a0::common::protocol_json;
namespace {
int failures{};
void Check(bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } }
fs::path Marker(const fs::path& root) {
    DWORD session{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) throw std::runtime_error("session lookup");
    return root / (L"armed-session-" + std::to_wstring(session) + L".marker");
}
struct OwnedHandle {
    HANDLE value{};
    ~OwnedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct JsonFailure {
    [[noreturn]] static void Fail(std::string_view, std::string_view) { throw std::runtime_error("fake worker JSON rejected"); }
};
bool ReadExact(HANDLE pipe, void* buffer, DWORD length) {
    DWORD offset{};
    while (offset < length) {
        DWORD read{};
        if (!ReadFile(pipe, static_cast<char*>(buffer) + offset, length - offset, &read, nullptr) || !read) return false;
        offset += read;
    }
    return true;
}
bool WriteExact(HANDLE pipe, const void* buffer, DWORD length) {
    DWORD offset{};
    while (offset < length) {
        DWORD written{};
        if (!WriteFile(pipe, static_cast<const char*>(buffer) + offset, length - offset, &written, nullptr) || !written) return false;
        offset += written;
    }
    return true;
}
// Invoked only when this SDK-stub test executable is explicitly selected as
// PreviewWorkerOwner's child. No Nikon transport is constructed in this path.
int FakeReplyWorker(char** argv) {
    std::uintptr_t raw_parent{}, raw_bootstrap{};
    const auto number = [](const char* value, std::uintptr_t& result) {
        const auto end = value + std::strlen(value);
        const auto parsed = std::from_chars(value, end, result);
        return parsed.ec == std::errc{} && parsed.ptr == end && result;
    };
    if (!number(argv[2], raw_parent) || !number(argv[3], raw_bootstrap)) return 2;
    const auto parent = reinterpret_cast<HANDLE>(raw_parent);
    const auto bootstrap = reinterpret_cast<HANDLE>(raw_bootstrap);
    if (!GetProcessId(parent) || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) return 2;
    std::uint32_t length{};
    if (!ReadExact(bootstrap, &length, sizeof(length)) || !length || length > 1024) return 2;
    std::string body(length, '\0');
    if (!ReadExact(bootstrap, body.data(), length)) return 2;
    CloseHandle(bootstrap);
    const auto setup = json::BasicJsonParser<JsonFailure>(body).Parse();
    const auto string = [&](const char* key) {
        return json::RequireFieldWith<JsonFailure>(setup, key, json::JsonKind::string).string;
    };
    const auto pipe_name = string("pipe");
    const auto epoch = string("epoch");
    const auto capability = string("capability");
    const auto lease_name = string("leaseName");
    const auto root = fs::u8path(string("testMarkerRoot"));
    if (setup.kind != json::JsonKind::object || setup.object.size() != 6 ||
        !HardwareProcessLease::ValidateWorkerDelegation(parent, epoch, lease_name, root)) return 2;
    if (root.filename() == "prior") return 3;
    const auto path = L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
    OwnedHandle pipe{CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 5000, nullptr)};
    if (!pipe.value || pipe.value == INVALID_HANDLE_VALUE) return 2;
    if (!ConnectNamedPipe(pipe.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return 2;
    std::uint32_t request_length{};
    if (!ReadExact(pipe.value, &request_length, sizeof(request_length)) || !request_length || request_length > 4096) return 3;
    std::string request_wire(request_length, '\0');
    if (!ReadExact(pipe.value, request_wire.data(), request_length)) return 3;
    const auto request = json::BasicJsonParser<JsonFailure>(request_wire).Parse();
    const auto& request_schema = json::RequireFieldWith<JsonFailure>(request, "schema", json::JsonKind::string).string;
    const auto& request_epoch = json::RequireFieldWith<JsonFailure>(request, "epoch", json::JsonKind::string).string;
    const auto& request_capability = json::RequireFieldWith<JsonFailure>(request, "capability", json::JsonKind::string).string;
    const auto& request_sequence = json::RequireFieldWith<JsonFailure>(request, "sequence", json::JsonKind::number).string;
    if (request.kind != json::JsonKind::object || request.object.size() != 6 ||
        request_schema != "a0.preview-worker.v1" || request_epoch != epoch ||
        request_capability != capability || request_sequence != "1") return 4;
    const auto& operation = json::RequireFieldWith<JsonFailure>(request, "operation", json::JsonKind::string).string;
    const bool failing = operation == "enumerate";
    if (!failing && operation != "close") return 4;
    if (failing && root.filename() == "missing") return 3; // No response or ACK; no second command.
    const auto receipt = R"({"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true})";
    const auto sequence = root.filename() == "invalid" && failing ? 2 : 1;
    const auto response = "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"" + epoch +
        "\",\"workerPid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"sequence\":" + std::to_string(sequence) +
        (failing ? ",\"status\":\"failed\",\"payload\":{\"error\":\"injected_select_failure\",\"close\":" +
                   std::string(receipt) + "}}" : std::string(",\"status\":\"closed\",\"payload\":") + receipt + "}");
    const auto response_length = static_cast<std::uint32_t>(response.size());
    if (!WriteExact(pipe.value, &response_length, sizeof(response_length)) ||
        !WriteExact(pipe.value, response.data(), response_length)) return 3;
    unsigned char ack{};
    const bool acknowledged = ReadExact(pipe.value, &ack, 1) && ack == 0x06;
    if (root.filename() == "invalid" && failing) return acknowledged ? 5 : 3;
    return acknowledged ? (failing ? 3 : 0) : 5;
}
int ReplyContract(const fs::path& executable, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerReplyContract-" + std::to_wstring(GetCurrentProcessId()));
    for (const char* scenario : {"failed", "invalid", "missing", "prior"}) {
        const auto root = base / scenario;
        const auto name = "A0.Poc.TestLease.Reply." + std::to_string(GetCurrentProcessId());
        std::array<OwnedHandle, 2> processes;
        {
            PreviewWorkerOwner owner(name, root, executable, std::chrono::seconds(10));
            const auto ids = owner.ProcessIds();
            for (std::size_t index = 0; index < ids.size(); ++index)
                processes[index].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[index]);
            if (std::string_view(scenario) == "prior")
                Check(processes[0].value && WaitForSingleObject(processes[0].value, 5000) == WAIT_OBJECT_0,
                      "prior-exit fixture ended before command");
            bool rejected{};
            try { (void)owner.Enumerate(0); } catch (const TransportError&) { rejected = true; }
            Check(rejected && !owner.Close() && !owner.Close(), "failure is terminal without a second close");
            const auto failure = owner.FirstFailure();
            Check(failure.has_value() && failure->worker_index == 0 && failure->operation == "enumerate",
                  "parent retains first command and worker identity");
            if (failure) {
                const auto display = FormatPreviewWorkerFailure(*failure);
                Check(display.find(L"operation=enumerate") != std::wstring::npos &&
                      display.find(L"worker=0") != std::wstring::npos,
                      "UI diagnostic string retains first command and worker");
                if (std::string_view(scenario) == "failed")
                    Check(failure->category == "injected_select_failure" && failure->response_validated &&
                          failure->ack_write_completed && failure->reported_close && failure->reported_close->Complete(),
                          "bound failed reply carries category, reported close and local ACK write");
                else if (std::string_view(scenario) == "invalid")
                    Check(failure->category == "worker_reply_invalid" && failure->response_received &&
                          !failure->response_validated && !failure->ack_write_completed && !failure->reported_close,
                          "wrong sequence is not ACKed or credited as a close receipt");
                else if (std::string_view(scenario) == "missing")
                    Check(failure->category == "worker_ipc_unconfirmed" && !failure->response_received &&
                          !failure->ack_write_completed, "missing response leaves delivery unconfirmed");
                else
                    Check(failure->category == "worker_prior_exit" && !failure->response_received,
                          "prior worker exit is distinct from a reported failure");
                Check(display.find(std::string_view(scenario) == "failed" ? L"ack_write=completed_not_processed" :
                                   L"ack_write=unconfirmed") != std::wstring::npos &&
                      display.find(std::string_view(scenario) == "failed" ? L"close_receipt=complete_reported" :
                                   L"close_receipt=missing") != std::wstring::npos,
                      "UI diagnostic distinguishes ACK write and worker-reported receipt");
            }
            Check(fs::exists(Marker(root)), "failure retains the isolated test marker");
        }
        for (std::size_t index = 0; index < processes.size(); ++index) {
            auto& process = processes[index];
            DWORD code{};
            if (!process.value || WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0 ||
                !GetExitCodeProcess(process.value, &code)) return 3; // Keep the marker if fake exit is unknown.
            Check(code == (index == 0 || std::string_view(scenario) == "prior" ? 3U : 0U),
                  "fake worker exit proves one terminal reply or one explicit close, never a resend");
        }
        bool blocked{};
        try { HardwareProcessLease denied(name, std::chrono::milliseconds(0), root); }
        catch (const TransportError& error) { blocked = error.Category() == "camera_control_delegation_quarantined"; }
        Check(blocked, "next owner cannot cross retained delegation marker");
        if (!DeleteFileW(Marker(root).c_str()) || !RemoveDirectoryW(root.c_str())) return 4;
    }
    Check(RemoveDirectoryW(base.c_str()), "isolated reply fixture root removed");
    std::cout << "{\"mode\":\"stub-parent-reply-contract\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
// The helper owns a real PreviewWorkerOwner, but this target is SDK-stub only.
// The outer observer retains child handles before permitting the abrupt exit.
int AbandonHelper(char** argv, const fs::path& temporary, const fs::path& worker) {
    auto number = [](const char* text) {
        std::uintptr_t result{};
        const auto end = text + std::char_traits<char>::length(text);
        const auto parsed = std::from_chars(text, end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !result) throw std::runtime_error("helper argument");
        return result;
    };
    const auto observer = number(argv[2]);
    const auto output = reinterpret_cast<HANDLE>(number(argv[3]));
    const auto release = reinterpret_cast<HANDLE>(number(argv[4]));
    const auto root = temporary / (L"A0WorkerOwnerTest-" + std::to_wstring(observer)) / "abandoned";
    const auto name = "A0.Poc.TestLease.WorkerOwner." + std::to_string(observer);
    PreviewWorkerOwner owner(name, root, worker, std::chrono::seconds(10));
    const auto ids = owner.ProcessIds();
    DWORD written{};
    if (!WriteFile(output, ids.data(), sizeof(ids), &written, nullptr) || written != sizeof(ids)) return 92;
    if (WaitForSingleObject(release, 5000) != WAIT_OBJECT_0) return 93;
    ExitProcess(91); // Deliberately bypasses the owner's destructor; stub only.
}
void CheckAbandonedOwner(const fs::path& executable, const fs::path& root, const std::string& name) {
    OwnedHandle reader, writer, release, parent;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&reader.value, &writer.value, &security, 0) ||
        !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("observer pipe");
    release.value = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!release.value) throw std::runtime_error("observer barrier");
    auto command = L"\"" + executable.wstring() + L"\" --abandon " + std::to_wstring(GetCurrentProcessId()) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(writer.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(release.value));
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &created)) throw std::runtime_error("observer launch");
    parent.value = created.hProcess; CloseHandle(created.hThread);
    CloseHandle(writer.value); writer.value = nullptr;
    const auto deadline = GetTickCount64() + 5000;
    DWORD available{};
    while (GetTickCount64() < deadline) {
        if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available >= sizeof(std::array<std::uint32_t, 2>)) break;
        Sleep(10);
    }
    if (available < sizeof(std::array<std::uint32_t, 2>)) throw std::runtime_error("observer identities unavailable");
    std::array<std::uint32_t, 2> ids{}; DWORD received{};
    if (!ReadFile(reader.value, ids.data(), sizeof(ids), &received, nullptr) || received != sizeof(ids))
        throw std::runtime_error("observer identities incomplete");
    std::array<OwnedHandle, 2> workers;
    for (std::size_t i = 0; i != workers.size(); ++i) {
        workers[i].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
        if (!workers[i].value) throw std::runtime_error("observer worker handle");
    }
    Check(fs::exists(Marker(root / "abandoned")), "real parent armed marker before worker bootstrap");
    if (!SetEvent(release.value)) throw std::runtime_error("observer release");
    DWORD code{};
    if (WaitForSingleObject(parent.value, 5000) != WAIT_OBJECT_0 || !GetExitCodeProcess(parent.value, &code))
        throw std::runtime_error("parent exit unconfirmed");
    Check(code == 91, "parent exited without destructors");
    for (auto& worker : workers) {
        if (WaitForSingleObject(worker.value, 5000) != WAIT_OBJECT_0 || !GetExitCodeProcess(worker.value, &code))
            throw std::runtime_error("worker exit unconfirmed: preserve marker");
        // Parent may disappear before bootstrap validation (2) or after host start (3).
        Check(code == 2 || code == 3, "parent death is not normal authorized completion");
    }
    bool blocked{};
    try { HardwareProcessLease denied(name, std::chrono::milliseconds(0), root / "abandoned"); }
    catch (const TransportError& error) { blocked = error.Category() == "camera_control_delegation_quarantined"; }
    Check(blocked && fs::exists(Marker(root / "abandoned")), "real parent death preserves next-owner exclusion");
    // Test-only marker, exact path, with the parent and both stub workers observed exited.
    if (!DeleteFileW(Marker(root / "abandoned").c_str())) throw std::runtime_error("fixture marker cleanup");
    Check(RemoveDirectoryW((root / "abandoned").c_str()), "abandoned fixture directory removed");
}
}
int main(int argc, char** argv) {
    wchar_t temporary[MAX_PATH]{}, executable[32768]{};
    if (!GetTempPathW(MAX_PATH, temporary) || !GetModuleFileNameW(nullptr, executable, 32768)) return 2;
    const auto root = fs::path(temporary) / (L"A0WorkerOwnerTest-" + std::to_wstring(GetCurrentProcessId()));
    const auto worker = fs::path(executable).parent_path() / L"A0CameraStitcher.PreviewWorker.exe";
    const auto name = "A0.Poc.TestLease.WorkerOwner." + std::to_string(GetCurrentProcessId());
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--delegated-worker") return FakeReplyWorker(argv);
        if (argc == 2 && std::string_view(argv[1]) == "--reply-contract") return ReplyContract(executable, temporary);
        if (argc == 5 && std::string_view(argv[1]) == "--abandon") return AbandonHelper(argv, temporary, worker);
        if (argc != 1) return 2;
        {
            bool before_spawn{};
            try { PreviewWorkerOwner missing(name, root / "not-started", root / "missing-worker.exe", std::chrono::seconds(10)); }
            catch (const PreviewWorkerStartupError& error) { before_spawn = !error.WorkersMayExist(); }
            Check(before_spawn && !fs::exists(Marker(root / "not-started")), "missing executable is a verified pre-worker failure");
        }
        {
            PreviewWorkerOwner owner(name, root / "normal", worker, std::chrono::seconds(10));
            const auto ids = owner.ProcessIds();
            Check(ids[0] && ids[1] && ids[0] != ids[1], "two actual registered worker processes");
            Check(fs::exists(Marker(root / "normal")), "marker armed before bootstrap");
            const bool wrong_thread_rejected = std::async(std::launch::async, [&] {
                if (owner.Close()) return false;
                try { owner.Enumerate(0); } catch (const TransportError&) { return true; }
                return false;
            }).get();
            Check(wrong_thread_rejected && fs::exists(Marker(root / "normal")), "UI thread cannot mutate or close the lease owner's session");
            Check(owner.Close(), "both IPC receipts and OS exits allow disarm");
            Check(owner.Close(), "repeated close returns cached success without resend");
            Check(!fs::exists(Marker(root / "normal")), "marker removed only after verified close");
        }
        {
            HardwareProcessLease available(name, std::chrono::milliseconds(0), root / "normal");
            Check(!available.RecoveredAbandonedOwner(), "normal owner releases fresh lease");
        }
        Check(RemoveDirectoryW((root / "normal").c_str()), "normal fixture directory removed");
        {
            PreviewWorkerOwner owner(name, root / "expired", worker, std::chrono::milliseconds(300));
            const auto ids = owner.ProcessIds();
            for (auto id : ids) {
                HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
                if (!process) return 3;
                const auto wait = WaitForSingleObject(process, 5000);
                DWORD code{};
                const bool ended = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code);
                CloseHandle(process);
                if (!ended) return 3; // Never remove quarantine while a worker might live.
                Check(code == 3, "uncommanded expiry is not a successful close receipt");
            }
            bool command_denied{};
            try { owner.Enumerate(0); } catch (const TransportError&) { command_denied = true; }
            Check(command_denied, "dead pair cannot receive new camera commands");
            Check(!owner.Close() && !owner.Close(), "missing receipts remain terminal without retries");
            Check(fs::exists(Marker(root / "expired")), "clean OS exit alone never disarms");
        }
        bool blocked{};
        try { HardwareProcessLease denied(name, std::chrono::milliseconds(0), root / "expired"); }
        catch (const TransportError& error) { blocked = error.Category() == "camera_control_delegation_quarantined"; }
        Check(blocked, "next owner is quarantined after incomplete shutdown evidence");
        // Exact test-created marker; both stub workers were observed exited above.
        if (!DeleteFileW(Marker(root / "expired").c_str())) return 4;
        Check(RemoveDirectoryW((root / "expired").c_str()), "expired fixture directory removed");
        {
            std::array<OwnedHandle, 2> partial_workers;
            bool partial{};
            try {
                PreviewWorkerOwner injected(name, root / "partial", worker, std::chrono::seconds(10), [&](auto ids) {
                    for (std::size_t i = 0; i != ids.size(); ++i)
                        partial_workers[i].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
                    throw std::runtime_error("injected before registration and bootstrap");
                });
            } catch (const PreviewWorkerStartupError& error) { partial = error.WorkersMayExist(); }
            Check(partial && fs::exists(Marker(root / "partial")), "partial startup is never reported as pre-worker failure");
            for (auto& process : partial_workers) {
                DWORD code{};
                if (!process.value || WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0 ||
                    !GetExitCodeProcess(process.value, &code)) return 3; // Preserve all evidence if exit is unknown.
                Check(code == 3, "bootstrap EOF rejects partially started stub worker before SDK");
            }
            bool partial_blocked{};
            try { HardwareProcessLease denied(name, std::chrono::milliseconds(0), root / "partial"); }
            catch (const TransportError&) { partial_blocked = true; }
            Check(partial_blocked, "partial startup retains next-owner exclusion");
            // Exact test fixture; both stub children above were observed exited.
            if (!DeleteFileW(Marker(root / "partial").c_str())) return 4;
            Check(RemoveDirectoryW((root / "partial").c_str()), "partial fixture directory removed");
        }
        CheckAbandonedOwner(executable, root, name);
        Check(RemoveDirectoryW(root.c_str()), "fixture root removed");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    std::cout << "{\"mode\":\"stub-dual-worker-owner\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
