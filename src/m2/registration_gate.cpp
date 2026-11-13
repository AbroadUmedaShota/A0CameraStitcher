#include "a0/m2/experimental/registration_gate.hpp"
#include "overlap_estimation_detail.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace a0::m2::experimental::registration_gate {
namespace gate = seam_gate;
using render::Point;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
// Calls body(row) for every row below `rows` on up to `threads` threads; the threads take the rows in increasing order. The rows
// must not depend on one another. A row that throws stops the rows after it; the exception of the lowest row that threw is
// rethrown, as if the rows had run in order on one thread. A thread that cannot start leaves its rows to the others and the
// calling thread.
template <class F> void ForEachRow(std::uint32_t rows, unsigned threads, F&& body) {
    if (threads <= 1 || rows <= 1) { for (std::uint32_t y = 0; y < rows; ++y) body(y); return; }
    std::atomic<std::uint32_t> next{0}, failed_row{rows};
    std::mutex lock; std::exception_ptr failure;
    const auto work = [&] {
        for (;;) {
            const std::uint32_t y = next.fetch_add(1);
            if (y >= rows || y >= failed_row.load()) return;
            try { body(y); }
            catch (...) { const std::lock_guard guard(lock); if (y < failed_row.load()) { failed_row = y; failure = std::current_exception(); } }
        }
    };
    const unsigned workers = std::min<unsigned>(threads, rows);
    std::vector<std::thread> pool;
    pool.reserve(workers-1);
    try { for (unsigned t = 1; t < workers; ++t) pool.emplace_back(work); }
    catch (const std::system_error&) {} // fewer workers; the calling thread takes the rest
    catch (const std::bad_alloc&) {}
    work();
    for (auto& t : pool) t.join();
    if (failure) std::rethrow_exception(failure);
}
bool FinitePositive(double v) { return std::isfinite(v) && v > 0; }
bool Share(double v) { return std::isfinite(v) && v > 0 && v <= 1; }
double Dot(Point a, Point b) { return a.x*b.x+a.y*b.y; }

// The smoothing of the feature before matching (seam_gate.cpp kSmooth): the measurement of a point reads this many output
// pixels around its patch.
constexpr std::int64_t kFeatureSmoothingPx = 2;

bool HasReason(const gate::SeamGateResult& r, std::string_view reason) {
    return std::any_of(r.findings.begin(), r.findings.end(), [&](const gate::SeamGateFinding& f) { return f.reason == reason; });
}

// Rows [first, last] appended, merged with the previous range when they touch.
void AppendRows(std::vector<RowRange>& out, std::uint32_t first, std::uint32_t last) {
    if (!out.empty() && first <= out.back().last+1) { out.back().last = std::max(out.back().last, last); return; }
    out.push_back({first, last});
}

bool MeasuresBaseline(const gate::SeamPoint& p, Point baseline, double c_min) {
    return p.kind == gate::PointKind::two_directional || std::abs(Dot(p.normal, baseline)) >= c_min;
}

// ---- Content in each camera at a point (11.30: one-sided content) ----------------------------------------------------------
// The seam gate's content test (seam_gate.cpp MeasureAt) on one camera alone: the luminance (B + G + R) / 3 of the raw image,
// sampled with the Catmull-Rom kernel of render::SampleBicubic at the output pixels of the patch [x - h, x + h] x [y - h, y + h]
// (h = patch / 2), the forward-difference gradient magnitude on the 2h x 2h samples, and the number of them that reach
// content_gradient_dn (content when it is at least content_minimum_samples). The seam gate keeps only "A or B"; this tells the
// two apart. Empty when a sample is not covered (the camera's side is then not known).
struct ContentSamples { std::size_t at_level{}, at_half_level{}; };
std::optional<ContentSamples> CameraContentSamples(const render::BgrImage& image, const render::OutputToCamera& map, const gate::SeamPoint& p,
    const gate::SeamGateDesign& d) {
    const int h = static_cast<int>(d.patch_px/2), n = 2*h;
    overlap_detail::Plane l{n+1, n+1, std::vector<double>(static_cast<std::size_t>(n+1)*(n+1))};
    std::array<double, 3> px{};
    for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) {
        Point raw{};
        const Point at{static_cast<double>(static_cast<std::int64_t>(p.x)-h+x), static_cast<double>(static_cast<std::int64_t>(p.y)-h+y)};
        if (!map(at, raw)) return std::nullopt;
        Require(std::isfinite(raw.x) && std::isfinite(raw.y), "registration gate: a mapping returned a position that is not finite");
        if (!render::SampleBicubic(image, raw.x, raw.y, px)) return std::nullopt;
        l.v[static_cast<std::size_t>(y)*(n+1)+x] = (px[0]+px[1]+px[2])/3.0;
    }
    const auto g = overlap_detail::Gradient(l);
    ContentSamples count;
    for (const double v : g.v) { count.at_level += v >= d.content_gradient_dn; count.at_half_level += v >= d.content_gradient_dn/2; }
    return count;
}
// Content in one camera only at the point (both sides known): the measurer found no peak for it (a point it found one for,
// uncertain or not consistent both ways, shows its content in both cameras), one camera has at least twice the seam gate's
// content count, and the other has fewer than a quarter of it even at half the gradient level. The margins keep faint content
// (a dark area, the exposure of one camera lower than the other's) from being taken for a mark that one camera shows and the
// other does not: a mark on paper leaves the other camera's patch with the noise of blank paper only.
bool OneSided(const RegistrationGateRequest& r, const gate::SeamPoint& p, const gate::SeamGateDesign& d) {
    if (p.status == gate::PointStatus::uncertain || p.status == gate::PointStatus::round_trip_inconsistent) return false;
    const auto a = CameraContentSamples(*r.camera_a, r.mapped.camera_a, p, d), b = CameraContentSamples(*r.camera_b, r.mapped.camera_b, p, d);
    if (!a || !b) return false;
    const std::size_t clear = 2*static_cast<std::size_t>(d.content_minimum_samples), blank = d.content_minimum_samples/4;
    return (a->at_level >= clear && b->at_half_level < blank) || (b->at_level >= clear && a->at_half_level < blank);
}

// Blocks whose counted points the rows do not take (see GateRows).
std::vector<char> RefusedBlocks(const gate::SeamMeasurement& m) {
    std::vector<char> refused(m.blocks.size());
    for (std::size_t i = 0; i < m.blocks.size(); ++i) refused[i] = m.blocks[i].neighbor_inconsistent || m.blocks[i].unconfirmed_points > 0;
    return refused;
}

// The rows of a measurement (GateRows). `sides`: the request whose images tell one-sided content apart (Omega), or null.
GateRows RowsOf(const gate::SeamMeasurement& m, Point baseline, double c_min, const RegistrationGateRequest* sides = nullptr) {
    GateRows rows;
    const auto refused = RefusedBlocks(m);
    const auto accepted = [&](const gate::SeamPoint& p) {
        if (!p.Counted()) return false;
        const std::size_t b = static_cast<std::size_t>(p.block_row)*m.block_columns+p.block_column;
        return b >= refused.size() || !refused[b];
    };
    const std::uint32_t half = m.design.point_spacing_px/2;
    std::size_t i = 0;
    while (i < m.points.size()) {
        const std::uint32_t y = m.points[i].y;
        const std::size_t begin = i;
        bool any = false, content = false, unconfirmed = false, measured = false, dx = false;
        for (; i < m.points.size() && m.points[i].y == y; ++i) {
            const auto& p = m.points[i];
            if (p.status == gate::PointStatus::outside_coverage) continue;
            any = true;
            content = content || p.content;
            unconfirmed = unconfirmed || (p.status == gate::PointStatus::valid && p.confirmation == gate::Confirmation::unconfirmed);
            if (!accepted(p)) continue;
            measured = true;
            dx = dx || MeasuresBaseline(p, baseline, c_min);
        }
        if (!any) continue;
        const std::uint32_t first = y >= half ? y-half : 0;
        const std::uint32_t last = std::min<std::uint32_t>(m.height ? m.height-1 : 0, y+(half ? half-1 : 0));
        if (!content) { ++rows.rows_blank; AppendRows(rows.blank, first, last); continue; }
        ++rows.rows_with_content;
        if (unconfirmed) { ++rows.rows_unconfirmed; AppendRows(rows.content_unmeasured, first, last); continue; }
        if (measured) {
            ++rows.rows_measured;
            if (dx) ++rows.rows_dx_measured; else AppendRows(rows.unmeasured, first, last);
            continue;
        }
        ++rows.rows_content_unmeasured;
        AppendRows(rows.content_unmeasured, first, last);
        if (sides) {
            bool one = false;
            for (std::size_t k = begin; k < i && !one; ++k) {
                const auto& p = m.points[k];
                if (p.content && p.status != gate::PointStatus::outside_coverage) one = OneSided(*sides, p, m.design);
            }
            rows.rows_one_sided += one;
        }
    }
    if (rows.rows_with_content) {
        rows.dx_measured_share = static_cast<double>(rows.rows_dx_measured)/rows.rows_with_content;
        rows.content_unmeasured_share = static_cast<double>(rows.rows_content_unmeasured)/rows.rows_with_content;
    }
    return rows;
}

// The visible value of a point for a displacement d: |d| for two directions, |n . d| for lines in one direction (the seam
// gate's visible_px when d is the measured displacement).
double Visible(const gate::SeamPoint& p, Point d) {
    return p.kind == gate::PointKind::one_directional ? std::abs(Dot(p.normal, d)) : std::hypot(d.x, d.y);
}

// Maxima over the counted points, each with the displacement `shifted(p)`.
template <class Shift>
GateMaxima MaximaOf(const gate::SeamMeasurement& m, Point baseline, double c_min, Shift shifted) {
    GateMaxima out;
    for (const auto& p : m.points) {
        if (!p.Counted()) continue;
        const double v = Visible(p, shifted(p));
        if (p.kind == gate::PointKind::two_directional) {
            if (!out.two_directional_px || v > *out.two_directional_px) out.two_directional_px = v;
        } else if (!out.one_directional_px || v > *out.one_directional_px) {
            out.one_directional_px = v;
            out.one_directional_normal = p.normal;
            out.one_directional_component = std::abs(Dot(p.normal, baseline)) >= c_min ?
                OneDirectionalComponent::with_baseline : OneDirectionalComponent::across_baseline;
        }
    }
    return out;
}

GateMaxima MaximaOf(const gate::SeamMeasurement& m, Point baseline, double c_min) {
    return MaximaOf(m, baseline, c_min, [](const gate::SeamPoint& p) { return p.displacement; });
}

// The correction of layer 2 at a point (0 where the caller's function has none).
Point CorrectionAt(const RegistrationGateRequest& r, const gate::SeamPoint& p) {
    Point s{};
    if (!r.correction) return s;
    if (!r.correction({static_cast<double>(p.x), static_cast<double>(p.y)}, s)) return {};
    Require(std::isfinite(s.x) && std::isfinite(s.y), "registration gate: the correction of layer 2 is not finite");
    return s;
}

bool Exceeds(const gate::SeamPoint& p, const gate::SeamGateLimits& l) {
    return p.Counted() && p.visible_px+l.guard_band_px > l.limit_px;
}

// 11.22.3 lift: of the exceeding points, the share whose baseline component (before layer 2) dominates, and of those the share
// with the common sign. A one-directional point carries only n . d: it counts as dominated when its normal has a baseline
// component of at least c_min (its perpendicular part is not known), and never otherwise.
std::optional<GateLift> LiftOf(const RegistrationGateRequest& r, const gate::SeamMeasurement& m) {
    const Point e = r.baseline, perp{-r.baseline.y, r.baseline.x};
    std::uint32_t exceeding = 0, dominated = 0, positive = 0;
    std::vector<double> components;
    for (const auto& p : m.points) {
        if (!Exceeds(p, r.gate_limits)) continue;
        ++exceeding;
        const Point s = CorrectionAt(r, p);
        const Point d{p.displacement.x+s.x, p.displacement.y+s.y};
        double along = 0;
        bool is_dominated = false;
        if (p.kind == gate::PointKind::two_directional) {
            along = Dot(d, e);
            is_dominated = std::abs(along) >= r.design.lift_ratio*std::abs(Dot(d, perp)) && along != 0;
        } else {
            const double ne = Dot(p.normal, e);
            if (std::abs(ne) >= r.design.minimum_baseline_component) { along = Dot(p.normal, d)/ne; is_dominated = along != 0; }
        }
        if (!is_dominated) continue;
        ++dominated;
        if (along > 0) ++positive;
        components.push_back(along);
    }
    if (!exceeding) return std::nullopt;
    GateLift lift;
    lift.points_exceeding = exceeding;
    lift.baseline_share = static_cast<double>(dominated)/exceeding;
    lift.sign_agreement_share = dominated ? static_cast<double>(std::max(positive, dominated-positive))/dominated : 0.0;
    if (dominated && r.baseline_px_per_mm) {
        std::sort(components.begin(), components.end());
        const std::size_t n = components.size();
        const double median = n%2 ? components[n/2] : (components[n/2-1]+components[n/2])/2;
        lift.height_estimate_mm = median / *r.baseline_px_per_mm;
    }
    return lift;
}

// 11.22.3 pose: a similarity of the output plane, d(x, y) = (t_x + a x' - b y', t_y + b x' + a y') about the seam column and
// the middle row, fitted by least squares to the counted two-directional points of Omega. Needs three points on two rows at
// least.
bool ExplainedBySimilarity(const RegistrationGateRequest& r, const gate::SeamMeasurement& m) {
    const double cx = r.layout.seam_column, cy = m.height/2.0;
    double ata[4][4]{}, atb[4]{};
    std::size_t n = 0;
    std::uint32_t y_low = ~0u, y_high = 0;
    for (const auto& p : m.points) {
        if (!p.Counted() || p.kind != gate::PointKind::two_directional) continue;
        const double x = p.x-cx, y = p.y-cy;
        const double rows[2][4]{{1, 0, x, -y}, {0, 1, y, x}};
        const double values[2]{p.displacement.x, p.displacement.y};
        for (int k = 0; k < 2; ++k) {
            for (int i = 0; i < 4; ++i) { atb[i] += rows[k][i]*values[k]; for (int j = 0; j < 4; ++j) ata[i][j] += rows[k][i]*rows[k][j]; }
        }
        ++n; y_low = std::min(y_low, p.y); y_high = std::max(y_high, p.y);
    }
    if (n < 3 || y_low == y_high) return false;
    // Gaussian elimination with partial pivoting on the 4x4 normal equations.
    double a[4][5];
    for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) a[i][j] = ata[i][j]; a[i][4] = atb[i]; }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int i = c+1; i < 4; ++i) if (std::abs(a[i][c]) > std::abs(a[pivot][c])) pivot = i;
        if (!(std::abs(a[pivot][c]) > 1e-12)) return false;
        if (pivot != c) for (int j = 0; j < 5; ++j) std::swap(a[c][j], a[pivot][j]);
        for (int i = 0; i < 4; ++i) {
            if (i == c) continue;
            const double f = a[i][c]/a[c][c];
            for (int j = c; j < 5; ++j) a[i][j] -= f*a[c][j];
        }
    }
    const double tx = a[0][4]/a[0][0], ty = a[1][4]/a[1][1], s = a[2][4]/a[2][2], rot = a[3][4]/a[3][3];
    // Explained: the fit leaves every counted point within the line, and at every exceeding point the fit itself exceeds it
    // (a fit that only pulls a point just over the line under it does not explain the excess).
    const auto over = [&](double v) { return v+r.gate_limits.guard_band_px > r.gate_limits.limit_px; };
    for (const auto& p : m.points) {
        if (!p.Counted()) continue;
        const double x = p.x-cx, y = p.y-cy;
        const Point fit{tx+s*x-rot*y, ty+rot*x+s*y};
        if (over(Visible(p, {p.displacement.x-fit.x, p.displacement.y-fit.y}))) return false;
        if (Exceeds(p, r.gate_limits) && !over(Visible(p, fit))) return false;
    }
    return true;
}

// ---- The wide search (11.6 item 4) -----------------------------------------------------------------------------------
// Blocks of rows along the seam; per block, A's edge energy over the seam column +- the half width is compared with B's at
// offsets s e_b, |s| <= R, by zero-mean normalized correlation. A block counts when its best peak is above the floor, not on
// the edge of the window, and no other local peak reaches the secondary ratio of it. The offsets of the counting blocks vote:
// the largest group within the consistency gives the offset (its median). Integer offsets refined by a parabola. A diagnostic
// only: it never enters the maximum.

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Output-grid samples of a camera's luminance, [x0, x0 + w) x [y0, y0 + h); NaN where the camera has no sample.
struct Grid {
    std::int64_t x0{}, y0{};
    int w{}, h{};
    std::vector<double> v;
    double At(int x, int y) const { return v[static_cast<std::size_t>(y)*w+x]; }
};
Grid SampleLuma(const render::BgrImage& image, const render::OutputToCamera& map, std::int64_t x0, std::int64_t y0, int w, int h) {
    Grid g{x0, y0, w, h, std::vector<double>(static_cast<std::size_t>(w)*h, kNaN)};
    std::array<double, 3> px{};
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        Point raw{};
        if (!map({static_cast<double>(x0+x), static_cast<double>(y0+y)}, raw)) continue;
        Require(std::isfinite(raw.x) && std::isfinite(raw.y), "registration gate: a mapping returned a position that is not finite");
        if (render::SampleBilinear(image, raw.x, raw.y, px)) g.v[static_cast<std::size_t>(y)*w+x] = 0.114*px[0]+0.587*px[1]+0.299*px[2];
    }
    return g;
}
// Edge energy |L(x+1,y) - L(x,y)| + |L(x,y+1) - L(x,y)|, one sample smaller per axis.
Grid EdgeEnergy(const Grid& l) {
    Grid g{l.x0, l.y0, l.w-1, l.h-1, std::vector<double>(static_cast<std::size_t>(std::max(0, l.w-1))*std::max(0, l.h-1), kNaN)};
    for (int y = 0; y < g.h; ++y) for (int x = 0; x < g.w; ++x)
        g.v[static_cast<std::size_t>(y)*g.w+x] = std::abs(l.At(x+1, y)-l.At(x, y))+std::abs(l.At(x, y+1)-l.At(x, y));
    return g;
}
// Bilinear in grid coordinates; a neighbour with a zero weight is not read (an exact position gives the sample itself).
double Bilinear(const Grid& g, double x, double y) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const double tx = x-fx, ty = y-fy;
    if (ix < 0 || iy < 0 || ix >= g.w || iy >= g.h || (tx > 0 && ix+1 >= g.w) || (ty > 0 && iy+1 >= g.h)) return kNaN;
    double v = (1-tx)*(1-ty)*g.At(ix, iy);
    if (tx > 0) v += tx*(1-ty)*g.At(ix+1, iy);
    if (ty > 0) v += (1-tx)*ty*g.At(ix, iy+1);
    if (tx > 0 && ty > 0) v += tx*ty*g.At(ix+1, iy+1);
    return v;
}

struct WideBlock { bool content{}, unique{}; double offset{}; std::uint32_t first{}, last{}; };

WideBlock SearchBlock(const RegistrationGateRequest& r, std::uint32_t first, std::uint32_t last) {
    const auto& d = r.design;
    const Point e = r.baseline;
    const std::int64_t xs = r.layout.seam_column, hw = d.wide_search_half_width_px, radius = d.wide_search_radius_px;
    const int rows = static_cast<int>(last-first+1), cols = static_cast<int>(2*hw+1);
    WideBlock out{false, false, 0, first, last};
    const Grid a = EdgeEnergy(SampleLuma(*r.camera_a, r.mapped.camera_a, xs-hw, first, cols+1, rows+1));
    std::uint32_t ink = 0;
    for (const double v : a.v) if (!std::isnan(v) && v >= r.gate_design.content_gradient_dn) ++ink;
    out.content = ink >= r.gate_design.content_minimum_samples;
    if (!out.content) return out;
    const std::int64_t mx = static_cast<std::int64_t>(std::ceil(radius*std::abs(e.x)))+2, my = static_cast<std::int64_t>(std::ceil(radius*std::abs(e.y)))+2;
    const Grid b = EdgeEnergy(SampleLuma(*r.camera_b, r.mapped.camera_b, xs-hw-mx, first-my, static_cast<int>(cols+2*mx+1), static_cast<int>(rows+2*my+1)));
    std::vector<double> score(static_cast<std::size_t>(2*radius+1), kNaN);
    for (std::int64_t s = -radius; s <= radius; ++s) {
        double n = 0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) {
            const double va = a.At(x, y);
            if (std::isnan(va)) continue;
            const double vb = Bilinear(b, static_cast<double>(x+mx)+s*e.x, static_cast<double>(y+my)+s*e.y);
            if (std::isnan(vb)) continue;
            n += 1; sa += va; sb += vb; saa += va*va; sbb += vb*vb; sab += va*vb;
        }
        if (n < 0.5*cols*rows) continue;
        const double va = saa-sa*sa/n, vb = sbb-sb*sb/n;
        if (va > 0 && vb > 0) score[static_cast<std::size_t>(s+radius)] = (sab-sa*sb/n)/std::sqrt(va*vb);
    }
    std::size_t best = score.size();
    for (std::size_t k = 0; k < score.size(); ++k) if (!std::isnan(score[k]) && (best == score.size() || score[k] > score[best])) best = k;
    if (best == score.size() || best == 0 || best+1 == score.size() || !(score[best] >= d.wide_search_minimum_correlation)) return out;
    for (std::size_t k = 0; k < score.size(); ++k) {
        if (k+2 >= best && k <= best+2) continue;
        if (std::isnan(score[k])) continue;
        const bool left = k == 0 || std::isnan(score[k-1]) || score[k] >= score[k-1];
        const bool right = k+1 == score.size() || std::isnan(score[k+1]) || score[k] >= score[k+1];
        if (left && right && score[k] >= d.wide_search_secondary_ratio*score[best]) return out;
    }
    double offset = static_cast<double>(best)-static_cast<double>(radius);
    const double l = score[best-1], c = score[best], rr = score[best+1];
    if (!std::isnan(l) && !std::isnan(rr) && l-2*c+rr < 0) offset += 0.5*(l-rr)/(l-2*c+rr);
    out.unique = true; out.offset = offset;
    return out;
}

GateWideSearch WideSearch(const RegistrationGateRequest& r, const RegistrationGateLayout& layout) {
    const auto& d = r.design;
    GateWideSearch out;
    out.radius_px = d.wide_search_radius_px;
    // Blocks over the rows where Omega is measurable, from the top; a last block shorter than half the block height is left out.
    std::uint32_t top = layout.omega.height, bottom = 0;
    for (std::uint32_t y = 0; y < layout.omega.height; ++y) if (layout.omega.band[y].begin < layout.omega.band[y].end) { top = std::min(top, y); bottom = y; }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
    for (std::uint32_t y = top; top <= bottom && y <= bottom; y += d.wide_search_block_rows) {
        const std::uint32_t last = std::min(bottom, y+d.wide_search_block_rows-1);
        if (last-y+1 >= d.wide_search_block_rows/2) spans.push_back({y, last});
    }
    std::vector<WideBlock> blocks(spans.size());
    const unsigned workers = std::max(1u, std::min<unsigned>(r.thread_count, static_cast<unsigned>(spans.size())));
    std::vector<std::exception_ptr> errors(workers);
    const auto run = [&](unsigned t) {
        try { for (std::size_t i = t; i < spans.size(); i += workers) blocks[i] = SearchBlock(r, spans[i].first, spans[i].second); }
        catch (...) { errors[t] = std::current_exception(); }
    };
    // Share t on thread t; a share whose thread cannot start runs on the calling thread, so the blocks and the order in which the
    // errors are rethrown stay the same.
    std::vector<std::thread> pool;
    pool.reserve(workers);
    try { for (unsigned t = 1; t < workers; ++t) pool.emplace_back(run, t); }
    catch (const std::system_error&) {}
    catch (const std::bad_alloc&) {}
    for (unsigned t = static_cast<unsigned>(pool.size())+1; t < workers; ++t) run(t);
    run(0);
    for (auto& th : pool) th.join();
    for (const auto& e : errors) if (e) std::rethrow_exception(e);
    std::vector<std::size_t> unique;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        out.blocks += blocks[i].content;
        if (blocks[i].unique) unique.push_back(i);
    }
    out.unique_blocks = static_cast<std::uint32_t>(unique.size());
    std::vector<std::size_t> group;
    for (const auto i : unique) {
        std::vector<std::size_t> members;
        for (const auto j : unique) if (std::abs(blocks[j].offset-blocks[i].offset) <= d.wide_search_consistency_px) members.push_back(j);
        if (members.size() > group.size()) group = std::move(members);
    }
    out.agreeing_blocks = static_cast<std::uint32_t>(group.size());
    if (group.empty() || group.size() < d.wide_search_minimum_blocks) return out;
    std::vector<double> offsets;
    for (const auto j : group) offsets.push_back(blocks[j].offset);
    std::sort(offsets.begin(), offsets.end());
    const std::size_t n = offsets.size();
    out.offset_px = n%2 ? offsets[n/2] : (offsets[n/2-1]+offsets[n/2])/2;
    if (r.output_pixel_mm) out.offset_mm = *out.offset_px * *r.output_pixel_mm;
    out.rows = RowRange{blocks[group.front()].first, blocks[group.back()].last};
    return out;
}

void ValidateMeasurement(const gate::SeamMeasurement& m, const gate::SeamGateGeometry& g, const char* message) {
    Require(m.width == g.width && m.height == g.height && m.containment.supplied, message);
    for (const auto& p : m.points) {
        Require(p.y < g.height && p.x >= g.band[p.y].begin && p.x < g.band[p.y].end, message);
    }
}
} // namespace

void ValidateRegistrationGateRequest(const RegistrationGateRequest& r) {
    Require(r.camera_a && r.camera_b, "registration gate: both images are required");
    for (const auto* image : {r.camera_a, r.camera_b})
        Require(image->bgr.size() == render::PixelCount(image->width, image->height)*3, "registration gate: BGR buffer differs from dimensions");
    Require(static_cast<bool>(r.mapped.camera_a) && static_cast<bool>(r.mapped.camera_b), "registration gate: both mappings are required");
    (void)render::PixelCount(r.mapped.width, r.mapped.height);
    const auto& g = r.layout;
    Require(g.seam_column < r.mapped.width, "registration gate: the seam column is outside the output");
    Require(g.mixing_width_px >= 1 && g.band_half_width_px >= g.omega_half_width_px(),
        "registration gate: the mixing width is zero or the band does not reach past Omega");
    Require(r.band_measure_half_width_px == 0 || r.band_measure_half_width_px >= g.omega_half_width_px(),
        "registration gate: the measured band does not reach past Omega");
    ValidateSeamBlendSettings(r.blend);
    gate::ValidateSeamGateDesign(r.gate_design);
    Require(FinitePositive(r.gate_limits.limit_px) && std::isfinite(r.gate_limits.guard_band_px) && r.gate_limits.guard_band_px >= 0 &&
        r.gate_limits.guard_band_px < r.gate_limits.limit_px, "registration gate: the limits are not set");
    const auto& d = r.design;
    Require(Share(d.minimum_baseline_component) && FinitePositive(d.lift_ratio) && Share(d.lift_share) &&
        std::isfinite(d.maximum_unmeasured_content_share) && d.maximum_unmeasured_content_share >= 0 && d.maximum_unmeasured_content_share < 1,
        "registration gate: the design constants of the rows and the cause are not set");
    Require(d.wide_search_radius_px >= 1 && d.wide_search_radius_px <= 4096 && d.wide_search_half_width_px >= 1 &&
        d.wide_search_half_width_px <= 4096 && d.wide_search_block_rows >= 8 && d.wide_search_block_rows <= 4096 &&
        std::isfinite(d.wide_search_minimum_correlation) && d.wide_search_minimum_correlation > 0 && d.wide_search_minimum_correlation < 1 &&
        Share(d.wide_search_secondary_ratio) && FinitePositive(d.wide_search_consistency_px) && d.wide_search_minimum_blocks >= 1 &&
        Share(d.wide_search_trigger_share), "registration gate: the design constants of the wide search are not set");
    Require(std::isfinite(r.baseline.x) && std::isfinite(r.baseline.y) && std::abs(std::hypot(r.baseline.x, r.baseline.y)-1) <= 1e-9,
        "registration gate: the baseline is not a unit vector");
    Require(r.registered.parallax.has_value() == static_cast<bool>(r.correction),
        "registration gate: the correction of layer 2 is required exactly when the mapping holds a parallax profile");
    Require(!r.baseline_px_per_mm || FinitePositive(*r.baseline_px_per_mm), "registration gate: k must be finite and positive");
    Require(!r.output_pixel_mm || FinitePositive(*r.output_pixel_mm), "registration gate: the output pixel size must be finite and positive");
    Require(r.thread_count >= 1 && r.thread_count <= 256, "registration gate: thread count must be 1..256");
}

std::vector<gate::BandRow> OverlapBandOnThreads(const gate::SeamGateGeometry& g, std::uint32_t aw, std::uint32_t ah, std::uint32_t bw,
    std::uint32_t bh, std::uint32_t inset, unsigned thread_count) {
    Require(thread_count >= 1 && thread_count <= 256, "registration gate: thread count must be 1..256");
    // seam_gate::OverlapBand's checks, arithmetic and messages (seam_gate.cpp), with the walk of each row on the threads.
    Require(g.camera_a && g.camera_b, "seam gate: both output-to-camera mappings are required");
    (void)render::PixelCount(g.width, g.height);
    const auto inside = [](const gate::OutputToCamera& m, double x, double y, std::uint32_t w, std::uint32_t h) {
        Point raw{};
        if (!m({x, y}, raw)) return false;
        Require(std::isfinite(raw.x) && std::isfinite(raw.y), "seam gate: mapping returned nonfinite camera coordinates");
        return raw.x >= 0 && raw.y >= 0 && raw.x < w && raw.y < h;
    };
    const auto both = [&](std::uint32_t x, std::uint32_t y) { return inside(g.camera_a, x, y, aw, ah) && inside(g.camera_b, x, y, bw, bh); };
    // Per row the overlap is one run (convex coverages); found on a coarse scan and refined to the pixel at both ends.
    std::vector<std::int64_t> first(g.height, -1), last(g.height, -1);
    ForEachRow(g.height, thread_count, [&](std::uint32_t y) {
        std::int64_t seed = -1;
        for (std::uint32_t x = 0; x < g.width && seed < 0; x += 8) if (both(x, y)) seed = x;
        if (seed < 0) return;
        std::int64_t lo = seed; while (lo > 0 && both(static_cast<std::uint32_t>(lo-1), y)) --lo;
        std::int64_t hi = seed; while (hi+1 < g.width && both(static_cast<std::uint32_t>(hi+1), y)) ++hi;
        first[y] = lo; last[y] = hi;
    });
    std::vector<gate::BandRow> band(g.height);
    const std::int64_t m = inset;
    for (std::int64_t y = 0; y < g.height; ++y) {
        std::int64_t lo = 0, hi = g.width;
        bool ok = y-m >= 0 && y+m < g.height;
        for (std::int64_t v = y-m; ok && v <= y+m; ++v) {
            if (first[static_cast<std::size_t>(v)] < 0) { ok = false; break; }
            lo = std::max(lo, first[static_cast<std::size_t>(v)]); hi = std::min(hi, last[static_cast<std::size_t>(v)]);
        }
        lo += m; hi -= m;
        if (ok && lo <= hi) band[static_cast<std::size_t>(y)] = {static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi+1)};
    }
    return band;
}

RegistrationGateLayout MakeRegistrationGateLayout(const RegistrationGateRequest& r) {
    ValidateRegistrationGateRequest(r);
    gate::SeamGateGeometry base;
    base.width = r.mapped.width; base.height = r.mapped.height;
    base.camera_a = r.mapped.camera_a; base.camera_b = r.mapped.camera_b;
    const auto coverage = OverlapBandOnThreads(base, r.camera_a->width, r.camera_a->height, r.camera_b->width, r.camera_b->height,
        gate::RequiredBandInsetPx(r.gate_design), r.thread_count);
    const std::int64_t seam = r.layout.seam_column;
    const auto clip = [&](const gate::BandRow& row, std::int64_t half) {
        const std::int64_t begin = std::max<std::int64_t>(row.begin, seam-half), end = std::min<std::int64_t>(row.end, seam+half+1);
        return begin < end ? gate::BandRow{static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end)} : gate::BandRow{};
    };
    const std::int64_t band_half = r.band_measure_half_width_px ? std::min<std::int64_t>(r.band_measure_half_width_px, r.layout.band_half_width_px)
                                                                : r.layout.band_half_width_px;
    RegistrationGateLayout out;
    out.omega_half_width_px = r.layout.omega_half_width_px();
    auto transition = gate::SeamCutTransitionBand(std::vector<std::uint32_t>(base.height, r.layout.seam_column), r.blend.width_pixels, base.width);
    out.omega = base; out.band = base;
    out.omega.band.assign(base.height, {}); out.band.band.assign(base.height, {});
    for (std::uint32_t y = 0; y < base.height; ++y) {
        if (coverage[y].begin >= coverage[y].end) {
            // The measurer cannot reach this row (the inset of the overlap): the composite still mixes here; reported, not passed on.
            if (transition[y].begin < transition[y].end) AppendRows(out.outside_measurable, y, y);
            transition[y] = {};
            continue;
        }
        out.omega.band[y] = clip(coverage[y], out.omega_half_width_px);
        out.band.band[y] = clip(coverage[y], band_half);
    }
    out.omega.transition = transition;
    out.band.transition = std::move(transition);
    return out;
}

RegistrationGateResult DecideRegistrationGate(const RegistrationGateRequest& r, const RegistrationGateLayout& layout,
    RegistrationGateMeasurements measurements) {
    ValidateRegistrationGateRequest(r);
    Require(layout.omega.width == r.mapped.width && layout.omega.height == r.mapped.height && layout.omega.band.size() == layout.omega.height,
        "registration gate: the layout does not belong to the request");
    ValidateMeasurement(measurements.omega, layout.omega, "registration gate: the Omega measurement does not lie in Omega");
    Require(measurements.omega.limits.limit_px == r.gate_limits.limit_px && measurements.omega.limits.guard_band_px == r.gate_limits.guard_band_px,
        "registration gate: the Omega measurement was made with other limits");

    RegistrationGateResult out;
    out.design = r.design;
    const double c_min = r.design.minimum_baseline_component;
    const auto& omega = measurements.omega;
    // The decision reads Omega's points and blocks only. The anchors of the global placement check come from the whole band
    // when it was measured (11.6 item 3): Omega alone can be too narrow to hold enough of them.
    gate::SeamMeasurement decided = omega;
    const gate::SeamGateGeometry* anchors_from = &layout.omega;
    if (measurements.band) {
        ValidateMeasurement(*measurements.band, layout.band, "registration gate: the band measurement does not lie in the band");
        Require(measurements.band->design.global_minimum_anchors == omega.design.global_minimum_anchors &&
            measurements.band->design.global_search_radius_px == omega.design.global_search_radius_px,
            "registration gate: the band was measured with other placement constants");
        decided.global = measurements.band->global;
        anchors_from = &layout.band;
    }
    {
        std::uint32_t first = ~0u, last = 0;
        for (const auto& row : anchors_from->band) if (row.begin < row.end) { first = std::min(first, row.begin); last = std::max(last, row.end-1); }
        if (first <= last) out.anchor_columns = std::pair{first, last};
    }
    auto seam_result = gate::DecideSeamGate(decided, r.gate_limits);
    if (measurements.band) {
        GateBandDiagnostic band;
        band.result = gate::DecideSeamGate(*measurements.band, r.gate_limits);
        band.maxima = MaximaOf(*measurements.band, r.baseline, c_min);
        band.rows = RowsOf(*measurements.band, r.baseline, c_min);
        out.band = std::move(band);
    }

    out.rows = RowsOf(omega, r.baseline, c_min, &r);
    out.rows.outside_measurable = layout.outside_measurable;
    out.maxima = MaximaOf(omega, r.baseline, c_min);
    out.after.maximum_px = seam_result.maximum_px;
    out.after.maxima = out.maxima;
    if (r.correction) {
        out.before.maxima = MaximaOf(omega, r.baseline, c_min, [&](const gate::SeamPoint& p) {
            const Point s = CorrectionAt(r, p);
            return Point{p.displacement.x+s.x, p.displacement.y+s.y};
        });
        const auto& m = out.before.maxima;
        std::optional<double> top = m.two_directional_px;
        if (m.one_directional_px && (!top || *m.one_directional_px > *top)) top = m.one_directional_px;
        if (seam_result.predicted_maximum_px && (!top || *seam_result.predicted_maximum_px > *top)) top = seam_result.predicted_maximum_px;
        out.before.maximum_px = top;
    } else {
        out.before = out.after;
    }

    // State and cause (11.22.1, 11.22.3, 11.30). A value over the line and a contradicted placement are the seam gate's and stay
    // as they are. What could not be measured is decided here from the rows of Omega and the placement's basis, not from the
    // seam gate's block counts: a blank block (no content in either camera) holds nothing to see and is never a reason; content
    // that nothing measured is a reason when it is unconfirmed (#385 Q1), in one camera only, all of the content, or more than
    // the design's share of it; rows that only measured lines along the baseline are a reason only when no row measured the
    // baseline component; anchors that verify nothing leave the placement to a trusted estimate when the caller has one.
    const bool excess = HasReason(seam_result, "seam-misalignment") || HasReason(seam_result, "predicted-misalignment") ||
        HasReason(seam_result, "global-placement-mismatch");
    const bool transition = HasReason(seam_result, "transition-outside-band");
    // Seam gate reasons that this part decides again (above) or reports as its own (the transition).
    const bool other = std::any_of(seam_result.findings.begin(), seam_result.findings.end(), [](const gate::SeamGateFinding& f) {
        for (const char* known : {"seam-misalignment", "predicted-misalignment", "global-placement-mismatch", "unmeasurable-content",
                 "no-measurable-block", "unpredicted-empty-block", "global-placement-unverified", "transition-outside-band"})
            if (f.reason == known) return false;
        return true;
    });
    if (seam_result.global_contradicted) out.placement = PlacementBasis::none;
    else if (seam_result.global_verified >= decided.design.global_minimum_anchors) out.placement = PlacementBasis::anchors;
    else if (r.placement_by_estimate) out.placement = PlacementBasis::estimate;
    else out.placement = PlacementBasis::none;
    const bool placement_unverified = !seam_result.global_contradicted && out.placement == PlacementBasis::none;
    const auto& rows = out.rows;
    const bool unconfirmed = rows.rows_unconfirmed > 0, one_sided = rows.rows_one_sided > 0;
    // D1 (#385): nothing measured is never a pass, a blank Omega included.
    const bool nothing_measured = rows.rows_measured == 0;
    const bool content_unmeasured = !nothing_measured && rows.content_unmeasured_share > r.design.maximum_unmeasured_content_share;
    const bool dx_unmeasured = !nothing_measured && rows.rows_dx_measured == 0;
    if (seam_result.state == gate::SeamGateState::undetermined) out.state = GateState::undetermined;
    else if (excess) {
        out.state = GateState::fail;
        out.lift = LiftOf(r, omega);
        const auto& l = out.lift;
        if (l && l->baseline_share >= r.design.lift_share && l->sign_agreement_share >= r.design.lift_share) out.cause = GateCause::lift;
        else if (HasReason(seam_result, "seam-misalignment") && !HasReason(seam_result, "global-placement-mismatch") &&
            ExplainedBySimilarity(r, omega)) out.cause = GateCause::pose;
        else out.cause = GateCause::displacement;
        if (out.cause != GateCause::lift) out.lift.reset();
    } else if (unconfirmed || one_sided || nothing_measured || content_unmeasured) {
        out.state = GateState::fail;
        out.cause = GateCause::unmeasurable;
    } else if (other) {
        // A reason of the seam gate this part does not know: not a pass.
        out.state = GateState::fail;
        out.cause = GateCause::displacement;
    } else out.state = GateState::pass;
    if (seam_result.state != gate::SeamGateState::undetermined) {
        if (unconfirmed) out.gate_findings.push_back({"unconfirmed-content", "review-visually"});
        if (one_sided) out.gate_findings.push_back({"one-sided-content", "review-visually"});
        if (nothing_measured) out.gate_findings.push_back({"nothing-measured", "review-visually"});
        if (content_unmeasured) out.gate_findings.push_back({"content-unmeasured", "review-visually"});
        if (dx_unmeasured) out.gate_findings.push_back({"dx-unmeasured", "review-visually"});
        if (placement_unverified) out.gate_findings.push_back({"placement-unverified", "review-visually"});
    }
    if (transition) out.gate_findings.push_back({"transition-outside-band", "contact-technician"});
    if (seam_result.state == gate::SeamGateState::undetermined) out.gate_findings.push_back({"gate-undetermined", "contact-technician"});
    if (out.state == GateState::pass && (dx_unmeasured || placement_unverified || transition)) out.state = GateState::undetermined;
    out.remedy = std::string(GateCauseRemedy(out.cause));
    // The wide search (11.6 item 4): asked by the caller, or many of Omega's content points could not be told apart.
    {
        std::uint32_t content = 0, unclear = 0;
        for (const auto& p : omega.points) {
            if (!p.content) continue;
            ++content;
            unclear += p.status == gate::PointStatus::ambiguous || p.status == gate::PointStatus::search_boundary;
        }
        if (r.wide_search || (content && static_cast<double>(unclear)/content >= r.design.wide_search_trigger_share))
            out.wide_search = WideSearch(r, layout);
    }
    out.findings = seam_result.findings;
    out.seam_gate = std::move(seam_result);
    out.measurement = std::move(decided);
    // The correct rows of the split, for reference only (after the decision: nothing above reads them).
    Require(measurements.corrected.has_value() == layout.corrected_omega.has_value(),
        "registration gate: the correct rows are measured exactly when the layout is split");
    if (measurements.corrected) {
        const auto& m = *measurements.corrected;
        ValidateMeasurement(m, *layout.corrected_omega, "registration gate: the correct rows' measurement does not lie in their rows");
        GateCorrectedReference ref;
        ref.after.maximum_px = gate::DecideSeamGate(m, r.gate_limits).maximum_px;
        ref.after.maxima = MaximaOf(m, r.baseline, c_min);
        if (r.correction) {
            ref.before.maxima = MaximaOf(m, r.baseline, c_min, [&](const gate::SeamPoint& p) {
                const Point s = CorrectionAt(r, p);
                return Point{p.displacement.x+s.x, p.displacement.y+s.y};
            });
            std::optional<double> top = ref.before.maxima.two_directional_px;
            if (ref.before.maxima.one_directional_px && (!top || *ref.before.maxima.one_directional_px > *top)) top = ref.before.maxima.one_directional_px;
            ref.before.maximum_px = top;
        } else ref.before = ref.after;
        const auto counted = RowsOf(m, r.baseline, c_min);
        ref.rows_with_content = counted.rows_with_content;
        ref.rows_measured = counted.rows_measured;
        out.rows.rows_corrected = counted.rows_blank+counted.rows_with_content;
        out.rows.corrected = layout.corrected;
        out.corrected = ref;
        out.corrected_measurement = m;
    }
    return out;
}

gate::SeamMeasurement MeasureGateOmega(const RegistrationGateRequest& r, const RegistrationGateLayout& layout, bool global_decided_by_band) {
    ValidateRegistrationGateRequest(r);
    if (!global_decided_by_band)
        return gate::MeasureSeam(*r.camera_a, *r.camera_b, layout.omega, r.gate_design, r.gate_limits, r.prediction, r.thread_count);
    // The global check reads the points' blocks after they are measured and changes none of them; one anchor is the least it can run.
    auto least = r.gate_design;
    least.global_minimum_anchors = 1;
    least.global_maximum_evaluations = 1;
    auto omega = gate::MeasureSeam(*r.camera_a, *r.camera_b, layout.omega, least, r.gate_limits, r.prediction, r.thread_count);
    omega.design = r.gate_design;
    return omega;
}

RegistrationGateResult MeasureRegistrationGate(const RegistrationGateRequest& r) {
    const auto layout = MakeRegistrationGateLayout(r);
    RegistrationGateMeasurements m;
    m.omega = MeasureGateOmega(r, layout, r.measure_whole_band_for_anchors);
    if (r.measure_whole_band_for_anchors)
        m.band = gate::MeasureSeam(*r.camera_a, *r.camera_b, layout.band, r.gate_design, r.gate_limits, r.prediction, r.thread_count);
    return DecideRegistrationGate(r, layout, std::move(m));
}

GatePatch GatePatchOf(const gate::SeamPoint& p, const gate::SeamGateDesign& d) {
    std::int64_t hx = d.patch_px/2, y_low = -static_cast<std::int64_t>(d.patch_px/2), y_high = d.patch_px/2;
    for (const auto& v : p.remeasurements) {
        if (v.variant == gate::Variant::enlarged) {
            const std::int64_t hl = d.remeasure_patch_px/2;
            hx = std::max(hx, hl); y_low = std::min(y_low, -hl); y_high = std::max(y_high, hl);
        } else if (v.variant == gate::Variant::shifted) {
            // The patch moved down, and up when the move down was not valid; both may have been read.
            const std::int64_t s = d.remeasure_shift_px, h = d.patch_px/2;
            y_low = std::min(y_low, -s-h); y_high = std::max(y_high, s+h);
        }
    }
    const std::int64_t x = p.x, y = p.y, e = kFeatureSmoothingPx;
    return {x-hx-e, y+y_low-e, x+hx+e+1, y+y_high+e+1};
}

std::uint32_t SplitGateOmega(RegistrationGateLayout& layout, std::uint32_t first_row, std::uint32_t row_step, std::uint32_t row_count,
    std::uint32_t period, std::uint32_t phase, std::uint32_t spacing) {
    Require(period >= 1 && phase < period, "registration gate: the split's phase is not below its period");
    Require(spacing >= 2 && row_step == spacing && row_count >= 1 && first_row >= spacing/2, "registration gate: the split's rows are not on the gate's lattice");
    Require(!layout.corrected_omega && layout.corrected.empty(), "registration gate: the layout is split already");
    auto& o = layout.omega;
    Require(o.band.size() == o.height && (o.transition.empty() || o.transition.size() == o.height), "registration gate: the layout's rows do not match its height");
    const std::uint64_t begin = first_row-spacing/2, end = begin+static_cast<std::uint64_t>(row_count)*spacing;
    gate::SeamGateGeometry corrected = o;
    corrected.band.assign(o.height, {});
    if (!corrected.transition.empty()) corrected.transition.assign(o.height, {});
    std::uint32_t moved = 0;
    for (std::uint32_t y = 0; y < o.height; ++y) {
        const bool band = o.band[y].begin < o.band[y].end, mixes = !o.transition.empty() && o.transition[y].begin < o.transition[y].end;
        if (!band && !mixes) continue;
        if (y < begin || y >= end) throw std::logic_error("registration gate: a row of Omega lies off layer 2's rows");
        if (static_cast<std::uint32_t>((y-begin)/spacing)%period == phase) continue;
        corrected.band[y] = o.band[y];
        o.band[y] = {};
        if (mixes) { corrected.transition[y] = o.transition[y]; o.transition[y] = {}; }
        AppendRows(layout.corrected, y, y);
        ++moved;
    }
    // The measurer puts its rows at the first band row + spacing / 2: both parts must still begin on layer 2's lattice.
    for (const auto* g : {&o, &corrected}) for (std::uint32_t y = 0; y < g->height; ++y) if (g->band[y].begin < g->band[y].end) {
        if ((y-begin)%spacing != 0) throw std::logic_error("registration gate: a part of the split begins off layer 2's rows");
        break;
    }
    layout.corrected_omega = std::move(corrected);
    return moved;
}

std::vector<GatePatch> RegistrationGatePatches(const gate::SeamMeasurement& m) {
    std::vector<GatePatch> out;
    for (const auto& p : m.points) if (p.status != gate::PointStatus::outside_coverage) out.push_back(GatePatchOf(p, m.design));
    return out;
}
}
