#pragma once

#include "a0/phase0/phase0.hpp"

#include <cstddef>
#include <guiddef.h>
#include <memory>
#include <functional>
#include <cstdint>
#include <optional>
#include <vector>

namespace a0::phase0 {

// Derives a local-only WPD identity from the camera-reported serial property.
// PnP device instance IDs are excluded because they can follow a USB port.
[[nodiscard]] std::string DeriveWpdStableIdentity(std::string_view device_serial_utf8);

enum class WpdCommandTargetPolicy {
    functional,
    omit,
};

enum class WpdStatusAccess {
    read_only,
    read_write,
};

struct WpdCaptureTargetDiagnostic {
    std::string validation_state;
    std::string command_options_hresult;
    std::string option_value_hresult;
    std::size_t functional_object_count{};
    std::size_t valid_object_id_count{};
    std::size_t compatible_target_count{};
    bool valid_object_ids_option_present{};
    bool selected_target{};
};

// This is deliberately an aggregate only.  Individual vendor opcodes and
// vendor extension strings are never retained in Phase 0 evidence.
struct WpdVendorOpcodeCollectionSummary {
    bool available{};
    bool malformed{};
    std::size_t item_count{};
    std::size_t unique_count{};
    bool vendor_capture_9207_advertised{};
};

struct WpdVendorOpcodeDiagnostic {
    std::string validation_state{"not_probed"};
    std::string supported_commands_hresult{"not_queried"};
    std::string query_send_hresult{"not_sent"};
    std::string query_common_hresult{"not_read"};
    bool wpd_still_image_capture_command_advertised{};
    bool vendor_opcode_query_advertised{};
    bool read_only_command_sent{};
    bool vendor_opcode_collection_available{};
    std::size_t vendor_opcode_item_count{};
    std::size_t vendor_opcode_unique_count{};
    bool vendor_capture_9207_advertised{};
    // The WPD extension API only reports vendor opcodes.  It cannot establish
    // whether standard PTP opcode 0x100E is advertised.
    bool standard_opcode_100e_advertisement_available{};
    std::string standard_opcode_100e_advertisement_state{"unavailable_via_wpd_extension_api"};
    std::string requested_access{"read-only"};
    bool read_only_access{true};
    bool vendor_operation_executed{};
};

[[nodiscard]] WpdVendorOpcodeCollectionSummary SummarizeWpdVendorOpcodes(
    const std::vector<std::optional<std::uint32_t>>& opcodes);

// Folder and functional objects are WPD hierarchy. Every other object under
// the connected camera is payload and therefore makes a dedicated spool
// non-empty, regardless of its file extension or media type.
[[nodiscard]] bool WpdContentTypeCountsAsSpoolPayload(const GUID& content_type) noexcept;

// WPD reports both the device clock and object creation time as VT_DATE.
// Hybrid recovery fails closed when either value is unavailable or the object
// predates the stabilized pre-capture baseline.
[[nodiscard]] bool WpdObjectDateIsAttributable(
    std::optional<double> object_created,
    std::optional<double> baseline_device_time) noexcept;
[[nodiscard]] bool WpdDeviceClockAdvanced(
    std::optional<double> initial_device_time,
    std::optional<double> cutoff_device_time) noexcept;

// IStream::Read reports its byte count through an untrusted provider-owned
// out parameter. Reject over-reporting before the count is used as an iterator
// offset or persisted as canonical image evidence.
void ValidateWpdStreamReadLength(
    std::size_t reported_bytes,
    std::size_t requested_bytes);

// IEnumPortableDeviceObjectIDs::Next reports how many IDs it populated
// through an untrusted provider-owned out parameter. Reject over-reporting
// before the count is used to index the fixed-size, zero-initialized ID
// buffer that holds them.
void ValidateWpdEnumeratedObjectCount(
    std::string_view category,
    std::size_t reported_count,
    std::size_t buffer_capacity);

// A provider that claims to have populated a slot yet leaves the
// corresponding entry null would otherwise be read into
// std::wstring(nullptr) -- undefined behavior. Reject the batch before any
// entry in the reported range is constructed.
void ValidateWpdEnumeratedObjectId(std::string_view category, const wchar_t* id);

// Bounds how many objects a single content-tree walk may enumerate. A
// dedicated capture spool never legitimately holds more than a few thousand
// objects; the budget is set comfortably above realistic professional-card
// contents while remaining finite, so a runaway or hostile object tree
// cannot grow the scan without bound.
void ValidateWpdContentScanBudget(
    std::string_view category,
    std::size_t scanned_object_count,
    std::size_t maximum_object_count);

// Narrow read-only seam for the DualCamera WPD gate. Implementations may only
// enumerate D810s, inspect all card payload objects through GENERIC_READ, and
// close a retained read-only session during failure cleanup. SDK, capture,
// delete, vendor command, and settings-write capabilities are absent.
class IWpdDualReadOnlyProbeTransport {
public:
    virtual ~IWpdDualReadOnlyProbeTransport() = default;
    [[nodiscard]] virtual std::vector<CameraInfo>
        EnumerateForDualReadOnlyProbe() = 0;
    [[nodiscard]] virtual std::size_t InspectDualReadOnlySpoolPayloadCount(
        std::string_view stable_identity,
        std::chrono::seconds timeout) = 0;
    virtual void CloseDualReadOnlyProbeSession(
        std::chrono::seconds timeout) = 0;
    [[nodiscard]] virtual bool DualReadOnlyProbeSessionOpen() const noexcept = 0;
};

enum class DualWpdReadOnlyProbeError {
    None,
    CameraCountMismatch,
    AliasMapInvalid,
    AliasMatchMismatch,
    InventoryFailed,
    TopologyChanged,
    SpoolInspectionFailed,
    SessionCloseFailed,
    SessionCleanupUnconfirmed,
    SpoolNotEmpty,
    HostSetupFailed,
};

enum class DualWpdReadOnlyProbeCleanup {
    Ended,
    EndedAfterError,
    Unconfirmed,
};

enum class DualWpdReadOnlyProbeTerminalState {
    Pass,
    Blocked,
};

struct DualWpdReadOnlyProbeResult {
    std::optional<std::size_t> wpd_d810_count;
    std::size_t inventory_checks_completed{};
    std::size_t cam_a_match_count{};
    std::size_t cam_b_match_count{};
    std::size_t unbound_camera_count{};
    bool alias_identities_distinct{};
    std::optional<std::size_t> cam_a_payload_object_count;
    std::optional<std::size_t> cam_b_payload_object_count;
    bool cam_a_session_closed{};
    bool cam_b_session_closed{};
    std::size_t wpd_spool_sessions_closed{};
    bool topology_stable{};
    bool wpd_access_attempted{};
    bool wpd_session_open_at_exit{};
    DualWpdReadOnlyProbeError error{DualWpdReadOnlyProbeError::HostSetupFailed};
    DualWpdReadOnlyProbeCleanup cleanup{DualWpdReadOnlyProbeCleanup::Ended};
    DualWpdReadOnlyProbeTerminalState terminal_state{
        DualWpdReadOnlyProbeTerminalState::Blocked};
};

[[nodiscard]] DualWpdReadOnlyProbeResult RunDualWpdReadOnlyProbe(
    IWpdDualReadOnlyProbeTransport& transport,
    const IdentityMap& identity_map,
    std::chrono::seconds timeout);
[[nodiscard]] std::string SerializeDualWpdReadOnlyProbeResult(
    const DualWpdReadOnlyProbeResult& result);

class WpdTransport final : public ICameraTransport,
                           public IPostCardObservationTransport,
                           public ICorrelationObservationTransport,
                           public IWpdDualReadOnlyProbeTransport {
public:
    using BeforeCommandCallback = std::function<void()>;

    explicit WpdTransport(
        WpdCommandTargetPolicy command_target_policy = WpdCommandTargetPolicy::functional,
        BeforeCommandCallback before_command = {});
    ~WpdTransport() override;
    WpdTransport(const WpdTransport&) = delete;
    WpdTransport& operator=(const WpdTransport&) = delete;
    [[nodiscard]] std::string SdkVersion() const override;
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override;
    [[nodiscard]] std::vector<CameraInfo>
        EnumerateForDualReadOnlyProbe() override;
    // Product Camera Agent only: refreshes at each open boundary and rejects
    // unless exactly one D810 is present. Legacy pair experiments leave this
    // disabled.
    void RequireExactlyOneD810ForProductAgent();
    [[nodiscard]] WpdCaptureTargetDiagnostic ProbeCaptureTarget(std::string_view stable_identity);
    [[nodiscard]] WpdVendorOpcodeDiagnostic ProbeVendorOpcodes(
        std::string_view stable_identity,
        WpdStatusAccess access = WpdStatusAccess::read_only);
    // Opens one read-only WPD session, counts every non-structural payload
    // object, closes the session, and returns only the anonymous count.
    [[nodiscard]] std::size_t InspectSpoolPayloadCount(
        std::string_view stable_identity,
        std::chrono::seconds timeout);
    [[nodiscard]] std::size_t InspectDualReadOnlySpoolPayloadCount(
        std::string_view stable_identity,
        std::chrono::seconds timeout) override;
    void CloseDualReadOnlyProbeSession(
        std::chrono::seconds timeout) override;
    [[nodiscard]] bool DualReadOnlyProbeSessionOpen() const noexcept override;
    // Read-only session used only by wpd-correlation-status.
    void OpenReadOnlyObservation(std::string_view stable_identity, std::chrono::seconds timeout) override;
    [[nodiscard]] WpdCorrelationSample ReadCorrelationSample() override;
    void Open(std::string_view stable_identity, std::chrono::seconds timeout) override;
    [[nodiscard]] std::string Baseline(std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) override;
    [[nodiscard]] std::string BeginPostCardObservation(std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<ImageCandidate> ObserveAndDownloadPostCardCapture(
        std::string_view token,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) override;
    void DeleteRecoveredObject(
        std::string_view cleanup_token,
        std::chrono::seconds timeout) override;
    void VerifyJpegSpoolEmpty(std::chrono::seconds timeout) override;
    void AbandonPostCardObservation(std::string_view token) noexcept override;
    void Close(std::chrono::seconds timeout) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace a0::phase0
