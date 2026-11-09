// Colour match of B to A in the fixed-seam renders (#415).
//
// Camera, file and clock free; every image is synthesized here. A page is drawn by a function of the
// global pixel (paper, ruled lines, blocks of text-like ink), seen by two cameras that differ in
// per-channel gain (A yellowish, B bluish) and in vignetting. The ink positions are known to the test,
// so the paper pixels of the output are measured without the estimator's own mask.
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/experimental/review_alignment.hpp"
#include "similarity_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace a0::m2::experimental;
using a0::m2::render::BgrImage;
namespace render = a0::m2::render;

namespace {
using Rgb = std::array<double, 3>; // B, G, R

BgrImage Flat(unsigned w, unsigned h, std::array<std::uint8_t, 3> bgr) {
    BgrImage image{w, h, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 3)};
    for (size_t i = 0; i < image.bgr.size(); i += 3) for (int c = 0; c < 3; ++c) image.bgr[i + c] = bgr[c];
    return image;
}
void Fill(BgrImage& image, unsigned x0, unsigned x1, std::uint8_t v) {
    for (unsigned y = 0; y < image.height; ++y) for (unsigned x = x0; x < x1 && x < image.width; ++x)
        for (int c = 0; c < 3; ++c) image.bgr[(static_cast<size_t>(y) * image.width + x) * 3 + c] = v;
}
std::array<int, 3> Pixel(const BgrImage& image, unsigned x, unsigned y) {
    const auto i = (static_cast<size_t>(y) * image.width + x) * 3;
    return {image.bgr[i], image.bgr[i + 1], image.bgr[i + 2]};
}
render::PairRenderParameters Shift(double dx) {
    return {{1, 0, dx, 0, 1, 0, 0, 0, 1}, a0::m2::StitchLayout::camera_a_left_camera_b_right, {}};
}
double Luma(const Rgb& p) { return p[0] * 0.114 + p[1] * 0.587 + p[2] * 0.299; }

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
struct Camera { Rgb gain; double vignette; };

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
                static_cast<std::uint8_t>(std::clamp(std::lround((ink ? kInk : kPaper[c]) * camera.gain[c] * v), 0L, 255L));
    }
    return image;
}

// Mean colour of the paper pixels (no ink at the output pixel) of the output columns [x0,x1].
Rgb PaperMean(const BgrImage& out, const Page& page, unsigned x0, unsigned x1) {
    Rgb sum{};
    std::uint64_t n = 0;
    for (unsigned y = 0; y < out.height; ++y) for (unsigned x = x0; x <= x1; ++x) {
        if (page.Ink(x, y)) continue;
        const auto p = Pixel(out, x, y);
        for (int c = 0; c < 3; ++c) sum[c] += p[c];
        ++n;
    }
    for (auto& v : sum) v /= static_cast<double>(n);
    return sum;
}
// The colour with its luminance scaled to 200, so a vignetting that darkens all channels alike does not count.
Rgb Chroma(const Rgb& p) { const double s = 200 / Luma(p); return {p[0] * s, p[1] * s, p[2] * s}; }
double MaxDifference(const Rgb& a, const Rgb& b) {
    double m = 0;
    for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(a[c] - b[c]));
    return m;
}
std::string Text(const Rgb& p) {
    return std::to_string(p[0]).substr(0, 6) + "," + std::to_string(p[1]).substr(0, 6) + "," + std::to_string(p[2]).substr(0, 6);
}
bool SameRecord(const SeamBlendRecord& x, const SeamBlendRecord& y) {
    return x.seam_x == y.seam_x && x.difference_mean == y.difference_mean && x.difference_maximum == y.difference_maximum &&
        x.ratio_minimum == y.ratio_minimum && x.ratio_maximum == y.ratio_maximum && x.saturation_limited_pixels == y.saturation_limited_pixels &&
        x.color.mode == y.color.mode && x.color.global_measured == y.color.global_measured && x.color.global_limited == y.color.global_limited &&
        x.color.paper_cells == y.color.paper_cells && x.color.covered_cells == y.color.covered_cells && x.color.local_limited_rows == y.color.local_limited_rows &&
        x.color.measured_global == y.color.measured_global && x.color.global_gain == y.color.global_gain &&
        x.color.local_minimum == y.color.local_minimum && x.color.local_maximum == y.color.local_maximum;
}
bool Identical(const SeamBlendResult& x, const SeamBlendResult& y) {
    return x.image.width == y.image.width && x.image.height == y.image.height && x.image.bgr == y.image.bgr &&
        x.row_ratio == y.row_ratio && x.row_gain == y.row_gain && SameRecord(x.record, y.record);
}
} // namespace

int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL " << what << '\n'; } };
    const auto reject = [&](auto call, const char* what) {
        bool threw = false;
        try { call(); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, what);
    };
    const SeamBlendSettings small{4, 8, 8, 16};
    try {
        // ---- A yellowish, B bluish, a sparse page ----
        // A page seen by two cameras whose per-channel gains differ (A: low blue, B: low red) and whose
        // corners are darkened (A less than B). The luminance of the two papers is nearly equal, so the
        // earlier single luminance ratio leaves the colour step as it is.
        const unsigned w = 480, h = 320, dx = 240, seam = 360;
        const Camera camera_a{{0.80, 0.98, 1.06}, 0.06}, camera_b{{1.10, 1.02, 0.84}, 0.10};
        const Page sparse{6, false};
        const auto a = Capture(sparse, w, h, 0, 0, camera_a), b = Capture(sparse, w, h, dx, 0, camera_b);
        const auto legacy = RenderPairFixedSeam(a, b, Shift(dx), seam, kSeamBlendDesign, kColorMatchLuminanceRatio);
        const auto matched = RenderPairFixedSeam(a, b, Shift(dx), seam, kSeamBlendDesign);
        check(matched.record.color.mode == ColorMatchMode::rgb_paper && legacy.record.color.mode == ColorMatchMode::luminance_ratio && legacy.row_gain.empty(),
            "the default is the colour match; the luminance ratio is selectable and records no colour match");
        check(matched.image.width == legacy.image.width && matched.image.height == legacy.image.height && matched.row_gain.size() == h && matched.row_ratio.size() == h,
            "the colour match changes no size");
        // Colour step across the seam: the paper beside it on the A side and on the B side.
        const auto step = [&](const SeamBlendResult& r) {
            const auto left = PaperMean(r.image, sparse, seam - 14, seam - 5), right = PaperMean(r.image, sparse, seam + 5, seam + 14);
            return std::pair{left, right};
        };
        const auto [legacy_left, legacy_right] = step(legacy);
        const auto [matched_left, matched_right] = step(matched);
        const double legacy_step = MaxDifference(legacy_left, legacy_right), matched_step = MaxDifference(matched_left, matched_right);
        std::cout << "step across the seam (max channel difference, levels): luminance ratio " << legacy_step << "  colour match " << matched_step << '\n'
                  << "  luminance ratio  A side " << Text(legacy_left) << "  B side " << Text(legacy_right) << '\n'
                  << "  colour match     A side " << Text(matched_left) << "  B side " << Text(matched_right) << '\n';
        check(legacy_step > 40, "the earlier luminance ratio leaves a colour step of more than 40 levels in one channel");
        check(matched_step < 3 && matched_step * 10 < legacy_step, "the colour match leaves a step of less than 3 levels, a tenth of the earlier one");
        // The right half of B: its paper chroma against A's.
        const auto reference = Chroma(PaperMean(matched.image, sparse, 100, 230));
        const auto legacy_far = Chroma(PaperMean(legacy.image, sparse, 560, 700)), matched_far = Chroma(PaperMean(matched.image, sparse, 560, 700));
        const double legacy_far_step = MaxDifference(legacy_far, reference), matched_far_step = MaxDifference(matched_far, reference);
        std::cout << "right half of B against A, chroma at luminance 200: luminance ratio " << legacy_far_step << "  colour match " << matched_far_step << '\n'
                  << "  A " << Text(reference) << "  luminance ratio " << Text(legacy_far) << "  colour match " << Text(matched_far) << '\n';
        check(legacy_far_step > 15 && matched_far_step < 4, "the right half of B takes A's paper colour (less than 4 levels of chroma), not B's own");
        const auto& cm = matched.record.color;
        std::cout << "global gain B,G,R " << Text(cm.global_gain) << "  paper cells " << cm.paper_cells << " of " << cm.covered_cells
                  << "  residual B " << cm.local_minimum[0] << ".." << cm.local_maximum[0] << "  R " << cm.local_minimum[2] << ".." << cm.local_maximum[2] << '\n';
        check(cm.global_measured && !cm.global_limited && !cm.gain_limit_set && cm.global_gain == cm.measured_global && cm.paper_cells > 0 && cm.paper_cells <= cm.covered_cells,
            "global gain measured and applied as measured when no limit is given");
        check(cm.global_gain[0] < 0.85 && cm.global_gain[2] > 1.15, "global gain pulls B's blue down and its red up");
        // The record agrees with the gains applied at the seam.
        {
            bool inside = true;
            for (unsigned y = 0; y < h; ++y) for (int c = 0; c < 3; ++c) {
                const double residual = matched.row_gain[y][c] / cm.global_gain[c];
                if (residual < cm.local_minimum[c] * (1 - 1e-12) || residual > cm.local_maximum[c] * (1 + 1e-12)) inside = false;
            }
            check(inside, "every row's residual is inside the recorded range");
            check(matched.record.ratio_minimum <= matched.record.ratio_maximum && matched.record.ratio_minimum > 0.5 && matched.record.ratio_maximum < 2, "the luminance equivalent of the gain is recorded");
        }
        check(Identical(matched, RenderPairFixedSeam(a, b, Shift(dx), seam, kSeamBlendDesign)), "the colour match is deterministic");

        // The pair mapping as a mapped pair gives the same bytes and record.
        {
            const auto input = PairMappedFixedSeamInput(w, h, w, h, Shift(dx));
            check(Identical(RenderMappedFixedSeam(a, b, input, seam, kSeamBlendDesign), matched), "a mapped pair gives the bytes, the record and the gains of the pair render");
        }

        // ---- Lines and text over more than half of the band, B three columns off ----
        // The gain is measured on the paper only. A has, besides, dark stripes (a fold, a sticker, a shadow)
        // over a third of the overlap that B does not see, so a mean over all cells is pulled far off.
        {
            const Page dense{55, true};
            const auto stripe = [&](unsigned x, unsigned y) { return x >= dx && y % 40 < 14; };
            const Camera ca{{0.80, 0.98, 1.06}, 0.04}, cb{{1.10, 1.02, 0.84}, 0.04};
            auto da = Capture(dense, w, h, 0, 0, ca);
            const auto db = Capture(dense, w, h, dx + 3, 0, cb);
            for (unsigned y = 0; y < h; ++y) for (unsigned x = dx; x < w; ++x) if (stripe(x, y))
                for (int c = 0; c < 3; ++c) da.bgr[(static_cast<size_t>(y) * w + x) * 3 + c] = 30;
            const auto r = RenderPairFixedSeam(da, db, Shift(dx), seam, kSeamBlendDesign);
            Rgb paper_a{}, paper_b{}, all_a{}, all_b{};
            std::uint64_t paper_cells = 0, all_cells = 0;
            for (unsigned y = 0; y < h; ++y) for (unsigned x = dx; x < w; ++x) {
                const auto pa = Pixel(da, x, y), pb = Pixel(db, x - dx, y);
                for (int c = 0; c < 3; ++c) { all_a[c] += pa[c]; all_b[c] += pb[c]; }
                ++all_cells;
                if (dense.Ink(x, y) || dense.Ink(x + 3, y) || stripe(x, y)) continue;
                for (int c = 0; c < 3; ++c) { paper_a[c] += pa[c]; paper_b[c] += pb[c]; }
                ++paper_cells;
            }
            double worst = 0, naive_worst = 0;
            for (int c = 0; c < 3; ++c) {
                const double truth = paper_a[c] / paper_b[c], naive = all_a[c] / all_b[c];
                worst = std::max(worst, std::abs(r.record.color.measured_global[c] / truth - 1));
                naive_worst = std::max(naive_worst, std::abs(naive / truth - 1));
            }
            std::cout << "dense page: non-paper share of the overlap " << 100.0 * double(all_cells - paper_cells) / double(all_cells) << " %, paper cells used " << r.record.color.paper_cells
                      << " of " << r.record.color.covered_cells << "\n  global gain error against the paper truth: " << 100 * worst << " % (mean over all cells: " << 100 * naive_worst << " %)\n";
            check(naive_worst > 0.10, "the ink and the stripes pull a mean over all cells by more than 10 % (the test discriminates)");
            check(r.record.color.global_measured && worst < 0.015, "the global gain is within 1.5 % of the paper truth on a page that is mostly ink and misregistered");
            check(r.record.color.paper_cells < r.record.color.covered_cells * 8 / 10, "the ink is left out of the cells used");
        }

        // ---- A feature that only B sees ----
        // A greenish sticker over a tenth of the overlap in B alone, bright and flat, so the brightness and the
        // flatness do not leave it out. Its A/B ratio is far from the page's, and that is what does.
        {
            const Page page{10, false};
            const Camera ca{{0.80, 0.98, 1.06}, 0.04}, cb{{1.10, 1.02, 0.84}, 0.04};
            const auto sa = Capture(page, w, h, 0, 0, ca);
            auto sb = Capture(page, w, h, dx, 0, cb);
            const auto sticker = [&](unsigned x, unsigned y) { return x >= 300 && x < 420 && y % 50 < 10; };
            for (unsigned y = 0; y < h; ++y) for (unsigned x = dx; x < w; ++x) if (sticker(x, y)) {
                const std::array<std::uint8_t, 3> colour{120, 240, 120};
                for (int c = 0; c < 3; ++c) sb.bgr[(static_cast<size_t>(y) * w + (x - dx)) * 3 + c] = colour[c];
            }
            const auto r = RenderPairFixedSeam(sa, sb, Shift(dx), seam, kSeamBlendDesign);
            Rgb paper_a{}, paper_b{}, all_a{}, all_b{};
            std::uint64_t cells = 0, sticker_cells = 0;
            for (unsigned y = 0; y < h; ++y) for (unsigned x = dx; x < w; ++x) {
                const auto pa = Pixel(sa, x, y), pb = Pixel(sb, x - dx, y);
                for (int c = 0; c < 3; ++c) { all_a[c] += pa[c]; all_b[c] += pb[c]; }
                ++cells;
                if (sticker(x, y)) { ++sticker_cells; continue; }
                if (page.Ink(x, y)) continue;
                for (int c = 0; c < 3; ++c) { paper_a[c] += pa[c]; paper_b[c] += pb[c]; }
            }
            double worst = 0, naive_worst = 0;
            for (int c = 0; c < 3; ++c) {
                const double truth = paper_a[c] / paper_b[c];
                worst = std::max(worst, std::abs(r.record.color.measured_global[c] / truth - 1));
                naive_worst = std::max(naive_worst, std::abs(all_a[c] / all_b[c] / truth - 1));
            }
            std::cout << "sticker in B only (" << 100.0 * double(sticker_cells) / double(cells) << " % of the overlap): global gain error " << 100 * worst
                      << " % (mean over all cells: " << 100 * naive_worst << " %)\n";
            check(naive_worst > 0.05 && worst < 0.015, "a bright flat feature that only B sees does not move the global gain");
        }

        // ---- No colour difference: the output is the earlier output ----
        {
            const unsigned sw = 480, sh = 320;
            const auto world = similarity_scene::MakeScene(sw, sh, 260, 20, 23);
            const auto sa = similarity_scene::ViewA(world, sw, sh), exact_b = similarity_scene::ViewB(world, sw, sh, 240, 0, 0, 1, 1);
            const auto p = SimilarityReviewGeometry(240, 0, 0, 1, sw, sh);
            const auto old_render = RenderPairFixedSeam(sa, exact_b, p, 360, kSeamBlendDesign, kColorMatchLuminanceRatio);
            const auto new_render = RenderPairFixedSeam(sa, exact_b, p, 360, kSeamBlendDesign);
            check(new_render.image.bgr == old_render.image.bgr && new_render.row_ratio == old_render.row_ratio, "the same page in both cameras: the bytes are those of the luminance ratio");
            bool unit = true;
            for (const auto& g : new_render.row_gain) unit = unit && g[0] == 1 && g[1] == 1 && g[2] == 1;
            check(unit && new_render.record.color.global_gain == Rgb{1, 1, 1} && new_render.record.color.local_minimum == Rgb{1, 1, 1} && new_render.record.color.local_maximum == Rgb{1, 1, 1} &&
                new_render.record.ratio_minimum == 1 && new_render.record.ratio_maximum == 1 && new_render.record.difference_maximum == 0 && new_render.record.saturation_limited_pixels == 0,
                "the same page in both cameras: every gain is exactly 1");
            const auto fa = Flat(200, 80, {200, 210, 220}), fb = fa;
            check(RenderPairFixedSeam(fa, fb, Shift(100), 150, small).image.bgr == RenderPairFixedSeam(fa, fb, Shift(100), 150, small, kColorMatchLuminanceRatio).image.bgr,
                "equal flat images: the bytes are those of the luminance ratio");
            std::cout << "A = B: bytes equal to the luminance ratio, difference 0\n";
        }

        // ---- A single gray ratio is the luminance ratio ----
        {
            const auto fa = Flat(200, 80, {100, 100, 100}), fb = Flat(200, 80, {200, 200, 200});
            const auto r = RenderPairFixedSeam(fa, fb, Shift(100), 150, small);
            bool half = true;
            for (const auto& g : r.row_gain) half = half && std::abs(g[0] - 0.5) < 1e-12 && std::abs(g[1] - 0.5) < 1e-12 && std::abs(g[2] - 0.5) < 1e-12;
            check(half && std::abs(r.record.ratio_minimum - 0.5) < 1e-12 && std::abs(r.record.ratio_maximum - 0.5) < 1e-12 && r.record.difference_maximum < 1e-9,
                "A 100 against B 200: every channel gain and the recorded ratio are 0.5, and the seam pixel agrees with A");
            check(Pixel(r.image, 299, 40) == std::array<int, 3>{100, 100, 100} && Pixel(r.image, 180, 40) == std::array<int, 3>{100, 100, 100}, "B is brought to A beside the seam and at its far edge");
        }

        // ---- Brightest B pixels: the three gains are lowered together ----
        {
            const auto fa = Flat(200, 80, {150, 200, 240});
            auto fb = Flat(200, 80, {200, 200, 200});
            Fill(fb, 120, 160, 250); // output columns 220..259: B only, outside the overlap
            const auto r = RenderPairFixedSeam(fa, fb, Shift(100), 150, small);
            const auto paper = Pixel(r.image, 290, 40), patch = Pixel(r.image, 240, 40);
            std::cout << "gain (B,G,R) " << Text(r.record.color.global_gain) << "  B paper at the right edge " << paper[0] << "," << paper[1] << "," << paper[2]
                      << "  white patch " << patch[0] << "," << patch[1] << "," << patch[2] << '\n';
            check(std::abs(paper[0] - 150) <= 1 && std::abs(paper[1] - 200) <= 1 && std::abs(paper[2] - 240) <= 1, "B's paper at its right edge has A's colour");
            // 250 * (0.75, 1, 1.2) = (187.5, 250, 300): the red channel would pass 255, so all three are scaled by 255/300.
            check(r.record.saturation_limited_pixels > 0 && std::abs(patch[0] - 159.375) <= 1 && std::abs(patch[1] - 212.5) <= 1 && patch[2] == 255,
                "a channel that would pass 255 lowers the three gains together, keeping the hue of the match");
        }

        // ---- Paper that cannot be measured: clipped in A ----
        {
            const auto fa = Flat(200, 80, {255, 255, 255}), fb = Flat(200, 80, {200, 210, 220});
            const auto r = RenderPairFixedSeam(fa, fb, Shift(100), 150, small);
            const auto& c = r.record.color;
            check(!c.global_measured && c.paper_cells == 0 && c.covered_cells > 0 && c.global_gain == Rgb{1, 1, 1} && c.measured_global == Rgb{1, 1, 1},
                "a clipped reference gives no measurement, and the global gain stays 1");
            check(Pixel(r.image, 290, 40) == std::array<int, 3>{200, 210, 220}, "without a measurement B keeps its own colour");
        }

        // ---- The limit ----
        {
            const double limit = 1.05;
            ColorMatch limited;
            limited.gain_limit = limit;
            const auto r = RenderPairFixedSeam(a, b, Shift(dx), seam, kSeamBlendDesign, limited);
            const auto& c = r.record.color;
            bool inside = true;
            for (const auto& g : r.row_gain) for (int k = 0; k < 3; ++k) inside = inside && g[k] >= 1 / limit * (1 - 1e-12) && g[k] <= limit * (1 + 1e-12);
            check(c.gain_limit_set && c.gain_limit == limit && c.global_limited && inside, "a limit keeps the gain at the seam, global times residual, within [1/limit, limit]");
            bool global_inside = true;
            for (int k = 0; k < 3; ++k) global_inside = global_inside && c.global_gain[k] >= 1 / limit && c.global_gain[k] <= limit;
            check(global_inside && c.measured_global != c.global_gain, "the global gain is clamped, the measured one is kept in the record");
            const auto step_limited = step(r);
            check(MaxDifference(step_limited.first, step_limited.second) < legacy_step, "even a limited match leaves a smaller step than the luminance ratio");
            ColorMatch one;
            one.gain_limit = 1.0;
            bool unit = true;
            for (const auto& g : RenderPairFixedSeam(a, b, Shift(dx), seam, kSeamBlendDesign, one).row_gain) unit = unit && g[0] == 1 && g[1] == 1 && g[2] == 1;
            check(unit, "a limit of 1 applies no gain");
            ColorMatch wide;
            wide.gain_limit = 100.0;
            const auto w100 = RenderPairFixedSeam(a, b, Shift(dx), seam, kSeamBlendDesign, wide);
            check(w100.image.bgr == matched.image.bgr && !w100.record.color.global_limited && w100.record.color.local_limited_rows == 0, "a limit that is never reached changes nothing");
            for (const double bad : {0.5, 0.0, -2.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
                ColorMatch invalid;
                invalid.gain_limit = bad;
                reject([&] { (void)RenderPairFixedSeam(a, b, Shift(dx), seam, kSeamBlendDesign, invalid); }, "a limit below 1 or not finite is refused");
                reject([&] { (void)RenderMappedFixedSeam(a, b, PairMappedFixedSeamInput(w, h, w, h, Shift(dx)), seam, kSeamBlendDesign, invalid); }, "a mapped render refuses it too");
            }
        }

        // ---- Failures stay failures ----
        {
            const auto fa = Flat(200, 80, {200, 200, 200}), fb = fa;
            reject([&] { (void)RenderPairFixedSeam(fa, fb, Shift(100), 99, small); }, "a seam column out of the overlap fails with the colour match");
            reject([&] { (void)RenderPairFixedSeam(fa, fb, Shift(260), 150, small); }, "no overlap fails with the colour match");
            auto broken = fb;
            broken.bgr.pop_back();
            reject([&] { (void)RenderPairFixedSeam(fa, broken, Shift(100), 150, small); }, "a broken buffer fails with the colour match");
            reject([&] { (void)RenderPairFixedSeam(fa, fb, Shift(100), 150, {4, 8, 8, 40000}); }, "invalid settings fail with the colour match");
        }
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << e.what() << '\n';
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
