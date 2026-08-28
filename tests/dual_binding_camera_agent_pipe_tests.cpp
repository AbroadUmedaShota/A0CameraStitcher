// Pipe/host contract tests for the Dual binding v1 Named Pipe server
// (GitHub Issue #61).
//
// The binding host does not own a pipe loop -- it shares the one that already
// serves the Single-camera and Dual v2 hosts. So the job here is not to
// re-derive the framing contract, it is to prove that the third caller really
// did inherit it: the same 1 MiB bound, the same fail-closed handling of zero
// length, partial header, partial body and early disconnect, the same bounded
// delivery acknowledgment, and the same exit codes. If the wrapper had quietly
// grown its own loop, every one of these would drift.
//
// Protocol semantics are covered exhaustively at the Handle() level in
// dual_binding_camera_agent_tests.cpp and are not repeated here. What is
// exercised over the wire is the part only a real pipe can show: that a full
// five-operation binding survives being split across separate connections, and
// that a session started over one connection is still the session the next
// connection addresses.

#include "a0/phase0/dual_binding_camera_agent.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace a0::phase0;

namespace {

int failures = 0;
constexpr unsigned char kDeliveryAcknowledgment = 0x06U;
constexpr std::uint32_t kMaximumPipeFrame = 1024U * 1024U;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string UniqueSuffix() {
    static std::atomic<unsigned long long> sequence{};
    return std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence);
}

std::string PipeNameFor(std::string_view label) {
    return "A0CameraStitcher.CameraAgent.HardwareDualBinding.v1.test-" +
        std::string(label) + "-" + UniqueSuffix();
}

std::wstring FullPipeName(std::string_view pipe_name) {
    return L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
}

HANDLE ConnectClient(std::string_view pipe_name, std::chrono::seconds timeout) {
    const std::wstring full_name = FullPipeName(pipe_name);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const HANDLE pipe = CreateFileW(
            full_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return INVALID_HANDLE_VALUE;
}

bool WriteAll(HANDLE pipe, const void* source, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(source);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(size - offset, 64U * 1024U));
        if (!WriteFile(pipe, bytes + offset, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool ReadAll(HANDLE pipe, void* destination, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD read = 0;
        if (!ReadFile(
                pipe, bytes + offset, static_cast<DWORD>(size - offset), &read, nullptr) ||
            read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

std::array<unsigned char, 4> LengthHeader(std::uint32_t length) {
    return {
        static_cast<unsigned char>(length & 0xFFU),
        static_cast<unsigned char>((length >> 8U) & 0xFFU),
        static_cast<unsigned char>((length >> 16U) & 0xFFU),
        static_cast<unsigned char>((length >> 24U) & 0xFFU),
    };
}

std::uint32_t ParseLengthHeader(const std::array<unsigned char, 4>& header) {
    return static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8U) |
        (static_cast<std::uint32_t>(header[2]) << 16U) |
        (static_cast<std::uint32_t>(header[3]) << 24U);
}

std::optional<std::string> SendRequest(
    std::string_view pipe_name, const std::string& request) {
    const HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
    if (pipe == INVALID_HANDLE_VALUE) return std::nullopt;
    const auto header = LengthHeader(static_cast<std::uint32_t>(request.size()));
    if (!WriteAll(pipe, header.data(), header.size()) ||
        !WriteAll(pipe, request.data(), request.size())) {
        CloseHandle(pipe);
        return std::nullopt;
    }
    std::array<unsigned char, 4> response_header{};
    if (!ReadAll(pipe, response_header.data(), response_header.size())) {
        CloseHandle(pipe);
        return std::nullopt;
    }
    std::string response(ParseLengthHeader(response_header), '\0');
    if (!ReadAll(pipe, response.data(), response.size())) {
        CloseHandle(pipe);
        return std::nullopt;
    }
    if (!WriteAll(pipe, &kDeliveryAcknowledgment, 1U)) {
        CloseHandle(pipe);
        return std::nullopt;
    }
    CloseHandle(pipe);
    return response;
}

std::string Envelope(
    std::string_view request_id, std::string_view operation, std::string_view payload) {
    std::string json = R"({"schemaVersion":")";
    json += kDualBindingCameraAgentSchemaVersion;
    json += R"(","simulation":false,"marker":")";
    json += kDualBindingCameraAgentMarker;
    json += R"(","requestId":")";
    json += request_id;
    json += R"(","operation":")";
    json += operation;
    json += R"(","payload":)";
    json += payload;
    json += "}";
    return json;
}

std::string StringFieldOf(std::string_view response, std::string_view field) {
    const std::string key = "\"" + std::string(field) + "\":\"";
    const std::size_t start = response.find(key);
    if (start == std::string_view::npos) return {};
    const std::size_t value_start = start + key.size();
    const std::size_t end = response.find('"', value_start);
    if (end == std::string_view::npos) return {};
    return std::string(response.substr(value_start, end - value_start));
}

// ---------------------------------------------------------------------
// One binding, five operations, five separate connections.
// ---------------------------------------------------------------------
void TestFullBindingSurvivesSeparateConnections() {
    auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
    DualBindingCameraAgentDispatcher dispatcher(adapter);
    const std::string pipe_name = PipeNameFor("full-binding");
    // The budget bounds this test's wall clock, since the assertion at the end
    // is that the host stops on its own deadline rather than on a request. Eight
    // seconds is the smallest value with real margin: the seven round trips
    // below take well under a second even on a loaded runner, so the host cannot
    // expire mid-binding, and the test costs eight seconds instead of the
    // production ten minutes.
    auto server = std::async(std::launch::async, [&] {
        return RunDualBindingCameraAgentNamedPipeServer(
            pipe_name, dispatcher, false, {}, std::chrono::seconds(8));
    });

    const auto begin = SendRequest(
        pipe_name, Envelope("p-begin", "begin-binding", R"({"cameraMode":"DualCamera"})"));
    Check(begin.has_value(), "begin-binding must complete over the pipe");
    const std::string session = begin ? StringFieldOf(*begin, "sessionId") : std::string{};
    Check(session.size() == 32, "the host hands back an addressable session ID");

    const auto started = SendRequest(
        pipe_name,
        Envelope("p-start", "start-candidate-live-view",
                 R"({"sessionId":")" + session + R"(","candidateOrdinal":0})"));
    Check(
        started && started->find("\"success\":true") != std::string::npos,
        "a session started on one connection is still addressable on the next");

    const auto frame = SendRequest(
        pipe_name,
        Envelope("p-frame", "get-candidate-live-view-frame",
                 R"({"sessionId":")" + session + R"(","candidateOrdinal":0})"));
    Check(
        frame && frame->find("\"frameBase64\":") != std::string::npos,
        "a transient Live View frame crosses the pipe intact");

    for (const auto& [ordinal, alias] : {
             std::pair<int, std::string_view>{0, kDualIdentityCameraAliasA},
             std::pair<int, std::string_view>{1, kDualIdentityCameraAliasB}}) {
        if (ordinal == 1) {
            (void)SendRequest(
                pipe_name,
                Envelope("p-start2", "start-candidate-live-view",
                         R"({"sessionId":")" + session + R"(","candidateOrdinal":1})"));
        }
        const auto confirmed = SendRequest(
            pipe_name,
            Envelope("p-confirm", "confirm-alias",
                     R"({"sessionId":")" + session + R"(","candidateOrdinal":)" +
                         std::to_string(ordinal) + R"(,"cameraAlias":")" +
                         std::string(alias) + "\"}"));
        Check(
            confirmed && confirmed->find("\"success\":true") != std::string::npos,
            "each alias confirmation completes over its own connection");
    }

    const auto completed = SendRequest(
        pipe_name,
        Envelope("p-complete", "complete-binding",
                 R"({"sessionId":")" + session +
                     R"(","confirmedAtUtc":"2026-08-21T00:00:00Z"})"));
    Check(
        completed && completed->find("\"resultCode\":\"BindingCompleted\"") != std::string::npos,
        "the binding completes over the pipe");
    Check(
        completed && completed->find("fake-source-object") == std::string::npos,
        "nothing that crossed the pipe carried a source object token");
    Check(
        dispatcher.BindingState() == DualIdentitySessionBindingState::Ready,
        "the host's dispatcher holds the completed binding");

    Check(
        server.wait_for(std::chrono::seconds(20)) == std::future_status::ready,
        "the binding host terminates on its own lifetime bound");
    Check(server.get() == 0, "a fully delivered session ends the host cleanly");
}

void TestCancellationResponseIsDeliveredBeforeHostExit() {
    auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
    DualBindingCameraAgentDispatcher dispatcher(adapter);
    const std::string pipe_name = PipeNameFor("cancel-and-exit");
    auto server = std::async(std::launch::async, [&] {
        return RunDualBindingCameraAgentNamedPipeServer(
            pipe_name, dispatcher, false, {}, std::chrono::seconds(20));
    });

    const auto begin = SendRequest(
        pipe_name, Envelope("p-begin", "begin-binding", R"({"cameraMode":"DualCamera"})"));
    const std::string session = begin ? StringFieldOf(*begin, "sessionId") : std::string{};
    (void)SendRequest(
        pipe_name,
        Envelope("p-start", "start-candidate-live-view",
                 R"({"sessionId":")" + session + R"(","candidateOrdinal":0})"));
    const auto cancelled = SendRequest(
        pipe_name,
        Envelope("p-cancel", "cancel-binding",
                 R"({"sessionId":")" + session + "\"}"));

    Check(cancelled &&
          cancelled->find("\"resultCode\":\"BindingCancelled\"") != std::string::npos,
        "the client receives the checked cleanup response");
    Check(
        server.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "the binding host exits promptly after the delivered cancellation");
    Check(server.get() == 0, "a delivered successful cancellation exits cleanly");
    Check(adapter->EndBindingSessionCount() == 1 &&
          adapter->ActiveLiveViewCount() == 0,
        "pipe cancellation performs one cleanup and leaves no Live View");
}

// ---------------------------------------------------------------------
// A session belongs to the host process that produced it. Restarting the
// host is the strongest form of the invalidation rule, and it must hold
// over the wire and not merely in a unit test's memory.
// ---------------------------------------------------------------------
void TestRestartedHostRefusesThePreviousSession() {
    std::string session;
    {
        auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
        DualBindingCameraAgentDispatcher dispatcher(adapter);
        const std::string pipe_name = PipeNameFor("restart-before");
        auto server = std::async(std::launch::async, [&] {
            return RunDualBindingCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });
        const auto begin = SendRequest(
            pipe_name,
            Envelope("p-begin", "begin-binding", R"({"cameraMode":"DualCamera"})"));
        Check(begin.has_value(), "the pre-restart session must start");
        if (begin) session = StringFieldOf(*begin, "sessionId");
        Check(
            server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "the pre-restart host must terminate");
        Check(server.get() == 0, "the pre-restart request was fully delivered");
    }
    Check(session.size() == 32, "a session ID was captured before the restart");

    auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
    DualBindingCameraAgentDispatcher dispatcher(adapter);
    const std::string pipe_name = PipeNameFor("restart-after");
    auto server = std::async(std::launch::async, [&] {
        return RunDualBindingCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
    });
    const auto response = SendRequest(
        pipe_name,
        Envelope("p-start", "start-candidate-live-view",
                 R"({"sessionId":")" + session + R"(","candidateOrdinal":0})"));
    Check(
        response && response->find("\"resultCode\":\"SessionMismatch\"") != std::string::npos,
        "a restarted host refuses the session the previous one issued");
    Check(
        dispatcher.SafetyCounters().rejected_stale_session_count == 1,
        "the restarted host counts the refused pre-restart request");
    Check(
        server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
        "the post-restart host must terminate");
    Check(server.get() == 0, "a typed rejection is still a fully delivered response");
}

// ---------------------------------------------------------------------
// The inherited 1 MiB bound: limit-1 and limit are transported, limit+1
// fails closed before dispatch with the same exit code the other two
// hosts use.
// ---------------------------------------------------------------------
void TestMaximumFrameBoundaryMatchesTheOtherHosts() {
    for (const std::uint32_t length :
         {kMaximumPipeFrame - 1U, kMaximumPipeFrame, kMaximumPipeFrame + 1U}) {
        auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
        DualBindingCameraAgentDispatcher dispatcher(adapter);
        const std::string pipe_name = PipeNameFor("boundary");
        auto server = std::async(std::launch::async, [&] {
            return RunDualBindingCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });

        HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "boundary client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto header = LengthHeader(length);
            Check(
                WriteAll(pipe, header.data(), header.size()),
                "boundary frame header must be writable");
            if (length <= kMaximumPipeFrame) {
                const std::string body(length, ' ');
                Check(
                    WriteAll(pipe, body.data(), body.size()),
                    "limit-1 and limit frame bodies must be accepted for transport");
                std::array<unsigned char, 4> response_header{};
                Check(
                    ReadAll(pipe, response_header.data(), response_header.size()),
                    "limit-1 and limit frames must receive a response envelope");
                std::string response(ParseLengthHeader(response_header), '\0');
                Check(
                    ReadAll(pipe, response.data(), response.size()),
                    "limit-1 and limit frames must receive a complete response body");
                Check(
                    response.find("\"resultCode\":\"MalformedEnvelope\"") != std::string::npos,
                    "an oversized-but-transportable body is rejected by the parser, "
                    "not by the transport");
                Check(
                    WriteAll(pipe, &kDeliveryAcknowledgment, 1U),
                    "limit-1 and limit responses must be acknowledged");
            }
            CloseHandle(pipe);
        }
        Check(
            server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "boundary server must terminate");
        Check(
            server.get() == (length <= kMaximumPipeFrame ? 0 : 2),
            "the binding host uses the same 1 MiB frame limit as the other hosts");
        Check(
            dispatcher.SafetyCounters().binding_session_count == 0,
            "an arbitrary body never starts a binding session");
    }
}

// ---------------------------------------------------------------------
// Zero length, partial header, partial body and an early disconnect all
// fail closed before dispatch. "Before dispatch" is the part that matters
// for binding: a half-read frame must never begin a session an operator
// could then be shown.
// ---------------------------------------------------------------------
void TestTruncatedFramesNeverStartASession() {
    struct Scenario {
        std::string_view label;
        std::size_t header_bytes;
        std::uint32_t declared_length;
        std::size_t body_bytes;
    };
    const Scenario scenarios[]{
        {"zero-length", 4, 0, 0},
        {"partial-header", 2, 0x20, 0},
        {"partial-body", 4, 64, 10},
        {"header-only-disconnect", 4, 4096, 0},
    };

    for (const Scenario& scenario : scenarios) {
        auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
        DualBindingCameraAgentDispatcher dispatcher(adapter);
        const std::string pipe_name = PipeNameFor(scenario.label);
        auto server = std::async(std::launch::async, [&] {
            return RunDualBindingCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });

        HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "truncated-frame client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto header = LengthHeader(scenario.declared_length);
            Check(
                WriteAll(pipe, header.data(), scenario.header_bytes),
                "the truncated client must write its partial header");
            if (scenario.body_bytes > 0) {
                const std::string partial(scenario.body_bytes, 'x');
                Check(
                    WriteAll(pipe, partial.data(), partial.size()),
                    "the truncated client must write its partial body");
            }
            CloseHandle(pipe);
        }
        Check(
            server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "the truncated-frame server must terminate");
        Check(server.get() == 2, "a truncated frame fails closed before dispatch");
        Check(
            dispatcher.SafetyCounters().binding_session_count == 0 &&
                adapter->EnumerationCount() == 0,
            "a truncated frame never enumerates candidates or starts a session");
    }
}

// ---------------------------------------------------------------------
// A response the client never acknowledges is a delivery failure, and the
// host has to say so with the dispatched-but-undelivered exit code rather
// than reporting success. For binding this is what lets a client tell
// "the session was never created" from "it was created and I did not hear
// about it" -- and the latter means the session really does exist.
// ---------------------------------------------------------------------
void TestMissingAcknowledgmentIsADeliveryFailure() {
    auto adapter = std::make_shared<DualBindingFakeSdkAdapter>();
    DualBindingCameraAgentDispatcher dispatcher(adapter);
    const std::string pipe_name = PipeNameFor("missing-ack");
    auto server = std::async(std::launch::async, [&] {
        return RunDualBindingCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
    });

    HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
    Check(pipe != INVALID_HANDLE_VALUE, "missing-ack client must connect");
    if (pipe != INVALID_HANDLE_VALUE) {
        const std::string request =
            Envelope("p-begin", "begin-binding", R"({"cameraMode":"DualCamera"})");
        const auto header = LengthHeader(static_cast<std::uint32_t>(request.size()));
        Check(
            WriteAll(pipe, header.data(), header.size()) &&
                WriteAll(pipe, request.data(), request.size()),
            "missing-ack client must send a complete request");
        std::array<unsigned char, 4> response_header{};
        Check(
            ReadAll(pipe, response_header.data(), response_header.size()),
            "missing-ack client must receive the response header");
        std::string response(ParseLengthHeader(response_header), '\0');
        Check(
            ReadAll(pipe, response.data(), response.size()),
            "missing-ack client must receive the response body");
        // Deliberately no acknowledgment.
        CloseHandle(pipe);
    }
    Check(
        server.wait_for(std::chrono::seconds(15)) == std::future_status::ready,
        "the missing-ack server must terminate");
    Check(
        server.get() == 3,
        "an unacknowledged response is reported as dispatched-but-undelivered");
    Check(
        dispatcher.SafetyCounters().binding_session_count == 1,
        "the session really was created, which is why the client is told delivery failed");
}

} // namespace

int main() {
    TestFullBindingSurvivesSeparateConnections();
    TestCancellationResponseIsDeliveredBeforeHostExit();
    TestRestartedHostRefusesThePreviousSession();
    TestMaximumFrameBoundaryMatchesTheOtherHosts();
    TestTruncatedFramesNeverStartASession();
    TestMissingAcknowledgmentIsADeliveryFailure();

    if (failures != 0) {
        std::cerr << failures << " dual binding pipe contract failures\n";
        return 1;
    }
    std::cout << "dual binding pipe contracts passed\n";
    return 0;
}
