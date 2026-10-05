#pragma once

#include "a0/common/protocol_json.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/preview_topology_diag.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace a0::phase0::experimental {

enum class PreviewWorkerReplyStatus { ok, failed, closed, quarantined };

struct PreviewWorkerCloseReceipt {
    bool live_view_off{};
    bool source_closed{};
    bool module_closed{};
    bool process_claim_released{};
    bool safe_to_exit{};

    [[nodiscard]] bool Complete() const noexcept {
        return live_view_off && source_closed && module_closed && process_claim_released && safe_to_exit;
    }
};

struct PreviewWorkerReply {
    PreviewWorkerReplyStatus status{};
    a0::common::protocol_json::JsonValue payload;
    std::string error_category;
    std::optional<PreviewWorkerCloseReceipt> close_receipt;
    // Worker-reported module topology counters ("diag"). Diagnostic only: the
    // parent journals them and never bases ACK, close, verdict or disarm on them.
    PreviewTopologyDiag topology;
};

// Validate the whole bound reply before the parent acknowledges delivery.
// A reported close receipt is diagnostic until the parent has independently
// observed an explicit close response and a clean exit from both workers.
// Envelope v2: exactly schema, epoch, workerPid, sequence, status, payload and
// diag, where diag has exactly the 18 kPreviewTopologyDiagFields keys with
// uint32 decimal values. Any deviation, including in diag alone, rejects the
// whole reply; nothing is accepted partially.
inline PreviewWorkerReply ParsePreviewWorkerReply(
    std::string_view wire, std::string_view epoch, std::uint32_t worker_pid,
    std::uint64_t sequence, std::string_view operation) {
    namespace json = a0::common::protocol_json;
    struct Invalid {
        [[noreturn]] static void Fail(std::string_view, std::string_view) {
            throw TransportError("worker_reply_invalid", "worker reply contract rejected");
        }
    };
    const auto require = [](bool condition) {
        if (!condition) Invalid::Fail("worker_reply_invalid", "worker reply contract rejected");
    };
    constexpr std::size_t maximum = 512U * 1024U + 4096;
    auto envelope = json::BasicJsonParser<Invalid, maximum>(wire).Parse();
    require(envelope.kind == json::JsonKind::object && envelope.object.size() == 7);
    const auto field = [&](const char* name, json::JsonKind kind) -> const json::JsonValue& {
        return json::RequireFieldWith<Invalid>(envelope, name, kind);
    };
    require(field("schema", json::JsonKind::string).string == kPreviewWorkerSchema &&
            field("epoch", json::JsonKind::string).string == epoch &&
            field("workerPid", json::JsonKind::number).string == std::to_string(worker_pid) &&
            field("sequence", json::JsonKind::number).string == std::to_string(sequence));
    const auto status = field("status", json::JsonKind::string).string;
    const auto payload_field = envelope.object.find("payload");
    const auto diag_field = envelope.object.find("diag");
    require(payload_field != envelope.object.end() && diag_field != envelope.object.end());
    const auto& payload = payload_field->second;
    const auto topology = ParsePreviewTopologyDiag<Invalid>(diag_field->second);
    const auto close_receipt = [&](const json::JsonValue& value) {
        require(value.kind == json::JsonKind::object && value.object.size() == 5);
        const auto flag = [&](const char* name) {
            return json::RequireFieldWith<Invalid>(value, name, json::JsonKind::boolean).boolean;
        };
        return PreviewWorkerCloseReceipt{
            flag("liveViewOff"), flag("sourceClosed"), flag("moduleClosed"),
            flag("processClaimReleased"), flag("safeToExit")};
    };
    PreviewWorkerReply result;
    if (status == "ok" && operation != "close") {
        result.status = PreviewWorkerReplyStatus::ok;
    } else if (status == "failed") {
        require(payload.kind == json::JsonKind::object && payload.object.size() == 2);
        const auto& category = json::RequireFieldWith<Invalid>(payload, "error", json::JsonKind::string).string;
        require(!category.empty() && category.size() <= 48 &&
            std::all_of(category.begin(), category.end(), [](unsigned char character) {
                return (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9') || character == '_';
            }));
        result.error_category = category;
        result.close_receipt = close_receipt(
            json::RequireFieldWith<Invalid>(payload, "close", json::JsonKind::object));
        result.status = PreviewWorkerReplyStatus::failed;
    } else if (status == "closed" && operation == "close") {
        result.close_receipt = close_receipt(payload);
        require(result.close_receipt->Complete());
        result.status = PreviewWorkerReplyStatus::closed;
    } else if (status == "quarantined" && operation == "close") {
        result.close_receipt = close_receipt(payload);
        result.status = PreviewWorkerReplyStatus::quarantined;
    } else {
        Invalid::Fail("worker_reply_invalid", "worker reply status rejected");
    }
    result.payload = std::move(payload);
    result.topology = topology;
    return result;
}

} // namespace a0::phase0::experimental
