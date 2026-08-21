#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent_store.hpp"

#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace a0::phase0;
namespace fs = std::filesystem;

int failures = 0;

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

void CheckNotContains(
    std::string_view value,
    std::string_view unexpected,
    std::string_view message) {
    Check(value.find(unexpected) == std::string_view::npos, message);
}

std::string ReplaceOnce(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    if (position == std::string::npos) throw std::runtime_error("test mutation target was not found");
    value.replace(position, from.size(), to);
    return value;
}

class TempSandbox final {
public:
    TempSandbox() {
        static std::atomic<unsigned long long> sequence{};
        parent_ = fs::absolute(fs::temp_directory_path()).lexically_normal();
        if (parent_.filename().empty()) parent_ = parent_.parent_path();
        const auto timestamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        root_ = parent_ / ("a0-dual-agent-store-test-" +
            std::to_string(timestamp) + "-" + std::to_string(++sequence));
        if (root_.parent_path() != parent_ || root_.filename().empty()) {
            throw std::runtime_error("test sandbox path escaped temporary parent");
        }
        fs::create_directory(root_);
    }

    ~TempSandbox() {
        const fs::path normalized = fs::absolute(root_).lexically_normal();
        if (normalized.parent_path() != parent_ || normalized.filename().empty()) {
            return;
        }
        std::error_code cleanup_error;
        fs::remove_all(normalized, cleanup_error);
    }

    [[nodiscard]] fs::path Child(std::string_view name) const {
        return root_ / std::string(name);
    }

private:
    fs::path parent_;
    fs::path root_;
};

#pragma pack(push, 1)
struct MountPointReparseBufferHeader final {
    DWORD reparse_tag;
    WORD reparse_data_length;
    WORD reserved;
    WORD substitute_name_offset;
    WORD substitute_name_length;
    WORD print_name_offset;
    WORD print_name_length;
};
#pragma pack(pop)

class ScopedDirectoryJunction final {
public:
    ScopedDirectoryJunction(const fs::path& link, const fs::path& target) : link_(link) {
        if (!CreateDirectoryW(link_.c_str(), nullptr))
            throw std::runtime_error("junction fixture directory could not be created");
        const HANDLE handle = CreateFileW(link_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            (void)RemoveDirectoryW(link_.c_str());
            throw std::runtime_error("junction fixture directory could not be opened");
        }
        const std::wstring print_name = fs::absolute(target).lexically_normal().native();
        const std::wstring substitute_name = L"\\??\\" + print_name;
        const std::size_t substitute_bytes = substitute_name.size() * sizeof(wchar_t);
        const std::size_t print_bytes = print_name.size() * sizeof(wchar_t);
        const std::size_t path_bytes = substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
        const std::size_t reparse_data_length = 8U + path_bytes;
        std::vector<unsigned char> buffer(8U + reparse_data_length, 0U);
        auto* header = reinterpret_cast<MountPointReparseBufferHeader*>(buffer.data());
        header->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
        header->reparse_data_length = static_cast<WORD>(reparse_data_length);
        header->substitute_name_length = static_cast<WORD>(substitute_bytes);
        header->print_name_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
        header->print_name_length = static_cast<WORD>(print_bytes);
        auto* path_buffer = reinterpret_cast<wchar_t*>(buffer.data() + sizeof(MountPointReparseBufferHeader));
        CopyMemory(path_buffer, substitute_name.data(), substitute_bytes);
        auto* print_buffer = reinterpret_cast<wchar_t*>(buffer.data() + sizeof(MountPointReparseBufferHeader) +
            header->print_name_offset);
        CopyMemory(print_buffer, print_name.data(), print_bytes);
        DWORD bytes_returned = 0;
        const BOOL created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
            static_cast<DWORD>(buffer.size()), nullptr, 0, &bytes_returned, nullptr);
        (void)CloseHandle(handle);
        if (!created) {
            const auto error = GetLastError();
            (void)RemoveDirectoryW(link_.c_str());
            throw std::runtime_error("junction fixture could not be established, win32=" + std::to_string(error));
        }
        active_ = true;
    }
    ~ScopedDirectoryJunction() { if (active_) (void)RemoveDirectoryW(link_.c_str()); }
    ScopedDirectoryJunction(const ScopedDirectoryJunction&) = delete;
    ScopedDirectoryJunction& operator=(const ScopedDirectoryJunction&) = delete;
private:
    fs::path link_;
    bool active_{};
};

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("test file could not be opened");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) throw std::runtime_error("test file could not be written");
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("test file could not be read");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

DualHardwareCameraAgentDispatcher CreateOwnedDispatcher(
    const fs::path& root,
    std::weak_ptr<DualHardwarePairJournalStore>& observer) {
    auto external_store =
        std::make_shared<DualHardwarePairJournalStore>(root);
    observer = external_store;
    DualHardwareCameraAgentDispatcher dispatcher(external_store);
    external_store.reset();
    return dispatcher;
}

std::string Envelope(
    std::string_view operation,
    std::string_view payload,
    std::string_view request_id = "request-contract-1",
    std::string_view schema = "a0.camera-agent.hardware-dual.v2",
    std::string_view marker = "Hardware",
    std::string_view simulation = "false") {
    return "{\"schemaVersion\":\"" + std::string(schema) +
        "\",\"simulation\":" + std::string(simulation) +
        ",\"marker\":\"" + std::string(marker) +
        "\",\"requestId\":\"" + std::string(request_id) +
        "\",\"operation\":\"" + std::string(operation) +
        "\",\"payload\":" + std::string(payload) + "}";
}

std::string ReservationPayload(
    std::string_view transaction_id = "0123456789abcdef0123456789abcdef",
    std::string_view aliases = "[\"CAM-A\",\"CAM-B\"]") {
    return "{\"transactionId\":\"" + std::string(transaction_id) +
        "\",\"cameraMode\":\"DualCamera\",\"orderedRequiredAliases\":" +
        std::string(aliases) + "}";
}

std::string StartPayload(
    std::string_view transaction_id = "0123456789abcdef0123456789abcdef",
    std::string_view aliases = "[\"CAM-A\",\"CAM-B\"]",
    std::string_view transaction_directory = "C:/anonymous/pair") {
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
    return "{\"cameraMode\":\"DualCamera\",\"orderedRequiredAliases\":" +
        std::string(aliases) +
        ",\"transaction\":{\"transactionId\":\"" +
        std::string(transaction_id) +
        "\",\"transactionDirectory\":\"" + std::string(transaction_directory) + "\""
        ",\"identitySnapshot\":{\"status\":\"Ready\",\"reasonCode\":\"anonymous-test-ready\","
        "\"observedAtUtc\":\"2026-08-14T00:00:00Z\",\"expiresAtUtc\":\"2026-08-14T01:00:00+00:00\"}"
        ",\"captureProfileSnapshot\":{\"profileId\":\"anonymous-profile\",\"version\":\"1\","
        "\"schemaVersion\":\"a0.hardware-dual-capture-profile.v1\",\"status\":\"Approved\","
        "\"approvedAtUtc\":\"2026-08-13T00:00:00Z\",\"validUntilUtc\":\"2026-08-15T00:00:00Z\","
        "\"bodies\":[" + std::string(body_a) + "," + std::string(body_b) + "]}"
        ",\"rigProfileSnapshot\":{\"profileId\":\"anonymous-rig\",\"version\":\"1\","
        "\"status\":\"Approved\",\"schemaVersion\":\"1.1.0\",\"provenance\":\"anonymous-test\","
        "\"measuredAtUtc\":\"2026-08-12T00:00:00Z\",\"validUntilUtc\":\"2026-08-15T00:00:00Z\","
        "\"assessedAtUtc\":\"2026-08-13T00:00:00Z\",\"expectedInputWidth\":7360,"
        "\"expectedInputHeight\":4912,\"cameraBToCameraA\":[1,0,12,0,1,0,0,0,1],"
        "\"layout\":\"camera-a-left-camera-b-right\",\"crop\":[1,1,1,1],"
        "\"cameraAliases\":[\"CAM-A\",\"CAM-B\"]}"
        ",\"operatorConfirmations\":{\"identitySnapshotApproved\":true,\"captureProfileFrozen\":true,"
        "\"rigProfileFrozen\":true,\"liveViewStoppedAndClosed\":true,\"bothCardsConfirmedEmpty\":true}"
        ",\"startedAtUtc\":\"2026-08-14T00:00:00+00:00\""
        ",\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00+00:00\"}}";
}

std::chrono::system_clock::time_point FixedNow() {
    using namespace std::chrono;
    return sys_days{year{2026}/August/14} + minutes{1};
}

DualHardwareCameraAgentDispatcher CreateFixedDispatcher(
    std::shared_ptr<DualHardwarePairJournalStore> store = {}) {
    return DualHardwareCameraAgentDispatcher(
        std::move(store), [] { return FixedNow(); });
}

class RecordingFakePairBackend final : public DualHardwareFakePairCaptureBackend {
public:
    std::vector<DualHardwareFakeCaptureOutcome> outcomes;
    std::string throw_alias;
    std::vector<std::string> aliases;
    std::vector<fs::path> paths;
    std::vector<std::int64_t> deadlines;
    std::function<void(std::string_view, const fs::path&)> after_capture;

    DualHardwareFakeCaptureOutcome Capture(
        std::string_view alias,
        const fs::path& canonical_original_path,
        std::int64_t watchdog_deadline_100ns) override {
        aliases.emplace_back(alias);
        paths.push_back(canonical_original_path);
        deadlines.push_back(watchdog_deadline_100ns);
        if (alias == throw_alias) throw std::runtime_error("anonymous fake backend failure");
        const std::size_t index = aliases.size() - 1;
        const auto outcome = index < outcomes.size()
            ? outcomes[index]
            : DualHardwareFakeCaptureOutcome{true, true, true};
        if (outcome.succeeded) {
            fs::create_directories(canonical_original_path.parent_path());
            WriteText(canonical_original_path, "anonymous-fake-original");
        }
        if (after_capture) after_capture(alias, canonical_original_path);
        return outcome;
    }
};

void CheckRejected(
    DualHardwareCameraAgentDispatcher& dispatcher,
    const std::string& request,
    std::string_view result_code,
    std::string_view message) {
    const auto response = dispatcher.Handle(request);
    CheckContains(response, "\"success\":false", message);
    CheckContains(
        response,
        "\"resultCode\":\"" + std::string(result_code) + "\"",
        message);
}

void TestCapabilitiesAndRecognizedOperations() {
    auto dispatcher = CreateFixedDispatcher();

    const auto capabilities = dispatcher.Handle(Envelope(
        "get-dual-capabilities", "{\"cameraMode\":\"DualCamera\"}"));
    CheckContains(capabilities, "\"schemaVersion\":\"a0.camera-agent.hardware-dual.v2\"",
        "capabilities must use the Dual hardware v2 schema");
    CheckContains(capabilities, "\"simulation\":false",
        "capabilities must be hardware-only");
    CheckContains(capabilities, "\"marker\":\"Hardware\"",
        "capabilities must retain the Hardware marker");
    CheckContains(capabilities, "\"requestId\":\"request-contract-1\"",
        "capabilities must correlate the bounded request ID");
    CheckContains(capabilities, "\"success\":true",
        "capabilities must succeed without a camera backend");
    CheckContains(capabilities, "\"resultCode\":\"DualCapabilities\"",
        "capabilities must have a typed result");
    CheckContains(capabilities, "\"cameraMode\":\"DualCamera\"",
        "capabilities must identify DualCamera mode");
    CheckContains(capabilities, "\"protocolVersion\":2",
        "capabilities must advertise protocol version 2");
    CheckContains(capabilities, "\"orderedRequiredAliases\":[\"CAM-A\",\"CAM-B\"]",
        "capabilities must freeze the CAM-A then CAM-B alias order");
    CheckContains(capabilities,
        "\"supportedOperations\":[\"get-dual-capabilities\",\"reserve-pair-transaction\",\"start-reserved-pair\",\"get-pair-transaction-result\",\"close-reserved-pair-transaction\"]",
        "capabilities must advertise the five operations in stable order");
    CheckContains(capabilities, "\"pairJournalDurable\":true",
        "capabilities must require a durable pair journal");
    CheckContains(capabilities, "\"sameTransactionQueryOnly\":true",
        "capabilities must require query-only recovery for the same transaction");
    CheckContains(capabilities, "\"automaticRetryCount\":0",
        "capabilities must prohibit automatic retries");

    const auto reservation = dispatcher.Handle(Envelope(
        "reserve-pair-transaction", ReservationPayload()));
    CheckContains(reservation, "\"resultCode\":\"PairStoreUnavailable\"",
        "reservation must be recognized and fail typed while the store is absent");
    CheckContains(reservation,
        "\"payload\":{\"transactionId\":\"0123456789abcdef0123456789abcdef\",\"accepted\":false}",
        "reservation rejection must remain compatible with the application payload");

    const auto start = dispatcher.Handle(Envelope(
        "start-reserved-pair", StartPayload()));
    CheckContains(start, "\"resultCode\":\"PairDispatcherUnavailable\"",
        "start must be recognized and fail typed while dispatch is absent");

    const auto query = dispatcher.Handle(Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"0123456789abcdef0123456789abcdef\"}"));
    CheckContains(query, "\"resultCode\":\"PairStoreUnavailable\"",
        "query must be recognized and fail typed while the store is absent");
    CheckContains(query,
        "\"payload\":{\"transactionId\":\"0123456789abcdef0123456789abcdef\",\"found\":false,\"result\":null}",
        "query rejection must remain compatible with the application payload");

    const auto close = dispatcher.Handle(Envelope(
        "close-reserved-pair-transaction",
        "{\"transactionId\":\"0123456789abcdef0123456789abcdef\"}"));
    CheckContains(close, "\"resultCode\":\"PairStoreUnavailable\"",
        "close must be recognized and fail typed while the store is absent");
    CheckContains(close, "\"closedBeforeDispatch\":false",
        "unavailable close must never claim that the reservation was closed");

    const auto counters = dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0,
        "the contract skeleton must perform zero camera access");
    Check(counters.pair_dispatch_count == 0,
        "the contract skeleton must perform zero pair dispatches");
    Check(counters.automatic_retry_count == 0,
        "the contract skeleton must perform zero automatic retries");
}

void TestCloseReservedPairOperationIsExactAndDurable() {
    TempSandbox sandbox;
    const std::string transaction_id = "56565656565656565656565656565656";
    const std::string other_id = "67676767676767676767676767676767";
    const fs::path root = sandbox.Child("close-reserved-dispatcher");
    auto store = std::make_shared<DualHardwarePairJournalStore>(root);
    DualHardwareCameraAgentDispatcher dispatcher(store);

    const auto missing = dispatcher.Handle(Envelope(
        "close-reserved-pair-transaction",
        "{\"transactionId\":\"" + transaction_id + "\"}",
        "request-close-missing"));
    CheckContains(missing, "\"resultCode\":\"PairCloseRejected\"",
        "a missing reservation must not be forged closed");

    (void)dispatcher.Handle(Envelope(
        "reserve-pair-transaction", ReservationPayload(transaction_id),
        "request-close-reserve"));
    const auto wrong = dispatcher.Handle(Envelope(
        "close-reserved-pair-transaction",
        "{\"transactionId\":\"" + other_id + "\"}",
        "request-close-wrong"));
    CheckContains(wrong, "\"resultCode\":\"PairCloseRejected\"",
        "another transaction ID must not close the active reservation");

    const auto closed = dispatcher.Handle(Envelope(
        "close-reserved-pair-transaction",
        "{\"transactionId\":\"" + transaction_id + "\"}",
        "request-close-exact"));
    CheckContains(closed, "\"success\":true",
        "the exact Reserved transaction must close successfully");
    CheckContains(closed, "\"resultCode\":\"PairTransactionClosedBeforeDispatch\"",
        "the close response must carry the durable tombstone result code");
    CheckContains(closed, "\"closedBeforeDispatch\":true",
        "the close response must confirm the reread tombstone");
    Check(!fs::exists(root / "active"),
        "close response must not be emitted before active state is removed");

    DualHardwareCameraAgentDispatcher restarted(
        std::make_shared<DualHardwarePairJournalStore>(root));
    const auto queried = restarted.Handle(Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"" + transaction_id + "\"}",
        "request-query-closed"));
    CheckContains(queried, "\"success\":true",
        "restart query must confirm the close tombstone");
    CheckContains(queried, "\"resultCode\":\"PairTransactionClosedBeforeDispatch\"",
        "restart query must distinguish ClosedBeforeDispatch from capture terminal");
    CheckContains(queried, "\"found\":true,\"result\":null",
        "close tombstone query must never forge a capture result");

    const auto retried = restarted.Handle(Envelope(
        "close-reserved-pair-transaction",
        "{\"transactionId\":\"" + transaction_id + "\"}",
        "request-close-retry"));
    CheckContains(retried, "\"resultCode\":\"PairTransactionClosedBeforeDispatch\"",
        "same-ID close after a lost response must be idempotent");
    const auto counters = restarted.SafetyCounters();
    Check(counters.camera_access_count == 0 &&
          counters.pair_dispatch_count == 0 &&
          counters.automatic_retry_count == 0,
        "close/query recovery must have zero camera, dispatch, and retry effects");
}

void TestStrictEnvelopeAndPayloadValidation() {
    DualHardwareCameraAgentDispatcher dispatcher;
    const auto capabilities = Envelope(
        "get-dual-capabilities", "{\"cameraMode\":\"DualCamera\"}");

    CheckRejected(dispatcher, capabilities + " trailing", "MalformedEnvelope",
        "trailing JSON data must be rejected");
    CheckRejected(dispatcher,
        "{\"schemaVersion\":\"a0.camera-agent.hardware-dual.v2\","
        "\"schemaVersion\":\"a0.camera-agent.hardware-dual.v2\","
        "\"simulation\":false,\"marker\":\"Hardware\","
        "\"requestId\":\"duplicate\",\"operation\":\"get-dual-capabilities\","
        "\"payload\":{\"cameraMode\":\"DualCamera\"}}",
        "DuplicateField", "duplicate JSON fields must be rejected");
    CheckRejected(dispatcher,
        "{\"schemaVersion\":\"a0.camera-agent.hardware-dual.v2\","
        "\"simulation\":false,\"marker\":\"Hardware\","
        "\"requestId\":\"missing\",\"operation\":\"get-dual-capabilities\"}",
        "UnexpectedField", "missing envelope fields must be rejected");
    CheckRejected(dispatcher,
        capabilities.substr(0, capabilities.size() - 1) + ",\"extra\":true}",
        "UnexpectedField", "unknown envelope fields must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities",
            "{\"cameraMode\":\"DualCamera\",\"extra\":true}"),
        "UnexpectedField", "unknown payload fields must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", "{}"),
        "UnexpectedField", "missing payload fields must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", "{\"cameraMode\":false}"),
        "InvalidFieldType", "wrong payload field types must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", "{\"cameraMode\":\"DualCamera\"}",
            "request-type", "a0.camera-agent.hardware-dual.v2", "Hardware", "\"false\""),
        "InvalidFieldType", "wrong envelope field types must be rejected");

    std::string oversized = capabilities;
    oversized.insert(oversized.size() - 1, 256U * 1024U, ' ');
    CheckRejected(dispatcher, oversized, "MalformedEnvelope",
        "oversized JSON must be rejected before dispatch");

    std::string nested = "\"DualCamera\"";
    for (int index = 0; index < 33; ++index) {
        nested = "{\"level\":" + nested + "}";
    }
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", "{\"cameraMode\":" + nested + "}"),
        "MalformedEnvelope", "excessively deep JSON must be rejected before dispatch");
}

void TestProtocolIdentityAndSafeTokens() {
    DualHardwareCameraAgentDispatcher dispatcher;
    const std::string payload = "{\"cameraMode\":\"DualCamera\"}";

    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", payload, "request-schema",
            "a0.camera-agent.hardware.v2"),
        "DualHardwareProtocolRequired", "another schema must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", payload, "request-marker",
            "a0.camera-agent.hardware-dual.v2", "Simulation"),
        "DualHardwareProtocolRequired", "another marker must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", payload, "request-simulation",
            "a0.camera-agent.hardware-dual.v2", "Hardware", "true"),
        "DualHardwareProtocolRequired", "simulation requests must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", payload, "request/unsafe"),
        "InvalidRequestId", "unsafe request IDs must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-dual-capabilities", payload, std::string(129, 'a')),
        "InvalidRequestId", "overlong request IDs must be rejected");
    CheckRejected(dispatcher,
        Envelope("unsupported", payload),
        "UnsupportedOperation", "unknown operations must be rejected");
}

void TestTransactionAndAliasValidation() {
    DualHardwareCameraAgentDispatcher dispatcher;

    const auto empty_identity = ReplaceOnce(StartPayload(),
        "{\"status\":\"Ready\",\"reasonCode\":\"anonymous-test-ready\","
        "\"observedAtUtc\":\"2026-08-14T00:00:00Z\",\"expiresAtUtc\":\"2026-08-14T01:00:00+00:00\"}",
        "{}");
    CheckRejected(dispatcher,
        Envelope("start-reserved-pair", empty_identity),
        "InvalidPairRequest",
        "an empty identity snapshot must be rejected before pair dispatch");

    CheckRejected(dispatcher,
        Envelope("reserve-pair-transaction", ReservationPayload("abc")),
        "InvalidTransactionId", "short transaction IDs must be rejected");
    CheckRejected(dispatcher,
        Envelope("reserve-pair-transaction", ReservationPayload("00000000000000000000000000000000")),
        "InvalidTransactionId", "an empty Guid transaction ID must be rejected");
    CheckRejected(dispatcher,
        Envelope("reserve-pair-transaction",
            ReservationPayload("0123456789abcdef0123456789abcdeg")),
        "InvalidTransactionId", "non-hex transaction IDs must be rejected");
    CheckRejected(dispatcher,
        Envelope("reserve-pair-transaction",
            ReservationPayload("0123456789abcdef0123456789abcdef", "[\"CAM-B\",\"CAM-A\"]")),
        "InvalidAliasOrder", "reversed aliases must be rejected");
    CheckRejected(dispatcher,
        Envelope("reserve-pair-transaction",
            ReservationPayload("0123456789abcdef0123456789abcdef", "[\"CAM-A\"]")),
        "InvalidAliasOrder", "missing aliases must be rejected");
    CheckRejected(dispatcher,
        Envelope("start-reserved-pair",
            StartPayload("0123456789abcdef0123456789abcdef", "[\"CAM-A\",\"CAM-C\"]")),
        "InvalidAliasOrder", "unknown aliases must be rejected");
    CheckRejected(dispatcher,
        Envelope("get-pair-transaction-result",
            "{\"transactionId\":32}"),
        "InvalidFieldType", "transaction IDs with a wrong JSON type must be rejected");
}

void TestPairStartSemanticValidation() {
    std::size_t clock_reads = 0;
    DualHardwareCameraAgentDispatcher dispatcher({}, [&] {
        ++clock_reads;
        return FixedNow();
    });
    const auto valid = dispatcher.Handle(Envelope("start-reserved-pair", StartPayload(), "request-valid-start"));
    CheckContains(valid, "\"resultCode\":\"PairDispatcherUnavailable\"",
        "a complete safe request must reach only the disabled dispatcher boundary");
    Check(clock_reads == 1, "a start request must read its injected UTC clock exactly once");
    auto fractional_payload = ReplaceOnce(StartPayload(),
        "\"startedAtUtc\":\"2026-08-14T00:00:00+00:00\"",
        "\"startedAtUtc\":\"2026-08-14T00:00:00.0000001Z\"");
    fractional_payload = ReplaceOnce(std::move(fractional_payload),
        "\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00+00:00\"",
        "\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00.0000001Z\"");
    const auto fractional = dispatcher.Handle(Envelope(
        "start-reserved-pair", fractional_payload, "request-fractional-start"));
    CheckContains(fractional, "\"resultCode\":\"PairDispatcherUnavailable\"",
        "zero-offset RFC3339 timestamps with seven fractional digits must be accepted");

    const auto top_bottom = dispatcher.Handle(Envelope("start-reserved-pair",
        ReplaceOnce(StartPayload(),
            "\"layout\":\"camera-a-left-camera-b-right\"",
            "\"layout\":\"camera-a-top-camera-b-bottom\""),
        "request-top-bottom-start"));
    CheckContains(top_bottom, "\"resultCode\":\"PairDispatcherUnavailable\"",
        "the approved top-bottom rig layout must reach the disabled dispatcher boundary");
    const auto top_bottom_counters = dispatcher.SafetyCounters();
    Check(top_bottom_counters.camera_access_count == 0 &&
          top_bottom_counters.pair_dispatch_count == 0 &&
          top_bottom_counters.automatic_retry_count == 0,
        "top-bottom validation must perform zero camera, dispatch, or retry effects");

    const std::vector<std::pair<std::string, std::string>> mutations = {
        {"\"status\":\"Ready\"", "\"status\":\"Expired\""},
        {"\"reasonCode\":\"anonymous-test-ready\"", "\"reasonCode\":\"   \""},
        {"\"identitySnapshotApproved\":true", "\"identitySnapshotApproved\":false"},
        {"\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00+00:00\"",
            "\"watchdogDeadlineUtc\":\"2026-08-14T00:02:59+00:00\""},
        {"\"startedAtUtc\":\"2026-08-14T00:00:00+00:00\"",
            "\"startedAtUtc\":\"2026-08-14T00:00:00+01:00\""},
        {"\"startedAtUtc\":\"2026-08-14T00:00:00+00:00\"",
            "\"startedAtUtc\":\"2026-08-14T00:00:00.00000001Z\""},
        {"\"imageArea\":\"FX\"", "\"imageArea\":\"DX\""},
        {"\"cameraBToCameraA\":[1,0,12,0,1,0,0,0,1]",
            "\"cameraBToCameraA\":[1,0,0,0,0,0,0,0,1]"},
        {"\"layout\":\"camera-a-left-camera-b-right\"",
            "\"layout\":\"camera-b-left-camera-a-right\""},
        {"\"bothCardsConfirmedEmpty\":true", "\"bothCardsConfirmedEmpty\":false"},
    };
    for (const auto& [from, to] : mutations) {
        const auto response = dispatcher.Handle(Envelope(
            "start-reserved-pair", ReplaceOnce(StartPayload(), from, to), "request-invalid-start"));
        CheckContains(response, "\"resultCode\":\"InvalidPairRequest\"",
            "every malformed or unsafe frozen snapshot must fail closed");
        CheckNotContains(response, "anonymous-test", "semantic rejection must not echo snapshot details");
    }

    for (const auto& unsafe_path : {"relative/pair", "C:/safe/../pair", "C:/safe/file:stream"}) {
        const auto response = dispatcher.Handle(Envelope(
            "start-reserved-pair", StartPayload("0123456789abcdef0123456789abcdef",
                "[\"CAM-A\",\"CAM-B\"]", unsafe_path), "request-unsafe-path"));
        CheckContains(response, "\"resultCode\":\"InvalidPairRequest\"",
            "unsafe transaction paths must fail closed");
        CheckNotContains(response, unsafe_path, "path rejection must not echo a local path");
    }
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0 && counters.pair_dispatch_count == 0 &&
          counters.automatic_retry_count == 0,
        "semantic validation must retain all zero-side-effect counters");
}

void TestPairStartBoundaryAndPoisonStoreIsolation() {
    TempSandbox sandbox;
    const fs::path root = sandbox.Child("poison-store");
    const fs::path journal = root / "active" / "pair-journal.json";
    fs::create_directories(journal.parent_path());
    WriteText(journal, "{\"malformed\":true}");
    const auto original = ReadText(journal);
    auto store = std::make_shared<DualHardwarePairJournalStore>(root);
    auto dispatcher = CreateFixedDispatcher(store);

    const auto accepted_boundary = dispatcher.Handle(Envelope(
        "start-reserved-pair", StartPayload(), "request-boundary"));
    CheckContains(accepted_boundary, "\"resultCode\":\"PairDispatcherUnavailable\"",
        "now inside the exact 180-second interval must reach the disabled dispatcher boundary");
    const auto expired = dispatcher.Handle(Envelope("start-reserved-pair",
        ReplaceOnce(StartPayload(), "\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00+00:00\"",
            "\"watchdogDeadlineUtc\":\"2026-08-14T00:01:00+00:00\""), "request-expired"));
    CheckContains(expired, "\"resultCode\":\"InvalidPairRequest\"",
        "now equal to the watchdog deadline must be rejected");
    DualHardwareCameraAgentDispatcher exact_deadline({}, [] {
        using namespace std::chrono;
        return sys_days{year{2026}/August/14} + minutes{3};
    });
    const auto exact_deadline_response = exact_deadline.Handle(Envelope(
        "start-reserved-pair", StartPayload(), "request-exact-deadline"));
    CheckContains(exact_deadline_response, "\"resultCode\":\"InvalidPairRequest\"",
        "the exact 180-second deadline boundary must be rejected");
    Check(ReadText(journal) == original,
        "start validation must not read through or rewrite a poisoned durable store");
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0 && counters.pair_dispatch_count == 0 &&
          counters.automatic_retry_count == 0,
        "boundary and poison-store checks must have zero camera/dispatch/retry effects");

    const fs::path existing_file = sandbox.Child("not-a-directory");
    WriteText(existing_file, "unchanged");
    const auto file_path = dispatcher.Handle(Envelope("start-reserved-pair",
        StartPayload("0123456789abcdef0123456789abcdef", "[\"CAM-A\",\"CAM-B\"]",
            (existing_file / "child").generic_string()), "request-file-path"));
    CheckContains(file_path, "\"resultCode\":\"InvalidPairRequest\"",
        "an existing file in the transaction directory chain must be rejected");
    Check(ReadText(existing_file) == "unchanged",
        "file-path rejection must not alter the existing file");
}

void TestPairStartRejectsJunctionWithoutFollowingIt() {
    TempSandbox sandbox;
    const fs::path outside = sandbox.Child("outside-target");
    fs::create_directories(outside);
    WriteText(outside / "sentinel.txt", "unchanged");
    const fs::path junction = sandbox.Child("transaction-junction");
    ScopedDirectoryJunction fixture(junction, outside);
    auto dispatcher = CreateFixedDispatcher();
    const auto response = dispatcher.Handle(Envelope("start-reserved-pair",
        StartPayload("0123456789abcdef0123456789abcdef", "[\"CAM-A\",\"CAM-B\"]",
            junction.generic_string()), "request-junction"));
    CheckContains(response, "\"resultCode\":\"InvalidPairRequest\"",
        "a transaction directory containing a junction must fail closed");
    CheckNotContains(response, junction.filename().string(),
        "junction rejection must not reveal the local path");
    Check(ReadText(outside / "sentinel.txt") == "unchanged",
        "junction rejection must not follow or alter the outside target");
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0 && counters.pair_dispatch_count == 0 &&
          counters.automatic_retry_count == 0,
        "junction rejection must have zero camera/dispatch/retry effects");
}

void TestInjectedPairStoreReservationAndRestartQuery() {
    TempSandbox sandbox;
    const std::string transaction_id = "33333333333333333333333333333333";
    const std::string other_id = "44444444444444444444444444444444";
    const fs::path root = sandbox.Child("durable-store");
    const fs::path journal = root / "active" / "pair-journal.json";

    auto store = std::make_shared<DualHardwarePairJournalStore>(root);
    DualHardwareCameraAgentDispatcher dispatcher(store);
    const auto reserved = dispatcher.Handle(Envelope(
        "reserve-pair-transaction", ReservationPayload(transaction_id),
        "request-reserve"));
    CheckContains(reserved, "\"success\":true",
        "an injected store must accept the first reservation");
    CheckContains(reserved, "\"resultCode\":\"PairTransactionReserved\"",
        "a successful reservation must return the typed reserved result");
    CheckContains(reserved,
        "\"payload\":{\"transactionId\":\"" + transaction_id +
            "\",\"accepted\":true}",
        "a successful reservation must acknowledge only the requested ID");
    Check(fs::is_regular_file(journal),
        "an accepted reservation must be durably journaled");
    const std::string original_journal = ReadText(journal);

    const auto duplicate = dispatcher.Handle(Envelope(
        "reserve-pair-transaction", ReservationPayload(transaction_id),
        "request-duplicate"));
    CheckContains(duplicate, "\"success\":false",
        "the same reservation must be rejected");
    CheckContains(duplicate, "\"resultCode\":\"DuplicateTransactionId\"",
        "the same reservation must have the typed duplicate result");
    Check(ReadText(journal) == original_journal,
        "duplicate rejection must not rewrite the durable journal");

    const auto active = dispatcher.Handle(Envelope(
        "reserve-pair-transaction", ReservationPayload(other_id),
        "request-active"));
    CheckContains(active, "\"success\":false",
        "another active transaction must be rejected");
    CheckContains(active, "\"resultCode\":\"ActiveTransactionExists\"",
        "another active transaction must have a fixed typed result");
    CheckContains(active, "\"transactionId\":\"" + other_id + "\"",
        "active rejection must correlate only the requested ID");
    CheckNotContains(active, transaction_id,
        "active rejection must not leak the stored active transaction ID");
    Check(ReadText(journal) == original_journal,
        "active rejection must not rewrite the durable journal");

    auto restarted_store =
        std::make_shared<DualHardwarePairJournalStore>(root);
    DualHardwareCameraAgentDispatcher restarted(restarted_store);
    const auto found = restarted.Handle(Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"" + transaction_id + "\"}",
        "request-found"));
    CheckContains(found, "\"success\":false",
        "a Reserved query is non-terminal and must not claim success");
    CheckContains(found, "\"resultCode\":\"PairTransactionReserved\"",
        "a restarted dispatcher must recover the Reserved state");
    CheckContains(found,
        "\"payload\":{\"transactionId\":\"" + transaction_id +
            "\",\"found\":true,\"result\":null}",
        "a Reserved query must return found without forging a result");

    const auto other = restarted.Handle(Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"" + other_id + "\"}",
        "request-other"));
    CheckContains(other, "\"resultCode\":\"PairTransactionNotFound\"",
        "querying another ID must be typed not-found");
    CheckContains(other, "\"found\":false,\"result\":null",
        "another-ID query must not forge a result");
    CheckNotContains(other, transaction_id,
        "another-ID query must not leak the active transaction ID");
    Check(ReadText(journal) == original_journal,
        "queries must not mutate the durable journal");

    const fs::path empty_root = sandbox.Child("empty-store");
    auto empty_store =
        std::make_shared<DualHardwarePairJournalStore>(empty_root);
    DualHardwareCameraAgentDispatcher empty_dispatcher(empty_store);
    const auto empty = empty_dispatcher.Handle(Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"" + other_id + "\"}",
        "request-empty"));
    CheckContains(empty, "\"resultCode\":\"PairTransactionNotFound\"",
        "an empty injected store must return typed not-found");
    Check(!fs::exists(empty_root),
        "an empty-store query must perform zero store writes");

    const fs::path start_root = sandbox.Child("start-store");
    auto start_store =
        std::make_shared<DualHardwarePairJournalStore>(start_root);
    auto start_dispatcher = CreateFixedDispatcher(start_store);
    const auto start = start_dispatcher.Handle(Envelope(
        "start-reserved-pair", StartPayload(transaction_id), "request-start"));
    CheckContains(start, "\"resultCode\":\"PairDispatcherUnavailable\"",
        "start must remain unavailable even when a store is injected");
    Check(!fs::exists(start_root),
        "unavailable start must perform zero store reads or writes");

    for (const auto* checked : {
            &dispatcher, &restarted, &empty_dispatcher, &start_dispatcher}) {
        const auto counters = checked->SafetyCounters();
        Check(counters.camera_access_count == 0,
            "store-backed operations must perform zero camera access");
        Check(counters.pair_dispatch_count == 0,
            "store-backed operations must perform zero pair dispatches");
        Check(counters.automatic_retry_count == 0,
            "store-backed operations must perform zero automatic retries");
    }
}

void TestFakePairBackendSuccessAndRestartQuery() {
    TempSandbox sandbox;
    const std::string transaction_id = "77777777777777777777777777777777";
    const fs::path store_root = sandbox.Child("fake-success-store");
    const fs::path transaction_root = sandbox.Child("fake-success-transaction");
    auto store = std::make_shared<DualHardwarePairJournalStore>(store_root);
    (void)store->Reserve(transaction_id);
    auto backend = std::make_shared<RecordingFakePairBackend>();
    DualHardwareCameraAgentDispatcher dispatcher(
        store, [] { return FixedNow(); }, backend);

    const auto response = dispatcher.Handle(Envelope("start-reserved-pair",
        StartPayload(transaction_id, "[\"CAM-A\",\"CAM-B\"]",
            transaction_root.generic_string()), "request-fake-success"));
    CheckContains(response, "\"success\":true", "fake pair success must return a successful envelope");
    CheckContains(response, "\"resultCode\":\"PairDispatchAccepted\"",
        "fake pair success must return the .NET-compatible dispatch result code");
    CheckContains(response, "\"dispatchState\":\"Completed\"",
        "a durable terminal result must use Completed dispatch state");
    CheckContains(response, "\"terminalState\":\"Succeeded\"",
        "two successful fake captures must terminalize as Succeeded");
    CheckContains(response, "\"failureCode\":\"None\"",
        "a successful pair must use the None failure code");
    Check(backend->aliases == std::vector<std::string>{"CAM-A", "CAM-B"},
        "the fake orchestrator must invoke CAM-A then CAM-B exactly once");
    Check(backend->paths.size() == 2 &&
          backend->paths[0] == transaction_root / "CAM-A" / "original.jpg" &&
          backend->paths[1] == transaction_root / "CAM-B" / "original.jpg",
        "the orchestrator must own both canonical original paths");
    Check(backend->deadlines.size() == 2 && backend->deadlines[0] == backend->deadlines[1],
        "both fake captures must share the frozen watchdog deadline");
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0 && counters.pair_dispatch_count == 1 &&
          counters.automatic_retry_count == 0,
        "fake success must record one pair dispatch and no real camera access or retry");

    auto restarted_store = std::make_shared<DualHardwarePairJournalStore>(store_root);
    DualHardwareCameraAgentDispatcher restarted(restarted_store);
    const auto queried = restarted.Handle(Envelope("get-pair-transaction-result",
        "{\"transactionId\":\"" + transaction_id + "\"}", "request-restart-terminal"));
    CheckContains(queried, "\"success\":true", "restart query must recover a durable terminal result");
    CheckContains(queried, "\"resultCode\":\"PairTransactionFound\"",
        "restart query must use the .NET-compatible terminal query code");
    CheckContains(queried, "\"found\":true", "restart terminal query must be found");
    CheckContains(queried, "\"terminalState\":\"Succeeded\"",
        "restart query must return the same terminal state without recapture");
    Check(backend->aliases.size() == 2, "restart query must not invoke the backend again");
}

void TestFakePairBackendFailuresAndDeadlineAreNoRetry() {
    const auto run = [](std::string_view name,
                         std::vector<DualHardwareFakeCaptureOutcome> outcomes,
                         std::string throw_alias,
                         std::vector<std::chrono::system_clock::time_point> times) {
        auto sandbox = std::make_unique<TempSandbox>();
        const std::string transaction_id = name == "a-fail"
            ? "88888888888888888888888888888888"
            : name == "b-fail" ? "99999999999999999999999999999999"
            : name == "exception" ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            : "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        const fs::path store_root = sandbox->Child(std::string(name) + "-store");
        const fs::path transaction_root = sandbox->Child(std::string(name) + "-transaction");
        auto store = std::make_shared<DualHardwarePairJournalStore>(store_root);
        (void)store->Reserve(transaction_id);
        auto backend = std::make_shared<RecordingFakePairBackend>();
        backend->outcomes = std::move(outcomes);
        backend->throw_alias = std::move(throw_alias);
        auto time_index = std::make_shared<std::size_t>(0);
        DualHardwareCameraAgentDispatcher dispatcher(store, [times = std::move(times), time_index] {
            const auto index = (std::min)(*time_index, times.size() - 1);
            ++*time_index;
            return times[index];
        }, backend);
        const auto response = dispatcher.Handle(Envelope("start-reserved-pair",
            StartPayload(transaction_id, "[\"CAM-A\",\"CAM-B\"]", transaction_root.generic_string()),
            "request-fake-failure"));
        return std::tuple{std::move(sandbox), response, backend, transaction_root};
    };
    const std::vector normal_times(4, FixedNow());
    auto [a_sandbox, a_response, a_backend, a_root] = run(
        "a-fail", {{false, true, true}}, "", normal_times);
    CheckContains(a_response, "\"terminalState\":\"Failed\"", "CAM-A failure must be terminal Failed");
    CheckContains(a_response, "\"failureCode\":\"CaptureCameraA\"", "CAM-A failure must be typed");
    Check(a_backend->aliases == std::vector<std::string>{"CAM-A"},
        "CAM-A failure must prevent CAM-B and retry");

    auto [b_sandbox, b_response, b_backend, b_root] = run(
        "b-fail", {{true, true, true}, {false, true, true}}, "", normal_times);
    CheckContains(b_response, "\"terminalState\":\"FailedPartial\"", "CAM-B failure must be FailedPartial");
    CheckContains(b_response, "\"failureCode\":\"CaptureCameraB\"", "CAM-B failure must be typed");
    Check(b_backend->aliases == std::vector<std::string>{"CAM-A", "CAM-B"} &&
          fs::is_regular_file(b_root / "CAM-A" / "original.jpg"),
        "CAM-B failure must retain the CAM-A original without retry");

    auto [e_sandbox, e_response, e_backend, e_root] = run(
        "exception", {}, "CAM-A", normal_times);
    CheckContains(e_response, "\"terminalState\":\"Failed\"",
        "a fake backend exception must terminalize without escaping or retry");
    Check(e_backend->aliases == std::vector<std::string>{"CAM-A"},
        "a CAM-A exception must prevent CAM-B and retry");

    using namespace std::chrono;
    const auto deadline = sys_days{year{2026}/August/14} + minutes{3};
    auto [w_sandbox, w_response, w_backend, w_root] = run(
        "watchdog", {{true, true, true}}, "", {FixedNow(), FixedNow(), deadline});
    CheckContains(w_response, "\"terminalState\":\"WatchdogExpired\"",
        "deadline reached after CAM-A must terminalize as WatchdogExpired");
    CheckContains(w_response, "\"failureCode\":\"WatchdogExpired\"",
        "deadline failure code must match the .NET enum");
    Check(w_backend->aliases == std::vector<std::string>{"CAM-A"} &&
          fs::is_regular_file(w_root / "CAM-A" / "original.jpg"),
        "deadline after CAM-A must retain its original and prevent CAM-B");

    auto [pre_sandbox, pre_response, pre_backend, pre_root] = run(
        "before-a", {}, "", {FixedNow(), deadline});
    CheckContains(pre_response, "\"terminalState\":\"WatchdogExpired\"",
        "an exact deadline before CAM-A must expire without backend access");
    Check(pre_backend->aliases.empty(),
        "an exact deadline before CAM-A must invoke no backend");

    auto [fd_sandbox, fd_response, fd_backend, fd_root] = run(
        "failure-deadline", {{false, true, true}}, "",
        {FixedNow(), FixedNow(), deadline});
    CheckContains(fd_response, "\"terminalState\":\"WatchdogExpired\"",
        "deadline reached with a capture failure must take watchdog precedence");

    auto [spool_sandbox, spool_response, spool_backend, spool_root] = run(
        "spool-evidence", {{true, false, true}}, "", normal_times);
    CheckNotContains(spool_response, "\"terminalState\":\"Succeeded\"",
        "incomplete exact-delete evidence must never generate Succeeded");
    Check(spool_backend->aliases == std::vector<std::string>{"CAM-A"},
        "incomplete CAM-A spool evidence must prevent CAM-B");
}

void TestTerminalPublishFailureKeepsDispatchingAndDoesNotRedispatch() {
    TempSandbox sandbox;
    const std::string transaction_id = "cccccccccccccccccccccccccccccccc";
    const fs::path store_root = sandbox.Child("publish-failure-store");
    const fs::path transaction_root = sandbox.Child("publish-failure-transaction");
    auto store = std::make_shared<DualHardwarePairJournalStore>(store_root);
    (void)store->Reserve(transaction_id);
    auto backend = std::make_shared<RecordingFakePairBackend>();
    backend->outcomes = {{true, true, true}, {true, true, true}};
    backend->after_capture = [store_root, transaction_id](std::string_view, const fs::path&) {
        fs::create_directories(store_root / "terminal");
        WriteText(store_root / "terminal" / (transaction_id + ".json.partial"), "poison");
    };
    DualHardwareCameraAgentDispatcher dispatcher(store, [] { return FixedNow(); }, backend);
    const auto request = Envelope("start-reserved-pair",
        StartPayload(transaction_id, "[\"CAM-A\",\"CAM-B\"]",
            transaction_root.generic_string()), "request-publish-failure");
    const auto first = dispatcher.Handle(request);
    CheckContains(first, "\"resultCode\":\"PairStoreFailure\"",
        "terminal publish failure must not return Completed");
    CheckContains(first, "\"dispatchStarted\":true",
        "a failure after BeginDispatch must not claim dispatchStarted false");
    CheckContains(ReadText(store_root / "active" / "pair-journal.json"),
        "\"state\":\"Dispatching\"",
        "terminal publish failure must retain durable Dispatching");
    Check(fs::is_regular_file(transaction_root / "CAM-A" / "original.jpg"),
        "terminal publish failure must retain an already written artifact");
    const auto second = dispatcher.Handle(request);
    CheckContains(second, "\"resultCode\":\"PairStoreFailure\"",
        "a restart attempt with poisoned terminal publication must fail closed");
    Check(backend->aliases.size() == 2,
        "a restart after terminal publish failure must never redispatch");
}

void TestStoreFailureIsFixedAndRedacted() {
    TempSandbox sandbox;
    const std::string transaction_id = "55555555555555555555555555555555";
    const fs::path root = sandbox.Child("sensitive-store-path");
    const fs::path journal = root / "active" / "pair-journal.json";
    fs::create_directories(journal.parent_path());
    WriteText(journal, "{\"malformed\":true}");
    const std::string original_journal = ReadText(journal);

    auto store = std::make_shared<DualHardwarePairJournalStore>(root);
    DualHardwareCameraAgentDispatcher dispatcher(store);
    const auto response = dispatcher.Handle(Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"" + transaction_id + "\"}",
        "request-store-error"));
    CheckContains(response, "\"success\":false",
        "a store failure must fail closed");
    CheckContains(response, "\"resultCode\":\"PairStoreFailure\"",
        "all internal store failures must use one fixed public result code");
    CheckContains(response,
        "\"transactionId\":\"" + transaction_id +
            "\",\"found\":false,\"result\":null",
        "a store failure must preserve the safe query response shape");
    CheckNotContains(response, root.filename().string(),
        "a store failure must not leak a local path");
    CheckNotContains(response, "malformed or unsupported",
        "a store failure must not leak internal diagnostic detail");
    Check(ReadText(journal) == original_journal,
        "a store failure must not rewrite the invalid journal");

    const auto reserve_response = dispatcher.Handle(Envelope(
        "reserve-pair-transaction", ReservationPayload(transaction_id),
        "request-store-reserve-error"));
    CheckContains(reserve_response, "\"resultCode\":\"PairStoreFailure\"",
        "reservation store failures must use the same fixed public result code");
    CheckContains(reserve_response, "\"accepted\":false",
        "a reservation store failure must never be accepted");
    CheckNotContains(reserve_response, root.filename().string(),
        "a reservation store failure must not leak a local path");
    CheckNotContains(reserve_response, "malformed or unsupported",
        "a reservation store failure must not leak internal detail");
    Check(ReadText(journal) == original_journal,
        "a reservation store failure must not rewrite the invalid journal");
    const auto counters = dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0 &&
          counters.pair_dispatch_count == 0 &&
          counters.automatic_retry_count == 0,
        "a store failure must retain all zero-side-effect counters");
}

void TestDispatcherOwnsInjectedStoreLifetime() {
    TempSandbox sandbox;
    const fs::path root = sandbox.Child("owned-store");
    const std::string transaction_id = "66666666666666666666666666666666";
    std::weak_ptr<DualHardwarePairJournalStore> observer;

    {
        auto dispatcher = CreateOwnedDispatcher(root, observer);
        Check(!observer.expired(),
            "dispatcher must keep the injected store alive after factory exit");
        const auto reserved = dispatcher.Handle(Envelope(
            "reserve-pair-transaction", ReservationPayload(transaction_id),
            "request-owned-reserve"));
        CheckContains(reserved, "\"resultCode\":\"PairTransactionReserved\"",
            "an owned store must remain usable after external reset");
    }
    Check(observer.expired(),
        "the store must be released when its dispatcher is destroyed");

    {
        auto restarted = CreateOwnedDispatcher(root, observer);
        Check(!observer.expired(),
            "a restarted dispatcher must own its new store instance");
        const auto found = restarted.Handle(Envelope(
            "get-pair-transaction-result",
            "{\"transactionId\":\"" + transaction_id + "\"}",
            "request-owned-query"));
        CheckContains(found, "\"resultCode\":\"PairTransactionReserved\"",
            "an owned restarted store must recover the durable reservation");
        CheckContains(found, "\"found\":true,\"result\":null",
            "owned lifetime recovery must not forge a terminal result");
    }
    Check(observer.expired(),
        "the restarted store must be released with its dispatcher");

    DualHardwareCameraAgentDispatcher null_dispatcher(
        std::shared_ptr<DualHardwarePairJournalStore>{});
    const auto null_reserve = null_dispatcher.Handle(Envelope(
        "reserve-pair-transaction", ReservationPayload(transaction_id),
        "request-null-reserve"));
    CheckContains(null_reserve, "\"resultCode\":\"PairStoreUnavailable\"",
        "an explicitly null store must fail closed as unavailable");
    const auto null_query = null_dispatcher.Handle(Envelope(
        "get-pair-transaction-result",
        "{\"transactionId\":\"" + transaction_id + "\"}",
        "request-null-query"));
    CheckContains(null_query, "\"resultCode\":\"PairStoreUnavailable\"",
        "an explicitly null query must fail closed as unavailable");
    const auto counters = null_dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0 &&
          counters.pair_dispatch_count == 0 &&
          counters.automatic_retry_count == 0,
        "a null store must retain all zero-side-effect counters");
}

} // namespace

int main() {
    TestCapabilitiesAndRecognizedOperations();
    TestStrictEnvelopeAndPayloadValidation();
    TestProtocolIdentityAndSafeTokens();
    TestTransactionAndAliasValidation();
    TestPairStartSemanticValidation();
    TestPairStartBoundaryAndPoisonStoreIsolation();
    TestPairStartRejectsJunctionWithoutFollowingIt();
    TestInjectedPairStoreReservationAndRestartQuery();
    TestCloseReservedPairOperationIsExactAndDurable();
    TestFakePairBackendSuccessAndRestartQuery();
    TestFakePairBackendFailuresAndDeadlineAreNoRetry();
    TestTerminalPublishFailureKeepsDispatchingAndDoesNotRedispatch();
    TestStoreFailureIsFixedAndRedacted();
    TestDispatcherOwnsInjectedStoreLifetime();
    if (failures != 0) {
        std::cerr << failures << " Dual hardware Camera Agent test(s) failed\n";
        return 1;
    }
    std::cout << "Dual hardware Camera Agent contracts passed\n";
    return 0;
}
