// Thread count of the fixed-seam renders (#417).
//
// Camera, file and clock free; every image is synthesized here and no random generator is used. The
// renders of seam_blend.hpp take the output rows in parallel. The result must not depend on how many
// threads there are: the image bytes, the record, row_ratio and row_gain are compared bit for bit between
// thread counts, and the single-thread result is pinned by a SHA-256 taken from the render before the
// rows were run in parallel (the digests below were computed with that code).
//
// The scenes are chosen so that every part that was split by rows has something to do: a colour match
// with paper pixels, with a gain limit, and with the luminance fallback; the luminance ratio; pixels whose
// gain is lowered (a count that is summed over rows); a mapped pair whose mapping is not a similarity;
// a height that is not a multiple of the block of rows of the colour match; fewer rows than threads.
#include "a0/m2/experimental/seam_blend.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace a0::m2::experimental;
using a0::m2::render::BgrImage;
namespace render = a0::m2::render;

namespace {
// SHA-256 (FIPS 180-4).
class Sha256 {
public:
    void Update(const std::uint8_t* data, size_t size) { for (size_t i = 0; i < size; ++i) { block_[used_++] = data[i]; if (used_ == 64) { Compress(); used_ = 0; } } length_ += size; }
    std::string Hex() {
        const std::uint64_t bits = length_ * 8;
        const std::uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (used_ != 56) Update(&zero, 1);
        for (int i = 7; i >= 0; --i) { const auto b = static_cast<std::uint8_t>(bits >> (8 * i)); Update(&b, 1); }
        std::string out;
        for (const auto v : h_) { char text[9]; std::snprintf(text, sizeof text, "%08x", v); out += text; }
        return out;
    }
private:
    static std::uint32_t Rotr(std::uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
    void Compress() {
        static const std::uint32_t k[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = std::uint32_t(block_[4 * i]) << 24 | std::uint32_t(block_[4 * i + 1]) << 16 | std::uint32_t(block_[4 * i + 2]) << 8 | block_[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const auto s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h_;
        for (int i = 0; i < 64; ++i) {
            const auto s1 = Rotr(v[4], 6) ^ Rotr(v[4], 11) ^ Rotr(v[4], 25), ch = (v[4] & v[5]) ^ (~v[4] & v[6]), t1 = v[7] + s1 + ch + k[i] + w[i];
            const auto s0 = Rotr(v[0], 2) ^ Rotr(v[0], 13) ^ Rotr(v[0], 22), maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]), t2 = s0 + maj;
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) h_[i] += v[i];
    }
    std::array<std::uint32_t, 8> h_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::uint8_t block_[64]{};
    size_t used_{};
    std::uint64_t length_{};
};
// The whole result: size, image bytes, every record field (settings, seam columns, summary, difference
// and ratio fields, saturation count, the colour match record), the row ratios and the row gains, all
// little-endian, doubles as their bit patterns.
std::string ResultSha256(const SeamBlendResult& r) {
    Sha256 sha;
    const auto put = [&](std::uint64_t v, int bytes) { std::uint8_t b[8]; for (int i = 0; i < bytes; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * i)); sha.Update(b, static_cast<size_t>(bytes)); };
    const auto real = [&](double v) { std::uint64_t bits{}; std::memcpy(&bits, &v, sizeof bits); put(bits, 8); };
    const auto triple = [&](const std::array<double, 3>& t) { for (const auto v : t) real(v); };
    const auto& rec = r.record;
    put(r.image.width, 4); put(r.image.height, 4); put(r.image.bgr.size(), 8); sha.Update(r.image.bgr.data(), r.image.bgr.size());
    put(rec.settings.width_pixels, 4); put(rec.settings.edge_margin_pixels, 4); put(rec.settings.brightness_half_width_pixels, 4); put(rec.settings.brightness_smoothing_rows, 4);
    put(rec.fixed_seam, 1); put(rec.seam_x.size(), 8); for (const auto v : rec.seam_x) put(v, 4);
    put(rec.summary.x_minimum, 4); put(rec.summary.x_maximum, 4); real(rec.summary.x_mean);
    real(rec.difference_mean); real(rec.difference_maximum); real(rec.ratio_minimum); real(rec.ratio_maximum); put(rec.saturation_limited_pixels, 8);
    const auto& c = rec.color;
    put(static_cast<unsigned>(c.mode), 1); put(c.gain_limit_set, 1); real(c.gain_limit); put(c.global_measured, 1); put(c.global_limited, 1); put(c.luminance_fallback, 1);
    put(c.paper_cells, 8); put(c.covered_cells, 8); put(c.local_limited_rows, 8);
    triple(c.measured_global); triple(c.global_gain); triple(c.local_minimum); triple(c.local_maximum);
    put(r.row_ratio.size(), 8); for (const auto v : r.row_ratio) real(v);
    put(r.row_gain.size(), 8); for (const auto& g : r.row_gain) triple(g);
    return sha.Hex();
}

BgrImage Flat(unsigned w, unsigned h, std::array<std::uint8_t, 3> bgr) {
    BgrImage image{w, h, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 3)};
    for (size_t i = 0; i < image.bgr.size(); i += 3) for (int c = 0; c < 3; ++c) image.bgr[i + c] = bgr[c];
    return image;
}
render::PairRenderParameters Shift(double dx) {
    return {{1, 0, dx, 0, 1, 0, 0, 0, 1}, a0::m2::StitchLayout::camera_a_left_camera_b_right, {}};
}

// The page: ruled lines (optional) and blocks of ink whose share of the area is `density` percent.
struct Page {
    unsigned density{};
    bool lines{};
    bool Ink(long gx, long gy) const {
        if (gx < 0 || gy < 0) return false;
        if (lines && (gy % 20 < 3 || gx % 26 < 3)) return true;
        std::uint32_t h = static_cast<std::uint32_t>(gx / 6) * 2654435761u ^ static_cast<std::uint32_t>(gy / 10) * 40503u ^ 0x9e3779b9u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        return h % 100 < density && gx % 6 < 4 && gy % 10 < 7;
    }
};
constexpr std::array<double, 3> kPaper{214, 224, 228}; // B, G, R
constexpr double kInk = 40;
// A fixed pattern of the pixel, in [-amplitude, amplitude] levels (a hash, no generator).
double Pattern(unsigned x, unsigned y, int channel, unsigned amplitude) {
    if (!amplitude) return 0;
    std::uint32_t h = x * 73856093u ^ y * 19349663u ^ static_cast<std::uint32_t>(channel) * 83492791u ^ 0x85ebca6bu;
    h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return static_cast<double>(h % (2 * amplitude + 1)) - static_cast<double>(amplitude);
}
struct Camera { std::array<double, 3> gain; double vignette; unsigned noise{}; }; // noise: levels of a fixed pattern (+-noise) per pixel and channel
// A camera frame of size w x h whose pixel (x,y) sees the page at (x+ox, y+oy), scaled per channel by
// the camera gain and darkened towards the corners.
BgrImage Capture(const Page& page, unsigned w, unsigned h, long ox, long oy, const Camera& camera) {
    BgrImage image{w, h, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 3)};
    const double cx = w / 2.0, cy = h / 2.0, radius2 = cx * cx + cy * cy;
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const bool ink = page.Ink(x + ox, y + oy);
        const double dx = x - cx, dy = y - cy, v = 1 - camera.vignette * (dx * dx + dy * dy) / radius2;
        for (int c = 0; c < 3; ++c)
            image.bgr[(static_cast<size_t>(y) * w + x) * 3 + c] =
                static_cast<std::uint8_t>(std::clamp(std::lround((ink ? kInk : kPaper[c]) * camera.gain[c] * v + Pattern(x, y, c, camera.noise)), 0L, 255L));
    }
    return image;
}
// Bright flat patches (a highlight that only this camera sees): B takes a gain above 1, so they pass 255.
void Highlights(BgrImage& image, unsigned x0) {
    for (unsigned y = 0; y < image.height; ++y) for (unsigned x = x0; x < image.width; ++x)
        if (y % 50 < 20 && x % 64 < 24) for (int c = 0; c < 3; ++c) image.bgr[(static_cast<size_t>(y) * image.width + x) * 3 + c] = 252;
}

// One scene: the images, how they are placed, and the seam.
struct Scene {
    std::string name;
    BgrImage a, b;
    render::PairRenderParameters pair;
    std::uint32_t seam{};
    SeamBlendSettings settings{kSeamBlendDesign};
    std::optional<MappedFixedSeamInput> mapped; // empty: the pair mapping of `pair`
    ColorMatch color;
};

SeamBlendResult Render(const Scene& s, unsigned threads) {
    if (s.mapped) return RenderMappedFixedSeam(s.a, s.b, *s.mapped, s.seam, s.settings, s.color, threads);
    return RenderPairFixedSeam(s.a, s.b, s.pair, s.seam, s.settings, s.color, threads);
}
// The call without a thread count: the one that existed before, which runs on the calling thread.
SeamBlendResult RenderUnthreaded(const Scene& s) {
    if (s.mapped) return RenderMappedFixedSeam(s.a, s.b, *s.mapped, s.seam, s.settings, s.color);
    return RenderPairFixedSeam(s.a, s.b, s.pair, s.seam, s.settings, s.color);
}
bool Can(const Scene& s, unsigned threads) {
    if (s.mapped) return CanRenderMappedFixedSeam(s.a, s.b, *s.mapped, s.seam, s.settings, threads);
    return CanRenderFixedSeam(s.a, s.b, s.pair, s.seam, s.settings, threads);
}

// Same bytes, same record, same row ratios and gains, bit for bit (doubles compared as bit patterns).
bool Bits(double x, double y) { return std::memcmp(&x, &y, sizeof x) == 0; }
bool Bits(const std::array<double, 3>& x, const std::array<double, 3>& y) { return Bits(x[0], y[0]) && Bits(x[1], y[1]) && Bits(x[2], y[2]); }
bool Bits(const std::vector<double>& x, const std::vector<double>& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) if (!Bits(x[i], y[i])) return false;
    return true;
}
bool Identical(const SeamBlendResult& x, const SeamBlendResult& y) {
    const auto& p = x.record;
    const auto& q = y.record;
    const auto& c = p.color;
    const auto& d = q.color;
    if (x.row_gain.size() != y.row_gain.size()) return false;
    for (size_t i = 0; i < x.row_gain.size(); ++i) if (!Bits(x.row_gain[i], y.row_gain[i])) return false;
    return x.image.width == y.image.width && x.image.height == y.image.height && x.image.bgr == y.image.bgr && Bits(x.row_ratio, y.row_ratio) &&
        p.seam_x == q.seam_x && p.fixed_seam == q.fixed_seam && p.summary.x_minimum == q.summary.x_minimum && p.summary.x_maximum == q.summary.x_maximum && Bits(p.summary.x_mean, q.summary.x_mean) &&
        p.settings.width_pixels == q.settings.width_pixels && p.settings.edge_margin_pixels == q.settings.edge_margin_pixels &&
        p.settings.brightness_half_width_pixels == q.settings.brightness_half_width_pixels && p.settings.brightness_smoothing_rows == q.settings.brightness_smoothing_rows &&
        Bits(p.difference_mean, q.difference_mean) && Bits(p.difference_maximum, q.difference_maximum) && Bits(p.ratio_minimum, q.ratio_minimum) && Bits(p.ratio_maximum, q.ratio_maximum) &&
        p.saturation_limited_pixels == q.saturation_limited_pixels &&
        c.mode == d.mode && c.gain_limit_set == d.gain_limit_set && Bits(c.gain_limit, d.gain_limit) && c.global_measured == d.global_measured && c.global_limited == d.global_limited &&
        c.luminance_fallback == d.luminance_fallback && c.paper_cells == d.paper_cells && c.covered_cells == d.covered_cells && c.local_limited_rows == d.local_limited_rows &&
        Bits(c.measured_global, d.measured_global) && Bits(c.global_gain, d.global_gain) && Bits(c.local_minimum, d.local_minimum) && Bits(c.local_maximum, d.local_maximum);
}

// A mapped pair of the pair mapping of `pair`, with a mapping that is not a similarity: B is stretched a
// little and shifted by a sine of its column, so the rows are sampled at fractional heights.
MappedFixedSeamInput Wavy(unsigned w, unsigned h, const render::PairRenderParameters& pair) {
    auto input = PairMappedFixedSeamInput(w, h, w, h, pair);
    const auto inner = input.camera_b;
    input.camera_b = [inner](render::Point output, render::Point& raw) {
        if (!inner(output, raw)) return false;
        raw.y = raw.y * 0.99 + 1.5 + 0.8 * std::sin(raw.x * 0.013);
        return true;
    };
    return input;
}
// Both cameras sampled at fractional positions everywhere (a scale and an offset that are not dyadic), so
// the samples are not integers and a sum of them depends on the order of the additions.
MappedFixedSeamInput Fractional(unsigned w, unsigned h, const render::PairRenderParameters& pair) {
    auto input = PairMappedFixedSeamInput(w, h, w, h, pair);
    const auto scaled = [](render::OutputToCamera inner) {
        return render::OutputToCamera([inner](render::Point output, render::Point& raw) {
            if (!inner(output, raw)) return false;
            raw.x = raw.x * 0.9731 + 0.37;
            raw.y = raw.y * 0.9873 + 0.41;
            return true;
        });
    };
    input.camera_a = scaled(input.camera_a);
    input.camera_b = scaled(input.camera_b);
    return input;
}
} // namespace

int main(int argc, char** argv) {
    const bool print_pins = argc > 1 && std::string(argv[1]) == "--print-pins";
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const std::string& what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL " << what << '\n'; } };
    const std::array<unsigned, 5> thread_counts{1, 2, 3, 4, 8};
    try {
        // ---- Scenes ----
        std::vector<Scene> scenes;
        const unsigned w = 480, h = 320, dx = 240;
        {   // Yellowish A against dimmer B with highlights only B has: paper pixels, a gain above 1, pixels that pass 255.
            const Page page{6, false};
            Scene s{"paper highlights", Capture(page, w, h, 0, 0, {{1.10, 1.10, 1.10}, 0.05}), Capture(page, w, h, dx, 0, {{0.85, 0.85, 0.85}, 0.08}), Shift(dx), 360, kSeamBlendDesign, {}, {}};
            Highlights(s.b, 170);
            scenes.push_back(s);
        }
        {   // A page that is mostly ink and lines, B three columns off, a gain limit.
            const Page page{55, true};
            Scene s{"dense lines, gain limit", Capture(page, w, h, 0, 0, {{0.80, 0.98, 1.06}, 0.04}), Capture(page, w, h, dx + 3, 0, {{1.10, 1.02, 0.84}, 0.04}), Shift(dx), 360, kSeamBlendDesign, {}, {}};
            s.color.gain_limit = 1.2;
            scenes.push_back(s);
        }
        {   // The luminance ratio, with highlights: the other branch of the mixing.
            const Page page{20, false};
            Scene s{"luminance ratio", Capture(page, w, h, 0, 0, {{1.05, 1.0, 0.95}, 0.05}), Capture(page, w, h, dx, 0, {{0.80, 0.80, 0.80}, 0.08}), Shift(dx), 300, kSeamBlendDesign, {}, {}};
            Highlights(s.b, 120);
            s.color = kColorMatchLuminanceRatio;
            scenes.push_back(s);
        }
        {   // A mapped pair with a wavy mapping for B.
            const Page page{8, true};
            Scene s{"mapped wavy", Capture(page, w, h, 0, 0, {{1.08, 1.08, 1.08}, 0.05}), Capture(page, w, h, dx, 0, {{0.88, 0.9, 0.86}, 0.06}), Shift(dx), 350, kSeamBlendDesign, {}, {}};
            Highlights(s.b, 150);
            s.mapped = Wavy(w, h, s.pair);
            scenes.push_back(s);
        }
        {   // The global gain cannot be measured (no channel is positive in both): the luminance fallback.
            const Page page{30, false};
            Scene s{"luminance fallback", Capture(page, w, h, 0, 0, {{1.0, 1.0, 1.0}, 0.05}), Capture(page, w, h, dx, 0, {{0.9, 0.9, 0.9}, 0.05}), Shift(dx), 360, kSeamBlendDesign, {}, {}};
            for (auto* image : {&s.a, &s.b}) for (size_t i = 0; i < image->bgr.size(); i += 3) image->bgr[i] = 0;
            scenes.push_back(s);
        }
        {   // Fewer rows than the largest thread count, and a width that does not divide anything.
            const Page page{10, false};
            const unsigned tw = 97, th = 5, tdx = 47;
            Scene s{"few rows", Capture(page, tw, th, 0, 0, {{1.1, 1.0, 0.9}, 0.02}), Capture(page, tw, th, tdx, 0, {{0.8, 0.9, 1.0}, 0.02}), Shift(tdx), 72, {4, 8, 8, 16}, {}, {}};
            scenes.push_back(s);
        }
        {   // A height that is not a multiple of the 128-row block of the colour match, and more than two blocks.
            const Page page{6, true};
            const unsigned tw = 200, th = 301, tdx = 100;
            Scene s{"three blocks", Capture(page, tw, th, 0, 0, {{1.1, 1.05, 1.0}, 0.04}), Capture(page, tw, th, tdx, 0, {{0.9, 0.85, 0.8}, 0.04}), Shift(tdx), 150, {4, 8, 8, 16}, {}, {}};
            Highlights(s.b, 40);
            scenes.push_back(s);
        }
        {   // Samples that are not integers: the order of the additions of the colour match shows in the bits.
            const Page page{8, true};
            Scene s{"mapped fractional", Capture(page, w, h, 0, 0, {{1.07, 1.08, 1.09}, 0.05, 3}), Capture(page, w, h, dx, 0, {{0.86, 0.88, 0.9}, 0.06, 3}), Shift(dx), 360, kSeamBlendDesign, {}, {}};
            Highlights(s.b, 150);
            s.mapped = Fractional(w, h, s.pair);
            scenes.push_back(s);
        }

        // Digests of the single-thread result of each scene, taken from the code before the rows were run
        // in parallel. `--print-pins` prints the digests of the current code instead of checking them.
        const std::vector<std::pair<std::string, std::string>> pins{
            {"paper highlights", "5b578e56f06e810953aa2e9714c09b2eb3be56b51ebc1783fe755dae25a8e2aa"},
            {"dense lines, gain limit", "64867e44baadc548973b0f67e2b8c891e43162d9d248b5f1f5ef939048eae286"},
            {"luminance ratio", "676c7d9c1b33f99c8ef87d1f2f705f8922656bc01607825b781720fa70d28445"},
            {"mapped wavy", "6563e0a4254c953766f63c4794fc9d4723cfe64cb5e7a6c4e6cf415ea10c2637"},
            {"luminance fallback", "67ef97733bf60a8ce7d6efde1f2e48a3fd09d7abf87cc95feef2bb62f8e17bc5"},
            {"few rows", "9f90b41273ac84e24fcc22a2fd36fa93b00dedcb44e66a992d47c225c3b53f03"},
            {"three blocks", "5818c5a5cfa48d254b4af396b3b5732bbfa30456d3f7db3bb1ae21d185b954d8"},
            {"mapped fractional", "ce453b6565b255488b7a5236c1a52a27b2dad74925cc214d023e346ffaa82b1b"},
        };
        if (print_pins) {
            for (const auto& scene : scenes) std::cout << "    {\"" << scene.name << "\", \"" << ResultSha256(RenderUnthreaded(scene)) << "\"},\n";
            return 0;
        }

        for (const auto& scene : scenes) {
            const auto reference = RenderUnthreaded(scene);
            const auto digest = ResultSha256(reference);
            {
                const auto pin = std::find_if(pins.begin(), pins.end(), [&](const auto& p) { return p.first == scene.name; });
                check(pin != pins.end() && pin->second == digest, scene.name + ": the single-thread result is the pinned one");
            }
            // The scenes exercise what they were made for.
            if (scene.name == "paper highlights" || scene.name == "mapped wavy" || scene.name == "luminance ratio" || scene.name == "three blocks")
                check(reference.record.saturation_limited_pixels > 0, scene.name + ": some pixels have their gain lowered");
            if (scene.name == "paper highlights" || scene.name == "dense lines, gain limit" || scene.name == "mapped wavy" || scene.name == "three blocks" || scene.name == "mapped fractional")
                check(reference.record.color.mode == ColorMatchMode::rgb_paper && reference.record.color.global_measured && !reference.record.color.luminance_fallback &&
                      reference.record.color.paper_cells > 0 && reference.row_gain.size() == scene.a.height, scene.name + ": the colour match measured paper pixels");
            if (scene.name == "dense lines, gain limit")
                check(reference.record.color.global_limited || reference.record.color.local_limited_rows > 0, scene.name + ": the gain limit is reached");
            if (scene.name == "luminance fallback")
                check(reference.record.color.luminance_fallback && !reference.record.color.global_measured && reference.row_gain.empty(), scene.name + ": the global gain is not measurable");
            if (scene.name == "luminance ratio") check(reference.record.color.mode == ColorMatchMode::luminance_ratio, scene.name + ": the luminance ratio");

            std::vector<unsigned> counts(thread_counts.begin(), thread_counts.end());
            if (scene.name == "few rows") counts.push_back(256); // many more threads than rows
            for (const auto threads : counts) {
                const auto r = Render(scene, threads);
                check(Identical(r, reference), scene.name + ": " + std::to_string(threads) + " threads give the bytes, the record, the gains and the counts of one thread");
                check(ResultSha256(r) == digest, scene.name + ": " + std::to_string(threads) + " threads give the pinned digest");
                check(Can(scene, threads), scene.name + ": the seam column is accepted with " + std::to_string(threads) + " threads");
            }
        }

        // ---- The band of a mapped pair ----
        {
            const auto& s = scenes[3];
            const auto reference = MappedFixedSeamBand(s.a, s.b, *s.mapped);
            bool same = true;
            for (const auto threads : thread_counts) {
                const auto rows = MappedFixedSeamBand(s.a, s.b, *s.mapped, threads);
                same = same && rows.size() == reference.size();
                for (size_t y = 0; same && y < rows.size(); ++y) same = rows[y].first == reference[y].first && rows[y].end == reference[y].end;
            }
            check(same && reference.size() == s.a.height && reference[0].end > reference[0].first, "the overlap rows of a mapped pair do not depend on the thread count");
        }

        // ---- Thread counts ----
        {
            const auto& s = scenes[0];
            const auto rejects = [&](auto call) { try { call(); } catch (const std::invalid_argument&) { return true; } return false; };
            check(rejects([&] { (void)Render(s, 0); }), "0 threads fail");
            check(rejects([&] { (void)Render(s, 257); }), "257 threads fail");
            check(rejects([&] { (void)Can(s, 0); }), "0 threads fail for the check");
            check(rejects([&] { (void)MappedFixedSeamBand(s.a, s.b, *scenes[3].mapped, 0); }), "0 threads fail for the band");
            check(rejects([&] { (void)RenderPairFixedSeam(s.a, s.b, s.pair, s.seam, s.settings, s.color, 257); }), "257 threads fail for the pair render");
        }

        // ---- A failure is that of the lowest failing row ----
        // Row 100 fails with a non-finite coordinate, but only after a pause; row 105 fails at once because B
        // has no sample in it. Run in order, row 100 comes first, so every thread count must report its error.
        {
            const Page page{8, false};
            Scene s{"failing rows", Capture(page, w, h, 0, 0, {{1.0, 1.0, 1.0}, 0.04}), Capture(page, w, h, dx, 0, {{0.9, 0.9, 0.9}, 0.04}), Shift(dx), 360, kSeamBlendDesign, {}, {}};
            auto input = PairMappedFixedSeamInput(w, h, w, h, s.pair);
            const auto inner = input.camera_b;
            input.camera_b = [inner](render::Point output, render::Point& raw) {
                if (output.y == 105) return false;
                if (output.y == 100 && output.x == 300) std::this_thread::sleep_for(std::chrono::milliseconds(40));
                if (output.y == 100 && output.x == 479) { raw = {std::numeric_limits<double>::quiet_NaN(), 0}; return true; }
                return inner(output, raw);
            };
            s.mapped = input;
            const auto message = [&](unsigned threads) {
                try { (void)Render(s, threads); } catch (const std::invalid_argument& e) { return std::string(e.what()); }
                return std::string("(no exception)");
            };
            const auto expected = message(1);
            check(expected.find("nonfinite") != std::string::npos, "one thread reports the lowest failing row (row 100, a non-finite coordinate): " + expected);
            for (const auto threads : thread_counts) check(message(threads) == expected, "the error of " + std::to_string(threads) + " threads is that of one thread");
        }
        // The same for the mixing, which only B covers right of column 480: row 120 gives a non-finite
        // coordinate after a pause, row 125 has no sample in B at once (an uncovered pixel).
        {
            const Page page{8, false};
            Scene s{"failing pixels", Capture(page, w, h, 0, 0, {{1.0, 1.0, 1.0}, 0.04}), Capture(page, w, h, dx, 0, {{0.9, 0.9, 0.9}, 0.04}), Shift(dx), 360, kSeamBlendDesign, {}, {}};
            auto input = PairMappedFixedSeamInput(w, h, w, h, s.pair);
            const auto inner_b = input.camera_b;
            input.camera_b = [inner_b](render::Point output, render::Point& raw) {
                if (output.y == 120 && output.x == 700) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    raw = {std::numeric_limits<double>::quiet_NaN(), 0};
                    return true;
                }
                if (output.y == 125 && output.x == 700) return false;
                return inner_b(output, raw);
            };
            s.mapped = input;
            const auto message = [&](unsigned threads) {
                try { (void)Render(s, threads); return std::string("(no exception)"); } catch (const std::invalid_argument& e) { return std::string(e.what()); }
            };
            const auto expected = message(1);
            check(expected.find("nonfinite") != std::string::npos, "one thread reports the lowest failing row (row 120, a non-finite coordinate): " + expected);
            for (const auto threads : thread_counts) check(message(threads) == expected, "the error of " + std::to_string(threads) + " threads is that of one thread (mixing)");
            // Without the row that pauses, the other failure is what is reported.
            input.camera_b = [inner_b](render::Point output, render::Point& raw) {
                if (output.y == 125 && output.x == 700) return false;
                return inner_b(output, raw);
            };
            s.mapped = input;
            const auto other = message(1);
            check(other.find("uncovered output pixel") != std::string::npos, "an uncovered pixel is rejected: " + other);
            for (const auto threads : thread_counts) check(message(threads) == other, "the uncovered pixel is rejected alike with " + std::to_string(threads) + " threads");
        }
        // A row with no overlap fails with every thread count.
        {
            const auto a = Flat(200, 80, {200, 200, 200}), b = a;
            const SeamBlendSettings small{4, 8, 8, 16};
            for (const auto threads : thread_counts) {
                bool threw = false;
                try { (void)RenderPairFixedSeam(a, b, Shift(100), 99, small, {}, threads); } catch (const std::invalid_argument&) { threw = true; }
                check(threw, "a seam column outside the overlap fails with " + std::to_string(threads) + " threads");
            }
        }
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "exception: " << e.what() << '\n';
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
