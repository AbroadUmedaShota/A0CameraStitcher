#pragma once

// Ground-truth synthetic image pair generator (GitHub Issue #269).
//
// Draws an in-house chart (grid, ring fiducials, slanted edges, fine lines,
// colour and grey patches) from code and projects it through per-camera
// models: document (mm) -> undistorted image pixel by a 3x3 homography,
// Brown-Conrady distortion on normalized coordinates, exposure gain, white
// balance gain, vignetting and seeded noise. The output is a canonical JPEG per
// camera plus a ground truth JSON that records where every reference point
// really is in each image.
//
// Coordinate conventions are those of ADR-0034 and docs/design/rig-profile-v2.md:
// document in mm with the origin at the top-left and y downward, camera pixels
// in stored order with the sample of pixel (i, j) at (i, j), matrices row-major
// with column vectors. The per-camera projection has the same names and shape
// as `cameras.*.projection` of docs/schemas/rig-profile.v2.schema.json.
//
// This library deliberately does not depend on the product warp/blend
// (a0_m2_offline_stitcher); CMakeLists.txt checks that at configure time.
// It is a test and calibration aid, not a decoder for real camera originals.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a0/m2/synthetic_pair_json.hpp"

namespace a0::m2::synthetic {

// The /1 formats are not accepted: ADR-0034 moved the pixel sample point from
// i + 0.5 to i and replaced the lens fields.
inline constexpr std::string_view kGeneratorVersion = "a0.m2.synthetic-pair-generator/2";
inline constexpr std::string_view kSpecSchemaVersion = "a0.m2.synthetic-pair-spec/2";
inline constexpr std::string_view kGroundTruthSchemaVersion = "a0.m2.synthetic-pair-ground-truth/2";
inline constexpr std::string_view kGroundTruthFileName = "ground-truth.json";
inline constexpr std::string_view kCanonicalFileName = "original.jpg";

// The four constants of `conventions` in the rig profile v2 schema.
inline constexpr std::string_view kConventionDocumentPlane = "mm-origin-sheet-top-left-x-right-y-down";
inline constexpr std::string_view kConventionCameraPixel = "stored-order-sample-at-integer-index";
inline constexpr std::string_view kConventionMatrix = "row-major-column-vector";
inline constexpr std::string_view kConventionExifOrientation = "ignored";

// `distortion.model` of the rig profile v2 schema.
inline constexpr std::string_view kDistortionModel = "brown-conrady-k1k2k3-p1p2";

struct Vec2 {
    double x{};
    double y{};
};

struct Rgb {
    double r{};
    double g{};
    double b{};
};

// Every length is in millimetres on the document plane (origin top-left,
// x to the right, y downward). There are no defaults: the spec supplies all of it.
struct ChartSpec {
    double width_mm{};
    double height_mm{};
    double margin_mm{};
    double grid_pitch_mm{};
    double grid_line_width_mm{};
    // Ring fiducials sit on a lattice starting at margin_mm. Their centres are
    // the reference points of the ground truth. Must be an integer multiple of
    // grid_pitch_mm.
    double fiducial_pitch_mm{};
    double fiducial_radius_mm{};
    // Slanted edge squares are rotated by atan(slope).
    double slanted_edge_slope{};
    // Fine line blocks use line widths of 1x to 4x this value.
    double fine_line_base_width_mm{};
};

enum class ChromaSubsampling { s444, s422, s420, s440 };

struct JpegSpec {
    double quality{};
    ChromaSubsampling chroma_subsampling{ChromaSubsampling::s444};
};

// Convenience form of a homography for an upright or quarter-turned camera.
// undistorted pixel = pixels_per_mm * R(quarter_turns) * (document - center_mm)
//                     + ((width - 1) / 2, (height - 1) / 2)
// with R(0) = [[1,0],[0,1]], R(1) = [[0,-1],[1,0]], R(2) = [[-1,0],[0,-1]],
// R(3) = [[0,1],[-1,0]]. The point `center_mm` lands on the centre of the
// sample grid, which is at (width - 1) / 2 because the sample of pixel i is at i.
struct PlacementSpec {
    Vec2 center_mm;
    double pixels_per_mm{};
    int quarter_turns{};
};

// Same fields as `projection.intrinsics` of the rig profile v2 schema. skew is 0.
struct IntrinsicsSpec {
    double fx_pixels{};
    double fy_pixels{};
    double cx_pixels{};
    double cy_pixels{};
};

// Brown-Conrady on normalized coordinates x = (u - cx) / fx, y = (v - cy) / fy:
//   r2 = x^2 + y^2, radial = 1 + k1 r2 + k2 r2^2 + k3 r2^3
//   xd = x radial + 2 p1 x y + p2 (r2 + 2 x^2)
//   yd = y radial + p1 (r2 + 2 y^2) + 2 p2 x y
// followed by u_raw = fx xd + cx, v_raw = fy yd + cy. All five coefficients are
// required by the spec; a lens without tangential terms writes 0 explicitly.
struct DistortionSpec {
    double k1{};
    double k2{};
    double k3{};
    double p1{};
    double p2{};
};

// Noise sigma per channel, in units of full scale (1.0 = 255):
// sigma = read_sigma + shot_sigma * sqrt(signal).
struct NoiseSpec {
    double read_sigma{};
    double shot_sigma{};
};

struct CameraSpec {
    // 1 to 32 characters of letters, digits, '-' and '_' (not first). Names the
    // output folder, so it cannot be a Windows reserved device name and two
    // aliases that differ only by case count as the same alias.
    std::string alias;
    IntrinsicsSpec intrinsics;
    DistortionSpec distortion;
    // Document (mm) -> undistorted image pixel, row-major, column-vector:
    // [u', v', w']^T = H [X, Y, 1]^T, (u, v) = (u' / w', v' / w'). Always
    // resolved: when the spec gave a placement, the matrix derived from it is
    // stored here. document_to_image[8] is exactly 1.
    std::array<double, 9> document_to_image{};
    double exposure_gain{};
    std::array<double, 3> white_balance_gain{};  // R, G, B
    // Falloff factor 1 + v1 rho^2 + v2 rho^4 with rho^2 = ((i - cx) / fx)^2 + ((j - cy) / fy)^2
    // at the raw pixel position (i, j).
    std::array<double, 2> vignette{};
    NoiseSpec noise;
};

struct PairSpec {
    std::uint64_t seed{};
    std::uint32_t image_width_px{};
    std::uint32_t image_height_px{};
    // Samples per pixel edge (supersample^2 chart samples per pixel).
    std::uint32_t supersample{};
    JpegSpec jpeg;
    ChartSpec chart;
    std::vector<CameraSpec> cameras;
};

// Checks every rule of a spec that a parsed document or a hand built C++ value
// can break: ranges, the chart layout, aliases, matrices and the lens. For each
// camera it also checks that the whole image can be inverted: the four chart
// corners and the preimages of the image corners and of a grid over the image
// lie in front of the camera (w' > 0), the radial map is monotonic up to the
// radius that reaches the image corners, and the Newton inversion of the
// distortion converges over the image. Throws std::invalid_argument.
// ParsePairSpec, CameraRenderer and GeneratePair call it themselves.
void ValidatePairSpec(const PairSpec& spec);

// Parses and validates a spec document. Throws std::invalid_argument on a
// missing or unknown key, a value out of range, or an inconsistent chart.
[[nodiscard]] PairSpec ParsePairSpec(std::string_view json_text);
[[nodiscard]] PairSpec ParsePairSpec(const JsonValue& document);
// Canonical JSON of the spec with every placement resolved. ParsePairSpec of
// this text gives back an equal spec.
[[nodiscard]] std::string SerializePairSpec(const PairSpec& spec);

[[nodiscard]] std::array<double, 9> HomographyFromPlacement(
    const PlacementSpec& placement, std::uint32_t image_width_px, std::uint32_t image_height_px);

// Analytic forward model. Returns the raw (distorted) pixel position of a
// document point, or no value when the point is behind the camera (w' <= 0) or
// farther from the optical axis than the radius where the radial map folds back
// (such a point is not imaged at that position).
[[nodiscard]] std::optional<Vec2> ProjectDocumentPoint(const CameraSpec& camera, Vec2 document_mm);

// Inverse of ProjectDocumentPoint for a raw pixel position (continuous
// coordinates, pixel (i, j) has its sample at (i, j)). Returns no value when
// the distortion cannot be inverted to 1e-7 px or the point is behind the camera.
[[nodiscard]] std::optional<Vec2> UnprojectImagePoint(const CameraSpec& camera, Vec2 image_px);

struct ReferencePoint {
    std::string id;
    std::uint32_t column{};
    std::uint32_t row{};
    Vec2 document_mm;
};

struct ChartPattern {
    std::string kind;
    std::uint32_t column{};
    std::uint32_t row{};
    Vec2 center_mm;
    double half_size_mm{};
};

[[nodiscard]] std::vector<ReferencePoint> ListReferencePoints(const ChartSpec& chart);
[[nodiscard]] std::vector<ChartPattern> ListChartPatterns(const ChartSpec& chart);
// Chart colour at a document position, values in 0..1. Outside the document
// it returns the table colour.
[[nodiscard]] Rgb SampleChart(const ChartSpec& chart, Vec2 document_mm);

// Draws one camera. Reads only the spec, holds no mutable state, and gives the
// same bytes regardless of how rows are split or which thread renders them.
// Raw pixel (i, j) is the mean of supersample^2 chart samples at
// i - 0.5 + (s + 0.5) / S (same for j), so it covers [i - 0.5, i + 0.5).
// Throws std::invalid_argument for an invalid spec and std::runtime_error when
// the lens cannot be inverted at a sample.
class CameraRenderer final {
public:
    CameraRenderer(const PairSpec& spec, std::size_t camera_index);

    // Writes row_count rows of 24 bit BGR starting at first_row.
    void RenderRows(std::uint32_t first_row, std::uint32_t row_count, std::uint8_t* bgr_output) const;

private:
    const PairSpec* spec_;
    std::size_t camera_index_;
    std::array<double, 9> image_to_document_{};
    std::uint64_t camera_key_{};
};

struct OutputFile {
    std::string alias;
    std::string relative_path;  // forward slashes, relative to the output directory
    std::string sha256;         // lowercase hex of the file bytes
    std::uint64_t size_bytes{};
};

// Serialises the ground truth. output_files may be empty (then the JSON lists
// no files); the reference points and projections do not depend on it. A
// reference point is recorded for a camera when its raw position satisfies
// 0 <= u < width and 0 <= v < height (the coverage rule of the rig profile v2),
// and is null otherwise.
[[nodiscard]] std::string SerializeGroundTruth(const PairSpec& spec, const std::vector<OutputFile>& output_files);

struct GenerateOptions {
    std::filesystem::path output_directory;
    // 0 selects std::thread::hardware_concurrency(). The output does not depend on it.
    unsigned thread_count{};
};

struct GenerateResult {
    std::vector<OutputFile> files;
    std::string ground_truth_relative_path;
    std::string ground_truth_sha256;
    std::uint64_t elapsed_milliseconds{};
    std::uint64_t peak_working_set_bytes{};
};

// Writes <output>/<alias>/original.jpg for each camera and <output>/ground-truth.json.
// Every file is first written next to its destination as <name>.partial; only
// when all of them are complete are the images moved into place, and the
// ground truth last. A reader that finds ground-truth.json therefore finds
// complete images. When anything fails, the files already moved are removed
// again and the .partial files are deleted. Refuses to overwrite an existing
// file. Throws std::invalid_argument for bad options or spec and
// std::runtime_error for I/O or encoder failures.
[[nodiscard]] GenerateResult GeneratePair(const PairSpec& spec, const GenerateOptions& options);

[[nodiscard]] std::string Sha256Hex(const std::filesystem::path& file);

} // namespace a0::m2::synthetic
