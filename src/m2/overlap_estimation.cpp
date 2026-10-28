#include "a0/m2/experimental/overlap_estimation.hpp"
#include "overlap_estimation_detail.hpp"
#include "zero_mean_correlation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace a0::m2::experimental {
namespace {
using correlation_detail::Center;
using correlation_detail::NormalizedDot;
using overlap_detail::Gradient;
using overlap_detail::Half;
using overlap_detail::Plane;
// Design constants of the search, not acceptance values.
constexpr int kCoarsestSide = 96;      // the top level's larger side is at most this
constexpr std::size_t kPeaks = 8;      // local maxima carried down from the top level
constexpr int kRefineRadius = 2;       // per-level search around 2x the coarser position
constexpr int kSmallestHalved = 16;    // a level is halved only while its smaller side is at least this

struct Range { int dx0, dx1, dy; }; // dx0 <= dx <= dx1, |dy| <= dy
struct Peak {
    int dx{}, dy{};
    double r{}, z{};
    std::uint64_t n{};
    bool tied{}, edge{}, boundary{};
};

Plane Luma(const render::BgrImage& image) {
    Plane p{static_cast<int>(image.width),static_cast<int>(image.height),{}};
    p.v.resize(static_cast<std::size_t>(p.width)*p.height);
    for (std::size_t i = 0; i < p.v.size(); ++i)
        p.v[i] = image.bgr[i*3]*0.114+image.bgr[i*3+1]*0.587+image.bgr[i*3+2]*0.299;
    return p;
}
// output(x,y)=input(w-1-x,h-1-y)
Plane Turn(Plane p) { std::reverse(p.v.begin(),p.v.end()); return p; }
bool Score(const Plane& a, const Plane& b, int dx, int dy, std::vector<double>& va, std::vector<double>& vb, Peak& out) {
    const int x0 = dx, x1 = std::min(a.width,b.width+dx), y0 = std::max(0,dy), y1 = std::min(a.height,b.height+dy);
    if (dx <= 0 || x1-x0 < 2 || y1-y0 < 2) return false;
    const auto cols = static_cast<std::size_t>(x1-x0);
    va.resize(cols*static_cast<std::size_t>(y1-y0)); vb.resize(va.size());
    for (int y = y0; y < y1; ++y) {
        const auto row = static_cast<std::size_t>(y-y0)*cols;
        std::copy_n(a.v.data()+static_cast<std::size_t>(y)*a.width+x0,cols,va.data()+row);
        std::copy_n(b.v.data()+static_cast<std::size_t>(y-dy)*b.width+(x0-dx),cols,vb.data()+row);
    }
    const double na = Center(va), nb = Center(vb);
    if (!(na > 0) || !(nb > 0)) return false;
    double r = NormalizedDot(va,na,vb,nb);
    if (!std::isfinite(r)) return false;
    r = std::clamp(r,std::nextafter(-1.0,0.0),std::nextafter(1.0,0.0));
    const auto n = static_cast<std::uint64_t>(va.size());
    out = {dx,dy,r,std::atanh(r)*std::sqrt(static_cast<double>(n-3)),n,false,false,false};
    return true;
}
bool Before(const Peak& p, const Peak& q) {
    if (p.z != q.z) return p.z > q.z;
    return p.dy != q.dy ? p.dy < q.dy : p.dx < q.dx;
}
Range Scaled(const Range& r, int level) {
    const int s = 1 << level;
    return {(r.dx0+s-1)/s,r.dx1/s,r.dy/s};
}
using Pyramid = std::vector<std::pair<Plane,Plane>>;
// Gradient planes, full resolution first.
Pyramid Build(const Plane& la, const Plane& lb) {
    Pyramid pyramid{{Gradient(la),Gradient(lb)}};
    while (std::max(pyramid.back().first.width,pyramid.back().first.height) > kCoarsestSide &&
        std::min(pyramid.back().first.width,pyramid.back().first.height) >= kSmallestHalved)
        pyramid.emplace_back(Half(pyramid.back().first),Half(pyramid.back().second));
    return pyramid;
}
// Best first, distinct positions. Equal z at the head is reported, not broken.
std::vector<Peak> Search(const Pyramid& pyramid, const Range& full) {
    std::vector<double> va, vb;
    const int top = static_cast<int>(pyramid.size())-1;
    const auto range = Scaled(full,top);
    const int columns = range.dx1-range.dx0+1, rows = 2*range.dy+1;
    std::vector<Peak> grid;
    std::vector<char> valid;
    if (columns > 0) { grid.resize(static_cast<std::size_t>(columns)*rows); valid.resize(grid.size()); }
    for (int j = 0; j < rows && columns > 0; ++j) for (int i = 0; i < columns; ++i) {
        const auto k = static_cast<std::size_t>(j)*columns+i;
        valid[k] = Score(pyramid[top].first,pyramid[top].second,range.dx0+i,j-range.dy,va,vb,grid[k]);
    }
    std::vector<Peak> peaks;
    for (int j = 0; j < rows && columns > 0; ++j) for (int i = 0; i < columns; ++i) {
        const auto k = static_cast<std::size_t>(j)*columns+i;
        if (!valid[k]) continue;
        bool maximum = true, tied = false;
        for (int v = -1; v <= 1; ++v) for (int u = -1; u <= 1; ++u) {
            const int ii = i+u, jj = j+v;
            if ((!u && !v) || ii < 0 || jj < 0 || ii >= columns || jj >= rows) continue;
            const auto q = static_cast<std::size_t>(jj)*columns+ii;
            if (!valid[q]) continue;
            if (grid[q].z > grid[k].z) maximum = false; else if (grid[q].z == grid[k].z) tied = true;
        }
        if (!maximum) continue;
        auto p = grid[k];
        p.tied = tied; p.edge = false;
        p.boundary = top == 0 && (p.dx == range.dx0 || p.dx == range.dx1 || std::abs(p.dy) == range.dy);
        peaks.push_back(p);
    }
    std::sort(peaks.begin(),peaks.end(),Before);
    if (peaks.size() > kPeaks) peaks.resize(kPeaks);
    for (int level = top-1; level >= 0; --level) {
        const auto r = Scaled(full,level);
        std::vector<Peak> next;
        for (const auto& p : peaks) {
            const int lx = std::max(r.dx0,2*p.dx-kRefineRadius), hx = std::min(r.dx1,2*p.dx+kRefineRadius);
            const int ly = std::max(-r.dy,2*p.dy-kRefineRadius), hy = std::min(r.dy,2*p.dy+kRefineRadius);
            Peak best{}, s{};
            bool found = false, tied = false;
            for (int dy = ly; dy <= hy; ++dy) for (int dx = lx; dx <= hx; ++dx) {
                if (!Score(pyramid[level].first,pyramid[level].second,dx,dy,va,vb,s)) continue;
                if (!found || s.z > best.z) { best = s; found = true; tied = false; }
                else if (s.z == best.z) tied = true;
            }
            if (!found) continue;
            best.tied = tied;
            // A best on an unclipped window side means the coarse peak did not
            // contain this level's maximum; the path is not trusted.
            best.edge = p.edge || (best.dx == lx && lx != r.dx0) || (best.dx == hx && hx != r.dx1) ||
                (best.dy == ly && ly != -r.dy) || (best.dy == hy && hy != r.dy);
            best.boundary = level == 0 && (best.dx == r.dx0 || best.dx == r.dx1 || std::abs(best.dy) == r.dy);
            next.push_back(best);
        }
        std::sort(next.begin(),next.end(),Before);
        peaks.clear();
        for (const auto& p : next) {
            const auto same = std::find_if(peaks.begin(),peaks.end(),[&](const Peak& q){ return q.dx == p.dx && q.dy == p.dy; });
            if (same == peaks.end()) peaks.push_back(p);
            else { same->tied = same->tied || p.tied; same->edge = same->edge || p.edge; }
        }
    }
    return peaks;
}
OverlapCandidate Candidate(const Peak& p, unsigned width) {
    return {width-static_cast<unsigned>(p.dx),p.dy,p.r,p.z,p.n};
}
bool Clean(const std::vector<Peak>& peaks) {
    return !peaks.empty() && !peaks[0].tied && !peaks[0].edge && !peaks[0].boundary &&
        (peaks.size() < 2 || peaks[1].z != peaks[0].z);
}
} // namespace

OverlapEstimate EstimateOverlapTranslation(const render::BgrImage& a, const render::BgrImage& b) {
    if (a.width != b.width || a.height != b.height) throw std::invalid_argument("overlap estimate: frame dimensions differ");
    if (a.width < 8 || a.height < 8 || a.width > 640 || a.height > 640) throw std::invalid_argument("overlap estimate: unsupported frame size");
    for (const auto* image : {&a,&b})
        if (image->bgr.size() != static_cast<std::size_t>(image->width)*image->height*3)
            throw std::invalid_argument("overlap estimate: BGR buffer differs from dimensions");
    const int width = static_cast<int>(a.width), height = static_cast<int>(a.height);
    const Range full{(width+9)/10,width*9/10,std::min(100,height/4)};
    const auto la = Luma(a), lb = Luma(b);
    OverlapEstimate result;
    const auto pyramid = Build(la,lb);
    result.levels = static_cast<unsigned>(pyramid.size());
    const auto forward = Search(pyramid,full);
    for (const auto& p : forward) result.candidates.push_back(Candidate(p,a.width));
    if (forward.empty()) { result.reason = OverlapUndeterminedReason::no_candidate; return result; }
    const auto& best = forward[0];
    if (best.tied || (forward.size() > 1 && forward[1].z == best.z)) { result.reason = OverlapUndeterminedReason::tied_candidates; return result; }
    if (best.boundary) { result.reason = OverlapUndeterminedReason::search_boundary; return result; }
    if (best.edge) { result.reason = OverlapUndeterminedReason::refinement_boundary; return result; }
    // Round trip: B becomes the reference. Turning both by 180 degrees keeps
    // B left of A and the same (dx,dy). Above full resolution every pyramid
    // cell and gradient difference is sampled from the other side, so a peak
    // that only the forward cells lead to is not reached. Full resolution
    // compares the forward gradient samples, turned: the integer position is
    // rounded once, the same way in both directions, so a shift near half a
    // pixel does not flip the round trip. With a single level the search is
    // exhaustive at full resolution and the round trip repeats it.
    auto turned = Build(Turn(lb),Turn(la));
    turned.front() = {Turn(pyramid.front().second),Turn(pyramid.front().first)};
    const auto reverse = Search(turned,full);
    if (!reverse.empty()) result.reverse = Candidate(reverse[0],a.width);
    if (!Clean(reverse) || reverse[0].dx != best.dx || reverse[0].dy != best.dy) {
        result.reason = OverlapUndeterminedReason::reverse_mismatch; return result;
    }
    result.status = OverlapEstimateStatus::estimated_not_judged;
    result.reason = OverlapUndeterminedReason::none;
    return result;
}
} // namespace a0::m2::experimental
