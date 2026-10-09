#include "a0/m2/rig_profile_v2.hpp"
#include "a0/common/protocol_json.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace a0::m2 {
namespace {

using a0::common::protocol_json::JsonKind;
using a0::common::protocol_json::JsonValue;
using namespace render;

[[noreturn]] void Reject(const std::string& context) {
    throw std::invalid_argument("rig profile v2: " + context);
}

struct JsonFailure {
    [[noreturn]] static void Fail(std::string_view code, std::string_view) {
        Reject("JSON " + std::string(code));
    }
};

void Fields(const JsonValue& value, const std::string& path, const std::initializer_list<const char*> names) {
    if (value.kind != JsonKind::object || value.object.size() != names.size()) Reject(path + " shape");
    for (const auto* name : names) {
        if (!value.object.contains(name)) Reject(path + " missing or unknown field");
    }
}

const JsonValue& At(const JsonValue& value, const char* name) {
    return value.object.at(name); // Only used after Fields has established the exact shape.
}

struct DecimalParts {
    bool negative;
    std::string digits;
    std::int64_t power;
};

// The common JSON parser already established number syntax. Keep exact decimal
// digits for micrometre divisibility and for underflow/overflow classification.
DecimalParts Decimal(const std::string& token) {
    DecimalParts parts{!token.empty() && token.front() == '-', {}, 0};
    const auto exponent_at = token.find_first_of("eE");
    const auto end = exponent_at == std::string::npos ? token.size() : exponent_at;
    const auto dot = token.find('.');
    const auto begin = parts.negative ? std::size_t{1} : std::size_t{0};
    for (std::size_t index = begin; index < end; ++index) {
        if (token[index] != '.') parts.digits += token[index];
    }
    const auto fraction = dot == std::string::npos ? std::size_t{0} : end - dot - 1;
    std::int64_t exponent = 0;
    if (exponent_at != std::string::npos) {
        std::size_t index = exponent_at + 1;
        bool minus = false;
        if (token[index] == '+' || token[index] == '-') { minus = token[index] == '-'; ++index; }
        for (; index < token.size(); ++index) {
            // A saturated exponent still preserves all meaningful comparisons
            // for a bounded JSON document, without integer overflow.
            exponent = std::min<std::int64_t>(1000000, exponent * 10 + token[index] - '0');
        }
        if (minus) exponent = -exponent;
    }
    parts.power = exponent - static_cast<std::int64_t>(fraction);
    const auto nonzero = parts.digits.find_first_not_of('0');
    if (nonzero == std::string::npos) { parts.digits = "0"; parts.negative = false; }
    else parts.digits.erase(0, nonzero);
    return parts;
}

double Binary64(const JsonValue& value, const std::string& path) {
    if (value.kind != JsonKind::number) Reject(path + " number required");
    double number = 0;
    const auto parsed = std::from_chars(value.string.data(), value.string.data() + value.string.size(), number);
    if (parsed.ptr != value.string.data() + value.string.size()) Reject(path + " number conversion");
    if (parsed.ec == std::errc::result_out_of_range) {
        const auto decimal = Decimal(value.string);
        const auto magnitude = static_cast<std::int64_t>(decimal.digits.size()) - 1 + decimal.power;
        // from_chars reports range failure when a tiny value rounds to zero.
        // The nearest-even result is zero; overflow cannot have negative order.
        if (decimal.digits == "0" || magnitude < 0) number = 0;
        else Reject(path + " binary64 overflow");
    } else if (parsed.ec != std::errc{}) Reject(path + " number conversion");
    if (!std::isfinite(number)) Reject(path + " finite number required");
    return number == 0 ? 0.0 : number; // canonicalize either signed zero
}

std::int64_t Micrometres(const JsonValue& value, const std::string& path) {
    if (value.kind != JsonKind::number) Reject(path + " number required");
    auto decimal = Decimal(value.string);
    if (decimal.digits == "0") return 0;
    if (decimal.negative) Reject(path + " nonnegative region required");
    decimal.power += 3;
    if (decimal.power < 0) {
        const auto remove = static_cast<std::uint64_t>(-decimal.power);
        if (remove >= decimal.digits.size()) Reject(path + " exact micrometre multiple required");
        for (std::size_t index = decimal.digits.size() - static_cast<std::size_t>(remove); index < decimal.digits.size(); ++index) {
            if (decimal.digits[index] != '0') Reject(path + " exact micrometre multiple required");
        }
        decimal.digits.resize(decimal.digits.size() - static_cast<std::size_t>(remove));
        decimal.power = 0;
    }
    if (decimal.digits.size() + static_cast<std::uint64_t>(decimal.power) > 18) Reject(path + " region integer range");
    std::int64_t result = 0;
    const auto parsed = std::from_chars(decimal.digits.data(), decimal.digits.data() + decimal.digits.size(), result);
    if (parsed.ec != std::errc{}) Reject(path + " region integer range");
    for (std::int64_t index = 0; index < decimal.power; ++index) result *= 10;
    return result;
}

int Digits(std::string_view value, const std::size_t begin, const std::size_t count) {
    int number = 0;
    for (std::size_t index = begin; index < begin + count; ++index) {
        if (value[index] < '0' || value[index] > '9') Reject("UTC digits");
        number = number * 10 + value[index] - '0';
    }
    return number;
}

void Utc(std::string_view value) {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T'
        || value[13] != ':' || value[16] != ':' || value[19] != 'Z') Reject("UTC whole-second Z format");
    const int year = Digits(value, 0, 4), month = Digits(value, 5, 2), day = Digits(value, 8, 2);
    const int hour = Digits(value, 11, 2), minute = Digits(value, 14, 2), second = Digits(value, 17, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) Reject("UTC calendar range");
    constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[static_cast<std::size_t>(month - 1)] + (month == 2 && leap ? 1 : 0)) Reject("UTC calendar day");
}

bool Alnum(const char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

struct Reader {
    std::string canonical = "a0.rig-profile-fingerprint.v2\n";

    void Line(const std::string& path, const std::string& value) { canonical += path + '=' + value + '\n'; }
    bool Null(const JsonValue& value, const std::string& path) {
        if (value.kind != JsonKind::null_value) return false;
        Line(path, "null"); return true;
    }
    void RequiredNull(const JsonValue& value, const std::string& path) {
        if (!Null(value, path)) Reject(path + " must be null");
    }
    std::string String(const JsonValue& value, const std::string& path) {
        if (value.kind != JsonKind::string) Reject(path + " string required");
        for (const unsigned char ch : value.string) {
            if (ch < 0x20 || ch > 0x7e || ch == '=') Reject(path + " canonical ASCII required");
        }
        Line(path, "s:" + value.string); return value.string;
    }
    std::string Identifier(const JsonValue& value, const std::string& path) {
        const auto text = String(value, path);
        if (text.empty() || text.size() > 128 || !Alnum(text.front())) Reject(path + " identifier");
        for (const auto ch : text) {
            if (!Alnum(ch) && ch != '.' && ch != '_' && ch != '-') Reject(path + " identifier");
        }
        return text;
    }
    void Constant(const JsonValue& value, const std::string& path, const char* expected) {
        if (String(value, path) != expected) Reject(path + " constant");
    }
    std::string Timestamp(const JsonValue& value, const std::string& path) {
        const auto text = String(value, path); Utc(text); return text;
    }
    void Hash(const JsonValue& value, const std::string& path) {
        const auto text = String(value, path);
        if (text.size() != 64) Reject(path + " SHA-256");
        for (const auto ch : text) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) Reject(path + " lowercase SHA-256");
    }
    double Number(const JsonValue& value, const std::string& path) {
        const double number = Binary64(value, path);
        const auto bits = std::bit_cast<std::uint64_t>(number);
        constexpr char hex[] = "0123456789abcdef";
        std::string encoded(16, '0');
        for (std::size_t index = 0; index < 16; ++index) encoded[index] = hex[(bits >> ((15 - index) * 4)) & 15];
        Line(path, "f:" + encoded); return number;
    }
    double Nonnegative(const JsonValue& value, const std::string& path, const bool positive = false) {
        const double number = Number(value, path);
        if (Decimal(value.string).negative || number < 0 || (positive && number == 0)) Reject(path + " numeric range");
        return number;
    }
    void OptionalMeasurement(const JsonValue& value, const std::string& path, const bool approved, const bool positive) {
        if (value.kind == JsonKind::null_value) {
            if (approved) Reject(path + " approved measurement required");
            Null(value, path);
        } else (void)Nonnegative(value, path, positive);
    }
    std::uint32_t Integer(const JsonValue& value, const std::string& path, const std::uint32_t minimum, const std::uint32_t maximum) {
        if (value.kind != JsonKind::number || value.string.empty()) Reject(path + " integer required");
        for (const auto ch : value.string) if (ch < '0' || ch > '9') Reject(path + " integer lexeme required");
        std::uint32_t number = 0;
        const auto parsed = std::from_chars(value.string.data(), value.string.data() + value.string.size(), number);
        if (parsed.ec != std::errc{} || number < minimum || number > maximum) Reject(path + " integer range");
        Line(path, "i:" + std::to_string(number)); return number;
    }
};

struct State {
    bool approved = false, calibrated = false;
    std::optional<std::string> id;
    double sheet_width = 0, sheet_height = 0;
    std::optional<StitchLayout> layout;
    std::optional<CameraProjection> camera_a, camera_b;
    std::optional<OutputRaster> raster;
    std::string measured_at, approved_at, valid_until, canonical;
};

std::optional<CameraProjection> Projection(Reader& reader, const JsonValue& value, const std::string& path) {
    if (reader.Null(value, path)) return std::nullopt;
    Fields(value, path, {"intrinsics", "distortion", "documentToImage"});
    const auto& intrinsics = At(value, "intrinsics");
    Fields(intrinsics, path + "/intrinsics", {"fxPixels", "fyPixels", "cxPixels", "cyPixels"});
    const double fx = reader.Nonnegative(At(intrinsics, "fxPixels"), path + "/intrinsics/fxPixels", true);
    const double fy = reader.Nonnegative(At(intrinsics, "fyPixels"), path + "/intrinsics/fyPixels", true);
    const double cx = reader.Number(At(intrinsics, "cxPixels"), path + "/intrinsics/cxPixels");
    const double cy = reader.Number(At(intrinsics, "cyPixels"), path + "/intrinsics/cyPixels");
    const auto& distortion = At(value, "distortion");
    Fields(distortion, path + "/distortion", {"model", "k1", "k2", "k3", "p1", "p2"});
    reader.Constant(At(distortion, "model"), path + "/distortion/model", "brown-conrady-k1k2k3-p1p2");
    std::array<double, 5> d{};
    constexpr std::array<const char*, 5> names{"k1", "k2", "k3", "p1", "p2"};
    for (std::size_t index = 0; index < d.size(); ++index) d[index] = reader.Number(At(distortion, names[index]), path + "/distortion/" + names[index]);
    const auto& rows = At(value, "documentToImage");
    if (rows.kind != JsonKind::array || rows.array.size() != 3) Reject(path + " matrix rows");
    Matrix3 h{};
    for (std::size_t row = 0; row < 3; ++row) {
        if (rows.array[row].kind != JsonKind::array || rows.array[row].array.size() != 3) Reject(path + " matrix columns");
        for (std::size_t column = 0; column < 3; ++column) {
            h[row * 3 + column] = reader.Number(rows.array[row].array[column], path + "/documentToImage/" + std::to_string(row) + '/' + std::to_string(column));
        }
    }
    if (h[8] != 1) Reject(path + " matrix normalization");
    const long double determinant = static_cast<long double>(h[0]) * (static_cast<long double>(h[4]) * h[8] - static_cast<long double>(h[5]) * h[7])
        - static_cast<long double>(h[1]) * (static_cast<long double>(h[3]) * h[8] - static_cast<long double>(h[5]) * h[6])
        + static_cast<long double>(h[2]) * (static_cast<long double>(h[3]) * h[7] - static_cast<long double>(h[4]) * h[6]);
    if (!std::isfinite(determinant) || determinant == 0) Reject(path + " singular matrix");
    return CameraProjection{{fx, fy, cx, cy}, {d[0], d[1], d[2], d[3], d[4]}, h};
}

void Calibration(Reader& reader, const JsonValue& value, State& state) {
    const std::string path = "/calibration";
    if (reader.Null(value, path)) return;
    Fields(value, path, {"method", "chartId", "provenanceId", "measuredAt", "tool", "inputSha256", "fitResidualPixels", "rigMeasurements"});
    reader.Constant(At(value, "method"), path + "/method", "chart-fiducial-planar");
    (void)reader.Identifier(At(value, "chartId"), path + "/chartId");
    (void)reader.Identifier(At(value, "provenanceId"), path + "/provenanceId");
    state.measured_at = reader.Timestamp(At(value, "measuredAt"), path + "/measuredAt");
    const auto& tool = At(value, "tool"); Fields(tool, path + "/tool", {"toolId", "version"});
    (void)reader.Identifier(At(tool, "toolId"), path + "/tool/toolId");
    (void)reader.Identifier(At(tool, "version"), path + "/tool/version");
    const auto& hashes = At(value, "inputSha256"); Fields(hashes, path + "/inputSha256", {"CAM-A", "CAM-B"});
    for (const auto* alias : {"CAM-A", "CAM-B"}) reader.Hash(At(hashes, alias), path + "/inputSha256/" + alias);
    const auto& residuals = At(value, "fitResidualPixels"); Fields(residuals, path + "/fitResidualPixels", {"CAM-A", "CAM-B"});
    for (const auto* alias : {"CAM-A", "CAM-B"}) {
        const auto& residual = At(residuals, alias); Fields(residual, path + "/fitResidualPixels/" + alias, {"rms", "max"});
        for (const auto* field : {"rms", "max"}) (void)reader.Nonnegative(At(residual, field), path + "/fitResidualPixels/" + alias + '/' + field);
    }
    const auto& rig = At(value, "rigMeasurements"); Fields(rig, path + "/rigMeasurements", {"baselineMm", "overlapMm", "cameraToDocumentMm", "lensFocalLengthMm"});
    reader.OptionalMeasurement(At(rig, "baselineMm"), path + "/rigMeasurements/baselineMm", state.approved, true);
    reader.OptionalMeasurement(At(rig, "overlapMm"), path + "/rigMeasurements/overlapMm", state.approved, false);
    for (const auto* field : {"cameraToDocumentMm", "lensFocalLengthMm"}) {
        const auto& pair = At(rig, field); Fields(pair, path + "/rigMeasurements/" + field, {"CAM-A", "CAM-B"});
        for (const auto* alias : {"CAM-A", "CAM-B"}) reader.OptionalMeasurement(At(pair, alias), path + "/rigMeasurements/" + field + '/' + alias, state.approved, true);
    }
}

void Geometry(const State& state, const OutputRaster& raster) {
    const auto& r = raster.region_um;
    if (r.left < 0 || r.top < 0 || r.right > state.sheet_width * 1000 || r.bottom > state.sheet_height * 1000) Reject("output region outside sheet");
    ValidateOutputRaster(raster);
    ValidateCameraProjection(*state.camera_a, raster);
    ValidateCameraProjection(*state.camera_b, raster);
}

void Kernel(const Resampling kernel) {
    if (kernel != Resampling::bilinear && kernel != Resampling::bicubic_catmull_rom) Reject("explicit supported kernel required");
}

State ParseState(const JsonValue& root) {
    Reader reader; State state;
    Fields(root, "/", {"schemaVersion", "status", "profileId", "conventions", "cameraModel", "documentPlane", "cameras", "calibration", "outputRaster", "correctionEnvelope", "qualityContract", "approval"});
    reader.Constant(At(root, "schemaVersion"), "/schemaVersion", "2.0.0");
    const auto status = reader.String(At(root, "status"), "/status");
    if (status != "draft" && status != "approved") Reject("status");
    state.approved = status == "approved";
    if (!reader.Null(At(root, "profileId"), "/profileId")) state.id = reader.Identifier(At(root, "profileId"), "/profileId");
    if (state.approved && !state.id) Reject("approved profileId required");
    const auto& conventions = At(root, "conventions"); Fields(conventions, "/conventions", {"documentPlane", "cameraPixel", "matrix", "exifOrientation"});
    reader.Constant(At(conventions, "documentPlane"), "/conventions/documentPlane", "mm-origin-sheet-top-left-x-right-y-down");
    reader.Constant(At(conventions, "cameraPixel"), "/conventions/cameraPixel", "stored-order-sample-at-integer-index");
    reader.Constant(At(conventions, "matrix"), "/conventions/matrix", "row-major-column-vector");
    reader.Constant(At(conventions, "exifOrientation"), "/conventions/exifOrientation", "ignored");
    const auto& model = At(root, "cameraModel"); Fields(model, "/cameraModel", {"manufacturer", "model", "sensorWidthPixels", "sensorHeightPixels"});
    reader.Constant(At(model, "manufacturer"), "/cameraModel/manufacturer", "Nikon");
    reader.Constant(At(model, "model"), "/cameraModel/model", "D810");
    (void)reader.Integer(At(model, "sensorWidthPixels"), "/cameraModel/sensorWidthPixels", 7360, 7360);
    (void)reader.Integer(At(model, "sensorHeightPixels"), "/cameraModel/sensorHeightPixels", 4912, 4912);
    const auto& plane = At(root, "documentPlane");
    const bool has_plane = !reader.Null(plane, "/documentPlane");
    if (has_plane) {
        Fields(plane, "/documentPlane", {"paper", "orientation", "sheetWidthMm", "sheetHeightMm", "cameraOrder"});
        reader.Constant(At(plane, "paper"), "/documentPlane/paper", "ISO-216-A0");
        const auto orientation = reader.String(At(plane, "orientation"), "/documentPlane/orientation");
        if (orientation != "landscape" && orientation != "portrait") Reject("orientation");
        state.sheet_width = reader.Number(At(plane, "sheetWidthMm"), "/documentPlane/sheetWidthMm");
        state.sheet_height = reader.Number(At(plane, "sheetHeightMm"), "/documentPlane/sheetHeightMm");
        if (state.sheet_width != (orientation == "landscape" ? 1189 : 841) || state.sheet_height != (orientation == "landscape" ? 841 : 1189)) Reject("orientation sheet dimensions");
        const auto order = reader.String(At(plane, "cameraOrder"), "/documentPlane/cameraOrder");
        if (order == "CAM-A-left-CAM-B-right") state.layout = StitchLayout::camera_a_left_camera_b_right;
        else if (order == "CAM-A-top-CAM-B-bottom") state.layout = StitchLayout::camera_a_top_camera_b_bottom;
        else Reject("camera order");
    }
    const auto& cameras = At(root, "cameras"); Fields(cameras, "/cameras", {"CAM-A", "CAM-B"});
    for (const auto* alias : {"CAM-A", "CAM-B"}) {
        const auto& camera = At(cameras, alias); const std::string path = std::string("/cameras/") + alias;
        Fields(camera, path, {"physicalIdentity", "projection"});
        reader.RequiredNull(At(camera, "physicalIdentity"), path + "/physicalIdentity");
        auto projection = Projection(reader, At(camera, "projection"), path + "/projection");
        if (std::string_view(alias) == "CAM-A") state.camera_a = std::move(projection);
        else state.camera_b = std::move(projection);
    }
    Calibration(reader, At(root, "calibration"), state);
    const bool has_calibration = At(root, "calibration").kind != JsonKind::null_value;
    state.calibrated = has_plane && state.camera_a && state.camera_b && has_calibration;
    const bool template_draft = !has_plane && !state.camera_a && !state.camera_b && !has_calibration;
    if ((!state.calibrated && !template_draft) || (state.approved && !state.calibrated)) Reject("calibration blocks must be complete together");
    const auto& raster = At(root, "outputRaster");
    if (!reader.Null(raster, "/outputRaster")) {
        if (!state.approved) Reject("draft owner decisions must be null");
        Fields(raster, "/outputRaster", {"dpi", "regionMm", "widthPixels", "heightPixels"});
        const auto dpi = reader.Integer(At(raster, "dpi"), "/outputRaster/dpi", 1, 65535);
        const auto& region = At(raster, "regionMm"); Fields(region, "/outputRaster/regionMm", {"left", "top", "right", "bottom"});
        std::array<std::int64_t, 4> um{}; constexpr std::array<const char*, 4> names{"left", "top", "right", "bottom"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto& field = At(region, names[index]); const auto path = std::string("/outputRaster/regionMm/") + names[index];
            um[index] = Micrometres(field, path); (void)reader.Number(field, path);
        }
        const auto width = reader.Integer(At(raster, "widthPixels"), "/outputRaster/widthPixels", 1, 65535);
        const auto height = reader.Integer(At(raster, "heightPixels"), "/outputRaster/heightPixels", 1, 65535);
        state.raster.emplace(DocumentRegionUm{um[0], um[1], um[2], um[3]}, dpi, width, height);
    } else if (state.approved) Reject("approved output raster required");
    const auto& envelope = At(root, "correctionEnvelope");
    if (!reader.Null(envelope, "/correctionEnvelope")) {
        if (!state.approved) Reject("draft owner decisions must be null");
        Fields(envelope, "/correctionEnvelope", {"minimumOverlapMm", "registrationErrorOutputPixels", "rotationErrorDegrees", "scaleDifferencePercent", "exposureDifferenceEv", "colorDeltaE"});
        (void)reader.Nonnegative(At(envelope, "minimumOverlapMm"), "/correctionEnvelope/minimumOverlapMm");
        for (const auto* name : {"registrationErrorOutputPixels", "rotationErrorDegrees", "scaleDifferencePercent", "exposureDifferenceEv", "colorDeltaE"}) {
            const auto& limits = At(envelope, name); const auto path = std::string("/correctionEnvelope/") + name;
            Fields(limits, path, {"targetMax", "autoCorrectionMax"});
            const auto target = reader.Nonnegative(At(limits, "targetMax"), path + "/targetMax");
            const auto automatic = reader.Nonnegative(At(limits, "autoCorrectionMax"), path + "/autoCorrectionMax");
            if (target > automatic) Reject(path + " target exceeds autoCorrectionMax");
        }
    } else if (state.approved) Reject("approved correction envelope required");
    const auto& quality = At(root, "qualityContract");
    if (!reader.Null(quality, "/qualityContract")) {
        if (!state.approved) Reject("draft owner decisions must be null");
        Fields(quality, "/qualityContract", {"seamErrorOutputPixelsMax", "registrationErrorOutputPixelsMax", "colorDeltaEMax"});
        for (const auto* name : {"seamErrorOutputPixelsMax", "registrationErrorOutputPixelsMax", "colorDeltaEMax"}) (void)reader.Nonnegative(At(quality, name), std::string("/qualityContract/") + name);
    } else if (state.approved) Reject("approved quality contract required");
    const auto& approval = At(root, "approval");
    if (!reader.Null(approval, "/approval")) {
        if (!state.approved) Reject("draft owner decisions must be null");
        Fields(approval, "/approval", {"decisionRef", "approvedAt", "validUntil"});
        (void)reader.Identifier(At(approval, "decisionRef"), "/approval/decisionRef");
        state.approved_at = reader.Timestamp(At(approval, "approvedAt"), "/approval/approvedAt");
        state.valid_until = reader.Timestamp(At(approval, "validUntil"), "/approval/validUntil");
        if (state.measured_at > state.approved_at || state.approved_at >= state.valid_until) Reject("approval time ordering");
    } else if (state.approved) Reject("approved approval record required");
    if (state.approved) Geometry(state, *state.raster);
    state.canonical = std::move(reader.canonical);
    return state;
}

std::string Sha256(std::string_view text) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("profile SHA-256 provider failed");
    struct AlgorithmGuard { BCRYPT_ALG_HANDLE value; ~AlgorithmGuard() { BCryptCloseAlgorithmProvider(value, 0); } } ag{algorithm};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("profile SHA-256 hash failed");
    struct HashGuard { BCRYPT_HASH_HANDLE value; ~HashGuard() { BCryptDestroyHash(value); } } hg{hash};
    if (text.size() > std::numeric_limits<ULONG>::max() || BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), 0) < 0) throw std::runtime_error("profile SHA-256 input failed");
    std::array<UCHAR, 32> bytes{};
    if (BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) throw std::runtime_error("profile SHA-256 finish failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    for (const auto byte : bytes) { output += hex[byte >> 4]; output += hex[byte & 15]; }
    return output;
}

} // namespace

struct RigProfileV2::Data {
    explicit Data(State value) : state(std::move(value)) {}
    const State state;
};

RigProfileV2::RigProfileV2(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

RigProfileV2 RigProfileV2::Parse(const std::string_view json) {
    auto value = a0::common::protocol_json::BasicJsonParser<JsonFailure>(json).Parse();
    return RigProfileV2(std::make_shared<const Data>(ParseState(value)));
}

const std::string& RigProfileV2::CanonicalFingerprintText() const noexcept { return data_->state.canonical; }
std::string RigProfileV2::FingerprintSha256() const { return Sha256(data_->state.canonical); }
bool RigProfileV2::IsApproved() const noexcept { return data_->state.approved; }
bool RigProfileV2::IsCalibrated() const noexcept { return data_->state.calibrated; }
const std::optional<std::string>& RigProfileV2::ProfileId() const noexcept { return data_->state.id; }

DocumentRenderParameters RigProfileV2::ValidateApprovedForUse(const std::string_view assessed_at_utc, const Resampling kernel) const {
    const auto& state = data_->state;
    if (!state.approved || !state.calibrated || !state.raster) Reject("approved calibrated profile required for use");
    Utc(assessed_at_utc);
    if (assessed_at_utc < state.approved_at || assessed_at_utc >= state.valid_until) Reject("profile approval does not cover assessment");
    Kernel(kernel); Geometry(state, *state.raster);
    return {*state.raster, *state.camera_a, *state.camera_b, *state.layout, kernel};
}

DocumentRenderParameters RigProfileV2::ValidateCalibrationForEvaluation(const OutputRaster& raster, const Resampling kernel) const {
    const auto& state = data_->state;
    if (state.approved || !state.calibrated) Reject("calibrated draft required for evaluation");
    Kernel(kernel); Geometry(state, raster);
    return {raster, *state.camera_a, *state.camera_b, *state.layout, kernel};
}

} // namespace a0::m2
