#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent_store.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
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
    std::string_view aliases = "[\"CAM-A\",\"CAM-B\"]") {
    return "{\"cameraMode\":\"DualCamera\",\"orderedRequiredAliases\":" +
        std::string(aliases) +
        ",\"transaction\":{\"transactionId\":\"" +
        std::string(transaction_id) +
        "\",\"transactionDirectory\":\"C:/anonymous/pair\""
        ",\"identitySnapshot\":{},\"captureProfileSnapshot\":{}"
        ",\"rigProfileSnapshot\":{},\"operatorConfirmations\":{}"
        ",\"startedAtUtc\":\"2026-08-14T00:00:00+00:00\""
        ",\"watchdogDeadlineUtc\":\"2026-08-14T00:03:00+00:00\"}}";
}

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
    DualHardwareCameraAgentDispatcher dispatcher;

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
        "\"supportedOperations\":[\"get-dual-capabilities\",\"reserve-pair-transaction\",\"start-reserved-pair\",\"get-pair-transaction-result\"]",
        "capabilities must advertise the four operations in stable order");
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

    const auto counters = dispatcher.SafetyCounters();
    Check(counters.camera_access_count == 0,
        "the contract skeleton must perform zero camera access");
    Check(counters.pair_dispatch_count == 0,
        "the contract skeleton must perform zero pair dispatches");
    Check(counters.automatic_retry_count == 0,
        "the contract skeleton must perform zero automatic retries");
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

    CheckRejected(dispatcher,
        Envelope("reserve-pair-transaction", ReservationPayload("abc")),
        "InvalidTransactionId", "short transaction IDs must be rejected");
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
    DualHardwareCameraAgentDispatcher start_dispatcher(start_store);
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
    TestInjectedPairStoreReservationAndRestartQuery();
    TestStoreFailureIsFixedAndRedacted();
    TestDispatcherOwnsInjectedStoreLifetime();
    if (failures != 0) {
        std::cerr << failures << " Dual hardware Camera Agent test(s) failed\n";
        return 1;
    }
    std::cout << "Dual hardware Camera Agent contracts passed\n";
    return 0;
}
