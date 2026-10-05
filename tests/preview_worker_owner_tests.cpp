// Only configured with the SDK stub: no test process can open a physical camera.
#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/preview_worker_failure_journal.hpp"
#include "a0/phase0/preview_run_journal.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/preview_topology_diag.hpp"
#include "a0/phase0/worker_topology_counters.hpp"
#include "a0/common/protocol_json.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <charconv>
#include <functional>
#include <future>
#include <cstring>
#include <regex>
#include <sstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
using namespace a0::phase0;
using namespace a0::phase0::experimental;
namespace fs = std::filesystem;
namespace json = a0::common::protocol_json;
namespace timing = a0::phase0::experimental::preview_worker_timing;
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
constexpr std::string_view kSchema = "a0.preview-worker.v2";
// A reply "diag" object for the fake workers: the 18 table keys, value
// `base + index` for each, and one key left out when `skip` names it.
std::string DiagJson(std::uint32_t base = 0, std::size_t skip = kPreviewTopologyDiagFieldCount) {
    std::string result = "{";
    for (std::size_t index = 0; index < kPreviewTopologyDiagFieldCount; ++index) {
        if (index == skip) continue;
        if (result.size() > 1) result += ',';
        result += "\"" + std::string(kPreviewTopologyDiagFields[index].json_key) + "\":" +
            std::to_string(base + static_cast<std::uint32_t>(index));
    }
    return result + "}";
}
// The PreviewTopologyDiag that DiagJson(base) encodes.
PreviewTopologyDiag DiagValues(std::uint32_t base) {
    PreviewTopologyDiag diag;
    for (std::size_t index = 0; index < kPreviewTopologyDiagFieldCount; ++index)
        diag.values[index] = base + static_cast<std::uint32_t>(index);
    return diag;
}
// Fake worker diag bases: enumerate replies, failing replies, close replies.
constexpr std::uint32_t kEnumerateDiagBase = 300;
constexpr std::uint32_t kFailureDiagBase = 100;
constexpr std::uint32_t kCloseDiagBase = 200;
// A bound v2 reply as the real dispatcher writes it.
std::string FakeReply(const std::string& epoch, std::uint64_t sequence, std::string_view status, std::string_view payload,
                      const std::string& diag) {
    return "{\"schema\":\"" + std::string(kSchema) + "\",\"epoch\":\"" + epoch + "\",\"workerPid\":" +
        std::to_string(GetCurrentProcessId()) + ",\"sequence\":" + std::to_string(sequence) + ",\"status\":\"" +
        std::string(status) + "\",\"payload\":" + std::string(payload) + ",\"diag\":" + diag + "}";
}
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
// PreviewWorkerOwner::Exchange opens a brand-new named-pipe client connection
// for every single command (its `Handle pipe` is local to one Exchange call
// and closes when that call returns) -- it does not hold one connection open
// across commands. A multi-command fixture must disconnect and accept the
// next client between replies, or the second command's read simply fails.
bool ReconnectPipe(HANDLE pipe) {
    if (!DisconnectNamedPipe(pipe)) return false;
    return ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
}
// Drives enumerate -> select -> start -> frame to a validated bound `failed`
// reply at exactly `fail_at`, with a realistic bounded SDK-transport category
// for that stage. A bare `close` (the paired worker, which never receives
// anything before the owner's own close attempt) is answered the same way
// the legacy single-shot fixture below answers it.
int StagedFailureWorker(HANDLE pipe, const std::string& epoch, const std::string& capability,
                        const std::string& fail_at) {
    std::uint64_t sequence{};
    bool enumerated{};
    for (;;) {
        std::uint32_t request_length{};
        if (!ReadExact(pipe, &request_length, sizeof(request_length)) || !request_length || request_length > 4096) return 3;
        std::string request_wire(request_length, '\0');
        if (!ReadExact(pipe, request_wire.data(), request_length)) return 3;
        const auto request = json::BasicJsonParser<JsonFailure>(request_wire).Parse();
        if (request.kind != json::JsonKind::object || request.object.size() != 6) return 4;
        const auto& request_schema = json::RequireFieldWith<JsonFailure>(request, "schema", json::JsonKind::string).string;
        const auto& request_epoch = json::RequireFieldWith<JsonFailure>(request, "epoch", json::JsonKind::string).string;
        const auto& request_capability = json::RequireFieldWith<JsonFailure>(request, "capability", json::JsonKind::string).string;
        const auto& request_sequence = json::RequireFieldWith<JsonFailure>(request, "sequence", json::JsonKind::number).string;
        if (request_schema != kSchema || request_epoch != epoch || request_capability != capability) return 4;
        ++sequence;
        if (request_sequence != std::to_string(sequence)) return 4;
        const auto& operation = json::RequireFieldWith<JsonFailure>(request, "operation", json::JsonKind::string).string;
        const auto respond = [&](const std::string& response) {
            const auto response_length = static_cast<std::uint32_t>(response.size());
            if (!WriteExact(pipe, &response_length, sizeof(response_length)) || !WriteExact(pipe, response.data(), response_length))
                return false;
            unsigned char ack{};
            return ReadExact(pipe, &ack, 1) && ack == 0x06;
        };
        if (operation == "close") {
            const auto receipt = R"({"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true})";
            return respond(FakeReply(epoch, sequence, "closed", receipt, DiagJson(kCloseDiagBase))) ? 0 : 5;
        }
        if (!enumerated) {
            if (operation != "enumerate" || sequence != 1) return 4;
            enumerated = true;
            if (!respond(FakeReply(epoch, sequence, "ok", R"(["staged-cand-0","staged-cand-1"])",
                                   DiagJson(kEnumerateDiagBase)))) return 5;
            if (!ReconnectPipe(pipe)) return 2;
            continue;
        }
        if (operation != "select" && operation != "start" && operation != "frame") return 4;
        const bool should_fail = operation == fail_at;
        if (!should_fail) {
            if (!respond(FakeReply(epoch, sequence, "ok", "null", DiagJson(kEnumerateDiagBase)))) return 5;
            if (!ReconnectPipe(pipe)) return 2;
            continue;
        }
        // Realistic bounded SDK-transport categories for each staged command
        // (see nikon_sdk_transport.cpp: OpenWorkerPreviewCandidate/select,
        // StartSelectedWorkerLiveView/start, and the frame SDK command failure
        // of ReadLiveViewFrame/frame).
        const std::string_view category = operation == "select" ? "open_failed" :
            operation == "start" ? "live_view_start_failed" : "live_view_frame_failed";
        const auto receipt = R"({"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true})";
        const auto payload = "{\"error\":\"" + std::string(category) + "\",\"close\":" + receipt + "}";
        return respond(FakeReply(epoch, sequence, "failed", payload, DiagJson(kFailureDiagBase))) ? 3 : 5;
    }
}
// "late" fixture: Close() closes worker 0 and waits for it before sending close
// to worker 1, so the first child to claim this file is worker 0. Test-only
// file next to the isolated marker root; the test removes it.
bool ClaimFirstClose(const fs::path& base) {
    OwnedHandle claim{CreateFileW((base / L"late-first-close.claim").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
    return claim.value && claim.value != INVALID_HANDLE_VALUE;
}
// ---- Scripted fake children for the T2 timing contracts ----
// Every delay below comes from the production budget table and the same scale
// the parent side of these tests passes to PreviewWorkerOwner, so the fake
// child and the owner agree on every window.
constexpr timing::TimeScale kTestScale{1, 5};
DWORD ScaledMs(std::chrono::milliseconds value) { return static_cast<DWORD>(kTestScale.Apply(value).count()); }
DWORD ScaledExchange(std::string_view operation) { return ScaledMs(*timing::ParentExchangeDeadline(operation)); }
// D + S: the parent's first exchange with a worker.
DWORD ScaledFirstExchange(std::string_view operation) {
    return ScaledMs(*timing::ParentExchangeDeadline(operation) + timing::kWorkerStartupAllowance);
}
// GetTickCount64 advances in steps of about 15.6 ms, so a wait measured with it
// can read up to one step short of the deadline the owner waited for.
constexpr ULONGLONG kTickGranularityMs = 16;
void SleepUntil(ULONGLONG tick) {
    const auto now = GetTickCount64();
    if (now < tick) Sleep(static_cast<DWORD>(tick - now));
}
DWORD ScaledSafeReceiptWindow() { return ScaledMs(timing::kExitWindowAfterSafeReceipt); }
DWORD ScaledCleanupWindow() { return ScaledMs(timing::ExitWindowWithoutReceipt(timing::kWorkerOperationBudgets)); }
// The single 30 s exchange deadline every command had before T2.
DWORD ScaledOldExchange() { return ScaledMs(std::chrono::milliseconds{30000}); }
// slowstart: later than the old deadline, 30 % into the gap to the start budget.
DWORD SlowStartDelay() { return ScaledOldExchange() + (ScaledExchange("start") - ScaledOldExchange()) * 3 / 10; }
// slowenumerate: past D(enumerate), halfway into the S the first exchange adds.
DWORD SlowEnumerateDelay() { return ScaledExchange("enumerate") + (ScaledFirstExchange("enumerate") - ScaledExchange("enumerate")) / 2; }
// overlap: worker 1 answers close after 60 % of the close budget; worker 0,
// which never answers, exits after 80 % of its E_fail window.
DWORD OverlapReplyDelay() { return ScaledExchange("close") * 6 / 10; }
DWORD OverlapExitDelay() { return ScaledCleanupWindow() * 8 / 10; }
// silentclose: worker 0 exits halfway through its E_fail window.
DWORD SilentExitDelay() { return ScaledCleanupWindow() / 2; }
// lingerclosed: worker 0 stays alive 3 s past its E_ok window, long enough
// for the repeated Close() to look at it while it still runs.
DWORD LingerAfterSafeReceipt() { return ScaledSafeReceiptWindow() + 3000; }
// quarantined: worker 0 stays alive this long after its quarantined reply.
DWORD QuarantinedLinger() { return ScaledSafeReceiptWindow() + 2000; }
// cappedwindow (R1): time scale 1/8 and the close budget raised by 20 s, so
// worker 1's close exchange may last until the session end. Close W 42 s, D
// 47 s, first exchange D + S 56 s (7 s scaled). Serving check:
// 2 s + (47 s + 2 x 47 s) / 8 = 19.6 s <= 22.5 s <= 180 s / 8.
constexpr timing::TimeScale kCappedScale{1, 8};
PreviewWorkerTestTiming CappedWindowTiming() {
    PreviewWorkerTestTiming result;
    result.operation_deadline = std::chrono::seconds(2);
    result.serving_lifetime = kCappedScale.Apply(timing::kPreviewSessionLimit);
    result.scale = kCappedScale;
    auto table = timing::kWorkerOperationBudgets;
    for (auto& entry : table)
        if (entry.operation == "close") entry.sdk += std::chrono::seconds(20);
    result.budget_table = table;
    return result;
}
PreviewWorkerTestTiming ScaledTiming() {
    PreviewWorkerTestTiming result;
    // The owner counts its operation deadline from construction, which
    // includes both process spawns; under ctest a spawn has taken seconds, so
    // the scripted commands right after construction need room.
    // 10 s + (47 s + 54 s) / 5 = 30.2 s <= 32 s <= 180 s / 5.
    result.operation_deadline = std::chrono::seconds(10);
    result.serving_lifetime = std::chrono::seconds(32);
    result.scale = kTestScale;
    return result;
}
// Operation deadline of test-root owners that send commands right after
// construction. The owner counts it from construction, which includes both
// process spawns (seconds under ctest at times). 30 s + 47 s + 54 s fits the
// 180 s serving lifetime of the test-root constructor.
constexpr std::chrono::seconds kCommandingTestLifetime{30};
constexpr const char* kSafeReceipt =
    R"({"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true})";
constexpr const char* kUnsafeReceipt =
    R"({"liveViewOff":true,"sourceClosed":false,"moduleClosed":false,"processClaimReleased":false,"safeToExit":false})";
std::string FailedPayload(std::string_view category, std::string_view receipt) {
    return "{\"error\":\"" + std::string(category) + "\",\"close\":" + std::string(receipt) + "}";
}
// Overlapped single-instance server pipe with bounded waits, so a scripted
// fake can stop listening on its own.
class FakeServer {
public:
    explicit FakeServer(const std::wstring& path) {
        pipe_.value = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 5000, nullptr);
        event_.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    bool Valid() const { return pipe_.value && pipe_.value != INVALID_HANDLE_VALUE && event_.value; }
    bool Accept(DWORD timeout_ms) {
        OVERLAPPED pending{};
        pending.hEvent = event_.value;
        ResetEvent(event_.value);
        if (ConnectNamedPipe(pipe_.value, &pending)) return true;
        const auto error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) return true;
        if (error != ERROR_IO_PENDING) return false;
        DWORD ignored{};
        return Finish(pending, timeout_ms, ignored);
    }
    bool Read(void* buffer, DWORD size, DWORD timeout_ms) { return Transfer(buffer, size, false, timeout_ms); }
    bool Write(const void* buffer, DWORD size, DWORD timeout_ms) {
        return Transfer(const_cast<void*>(buffer), size, true, timeout_ms);
    }
    // Ends the current client connection without any reply.
    void Drop() { DisconnectNamedPipe(pipe_.value); }
private:
    bool Finish(OVERLAPPED& pending, DWORD timeout_ms, DWORD& done) {
        if (WaitForSingleObject(event_.value, timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_.value, &pending);
            WaitForSingleObject(event_.value, INFINITE); // OVERLAPPED must outlive the cancellation.
            return false;
        }
        return GetOverlappedResult(pipe_.value, &pending, &done, FALSE) != FALSE;
    }
    bool Transfer(void* buffer, DWORD size, bool write, DWORD timeout_ms) {
        DWORD offset{};
        while (offset < size) {
            OVERLAPPED pending{};
            pending.hEvent = event_.value;
            ResetEvent(event_.value);
            DWORD done{};
            auto* at = static_cast<char*>(buffer) + offset;
            const BOOL immediate = write ? WriteFile(pipe_.value, at, size - offset, &done, &pending)
                                         : ReadFile(pipe_.value, at, size - offset, &done, &pending);
            if (!immediate) {
                if (GetLastError() != ERROR_IO_PENDING || !Finish(pending, timeout_ms, done)) return false;
            }
            if (!done) return false;
            offset += done;
        }
        return true;
    }
    OwnedHandle pipe_, event_;
};
// The worker side of one fake session: bound replies with the epoch, the fake
// worker's own PID and the parent's sequence, as the real dispatcher writes them.
struct FakeSession {
    FakeServer& server;
    std::string epoch, capability;
    std::uint64_t sequence{};
    std::optional<std::string> Receive(DWORD timeout_ms) {
        std::uint32_t length{};
        if (!server.Read(&length, sizeof(length), timeout_ms) || !length || length > 4096) return std::nullopt;
        std::string wire(length, '\0');
        if (!server.Read(wire.data(), length, timeout_ms)) return std::nullopt;
        const auto request = json::BasicJsonParser<JsonFailure>(wire).Parse();
        if (request.kind != json::JsonKind::object || request.object.size() != 6) return std::nullopt;
        const auto field = [&](const char* key, json::JsonKind kind) -> const std::string& {
            return json::RequireFieldWith<JsonFailure>(request, key, kind).string;
        };
        if (field("schema", json::JsonKind::string) != kSchema ||
            field("epoch", json::JsonKind::string) != epoch || field("capability", json::JsonKind::string) != capability ||
            field("sequence", json::JsonKind::number) != std::to_string(sequence + 1)) return std::nullopt;
        ++sequence;
        return field("operation", json::JsonKind::string);
    }
    // Writes the reply for the current sequence, then waits for the ACK byte.
    bool Respond(std::string_view status, std::string_view payload, DWORD timeout_ms,
                 const std::string& diag = DiagJson(kCloseDiagBase)) {
        return Send(status, payload, timeout_ms, diag) && ReceivedAck(timeout_ms);
    }
    // Writes the reply only.
    bool Send(std::string_view status, std::string_view payload, DWORD timeout_ms, const std::string& diag) {
        const auto response = FakeReply(epoch, sequence, status, payload, diag);
        const auto length = static_cast<std::uint32_t>(response.size());
        return server.Write(&length, sizeof(length), timeout_ms) && server.Write(response.data(), length, timeout_ms);
    }
    // True only when the ACK byte arrives within `timeout_ms`.
    bool ReceivedAck(DWORD timeout_ms) {
        unsigned char ack{};
        return server.Read(&ack, 1, timeout_ms) && ack == 0x06;
    }
};
// Close is sent to worker 0 and answered before worker 1 receives its close,
// so the first child to claim this file is worker 0. Test-only file next to
// the isolated marker root; the test removes it.
fs::path ScenarioClaim(const fs::path& base, std::string_view scenario) {
    return base / (std::string(scenario) + "-first-close.claim");
}
bool ClaimFirstClose(const fs::path& base, std::string_view scenario) {
    OwnedHandle claim{CreateFileW(ScenarioClaim(base, scenario).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
    return claim.value && claim.value != INVALID_HANDLE_VALUE;
}
bool IsScriptedScenario(std::string_view scenario) {
    return scenario == "slowstart" || scenario == "stuckstart" || scenario == "lingerclosed" ||
        scenario == "quickclosed" || scenario == "silentclose" || scenario == "overlap" || scenario == "quarantined" ||
        scenario == "slowenumerate" || scenario == "cappedwindow" || scenario == "diagselect";
}
// cappedwindow: the test writes the tick at which worker 1 answers close, as a
// decimal GetTickCount64 value, before it calls Close(). Test-only file next to
// the isolated marker root; the test removes it.
fs::path AnswerTickFile(const fs::path& base, std::string_view scenario) {
    return base / (std::string(scenario) + "-answer.tick");
}
std::optional<ULONGLONG> ReadAnswerTick(const fs::path& base, std::string_view scenario) {
    std::ifstream stream(AnswerTickFile(base, scenario));
    ULONGLONG tick{};
    if (!(stream >> tick) || !tick) return std::nullopt;
    return tick;
}
// Scenarios (worker 0 is the child that receives enumerate, or the first close):
//   slowstart    worker 0: enumerate ok, select ok, start answered `failed`
//                (live_view_start_failed, safe receipt) after SlowStartDelay(); exit 3
//   stuckstart   worker 0: as slowstart, but start is never answered inside its
//                budget; then listens 1 s for another command; exit 40 + commands
//   lingerclosed worker 0: `closed`, then alive past E_ok while listening; exit 0,
//                or 6 if any further command arrives
//   quickclosed  both: `closed`; exit 0
//   silentclose  worker 0: drops close unanswered; exits 3 after SilentExitDelay()
//   overlap      worker 0: drops close unanswered; exits 3 after OverlapExitDelay();
//                worker 1: answers `closed` after OverlapReplyDelay(); exit 0
//   quarantined  worker 0: `quarantined` with an unsafe receipt, then alive for
//                QuarantinedLinger() while listening; exit 7, or 6 on another command
//   slowenumerate worker 0: enumerate answered `ok` after SlowEnumerateDelay(),
//                then `closed`; exit 0
//   cappedwindow worker 0: `closed` at once, then alive while listening until
//                2 s past the answer tick; exit 0, or 6 on another command.
//                worker 1: `closed` at the answer tick; exit 0
//   diagselect   worker 0: enumerate ok, then select answered `ok` with a diag
//                missing one key (17 keys), then waits up to 3 s for an ACK
//                byte (exit 5 if one arrives) and listens 1 s for any further
//                command (exit 6); otherwise exit 3
// Every other worker answers close with `closed` and exits 0.
int ScriptedFakeWorker(const std::wstring& path, const std::string& epoch, const std::string& capability,
                       const std::string& scenario, const fs::path& base) {
    constexpr DWORD kFirstCommandWait = 30000;
    constexpr DWORD kIo = 5000;
    FakeServer server(path);
    if (!server.Valid()) return 2;
    FakeSession session{server, epoch, capability};
    if (!server.Accept(kFirstCommandWait)) return 2;
    auto operation = session.Receive(kIo);
    if (!operation) return 3;
    const auto reconnect = [&] {
        server.Drop();
        return server.Accept(kIo);
    };
    if (*operation == "enumerate" && scenario == "diagselect") {
        if (!session.Respond("ok", R"(["scripted-cand-0","scripted-cand-1"])", kIo) || !reconnect()) return 5;
        operation = session.Receive(kIo);
        if (operation != "select") return 4;
        if (!session.Send("ok", "null", kIo, DiagJson(kFailureDiagBase, 7))) return 5;
        if (session.ReceivedAck(3000)) return 5; // The parent must never ACK a rejected reply.
        server.Drop();
        return server.Accept(1000) ? 6 : 3;
    }
    if (*operation == "enumerate" && scenario == "slowenumerate") {
        Sleep(SlowEnumerateDelay());
        if (!session.Respond("ok", R"(["scripted-cand-0","scripted-cand-1"])", kIo) || !reconnect()) return 5;
        operation = session.Receive(kIo);
        if (operation != "close") return 4;
        return session.Respond("closed", kSafeReceipt, kIo) ? 0 : 5;
    }
    if (*operation == "enumerate") {
        if (scenario != "slowstart" && scenario != "stuckstart") return 4;
        if (!session.Respond("ok", R"(["scripted-cand-0","scripted-cand-1"])", kIo) || !reconnect()) return 5;
        operation = session.Receive(kIo);
        if (operation != "select" || !session.Respond("ok", "null", kIo) || !reconnect()) return 5;
        operation = session.Receive(kIo);
        if (operation != "start") return 4;
        const auto failed = FailedPayload("live_view_start_failed", kSafeReceipt);
        if (scenario == "slowstart") {
            Sleep(SlowStartDelay());
            return session.Respond("failed", failed, kIo) ? 3 : 5;
        }
        Sleep(ScaledExchange("start") + 500);
        (void)session.Respond("failed", failed, 200); // The parent has already given up on this reply.
        server.Drop();
        return 40 + (server.Accept(1000) ? 1 : 0);
    }
    if (*operation != "close") return 4;
    const bool first = ClaimFirstClose(base, scenario);
    if (scenario == "cappedwindow") {
        const auto answer_at = ReadAnswerTick(base, scenario);
        if (!answer_at) return 4;
        if (first) {
            if (!session.Respond("closed", kSafeReceipt, kIo)) return 5;
            server.Drop();
            const auto linger_end = *answer_at + 2000;
            const auto now = GetTickCount64();
            return server.Accept(now < linger_end ? static_cast<DWORD>(linger_end - now) : 0) ? 6 : 0;
        }
        while (GetTickCount64() < *answer_at) Sleep(5);
        return session.Respond("closed", kSafeReceipt, kIo) ? 0 : 5;
    }
    if (!first || scenario == "quickclosed" || scenario == "slowstart" || scenario == "stuckstart" ||
        scenario == "slowenumerate" || scenario == "diagselect") {
        if (scenario == "overlap") Sleep(OverlapReplyDelay());
        return session.Respond("closed", kSafeReceipt, kIo) ? 0 : 5;
    }
    if (scenario == "lingerclosed") {
        if (!session.Respond("closed", kSafeReceipt, kIo)) return 5;
        server.Drop();
        return server.Accept(LingerAfterSafeReceipt()) ? 6 : 0;
    }
    if (scenario == "quarantined") {
        if (!session.Respond("quarantined", kUnsafeReceipt, kIo)) return 5;
        server.Drop();
        return server.Accept(QuarantinedLinger()) ? 6 : 7;
    }
    if (scenario == "silentclose" || scenario == "overlap") {
        server.Drop(); // No reply: the parent's read of this close ends at once.
        Sleep(scenario == "silentclose" ? SilentExitDelay() : OverlapExitDelay());
        return 3;
    }
    return 4;
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
    // pipe, epoch, capability, lifetimeMs, servingMs, leaseName, testMarkerRoot.
    if (setup.kind != json::JsonKind::object || setup.object.size() != 7 ||
        setup.object.find("servingMs") == setup.object.end() ||
        !HardwareProcessLease::ValidateWorkerDelegation(parent, epoch, lease_name, root)) return 2;
    if (root.filename() == "prior") return 3;
    if (IsScriptedScenario(root.filename().string()))
        return ScriptedFakeWorker(L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end()), epoch, capability,
                                  root.filename().string(), root.parent_path());
    const auto path = L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
    OwnedHandle pipe{CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 5000, nullptr)};
    if (!pipe.value || pipe.value == INVALID_HANDLE_VALUE) return 2;
    if (!ConnectNamedPipe(pipe.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return 2;
    const auto scenario_name = root.filename().string();
    if (scenario_name == "select" || scenario_name == "start" || scenario_name == "frame")
        return StagedFailureWorker(pipe.value, epoch, capability, scenario_name);
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
        request_schema != kSchema || request_epoch != epoch ||
        request_capability != capability || request_sequence != "1") return 4;
    const auto& operation = json::RequireFieldWith<JsonFailure>(request, "operation", json::JsonKind::string).string;
    const bool failing = operation == "enumerate";
    if (!failing && operation != "close") return 4;
    if (failing && root.filename() == "missing") return 3; // No response or ACK; no second command.
    // "late": the second child asked to close (worker 1) exits after reading the
    // request, without a response; the first one answers close normally.
    if (!failing && root.filename() == "late" && !ClaimFirstClose(root.parent_path())) return 3;
    const auto receipt = R"({"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true})";
    const auto sequence = root.filename() == "invalid" && failing ? 2 : 1;
    const auto response = failing
        ? FakeReply(epoch, sequence, "failed", "{\"error\":\"injected_select_failure\",\"close\":" + std::string(receipt) + "}",
                    DiagJson(kFailureDiagBase))
        : FakeReply(epoch, sequence, "closed", receipt, DiagJson(kCloseDiagBase));
    const auto response_length = static_cast<std::uint32_t>(response.size());
    if (!WriteExact(pipe.value, &response_length, sizeof(response_length)) ||
        !WriteExact(pipe.value, response.data(), response_length)) return 3;
    unsigned char ack{};
    const bool acknowledged = ReadExact(pipe.value, &ack, 1) && ack == 0x06;
    if ((root.filename() == "invalid" || root.filename() == "ackfail") && failing)
        return acknowledged ? 5 : 3;
    return acknowledged ? (failing ? 3 : 0) : 5;
}
int ReplyContract(const fs::path& executable, const fs::path& temporary, bool ack_only = false) {
    const auto base = temporary / (L"A0WorkerReplyContract-" + std::to_wstring(GetCurrentProcessId()));
    for (const char* scenario : {"failed", "invalid", "missing", "prior", "ackfail"}) {
        if (ack_only != (std::string_view(scenario) == "ackfail")) continue;
        const auto root = base / scenario;
        const auto name = "A0.Poc.TestLease.Reply." + std::to_string(GetCurrentProcessId());
        std::array<OwnedHandle, 2> processes;
        {
            PreviewWorkerOwner owner(name, root, executable, kCommandingTestLifetime, {}, ack_only);
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
                          failure->ack_write_completed && failure->reported_close && failure->reported_close->Complete() &&
                          failure->topology == DiagValues(kFailureDiagBase),
                          "bound failed reply carries category, reported close, diag and local ACK write");
                else if (std::string_view(scenario) == "invalid")
                    Check(failure->category == "worker_reply_invalid" && failure->response_received &&
                          !failure->response_validated && !failure->ack_write_completed && !failure->reported_close &&
                          !failure->topology,
                          "wrong sequence is not ACKed or credited as a close receipt or diag");
                else if (std::string_view(scenario) == "missing")
                    Check(failure->category == "worker_ipc_unconfirmed" && !failure->response_received &&
                          !failure->ack_write_completed, "missing response leaves delivery unconfirmed");
                else if (std::string_view(scenario) == "ackfail")
                    Check(failure->category == "injected_select_failure" && failure->response_received &&
                          failure->response_validated && !failure->ack_write_completed &&
                          failure->reported_close && failure->reported_close->Complete(),
                          "validated failure with ACK write fault retains receipt but never credits ACK");
                else
                    Check(failure->category == "worker_prior_exit" && !failure->response_received,
                          "prior worker exit is distinct from a reported failure");
                Check(display.find(std::string_view(scenario) == "failed" ? L"ack_write=completed_not_processed" :
                                   L"ack_write=unconfirmed") != std::wstring::npos &&
                      display.find(std::string_view(scenario) == "failed" || std::string_view(scenario) == "ackfail" ?
                                   L"close_receipt=complete_reported" :
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
    std::cout << "{\"mode\":\"" << (ack_only ? "stub-parent-ack-write-fault" : "stub-parent-reply-contract") <<
        "\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
std::string ReadWholeFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}
// Coarse net for nonce/epoch/serial-shaped values: the literal words, or any
// run of 12+ hex characters that includes at least one a-f letter (so a plain
// decimal tickCount64/sequence/exit-code/byte-count value, which never
// contains a letter, can never trip this). The journal schema has no field
// that could carry such a value in the first place; this only guards against
// a future regression widening that schema.
bool ContainsSuspiciousValue(const std::string& content) {
    static const std::regex pattern(R"(nonce|serial|epoch|[0-9a-f]*[a-f][0-9a-f]{11,})");
    return std::regex_search(content, pattern);
}
struct JournalEvent {
    std::string name;
    std::uint64_t value{};
};
using JournalEvents = std::vector<JournalEvent>;
// Every line must have exactly the PreviewRunJournal shape with consecutive
// sequence numbers; anything else becomes "<unparsed>", which no expected
// sequence below can match.
JournalEvents ParseJournal(const std::string& content) {
    static const std::regex line(R"re(\{"sequence":(\d+),"tickCount64":\d+,"event":"([a-z_]+)","value":(\d+)\})re");
    JournalEvents events;
    std::istringstream stream(content);
    std::string text;
    std::uint64_t expected_sequence = 1;
    while (std::getline(stream, text)) {
        std::smatch match;
        if (std::regex_match(text, match, line) && std::stoull(match[1].str()) == expected_sequence)
            events.push_back({match[2].str(), std::stoull(match[3].str())});
        else
            events.push_back({"<unparsed>", 0});
        ++expected_sequence;
    }
    return events;
}
JournalEvents ReadJournalEvents(const fs::path& path) { return ParseJournal(ReadWholeFile(path)); }
bool SameEvents(const JournalEvents& actual, std::size_t start, const JournalEvents& expected) {
    if (start > actual.size() || actual.size() - start < expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[start + index].name != expected[index].name || actual[start + index].value != expected[index].value)
            return false;
    }
    return true;
}
void DumpEvents(const JournalEvents& events) {
    // Fixed event names and numbers only; safe to print.
    for (const auto& event : events) std::cerr << "  " << event.name << '=' << event.value << '\n';
}
void CheckEventsAt(const JournalEvents& actual, std::size_t start, const JournalEvents& expected, const char* message) {
    const bool same = SameEvents(actual, start, expected);
    Check(same, message);
    if (!same) DumpEvents(actual);
}
void CheckEventsExactly(const JournalEvents& actual, const JournalEvents& expected, const char* message) {
    const bool same = actual.size() == expected.size() && SameEvents(actual, 0, expected);
    Check(same, message);
    if (!same) DumpEvents(actual);
}
std::size_t CountEvents(const JournalEvents& events, std::string_view name) {
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
        [&](const JournalEvent& event) { return event.name == name; }));
}
// One worker's journal block: its worker_exit_observation_index line and the
// worker_exit_* and worker_close_* lines after it, up to the next index line.
// Only the first block for that worker (the one written with the details).
JournalEvents WorkerBlock(const JournalEvents& events, std::uint64_t worker) {
    JournalEvents block;
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (events[index].name != "worker_exit_observation_index" || events[index].value != worker) continue;
        block.push_back(events[index]);
        for (++index; index < events.size(); ++index) {
            const auto& event = events[index];
            if (event.name == "worker_exit_observation_index") break;
            if (event.name.rfind("worker_exit_", 0) != 0 && event.name.rfind("worker_close_", 0) != 0) break;
            block.push_back(event);
        }
        break;
    }
    return block;
}
// The close reply summary of a validated `closed` receipt with all five fields set.
JournalEvents ClosedReplyBlock() {
    return {{"worker_close_status_closed"}, {"worker_close_ack_write_completed"},
            {"worker_close_receipt_live_view_off", 1}, {"worker_close_receipt_source_closed", 1},
            {"worker_close_receipt_module_closed", 1}, {"worker_close_receipt_process_claim_released", 1},
            {"worker_close_receipt_safe_to_exit", 1}};
}
JournalEvents Concat(JournalEvents head, const JournalEvents& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}
JournalEvents CleanExitBlock(std::uint64_t worker) {
    return {{"worker_exit_observation_index", worker}, {"worker_exit_check_waited_after_close_ack"},
            {"worker_exit_code", 0}};
}
bool IsJournalEventName(std::string_view name) {
    return !name.empty() && name.size() <= 48 &&
        std::all_of(name.begin(), name.end(), [](char character) { return (character >= 'a' && character <= 'z') || character == '_'; });
}

struct OwnerRun {
    bool closed{};
    bool repeated_close{};
    std::optional<PreviewWorkerFailureObservation> failure;
    std::array<PreviewWorkerExitObservation, 2> exits{};
    std::array<DWORD, 2> codes{}; // Test-side OS exit codes, observed after the owner is gone.
};
// Drives one fake-worker scenario through a real PreviewWorkerOwner:
//   "select"/"start"/"frame": enumerate succeeds, then Preview(0) fails at that stage
//   "missing": worker 0 exits with 3 after reading enumerate, before replying
//   "late":    no command before Close(); worker 0 closes cleanly, worker 1 exits
//              with 3 after reading its close request, before replying
// Returns 0, or 3 when a fake worker's exit could not be confirmed (keep marker).
int RunOwnerScenario(const fs::path& executable, const fs::path& root, const std::string& name,
                     std::string_view scenario, OwnerRun& run) {
    std::array<OwnedHandle, 2> processes;
    {
        PreviewWorkerOwner owner(name, root, executable, kCommandingTestLifetime);
        const auto ids = owner.ProcessIds();
        for (std::size_t index = 0; index < ids.size(); ++index)
            processes[index].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[index]);
        if (scenario == "missing") {
            bool rejected{};
            try { (void)owner.Enumerate(0); } catch (const TransportError&) { rejected = true; }
            Check(rejected, "missing reply propagates out of Enumerate");
        } else if (scenario == "select" || scenario == "start" || scenario == "frame") {
            const auto candidates = owner.Enumerate(0);
            Check(candidates[0] == "staged-cand-0" && candidates[1] == "staged-cand-1",
                  "staged fixture enumerate succeeds before the targeted stage fails");
            bool rejected{};
            try { (void)owner.Preview(0, candidates[0]); } catch (const TransportError&) { rejected = true; }
            Check(rejected, "targeted stage failure propagates out of Preview");
        }
        // After a failed command the owner has already closed internally, so
        // both calls return the cached verdict without resending close. For
        // "late" the first call is the actual close attempt.
        run.closed = owner.Close();
        run.repeated_close = owner.Close();
        run.failure = owner.FirstFailure();
        run.exits = owner.ExitObservations();
    }
    for (std::size_t index = 0; index < processes.size(); ++index) {
        DWORD code{};
        if (!processes[index].value || WaitForSingleObject(processes[index].value, 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(processes[index].value, &code)) return 3;
        run.codes[index] = code;
    }
    return 0;
}
// Exact test-created marker and directory only; both fake children were
// observed exited by RunOwnerScenario before this runs.
int RemoveQuarantinedFixture(const fs::path& root) {
    if (!DeleteFileW(Marker(root).c_str()) || !RemoveDirectoryW(root.c_str())) return 4;
    return 0;
}
fs::path LateClaim(const fs::path& base) { return base / L"late-first-close.claim"; }

// --category-table: the allow-list is the reviewed set, every entry is accepted
// by the real PreviewRunJournal, and every entry is journaled verbatim by the
// real recording function (never degraded to failure_category_unknown).
int CategoryTable(const fs::path& temporary) {
    // Reviewed set: every category thrown on the worker preview path
    // (nikon_sdk_transport.cpp, worker_preview_selection.hpp,
    // worker_preview_dispatcher.hpp, preview_worker_owner.cpp), the capture-path
    // categories of nikon_sdk_transport.cpp, delegation_disarm_unconfirmed, and
    // the dispatcher's worker_unexpected. Kept here independently of the
    // production table so that removing or adding a category is a visible,
    // reviewed change to both.
    static constexpr std::string_view required[] = {
        "ambiguous_image_event", "baseline_mismatch", "close_failed", "download_failed", "identity_collision",
        "image_event_timeout", "invalid_jpeg", "live_view_frame_failed", "live_view_invalid_frame",
        "live_view_not_started", "live_view_prohibited", "live_view_recovery_failed", "live_view_start_failed",
        "live_view_stop_already_attempted", "live_view_stop_failed", "live_view_unavailable", "open_failed",
        "sdk_load_failed", "session_busy", "session_mode_mismatch", "session_not_open", "session_poisoned",
        "transaction_watchdog", "worker_ack_unconfirmed", "worker_authority", "worker_authority_expired",
        "worker_authority_missing", "worker_camera_type_failed", "worker_camera_type_mismatch", "worker_candidate",
        "worker_candidate_unavailable", "worker_envelope", "worker_exit_unconfirmed", "worker_frame_size",
        "worker_handoff_unavailable", "worker_handoff_unconfirmed", "worker_inventory", "worker_inventory_failed",
        "worker_inventory_not_pair", "worker_ipc_unconfirmed", "worker_live_view_already_on",
        "worker_operation_failed", "worker_operation_unavailable", "worker_owner_failed", "worker_prior_exit",
        "worker_reply_invalid", "worker_selection_invalidated", "worker_selection_required", "worker_sequence",
        "worker_session_required", "worker_source_required", "worker_stop_required", "worker_terminal",
        "worker_topology_changed", "worker_topology_failed", "delegation_disarm_unconfirmed", "worker_unexpected",
        // T2: the parent's close-step classification and the gated build's adapter category.
        "worker_stop_in_progress", "licensed_adapter_unavailable",
        // T2: a command refused by the owner past its operation deadline.
        "owner_operation_deadline_expired",
        // Topology diagnostics: the split of worker_selection_invalidated, which stays above.
        "worker_selection_inventory_changed", "worker_selection_topology_event",
    };
    Check(std::size(required) == 62, "reviewed set has 62 categories");
    Check(IsKnownFailureCategory("worker_selection_invalidated"),
          "the pre-split category stays readable for older journals");
    const auto table_begin = std::begin(kPreviewWorkerFailureCategories);
    const auto table_end = std::end(kPreviewWorkerFailureCategories);
    for (const auto category : required) {
        if (!IsKnownFailureCategory(category)) {
            Check(false, "reviewed failure category is missing from kPreviewWorkerFailureCategories");
            std::cerr << "  missing: " << category << '\n';
        }
    }
    for (auto entry = table_begin; entry != table_end; ++entry) {
        Check(std::find(std::begin(required), std::end(required), *entry) != std::end(required),
              "table entry is part of the reviewed set");
        Check(std::count(table_begin, table_end, *entry) == 1, "table entry is unique");
        Check(IsKnownFailureCategory(*entry), "every table entry passes IsKnownFailureCategory");
        Check(IsJournalEventName(*entry), "every table entry is a 1..48 char [a-z_] event name");
    }
    Check(std::size(kPreviewWorkerFailureCategories) == std::size(required), "table and reviewed set have the same size");
    for (const auto operation : kPreviewWorkerJournalOperations) {
        Check(IsKnownOperation(operation), "every operation passes IsKnownOperation");
        Check(IsJournalEventName("failure_operation_" + std::string(operation)),
              "failure_operation_<name> is a 1..48 char [a-z_] event name");
        Check(!IsKnownFailureCategory("failure_operation_" + std::string(operation)),
              "operation events never collide with category events");
    }
    for (const auto rejected : {std::string_view{}, std::string_view{"injected_select_failure"},
                                std::string_view{"OPEN_FAILED"}, std::string_view{"open_failed "},
                                std::string_view{"live_view"}, std::string_view{"failure_category_unknown"}}) {
        Check(!IsKnownFailureCategory(rejected), "values outside the table are not known categories");
    }
    Check(!IsKnownOperation("capture") && !IsKnownOperation("") && !IsKnownOperation("Close"),
          "values outside the operation table are not known operations");

    // Real journal: Record() rejects any event outside [a-z_]{1,48}, so writing
    // every entry through RecordWorkerFailureToJournal proves acceptance.
    const auto journal_path = temporary / (L"A0WorkerCategoryTable-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    std::size_t operation_index{};
    {
        PreviewRunJournal journal(journal_path);
        for (const auto category : kPreviewWorkerFailureCategories) {
            PreviewWorkerFailureObservation failure;
            failure.worker_index = 0;
            failure.operation = std::string(kPreviewWorkerJournalOperations[operation_index++ % std::size(kPreviewWorkerJournalOperations)]);
            failure.category = std::string(category);
            try { RecordWorkerFailureToJournal(journal, failure); }
            catch (const std::exception&) {
                Check(false, "PreviewRunJournal accepts every table entry");
                std::cerr << "  rejected: " << category << '\n';
            }
        }
    }
    const auto events = ReadJournalEvents(journal_path);
    std::size_t position{};
    operation_index = 0;
    for (const auto category : kPreviewWorkerFailureCategories) {
        const auto operation = kPreviewWorkerJournalOperations[operation_index++ % std::size(kPreviewWorkerJournalOperations)];
        // failure_worker_index, failure_operation_<op>, <category>, then 6 fixed lines.
        CheckEventsAt(events, position, {{"failure_worker_index", 0}, {"failure_operation_" + std::string(operation)},
                                         {std::string(category)}},
                      "each known category is journaled verbatim right after its operation");
        position += 3 + 4; // response, status, ack, close_receipt_missing
    }
    Check(position == events.size(), "category table journal has exactly one block per category");
    Check(CountEvents(events, "failure_category_unknown") == 0, "no table entry degrades to failure_category_unknown");
    Check(CountEvents(events, "failure_operation_unknown") == 0, "no table operation degrades to failure_operation_unknown");
    Check(!ContainsSuspiciousValue(ReadWholeFile(journal_path)), "journal contains no nonce/epoch/serial-shaped value");
    if (!DeleteFileW(journal_path.c_str())) return 4;
    std::cout << "{\"mode\":\"stub-failure-category-table\",\"categories\":" << std::size(kPreviewWorkerFailureCategories)
              << ",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

// --journal-vocabulary: every branch of the recording functions with synthetic
// observations, in the exact order production writes them. In-process only.
int JournalVocabulary(const fs::path& temporary) {
    const auto journal_path = temporary / (L"A0WorkerJournalVocabulary-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    JournalEvents expected;
    const auto append = [&](const JournalEvents& more) { expected.insert(expected.end(), more.begin(), more.end()); };
    bool all_returned_true = true;
    bool details_after_first = false, details_after_second = false;
    {
        PreviewRunJournal journal(journal_path);
        // Owner-wide disarm failure: worker index 2, nothing received.
        PreviewWorkerFailureObservation disarm;
        disarm.worker_index = 2;
        disarm.operation = "disarm";
        disarm.category = "delegation_disarm_unconfirmed";
        RecordWorkerFailureToJournal(journal, disarm);
        append({{"failure_worker_index", 2}, {"failure_operation_disarm"}, {"delegation_disarm_unconfirmed"},
                {"worker_response_missing"}, {"worker_status_missing"}, {"worker_ack_write_unconfirmed"},
                {"close_receipt_missing"}});
        // Received but invalid; unknown operation; unknown category.
        PreviewWorkerFailureObservation invalid;
        invalid.worker_index = 1;
        invalid.operation = "capture";
        invalid.category = "injected_select_failure";
        invalid.response_received = true;
        RecordWorkerFailureToJournal(journal, invalid);
        append({{"failure_worker_index", 1}, {"failure_operation_unknown"}, {"failure_category_unknown"},
                {"worker_response_invalid"}, {"worker_status_missing"}, {"worker_ack_write_unconfirmed"},
                {"close_receipt_missing"}});
        // Validated replies: each status, empty category, mixed receipt values
        // (each of the five fields maps to its own line).
        const std::pair<PreviewWorkerReplyStatus, const char*> statuses[] = {
            {PreviewWorkerReplyStatus::ok, "worker_status_ok"},
            {PreviewWorkerReplyStatus::failed, "worker_status_failed"},
            {PreviewWorkerReplyStatus::closed, "worker_status_closed"},
            {PreviewWorkerReplyStatus::quarantined, "worker_status_quarantined"},
        };
        for (const auto& [status, status_event] : statuses) {
            PreviewWorkerFailureObservation validated;
            validated.worker_index = 0;
            validated.operation = "frame";
            validated.response_received = true;
            validated.response_validated = true;
            validated.ack_write_completed = true;
            validated.response_status = status;
            validated.reported_close = PreviewWorkerCloseReceipt{true, false, true, false, true};
            RecordWorkerFailureToJournal(journal, validated);
            append({{"failure_worker_index", 0}, {"failure_operation_frame"}, {"failure_category_missing"},
                    {"worker_response_validated"}, {status_event}, {"worker_ack_write_completed"},
                    {"close_receipt_live_view_off", 1}, {"close_receipt_source_closed", 0},
                    {"close_receipt_module_closed", 1}, {"close_receipt_process_claim_released", 0},
                    {"close_receipt_safe_to_exit", 1}});
        }
        // Exit observations: every kind and result.
        const auto check = [](bool exited, bool code_available, std::uint32_t code, bool wait_failed = false) {
            PreviewWorkerProcessCheck result;
            result.exited = exited;
            result.code_available = code_available;
            result.exit_code = code;
            result.wait_failed = wait_failed;
            return result;
        };
        const auto exit_observation = [](PreviewWorkerExitCheckKind kind, PreviewWorkerProcessCheck at_close,
                                         std::optional<PreviewWorkerProcessCheck> recheck = std::nullopt) {
            PreviewWorkerExitObservation result;
            result.kind = kind;
            result.at_close = at_close;
            result.after_both_closes = recheck;
            return result;
        };
        using Kind = PreviewWorkerExitCheckKind;
        RecordWorkerExitObservationToJournal(journal, 0, PreviewWorkerExitObservation{});
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_not_run"}});
        RecordWorkerExitObservationToJournal(journal, 1, exit_observation(Kind::waited_after_close_ack, check(true, true, 0)));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_after_close_ack"}, {"worker_exit_code", 0}});
        RecordWorkerExitObservationToJournal(journal, 0, exit_observation(Kind::waited_after_close_ack, check(false, false, 0)));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_close_ack"},
                {"worker_exit_wait_timed_out"}});
        RecordWorkerExitObservationToJournal(journal, 1, exit_observation(Kind::waited_after_close_ack, check(true, false, 0)));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_after_close_ack"},
                {"worker_exit_code_unavailable"}});
        RecordWorkerExitObservationToJournal(journal, 0, exit_observation(Kind::waited_after_close_ack, check(false, false, 0, true)));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_close_ack"},
                {"worker_exit_wait_failed"}});
        RecordWorkerExitObservationToJournal(journal, 1, exit_observation(Kind::instant_at_close_failure, check(true, true, 3)));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_instant_at_close_failure"},
                {"worker_exit_code", 3}});
        RecordWorkerExitObservationToJournal(journal, 0, exit_observation(Kind::instant_at_close_failure, check(false, false, 0),
                                                                          check(true, true, 5)));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_instant_at_close_failure"},
                {"worker_exit_not_observed_at_close"}, {"worker_exit_recheck_after_both_closes"}, {"worker_exit_code", 5}});
        RecordWorkerExitObservationToJournal(journal, 1, exit_observation(Kind::instant_at_close_failure, check(false, false, 0),
                                                                          check(false, false, 0)));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_instant_at_close_failure"},
                {"worker_exit_not_observed_at_close"}, {"worker_exit_recheck_after_both_closes"},
                {"worker_exit_not_observed_at_recheck"}});
        RecordWorkerExitObservationToJournal(journal, 0, exit_observation(Kind::instant_at_close_failure, check(false, false, 0, true),
                                                                          check(true, false, 0)));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_instant_at_close_failure"},
                {"worker_exit_wait_failed"}, {"worker_exit_recheck_after_both_closes"}, {"worker_exit_code_unavailable"}});
        // T2 exit windows, later looks, and the close reply summary.
        // `sent` here also sets `attempted`: a written request was attempted.
        const auto reply = [](bool sent, bool received, bool validated, bool ack,
                              std::optional<PreviewWorkerReplyStatus> status,
                              std::optional<PreviewWorkerCloseReceipt> receipt) {
            PreviewWorkerCloseReply result;
            result.attempted = sent;
            result.sent = sent;
            result.response_received = received;
            result.response_validated = validated;
            result.ack_write_completed = ack;
            result.status = status;
            result.receipt = receipt;
            return result;
        };
        const auto with_reply = [](PreviewWorkerExitObservation observation, const PreviewWorkerCloseReply& close_reply) {
            observation.close_reply = close_reply;
            return observation;
        };
        const auto with_repeated = [](PreviewWorkerExitObservation observation, PreviewWorkerProcessCheck look,
                                      std::uint32_t looks) {
            observation.at_repeated_close = look;
            observation.repeated_close_looks = looks;
            return observation;
        };
        using Status = PreviewWorkerReplyStatus;
        const PreviewWorkerCloseReceipt safe{true, true, true, true, true};
        const PreviewWorkerCloseReceipt unsafe{false, false, false, false, false};
        const PreviewWorkerCloseReceipt mixed{true, false, true, false, true};
        const auto not_sent = reply(false, false, false, false, std::nullopt, std::nullopt);
        const auto closed_reply = reply(true, true, true, true, Status::closed, safe);
        // E_ok after a closed receipt; each of the five receipt fields maps to its own line.
        RecordWorkerExitObservationToJournal(journal, 1, with_reply(exit_observation(Kind::waited_after_close_ack, check(true, true, 0)),
                                                                    reply(true, true, true, true, Status::closed, mixed)));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_after_close_ack"}, {"worker_exit_code", 0},
                {"worker_close_status_closed"}, {"worker_close_ack_write_completed"},
                {"worker_close_receipt_live_view_off", 1}, {"worker_close_receipt_source_closed", 0},
                {"worker_close_receipt_module_closed", 1}, {"worker_close_receipt_process_claim_released", 0},
                {"worker_close_receipt_safe_to_exit", 1}});
        // E_ok after a failed command's safe receipt; close was not sent.
        RecordWorkerExitObservationToJournal(journal, 0, with_reply(exit_observation(Kind::waited_after_failure_receipt,
                                                                                     check(true, true, 3)), not_sent));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_failure_receipt"},
                {"worker_exit_code", 3}, {"worker_close_not_sent"}});
        RecordWorkerExitObservationToJournal(journal, 1, with_reply(exit_observation(Kind::waited_after_failure_receipt,
                                                                                     check(false, false, 0, true)),
                                                                    reply(true, true, true, false, Status::failed, safe)));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_after_failure_receipt"},
                {"worker_exit_wait_failed"}, {"worker_close_status_failed"}, {"worker_close_ack_write_unconfirmed"},
                {"worker_close_receipt_live_view_off", 1}, {"worker_close_receipt_source_closed", 1},
                {"worker_close_receipt_module_closed", 1}, {"worker_close_receipt_process_claim_released", 1},
                {"worker_close_receipt_safe_to_exit", 1}});
        // E_fail: close sent but unanswered; still running at the window end,
        // at the recheck, and seen exiting by a repeated Close().
        RecordWorkerExitObservationToJournal(journal, 0, with_reply(with_repeated(
            exit_observation(Kind::waited_for_worker_cleanup, check(false, false, 0), check(false, false, 0)),
            check(true, true, 0), 1), reply(true, false, false, false, std::nullopt, std::nullopt)));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_for_worker_cleanup"},
                {"worker_exit_wait_timed_out"}, {"worker_exit_recheck_after_both_closes"},
                {"worker_exit_not_observed_at_recheck"}, {"worker_exit_recheck_at_repeated_close"}, {"worker_exit_code", 0},
                {"worker_close_response_missing"}, {"worker_close_receipt_missing"}});
        // E_fail: an invalid close reply; exit code inside the window.
        RecordWorkerExitObservationToJournal(journal, 1, with_reply(exit_observation(Kind::waited_for_worker_cleanup,
                                                                                     check(true, true, 3)),
                                                                    reply(true, true, false, false, std::nullopt, std::nullopt)));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_for_worker_cleanup"},
                {"worker_exit_code", 3}, {"worker_close_response_invalid"}, {"worker_close_receipt_missing"}});
        // 0 ms after a quarantined close reply; a repeated Close() still sees it running.
        RecordWorkerExitObservationToJournal(journal, 0, with_reply(with_repeated(
            exit_observation(Kind::instant_at_close_failure, check(false, false, 0), check(false, false, 0)),
            check(false, false, 0), 2), reply(true, true, true, true, Status::quarantined, unsafe)));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_instant_at_close_failure"},
                {"worker_exit_not_observed_at_close"}, {"worker_exit_recheck_after_both_closes"},
                {"worker_exit_not_observed_at_recheck"}, {"worker_exit_recheck_at_repeated_close"},
                {"worker_exit_not_observed_at_repeated_close"}, {"worker_close_status_quarantined"},
                {"worker_close_ack_write_completed"}, {"worker_close_receipt_live_view_off", 0},
                {"worker_close_receipt_source_closed", 0}, {"worker_close_receipt_module_closed", 0},
                {"worker_close_receipt_process_claim_released", 0}, {"worker_close_receipt_safe_to_exit", 0}});
        // Close attempted but not written in full (the worker had already
        // ended): worker_close_not_delivered, no response or receipt lines.
        PreviewWorkerCloseReply undelivered;
        undelivered.attempted = true;
        RecordWorkerExitObservationToJournal(journal, 1, with_reply(exit_observation(Kind::waited_for_worker_cleanup,
                                                                                     check(true, true, 3)), undelivered));
        append({{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_for_worker_cleanup"},
                {"worker_exit_code", 3}, {"worker_close_not_delivered"}});
        // Window flags: each written right after the window result and before
        // the later looks, capped first.
        const auto flagged = [](PreviewWorkerExitObservation observation, bool capped, bool looked_after) {
            observation.window_capped_by_session_limit = capped;
            observation.looked_after_window_end = looked_after;
            return observation;
        };
        RecordWorkerExitObservationToJournal(journal, 0, with_reply(flagged(exit_observation(
            Kind::waited_after_close_ack, check(true, true, 0)), true, false), closed_reply));
        append(Concat({{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_close_ack"},
                       {"worker_exit_code", 0}, {"worker_exit_window_capped_by_session_limit"}}, ClosedReplyBlock()));
        RecordWorkerExitObservationToJournal(journal, 1, with_reply(flagged(exit_observation(
            Kind::waited_after_close_ack, check(false, false, 0), check(false, false, 0)), false, true), closed_reply));
        append(Concat({{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_after_close_ack"},
                       {"worker_exit_wait_timed_out"}, {"worker_exit_looked_after_window_end"},
                       {"worker_exit_recheck_after_both_closes"}, {"worker_exit_not_observed_at_recheck"}},
                      ClosedReplyBlock()));
        RecordWorkerExitObservationToJournal(journal, 0, with_reply(flagged(exit_observation(
            Kind::waited_for_worker_cleanup, check(true, true, 3)), true, true), reply(true, false, false, false,
                                                                                    std::nullopt, std::nullopt)));
        append({{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_for_worker_cleanup"},
                {"worker_exit_code", 3}, {"worker_exit_window_capped_by_session_limit"},
                {"worker_exit_looked_after_window_end"}, {"worker_close_response_missing"},
                {"worker_close_receipt_missing"}});
        // Production close outcome, not closed, no failure observation, called
        // as successive CloseOnce calls would: the verdict every time, the
        // details once, then each new repeated-close look once.
        const auto held = exit_observation(Kind::instant_at_close_failure, check(false, false, 0), check(false, false, 0));
        const JournalEvents held_lines{{"worker_exit_observation_index", 1}, {"worker_exit_check_instant_at_close_failure"},
                                       {"worker_exit_not_observed_at_close"}, {"worker_exit_recheck_after_both_closes"},
                                       {"worker_exit_not_observed_at_recheck"}};
        const auto clean_exit = with_reply(exit_observation(Kind::waited_after_close_ack, check(true, true, 0)), closed_reply);
        std::array<PreviewWorkerExitObservation, 2> exits{clean_exit, held};
        PreviewCloseJournalState state;
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, state) && all_returned_true;
        details_after_first = state.details_recorded;
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, state) && all_returned_true;
        details_after_second = state.details_recorded;
        // Topology blocks after both exit blocks: no failure observation (2,
        // reason 0), worker 0's validated close reply without counters (reason
        // 0, defense in depth), worker 1 without a close reply (reason 1).
        const JournalEvents topo_lines{{"topo_block_failure", 2}, {"topo_unavailable", 0}, {"topo_block_close", 0},
                                       {"topo_unavailable", 0}, {"topo_block_close", 1}, {"topo_unavailable", 1}};
        append({{"close_unconfirmed"}, {"failure_observation_missing"}});
        append(Concat(CleanExitBlock(0), ClosedReplyBlock()));
        append(held_lines);
        append(topo_lines);
        append({{"close_unconfirmed"}});
        exits[1] = with_repeated(held, check(false, false, 0), 1);
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, state) && all_returned_true;
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, state) && all_returned_true;
        append({{"close_unconfirmed"}, {"worker_exit_observation_index", 1}, {"worker_exit_recheck_at_repeated_close"},
                {"worker_exit_not_observed_at_repeated_close"}, {"close_unconfirmed"}});
        exits[1] = with_repeated(held, check(true, true, 9), 2);
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, state) && all_returned_true;
        append({{"close_unconfirmed"}, {"worker_exit_observation_index", 1}, {"worker_exit_recheck_at_repeated_close"},
                {"worker_exit_code", 9}});
        // Details first written after a repeated look: the look is part of the
        // exit block and is not written again by the next call.
        PreviewCloseJournalState late_state;
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, late_state) && all_returned_true;
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, late_state) && all_returned_true;
        append({{"close_unconfirmed"}, {"failure_observation_missing"}});
        append(Concat(CleanExitBlock(0), ClosedReplyBlock()));
        append(held_lines);
        append({{"worker_exit_recheck_at_repeated_close"}, {"worker_exit_code", 9}});
        append(topo_lines);
        append({{"close_unconfirmed"}});
        // Production close outcome, closed: verdict then exit blocks, no failure block.
        const std::array<PreviewWorkerExitObservation, 2> clean_exits{clean_exit, clean_exit};
        PreviewCloseJournalState closed_state;
        all_returned_true = RecordCloseOutcomeToJournal(journal, true, std::nullopt, clean_exits, closed_state) &&
            all_returned_true;
        append({{"both_workers_close_verified"}});
        append(Concat(CleanExitBlock(0), ClosedReplyBlock()));
        append(Concat(CleanExitBlock(1), ClosedReplyBlock()));
        append({{"topo_block_close", 0}, {"topo_unavailable", 0}, {"topo_block_close", 1}, {"topo_unavailable", 0}});
    }
    const auto events = ReadJournalEvents(journal_path);
    CheckEventsExactly(events, expected, "every recording branch writes its fixed lines in production order");
    Check(all_returned_true, "RecordCloseOutcomeToJournal reports every line written");
    Check(details_after_first && details_after_second, "details flag is set by the first close outcome and stays set");
    // Fixed (non-category) vocabulary never collides with a category name, so a
    // reader can tell the category line apart by name alone.
    for (const auto& event : expected) {
        if (event.name == "delegation_disarm_unconfirmed") continue; // The one category written on purpose above.
        Check(!IsKnownFailureCategory(event.name), "fixed journal vocabulary never collides with a failure category");
    }
    for (const auto verdict : {"run_started", "close_unconfirmed", "both_workers_close_verified", "startup_quarantined",
                               "no_active_owner", "operation_failed", "display_failed", "startup_no_workers"}) {
        Check(!IsKnownFailureCategory(verdict), "commissioning verdict events never collide with a failure category");
    }
    Check(!ContainsSuspiciousValue(ReadWholeFile(journal_path)), "journal contains no nonce/epoch/serial-shaped value");
    if (!DeleteFileW(journal_path.c_str())) return 4;
    std::cout << "{\"mode\":\"stub-failure-journal-vocabulary\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

// --exit-recheck: a worker that exits before replying is observed inside its
// E_fail window, so its exit code is a number on every attempt (T2/WU2).
// The mode name is kept from T1.
bool CodeInCleanupWindow(const PreviewWorkerExitObservation& observation, std::uint32_t code) {
    return observation.kind == PreviewWorkerExitCheckKind::waited_for_worker_cleanup && !observation.at_close.wait_failed &&
        observation.at_close.exited && observation.at_close.code_available && observation.at_close.exit_code == code &&
        !observation.after_both_closes;
}
bool CleanAfterClosedReceipt(const PreviewWorkerExitObservation& observation) {
    return observation.kind == PreviewWorkerExitCheckKind::waited_after_close_ack && observation.at_close.exited &&
        observation.at_close.code_available && observation.at_close.exit_code == 0 && !observation.after_both_closes &&
        observation.close_reply && observation.close_reply->sent && observation.close_reply->response_validated &&
        observation.close_reply->status == PreviewWorkerReplyStatus::closed && observation.close_reply->ack_write_completed &&
        observation.close_reply->receipt && observation.close_reply->receipt->Complete();
}
int ExitRecheck(const fs::path& executable, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerExitRecheck-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    constexpr int kCase4Attempts = 5;
    constexpr int kCase4bAttempts = 3;
    int case4_codes{}, case4b_codes{};
    // Case 4: worker 0 exits with 3 before replying to enumerate. Close is not
    // sent to it; its E_fail window starts at the close step.
    for (int attempt = 1; attempt <= kCase4Attempts; ++attempt) {
        const auto root = base / "missing";
        const auto name = "A0.Poc.TestLease.Recheck." + std::to_string(GetCurrentProcessId()) + ".missing." +
            std::to_string(attempt);
        OwnerRun run;
        if (const auto code = RunOwnerScenario(executable, root, name, "missing", run)) return code;
        Check(!run.closed && !run.repeated_close, "worker exit before reply is terminal without a second close");
        Check(run.codes[0] == 3 && run.codes[1] == 0, "fake workers exit as the missing fixture expects");
        const bool numeric = CodeInCleanupWindow(run.exits[0], 3);
        Check(numeric, "case 4: worker 0's exit code 3 is observed inside its E_fail window");
        if (numeric) ++case4_codes;
        Check(run.exits[0].close_reply && !run.exits[0].close_reply->sent,
              "case 4: close is never sent after the unconfirmed command");
        Check(CleanAfterClosedReceipt(run.exits[1]), "paired worker exit code 0 comes from its E_ok window");
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // Case 4b: worker 1 exits with 3 after reading its own close request.
    for (int attempt = 1; attempt <= kCase4bAttempts; ++attempt) {
        const auto root = base / "late";
        const auto name = "A0.Poc.TestLease.Recheck." + std::to_string(GetCurrentProcessId()) + ".late." +
            std::to_string(attempt);
        OwnerRun run;
        if (const auto code = RunOwnerScenario(executable, root, name, "late", run)) return code;
        Check(!run.closed && !run.repeated_close, "late worker exit is terminal without a second close");
        Check(run.codes[0] == 0 && run.codes[1] == 3, "worker 0 closes cleanly and worker 1 exits with 3");
        Check(run.failure.has_value() && run.failure->worker_index == 1 && run.failure->operation == "close" &&
              run.failure->category == "worker_ipc_unconfirmed" && !run.failure->response_received,
              "first failure names worker 1's unanswered close");
        Check(CleanAfterClosedReceipt(run.exits[0]), "worker 0 exit code 0 comes from its E_ok window");
        const bool numeric = CodeInCleanupWindow(run.exits[1], 3);
        Check(numeric, "case 4b: worker 1's exit code 3 is observed inside its E_fail window");
        if (numeric) ++case4b_codes;
        Check(run.exits[1].close_reply && run.exits[1].close_reply->sent && !run.exits[1].close_reply->response_received,
              "case 4b: close was sent once and never answered");
        if (!DeleteFileW(LateClaim(base).c_str())) return 4;
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    Check(case4_codes == kCase4Attempts && case4b_codes == kCase4bAttempts, "every attempt records the numeric exit code");
    Check(RemoveDirectoryW(base.c_str()), "exit recheck fixture root removed");
    std::cout << "{\"mode\":\"stub-worker-exit-recheck\",\"case4Codes\":" << case4_codes
              << ",\"case4Attempts\":" << kCase4Attempts << ",\"case4bCodes\":" << case4b_codes
              << ",\"case4bAttempts\":" << kCase4bAttempts << ",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

JournalEvents ClosedWorkerBlock(std::uint64_t worker) { return Concat(CleanExitBlock(worker), ClosedReplyBlock()); }
// A topology block with values: the header, then the 18 counter lines in table order.
JournalEvents TopoBlock(std::string_view header, std::uint64_t index, const PreviewTopologyDiag& diag) {
    JournalEvents block{{std::string(header), index}};
    for (std::size_t field = 0; field < kPreviewTopologyDiagFieldCount; ++field)
        block.push_back({std::string(kPreviewTopologyDiagFields[field].journal_event), diag.values[field]});
    return block;
}
// A topology block without values: the header, then topo_unavailable(reason).
JournalEvents TopoUnavailable(std::string_view header, std::uint64_t index, std::uint64_t reason) {
    return {{std::string(header), index}, {"topo_unavailable", reason}};
}
std::size_t FindEvent(const JournalEvents& events, std::string_view name) {
    const auto found = std::find_if(events.begin(), events.end(), [&](const JournalEvent& event) { return event.name == name; });
    return static_cast<std::size_t>(found - events.begin());
}

// --journal-contract: journals what a real owner produced, through the same
// RecordCloseOutcomeToJournal call preview_commissioning_main.cpp's CloseOnce
// makes, then reads the result back through a separate handle while the writer
// stays open (the screen-process-dies-mid-quarantine scenario).
int JournalContract(const fs::path& executable, const fs::path& worker, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerJournalContract-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    // Cases 1-3: a bound, validated `failed` reply at select/start/frame.
    for (const char* stage : {"select", "start", "frame"}) {
        const auto root = base / stage;
        const auto name = "A0.Poc.TestLease.Journal." + std::to_string(GetCurrentProcessId()) + "." + stage;
        OwnerRun run;
        if (const auto code = RunOwnerScenario(executable, root, name, stage, run)) return code;
        Check(!run.closed && !run.repeated_close, "staged failure is terminal without a second close");
        Check(run.codes[0] == 3 && run.codes[1] == 0, "failing worker and paired worker exit as the staged fixture expects");
        const std::string expected_category = std::string(stage) == "select" ? "open_failed" :
            std::string(stage) == "start" ? "live_view_start_failed" : "live_view_frame_failed";
        const auto& failure = run.failure;
        Check(failure.has_value() && failure->worker_index == 0 && failure->operation == stage &&
              failure->category == expected_category && failure->response_validated && failure->ack_write_completed &&
              failure->reported_close && failure->reported_close->Complete(),
              "owner retains worker/operation/category/receipt for the targeted stage");
        const auto journal_path = base / (std::string(stage) + ".jsonl");
        {
            PreviewRunJournal journal(journal_path);
            journal.Record("run_started");
            PreviewCloseJournalState state;
            Check(RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, state),
                  "close outcome fully journaled");
            const auto content = ReadWholeFile(journal_path); // Separate handle; writer handle is still open.
            const auto events = ParseJournal(content);
            CheckEventsAt(events, 0, {{"run_started"}, {"close_unconfirmed"},
                {"failure_worker_index", 0}, {"failure_operation_" + std::string(stage)}, {expected_category},
                {"worker_response_validated"}, {"worker_status_failed"}, {"worker_ack_write_completed"},
                {"close_receipt_live_view_off", 1}, {"close_receipt_source_closed", 1},
                {"close_receipt_module_closed", 1}, {"close_receipt_process_claim_released", 1},
                {"close_receipt_safe_to_exit", 1}},
                "journal retains verdict, worker, command, known category, response, status, ACK and all five receipt fields in order");
            Check(CountEvents(events, "failure_category_unknown") == 0, "a recognized category is never degraded to unknown");
            CheckEventsExactly(WorkerBlock(events, 0),
                {{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_failure_receipt"},
                 {"worker_exit_code", 3}, {"worker_close_not_sent"}},
                "journal retains the failing worker's exit code 3 from the E_ok window after its safe failure receipt");
            CheckEventsExactly(WorkerBlock(events, 1), ClosedWorkerBlock(1),
                               "journal retains the paired worker's clean exit code and its close reply summary");
            // The run-05 shape: the failing reply's diag, no close sent to the
            // failing worker, and the paired worker's close reply diag.
            const auto topo = FindEvent(events, "topo_block_failure");
            Check(topo > 0 && topo < events.size() && events[topo - 1].name == "worker_close_receipt_safe_to_exit",
                  "worker 1's close reply summary is the last exit block, right before the topology blocks");
            const auto topology = Concat(Concat(TopoBlock("topo_block_failure", 0, DiagValues(kFailureDiagBase)),
                                                TopoUnavailable("topo_block_close", 0, 1)),
                                         TopoBlock("topo_block_close", 1, DiagValues(kCloseDiagBase)));
            CheckEventsAt(events, topo, topology, "topology blocks: failure diag, close not sent to worker 0, worker 1 close diag");
            Check(topo + topology.size() == events.size(), "the topology blocks end the journal");
            Check(!ContainsSuspiciousValue(content), "journal contains no nonce/epoch/serial-shaped value");
        }
        if (!DeleteFileW(journal_path.c_str())) return 4;
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // Case 4: worker 0 exits with 3 before any reply reaches the parent. The
    // close outcome is journaled twice, as a retried CloseOnce would: details
    // only once.
    {
        const auto root = base / "missing";
        const auto name = "A0.Poc.TestLease.Journal." + std::to_string(GetCurrentProcessId()) + ".missing";
        OwnerRun run;
        if (const auto code = RunOwnerScenario(executable, root, name, "missing", run)) return code;
        Check(!run.closed && !run.repeated_close, "missing response is terminal without a second close");
        Check(run.codes[0] == 3 && run.codes[1] == 0, "worker that never replied still exits observably");
        Check(run.failure.has_value() && run.failure->category == "worker_ipc_unconfirmed" && !run.failure->response_received,
              "missing response leaves delivery unconfirmed");
        const auto journal_path = base / "missing.jsonl";
        {
            PreviewRunJournal journal(journal_path);
            journal.Record("run_started");
            PreviewCloseJournalState state;
            Check(RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, state) &&
                  RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, state),
                  "repeated close outcome fully journaled");
            const auto content = ReadWholeFile(journal_path);
            const auto events = ParseJournal(content);
            CheckEventsAt(events, 0, {{"run_started"}, {"close_unconfirmed"},
                {"failure_worker_index", 0}, {"failure_operation_enumerate"}, {"worker_ipc_unconfirmed"},
                {"worker_response_missing"}, {"worker_status_missing"}, {"worker_ack_write_unconfirmed"},
                {"close_receipt_missing"}},
                "journal retains the missing-response failure block in order");
            CheckEventsExactly(WorkerBlock(events, 0),
                {{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_for_worker_cleanup"},
                 {"worker_exit_code", 3}, {"worker_close_not_sent"}},
                "case 4: journal retains worker 0's exit code 3 from its E_fail window");
            CheckEventsExactly(WorkerBlock(events, 1), ClosedWorkerBlock(1), "journal retains the paired worker's clean exit code");
            Check(CountEvents(events, "close_unconfirmed") == 2 && CountEvents(events, "failure_worker_index") == 1 &&
                  CountEvents(events, "worker_exit_observation_index") == 2 && events.back().name == "close_unconfirmed",
                  "a repeated close records the verdict again but the failure and exit blocks once");
            // Request written, no response: reason 2. Close never sent to worker 0: reason 1.
            CheckEventsAt(events, FindEvent(events, "topo_block_failure"),
                          Concat(Concat(TopoUnavailable("topo_block_failure", 0, 2), TopoUnavailable("topo_block_close", 0, 1)),
                                 TopoBlock("topo_block_close", 1, DiagValues(kCloseDiagBase))),
                          "case 4: topology blocks name the missing response and worker 1's close diag");
            Check(CountEvents(events, "topo_block_failure") == 1 && CountEvents(events, "topo_block_close") == 2,
                  "case 4: the repeated close writes no topology block again");
            Check(!ContainsSuspiciousValue(content), "journal contains no nonce/epoch/serial-shaped value");
        }
        if (!DeleteFileW(journal_path.c_str())) return 4;
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // Case 4b: worker 1 exits with 3 after reading its own close request.
    {
        const auto root = base / "late";
        const auto name = "A0.Poc.TestLease.Journal." + std::to_string(GetCurrentProcessId()) + ".late";
        OwnerRun run;
        if (const auto code = RunOwnerScenario(executable, root, name, "late", run)) return code;
        Check(!run.closed && !run.repeated_close, "late worker exit is terminal without a second close");
        Check(run.codes[0] == 0 && run.codes[1] == 3, "worker 0 closes cleanly and worker 1 exits with 3");
        const auto journal_path = base / "late.jsonl";
        {
            PreviewRunJournal journal(journal_path);
            journal.Record("run_started");
            PreviewCloseJournalState state;
            Check(RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, state),
                  "close outcome fully journaled");
            const auto content = ReadWholeFile(journal_path);
            const auto events = ParseJournal(content);
            CheckEventsAt(events, 0, {{"run_started"}, {"close_unconfirmed"},
                {"failure_worker_index", 1}, {"failure_operation_close"}, {"worker_ipc_unconfirmed"},
                {"worker_response_missing"}, {"worker_status_missing"}, {"worker_ack_write_unconfirmed"},
                {"close_receipt_missing"}},
                "journal retains worker 1's unanswered close as the failure block");
            CheckEventsExactly(WorkerBlock(events, 0), ClosedWorkerBlock(0), "journal retains worker 0's clean exit code");
            CheckEventsExactly(WorkerBlock(events, 1),
                {{"worker_exit_observation_index", 1}, {"worker_exit_check_waited_for_worker_cleanup"},
                 {"worker_exit_code", 3}, {"worker_close_response_missing"}, {"worker_close_receipt_missing"}},
                "case 4b: journal retains worker 1's exit code 3 and its unanswered close");
            const auto topology = Concat(Concat(TopoUnavailable("topo_block_failure", 1, 2),
                                                TopoBlock("topo_block_close", 0, DiagValues(kCloseDiagBase))),
                                         TopoUnavailable("topo_block_close", 1, 2));
            const auto topo = FindEvent(events, "topo_block_failure");
            CheckEventsAt(events, topo, topology, "case 4b: worker 0's close diag, worker 1's close sent but unanswered");
            Check(topo + topology.size() == events.size(), "case 4b: the topology blocks end the journal");
            Check(!ContainsSuspiciousValue(content), "journal contains no nonce/epoch/serial-shaped value");
        }
        if (!DeleteFileW(journal_path.c_str())) return 4;
        if (!DeleteFileW(LateClaim(base).c_str())) return 4;
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // Case 5: a normal close journals both worker exit codes as 0, after (never
    // before) the existing both_workers_close_verified marker.
    {
        const auto root = base / "normal";
        const auto name = "A0.Poc.TestLease.Journal." + std::to_string(GetCurrentProcessId()) + ".normal";
        bool closed{};
        std::optional<PreviewWorkerFailureObservation> failure;
        std::array<PreviewWorkerExitObservation, 2> exits{};
        {
            PreviewWorkerOwner owner(name, root, worker, std::chrono::seconds(10));
            closed = owner.Close();
            failure = owner.FirstFailure();
            exits = owner.ExitObservations();
        }
        Check(closed && !failure, "normal close succeeds without a failure observation");
        const auto journal_path = base / "normal.jsonl";
        {
            PreviewRunJournal journal(journal_path);
            journal.Record("run_started");
            PreviewCloseJournalState state;
            Check(RecordCloseOutcomeToJournal(journal, closed, failure, exits, state), "close outcome fully journaled");
            const auto content = ReadWholeFile(journal_path);
            const auto events = ParseJournal(content);
            // The real stub worker never enumerated: its frozen counters are all zero.
            const auto expected = Concat(Concat(Concat(Concat({{"run_started"}, {"both_workers_close_verified"}},
                                                              ClosedWorkerBlock(0)), ClosedWorkerBlock(1)),
                                                TopoBlock("topo_block_close", 0, PreviewTopologyDiag{})),
                                         TopoBlock("topo_block_close", 1, PreviewTopologyDiag{}));
            CheckEventsExactly(events, expected,
                               "close verification precedes both workers' exit code 0, close reply summaries and close diag, with no failure block");
            Check(!ContainsSuspiciousValue(content), "journal contains no nonce/epoch/serial-shaped value");
        }
        if (!DeleteFileW(journal_path.c_str())) return 4;
        Check(!fs::exists(Marker(root)), "normal close already removed the marker via DisarmDualDelegation");
        Check(RemoveDirectoryW(root.c_str()), "normal journal fixture directory removed");
    }
    Check(RemoveDirectoryW(base.c_str()), "journal contract fixture root removed");
    std::cout << "{\"mode\":\"stub-worker-failure-journal\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

// ---- T2 contracts driven through scripted fake children ----
std::string ScriptedName(std::string_view scenario) {
    return "A0.Poc.TestLease.Scripted." + std::to_string(GetCurrentProcessId()) + "." + std::string(scenario);
}
bool NextOwnerBlocked(const fs::path& root, const std::string& name) {
    if (!fs::exists(Marker(root))) return false;
    try { HardwareProcessLease denied(name, std::chrono::milliseconds(0), root); }
    catch (const TransportError& error) { return error.Category() == "camera_control_delegation_quarantined"; }
    return false;
}
struct ScriptedRun {
    bool command_rejected{}, candidates_received{};
    ULONGLONG construct_ms{}; // Owner construction: lease, two spawns and both bootstraps.
    ULONGLONG command_ms{};
    bool closed{}, repeated{};
    ULONGLONG close_ms{};
    std::optional<PreviewWorkerFailureObservation> failure;
    std::array<PreviewWorkerExitObservation, 2> exits{}, exits_after_repeat{};
    std::array<DWORD, 2> codes{};
    JournalEvents journal;
    bool journal_complete{};
};
enum class ScriptedCommands { none, enumerate, preview };
struct ScriptedOptions {
    // none: Close() only. enumerate: Enumerate(0), timed. preview: Enumerate(0),
    // then Preview(0) (select and start), timed.
    ScriptedCommands commands{ScriptedCommands::none};
    // ScaledTiming() when empty.
    std::optional<PreviewWorkerTestTiming> timing;
    // Runs after the commands and right before Close(), with the tick taken
    // right before the owner was constructed.
    std::function<void(ULONGLONG)> before_close;
};
// One scripted scenario through a real owner: the commands of `options`, then
// Close() twice, and the close outcome journaled after each Close() as
// CloseOnce would. Returns 0, or 3/4 when a fake worker's exit or a journal
// file could not be confirmed.
int RunScripted(const fs::path& executable, const fs::path& base, std::string_view scenario,
                const ScriptedOptions& options, ScriptedRun& run) {
    const auto root = base / std::string(scenario);
    std::array<OwnedHandle, 2> processes;
    {
        const auto constructed = GetTickCount64();
        PreviewWorkerOwner owner(ScriptedName(scenario), root, executable, options.timing.value_or(ScaledTiming()));
        run.construct_ms = GetTickCount64() - constructed;
        const auto ids = owner.ProcessIds();
        for (std::size_t index = 0; index < ids.size(); ++index)
            processes[index].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[index]);
        if (options.commands == ScriptedCommands::enumerate) {
            const auto started = GetTickCount64();
            try {
                const auto candidates = owner.Enumerate(0);
                run.candidates_received = candidates[0] == "scripted-cand-0" && candidates[1] == "scripted-cand-1";
            } catch (const TransportError&) { run.command_rejected = true; }
            run.command_ms = GetTickCount64() - started;
        } else if (options.commands == ScriptedCommands::preview) {
            const auto candidates = owner.Enumerate(0);
            const auto started = GetTickCount64();
            try { (void)owner.Preview(0, candidates[0]); } catch (const TransportError&) { run.command_rejected = true; }
            run.command_ms = GetTickCount64() - started;
        }
        if (options.before_close) options.before_close(constructed);
        const auto started = GetTickCount64();
        run.closed = owner.Close();
        run.close_ms = GetTickCount64() - started;
        run.exits = owner.ExitObservations();
        run.repeated = owner.Close();
        run.failure = owner.FirstFailure();
        run.exits_after_repeat = owner.ExitObservations();
        const auto journal_path = base / (std::string(scenario) + ".jsonl");
        {
            PreviewRunJournal journal(journal_path);
            PreviewCloseJournalState state;
            run.journal_complete = RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, state);
            run.journal_complete = RecordCloseOutcomeToJournal(journal, run.repeated, run.failure,
                                                               run.exits_after_repeat, state) && run.journal_complete;
        }
        const auto content = ReadWholeFile(journal_path);
        run.journal = ParseJournal(content);
        Check(!ContainsSuspiciousValue(content), "journal contains no nonce/epoch/serial-shaped value");
        if (!DeleteFileW(journal_path.c_str())) return 4;
    }
    for (std::size_t index = 0; index < processes.size(); ++index) {
        DWORD code{};
        if (!processes[index].value || WaitForSingleObject(processes[index].value, 15000) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(processes[index].value, &code)) return 3; // Keep the marker if a fake exit is unknown.
        run.codes[index] = code;
    }
    return 0;
}
// Exact test-created claim, marker and directory; both fakes observed exited.
int RemoveScriptedFixture(const fs::path& base, std::string_view scenario, bool marker_retained) {
    DeleteFileW(ScenarioClaim(base, scenario).c_str()); // Absent when no fake received close.
    const auto root = base / std::string(scenario);
    if (marker_retained) return RemoveQuarantinedFixture(root);
    return RemoveDirectoryW(root.c_str()) ? 0 : 4;
}
bool ClosedReceiptReply(const PreviewWorkerExitObservation& observation) {
    return observation.close_reply && observation.close_reply->sent && observation.close_reply->response_validated &&
        observation.close_reply->status == PreviewWorkerReplyStatus::closed && observation.close_reply->ack_write_completed &&
        observation.close_reply->receipt && observation.close_reply->receipt->Complete();
}

// --exchange-deadline (T2/WU1): the parent waits for each command as long as
// that command's budget, not a flat 30 s. Time scale 1/5 (kTestScale).
int ExchangeDeadlineContract(const fs::path& executable, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerExchangeDeadline-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    const auto old_deadline = ScaledOldExchange();
    const auto start_deadline = ScaledExchange("start");
    ULONGLONG slow_ms{}, stuck_ms{};
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "slowstart", {ScriptedCommands::preview}, run)) return code;
        slow_ms = run.command_ms;
        Check(run.command_rejected, "slowstart: the failed start reply propagates out of Preview");
        Check(run.command_ms > old_deadline, "slowstart: the reply arrived later than the old flat 30 s deadline (scaled)");
        const auto& failure = run.failure;
        Check(failure && failure->worker_index == 0 && failure->operation == "start" &&
              failure->category == "live_view_start_failed" && failure->response_received && failure->response_validated &&
              failure->ack_write_completed && failure->reported_close && failure->reported_close->Complete(),
              "slowstart: a reply inside the start budget is validated, keeps its fixed category and receipt, and is ACKed");
        Check(!failure || failure->category != "worker_ipc_unconfirmed", "slowstart: never worker_ipc_unconfirmed");
        Check(!run.closed && !run.repeated, "slowstart: a failed command stays terminal");
        Check(run.codes[0] == 3 && run.codes[1] == 0, "slowstart: fake workers exit as scripted");
        Check(NextOwnerBlocked(base / "slowstart", ScriptedName("slowstart")), "slowstart: marker retained and next owner blocked");
        if (const auto code = RemoveScriptedFixture(base, "slowstart", true)) return code;
    }
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "stuckstart", {ScriptedCommands::preview}, run)) return code;
        stuck_ms = run.command_ms;
        Check(run.command_rejected, "stuckstart: the unanswered start propagates out of Preview");
        Check(run.command_ms + kTickGranularityMs >= start_deadline,
              "stuckstart: the parent waited the whole start budget before giving up");
        const auto& failure = run.failure;
        Check(failure && failure->worker_index == 0 && failure->operation == "start" &&
              failure->category == "worker_ipc_unconfirmed" && !failure->response_received && !failure->ack_write_completed,
              "stuckstart: a reply past the start budget is worker_ipc_unconfirmed");
        Check(!run.closed && !run.repeated, "stuckstart: Close() is false both times");
        Check(run.exits[0].close_reply && !run.exits[0].close_reply->sent, "stuckstart: close is not sent to the stuck worker");
        Check(run.codes[0] == 40, "stuckstart: the stuck worker received no further sequence");
        Check(run.codes[1] == 0, "stuckstart: the paired worker closes cleanly");
        Check(NextOwnerBlocked(base / "stuckstart", ScriptedName("stuckstart")), "stuckstart: marker retained and next owner blocked");
        if (const auto code = RemoveScriptedFixture(base, "stuckstart", true)) return code;
    }
    // C1: the first exchange with a worker waits D + S. Worker 0 answers its
    // first enumerate after D(enumerate) but inside D + S.
    ULONGLONG slow_enumerate_ms{};
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "slowenumerate", {ScriptedCommands::enumerate}, run)) return code;
        slow_enumerate_ms = run.command_ms;
        Check(!run.command_rejected && run.candidates_received,
              "slowenumerate: a first enumerate answered inside D + S is accepted");
        if (run.command_rejected) // Fixed vocabulary and numbers only.
            std::cerr << "  slowenumerate: constructMs=" << run.construct_ms << " category="
                      << (run.failure ? run.failure->category : std::string("none")) << '\n';
        Check(run.command_ms > ScaledExchange("enumerate"),
              "slowenumerate: the reply arrived after D(enumerate) alone would have ended (scaled)");
        Check(run.closed && run.repeated && !run.failure, "slowenumerate: close is clean afterwards");
        Check(CleanAfterClosedReceipt(run.exits[0]) && CleanAfterClosedReceipt(run.exits[1]),
              "slowenumerate: both exits are code 0 inside their E_ok windows");
        Check(run.codes[0] == 0 && run.codes[1] == 0, "slowenumerate: fake workers exit as scripted");
        if (const auto code = RemoveScriptedFixture(base, "slowenumerate", false)) return code;
    }
    Check(RemoveDirectoryW(base.c_str()), "exchange deadline fixture root removed");
    std::cout << "{\"mode\":\"stub-worker-exchange-deadline\",\"oldDeadlineMs\":" << old_deadline
              << ",\"startDeadlineMs\":" << start_deadline << ",\"slowStartReplyMs\":" << SlowStartDelay()
              << ",\"slowStartCommandMs\":" << slow_ms << ",\"stuckStartCommandMs\":" << stuck_ms
              << ",\"enumerateDeadlineMs\":" << ScaledExchange("enumerate")
              << ",\"firstEnumerateDeadlineMs\":" << ScaledFirstExchange("enumerate")
              << ",\"slowEnumerateReplyMs\":" << SlowEnumerateDelay()
              << ",\"slowEnumerateCommandMs\":" << slow_enumerate_ms << ",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

// --close-windows (T2/WU2): close is sent once per child first, then both
// exits are observed, each in the window its last validated reply allows.
// Time scale 1/5 (kTestScale).
int CloseWindowsContract(const fs::path& executable, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerCloseWindows-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    const auto safe_window = ScaledSafeReceiptWindow();
    const auto cleanup_window = ScaledCleanupWindow();
    const auto close_deadline = ScaledExchange("close");
    // Close is the first exchange with each worker in these scenarios: D + S.
    const auto first_close_deadline = ScaledFirstExchange("close");
    std::array<ULONGLONG, 6> close_ms{};
    // (1) and (6): alive after E_ok, exits 0 later; a repeated Close() sends nothing.
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "lingerclosed", {}, run)) return code;
        close_ms[0] = run.close_ms;
        Check(!run.closed && !run.repeated, "(1) a worker alive after E_ok is not a successful close; (6) same result again");
        const auto& failure = run.failure;
        Check(failure && failure->worker_index == 0 && failure->operation == "close" &&
              failure->category == "worker_stop_in_progress" && failure->response_validated &&
              failure->response_status == PreviewWorkerReplyStatus::closed && failure->ack_write_completed &&
              failure->reported_close && failure->reported_close->Complete(),
              "(1) worker_stop_in_progress keeps the validated closed reply it came after");
        if (failure) {
            const auto display = FormatPreviewWorkerFailure(*failure);
            // U+533A U+5225: "distinguish" in the fixed note appended for this category.
            const std::wstring distinguish{static_cast<wchar_t>(0x533a), static_cast<wchar_t>(0x5225)};
            Check(display.find(L"category=worker_stop_in_progress") != std::wstring::npos &&
                  display.find(distinguish) != std::wstring::npos,
                  "(1) the display names the category and says the parent cannot tell SDK stop from a recovery hold");
        }
        const auto& held = run.exits[0];
        Check(held.kind == PreviewWorkerExitCheckKind::waited_after_close_ack && !held.at_close.exited &&
              !held.at_close.wait_failed, "(1) worker 0 is still running when its E_ok window ends");
        Check(ClosedReceiptReply(held), "(1) worker 0's close reply summary is a validated closed receipt");
        Check(CleanAfterClosedReceipt(run.exits[1]), "(1) worker 1 closes cleanly inside E_ok");
        const auto& repeated = run.exits_after_repeat[0];
        Check(repeated.repeated_close_looks == 1 && repeated.at_repeated_close.has_value(),
              "(6) the repeated Close() takes one 0 ms look and sends nothing");
        Check(run.codes[0] == 0, "(1)(6) worker 0 later exits 0 on its own: one close received, never killed or resent");
        Check(run.codes[1] == 0, "(1) worker 1 exit code 0");
        Check(CountEvents(run.journal, "worker_stop_in_progress") == 1 &&
              CountEvents(run.journal, "worker_exit_recheck_at_repeated_close") == 1 && run.journal_complete,
              "(1)(6) journal: the category once, the repeated-close look once");
        Check(run.journal.size() >= 3 && run.journal[run.journal.size() - 3].name == "worker_exit_observation_index" &&
              run.journal[run.journal.size() - 3].value == 0 &&
              run.journal[run.journal.size() - 2].name == "worker_exit_recheck_at_repeated_close",
              "(6) the repeated-close look follows the second verdict");
        if (!run.journal.empty() && run.journal.back().name == "<unparsed>") DumpEvents(run.journal);
        Check(NextOwnerBlocked(base / "lingerclosed", ScriptedName("lingerclosed")),
              "(1) marker retained and next owner blocked");
        if (const auto code = RemoveScriptedFixture(base, "lingerclosed", true)) return code;
    }
    // (2) exit 0 inside E_ok.
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "quickclosed", {}, run)) return code;
        close_ms[1] = run.close_ms;
        Check(run.closed && run.repeated && !run.failure, "(2) exit 0 inside E_ok: Close() succeeds and stays cached");
        Check(CleanAfterClosedReceipt(run.exits[0]) && CleanAfterClosedReceipt(run.exits[1]),
              "(2) both exits are code 0 inside their E_ok windows");
        Check(!fs::exists(Marker(base / "quickclosed")), "(2) the marker is removed after the verified close");
        Check(run.codes[0] == 0 && run.codes[1] == 0, "(2) both fakes exit 0");
        Check(!run.journal.empty() && run.journal.front().name == "both_workers_close_verified" && run.journal_complete,
              "(2) journal verdict is both_workers_close_verified");
        if (const auto code = RemoveScriptedFixture(base, "quickclosed", false)) return code;
    }
    // (3) close unanswered, exit 3 inside the E_fail window.
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "silentclose", {}, run)) return code;
        close_ms[2] = run.close_ms;
        Check(!run.closed && !run.repeated, "(3) an unanswered close is not a successful close");
        Check(run.failure && run.failure->worker_index == 0 && run.failure->operation == "close" &&
              run.failure->category == "worker_ipc_unconfirmed" && !run.failure->response_received,
              "(3) first failure stays worker_ipc_unconfirmed");
        Check(CodeInCleanupWindow(run.exits[0], 3), "(3) worker 0's exit code 3 is observed inside its E_fail window");
        Check(run.exits[0].close_reply && run.exits[0].close_reply->sent && !run.exits[0].close_reply->response_received,
              "(3) close was sent once and never answered");
        Check(run.close_ms < cleanup_window, "(3) the exit ended the wait before the window did");
        Check(run.codes[0] == 3 && run.codes[1] == 0, "(3) fake workers exit as scripted");
        if (const auto code = RemoveScriptedFixture(base, "silentclose", true)) return code;
    }
    // (4) worker 0 unanswered and slow to exit, worker 1 slow to answer: the
    // windows overlap the other worker's close instead of adding up. The wall
    // clock thresholds assume this test runs alone (ctest -j 1); other load on
    // the machine can delay the fakes' replies and exits past them.
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "overlap", {}, run)) return code;
        close_ms[3] = run.close_ms;
        Check(!run.closed, "(4) worker 0's unanswered close keeps the verdict false");
        Check(CodeInCleanupWindow(run.exits[0], 3), "(4) worker 0's exit code 3 is observed inside its E_fail window");
        Check(CleanAfterClosedReceipt(run.exits[1]), "(4) worker 1 closes cleanly");
        const ULONGLONG bound = 2ULL * first_close_deadline + cleanup_window + 1000;
        Check(run.close_ms <= bound, "(4) close step wall clock <= (D(close) + S) x 2 + longest window + 1 s");
        // Overlapping: about max(exit, reply). Serial waiting: exit + reply.
        // The check sits halfway between the two.
        const auto overlapped = (std::max)(OverlapExitDelay(), OverlapReplyDelay());
        const auto shorter = (std::min)(OverlapExitDelay(), OverlapReplyDelay());
        Check(run.close_ms < overlapped + shorter / 2,
              "(4) worker 1's close ran inside worker 0's window: no serial waiting");
        Check(run.close_ms + 200 >= OverlapExitDelay(), "(4) the parent waited for worker 0's exit inside its window");
        Check(run.codes[0] == 3 && run.codes[1] == 0, "(4) fake workers exit as scripted");
        if (const auto code = RemoveScriptedFixture(base, "overlap", true)) return code;
    }
    // (5) quarantined reply: 0 ms window, the worker is observed alive.
    {
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "quarantined", {}, run)) return code;
        close_ms[4] = run.close_ms;
        Check(!run.closed && !run.repeated, "(5) a quarantined close is not a successful close");
        const auto& failure = run.failure;
        Check(failure && failure->worker_index == 0 && failure->operation == "close" &&
              failure->category == "worker_operation_failed" && failure->response_validated &&
              failure->response_status == PreviewWorkerReplyStatus::quarantined && failure->ack_write_completed &&
              failure->reported_close && !failure->reported_close->safe_to_exit,
              "(5) the quarantined reply stays worker_operation_failed, never worker_ack_unconfirmed");
        const auto& held = run.exits[0];
        Check(held.kind == PreviewWorkerExitCheckKind::instant_at_close_failure && !held.at_close.exited &&
              !held.at_close.wait_failed, "(5) one 0 ms look sees the held worker alive");
        Check(run.close_ms + 1000 < QuarantinedLinger(), "(5) the parent did not wait for the held worker");
        Check(run.codes[0] == 7, "(5)(6) the held worker received no further command, including from the repeated Close()");
        Check(run.codes[1] == 0, "(5) worker 1 exit code 0");
        if (const auto code = RemoveScriptedFixture(base, "quarantined", true)) return code;
    }
    // (7) R1: worker 1 answers close about E_ok / 2 before the session end, so
    // its E_ok window is capped by kPreviewSessionLimit. Worker 0 answered close
    // at once and stays alive: its look starts after its own window ended,
    // because worker 1's close exchange ran past it. The capped margin is
    // E_ok / 2 = 312 ms on each side and assumes this test runs alone.
    ULONGLONG capped_answer_offset{};
    {
        const auto capped_timing = CappedWindowTiming();
        const auto* raised_close = timing::FindWorkerOperationBudget(*capped_timing.budget_table, "close");
        const auto session_ms = static_cast<ULONGLONG>(kCappedScale.Apply(timing::kPreviewSessionLimit).count());
        const auto capped_safe_window =
            static_cast<ULONGLONG>(kCappedScale.Apply(timing::kExitWindowAfterSafeReceipt).count());
        const auto first_close_ms = static_cast<ULONGLONG>(
            kCappedScale.Apply(timing::ParentExchangeDeadline(*raised_close, true)).count());
        capped_answer_offset = session_ms - capped_safe_window / 2;
        ScriptedOptions options;
        options.timing = capped_timing;
        options.before_close = [&](ULONGLONG constructed) {
            const auto answer_at = constructed + capped_answer_offset;
            {
                std::ofstream tick(AnswerTickFile(base, "cappedwindow"), std::ios::trunc);
                tick << answer_at;
            }
            // Close late enough that worker 1's close exchange, D + S scaled
            // from here, would run past the session end: its deadline is the
            // session end, and the answer arrives inside it.
            SleepUntil(answer_at - first_close_ms * 3 / 4);
        };
        ScriptedRun run;
        if (const auto code = RunScripted(executable, base, "cappedwindow", options, run)) return code;
        close_ms[5] = run.close_ms;
        Check(!run.closed && !run.repeated, "(7) a worker still running after its window keeps the verdict false");
        const auto& failure = run.failure;
        Check(failure && failure->worker_index == 0 && failure->operation == "close" &&
              failure->category == "worker_stop_in_progress" && failure->response_validated &&
              failure->response_status == PreviewWorkerReplyStatus::closed && failure->ack_write_completed,
              "(7) first failure is worker 0, still running after its window, with its validated closed reply");
        const auto& early = run.exits[0];
        Check(early.kind == PreviewWorkerExitCheckKind::waited_after_close_ack && early.looked_after_window_end &&
              !early.window_capped_by_session_limit && !early.at_close.exited && !early.at_close.wait_failed &&
              ClosedReceiptReply(early),
              "(7) worker 0 is looked at after its own E_ok window ended and is still running");
        const auto& capped = run.exits[1];
        Check(capped.kind == PreviewWorkerExitCheckKind::waited_after_close_ack && capped.window_capped_by_session_limit &&
              !capped.looked_after_window_end && ClosedReceiptReply(capped),
              "(7) worker 1's window ends at the session end, not E_ok after its close reply");
        Check(CountEvents(run.journal, "worker_exit_window_capped_by_session_limit") == 1 &&
              CountEvents(run.journal, "worker_exit_looked_after_window_end") == 1 &&
              CountEvents(run.journal, "worker_stop_in_progress") == 1 && run.journal_complete,
              "(7) journal: the capped line once, the late-look line once, worker_stop_in_progress once");
        CheckEventsAt(WorkerBlock(run.journal, 0), 0,
                      {{"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_close_ack"},
                       {"worker_exit_wait_timed_out"}, {"worker_exit_looked_after_window_end"}},
                      "(7) worker 0's late-look line follows its window result");
        const auto block1 = WorkerBlock(run.journal, 1);
        Check(block1.size() >= 4 && block1[1].name == "worker_exit_check_waited_after_close_ack" &&
              block1[3].name == "worker_exit_window_capped_by_session_limit",
              "(7) worker 1's capped line follows its window result");
        Check(run.codes[0] == 0 && run.codes[1] == 0, "(7) both fakes exit 0 on their own: nothing resent or killed");
        if (!DeleteFileW(AnswerTickFile(base, "cappedwindow").c_str())) return 4;
        if (const auto code = RemoveScriptedFixture(base, "cappedwindow", true)) return code;
    }
    Check(RemoveDirectoryW(base.c_str()), "close window fixture root removed");
    std::cout << "{\"mode\":\"stub-worker-close-windows\",\"safeWindowMs\":" << safe_window
              << ",\"cleanupWindowMs\":" << cleanup_window << ",\"closeDeadlineMs\":" << close_deadline
              << ",\"firstCloseDeadlineMs\":" << first_close_deadline
              << ",\"lingerCloseMs\":" << close_ms[0] << ",\"quickCloseMs\":" << close_ms[1]
              << ",\"silentCloseMs\":" << close_ms[2] << ",\"overlapCloseMs\":" << close_ms[3]
              << ",\"quarantinedCloseMs\":" << close_ms[4] << ",\"cappedCloseMs\":" << close_ms[5]
              << ",\"cappedAnswerOffsetMs\":" << capped_answer_offset << ",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

// ---- Topology diagnostics: reply envelope v2 and the topology journal blocks ----
constexpr const char* kCompleteReceipt =
    R"({"liveViewOff":true,"sourceClosed":true,"moduleClosed":true,"processClaimReleased":true,"safeToExit":true})";
// A reply with schema/epoch/workerPid/sequence/status (workerPid 11, sequence 2),
// then `payload` when given, then `diag` when given, then `extra` verbatim.
std::string TableReply(std::string_view schema, std::string_view status, std::optional<std::string_view> payload,
                       std::optional<std::string> diag, std::string_view extra = "") {
    std::string reply = "{\"schema\":\"" + std::string(schema) +
        "\",\"epoch\":\"epoch\",\"workerPid\":11,\"sequence\":2,\"status\":\"" + std::string(status) + "\"";
    if (payload) reply += ",\"payload\":" + std::string(*payload);
    if (diag) reply += ",\"diag\":" + *diag;
    return reply + std::string(extra) + "}";
}
// A full diag whose last key (checkCurrentCount) carries `lexeme` verbatim.
std::string DiagWithLast(std::string_view lexeme) {
    auto diag = DiagJson(0, kPreviewTopologyDiagFieldCount - 1);
    diag.pop_back();
    return diag + ",\"checkCurrentCount\":" + std::string(lexeme) + "}";
}
bool ReplyRejected(const std::string& wire, std::string_view operation) {
    try {
        (void)ParsePreviewWorkerReply(wire, "epoch", 11, 2, operation);
    } catch (const TransportError& error) {
        return error.Category() == "worker_reply_invalid";
    } catch (...) {
        return false;
    }
    return false;
}
std::optional<PreviewWorkerReply> ReplyAccepted(const std::string& wire, std::string_view operation) {
    try {
        return ParsePreviewWorkerReply(wire, "epoch", 11, 2, operation);
    } catch (...) {
        return std::nullopt;
    }
}

// --reply-diag-table (T-f): the parent's v2 rejection rules, one wire at a time.
int ReplyDiagTable() {
    constexpr std::string_view v1 = "a0.preview-worker.v1";
    constexpr std::string_view v2 = "a0.preview-worker.v2";
    const auto failed_payload = FailedPayload("open_failed", kCompleteReceipt);
    struct Case { const char* name; std::string wire; std::string_view operation; };
    std::vector<Case> rejected{
        {"v1 envelope with 6 fields", TableReply(v1, "ok", "null", std::nullopt), "select"},
        {"v1 envelope with diag (7 fields)", TableReply(v1, "ok", "null", DiagJson()), "select"},
        {"v2 without diag", TableReply(v2, "ok", "null", std::nullopt), "select"},
        {"v2 with 8 fields", TableReply(v2, "ok", "null", DiagJson(), ",\"extra\":0"), "select"},
        {"v2 with 7 fields but no payload", TableReply(v2, "ok", std::nullopt, DiagJson(), ",\"extra\":null"), "select"},
        {"diag array", TableReply(v2, "ok", "null", std::string("[]")), "select"},
        {"diag string", TableReply(v2, "ok", "null", std::string("\"x\"")), "select"},
        {"diag null", TableReply(v2, "ok", "null", std::string("null")), "select"},
        {"diag number", TableReply(v2, "ok", "null", std::string("1")), "select"},
        {"diag boolean", TableReply(v2, "ok", "null", std::string("true")), "select"},
        {"diag 17 keys", TableReply(v2, "ok", "null", DiagJson(0, 5)), "select"},
        {"diag 19 keys", TableReply(v2, "ok", "null", DiagJson().insert(1, "\"extraKey\":0,")), "select"},
        {"diag key name differs", TableReply(v2, "ok", "null", "{\"openadd\":0," + DiagJson(0, 0).substr(1)), "select"},
        {"diag duplicate key", TableReply(v2, "ok", "null", DiagJson().insert(1, "\"openRemove\":0,")), "select"},  // all 18 keys present plus one duplicate: rejected for the duplicate, not for a missing key
    };
    for (const char* lexeme : {"-1", "1.0", "1e3", "1E3", "4294967296", "18446744073709551616", "true", "\"1\"", "null",
                               "{}", "01", "[]"}) {
        rejected.push_back({lexeme, TableReply(v2, "ok", "null", DiagWithLast(lexeme)), "select"});
    }
    const struct { const char* status; std::string payload; std::string_view operation; } statuses[] = {
        {"ok", "null", "select"},
        {"failed", failed_payload, "select"},
        {"closed", kCompleteReceipt, "close"},
        {"quarantined", kCompleteReceipt, "close"},
    };
    for (const auto& status : statuses) {
        rejected.push_back({status.status, TableReply(v2, status.status, status.payload, std::nullopt), status.operation});
        // The same reply with a diag is accepted: only the missing diag rejected it.
        const auto accepted = ReplyAccepted(TableReply(v2, status.status, status.payload, DiagJson(7)), status.operation);
        Check(accepted && accepted->topology == DiagValues(7), "each status is accepted with a full diag");
    }
    for (const auto& c : rejected) {
        const bool ok = ReplyRejected(c.wire, c.operation);
        Check(ok, "T-f: reply rejected as worker_reply_invalid");
        if (!ok) std::cerr << "  case: " << c.name << '\n';
    }
    // Accepted boundary values and key order.
    const auto zero = ReplyAccepted(TableReply(v2, "ok", "null", DiagWithLast("0")), "select");
    const auto maximum = ReplyAccepted(TableReply(v2, "ok", "null", DiagWithLast("4294967295")), "select");
    Check(zero && zero->topology.Get(PreviewTopologyField::check_current_count) == 0 &&
          maximum && maximum->topology.Get(PreviewTopologyField::check_current_count) == 4294967295U,
          "T-f: 0 and 4294967295 are accepted");
    std::string reversed = "{";
    for (std::size_t index = kPreviewTopologyDiagFieldCount; index-- > 0;) {
        if (reversed.size() > 1) reversed += ',';
        reversed += "\"" + std::string(kPreviewTopologyDiagFields[index].json_key) + "\":" + std::to_string(40 + index);
    }
    reversed += "}";
    const auto shuffled = ReplyAccepted(TableReply(v2, "ok", "null", reversed), "select");
    Check(shuffled && shuffled->topology == DiagValues(40), "T-f: diag key order carries no meaning");
    std::cout << "{\"mode\":\"stub-reply-diag-table\",\"rejectedCases\":" << rejected.size()
              << ",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

// --reply-diag-contract (T-f): a real owner and a fake child that answers select
// with a 17-key diag. The whole reply is rejected: no ACK, no close to that
// child, the marker stays, and the journal says the reply was rejected.
int ReplyDiagContract(const fs::path& executable, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerReplyDiag-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    // Nothing here waits for a scaled budget, so a wide operation deadline costs
    // no time; it keeps a slow spawn under load from turning select into
    // owner_operation_deadline_expired. 30 s + (47 s + 54 s) / 2 = 80.5 s <= 90 s <= 180 s / 2.
    ScriptedOptions options;
    options.commands = ScriptedCommands::preview;
    PreviewWorkerTestTiming timing;
    timing.operation_deadline = std::chrono::seconds(30);
    timing.serving_lifetime = std::chrono::seconds(90);
    timing.scale = {1, 2};
    options.timing = timing;
    ScriptedRun run;
    if (const auto code = RunScripted(executable, base, "diagselect", options, run)) return code;
    if (run.failure && run.failure->category != "worker_reply_invalid") // Fixed vocabulary and numbers only.
        std::cerr << "  diagselect: constructMs=" << run.construct_ms << " commandMs=" << run.command_ms
                  << " category=" << run.failure->category << " operation=" << run.failure->operation << "\n";
    Check(run.command_rejected, "diagselect: the rejected select reply propagates out of Preview");
    const auto& failure = run.failure;
    Check(failure && failure->worker_index == 0 && failure->operation == "select" &&
          failure->category == "worker_reply_invalid" && failure->request_written && failure->response_received &&
          !failure->response_validated && !failure->ack_write_completed && !failure->reported_close && !failure->topology,
          "diagselect: first failure is select / worker_reply_invalid, received but not validated, no ACK, no diag");
    Check(!run.closed && !run.repeated, "diagselect: Close() is false both times");
    Check(run.exits[0].close_reply && !run.exits[0].close_reply->attempted,
          "diagselect: close is never sent to the child whose reply was rejected");
    Check(run.exits[0].kind == PreviewWorkerExitCheckKind::waited_for_worker_cleanup,
          "diagselect: worker 0 gets the E_fail window (no validated receipt)");
    Check(run.codes[0] == 3, "diagselect: the child received no ACK byte and no further command");
    Check(run.codes[1] == 0, "diagselect: the paired worker closes cleanly");
    const auto topo = FindEvent(run.journal, "topo_block_failure");
    CheckEventsAt(run.journal, topo, TopoUnavailable("topo_block_failure", 0, 3),
                  "diagselect: journal has topo_block_failure(0) then topo_unavailable(3)");
    Check(run.journal_complete, "diagselect: the close outcome is fully journaled");
    Check(NextOwnerBlocked(base / "diagselect", ScriptedName("diagselect")), "diagselect: marker retained and next owner blocked");
    if (const auto code = RemoveScriptedFixture(base, "diagselect", true)) return code;
    Check(RemoveDirectoryW(base.c_str()), "reply diag fixture root removed");
    std::cout << "{\"mode\":\"stub-reply-diag-contract\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

fs::path TopologyJournalPath(const fs::path& temporary, std::string_view name) {
    return temporary / (L"A0WorkerTopologyJournal-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::wstring(name.begin(), name.end()) + L".jsonl");
}
// Lines written so far to an open topology journal, read through a separate handle.
std::size_t ReadJournalEventsCount(const fs::path& temporary, std::string_view name) {
    return ReadJournalEvents(TopologyJournalPath(temporary, name)).size();
}
// Writes through `write` into a fresh real PreviewRunJournal and reads it back.
template <class Write>
JournalEvents JournalOf(const fs::path& temporary, std::string_view name, Write&& write, std::string* text = nullptr) {
    const auto path = TopologyJournalPath(temporary, name);
    {
        PreviewRunJournal journal(path);
        write(journal);
    }
    const auto content = ReadWholeFile(path);
    Check(!ContainsSuspiciousValue(content), "topology journal contains no nonce/epoch/serial-shaped value");
    if (text) *text = content;
    if (!DeleteFileW(path.c_str())) throw std::runtime_error("topology journal cleanup");
    return ParseJournal(content);
}
PreviewWorkerProcessCheck Exited(std::uint32_t code) {
    PreviewWorkerProcessCheck check;
    check.exited = true;
    check.code_available = true;
    check.exit_code = code;
    return check;
}
PreviewWorkerExitObservation ClosedExit(std::optional<PreviewTopologyDiag> topology) {
    PreviewWorkerExitObservation exit;
    exit.kind = PreviewWorkerExitCheckKind::waited_after_close_ack;
    exit.at_close = Exited(0);
    PreviewWorkerCloseReply reply;
    reply.attempted = reply.sent = reply.response_received = reply.response_validated = reply.ack_write_completed = true;
    reply.status = PreviewWorkerReplyStatus::closed;
    reply.receipt = PreviewWorkerCloseReceipt{true, true, true, true, true};
    reply.topology = topology;
    exit.close_reply = reply;
    return exit;
}

// --topology-journal (T-g): order and vocabulary of the topology blocks.
int TopologyJournal(const fs::path& temporary) {
    const auto d0 = DiagValues(1000), d1 = DiagValues(2000), failed = DiagValues(3000);
    // g1: closed.
    {
        PreviewCloseJournalState state;
        bool complete{};
        const auto events = JournalOf(temporary, "g1", [&](PreviewRunJournal& journal) {
            complete = RecordCloseOutcomeToJournal(journal, true, std::nullopt, {ClosedExit(d0), ClosedExit(d1)}, state);
        });
        CheckEventsExactly(events, Concat(Concat(Concat(Concat({{"both_workers_close_verified"}}, ClosedWorkerBlock(0)),
                                                        ClosedWorkerBlock(1)), TopoBlock("topo_block_close", 0, d0)),
                                          TopoBlock("topo_block_close", 1, d1)),
                           "g1: closed verdict, both exit blocks, then each worker's close diag in table order");
        Check(complete, "g1: every line written");
    }
    // g2: the run-05 shape.
    PreviewWorkerFailureObservation select_failure;
    select_failure.worker_index = 0;
    select_failure.operation = "select";
    select_failure.category = "worker_selection_topology_event";
    select_failure.request_written = select_failure.response_received = select_failure.response_validated = true;
    select_failure.ack_write_completed = true;
    select_failure.response_status = PreviewWorkerReplyStatus::failed;
    select_failure.reported_close = PreviewWorkerCloseReceipt{true, true, true, true, true};
    select_failure.topology = failed;
    PreviewWorkerExitObservation not_sent;
    not_sent.kind = PreviewWorkerExitCheckKind::waited_after_failure_receipt;
    not_sent.at_close = Exited(3);
    not_sent.close_reply = PreviewWorkerCloseReply{};
    {
        PreviewCloseJournalState state;
        const std::array<PreviewWorkerExitObservation, 2> exits{not_sent, ClosedExit(d1)};
        std::size_t after_first{};
        bool first_complete{}, second_complete{};
        // One journal, two calls (as a retried CloseOnce would): g2 reads the
        // first call's lines, g4 the second call's.
        const auto repeated = JournalOf(temporary, "g2", [&](PreviewRunJournal& journal) {
            first_complete = RecordCloseOutcomeToJournal(journal, false, select_failure, exits, state);
            after_first = ReadJournalEventsCount(temporary, "g2");
            second_complete = RecordCloseOutcomeToJournal(journal, false, select_failure, exits, state);
        });
        const JournalEvents events(repeated.begin(), repeated.begin() + static_cast<std::ptrdiff_t>(
                                                         (std::min)(after_first, repeated.size())));
        Check(first_complete && second_complete, "g2/g4: every line written");
        const JournalEvents head{
            {"close_unconfirmed"}, {"failure_worker_index", 0}, {"failure_operation_select"},
            {"worker_selection_topology_event"}, {"worker_response_validated"}, {"worker_status_failed"},
            {"worker_ack_write_completed"}, {"close_receipt_live_view_off", 1}, {"close_receipt_source_closed", 1},
            {"close_receipt_module_closed", 1}, {"close_receipt_process_claim_released", 1}, {"close_receipt_safe_to_exit", 1},
            {"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_failure_receipt"}, {"worker_exit_code", 3},
            {"worker_close_not_sent"}};
        auto expected = Concat(head, ClosedWorkerBlock(1));
        expected = Concat(expected, TopoBlock("topo_block_failure", 0, failed));
        expected = Concat(expected, TopoUnavailable("topo_block_close", 0, 1));
        expected = Concat(expected, TopoBlock("topo_block_close", 1, d1));
        CheckEventsExactly(events, expected, "g2: the run-05 shape matches design section 6.3 from close_unconfirmed on");
        // g4: a repeated close outcome adds the verdict only, never a topo_ line.
        const auto topo_lines = [](const JournalEvents& list) {
            return std::count_if(list.begin(), list.end(),
                                 [](const JournalEvent& event) { return event.name.rfind("topo_", 0) == 0; });
        };
        Check(repeated.size() == events.size() + 1 && repeated.back().name == "close_unconfirmed" &&
              topo_lines(repeated) == topo_lines(events) && topo_lines(events) == 19 + 2 + 19,
              "g4: the second RecordCloseOutcomeToJournal writes the verdict and no topo_ line");
    }
    // g3: the unavailable reasons.
    const auto failure_reason = [&](const std::optional<PreviewWorkerFailureObservation>& failure, const char* name) {
        PreviewCloseJournalState state;
        // Exits without a close reply keep these journals short (each line is flushed).
        const auto events = JournalOf(temporary, name, [&](PreviewRunJournal& journal) {
            (void)RecordCloseOutcomeToJournal(journal, false, failure, {}, state);
        });
        const auto at = FindEvent(events, "topo_block_failure");
        return at + 1 < events.size() ? std::make_pair(events[at], events[at + 1]) : std::make_pair(JournalEvent{}, JournalEvent{});
    };
    const auto same = [](const std::pair<JournalEvent, JournalEvent>& actual, std::uint64_t index, std::uint64_t reason) {
        return actual.first.name == "topo_block_failure" && actual.first.value == index &&
            actual.second.name == "topo_unavailable" && actual.second.value == reason;
    };
    {
        PreviewWorkerFailureObservation written;
        written.worker_index = 0;
        written.operation = "select";
        written.category = "worker_ipc_unconfirmed";
        written.request_written = true;
        Check(same(failure_reason(written, "g3a"), 0, 2), "g3: request written, no response -> topo_unavailable(2)");
        auto rejected = written;
        rejected.response_received = true;
        rejected.category = "worker_reply_invalid";
        Check(same(failure_reason(rejected, "g3b"), 0, 3), "g3: response rejected -> topo_unavailable(3)");
        PreviewWorkerFailureObservation refused;
        refused.worker_index = 1;
        refused.operation = "enumerate";
        refused.category = "owner_operation_deadline_expired";
        Check(same(failure_reason(refused, "g3c"), 1, 1), "g3: request not written (owner side) -> topo_unavailable(1)");
        Check(same(failure_reason(std::nullopt, "g3d"), 2, 0), "g3: no failure observation -> topo_block_failure(2) + (0)");
        PreviewWorkerFailureObservation disarm;
        disarm.worker_index = 2;
        disarm.operation = "disarm";
        disarm.category = "delegation_disarm_unconfirmed";
        Check(same(failure_reason(disarm, "g3e"), 2, 1), "g3: the owner-wide stage keeps index 2 (nothing was sent)");
        auto validated_without = select_failure;
        validated_without.topology.reset();
        Check(same(failure_reason(validated_without, "g3f"), 0, 0), "g3: validated without counters -> defense (0)");
    }
    {
        // Close-side reasons: attempted but not written, written but unanswered,
        // answered but rejected.
        auto partial = ClosedExit(std::nullopt);
        partial.close_reply = PreviewWorkerCloseReply{};
        partial.close_reply->attempted = true;
        auto unanswered = ClosedExit(std::nullopt);
        unanswered.close_reply->response_received = unanswered.close_reply->response_validated = false;
        unanswered.close_reply->status.reset();
        auto invalid = unanswered;
        invalid.close_reply->response_received = true;
        PreviewWorkerExitObservation none;
        PreviewCloseJournalState state;
        const auto events = JournalOf(temporary, "g3close", [&](PreviewRunJournal& journal) {
            (void)RecordCloseOutcomeToJournal(journal, true, std::nullopt, {partial, unanswered}, state);
            PreviewCloseJournalState second;
            (void)RecordCloseOutcomeToJournal(journal, true, std::nullopt, {invalid, none}, second);
        });
        JournalEvents topo;
        for (const auto& event : events)
            if (event.name.rfind("topo_", 0) == 0) topo.push_back(event);
        CheckEventsExactly(topo, Concat(Concat(Concat(TopoUnavailable("topo_block_close", 0, 1),
                                                      TopoUnavailable("topo_block_close", 1, 2)),
                                               TopoUnavailable("topo_block_close", 0, 3)),
                                        TopoUnavailable("topo_block_close", 1, 1)),
                           "g3: close not written (1), unanswered (2), rejected (3), no close reply (1)");
    }
    // g5: RecordEnumerateOutcome.
    {
        bool rejected_worker{};
        const auto events = JournalOf(temporary, "g5", [&](PreviewRunJournal& journal) {
            RecordEnumerateOutcome(journal, 0, 2, DiagValues(300));
            RecordEnumerateOutcome(journal, 1, 2, DiagValues(400));
            RecordEnumerateOutcome(journal, 0, 2, std::nullopt);
            try { RecordEnumerateOutcome(journal, 2, 2, DiagValues(300)); }
            catch (const std::invalid_argument&) { rejected_worker = true; }
        });
        CheckEventsExactly(events,
            Concat(Concat(Concat(Concat(Concat(JournalEvents{{"worker_a_enumerated", 2}},
                                               TopoBlock("topo_block_enumerate", 0, DiagValues(300))),
                                        JournalEvents{{"worker_b_enumerated", 2}}),
                                 TopoBlock("topo_block_enumerate", 1, DiagValues(400))),
                          JournalEvents{{"worker_a_enumerated", 2}}),
                   TopoUnavailable("topo_block_enumerate", 0, 0)),
            "g5: worker_a/b_enumerated then that worker's enumerate block; a missing diag is topo_unavailable(0)");
        Check(rejected_worker, "g5: a worker index other than 0 or 1 is rejected before any line");
    }
    // g6: vocabulary.
    {
        std::vector<std::string> names;
        for (const auto& field : kPreviewTopologyDiagFields) names.emplace_back(field.journal_event);
        for (const char* name : {"topo_block_enumerate", "topo_block_failure", "topo_block_close", "topo_unavailable",
                                 "preview_requested"})
            names.emplace_back(name);
        Check(names.size() == 23, "g6: 18 counters, 3 block headers, topo_unavailable and preview_requested");
        static constexpr std::string_view existing[] = {
            "run_started", "worker_a_enumerated", "worker_b_enumerated", "worker_a_observed_bytes",
            "worker_b_observed_bytes", "operator_confirmed_cam_a", "operator_confirmed_cam_b", "both_live_views_started",
            "cam_a_frame_bytes", "cam_b_frame_bytes", "frame_pair_received", "display_failed", "no_active_owner",
            "operation_failed", "startup_no_workers", "startup_quarantined", "both_workers_close_verified",
            "close_unconfirmed", "failure_observation_missing", "failure_worker_index", "failure_operation_unknown",
            "failure_category_missing", "failure_category_unknown", "worker_response_validated", "worker_response_invalid",
            "worker_response_missing", "worker_status_ok", "worker_status_failed", "worker_status_closed",
            "worker_status_quarantined", "worker_status_missing", "worker_status_unknown", "worker_ack_write_completed",
            "worker_ack_write_unconfirmed", "close_receipt_live_view_off", "close_receipt_source_closed",
            "close_receipt_module_closed", "close_receipt_process_claim_released", "close_receipt_safe_to_exit",
            "close_receipt_missing", "worker_exit_observation_index", "worker_exit_check_not_run",
            "worker_exit_check_waited_after_close_ack", "worker_exit_check_instant_at_close_failure",
            "worker_exit_check_waited_after_failure_receipt", "worker_exit_check_waited_for_worker_cleanup",
            "worker_exit_code", "worker_exit_code_unavailable", "worker_exit_wait_failed", "worker_exit_wait_timed_out",
            "worker_exit_not_observed_at_close", "worker_exit_not_observed_at_recheck",
            "worker_exit_not_observed_at_repeated_close", "worker_exit_recheck_after_both_closes",
            "worker_exit_recheck_at_repeated_close", "worker_exit_window_capped_by_session_limit",
            "worker_exit_looked_after_window_end", "worker_close_not_sent", "worker_close_not_delivered",
            "worker_close_status_closed", "worker_close_status_quarantined", "worker_close_status_failed",
            "worker_close_status_unknown", "worker_close_ack_write_completed", "worker_close_ack_write_unconfirmed",
            "worker_close_response_invalid", "worker_close_response_missing", "worker_close_receipt_live_view_off",
            "worker_close_receipt_source_closed", "worker_close_receipt_module_closed",
            "worker_close_receipt_process_claim_released", "worker_close_receipt_safe_to_exit",
            "worker_close_receipt_missing",
        };
        for (const auto& name : names) {
            Check(IsJournalEventName(name), "g6: every new event is a 1..48 char [a-z_] name");
            Check(std::count(names.begin(), names.end(), name) == 1, "g6: new events are distinct");
            Check(!IsKnownFailureCategory(name), "g6: no new event collides with the 62 failure categories");
            Check(name.rfind("failure_operation_", 0) != 0, "g6: no new event looks like failure_operation_*");
            Check(std::find(std::begin(existing), std::end(existing), name) == std::end(existing),
                  "g6: no new event collides with an existing fixed event");
            if (!IsJournalEventName(name)) std::cerr << "  name: " << name << '\n';
        }
        for (const auto operation : kPreviewWorkerJournalOperations)
            Check(std::find(names.begin(), names.end(), "failure_operation_" + std::string(operation)) == names.end(),
                  "g6: failure_operation_<op> stays distinct");
        // g1-g5 above already wrote every topo_ event through the real
        // PreviewRunJournal (Record throws on an invalid name); preview_requested
        // is written by the display only, so it is written here once.
        bool accepted = true;
        (void)JournalOf(temporary, "g6", [&](PreviewRunJournal& journal) {
            try { journal.Record("preview_requested", 1); } catch (const std::exception&) { accepted = false; }
        });
        Check(accepted, "g6: the real PreviewRunJournal accepts preview_requested");
    }
    // g7: no source ID reaches the journal.
    {
        constexpr std::uint32_t id = 3735928559U;
        WorkerTopologyCounters counters;
        const auto start = WorkerTopologyCounters::Clock::now();
        counters.Reset(start);
        counters.Observe(true, id, start, true);
        counters.BeginInventoryWait();
        const std::array<std::uint32_t, 2> ids{id, 12};
        counters.Snapshot(2, 2, ids, start);
        counters.Mark(PreviewTopologyOperation::select);
        counters.Observe(false, id, start, true);
        counters.Observe(true, id - 1, start, true);
        std::string content;
        (void)JournalOf(temporary, "g7", [&](PreviewRunJournal& journal) {
            RecordTopologyBlock(journal, PreviewTopologyBlock::failure, 0, counters.Values(),
                                PreviewTopologyUnavailable::internal);
        }, &content);
        Check(content.find(std::to_string(id)) == std::string::npos && content.find(std::to_string(id - 1)) == std::string::npos,
              "g7: the journal never contains a source ID");
        Check(content.find("topo_post_remove_known\",\"value\":1") != std::string::npos,
              "g7: the event is counted, not recorded");
    }
    std::cout << "{\"mode\":\"stub-topology-journal\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

bool BothAlive(const std::array<OwnedHandle, 2>& processes) {
    return processes[0].value && processes[1].value && WaitForSingleObject(processes[0].value, 0) == WAIT_TIMEOUT &&
        WaitForSingleObject(processes[1].value, 0) == WAIT_TIMEOUT;
}
// --serving-lifetime (T2/WU3): the real SDK-stub PreviewWorker keeps serving
// close after its operation deadline, closes itself when its serving lifetime
// ends, and the owner refuses to start when the lifetimes cannot cover the
// close step.
int ServingLifetimeContract(const fs::path& worker, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerServingLifetime-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    const auto name_for = [](const char* scenario) {
        return "A0.Poc.TestLease.Serving." + std::to_string(GetCurrentProcessId()) + "." + scenario;
    };
    ULONGLONG close_at{}, self_close_at{}, refused_at{};
    // (1) Operation deadline 2 s, serving lifetime 25 s, close sent at 2.5 s.
    // Scale 1/5 leaves the real worker an E_ok of 1 s for its process teardown.
    {
        PreviewWorkerTestTiming lifetimes;
        lifetimes.operation_deadline = std::chrono::seconds(2);
        lifetimes.serving_lifetime = std::chrono::seconds(25);
        lifetimes.scale = {1, 5}; // 2 s + (47 s + 54 s) / 5 = 22.2 s <= 25 s <= 180 s / 5.
        const auto root = base / "close-after-deadline";
        std::array<OwnedHandle, 2> processes;
        bool closed{}, alive_after_deadline{};
        std::optional<PreviewWorkerFailureObservation> failure;
        std::array<PreviewWorkerExitObservation, 2> exits{};
        const auto constructed = GetTickCount64();
        {
            PreviewWorkerOwner owner(name_for("close-after-deadline"), root, worker, lifetimes);
            const auto ids = owner.ProcessIds();
            for (std::size_t index = 0; index < ids.size(); ++index)
                processes[index].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[index]);
            SleepUntil(constructed + 2200);
            alive_after_deadline = BothAlive(processes);
            SleepUntil(constructed + 2500);
            close_at = GetTickCount64() - constructed;
            closed = owner.Close();
            failure = owner.FirstFailure();
            exits = owner.ExitObservations();
        }
        Check(alive_after_deadline, "(1) both workers still serve after the 2 s operation deadline");
        Check(closed && !failure, "(1) close after the operation deadline gets closed receipts and clean exits");
        Check(CleanAfterClosedReceipt(exits[0]) && CleanAfterClosedReceipt(exits[1]),
              "(1) both closed receipts are complete and both exits are code 0");
        Check(!fs::exists(Marker(root)), "(1) the marker is removed");
        for (auto& process : processes) {
            DWORD code{};
            if (!process.value || WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0 ||
                !GetExitCodeProcess(process.value, &code)) return 3;
            Check(code == 0, "(1) worker exit code 0");
        }
        Check(RemoveDirectoryW(root.c_str()), "(1) fixture directory removed");
    }
    // (6) R3: past the owner's operation deadline a command is refused before
    // any IPC. The real worker accepts the following close as sequence 1 and
    // answers `closed`, which proves the refused command consumed no sequence.
    {
        PreviewWorkerTestTiming lifetimes;
        lifetimes.operation_deadline = std::chrono::seconds(2);
        lifetimes.serving_lifetime = std::chrono::seconds(25);
        lifetimes.scale = {1, 5}; // 2 s + (47 s + 54 s) / 5 = 22.2 s <= 25 s <= 180 s / 5.
        const auto root = base / "owner-deadline";
        std::array<OwnedHandle, 2> processes;
        bool refused{}, closed{};
        std::optional<PreviewWorkerFailureObservation> failure;
        std::array<PreviewWorkerExitObservation, 2> exits{};
        const auto constructed = GetTickCount64();
        {
            PreviewWorkerOwner owner(name_for("owner-deadline"), root, worker, lifetimes);
            const auto ids = owner.ProcessIds();
            for (std::size_t index = 0; index < ids.size(); ++index)
                processes[index].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[index]);
            SleepUntil(constructed + 2200);
            refused_at = GetTickCount64() - constructed;
            // A refused command closes the owner, as every failed command does.
            try { (void)owner.Enumerate(0); }
            catch (const TransportError& error) { refused = error.Category() == "owner_operation_deadline_expired"; }
            closed = owner.Close(); // Cached result of the close the refusal ran.
            failure = owner.FirstFailure();
            exits = owner.ExitObservations();
        }
        Check(refused, "(6) a command past the operation deadline is refused by the owner");
        Check(failure && failure->worker_index == 0 && failure->operation == "enumerate" &&
              failure->category == "owner_operation_deadline_expired" && !failure->request_written &&
              !failure->response_received && !failure->response_validated && !failure->ack_write_completed,
              "(6) first failure names the refused command; nothing was written to the worker");
        Check(closed, "(6) close is still sent and verified: the worker's sequence did not advance");
        Check(CleanAfterClosedReceipt(exits[0]) && CleanAfterClosedReceipt(exits[1]),
              "(6) both closed receipts are complete and both exits are code 0");
        Check(!fs::exists(Marker(root)), "(6) the marker is removed");
        for (auto& process : processes) {
            DWORD code{};
            if (!process.value || WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0 ||
                !GetExitCodeProcess(process.value, &code)) return 3;
            Check(code == 0, "(6) worker exit code 0");
        }
        Check(RemoveDirectoryW(root.c_str()), "(6) fixture directory removed");
    }
    // (3) No close before the serving lifetime ends: the workers close
    // themselves and exit 3; the parent cannot confirm a close. Scale 1/10
    // keeps the E_fail window (3 s) inside the session end (18 s).
    {
        PreviewWorkerTestTiming lifetimes;
        lifetimes.operation_deadline = std::chrono::seconds(1);
        lifetimes.serving_lifetime = std::chrono::seconds(12);
        lifetimes.scale = {1, 10}; // 1 s + (47 s + 54 s) / 10 = 11.1 s <= 12 s <= 180 s / 10.
        const auto root = base / "serving-expired";
        const auto name = name_for("serving-expired");
        std::array<OwnedHandle, 2> processes;
        bool closed{}, repeated{}, alive_after_deadline{};
        std::optional<PreviewWorkerFailureObservation> failure;
        std::array<PreviewWorkerExitObservation, 2> exits{};
        std::array<DWORD, 2> codes{};
        const auto constructed = GetTickCount64();
        {
            PreviewWorkerOwner owner(name, root, worker, lifetimes);
            const auto ids = owner.ProcessIds();
            for (std::size_t index = 0; index < ids.size(); ++index)
                processes[index].value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[index]);
            SleepUntil(constructed + 1500);
            alive_after_deadline = BothAlive(processes);
            for (std::size_t index = 0; index < processes.size(); ++index) {
                if (!processes[index].value || WaitForSingleObject(processes[index].value, 15000) != WAIT_OBJECT_0 ||
                    !GetExitCodeProcess(processes[index].value, &codes[index])) return 3;
            }
            self_close_at = GetTickCount64() - constructed;
            closed = owner.Close();
            repeated = owner.Close();
            failure = owner.FirstFailure();
            exits = owner.ExitObservations();
        }
        Check(alive_after_deadline, "(3) the operation deadline alone does not end the workers");
        Check(codes[0] == 3 && codes[1] == 3, "(3) each worker closes itself at its serving lifetime and exits 3");
        Check(self_close_at >= 12000, "(3) the workers ended no earlier than the serving lifetime");
        Check(!closed && !repeated, "(3) the parent cannot confirm a close");
        Check(failure && failure->worker_index == 0 && failure->operation == "close" &&
              failure->category == "worker_ipc_unconfirmed" && !failure->response_received,
              "(3) first failure is worker 0's unconfirmed close");
        Check(CodeInCleanupWindow(exits[0], 3) && CodeInCleanupWindow(exits[1], 3),
              "(3) both exit codes 3 are observed in the E_fail windows");
        for (const auto& exit : exits) {
            Check(exit.close_reply && exit.close_reply->attempted && !exit.close_reply->sent &&
                  !exit.close_reply->response_received,
                  "(3) close was attempted but never written: the worker had already ended");
        }
        Check(NextOwnerBlocked(root, name), "(3) marker retained and next owner blocked");
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // (5) A budget or lifetime that cannot cover the close step stops owner
    // construction before the marker is armed and before any worker exists.
    {
        auto raised = timing::kWorkerOperationBudgets;
        for (auto& entry : raised)
            if (entry.operation == "start") entry.sdk += std::chrono::seconds(20);
        Check(timing::ServingLifetimeFits(timing::kDefaultOperationDeadline, timing::kDefaultServingLifetime,
                                          timing::kWorkerOperationBudgets) &&
              !timing::ServingLifetimeFits(timing::kDefaultOperationDeadline, timing::kDefaultServingLifetime, raised),
              "(5) start + 20 s: 60 s + 67 s + 54 s = 181 s no longer fits in 170 s");
        struct Refused { const char* name; std::chrono::milliseconds operation, serving; bool raised_table; };
        const Refused refused_cases[] = {
            {"raised-budget", timing::kDefaultOperationDeadline, timing::kDefaultServingLifetime, true},
            {"serving-over-limit", timing::kDefaultOperationDeadline, std::chrono::milliseconds(180001), false},
            {"serving-not-above-deadline", std::chrono::seconds(60), std::chrono::seconds(60), false},
        };
        for (const auto& refused : refused_cases) {
            const auto root = base / refused.name;
            PreviewWorkerTestTiming lifetimes;
            lifetimes.operation_deadline = refused.operation;
            lifetimes.serving_lifetime = refused.serving;
            if (refused.raised_table) lifetimes.budget_table = raised;
            bool spawned{}, startup_refused{};
            try {
                PreviewWorkerOwner owner(name_for(refused.name), root, worker, lifetimes,
                                         [&](std::array<std::uint32_t, 2>) { spawned = true; });
            } catch (const PreviewWorkerStartupError& error) {
                startup_refused = !error.WorkersMayExist();
            }
            Check(startup_refused && !spawned && !fs::exists(Marker(root)),
                  "(5) PreviewWorkerStartupError(false): no marker, no worker");
            std::error_code ignored;
            fs::remove(root, ignored); // Created only if the lease had run; it must not have.
        }
    }
    Check(RemoveDirectoryW(base.c_str()), "serving lifetime fixture root removed");
    std::cout << "{\"mode\":\"stub-worker-serving-lifetime\",\"closeSentAtMs\":" << close_at
              << ",\"refusedCommandAtMs\":" << refused_at
              << ",\"selfCloseObservedAtMs\":" << self_close_at << ",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}

// Starts the real PreviewWorker with only its parent handle and bootstrap
// reader inherited, as PreviewWorkerOwner does.
bool SpawnRealWorker(const fs::path& worker, OwnedHandle& process, OwnedHandle& bootstrap_writer) {
    OwnedHandle parent, reader;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent.value,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) return false;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&reader.value, &bootstrap_writer.value, &security, 2048) ||
        !SetHandleInformation(bootstrap_writer.value, HANDLE_FLAG_INHERIT, 0)) return false;
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> memory(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    if (!size || !InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return false;
    HANDLE inherited[]{parent.value, reader.value};
    const bool restricted = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION created{};
    auto command = L"\"" + worker.wstring() + L"\" --delegated-worker " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(reader.value));
    const bool success = restricted && CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &created);
    DeleteProcThreadAttributeList(attributes);
    if (!success) return false;
    CloseHandle(created.hThread);
    process.value = created.hProcess;
    return true;
}
bool WriteBootstrap(HANDLE writer, const std::string& body) {
    const auto size = static_cast<std::uint32_t>(body.size());
    return WriteExact(writer, &size, sizeof(size)) && WriteExact(writer, body.data(), size);
}
// --bootstrap-limits (T2/WU3): the real PreviewWorker exits 2 for a bootstrap
// with the wrong item count or lifetimes outside 0 < operation < serving <=
// 180 s, before it constructs its SDK transport. The delegation is real (armed
// marker, both workers registered, current epoch), so exit 2 can come only
// from those checks; the accepted bootstrap serves until its serving lifetime
// ends and exits 3.
int BootstrapLimitsContract(const fs::path& worker, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerBootstrapLimits-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    struct BootstrapCase { const char* name; std::uint32_t operation_ms, serving_ms; bool omit_serving; DWORD expected; };
    const BootstrapCase cases[] = {
        {"item-count", 1000, 2000, true, 2},
        {"serving-below-deadline", 2000, 1000, false, 2},
        {"serving-equal-deadline", 1500, 1500, false, 2},
        {"serving-over-limit", 1000, 180001, false, 2},
        {"accepted", 500, 1500, false, 3},
    };
    for (const auto& c : cases) {
        const auto root = base / c.name;
        const auto name = "A0.Poc.TestLease.Bootstrap." + std::to_string(GetCurrentProcessId()) + "." + c.name;
        std::array<OwnedHandle, 2> processes, writers;
        std::array<DWORD, 2> codes{};
        {
            HardwareProcessLease lease(name, std::chrono::milliseconds(0), root);
            lease.ArmDualDelegation();
            for (std::size_t index = 0; index < processes.size(); ++index)
                if (!SpawnRealWorker(worker, processes[index], writers[index])) return 2;
            lease.RegisterDualWorkers(processes[0].value, processes[1].value);
            const auto& epoch = lease.DelegationEpoch();
            const auto encoded = root.u8string();
            const std::string root_utf8(encoded.begin(), encoded.end());
            for (std::size_t index = 0; index < processes.size(); ++index) {
                std::string body = "{\"pipe\":\"A0.Preview." + std::to_string(GetCurrentProcessId()) + ".boot." +
                    std::to_string(index) + "\",\"epoch\":\"" + epoch + "\",\"capability\":\"bootstrap-limit\"," +
                    "\"lifetimeMs\":" + std::to_string(c.operation_ms);
                if (!c.omit_serving) body += ",\"servingMs\":" + std::to_string(c.serving_ms);
                body += ",\"leaseName\":\"" + json::JsonEscape(name) + "\",\"testMarkerRoot\":\"" +
                    json::JsonEscape(root_utf8) + "\"}";
                if (!WriteBootstrap(writers[index].value, body)) return 2;
                CloseHandle(writers[index].value);
                writers[index].value = nullptr;
            }
            for (std::size_t index = 0; index < processes.size(); ++index) {
                if (WaitForSingleObject(processes[index].value, 8000) != WAIT_OBJECT_0 ||
                    !GetExitCodeProcess(processes[index].value, &codes[index])) return 3; // Keep the marker.
            }
        }
        Check(codes[0] == c.expected && codes[1] == c.expected, c.name);
        if (codes[0] != c.expected || codes[1] != c.expected)
            std::cerr << "  " << c.name << " exit codes " << codes[0] << ',' << codes[1] << '\n';
        // Exact test-created marker; both workers were observed exited above.
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    Check(RemoveDirectoryW(base.c_str()), "bootstrap limit fixture root removed");
    std::cout << "{\"mode\":\"stub-worker-bootstrap-limits\",\"cases\":" << std::size(cases)
              << ",\"failures\":" << failures << "}\n";
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
        // Parent may disappear before bootstrap validation (2), between the
        // delegation check and the host's own parent check (4, an exception in
        // worker main before any dispatcher), or after host start (3).
        Check(code == 2 || code == 3 || code == 4, "parent death is not normal authorized completion");
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
        if (argc == 2 && std::string_view(argv[1]) == "--ack-failure-contract")
            return ReplyContract(executable, temporary, true);
        if (argc == 2 && std::string_view(argv[1]) == "--journal-contract")
            return JournalContract(executable, worker, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--category-table") return CategoryTable(temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--journal-vocabulary") return JournalVocabulary(temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--exit-recheck") return ExitRecheck(executable, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--exchange-deadline")
            return ExchangeDeadlineContract(executable, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--close-windows") return CloseWindowsContract(executable, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--serving-lifetime") return ServingLifetimeContract(worker, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--bootstrap-limits") return BootstrapLimitsContract(worker, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--reply-diag-table") return ReplyDiagTable();
        if (argc == 2 && std::string_view(argv[1]) == "--reply-diag-contract") return ReplyDiagContract(executable, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--topology-journal") return TopologyJournal(temporary);
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
            // Operation deadline 300 ms, serving lifetime 1500 ms, budget scaled
            // 1/100: 300 ms + (47 s + 54 s) / 100 = 1310 ms <= 1500 ms. No
            // command is sent; the workers close themselves when serving ends.
            PreviewWorkerTestTiming expiring;
            expiring.operation_deadline = std::chrono::milliseconds(300);
            expiring.serving_lifetime = std::chrono::milliseconds(1500);
            expiring.scale = {1, 100};
            PreviewWorkerOwner owner(name, root / "expired", worker, expiring);
            const auto ids = owner.ProcessIds();
            for (auto id : ids) {
                HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
                if (!process) return 3;
                const auto wait = WaitForSingleObject(process, 5000);
                DWORD code{};
                const bool ended = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code);
                CloseHandle(process);
                if (!ended) return 3; // Never remove quarantine while a worker might live.
                Check(code == 3, "uncommanded serving-lifetime expiry is not a successful close receipt");
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
                // The bootstrap read throws inside worker main: exit 4, SDK untouched.
                Check(code == 4, "bootstrap EOF rejects partially started stub worker before SDK");
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
