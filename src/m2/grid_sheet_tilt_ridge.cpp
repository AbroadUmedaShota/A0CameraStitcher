#include "grid_sheet_tilt_detail.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace a0::m2::experimental::grid_sheet_tilt::detail {
namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

// One-sided kernels, index 0..radius. g and g2 are even, g1 is odd; g1[j] is the weight at +j.
struct Kernels {
    int radius{};
    std::vector<float> g, g1, g2;
};
Kernels MakeKernels(double sigma) {
    Kernels k;
    k.radius = static_cast<int>(std::ceil(3.5 * sigma));
    const int r = k.radius;
    std::vector<double> g(r + 1);
    double sum = 0;
    for (int j = 0; j <= r; ++j) {
        g[j] = std::exp(-double(j) * j / (2 * sigma * sigma));
        sum += j == 0 ? g[j] : 2 * g[j];
    }
    k.g.resize(r + 1); k.g1.resize(r + 1); k.g2.resize(r + 1);
    for (int j = 0; j <= r; ++j) {
        const double gn = g[j] / sum, x = j;
        k.g[j] = static_cast<float>(gn);
        k.g1[j] = static_cast<float>(x / (sigma * sigma) * gn);                       // correlation with this gives +df/dx
        k.g2[j] = static_cast<float>((x * x / std::pow(sigma, 4) - 1 / (sigma * sigma)) * gn);
    }
    return k;
}

struct Scratch {
    std::vector<float> src, hg, hg1, hg2, lxx, lyy, lxy, lx, ly;
};

// Rows [y0, y1) of the picture as float line signal, with `r` rows and columns of edge replication around them.
void LoadBand(const ImageView& im, int y0, int y1, int r, Scratch& s) {
    const int w = static_cast<int>(im.width), h = static_cast<int>(im.height);
    const int rows = y1 - y0 + 2 * r, pw = w + 2 * r;
    s.src.resize(static_cast<std::size_t>(rows) * pw);
    const std::size_t stride = im.stride_bytes ? im.stride_bytes : static_cast<std::size_t>(w) * (im.format == PixelFormat::bgr8 ? 3 : 1);
    for (int i = 0; i < rows; ++i) {
        const int yy = std::clamp(y0 - r + i, 0, h - 1);
        const std::uint8_t* row = im.pixels + static_cast<std::size_t>(yy) * stride;
        float* out = s.src.data() + static_cast<std::size_t>(i) * pw + r;
        if (im.format == PixelFormat::bgr8) {
            for (int x = 0; x < w; ++x) out[x] = 0.5f * (static_cast<float>(row[3 * x + 1]) + static_cast<float>(row[3 * x + 2]));
        } else {
            for (int x = 0; x < w; ++x) out[x] = static_cast<float>(row[x]);
        }
        for (int j = 1; j <= r; ++j) { out[-j] = out[0]; out[w - 1 + j] = out[w - 1]; }
    }
}

// Correlations along the columns of the padded band (row-wise) and then along the rows.
void Filter(const Kernels& k, int w, int rows_out, Scratch& s) {
    const int r = k.radius, pw = w + 2 * r, rows_in = rows_out + 2 * r;
    const std::size_t plane = static_cast<std::size_t>(rows_in) * w;
    s.hg.resize(plane); s.hg1.resize(plane); s.hg2.resize(plane);
    for (int i = 0; i < rows_in; ++i) {
        const float* a = s.src.data() + static_cast<std::size_t>(i) * pw + r;
        float* og = s.hg.data() + static_cast<std::size_t>(i) * w;
        float* o1 = s.hg1.data() + static_cast<std::size_t>(i) * w;
        float* o2 = s.hg2.data() + static_cast<std::size_t>(i) * w;
        for (int x = 0; x < w; ++x) {
            float sg = k.g[0] * a[x], s2 = k.g2[0] * a[x], s1 = 0;
            for (int j = 1; j <= r; ++j) {
                const float p = a[x + j], m = a[x - j];
                sg += k.g[j] * (p + m);
                s2 += k.g2[j] * (p + m);
                s1 += k.g1[j] * (p - m);
            }
            og[x] = sg; o1[x] = s1; o2[x] = s2;
        }
    }
    const std::size_t out_plane = static_cast<std::size_t>(rows_out) * w;
    s.lxx.resize(out_plane); s.lyy.resize(out_plane); s.lxy.resize(out_plane); s.lx.resize(out_plane); s.ly.resize(out_plane);
    for (int i = 0; i < rows_out; ++i) {
        const int c = i + r;
        const auto row = [&](const std::vector<float>& plane_data, int rr) { return plane_data.data() + static_cast<std::size_t>(rr) * w; };
        float* lxx = s.lxx.data() + static_cast<std::size_t>(i) * w;
        float* lyy = s.lyy.data() + static_cast<std::size_t>(i) * w;
        float* lxy = s.lxy.data() + static_cast<std::size_t>(i) * w;
        float* lx = s.lx.data() + static_cast<std::size_t>(i) * w;
        float* ly = s.ly.data() + static_cast<std::size_t>(i) * w;
        const float *g0 = row(s.hg, c), *a0 = row(s.hg1, c), *b0 = row(s.hg2, c);
        for (int x = 0; x < w; ++x) { lxx[x] = k.g[0] * b0[x]; lyy[x] = k.g2[0] * g0[x]; lxy[x] = 0; lx[x] = k.g[0] * a0[x]; ly[x] = 0; }
        for (int j = 1; j <= r; ++j) {
            const float *gp = row(s.hg, c + j), *gm = row(s.hg, c - j);
            const float *ap = row(s.hg1, c + j), *am = row(s.hg1, c - j);
            const float *bp = row(s.hg2, c + j), *bm = row(s.hg2, c - j);
            const float wg = k.g[j], wg1 = k.g1[j], wg2 = k.g2[j];
            for (int x = 0; x < w; ++x) {
                lxx[x] += wg * (bp[x] + bm[x]);
                lyy[x] += wg2 * (gp[x] + gm[x]);
                lxy[x] += wg1 * (ap[x] - am[x]);
                lx[x] += wg * (ap[x] + am[x]);
                ly[x] += wg1 * (gp[x] - gm[x]);
            }
        }
    }
}

// Ridge points of rows [y0, y1).
void BandRidges(const ImageView& im, const Kernels& k, int y0, int y1, float threshold, float axis_ratio, Scratch& s, std::vector<RidgePoint>& out) {
    const int w = static_cast<int>(im.width), rows = y1 - y0;
    LoadBand(im, y0, y1, k.radius, s);
    Filter(k, w, rows, s);
    for (int i = 0; i < rows; ++i) {
        const float* lxx = s.lxx.data() + static_cast<std::size_t>(i) * w;
        const float* lyy = s.lyy.data() + static_cast<std::size_t>(i) * w;
        const float* lxy = s.lxy.data() + static_cast<std::size_t>(i) * w;
        const float* lx = s.lx.data() + static_cast<std::size_t>(i) * w;
        const float* ly = s.ly.data() + static_cast<std::size_t>(i) * w;
        for (int x = 0; x < w; ++x) {
            const float a = lxx[x], c = lyy[x], b = lxy[x];
            const float half_trace = 0.5f * (a + c);
            // A ridge has lambda1 < -threshold and |lambda2| < ratio |lambda1|, so half its trace is below -(1 - ratio) / 2 threshold.
            if (half_trace >= -0.5f * (1.0f - axis_ratio) * threshold) continue;
            const float half_difference = 0.5f * (a - c);
            const float disc = std::hypot(half_difference, b);
            const float l1 = half_trace - disc, l2 = half_trace + disc;
            if (!(l1 < -threshold) || !(std::abs(l2) < axis_ratio * (-l1))) continue;
            // The eigenvector of l1 is (b, l1 - a) and also (l1 - c, b). Of l1 - a = -(half_difference + disc) and
            // l1 - c = half_difference - disc, the one whose two terms add up is used: the other cancels to rounding noise
            // for a line along an image axis (b near zero) and would point along the line.
            float nx, ny;
            if (half_difference >= 0) { nx = b; ny = -(half_difference + disc); }
            else { nx = half_difference - disc; ny = b; }
            const float norm = std::hypot(nx, ny);
            if (!(norm > 0)) continue;
            nx /= norm; ny /= norm;
            // The first derivative along the normal vanishes at the line's centre: f'(t) = f'_n + t f''_n = 0, f''_n = lambda1.
            const float t = -(lx[x] * nx + ly[x] * ny) / l1;
            if (!(std::abs(t * nx) <= 0.5f) || !(std::abs(t * ny) <= 0.5f)) continue;
            out.push_back({static_cast<float>(x) + t * nx, static_cast<float>(y0 + i) + t * ny, nx, ny});
        }
    }
}

// Noise scale of min(Lxx, Lyy) in the band: 1.4826 x the median absolute deviation, from every other pixel and row.
double BandNoise(const ImageView& im, const Kernels& k, int y0, int y1, Scratch& s) {
    const int w = static_cast<int>(im.width), rows = y1 - y0;
    LoadBand(im, y0, y1, k.radius, s);
    Filter(k, w, rows, s);
    std::vector<float> m;
    m.reserve(static_cast<std::size_t>(w) * rows / 4 + 1);
    for (int i = 0; i < rows; i += 2) {
        const float* lxx = s.lxx.data() + static_cast<std::size_t>(i) * w;
        const float* lyy = s.lyy.data() + static_cast<std::size_t>(i) * w;
        for (int x = 0; x < w; x += 2) m.push_back(std::min(lxx[x], lyy[x]));
    }
    if (m.empty()) return 0;
    const auto median = [](std::vector<float>& v) {
        const std::size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
        return v[mid];
    };
    const float med = median(m);
    for (auto& v : m) v = std::abs(v - med);
    return 1.4826 * median(m);
}

// Calls work(i) for every i below `count` on up to `threads` threads. The indices share no state, so the result does not
// depend on the thread count. An exception in a worker is caught there and the first one in thread order is rethrown
// after every thread has been joined; a thread that cannot be started leaves its share to the others.
template <class Work>
void RunParallel(std::size_t count, unsigned threads, Work work) {
    if (threads <= 1 || count <= 1) {
        for (std::size_t i = 0; i < count; ++i) work(i);
        return;
    }
    const unsigned n = static_cast<unsigned>(std::min<std::size_t>(threads, count));
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    std::vector<std::exception_ptr> errors(n);
    const auto body = [&](unsigned t) {
        try {
            while (!stop.load()) {
                const std::size_t i = next.fetch_add(1);
                if (i >= count) return;
                work(i);
            }
        } catch (...) {
            errors[t] = std::current_exception();
            stop.store(true);
        }
    };
    std::vector<std::thread> pool;
    try {
        for (unsigned t = 1; t < n; ++t) pool.emplace_back(body, t);
    } catch (const std::system_error&) {
        // A thread could not start: the threads that did, and this one, take the remaining indices.
    }
    body(0);
    for (auto& t : pool) t.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
}

}   // namespace

RidgeResult DetectRidges(const ImageView& picture, const GridSheetDetectionSettings& settings) {
    Require(picture.pixels != nullptr && picture.width >= 16 && picture.height >= 16, "grid sheet tilt: the picture is empty or too small");
    Require(picture.width <= 32768 && picture.height <= 32768, "grid sheet tilt: the picture is too large");
    const std::size_t min_stride = static_cast<std::size_t>(picture.width) * (picture.format == PixelFormat::bgr8 ? 3 : 1);
    Require(picture.stride_bytes == 0 || picture.stride_bytes >= min_stride, "grid sheet tilt: the stride is shorter than a row");
    Require(std::isfinite(settings.ridge_sigma_px) && settings.ridge_sigma_px >= 0.8 && settings.ridge_sigma_px <= 8, "grid sheet tilt: the ridge sigma is not set");
    Require(settings.ridge_band_rows >= 8 && settings.noise_bands >= 1 && settings.noise_band_rows >= 8, "grid sheet tilt: the band sizes are not set");
    Require(std::isfinite(settings.ridge_noise_multiplier) && settings.ridge_noise_multiplier > 0 && std::isfinite(settings.ridge_minimum_strength) &&
        settings.ridge_minimum_strength > 0 && settings.ridge_axis_ratio > 0 && settings.ridge_axis_ratio < 1, "grid sheet tilt: the ridge thresholds are not set");
    const Kernels k = MakeKernels(settings.ridge_sigma_px);
    const int h = static_cast<int>(picture.height);
    const int band = static_cast<int>(settings.ridge_band_rows);
    unsigned threads = settings.worker_threads;
    if (threads == 0) threads = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));

    // The noise scale from a few bands spread over the picture.
    const int sample_rows = std::min(static_cast<int>(settings.noise_band_rows), h);
    const int bands_n = static_cast<int>(settings.noise_bands);
    std::vector<double> noise(static_cast<std::size_t>(bands_n), 0.0);
    RunParallel(static_cast<std::size_t>(bands_n), threads, [&](std::size_t i) {
        Scratch s;
        const int span = h - sample_rows;
        const int y0 = bands_n == 1 ? span / 2 : static_cast<int>(static_cast<double>(span) * static_cast<double>(i) / (bands_n - 1));
        noise[i] = BandNoise(picture, k, y0, y0 + sample_rows, s);
    });
    std::sort(noise.begin(), noise.end());
    const double scale = noise[noise.size() / 2];
    RidgeResult result;
    result.threshold = std::max(settings.ridge_noise_multiplier * scale, settings.ridge_minimum_strength);

    const std::size_t bands = static_cast<std::size_t>((h + band - 1) / band);
    std::vector<std::vector<RidgePoint>> found(bands);
    RunParallel(bands, threads, [&](std::size_t i) {
        Scratch s;
        const int y0 = static_cast<int>(i) * band, y1 = std::min(y0 + band, h);
        BandRidges(picture, k, y0, y1, static_cast<float>(result.threshold), static_cast<float>(settings.ridge_axis_ratio), s, found[i]);
    });
    std::size_t total = 0;
    for (const auto& f : found) total += f.size();
    result.points.reserve(total);
    for (auto& f : found) result.points.insert(result.points.end(), f.begin(), f.end());
    return result;
}

}   // namespace a0::m2::experimental::grid_sheet_tilt::detail
