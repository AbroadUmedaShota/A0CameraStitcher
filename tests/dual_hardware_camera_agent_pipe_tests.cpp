// Native pipe/host contract tests for the Dual hardware v2 Named Pipe
// server (Issue #5). These tests exercise RunDualHardwareCameraAgentNamedPipeServer
// end to end over a real Windows named pipe: framing, the dedicated
// current-logon access boundary (implicitly, since these tests run as the
// same logon session that creates the pipe), the shared bounded delivery-ACK
// contract, persistent multi-request handling, and the
// backend-unavailable / response-unknown resilience contracts described in
// GitHub Issue #5.
//
// dual_hardware_camera_agent_tests.cpp already covers Handle()-level
// protocol/parsing/semantic-preflight contracts exhaustively; this file does
// not repeat that coverage and instead focuses on the transport/host layer
// this Issue adds.

#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent_store.hpp"
#include "a0/phase0/agent_host_lifetime.hpp"

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
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

int failures = 0;
constexpr unsigned char kDeliveryAcknowledgment = 0x06U;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckContains(
    std::string_view value,
    std::string_view expected,
    std::string_view message) {
    Check(value.find(expected) != std::string_view::npos, message);
}

void JoinPersistentHost(std::future<int>& server) {
    if (server.wait_for(std::chrono::seconds(8)) != std::future_status::ready) {
        // This executable has no hardware backend. Fail the test process instead
        // of hanging in future::get if the fixed-deadline contract regresses.
        std::cerr << "FAIL: persistent host did not stop at the fixed deadline\n" << std::flush;
        std::_Exit(1);
    }
    Check(server.get() == 0, "a fixed-deadline shutdown must exit 0");
}

std::string UniqueSuffix() {
    static std::atomic<unsigned long long> sequence{};
    return std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence);
}

fs::path MakeTempRoot(std::string_view label) {
    const fs::path root = fs::temp_directory_path() /
        ("a0-dual-pipe-test-" + std::string(label) + "-" + UniqueSuffix());
    fs::create_directories(root);
    return root;
}

std::string PipeNameFor(std::string_view label) {
    return "A0CameraStitcher.CameraAgent.HardwareDual.v2.test-" +
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
            full_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
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
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(size - offset, 64U * 1024U));
        if (!WriteFile(pipe, bytes + offset, chunk, &written, nullptr) ||
            written == 0) {
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

// Sends one length-prefixed request over a fresh pipe connection and returns
// the complete response body, or std::nullopt if the connection or framing
// never completed.
std::optional<std::string> SendRequest(
    std::string_view pipe_name,
    const std::string& request) {
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
    const std::uint32_t response_length = ParseLengthHeader(response_header);
    std::string response(response_length, '\0');
    if (!ReadAll(pipe, response.data(), response.size())) {
        CloseHandle(pipe);
        return std::nullopt;
    }
    if (!WriteAll(pipe, &kDeliveryAcknowledgment, 1U)) {
        CloseHandle(pipe);
        return std::nullopt;
    }
    // Wait for the server's disconnect, so callers advancing an injected
    // clock cannot race the server's acceptance of the just-written ACK.
    unsigned char trailing{};
    DWORD trailing_read{};
    (void)ReadFile(pipe, &trailing, 1, &trailing_read, nullptr);
    Check(trailing_read == 0, "the server must not append bytes after a complete response");
    CloseHandle(pipe);
    return response;
}

std::chrono::system_clock::time_point FixedNow();
std::string CapabilitiesEnvelope(std::string_view request_id);

void TestDeliveryAcknowledgmentIsRequired() {
    enum class AckCase { header_only, partial_body, full_no_ack, invalid, late, unread, injected };
    for (const AckCase test_case : {
             AckCase::header_only,
             AckCase::partial_body,
             AckCase::full_no_ack,
             AckCase::invalid,
             AckCase::late,
             AckCase::unread,
             AckCase::injected}) {
        const std::string pipe_name = PipeNameFor("delivery-ack");
        DualHardwareCameraAgentDispatcher dispatcher;
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(
                pipe_name,
                dispatcher,
                true,
                DualHardwareCameraAgentPipeFailureInjectionForTesting{
                    .fail_delivery_ack_wait = test_case == AckCase::injected});
        });
        const HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "delivery-ack client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const std::string request = CapabilitiesEnvelope("delivery-ack-required");
            const auto header = LengthHeader(static_cast<std::uint32_t>(request.size()));
            Check(WriteAll(pipe, header.data(), header.size()) &&
                    WriteAll(pipe, request.data(), request.size()),
                "delivery-ack request must be writable");
            if (test_case != AckCase::unread) {
                std::array<unsigned char, 4> response_header{};
                const bool header_read =
                    ReadAll(pipe, response_header.data(), response_header.size());
                Check(header_read || test_case == AckCase::injected,
                    "delivery-ack response header must be readable unless ACK wait failure is injected");
                if (header_read) {
                    const std::uint32_t response_length = ParseLengthHeader(response_header);
                    if (test_case == AckCase::partial_body) {
                        unsigned char one_byte = 0;
                        Check(ReadAll(pipe, &one_byte, 1U),
                            "partial-body case must read one response byte");
                    } else if (test_case != AckCase::header_only) {
                        std::string response(response_length, '\0');
                        const bool body_read = ReadAll(pipe, response.data(), response.size());
                        Check(body_read || test_case == AckCase::injected,
                            "delivery-ack response body must be readable unless wait failure is injected");
                        if (body_read && test_case == AckCase::invalid) {
                            const unsigned char invalid = 0x15U;
                            (void)WriteAll(pipe, &invalid, 1U);
                        } else if (body_read && test_case == AckCase::late) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                            (void)WriteAll(pipe, &kDeliveryAcknowledgment, 1U);
                        } else if (body_read && test_case == AckCase::injected) {
                            (void)WriteAll(pipe, &kDeliveryAcknowledgment, 1U);
                        }
                    }
                }
            }
            Check(server.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                "missing, partial, invalid, late, unread, or injected ACK must terminate within the bound");
            CloseHandle(pipe);
        }
        if (server.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
            Check(server.get() == 3,
                "delivery ACK failure after dispatch must exit with code 3");
        } else {
            Check(false, "delivery ACK failure server must terminate");
        }
        Check(dispatcher.SafetyCounters().pair_dispatch_count == 0,
            "delivery ACK failures must never redispatch a pair");
    }
}

std::string Envelope(
    std::string_view operation,
    std::string_view payload,
    std::string_view request_id,
    std::string_view schema = kDualHardwareCameraAgentSchemaVersion) {
    return "{\"schemaVersion\":\"" + std::string(schema) + "\","
           "\"simulation\":false,\"marker\":\"Hardware\",\"requestId\":\"" +
        std::string(request_id) + "\",\"operation\":\"" + std::string(operation) +
        "\",\"payload\":" + std::string(payload) + "}";
}

std::string CapabilitiesEnvelope(std::string_view request_id = "pipe-contract-caps") {
    return Envelope("get-dual-capabilities", "{\"cameraMode\":\"DualCamera\"}", request_id);
}

std::string ReservationEnvelope(
    std::string_view transaction_id,
    std::string_view request_id = "pipe-contract-reserve") {
    const std::string payload = "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"cameraMode\":\"DualCamera\",\"orderedRequiredAliases\":[\"CAM-A\",\"CAM-B\"]}";
    return Envelope("reserve-pair-transaction", payload, request_id);
}

std::string QueryEnvelope(
    std::string_view transaction_id,
    std::string_view request_id = "pipe-contract-query",
    std::string_view schema = kDualHardwareCameraAgentSchemaVersion) {
    return Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"" + std::string(transaction_id) + "\"}",
        request_id, schema);
}

std::string CloseEnvelope(
    std::string_view transaction_id,
    std::string_view request_id = "pipe-contract-close",
    std::string_view schema = kDualHardwareCameraAgentSchemaVersion) {
    return Envelope(
        "close-reserved-pair-transaction",
        "{\"transactionId\":\"" + std::string(transaction_id) + "\"}",
        request_id, schema);
}

// Fixed clock/timestamps mirror dual_hardware_camera_agent_tests.cpp exactly,
// so this file stays focused on transport/host behavior instead of
// re-deriving the semantic-preflight math that file already covers. Using
// an injected clock (rather than the real wall clock the production host
// falls back to -- see dual_hardware_camera_agent_main.cpp) keeps the
// timestamps below valid regardless of how long a test machine takes to run.
std::chrono::system_clock::time_point FixedNow() {
    using namespace std::chrono;
    return sys_days{year{2026} / August / 14} + minutes{1};
}

std::string StartEnvelope(
    std::string_view transaction_id,
    std::string_view request_id) {
    constexpr std::string_view body_a =
        "{\"alias\":\"CAM-A\",\"imageArea\":\"FX\",\"fileFormat\":\"JPEG\","
        "\"jpegQuality\":\"Fine\",\"imageSize\":\"L\",\"exposureMode\":\"Manual\","
        "\"autoIsoEnabled\":false,\"focusMode\":\"Manual\",\"whiteBalanceMode\":\"Fixed\","
        "\"vibrationReductionEnabled\":false}";
    constexpr std::string_view body_b =
        "{\"alias\":\"CAM-B\",\"imageArea\":\"FX\",\"fileFormat\":\"JPEG\","
        "\"jpegQuality\":\"Fine\",\"imageSize\":\"L\",\"exposureMode\":\"Manual\","
        "\"autoIsoEnabled\":false,\"focusMode\":\"Manual\",\"whiteBalanceMode\":\"Fixed\","
        "\"vibrationReductionEnabled\":false}";
    const std::string payload =
        "{\"cameraMode\":\"DualCamera\",\"orderedRequiredAliases\":[\"CAM-A\",\"CAM-B\"]"
        ",\"transaction\":{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"transactionDirectory\":\"C:/anonymous/pair\""
        ",\"identitySnapshot\":{\"status\":\"Ready\",\"reasonCode\":\"pipe-test-ready\","
        "\"observedAtUtc\":\"2026-08-14T00:00:00Z\",\"expiresAtUtc\":\"2026-08-14T01:00:00+00:00\"}"
        ",\"captureProfileSnapshot\":{\"profileId\":\"pipe-test-profile\",\"version\":\"1\","
        "\"schemaVersion\":\"a0.hardware-dual-capture-profile.v1\",\"status\":\"Approved\","
        "\"approvedAtUtc\":\"2026-08-13T00:00:00Z\",\"validUntilUtc\":\"2026-08-15T00:00:00Z\","
        "\"bodies\":[" + std::string(body_a) + "," + std::string(body_b) + "]}"
        ",\"rigProfileSnapshot\":{\"profileId\":\"pipe-test-rig\",\"version\":\"1\","
        "\"status\":\"Approved\",\"schemaVersion\":\"1.1.0\",\"provenance\":\"pipe-test\","
        "\"measuredAtUtc\":\"2026-08-12T00:00:00Z\",\"validUntilUtc\":\"2026-08-15T00:00:00Z\","
        "\"assessedAtUtc\":\"2026-08-13T00:00:00Z\",\"expectedInputWidth\":7360,"
        "\"expectedInputHeight\":4912,\"cameraBToCameraA\":[1,0,12,0,1,0,0,0,1],"
        "\"layout\":\"camera-a-left-camera-b-right\",\"crop\":[1,1,1,1],"
        "\"cameraAliases\":[\"CAM-A\",\"CAM-B\"]}"
        ",\"operatorConfirmations\":{\"identitySnapshotApproved\":true,\"captureProfileFrozen\":true,"
        "\"rigProfileFrozen\":true,\"liveViewStoppedAndClosed\":true,\"bothCardsConfirmedEmpty\":true}"
        ",\"startedAtUtc\":\"2026-08-14T00:00:00+00:00\""
        ",\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00+00:00\"}}";
    return Envelope("start-reserved-pair", payload, request_id);
}

std::string CaptureRecoveryOnlyEnvelope(
    std::string_view transaction_id,
    std::string_view request_id) {
    const std::string payload =
        "{\"cameraMode\":\"DualCamera\",\"orderedRequiredAliases\":[\"CAM-A\",\"CAM-B\"]"
        ",\"transaction\":{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"transactionDirectory\":\"C:/anonymous/capture-recovery-only\""
        ",\"identitySnapshot\":{\"status\":\"Ready\",\"reasonCode\":\"pipe-test-ready\","
        "\"observedAtUtc\":\"2026-08-14T00:00:00Z\",\"expiresAtUtc\":\"2026-08-14T01:00:00+00:00\"}"
        ",\"captureProfileSnapshot\":{\"schemaVersion\":\"a0.dual-capture-profile.operator-approved.v1\","
        "\"cameraMode\":\"DualCamera\",\"cameraModel\":\"Nikon D810\",\"imageFormat\":\"JPEG Fine\","
        "\"imageSize\":\"L\",\"pixelDimensions\":\"7360x4912\","
        "\"cameraSettingWritesApproved\":false,\"automaticRetryApproved\":false,"
        "\"actualShutterSynchronizationGuaranteed\":false,\"approvalBasis\":\"operator-approved-capture-recovery-only-v1\"}"
        ",\"operatorConfirmations\":{\"identitySnapshotApproved\":true,\"captureProfileFrozen\":true,"
        "\"liveViewStoppedAndClosed\":true,\"bothCardsConfirmedEmpty\":true,"
        "\"captureRecoveryOnlyApproved\":true}"
        ",\"startedAtUtc\":\"2026-08-14T00:00:00+00:00\""
        ",\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00+00:00\"}}";
    return Envelope(
        "start-reserved-capture-recovery-only",
        payload,
        request_id,
        kDualHardwareCameraAgentCaptureRecoveryOnlySchemaVersion);
}

// ---------------------------------------------------------------------
// 1 MiB frame boundary: limit-1/limit accepted, limit+1 rejected fail
// closed. Mirrors hardware_camera_agent_tests.cpp's
// TestNamedPipeMaximumFrameBoundary for the Single host, proving the two
// hosts share byte-identical framing (kMaximumPipeFrameBytes).
// ---------------------------------------------------------------------
void TestMaximumFrameBoundaryMatchesSingleContract() {
    constexpr std::uint32_t maximum = 1024U * 1024U;
    for (const std::uint32_t length : {maximum - 1U, maximum, maximum + 1U}) {
        DualHardwareCameraAgentDispatcher dispatcher;
        const std::string pipe_name = PipeNameFor("boundary");
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });

        HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "boundary client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto header = LengthHeader(length);
            Check(WriteAll(pipe, header.data(), header.size()),
                "boundary frame header must be writable");
            if (length <= maximum) {
                const std::string body(length, ' ');
                Check(WriteAll(pipe, body.data(), body.size()),
                    "limit-1 and limit frame bodies must be accepted for transport");
                std::array<unsigned char, 4> response_header{};
                Check(ReadAll(pipe, response_header.data(), response_header.size()),
                    "limit-1 and limit frames must receive a response envelope");
                const std::uint32_t response_length = ParseLengthHeader(response_header);
                Check(response_length > 0 && response_length <= maximum,
                    "response header must describe a bounded body");
                std::string response(response_length, '\0');
                Check(ReadAll(pipe, response.data(), response.size()),
                    "limit-1 and limit frames must receive a complete response body");
                Check(WriteAll(pipe, &kDeliveryAcknowledgment, 1U),
                    "limit-1 and limit responses must be acknowledged");
            }
            CloseHandle(pipe);
        }
        Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "boundary server must terminate");
        const int exit_code = server.get();
        Check(exit_code == (length <= maximum ? 0 : 2),
            "1 MiB frame limit must accept limit-1/limit and reject limit+1");
        const auto counters = dispatcher.SafetyCounters();
        Check(counters.pair_dispatch_count == 0 && counters.camera_access_count == 0,
            "oversized or arbitrary-body frames must never reach a pair dispatch");
    }
}

// ---------------------------------------------------------------------
// length 0 and partial header/body frames must fail closed before any
// dispatch, exactly like the Single host.
// ---------------------------------------------------------------------
void TestZeroLengthAndPartialFramesFailClosedWithoutDispatch() {
    {
        DualHardwareCameraAgentDispatcher dispatcher;
        const std::string pipe_name = PipeNameFor("zero-length");
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });
        HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "zero-length client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto header = LengthHeader(0);
            Check(WriteAll(pipe, header.data(), header.size()),
                "zero-length header must be writable");
            CloseHandle(pipe);
        }
        Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "zero-length server must terminate");
        Check(server.get() == 2, "a zero-length frame must fail closed before dispatch");
        const auto counters = dispatcher.SafetyCounters();
        Check(counters.pair_dispatch_count == 0 && counters.camera_access_count == 0,
            "a zero-length frame must never reach a pair dispatch");
    }
    {
        DualHardwareCameraAgentDispatcher dispatcher;
        const std::string pipe_name = PipeNameFor("partial-header");
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });
        HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "partial-header client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const std::array<unsigned char, 2> partial_header{0x20U, 0x00U};
            Check(WriteAll(pipe, partial_header.data(), partial_header.size()),
                "partial-header client must write only half of the length header");
            CloseHandle(pipe);
        }
        Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "partial-header server must terminate");
        Check(server.get() == 2, "a partial length header must fail closed before dispatch");
        const auto counters = dispatcher.SafetyCounters();
        Check(counters.pair_dispatch_count == 0 && counters.camera_access_count == 0,
            "a partial length header must never reach a pair dispatch");
    }
    {
        DualHardwareCameraAgentDispatcher dispatcher;
        const std::string pipe_name = PipeNameFor("partial-body");
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });
        HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "partial-body client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto header = LengthHeader(64);
            const std::string partial_body(10, 'x');
            Check(WriteAll(pipe, header.data(), header.size()) &&
                    WriteAll(pipe, partial_body.data(), partial_body.size()),
                "partial-body client must write a full header and a truncated body");
            CloseHandle(pipe);
        }
        Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "partial-body server must terminate");
        Check(server.get() == 2, "a partial frame body must fail closed before dispatch");
        const auto counters = dispatcher.SafetyCounters();
        Check(counters.pair_dispatch_count == 0 && counters.camera_access_count == 0,
            "a partial frame body must never reach a pair dispatch");
    }
}

// ---------------------------------------------------------------------
// Malformed JSON is a dispatch-level rejection (Handle() already returns a
// typed AgentFailure/MalformedEnvelope response), not a transport failure:
// the frame must still be fully delivered (exit 0), proving the pipe host
// really does hand the body to the existing parser rather than special-
// casing it.
// ---------------------------------------------------------------------
void TestMalformedJsonBodyGetsTypedRejectionOverRealHost() {
    DualHardwareCameraAgentDispatcher dispatcher;
    const std::string pipe_name = PipeNameFor("malformed-json");
    auto server = std::async(std::launch::async, [&] {
        return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
    });

    const auto response = SendRequest(pipe_name, "{not-json");
    Check(response.has_value(), "malformed JSON must still be fully delivered as a rejection");
    if (response) {
        CheckContains(*response, "\"success\":false",
            "malformed JSON must be rejected, not silently accepted");
        CheckContains(*response, "\"rejected\":true",
            "a parser-level rejection must use the typed rejection payload shape");
    }
    Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
        "malformed-JSON server must terminate");
    Check(server.get() == 0,
        "a malformed JSON body is a dispatch-level rejection, not a transport failure");
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.pair_dispatch_count == 0 && counters.camera_access_count == 0,
        "a malformed request must never reach a pair dispatch");
}

// ---------------------------------------------------------------------
// One persistent (non-serve-once) host answers capabilities, reserve, a
// duplicate reservation, same-ID query, close, and close-tombstone query as
// separate pipe connections,
// proving: (a) the persistent multi-request contract, (b) capabilities/
// reserve/query match the existing typed contract when served through the
// real host, and (c) duplicate delivery of the same reservation is rejected
// rather than silently re-accepted. Keep the lifetime clock fixed during
// durable I/O, then advance it explicitly. This is a protocol test, not a
// claim that six filesystem/pipe round trips always finish within 4.5s.
// ---------------------------------------------------------------------
void TestPersistentMultiRequestCapabilitiesReserveDuplicateAndQuery() {
    const fs::path root = MakeTempRoot("persistent");
    auto store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
    DualHardwareCameraAgentDispatcher dispatcher(store);
    const std::string pipe_name = PipeNameFor("persistent");
    std::atomic<std::uint64_t> lifetime_ticks{1000};
    auto server = std::async(std::launch::async, [&] {
        return RunDualHardwareCameraAgentNamedPipeServer(
            pipe_name, dispatcher, /*serve_once=*/false,
            {.lifetime_ticks_for_testing = [&] { return lifetime_ticks.load(); }},
            std::chrono::milliseconds(4500));
    });

    const std::string transaction_id = "10101010101010101010101010101010";

    const auto capabilities = SendRequest(pipe_name, CapabilitiesEnvelope());
    Check(capabilities.has_value(), "capabilities must be delivered over the real host");
    if (capabilities) {
        CheckContains(*capabilities, "\"schemaVersion\":\"a0.camera-agent.hardware-dual.v2\"",
            "capabilities must use the Dual hardware v2 schema over the real host");
        CheckContains(*capabilities, "\"resultCode\":\"DualCapabilities\"",
            "capabilities must have the typed result code over the real host");
        CheckContains(*capabilities, "\"pairJournalDurable\":true",
            "capabilities must still advertise a durable pair journal over the real host");
        CheckContains(*capabilities, "\"automaticRetryCount\":0",
            "capabilities must still prohibit automatic retries over the real host");
    }

    const auto reserved = SendRequest(pipe_name, ReservationEnvelope(transaction_id));
    lifetime_ticks.store(2000);
    Check(reserved.has_value(), "reservation must be delivered over the real host");
    if (reserved) {
        CheckContains(*reserved, "\"resultCode\":\"PairTransactionReserved\"",
            "a fresh reservation must succeed over the real host and durable store");
        CheckContains(*reserved, "\"accepted\":true",
            "a fresh reservation must be accepted over the real host");
    }

    // Duplicate delivery: a second connection replaying the identical
    // reservation (as a client would after a response it could not trust)
    // must not create a second reservation.
    const auto duplicate = SendRequest(
        pipe_name, ReservationEnvelope(transaction_id, "pipe-contract-reserve-dup"));
    lifetime_ticks.store(2500);
    Check(duplicate.has_value(), "the duplicate reservation attempt must be delivered");
    if (duplicate) {
        CheckContains(*duplicate, "\"success\":false",
            "a duplicate reservation must be rejected");
        CheckContains(*duplicate, "\"resultCode\":\"DuplicateTransactionId\"",
            "a duplicate reservation for the same transaction ID must be typed DuplicateTransactionId");
        CheckContains(*duplicate, "\"accepted\":false",
            "a duplicate reservation must never report accepted:true");
    }

    const auto queried = SendRequest(
        pipe_name, QueryEnvelope(transaction_id, "pipe-contract-query-1"));
    lifetime_ticks.store(3000);
    Check(queried.has_value(), "the same-ID query must be delivered over the real host");
    if (queried) {
        CheckContains(*queried, "\"resultCode\":\"PairTransactionReserved\"",
            "a same-ID query for a Reserved transaction must report PairTransactionReserved");
        CheckContains(*queried, "\"found\":true",
            "a same-ID query for a reserved transaction must report found:true");
    }


    const auto closed = SendRequest(
        pipe_name, CloseEnvelope(transaction_id));
    lifetime_ticks.store(3500);
    Check(closed.has_value(), "the exact close must be delivered over the real host");
    if (closed) {
        CheckContains(*closed, "\"resultCode\":\"PairTransactionClosedBeforeDispatch\"",
            "a real-host close must confirm the durable tombstone");
        CheckContains(*closed, "\"closedBeforeDispatch\":true",
            "a real-host close must not acknowledge before the tombstone reread");
    }
    // Regression: a slow client exceeds the old shortened test lifetime.
    // The semantic clock remains inside the deadline; no product budget changes.
    std::this_thread::sleep_for(std::chrono::milliseconds(5600));
    const auto closed_query = SendRequest(
        pipe_name, QueryEnvelope(transaction_id, "pipe-contract-query-closed"));
    Check(closed_query.has_value(), "the close tombstone query must be delivered");
    if (closed_query) {
        CheckContains(*closed_query, "\"resultCode\":\"PairTransactionClosedBeforeDispatch\"",
            "restart-safe query must distinguish a closed reservation");
        CheckContains(*closed_query, "\"found\":true,\"result\":null",
            "a close tombstone must never forge a capture result");
    }

    lifetime_ticks.store(5500); // Exactly the fixed launch deadline.
    // One already-pending accept may still take 5s. Join before reading counters.
    JoinPersistentHost(server);

    const auto counters = dispatcher.SafetyCounters();
    Check(counters.pair_dispatch_count == 0,
        "capabilities/reserve/duplicate/query/close must never touch pair_dispatch_count");
    Check(counters.camera_access_count == 0,
        "the Dual host must perform zero camera access without a real backend");
}

// ---------------------------------------------------------------------
// Client disconnect mid-frame: the well-established "client disconnect"
// negative case, mirroring hardware_camera_agent_tests.cpp's
// TestServeOnceRejectsPartialFrameWithoutDispatch for the Single host.
// ---------------------------------------------------------------------
void TestClientDisconnectBeforeFrameCompletesNeverDispatches() {
    const fs::path root = MakeTempRoot("disconnect");
    auto store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
    DualHardwareCameraAgentDispatcher dispatcher(store);
    const std::string pipe_name = PipeNameFor("disconnect");
    auto server = std::async(std::launch::async, [&] {
        return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
    });

    HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
    Check(pipe != INVALID_HANDLE_VALUE, "disconnect-test client must connect");
    if (pipe != INVALID_HANDLE_VALUE) {
        const std::array<unsigned char, 2> partial_header{0x10U, 0x00U};
        Check(WriteAll(pipe, partial_header.data(), partial_header.size()),
            "disconnect-test client must write only half of the length header");
        CloseHandle(pipe);
    }

    Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
        "disconnect-test server must terminate");
    Check(server.get() == 2, "a client disconnect mid-frame must fail closed before dispatch");
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.pair_dispatch_count == 0 && counters.camera_access_count == 0,
        "a client disconnect mid-frame must never reach a pair dispatch");
}

// ---------------------------------------------------------------------
// A production-shaped dispatcher (durable store, no fake backend --
// matching dual_hardware_camera_agent_main.cpp exactly) must answer
// start-reserved-pair with PairDispatcherUnavailable only *after* running
// the complete identity/capture-profile/rig-profile/confirmation preflight,
// and must never touch pair_dispatch_count (store-before-side-effect
// order). A repeated start attempt must behave identically. All requests,
// including the post-start query, are sent to the *same* still-running host
// instance -- the query must run before the host is allowed to shut down,
// not after (a shut-down host has no listener left to connect to).
// ---------------------------------------------------------------------
void TestBackendUnavailableStartFailsClosedAfterFullPreflight() {
    const fs::path root = MakeTempRoot("start-unavailable");
    auto store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
    DualHardwareCameraAgentDispatcher dispatcher(store, [] { return FixedNow(); });
    const std::string pipe_name = PipeNameFor("start-unavailable");
    std::atomic<std::uint64_t> lifetime_ticks{1000};
    auto server = std::async(std::launch::async, [&] {
        return RunDualHardwareCameraAgentNamedPipeServer(
            pipe_name, dispatcher, /*serve_once=*/false,
            {.lifetime_ticks_for_testing = [&] { return lifetime_ticks.load(); }},
            std::chrono::milliseconds(4500));
    });

    const std::string transaction_id = "20202020202020202020202020202020";
    const auto reserved = SendRequest(pipe_name, ReservationEnvelope(transaction_id));
    Check(reserved.has_value() &&
              reserved->find("\"accepted\":true") != std::string::npos,
        "the reservation preceding start must succeed");

    const auto started = SendRequest(
        pipe_name, StartEnvelope(transaction_id, "pipe-contract-start-1"));
    Check(started.has_value(), "start-reserved-pair must be delivered over the real host");
    if (started) {
        CheckContains(*started, "\"success\":false",
            "a start without an injected backend must fail");
        CheckContains(*started, "\"resultCode\":\"PairDispatcherUnavailable\"",
            "a start with no injected backend must be typed PairDispatcherUnavailable");
        CheckContains(*started, "\"dispatchStarted\":false",
            "a PairDispatcherUnavailable start must report dispatchStarted:false");
    }

    // A second attempt exercises the exact same code path again: the
    // preflight is not skipped just because a prior attempt already failed.
    const auto started_again = SendRequest(
        pipe_name, StartEnvelope(transaction_id, "pipe-contract-start-2"));
    Check(started_again.has_value() &&
              started_again->find("\"resultCode\":\"PairDispatcherUnavailable\"") !=
                  std::string::npos,
        "a repeated start attempt must remain PairDispatcherUnavailable");

    const auto capture_only = SendRequest(
        pipe_name, CaptureRecoveryOnlyEnvelope(
            transaction_id, "pipe-contract-capture-recovery-only"));
    Check(capture_only.has_value(),
        "CaptureRecoveryOnly start must be delivered over the real host");
    if (capture_only) {
        CheckContains(*capture_only, "\"resultCode\":\"PairDispatcherUnavailable\"",
            "CaptureRecoveryOnly must reach the same unavailable backend boundary");
        CheckContains(*capture_only, "\"dispatchStarted\":false",
            "unavailable CaptureRecoveryOnly must not start dispatch");
    }

    // This query must happen here, while the host is still up -- see the
    // function-level comment above. It also completes the coverage that a
    // failed-closed start leaves the transaction Reserved rather than
    // silently terminal.
    const auto queried = SendRequest(
        pipe_name, QueryEnvelope(transaction_id, "pipe-contract-query-after-start"));
    Check(queried.has_value(), "the post-start query must be delivered");
    if (queried) {
        CheckContains(*queried, "\"resultCode\":\"PairTransactionReserved\"",
            "a failed-closed start must leave the transaction Reserved, not silently terminal");
    }

    lifetime_ticks.store(5500);
    // One already-pending accept may still take 5s. Join before reading counters.
    JoinPersistentHost(server);

    const auto counters = dispatcher.SafetyCounters();
    Check(counters.pair_dispatch_count == 0,
        "PairDispatcherUnavailable must never call BeginDispatch "
        "(store-before-side-effect order is preserved)");
    Check(counters.camera_access_count == 0,
        "no camera/SDK/WPD access can occur without an injected backend");
    Check(counters.automatic_retry_count == 0,
        "a failed-closed start must never be retried automatically");
}

// ---------------------------------------------------------------------
// The full "response unknown" resilience contract: a reservation is
// dispatched and durably persisted, but its response never reaches the
// client (injected body-write failure; failure teardown deliberately performs
// no second unbounded flush, so a partially written header is not promised). Two
// further host instances against the *same* --pair-journal-root (modelling
// a process restart) prove: the same-ID query recovers the durable state,
// and a replayed reserve-pair-transaction is rejected rather than replayed.
// ---------------------------------------------------------------------
void TestResponseUnknownRecoveryHasZeroReplayAcrossHostRestarts() {
    const fs::path root = MakeTempRoot("response-unknown");
    const fs::path journal_root = root / "journal";
    const std::string pipe_name = PipeNameFor("response-unknown");
    const std::string transaction_id = "30303030303030303030303030303030";

    {
        auto store = std::make_shared<DualHardwarePairJournalStore>(journal_root);
        DualHardwareCameraAgentDispatcher dispatcher(store);
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(
                pipe_name, dispatcher, /*serve_once=*/true,
                DualHardwareCameraAgentPipeFailureInjectionForTesting{
                    .fail_response_body_write = true});
        });

        HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "response-unknown client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const std::string request = ReservationEnvelope(transaction_id);
            const auto header = LengthHeader(static_cast<std::uint32_t>(request.size()));
            Check(WriteAll(pipe, header.data(), header.size()) &&
                    WriteAll(pipe, request.data(), request.size()),
                "the response-unknown reservation request must be writable");
            std::array<unsigned char, 4> response_header{};
            const bool header_received =
                ReadAll(pipe, response_header.data(), response_header.size());
            if (header_received) {
                const std::uint32_t response_length = ParseLengthHeader(response_header);
                std::string response(response_length, '\0');
                Check(!ReadAll(pipe, response.data(), response.size()),
                    "the injected body-write failure must prevent the response body from "
                    "ever being delivered, so the client cannot trust it");
            }
            CloseHandle(pipe);
        }

        Check(server.wait_for(std::chrono::seconds(8)) == std::future_status::ready,
            "the response-unknown server must terminate");
        Check(server.get() == 3,
            "a dispatched-but-undelivered response must exit with the typed "
            "delivery-failure code (kDispatchedDeliveryFailureExitCode)");
        const auto counters = dispatcher.SafetyCounters();
        Check(counters.pair_dispatch_count == 0,
            "a reservation alone must never touch pair_dispatch_count");
    }

    // A fresh dispatcher/host pair against the same --pair-journal-root
    // models a process restart. Only the same-ID query can recover state.
    {
        auto store = std::make_shared<DualHardwarePairJournalStore>(journal_root);
        DualHardwareCameraAgentDispatcher dispatcher(store);
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });
        const auto queried = SendRequest(
            pipe_name, QueryEnvelope(transaction_id, "pipe-contract-query-recovery"));
        Check(server.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "the recovery-query server must terminate");
        Check(server.get() == 0, "a successfully delivered query must exit 0");
        Check(queried.has_value(), "the recovery query must be delivered");
        if (queried) {
            CheckContains(*queried, "\"resultCode\":\"PairTransactionReserved\"",
                "a same-ID query after an unknown response must recover the durable "
                "reservation that the failed delivery could not confirm to the client");
            CheckContains(*queried, "\"found\":true",
                "the recovered reservation must report found:true");
        }
    }

    // A third host instance proves reserve replay is zero: replaying the
    // same reservation must be rejected, never silently re-accepted.
    {
        auto store = std::make_shared<DualHardwarePairJournalStore>(journal_root);
        DualHardwareCameraAgentDispatcher dispatcher(store);
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true);
        });
        const auto replay = SendRequest(
            pipe_name, ReservationEnvelope(transaction_id, "pipe-contract-replay"));
        Check(server.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "the replay-attempt server must terminate");
        Check(server.get() == 0, "a successfully delivered rejection must exit 0");
        Check(replay.has_value(), "the replay attempt must be delivered");
        if (replay) {
            CheckContains(*replay, "\"accepted\":false",
                "reserve replay must be zero: a replayed reserve must never re-accept");
            CheckContains(*replay, "\"resultCode\":\"DuplicateTransactionId\"",
                "a replayed reserve for an already-active transaction must be typed "
                "DuplicateTransactionId");
        }
        const auto counters = dispatcher.SafetyCounters();
        Check(counters.pair_dispatch_count == 0,
            "a rejected replay must never dispatch a pair");
    }
}

void TestEarlyAckDoesNotProveResponseConsumption() {
    constexpr std::uint32_t maximum = 1024U * 1024U;
    for (const bool consume_response : {true, false}) {
        const auto pipe_name = PipeNameFor("early-ack");
        DualHardwareCameraAgentDispatcher dispatcher;
        std::atomic<bool> at_ack{false};
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, true,
                {.before_stage_for_testing = [&](std::string_view stage) {
                    if (stage == "ack") at_ack.store(true);
                }});
        });
        const HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "early-ACK client must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto request = CapabilitiesEnvelope("early-ack");
            const auto header = LengthHeader(static_cast<std::uint32_t>(request.size()));
            Check(WriteAll(pipe, header.data(), header.size()) &&
                  WriteAll(pipe, request.data(), request.size()) &&
                  WriteAll(pipe, &kDeliveryAcknowledgment, 1),
                "early ACK must be submitted without reading any response bytes");
            const auto observed_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!at_ack.load() && std::chrono::steady_clock::now() < observed_by) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Check(at_ack.load(), "server must reach response ACK processing");
            Check(server.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready,
                "an early ACK cannot complete delivery while the entire response remains unread");
            if (consume_response) {
                std::array<unsigned char, 4> response_header{};
                const bool header_read = ReadAll(pipe, response_header.data(), response_header.size());
                Check(header_read, "the response must survive until the client reads it");
                if (header_read) {
                    const auto length = ParseLengthHeader(response_header);
                    Check(length > 0 && length <= maximum,
                        "early-ACK response must retain valid framing");
                    if (length > 0 && length <= maximum) {
                        std::string body(length, '\0');
                        Check(ReadAll(pipe, body.data(), body.size()),
                            "only full response consumption can complete delivery");
                    }
                }
                // Keep the connection alive until the server observes the full read.
                if (server.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                    std::cerr << "FAIL: full-read delivery did not complete\n" << std::flush;
                    std::_Exit(1);
                }
            }
            CloseHandle(pipe);
        }
        if (server.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            std::cerr << "FAIL: early-ACK server did not stop after read/disconnect\n" << std::flush;
            std::_Exit(1);
        }
        Check(server.get() == (consume_response ? 0 : 3),
            "full read confirms delivery; an unread disconnect remains code 3");
        Check(dispatcher.SafetyCounters().camera_access_count == 0 &&
              dispatcher.SafetyCounters().pair_dispatch_count == 0 &&
              dispatcher.SafetyCounters().automatic_retry_count == 0,
            "early-ACK proof uses no camera, capture, or retry");
    }
}

void TestDeadlineAfterAcceptedConnection() {
    const std::string pipe_name = PipeNameFor("accepted-deadline");
    DualHardwareCameraAgentDispatcher dispatcher;
    std::atomic<std::uint64_t> ticks{1000};
    auto server = std::async(std::launch::async, [&] {
        return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, false,
            {.lifetime_ticks_for_testing = [&] { return ticks.load(); }},
            std::chrono::milliseconds(4500));
    });
    const HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
    Check(pipe != INVALID_HANDLE_VALUE, "deadline regression client must connect");
    if (pipe != INVALID_HANDLE_VALUE) {
        // The connection was admitted in time; the complete frame was not.
        ticks.store(5500);
        const auto request = CapabilitiesEnvelope("accepted-deadline");
        const auto header = LengthHeader(static_cast<std::uint32_t>(request.size()));
        (void)WriteAll(pipe, header.data(), header.size());
        (void)WriteAll(pipe, request.data(), request.size());
        unsigned char response{};
        DWORD read{};
        const bool delivered = ReadFile(pipe, &response, 1, &read, nullptr) && read != 0;
        Check(!delivered, "an expired accepted connection must not dispatch or return a response");
        CloseHandle(pipe);
    }
    ticks.store(5500);
    JoinPersistentHost(server);
}

void TestHostLifetimeArithmeticAndSharedOrigin() {
    std::uint64_t ticks = 1000;
    AgentHostLifetime lifetime(600000, [&] { return ticks; });
    Check(lifetime.RemainingMilliseconds() == 600000, "host starts with exactly 600 seconds");
    ticks = 600000;
    Check(lifetime.RemainingMilliseconds() == 1000, "t=599 seconds leaves only one second");
    ticks = 601000;
    Check(lifetime.RemainingMilliseconds() == 0, "the exact absolute boundary expires");
    ticks = 1000;
    Check(lifetime.RemainingMilliseconds() == 0, "an expired clock never replenishes the host");

    ticks = 1000;
    AgentHostLifetime backwards(600000, [&] { return ticks; });
    ticks = 2000;
    (void)backwards.RemainingMilliseconds();
    ticks = 1999;
    Check(backwards.RemainingMilliseconds() == 0, "clock rollback fails closed");
    ticks = std::numeric_limits<std::uint64_t>::max() - 100;
    AgentHostLifetime wrapped(600000, [&] { return ticks; });
    ticks = 25;
    Check(wrapped.RemainingMilliseconds() == 599874, "tick wrap does not overflow the deadline");
    AgentHostLifetime zero(0, [&] { return ticks; });
    Check(zero.RemainingMilliseconds() == 0, "a zero budget is already expired");
    AgentHostLifetime throwing(600000, []() -> std::uint64_t { throw 1; });
    Check(throwing.RemainingMilliseconds() == 0, "a failed clock cannot grant time");

    // A successor pipe cannot reset a process budget consumed by binding.
    ticks = 0;
    AgentHostLifetime shared(600000, [&] { return ticks; });
    ticks = 600000;
    for (int successor = 0; successor != 2; ++successor) {
        DualHardwareCameraAgentDispatcher dispatcher;
        int accepts = 0;
        const int result = RunDualHardwareCameraAgentNamedPipeServer(
            PipeNameFor("shared-expired"), dispatcher, false,
            {.before_stage_for_testing = [&](std::string_view) { ++accepts; }},
            std::nullopt, &shared);
        Check(result == 0 && accepts == 0,
            "a shared expired origin must reject successor hosts before pipe I/O");
    }
}

void TestEveryIoStageUsesRemainingHostBudget() {
    for (const std::string_view target : {"accept", "header", "body", "dispatch",
             "response-header", "response-body", "ack", "flush"}) {
        for (const bool expire : {false, true}) {
            if (!expire && (target == "dispatch" || target == "flush")) continue;
            const auto root = MakeTempRoot("stage-budget");
            auto store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
            DualHardwareCameraAgentDispatcher dispatcher(store);
            const auto pipe_name = PipeNameFor("stage-budget");
            const std::string id = "90909090909090909090909090909090";
            std::atomic<std::uint64_t> ticks{0};
            bool triggered = false;
            std::vector<std::pair<std::string, std::uint32_t>> waits;
            auto server = std::async(std::launch::async, [&] {
                return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, false,
                    {.lifetime_ticks_for_testing = [&] { return ticks.load(); },
                     .before_stage_for_testing = [&](std::string_view stage) {
                         if (!triggered && stage == target) {
                             triggered = true;
                             ticks.store(expire ? 600000 : 599990);
                         }
                     },
                     .wait_timeout_for_testing = [&](std::string_view stage, std::uint32_t ms) {
                         waits.emplace_back(stage, ms);
                     }});
            });
            if (!(target == "accept" && expire)) {
                (void)SendRequest(pipe_name, ReservationEnvelope(id, "stage-budget-reserve"));
            }
            if (!expire) ticks.store(600000);
            if (server.wait_for(std::chrono::seconds(8)) != std::future_status::ready) {
                std::cerr << "FAIL: stage-budget server failed to stop\n" << std::flush;
                std::_Exit(1);
            }
            const int result = server.get();
            const bool after_dispatch = target == "response-header" ||
                target == "response-body" || target == "ack" || target == "flush";
            Check(triggered, "every target I/O stage must be exercised");
            if (expire) {
                Check(result == (after_dispatch ? 3 : 0),
                    "expired delivery is unknown, while an undispatched frame remains undispatched");
                Check(std::none_of(waits.begin(), waits.end(), [&](const auto& wait) {
                    return wait.first == target;
                }), "zero remaining time must not submit I/O at the expired stage");
                Check(store->Query(id).has_value() == after_dispatch,
                    "only an already-dispatched reservation survives a delivery timeout");
            } else {
                bool observed = false;
                for (const auto& wait : waits) {
                    if (wait.first != target) continue;
                    observed = true;
                    Check(wait.second > 0 && wait.second <= 10,
                        "accept/read/write/ACK timeout must be clamped to the last 10ms");
                }
                Check(observed, "the clamped timeout must reach the actual I/O seam");
            }
            const auto counters = dispatcher.SafetyCounters();
            Check(counters.pair_dispatch_count == 0 && counters.camera_access_count == 0 &&
                    counters.automatic_retry_count == 0,
                "stage expiry never dispatches, accesses a camera, or retries");
        }
    }
}

class AdmissionProbeBackend final : public DualHardwarePairCaptureBackend {
public:
    int preflight_calls{};
    int capture_calls{};
    DualHardwarePairPreflightOutcome PreflightPair(bool, std::int64_t) override {
        ++preflight_calls;
        return {}; // Pending: this test never acquires hardware or writes images.
    }
    DualHardwareFakeCaptureOutcome Capture(std::string_view, const fs::path&, std::int64_t) override {
        ++capture_calls;
        return {};
    }
};

void TestRealPipeForwardsHostAdmissionAndPreservesQuery() {
    for (const bool recovery : {false, true}) {
        for (const std::uint64_t remaining : {1000ULL, 184999ULL, 185000ULL}) {
            const auto root = MakeTempRoot("admission");
            auto store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
            auto backend = std::make_shared<AdmissionProbeBackend>();
            DualHardwareCameraAgentDispatcher dispatcher(store, [] { return FixedNow(); }, backend);
            const std::string id = "81818181818181818181818181818181";
            (void)store->Reserve(id);
            std::atomic<std::uint64_t> ticks{0};
            bool advanced = false;
            const auto pipe_name = PipeNameFor("admission");
            auto server = std::async(std::launch::async, [&] {
                return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, false,
                    {.lifetime_ticks_for_testing = [&] { return ticks.load(); },
                     .before_stage_for_testing = [&](std::string_view stage) {
                         if (!advanced && stage == "dispatch") {
                             advanced = true;
                             ticks.store(600000 - remaining);
                         }
                     }});
            });
            const auto response = SendRequest(pipe_name, recovery
                ? CaptureRecoveryOnlyEnvelope(id, "admission-start")
                : StartEnvelope(id, "admission-start"));
            Check(response.has_value(), "host admission must return its fixed typed refusal");
            if (response) CheckContains(*response, recovery ? "ConfirmedUndispatched" : "PairDispatcherUnavailable",
                "normal and CR retain their existing typed no-dispatch schemas");
            if (remaining < 185000) {
                const auto new_id = SendRequest(pipe_name, ReservationEnvelope(
                    "92929292929292929292929292929292", "admission-old-host-new-id"));
                Check(new_id.has_value(), "old host must deliver the typed new-ID refusal");
                if (new_id) CheckContains(*new_id, "HostTerminalPendingReservationClose",
                    "a spent host must reject new IDs before same-ID query/close recovery");
            }
            const auto query = SendRequest(pipe_name, QueryEnvelope(id, "admission-query",
                recovery ? kDualHardwareCameraAgentCaptureRecoveryOnlySchemaVersion : kDualHardwareCameraAgentSchemaVersion));
            Check(query.has_value(), "same-ID query must remain available after no-dispatch");
            if (recovery || remaining >= 185000) {
                const auto close = SendRequest(pipe_name, CloseEnvelope(id, "admission-close",
                    recovery ? kDualHardwareCameraAgentCaptureRecoveryOnlySchemaVersion : kDualHardwareCameraAgentSchemaVersion));
                Check(close.has_value(), "CR and ordinary pending reservations retain explicit close");
            } else {
                if (query) CheckContains(*query, "PairTransactionClosedBeforeDispatch",
                    "ordinary recovery must deliver its tombstone before host exit");
                // Foundation does not issue close after a ClosedBeforeDispatch query.
                JoinPersistentHost(server);
            }
            ticks.store(600000);
            if (server.valid()) JoinPersistentHost(server);
            Check(backend->preflight_calls == (remaining >= 185000 ? 1 : 0),
                "host budget must be forwarded and re-evaluated after the complete frame");
            Check(backend->capture_calls == 0 && dispatcher.SafetyCounters().pair_dispatch_count == 0 &&
                    dispatcher.SafetyCounters().automatic_retry_count == 0,
                "admission refusals make no capture, dispatch, or retry");
            if (!recovery && remaining < 185000) {
                auto fresh_store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
                DualHardwareCameraAgentDispatcher fresh(fresh_store);
                const auto fresh_pipe = PipeNameFor("admission-fresh");
                auto fresh_server = std::async(std::launch::async, [&] {
                    return RunDualHardwareCameraAgentNamedPipeServer(fresh_pipe, fresh, true);
                });
                const auto reserved = SendRequest(fresh_pipe, ReservationEnvelope(
                    "92929292929292929292929292929292", "admission-fresh-new-id"));
                Check(reserved.has_value(), "fresh host new-ID reservation must be delivered");
                if (reserved) CheckContains(*reserved, "\"accepted\":true",
                    "only a fresh host can resume with a new transaction ID");
                JoinPersistentHost(fresh_server);
            }
        }
    }
}

void TestHostBudgetClosedQueryWithoutAckIsUnknown() {
    for (const bool disconnect : {false, true}) {
        const auto root = MakeTempRoot("closed-query-no-ack");
        auto store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
        auto backend = std::make_shared<AdmissionProbeBackend>();
        DualHardwareCameraAgentDispatcher dispatcher(store, [] { return FixedNow(); }, backend);
        const std::string id = "93939393939393939393939393939393";
        (void)store->Reserve(id);
        std::atomic<std::uint64_t> ticks{0};
        const auto pipe_name = PipeNameFor("closed-query-no-ack");
        auto server = std::async(std::launch::async, [&] {
            return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, false,
                {.lifetime_ticks_for_testing = [&] { return ticks.load(); },
                 .before_stage_for_testing = [&](std::string_view stage) {
                     if (stage == "dispatch") ticks.store(599000);
                 }});
        });
        const auto refused = SendRequest(pipe_name, StartEnvelope(id, "closed-query-start"));
        Check(refused.has_value(), "the no-dispatch refusal must be delivered first");
        const HANDLE pipe = ConnectClient(pipe_name, std::chrono::seconds(5));
        Check(pipe != INVALID_HANDLE_VALUE, "the same-ID query must connect");
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto request = QueryEnvelope(id, "closed-query-without-ack");
            const auto header = LengthHeader(static_cast<std::uint32_t>(request.size()));
            Check(WriteAll(pipe, header.data(), header.size()) &&
                  WriteAll(pipe, request.data(), request.size()), "same-ID query must be submitted");
            std::array<unsigned char, 4> response_header{};
            const bool header_read = ReadAll(pipe, response_header.data(), response_header.size());
            Check(header_read, "closed query must not exit before delivering its response");
            if (header_read) {
                const auto length = ParseLengthHeader(response_header);
                if (length > 0 && length <= 1024U * 1024U) {
                    std::string body(length, '\0');
                    Check(ReadAll(pipe, body.data(), body.size()), "closed query frame must be complete");
                    CheckContains(body, "PairTransactionClosedBeforeDispatch",
                        "the no-ACK query must still contain durable no-dispatch proof");
                } else {
                    Check(false, "closed query response length must be bounded");
                }
            }
            // Intentionally send no ACK: either leave the pipe connected or close it.
            if (disconnect) CloseHandle(pipe);
        }
        if (server.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
            std::cerr << "FAIL: no-ACK closed query exceeded its bounded wait\n" << std::flush;
            std::_Exit(1);
        }
        if (pipe != INVALID_HANDLE_VALUE && !disconnect) CloseHandle(pipe);
        Check(server.get() == 3, "a missing query ACK/disconnect must stay unknown, not success");
        auto recovered_store = std::make_shared<DualHardwarePairJournalStore>(root / "journal");
        const auto recovered = recovered_store->Query(id);
        Check(recovered && recovered->state == DualHardwarePairJournalState::closed_before_dispatch &&
              recovered->terminal_result_json.empty(),
            "ACK loss must preserve restart-queryable no-dispatch proof without a capture result");
        Check(backend->preflight_calls == 0 && backend->capture_calls == 0 &&
              dispatcher.SafetyCounters().pair_dispatch_count == 0 &&
              dispatcher.SafetyCounters().automatic_retry_count == 0,
            "closed-query recovery must not preflight, capture, dispatch, or retry");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--delivery-proof-contract") {
        TestEarlyAckDoesNotProveResponseConsumption();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--host-deadline-contract") {
        TestEarlyAckDoesNotProveResponseConsumption();
        TestDeadlineAfterAcceptedConnection();
        TestHostLifetimeArithmeticAndSharedOrigin();
        TestEveryIoStageUsesRemainingHostBudget();
        TestRealPipeForwardsHostAdmissionAndPreservesQuery();
        TestHostBudgetClosedQueryWithoutAckIsUnknown();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--persistent-contract") {
        TestPersistentMultiRequestCapabilitiesReserveDuplicateAndQuery();
        return failures == 0 ? 0 : 1;
    }
    TestDeliveryAcknowledgmentIsRequired();
    TestMaximumFrameBoundaryMatchesSingleContract();
    TestZeroLengthAndPartialFramesFailClosedWithoutDispatch();
    TestMalformedJsonBodyGetsTypedRejectionOverRealHost();
    TestPersistentMultiRequestCapabilitiesReserveDuplicateAndQuery();
    TestClientDisconnectBeforeFrameCompletesNeverDispatches();
    TestBackendUnavailableStartFailsClosedAfterFullPreflight();
    TestResponseUnknownRecoveryHasZeroReplayAcrossHostRestarts();
    TestEarlyAckDoesNotProveResponseConsumption();
    TestDeadlineAfterAcceptedConnection();
    TestHostLifetimeArithmeticAndSharedOrigin();
    TestEveryIoStageUsesRemainingHostBudget();
    TestRealPipeForwardsHostAdmissionAndPreservesQuery();
    TestHostBudgetClosedQueryWithoutAckIsUnknown();
    if (failures != 0) {
        std::cerr << failures << " Dual hardware Camera Agent pipe test(s) failed\n";
        return 1;
    }
    std::cout << "Dual hardware Camera Agent pipe contracts passed\n";
    return 0;
}
