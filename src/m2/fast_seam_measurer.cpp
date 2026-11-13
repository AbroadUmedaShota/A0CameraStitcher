#include "a0/m2/experimental/fast_seam_measurer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace a0::m2::experimental::fast_seam_measurer {
namespace {
namespace render_ns = a0::m2::render;

[[noreturn]] void Fail(const char* what) {throw std::invalid_argument(what);}
void Require(bool ok, const char* what) {if (!ok) Fail(what);}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr std::size_t Sz(int v) {return static_cast<std::size_t>(v);}

template <class F> void ParallelFor(std::size_t count, unsigned threads, F&& body) {
    std::atomic<std::size_t> next{0};
    std::exception_ptr failure;
    std::mutex failure_lock;
    const auto work = [&](unsigned worker) {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= count) return;
            try {body(i, worker);}
            catch (...) {const std::lock_guard lock(failure_lock); if (!failure) failure = std::current_exception(); next = count; return;}
        }
    };
    const unsigned extra = static_cast<unsigned>(std::min<std::size_t>(threads, count ? count : 1)) - 1;
    std::vector<std::thread> pool;
    pool.reserve(extra);
    for (unsigned t = 0; t < extra; ++t) pool.emplace_back(work, t + 1);
    work(0);
    for (auto& t : pool) t.join();
    if (failure) std::rethrow_exception(failure);
}

struct Img {
    int w = 0, h = 0;
    std::vector<float> v;
    Img() = default;
    Img(int width, int height, float fill = 0): w(width), h(height), v(Sz(width) * Sz(height), fill) {}
    float at(int x, int y) const {return v[Sz(y) * Sz(w) + Sz(x)];}
    float& at(int x, int y) {return v[Sz(y) * Sz(w) + Sz(x)];}
};

// Binomial 5-tap in x then y; the two outermost pixels of every side stay NaN.
Img Smooth(const Img& in) {
    Img t(in.w, in.h, kNaN), out(in.w, in.h, kNaN);
    for (int y = 0; y < in.h; ++y) {
        const float* r = in.v.data() + Sz(y) * Sz(in.w);
        float* o = t.v.data() + Sz(y) * Sz(in.w);
        for (int x = 2; x + 2 < in.w; ++x) o[x] = (r[x-2] + 4*r[x-1] + 6*r[x] + 4*r[x+1] + r[x+2]) / 16;
    }
    for (int y = 2; y + 2 < in.h; ++y) {
        float* o = out.v.data() + Sz(y) * Sz(in.w);
        const float* a = t.v.data() + Sz(y-2) * Sz(in.w);
        const float* b = a + in.w; const float* c = b + in.w; const float* d = c + in.w; const float* e = d + in.w;
        for (int x = 0; x < in.w; ++x) o[x] = (a[x] + 4*b[x] + 6*c[x] + 4*d[x] + e[x]) / 16;
    }
    return out;
}
Img Gradient(const Img& l) {
    Img g(l.w, l.h, kNaN);
    for (int y = 0; y + 1 < l.h; ++y)
        for (int x = 0; x + 1 < l.w; ++x) {
            const float b = l.at(x, y), gx = l.at(x+1, y) - b, gy = l.at(x, y+1) - b;
            g.at(x, y) = std::sqrt(gx*gx + gy*gy);
        }
    return g;
}

void Cubic(double t, double w[4]) {
    const double t2 = t*t, t3 = t2*t;
    w[0] = -.5*t + t2 - .5*t3; w[1] = 1 - 2.5*t2 + 1.5*t3; w[2] = .5*t + 2*t2 - 1.5*t3; w[3] = -.5*t2 + .5*t3;
}
// An m x m block of the image shifted by a fraction (bicubic); false when it leaves the image or meets a NaN.
bool SampleShift(const Img& s, double x0, double y0, int m, std::vector<float>& out, std::vector<float>& scratch) {
    const int ix = static_cast<int>(std::floor(x0)), iy = static_cast<int>(std::floor(y0));
    if (ix - 1 < 0 || iy - 1 < 0 || ix + m + 2 > s.w || iy + m + 2 > s.h) return false;
    double wx[4], wy[4];
    Cubic(x0 - ix, wx); Cubic(y0 - iy, wy);
    const int rows = m + 3;
    scratch.resize(Sz(rows) * Sz(m)); out.resize(Sz(m) * Sz(m));
    for (int r = 0; r < rows; ++r) {
        const float* in = s.v.data() + Sz(iy - 1 + r) * Sz(s.w) + Sz(ix - 1);
        float* o = scratch.data() + Sz(r) * Sz(m);
        for (int i = 0; i < m; ++i) o[i] = static_cast<float>(wx[0]*in[i] + wx[1]*in[i+1] + wx[2]*in[i+2] + wx[3]*in[i+3]);
    }
    for (int j = 0; j < m; ++j) {
        const float* a = scratch.data() + Sz(j) * Sz(m);
        float* o = out.data() + Sz(j) * Sz(m);
        for (int i = 0; i < m; ++i) o[i] = static_cast<float>(wy[0]*a[i] + wy[1]*a[i+m] + wy[2]*a[i+2*m] + wy[3]*a[i+3*m]);
    }
    for (const float v : out) if (std::isnan(v)) return false;
    return true;
}
// Subtracts the mean and returns the norm of what is left.
double CenterNorm(std::vector<float>& v) {
    double sum = 0;
    for (const float x : v) sum += x;
    const float mean = static_cast<float>(sum / static_cast<double>(v.size()));
    double sq = 0;
    for (auto& x : v) {x -= mean; sq += static_cast<double>(x) * x;}
    return std::sqrt(sq);
}
void Half(const std::vector<float>& in, int n, std::vector<float>& out) {
    const int h = n / 2;
    out.resize(Sz(h) * Sz(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < h; ++x)
            out[Sz(y) * Sz(h) + Sz(x)] = (in[Sz(2*y) * Sz(n) + Sz(2*x)] + in[Sz(2*y) * Sz(n) + Sz(2*x+1)] + in[Sz(2*y+1) * Sz(n) + Sz(2*x)] + in[Sz(2*y+1) * Sz(n) + Sz(2*x+1)]) / 4;
}
bool Cut(const Img& g, int x0, int y0, int n, std::vector<float>& out) {
    if (x0 < 0 || y0 < 0 || x0 + n > g.w || y0 + n > g.h) return false;
    out.resize(Sz(n) * Sz(n));
    for (int y = 0; y < n; ++y) {
        const float* r = g.v.data() + Sz(y0 + y) * Sz(g.w) + Sz(x0);
        for (int x = 0; x < n; ++x) {
            if (std::isnan(r[x])) return false;
            out[Sz(y) * Sz(n) + Sz(x)] = r[x];
        }
    }
    return true;
}
// Zero-mean normalised correlation of the (already centred) template with the n x n window of w at (ox, oy).
double Zncc(const std::vector<float>& t, double tn, int n, const std::vector<float>& w, int wn, int ox, int oy) {
    double sum = 0, sq = 0, dot = 0;
    for (int y = 0; y < n; ++y) {
        const float* r = w.data() + Sz(oy + y) * Sz(wn) + Sz(ox);
        const float* tr = t.data() + Sz(y) * Sz(n);
        for (int x = 0; x < n; ++x) {const double v = r[x]; sum += v; sq += v*v; dot += tr[x] * v;}
    }
    const double var = sq - sum * sum / (static_cast<double>(n) * n);
    return var > 0 && tn > 0 ? dot / tn / std::sqrt(var) : -2;
}

struct Scratch {std::vector<float> t0, t1, t2, w0, w1, w2, p, s, cells, b, bx, by;};

struct Strips {
    int x0 = 0, y0 = 0;
    Img a, b, sa, sb, ga, gb;
};

// One Gauss-Newton refinement of the fractional placement of B's patch against the template (A's gradient magnitude), starting at
// (fx, fy). `one`: only along the normal. Returns false when a window leaves the strip or the system is singular.
bool Refine(const Img& smooth_moving, const std::vector<float>& tmpl, double tnorm, int n, int x0, int y0, bool one, double nx, double ny,
    std::uint32_t max_iterations, double& fx, double& fy, double& score, double& curvature, int& iterations, Scratch& k) {
    const int m = n + 4;
    for (iterations = 0; iterations < static_cast<int>(max_iterations); ++iterations) {
        if (!SampleShift(smooth_moving, x0 - 1 + fx, y0 - 1 + fy, m, k.p, k.s)) return false;
        const int c = n + 2;
        k.cells.resize(Sz(c) * Sz(c));
        for (int j = 0; j < c; ++j)
            for (int i = 0; i < c; ++i) {
                const float b = k.p[Sz(j) * Sz(m) + Sz(i)], gx = k.p[Sz(j) * Sz(m) + Sz(i+1)] - b, gy = k.p[Sz(j+1) * Sz(m) + Sz(i)] - b;
                k.cells[Sz(j) * Sz(c) + Sz(i)] = std::sqrt(gx*gx + gy*gy);
            }
        k.b.resize(Sz(n) * Sz(n)); k.bx.resize(k.b.size()); k.by.resize(k.b.size());
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const std::size_t q = Sz(j+1) * Sz(c) + Sz(i+1);
                k.b[Sz(j) * Sz(n) + Sz(i)] = k.cells[q];
                k.bx[Sz(j) * Sz(n) + Sz(i)] = (k.cells[q+1] - k.cells[q-1]) / 2;
                k.by[Sz(j) * Sz(n) + Sz(i)] = (k.cells[q+Sz(c)] - k.cells[q-Sz(c)]) / 2;
            }
        const double bn = CenterNorm(k.b);
        if (!(bn > 0)) return false;
        (void)CenterNorm(k.bx); (void)CenterNorm(k.by);
        double jxx = 0, jxy = 0, jyy = 0, gx = 0, gy = 0, dot = 0;
        for (std::size_t q = 0; q < k.b.size(); ++q) {
            const double bx = k.bx[q] / bn, by = k.by[q] / bn, bb = k.b[q] / bn, tt = tmpl[q] / tnorm, r = tt - bb;
            jxx += bx*bx; jxy += bx*by; jyy += by*by; gx += bx*r; gy += by*r; dot += tt*bb;
        }
        score = dot;
        double sx, sy;
        if (one) {
            const double a = nx*nx*jxx + 2*nx*ny*jxy + ny*ny*jyy;
            if (!(a > 0)) return false;
            const double s = (nx*gx + ny*gy) / a;
            sx = s*nx; sy = s*ny; curvature = a;
        } else {
            const double det = jxx*jyy - jxy*jxy;
            if (!(det > 1e-18)) return false;
            sx = (jyy*gx - jxy*gy) / det; sy = (jxx*gy - jxy*gx) / det;
            const double mean = (jxx + jyy) / 2, spread = std::sqrt((jxx - jyy) * (jxx - jyy) / 4 + jxy*jxy);
            curvature = mean - spread;
        }
        const double step = std::hypot(sx, sy);
        if (step > 1) {sx /= step; sy /= step;}
        fx += sx; fy += sy;
        if (step < 1e-3) {++iterations; break;}
    }
    return true;
}

// The local maxima (strict against the eight neighbours that are inside) of a score table other than the neighbourhood of
// (bx, by) are candidates for a second peak.
double SecondPeak(const std::vector<double>& sc, int r, int bx, int by, bool one, double nx, double ny) {
    const int side = 2 * r + 1;
    const auto near_best = [&](int ox, int oy) {
        return one ? std::abs(nx * ox + ny * oy) <= 1.0 : std::max(std::abs(ox), std::abs(oy)) <= 1;
    };
    double second = -2;
    for (int oy = -r; oy <= r; ++oy)
        for (int ox = -r; ox <= r; ++ox) {
            const double s = sc[Sz(oy + r) * Sz(side) + Sz(ox + r)];
            if (s <= -2 || near_best(ox - bx, oy - by)) continue;
            bool local = true;
            for (int dy = -1; dy <= 1 && local; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int u = ox + dx, v = oy + dy;
                    if ((dx || dy) && std::abs(u) <= r && std::abs(v) <= r && sc[Sz(v + r) * Sz(side) + Sz(u + r)] > s) {local = false; break;}
                }
            if (local) second = std::max(second, s);
        }
    return second;
}

struct Measured {
    Status status = Status::no_content;
    bool content = false, one = false;
    double nx = 0, ny = 0, dx = 0, dy = 0, score = 0, u = 0, rt = 0;
};

Measured MeasureAt(const Strips& s, int cx, int cy, int n, const Design& o, Scratch& k) {
    Measured out;
    const int h = n / 2, R = static_cast<int>(o.search_radius_px);
    const int x0 = cx - h, y0 = cy - h;
    if (x0 - R - 8 < 0 || y0 - R - 8 < 0 || x0 + n + R + 8 >= s.a.w || y0 + n + R + 8 >= s.a.h) {out.status = Status::outside; return out;}
    int count_a = 0, count_b = 0;
    double sxx = 0, sxy = 0, syy = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const float a = s.a.at(x0 + i, y0 + j), ax = s.a.at(x0 + i + 1, y0 + j) - a, ay = s.a.at(x0 + i, y0 + j + 1) - a;
            const float b = s.b.at(x0 + i, y0 + j), bx = s.b.at(x0 + i + 1, y0 + j) - b, by = s.b.at(x0 + i, y0 + j + 1) - b;
            if (std::isnan(ax) || std::isnan(ay) || std::isnan(bx) || std::isnan(by)) {out.status = Status::outside; return out;}
            if (std::sqrt(ax*ax + ay*ay) >= o.content_level_dn) ++count_a;
            if (std::sqrt(bx*bx + by*by) >= o.content_level_dn) ++count_b;
            sxx += ax*ax; sxy += ax*ay; syy += ay*ay;
        }
    out.content = count_a >= static_cast<int>(o.content_samples) || count_b >= static_cast<int>(o.content_samples);
    if (!out.content) return out;
    // Lines in one direction: the structure tensor of A's patch has one dominant direction; the measurement then runs along its normal.
    const double mean = (sxx + syy) / 2, spread = std::sqrt((sxx - syy) * (sxx - syy) / 4 + sxy*sxy), l1 = mean + spread, l2 = mean - spread;
    if (l1 > 0 && l2 < o.one_directional_ratio * l1) {
        double vx, vy;
        if (std::abs(sxx - l2) >= std::abs(syy - l2)) {vx = sxx - l2; vy = sxy;} else {vx = sxy; vy = syy - l2;}
        const double len = std::hypot(vx, vy);
        if (len > 0) {
            out.one = true; out.nx = vx / len; out.ny = vy / len;
            if (out.nx < 0 || (out.nx == 0 && out.ny < 0)) {out.nx = -out.nx; out.ny = -out.ny;}
        }
    }
    if (!Cut(s.ga, x0, y0, n, k.t0)) {out.status = Status::outside; return out;}
    Half(k.t0, n, k.t1); Half(k.t1, n / 2, k.t2);
    const int wn = n + 2 * R;
    if (!Cut(s.gb, x0 - R, y0 - R, wn, k.w0)) {out.status = Status::outside; return out;}
    Half(k.w0, wn, k.w1); Half(k.w1, wn / 2, k.w2);
    auto tmpl0 = k.t0;
    const double n0 = CenterNorm(tmpl0), n1 = CenterNorm(k.t1), n2 = CenterNorm(k.t2);
    if (!(n0 > 0) || !(n2 > 0)) {out.status = Status::flat; return out;}
    const auto allowed = [&](int ox, int oy) {return !out.one || std::abs(-out.ny * ox + out.nx * oy) <= 0.5 + 1e-9;};
    int bx = 0, by = 0;
    {   // The integer search at a quarter of the scale.
        const int r2 = R / 4, side = 2 * r2 + 1;
        double best = -2;
        std::vector<double> sc(Sz(side) * Sz(side), -2);
        for (int oy = -r2; oy <= r2; ++oy)
            for (int ox = -r2; ox <= r2; ++ox) {
                if (!allowed(ox, oy)) continue;
                const double v = Zncc(k.t2, n2, n / 4, k.w2, wn / 4, ox + r2, oy + r2);
                sc[Sz(oy + r2) * Sz(side) + Sz(ox + r2)] = v;
                if (v > best) {best = v; bx = ox; by = oy;}
            }
        if (!(best > 0)) {out.status = Status::flat; return out;}
        if (SecondPeak(sc, r2, bx, by, out.one, out.nx, out.ny) >= o.second_peak_ratio * best) {out.status = Status::ambiguous; return out;}
        if (std::abs(bx) == r2 || std::abs(by) == r2) {out.status = Status::search_boundary; return out;}
    }
    for (int level = 1; level >= 0; --level) {   // up to the full scale, +-2 around the doubled estimate
        const int r = R >> level, cx2 = 2 * bx, cy2 = 2 * by, nn = n >> level, ww = wn >> level;
        const auto& t = level ? k.t1 : tmpl0;
        const double tn = level ? n1 : n0;
        const auto& w = level ? k.w1 : k.w0;
        double best = -2;
        int qx = cx2, qy = cy2;
        for (int oy = cy2 - 2; oy <= cy2 + 2; ++oy)
            for (int ox = cx2 - 2; ox <= cx2 + 2; ++ox) {
                if (std::abs(ox) > r || std::abs(oy) > r || !allowed(ox - cx2, oy - cy2)) continue;
                const double v = Zncc(t, tn, nn, w, ww, ox + r, oy + r);
                if (v > best) {best = v; qx = ox; qy = oy;}
            }
        if (!(best > 0)) {out.status = Status::flat; return out;}
        bx = qx; by = qy;
    }
    double fx = bx, fy = by, score = 0, curvature = 0;
    int it = 0;
    if (!Refine(s.sb, tmpl0, n0, n, x0, y0, out.one, out.nx, out.ny, o.maximum_iterations, fx, fy, score, curvature, it, k)) {out.status = Status::window; return out;}
    if (std::max(std::abs(fx), std::abs(fy)) > R) {out.status = Status::search_boundary; return out;}
    out.dx = fx; out.dy = fy; out.score = score;
    const int cells = n * n;
    double u = curvature > 0 ? std::sqrt(std::max(0.0, 1 - score) / (curvature * cells)) : 1e9;
    {   // The round trip: B's patch where it was found, measured back against A.
        std::vector<float> p, sc;
        if (!SampleShift(s.sb, x0 + fx, y0 + fy, n + 1, p, sc)) {out.status = Status::window; return out;}
        std::vector<float> tb(Sz(n) * Sz(n));
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const float b = p[Sz(j) * Sz(n+1) + Sz(i)], gx = p[Sz(j) * Sz(n+1) + Sz(i+1)] - b, gy = p[Sz(j+1) * Sz(n+1) + Sz(i)] - b;
                tb[Sz(j) * Sz(n) + Sz(i)] = std::sqrt(gx*gx + gy*gy);
            }
        const double tbn = CenterNorm(tb);
        if (!(tbn > 0)) {out.status = Status::flat; return out;}
        double ex = 0, ey = 0, score2 = 0, curvature2 = 0;
        int it2 = 0;
        if (!Refine(s.sa, tb, tbn, n, x0, y0, out.one, out.nx, out.ny, o.maximum_iterations, ex, ey, score2, curvature2, it2, k)) {out.status = Status::window; return out;}
        out.rt = out.one ? std::abs(out.nx * ex + out.ny * ey) : std::hypot(ex, ey);
        const double u2 = curvature2 > 0 ? std::sqrt(std::max(0.0, 1 - score2) / (curvature2 * cells)) : 1e9;
        u = std::max(u, u2);
        out.score = std::min(score, score2);
    }
    out.u = std::sqrt(u*u + out.rt*out.rt) + o.uncertainty_floor_px;
    if (out.rt > o.round_trip_px) out.status = Status::round_trip;
    else if (!(out.u <= o.maximum_uncertainty_px)) out.status = Status::uncertain;
    else out.status = Status::valid;
    return out;
}

// A label, not a rejection: A's own feature repeats near the point. The self-correlation of A's patch over +-reach px at a quarter of
// the scale (along the normal only for lines in one direction) has a second strict local maximum, more than 4 px from the centre, at
// or above `ratio` times the centre's. A window that leaves the strip gives no label.
bool SelfRepeats(const Img& ga, int cx, int cy, int n, int reach, bool one, double nx, double ny, double ratio, Scratch& k) {
    const int h = n / 2, x0 = cx - h, y0 = cy - h;
    if (!Cut(ga, x0, y0, n, k.t0)) return false;
    Half(k.t0, n, k.t1); Half(k.t1, n / 2, k.t2);
    const int wn = n + 2 * reach;
    if (!Cut(ga, x0 - reach, y0 - reach, wn, k.w0)) return false;
    Half(k.w0, wn, k.w1); Half(k.w1, wn / 2, k.w2);
    const double tn = CenterNorm(k.t2);
    if (!(tn > 0)) return false;
    const int r = reach / 4, side = 2 * r + 1;
    std::vector<double> sc(Sz(side) * Sz(side), -2);
    for (int oy = -r; oy <= r; ++oy)
        for (int ox = -r; ox <= r; ++ox) {
            if (one && std::abs(-ny * ox + nx * oy) > 0.5 + 1e-9) continue;
            sc[Sz(oy + r) * Sz(side) + Sz(ox + r)] = Zncc(k.t2, tn, n / 4, k.w2, wn / 4, ox + r, oy + r);
        }
    const double centre = sc[Sz(r) * Sz(side) + Sz(r)];
    if (!(centre > 0)) return false;
    return SecondPeak(sc, r, 0, 0, one, nx, ny) >= ratio * centre;
}

Img StripLuminance(const render_ns::BgrImage& image, const render_ns::OutputToCamera& map, int x0, int y0, int w, int h, unsigned threads) {
    Img s(w, h, kNaN);
    ParallelFor(Sz(h), threads, [&](std::size_t row, unsigned) {
        const int y = static_cast<int>(row);
        for (int i = 0; i < w; ++i) {
            render_ns::Point raw{};
            std::array<double, 3> px{};
            if (!map({static_cast<double>(x0 + i), static_cast<double>(y0 + y)}, raw) || !render_ns::SampleBicubic(image, raw.x, raw.y, px)) continue;
            s.at(i, y) = static_cast<float>((px[0] + px[1] + px[2]) / 3.0);
        }
    });
    return s;
}
}

std::string_view StatusCode(Status status) {
    switch (status) {
    case Status::valid: return "valid";
    case Status::no_content: return "no-content";
    case Status::outside: return "outside";
    case Status::flat: return "flat";
    case Status::ambiguous: return "ambiguous";
    case Status::search_boundary: return "search-boundary";
    case Status::window: return "window";
    case Status::round_trip: return "round-trip";
    case Status::uncertain: return "uncertain";
    }
    return {};
}

void ValidateDesign(const Design& d) {
    Require(d.search_radius_px >= 4 && d.search_radius_px % 4 == 0 && d.search_radius_px <= 64, "fast seam measurer: the search radius is not a multiple of 4 in 4..64");
    Require(std::isfinite(d.content_level_dn) && d.content_level_dn > 0, "fast seam measurer: the content level is not positive");
    Require(d.content_samples >= 1, "fast seam measurer: the content sample count is not set");
    Require(std::isfinite(d.one_directional_ratio) && d.one_directional_ratio > 0 && d.one_directional_ratio < 1, "fast seam measurer: the one-directional ratio is not in (0, 1)");
    Require(std::isfinite(d.second_peak_ratio) && d.second_peak_ratio > 0 && d.second_peak_ratio <= 1, "fast seam measurer: the second peak ratio is not in (0, 1]");
    Require(std::isfinite(d.round_trip_px) && d.round_trip_px > 0, "fast seam measurer: the round trip line is not positive");
    Require(std::isfinite(d.maximum_uncertainty_px) && d.maximum_uncertainty_px > 0, "fast seam measurer: the uncertainty line is not positive");
    Require(std::isfinite(d.uncertainty_floor_px) && d.uncertainty_floor_px >= 0 && d.uncertainty_floor_px < d.maximum_uncertainty_px, "fast seam measurer: the uncertainty floor is not in [0, line)");
    Require(d.maximum_iterations >= 1 && d.maximum_iterations <= 64, "fast seam measurer: the iteration count is not in 1..64");
    Require(d.periodic_reach_px >= 8 && d.periodic_reach_px % 4 == 0 && d.periodic_reach_px <= 128, "fast seam measurer: the periodic reach is not a multiple of 4 in 8..128");
    Require(std::isfinite(d.neighbour_line_px) && d.neighbour_line_px > 0, "fast seam measurer: the neighbour line is not positive");
    Require(d.neighbour_radius_px >= 1 && d.neighbour_wide_radius_px >= d.neighbour_radius_px, "fast seam measurer: the neighbour radii are not set or not ordered");
    Require(std::isfinite(d.edge_share_line) && d.edge_share_line >= 0 && d.edge_share_line <= 1, "fast seam measurer: the edge share line is not in [0, 1]");
    Require(d.strip_margin_px >= 1, "fast seam measurer: the strip margin is not set");
}

std::vector<Point> Measure(const Request& r, const Design& design) {
    ValidateDesign(design);
    Require(r.camera_a && r.camera_b && r.map_a && r.map_b, "fast seam measurer: both images and both mappings are required");
    Require(r.thread_count >= 1 && r.thread_count <= 256, "fast seam measurer: the thread count is not in 1..256");
    Require(r.patch_px >= 8 && r.patch_px % 4 == 0 && r.patch_px <= 128, "fast seam measurer: the patch side is not a multiple of 4 in 8..128");
    Require(r.lattice_spacing_px >= 1 && r.output_height >= 1, "fast seam measurer: the lattice spacing or the output height is not set");
    Require(std::isfinite(r.baseline.x) && std::isfinite(r.baseline.y) && std::isfinite(r.minimum_baseline_component) && r.minimum_baseline_component > 0,
        "fast seam measurer: the baseline or its minimum component is not set");
    const unsigned threads = r.thread_count;
    std::vector<Point> out(r.lattice.size());
    if (r.lattice.empty()) return out;
    // The strip around the lattice (a sufficient margin makes a point's result independent of the strip's extent).
    std::uint32_t minx = r.lattice.front().first, maxx = minx, miny = r.lattice.front().second, maxy = miny;
    for (const auto& p : r.lattice) {
        minx = std::min(minx, p.first); maxx = std::max(maxx, p.first);
        miny = std::min(miny, p.second); maxy = std::max(maxy, p.second);
    }
    const std::int64_t margin = design.strip_margin_px;
    // The strip holds a point's patch with its search window, the border the smoothing leaves out and the periodic reach, so that a
    // point's result does not depend on how far the strip reaches.
    Require(margin >= static_cast<std::int64_t>(r.patch_px) / 2 + std::max<std::int64_t>(design.search_radius_px + 8, design.periodic_reach_px) + 4,
        "fast seam measurer: the strip margin is too small for the point's windows");
    const std::int64_t sx0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(minx) - margin);
    const std::int64_t sy0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(miny) - margin);
    const std::int64_t sx1 = static_cast<std::int64_t>(maxx) + margin + 1;
    const std::int64_t sy1 = std::min<std::int64_t>(r.output_height, static_cast<std::int64_t>(maxy) + margin + 1);
    Require(sy1 > sy0 && sx1 > sx0, "fast seam measurer: the lattice is outside the output");
    Strips s;
    s.x0 = static_cast<int>(sx0); s.y0 = static_cast<int>(sy0);
    const int w = static_cast<int>(sx1 - sx0), h = static_cast<int>(sy1 - sy0);
    s.a = StripLuminance(*r.camera_a, r.map_a, s.x0, s.y0, w, h, threads);
    s.b = StripLuminance(*r.camera_b, r.map_b, s.x0, s.y0, w, h, threads);
    s.sa = Smooth(s.a); s.sb = Smooth(s.b);
    s.ga = Gradient(s.sa); s.gb = Gradient(s.sb);
    std::vector<Scratch> scratch(threads);
    const int n = static_cast<int>(r.patch_px);
    ParallelFor(r.lattice.size(), threads, [&](std::size_t i, unsigned worker) {
        auto& p = out[i];
        p.x = r.lattice[i].first; p.y = r.lattice[i].second;
        const int cx = static_cast<int>(p.x) - s.x0, cy = static_cast<int>(p.y) - s.y0;
        const Measured m = MeasureAt(s, cx, cy, n, design, scratch[worker]);
        p.status = m.status; p.content = m.content; p.one_directional = m.one;
        p.normal = {m.nx, m.ny}; p.displacement = {m.dx, m.dy}; p.score = m.score; p.uncertainty_px = m.u; p.round_trip_px = m.rt;
        if (m.content) p.periodic = SelfRepeats(s.ga, cx, cy, n, static_cast<int>(design.periodic_reach_px), m.one, m.nx, m.ny, design.second_peak_ratio, scratch[worker]);
        if (!p.Valid()) return;
        if (!m.one) {
            p.along_baseline = 1;
            p.baseline_component_px = r.baseline.x * m.dx + r.baseline.y * m.dy;
            p.usable = true;
        } else {
            const double a = m.nx * r.baseline.x + m.ny * r.baseline.y;
            p.along_baseline = a;
            if (std::abs(a) >= r.minimum_baseline_component) {
                p.baseline_component_px = (m.nx * m.dx + m.ny * m.dy) / a;
                p.usable = true;
            }
        }
    });
    return out;
}

Selection Select(std::vector<Point>& points, const Request& r, const Design& design) {
    ValidateDesign(design);
    Require(r.lattice_spacing_px >= 1 && r.patch_px >= 2, "fast seam measurer: the lattice spacing or the patch side is not set");
    Selection sel;
    sel.lattice_points = static_cast<std::uint32_t>(points.size());
    const std::int64_t spacing = r.lattice_spacing_px;
    const auto key = [](std::int64_t x, std::int64_t y) {return static_cast<std::uint64_t>(x + 100000) << 32 | static_cast<std::uint64_t>(y + 100000);};
    std::unordered_map<std::uint64_t, std::size_t> index;
    for (std::size_t i = 0; i < points.size(); ++i) index[key(points[i].x, points[i].y)] = i;
    // The disposition of every point is decided from the measurement alone (never from another point's disposition).
    for (std::size_t i = 0; i < points.size(); ++i) {
        auto& p = points[i];
        p.disposition = Disposition::not_used;
        if (p.content) {
            ++sel.content_points;
            if (p.status == Status::search_boundary) ++sel.edge_points;
            if (p.periodic) ++sel.periodic_points;
        }
        if (p.Valid()) ++sel.valid_points;
        if (!p.usable) continue;
        if (!p.periodic) {p.disposition = Disposition::kept; continue;}
        std::vector<double> z;
        for (const std::uint32_t radius : {design.neighbour_radius_px, design.neighbour_wide_radius_px}) {
            const std::int64_t k = radius / spacing;
            for (std::int64_t j = -k; j <= k; ++j)
                for (std::int64_t q = -k; q <= k; ++q) {
                    const auto it = index.find(key(static_cast<std::int64_t>(p.x) + q * spacing, static_cast<std::int64_t>(p.y) + j * spacing));
                    if (it == index.end()) continue;
                    const auto& nb = points[it->second];
                    if (nb.usable && !nb.periodic) z.push_back(nb.baseline_component_px);
                }
            if (!z.empty()) break;
        }
        if (z.empty()) {p.disposition = Disposition::dropped_unchecked; continue;}
        std::nth_element(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(z.size() / 2), z.end());
        p.disposition = std::abs(p.baseline_component_px - z[z.size() / 2]) > design.neighbour_line_px ? Disposition::dropped_by_neighbours : Disposition::kept;
    }
    std::vector<parallax_correction::ParallaxPoint> kept;
    for (const auto& p : points) {
        if (p.disposition == Disposition::dropped_by_neighbours) ++sel.dropped_by_neighbours;
        if (p.disposition == Disposition::dropped_unchecked) ++sel.dropped_unchecked;
        if (p.disposition != Disposition::kept) continue;
        parallax_correction::ParallaxPoint q;
        q.position = {static_cast<double>(p.x), static_cast<double>(p.y)};
        q.displacement = p.displacement;
        q.uncertainty_px = p.uncertainty_px;
        q.kind = p.one_directional ? seam_gate::PointKind::one_directional : seam_gate::PointKind::two_directional;
        q.normal = p.one_directional ? p.normal : render::Point{};
        q.footprint = parallax_correction::PointFootprint(p.x, p.y, r.patch_px);
        kept.push_back(q);
    }
    sel.points_kept = static_cast<std::uint32_t>(kept.size());
    sel.edge_share = sel.content_points ? static_cast<double>(sel.edge_points) / static_cast<double>(sel.content_points) : 0.0;
    if (sel.edge_share > design.edge_share_line) sel.fallback_reason = "edge-share";
    else if (kept.empty()) sel.fallback_reason = "no-points";
    else {sel.use = true; sel.points = std::move(kept);}
    return sel;
}
}
