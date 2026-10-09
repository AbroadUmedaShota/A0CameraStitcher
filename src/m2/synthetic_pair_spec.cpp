#include "synthetic_pair_chart.hpp"
#include "synthetic_pair_geometry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace a0::m2::synthetic {

namespace {

constexpr std::uint32_t kMaximumImageDimension = 16384;
constexpr std::size_t kMaximumCameraCount = 8;
constexpr double kMaximumChartExtentMm = 100000.0;
constexpr int kInversionGridSteps = 32;

[[noreturn]] void Reject(const std::string& message) {
    throw std::invalid_argument("synthetic pair spec: " + message);
}

// Reads one JSON object and rejects keys nobody asked for.
class ObjectReader final {
public:
    ObjectReader(const JsonValue& value, std::string context) : value_(value), context_(std::move(context)) {
        if (value.kind != JsonValue::Kind::object) Reject(context_ + " must be an object");
    }

    const JsonValue& Require(const std::string_view key) {
        const auto* found = value_.Find(key);
        if (found == nullptr) Reject(context_ + " is missing \"" + std::string(key) + "\"");
        used_.emplace_back(key);
        return *found;
    }

    [[nodiscard]] const JsonValue* Optional(const std::string_view key) {
        const auto* found = value_.Find(key);
        if (found != nullptr) used_.emplace_back(key);
        return found;
    }

    void Finish() const {
        for (const auto& key : value_.keys) {
            if (std::find(used_.begin(), used_.end(), key) == used_.end()) {
                Reject(context_ + " has unknown key \"" + key + "\"");
            }
        }
    }

    [[nodiscard]] const std::string& Context() const noexcept { return context_; }

    double Number(const std::string_view key) {
        const auto& member = Require(key);
        try {
            return member.AsDouble();
        } catch (const std::invalid_argument& error) {
            Reject(context_ + "." + std::string(key) + ": " + error.what());
        }
    }

    std::uint64_t Unsigned(const std::string_view key) {
        const auto& member = Require(key);
        try {
            return member.AsUnsigned();
        } catch (const std::invalid_argument& error) {
            Reject(context_ + "." + std::string(key) + ": " + error.what());
        }
    }

    const std::string& String(const std::string_view key) {
        const auto& member = Require(key);
        try {
            return member.AsString();
        } catch (const std::invalid_argument& error) {
            Reject(context_ + "." + std::string(key) + ": " + error.what());
        }
    }

    template <std::size_t Count>
    std::array<double, Count> NumberArray(const std::string_view key) {
        return ToArray<Count>(Require(key), context_ + "." + std::string(key));
    }

    template <std::size_t Count>
    static std::array<double, Count> ToArray(const JsonValue& value, const std::string& context) {
        if (value.kind != JsonValue::Kind::array || value.items.size() != Count) {
            Reject(context + " must be an array of " + std::to_string(Count) + " numbers");
        }
        std::array<double, Count> numbers{};
        for (std::size_t index = 0; index < Count; ++index) {
            try {
                numbers[index] = value.items[index].AsDouble();
            } catch (const std::invalid_argument& error) {
                Reject(context + "[" + std::to_string(index) + "]: " + error.what());
            }
        }
        return numbers;
    }

    // Three rows of three numbers, flattened row-major.
    static std::array<double, 9> ToMatrix3(const JsonValue& value, const std::string& context) {
        if (value.kind != JsonValue::Kind::array || value.items.size() != 3) {
            Reject(context + " must be an array of 3 rows");
        }
        std::array<double, 9> matrix{};
        for (std::size_t row = 0; row < 3; ++row) {
            const auto values = ToArray<3>(value.items[row], context + "[" + std::to_string(row) + "]");
            for (std::size_t column = 0; column < 3; ++column) matrix[row * 3 + column] = values[column];
        }
        return matrix;
    }

private:
    const JsonValue& value_;
    std::string context_;
    std::vector<std::string> used_;
};

void RequireFinitePositive(const double value, const std::string& name) {
    if (!std::isfinite(value) || value <= 0.0) Reject(name + " must be a finite number greater than 0");
}

void RequireFinite(const double value, const std::string& name) {
    if (!std::isfinite(value)) Reject(name + " must be finite");
}

void RequireNonNegative(const double value, const std::string& name) {
    if (!std::isfinite(value) || value < 0.0) Reject(name + " must be a finite number of at least 0");
}

ChromaSubsampling ParseSubsampling(const std::string& text) {
    if (text == "444") return ChromaSubsampling::s444;
    if (text == "422") return ChromaSubsampling::s422;
    if (text == "420") return ChromaSubsampling::s420;
    if (text == "440") return ChromaSubsampling::s440;
    Reject("jpeg.chroma_subsampling must be one of 444, 422, 420, 440");
}

const char* SubsamplingName(const ChromaSubsampling value) noexcept {
    switch (value) {
    case ChromaSubsampling::s444: return "444";
    case ChromaSubsampling::s422: return "422";
    case ChromaSubsampling::s420: return "420";
    case ChromaSubsampling::s440: return "440";
    }
    return nullptr;
}

void ValidateChart(const ChartSpec& chart) {
    RequireFinitePositive(chart.width_mm, "chart.width_mm");
    RequireFinitePositive(chart.height_mm, "chart.height_mm");
    if (chart.width_mm > kMaximumChartExtentMm || chart.height_mm > kMaximumChartExtentMm) {
        Reject("chart size is out of range");
    }
    RequireFinitePositive(chart.grid_pitch_mm, "chart.grid_pitch_mm");
    RequireFinitePositive(chart.grid_line_width_mm, "chart.grid_line_width_mm");
    RequireFinitePositive(chart.fiducial_pitch_mm, "chart.fiducial_pitch_mm");
    RequireFinitePositive(chart.fiducial_radius_mm, "chart.fiducial_radius_mm");
    RequireFinitePositive(chart.fine_line_base_width_mm, "chart.fine_line_base_width_mm");
    RequireNonNegative(chart.margin_mm, "chart.margin_mm");
    RequireNonNegative(chart.slanted_edge_slope, "chart.slanted_edge_slope");
    if (chart.slanted_edge_slope > 0.2) Reject("chart.slanted_edge_slope must be at most 0.2");
    if (chart.margin_mm < 2.0 * chart.fiducial_radius_mm + 4.0 * chart.grid_line_width_mm) {
        Reject("chart.margin_mm must be at least 2 * fiducial_radius_mm + 4 * grid_line_width_mm "
               "so the frame and the outer fiducials fit");
    }
    if (chart.grid_line_width_mm > 0.2 * chart.grid_pitch_mm) {
        Reject("chart.grid_line_width_mm must be at most 0.2 times grid_pitch_mm");
    }
    const double ratio = chart.fiducial_pitch_mm / chart.grid_pitch_mm;
    if (!std::isfinite(ratio) || ratio >= 0x1p62 || std::abs(ratio - std::round(ratio)) > 1e-9 || ratio < 1.0) {
        Reject("chart.fiducial_pitch_mm must be an integer multiple of grid_pitch_mm");
    }
    // Keep every floor-to-int64 grid coordinate, including its rounding margin,
    // representable. Checking only the lattice pitch ratio misses tiny grid pitches.
    const double grid_extent = (std::max(chart.width_mm, chart.height_mm) + chart.fiducial_radius_mm) / chart.grid_pitch_mm;
    if (!std::isfinite(grid_extent) || grid_extent >= 0x1p62) Reject("chart grid coordinates exceed the supported integer range");
    if (chart.fiducial_radius_mm > 0.15 * chart.fiducial_pitch_mm) {
        Reject("chart.fiducial_radius_mm must be at most 0.15 times fiducial_pitch_mm");
    }
    if (chart.fine_line_base_width_mm > chart.fiducial_pitch_mm / 16.0) {
        Reject("chart.fine_line_base_width_mm must be at most fiducial_pitch_mm / 16");
    }
    const double usable_x = (chart.width_mm - 2.0 * chart.margin_mm) / chart.fiducial_pitch_mm;
    const double usable_y = (chart.height_mm - 2.0 * chart.margin_mm) / chart.fiducial_pitch_mm;
    if (!(usable_x >= 1.0) || !(usable_y >= 1.0)) Reject("chart must hold at least one lattice cell in each direction");
    // A tooling resource bound, not an optical-quality threshold. Test before
    // uint32 conversion, +1, reserve or iteration over the lattice.
    constexpr double kMaxReferencePoints = 1000000;
    const double nodes_x = std::floor(usable_x) + 1, nodes_y = std::floor(usable_y) + 1;
    if (!std::isfinite(nodes_x) || !std::isfinite(nodes_y) || nodes_x * nodes_y > kMaxReferencePoints) {
        Reject("chart supports at most 1000000 reference points");
    }
}

std::string UpperAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

// Names Windows treats as devices whatever the extension.
bool IsReservedDeviceName(const std::string& upper) {
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return true;
    if (upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0)) {
        return upper[3] >= '1' && upper[3] <= '9';
    }
    return false;
}

void ValidateAlias(const std::string& alias, const std::string& context) {
    if (alias.empty() || alias.size() > 32) Reject(context + ".alias must be 1 to 32 characters");
    for (std::size_t index = 0; index < alias.size(); ++index) {
        const char c = alias[index];
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!(alnum || (index > 0 && (c == '-' || c == '_')))) {
            Reject(context + ".alias may only use letters, digits, '-' and '_' (not first)");
        }
    }
    if (IsReservedDeviceName(UpperAscii(alias))) {
        Reject(context + ".alias is a reserved Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9)");
    }
}

std::string PixelText(const double u, const double v) {
    char text[64]{};
    std::snprintf(text, sizeof(text), "(%.1f, %.1f)", u, v);
    return text;
}

// Radial reach: the map r -> r * radial(r) must rise monotonically over every
// radius that is needed to reach the farthest corner of the image.
void ValidateLensReach(const CameraSpec& camera, const PairSpec& spec, const std::string& context) {
    const auto& k = camera.intrinsics;
    const double left = -0.5;
    const double top = -0.5;
    const double right = static_cast<double>(spec.image_width_px) - 0.5;
    const double bottom = static_cast<double>(spec.image_height_px) - 0.5;
    double farthest2 = 0.0;
    for (const double u : {left, right}) {
        for (const double v : {top, bottom}) {
            const double x = (u - k.cx_pixels) / k.fx_pixels;
            const double y = (v - k.cy_pixels) / k.fy_pixels;
            farthest2 = std::max(farthest2, x * x + y * y);
        }
    }
    const double fold2 = detail::RadialFoldSquared(camera.distortion);
    if (!std::isfinite(fold2)) return;
    const auto& d = camera.distortion;
    const double reach = std::sqrt(fold2) * (1.0 + fold2 * (d.k1 + fold2 * (d.k2 + fold2 * d.k3)));
    if (!(reach * reach > farthest2)) {
        Reject(context + ".projection.distortion is not monotonic up to the radius that reaches the image corners "
                         "(the radial map folds back at a distorted radius of "
            + std::to_string(reach) + ", the corners need " + std::to_string(std::sqrt(farthest2)) + ")");
    }
}

void ValidateCamera(const CameraSpec& camera, const PairSpec& spec, const std::string& context) {
    ValidateAlias(camera.alias, context);

    const auto& k = camera.intrinsics;
    RequireFinitePositive(k.fx_pixels, context + ".projection.intrinsics.fxPixels");
    RequireFinitePositive(k.fy_pixels, context + ".projection.intrinsics.fyPixels");
    RequireFinite(k.cx_pixels, context + ".projection.intrinsics.cxPixels");
    RequireFinite(k.cy_pixels, context + ".projection.intrinsics.cyPixels");
    const auto& d = camera.distortion;
    RequireFinite(d.k1, context + ".projection.distortion.k1");
    RequireFinite(d.k2, context + ".projection.distortion.k2");
    RequireFinite(d.k3, context + ".projection.distortion.k3");
    RequireFinite(d.p1, context + ".projection.distortion.p1");
    RequireFinite(d.p2, context + ".projection.distortion.p2");

    const auto& h = camera.document_to_image;
    for (std::size_t index = 0; index < h.size(); ++index) {
        RequireFinite(h[index], context + ".projection.documentToImage[" + std::to_string(index / 3) + "]["
            + std::to_string(index % 3) + "]");
    }
    if (h[8] != 1.0) Reject(context + ".projection.documentToImage[2][2] must be 1");
    std::array<double, 9> inverse{};
    if (!detail::InvertMatrix3(h, inverse)) Reject(context + ".projection.documentToImage is not invertible");

    // w' is a linear function of the document position, so the four corners decide the whole chart.
    for (const double x : {0.0, spec.chart.width_mm}) {
        for (const double y : {0.0, spec.chart.height_mm}) {
            if (!(h[6] * x + h[7] * y + h[8] > 0.0)) {
                Reject(context + ".projection.documentToImage puts a chart corner behind the camera (w' <= 0)");
            }
        }
    }

    ValidateLensReach(camera, spec, context);

    // The preimage of the whole image must exist: invert the lens on a grid over the
    // image (corners included) and make sure each point is in front of the camera.
    const double left = -0.5;
    const double top = -0.5;
    const double width = static_cast<double>(spec.image_width_px);
    const double height = static_cast<double>(spec.image_height_px);
    for (int row = 0; row <= kInversionGridSteps; ++row) {
        for (int column = 0; column <= kInversionGridSteps; ++column) {
            const double u = left + width * static_cast<double>(column) / kInversionGridSteps;
            const double v = top + height * static_cast<double>(row) / kInversionGridSteps;
            const bool corner = (row == 0 || row == kInversionGridSteps) && (column == 0 || column == kInversionGridSteps);
            Vec2 ideal;
            if (!detail::UndistortPixel(camera, u, v, ideal)) {
                Reject(context + ".projection.distortion cannot be inverted at the raw pixel " + PixelText(u, v)
                    + (corner ? " (an image corner)" : ""));
            }
            // The third row of the inverse is 1 / w' of the preimage.
            if (!(inverse[6] * ideal.x + inverse[7] * ideal.y + inverse[8] > 0.0)) {
                Reject(context + ".projection puts " + (corner ? "an image corner" : "the raw pixel " + PixelText(u, v))
                    + " behind the camera: the preimage has w' <= 0");
            }
        }
    }

    RequireNonNegative(camera.exposure_gain, context + ".exposure_gain");
    for (std::size_t index = 0; index < camera.white_balance_gain.size(); ++index) {
        RequireNonNegative(camera.white_balance_gain[index], context + ".white_balance_gain[" + std::to_string(index) + "]");
    }
    RequireFinite(camera.vignette[0], context + ".vignette[0]");
    RequireFinite(camera.vignette[1], context + ".vignette[1]");
    RequireNonNegative(camera.noise.read_sigma, context + ".noise.read_sigma");
    RequireNonNegative(camera.noise.shot_sigma, context + ".noise.shot_sigma");
}

// ---- JSON output helpers ---------------------------------------------------

void AppendDouble(std::string& out, const double value) {
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) throw std::runtime_error("number formatting failed");
    out.append(buffer, result.ptr);
}

void AppendUnsigned(std::string& out, const std::uint64_t value) { out += std::to_string(value); }

void AppendString(std::string& out, const std::string_view value) {
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                char escape[8]{};
                std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
                out += escape;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void AppendVec2(std::string& out, const Vec2 value) {
    out.push_back('[');
    AppendDouble(out, value.x);
    out.push_back(',');
    AppendDouble(out, value.y);
    out.push_back(']');
}

template <std::size_t Count>
void AppendArray(std::string& out, const std::array<double, Count>& values) {
    out.push_back('[');
    for (std::size_t index = 0; index < Count; ++index) {
        if (index > 0) out.push_back(',');
        AppendDouble(out, values[index]);
    }
    out.push_back(']');
}

void AppendMatrix3(std::string& out, const std::array<double, 9>& values) {
    out.push_back('[');
    for (std::size_t row = 0; row < 3; ++row) {
        if (row > 0) out.push_back(',');
        out.push_back('[');
        for (std::size_t column = 0; column < 3; ++column) {
            if (column > 0) out.push_back(',');
            AppendDouble(out, values[row * 3 + column]);
        }
        out.push_back(']');
    }
    out.push_back(']');
}

void AppendKey(std::string& out, const std::string_view key) {
    AppendString(out, key);
    out.push_back(':');
}

void AppendNamedDouble(std::string& out, const std::string_view key, const double value) {
    AppendKey(out, key);
    AppendDouble(out, value);
}

IntrinsicsSpec ParseIntrinsics(const JsonValue& value, const std::string& context) {
    ObjectReader reader(value, context);
    IntrinsicsSpec intrinsics;
    intrinsics.fx_pixels = reader.Number("fxPixels");
    intrinsics.fy_pixels = reader.Number("fyPixels");
    intrinsics.cx_pixels = reader.Number("cxPixels");
    intrinsics.cy_pixels = reader.Number("cyPixels");
    reader.Finish();
    return intrinsics;
}

DistortionSpec ParseDistortion(const JsonValue& value, const std::string& context) {
    ObjectReader reader(value, context);
    if (reader.String("model") != kDistortionModel) {
        Reject(context + ".model must be \"" + std::string(kDistortionModel) + "\"");
    }
    DistortionSpec distortion;
    distortion.k1 = reader.Number("k1");
    distortion.k2 = reader.Number("k2");
    distortion.k3 = reader.Number("k3");
    distortion.p1 = reader.Number("p1");
    distortion.p2 = reader.Number("p2");
    reader.Finish();
    return distortion;
}

} // namespace

std::array<double, 9> HomographyFromPlacement(
    const PlacementSpec& placement, const std::uint32_t image_width_px, const std::uint32_t image_height_px) {
    static constexpr std::array<std::array<double, 4>, 4> kRotations{{
        {1.0, 0.0, 0.0, 1.0},
        {0.0, -1.0, 1.0, 0.0},
        {-1.0, 0.0, 0.0, -1.0},
        {0.0, 1.0, -1.0, 0.0},
    }};
    if (placement.quarter_turns < 0 || placement.quarter_turns > 3) {
        throw std::invalid_argument("synthetic pair spec: quarter_turns must be 0 to 3");
    }
    if (!std::isfinite(placement.pixels_per_mm) || placement.pixels_per_mm <= 0.0
        || !std::isfinite(placement.center_mm.x) || !std::isfinite(placement.center_mm.y)) {
        throw std::invalid_argument("synthetic pair spec: placement values must be finite and pixels_per_mm positive");
    }
    const auto& r = kRotations[static_cast<std::size_t>(placement.quarter_turns)];
    const double s = placement.pixels_per_mm;
    const double a = s * r[0];
    const double b = s * r[1];
    const double c = s * r[2];
    const double d = s * r[3];
    // The sample of pixel i sits at i, so the middle of the sample grid is (size - 1) / 2.
    const double centre_u = 0.5 * (static_cast<double>(image_width_px) - 1.0);
    const double centre_v = 0.5 * (static_cast<double>(image_height_px) - 1.0);
    const double tx = centre_u - (a * placement.center_mm.x + b * placement.center_mm.y);
    const double ty = centre_v - (c * placement.center_mm.x + d * placement.center_mm.y);
    return {a, b, tx, c, d, ty, 0.0, 0.0, 1.0};
}

std::optional<Vec2> ProjectDocumentPoint(const CameraSpec& camera, const Vec2 document_mm) {
    const auto& h = camera.document_to_image;
    const double w = h[6] * document_mm.x + h[7] * document_mm.y + h[8];
    if (!(w > 0.0)) return std::nullopt;
    const double u = (h[0] * document_mm.x + h[1] * document_mm.y + h[2]) / w;
    const double v = (h[3] * document_mm.x + h[4] * document_mm.y + h[5]) / w;
    const auto& k = camera.intrinsics;
    const double x = (u - k.cx_pixels) / k.fx_pixels;
    const double y = (v - k.cy_pixels) / k.fy_pixels;
    // Beyond the fold radius the radial map comes back toward the axis: such a
    // point is not seen at the position the formula gives.
    if (x * x + y * y > detail::RadialFoldSquared(camera.distortion)) return std::nullopt;
    const auto distorted = detail::DistortNormalized(camera.distortion, x, y);
    return Vec2{k.fx_pixels * distorted.x + k.cx_pixels, k.fy_pixels * distorted.y + k.cy_pixels};
}

std::optional<Vec2> UnprojectImagePoint(const CameraSpec& camera, const Vec2 image_px) {
    std::array<double, 9> inverse{};
    if (!detail::InvertMatrix3(camera.document_to_image, inverse)) return std::nullopt;
    Vec2 ideal;
    if (!detail::UndistortPixel(camera, image_px.x, image_px.y, ideal)) return std::nullopt;
    const double w = inverse[6] * ideal.x + inverse[7] * ideal.y + inverse[8];
    if (!(w > 0.0)) return std::nullopt;
    return Vec2{(inverse[0] * ideal.x + inverse[1] * ideal.y + inverse[2]) / w,
        (inverse[3] * ideal.x + inverse[4] * ideal.y + inverse[5]) / w};
}

void ValidatePairSpec(const PairSpec& spec) {
    if (spec.image_width_px == 0 || spec.image_height_px == 0 || spec.image_width_px > kMaximumImageDimension
        || spec.image_height_px > kMaximumImageDimension) {
        Reject("image.width_px and height_px must be 1 to " + std::to_string(kMaximumImageDimension));
    }
    if (spec.supersample == 0 || spec.supersample > 8) Reject("image.supersample must be 1 to 8");
    if (!(spec.jpeg.quality > 0.0 && spec.jpeg.quality <= 1.0)) Reject("jpeg.quality must be in (0, 1]");
    if (SubsamplingName(spec.jpeg.chroma_subsampling) == nullptr) {
        Reject("jpeg.chroma_subsampling must be one of 444, 422, 420, 440");
    }
    ValidateChart(spec.chart);

    if (spec.cameras.empty() || spec.cameras.size() > kMaximumCameraCount) {
        Reject("spec.cameras must be an array of 1 to " + std::to_string(kMaximumCameraCount) + " cameras");
    }
    std::vector<std::string> seen;
    for (std::size_t index = 0; index < spec.cameras.size(); ++index) {
        const std::string context = "spec.cameras[" + std::to_string(index) + "]";
        const auto& camera = spec.cameras[index];
        ValidateCamera(camera, spec, context);
        const std::string upper = UpperAscii(camera.alias);
        if (std::find(seen.begin(), seen.end(), upper) != seen.end()) {
            Reject(context + ".alias duplicates another camera (aliases are compared without regard to case)");
        }
        seen.push_back(upper);
    }
}

PairSpec ParsePairSpec(const std::string_view json_text) {
    JsonValue document;
    try {
        document = ParseJson(json_text);
    } catch (const std::invalid_argument& error) {
        Reject(error.what());
    }
    return ParsePairSpec(document);
}

PairSpec ParsePairSpec(const JsonValue& document) {
    PairSpec spec;
    ObjectReader root(document, "spec");
    const auto& schema = root.String("schema");
    if (schema == "a0.m2.synthetic-pair-spec/1") {
        Reject("schema \"a0.m2.synthetic-pair-spec/1\" is no longer accepted: ADR-0034 changed the pixel convention "
               "(the sample of pixel (i, j) is at (i, j)) and the camera fields (projection.intrinsics, "
               "projection.distortion, projection.documentToImage); write the spec as \""
            + std::string(kSpecSchemaVersion) + "\"");
    }
    if (schema != kSpecSchemaVersion) {
        Reject("schema must be \"" + std::string(kSpecSchemaVersion) + "\"");
    }
    spec.seed = root.Unsigned("seed");

    ObjectReader image(root.Require("image"), "spec.image");
    const auto width = image.Unsigned("width_px");
    const auto height = image.Unsigned("height_px");
    const auto supersample = image.Unsigned("supersample");
    image.Finish();
    if (width == 0 || height == 0 || width > kMaximumImageDimension || height > kMaximumImageDimension) {
        Reject("image.width_px and height_px must be 1 to " + std::to_string(kMaximumImageDimension));
    }
    if (supersample == 0 || supersample > 8) Reject("image.supersample must be 1 to 8");
    spec.image_width_px = static_cast<std::uint32_t>(width);
    spec.image_height_px = static_cast<std::uint32_t>(height);
    spec.supersample = static_cast<std::uint32_t>(supersample);

    ObjectReader jpeg(root.Require("jpeg"), "spec.jpeg");
    spec.jpeg.quality = jpeg.Number("quality");
    spec.jpeg.chroma_subsampling = ParseSubsampling(jpeg.String("chroma_subsampling"));
    jpeg.Finish();

    ObjectReader chart(root.Require("chart"), "spec.chart");
    spec.chart.width_mm = chart.Number("width_mm");
    spec.chart.height_mm = chart.Number("height_mm");
    spec.chart.margin_mm = chart.Number("margin_mm");
    spec.chart.grid_pitch_mm = chart.Number("grid_pitch_mm");
    spec.chart.grid_line_width_mm = chart.Number("grid_line_width_mm");
    spec.chart.fiducial_pitch_mm = chart.Number("fiducial_pitch_mm");
    spec.chart.fiducial_radius_mm = chart.Number("fiducial_radius_mm");
    spec.chart.slanted_edge_slope = chart.Number("slanted_edge_slope");
    spec.chart.fine_line_base_width_mm = chart.Number("fine_line_base_width_mm");
    chart.Finish();

    const auto& cameras = root.Require("cameras");
    if (cameras.kind != JsonValue::Kind::array || cameras.items.empty() || cameras.items.size() > kMaximumCameraCount) {
        Reject("spec.cameras must be an array of 1 to " + std::to_string(kMaximumCameraCount) + " cameras");
    }
    for (std::size_t index = 0; index < cameras.items.size(); ++index) {
        const std::string context = "spec.cameras[" + std::to_string(index) + "]";
        ObjectReader reader(cameras.items[index], context);
        CameraSpec camera;
        camera.alias = reader.String("alias");

        ObjectReader projection(reader.Require("projection"), context + ".projection");
        camera.intrinsics = ParseIntrinsics(projection.Require("intrinsics"), context + ".projection.intrinsics");
        camera.distortion = ParseDistortion(projection.Require("distortion"), context + ".projection.distortion");
        const auto* matrix = projection.Optional("documentToImage");
        const auto* placement = projection.Optional("placement");
        if ((matrix == nullptr) == (placement == nullptr)) {
            Reject(context + ".projection needs exactly one of \"documentToImage\" and \"placement\"");
        }
        if (matrix != nullptr) {
            camera.document_to_image = ObjectReader::ToMatrix3(*matrix, context + ".projection.documentToImage");
        } else {
            ObjectReader place(*placement, context + ".projection.placement");
            PlacementSpec parsed;
            const auto center = place.NumberArray<2>("center_mm");
            parsed.center_mm = {center[0], center[1]};
            parsed.pixels_per_mm = place.Number("pixels_per_mm");
            const auto turns = place.Unsigned("quarter_turns");
            place.Finish();
            if (turns > 3) Reject(context + ".projection.placement.quarter_turns must be 0 to 3");
            parsed.quarter_turns = static_cast<int>(turns);
            camera.document_to_image = HomographyFromPlacement(parsed, spec.image_width_px, spec.image_height_px);
        }
        projection.Finish();

        camera.exposure_gain = reader.Number("exposure_gain");
        camera.white_balance_gain = reader.NumberArray<3>("white_balance_gain");
        camera.vignette = reader.NumberArray<2>("vignette");
        ObjectReader noise(reader.Require("noise"), context + ".noise");
        camera.noise.read_sigma = noise.Number("read_sigma");
        camera.noise.shot_sigma = noise.Number("shot_sigma");
        noise.Finish();
        reader.Finish();
        spec.cameras.push_back(std::move(camera));
    }
    root.Finish();
    ValidatePairSpec(spec);
    return spec;
}

std::string SerializePairSpec(const PairSpec& spec) {
    std::string out;
    out += "{\n";
    out += "\"schema\":";
    AppendString(out, kSpecSchemaVersion);
    out += ",\n\"seed\":";
    AppendUnsigned(out, spec.seed);
    out += ",\n\"image\":{\"width_px\":";
    AppendUnsigned(out, spec.image_width_px);
    out += ",\"height_px\":";
    AppendUnsigned(out, spec.image_height_px);
    out += ",\"supersample\":";
    AppendUnsigned(out, spec.supersample);
    out += "},\n\"jpeg\":{\"quality\":";
    AppendDouble(out, spec.jpeg.quality);
    out += ",\"chroma_subsampling\":";
    const char* subsampling = SubsamplingName(spec.jpeg.chroma_subsampling);
    AppendString(out, subsampling != nullptr ? subsampling : "");
    out += "},\n\"chart\":{";
    const auto& chart = spec.chart;
    const std::pair<const char*, double> chart_fields[] = {
        {"width_mm", chart.width_mm}, {"height_mm", chart.height_mm}, {"margin_mm", chart.margin_mm},
        {"grid_pitch_mm", chart.grid_pitch_mm}, {"grid_line_width_mm", chart.grid_line_width_mm},
        {"fiducial_pitch_mm", chart.fiducial_pitch_mm}, {"fiducial_radius_mm", chart.fiducial_radius_mm},
        {"slanted_edge_slope", chart.slanted_edge_slope}, {"fine_line_base_width_mm", chart.fine_line_base_width_mm},
    };
    bool first = true;
    for (const auto& [name, value] : chart_fields) {
        if (!first) out.push_back(',');
        first = false;
        AppendNamedDouble(out, name, value);
    }
    out += "},\n\"cameras\":[";
    for (std::size_t index = 0; index < spec.cameras.size(); ++index) {
        const auto& camera = spec.cameras[index];
        if (index > 0) out += ",";
        out += "\n{\"alias\":";
        AppendString(out, camera.alias);
        out += ",\"projection\":{\"intrinsics\":{";
        AppendNamedDouble(out, "fxPixels", camera.intrinsics.fx_pixels);
        out.push_back(',');
        AppendNamedDouble(out, "fyPixels", camera.intrinsics.fy_pixels);
        out.push_back(',');
        AppendNamedDouble(out, "cxPixels", camera.intrinsics.cx_pixels);
        out.push_back(',');
        AppendNamedDouble(out, "cyPixels", camera.intrinsics.cy_pixels);
        out += "},\"distortion\":{\"model\":";
        AppendString(out, kDistortionModel);
        out.push_back(',');
        AppendNamedDouble(out, "k1", camera.distortion.k1);
        out.push_back(',');
        AppendNamedDouble(out, "k2", camera.distortion.k2);
        out.push_back(',');
        AppendNamedDouble(out, "k3", camera.distortion.k3);
        out.push_back(',');
        AppendNamedDouble(out, "p1", camera.distortion.p1);
        out.push_back(',');
        AppendNamedDouble(out, "p2", camera.distortion.p2);
        out += "},\"documentToImage\":";
        AppendMatrix3(out, camera.document_to_image);
        out += "},\"exposure_gain\":";
        AppendDouble(out, camera.exposure_gain);
        out += ",\"white_balance_gain\":";
        AppendArray(out, camera.white_balance_gain);
        out += ",\"vignette\":";
        AppendArray(out, camera.vignette);
        out += ",\"noise\":{\"read_sigma\":";
        AppendDouble(out, camera.noise.read_sigma);
        out += ",\"shot_sigma\":";
        AppendDouble(out, camera.noise.shot_sigma);
        out += "}}";
    }
    out += "\n]\n}";
    return out;
}

std::string SerializeGroundTruth(const PairSpec& spec, const std::vector<OutputFile>& output_files) {
    std::string out;
    out += "{\n\"schema\":";
    AppendString(out, kGroundTruthSchemaVersion);
    out += ",\n\"generator_version\":";
    AppendString(out, kGeneratorVersion);
    out += ",\n\"seed\":";
    AppendUnsigned(out, spec.seed);
    out += ",\n\"conventions\":{\"documentPlane\":";
    AppendString(out, kConventionDocumentPlane);
    out += ",\"cameraPixel\":";
    AppendString(out, kConventionCameraPixel);
    out += ",\"matrix\":";
    AppendString(out, kConventionMatrix);
    out += ",\"exifOrientation\":";
    AppendString(out, kConventionExifOrientation);
    out += "},\n\"spec\":";
    out += SerializePairSpec(spec);
    out += ",\n\"output_files\":[";
    for (std::size_t index = 0; index < output_files.size(); ++index) {
        const auto& file = output_files[index];
        if (index > 0) out += ",";
        out += "\n{\"alias\":";
        AppendString(out, file.alias);
        out += ",\"path\":";
        AppendString(out, file.relative_path);
        out += ",\"sha256\":";
        AppendString(out, file.sha256);
        out += ",\"size_bytes\":";
        AppendUnsigned(out, file.size_bytes);
        out += "}";
    }
    out += "\n],\n\"reference_points\":[";
    const auto points = ListReferencePoints(spec.chart);
    const double image_width = static_cast<double>(spec.image_width_px);
    const double image_height = static_cast<double>(spec.image_height_px);
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        if (index > 0) out += ",";
        out += "\n{\"id\":";
        AppendString(out, point.id);
        out += ",\"document_mm\":";
        AppendVec2(out, point.document_mm);
        out += ",\"image_px\":{";
        for (std::size_t camera_index = 0; camera_index < spec.cameras.size(); ++camera_index) {
            const auto& camera = spec.cameras[camera_index];
            if (camera_index > 0) out.push_back(',');
            AppendKey(out, camera.alias);
            const auto projected = ProjectDocumentPoint(camera, point.document_mm);
            // Coverage rule of the rig profile v2: 0 <= u < width, 0 <= v < height.
            const bool inside = projected && projected->x >= 0.0 && projected->x < image_width
                && projected->y >= 0.0 && projected->y < image_height;
            if (inside) AppendVec2(out, *projected);
            else out += "null";
        }
        out += "}}";
    }
    out += "\n],\n\"patterns\":[";
    const auto patterns = ListChartPatterns(spec.chart);
    for (std::size_t index = 0; index < patterns.size(); ++index) {
        const auto& pattern = patterns[index];
        if (index > 0) out += ",";
        out += "\n{\"kind\":";
        AppendString(out, pattern.kind);
        out += ",\"cell\":[";
        AppendUnsigned(out, pattern.column);
        out.push_back(',');
        AppendUnsigned(out, pattern.row);
        out += "],\"center_mm\":";
        AppendVec2(out, pattern.center_mm);
        out += ",\"half_size_mm\":";
        AppendDouble(out, pattern.half_size_mm);
        out += "}";
    }
    out += "\n]\n}\n";
    return out;
}

} // namespace a0::m2::synthetic
