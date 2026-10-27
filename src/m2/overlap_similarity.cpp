#include "a0/m2/experimental/overlap_estimation.hpp"
#include "a0/m2/experimental/review_alignment.hpp"
#include "overlap_estimation_detail.hpp"
#include "zero_mean_correlation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace a0::m2::experimental {
namespace {
using correlation_detail::Center;
using correlation_detail::WindowCorrelation;
using overlap_detail::Gradient;
using overlap_detail::Half;
using overlap_detail::Plane;
// Design constants of the tile search, not acceptance values. Levels halve;
// their number is chosen so the coarsest level spans about kTopWidth pixels.
constexpr double kTopWidth = 920;
constexpr int kTopTemplate = 32;          // tile side at the coarsest level
constexpr int kTopRadius = 25;            // offset range at the coarsest level, either sign
constexpr int kColumns = 4, kRows = 10;   // tile grid over the overlap band, at most
constexpr std::size_t kPeaks = 8;         // local maxima carried down from the top level
constexpr std::size_t kFinePeaks = 2;     // carried through the two finest levels
constexpr int kRefineRadius = 2;          // per-level search around 2x the coarser position
constexpr unsigned kWideSide = 640;       // input limit of the wide translation search
constexpr std::size_t kWideStarts = 2;    // wide-search candidates tried as extra starts
// Statistical constants of the robust fit (Gaussian consistency of the median
// and the usual reweighting cut), not pixel tolerances or acceptance values.
constexpr double kGaussianScale = 1.4826, kInlierCut = 2.5;
constexpr int kRefits = 10;               // cap on reweighted refits; the loop stops when the set repeats

struct Peak {
    int u{}, v{};
    double r{};
    bool tied{}, edge{}, boundary{};
};
struct Centered { std::vector<double> t; double norm{}; };
struct Located { double u{}, v{}; int iu{}, iv{}; };

Plane LumaRegion(const render::BgrImage& image, int x0, int y0, int w, int h) {
    Plane p{w,h,{}};
    p.v.resize(static_cast<std::size_t>(w)*h);
    for (int y = 0; y < h; ++y) {
        const auto* row = image.bgr.data()+(static_cast<std::size_t>(y0+y)*image.width+x0)*3;
        double* out = p.v.data()+static_cast<std::size_t>(y)*w;
        for (int x = 0; x < w; ++x) out[x] = row[x*3]*0.114+row[x*3+1]*0.587+row[x*3+2]*0.299;
    }
    return p;
}
std::vector<Plane> Pyramid(const Plane& luma, int levels) {
    std::vector<Plane> p{Gradient(luma)};
    while (static_cast<int>(p.size()) < levels) p.push_back(Half(p.back()));
    return p;
}
// Two non-zero eigenvalues of the summed gradient outer product: the texture
// constrains both directions. No magnitude threshold is applied.
bool TwoDimensional(const Plane& l) {
    double xx = 0, yy = 0, xy = 0;
    for (int y = 0; y+1 < l.height; ++y) {
        const double* const row = l.v.data()+static_cast<std::size_t>(y)*l.width; const double* const below = row+l.width;
        for (int x = 0; x+1 < l.width; ++x) {
            const double base = row[x], gx = row[x+1]-base, gy = below[x]-base;
            xx += gx*gx; yy += gy*gy; xy += gx*gy;
        }
    }
    return xx*yy-xy*xy > 0;
}
bool ScoreAt(const Plane& region, const Plane& templ, const Centered& t, int radius, int u, int v, double& r) {
    return WindowCorrelation(t.t,t.norm,static_cast<std::size_t>(templ.width),static_cast<std::size_t>(templ.height),
        region.v.data()+static_cast<std::size_t>(radius+v)*region.width+(radius+u),static_cast<std::size_t>(region.width),r);
}
bool Before(const Peak& p, const Peak& q) {
    if (p.r != q.r) return p.r > q.r;
    return p.v != q.v ? p.v < q.v : p.u < q.u;
}
// Template offset inside a region that extends `radius` full-resolution
// pixels on every side; offset 0 is the template's expected position. Same
// peak handling as the translation search: distinct peaks, best first.
std::optional<Located> Locate(const std::vector<Plane>& templ, const std::vector<Plane>& region, int radius) {
    const int levels = static_cast<int>(templ.size()), top = levels-1;
    std::vector<Centered> centered(templ.size());
    for (std::size_t l = 0; l < templ.size(); ++l) {
        centered[l].t = templ[l].v; centered[l].norm = Center(centered[l].t);
        if (!(centered[l].norm > 0)) return {};
    }
    const int rt = radius>>top, side = 2*rt+1;
    std::vector<Peak> grid(static_cast<std::size_t>(side)*side);
    std::vector<char> valid(grid.size());
    for (int v = -rt; v <= rt; ++v) for (int u = -rt; u <= rt; ++u) {
        const auto k = static_cast<std::size_t>(v+rt)*side+(u+rt);
        double r{}; valid[k] = ScoreAt(region[top],templ[top],centered[top],rt,u,v,r); grid[k] = {u,v,r};
    }
    std::vector<Peak> peaks;
    for (int j = 0; j < side; ++j) for (int i = 0; i < side; ++i) {
        const auto k = static_cast<std::size_t>(j)*side+i;
        if (!valid[k]) continue;
        bool maximum = true, tied = false;
        for (int dv = -1; dv <= 1; ++dv) for (int du = -1; du <= 1; ++du) {
            const int ii = i+du, jj = j+dv;
            if ((!du && !dv) || ii < 0 || jj < 0 || ii >= side || jj >= side) continue;
            const auto q = static_cast<std::size_t>(jj)*side+ii;
            if (!valid[q]) continue;
            if (grid[q].r > grid[k].r) maximum = false; else if (grid[q].r == grid[k].r) tied = true;
        }
        if (!maximum) continue;
        auto p = grid[k];
        p.tied = tied;
        p.boundary = top == 0 && (std::abs(p.u) == rt || std::abs(p.v) == rt);
        peaks.push_back(p);
    }
    std::sort(peaks.begin(),peaks.end(),Before);
    if (peaks.size() > kPeaks) peaks.resize(kPeaks);
    for (int level = top-1; level >= 0; --level) {
        if (level <= 1 && peaks.size() > kFinePeaks) peaks.resize(kFinePeaks);
        const int rl = radius>>level;
        std::vector<Peak> next;
        for (const auto& p : peaks) {
            const int lu = std::max(-rl,2*p.u-kRefineRadius), hu = std::min(rl,2*p.u+kRefineRadius);
            const int lv = std::max(-rl,2*p.v-kRefineRadius), hv = std::min(rl,2*p.v+kRefineRadius);
            Peak best{};
            bool found = false, tied = false;
            for (int v = lv; v <= hv; ++v) for (int u = lu; u <= hu; ++u) {
                double r{};
                if (!ScoreAt(region[level],templ[level],centered[level],rl,u,v,r)) continue;
                if (!found || r > best.r) { best = {u,v,r}; found = true; tied = false; }
                else if (r == best.r) tied = true;
            }
            if (!found) continue;
            best.tied = tied;
            best.edge = p.edge || (best.u == lu && lu != -rl) || (best.u == hu && hu != rl) ||
                (best.v == lv && lv != -rl) || (best.v == hv && hv != rl);
            best.boundary = level == 0 && (std::abs(best.u) == rl || std::abs(best.v) == rl);
            next.push_back(best);
        }
        std::sort(next.begin(),next.end(),Before);
        peaks.clear();
        for (const auto& p : next) {
            const auto same = std::find_if(peaks.begin(),peaks.end(),[&](const Peak& q){ return q.u == p.u && q.v == p.v; });
            if (same == peaks.end()) peaks.push_back(p);
            else { same->tied = same->tied || p.tied; same->edge = same->edge || p.edge; }
        }
    }
    if (peaks.empty() || peaks[0].tied || peaks[0].edge || peaks[0].boundary || (peaks.size() > 1 && peaks[1].r == peaks[0].r))
        return {};
    // Fraction from a parabola through the strict best and its four
    // neighbours, which lie inside the range because the best is not on it.
    const auto& best = peaks[0];
    double l{}, r{}, t{}, b{};
    if (!ScoreAt(region[0],templ[0],centered[0],radius,best.u-1,best.v,l) || !ScoreAt(region[0],templ[0],centered[0],radius,best.u+1,best.v,r) ||
        !ScoreAt(region[0],templ[0],centered[0],radius,best.u,best.v-1,t) || !ScoreAt(region[0],templ[0],centered[0],radius,best.u,best.v+1,b))
        return {};
    const double dx = l-2*best.r+r, dy = t-2*best.r+b;
    if (!(dx < 0) || !(dy < 0)) return {};
    return Located{best.u+(l-r)/(2*dx),best.v+(t-b)/(2*dy),best.u,best.v};
}
struct Correspondence { double ax, ay, bx, by; };
FitResidual Residual(const std::vector<Correspondence>& c, const render::Matrix3& m) {
    double sum = 0, maximum = 0;
    for (const auto& p : c) {
        const double ex = p.ax-(m[0]*p.bx+m[1]*p.by+m[2]), ey = p.ay-(m[3]*p.bx+m[4]*p.by+m[5]);
        const double d = std::sqrt(ex*ex+ey*ey); sum += d*d; maximum = std::max(maximum,d);
    }
    return {std::sqrt(sum/static_cast<double>(c.size())),maximum};
}
// Least squares a = [p -q; q p](b - c) + c + (x, y), c the image center.
struct Linear {
    double p{}, q{}, x{}, y{};
    render::Matrix3 Matrix(double cx, double cy) const { return {p,-q,x+cx-p*cx+q*cy,q,p,y+cy-q*cx-p*cy,0,0,1}; }
};
Linear FitSimilarity(const std::vector<Correspondence>& fit, double cx, double cy) {
    const double n = static_cast<double>(fit.size());
    double mbx = 0, mby = 0, mean_ax = 0, mean_ay = 0;
    for (const auto& c : fit) { mbx += c.bx-cx; mby += c.by-cy; mean_ax += c.ax; mean_ay += c.ay; }
    mbx /= n; mby /= n; mean_ax /= n; mean_ay /= n;
    double norm = 0, p = 0, q = 0;
    for (const auto& c : fit) {
        const double bx = c.bx-cx-mbx, by = c.by-cy-mby, ax = c.ax-mean_ax, ay = c.ay-mean_ay;
        norm += bx*bx+by*by; p += bx*ax+by*ay; q += bx*ay-by*ax;
    }
    p /= norm; q /= norm;
    return {p,q,mean_ax-cx-(p*mbx-q*mby),mean_ay-cy-(q*mbx+p*mby)};
}
// Scale fixed to 1: the similarity's angle (atan2) is kept and only the
// translation is solved again, t = mean(a) - c - R (mean(b) - c).
Linear FitRigid(const std::vector<Correspondence>& fit, double cx, double cy, double radians) {
    const double n = static_cast<double>(fit.size()), p = std::cos(radians), q = std::sin(radians);
    double mbx = 0, mby = 0, mean_ax = 0, mean_ay = 0;
    for (const auto& c : fit) { mbx += c.bx-cx; mby += c.by-cy; mean_ax += c.ax; mean_ay += c.ay; }
    mbx /= n; mby /= n; mean_ax /= n; mean_ay /= n;
    return {p,q,mean_ax-cx-(p*mbx-q*mby),mean_ay-cy-(q*mbx+p*mby)};
}
// One attempt from one start. Tiles are placed so both search regions stay
// inside both frames; the grid is row-major and fixed by the start.
OverlapSimilarityEstimate Attempt(const render::BgrImage& a, const render::BgrImage& b, int px, int py) {
    OverlapSimilarityEstimate result;
    const int width = static_cast<int>(a.width), height = static_cast<int>(a.height);
    const int levels = 1+std::max(0,static_cast<int>(std::lround(std::log2(width/kTopWidth))));
    const int unit = 1<<(levels-1), side = kTopTemplate*unit, radius = kTopRadius*unit;
    result.levels = static_cast<unsigned>(levels); result.tile_side = static_cast<unsigned>(side); result.search_radius = static_cast<unsigned>(radius);
    const int x_lo = std::max(radius,px+radius), x_hi = std::min(width,width+px)-side-radius-1;
    const int y_lo = std::max(radius,py+radius), y_hi = std::min(height,height+py)-side-radius-1;
    if (x_hi < x_lo || y_hi < y_lo) { result.reason = SimilarityUndeterminedReason::no_tiles; return result; }
    const int columns = std::min(kColumns,1+(x_hi-x_lo)/side), rows = std::min(kRows,1+(y_hi-y_lo)/side);
    const auto spread = [](int lo, int hi, int n, int i) { return n > 1 ? lo+static_cast<int>(static_cast<long long>(hi-lo)*i/(n-1)) : lo+(hi-lo)/2; };
    std::vector<Correspondence> matched;
    std::vector<std::size_t> matched_tiles;
    for (int row = 0; row < rows; ++row) for (int column = 0; column < columns; ++column) {
        const int ax = spread(x_lo,x_hi,columns,column), ay = spread(y_lo,y_hi,rows,row);
        SimilarityTile tile{ax+side/2.0,ay+side/2.0,0,0,static_cast<unsigned>(row),static_cast<unsigned>(column),false};
        const auto a_luma = LumaRegion(a,ax,ay,side+1,side+1);
        std::optional<Located> forward;
        if (TwoDimensional(a_luma))
            forward = Locate(Pyramid(a_luma,levels),Pyramid(LumaRegion(b,ax-px-radius,ay-py-radius,side+2*radius+1,side+2*radius+1),levels),radius);
        if (forward) {
            // Round trip: B's tile at the found integer position, searched
            // back over A around the A tile, must land on offset zero.
            const int bx = ax-px+forward->iu, by = ay-py+forward->iv;
            const auto reverse = Locate(Pyramid(LumaRegion(b,bx,by,side+1,side+1),levels),
                Pyramid(LumaRegion(a,ax-radius,ay-radius,side+2*radius+1,side+2*radius+1),levels),radius);
            if (reverse && !reverse->iu && !reverse->iv) {
                // Gradient sample i sits on the corner between pixels i and
                // i+1 in both frames, so both centers move by the same half pixel.
                tile.a_x += 0.5; tile.a_y += 0.5;
                tile.b_x = tile.a_x-px+forward->u; tile.b_y = tile.a_y-py+forward->v; tile.matched = true;
                matched.push_back({tile.a_x,tile.a_y,tile.b_x,tile.b_y}); matched_tiles.push_back(result.tiles.size());
            }
        }
        result.tiles.push_back(tile);
    }
    result.matched = static_cast<unsigned>(matched.size());
    const double cx = width/2.0, cy = height/2.0;
    // Consensus without a pixel tolerance: the similarity through each tile
    // pair (two points fix it exactly) with the least median squared residual
    // over all matched tiles, then the tiles within kInlierCut robust standard
    // deviations of it. A tile repeated by a periodic pattern (desk grid) can
    // pass the round trip at a wrong period; it falls outside here.
    std::vector<std::size_t> agreed;
    if (matched.size() >= 3) {
        double best = std::numeric_limits<double>::infinity();
        render::Matrix3 chosen{};
        std::vector<double> squares(matched.size()), sorted;
        const auto residuals = [&](const render::Matrix3& m) {
            for (std::size_t k = 0; k < matched.size(); ++k) {
                const auto& c = matched[k];
                const double ex = c.ax-(m[0]*c.bx+m[1]*c.by+m[2]), ey = c.ay-(m[3]*c.bx+m[4]*c.by+m[5]);
                squares[k] = ex*ex+ey*ey;
            }
        };
        for (std::size_t i = 0; i < matched.size(); ++i) for (std::size_t j = i+1; j < matched.size(); ++j) {
            const double bx = matched[j].bx-matched[i].bx, by = matched[j].by-matched[i].by, ax = matched[j].ax-matched[i].ax, ay = matched[j].ay-matched[i].ay;
            const double d = bx*bx+by*by;
            if (!(d > 0)) continue;
            const double p = (ax*bx+ay*by)/d, q = (ay*bx-ax*by)/d;
            const render::Matrix3 m{p,-q,matched[i].ax-p*matched[i].bx+q*matched[i].by,q,p,matched[i].ay-q*matched[i].bx-p*matched[i].by,0,0,1};
            residuals(m);
            sorted = squares;
            std::nth_element(sorted.begin(),sorted.begin()+static_cast<std::ptrdiff_t>(sorted.size()/2),sorted.end());
            if (sorted[sorted.size()/2] < best) { best = sorted[sorted.size()/2]; chosen = m; }
        }
        const auto within = [&](const render::Matrix3& m) {
            residuals(m); sorted = squares;
            std::nth_element(sorted.begin(),sorted.begin()+static_cast<std::ptrdiff_t>(sorted.size()/2),sorted.end());
            const double cut = kInlierCut*kGaussianScale*std::sqrt(sorted[sorted.size()/2]);
            std::vector<std::size_t> inside;
            for (std::size_t k = 0; k < matched.size(); ++k) if (squares[k] <= cut*cut) inside.push_back(k);
            return inside;
        };
        if (std::isfinite(best)) agreed = within(chosen);
        // Reweighted refits: the spread is estimated again from the refitted
        // residuals, so a wide first spread (lens distortion near B's edge)
        // does not keep wrong tiles. Stops when the agreed set repeats.
        for (int k = 0; k < kRefits && agreed.size() >= 3; ++k) {
            std::vector<Correspondence> subset;
            for (const auto i : agreed) subset.push_back(matched[i]);
            auto next = within(FitSimilarity(subset,cx,cy).Matrix(cx,cy));
            if (next == agreed || next.size() < 3) break;
            agreed = std::move(next);
        }
    }
    std::vector<char> agreed_rows(static_cast<std::size_t>(rows));
    std::vector<Correspondence> fit;
    for (const auto k : agreed) {
        fit.push_back(matched[k]); auto& tile = result.tiles[matched_tiles[k]];
        tile.agreed = true; agreed_rows[tile.row] = 1;
    }
    result.agreed = static_cast<unsigned>(fit.size());
    // At least three tiles (a residual exists) over two rows (the long axis
    // constrains rotation and scale).
    if (fit.size() < 3 || std::count(agreed_rows.begin(),agreed_rows.end(),1) < 2) {
        result.reason = SimilarityUndeterminedReason::insufficient_tiles; return result;
    }
    const auto s = FitSimilarity(fit,cx,cy);
    const double x = s.x, y = s.y, n = static_cast<double>(fit.size());
    double tx = 0, ty = 0;
    for (const auto& c : fit) { tx += c.ax-c.bx; ty += c.ay-c.by; }
    tx /= n; ty /= n;
    const double radians = std::atan2(s.q,s.p);
    const auto rigid = FitRigid(fit,cx,cy,radians);
    result.similarity = {x,y,radians*180/std::numbers::pi,std::hypot(s.p,s.q)};
    result.rigid = {rigid.x,rigid.y,result.similarity.rotation_degrees,1};
    result.translation_x = tx; result.translation_y = ty;
    result.similarity_residual = Residual(fit,s.Matrix(cx,cy));
    result.rigid_residual = Residual(fit,rigid.Matrix(cx,cy));
    result.translation_residual = Residual(fit,{1,0,tx,0,1,ty,0,0,1});
    // Every fit the caller may apply must render with integer B-center positions.
    try {
        for (const auto& [fx,fy,degrees,scale] : {std::tuple{x,y,result.similarity.rotation_degrees,result.similarity.scale},
                 std::tuple{rigid.x,rigid.y,result.rigid.rotation_degrees,1.0},std::tuple{tx,ty,0.0,1.0}}) {
            const auto ix = std::llround(fx), iy = std::llround(fy);
            if (ix <= 0 || ix >= width || iy <= -height || iy >= height) throw std::invalid_argument("outside");
            (void)SimilarityReviewGeometry(static_cast<int>(ix),static_cast<int>(iy),degrees,scale,a.width,a.height);
        }
    } catch (const std::invalid_argument&) { result.reason = SimilarityUndeterminedReason::outside_image; return result; }
    result.status = OverlapEstimateStatus::estimated_not_judged;
    result.reason = SimilarityUndeterminedReason::none;
    return result;
}
// Extra starts from the wide translation search on a box-reduced copy: its
// best distinct candidates, applicable or not. Box cell i covers [ik,ik+k),
// so offsets scale by k.
std::vector<std::array<int,2>> WideStarts(const render::BgrImage& a, const render::BgrImage& b) {
    const unsigned k = (std::max(a.width,a.height)+kWideSide-1)/kWideSide, w = a.width/k, h = a.height/k;
    if (w < 8 || h < 8) return {};
    const auto reduce = [&](const render::BgrImage& s) {
        render::BgrImage o{w,h,std::vector<std::uint8_t>(static_cast<std::size_t>(w)*h*3)};
        for (unsigned j = 0; j < h; ++j) for (unsigned i = 0; i < w; ++i) for (unsigned c = 0; c < 3; ++c) {
            unsigned sum = 0;
            for (unsigned v = 0; v < k; ++v) for (unsigned u = 0; u < k; ++u) sum += s.bgr[(static_cast<std::size_t>(j*k+v)*s.width+i*k+u)*3+c];
            o.bgr[(static_cast<std::size_t>(j)*w+i)*3+c] = static_cast<std::uint8_t>((sum+k*k/2)/(k*k));
        }
        return o;
    };
    const auto estimate = EstimateOverlapTranslation(reduce(a),reduce(b));
    std::vector<std::array<int,2>> starts;
    for (const auto& c : estimate.candidates) {
        if (starts.size() == kWideStarts) break;
        starts.push_back({static_cast<int>((w-c.overlap_pixels)*k),c.vertical_pixels*static_cast<int>(k)});
    }
    return starts;
}
// More agreeing tiles first, then the smaller similarity residual.
bool Better(const OverlapSimilarityEstimate& p, const OverlapSimilarityEstimate& q) {
    if (p.agreed != q.agreed) return p.agreed > q.agreed;
    return p.similarity_residual.rms < q.similarity_residual.rms;
}
} // namespace

OverlapSimilarityEstimate EstimateOverlapSimilarity(const render::BgrImage& a, const render::BgrImage& b, int prior_x, int prior_y) {
    if (a.width != b.width || a.height != b.height) throw std::invalid_argument("similarity estimate: frame dimensions differ");
    (void)render::PixelCount(a.width,a.height);
    for (const auto* image : {&a,&b})
        if (image->bgr.size() != static_cast<std::size_t>(image->width)*image->height*3)
            throw std::invalid_argument("similarity estimate: BGR buffer differs from dimensions");
    const int width = static_cast<int>(a.width), height = static_cast<int>(a.height);
    if (prior_x <= 0 || prior_x >= width || prior_y <= -height || prior_y >= height) throw std::invalid_argument("similarity estimate: prior outside frame");
    // A wrong start can still yield a few mutually consistent random tiles,
    // so every start is tried and the attempts compete; the prior goes first.
    auto result = Attempt(a,b,prior_x,prior_y);
    std::vector<std::array<int,2>> tried{{prior_x,prior_y}};
    bool tie = false;
    for (const auto& start : WideStarts(a,b)) {
        if (start[0] <= 0 || start[0] >= width || std::abs(start[1]) >= height || std::find(tried.begin(),tried.end(),start) != tried.end()) continue;
        tried.push_back(start);
        auto next = Attempt(a,b,start[0],start[1]);
        if (next.status != OverlapEstimateStatus::estimated_not_judged) continue;
        next.wide_search = true;
        if (result.status != OverlapEstimateStatus::estimated_not_judged || Better(next,result)) { result = std::move(next); tie = false; }
        else if (!Better(result,next) && std::memcmp(&next.similarity,&result.similarity,sizeof(SimilarityFit)) != 0) tie = true;
    }
    if (tie) {
        result.status = OverlapEstimateStatus::undetermined;
        result.reason = SimilarityUndeterminedReason::tied_starts;
    }
    return result;
}
} // namespace a0::m2::experimental
