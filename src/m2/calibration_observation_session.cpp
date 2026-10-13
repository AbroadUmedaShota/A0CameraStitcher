#include "a0/m2/calibration_observation_session.hpp"

#include "a0/common/protocol_json.hpp"
#include "calibration_exact_predicates.hpp"
#include "calibration_session_json_scan.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

// Rejection categories are listed in docs/M2_CALIBRATION_OBSERVATION_SESSION.md.
// This translation unit depends on the STL and the shared JSON parser only.
namespace a0::m2::calibration {
namespace {

using a0::common::protocol_json::JsonKind;
using a0::common::protocol_json::JsonValue;

static_assert(kMaximumObservationSessionJsonDepth == a0::common::protocol_json::kMaximumProtocolJsonDepth);

// JSON Pointer built from known keys and array indices only, so that neither
// input values nor unknown key names reach an error message.
class Path {
public:
    Path() = default;
    Path(const Path& parent, std::string_view key) : parent_(&parent), key_(key) {}
    Path(const Path& parent, std::size_t index) : parent_(&parent), index_(index), is_index_(true) {}
    [[nodiscard]] std::string Render() const {
        std::string text;
        Append(text);
        return text.empty() ? std::string("/") : text;
    }

private:
    void Append(std::string& text) const {
        if (parent_ == nullptr) return;
        parent_->Append(text);
        text += '/';
        if (is_index_) text += std::to_string(index_);
        else text += key_;
    }
    const Path* parent_{};
    std::string_view key_{};
    std::size_t index_{};
    bool is_index_{};
};

[[noreturn]] void Reject(std::string_view category, const Path& path) {
    throw std::invalid_argument("calibration observation session: " + std::string(category) + " at " + path.Render());
}

struct SessionJsonFailure {
    [[noreturn]] static void Fail(std::string_view code, std::string_view) {
        Reject(code == "DuplicateField" ? "json-duplicate-key" : "json-syntax", Path());
    }
};

// Keys of the synthetic generator ground truth and of lens or projection
// models. An observation session carries none of them.
constexpr std::array<std::string_view, 10> kTruthKeys{
    "groundTruth", "ground_truth", "intrinsics", "distortion", "projection",
    "documentToImage", "image_px", "document_mm", "reference_points", "generator_version"};

// Keys of the schema. Only these are rendered in a pointer; any other key
// leaves the pointer at its parent.
constexpr std::array<std::string_view, 33> kSchemaKeys{
    "schemaVersion", "sessionId", "chartId", "provenance", "measuredAt", "tool", "toolId", "version",
    "chartPoints", "pointId", "xMm", "yMm", "cameras", "CAM-A", "CAM-B", "widthPixels", "heightPixels",
    "lensLabel", "focalLengthLabel", "focusLabel", "views", "viewId", "camera", "imageSha256", "role",
    "usedPointIds", "posePointIds", "measurementPointIds", "observations", "state", "pixel", "x", "y"};

void RejectTruthKeys(const JsonValue& value, const Path& path) {
    if (value.kind == JsonKind::object) {
        for (const auto& [key, child] : value.object) {
            const auto truth = std::find(kTruthKeys.begin(), kTruthKeys.end(), key);
            if (truth != kTruthKeys.end()) Reject("truth-field", Path(path, *truth));
            const auto known = std::find(kSchemaKeys.begin(), kSchemaKeys.end(), key);
            RejectTruthKeys(child, known != kSchemaKeys.end() ? Path(path, *known) : path);
        }
    } else if (value.kind == JsonKind::array) {
        for (std::size_t index = 0; index < value.array.size(); ++index) RejectTruthKeys(value.array[index], Path(path, index));
    }
}

// The object must hold exactly the listed keys.
void CheckShape(const JsonValue& value, const Path& path, std::initializer_list<std::string_view> required) {
    if (value.kind != JsonKind::object) Reject("type", path);
    for (const auto& entry : value.object) {
        if (std::find(required.begin(), required.end(), entry.first) == required.end()) Reject("unknown-field", path);
    }
    if (value.object.size() != required.size()) Reject("missing-field", path);
}

const JsonValue& Field(const JsonValue& object, std::string_view key) {
    return object.object.find(std::string(key))->second; // only used after CheckShape
}

const std::string& Text(const JsonValue& value, const Path& path) {
    if (value.kind != JsonKind::string) Reject("type", path);
    return value.string;
}

bool Alnum(const char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

const std::string& Identifier(const JsonValue& value, const Path& path, const bool allow_dot) {
    const auto& text = Text(value, path);
    if (text.empty() || text.size() > kMaximumIdentifierLength || !Alnum(text.front())) Reject("identifier", path);
    for (const char c : text) {
        if (!Alnum(c) && c != '_' && c != '-' && !(allow_dot && c == '.')) Reject("identifier", path);
    }
    return text;
}
const std::string& SafeId(const JsonValue& value, const Path& path) { return Identifier(value, path, false); }
const std::string& ToolIdentifier(const JsonValue& value, const Path& path) { return Identifier(value, path, true); }

int Digits(const std::string& value, const std::size_t begin, const std::size_t count, const Path& path) {
    int number = 0;
    for (std::size_t index = begin; index < begin + count; ++index) {
        if (value[index] < '0' || value[index] > '9') Reject("utc", path);
        number = number * 10 + (value[index] - '0');
    }
    return number;
}

const std::string& Utc(const JsonValue& value, const Path& path) {
    const auto& text = Text(value, path);
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T'
        || text[13] != ':' || text[16] != ':' || text[19] != 'Z') Reject("utc", path);
    const int year = Digits(text, 0, 4, path), month = Digits(text, 5, 2, path), day = Digits(text, 8, 2, path);
    const int hour = Digits(text, 11, 2, path), minute = Digits(text, 14, 2, path), second = Digits(text, 17, 2, path);
    if (year < 1 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) Reject("utc", path);
    constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[static_cast<std::size_t>(month - 1)] + (month == 2 && leap ? 1 : 0)) Reject("utc", path);
    return text;
}

const std::string& Sha256(const JsonValue& value, const Path& path) {
    const auto& text = Text(value, path);
    if (text.size() != 64) Reject("sha256", path);
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) Reject("sha256", path);
    }
    return text;
}

// A whole number written with digits only: no sign, fraction or exponent.
std::uint32_t Integer(const JsonValue& value, const Path& path, const std::uint32_t minimum, const std::uint32_t maximum) {
    if (value.kind != JsonKind::number) Reject("type", path);
    const auto& lexeme = value.string;
    if (lexeme.empty()) Reject("integer", path);
    for (const char c : lexeme) {
        if (c < '0' || c > '9') Reject("integer", path);
    }
    std::uint32_t number = 0;
    const auto parsed = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != lexeme.data() + lexeme.size()) Reject("integer", path);
    if (number < minimum || number > maximum) Reject("integer", path);
    return number;
}

// Finite binary64. Overflow and underflow to zero are both rejected, which is
// stricter than the rig profile reader (it rounds a vanishing value to zero).
double Binary64(const JsonValue& value, const Path& path) {
    if (value.kind != JsonKind::number) Reject("type", path);
    const auto& lexeme = value.string;
    double number = 0;
    const auto parsed = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != lexeme.data() + lexeme.size()) Reject("number", path);
    if (!std::isfinite(number)) Reject("number", path);
    return number == 0 ? 0.0 : number; // -0 becomes +0
}

struct PlanePoint {
    double x;
    double y;
};

bool Collinear(const PlanePoint& a, const PlanePoint& b, const PlanePoint& c) {
    return exact_detail::ExactCollinear(a.x, a.y, b.x, b.y, c.x, c.y);
}

// True if some four of the points have no three on one line. The points must
// be finite and pairwise distinct. A set has no such quadruple exactly when
// all points but at most one lie on a single line; a line holding all but one
// point contains two of p0, p1, pk, so only three lines need testing.
bool HasGeneralPositionQuadruple(const std::vector<PlanePoint>& p) {
    if (p.size() < kMinimumMappingPoints) return false;
    std::size_t k = 2;
    while (k < p.size() && Collinear(p[0], p[1], p[k])) ++k;
    if (k == p.size()) return false;
    const std::array<std::array<std::size_t, 2>, 3> lines{{{0, 1}, {0, k}, {1, k}}};
    for (const auto& line : lines) {
        std::size_t off_line = 0;
        for (const auto& q : p) {
            if (!Collinear(p[line[0]], p[line[1]], q) && ++off_line == 2) break;
        }
        if (off_line <= 1) return false;
    }
    return true;
}

struct SessionData {
    std::string session_id;
    std::string chart_id;
    SessionProvenance provenance;
    std::vector<ChartPoint> chart_points;
    std::map<std::string, std::size_t, std::less<>> chart_index;
    std::array<CameraGroup, 2> cameras{};
    std::vector<SessionView> views;
};

constexpr std::size_t CameraIndex(const CameraAlias camera) { return camera == CameraAlias::CamA ? 0 : 1; }

class Reader {
public:
    SessionData Read(const JsonValue& root) {
        const Path root_path;
        CheckShape(root, root_path, {"schemaVersion", "sessionId", "chartId", "provenance", "chartPoints", "cameras", "views"});
        if (Text(Field(root, "schemaVersion"), Path(root_path, "schemaVersion")) != kObservationSessionSchemaVersion) {
            Reject("schema-version", Path(root_path, "schemaVersion"));
        }
        data_.session_id = SafeId(Field(root, "sessionId"), Path(root_path, "sessionId"));
        data_.chart_id = SafeId(Field(root, "chartId"), Path(root_path, "chartId"));
        ReadProvenance(Field(root, "provenance"), Path(root_path, "provenance"));
        ReadChart(Field(root, "chartPoints"), Path(root_path, "chartPoints"));
        ReadCameras(Field(root, "cameras"), Path(root_path, "cameras"));
        ReadViews(Field(root, "views"), Path(root_path, "views"));
        return std::move(data_);
    }

private:
    void ReadProvenance(const JsonValue& value, const Path& path) {
        CheckShape(value, path, {"measuredAt", "tool"});
        data_.provenance.measured_at_utc = Utc(Field(value, "measuredAt"), Path(path, "measuredAt"));
        const Path tool_path(path, "tool");
        const auto& tool = Field(value, "tool");
        CheckShape(tool, tool_path, {"toolId", "version"});
        data_.provenance.tool.id = ToolIdentifier(Field(tool, "toolId"), Path(tool_path, "toolId"));
        data_.provenance.tool.version = ToolIdentifier(Field(tool, "version"), Path(tool_path, "version"));
    }

    void ReadChart(const JsonValue& value, const Path& path) {
        if (value.kind != JsonKind::array) Reject("type", path);
        if (value.array.size() > kMaximumChartPoints) Reject("limit-chart-points", path);
        data_.chart_points.reserve(value.array.size());
        std::vector<PlanePoint> plane;
        plane.reserve(value.array.size());
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            const Path point_path(path, index);
            const auto& point = value.array[index];
            CheckShape(point, point_path, {"pointId", "xMm", "yMm"});
            const Path id_path(point_path, "pointId");
            const auto& id = SafeId(Field(point, "pointId"), id_path);
            if (!data_.chart_index.emplace(id, index).second) Reject("duplicate-id", id_path);
            const double x = Binary64(Field(point, "xMm"), Path(point_path, "xMm"));
            const double y = Binary64(Field(point, "yMm"), Path(point_path, "yMm"));
            data_.chart_points.push_back({id, x, y});
            plane.push_back({x, y});
        }
        // Equal coordinates (compared by value, so -0 equals +0) name one place twice.
        std::vector<std::tuple<double, double, std::size_t>> sorted;
        sorted.reserve(plane.size());
        for (std::size_t index = 0; index < plane.size(); ++index) sorted.emplace_back(plane[index].x, plane[index].y, index);
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t index = 1; index < sorted.size(); ++index) {
            if (std::get<0>(sorted[index]) == std::get<0>(sorted[index - 1])
                && std::get<1>(sorted[index]) == std::get<1>(sorted[index - 1])) {
                Reject("duplicate-chart-position", Path(path, std::get<2>(sorted[index])));
            }
        }
        if (!HasGeneralPositionQuadruple(plane)) Reject("chart-degenerate", path);
    }

    void ReadCameras(const JsonValue& value, const Path& path) {
        CheckShape(value, path, {"CAM-A", "CAM-B"});
        for (const auto alias : {CameraAlias::CamA, CameraAlias::CamB}) {
            const Path group_path(path, CameraAliasCode(alias));
            const auto& group = Field(value, CameraAliasCode(alias));
            CheckShape(group, group_path, {"widthPixels", "heightPixels", "lensLabel", "focalLengthLabel", "focusLabel"});
            auto& camera = data_.cameras[CameraIndex(alias)];
            camera.width_pixels = Integer(Field(group, "widthPixels"), Path(group_path, "widthPixels"), kMinimumImageSidePixels, kMaximumImageSidePixels);
            camera.height_pixels = Integer(Field(group, "heightPixels"), Path(group_path, "heightPixels"), kMinimumImageSidePixels, kMaximumImageSidePixels);
            camera.lens_label = SafeId(Field(group, "lensLabel"), Path(group_path, "lensLabel"));
            camera.focal_length_label = SafeId(Field(group, "focalLengthLabel"), Path(group_path, "focalLengthLabel"));
            camera.focus_label = SafeId(Field(group, "focusLabel"), Path(group_path, "focusLabel"));
        }
    }

    void ReadViews(const JsonValue& value, const Path& path) {
        if (value.kind != JsonKind::array) Reject("type", path);
        // At most 128 views per camera and two cameras.
        if (value.array.size() > 2 * kMaximumViewsPerCamera) Reject("limit-views-per-camera", path);
        data_.views.reserve(value.array.size());
        scratch_observation_.assign(data_.chart_points.size(), -1);
        scratch_mark_.assign(data_.chart_points.size(), 0);
        std::set<std::string> view_ids, hashes;
        std::array<std::size_t, 2> view_count{}, reference_count{};
        std::size_t total_observations = 0;
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            const Path view_path(path, index);
            const auto& view = value.array[index];
            if (view.kind != JsonKind::object) Reject("type", view_path);
            const auto role_found = view.object.find("role");
            if (role_found == view.object.end()) Reject("missing-field", view_path);
            const Path role_path(view_path, "role");
            const auto& role_text = Text(role_found->second, role_path);
            ViewRole role{};
            if (role_text == ViewRoleCode(ViewRole::Training)) role = ViewRole::Training;
            else if (role_text == ViewRoleCode(ViewRole::Validation)) role = ViewRole::Validation;
            else if (role_text == ViewRoleCode(ViewRole::Reference)) role = ViewRole::Reference;
            else Reject("role", role_path);
            if (role == ViewRole::Validation) {
                CheckShape(view, view_path, {"viewId", "camera", "imageSha256", "widthPixels", "heightPixels", "role",
                    "posePointIds", "measurementPointIds", "observations"});
            } else {
                CheckShape(view, view_path, {"viewId", "camera", "imageSha256", "widthPixels", "heightPixels", "role",
                    "usedPointIds", "observations"});
            }

            SessionView result{};
            result.role = role;
            const Path id_path(view_path, "viewId");
            result.id = SafeId(Field(view, "viewId"), id_path);
            if (!view_ids.insert(result.id).second) Reject("duplicate-id", id_path);
            const Path camera_path(view_path, "camera");
            const auto& camera_text = Text(Field(view, "camera"), camera_path);
            if (camera_text == CameraAliasCode(CameraAlias::CamA)) result.camera = CameraAlias::CamA;
            else if (camera_text == CameraAliasCode(CameraAlias::CamB)) result.camera = CameraAlias::CamB;
            else Reject("camera-alias", camera_path);
            const auto camera_slot = CameraIndex(result.camera);
            if (++view_count[camera_slot] > kMaximumViewsPerCamera) Reject("limit-views-per-camera", view_path);
            const Path hash_path(view_path, "imageSha256");
            result.image_sha256 = Sha256(Field(view, "imageSha256"), hash_path);
            if (!hashes.insert(result.image_sha256).second) Reject("duplicate-sha256", hash_path);
            const Path width_path(view_path, "widthPixels");
            const Path height_path(view_path, "heightPixels");
            result.width_pixels = Integer(Field(view, "widthPixels"), width_path, kMinimumImageSidePixels, kMaximumImageSidePixels);
            result.height_pixels = Integer(Field(view, "heightPixels"), height_path, kMinimumImageSidePixels, kMaximumImageSidePixels);
            const auto& group = data_.cameras[camera_slot];
            if (result.width_pixels != group.width_pixels) Reject("dimension-mismatch", width_path);
            if (result.height_pixels != group.height_pixels) Reject("dimension-mismatch", height_path);

            ReadObservations(Field(view, "observations"), Path(view_path, "observations"), result, total_observations);
            if (role == ViewRole::Validation) {
                ReadPointIds(Field(view, "posePointIds"), Path(view_path, "posePointIds"), result, 1, false, result.pose_point_ids);
                ReadPointIds(Field(view, "measurementPointIds"), Path(view_path, "measurementPointIds"), result, 2, true, result.measurement_point_ids);
            } else {
                ReadPointIds(Field(view, "usedPointIds"), Path(view_path, "usedPointIds"), result, 1, false, result.used_point_ids);
            }
            ClearScratch();
            if (role == ViewRole::Reference && ++reference_count[camera_slot] > 1) Reject("reference-duplicate", role_path);
            data_.views.push_back(std::move(result));
        }
    }

    void ReadObservations(const JsonValue& value, const Path& path, SessionView& view, std::size_t& total_observations) {
        if (value.kind != JsonKind::array) Reject("type", path);
        if (value.array.size() > kMaximumObservationsPerView
            || total_observations + value.array.size() > kMaximumObservationsPerSession) {
            Reject("limit-observations", path);
        }
        total_observations += value.array.size();
        view.observations.reserve(value.array.size());
        // Pixel centres are integers, so the valid span reaches half a pixel
        // beyond the outermost centres. W - 0.5 is exact for W <= 32768.
        const double x_maximum = static_cast<double>(view.width_pixels) - 0.5;
        const double y_maximum = static_cast<double>(view.height_pixels) - 0.5;
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            const Path observation_path(path, index);
            const auto& observation = value.array[index];
            CheckShape(observation, observation_path, {"pointId", "state", "pixel"});
            const Path id_path(observation_path, "pointId");
            const auto& id = SafeId(Field(observation, "pointId"), id_path);
            const auto chart = data_.chart_index.find(id);
            if (chart == data_.chart_index.end()) Reject("undefined-point", id_path);
            if (scratch_observation_[chart->second] >= 0) Reject("duplicate-id", id_path);
            scratch_observation_[chart->second] = static_cast<std::int64_t>(index);
            touched_.push_back(chart->second);

            const Path state_path(observation_path, "state");
            const auto& state_text = Text(Field(observation, "state"), state_path);
            Observation result{id, ObservationState::Detected, std::nullopt};
            if (state_text == ObservationStateCode(ObservationState::Detected)) result.state = ObservationState::Detected;
            else if (state_text == ObservationStateCode(ObservationState::Missing)) result.state = ObservationState::Missing;
            else if (state_text == ObservationStateCode(ObservationState::Ambiguous)) result.state = ObservationState::Ambiguous;
            else Reject("state", state_path);

            const Path pixel_path(observation_path, "pixel");
            const auto& pixel = Field(observation, "pixel");
            if (result.state == ObservationState::Detected) {
                if (pixel.kind == JsonKind::null_value) Reject("state-pixel", pixel_path);
                CheckShape(pixel, pixel_path, {"x", "y"});
                const double x = Binary64(Field(pixel, "x"), Path(pixel_path, "x"));
                const double y = Binary64(Field(pixel, "y"), Path(pixel_path, "y"));
                if (!(x >= -0.5 && x <= x_maximum)) Reject("pixel-range", Path(pixel_path, "x"));
                if (!(y >= -0.5 && y <= y_maximum)) Reject("pixel-range", Path(pixel_path, "y"));
                result.pixel = PixelPosition{x, y};
            } else if (pixel.kind != JsonKind::null_value) {
                Reject("state-pixel", pixel_path);
            }
            view.observations.push_back(std::move(result));
        }
    }

    // mark: 1 for the first list of a view, 2 for the second of a validation view.
    void ReadPointIds(const JsonValue& value, const Path& path, const SessionView& view, const std::uint8_t mark,
        const bool must_be_nonempty, std::vector<std::string>& output) {
        if (value.kind != JsonKind::array) Reject("type", path);
        if (must_be_nonempty && value.array.empty()) Reject("validation-measurement-empty", path);
        output.reserve(value.array.size());
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            const Path id_path(path, index);
            const auto& id = SafeId(value.array[index], id_path);
            const auto chart = data_.chart_index.find(id);
            if (chart == data_.chart_index.end()) Reject("undefined-point", id_path);
            const auto chart_slot = chart->second;
            if (scratch_mark_[chart_slot] == mark) Reject("duplicate-id", id_path);
            if (scratch_mark_[chart_slot] != 0) Reject("validation-overlap", id_path);
            const auto observation = scratch_observation_[chart_slot];
            if (observation < 0) Reject("undefined-point", id_path);
            if (view.observations[static_cast<std::size_t>(observation)].state != ObservationState::Detected) {
                Reject("used-not-detected", id_path);
            }
            scratch_mark_[chart_slot] = mark;
            output.push_back(id);
        }
    }

    void ClearScratch() {
        for (const auto slot : touched_) {
            scratch_observation_[slot] = -1;
            scratch_mark_[slot] = 0;
        }
        touched_.clear();
    }

    SessionData data_;
    std::vector<std::size_t> touched_;              // chart slots set while reading the current view
    std::vector<std::int64_t> scratch_observation_; // chart slot -> observation index in the current view, or -1
    std::vector<std::uint8_t> scratch_mark_;        // chart slot -> list that already named it in the current view
};

} // namespace

struct CalibrationObservationSession::Data {
    explicit Data(SessionData value) : value(std::move(value)) {}
    const SessionData value;
};

CalibrationObservationSession::CalibrationObservationSession(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

CalibrationObservationSession CalibrationObservationSession::Parse(const std::string_view json) {
    // The byte limit and the value count are checked before any parsing work.
    if (json.empty() || json.size() > kMaximumObservationSessionJsonBytes) Reject("json-byte-limit", Path());
    // An input within the byte limit can still hold millions of values, and the shared parser
    // spends one tree node on each; bound the count before the tree exists.
    if (detail::JsonValueCountUpperBound(json) > kMaximumObservationSessionJsonValues) Reject("limit-json-values", Path());
    const auto root = a0::common::protocol_json::BasicJsonParser<SessionJsonFailure, kMaximumObservationSessionJsonBytes>(json).Parse();
    // Truth keys are looked for before the shape so that a generator document
    // is reported as such instead of as a missing field.
    RejectTruthKeys(root, Path());
    Reader reader;
    return CalibrationObservationSession(std::make_shared<const Data>(reader.Read(root)));
}

const std::string& CalibrationObservationSession::SessionId() const noexcept { return data_->value.session_id; }
const std::string& CalibrationObservationSession::ChartId() const noexcept { return data_->value.chart_id; }
const SessionProvenance& CalibrationObservationSession::Provenance() const noexcept { return data_->value.provenance; }
std::span<const ChartPoint> CalibrationObservationSession::ChartPoints() const noexcept { return data_->value.chart_points; }
std::span<const SessionView> CalibrationObservationSession::Views() const noexcept { return data_->value.views; }

const ChartPoint* CalibrationObservationSession::FindChartPoint(const std::string_view id) const noexcept {
    const auto found = data_->value.chart_index.find(id);
    return found == data_->value.chart_index.end() ? nullptr : &data_->value.chart_points[found->second];
}

const CameraGroup& CalibrationObservationSession::Camera(const CameraAlias camera) const {
    if (camera != CameraAlias::CamA && camera != CameraAlias::CamB) {
        throw std::invalid_argument("calibration observation session: camera alias out of range");
    }
    return data_->value.cameras[CameraIndex(camera)];
}

std::vector<SessionDiagnostic> DiagnoseObservationSession(const CalibrationObservationSession& session) {
    std::vector<SessionDiagnostic> diagnostics;
    const auto views = session.Views();
    for (const auto camera : {CameraAlias::CamA, CameraAlias::CamB}) {
        bool has_reference = false, has_validation = false;
        for (const auto& view : views) {
            if (view.camera != camera) continue;
            has_reference = has_reference || view.role == ViewRole::Reference;
            has_validation = has_validation || view.role == ViewRole::Validation;
        }
        if (!has_reference) diagnostics.push_back({SessionDiagnosticCode::ReferenceViewMissing, camera, std::nullopt});
        if (!has_validation) diagnostics.push_back({SessionDiagnosticCode::ValidationViewMissing, camera, std::nullopt});
    }
    for (const auto& view : views) {
        // Measurement points of a validation view are not used for the pose.
        const auto& used = view.role == ViewRole::Validation ? view.pose_point_ids : view.used_point_ids;
        if (used.size() < kMinimumMappingPoints) {
            diagnostics.push_back({SessionDiagnosticCode::InsufficientObservations, view.camera, view.id});
        } else {
            std::vector<PlanePoint> plane;
            plane.reserve(used.size());
            for (const auto& id : used) {
                const auto* point = session.FindChartPoint(id);
                plane.push_back({point->x_mm, point->y_mm}); // Parse guarantees the point exists
            }
            if (!HasGeneralPositionQuadruple(plane)) {
                diagnostics.push_back({SessionDiagnosticCode::DegeneratePointConfiguration, view.camera, view.id});
            }
        }
        // Distinct point ids sharing one pixel cannot all be correct. Every
        // detected observation counts, not only the points used.
        std::vector<std::pair<double, double>> pixels;
        for (const auto& observation : view.observations) {
            if (observation.pixel) pixels.emplace_back(observation.pixel->x, observation.pixel->y);
        }
        std::sort(pixels.begin(), pixels.end());
        if (std::adjacent_find(pixels.begin(), pixels.end()) != pixels.end()) {
            diagnostics.push_back({SessionDiagnosticCode::AmbiguousCorrespondence, view.camera, view.id});
        }
    }
    return diagnostics;
}

} // namespace a0::m2::calibration
