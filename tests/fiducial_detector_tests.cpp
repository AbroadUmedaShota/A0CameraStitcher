// Contracts for the development-only fiducial detector (Issue #310, #277 stage 3b).
//
// Images come from the synthetic pair generator (#269). The generator's ground
// truth is used only by this test, to measure the detector: the detector
// library and executable never see it (the build checks that they do not link
// the generator). The one thing the test hands the detector is what an operator
// would: for three rings, the lattice node and a rough pixel position with a
// search radius. These anchors are rounded coarsely, they are written to the
// detection report, and the detector's tolerances for them are test inputs.
//
// Order of the measurements: no lens distortion, uncompressed -> lens
// distortion, uncompressed -> lens distortion, noise and JPEG compression ->
// the detected observations as an observation session through the lens
// estimator (#290), compared with the true mapping.
//
// Ordinary contracts use a throw-away system temp folder. The explicit full
// calibration mode retains synthetic artifacts in a new caller-named folder.
// No camera is touched.

#include "a0/m2/calibration_lens.hpp"
#include "a0/m2/calibration_observation_session.hpp"
#include "a0/m2/fiducial_detector.hpp"
#include "a0/m2/synthetic_pair.hpp"
#ifdef A0_TEST_DRAFT_CALIBRATOR
#include "a0/m2/draft_calibration_artifacts.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#endif

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace synth = a0::m2::synthetic;
namespace fid = a0::m2::fiducial;
namespace cal = a0::m2::calibration;

namespace {

int checks = 0;
int failures = 0;

void Check(const bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void Info(const std::string& message) { std::cout << "info: " << message << '\n'; }

std::string Fixed(const double value, const int digits = 4) {
    char text[48]{};
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read a test file");
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Test inputs. These are the tolerances of this test, not product thresholds.

struct Tolerances {
    double rms_px;
    double max_px;
};
// Detected centre against the true projected centre of the same ring. Looser
// for distortion (the ring's centroid is not the image of its centre) and for
// the lossy stages.
constexpr Tolerances kUndistorted{0.08, 0.20};
constexpr Tolerances kDistorted{0.08, 0.20};
constexpr Tolerances kNoisyJpeg{0.09, 0.25};
constexpr Tolerances kHarshJpeg{0.12, 0.30};
// Rings farther than this from every image edge must be found.
constexpr double kRequiredEdgeMarginPx = 60.0;
// Mapping against the truth, over the lattice nodes inside the image.
constexpr Tolerances kMapping{0.05, 0.15};

// ---------------------------------------------------------------------------
// Camera model and views.

struct Intrinsics { double fx, fy, cx, cy; };
struct Dist { double k1, k2, k3, p1, p2; };
struct ViewPose {
    double tilt_x_deg;
    double tilt_y_deg;
    double roll_deg;
    double center_x_mm; // the sheet point on the optical axis
    double center_y_mm;
    double distance_mm;
};

constexpr std::uint32_t kWidth = 1800;
constexpr std::uint32_t kHeight = 1200;

std::array<double, 9> Multiply(const std::array<double, 9>& a, const std::array<double, 9>& b) {
    std::array<double, 9> r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) r[static_cast<std::size_t>(i * 3 + j)] += a[static_cast<std::size_t>(i * 3 + k)] * b[static_cast<std::size_t>(k * 3 + j)];
        }
    }
    return r;
}

struct Pose {
    std::array<double, 9> r;
    std::array<double, 3> t;
};

Pose MakePose(const ViewPose& v) {
    constexpr double kDegrees = 3.14159265358979323846 / 180.0;
    const double ax = v.tilt_x_deg * kDegrees;
    const double ay = v.tilt_y_deg * kDegrees;
    const double az = v.roll_deg * kDegrees;
    const std::array<double, 9> rx{1, 0, 0, 0, std::cos(ax), -std::sin(ax), 0, std::sin(ax), std::cos(ax)};
    const std::array<double, 9> ry{std::cos(ay), 0, std::sin(ay), 0, 1, 0, -std::sin(ay), 0, std::cos(ay)};
    const std::array<double, 9> rz{std::cos(az), -std::sin(az), 0, std::sin(az), std::cos(az), 0, 0, 0, 1};
    Pose pose{};
    pose.r = Multiply(rz, Multiply(ry, rx));
    // The sheet point (center) lies on the optical axis at the given distance.
    const double px = pose.r[0] * v.center_x_mm + pose.r[1] * v.center_y_mm;
    const double py = pose.r[3] * v.center_x_mm + pose.r[4] * v.center_y_mm;
    const double pz = pose.r[6] * v.center_x_mm + pose.r[7] * v.center_y_mm;
    pose.t = {-px, -py, v.distance_mm - pz};
    return pose;
}

// Document (mm) -> undistorted pixel: K [r1 r2 t], scaled so that [2][2] is 1.
std::array<double, 9> DocumentToImage(const Intrinsics& k, const Pose& pose) {
    const std::array<double, 9> m{pose.r[0], pose.r[1], pose.t[0], pose.r[3], pose.r[4], pose.t[1], pose.r[6], pose.r[7], pose.t[2]};
    const std::array<double, 9> kk{k.fx, 0, k.cx, 0, k.fy, k.cy, 0, 0, 1};
    auto h = Multiply(kk, m);
    const double scale = h[8];
    for (auto& value : h) value /= scale;
    h[8] = 1.0;
    return h;
}

struct CameraModel {
    Intrinsics k;
    Dist d;
};

synth::CameraSpec MakeCamera(const std::string& alias, const CameraModel& model, const ViewPose& view, const bool nuisance) {
    synth::CameraSpec camera;
    camera.alias = alias;
    camera.intrinsics = {model.k.fx, model.k.fy, model.k.cx, model.k.cy};
    camera.distortion = {model.d.k1, model.d.k2, model.d.k3, model.d.p1, model.d.p2};
    camera.document_to_image = DocumentToImage(model.k, MakePose(view));
    camera.exposure_gain = nuisance ? 1.04 : 1.0;
    camera.white_balance_gain = nuisance ? std::array<double, 3>{1.03, 1.0, 0.97} : std::array<double, 3>{1.0, 1.0, 1.0};
    camera.vignette = nuisance ? std::array<double, 2>{-0.18, 0.04} : std::array<double, 2>{0.0, 0.0};
    camera.noise = nuisance ? synth::NoiseSpec{0.004, 0.006} : synth::NoiseSpec{0.0, 0.0};
    return camera;
}

synth::PairSpec MakeSpec(std::vector<synth::CameraSpec> cameras, const double jpeg_quality,
    const synth::ChromaSubsampling subsampling, const std::uint64_t seed,
    const std::uint32_t width = kWidth, const std::uint32_t height = kHeight) {
    synth::PairSpec spec;
    spec.seed = seed;
    spec.image_width_px = width;
    spec.image_height_px = height;
    spec.supersample = 2;
    spec.jpeg = {jpeg_quality, subsampling};
    spec.chart = {1189.0, 841.0, 20.0, 10.0, 0.3, 50.0, 7.0, 0.0875, 0.2};
    spec.cameras = std::move(cameras);
    return spec;
}

cv::Mat RenderGray(const synth::PairSpec& spec, const std::size_t camera_index) {
    const synth::CameraRenderer renderer(spec, camera_index);
    cv::Mat bgr(static_cast<int>(spec.image_height_px), static_cast<int>(spec.image_width_px), CV_8UC3);
    const unsigned threads = 4;
    std::vector<std::thread> workers;
    const std::uint32_t band = (spec.image_height_px + threads - 1) / threads;
    for (unsigned i = 0; i < threads; ++i) {
        const std::uint32_t first = i * band;
        if (first >= spec.image_height_px) break;
        const std::uint32_t count = std::min(band, spec.image_height_px - first);
        workers.emplace_back([&renderer, &bgr, first, count] { renderer.RenderRows(first, count, bgr.ptr<std::uint8_t>(static_cast<int>(first))); });
    }
    for (auto& worker : workers) worker.join();
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

// ---------------------------------------------------------------------------
// The operator's part (the only use of the truth on the way in): name three
// rings near the middle of the picture and give a rough place for each.

fid::ChartDefinition ChartOf(const synth::ChartSpec& chart) {
    const auto refs = synth::ListReferencePoints(chart);
    std::uint32_t columns = 0, rows = 0;
    for (const auto& ref : refs) { columns = std::max(columns, ref.column); rows = std::max(rows, ref.row); }
    return {columns, rows, chart.margin_mm, chart.margin_mm, chart.fiducial_pitch_mm};
}

std::optional<synth::Vec2> TrueCentre(const synth::CameraSpec& camera, const fid::ChartDefinition& chart, std::uint32_t column, std::uint32_t row) {
    const auto p = synth::ProjectDocumentPoint(camera,
        {chart.origin_x_mm + column * chart.pitch_mm, chart.origin_y_mm + row * chart.pitch_mm});
    if (!p) return std::nullopt;
    return p;
}

bool Inside(const synth::Vec2& p, const double margin, const std::uint32_t width = kWidth, const std::uint32_t height = kHeight) {
    return p.x >= margin && p.y >= margin && p.x <= width - 1 - margin && p.y <= height - 1 - margin;
}

constexpr double kAnchorRoundingPx = 12.0;
constexpr double kAnchorRadiusPx = 30.0;

std::vector<fid::LatticeAnchor> OperatorAnchors(const synth::CameraSpec& camera, const fid::ChartDefinition& chart,
    const std::uint32_t width = kWidth, const std::uint32_t height = kHeight) {
    // The visible node nearest the picture centre, and the nodes two steps right and two steps down of it.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> best;
    double best_distance = 1e30;
    for (std::uint32_t row = 0; row + 2 <= chart.rows; ++row) {
        for (std::uint32_t column = 0; column + 2 <= chart.columns; ++column) {
            const auto a = TrueCentre(camera, chart, column, row);
            const auto b = TrueCentre(camera, chart, column + 2, row);
            const auto c = TrueCentre(camera, chart, column, row + 2);
            if (!a || !b || !c || !Inside(*a, 100, width, height) || !Inside(*b, 100, width, height) || !Inside(*c, 100, width, height)) continue;
            const double distance = std::hypot(a->x - width / 2.0, a->y - height / 2.0);
            if (distance < best_distance) { best_distance = distance; best = {{column, row}}; }
        }
    }
    if (!best) throw std::runtime_error("no anchor triple is visible");
    std::vector<fid::LatticeAnchor> anchors;
    for (const auto& node : {std::pair{best->first, best->second}, std::pair{best->first + 2, best->second}, std::pair{best->first, best->second + 2}}) {
        const auto p = *TrueCentre(camera, chart, node.first, node.second);
        anchors.push_back({node.first, node.second, std::round(p.x / kAnchorRoundingPx) * kAnchorRoundingPx,
            std::round(p.y / kAnchorRoundingPx) * kAnchorRoundingPx, kAnchorRadiusPx});
    }
    return anchors;
}

// ---------------------------------------------------------------------------
// Scoring against the truth (test side only).

struct Stats {
    std::size_t n = 0;
    double sum2 = 0.0;
    double max = 0.0;
    void Add(const double e) { ++n; sum2 += e * e; max = std::max(max, e); }
    [[nodiscard]] double Rms() const { return n == 0 ? 0.0 : std::sqrt(sum2 / static_cast<double>(n)); }
};

struct Score {
    Stats error;
    std::size_t expected = 0;
    std::size_t expected_found = 0;
    std::size_t wrong_identity = 0; // detected but farther than 5 px from its own true centre
    std::size_t missing = 0;
    std::size_t ambiguous = 0;
    std::size_t detected = 0;
};

Score ScoreDetection(const fid::ViewDetection& detection, const synth::CameraSpec& camera, const fid::ChartDefinition& chart) {
    Score score;
    std::map<std::pair<std::uint32_t, std::uint32_t>, const fid::NodeObservation*> by_node;
    for (const auto& node : detection.nodes) by_node[{node.column, node.row}] = &node;
    for (std::uint32_t row = 0; row <= chart.rows; ++row) {
        for (std::uint32_t column = 0; column <= chart.columns; ++column) {
            const auto truth = TrueCentre(camera, chart, column, row);
            const auto it = by_node.find({column, row});
            const bool required = truth && Inside(*truth, kRequiredEdgeMarginPx);
            if (required) ++score.expected;
            if (it == by_node.end()) continue;
            const auto& node = *it->second;
            if (node.state == fid::NodeState::Missing) { ++score.missing; continue; }
            if (node.state == fid::NodeState::Ambiguous) { ++score.ambiguous; continue; }
            ++score.detected;
            if (!truth) { ++score.wrong_identity; continue; }
            const double e = std::hypot(node.x_px - truth->x, node.y_px - truth->y);
            if (e > 5.0) { ++score.wrong_identity; continue; }
            score.error.Add(e);
            if (required) ++score.expected_found;
        }
    }
    return score;
}

void CheckScore(const Score& score, const Tolerances& tolerance, const std::string& label) {
    Info(label + ": detected " + std::to_string(score.detected) + ", required " + std::to_string(score.expected)
        + ", required found " + std::to_string(score.expected_found) + ", missing " + std::to_string(score.missing)
        + ", ambiguous " + std::to_string(score.ambiguous) + ", wrong identity " + std::to_string(score.wrong_identity)
        + ", rms " + Fixed(score.error.Rms()) + " px, max " + Fixed(score.error.max) + " px");
    Check(score.expected >= 40, label + ": the view shows enough required rings (" + std::to_string(score.expected) + ")");
    Check(score.wrong_identity == 0, label + ": no ring got the identity of another node");
    Check(score.expected_found == score.expected, label + ": every ring away from the edges is found");
    Check(score.ambiguous == 0, label + ": no ambiguous node");
    Check(score.error.n > 0 && score.error.Rms() <= tolerance.rms_px, label + ": rms within the test tolerance");
    Check(score.error.max <= tolerance.max_px, label + ": max within the test tolerance");
}

void CheckVerified(const fid::ViewDetection& detection, const std::string& label) {
    Check(detection.failure.empty(), label + ": detection succeeds (" + detection.failure + ")");
    Check(detection.identity.result == "verified", label + ": chart identity is verified");
    Check(detection.identity.cells_examined >= 8 && detection.identity.gray_cells_examined >= 2,
        label + ": verification measured sufficient chart cells and grey patches");
}

// ---------------------------------------------------------------------------
// Fixture: two lens models and the views of each.

const CameraModel kModelA{{1500.0, 1500.0, 899.5, 599.5}, {-0.09, 0.02, 0.0, 4e-4, -3e-4}};
const CameraModel kModelB{{1520.0, 1510.0, 905.0, 590.0}, {-0.06, 0.01, 0.0, -2e-4, 1e-4}};
const CameraModel kNoLensA{kModelA.k, {0, 0, 0, 0, 0}};

struct NamedView {
    const char* id;
    const char* role;
    ViewPose pose_a;
    ViewPose pose_b;
};
const std::vector<NamedView> kViews = {
    {"T1", "training", {25, 0, 0, 480, 420, 560}, {0, -25, 15, 730, 400, 580}},
    {"T2", "training", {-25, 8, 12, 700, 380, 560}, {-8, 25, 27, 900, 400, 570}},
    {"T3", "training", {5, 25, -12, 560, 450, 560}, {-25, -5, 3, 820, 440, 590}},
    {"T4", "training", {-6, -25, 22, 420, 360, 560}, {25, 6, 37, 700, 380, 560}},
    {"T5", "training", {18, 18, 40, 650, 430, 560}, {-18, 18, 55, 880, 410, 580}},
    {"T6", "training", {-20, -14, -28, 520, 400, 560}, {20, -14, -13, 760, 420, 570}},
    {"V1", "validation", {12, -20, 6, 600, 420, 560}, {20, 12, 21, 840, 390, 575}},
    {"V2", "validation", {-10, 16, -20, 470, 400, 560}, {-12, -20, -5, 780, 410, 565}},
};

// ---------------------------------------------------------------------------
// JSON text helpers for building jobs.

std::string Number(const double value) {
    char text[64]{};
    std::snprintf(text, sizeof(text), "%.17g", value);
    return text;
}

struct JobView {
    std::string id;
    std::string camera;
    std::string role;
    std::string image;
    std::vector<fid::LatticeAnchor> anchors;
};

std::string JobJson(const fid::ChartDefinition& chart, const std::vector<JobView>& views, const std::string& extra_key = {},
    std::uint32_t width = kWidth, std::uint32_t height = kHeight, const std::string& measured_at = "2026-10-14T00:00:00Z") {
    std::string text = "{\"schema\":\"a0.fiducial-detector-job/1\",\"sessionId\":\"synthetic-fiducial-session\",\"chartId\":\"synthetic-chart\","
                       "\"measuredAtUtc\":\"" + measured_at + '"';
    text += ",\"chart\":{\"columns\":" + std::to_string(chart.columns) + ",\"rows\":" + std::to_string(chart.rows)
        + ",\"originXMm\":" + Number(chart.origin_x_mm) + ",\"originYMm\":" + Number(chart.origin_y_mm) + ",\"pitchMm\":" + Number(chart.pitch_mm) + '}';
    text += ",\"cameras\":{";
    for (const char* alias : {"CAM-A", "CAM-B"}) {
        if (std::string(alias) == "CAM-B") text += ',';
        text += std::string("\"") + alias + "\":{\"widthPixels\":" + std::to_string(width) + ",\"heightPixels\":" + std::to_string(height)
            + ",\"lensLabel\":\"synthetic-lens\",\"focalLengthLabel\":\"f1500px\",\"focusLabel\":\"fixed\"}";
    }
    text += "},\"views\":[";
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto& v = views[i];
        if (i != 0) text += ',';
        text += "{\"viewId\":\"" + v.id + "\",\"camera\":\"" + v.camera + "\",\"role\":\"" + v.role + "\",\"image\":\"" + v.image + "\",\"anchors\":[";
        for (std::size_t k = 0; k < v.anchors.size(); ++k) {
            const auto& a = v.anchors[k];
            if (k != 0) text += ',';
            text += "{\"column\":" + std::to_string(a.column) + ",\"row\":" + std::to_string(a.row) + ",\"xPx\":" + Number(a.x_px)
                + ",\"yPx\":" + Number(a.y_px) + ",\"radiusPx\":" + Number(a.radius_px) + '}';
        }
        text += "]}";
    }
    text += ']';
    if (!extra_key.empty()) text += ",\"" + extra_key + "\":1";
    text += '}';
    return text;
}

// ---------------------------------------------------------------------------
// Stages 1 to 3 on single views.

void TestDetectionStages(const std::filesystem::path& temp) {
    const fid::DetectorParams params;
    const auto chart_spec = MakeSpec({}, 0.95, synth::ChromaSubsampling::s444, 1).chart;
    const auto chart = ChartOf(chart_spec);
    Check(chart.columns == 22 && chart.rows == 16, "the chart definition has 23 x 17 nodes");

    const ViewPose pose{25, -10, 8, 560, 420, 560};

    // Stage 1: no lens distortion, no noise, uncompressed.
    {
        const auto camera = MakeCamera("CAM-A", kNoLensA, pose, false);
        const auto spec = MakeSpec({camera}, 0.95, synth::ChromaSubsampling::s444, 11);
        const auto gray = RenderGray(spec, 0);
        const auto anchors = OperatorAnchors(camera, chart);
        const auto detection = fid::DetectView(gray, chart, anchors, params);
        CheckVerified(detection, "stage 1");
        CheckScore(ScoreDetection(detection, camera, chart), kUndistorted, "stage 1, no distortion, uncompressed");
    }

    // Stage 2: lens distortion, no noise, uncompressed.
    {
        const auto camera = MakeCamera("CAM-A", kModelA, pose, false);
        const auto spec = MakeSpec({camera}, 0.95, synth::ChromaSubsampling::s444, 12);
        const auto gray = RenderGray(spec, 0);
        const auto anchors = OperatorAnchors(camera, chart);
        const auto detection = fid::DetectView(gray, chart, anchors, params);
        CheckVerified(detection, "stage 2");
        CheckScore(ScoreDetection(detection, camera, chart), kDistorted, "stage 2, lens distortion, uncompressed");
    }

    // Stage 3: lens distortion, noise, vignetting, white balance, JPEG compression.
    struct JpegCase { const char* name; double quality; synth::ChromaSubsampling subsampling; Tolerances tolerance; };
    const JpegCase cases[] = {
        {"quality 0.95, 4:4:4", 0.95, synth::ChromaSubsampling::s444, kNoisyJpeg},
        {"quality 0.75, 4:2:0", 0.75, synth::ChromaSubsampling::s420, kNoisyJpeg},
        {"quality 0.40, 4:2:0", 0.40, synth::ChromaSubsampling::s420, kHarshJpeg},
    };
    int index = 0;
    for (const auto& c : cases) {
        const auto camera = MakeCamera("CAM-A", kModelA, pose, true);
        const auto spec = MakeSpec({camera}, c.quality, c.subsampling, 20 + static_cast<std::uint64_t>(index));
        const auto folder = temp / ("stage3-" + std::to_string(index));
        const auto generated = synth::GeneratePair(spec, {folder, 4});
        const auto file = folder / generated.files.at(0).relative_path;
        const auto bytes = ReadText(file);
        const cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1, const_cast<char*>(bytes.data()));
        const cv::Mat gray = cv::imdecode(encoded, cv::IMREAD_GRAYSCALE | cv::IMREAD_IGNORE_ORIENTATION);
        Check(gray.cols == static_cast<int>(kWidth) && gray.rows == static_cast<int>(kHeight), std::string("stage 3 ") + c.name + ": the JPEG decodes at the right size");
        const auto anchors = OperatorAnchors(camera, chart);
        const auto detection = fid::DetectView(gray, chart, anchors, params);
        CheckVerified(detection, std::string("stage 3 ") + c.name);
        Info(std::string("stage 3 ") + c.name + ": JPEG " + std::to_string(bytes.size()) + " bytes");
        CheckScore(ScoreDetection(detection, camera, chart), c.tolerance, std::string("stage 3, lens distortion, noise, JPEG ") + c.name);
        ++index;
    }
}

// ---------------------------------------------------------------------------
// Failure paths of a single view.

void TestFailurePaths() {
    const fid::DetectorParams params;
    const auto chart = ChartOf(MakeSpec({}, 0.95, synth::ChromaSubsampling::s444, 1).chart);
    const ViewPose pose{15, 5, 0, 560, 420, 560};
    const auto camera = MakeCamera("CAM-A", kModelA, pose, false);
    const auto spec = MakeSpec({camera}, 0.95, synth::ChromaSubsampling::s444, 31);
    const auto gray = RenderGray(spec, 0);
    const auto anchors = OperatorAnchors(camera, chart);
    Check(fid::DetectView(gray, chart, anchors, params).failure.empty(), "failure paths: the baseline view resolves");

    {
        auto two = anchors;
        two.pop_back();
        Check(fid::DetectView(gray, chart, two, params).failure == "anchor-count", "fewer than three anchors are refused");
    }
    {
        auto moved = anchors;
        moved[1].x_px += 400.0;
        moved[1].y_px += 400.0;
        Check(fid::DetectView(gray, chart, moved, params).failure == "anchor-not-found", "an anchor with no ring in its radius is refused");
    }
    {
        auto wide = anchors;
        wide[0].radius_px = 400.0;
        Check(fid::DetectView(gray, chart, wide, params).failure == "anchor-ambiguous", "an anchor radius that holds several rings is refused");
    }
    {
        auto twice = anchors;
        twice[2] = twice[0];
        Check(fid::DetectView(gray, chart, twice, params).failure == "anchor-duplicate", "two anchors on one node are refused");
    }
    {
        auto line = anchors;
        line[2].column = line[0].column + 4;
        line[2].row = line[0].row;
        // The third anchor names a node on the first anchors' lattice row, with a hint on the true ring.
        const auto p = *TrueCentre(camera, chart, line[2].column, line[2].row);
        line[2].x_px = std::round(p.x / kAnchorRoundingPx) * kAnchorRoundingPx;
        line[2].y_px = std::round(p.y / kAnchorRoundingPx) * kAnchorRoundingPx;
        Check(fid::DetectView(gray, chart, line, params).failure == "anchor-collinear", "anchors on one lattice row are refused");
    }
    {
        auto outside = anchors;
        outside[0].column = chart.columns + 1;
        Check(fid::DetectView(gray, chart, outside, params).failure == "anchor-outside-chart", "an anchor outside the chart is refused");
    }
    {
        const cv::Mat blank(static_cast<int>(kHeight), static_cast<int>(kWidth), CV_8UC1, cv::Scalar(200));
        Check(fid::DetectView(blank, chart, anchors, params).failure == "no-rings", "a blank image has no rings");
    }
    {
        // A coherent shift used to produce plausible observations with wrong IDs.
        auto shifted = anchors;
        for (auto& a : shifted) a.column += 1;
        const auto detection = fid::DetectView(gray, chart, shifted, params);
        Check(detection.failure == "identity-mismatch" && detection.nodes.empty(),
            "anchors shifted by one lattice step are rejected without observations");
    }
    {
        auto mirrored = anchors;
        std::swap(mirrored[1].column, mirrored[2].column);
        std::swap(mirrored[1].row, mirrored[2].row);
        const auto detection = fid::DetectView(gray, chart, mirrored, params);
        Check(detection.failure == "anchor-mirrored" && detection.nodes.empty(),
            "anchors with swapped identities are rejected without observations");
    }
}

// The oracle is the chart's printed identity, not reprojection residuals: a
// translated or reflected chart can fit a wrong pose with a tiny residual.
void TestIdentityChecks(const std::filesystem::path& temp) {
    const auto camera = MakeCamera("CAM-A", kModelA, {15, 5, 0, 560, 420, 560}, false);
    const auto spec = MakeSpec({camera}, 0.95, synth::ChromaSubsampling::s444, 51);
    const auto chart = ChartOf(spec.chart);
    const auto gray = RenderGray(spec, 0);
    const auto anchors = OperatorAnchors(camera, chart);
    CheckVerified(fid::DetectView(gray, chart, anchors, fid::DetectorParams{}), "identity baseline");

    struct WrongAnchors { std::string name; std::vector<fid::LatticeAnchor> anchors; std::string failure; };
    std::vector<WrongAnchors> wrong;
    for (const auto& [dc, dr] : std::array<std::pair<int, int>, 6>{{{1, 0}, {0, 1}, {1, 1}, {-1, 0}, {0, -1}, {-1, -1}}}) {
        auto shifted = anchors;
        for (auto& a : shifted) {
            a.column = static_cast<std::uint32_t>(static_cast<int>(a.column) + dc);
            a.row = static_cast<std::uint32_t>(static_cast<int>(a.row) + dr);
        }
        wrong.push_back({"shift " + std::to_string(dc) + "," + std::to_string(dr), shifted, "identity-mismatch"});
    }
    auto mirrored = anchors;
    std::swap(mirrored[1].column, mirrored[2].column);
    std::swap(mirrored[1].row, mirrored[2].row);
    wrong.push_back({"swap", mirrored, "anchor-mirrored"});
    auto typo = anchors;
    ++typo[1].column;
    wrong.push_back({"one-node typo", typo, ""});
    auto cycled = anchors;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        cycled[i].column = anchors[(i + 1) % anchors.size()].column;
        cycled[i].row = anchors[(i + 1) % anchors.size()].row;
    }
    wrong.push_back({"cycle", cycled, ""});

    // First three anchors have the correct handedness. A later triple must
    // still be checked; only checking the first triple would miss this.
    auto four = anchors;
    const auto fourth = *TrueCentre(camera, chart, anchors[1].column, anchors[2].row);
    four.push_back({anchors[0].column - 1, anchors[2].row,
        std::round(fourth.x / kAnchorRoundingPx) * kAnchorRoundingPx,
        std::round(fourth.y / kAnchorRoundingPx) * kAnchorRoundingPx, kAnchorRadiusPx});
    wrong.push_back({"later mirrored triple", four, "anchor-mirrored"});

    const auto image = temp / "identity.png";
    Check(cv::imwrite(image.string(), gray), "identity fixture is written");
    for (const auto& c : wrong) {
        const auto detection = fid::DetectView(gray, chart, c.anchors, fid::DetectorParams{});
        Info(c.name + ": " + detection.failure + ", cells " + std::to_string(detection.identity.cells_examined)
            + ", mismatched " + std::to_string(detection.identity.cells_mismatched)
            + ", grey mismatched " + std::to_string(detection.identity.gray_cells_mismatched));
        Check(!detection.failure.empty() && detection.nodes.empty(), c.name + ": no wrong observations escape");
        if (!c.failure.empty()) Check(detection.failure == c.failure, c.name + ": expected refusal reason");
        else Check(detection.failure == "identity-mismatch" || detection.failure == "identity-unverifiable",
            c.name + ": the identity check rejects the otherwise valid anchor input");
        if (c.name == "shift 1,1" || c.name == "shift -1,-1") {
            Check(detection.identity.gray_cells_mismatched > 0,
                c.name + ": grey levels catch the diagonal shift invisible to the pattern kind");
        }
        // A reference observation is the next stage's public input. It must
        // never be emitted on a bad identity, even when the geometry fits.
        const auto result = fid::RunJob(JobJson(chart, {{"reference-A", "CAM-A", "reference", "identity.png", c.anchors}}), temp);
        Check(!result.ok && result.session_json.empty(), c.name + ": no reference session is emitted");
        Check(result.report_json.find("\"status\":\"" + detection.failure + "\"") != std::string::npos
            && result.report_json.find("\"identity\":{") != std::string::npos,
            c.name + ": refusal and identity evidence are reported");
    }
    {
        fid::DetectorParams params;
        params.identity_min_cells = static_cast<std::uint32_t>(chart.NodeCount() + 1);
        const auto detection = fid::DetectView(gray, chart, anchors, params);
        Check(detection.failure == "identity-unverifiable" && detection.nodes.empty(),
            "insufficient identity evidence refuses observations");
    }
    for (int invalid = 0; invalid < 3; ++invalid) {
        fid::DetectorParams params;
        if (invalid == 0) params.identity_max_mismatch_fraction = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 1) params.identity_min_gray_cells = 0;
        if (invalid == 2) params.identity_line_share = std::numeric_limits<double>::infinity();
        bool rejected = false;
        try { (void)fid::DetectView(gray, chart, anchors, params); } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "invalid identity parameters cannot disable verification " + std::to_string(invalid));
    }
}

// ---------------------------------------------------------------------------
// Job: end to end through JPEG files, the session, and the lens estimator.

struct Mapping {
    Stats training;
    Stats held_out;
};

std::optional<synth::Vec2> EstimatedProjection(const cal::PinholeIntrinsics& k, const cal::BrownConradyDistortion& d,
    const cal::ChartPose& pose, double x, double y) {
    const double px = pose.rotation[0] * x + pose.rotation[1] * y + pose.translation_mm[0];
    const double py = pose.rotation[3] * x + pose.rotation[4] * y + pose.translation_mm[1];
    const double pz = pose.rotation[6] * x + pose.rotation[7] * y + pose.translation_mm[2];
    if (!(pz > 0.0)) return std::nullopt;
    const double xn = px / pz, yn = py / pz;
    const double r2 = xn * xn + yn * yn;
    const double radial = 1.0 + d.k1 * r2 + d.k2 * r2 * r2 + d.k3 * r2 * r2 * r2;
    const double xd = xn * radial + 2.0 * d.p1 * xn * yn + d.p2 * (r2 + 2.0 * xn * xn);
    const double yd = yn * radial + d.p1 * (r2 + 2.0 * yn * yn) + 2.0 * d.p2 * xn * yn;
    return synth::Vec2{k.fx * xd + k.cx, k.fy * yd + k.cy};
}

void TestEndToEnd(const std::filesystem::path& temp, const std::filesystem::path& executable) {
    const fid::DetectorParams params;
    const auto chart_spec = MakeSpec({}, 0.9, synth::ChromaSubsampling::s444, 1).chart;
    const auto chart = ChartOf(chart_spec);

    // One pair of JPEGs per view (CAM-A and CAM-B), with noise, vignetting and compression.
    std::vector<JobView> job_views;
    std::map<std::string, synth::CameraSpec> truth_cameras; // view id -> camera
    std::map<std::string, std::vector<fid::LatticeAnchor>> anchors_by_view;
    std::size_t index = 0;
    for (const auto& view : kViews) {
        const auto camera_a = MakeCamera("CAM-A", kModelA, view.pose_a, true);
        const auto camera_b = MakeCamera("CAM-B", kModelB, view.pose_b, true);
        const auto spec = MakeSpec({camera_a, camera_b}, 0.9, synth::ChromaSubsampling::s444, 100 + index);
        const auto folder = temp / ("view-" + std::string(view.id));
        (void)synth::GeneratePair(spec, {folder, 4});
        const std::string a_id = std::string("A-") + view.id;
        const std::string b_id = std::string("B-") + view.id;
        const auto anchors_a = OperatorAnchors(camera_a, chart);
        const auto anchors_b = OperatorAnchors(camera_b, chart);
        job_views.push_back({a_id, "CAM-A", view.role, std::string("view-") + view.id + "/CAM-A/original.jpg", anchors_a});
        job_views.push_back({b_id, "CAM-B", view.role, std::string("view-") + view.id + "/CAM-B/original.jpg", anchors_b});
        truth_cameras.emplace(a_id, camera_a);
        truth_cameras.emplace(b_id, camera_b);
        anchors_by_view[a_id] = anchors_a;
        anchors_by_view[b_id] = anchors_b;
        ++index;
    }

    const auto job = JobJson(chart, job_views);
    const auto started = std::chrono::steady_clock::now();
    const auto result = fid::RunJob(job, temp, params);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Info("job: " + std::to_string(job_views.size()) + " JPEG views detected in " + std::to_string(elapsed) + " ms");
    Check(result.ok, "the job resolves every view and the session is accepted by the reader");
    if (!result.ok) { std::cerr << result.report_json << '\n'; return; }

    // The session reads back, and a second run gives the same bytes.
    const auto session = cal::CalibrationObservationSession::Parse(result.session_json);
    Check(session.Views().size() == job_views.size(), "the session has every view");
    const auto again = fid::RunJob(job, temp, params);
    Check(again.session_json == result.session_json && again.report_json == result.report_json, "a second run gives identical session and report text");
    Check(result.report_json.find(temp.string()) == std::string::npos, "the report contains no path of the test folder");
    Check(result.report_json.find("groundTruth") == std::string::npos && result.session_json.find("groundTruth") == std::string::npos,
        "no ground truth key appears in the outputs");
    Check(result.report_json.find("hintXPx") != std::string::npos && result.report_json.find("searchRadiusPx") != std::string::npos,
        "the operator anchors are recorded in the report");
    const auto diagnostics = cal::DiagnoseObservationSession(session);
    for (const auto& d : diagnostics) {
        Info(std::string("session diagnostic: ") + std::string(cal::DiagnosticCode(d.code)) + " " + std::string(cal::CameraAliasCode(d.camera))
            + (d.view_id ? " " + *d.view_id : ""));
    }
    for (const auto& d : diagnostics) {
        Check(d.code == cal::SessionDiagnosticCode::ReferenceViewMissing, "the only diagnostics are the reference views this session does not carry");
    }

    // Detection error of every view against the truth, read back from the session.
    Stats all_error;
    std::size_t wrong = 0;
    for (const auto& view : session.Views()) {
        const auto& camera = truth_cameras.at(view.id);
        Stats stats;
        for (const auto& observation : view.observations) {
            if (observation.state != cal::ObservationState::Detected) continue;
            const auto* point = session.FindChartPoint(observation.point_id);
            const auto truth = synth::ProjectDocumentPoint(camera, {point->x_mm, point->y_mm});
            if (!truth) { ++wrong; continue; }
            const double e = std::hypot(observation.pixel->x - truth->x, observation.pixel->y - truth->y);
            if (e > 5.0) { ++wrong; continue; }
            stats.Add(e);
            all_error.Add(e);
        }
        Info("view " + view.id + ": " + std::to_string(stats.n) + " rings, rms " + Fixed(stats.Rms()) + " px, max " + Fixed(stats.max) + " px");
    }
    Check(wrong == 0, "no observation in the session carries another node's identity");
    Check(all_error.Rms() <= kNoisyJpeg.rms_px && all_error.max <= kNoisyJpeg.max_px, "the session observations are within the test tolerance of the truth");

    // The lens estimator on the detected observations.
    const cal::LensIntrinsicsPolicy policy{2, 1e-2, cal::ObservationModel::DistortedAnalytic, 100, 1e-10, 1e-12, 1e-3};
    const auto lens = cal::EstimateLensIntrinsics(session, policy);
    const std::array<const CameraModel*, 2> models{&kModelA, &kModelB};
    for (std::size_t c = 0; c < 2; ++c) {
        const auto& camera = lens.cameras[c];
        const std::string name = c == 0 ? "CAM-A" : "CAM-B";
        Info(name + ": status " + std::string(cal::LensStatusCode(camera.status)));
        Check(camera.status == cal::LensStatus::LensIntrinsicsEstimated, name + ": the lens estimate completes on the detected observations");
        if (camera.status != cal::LensStatus::LensIntrinsicsEstimated) continue;
        const auto& k = *camera.intrinsics;
        const auto& d = *camera.distortion;
        const auto& truth = *models[c];
        Info(name + ": fx " + Fixed(k.fx, 2) + " (true " + Fixed(truth.k.fx, 2) + "), fy " + Fixed(k.fy, 2) + " (true " + Fixed(truth.k.fy, 2)
            + "), cx " + Fixed(k.cx, 2) + " (true " + Fixed(truth.k.cx, 2) + "), cy " + Fixed(k.cy, 2) + " (true " + Fixed(truth.k.cy, 2) + ")");
        Info(name + ": k1 " + Fixed(d.k1, 5) + " (true " + Fixed(truth.d.k1, 5) + "), k2 " + Fixed(d.k2, 5) + " (true " + Fixed(truth.d.k2, 5)
            + "), p1 " + Fixed(d.p1, 5) + " (true " + Fixed(truth.d.p1, 5) + "), p2 " + Fixed(d.p2, 5) + " (true " + Fixed(truth.d.p2, 5) + ")");
        Info(name + ": training residual rms " + Fixed(camera.training_residual->rms_pixels) + " px over " + std::to_string(camera.training_residual->point_count) + " points");
        if (camera.held_out_residual) {
            Info(name + ": held-out residual rms " + Fixed(camera.held_out_residual->rms_pixels) + " px, max " + Fixed(camera.held_out_residual->max_pixels)
                + " px over " + std::to_string(camera.held_out_residual->point_count) + " points");
        }
        Check(camera.held_out_coverage == cal::HeldOutCoverage::AllViewsEvaluated, name + ": both validation views are evaluated");

        // The mapping difference: the estimated document -> pixel mapping of each view
        // against the generator's true mapping, over all lattice nodes inside the image.
        Mapping mapping;
        for (const auto& view : camera.views) {
            if (!view.pose) { Check(false, name + " " + view.view_id + ": the view has a pose"); continue; }
            const auto& truth_camera = truth_cameras.at(view.view_id);
            Stats stats;
            for (std::uint32_t row = 0; row <= chart.rows; ++row) {
                for (std::uint32_t column = 0; column <= chart.columns; ++column) {
                    const double x = chart.origin_x_mm + column * chart.pitch_mm;
                    const double y = chart.origin_y_mm + row * chart.pitch_mm;
                    const auto truth_px = synth::ProjectDocumentPoint(truth_camera, {x, y});
                    if (!truth_px || !Inside(*truth_px, 30.0)) continue;
                    const auto estimate = EstimatedProjection(k, d, *view.pose, x, y);
                    if (!estimate) { Check(false, name + " " + view.view_id + ": the estimated mapping places a visible node in front"); continue; }
                    const double e = std::hypot(estimate->x - truth_px->x, estimate->y - truth_px->y);
                    stats.Add(e);
                    (view.role == cal::ViewRole::Training ? mapping.training : mapping.held_out).Add(e);
                }
            }
            Info(name + " " + view.view_id + " (" + std::string(cal::ViewRoleCode(view.role)) + "): mapping vs truth over " + std::to_string(stats.n)
                + " nodes, rms " + Fixed(stats.Rms()) + " px, max " + Fixed(stats.max) + " px");
        }
        Info(name + ": training mapping rms " + Fixed(mapping.training.Rms()) + " px, max " + Fixed(mapping.training.max)
            + " px; held-out mapping rms " + Fixed(mapping.held_out.Rms()) + " px, max " + Fixed(mapping.held_out.max) + " px");
        Check(mapping.training.n > 0 && mapping.training.Rms() <= kMapping.rms_px && mapping.training.max <= kMapping.max_px,
            name + ": training views map within the test tolerance of the truth");
        Check(mapping.held_out.n > 0 && mapping.held_out.Rms() <= kMapping.rms_px && mapping.held_out.max <= kMapping.max_px,
            name + ": held-out views map within the test tolerance of the truth");
    }

    // The executable: same job, same bytes; refuses to overwrite; no session on a failed view.
    if (!executable.empty()) {
        const auto run = [&](const std::filesystem::path& job, const std::filesystem::path& output) {
            std::wstring command = L"\"" + executable.wstring() + L"\" --job \"" + job.wstring() + L"\" --output \"" + output.wstring() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return -1;
            WaitForSingleObject(process.hProcess, 600000);
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            return static_cast<int>(code);
        };
        const auto job_file = temp / "job.json";
        {
            std::ofstream file(job_file, std::ios::binary);
            file << job;
        }
        const auto out_ok = temp / "exe-ok";
        Check(run(job_file, out_ok) == 0, "the executable exits 0 when every view resolves");
        Check(std::filesystem::exists(out_ok / "session.json") && std::filesystem::exists(out_ok / "detection-report.json"),
            "the executable writes the session and the report");
        if (std::filesystem::exists(out_ok / "session.json")) {
            Check(ReadText(out_ok / "session.json") == result.session_json, "the executable's session equals the library's");
            Check(ReadText(out_ok / "detection-report.json") == result.report_json, "the executable's report equals the library's");
        }
        Check(run(job_file, out_ok) == 2, "the executable refuses to overwrite an earlier output");

        // Either pre-existing output blocks the whole job. In particular a
        // report collision must not leave a new, apparently usable session.
        for (const auto* existing : {"session.json", "detection-report.json"}) {
            const auto collision = temp / (std::string("collision-") + existing);
            std::filesystem::create_directories(collision);
            { std::ofstream file(collision / existing, std::ios::binary); file << "sentinel"; }
            Check(run(job_file, collision) == 2, "either existing output blocks the job");
            Check(ReadText(collision / existing) == "sentinel", "existing output bytes are unchanged");
            const auto* other = std::string(existing) == "session.json" ? "detection-report.json" : "session.json";
            Check(!std::filesystem::exists(collision / other), "collision leaves no second output");
        }

        auto short_views = job_views;
        short_views.resize(2);
        short_views[0].anchors.pop_back();
        const auto bad_job = temp / "bad-job.json";
        {
            std::ofstream file(bad_job, std::ios::binary);
            file << JobJson(chart, short_views);
        }
        const auto out_bad = temp / "exe-bad";
        Check(run(bad_job, out_bad) == 3, "the executable exits 3 when a view does not resolve");
        Check(!std::filesystem::exists(out_bad / "session.json") && std::filesystem::exists(out_bad / "detection-report.json"),
            "a failed job leaves a report and no session");
    }

    // Job failures that are recorded, not thrown.
    {
        auto bad_views = job_views;
        bad_views.resize(2);
        bad_views[0].anchors.pop_back();
        const auto failed = fid::RunJob(JobJson(chart, bad_views), temp, params);
        Check(!failed.ok && failed.session_json.empty(), "a view with too few anchors produces no session");
        Check(failed.report_json.find("anchor-count") != std::string::npos, "the report names the failure");
    }
    {
        const auto mismatch = fid::RunJob(JobJson(chart, {job_views[0]}, {}, kWidth + 1, kHeight), temp, params);
        Check(!mismatch.ok && mismatch.report_json.find("dimension-mismatch") != std::string::npos, "an image that differs from the camera size is recorded");
    }
    // Jobs that are malformed throw.
    auto rejects = [&](const std::string& text, const std::string& label) {
        bool threw = false;
        try { (void)fid::RunJob(text, temp, params); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, label);
    };
    rejects(JobJson(chart, {job_views[0]}, "unknownKey"), "an unknown key in the job is refused");
    rejects("{}", "an empty job object is refused");
    rejects("not json", "a malformed job is refused");
    {
        auto escape = job_views[0];
        escape.camera = "CAM-C";
        rejects(JobJson(chart, {escape}), "a camera other than CAM-A and CAM-B is refused");
    }
    {
        auto role = job_views[0];
        role.role = "holdout";
        rejects(JobJson(chart, {role}), "an unknown role is refused");
    }
}

#ifdef A0_TEST_DRAFT_CALIBRATOR
// Explicit, retained-artifact integration run (#314). Ground truth belongs to
// this test only. The public detector receives JPEGs and coarse operator hints;
// the public writer receives only the resulting session and explicit job.
void TestFullResolutionCalibration(const std::filesystem::path& output) {
    constexpr std::uint32_t width = 7360, height = 4912;
    // Test tolerances chosen before this run; never optical acceptance limits.
    constexpr Tolerances tolerance{0.5, 1.5};
    const std::array<CameraModel, 2> models{{
        {{6200, 6250, 3679.5, 2455.5}, {-.04, .008, 0, .0003, -.0002}},
        {{6230, 6190, 3684.5, 2451.5}, {-.03, .006, 0, -.0002, .0001}}
    }};
    const std::array<NamedView, 9> views{{
        {"T1", "training", {25, 7, 82, 290, 410, 840}, {-22, 8, 97, 880, 420, 850}},
        {"T2", "training", {-25, 6, 100, 320, 400, 850}, {24, -8, 80, 860, 430, 840}},
        {"T3", "training", {7, 25, 72, 280, 430, 830}, {-8, 25, 110, 910, 400, 860}},
        {"T4", "training", {-6, -25, 112, 330, 390, 850}, {25, 6, 75, 870, 440, 850}},
        {"T5", "training", {18, 18, 130, 300, 440, 840}, {-18, 18, 120, 900, 420, 840}},
        {"T6", "training", {-20, -14, 62, 310, 420, 860}, {20, -14, 65, 890, 410, 850}},
        {"V1", "validation", {12, -20, 96, 300, 420, 840}, {20, 12, 105, 880, 390, 850}},
        {"V2", "validation", {-10, 16, 70, 320, 400, 850}, {-12, -20, 85, 900, 430, 840}},
        {"reference", "reference", {0, 0, 90, 300, 420.5, 840}, {0, 0, 90, 890, 420.5, 840}}
    }};
    auto write = [&](const char* name, const std::string& value) {
        std::ofstream file(output / name, std::ios::binary);
        file << value;
        if (!file) throw std::runtime_error("cannot retain integration artifact");
    };
    const auto chart = ChartOf(MakeSpec({}, .95, synth::ChromaSubsampling::s444, 1).chart);
    std::vector<JobView> job_views;
    std::map<std::string, synth::CameraSpec> truth_cameras;
    std::size_t index = 0;
    for (const auto& view : views) {
        const std::array<synth::CameraSpec, 2> cameras{
            MakeCamera("CAM-A", models[0], view.pose_a, true),
            MakeCamera("CAM-B", models[1], view.pose_b, true)};
        auto spec = MakeSpec({cameras[0], cameras[1]}, .95, synth::ChromaSubsampling::s444, 31400 + index, width, height);
        spec.supersample = 1;
        const std::string folder = "view-" + std::string(view.id);
        Info("full-resolution generate " + folder);
        std::cout.flush();
        (void)synth::GeneratePair(spec, {output / folder, 4});
        for (std::size_t c = 0; c < 2; ++c) {
            const std::string prefix = c == 0 ? "A-" : "B-";
            const std::string alias = c == 0 ? "CAM-A" : "CAM-B";
            const auto id = prefix + view.id;
            job_views.push_back({id, alias, view.role, folder + '/' + alias + "/original.jpg",
                OperatorAnchors(cameras[c], chart, width, height)});
            truth_cameras.emplace(id, cameras[c]);
        }
        ++index;
    }
    const auto detection_job = JobJson(chart, job_views, {}, width, height, "2000-01-01T00:00:00Z");
    write("detection-job.json", detection_job);
    Info("full-resolution detect 18 JPEGs");
    std::cout.flush();
    const auto detected = fid::RunJob(detection_job, output);
    write("detection-report.json", detected.report_json);
    Check(detected.ok && !detected.session_json.empty(), "all full-resolution JPEGs resolve and emit a session");
    if (!detected.ok) return;
    write("session.json", detected.session_json);
    const auto session = cal::CalibrationObservationSession::Parse(detected.session_json);
    Check(session.Views().size() == 18, "six training, two validation and one reference per camera retained");
    std::string scores = "{\"schema\":\"a0.synthetic-full-calibration-test/1\",\"quality\":\"not-assessed\","
        "\"testTolerancePixels\":{\"rms\":0.5,\"max\":1.5},\"detection\":[";
    bool first = true;
    for (const auto& view : session.Views()) {
        Stats stats;
        std::size_t wrong = 0;
        for (const auto& observation : view.observations) {
            if (observation.state != cal::ObservationState::Detected) continue;
            const auto* point = session.FindChartPoint(observation.point_id);
            const auto truth = synth::ProjectDocumentPoint(truth_cameras.at(view.id), {point->x_mm, point->y_mm});
            if (!truth) { ++wrong; continue; }
            const double error = std::hypot(observation.pixel->x - truth->x, observation.pixel->y - truth->y);
            stats.Add(error);
            if (error > 5) ++wrong;
        }
        Check(wrong == 0 && stats.n >= 40 && stats.Rms() <= tolerance.rms_px && stats.max <= tolerance.max_px,
            view.id + ": detected point identities/positions agree with independent truth");
        Info(view.id + " detection: n=" + std::to_string(stats.n) + " rms=" + Fixed(stats.Rms()) + " max=" + Fixed(stats.max));
        if (!first) scores += ',';
        first = false;
        scores += "{\"viewId\":\"" + view.id + "\",\"pointCount\":" + std::to_string(stats.n)
            + ",\"wrongIdentityCount\":" + std::to_string(wrong) + ",\"rmsPixels\":" + Number(stats.Rms()) + ",\"maxPixels\":" + Number(stats.max) + '}';
    }
    // Chart mm already coincide with document mm; no learned pose is supplied.
    const std::string job = R"({"schemaVersion":"a0.draft-calibration-job.v1","profileId":"synthetic-full-draft","provenanceId":"synthetic-full-provenance","measuredAt":"2000-01-02T00:00:00Z","documentPlane":{"orientation":"landscape","cameraOrder":"CAM-A-left-CAM-B-right"},"referencePair":{"CAM-A":"A-reference","CAM-B":"B-reference"},"referencePlacement":{"angleRadians":0,"translationXMm":0,"translationYMm":0,"coplanarConfirmed":true},"numericalPolicy":{"minimumViews":2,"relativeSingularCutoff":0.01,"maxIterations":100,"relativeCostTolerance":1e-10,"relativeStepTolerance":1e-12,"initialDamping":0.001,"maxUndistortionIterations":40,"undistortionTolerancePixels":1e-9},"rigMeasurements":{"baselineMm":null,"overlapMm":null,"cameraToDocumentMm":{"CAM-A":null,"CAM-B":null},"lensFocalLengthMm":{"CAM-A":null,"CAM-B":null}}})";
    write("job.json", job);
    Info("full-resolution estimate fixed mapping and write draft/report");
    std::cout.flush();
    const auto artifacts = cal::RunDraftCalibrationJob(detected.session_json, job);
    write("calibration-report.json", artifacts.report_json);
    Check(artifacts.ok && !artifacts.profile_json.empty(), "JPEG observations produce both reference mappings and independently evaluated validation views");
    if (!artifacts.ok) { write("truth-comparison.json", scores + "],\"draftProduced\":false}"); return; }
    write("profile.json", artifacts.profile_json);
    const auto profile = a0::m2::RigProfileV2::Parse(artifacts.profile_json);
    Check(profile.IsCalibrated() && !profile.IsApproved(), "existing profile reader accepts only a calibrated draft");
    using namespace a0::m2::render;
    // Deliberately low-resolution A0 evaluation preview; no product raster or
    // optical threshold is being selected by this synthetic test.
    const OutputRaster raster{{0, 0, 1189000, 841000}, 4, 188, 133};
    const auto parameters = profile.ValidateCalibrationForEvaluation(raster, Resampling::bilinear);
    scores += "],\"draftProduced\":true,\"referenceMapping\":[";
    for (std::size_t c = 0; c < 2; ++c) {
        Stats stats;
        bool projects = true;
        const auto& projection = c == 0 ? parameters.camera_a : parameters.camera_b;
        const std::string id = c == 0 ? "A-reference" : "B-reference";
        // Dense grid DIFFERENT from detected lattice centres. This tests the
        // serialized profile's production projection against generator truth.
        for (double y = 11.3; y < 841; y += 17.1) for (double x = 13.7; x < 1189; x += 17.3) {
            const auto truth = synth::ProjectDocumentPoint(truth_cameras.at(id), {x, y});
            if (!truth || !Inside(*truth, 30, width, height)) continue;
            Point estimated;
            if (!TryProjectDocument(projection, {x, y}, estimated)) { projects = false; continue; }
            stats.Add(std::hypot(estimated.x - truth->x, estimated.y - truth->y));
        }
        Check(projects && stats.n > 500 && stats.Rms() <= tolerance.rms_px && stats.max <= tolerance.max_px,
            id + ": serialized fixed mapping agrees with independent dense-grid truth");
        Info(id + " mapping: n=" + std::to_string(stats.n) + " rms=" + Fixed(stats.Rms()) + " max=" + Fixed(stats.max));
        if (c) scores += ',';
        scores += "{\"viewId\":\"" + id + "\",\"pointCount\":" + std::to_string(stats.n)
            + ",\"rmsPixels\":" + Number(stats.Rms()) + ",\"maxPixels\":" + Number(stats.max);
        const auto& k = projection.intrinsics;
        const auto& d = projection.distortion;
        const auto& model = models[c];
        const std::array<double, 9> estimated{k.fx_pixels, k.fy_pixels, k.cx_pixels, k.cy_pixels, d.k1, d.k2, d.k3, d.p1, d.p2};
        const std::array<double, 9> expected{model.k.fx, model.k.fy, model.k.cx, model.k.cy,
            model.d.k1, model.d.k2, model.d.k3, model.d.p1, model.d.p2};
        const std::array<const char*, 9> names{"fxPixels", "fyPixels", "cxPixels", "cyPixels", "k1", "k2", "k3", "p1", "p2"};
        scores += ",\"parameters\":{";
        for (std::size_t p = 0; p < names.size(); ++p) {
            if (p) scores += ',';
            scores += std::string("\"") + names[p] + "\":{\"truth\":" + Number(expected[p])
                + ",\"estimated\":" + Number(estimated[p]) + ",\"difference\":" + Number(estimated[p] - expected[p]) + '}';
        }
        scores += "}}";
    }
    write("truth-comparison.json", scores + "]}");
    auto decode = [&](const char* alias) {
        const auto bytes = ReadText(output / "view-reference" / alias / "original.jpg");
        const cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1, const_cast<char*>(bytes.data()));
        const auto decoded = cv::imdecode(encoded, cv::IMREAD_COLOR | cv::IMREAD_IGNORE_ORIENTATION);
        if (decoded.cols != width || decoded.rows != height || !decoded.isContinuous()) throw std::runtime_error("full reference JPEG decode");
        return BgrImage{width, height, std::vector<std::uint8_t>(decoded.data, decoded.data + decoded.total() * decoded.elemSize())};
    };
    const auto a = decode("CAM-A"), b = decode("CAM-B");
    const auto rendered = RenderDocumentPair(a, b, parameters);
    Check(rendered.image.width == 188 && rendered.image.height == 133 && rendered.seam_navigation.available,
        "real JPEG buffers compose the whole A0 evaluation raster with overlap");
    const cv::Mat preview(133, 188, CV_8UC3, const_cast<std::uint8_t*>(rendered.image.bgr.data()));
    Check(cv::imwrite((output / "evaluation-preview.png").string(), preview), "evaluation preview retained");
    Check(std::filesystem::exists(output / "view-reference/CAM-A/original.jpg")
        && std::filesystem::exists(output / "view-reference/CAM-B/original.jpg"), "both synthetic reference originals retained");
}
#endif

} // namespace

int main(const int argc, char* argv[]) {
#ifdef A0_TEST_DRAFT_CALIBRATOR
    if (argc == 3 && std::string(argv[1]) == "--full-resolution-calibration") {
        const auto started = std::chrono::steady_clock::now();
        try {
            const std::filesystem::path output(argv[2]);
            // Never remove an existing directory, even an empty one. Failed
            // runs retain their diagnostics and inputs in their new directory.
            if (!std::filesystem::create_directory(output)) {
                std::cerr << "output must be a new directory with an existing parent\n";
                return 2;
            }
            TestFullResolutionCalibration(output);
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
        Info("full-resolution elapsed " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count()) + " s");
        std::cout << "checks=" << checks << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
#endif
    // Usage: m2_fiducial_detector_contracts [--exe <A0CameraStitcher.FiducialDetector.exe>]
    std::filesystem::path executable;
    bool identity_only = false;
    if (argc == 3 && std::string(argv[1]) == "--exe") {
        executable = argv[2];
    } else if (argc == 2 && std::string(argv[1]) == "--identity-only") {
        identity_only = true;
    } else if (argc != 1) {
        std::cerr << "usage: m2_fiducial_detector_contracts [--exe <detector executable> | --identity-only]\n";
        return 2;
    }
    const auto temp = std::filesystem::temp_directory_path() / ("a0_fiducial_detector_test_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp);
    int exit_code = 0;
    try {
        const auto started = std::chrono::steady_clock::now();
        Info(std::string("OpenCV ") + CV_VERSION);
        TestIdentityChecks(temp);
        if (!identity_only) {
            TestDetectionStages(temp);
            TestFailurePaths();
            TestEndToEnd(temp, executable);
        }
        Info("elapsed " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count()) + " s");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        exit_code = 1;
    }
    std::error_code ignored;
    std::filesystem::remove_all(temp, ignored);
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return exit_code != 0 || failures != 0 ? 1 : 0;
}
