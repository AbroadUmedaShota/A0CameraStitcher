#include "a0/phase0/dual_hardware_camera_agent.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace a0::phase0;

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

} // namespace

int main() {
    TestCapabilitiesAndRecognizedOperations();
    TestStrictEnvelopeAndPayloadValidation();
    TestProtocolIdentityAndSafeTokens();
    TestTransactionAndAliasValidation();
    if (failures != 0) {
        std::cerr << failures << " Dual hardware Camera Agent test(s) failed\n";
        return 1;
    }
    std::cout << "Dual hardware Camera Agent contracts passed\n";
    return 0;
}
