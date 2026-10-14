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
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
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

    void SetOrientation(double sign) { orientation_ = sign; }

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
        if (!(determinant * orientation_ > 0.0)) return std::nullopt;
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
    double orientation_{1.0};
};

ViewDetection Fail(ViewDetection result, std::string code, std::string detail) {
    result.failure = std::move(code);
    result.failure_detail = std::move(detail);
    result.nodes.clear();
    return result;
}

} // namespace

ViewDetection DetectView(const cv::Mat& gray, const ChartDefinition& chart,
    std::span<const LatticeAnchor> anchors, const DetectorParams& params) {
    ValidateChart(chart);
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

    // The anchors must span the lattice in two directions, and they fix the
    // orientation (a mirrored lattice is rejected from here on).
    {
        double sign = 0.0;
        bool spans = false;
        for (std::size_t i = 0; i + 2 < anchors.size() && !spans; ++i) {
            for (std::size_t j = i + 1; j < anchors.size() && !spans; ++j) {
                for (std::size_t k = j + 1; k < anchors.size() && !spans; ++k) {
                    const double lattice_area =
                        (static_cast<double>(anchors[j].column) - anchors[i].column) * (static_cast<double>(anchors[k].row) - anchors[i].row)
                        - (static_cast<double>(anchors[k].column) - anchors[i].column) * (static_cast<double>(anchors[j].row) - anchors[i].row);
                    if (lattice_area == 0.0) continue;
                    const auto& a = *result.anchors[i].ring;
                    const auto& b = *result.anchors[j].ring;
                    const auto& c = *result.anchors[k].ring;
                    const double pixel_area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
                    if (pixel_area == 0.0) continue;
                    sign = (lattice_area > 0.0) == (pixel_area > 0.0) ? 1.0 : -1.0;
                    spans = true;
                }
            }
        }
        if (!spans) return Fail(std::move(result), "anchor-collinear", "the anchors do not span two lattice directions");
        lattice.SetOrientation(sign);
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
    out += ",\"validationSplit\":\"detected nodes with even (column + row) give the pose; the others are measured\"";
    out += ",\"params\":{\"darkFraction\":" + Num(params.dark_fraction) + ",\"blurSigma\":" + Num(params.blur_sigma)
        + ",\"minHoleRadiusPx\":" + Num(params.min_hole_radius_px) + ",\"maxHoleRadiusPx\":" + Num(params.max_hole_radius_px)
        + ",\"minAxisRatio\":" + Num(params.min_axis_ratio) + ",\"ellipseAreaTolerance\":" + Num(params.ellipse_area_tolerance)
        + ",\"minDotAreaRatio\":" + Num(params.min_dot_area_ratio) + ",\"maxDotAreaRatio\":" + Num(params.max_dot_area_ratio)
        + ",\"concentricTolerance\":" + Num(params.concentric_tolerance) + ",\"minRingDarkFraction\":" + Num(params.min_ring_dark_fraction)
        + ",\"refineIterations\":" + std::to_string(params.refine_iterations) + ",\"acceptFraction\":" + Num(params.accept_fraction) + '}';
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
        out += "]}";
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
