#include "a0/m2/fiducial_detector.hpp"

#include "a0/common/protocol_json.hpp"
#include "a0/m2/calibration_observation_session.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace a0::m2::fiducial {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Proportions of the chart's ring fiducial (src/m2/synthetic_pair_chart.cpp):
// black ring from 0.6 R to R, white ring from 0.25 R to 0.6 R, black dot
// inside 0.25 R. In units of the white ring's outer radius the dot is at 0.4167
// and the black ring spans 1.0 to 1.667.
constexpr double kRingProbeScale = 1.333;  // middle of the black ring
constexpr double kWindowOuter = 0.78;      // refinement window, white ring radii
constexpr double kBackgroundInner = 0.62;  // white ring samples used for the background plane
constexpr double kDotCore = 0.25;          // samples used for the dot level

[[noreturn]] void Reject(const std::string& message) { throw std::invalid_argument("fiducial detector: " + message); }

// ---------------------------------------------------------------------------
// Ring candidates.

struct Ellipse {
    double cx{};
    double cy{};
    double a{};     // semi-axis along u
    double b{};     // semi-axis along v
    double ux{};
    double uy{};
};

Ellipse FromRotatedRect(const cv::RotatedRect& box) {
    const double theta = static_cast<double>(box.angle) * kPi / 180.0;
    Ellipse e;
    e.cx = box.center.x;
    e.cy = box.center.y;
    e.a = 0.5 * static_cast<double>(box.size.width);
    e.b = 0.5 * static_cast<double>(box.size.height);
    e.ux = std::cos(theta);
    e.uy = std::sin(theta);
    return e;
}

// Squared elliptical radius of (x, y) around (cx, cy), 1 on the ellipse.
double EllipticalRadiusSquared(const Ellipse& e, double cx, double cy, double x, double y) {
    const double dx = x - cx;
    const double dy = y - cy;
    const double along = dx * e.ux + dy * e.uy;
    const double across = -dx * e.uy + dy * e.ux;
    return (along * along) / (e.a * e.a) + (across * across) / (e.b * e.b);
}

bool Solve3(std::array<std::array<double, 4>, 3> m, std::array<double, 3>& solution) {
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::abs(m[row][column]) > std::abs(m[pivot][column])) pivot = row;
        }
        if (!(std::abs(m[pivot][column]) > 1e-12)) return false;
        std::swap(m[pivot], m[column]);
        for (int row = column + 1; row < 3; ++row) {
            const double factor = m[row][column] / m[column][column];
            for (int k = column; k < 4; ++k) m[row][k] -= factor * m[column][k];
        }
    }
    for (int row = 2; row >= 0; --row) {
        double value = m[row][3];
        for (int k = row + 1; k < 3; ++k) value -= m[row][k] * solution[k];
        solution[row] = value / m[row][row];
    }
    return true;
}

// Weighted centroid of the dot inside the white ring, with a plane fitted to
// the white ring as the local background so that a falloff does not move it.
// `gray` is CV_8UC1. Returns no value when the window leaves the image or the
// dot has no contrast.
std::optional<std::pair<double, double>> RefineCentre(
    const cv::Mat& gray, const Ellipse& shape, double start_x, double start_y, int iterations) {
    double cx = start_x;
    double cy = start_y;
    const double extent = std::max(shape.a, shape.b) * kWindowOuter + 2.0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int x0 = static_cast<int>(std::floor(cx - extent));
        const int x1 = static_cast<int>(std::ceil(cx + extent));
        const int y0 = static_cast<int>(std::floor(cy - extent));
        const int y1 = static_cast<int>(std::ceil(cy + extent));
        if (x0 < 0 || y0 < 0 || x1 >= gray.cols || y1 >= gray.rows) return std::nullopt;

        // Background plane I = w0 + wx dx + wy dy over the white ring.
        std::array<std::array<double, 4>, 3> normal{};
        std::size_t samples = 0;
        for (int y = y0; y <= y1; ++y) {
            const auto* row = gray.ptr<std::uint8_t>(y);
            for (int x = x0; x <= x1; ++x) {
                const double s2 = EllipticalRadiusSquared(shape, cx, cy, x, y);
                if (s2 < kBackgroundInner * kBackgroundInner || s2 > kWindowOuter * kWindowOuter) continue;
                const double basis[3] = {1.0, x - cx, y - cy};
                const double value = row[x];
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) normal[i][j] += basis[i] * basis[j];
                    normal[i][3] += basis[i] * value;
                }
                ++samples;
            }
        }
        std::array<double, 3> plane{};
        if (samples < 24 || !Solve3(normal, plane) || !(plane[0] > 8.0)) return std::nullopt;

        // Dot level relative to the background, from the dot core.
        double core_sum = 0.0;
        std::size_t core_count = 0;
        for (int y = y0; y <= y1; ++y) {
            const auto* row = gray.ptr<std::uint8_t>(y);
            for (int x = x0; x <= x1; ++x) {
                const double s2 = EllipticalRadiusSquared(shape, cx, cy, x, y);
                if (s2 > kDotCore * kDotCore) continue;
                const double background = plane[0] + plane[1] * (x - cx) + plane[2] * (y - cy);
                if (!(background > 8.0)) return std::nullopt;
                core_sum += row[x] / background;
                ++core_count;
            }
        }
        if (core_count == 0) return std::nullopt;
        const double dot_level = core_sum / static_cast<double>(core_count);
        if (!(dot_level < 0.7)) return std::nullopt;

        double weight_sum = 0.0;
        double weighted_x = 0.0;
        double weighted_y = 0.0;
        for (int y = y0; y <= y1; ++y) {
            const auto* row = gray.ptr<std::uint8_t>(y);
            for (int x = x0; x <= x1; ++x) {
                const double s2 = EllipticalRadiusSquared(shape, cx, cy, x, y);
                if (s2 > kWindowOuter * kWindowOuter) continue;
                const double background = plane[0] + plane[1] * (x - cx) + plane[2] * (y - cy);
                const double weight = (1.0 - row[x] / background) / (1.0 - dot_level);
                weight_sum += weight;
                weighted_x += weight * x;
                weighted_y += weight * y;
            }
        }
        if (!(weight_sum > 1.0)) return std::nullopt;
        const double next_x = weighted_x / weight_sum;
        const double next_y = weighted_y / weight_sum;
        if (std::hypot(next_x - start_x, next_y - start_y) > 0.5 * std::min(shape.a, shape.b)) return std::nullopt;
        cx = next_x;
        cy = next_y;
    }
    return std::pair<double, double>{cx, cy};
}

double Percentile(const cv::Mat& gray, double fraction) {
    std::array<std::uint64_t, 256> histogram{};
    for (int y = 0; y < gray.rows; ++y) {
        const auto* row = gray.ptr<std::uint8_t>(y);
        for (int x = 0; x < gray.cols; ++x) ++histogram[row[x]];
    }
    const double target = fraction * static_cast<double>(gray.total());
    double cumulative = 0.0;
    for (int level = 0; level < 256; ++level) {
        cumulative += static_cast<double>(histogram[static_cast<std::size_t>(level)]);
        if (cumulative >= target) return level;
    }
    return 255.0;
}

} // namespace

std::vector<RingCandidate> FindRingCandidates(const cv::Mat& gray, const DetectorParams& params, double* dark_threshold) {
    if (gray.empty() || gray.type() != CV_8UC1) Reject("the image must be non-empty 8-bit grayscale");
    const double low = Percentile(gray, 0.01);
    const double high = Percentile(gray, 0.99);
    const double threshold = low + params.dark_fraction * (high - low);
    if (dark_threshold != nullptr) *dark_threshold = threshold;
    if (!(high - low >= 32.0)) return {};

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(0, 0), params.blur_sigma);
    cv::Mat ink;
    cv::threshold(blurred, ink, threshold, 255, cv::THRESH_BINARY_INV);

    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(ink, contours, hierarchy, cv::RETR_TREE, cv::CHAIN_APPROX_NONE);

    const auto depth_of = [&](std::size_t index) {
        int depth = 0;
        for (int parent = hierarchy[index][3]; parent >= 0; parent = hierarchy[static_cast<std::size_t>(parent)][3]) ++depth;
        return depth;
    };

    std::vector<RingCandidate> found;
    for (std::size_t i = 0; i < contours.size(); ++i) {
        if (contours[i].size() < 8) continue;
        if (depth_of(i) % 2 != 1) continue; // a hole of a dark component
        const double hole_area = std::abs(cv::contourArea(contours[i]));
        const double hole_radius = std::sqrt(hole_area / kPi);
        if (hole_radius < params.min_hole_radius_px || hole_radius > params.max_hole_radius_px) continue;

        const auto hole_box = cv::fitEllipse(contours[i]);
        const Ellipse hole = FromRotatedRect(hole_box);
        const double minor = std::min(hole.a, hole.b);
        const double major = std::max(hole.a, hole.b);
        if (!(minor > 0.0) || minor / major < params.min_axis_ratio) continue;
        if (std::abs(hole_area / (kPi * hole.a * hole.b) - 1.0) > params.ellipse_area_tolerance) continue;

        // The dot: one island inside the hole with the designed share of its area.
        int dot_index = -1;
        int dot_count = 0;
        for (int child = hierarchy[i][2]; child >= 0; child = hierarchy[static_cast<std::size_t>(child)][0]) {
            const double area = std::abs(cv::contourArea(contours[static_cast<std::size_t>(child)]));
            const double ratio = area / hole_area;
            if (ratio >= params.min_dot_area_ratio && ratio <= params.max_dot_area_ratio) {
                dot_index = child;
                ++dot_count;
            } else if (ratio > 0.03) {
                dot_count += 2; // a second sizeable island: not a ring fiducial
            }
        }
        if (dot_count != 1 || contours[static_cast<std::size_t>(dot_index)].size() < 5) continue;
        const auto dot_box = cv::fitEllipse(contours[static_cast<std::size_t>(dot_index)]);
        if (std::hypot(dot_box.center.x - hole.cx, dot_box.center.y - hole.cy) > params.concentric_tolerance * hole_radius) continue;

        // The black ring around the white ring must be dark all the way round.
        int dark = 0;
        constexpr int kProbes = 24;
        for (int k = 0; k < kProbes; ++k) {
            const double t = 2.0 * kPi * k / kProbes;
            const double along = hole.a * kRingProbeScale * std::cos(t);
            const double across = hole.b * kRingProbeScale * std::sin(t);
            const int px = static_cast<int>(std::lround(hole.cx + along * hole.ux - across * hole.uy));
            const int py = static_cast<int>(std::lround(hole.cy + along * hole.uy + across * hole.ux));
            if (px >= 0 && py >= 0 && px < ink.cols && py < ink.rows && ink.at<std::uint8_t>(py, px) != 0) ++dark;
        }
        if (static_cast<double>(dark) < params.min_ring_dark_fraction * kProbes) continue;

        const auto refined = RefineCentre(gray, hole, dot_box.center.x, dot_box.center.y, params.refine_iterations);
        if (!refined) continue;
        found.push_back({refined->first, refined->second, std::sqrt(hole.a * hole.b)});
    }

    // One ring can show up twice when the dot island is split by noise.
    std::sort(found.begin(), found.end(), [](const RingCandidate& l, const RingCandidate& r) {
        return l.x != r.x ? l.x < r.x : l.y < r.y;
    });
    std::vector<RingCandidate> unique;
    for (const auto& candidate : found) {
        bool duplicate = false;
        for (auto it = unique.rbegin(); it != unique.rend() && candidate.x - it->x < candidate.hole_radius_px; ++it) {
            if (std::hypot(candidate.x - it->x, candidate.y - it->y) < 0.5 * candidate.hole_radius_px) { duplicate = true; break; }
        }
        if (!duplicate) unique.push_back(candidate);
    }
    return unique;
}

// ---------------------------------------------------------------------------
// Lattice growth.

void ValidateChart(const ChartDefinition& chart) {
    if (chart.columns == 0 || chart.rows == 0 || chart.columns > kMaximumLatticeIndex || chart.rows > kMaximumLatticeIndex) {
        Reject("chart columns and rows must be 1 to 999");
    }
    if (!std::isfinite(chart.origin_x_mm) || !std::isfinite(chart.origin_y_mm) || !std::isfinite(chart.pitch_mm) || !(chart.pitch_mm > 0.0)) {
        Reject("chart origin must be finite and pitch must be positive");
    }
}

std::string PointId(std::uint32_t column, std::uint32_t row) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "F-%03u-%03u", column, row);
    return text;
}

// ---------------------------------------------------------------------------
// The chart's cell pattern rule (see CellPattern in the header). These are
// design values of the in-house chart, copied here so that the detector does
// not link the generator; the contract test compares them with the generator.

namespace {
// The slanted edges' design levels (0.08 and 0.96) are not used: the dark side
// is held to the view's own dark reference and the light side to paper (#375).
constexpr double kChartPaperLevel = 0.92;
constexpr std::array<double, 6> kChartGrayLevels{0.05, 0.2, 0.4, 0.6, 0.8, 0.95};
} // namespace

CellPattern ExpectedCellPattern(const std::uint32_t column, const std::uint32_t row) noexcept {
    switch ((3U * column + 5U * row) % 8U) {
    case 1: return CellPattern::SlantedEdgeDarkOnLight;
    case 2: return CellPattern::ColorPatch;
    case 3: return CellPattern::FineLinesVertical;
    case 4: return CellPattern::FineLinesHorizontal;
    case 5: return CellPattern::GrayPatch;
    case 7: return CellPattern::SlantedEdgeLightOnDark;
    default: return CellPattern::Blank;
    }
}

const char* CellPatternCode(const CellPattern pattern) noexcept {
    switch (pattern) {
    case CellPattern::Blank: return "blank";
    case CellPattern::SlantedEdgeDarkOnLight: return "slanted-edge-dark-on-light";
    case CellPattern::SlantedEdgeLightOnDark: return "slanted-edge-light-on-dark";
    case CellPattern::ColorPatch: return "color-patch";
    case CellPattern::GrayPatch: return "gray-patch";
    case CellPattern::FineLinesVertical: return "fine-lines-vertical";
    case CellPattern::FineLinesHorizontal: return "fine-lines-horizontal";
    }
    return "";
}

double ExpectedGrayPatchLevel(const std::uint32_t column, const std::uint32_t row) noexcept {
    return kChartGrayLevels[(column + row) % kChartGrayLevels.size()];
}

namespace {

struct Slot {
    enum class Status { Unvisited, Detected, Missing, Ambiguous, Outside };
    Status status{Status::Unvisited};
    double x{};
    double y{};
    int candidate{-1};
};

struct Prediction {
    double x;
    double y;
    double spacing;
};

class Lattice {
public:
    Lattice(const ChartDefinition& chart, const std::vector<RingCandidate>& candidates, int width, int height,
        const DetectorParams& params)
        : chart_(chart), candidates_(candidates), width_(width), height_(height), params_(params),
          slots_(chart.NodeCount()), used_(candidates.size(), false) {
        std::vector<double> radii;
        for (const auto& c : candidates) radii.push_back(c.hole_radius_px);
        if (!radii.empty()) {
            std::nth_element(radii.begin(), radii.begin() + static_cast<std::ptrdiff_t>(radii.size() / 2), radii.end());
            median_radius_ = radii[radii.size() / 2];
        }
    }

    Slot& At(std::uint32_t column, std::uint32_t row) { return slots_[row * (chart_.columns + 1U) + column]; }
    const Slot& At(std::uint32_t column, std::uint32_t row) const { return slots_[row * (chart_.columns + 1U) + column]; }

    void Detect(std::uint32_t column, std::uint32_t row, int candidate) {
        auto& slot = At(column, row);
        slot.status = Slot::Status::Detected;
        slot.candidate = candidate;
        slot.x = candidates_[static_cast<std::size_t>(candidate)].x;
        slot.y = candidates_[static_cast<std::size_t>(candidate)].y;
        used_[static_cast<std::size_t>(candidate)] = true;
    }

    // Local affine map of the lattice index to pixels, from the nearest detected
    // nodes. Lens distortion and perspective are handled by keeping it local.
    std::optional<Prediction> Predict(std::uint32_t column, std::uint32_t row) const {
        struct Neighbour { double dc; double dr; double x; double y; double d2; };
        std::vector<Neighbour> nearby;
        const int c0 = static_cast<int>(column);
        const int r0 = static_cast<int>(row);
        for (int r = std::max(0, r0 - 3); r <= std::min<int>(static_cast<int>(chart_.rows), r0 + 3); ++r) {
            for (int c = std::max(0, c0 - 3); c <= std::min<int>(static_cast<int>(chart_.columns), c0 + 3); ++c) {
                const auto& slot = At(static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(r));
                if (slot.status != Slot::Status::Detected) continue;
                const double dc = c - c0;
                const double dr = r - r0;
                nearby.push_back({dc, dr, slot.x, slot.y, dc * dc + dr * dr});
            }
        }
        if (nearby.size() < 3) return std::nullopt;
        std::sort(nearby.begin(), nearby.end(), [](const Neighbour& l, const Neighbour& r) { return l.d2 < r.d2; });
        if (nearby.size() > 12) nearby.resize(12);

        // Least squares x = a0 + a1 dc + a2 dr (the target is at dc = dr = 0).
        std::array<std::array<double, 4>, 3> nx{};
        std::array<std::array<double, 4>, 3> ny{};
        for (const auto& n : nearby) {
            const double basis[3] = {1.0, n.dc, n.dr};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) { nx[i][j] += basis[i] * basis[j]; ny[i][j] += basis[i] * basis[j]; }
                nx[i][3] += basis[i] * n.x;
                ny[i][3] += basis[i] * n.y;
            }
        }
        std::array<double, 3> ax{};
        std::array<double, 3> ay{};
        if (!Solve3(nx, ax) || !Solve3(ny, ay)) return std::nullopt;
        const double determinant = ax[1] * ay[2] - ax[2] * ay[1];
        if (!(determinant > 0.0)) return std::nullopt; // a mirrored local map is never accepted
        return Prediction{ax[0], ay[0], std::min(std::hypot(ax[1], ay[1]), std::hypot(ax[2], ay[2]))};
    }

    // Edge of the image where a ring would be cut: a node closer than this is not examined.
    double Margin() const { return 2.2 * median_radius_; }

    bool InsideForExamination(const Prediction& p) const {
        const double m = Margin();
        return p.x >= m && p.y >= m && p.x <= width_ - 1 - m && p.y <= height_ - 1 - m;
    }

    std::size_t DetectedNeighbours(std::uint32_t column, std::uint32_t row) const {
        std::size_t count = 0;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                const int c = static_cast<int>(column) + dc;
                const int r = static_cast<int>(row) + dr;
                if (c < 0 || r < 0 || c > static_cast<int>(chart_.columns) || r > static_cast<int>(chart_.rows)) continue;
                if (At(static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(r)).status == Slot::Status::Detected) ++count;
            }
        }
        return count;
    }

    // Returns true when a node was newly detected.
    bool Examine(std::uint32_t column, std::uint32_t row) {
        auto& slot = At(column, row);
        const auto prediction = Predict(column, row);
        if (!prediction) return false;
        if (!InsideForExamination(*prediction)) { slot.status = Slot::Status::Outside; return false; }
        const double tolerance = params_.accept_fraction * prediction->spacing;
        int found = -1;
        int count = 0;
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            if (used_[i]) continue;
            if (std::hypot(candidates_[i].x - prediction->x, candidates_[i].y - prediction->y) <= tolerance) {
                found = static_cast<int>(i);
                ++count;
            }
        }
        if (count == 1) { Detect(column, row, found); return true; }
        slot.status = count == 0 ? Slot::Status::Missing : Slot::Status::Ambiguous;
        return false;
    }

    void Grow() {
        for (;;) {
            struct Item { std::uint32_t column; std::uint32_t row; std::size_t neighbours; };
            std::vector<Item> frontier;
            for (std::uint32_t row = 0; row <= chart_.rows; ++row) {
                for (std::uint32_t column = 0; column <= chart_.columns; ++column) {
                    const auto status = At(column, row).status;
                    if (status != Slot::Status::Unvisited && status != Slot::Status::Missing) continue;
                    const auto neighbours = DetectedNeighbours(column, row);
                    if (neighbours > 0) frontier.push_back({column, row, neighbours});
                }
            }
            std::stable_sort(frontier.begin(), frontier.end(), [](const Item& l, const Item& r) { return l.neighbours > r.neighbours; });
            bool progress = false;
            for (const auto& item : frontier) {
                const auto status = At(item.column, item.row).status;
                if (status != Slot::Status::Unvisited && status != Slot::Status::Missing) continue;
                if (Examine(item.column, item.row)) progress = true;
            }
            if (!progress) return;
        }
    }

    std::uint32_t UnmatchedCandidates() const {
        return static_cast<std::uint32_t>(std::count(used_.begin(), used_.end(), false));
    }

private:
    ChartDefinition chart_;
    const std::vector<RingCandidate>& candidates_;
    int width_;
    int height_;
    DetectorParams params_;
    std::vector<Slot> slots_;
    std::vector<bool> used_;
    double median_radius_{};
};

ViewDetection Fail(ViewDetection result, std::string code, std::string detail) {
    result.failure = std::move(code);
    result.failure_detail = std::move(detail);
    result.nodes.clear();
    return result;
}

// ---------------------------------------------------------------------------
// Identity check: does the pattern printed in a cell agree with the node names?
//
// A cell whose four corner nodes are detected is mapped to the image by the
// homography of its corners (a local map, so lens distortion stays small
// inside one cell). Positions inside the cell are in units of the lattice
// pitch from the middle of the cell: s along the columns, t along the rows.
// Four zones are measured, all as a ratio to the white of the same cell, which
// cancels exposure and vignetting:
//   white  - strips near the middle of the cell edges (paper everywhere);
//   ring   - a thin band just outside the pattern area but inside the backing
//            of a slanted edge (paper, except dark around a light-on-dark edge);
//   centre - the middle of the pattern area.
// In the centre the spread of the levels (90th - 10th percentile) tells a flat
// patch from fine lines, and the direction of the lines is read from how often
// the level crosses its mid value along the columns and along the rows of a
// regular resampling of the centre (a gradient direction is not used: the
// widest bands of the lines are only a few pixels wide and the narrow ones
// alias, which makes a gradient direction unreliable).

constexpr double kCenterHalf = 0.15;
constexpr double kRingInner = 0.272;
constexpr double kRingOuter = 0.288;
constexpr double kRingHalfLength = 0.10;
constexpr double kWhiteInner = 0.34;
constexpr double kWhiteOuter = 0.41;
constexpr double kWhiteHalfLength = 0.18;
constexpr double kMinimumCellSidePx = 80.0;
constexpr std::size_t kMinimumZoneSamples = 60;
constexpr std::size_t kMaximumRecordedMismatches = 16;
constexpr double kResampleStepPx = 0.5;
constexpr double kCrossingHysteresis = 0.15; // of the (90th - 10th percentile) range, each side of the mid value

struct CellMeasurement {
    double center_ratio{};  // median of the centre / white
    double ring_ratio{};    // median of the ring band / white
    double spread{};        // (90th - 10th percentile of the centre) / white
    double axis_share{0.5}; // crossings along the columns / all crossings; 0.5 when the centre is flat
};

// Pixel at a fractional position (bilinear). The caller keeps the position inside the image.
double Bilinear(const cv::Mat& gray, double x, double y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0;
    const double fy = y - y0;
    const auto* r0 = gray.ptr<std::uint8_t>(y0);
    const auto* r1 = gray.ptr<std::uint8_t>(y0 + 1);
    return (1.0 - fy) * ((1.0 - fx) * r0[x0] + fx * r0[x0 + 1]) + fy * ((1.0 - fx) * r1[x0] + fx * r1[x0 + 1]);
}

// Number of times a sequence moves from below to above the band around `mid`, or back.
std::uint32_t Crossings(const std::vector<double>& line, double mid, double margin) {
    std::uint32_t count = 0;
    int state = 0;
    for (const double value : line) {
        const int now = value > mid + margin ? 1 : (value < mid - margin ? -1 : 0);
        if (now == 0) continue;
        if (state != 0 && now != state) ++count;
        state = now;
    }
    return count;
}

double MedianOf(std::vector<float>& values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// `corners` are top-left, top-right, bottom-right and bottom-left in pixels.
std::optional<CellMeasurement> MeasureCell(const cv::Mat& gray, const std::array<std::array<double, 2>, 4>& corners,
    const DetectorParams& params) {
    cv::Point2f source[4] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
    cv::Point2f target[4];
    double min_x = 1e30, max_x = -1e30, min_y = 1e30, max_y = -1e30;
    for (std::size_t i = 0; i < 4; ++i) {
        target[i] = {static_cast<float>(corners[i][0]), static_cast<float>(corners[i][1])};
        min_x = std::min(min_x, corners[i][0]);
        max_x = std::max(max_x, corners[i][0]);
        min_y = std::min(min_y, corners[i][1]);
        max_y = std::max(max_y, corners[i][1]);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& a = corners[i];
        const auto& b = corners[(i + 1) % 4];
        if (std::hypot(b[0] - a[0], b[1] - a[1]) < kMinimumCellSidePx) return std::nullopt;
    }
    const int x0 = static_cast<int>(std::floor(min_x));
    const int x1 = static_cast<int>(std::ceil(max_x));
    const int y0 = static_cast<int>(std::floor(min_y));
    const int y1 = static_cast<int>(std::ceil(max_y));
    if (x0 < 2 || y0 < 2 || x1 > gray.cols - 3 || y1 > gray.rows - 3) return std::nullopt;

    const cv::Mat forward = cv::getPerspectiveTransform(source, target);
    cv::Mat inverse;
    if (cv::invert(forward, inverse, cv::DECOMP_LU) == 0.0) return std::nullopt;
    const auto f = [&](int r, int c) { return forward.at<double>(r, c); };
    const auto g = [&](int r, int c) { return inverse.at<double>(r, c); };

    std::vector<float> center;
    std::vector<float> ring;
    std::vector<float> white;
    for (int y = y0; y <= y1; ++y) {
        const auto* row = gray.ptr<std::uint8_t>(y);
        for (int x = x0; x <= x1; ++x) {
            const double w = g(2, 0) * x + g(2, 1) * y + g(2, 2);
            if (!(std::abs(w) > 1e-12)) return std::nullopt;
            const double u = (g(0, 0) * x + g(0, 1) * y + g(0, 2)) / w;
            const double v = (g(1, 0) * x + g(1, 1) * y + g(1, 2)) / w;
            const double s = u - 0.5;
            const double t = v - 0.5;
            const double as = std::abs(s);
            const double at = std::abs(t);
            const double m = std::max(as, at);
            if (m > kWhiteOuter) continue;
            const float value = row[x];
            if (m <= kCenterHalf) {
                center.push_back(value);
            } else if ((as >= kRingInner && as <= kRingOuter && at <= kRingHalfLength)
                || (at >= kRingInner && at <= kRingOuter && as <= kRingHalfLength)) {
                ring.push_back(value);
            } else if (m >= kWhiteInner && std::min(as, at) <= kWhiteHalfLength) {
                white.push_back(value);
            }
        }
    }
    if (center.size() < kMinimumZoneSamples || ring.size() < kMinimumZoneSamples || white.size() < kMinimumZoneSamples) return std::nullopt;
    const double paper = MedianOf(white);
    if (!(paper >= 16.0)) return std::nullopt;

    CellMeasurement measurement;
    std::sort(center.begin(), center.end());
    measurement.center_ratio = center[center.size() / 2] / paper;
    measurement.spread = (center[center.size() * 9 / 10] - center[center.size() / 10]) / paper;
    measurement.ring_ratio = MedianOf(ring) / paper;
    if (measurement.spread < params.identity_line_spread) return measurement; // flat: no line direction to read

    // Resample the centre on a regular grid of the cell axes (about half a pixel apart) and count
    // the level crossings along each row (the columns direction s) and each column (the rows direction t).
    const double side_px = 0.25 * (std::hypot(corners[1][0] - corners[0][0], corners[1][1] - corners[0][1])
        + std::hypot(corners[2][0] - corners[1][0], corners[2][1] - corners[1][1])
        + std::hypot(corners[3][0] - corners[2][0], corners[3][1] - corners[2][1])
        + std::hypot(corners[0][0] - corners[3][0], corners[0][1] - corners[3][1]));
    const int n = std::clamp(static_cast<int>(std::ceil(2.0 * kCenterHalf * side_px / kResampleStepPx)), 16, 400);
    std::vector<double> grid(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j) {
        const double v = 0.5 - kCenterHalf + 2.0 * kCenterHalf * (j + 0.5) / n;
        for (int i = 0; i < n; ++i) {
            const double u = 0.5 - kCenterHalf + 2.0 * kCenterHalf * (i + 0.5) / n;
            const double w = f(2, 0) * u + f(2, 1) * v + f(2, 2);
            if (!std::isfinite(w) || !(std::abs(w) > 1e-12)) return std::nullopt;
            const double x = (f(0, 0) * u + f(0, 1) * v + f(0, 2)) / w;
            const double y = (f(1, 0) * u + f(1, 1) * v + f(1, 2)) / w;
            if (!std::isfinite(x) || !std::isfinite(y)
                || x < 0.0 || y < 0.0 || x >= gray.cols - 1.0 || y >= gray.rows - 1.0) return std::nullopt;
            grid[static_cast<std::size_t>(j) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i)] = Bilinear(gray, x, y);
        }
    }
    const double low = center[center.size() / 10];
    const double high = center[center.size() * 9 / 10];
    const double mid = 0.5 * (low + high);
    const double margin = kCrossingHysteresis * (high - low);
    std::uint32_t along_s = 0;
    std::uint32_t along_t = 0;
    std::vector<double> line(static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) line[static_cast<std::size_t>(i)] = grid[static_cast<std::size_t>(j) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i)];
        along_s += Crossings(line, mid, margin);
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) line[static_cast<std::size_t>(j)] = grid[static_cast<std::size_t>(j) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i)];
        along_t += Crossings(line, mid, margin);
    }
    const double total = static_cast<double>(along_s) + static_cast<double>(along_t);
    if (total > 0.0) measurement.axis_share = static_cast<double>(along_s) / total;
    return measurement;
}

// Tone of the view (#375). A printed and photographed chart does not keep the
// design levels: the dark side lifts (ink, flare, the camera's tone curve) and
// the middle greys move. The levels that the design cannot fix are measured in
// the view itself, from the cells the node names expect, and then every cell is
// held to them with the same tolerance as before:
//   - the dark side of the slanted edges is compared with the median dark side
//     of all the expected slanted edges of the view;
//   - the grey patches are compared through the view's tone map: piecewise
//     linear from each expected level to the median measured level of its
//     cells. The residual of a cell is its distance from its level's median,
//     taken back to design units through the map.
// Both references come from the assignment under test. A wrong assignment gives
// references that do not fit (a dark reference near paper, a grey map that is
// flat or falls, large residuals), so both are bounded.

// The median dark side of the expected slanted edges must be at most this
// fraction of the paper: the dark side is printed as near black, and halfway to
// paper is the most a view may lift it and still call it dark.
constexpr double kEdgeDarkReferenceMaximum = 0.5;
// From each expected grey level to the next, the view's map must rise by at least
// this fraction of the design step. Without a lower bound a flat map (for
// example blank cells in the place of the grey patches) would fit every level
// with no residual, and a map that falls would accept levels in the wrong order.
constexpr double kGrayMinimumRise = 0.5;
// The map needs this many distinct expected levels: the chart's grey patches
// show three ((column + row) is odd in a grey cell, so the levels are the
// second, fourth and sixth), and all three must be seen for the order to be checked.
constexpr std::size_t kGrayMinimumFitLevels = 3;

double MedianValue(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

// Structure of the pattern an expected kind has: where the levels are.
// `edge_dark` is the view's dark reference; absent when no slanted edge was measured.
bool StructureAgrees(const CellPattern expected, const CellMeasurement& m, const DetectorParams& params,
    const std::optional<double> edge_dark) {
    const double tolerance = params.identity_level_tolerance;
    const bool ring_is_paper = std::abs(m.ring_ratio - 1.0) <= tolerance;
    const bool flat = m.spread < params.identity_line_spread;
    const bool dark_known = edge_dark.has_value() && *edge_dark <= kEdgeDarkReferenceMaximum;
    switch (expected) {
    case CellPattern::Blank:
        return ring_is_paper && flat && std::abs(m.center_ratio - 1.0) <= tolerance;
    case CellPattern::SlantedEdgeDarkOnLight:
        // The light side is printed as paper (the generator's 0.96 / 0.92 lies within the tolerance).
        return dark_known && flat && ring_is_paper && std::abs(m.center_ratio - *edge_dark) <= tolerance;
    case CellPattern::SlantedEdgeLightOnDark:
        return dark_known && flat && std::abs(m.center_ratio - 1.0) <= tolerance && std::abs(m.ring_ratio - *edge_dark) <= tolerance;
    case CellPattern::ColorPatch:
        // The colour is not compared: the image is read as grey, and the patch levels overlap the grey ones.
        return ring_is_paper && flat && m.center_ratio >= 0.25 && m.center_ratio <= 0.95;
    case CellPattern::GrayPatch:
        return ring_is_paper && flat;
    case CellPattern::FineLinesVertical:
        return ring_is_paper && !flat && m.axis_share >= params.identity_line_share;
    case CellPattern::FineLinesHorizontal:
        return ring_is_paper && !flat && m.axis_share <= 1.0 - params.identity_line_share;
    }
    return false;
}

bool Rejects(const std::uint32_t mismatched, const std::uint32_t examined, const DetectorParams& params) {
    return mismatched >= params.identity_min_mismatches
        && static_cast<double>(mismatched) > params.identity_max_mismatch_fraction * static_cast<double>(examined);
}

void CheckIdentity(const cv::Mat& gray, const ChartDefinition& chart, const std::vector<NodeObservation>& nodes,
    const DetectorParams& params, IdentityCheck& identity) {
    const std::size_t stride = static_cast<std::size_t>(chart.columns) + 1U;
    std::vector<const NodeObservation*> detected(chart.NodeCount(), nullptr);
    for (const auto& node : nodes) {
        if (node.state == NodeState::Detected) detected[node.row * stride + node.column] = &node;
    }
    const auto record = [&](std::uint32_t column, std::uint32_t row, CellPattern expected, const char* check, const CellMeasurement& m) {
        if (identity.mismatches.size() >= kMaximumRecordedMismatches) return;
        identity.mismatches.push_back({column, row, expected, check, m.center_ratio, m.ring_ratio, m.spread, m.axis_share});
    };

    // Measure every cell with four detected corners first: the view's references need all of them.
    struct MeasuredCell {
        std::uint32_t column;
        std::uint32_t row;
        CellPattern expected;
        CellMeasurement m;
    };
    std::vector<MeasuredCell> cells;
    std::vector<double> dark_sides;
    for (std::uint32_t row = 0; row < chart.rows; ++row) {
        for (std::uint32_t column = 0; column < chart.columns; ++column) {
            const auto* tl = detected[row * stride + column];
            const auto* tr = detected[row * stride + column + 1];
            const auto* br = detected[(row + 1) * stride + column + 1];
            const auto* bl = detected[(row + 1) * stride + column];
            if (tl == nullptr || tr == nullptr || br == nullptr || bl == nullptr) continue;
            const std::array<std::array<double, 2>, 4> corners{{{tl->x_px, tl->y_px}, {tr->x_px, tr->y_px}, {br->x_px, br->y_px}, {bl->x_px, bl->y_px}}};
            const auto measurement = MeasureCell(gray, corners, params);
            if (!measurement) { ++identity.cells_skipped; continue; }
            ++identity.cells_examined;
            const auto expected = ExpectedCellPattern(column, row);
            if (expected == CellPattern::SlantedEdgeDarkOnLight) dark_sides.push_back(measurement->center_ratio);
            if (expected == CellPattern::SlantedEdgeLightOnDark) dark_sides.push_back(measurement->ring_ratio);
            cells.push_back({column, row, expected, *measurement});
        }
    }
    identity.edge_cells_examined = static_cast<std::uint32_t>(dark_sides.size());
    if (!dark_sides.empty()) identity.edge_dark_reference = MedianValue(dark_sides);

    std::vector<const MeasuredCell*> grays;
    for (const auto& cell : cells) {
        if (!StructureAgrees(cell.expected, cell.m, params, identity.edge_dark_reference)) {
            ++identity.cells_mismatched;
            record(cell.column, cell.row, cell.expected, "structure", cell.m);
            continue;
        }
        if (cell.expected == CellPattern::GrayPatch) grays.push_back(&cell);
    }
    identity.gray_cells_examined = static_cast<std::uint32_t>(grays.size());

    // Grey levels through the view's tone map: piecewise linear through the median measured
    // level of each expected level, which must rise from each level to the next.
    auto& fit = identity.gray_fit;
    std::map<double, std::vector<double>> by_level;
    for (const auto* cell : grays) {
        const double x = ExpectedGrayPatchLevel(cell->column, cell->row) / kChartPaperLevel;
        by_level[x].push_back(cell->m.center_ratio);
        fit.cells.push_back({cell->column, cell->row, x, cell->m.center_ratio, 0.0});
    }
    for (const auto& [x, values] : by_level) {
        fit.knots.push_back({x, MedianValue(values), static_cast<std::uint32_t>(values.size())});
    }
    const auto rise = [&fit](std::size_t i) {
        return (fit.knots[i + 1].measured - fit.knots[i].measured) / (fit.knots[i + 1].expected - fit.knots[i].expected);
    };
    if (fit.knots.size() < kGrayMinimumFitLevels) {
        fit.status = "too-few-levels";
    } else {
        fit.min_rise_ratio = rise(0);
        for (std::size_t i = 1; i + 1 < fit.knots.size(); ++i) fit.min_rise_ratio = std::min(fit.min_rise_ratio, rise(i));
        fit.status = fit.min_rise_ratio >= kGrayMinimumRise ? "fitted" : "not-rising";
    }
    if (fit.status == "fitted") {
        double sum2 = 0.0;
        for (std::size_t i = 0; i < grays.size(); ++i) {
            auto& c = fit.cells[i];
            // Residual in design units: the level the map gives for the measurement, minus the
            // expected level. The map is inverted on the segment the measurement lies towards.
            std::size_t k = 0;
            while (fit.knots[k].expected != c.expected) ++k;
            const double difference = c.measured - fit.knots[k].measured;
            const bool up = difference >= 0.0 ? k + 1 < fit.knots.size() : k == 0;
            c.residual = difference / rise(up ? k : k - 1);
            sum2 += c.residual * c.residual;
            fit.residual_max_abs = std::max(fit.residual_max_abs, std::abs(c.residual));
            if (std::abs(c.residual) > params.identity_level_tolerance) {
                ++identity.gray_cells_mismatched;
                record(grays[i]->column, grays[i]->row, CellPattern::GrayPatch, "gray-level", grays[i]->m);
            }
        }
        fit.residual_rms = grays.empty() ? 0.0 : std::sqrt(sum2 / static_cast<double>(grays.size()));
    } else if (fit.status == "not-rising") {
        // The levels do not rise in the order the node names give: no grey patch shows its level.
        identity.gray_cells_mismatched = identity.gray_cells_examined;
        for (const auto* cell : grays) record(cell->column, cell->row, CellPattern::GrayPatch, "gray-tone", cell->m);
    }

    if (Rejects(identity.cells_mismatched, identity.cells_examined, params)
        || Rejects(identity.gray_cells_mismatched, identity.gray_cells_examined, params)) {
        identity.result = "mismatch";
    } else if (identity.cells_examined < params.identity_min_cells || identity.gray_cells_examined < params.identity_min_gray_cells
        || fit.status != "fitted") {
        identity.result = "unverifiable";
    } else {
        identity.result = "verified";
    }
}

} // namespace

ViewDetection DetectView(const cv::Mat& gray, const ChartDefinition& chart,
    std::span<const LatticeAnchor> anchors, const DetectorParams& params) {
    ValidateChart(chart);
    if (!std::isfinite(params.identity_level_tolerance) || !(params.identity_level_tolerance > 0.0 && params.identity_level_tolerance < 0.5)
        || !std::isfinite(params.identity_line_spread) || !(params.identity_line_spread > 0.0)
        || !std::isfinite(params.identity_line_share) || !(params.identity_line_share > 0.5 && params.identity_line_share <= 1.0)
        || !std::isfinite(params.identity_max_mismatch_fraction)
        || !(params.identity_max_mismatch_fraction >= 0.0 && params.identity_max_mismatch_fraction < 1.0)
        || params.identity_min_cells == 0 || params.identity_min_gray_cells == 0 || params.identity_min_mismatches == 0) {
        Reject("identity check parameters must be finite and cannot disable verification");
    }
    ViewDetection result;
    result.candidates = FindRingCandidates(gray, params, &result.dark_threshold);
    if (result.candidates.empty()) return Fail(std::move(result), "no-rings", "no ring fiducial was found in the image");
    if (anchors.size() < 3) return Fail(std::move(result), "anchor-count", "at least three anchors are required");

    Lattice lattice(chart, result.candidates, gray.cols, gray.rows, params);
    std::vector<int> bound;
    for (const auto& anchor : anchors) {
        AnchorResolution resolution{anchor, std::nullopt};
        result.anchors.push_back(resolution);
    }
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        const auto& anchor = anchors[i];
        if (anchor.column > chart.columns || anchor.row > chart.rows) {
            return Fail(std::move(result), "anchor-outside-chart", "an anchor names a node outside the chart");
        }
        if (!std::isfinite(anchor.x_px) || !std::isfinite(anchor.y_px) || !std::isfinite(anchor.radius_px) || !(anchor.radius_px > 0.0)) {
            return Fail(std::move(result), "anchor-invalid", "an anchor has a non-finite value or a non-positive radius");
        }
        if (lattice.At(anchor.column, anchor.row).status != Slot::Status::Unvisited) {
            return Fail(std::move(result), "anchor-duplicate", "two anchors name the same node");
        }
        int found = -1;
        int count = 0;
        for (std::size_t k = 0; k < result.candidates.size(); ++k) {
            if (std::hypot(result.candidates[k].x - anchor.x_px, result.candidates[k].y - anchor.y_px) <= anchor.radius_px) {
                found = static_cast<int>(k);
                ++count;
            }
        }
        if (count == 0) return Fail(std::move(result), "anchor-not-found", "no ring lies inside an anchor's search radius");
        if (count > 1) return Fail(std::move(result), "anchor-ambiguous", "more than one ring lies inside an anchor's search radius");
        if (std::find(bound.begin(), bound.end(), found) != bound.end()) {
            return Fail(std::move(result), "anchor-duplicate", "two anchors resolve to the same ring");
        }
        bound.push_back(found);
        result.anchors[i].ring = result.candidates[static_cast<std::size_t>(found)];
        lattice.Detect(anchor.column, anchor.row, found);
    }

    // The anchors must span the lattice in two directions, and no triple of
    // them may be mirrored: the chart is seen from the front, so the lattice
    // axes (column right, row down) keep their handedness in the image. Every
    // non-degenerate triple is checked, not only the first.
    {
        for (std::size_t i = 0; i + 2 < anchors.size(); ++i) {
            for (std::size_t j = i + 1; j + 1 < anchors.size(); ++j) {
                for (std::size_t k = j + 1; k < anchors.size(); ++k) {
                    const double lattice_area =
                        (static_cast<double>(anchors[j].column) - anchors[i].column) * (static_cast<double>(anchors[k].row) - anchors[i].row)
                        - (static_cast<double>(anchors[k].column) - anchors[i].column) * (static_cast<double>(anchors[j].row) - anchors[i].row);
                    if (lattice_area == 0.0) continue;
                    const auto& a = *result.anchors[i].ring;
                    const auto& b = *result.anchors[j].ring;
                    const auto& c = *result.anchors[k].ring;
                    const double pixel_area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
                    if (pixel_area == 0.0) continue;
                    ++result.identity.anchor_triples;
                    if ((lattice_area > 0.0) != (pixel_area > 0.0)) ++result.identity.mirrored_triples;
                }
            }
        }
        if (result.identity.anchor_triples == 0) return Fail(std::move(result), "anchor-collinear", "the anchors do not span two lattice directions");
        if (result.identity.mirrored_triples != 0) {
            result.identity.result = "mismatch";
            return Fail(std::move(result), "anchor-mirrored", "the node names of the anchors give a mirrored lattice, which a chart seen from the front cannot have");
        }
    }

    lattice.Grow();

    for (std::uint32_t row = 0; row <= chart.rows; ++row) {
        for (std::uint32_t column = 0; column <= chart.columns; ++column) {
            const auto& slot = lattice.At(column, row);
            if (slot.status == Slot::Status::Detected) {
                result.nodes.push_back({column, row, NodeState::Detected, slot.x, slot.y});
            } else if (slot.status == Slot::Status::Missing) {
                result.nodes.push_back({column, row, NodeState::Missing, 0.0, 0.0});
            } else if (slot.status == Slot::Status::Ambiguous) {
                result.nodes.push_back({column, row, NodeState::Ambiguous, 0.0, 0.0});
            }
        }
    }
    result.unmatched_candidates = lattice.UnmatchedCandidates();

    CheckIdentity(gray, chart, result.nodes, params, result.identity);
    if (result.identity.result == "mismatch") {
        return Fail(std::move(result), "identity-mismatch", "the cell patterns do not agree with the node names the anchors gave");
    }
    if (result.identity.result == "unverifiable") {
        return Fail(std::move(result), "identity-unverifiable", "too few cells with four detected corners could be checked against the chart");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Job, session and report.

namespace {

using a0::common::protocol_json::JsonKind;
using a0::common::protocol_json::JsonValue;

struct JobFailure {
    [[noreturn]] static void Fail(std::string_view code, std::string_view) { Reject("job JSON " + std::string(code)); }
};

const JsonValue& Member(const JsonValue& object, const std::string& key, const std::string& path) {
    if (object.kind != JsonKind::object) Reject(path + " must be an object");
    const auto it = object.object.find(key);
    if (it == object.object.end()) Reject(path + "/" + key + " is missing");
    return it->second;
}

void ExactKeys(const JsonValue& object, const std::set<std::string>& keys, const std::string& path) {
    if (object.kind != JsonKind::object) Reject(path + " must be an object");
    for (const auto& [key, value] : object.object) {
        (void)value;
        if (!keys.contains(key)) Reject(path + " has an unknown key");
    }
    for (const auto& key : keys) {
        if (!object.object.contains(key)) Reject(path + "/" + key + " is missing");
    }
}

const std::string& String(const JsonValue& value, const std::string& path) {
    if (value.kind != JsonKind::string) Reject(path + " must be a string");
    return value.string;
}

double Number(const JsonValue& value, const std::string& path) {
    if (value.kind != JsonKind::number) Reject(path + " must be a number");
    double result = 0.0;
    const auto* begin = value.string.data();
    const auto* end = begin + value.string.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(result)) Reject(path + " is not a finite number");
    return result;
}

std::uint32_t Integer(const JsonValue& value, const std::string& path, std::uint32_t maximum) {
    if (value.kind != JsonKind::number) Reject(path + " must be an integer");
    std::uint32_t result = 0;
    const auto* begin = value.string.data();
    const auto* end = begin + value.string.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result > maximum) Reject(path + " is not an integer in range");
    return result;
}

struct ViewJob {
    std::string id;
    std::string camera;
    std::string role;
    std::string image;
    std::vector<LatticeAnchor> anchors;
};

struct CameraJob {
    std::uint32_t width{};
    std::uint32_t height{};
    std::string lens;
    std::string focal;
    std::string focus;
};

struct Job {
    std::string session_id;
    std::string chart_id;
    std::string measured_at;
    ChartDefinition chart;
    std::map<std::string, CameraJob> cameras;
    std::vector<ViewJob> views;
};

Job ParseJob(std::string_view text) {
    const auto root = a0::common::protocol_json::BasicJsonParser<JobFailure, static_cast<std::size_t>(kMaximumJobBytes)>(text).Parse();
    ExactKeys(root, {"schema", "sessionId", "chartId", "measuredAtUtc", "chart", "cameras", "views"}, "job");
    if (String(Member(root, "schema", "job"), "job/schema") != kJobSchemaVersion) Reject("job/schema is not supported");
    Job job;
    job.session_id = String(Member(root, "sessionId", "job"), "job/sessionId");
    job.chart_id = String(Member(root, "chartId", "job"), "job/chartId");
    job.measured_at = String(Member(root, "measuredAtUtc", "job"), "job/measuredAtUtc");

    const auto& chart = Member(root, "chart", "job");
    ExactKeys(chart, {"columns", "rows", "originXMm", "originYMm", "pitchMm"}, "job/chart");
    job.chart.columns = Integer(Member(chart, "columns", "job/chart"), "job/chart/columns", kMaximumLatticeIndex);
    job.chart.rows = Integer(Member(chart, "rows", "job/chart"), "job/chart/rows", kMaximumLatticeIndex);
    job.chart.origin_x_mm = Number(Member(chart, "originXMm", "job/chart"), "job/chart/originXMm");
    job.chart.origin_y_mm = Number(Member(chart, "originYMm", "job/chart"), "job/chart/originYMm");
    job.chart.pitch_mm = Number(Member(chart, "pitchMm", "job/chart"), "job/chart/pitchMm");
    ValidateChart(job.chart);

    const auto& cameras = Member(root, "cameras", "job");
    ExactKeys(cameras, {"CAM-A", "CAM-B"}, "job/cameras");
    for (const char* alias : {"CAM-A", "CAM-B"}) {
        const std::string path = std::string("job/cameras/") + alias;
        const auto& camera = Member(cameras, alias, "job/cameras");
        ExactKeys(camera, {"widthPixels", "heightPixels", "lensLabel", "focalLengthLabel", "focusLabel"}, path);
        CameraJob entry;
        entry.width = Integer(Member(camera, "widthPixels", path), path + "/widthPixels", 32768);
        entry.height = Integer(Member(camera, "heightPixels", path), path + "/heightPixels", 32768);
        entry.lens = String(Member(camera, "lensLabel", path), path + "/lensLabel");
        entry.focal = String(Member(camera, "focalLengthLabel", path), path + "/focalLengthLabel");
        entry.focus = String(Member(camera, "focusLabel", path), path + "/focusLabel");
        job.cameras[alias] = entry;
    }

    const auto& views = Member(root, "views", "job");
    if (views.kind != JsonKind::array || views.array.empty() || views.array.size() > 256) Reject("job/views must be a non-empty array of at most 256");
    for (std::size_t i = 0; i < views.array.size(); ++i) {
        const std::string path = "job/views/" + std::to_string(i);
        const auto& view = views.array[i];
        ExactKeys(view, {"viewId", "camera", "role", "image", "anchors"}, path);
        ViewJob entry;
        entry.id = String(Member(view, "viewId", path), path + "/viewId");
        entry.camera = String(Member(view, "camera", path), path + "/camera");
        entry.role = String(Member(view, "role", path), path + "/role");
        entry.image = String(Member(view, "image", path), path + "/image");
        if (!job.cameras.contains(entry.camera)) Reject(path + "/camera must be CAM-A or CAM-B");
        if (entry.role != "training" && entry.role != "validation" && entry.role != "reference") Reject(path + "/role is not supported");
        const auto& anchors = Member(view, "anchors", path);
        if (anchors.kind != JsonKind::array || anchors.array.size() > 64) Reject(path + "/anchors must be an array of at most 64");
        for (std::size_t k = 0; k < anchors.array.size(); ++k) {
            const std::string apath = path + "/anchors/" + std::to_string(k);
            const auto& anchor = anchors.array[k];
            ExactKeys(anchor, {"column", "row", "xPx", "yPx", "radiusPx"}, apath);
            LatticeAnchor value;
            value.column = Integer(Member(anchor, "column", apath), apath + "/column", kMaximumLatticeIndex);
            value.row = Integer(Member(anchor, "row", apath), apath + "/row", kMaximumLatticeIndex);
            value.x_px = Number(Member(anchor, "xPx", apath), apath + "/xPx");
            value.y_px = Number(Member(anchor, "yPx", apath), apath + "/yPx");
            value.radius_px = Number(Member(anchor, "radiusPx", apath), apath + "/radiusPx");
            entry.anchors.push_back(value);
        }
        job.views.push_back(std::move(entry));
    }
    return job;
}

std::string Sha256Hex(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("fiducial detector: SHA-256 provider failed");
    struct AlgorithmGuard { BCRYPT_ALG_HANDLE value; ~AlgorithmGuard() { BCryptCloseAlgorithmProvider(value, 0); } } algorithm_guard{algorithm};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("fiducial detector: SHA-256 hash failed");
    struct HashGuard { BCRYPT_HASH_HANDLE value; ~HashGuard() { BCryptDestroyHash(value); } } hash_guard{hash};
    constexpr std::size_t kChunk = 1U << 24;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk) {
        const auto size = static_cast<ULONG>(std::min(kChunk, bytes.size() - offset));
        if (BCryptHashData(hash, const_cast<PUCHAR>(bytes.data() + offset), size, 0) < 0) throw std::runtime_error("fiducial detector: SHA-256 input failed");
    }
    std::array<std::uint8_t, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("fiducial detector: SHA-256 finish failed");
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : digest) { text.push_back(kHex[byte >> 4]); text.push_back(kHex[byte & 15]); }
    return text;
}

std::vector<std::uint8_t> ReadImageFile(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumImageFileBytes) throw std::runtime_error("fiducial detector: an image file is missing, empty or too large");
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("fiducial detector: an image file could not be read");
    return bytes;
}

// ---- JSON text helpers.

std::string Quote(std::string_view text) {
    std::string out = "\"";
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) { char buffer[8]; std::snprintf(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
        else out.push_back(raw);
    }
    out.push_back('"');
    return out;
}

std::string Num(double value) {
    if (!std::isfinite(value)) throw std::runtime_error("fiducial detector: a non-finite number reached the output");
    if (value == 0.0) return "0";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

const char* StateCode(NodeState state) {
    switch (state) {
    case NodeState::Detected: return "detected";
    case NodeState::Missing: return "missing";
    case NodeState::Ambiguous: return "ambiguous";
    }
    return "";
}

struct ViewOutcome {
    const ViewJob* job{};
    std::string sha256;
    std::uint32_t width{};
    std::uint32_t height{};
    ViewDetection detection;
    std::string image_name;
};

std::string BuildSession(const Job& job, const std::vector<ViewOutcome>& outcomes) {
    std::string out;
    out += "{\"schemaVersion\":" + Quote(a0::m2::calibration::kObservationSessionSchemaVersion);
    out += ",\"sessionId\":" + Quote(job.session_id) + ",\"chartId\":" + Quote(job.chart_id);
    out += ",\"provenance\":{\"measuredAt\":" + Quote(job.measured_at) + ",\"tool\":{\"toolId\":" + Quote(kDetectorToolId)
        + ",\"version\":" + Quote(kDetectorVersion) + "}}";
    out += ",\"chartPoints\":[";
    bool first = true;
    for (std::uint32_t row = 0; row <= job.chart.rows; ++row) {
        for (std::uint32_t column = 0; column <= job.chart.columns; ++column) {
            if (!first) out += ',';
            first = false;
            out += "{\"pointId\":" + Quote(PointId(column, row))
                + ",\"xMm\":" + Num(job.chart.origin_x_mm + static_cast<double>(column) * job.chart.pitch_mm)
                + ",\"yMm\":" + Num(job.chart.origin_y_mm + static_cast<double>(row) * job.chart.pitch_mm) + '}';
        }
    }
    out += "],\"cameras\":{";
    bool first_camera = true;
    for (const char* alias : {"CAM-A", "CAM-B"}) {
        const auto& camera = job.cameras.at(alias);
        if (!first_camera) out += ',';
        first_camera = false;
        out += Quote(alias) + ":{\"widthPixels\":" + std::to_string(camera.width) + ",\"heightPixels\":" + std::to_string(camera.height)
            + ",\"lensLabel\":" + Quote(camera.lens) + ",\"focalLengthLabel\":" + Quote(camera.focal) + ",\"focusLabel\":" + Quote(camera.focus) + '}';
    }
    out += "},\"views\":[";
    bool first_view = true;
    for (const auto& outcome : outcomes) {
        if (!first_view) out += ',';
        first_view = false;
        const auto& view = *outcome.job;
        out += "{\"viewId\":" + Quote(view.id) + ",\"camera\":" + Quote(view.camera) + ",\"imageSha256\":" + Quote(outcome.sha256)
            + ",\"widthPixels\":" + std::to_string(outcome.width) + ",\"heightPixels\":" + std::to_string(outcome.height)
            + ",\"role\":" + Quote(view.role);
        std::vector<std::string> used;
        std::vector<std::string> pose;
        std::vector<std::string> measurement;
        for (const auto& node : outcome.detection.nodes) {
            if (node.state != NodeState::Detected) continue;
            auto id = PointId(node.column, node.row);
            if (view.role == "validation") {
                // Checkerboard split of the detected nodes: pose from even (column + row), error measured on the rest.
                ((node.column + node.row) % 2 == 0 ? pose : measurement).push_back(std::move(id));
            } else {
                used.push_back(std::move(id));
            }
        }
        const auto list = [](const std::vector<std::string>& ids) {
            std::string text = "[";
            for (std::size_t i = 0; i < ids.size(); ++i) { if (i != 0) text += ','; text += Quote(ids[i]); }
            return text + ']';
        };
        if (view.role == "validation") out += ",\"posePointIds\":" + list(pose) + ",\"measurementPointIds\":" + list(measurement);
        else out += ",\"usedPointIds\":" + list(used);
        out += ",\"observations\":[";
        bool first_observation = true;
        for (const auto& node : outcome.detection.nodes) {
            if (!first_observation) out += ',';
            first_observation = false;
            out += "{\"pointId\":" + Quote(PointId(node.column, node.row)) + ",\"state\":" + Quote(StateCode(node.state)) + ",\"pixel\":";
            if (node.state == NodeState::Detected) out += "{\"x\":" + Num(node.x_px) + ",\"y\":" + Num(node.y_px) + '}';
            else out += "null";
            out += '}';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

std::string BuildReport(const Job& job, const std::vector<ViewOutcome>& outcomes, const DetectorParams& params, bool ok,
    const std::string& session_failure) {
    std::string out;
    out += "{\"schema\":" + Quote(kReportSchemaVersion) + ",\"tool\":{\"toolId\":" + Quote(kDetectorToolId)
        + ",\"version\":" + Quote(kDetectorVersion) + "}";
    out += ",\"result\":" + Quote(ok ? "session-written" : "no-session");
    if (!session_failure.empty()) out += ",\"sessionRejection\":" + Quote(session_failure);
    out += ",\"sessionId\":" + Quote(job.session_id) + ",\"chartId\":" + Quote(job.chart_id);
    out += ",\"chart\":{\"columns\":" + std::to_string(job.chart.columns) + ",\"rows\":" + std::to_string(job.chart.rows)
        + ",\"originXMm\":" + Num(job.chart.origin_x_mm) + ",\"originYMm\":" + Num(job.chart.origin_y_mm) + ",\"pitchMm\":" + Num(job.chart.pitch_mm) + '}';
    out += ",\"idAssignment\":\"operator anchors, then lattice growth by local affine prediction; no ground truth is read\"";
    out += ",\"identityCheck\":\"anchor triples must not be mirrored; the pattern in each cell with four detected corners (kind = (3 * column + 5 * row) mod 8, "
           "grey patch level by (column + row) mod 6) must agree with the node names; levels are ratios to the white of the same cell; "
           "the dark side of a slanted edge is compared with the median dark side of the view's expected slanted edges (at most "
        + Num(kEdgeDarkReferenceMaximum) + "); grey levels are compared through the view's piecewise linear map from expected level to "
           "the median measured level of its cells (" + std::to_string(kGrayMinimumFitLevels) + " levels at least, each step rising by at least "
        + Num(kGrayMinimumRise) + " of the design step), residuals in design units\"";
    out += ",\"validationSplit\":\"detected nodes with even (column + row) give the pose; the others are measured\"";
    out += ",\"params\":{\"darkFraction\":" + Num(params.dark_fraction) + ",\"blurSigma\":" + Num(params.blur_sigma)
        + ",\"minHoleRadiusPx\":" + Num(params.min_hole_radius_px) + ",\"maxHoleRadiusPx\":" + Num(params.max_hole_radius_px)
        + ",\"minAxisRatio\":" + Num(params.min_axis_ratio) + ",\"ellipseAreaTolerance\":" + Num(params.ellipse_area_tolerance)
        + ",\"minDotAreaRatio\":" + Num(params.min_dot_area_ratio) + ",\"maxDotAreaRatio\":" + Num(params.max_dot_area_ratio)
        + ",\"concentricTolerance\":" + Num(params.concentric_tolerance) + ",\"minRingDarkFraction\":" + Num(params.min_ring_dark_fraction)
        + ",\"refineIterations\":" + std::to_string(params.refine_iterations) + ",\"acceptFraction\":" + Num(params.accept_fraction)
        + ",\"identityLevelTolerance\":" + Num(params.identity_level_tolerance) + ",\"identityLineSpread\":" + Num(params.identity_line_spread)
        + ",\"identityLineShare\":" + Num(params.identity_line_share) + ",\"identityMinCells\":" + std::to_string(params.identity_min_cells)
        + ",\"identityMinGrayCells\":" + std::to_string(params.identity_min_gray_cells)
        + ",\"identityMaxMismatchFraction\":" + Num(params.identity_max_mismatch_fraction)
        + ",\"identityMinMismatches\":" + std::to_string(params.identity_min_mismatches) + '}';
    out += ",\"views\":[";
    bool first = true;
    for (const auto& outcome : outcomes) {
        if (!first) out += ',';
        first = false;
        const auto& d = outcome.detection;
        std::size_t detected = 0, missing = 0, ambiguous = 0;
        for (const auto& node : d.nodes) {
            if (node.state == NodeState::Detected) ++detected;
            else if (node.state == NodeState::Missing) ++missing;
            else ++ambiguous;
        }
        out += "{\"viewId\":" + Quote(outcome.job->id) + ",\"camera\":" + Quote(outcome.job->camera) + ",\"role\":" + Quote(outcome.job->role)
            + ",\"imageName\":" + Quote(outcome.image_name) + ",\"imageSha256\":" + Quote(outcome.sha256)
            + ",\"widthPixels\":" + std::to_string(outcome.width) + ",\"heightPixels\":" + std::to_string(outcome.height);
        out += ",\"status\":" + Quote(d.failure.empty() ? "ok" : d.failure);
        if (!d.failure.empty()) out += ",\"statusDetail\":" + Quote(d.failure_detail);
        out += ",\"darkThreshold\":" + Num(d.dark_threshold) + ",\"ringCandidates\":" + std::to_string(d.candidates.size())
            + ",\"detected\":" + std::to_string(detected) + ",\"missing\":" + std::to_string(missing)
            + ",\"ambiguous\":" + std::to_string(ambiguous) + ",\"unmatchedCandidates\":" + std::to_string(d.unmatched_candidates);
        out += ",\"anchors\":[";
        for (std::size_t i = 0; i < d.anchors.size(); ++i) {
            const auto& r = d.anchors[i];
            if (i != 0) out += ',';
            out += "{\"pointId\":" + Quote(PointId(r.anchor.column, r.anchor.row)) + ",\"hintXPx\":" + Num(r.anchor.x_px)
                + ",\"hintYPx\":" + Num(r.anchor.y_px) + ",\"searchRadiusPx\":" + Num(r.anchor.radius_px);
            if (r.ring) out += ",\"ringXPx\":" + Num(r.ring->x) + ",\"ringYPx\":" + Num(r.ring->y);
            out += '}';
        }
        out += ']';
        const auto& identity = d.identity;
        out += ",\"identity\":{\"result\":" + Quote(identity.result) + ",\"anchorTriples\":" + std::to_string(identity.anchor_triples)
            + ",\"mirroredTriples\":" + std::to_string(identity.mirrored_triples) + ",\"cellsExamined\":" + std::to_string(identity.cells_examined)
            + ",\"cellsSkipped\":" + std::to_string(identity.cells_skipped) + ",\"cellsMismatched\":" + std::to_string(identity.cells_mismatched)
            + ",\"grayCellsExamined\":" + std::to_string(identity.gray_cells_examined)
            + ",\"grayCellsMismatched\":" + std::to_string(identity.gray_cells_mismatched)
            + ",\"edgeCellsExamined\":" + std::to_string(identity.edge_cells_examined)
            + ",\"edgeDarkReference\":" + (identity.edge_dark_reference ? Num(*identity.edge_dark_reference) : std::string("null"));
        const auto& fit = identity.gray_fit;
        out += ",\"grayFit\":{\"status\":" + Quote(fit.status) + ",\"cells\":" + std::to_string(fit.cells.size()) + ",\"knots\":[";
        for (std::size_t i = 0; i < fit.knots.size(); ++i) {
            const auto& k = fit.knots[i];
            if (i != 0) out += ',';
            out += "{\"expected\":" + Num(k.expected) + ",\"measured\":" + Num(k.measured) + ",\"cells\":" + std::to_string(k.cells) + '}';
        }
        out += ']';
        if (fit.status == "fitted" || fit.status == "not-rising") out += ",\"minRiseRatio\":" + Num(fit.min_rise_ratio);
        if (fit.status == "fitted") out += ",\"residualRms\":" + Num(fit.residual_rms) + ",\"residualMaxAbs\":" + Num(fit.residual_max_abs);
        out += ",\"levelsByCell\":[";
        for (std::size_t i = 0; i < fit.cells.size(); ++i) {
            const auto& c = fit.cells[i];
            if (i != 0) out += ',';
            out += "{\"cellColumn\":" + std::to_string(c.column) + ",\"cellRow\":" + std::to_string(c.row)
                + ",\"expected\":" + Num(c.expected) + ",\"measured\":" + Num(c.measured);
            if (fit.status == "fitted") out += ",\"residual\":" + Num(c.residual);
            out += '}';
        }
        out += "]},\"mismatches\":[";
        for (std::size_t i = 0; i < identity.mismatches.size(); ++i) {
            const auto& m = identity.mismatches[i];
            if (i != 0) out += ',';
            out += "{\"cellColumn\":" + std::to_string(m.column) + ",\"cellRow\":" + std::to_string(m.row)
                + ",\"expected\":" + Quote(CellPatternCode(m.expected)) + ",\"check\":" + Quote(m.check)
                + ",\"centerRatio\":" + Num(m.center_ratio) + ",\"ringRatio\":" + Num(m.ring_ratio)
                + ",\"spread\":" + Num(m.spread) + ",\"axisShare\":" + Num(m.axis_share) + '}';
        }
        out += "]}}";
    }
    out += "]}";
    return out;
}

} // namespace

JobResult RunJob(std::string_view job_json, const std::filesystem::path& image_base_directory, const DetectorParams& params) {
    const Job job = ParseJob(job_json);
    std::vector<ViewOutcome> outcomes;
    outcomes.reserve(job.views.size());
    bool all_ok = true;
    for (const auto& view : job.views) {
        const std::u8string image_name(view.image.begin(), view.image.end());
        const std::filesystem::path path = image_base_directory / std::filesystem::path(image_name);
        const auto bytes = ReadImageFile(path);
        ViewOutcome outcome;
        outcome.job = &view;
        outcome.sha256 = Sha256Hex(bytes);
        const auto file_name = path.filename().u8string();
        outcome.image_name.assign(file_name.begin(), file_name.end());
        const cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1, const_cast<std::uint8_t*>(bytes.data()));
        const cv::Mat gray = cv::imdecode(encoded, cv::IMREAD_GRAYSCALE | cv::IMREAD_IGNORE_ORIENTATION);
        if (gray.empty()) throw std::runtime_error("fiducial detector: an image could not be decoded");
        outcome.width = static_cast<std::uint32_t>(gray.cols);
        outcome.height = static_cast<std::uint32_t>(gray.rows);
        const auto& camera = job.cameras.at(view.camera);
        if (outcome.width != camera.width || outcome.height != camera.height) {
            ViewDetection mismatch;
            mismatch.failure = "dimension-mismatch";
            mismatch.failure_detail = "the image size differs from the camera size in the job";
            outcome.detection = std::move(mismatch);
        } else {
            outcome.detection = DetectView(gray, job.chart, view.anchors, params);
        }
        if (!outcome.detection.failure.empty()) all_ok = false;
        outcomes.push_back(std::move(outcome));
    }

    JobResult result;
    std::string rejection;
    if (all_ok) {
        auto session = BuildSession(job, outcomes);
        try {
            (void)a0::m2::calibration::CalibrationObservationSession::Parse(session);
            result.session_json = std::move(session);
            result.ok = true;
        } catch (const std::invalid_argument& error) {
            rejection = error.what();
            result.ok = false;
        }
    }
    result.report_json = BuildReport(job, outcomes, params, result.ok, rejection);
    return result;
}

} // namespace a0::m2::fiducial
