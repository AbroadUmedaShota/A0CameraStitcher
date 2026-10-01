// Only configured with the SDK stub: no test process can open a physical camera.
#include "a0/phase0/preview_worker_owner.hpp"
#include "a0/phase0/preview_worker_failure_journal.hpp"
#include "a0/phase0/preview_run_journal.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/common/protocol_json.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <charconv>
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
        if (request_schema != "a0.preview-worker.v1" || request_epoch != epoch || request_capability != capability) return 4;
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
            const auto response = "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"" + epoch +
                "\",\"workerPid\":" + std::to_string(GetCurrentProcessId()) +
                ",\"sequence\":" + std::to_string(sequence) + ",\"status\":\"closed\",\"payload\":" + receipt + "}";
            return respond(response) ? 0 : 5;
        }
        if (!enumerated) {
            if (operation != "enumerate" || sequence != 1) return 4;
            enumerated = true;
            const auto response = "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"" + epoch +
                "\",\"workerPid\":" + std::to_string(GetCurrentProcessId()) +
                ",\"sequence\":" + std::to_string(sequence) +
                ",\"status\":\"ok\",\"payload\":[\"staged-cand-0\",\"staged-cand-1\"]}";
            if (!respond(response)) return 5;
            if (!ReconnectPipe(pipe)) return 2;
            continue;
        }
        if (operation != "select" && operation != "start" && operation != "frame") return 4;
        const bool should_fail = operation == fail_at;
        if (!should_fail) {
            const auto response = "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"" + epoch +
                "\",\"workerPid\":" + std::to_string(GetCurrentProcessId()) +
                ",\"sequence\":" + std::to_string(sequence) + ",\"status\":\"ok\",\"payload\":null}";
            if (!respond(response)) return 5;
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
        const auto response = "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"" + epoch +
            "\",\"workerPid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"sequence\":" + std::to_string(sequence) +
            ",\"status\":\"failed\",\"payload\":{\"error\":\"" + std::string(category) + "\",\"close\":" + receipt + "}}";
        return respond(response) ? 3 : 5;
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
        request_schema != "a0.preview-worker.v1" || request_epoch != epoch ||
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
            PreviewWorkerOwner owner(name, root, executable, std::chrono::seconds(10), {}, ack_only);
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
// The contiguous worker_exit_* block that starts at worker_exit_observation_index == worker.
JournalEvents ExitBlock(const JournalEvents& events, std::uint64_t worker) {
    JournalEvents block;
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (events[index].name != "worker_exit_observation_index" || events[index].value != worker) continue;
        block.push_back(events[index]);
        for (++index; index < events.size(); ++index) {
            const auto& event = events[index];
            if (event.name == "worker_exit_observation_index" || event.name.rfind("worker_exit_", 0) != 0) break;
            block.push_back(event);
        }
        break;
    }
    return block;
}
bool BlockIsOneOf(const JournalEvents& block, std::initializer_list<JournalEvents> forms) {
    for (const auto& form : forms)
        if (block.size() == form.size() && SameEvents(block, 0, form)) return true;
    return false;
}
// The failing worker's exit block when it carries a numeric exit code: either
// the 0 ms look at the close attempt already saw it, or the 0 ms recheck after
// both close steps did.
bool FailedWorkerExitCodeRecorded(const JournalEvents& block, std::uint64_t worker, std::uint64_t code) {
    return BlockIsOneOf(block, {
        {{"worker_exit_observation_index", worker}, {"worker_exit_check_instant_at_close_failure"},
         {"worker_exit_code", code}},
        {{"worker_exit_observation_index", worker}, {"worker_exit_check_instant_at_close_failure"},
         {"worker_exit_not_observed_at_close"}, {"worker_exit_recheck_after_both_closes"}, {"worker_exit_code", code}},
    });
}
// Accepted outcomes for a worker that exits with a known code before replying,
// whose exit is looked at with 0 ms at its failed close attempt and, if that
// look missed it, once more with 0 ms after both close steps. The exit code is
// not guaranteed by the end of Close(): under build load the recheck still saw
// a numeric code in only 17 of 20 runs (2026-10-01), and T1 adds no wait on the
// failure path. A deterministic code is left to T2/WU2's wait window.
enum class FailedExitForm { invalid, code_at_close, code_at_recheck, not_observed };
FailedExitForm ClassifyFailedWorkerExit(const PreviewWorkerExitObservation& observation, std::uint32_t code) {
    if (observation.kind != PreviewWorkerExitCheckKind::instant_at_close_failure) return FailedExitForm::invalid;
    const auto numeric = [code](const PreviewWorkerProcessCheck& check) {
        return !check.wait_failed && check.exited && check.code_available && check.exit_code == code;
    };
    const auto running = [](const PreviewWorkerProcessCheck& check) { return !check.wait_failed && !check.exited; };
    const auto& recheck = observation.after_both_closes;
    if (numeric(observation.at_close)) return recheck ? FailedExitForm::invalid : FailedExitForm::code_at_close;
    if (!running(observation.at_close) || !recheck) return FailedExitForm::invalid;
    if (numeric(*recheck)) return FailedExitForm::code_at_recheck;
    if (running(*recheck)) return FailedExitForm::not_observed;
    return FailedExitForm::invalid;
}
// The journal exit block an accepted form must produce, in exact order.
JournalEvents FailedWorkerExitBlock(FailedExitForm form, std::uint64_t worker, std::uint64_t code) {
    JournalEvents block{{"worker_exit_observation_index", worker}, {"worker_exit_check_instant_at_close_failure"}};
    if (form == FailedExitForm::code_at_close) {
        block.push_back({"worker_exit_code", code});
        return block;
    }
    block.push_back({"worker_exit_not_observed_at_close"});
    block.push_back({"worker_exit_recheck_after_both_closes"});
    if (form == FailedExitForm::code_at_recheck) block.push_back({"worker_exit_code", code});
    else block.push_back({"worker_exit_not_observed_at_recheck"});
    return block;
}
// Checks the failing worker's owner observation, then its journal block against
// the block built from that observation.
void CheckFailedWorkerExitJournaled(const JournalEvents& events, const PreviewWorkerExitObservation& observation,
                                    std::uint64_t worker, std::uint32_t code, const char* message) {
    const auto form = ClassifyFailedWorkerExit(observation, code);
    Check(form != FailedExitForm::invalid, message);
    if (form != FailedExitForm::invalid)
        CheckEventsExactly(ExitBlock(events, worker), FailedWorkerExitBlock(form, worker, code), message);
    else
        DumpEvents(ExitBlock(events, worker));
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
        PreviewWorkerOwner owner(name, root, executable, std::chrono::seconds(10));
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
    };
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
        // Production close outcome, not closed, no failure observation, called
        // twice as a retried CloseOnce would: verdict twice, details once.
        const std::array<PreviewWorkerExitObservation, 2> exits{
            exit_observation(Kind::waited_after_close_ack, check(true, true, 0)),
            exit_observation(Kind::instant_at_close_failure, check(true, true, 3))};
        bool details = false;
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, details) && all_returned_true;
        details_after_first = details;
        all_returned_true = RecordCloseOutcomeToJournal(journal, false, std::nullopt, exits, details) && all_returned_true;
        details_after_second = details;
        append({{"close_unconfirmed"}, {"failure_observation_missing"},
                {"worker_exit_observation_index", 0}, {"worker_exit_check_waited_after_close_ack"}, {"worker_exit_code", 0},
                {"worker_exit_observation_index", 1}, {"worker_exit_check_instant_at_close_failure"}, {"worker_exit_code", 3},
                {"close_unconfirmed"}});
        // Production close outcome, closed: verdict then exit blocks, no failure block.
        const std::array<PreviewWorkerExitObservation, 2> clean_exits{
            exit_observation(Kind::waited_after_close_ack, check(true, true, 0)),
            exit_observation(Kind::waited_after_close_ack, check(true, true, 0))};
        bool closed_details = false;
        all_returned_true = RecordCloseOutcomeToJournal(journal, true, std::nullopt, clean_exits, closed_details) &&
            all_returned_true;
        append({{"both_workers_close_verified"}});
        append(CleanExitBlock(0));
        append(CleanExitBlock(1));
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

// --exit-recheck: the owner-level exit evidence for a worker that exits before
// replying. No journal; see --journal-contract for the journal rendering.
int ExitRecheck(const fs::path& executable, const fs::path& temporary) {
    const auto base = temporary / (L"A0WorkerExitRecheck-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    int seen_at_close{}, seen_at_recheck{}, not_observed{};
    // Case 4, repeated: worker 0 exits with 3 before replying to enumerate.
    // Worker 1's close exchange and success-path wait run between the 0 ms look
    // at worker 0's close attempt and the 0 ms recheck, which usually suffices
    // but not always under load (see ClassifyFailedWorkerExit): either code 3 or
    // "not observed" at both looks is accepted.
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const auto root = base / "missing";
        const auto name = "A0.Poc.TestLease.Recheck." + std::to_string(GetCurrentProcessId()) + ".missing." +
            std::to_string(attempt);
        OwnerRun run;
        if (const auto code = RunOwnerScenario(executable, root, name, "missing", run)) return code;
        Check(!run.closed && !run.repeated_close, "worker exit before reply is terminal without a second close");
        Check(run.codes[0] == 3 && run.codes[1] == 0, "fake workers exit as the missing fixture expects");
        const auto& failing = run.exits[0];
        Check(failing.kind == PreviewWorkerExitCheckKind::instant_at_close_failure,
              "failing worker is looked at with 0 ms at its failed close attempt");
        Check(failing.at_close.exited != failing.after_both_closes.has_value(),
              "a recheck exists exactly when the at-close look did not see the exit");
        const auto form = ClassifyFailedWorkerExit(failing, 3);
        Check(form != FailedExitForm::invalid,
              "case 4: worker 0's exit code 3 is observed by the end of Close(), or it is not observed at close and recheck");
        if (form == FailedExitForm::code_at_close) ++seen_at_close;
        else if (form == FailedExitForm::code_at_recheck) ++seen_at_recheck;
        else if (form == FailedExitForm::not_observed) ++not_observed;
        Check(run.exits[1].kind == PreviewWorkerExitCheckKind::waited_after_close_ack &&
              run.exits[1].at_close.exited && run.exits[1].at_close.code_available &&
              run.exits[1].at_close.exit_code == 0 && !run.exits[1].after_both_closes,
              "paired worker exit code 0 comes from the unchanged success-path wait");
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // Case 4b: worker 1 exits with 3 after reading its own close request. No
    // other close step runs after it, so the recheck follows the at-close look
    // almost immediately and may still find it running. Both outcomes are valid
    // here; a deterministic exit code for this case needs a designed wait window
    // (T2/WU2), which this change deliberately does not add.
    std::string late_form = "unset";
    {
        const auto root = base / "late";
        const auto name = "A0.Poc.TestLease.Recheck." + std::to_string(GetCurrentProcessId()) + ".late";
        OwnerRun run;
        if (const auto code = RunOwnerScenario(executable, root, name, "late", run)) return code;
        Check(!run.closed && !run.repeated_close, "late worker exit is terminal without a second close");
        Check(run.codes[0] == 0 && run.codes[1] == 3, "worker 0 closes cleanly and worker 1 exits with 3");
        Check(run.failure.has_value() && run.failure->worker_index == 1 && run.failure->operation == "close" &&
              run.failure->category == "worker_ipc_unconfirmed" && !run.failure->response_received,
              "first failure names worker 1's unanswered close");
        Check(run.exits[0].kind == PreviewWorkerExitCheckKind::waited_after_close_ack &&
              run.exits[0].at_close.code_available && run.exits[0].at_close.exit_code == 0 &&
              !run.exits[0].after_both_closes, "worker 0 exit code 0 comes from the success-path wait");
        const auto& late = run.exits[1];
        Check(late.kind == PreviewWorkerExitCheckKind::instant_at_close_failure && !late.at_close.wait_failed,
              "worker 1 is looked at with 0 ms at its failed close attempt");
        if (late.at_close.exited) {
            late_form = "at_close";
            Check(late.at_close.code_available && late.at_close.exit_code == 3 && !late.after_both_closes,
                  "case 4b: an at-close exit carries code 3 and no recheck");
        } else {
            Check(late.after_both_closes.has_value() && !late.after_both_closes->wait_failed,
                  "case 4b: a missed exit is rechecked once");
            if (late.after_both_closes && late.after_both_closes->exited) {
                late_form = "recheck";
                Check(late.after_both_closes->code_available && late.after_both_closes->exit_code == 3,
                      "case 4b: a rechecked exit carries code 3");
            } else {
                late_form = "not_observed";
            }
        }
        if (!DeleteFileW(LateClaim(base).c_str())) return 4;
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    Check(seen_at_close + seen_at_recheck + not_observed == 3, "case 4: every attempt lands in exactly one accepted form");
    Check(RemoveDirectoryW(base.c_str()), "exit recheck fixture root removed");
    std::cout << "{\"mode\":\"stub-worker-exit-recheck\",\"case4AtClose\":" << seen_at_close
              << ",\"case4Recheck\":" << seen_at_recheck << ",\"case4NotObserved\":" << not_observed
              << ",\"case4bForm\":\"" << late_form
              << "\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
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
            bool details = false;
            Check(RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, details),
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
            CheckFailedWorkerExitJournaled(events, run.exits[0], 0, 3,
                "journal retains the failing worker's exit code 3, or not observed at close and recheck, in the observed form");
            const auto paired = ExitBlock(events, 1);
            CheckEventsExactly(paired, CleanExitBlock(1), "journal retains the paired worker's clean exit code from the success-path wait");
            Check(!events.empty() && events.back().name == "worker_exit_code", "exit block of worker 1 is the last journal block");
            Check(!ContainsSuspiciousValue(content), "journal contains no nonce/epoch/serial-shaped value");
        }
        if (!DeleteFileW(journal_path.c_str())) return 4;
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // Case 4: worker 0 exits with 3 before any reply reaches the parent. Its
    // exit block is code 3 or "not observed" at close and recheck (see
    // ClassifyFailedWorkerExit). The close outcome is journaled twice, as a
    // retried CloseOnce would: details only once.
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
            bool details = false;
            Check(RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, details) &&
                  RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, details),
                  "repeated close outcome fully journaled");
            const auto content = ReadWholeFile(journal_path);
            const auto events = ParseJournal(content);
            CheckEventsAt(events, 0, {{"run_started"}, {"close_unconfirmed"},
                {"failure_worker_index", 0}, {"failure_operation_enumerate"}, {"worker_ipc_unconfirmed"},
                {"worker_response_missing"}, {"worker_status_missing"}, {"worker_ack_write_unconfirmed"},
                {"close_receipt_missing"}},
                "journal retains the missing-response failure block in order");
            CheckFailedWorkerExitJournaled(events, run.exits[0], 0, 3,
                "case 4: journal retains worker 0's exit code 3, or not observed at close and recheck, in the observed form");
            CheckEventsExactly(ExitBlock(events, 1), CleanExitBlock(1), "journal retains the paired worker's clean exit code");
            Check(CountEvents(events, "close_unconfirmed") == 2 && CountEvents(events, "failure_worker_index") == 1 &&
                  CountEvents(events, "worker_exit_observation_index") == 2 && events.back().name == "close_unconfirmed",
                  "a repeated close records the verdict again but the failure and exit blocks once");
            Check(!ContainsSuspiciousValue(content), "journal contains no nonce/epoch/serial-shaped value");
        }
        if (!DeleteFileW(journal_path.c_str())) return 4;
        if (RemoveQuarantinedFixture(root)) return 4;
    }
    // Case 4b: worker 1 exits with 3 after reading its own close request. The
    // recheck follows its 0 ms at-close look almost immediately, so either a
    // numeric exit code or "not observed" (at close and at recheck) is valid;
    // a deterministic code for this case is left to T2/WU2's wait window.
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
            bool details = false;
            Check(RecordCloseOutcomeToJournal(journal, run.closed, run.failure, run.exits, details),
                  "close outcome fully journaled");
            const auto content = ReadWholeFile(journal_path);
            const auto events = ParseJournal(content);
            CheckEventsAt(events, 0, {{"run_started"}, {"close_unconfirmed"},
                {"failure_worker_index", 1}, {"failure_operation_close"}, {"worker_ipc_unconfirmed"},
                {"worker_response_missing"}, {"worker_status_missing"}, {"worker_ack_write_unconfirmed"},
                {"close_receipt_missing"}},
                "journal retains worker 1's unanswered close as the failure block");
            CheckEventsExactly(ExitBlock(events, 0), CleanExitBlock(0), "journal retains worker 0's clean exit code");
            const auto late = ExitBlock(events, 1);
            Check(FailedWorkerExitCodeRecorded(late, 1, 3) ||
                  BlockIsOneOf(late, {{{"worker_exit_observation_index", 1}, {"worker_exit_check_instant_at_close_failure"},
                                       {"worker_exit_not_observed_at_close"}, {"worker_exit_recheck_after_both_closes"},
                                       {"worker_exit_not_observed_at_recheck"}}}),
                  "case 4b: worker 1's exit block is a numeric code 3 or not observed at close and recheck");
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
            bool details = false;
            Check(RecordCloseOutcomeToJournal(journal, closed, failure, exits, details), "close outcome fully journaled");
            const auto content = ReadWholeFile(journal_path);
            const auto events = ParseJournal(content);
            JournalEvents expected{{"run_started"}, {"both_workers_close_verified"}};
            for (std::uint64_t index = 0; index < 2; ++index)
                for (const auto& event : CleanExitBlock(index)) expected.push_back(event);
            CheckEventsExactly(events, expected,
                               "close verification precedes both workers' waited exit code 0, with no failure block");
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
        if (argc == 2 && std::string_view(argv[1]) == "--ack-failure-contract")
            return ReplyContract(executable, temporary, true);
        if (argc == 2 && std::string_view(argv[1]) == "--journal-contract")
            return JournalContract(executable, worker, temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--category-table") return CategoryTable(temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--journal-vocabulary") return JournalVocabulary(temporary);
        if (argc == 2 && std::string_view(argv[1]) == "--exit-recheck") return ExitRecheck(executable, temporary);
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
