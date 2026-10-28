#include "a0/m2/seam_gate.hpp"
#include "overlap_estimation_detail.hpp"
#include "zero_mean_correlation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace a0::m2::seam_gate {
namespace {
using overlap_detail::Plane;
namespace cd = correlation_detail;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kNegative = -std::numeric_limits<double>::infinity();
// Samples on each side consumed by the smoothing before matching (FeatureOf).
constexpr int kSmooth = 2;

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

// Original image as (B+G+R)/3, cropped to what the band can reach.
struct RawLuma {
    int x0{}, y0{}, width{}, height{}, image_width{}, image_height{};
    std::vector<float> v;
};
RawLuma MakeRawLuma(const BgrImage& image, int x0, int y0, int x1, int y1) {
    RawLuma l;
    l.image_width = static_cast<int>(image.width); l.image_height = static_cast<int>(image.height);
    l.x0 = std::clamp(x0, 0, l.image_width); l.y0 = std::clamp(y0, 0, l.image_height);
    l.width = std::clamp(x1, 0, l.image_width)-l.x0; l.height = std::clamp(y1, 0, l.image_height)-l.y0;
    if (l.width <= 0 || l.height <= 0) { l.width = l.height = 0; return l; }
    l.v.resize(static_cast<std::size_t>(l.width)*l.height);
    for (int y = 0; y < l.height; ++y) {
        const std::uint8_t* const in = image.bgr.data()+(static_cast<std::size_t>(y+l.y0)*image.width+l.x0)*3;
        float* const out = l.v.data()+static_cast<std::size_t>(y)*l.width;
        for (int x = 0; x < l.width; ++x) out[x] = static_cast<float>((in[3*x]+in[3*x+1]+in[3*x+2])/3.0);
    }
    return l;
}
// Catmull-Rom a=-0.5, the kernel of render::SampleBicubic: coverage
// [0,W)x[0,H), support indexes clamped at the image edges.
std::array<double,4> CubicWeights(double t) {
    const double t2 = t*t, t3 = t2*t;
    return {-.5*t+t2-.5*t3, 1-2.5*t2+1.5*t3, .5*t+2*t2-1.5*t3, -.5*t2+.5*t3};
}
bool SampleLuma(const RawLuma& l, Point raw, double& value) {
    if (!(raw.x >= 0 && raw.y >= 0 && raw.x < l.image_width && raw.y < l.image_height)) return false;
    const int ix = static_cast<int>(std::floor(raw.x)), iy = static_cast<int>(std::floor(raw.y));
    const auto wx = CubicWeights(raw.x-ix), wy = CubicWeights(raw.y-iy);
    std::array<int,4> sx{};
    for (int c = 0; c < 4; ++c) {
        sx[c] = std::clamp(ix+c-1, 0, l.image_width-1)-l.x0;
        if (sx[c] < 0 || sx[c] >= l.width) return false;
    }
    double sum = 0;
    for (int r = 0; r < 4; ++r) {
        const int sy = std::clamp(iy+r-1, 0, l.image_height-1)-l.y0;
        if (sy < 0 || sy >= l.height) return false;
        const float* const row = l.v.data()+static_cast<std::size_t>(sy)*l.width;
        sum += wy[r]*(wx[0]*row[sx[0]]+wx[1]*row[sx[1]]+wx[2]*row[sx[2]]+wx[3]*row[sx[3]]);
    }
    value = sum;
    return true;
}
bool Map(const OutputToCamera& mapping, Point output, Point& raw) {
    if (!mapping(output, raw)) return false;
    Require(std::isfinite(raw.x) && std::isfinite(raw.y), "seam gate: mapping returned nonfinite camera coordinates");
    return true;
}

struct Camera {
    const OutputToCamera* map{};
    RawLuma luma;
    Plane strip;          // output-grid luminance of the current strip, NaN where not covered
    int strip_x0{}, strip_y0{};
};
double SampleOutput(const Camera& camera, Point output) {
    Point raw{}; double value{};
    return Map(*camera.map, output, raw) && SampleLuma(camera.luma, raw, value) ? value : kNaN;
}
// Output-grid luminance on [x0,x0+w) x [y0,y0+h): from the strip when it
// holds the region, sampled directly otherwise. Same values either way.
Plane LumaRegion(const Camera& camera, int x0, int y0, int w, int h) {
    Plane p{w,h,std::vector<double>(static_cast<std::size_t>(w)*h)};
    const int sx = x0-camera.strip_x0, sy = y0-camera.strip_y0;
    if (sx >= 0 && sy >= 0 && sx+w <= camera.strip.width && sy+h <= camera.strip.height) {
        for (int y = 0; y < h; ++y) std::copy_n(camera.strip.v.data()+static_cast<std::size_t>(sy+y)*camera.strip.width+sx, w,
            p.v.data()+static_cast<std::size_t>(y)*w);
        return p;
    }
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        p.v[static_cast<std::size_t>(y)*w+x] = SampleOutput(camera, {static_cast<double>(x0+x), static_cast<double>(y0+y)});
    return p;
}
// Luminance for features of 2e cells starting at (cx-e, cy-e).
Plane FeatureRegion(const Camera& camera, int cx, int cy, int e) {
    return LumaRegion(camera, cx-e-kSmooth, cy-e-kSmooth, 2*e+1+2*kSmooth, 2*e+1+2*kSmooth);
}
bool HasNaN(const Plane& p) {
    for (const double v : p.v) if (std::isnan(v)) return true;
    return false;
}
Plane Crop(const Plane& p, int x0, int y0, int w, int h) {
    Plane c{w,h,std::vector<double>(static_cast<std::size_t>(w)*h)};
    for (int y = 0; y < h; ++y) std::copy_n(p.v.data()+static_cast<std::size_t>(y0+y)*p.width+x0, w, c.v.data()+static_cast<std::size_t>(y)*w);
    return c;
}

enum class Feature { gradient, luminance };
// Output-grid luminance is smoothed by the binomial [1 4 6 4 1]/16 in both
// axes before matching, the same for A and B. It removes the band near the
// sampling limit where Catmull-Rom resampling of a sharp line depends on the
// fractional phase, which would otherwise bias the subpixel position.
Plane Smooth(const Plane& p) {
    const auto pass = [](const Plane& in, int dx, int dy) {
        Plane out{in.width-4*dx,in.height-4*dy,{}};
        out.v.resize(static_cast<std::size_t>(out.width)*out.height);
        const std::size_t step = static_cast<std::size_t>(dy)*in.width+dx;
        for (int y = 0; y < out.height; ++y) {
            const double* const row = in.v.data()+static_cast<std::size_t>(y)*in.width;
            double* const o = out.v.data()+static_cast<std::size_t>(y)*out.width;
            for (int x = 0; x < out.width; ++x) {
                const double* const q = row+x;
                o[x] = (q[0]+4*q[step]+6*q[2*step]+4*q[3*step]+q[4*step])/16;
            }
        }
        return out;
    };
    return pass(pass(p,1,0),0,1);
}
// Feature cell i of a luminance plane sampled from position x0-kSmooth: the
// gradient magnitude between smoothed samples x0+i and x0+i+1 (#364/#365), or
// the smoothed luminance at x0+i. kSmooth*2+1 cells smaller per axis.
Plane FeatureOfSmoothed(const Plane& s, Feature feature) {
    return feature == Feature::gradient ? overlap_detail::Gradient(s) : Crop(s,0,0,s.width-1,s.height-1);
}
Plane FeatureOf(const Plane& luma, Feature feature) { return FeatureOfSmoothed(Smooth(luma),feature); }
// Margin of a smoothed plane kept for a fractional shift (Catmull-Rom support).
constexpr int kShiftMargin = 2;
// Smoothed plane s shifted by f (|f| < 1 per axis): out(i,j) = s(i+m+f.x, j+m+f.y)
// with m = kShiftMargin, by the separable Catmull-Rom of render::SampleBicubic.
// Smoothing and the shift commute, so the smoothing is done once per point;
// f = 0 returns the samples unchanged.
Plane ShiftPlane(const Plane& s, Point f, int size) {
    const int ix = static_cast<int>(std::floor(f.x)), iy = static_cast<int>(std::floor(f.y));
    const auto wx = CubicWeights(f.x-ix), wy = CubicWeights(f.y-iy);
    Plane rows{size,s.height,std::vector<double>(static_cast<std::size_t>(size)*s.height)};
    for (int y = 0; y < s.height; ++y) {
        const double* const in = s.v.data()+static_cast<std::size_t>(y)*s.width+kShiftMargin+ix-1;
        double* const out = rows.v.data()+static_cast<std::size_t>(y)*size;
        for (int x = 0; x < size; ++x) out[x] = wx[0]*in[x]+wx[1]*in[x+1]+wx[2]*in[x+2]+wx[3]*in[x+3];
    }
    Plane result{size,size,std::vector<double>(static_cast<std::size_t>(size)*size)};
    for (int y = 0; y < size; ++y) {
        const double* const in = rows.v.data()+static_cast<std::size_t>(y+kShiftMargin+iy-1)*size;
        double* const out = result.v.data()+static_cast<std::size_t>(y)*size;
        for (int x = 0; x < size; ++x)
            out[x] = wy[0]*in[x]+wy[1]*in[x+size]+wy[2]*in[x+2*static_cast<std::size_t>(size)]+wy[3]*in[x+3*static_cast<std::size_t>(size)];
    }
    return result;
}

using Levels = std::array<Plane,3>;
Levels Pyramid(Plane p) {
    Levels l; l[0] = std::move(p); l[1] = overlap_detail::Half(l[0]); l[2] = overlap_detail::Half(l[1]);
    return l;
}
struct Template {
    std::array<std::vector<double>,3> t;
    std::array<double,3> norm{};
    int n{};
};
Template MakeTemplate(const Plane& feature) {
    Template t; t.n = feature.width;
    const auto levels = Pyramid(feature);
    for (int k = 0; k < 3; ++k) { t.t[k] = levels[k].v; t.norm[k] = cd::Center(t.t[k]); }
    return t;
}
double Score(const Template& t, int k, const Plane& window, int x, int y) {
    const int n = t.n >> k;
    if (!(t.norm[k] > 0) || x < 0 || y < 0 || x+n > window.width || y+n > window.height) return kNegative;
    double r{};
    return cd::WindowCorrelation(t.t[k], t.norm[k], n, n, window.v.data()+static_cast<std::size_t>(y)*window.width+x, window.width, r) ? r : kNegative;
}

struct Direction { bool one{}; Point n{}; };
// Lines in one direction: only the cells nearest the line through the
// center along the normal. Along the line every offset matches equally; a
// tangential drift would mix the normal estimate error into the result.
bool Allowed(const Direction& d, double ox, double oy) {
    return !d.one || std::abs(-d.n.y*ox+d.n.x*oy) <= 0.5+1e-9;
}
bool NearBest(const Direction& d, int ox, int oy) {
    return d.one ? std::abs(d.n.x*ox+d.n.y*oy) <= 1.0 : std::max(std::abs(ox),std::abs(oy)) <= 1;
}
struct Surface {
    int x0{}, y0{}, w{}, h{};
    std::vector<double> s;
    double At(int x, int y) const { return s[static_cast<std::size_t>(y-y0)*w+(x-x0)]; }
};
struct Peak { PointStatus status{}; int x{}, y{}; double score{}; };
// Maximum (first in scan order), then the highest other local maximum away
// from it.
Peak Analyze(const Surface& surface, const Direction& direction, double ratio) {
    Peak peak{PointStatus::flat_correlation,0,0,kNegative};
    for (int y = surface.y0; y < surface.y0+surface.h; ++y) for (int x = surface.x0; x < surface.x0+surface.w; ++x) {
        const double v = surface.At(x,y);
        if (v > peak.score) { peak.score = v; peak.x = x; peak.y = y; }
    }
    if (peak.score == kNegative || !(peak.score > 0)) return peak;
    // An equal value next to the maximum (on the ridge for lines in one
    // direction) is the same peak; an equal value elsewhere is a second peak.
    double second = kNegative;
    for (int y = surface.y0; y < surface.y0+surface.h; ++y) for (int x = surface.x0; x < surface.x0+surface.w; ++x) {
        const double v = surface.At(x,y);
        if (v == kNegative || NearBest(direction,x-peak.x,y-peak.y)) continue;
        bool local = true;
        for (int dy = -1; dy <= 1 && local; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int u = x+dx, w = y+dy;
            if ((dx || dy) && u >= surface.x0 && w >= surface.y0 && u < surface.x0+surface.w && w < surface.y0+surface.h &&
                surface.At(u,w) > v) { local = false; break; }
        }
        if (local) second = std::max(second,v);
    }
    peak.status = second >= ratio*peak.score ? PointStatus::ambiguous : PointStatus::valid;
    return peak;
}
// Scores of offsets [cx-a,cx+a] x [cy-a,cy+a] clipped to [-r,r], at level k.
// The window plane holds the zero offset at (r,r).
Surface Scores(const Template& t, int k, const Plane& window, int r, int cx, int cy, int a, const Direction& d) {
    Surface s;
    s.x0 = std::max(cx-a,-r); s.y0 = std::max(cy-a,-r);
    s.w = std::min(cx+a,r)-s.x0+1; s.h = std::min(cy+a,r)-s.y0+1;
    s.s.assign(static_cast<std::size_t>(std::max(s.w,0))*std::max(s.h,0),kNegative);
    for (int y = s.y0; y < s.y0+s.h; ++y) for (int x = s.x0; x < s.x0+s.w; ++x)
        if (Allowed(d,x-cx,y-cy)) s.s[static_cast<std::size_t>(y-s.y0)*s.w+(x-s.x0)] = Score(t,k,window,x+r,y+r);
    return s;
}
// Coarse-to-fine integer search: the full window at quarter scale, then a
// +-3 neighbourhood at half and full scale. Every level must give a strict
// maximum without a competing peak, inside its neighbourhood.
Peak IntegerSearch(const Template& t, const Levels& window, int radius, const Direction& d, double ratio) {
    const int r2 = radius/4;
    auto peak = Analyze(Scores(t,2,window[2],r2,0,0,r2,d), d, ratio);
    if (peak.status != PointStatus::valid) return peak;
    if (std::abs(peak.x) == r2 || std::abs(peak.y) == r2) { peak.status = PointStatus::search_boundary; return peak; }
    for (int k = 1; k >= 0; --k) {
        const int r = radius >> k, cx = 2*peak.x, cy = 2*peak.y;
        const auto surface = Scores(t,k,window[k],r,cx,cy,3,d);
        peak = Analyze(surface, d, ratio);
        if (peak.status != PointStatus::valid) return peak;
        if (std::abs(peak.x) == r || std::abs(peak.y) == r) { peak.status = PointStatus::search_boundary; return peak; }
        if (std::abs(peak.x-cx) == 3 || std::abs(peak.y-cy) == 3) { peak.status = PointStatus::ambiguous; return peak; }
    }
    return peak;
}

struct Refined { PointStatus status{}; Point f{}; double score{}; double curvature{}; };
// Halving grid around the integer match (#323): 3x3 candidates, strict best
// at every level, no curve fit. For lines in one direction the integer search
// stays on the normal through the point, and the fraction is refined in 2-D
// so a small offset along the line does not bend the result across it.
// target: smoothed luminance of the matched patch with kShiftMargin on each side.
Refined Refine(const Template& t, Feature feature, const Plane& target, const Direction& d, unsigned levels) {
    Refined result{PointStatus::valid,{},0,0};
    const int size = t.n+1;
    const auto evaluate = [&](Point shift, double& score) {
        auto v = FeatureOfSmoothed(ShiftPlane(target,shift,size),feature).v;
        const double norm = cd::Center(v);
        if (!(norm > 0)) return false;
        score = cd::NormalizedDot(t.t[0],t.norm[0],v,norm);
        return std::isfinite(score);
    };
    if (!evaluate({0,0},result.score)) { result.status = PointStatus::search_window_unavailable; return result; }
    double step = 0.5;
    const unsigned curvature_level = std::min(1u,levels-1);
    for (unsigned level = 0; level < levels; ++level, step /= 2) {
        std::array<double,9> s{}; s.fill(kNegative); s[4] = result.score;
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
            if (!i && !j) continue;
            if (!evaluate({result.f.x+i*step,result.f.y+j*step},s[(j+1)*3+i+1])) { result.status = PointStatus::search_window_unavailable; return result; }
        }
        // Lines in one direction match equally along the line: an equal
        // score that moves only along the line is the same position across
        // it, and the candidate nearest the center along the line is kept.
        const auto along = [&](int c) { return std::abs(-d.n.y*(c%3-1)+d.n.x*(c/3-1)); };
        const auto across = [&](int c) { return d.n.x*(c%3-1)+d.n.y*(c/3-1); };
        int best = 4; bool tie = false;
        for (int c = 0; c < 9; ++c) {
            if (c == best) continue;
            if (s[c] > s[best]) { best = c; tie = false; }
            else if (s[c] == s[best]) {
                if (!d.one || std::abs(across(c)-across(best)) > 0.5) tie = true;
                else if (along(c) < along(best)) best = c;
            }
        }
        if (tie) { result.status = PointStatus::ambiguous; return result; }
        if (level == curvature_level) {
            const double h2 = step*step;
            const double fxx = (s[3]+s[5]-2*s[4])/h2, fyy = (s[1]+s[7]-2*s[4])/h2, fxy = (s[8]-s[6]-s[2]+s[0])/(4*h2);
            // Curvature of the score across the lines, or the weaker axis.
            result.curvature = d.one ? -(d.n.x*d.n.x*fxx+2*d.n.x*d.n.y*fxy+d.n.y*d.n.y*fyy) : std::min(-fxx,-fyy);
        }
        result.f = {result.f.x+(best%3-1)*step,result.f.y+(best/3-1)*step};
        result.score = s[best];
    }
    return result;
}

struct Context {
    const SeamGateDesign* design{};
    Camera a, b;
};
struct Single {
    PointStatus status{PointStatus::no_content};
    bool content{};
    PointKind kind{};
    Point normal{}, d{};
    double visible{}, u{}, rt{}, correlation{};
    int radius{};
};
double PeakUncertainty(double score, double curvature, int cells) {
    if (!(curvature > 0)) return std::numeric_limits<double>::infinity();
    return std::sqrt(std::max(0.0,1-score)/(curvature*cells));
}
std::size_t CountAtLeast(const Plane& p, int x0, int y0, int n, double level) {
    std::size_t count = 0;
    for (int y = y0; y < y0+n; ++y) for (int x = x0; x < x0+n; ++x) if (p.v[static_cast<std::size_t>(y)*p.width+x] >= level) ++count;
    return count;
}

// One measurement at output point (px,py) with patch half h. A is the
// template; B is searched. Forward A->B, then B->A from the result.
Single MeasureAt(const Context& c, int px, int py, int h, Feature feature, bool periodic_check) {
    const auto& dz = *c.design;
    Single out;
    const int n = 2*h;
    // Template neighbourhood of A: content, structure and coverage.
    const Plane a_region = FeatureRegion(c.a, px, py, h), b_region = FeatureRegion(c.b, px, py, h);
    if (HasNaN(a_region) || HasNaN(b_region)) { out.status = PointStatus::outside_coverage; return out; }
    // Content and structure on the unsmoothed samples of the patch.
    const Plane a_patch = Crop(a_region,kSmooth,kSmooth,n+1,n+1), b_patch = Crop(b_region,kSmooth,kSmooth,n+1,n+1);
    const auto ga = overlap_detail::Gradient(a_patch), gb = overlap_detail::Gradient(b_patch);
    const auto minimum = static_cast<std::size_t>(dz.content_minimum_samples);
    out.content = CountAtLeast(ga,0,0,n,dz.content_gradient_dn) >= minimum || CountAtLeast(gb,0,0,n,dz.content_gradient_dn) >= minimum;
    if (!out.content) { out.status = PointStatus::no_content; return out; }
    double sxx = 0, sxy = 0, syy = 0;
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const auto at = [&](int u, int v) { return a_patch.v[static_cast<std::size_t>(v)*a_patch.width+u]; };
        const double gx = at(x+1,y)-at(x,y), gy = at(x,y+1)-at(x,y);
        sxx += gx*gx; sxy += gx*gy; syy += gy*gy;
    }
    const double mean = (sxx+syy)/2, spread = std::sqrt((sxx-syy)*(sxx-syy)/4+sxy*sxy);
    const double l1 = mean+spread, l2 = mean-spread;
    Direction direction;
    if (l1 > 0 && l2 < dz.one_direction_ratio*l1) {
        Point v = std::abs(sxx-l2) >= std::abs(syy-l2) ? Point{sxx-l2,sxy} : Point{sxy,syy-l2};
        const double length = std::hypot(v.x,v.y);
        if (length > 0) {
            direction = {true,{v.x/length,v.y/length}};
            // One sign per line orientation keeps reports comparable.
            if (direction.n.x < 0 || (direction.n.x == 0 && direction.n.y < 0)) direction.n = {-direction.n.x,-direction.n.y};
        }
    }
    out.kind = direction.one ? PointKind::one_directional : PointKind::two_directional;
    out.normal = direction.n;
    const Template ta = MakeTemplate(FeatureOf(a_region,feature));
    if (!(ta.norm[0] > 0) || !(ta.norm[2] > 0)) { out.status = PointStatus::flat_correlation; return out; }
    int radius = static_cast<int>(dz.search_radius_px);
    for (;;) {
        out.radius = radius;
        const bool widened = radius > static_cast<int>(dz.search_radius_px);
        if (periodic_check) {
            const int e = h+2*radius;
            const Plane region = FeatureRegion(c.a, px, py, e);
            if (HasNaN(region)) { out.status = widened ? PointStatus::search_boundary : PointStatus::search_window_unavailable; return out; }
            const auto self = Pyramid(FeatureOf(region,feature));
            const int r = radius/2; // +-2R at quarter scale
            const auto surface = Scores(ta,2,self[2],r,0,0,r,direction);
            const auto peak = Analyze(surface,direction,dz.secondary_peak_ratio);
            if (peak.status != PointStatus::valid || !NearBest(direction,peak.x,peak.y)) { out.status = PointStatus::periodic; return out; }
        }
        const int e = h+radius;
        const Plane region_b = FeatureRegion(c.b, px, py, e);
        if (HasNaN(region_b)) { out.status = widened ? PointStatus::search_boundary : PointStatus::search_window_unavailable; return out; }
        const auto forward = IntegerSearch(ta, Pyramid(FeatureOf(region_b,feature)), radius, direction, dz.secondary_peak_ratio);
        if (forward.status == PointStatus::search_boundary && radius < static_cast<int>(dz.maximum_search_radius_px)) {
            radius = std::min(2*radius,static_cast<int>(dz.maximum_search_radius_px)); continue;
        }
        if (forward.status != PointStatus::valid) { out.status = forward.status; return out; }
        const int bx = px+forward.x, by = py+forward.y;
        const Plane sb = Smooth(FeatureRegion(c.b, bx, by, h+kShiftMargin));
        if (HasNaN(sb)) { out.status = PointStatus::search_window_unavailable; return out; }
        const auto rf = Refine(ta, feature, sb, direction, dz.refinement_levels);
        if (rf.status != PointStatus::valid) { out.status = rf.status; return out; }
        // Reverse: B's patch at q = (bx,by)+f, searched in A around (bx,by).
        const Template tb = MakeTemplate(FeatureOfSmoothed(ShiftPlane(sb, rf.f, n+1), feature));
        if (!(tb.norm[0] > 0) || !(tb.norm[2] > 0)) { out.status = PointStatus::flat_correlation; return out; }
        const Plane region_a = FeatureRegion(c.a, bx, by, e);
        if (HasNaN(region_a)) { out.status = PointStatus::search_window_unavailable; return out; }
        const auto reverse = IntegerSearch(tb, Pyramid(FeatureOf(region_a,feature)), radius, direction, dz.secondary_peak_ratio);
        if (reverse.status != PointStatus::valid) { out.status = reverse.status; return out; }
        const int ax = bx+reverse.x, ay = by+reverse.y;
        const Plane sa = Smooth(FeatureRegion(c.a, ax, ay, h+kShiftMargin));
        if (HasNaN(sa)) { out.status = PointStatus::search_window_unavailable; return out; }
        const auto rr = Refine(tb, feature, sa, direction, dz.refinement_levels);
        if (rr.status != PointStatus::valid) { out.status = rr.status; return out; }
        out.d = {forward.x+rf.f.x, forward.y+rf.f.y};
        // Where A shows B's patch at q, minus where it started.
        const Point trip{bx+reverse.x+rr.f.x-px, by+reverse.y+rr.f.y-py};
        out.rt = direction.one ? std::abs(direction.n.x*trip.x+direction.n.y*trip.y) : std::hypot(trip.x,trip.y);
        out.visible = direction.one ? std::abs(direction.n.x*out.d.x+direction.n.y*out.d.y) : std::hypot(out.d.x,out.d.y);
        out.correlation = std::min(rf.score,rr.score);
        const int cells = n*n;
        const double peak_u = std::max(PeakUncertainty(rf.score,rf.curvature,cells),PeakUncertainty(rr.score,rr.curvature,cells));
        out.u = std::sqrt(peak_u*peak_u+out.rt*out.rt)+dz.uncertainty_floor_px;
        if (out.rt > dz.round_trip_px) out.status = PointStatus::round_trip_inconsistent;
        else if (!(out.u <= dz.maximum_uncertainty_px)) out.status = PointStatus::uncertain;
        else out.status = PointStatus::valid;
        return out;
    }
}

struct Comparable { PointKind kind; Point normal, d; double u; };
// Agreement of two displacement measurements, in the components both define.
bool Agree(const Comparable& a, const Comparable& b, const SeamGateDesign& dz) {
    const double tolerance = std::max(dz.agreement_factor*std::max(a.u,b.u),dz.agreement_floor_px);
    const Point diff{b.d.x-a.d.x,b.d.y-a.d.y};
    const bool one_a = a.kind == PointKind::one_directional, one_b = b.kind == PointKind::one_directional;
    if (one_a && one_b) {
        if (std::abs(a.normal.x*b.normal.x+a.normal.y*b.normal.y) < std::cos(10*3.14159265358979323846/180)) return false;
        return std::abs(a.normal.x*diff.x+a.normal.y*diff.y) <= tolerance;
    }
    if (one_a) return std::abs(a.normal.x*diff.x+a.normal.y*diff.y) <= tolerance;
    if (one_b) return std::abs(b.normal.x*diff.x+b.normal.y*diff.y) <= tolerance;
    return std::hypot(diff.x,diff.y) <= tolerance;
}

// Workers of the global placement check. Each holds a wide window of the
// order of (2*(patch/2+radius))^2 doubles several times over (tens of MB), so
// the check uses at most this many workers whatever thread count the caller
// passes. The result does not depend on it.
constexpr unsigned kGlobalWorkers = 4;

// Wide search of one anchor (docs 16.11): the patch of A at the anchor, searched
// in B over +-design.global_search_radius_px by the same coarse-to-fine search
// as the local measurement. Two-directional patches only, so the search is
// two-dimensional. Verified when the single strict peak lies at the local
// displacement; the wide window is the part of the search the local radius
// (and its periodicity check) cannot see.
GlobalAnchor CheckGlobal(const Context& c, const SeamPoint& p) {
    const auto& dz = *c.design;
    const int px = static_cast<int>(p.x), py = static_cast<int>(p.y), h = static_cast<int>(dz.patch_px/2);
    const int radius = static_cast<int>(dz.global_search_radius_px);
    GlobalAnchor anchor;
    anchor.x = p.x; anchor.y = p.y; anchor.block_column = p.block_column; anchor.block_row = p.block_row;
    anchor.local = p.displacement;
    const Plane a_region = FeatureRegion(c.a, px, py, h);
    if (HasNaN(a_region)) { anchor.status = GlobalAnchorStatus::window_unavailable; return anchor; }
    const Template ta = MakeTemplate(FeatureOf(a_region, Feature::gradient));
    if (!(ta.norm[0] > 0) || !(ta.norm[2] > 0)) { anchor.status = GlobalAnchorStatus::flat_correlation; return anchor; }
    Plane region = FeatureRegion(c.b, px, py, h+radius);
    if (HasNaN(region)) { anchor.status = GlobalAnchorStatus::window_unavailable; return anchor; }
    Plane feature = FeatureOf(region, Feature::gradient);
    region = Plane{};
    const auto peak = IntegerSearch(ta, Pyramid(std::move(feature)), radius, Direction{}, dz.secondary_peak_ratio);
    switch (peak.status) {
    case PointStatus::valid: {
        anchor.wide = {static_cast<double>(peak.x), static_cast<double>(peak.y)};
        const bool same = std::hypot(anchor.wide.x-p.displacement.x, anchor.wide.y-p.displacement.y) <= dz.global_tolerance_px;
        anchor.status = same ? GlobalAnchorStatus::verified : GlobalAnchorStatus::contradicted;
        break;
    }
    case PointStatus::ambiguous: anchor.status = GlobalAnchorStatus::repeating; break;
    case PointStatus::search_boundary: anchor.status = GlobalAnchorStatus::search_boundary; break;
    default: anchor.status = GlobalAnchorStatus::flat_correlation; break;
    }
    return anchor;
}
// 0..n-1 in an order whose every prefix is spread over the range (bit reversal),
// so a bounded number of evaluations covers the whole band.
std::vector<std::size_t> SpreadOrder(std::size_t n) {
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < n) ++bits;
    std::vector<std::size_t> order;
    order.reserve(n);
    for (std::size_t k = 0; k < (std::size_t{1} << bits); ++k) {
        std::size_t r = 0;
        for (unsigned b = 0; b < bits; ++b) if (k & (std::size_t{1} << b)) r |= std::size_t{1} << (bits-1-b);
        if (r < n) order.push_back(r);
    }
    return order;
}

template <class F> void ParallelFor(std::size_t count, unsigned threads, F&& body) {
    std::atomic<std::size_t> next{0};
    std::exception_ptr failure;
    std::mutex failure_lock;
    const auto work = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= count) return;
            try { body(i); }
            catch (...) { const std::lock_guard lock(failure_lock); if (!failure) failure = std::current_exception(); next = count; return; }
        }
    };
    const unsigned extra = static_cast<unsigned>(std::min<std::size_t>(threads,count ? count : 1))-1;
    std::vector<std::thread> pool;
    pool.reserve(extra);
    for (unsigned t = 0; t < extra; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    if (failure) std::rethrow_exception(failure);
}

void ValidateGeometry(const BgrImage& a, const BgrImage& b, const SeamGateGeometry& g) {
    for (const auto* image : {&a,&b})
        Require(image->bgr.size() == render::PixelCount(image->width,image->height)*3, "seam gate: BGR buffer differs from dimensions");
    Require(g.camera_a && g.camera_b, "seam gate: both output-to-camera mappings are required");
    (void)render::PixelCount(g.width,g.height);
    Require(g.band.size() == g.height, "seam gate: one band row per output row is required");
    for (const auto& row : g.band) Require(row.begin <= row.end && row.end <= g.width, "seam gate: band row outside the output");
    Require(g.transition.empty() || g.transition.size() == g.height, "seam gate: one transition row per output row is required");
    for (const auto& row : g.transition) Require(row.begin <= row.end && row.end <= g.width, "seam gate: transition row outside the output");
}
} // namespace

std::string_view PointStatusCode(PointStatus s) {
    switch (s) {
    case PointStatus::outside_coverage: return "outside-coverage";
    case PointStatus::no_content: return "no-content";
    case PointStatus::search_window_unavailable: return "search-window-unavailable";
    case PointStatus::flat_correlation: return "flat-correlation";
    case PointStatus::ambiguous: return "ambiguous";
    case PointStatus::periodic: return "periodic";
    case PointStatus::search_boundary: return "search-boundary";
    case PointStatus::round_trip_inconsistent: return "round-trip-inconsistent";
    case PointStatus::uncertain: return "uncertain";
    case PointStatus::valid: return "valid";
    }
    throw std::invalid_argument("unknown point status");
}
std::string_view VariantCode(Variant v) {
    switch (v) {
    case Variant::shifted: return "shifted";
    case Variant::enlarged: return "enlarged";
    case Variant::luminance: return "luminance";
    }
    throw std::invalid_argument("unknown variant");
}
std::string_view GlobalAnchorStatusCode(GlobalAnchorStatus s) {
    switch (s) {
    case GlobalAnchorStatus::verified: return "verified";
    case GlobalAnchorStatus::contradicted: return "contradicted";
    case GlobalAnchorStatus::repeating: return "repeating";
    case GlobalAnchorStatus::search_boundary: return "search-boundary";
    case GlobalAnchorStatus::flat_correlation: return "flat-correlation";
    case GlobalAnchorStatus::window_unavailable: return "window-unavailable";
    }
    throw std::invalid_argument("unknown global anchor status");
}
std::string_view BlockClassCode(BlockClass v) {
    switch (v) {
    case BlockClass::outside: return "outside";
    case BlockClass::empty: return "empty";
    case BlockClass::measured: return "measured";
    case BlockClass::unmeasurable_content: return "unmeasurable-content";
    }
    throw std::invalid_argument("unknown block class");
}

void ValidateSeamGateDesign(const SeamGateDesign& d) {
    const auto finite_positive = [](double v) { return std::isfinite(v) && v > 0; };
    Require(d.point_spacing_px >= 4 && d.point_spacing_px <= 1024 && d.point_spacing_px%2 == 0 &&
        d.block_px%d.point_spacing_px == 0 && d.block_px <= 4096 &&
        d.patch_px >= 16 && d.patch_px <= 512 && d.patch_px%8 == 0 &&
        d.remeasure_patch_px >= 16 && d.remeasure_patch_px <= 512 && d.remeasure_patch_px%8 == 0 &&
        d.search_radius_px >= 8 && d.search_radius_px%4 == 0 && d.maximum_search_radius_px >= d.search_radius_px &&
        d.maximum_search_radius_px <= 1024 && d.maximum_search_radius_px%4 == 0 &&
        d.refinement_levels >= 1 && d.refinement_levels <= 8 &&
        finite_positive(d.content_gradient_dn) && d.content_minimum_samples >= 1 &&
        d.one_direction_ratio >= 0 && d.one_direction_ratio < 1 &&
        finite_positive(d.secondary_peak_ratio) && d.secondary_peak_ratio <= 1 &&
        finite_positive(d.round_trip_px) && std::isfinite(d.uncertainty_floor_px) && d.uncertainty_floor_px >= 0 &&
        finite_positive(d.maximum_uncertainty_px) && d.remeasure_shift_px <= 1024 &&
        finite_positive(d.agreement_factor) && finite_positive(d.agreement_floor_px) && finite_positive(d.block_neighbor_px) &&
        d.strip_rows >= d.block_px && d.strip_rows%d.block_px == 0 && d.strip_rows <= 32768,
        "seam gate: invalid design constants");
    // The wide search has to reach past the periodicity check of the local one
    // (2 radii), and its window is bounded for memory.
    Require(d.global_search_radius_px%4 == 0 && d.global_search_radius_px > 2*d.search_radius_px && d.global_search_radius_px <= 1024 &&
        d.global_minimum_anchors >= 1 && d.global_minimum_anchors <= d.global_maximum_evaluations &&
        d.global_maximum_evaluations <= 4096 && std::isfinite(d.global_tolerance_px) && d.global_tolerance_px > 0 &&
        d.global_tolerance_px <= 16,
        "seam gate: invalid global placement constants");
}
std::uint32_t RequiredBandInsetPx(const SeamGateDesign& d) {
    ValidateSeamGateDesign(d);
    const std::uint32_t h = d.patch_px/2, hl = d.remeasure_patch_px/2;
    return std::max({h+2*d.search_radius_px, hl+d.search_radius_px, h+d.search_radius_px})+d.remeasure_shift_px+4+kSmooth;
}

SeamGateGeometry PairSeamGeometry(std::uint32_t aw, std::uint32_t ah, std::uint32_t bw, std::uint32_t bh,
    const render::PairRenderParameters& p) {
    Require(p.layout == StitchLayout::camera_a_left_camera_b_right, "seam gate: only the A-left, B-right layout is supported");
    render::ValidateProjectiveDomain(p.camera_b_to_camera_a, bw, bh);
    const auto inverse = render::Invert(p.camera_b_to_camera_a);
    const auto plan = render::PlanPairCanvas(aw,ah,bw,bh,p);
    const double ox = plan.origin_x+p.crop.left, oy = plan.origin_y+p.crop.top;
    SeamGateGeometry g;
    g.width = plan.output_width; g.height = plan.output_height;
    // render::RenderPair samples A at the global position and B through the
    // inverse transform of the same position.
    g.camera_a = [ox,oy](Point o, Point& raw) { raw = {ox+o.x,oy+o.y}; return true; };
    g.camera_b = [ox,oy,inverse](Point o, Point& raw) { return render::TryTransform(inverse,{ox+o.x,oy+o.y},raw); };
    g.band.assign(g.height,{});
    return g;
}
SeamGateGeometry DocumentSeamGeometry(const render::DocumentRenderParameters& p) {
    Require(p.layout == StitchLayout::camera_a_left_camera_b_right, "seam gate: only the A-left, B-right layout is supported");
    render::ValidateCameraProjection(p.camera_a,p.raster);
    render::ValidateCameraProjection(p.camera_b,p.raster);
    SeamGateGeometry g;
    g.width = p.raster.width_pixels; g.height = p.raster.height_pixels;
    g.camera_a = render::DocumentOutputToCamera(p.raster,p.camera_a);
    g.camera_b = render::DocumentOutputToCamera(p.raster,p.camera_b);
    g.band.assign(g.height,{});
    return g;
}
std::vector<BandRow> OverlapBand(const SeamGateGeometry& g, std::uint32_t aw, std::uint32_t ah, std::uint32_t bw,
    std::uint32_t bh, std::uint32_t inset) {
    Require(g.camera_a && g.camera_b, "seam gate: both output-to-camera mappings are required");
    (void)render::PixelCount(g.width,g.height);
    const auto inside = [](const OutputToCamera& m, double x, double y, std::uint32_t w, std::uint32_t h) {
        Point raw{};
        return Map(m,{x,y},raw) && raw.x >= 0 && raw.y >= 0 && raw.x < w && raw.y < h;
    };
    const auto both = [&](std::uint32_t x, std::uint32_t y) {
        return inside(g.camera_a,x,y,aw,ah) && inside(g.camera_b,x,y,bw,bh);
    };
    // Per row the overlap is one run (convex coverages); found on a coarse
    // scan and refined to the pixel at both ends.
    std::vector<std::int64_t> first(g.height,-1), last(g.height,-1);
    for (std::uint32_t y = 0; y < g.height; ++y) {
        std::int64_t seed = -1;
        for (std::uint32_t x = 0; x < g.width && seed < 0; x += 8) if (both(x,y)) seed = x;
        if (seed < 0) continue;
        std::int64_t lo = seed; while (lo > 0 && both(static_cast<std::uint32_t>(lo-1),y)) --lo;
        std::int64_t hi = seed; while (hi+1 < g.width && both(static_cast<std::uint32_t>(hi+1),y)) ++hi;
        first[y] = lo; last[y] = hi;
    }
    std::vector<BandRow> band(g.height);
    const std::int64_t m = inset;
    for (std::int64_t y = 0; y < g.height; ++y) {
        std::int64_t lo = 0, hi = g.width;
        bool ok = y-m >= 0 && y+m < g.height;
        for (std::int64_t v = y-m; ok && v <= y+m; ++v) {
            if (first[v] < 0) { ok = false; break; }
            lo = std::max(lo,first[v]); hi = std::min(hi,last[v]);
        }
        lo += m; hi -= m;
        if (ok && lo <= hi) band[y] = {static_cast<std::uint32_t>(lo),static_cast<std::uint32_t>(hi+1)};
    }
    return band;
}

std::vector<BandRow> SeamCutTransitionBand(const std::vector<std::uint32_t>& seam_x, std::uint32_t blend_width_px,
    std::uint32_t output_width) {
    Require(blend_width_px >= 1 && blend_width_px <= 4096, "seam gate: blend width must be 1..4096");
    // RenderPairSeamCut mixes where 0 < (x - seam)/width + 0.5 < 1, that is
    // |x - seam| < width/2; whole pixels on both sides, rounded up.
    const std::int64_t half = (static_cast<std::int64_t>(blend_width_px)+1)/2;
    std::vector<BandRow> rows(seam_x.size());
    for (std::size_t y = 0; y < seam_x.size(); ++y) {
        Require(seam_x[y] < output_width, "seam gate: seam column outside the output");
        const std::int64_t begin = std::max<std::int64_t>(0, static_cast<std::int64_t>(seam_x[y])-half);
        const std::int64_t end = std::min<std::int64_t>(output_width, static_cast<std::int64_t>(seam_x[y])+half+1);
        rows[y] = {static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end)};
    }
    return rows;
}

TransitionContainment CheckTransitionBandContained(const std::vector<BandRow>& band, const std::vector<BandRow>& transition,
    std::uint32_t output_width) {
    Require(band.size() == transition.size(), "seam gate: band and transition differ in rows");
    TransitionContainment c;
    c.supplied = true;
    for (std::size_t y = 0; y < transition.size(); ++y) {
        const BandRow t = transition[y], b = band[y];
        Require(t.begin <= t.end && t.end <= output_width && b.begin <= b.end && b.end <= output_width,
            "seam gate: band row outside the output");
        if (t.begin == t.end) continue;
        std::uint64_t outside = 0;
        std::uint32_t first = t.begin;
        if (b.begin == b.end) outside = t.end-t.begin;
        else {
            if (t.begin < b.begin) outside += std::min(t.end, b.begin)-t.begin;
            if (t.end > b.end) {
                const std::uint32_t from = std::max(t.begin, b.end);
                if (!outside) first = from;
                outside += t.end-from;
            }
        }
        if (!outside) continue;
        if (!c.pixels_outside) { c.first_x = first; c.first_y = static_cast<std::uint32_t>(y); }
        ++c.rows_outside;
        c.pixels_outside += outside;
    }
    return c;
}

SeamMeasurement MeasureSeam(const BgrImage& image_a, const BgrImage& image_b, const SeamGateGeometry& g,
    const SeamGateDesign& dz, const SeamGateLimits& limits, const DisplacementPrediction& prediction, unsigned threads) {
    ValidateSeamGateDesign(dz);
    ValidateGeometry(image_a,image_b,g);
    Require(std::isfinite(limits.limit_px) && limits.limit_px > 0 && std::isfinite(limits.guard_band_px) &&
        limits.guard_band_px >= 0 && limits.guard_band_px < limits.limit_px, "seam gate: invalid limits");
    Require(threads >= 1 && threads <= 256, "seam gate: thread count must be 1..256");
    SeamMeasurement m;
    m.design = dz; m.limits = limits; m.width = g.width; m.height = g.height;
    m.prediction_supplied = static_cast<bool>(prediction);
    m.global.radius_px = dz.global_search_radius_px;
    if (!g.transition.empty()) m.containment = CheckTransitionBandContained(g.band,g.transition,g.width);
    std::uint32_t y0 = g.height, y1 = 0, x0 = g.width, x1 = 0;
    for (std::uint32_t y = 0; y < g.height; ++y) if (g.band[y].begin < g.band[y].end) {
        y0 = std::min(y0,y); y1 = y+1; x0 = std::min(x0,g.band[y].begin); x1 = std::max(x1,g.band[y].end);
    }
    if (y0 >= y1) return m;
    const std::uint32_t spacing = dz.point_spacing_px, block = dz.block_px;
    m.origin_x = x0; m.origin_y = y0;
    m.block_columns = (x1-x0+block-1)/block; m.block_rows = (y1-y0+block-1)/block;
    for (std::uint32_t y = y0+spacing/2; y < y1; y += spacing) {
        const auto& row = g.band[y];
        for (std::uint32_t x = x0+spacing/2; x < x1; x += spacing) if (x >= row.begin && x < row.end) {
            SeamPoint p; p.x = x; p.y = y; p.block_column = (x-x0)/block; p.block_row = (y-y0)/block;
            m.points.push_back(p);
        }
    }
    // Raw crops: everything any search (up to the widest radius) can reach.
    const int reach = static_cast<int>(std::max(std::max(dz.patch_px,dz.remeasure_patch_px)/2+2*dz.maximum_search_radius_px+dz.remeasure_shift_px,
        dz.patch_px/2+dz.global_search_radius_px)+8);
    Context c; c.design = &dz;
    c.a.map = &g.camera_a; c.b.map = &g.camera_b;
    for (auto* camera : {&c.a,&c.b}) {
        const BgrImage& image = camera == &c.a ? image_a : image_b;
        double lo_x = std::numeric_limits<double>::infinity(), lo_y = lo_x, hi_x = -lo_x, hi_y = -lo_x;
        const int ox0 = static_cast<int>(x0)-reach, ox1 = static_cast<int>(x1)+reach, oy0 = static_cast<int>(y0)-reach, oy1 = static_cast<int>(y1)+reach;
        for (int y = oy0; y <= oy1+15; y += 16) for (int x = ox0; x <= ox1+15; x += 16) {
            Point raw{};
            if (!Map(*camera->map,{static_cast<double>(std::min(x,ox1)),static_cast<double>(std::min(y,oy1))},raw)) continue;
            lo_x = std::min(lo_x,raw.x); hi_x = std::max(hi_x,raw.x); lo_y = std::min(lo_y,raw.y); hi_y = std::max(hi_y,raw.y);
        }
        if (!(lo_x <= hi_x)) camera->luma = MakeRawLuma(image,0,0,0,0);
        else {
            const auto clampi = [](double v) { return static_cast<int>(std::clamp(v,-1e9,1e9)); };
            camera->luma = MakeRawLuma(image,clampi(std::floor(lo_x))-48,clampi(std::floor(lo_y))-48,clampi(std::ceil(hi_x))+48,clampi(std::ceil(hi_y))+48);
        }
    }
    // Strips: output-grid luminance once per strip, reused by every point in it.
    const int margin = static_cast<int>(std::max(dz.patch_px/2+2*dz.search_radius_px,dz.remeasure_patch_px/2+dz.search_radius_px)+
        dz.search_radius_px+dz.remeasure_shift_px)+6+kSmooth;
    std::size_t begin = 0;
    while (begin < m.points.size()) {
        const std::uint32_t strip = (m.points[begin].y-y0)/dz.strip_rows;
        std::size_t end = begin;
        int px0 = std::numeric_limits<int>::max(), px1 = std::numeric_limits<int>::min(), py0 = px0, py1 = px1;
        while (end < m.points.size() && (m.points[end].y-y0)/dz.strip_rows == strip) {
            px0 = std::min(px0,static_cast<int>(m.points[end].x)); px1 = std::max(px1,static_cast<int>(m.points[end].x));
            py0 = std::min(py0,static_cast<int>(m.points[end].y)); py1 = std::max(py1,static_cast<int>(m.points[end].y));
            ++end;
        }
        const int sx0 = px0-margin, sy0 = py0-margin, sw = px1-px0+2*margin+2, sh = py1-py0+2*margin+2;
        for (auto* camera : {&c.a,&c.b}) {
            camera->strip = Plane{sw,sh,std::vector<double>(static_cast<std::size_t>(sw)*sh)};
            camera->strip_x0 = sx0; camera->strip_y0 = sy0;
            ParallelFor(static_cast<std::size_t>(sh),threads,[&](std::size_t row) {
                double* const out = camera->strip.v.data()+row*sw;
                for (int x = 0; x < sw; ++x) out[x] = SampleOutput(*camera,{static_cast<double>(sx0+x),static_cast<double>(sy0+static_cast<int>(row))});
            });
        }
        ParallelFor(end-begin,threads,[&](std::size_t k) {
            auto& p = m.points[begin+k];
            const int px = static_cast<int>(p.x), py = static_cast<int>(p.y), h = static_cast<int>(dz.patch_px/2);
            const auto base = MeasureAt(c,px,py,h,Feature::gradient,true);
            p.content = base.content; p.status = base.status; p.kind = base.kind; p.normal = base.normal; p.displacement = base.d;
            p.visible_px = base.visible; p.uncertainty_px = base.u; p.round_trip_px = base.rt; p.correlation = base.correlation;
            p.search_radius_px = static_cast<std::uint32_t>(base.radius);
            if (base.status != PointStatus::valid || !(base.visible+limits.guard_band_px > limits.limit_px)) return;
            // Over the line: confirmed only when two of three variants agree.
            const Comparable reference{base.kind,base.normal,base.d,base.u};
            int agreeing = 0;
            // The larger patch is the most work and goes last; the loop stops once
            // the outcome is fixed (two agree, or two do not).
            for (const auto variant : {Variant::shifted,Variant::luminance,Variant::enlarged}) {
                Single s;
                if (variant == Variant::shifted) {
                    const int shift = static_cast<int>(dz.remeasure_shift_px);
                    s = MeasureAt(c,px,py+shift,h,Feature::gradient,false);
                    if (s.status != PointStatus::valid) s = MeasureAt(c,px,py-shift,h,Feature::gradient,false);
                } else if (variant == Variant::enlarged) s = MeasureAt(c,px,py,static_cast<int>(dz.remeasure_patch_px/2),Feature::gradient,false);
                else s = MeasureAt(c,px,py,h,Feature::luminance,false);
                Remeasurement r{variant,s.status,s.kind,s.normal,s.d,s.u,false};
                r.agrees = s.status == PointStatus::valid && Agree(reference,{s.kind,s.normal,s.d,s.u},dz);
                if (r.agrees) ++agreeing;
                p.remeasurements.push_back(r);
                if (agreeing >= 2 || static_cast<int>(p.remeasurements.size())-agreeing >= 2) break;
            }
            p.confirmation = agreeing >= 2 ? Confirmation::confirmed : Confirmation::unconfirmed;
        });
        begin = end;
    }
    for (auto* camera : {&c.a,&c.b}) camera->strip = {};

    // Blocks.
    m.blocks.resize(static_cast<std::size_t>(m.block_columns)*m.block_rows);
    for (std::uint32_t r = 0; r < m.block_rows; ++r) for (std::uint32_t col = 0; col < m.block_columns; ++col) {
        auto& b = m.blocks[static_cast<std::size_t>(r)*m.block_columns+col];
        b.column = col; b.row = r; b.x = x0+col*block; b.y = y0+r*block;
        b.width = std::min(block,x1-b.x); b.height = std::min(block,y1-b.y);
    }
    std::vector<std::vector<std::size_t>> members(m.blocks.size());
    for (std::size_t i = 0; i < m.points.size(); ++i)
        members[static_cast<std::size_t>(m.points[i].block_row)*m.block_columns+m.points[i].block_column].push_back(i);
    std::vector<std::optional<Point>> representative(m.blocks.size());
    for (std::size_t bi = 0; bi < m.blocks.size(); ++bi) {
        auto& b = m.blocks[bi];
        std::vector<std::size_t> valid;
        bool confirmed = false;
        Point sum{}; int two = 0;
        for (const auto i : members[bi]) {
            const auto& p = m.points[i];
            if (p.status == PointStatus::outside_coverage) continue;
            ++b.points;
            if (p.content) ++b.content_points;
            if (p.status != PointStatus::valid) continue;
            ++b.valid_points;
            if (p.confirmation == Confirmation::unconfirmed) { ++b.unconfirmed_points; continue; }
            valid.push_back(i);
            ++b.counted_points;
            if (p.confirmation == Confirmation::confirmed) { ++b.confirmed_points; confirmed = true; }
            b.maximum_px = std::max(b.maximum_px.value_or(0),p.visible_px);
            b.uncertainty_px = std::max(b.uncertainty_px.value_or(0),p.uncertainty_px);
            if (p.kind == PointKind::two_directional) { sum.x += p.displacement.x; sum.y += p.displacement.y; ++two; }
        }
        bool pair = false;
        for (std::size_t i = 0; i < valid.size() && !pair; ++i) for (std::size_t j = i+1; j < valid.size() && !pair; ++j) {
            const auto& p = m.points[valid[i]]; const auto& q = m.points[valid[j]];
            pair = Agree({p.kind,p.normal,p.displacement,p.uncertainty_px},{q.kind,q.normal,q.displacement,q.uncertainty_px},dz);
        }
        if (!b.points) b.block_class = BlockClass::outside;
        // #385 Q1 (owner decision, #385): a point over the line that
        // remeasurement left unconfirmed makes the block unmeasurable, whatever
        // else the block holds.
        else if (b.unconfirmed_points) b.block_class = BlockClass::unmeasurable_content;
        else if (pair || confirmed) b.block_class = BlockClass::measured;
        else if (b.content_points) b.block_class = BlockClass::unmeasurable_content;
        else b.block_class = BlockClass::empty;
        if (b.block_class == BlockClass::measured && two) representative[bi] = Point{sum.x/two,sum.y/two};
    }
    // A measured block whose value differs from every measured neighbour by
    // more than the paper can bend is not counted as measured; its points
    // stay in the maximum.
    std::vector<char> inconsistent(m.blocks.size());
    for (std::size_t bi = 0; bi < m.blocks.size(); ++bi) {
        if (!representative[bi]) continue;
        const auto& b = m.blocks[bi];
        int neighbours = 0, close = 0;
        const int dx[4]{1,-1,0,0}, dy[4]{0,0,1,-1};
        for (int k = 0; k < 4; ++k) {
            const long cx = static_cast<long>(b.column)+dx[k], cy = static_cast<long>(b.row)+dy[k];
            if (cx < 0 || cy < 0 || cx >= static_cast<long>(m.block_columns) || cy >= static_cast<long>(m.block_rows)) continue;
            const auto& other = representative[static_cast<std::size_t>(cy)*m.block_columns+static_cast<std::size_t>(cx)];
            if (!other) continue;
            ++neighbours;
            if (std::hypot(other->x-representative[bi]->x,other->y-representative[bi]->y) <= dz.block_neighbor_px) ++close;
        }
        inconsistent[bi] = neighbours > 0 && close == 0;
    }
    for (std::size_t bi = 0; bi < m.blocks.size(); ++bi) if (inconsistent[bi]) {
        m.blocks[bi].neighbor_inconsistent = true;
        m.blocks[bi].block_class = BlockClass::unmeasurable_content;
    }
    // Calibration prediction for blocks without content (#385 D1).
    if (prediction) {
        for (const auto& p : m.points) {
            if (!p.Counted()) continue;
            Point predicted{};
            if (!prediction({static_cast<double>(p.x),static_cast<double>(p.y)},predicted)) continue;
            Require(std::isfinite(predicted.x) && std::isfinite(predicted.y), "seam gate: prediction is not finite");
            const Point diff{p.displacement.x-predicted.x,p.displacement.y-predicted.y};
            const double miss = p.kind == PointKind::one_directional ? std::abs(p.normal.x*diff.x+p.normal.y*diff.y) : std::hypot(diff.x,diff.y);
            m.prediction_mismatch_px = std::max(m.prediction_mismatch_px.value_or(0),miss);
        }
        for (auto& b : m.blocks) {
            if (b.block_class != BlockClass::empty) continue;
            Point predicted{};
            if (!prediction({b.x+b.width/2.0,b.y+b.height/2.0},predicted)) continue;
            Require(std::isfinite(predicted.x) && std::isfinite(predicted.y), "seam gate: prediction is not finite");
            b.predicted_px = std::hypot(predicted.x,predicted.y)+m.prediction_mismatch_px.value_or(0);
        }
    }
    // Global placement (#392): the local search cannot tell a placement that
    // is one period of a repeating pattern off. Measured blocks that hold a
    // two-directional counted point are anchors; each is searched again over
    // the wide radius. Anchors are taken in an order whose prefixes are spread
    // over the band, at most global_maximum_evaluations of them.
    m.global.performed = true;
    {
        std::vector<std::size_t> candidates;
        for (std::size_t bi = 0; bi < m.blocks.size(); ++bi) {
            if (m.blocks[bi].block_class != BlockClass::measured) continue;
            std::optional<std::size_t> best;
            for (const auto i : members[bi]) {
                const auto& p = m.points[i];
                if (!p.Counted() || p.status != PointStatus::valid || p.kind != PointKind::two_directional) continue;
                if (!best || p.uncertainty_px < m.points[*best].uncertainty_px) best = i;
            }
            if (best) candidates.push_back(*best);
        }
        m.global.candidates = static_cast<std::uint32_t>(candidates.size());
        auto order = SpreadOrder(candidates.size());
        order.resize(std::min<std::size_t>(order.size(),dz.global_maximum_evaluations));
        m.global.anchors.resize(order.size());
        ParallelFor(order.size(),std::min(threads,kGlobalWorkers),[&](std::size_t k) {
            m.global.anchors[k] = CheckGlobal(c,m.points[candidates[order[k]]]);
        });
        m.global.evaluated = static_cast<std::uint32_t>(order.size());
        for (const auto& a : m.global.anchors) {
            if (a.status == GlobalAnchorStatus::verified) ++m.global.verified;
            else if (a.status == GlobalAnchorStatus::contradicted) ++m.global.contradicted;
            else ++m.global.indeterminate;
        }
    }
    for (auto* camera : {&c.a,&c.b}) camera->luma = {};
    return m;
}

} // namespace a0::m2::seam_gate
