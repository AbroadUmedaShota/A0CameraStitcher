#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::calibration {

// docs/schemas/calibration-observation-session.v1.schema.json
inline constexpr std::string_view kObservationSessionSchemaVersion =
    "a0.calibration-observation-session.v1";

// Resource limits. Each value is accepted and the next value up is rejected.
inline constexpr std::size_t kMaximumObservationSessionJsonBytes = 8U * 1024U * 1024U; // checked before parsing
// Containers that may hold a value, counting the root. A document nested one
// level deeper (with a non-empty innermost container) is a syntax failure.
inline constexpr int kMaximumObservationSessionJsonDepth = 32;
// Upper bound on the number of JSON values, counted on the text before it is
// parsed (see calibration_session_json_scan.hpp). The shared parser keeps one
// tree node per value, so this caps the memory of the tree whatever the byte
// count. 2^20 is the smallest power of two above the most a legal session can
// produce (an upper bound of 527,128, derived in docs/M2_CALIBRATION_OBSERVATION_SESSION.md).
inline constexpr std::size_t kMaximumObservationSessionJsonValues = 1U << 20;
inline constexpr std::size_t kMaximumChartPoints = 16384;
inline constexpr std::size_t kMaximumViewsPerCamera = 128;
inline constexpr std::size_t kMaximumObservationsPerView = 16384;
inline constexpr std::size_t kMaximumObservationsPerSession = 65536; // summed over all views
inline constexpr std::uint32_t kMinimumImageSidePixels = 1;
inline constexpr std::uint32_t kMaximumImageSidePixels = 32768;
inline constexpr std::size_t kMaximumIdentifierLength = 128; // minimum length is 1
// A fixed rule, not a caller policy: a view mapping needs four chart points
// of which no three are collinear.
inline constexpr std::size_t kMinimumMappingPoints = 4;

enum class CameraAlias { CamA, CamB };
enum class ViewRole { Training, Validation, Reference };
enum class ObservationState { Detected, Missing, Ambiguous };

// Out-of-range enum values return an empty view instead of a fabricated code.
constexpr std::string_view CameraAliasCode(CameraAlias camera) {
    switch (camera) {
    case CameraAlias::CamA: return "CAM-A";
    case CameraAlias::CamB: return "CAM-B";
    }
    return {};
}
constexpr std::string_view ViewRoleCode(ViewRole role) {
    switch (role) {
    case ViewRole::Training: return "training";
    case ViewRole::Validation: return "validation";
    case ViewRole::Reference: return "reference";
    }
    return {};
}
constexpr std::string_view ObservationStateCode(ObservationState state) {
    switch (state) {
    case ObservationState::Detected: return "detected";
    case ObservationState::Missing: return "missing";
    case ObservationState::Ambiguous: return "ambiguous";
    }
    return {};
}

struct ChartPoint {
    std::string id;
    double x_mm;
    double y_mm;
    friend bool operator==(const ChartPoint&, const ChartPoint&) = default;
};

// Self-reported conditions. Labels do not prove physical identity.
struct CameraGroup {
    std::uint32_t width_pixels;  // stored order
    std::uint32_t height_pixels; // stored order
    std::string lens_label;
    std::string focal_length_label;
    std::string focus_label;
    friend bool operator==(const CameraGroup&, const CameraGroup&) = default;
};

// Distorted stored-order pixel; an integer index is a pixel centre.
struct PixelPosition {
    double x;
    double y;
    friend bool operator==(const PixelPosition&, const PixelPosition&) = default;
};

struct Observation {
    std::string point_id;
    ObservationState state;
    std::optional<PixelPosition> pixel; // engaged if and only if state is Detected
    friend bool operator==(const Observation&, const Observation&) = default;
};

struct SessionView {
    std::string id;
    CameraAlias camera;
    std::string image_sha256;
    std::uint32_t width_pixels;
    std::uint32_t height_pixels;
    ViewRole role;
    std::vector<Observation> observations;          // input order
    std::vector<std::string> used_point_ids;        // training and reference only
    std::vector<std::string> pose_point_ids;        // validation only
    std::vector<std::string> measurement_point_ids; // validation only, never empty
    friend bool operator==(const SessionView&, const SessionView&) = default;
};

struct ToolIdentity {
    std::string id;
    std::string version;
    friend bool operator==(const ToolIdentity&, const ToolIdentity&) = default;
};

struct SessionProvenance {
    // Time at which the observations (detection results) were finalized.
    // It is not the capture time of any image.
    std::string measured_at_utc; // YYYY-MM-DDThh:mm:ssZ
    ToolIdentity tool;
    friend bool operator==(const SessionProvenance&, const SessionProvenance&) = default;
};

// Immutable, structurally validated observation session. The only way to
// obtain one is Parse; a value of this type never violates the schema rules.
// It carries no ground truth, lens model, numerical policy or verdict.
class CalibrationObservationSession final {
public:
    // Throws std::invalid_argument for any structural violation. The message is
    // "calibration observation session: <category> at <json-pointer>"; it never
    // contains input values.
    static CalibrationObservationSession Parse(std::string_view json);

    [[nodiscard]] const std::string& SessionId() const noexcept;
    [[nodiscard]] const std::string& ChartId() const noexcept;
    [[nodiscard]] const SessionProvenance& Provenance() const noexcept;
    [[nodiscard]] std::span<const ChartPoint> ChartPoints() const noexcept; // input order
    [[nodiscard]] const ChartPoint* FindChartPoint(std::string_view id) const noexcept; // nullptr if absent
    // Throws std::invalid_argument for an out-of-range enum value.
    [[nodiscard]] const CameraGroup& Camera(CameraAlias camera) const;
    [[nodiscard]] std::span<const SessionView> Views() const noexcept; // input order

private:
    struct Data;
    explicit CalibrationObservationSession(std::shared_ptr<const Data> data);
    std::shared_ptr<const Data> data_;
};

enum class SessionDiagnosticCode {
    InsufficientObservations,
    DegeneratePointConfiguration,
    AmbiguousCorrespondence,
    ReferenceViewMissing,
    ValidationViewMissing
};

constexpr std::string_view DiagnosticCode(SessionDiagnosticCode code) {
    switch (code) {
    case SessionDiagnosticCode::InsufficientObservations: return "insufficient-observations";
    case SessionDiagnosticCode::DegeneratePointConfiguration: return "degenerate-point-configuration";
    case SessionDiagnosticCode::AmbiguousCorrespondence: return "ambiguous-correspondence";
    case SessionDiagnosticCode::ReferenceViewMissing: return "reference-view-missing";
    case SessionDiagnosticCode::ValidationViewMissing: return "validation-view-missing";
    }
    return {};
}

struct SessionDiagnostic {
    SessionDiagnosticCode code;
    CameraAlias camera;
    std::optional<std::string> view_id; // null for camera-level codes
    friend bool operator==(const SessionDiagnostic&, const SessionDiagnostic&) = default;
};

// Per-view and per-camera observations about the session, in a fixed order:
// camera-level codes for CAM-A then CAM-B (reference before validation), then
// views in input order (insufficient or degenerate, then ambiguous).
// Exact binary64 predicates, no tolerance. Never throws for a parsed session
// except std::bad_alloc. An empty result only means none of these five
// conditions was found; it is not a calibration readiness verdict.
[[nodiscard]] std::vector<SessionDiagnostic> DiagnoseObservationSession(
    const CalibrationObservationSession& session);

} // namespace a0::m2::calibration
