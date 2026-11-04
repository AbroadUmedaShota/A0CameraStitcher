// Fixed vertical seam on a pair of output-to-camera mappings (#405 W2).
//
// Camera, file and clock free. Every image is synthesized here: flat and noise images,
// the similarity scenes of the other seam tests, and a paper texture seen through two
// cameras with different lens distortion (the true projection of the document render,
// inverted by fixed-point iteration only in this test).
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
BgrImage Flat(unsigned w, unsigned h, std::array<std::uint8_t, 3> bgr) {
    BgrImage image{w, h, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 3)};
    for (size_t i = 0; i < image.bgr.size(); i += 3) for (int c = 0; c < 3; ++c) image.bgr[i + c] = bgr[c];
    return image;
}
BgrImage Noise(unsigned w, unsigned h, std::uint64_t seed, unsigned shift) {
    BgrImage image{w, h, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 3)};
    similarity_scene::Lcg r{seed};
    std::vector<std::uint8_t> base(static_cast<size_t>(w) * h);
    for (auto& v : base) v = static_cast<std::uint8_t>(60 + r.Next(121));
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) for (int c = 0; c < 3; ++c)
        image.bgr[(static_cast<size_t>(y) * w + x) * 3 + c] = base[static_cast<size_t>(y) * w + (x + shift) % w];
    return image;
}
render::PairRenderParameters Shift(double dx) {
    return {{1, 0, dx, 0, 1, 0, 0, 0, 1}, a0::m2::StitchLayout::camera_a_left_camera_b_right, {}};
}
double Luminance(const std::array<double, 3>& p) { return p[0] * 0.114 + p[1] * 0.587 + p[2] * 0.299; }

// Same bytes, same record, same row ratios.
bool Identical(const SeamBlendResult& x, const SeamBlendResult& y) {
    const auto& p = x.record;
    const auto& q = y.record;
    return x.image.width == y.image.width && x.image.height == y.image.height && x.image.bgr == y.image.bgr &&
        x.row_ratio == y.row_ratio && p.seam_x == q.seam_x && p.fixed_seam == q.fixed_seam &&
        p.settings.width_pixels == q.settings.width_pixels && p.settings.edge_margin_pixels == q.settings.edge_margin_pixels &&
        p.settings.brightness_half_width_pixels == q.settings.brightness_half_width_pixels &&
        p.settings.brightness_smoothing_rows == q.settings.brightness_smoothing_rows &&
        p.summary.x_minimum == q.summary.x_minimum && p.summary.x_maximum == q.summary.x_maximum && p.summary.x_mean == q.summary.x_mean &&
        p.difference_mean == q.difference_mean && p.difference_maximum == q.difference_maximum &&
        p.ratio_minimum == q.ratio_minimum && p.ratio_maximum == q.ratio_maximum &&
        p.saturation_limited_pixels == q.saturation_limited_pixels;
}

// ---- A paper seen by two lenses with different distortion ----
struct TrueCamera {
    render::CameraProjection projection;
    render::Matrix3 inverse;
};
TrueCamera MakeCamera(render::CameraProjection projection) {
    const auto inverse = render::Invert(projection.document_to_image);
    return {projection, inverse};
}
// Document plane (mm) to the ideal pixel plane: a rotation and a scale about (x0,y0), perspective terms g,h,
// placed so that (x0,y0) lands on the pixel (cx,cy).
render::Matrix3 Homography(double cx, double cy, double x0, double y0, double theta, double scale, double g, double h) {
    const double c = std::cos(theta) * scale, s = std::sin(theta) * scale, w0 = 1 + g * x0 + h * y0;
    return {c, -s, cx * w0 - (c * x0 - s * y0), s, c, cy * w0 - (s * x0 + c * y0), g, h, 1};
}
// Raw pixel to document point, the inverse of render::TryProjectDocument (test only).
bool TrueDocument(const TrueCamera& camera, render::Point raw, render::Point& document) {
    const auto& k = camera.projection.intrinsics;
    const auto& d = camera.projection.distortion;
    const double xd = (raw.x - k.cx_pixels) / k.fx_pixels, yd = (raw.y - k.cy_pixels) / k.fy_pixels;
    double x = xd, y = yd;
    for (int i = 0; i < 80; ++i) {
        const double r2 = x * x + y * y, radial = 1 + d.k1 * r2 + d.k2 * r2 * r2 + d.k3 * r2 * r2 * r2;
        const double tx = 2 * d.p1 * x * y + d.p2 * (r2 + 2 * x * x), ty = d.p1 * (r2 + 2 * y * y) + 2 * d.p2 * x * y;
        x = (xd - tx) / radial;
        y = (yd - ty) / radial;
    }
    return render::TryTransform(camera.inverse, {k.fx_pixels * x + k.cx_pixels, k.fy_pixels * y + k.cy_pixels}, document);
}
// Ink lines of three periods on paper.
double Paper(render::Point m) {
    const auto line = [](double t, double period) { return std::pow(0.5 * (1 + std::cos(2 * 3.14159265358979323846 * t / period)), 10); };
    return std::clamp(236 - 120 * line(m.x, 7.0) - 120 * line(m.y, 9.0) - 90 * line(m.x + m.y, 13.0), 0.0, 255.0);
}
BgrImage Photograph(const TrueCamera& camera, unsigned w, unsigned h) {
    BgrImage image{w, h, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 3)};
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        render::Point document{};
        if (!TrueDocument(camera, {double(x), double(y)}, document)) throw std::runtime_error("true camera does not invert");
        const auto v = static_cast<std::uint8_t>(std::lround(Paper(document)));
        for (int c = 0; c < 3; ++c) image.bgr[(static_cast<size_t>(y) * w + x) * 3 + c] = v;
    }
    return image;
}
// How far apart A and B show a point of the paper at output pixel p, in raw pixels of A: the document
// point B shows, projected into A, against where the mapping samples A.
double Displacement(const render::OutputToCamera& map_a, const render::OutputToCamera& map_b, const TrueCamera& a, const TrueCamera& b, render::Point p) {
    render::Point ra{}, rb{}, document{}, expected{};
    if (!map_a(p, ra) || !map_b(p, rb) || !TrueDocument(b, rb, document) || !render::TryProjectDocument(a.projection, document, expected))
        throw std::runtime_error("displacement is not defined here");
    return std::hypot(ra.x - expected.x, ra.y - expected.y);
}
// Least-squares similarity from B's raw pixels to A's over a lattice of document points (rotation, scale, shift).
render::PairRenderParameters FitSimilarity(const TrueCamera& a, const TrueCamera& b, double x0, double x1, double y0, double y1) {
    std::vector<render::Point> pa, pb;
    for (double x = x0; x <= x1; x += 2) for (double y = y0; y <= y1; y += 2) {
        render::Point ra{}, rb{};
        if (render::TryProjectDocument(a.projection, {x, y}, ra) && render::TryProjectDocument(b.projection, {x, y}, rb)) { pa.push_back(ra); pb.push_back(rb); }
    }
    render::Point ma{}, mb{};
    for (size_t i = 0; i < pa.size(); ++i) { ma.x += pa[i].x; ma.y += pa[i].y; mb.x += pb[i].x; mb.y += pb[i].y; }
    ma.x /= pa.size(); ma.y /= pa.size(); mb.x /= pb.size(); mb.y /= pb.size();
    double dot = 0, cross = 0, norm = 0;
    for (size_t i = 0; i < pa.size(); ++i) {
        const double bx = pb[i].x - mb.x, by = pb[i].y - mb.y, ax = pa[i].x - ma.x, ay = pa[i].y - ma.y;
        dot += bx * ax + by * ay; cross += bx * ay - by * ax; norm += bx * bx + by * by;
    }
    const double re = dot / norm, im = cross / norm;
    return {{re, -im, ma.x - (re * mb.x - im * mb.y), im, re, ma.y - (im * mb.x + re * mb.y), 0, 0, 1},
        a0::m2::StitchLayout::camera_a_left_camera_b_right, {}};
}
// The canvas of a similarity pair is the union bounding box of both frames: its corners are covered by neither
// camera and its first and last rows have no overlap. Crop it, as the review geometry does, to the rows and
// columns where every output pixel has a camera and the overlap is wide (two pixels inside the covered area).
render::PairRenderParameters CropToOverlap(render::PairRenderParameters parameters, unsigned width, unsigned height) {
    const auto input = PairMappedFixedSeamInput(width, height, width, height, parameters);
    const auto covers = [&](const render::OutputToCamera& map, unsigned x, unsigned y) {
        render::Point raw{};
        return map({double(x), double(y)}, raw) && raw.x >= 0 && raw.y >= 0 && raw.x < width && raw.y < height;
    };
    long long limit = input.width;
    for (unsigned y = input.height / 4; y < input.height * 3 / 4; ++y) {
        long long last = -1;
        for (unsigned x = 0; x < input.width; ++x) if (covers(input.camera_a, x, y) || covers(input.camera_b, x, y)) last = x;
        limit = std::min(limit, last + 1);
    }
    limit -= 2;
    std::vector<bool> good(input.height);
    for (unsigned y = 0; y < input.height; ++y) {
        unsigned overlap = 0;
        bool whole = true;
        for (long long x = 0; x < limit; ++x) {
            const bool ca = covers(input.camera_a, static_cast<unsigned>(x), y), cb = covers(input.camera_b, static_cast<unsigned>(x), y);
            overlap += ca && cb;
            whole = whole && (ca || cb);
        }
        good[y] = whole && overlap >= 60;
    }
    long long best_first = -1, best_last = -1;
    for (long long y = 0; y < static_cast<long long>(input.height);) {
        if (!good[y]) { ++y; continue; }
        long long z = y;
        while (z + 1 < static_cast<long long>(input.height) && good[z + 1]) ++z;
        if (best_first < 0 || z - y > best_last - best_first) { best_first = y; best_last = z; }
        y = z + 1;
    }
    if (best_first < 0) throw std::runtime_error("no overlap to crop to");
    parameters.crop.top = static_cast<std::uint32_t>(best_first + 2);
    parameters.crop.bottom = static_cast<std::uint32_t>(input.height - 1 - best_last + 2);
    parameters.crop.right = static_cast<std::uint32_t>(input.width - limit);
    return parameters;
}
// The columns that can be sewn are the ones with half a blend width of both cameras on every row:
// [first, last], from the runs of the rows. Returns the middle one, checked with CanRenderMappedFixedSeam.
struct SewableColumns { std::uint32_t first{}, last{}, middle{}; };
SewableColumns Sewable(const BgrImage& a, const BgrImage& b, const MappedFixedSeamInput& input, const SeamBlendSettings& settings) {
    const auto rows = MappedFixedSeamBand(a, b, input);
    const std::uint32_t half = (settings.width_pixels - 1) / 2;
    std::uint32_t first = 0, end = ~0u;
    for (const auto& r : rows) { first = std::max(first, r.first); end = std::min(end, r.end); }
    if (end < first + 2 * half + 1) return {};
    SewableColumns s{first + half, end - 1 - half, 0};
    s.middle = (s.first + s.last) / 2;
    if (!CanRenderMappedFixedSeam(a, b, input, s.middle, settings) || !CanRenderMappedFixedSeam(a, b, input, s.first, settings) ||
        !CanRenderMappedFixedSeam(a, b, input, s.last, settings) || CanRenderMappedFixedSeam(a, b, input, s.first - 1, settings) ||
        CanRenderMappedFixedSeam(a, b, input, s.last + 1, settings))
        throw std::runtime_error("the sewable columns do not match CanRenderMappedFixedSeam");
    return s;
}
struct Groups { double top{}, middle{}, bottom{}; };
// Maximum over the rows of the top, the middle and the bottom 15% of the output.
template<class F> Groups MaxByGroup(unsigned height, F value) {
    Groups g;
    const unsigned span = height * 15 / 100;
    for (unsigned y = 0; y < span; ++y) g.top = std::max(g.top, value(y));
    for (unsigned y = (height - span) / 2; y < (height + span) / 2; ++y) g.middle = std::max(g.middle, value(y));
    for (unsigned y = height - span; y < height; ++y) g.bottom = std::max(g.bottom, value(y));
    return g;
}
} // namespace

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << "FAIL " << what << '\n'; } };
    auto reject = [&](auto call, const char* what) {
        bool threw = false;
        try { call(); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, what);
    };
    const SeamBlendSettings small{4, 8, 8, 16};
    try {
        // ---- The pair mapping of render::RenderPair, as a pair of mappings, is RenderPairFixedSeam ----
        {
            const auto fa = Flat(200, 80, {200, 200, 200}), fb = Flat(200, 80, {100, 200, 238});
            const auto ca = Flat(200, 80, {250, 250, 250}), cb = Flat(200, 80, {180, 200, 240});
            const auto na = Noise(200, 64, 11, 29), nb = Noise(200, 64, 23, 53);
            const unsigned sw = 480, sh = 320;
            const auto world = similarity_scene::MakeScene(sw, sh, 260, 20, 23);
            const auto sa = similarity_scene::ViewA(world, sw, sh), exact_b = similarity_scene::ViewB(world, sw, sh, 240, 0, 0, 1, 1);
            auto dim = exact_b;
            for (auto& v : dim.bgr) v = static_cast<std::uint8_t>(std::lround(v * 0.8));
            const auto turned = similarity_scene::ViewB(world, sw, sh, 241, 3, 1.0, 1, 0.9);
            const auto q = SimilarityReviewGeometry(241, 3, 1.0, 1, sw, sh), p = SimilarityReviewGeometry(240, 0, 0, 1, sw, sh);
            const auto rb = similarity_scene::ViewB(world, sw, sh, 241, 3, 2.0, 1, 1);
            const auto rq = SimilarityReviewGeometry(241, 3, 2.0, 1, sw, sh);
            struct Case { const char* name; const BgrImage* a; const BgrImage* b; render::PairRenderParameters parameters; std::uint32_t column; SeamBlendSettings settings; };
            const std::vector<Case> cases{
                {"flat", &fa, &fb, Shift(100), 150, small},
                {"rim of the overlap", &fa, &fb, Shift(100), 101, {4, 5000, 8, 16}},
                {"design width", &fa, &fb, Shift(100), 103, kSeamBlendDesign},
                {"saturating gain", &ca, &cb, Shift(100), 150, small},
                {"noise", &na, &nb, Shift(100), 150, small},
                {"registered scene", &sa, &exact_b, p, 360, small},
                {"dimmed scene", &sa, &dim, p, 360, kSeamBlendDesign},
                {"rotation and scale", &sa, &turned, q, 362, kSeamBlendDesign},
                {"rotated by two degrees", &sa, &rb, rq, 363, small},
            };
            for (const auto& c : cases) {
                const auto reference = RenderPairFixedSeam(*c.a, *c.b, c.parameters, c.column, c.settings);
                const auto input = PairMappedFixedSeamInput(c.a->width, c.a->height, c.b->width, c.b->height, c.parameters);
                const auto mapped = RenderMappedFixedSeam(*c.a, *c.b, input, c.column, c.settings);
                check(Identical(reference, mapped), (std::string("a pair mapping gives the bytes and the record of RenderPairFixedSeam: ") + c.name).c_str());
                check(mapped.record.fixed_seam && mapped.image.width == input.width && mapped.image.height == input.height, "mapped fixed seam record and size");
            }
            // The same acceptance of columns as CanRenderFixedSeam, around the overlap of a rotated pair.
            {
                const auto input = PairMappedFixedSeamInput(sw, sh, sw, sh, rq);
                const auto sewable = Sewable(sa, rb, input, small);
                bool agrees = sewable.last > sewable.first + 40;
                for (const std::uint32_t column : {0u, 100u, sewable.first - 4, sewable.first - 1, sewable.first, sewable.first + 1, sewable.middle, sewable.last - 1, sewable.last, sewable.last + 1, sewable.last + 4, input.width - 1})
                    agrees = agrees && CanRenderFixedSeam(sa, rb, rq, column, small) == CanRenderMappedFixedSeam(sa, rb, input, column, small) &&
                        CanRenderFixedSeam(sa, rb, rq, column, small) == (column >= sewable.first && column <= sewable.last);
                check(agrees, "CanRenderMappedFixedSeam accepts the columns CanRenderFixedSeam accepts, and they are the columns with half a blend on every row");
                // The per-row run is the first to the last covered column, found by brute force.
                const auto rows = MappedFixedSeamBand(sa, rb, input);
                const auto inverse = render::Invert(rq.camera_b_to_camera_a);
                const auto plan = render::PlanPairCanvas(sw, sh, sw, sh, rq);
                const double gx = plan.origin_x + rq.crop.left, gy = plan.origin_y + rq.crop.top;
                bool same = rows.size() == plan.output_height;
                unsigned minimum_first = ~0u, maximum_first = 0;
                for (unsigned y = 0; y < plan.output_height && same; ++y) {
                    unsigned first = ~0u, last = 0, count = 0;
                    for (unsigned x = 0; x < plan.output_width; ++x) {
                        std::array<double, 3> va{}, vb{};
                        render::Point s{};
                        if (render::SampleBilinear(sa, gx + x, gy + y, va) && render::TryTransform(inverse, {gx + x, gy + y}, s) && render::SampleBilinear(rb, s.x, s.y, vb)) {
                            first = std::min(first, x); last = x; ++count;
                        }
                    }
                    same = same && rows[y].first == first && rows[y].end == last + 1 && count == last + 1 - first;
                    minimum_first = std::min(minimum_first, first); maximum_first = std::max(maximum_first, first);
                }
                check(same, "MappedFixedSeamBand is the covered run of every row");
                check(maximum_first >= minimum_first + 6, "the overlap edge of the rotated pair moves between rows");
            }
            // Flat pair: every row is one run [100,200).
            {
                const auto rows = MappedFixedSeamBand(fa, fb, PairMappedFixedSeamInput(200, 80, 200, 80, Shift(100)));
                bool all = rows.size() == 80;
                for (const auto& r : rows) all = all && r.first == 100 && r.end == 200;
                check(all, "the band of a shifted flat pair");
            }
            // Deterministic.
            {
                const auto input = PairMappedFixedSeamInput(sw, sh, sw, sh, rq);
                check(Identical(RenderMappedFixedSeam(sa, rb, input, 363, small), RenderMappedFixedSeam(sa, rb, input, 363, small)), "mapped render is deterministic");
            }
        }

        // ---- A document mapping without distortion places the pair like the v1 similarity ----
        {
            // 0.1 mm per output pixel (254 dpi); the projection maps document mm to pixels at 10 px/mm, so output pixel x
            // samples the raw position x (A) and x-100 (B), each plus 1e-9 so that the first row and column are not
            // lost to the last digit of the projection (the outermost samples sit exactly on the edge of the coverage).
            const unsigned w = 200, h = 64;
            const auto na = Noise(w, h, 11, 29), nb = Noise(w, h, 23, 53);
            const render::OutputRaster raster{{0, 0, 30000, 6400}, 254, 300, 64};
            const render::CameraProjection ca{{1, 1, 0, 0}, {0, 0, 0, 0, 0}, {10, 0, -0.5 + 1e-9, 0, 10, -0.5 + 1e-9, 0, 0, 1}};
            const render::CameraProjection cb{{1, 1, 0, 0}, {0, 0, 0, 0, 0}, {10, 0, -100.5 + 1e-9, 0, 10, -0.5 + 1e-9, 0, 0, 1}};
            const render::DocumentRenderParameters document{raster, ca, cb, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear};
            const auto vone = RenderPairFixedSeam(na, nb, Shift(100), 150, small);
            const auto vtwo = RenderDocumentFixedSeam(na, nb, document, 150, small);
            int worst = 0;
            size_t different = 0;
            check(vone.image.width == vtwo.image.width && vone.image.height == vtwo.image.height, "zero-distortion document raster has the pair canvas size");
            for (size_t i = 0; i < vone.image.bgr.size() && i < vtwo.image.bgr.size(); ++i) {
                const int d = std::abs(int(vone.image.bgr[i]) - int(vtwo.image.bgr[i]));
                worst = std::max(worst, d);
                different += d != 0;
            }
            std::cout << "zero_distortion_document_vs_pair different_bytes=" << different << " of " << vone.image.bgr.size() << " largest_difference=" << worst << '\n';
            double ratio_difference = 0;
            for (size_t y = 0; y < vone.row_ratio.size(); ++y) ratio_difference = std::max(ratio_difference, std::abs(vone.row_ratio[y] / vtwo.row_ratio[y] - 1));
            check(worst <= 1 && different * 1000 < vone.image.bgr.size(), "zero-distortion document mapping reproduces the similarity pair up to rounding of the sample position");
            check(vone.record.seam_x == vtwo.record.seam_x && vtwo.record.fixed_seam && ratio_difference < 1e-5 &&
                std::abs(vone.record.difference_mean - vtwo.record.difference_mean) < 1e-3 * (1 + vone.record.difference_mean) &&
                vone.record.saturation_limited_pixels == vtwo.record.saturation_limited_pixels, "zero-distortion document mapping: seam and brightness record agree");
            check(CanRenderDocumentFixedSeam(na, nb, document, 150, small) && !CanRenderDocumentFixedSeam(na, nb, document, 99, small), "CanRenderDocumentFixedSeam");
        }

        // ---- Lens distortion: a document mapping follows it, a similarity cannot ----
        const render::OutputRaster raster{{0, 2000, 200000, 78000}, 100, 788, 300};
        const TrueCamera true_a = MakeCamera({{520, 520, 239.5, 179.5}, {-0.12, 0.04, 0, 0.0006, -0.0004}, Homography(239.5, 179.5, 60, 40, 0.010, 4.0, 2e-5, -1e-5)});
        const TrueCamera true_b = MakeCamera({{515, 518, 239.5, 179.5}, {-0.16, 0.05, 0, -0.0005, 0.0007}, Homography(239.5, 179.5, 140, 40, -0.008, 4.02, -1.5e-5, 1.2e-5)});
        const unsigned cw = 480, ch = 360;
        const auto photo_a = Photograph(true_a, cw, ch), photo_b = Photograph(true_b, cw, ch);
        const auto truth = render::DocumentRenderParameters{raster, true_a.projection, true_b.projection, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear};
        // An estimate with wrong coefficients and placement, as a calibration of limited accuracy would give.
        const auto estimate_a = MakeCamera({{520, 520, 239.5, 179.5}, {-0.12 * 1.12, 0.04 * 0.8, 0, 0.0006, -0.0004}, Homography(239.5, 179.5, 60.05, 40, 0.0102, 4.004, 2e-5, -1e-5)});
        const auto estimate_b = MakeCamera({{515, 518, 239.5, 179.5}, {-0.16 * 0.9, 0.05 * 1.2, 0, -0.0005, 0.0007}, Homography(239.5, 179.5, 140, 40.05, -0.0078, 4.02, -1.5e-5, 1.2e-5)});
        const render::DocumentRenderParameters estimated{raster, estimate_a.projection, estimate_b.projection, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear};
        const auto truth_input = DocumentMappedFixedSeamInput(truth);
        const auto estimate_input = DocumentMappedFixedSeamInput(estimated);
        const auto similarity = CropToOverlap(FitSimilarity(true_a, true_b, 84, 116, 8, 72), cw, ch);
        const auto similarity_input = PairMappedFixedSeamInput(cw, ch, cw, ch, similarity);
        const SeamBlendSettings settings = kSeamBlendDesign;

        const auto truth_sewable = Sewable(photo_a, photo_b, truth_input, settings);
        const auto truth_column = truth_sewable.middle;
        const auto estimate_column = Sewable(photo_a, photo_b, estimate_input, settings).middle;
        const auto similarity_column = Sewable(photo_a, photo_b, similarity_input, settings).middle;
        std::cout << "distortion scene columns truth=" << truth_column << " estimate=" << estimate_column << " similarity=" << similarity_column
            << " output " << truth_input.width << "x" << truth_input.height << " / " << similarity_input.width << "x" << similarity_input.height << '\n';
        check(truth_column > 0 && estimate_column > 0 && similarity_column > 0 && truth_sewable.last > truth_sewable.first + 20, "every mapping has a column that can be sewn");

        // Geometry: where the two cameras show the same paper point, along the seam column.
        const auto measure = [&](const MappedFixedSeamInput& input, std::uint32_t column) {
            return MaxByGroup(input.height, [&](unsigned y) { return Displacement(input.camera_a, input.camera_b, true_a, true_b, {double(column), double(y)}); });
        };
        const auto by_truth = measure(truth_input, truth_column), by_estimate = measure(estimate_input, estimate_column), by_similarity = measure(similarity_input, similarity_column);
        std::cout << "seam displacement in raw pixels, maximum over the top / middle / bottom 15% of rows\n"
            << "  document mapping, true lens:      " << by_truth.top << " / " << by_truth.middle << " / " << by_truth.bottom << '\n'
            << "  document mapping, estimated lens: " << by_estimate.top << " / " << by_estimate.middle << " / " << by_estimate.bottom << '\n'
            << "  similarity of the same pair:      " << by_similarity.top << " / " << by_similarity.middle << " / " << by_similarity.bottom << '\n';
        check(by_truth.top < 0.01 && by_truth.middle < 0.01 && by_truth.bottom < 0.01, "a document mapping with the true lens puts A and B on the same paper point along the whole seam");
        check(by_similarity.top > 1.0 && by_similarity.bottom > 1.0, "a similarity leaves more than one raw pixel of displacement at both ends of the seam");
        check(by_truth.top * 20 < by_similarity.top && by_truth.bottom * 20 < by_similarity.bottom, "true-lens displacement at the ends is far below the similarity's");
        check(by_estimate.top < by_similarity.top * 0.5 && by_estimate.bottom < by_similarity.bottom * 0.5 && by_estimate.middle < by_similarity.middle + 0.5,
            "an estimated lens still beats the similarity at both ends");

        // The same, as the render sees it: A and B sampled at the seam differ far less through the document mapping.
        const auto ghost = [&](const MappedFixedSeamInput& input, std::uint32_t column) {
            return MaxByGroup(input.height, [&](unsigned y) {
                double sum = 0;
                unsigned n = 0;
                for (std::uint32_t x = column - 3; x <= column + 3; ++x) {
                    render::Point ra{}, rb{};
                    std::array<double, 3> va{}, vb{};
                    if (input.camera_a({double(x), double(y)}, ra) && input.camera_b({double(x), double(y)}, rb) &&
                        render::SampleBilinear(photo_a, ra.x, ra.y, va) && render::SampleBilinear(photo_b, rb.x, rb.y, vb)) { sum += std::abs(Luminance(va) - Luminance(vb)); ++n; }
                }
                return n ? sum / n : 0.0;
            });
        };
        const auto g_truth = ghost(truth_input, truth_column), g_similarity = ghost(similarity_input, similarity_column);
        std::cout << "luminance difference A-B at the seam (worst row mean of 7 columns)\n"
            << "  document mapping: " << g_truth.top << " / " << g_truth.middle << " / " << g_truth.bottom << '\n'
            << "  similarity:       " << g_similarity.top << " / " << g_similarity.middle << " / " << g_similarity.bottom << '\n';
        check(g_truth.top < 8 && g_truth.middle < 8 && g_truth.bottom < 8, "document mapping: A and B agree at the seam");
        check(g_similarity.top > 3 * g_truth.top && g_similarity.bottom > 3 * g_truth.bottom, "similarity: A and B disagree at both ends of the seam");

        // Render through the document entry.
        const auto sewn = RenderDocumentFixedSeam(photo_a, photo_b, truth, truth_column, settings);
        const auto sewn_similarity = RenderPairFixedSeam(photo_a, photo_b, similarity, similarity_column, settings);
        std::cout << "seam difference |A - ratio*B| mean / maximum: document " << sewn.record.difference_mean << " / " << sewn.record.difference_maximum
            << "   similarity " << sewn_similarity.record.difference_mean << " / " << sewn_similarity.record.difference_maximum
            << "   ratio " << sewn.record.ratio_minimum << ".." << sewn.record.ratio_maximum << '\n';
        check(sewn.record.fixed_seam && sewn.image.width == raster.width_pixels && sewn.image.height == raster.height_pixels, "document render has the raster size");
        check(sewn.record.seam_x.size() == raster.height_pixels && std::all_of(sewn.record.seam_x.begin(), sewn.record.seam_x.end(), [&](std::uint32_t x) { return x == truth_column; }), "document render: the seam column in every row");
        check(sewn.record.difference_mean * 3 < sewn_similarity.record.difference_mean && sewn.record.difference_maximum < sewn_similarity.record.difference_maximum,
            "the seam of the document render differs less than the similarity's");
        check(sewn.record.ratio_minimum > 0.97 && sewn.record.ratio_maximum < 1.03, "equal paper in both cameras gives a brightness ratio near one");
        check(Identical(sewn, RenderDocumentFixedSeam(photo_a, photo_b, truth, truth_column, settings)), "document render is deterministic");
        check(Identical(sewn, RenderMappedFixedSeam(photo_a, photo_b, truth_input, truth_column, settings)), "the document entry is the mapped entry on its mappings");

        // One sampling of the raw pixels: left of the blend the output is A at the mapped position, right of it B times the gain.
        {
            const double blend = settings.width_pixels;
            size_t wrong = 0, tested = 0;
            for (unsigned y = 0; y < raster.height_pixels; y += 7) for (unsigned x = 0; x < raster.width_pixels; x += 3) {
                const double seam = truth_column, target = std::clamp((x - seam) / blend + 0.5, 0.0, 1.0);
                if (target > 0 && target < 1) continue;
                render::Point raw{};
                std::array<double, 3> v{};
                const bool use_b = target >= 1;
                const auto& map = use_b ? truth_input.camera_b : truth_input.camera_a;
                if (!map({double(x), double(y)}, raw) || !render::SampleBilinear(use_b ? photo_b : photo_a, raw.x, raw.y, v)) continue;
                const double ratio = sewn.row_ratio[y], gain = use_b ? (x <= seam ? ratio : ratio + (1 - ratio) * (x - seam) / (raster.width_pixels - seam)) : 1.0;
                ++tested;
                for (int c = 0; c < 3; ++c) if (std::abs(int(sewn.image.bgr[(static_cast<size_t>(y) * raster.width_pixels + x) * 3 + c]) - std::lround(gain * v[c])) > 1) ++wrong;
            }
            check(tested > 5000 && wrong == 0, "outside the blend each output byte is one bilinear sample of one raw image at the mapped position");
        }

        // The columns that can be sewn, against the render, over the whole output.
        {
            const auto narrow = Sewable(photo_a, photo_b, truth_input, small);
            bool agrees = narrow.last > narrow.first + 10;
            for (const std::uint32_t column : {narrow.first - 1, narrow.first, narrow.last, narrow.last + 1}) {
                bool renders = true;
                try { (void)RenderDocumentFixedSeam(photo_a, photo_b, truth, column, small); } catch (const std::invalid_argument&) { renders = false; }
                agrees = agrees && CanRenderDocumentFixedSeam(photo_a, photo_b, truth, column, small) == renders && renders == (column >= narrow.first && column <= narrow.last);
            }
            check(agrees, "CanRenderDocumentFixedSeam agrees with RenderDocumentFixedSeam; the columns are those with half a blend width of both cameras on every row");
            reject([&] { (void)RenderDocumentFixedSeam(photo_a, photo_b, truth, 0, small); }, "a seam column left of the overlap fails");
            reject([&] { (void)RenderDocumentFixedSeam(photo_a, photo_b, truth, truth_input.width - 1, small); }, "a seam column right of the overlap fails");
            // The band is the brute-force run of every row, and each row is one run.
            const auto rows = MappedFixedSeamBand(photo_a, photo_b, truth_input);
            bool same = rows.size() == raster.height_pixels;
            unsigned widest = 0, narrowest = ~0u;
            for (unsigned y = 0; y < raster.height_pixels && same; y += 5) {
                unsigned first = ~0u, last = 0, count = 0;
                for (unsigned x = 0; x < raster.width_pixels; ++x) {
                    render::Point ra{}, rb{};
                    if (truth_input.camera_a({double(x), double(y)}, ra) && ra.x >= 0 && ra.y >= 0 && ra.x < cw && ra.y < ch &&
                        truth_input.camera_b({double(x), double(y)}, rb) && rb.x >= 0 && rb.y >= 0 && rb.x < cw && rb.y < ch) { first = std::min(first, x); last = x; ++count; }
                }
                same = same && rows[y].first == first && rows[y].end == last + 1 && count == last + 1 - first;
                widest = std::max(widest, rows[y].end - rows[y].first); narrowest = std::min(narrowest, rows[y].end - rows[y].first);
            }
            std::cout << "overlap run per row of the distortion scene: " << narrowest << ".." << widest << " columns\n";
            check(same && narrowest > 100, "MappedFixedSeamBand on the document mapping is the covered run of every row");
        }

        // ---- The geometry holds for a reduced output (the form the live view takes) ----
        {
            // Frames reduced by two with a box filter: reduced pixel j shows raw pixels 2j and 2j+1, centred on raw
            // coordinate 2j+0.5 (a pixel value sits at its integer coordinate). The reduced output is the same document
            // area with pixels twice as large: reduced pixel i is full-size output position 2i+0.5; S sends a raw position
            // r to (r-0.5)/2.
            const auto reduce = [](const BgrImage& image) {
                BgrImage out{image.width / 2, image.height / 2, std::vector<std::uint8_t>(static_cast<size_t>(image.width / 2) * (image.height / 2) * 3)};
                for (unsigned y = 0; y < out.height; ++y) for (unsigned x = 0; x < out.width; ++x) for (int c = 0; c < 3; ++c) {
                    const auto at = [&](unsigned i, unsigned j) { return int(image.bgr[(static_cast<size_t>(j) * image.width + i) * 3 + c]); };
                    out.bgr[(static_cast<size_t>(y) * out.width + x) * 3 + c] = static_cast<std::uint8_t>((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
                return out;
            };
            const auto live_a = reduce(photo_a), live_b = reduce(photo_b);
            const auto reduced_map = [](render::OutputToCamera full, double error) {
                return render::OutputToCamera([full, error](render::Point p, render::Point& raw) {
                    render::Point still{};
                    if (!full({2 * p.x + 0.5, 2 * p.y + 0.5}, still)) return false;
                    raw = {(still.x - 0.5) / 2 + error, (still.y - 0.5) / 2};
                    return true;
                });
            };
            const MappedFixedSeamInput live{truth_input.width / 2, truth_input.height / 2, reduced_map(truth_input.camera_a, 0), reduced_map(truth_input.camera_b, 0)};
            // The same with B read one reduced pixel off: what a wrong correspondence between the frames looks like.
            const MappedFixedSeamInput live_off{live.width, live.height, live.camera_a, reduced_map(truth_input.camera_b, 1)};
            const SeamBlendSettings live_settings{4, 8, 16, 64};
            // The full-size seam column c lies at reduced column (c+0.5)/2-0.5; try the nearest columns.
            const double target = (truth_column + 0.5) / 2 - 0.5;
            std::uint32_t live_column = 0;
            for (const int offset : {0, 1, -1, 2, -2}) {
                const auto c = static_cast<std::uint32_t>(std::lround(target) + offset);
                if (!live_column && CanRenderMappedFixedSeam(live_a, live_b, live, c, live_settings)) live_column = c;
            }
            check(live_column && std::abs(double(live_column) - target) <= 2, "a reduced seam column can be sewn within two columns of the scaled column");
            const auto reduced = RenderMappedFixedSeam(live_a, live_b, live, live_column, live_settings);
            const auto full = RenderMappedFixedSeam(photo_a, photo_b, truth_input, truth_column, kSeamBlendDesign);
            // The full-size render, reduced by the same box filter, against the reduced render, near the seam.
            const auto distance = [&](const SeamBlendResult& other) {
                double sum = 0;
                size_t n = 0;
                for (unsigned y = 0; y < live.height; ++y) for (unsigned x = live_column - 24; x < live_column + 24; ++x) for (int c = 0; c < 3; ++c) {
                    const auto at = [&](unsigned i, unsigned j) { return int(full.image.bgr[(static_cast<size_t>(j) * full.image.width + i) * 3 + c]); };
                    const double box = (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1)) / 4.0;
                    sum += std::abs(box - other.image.bgr[(static_cast<size_t>(y) * live.width + x) * 3 + c]);
                    ++n;
                }
                return sum / n;
            };
            const auto off = CanRenderMappedFixedSeam(live_a, live_b, live_off, live_column, live_settings)
                ? RenderMappedFixedSeam(live_a, live_b, live_off, live_column, live_settings) : reduced;
            const double agree = distance(reduced), disagree = distance(off);
            std::cout << "reduced vs full-size render near the seam: mean absolute difference " << agree << " levels; with B one reduced pixel off " << disagree << '\n';
            check(reduced.image.width == live.width && reduced.image.height == live.height && reduced.record.fixed_seam, "reduced render size");
            check(agree < 4 && disagree > 2 * agree, "the reduced mapped render agrees with the full-size one near the seam, and a one-pixel error in B shows");
        }

        // ---- Failures ----
        {
            const auto fa = Flat(200, 80, {200, 200, 200}), fb = Flat(200, 80, {100, 200, 238});
            const auto input = PairMappedFixedSeamInput(200, 80, 200, 80, Shift(100));
            check(RenderMappedFixedSeam(fa, fb, input, 150, small).record.seam_x.front() == 150, "a flat pair renders through its mappings");
            // The columns whose weight is strictly between 0 and 1 must be covered by both cameras.
            for (const std::uint32_t column : {0u, 50u, 99u, 100u, 199u, 200u, 299u, 300u, 4000000000u, 0xffffffffu})
                reject([&] { (void)RenderMappedFixedSeam(fa, fb, input, column, small); }, "mapped: seam column out of the overlap (width 4)");
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, input, 150, {4, 8, 8, 40000}); }, "mapped: invalid settings");
            auto broken = fb;
            broken.bgr.pop_back();
            reject([&] { (void)RenderMappedFixedSeam(fa, broken, input, 150, small); }, "mapped: broken buffer");
            reject([&] { (void)MappedFixedSeamBand(fa, broken, input); }, "mapped band: broken buffer");
            auto no_b = input;
            no_b.camera_b = nullptr;
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, no_b, 150, small); }, "mapped: missing mapping");
            auto empty = input;
            empty.width = 0;
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, empty, 150, small); }, "mapped: empty output");
            auto huge = input;
            huge.width = 100000;
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, huge, 150, small); }, "mapped: output too large");
            reject([&] { (void)CanRenderMappedFixedSeam(fa, fb, input, 150, {4, 8, 8, 40000}); }, "mapped can: invalid settings");
            // No row covered by both.
            auto disjoint = input;
            disjoint.camera_b = [](render::Point, render::Point& raw) { raw = {-5, -5}; return true; };
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, disjoint, 150, small); }, "mapped: no overlap");
            // A non-finite position is an error, not a missing sample.
            auto not_finite = input;
            not_finite.camera_b = [](render::Point p, render::Point& raw) { raw = {p.x - 100, p.y}; if (p.x == 150 && p.y == 40) raw.x = std::numeric_limits<double>::quiet_NaN(); return true; };
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, not_finite, 150, small); }, "mapped: a non-finite mapped position fails");
            // An output pixel covered by neither camera outside the overlap.
            auto wider = input;
            wider.width = 310;
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, wider, 150, small); }, "mapped: an uncovered output pixel fails");
            // A covered row that is not one run.
            auto holed = input;
            holed.camera_b = [base = input.camera_b](render::Point p, render::Point& raw) { return !(p.y == 30 && p.x >= 140 && p.x < 144) && base(p, raw); };
            reject([&] { (void)RenderMappedFixedSeam(fa, fb, holed, 150, small); }, "mapped: a row with a hole in its overlap fails");
            reject([&] { (void)CanRenderMappedFixedSeam(fa, fb, holed, 150, small); }, "mapped can: a row with a hole in its overlap fails");
            check(MappedFixedSeamBand(fa, fb, holed)[30].first == 100, "the band reports the run that holds the first covered column");
            // Document entry: unsupported layout and kernel, projection that does not validate.
            const render::OutputRaster tiny{{0, 0, 3000, 2000}, 254, 30, 20};
            const render::CameraProjection ok{{1, 1, 0, 0}, {0, 0, 0, 0, 0}, {10, 0, 0, 0, 10, 0, 0, 0, 1}};
            const auto ra = Flat(30, 20, {10, 10, 10});
            const render::DocumentRenderParameters good{tiny, ok, ok, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear};
            check(DocumentMappedFixedSeamInput(good).width == 30, "document input takes the raster size");
            reject([&] { (void)DocumentMappedFixedSeamInput({tiny, ok, ok, a0::m2::StitchLayout::camera_a_top_camera_b_bottom, render::Resampling::bilinear}); }, "document: top/bottom layout");
            reject([&] { (void)DocumentMappedFixedSeamInput({tiny, ok, ok, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bicubic_catmull_rom}); }, "document: bicubic kernel");
            const render::CameraProjection folded{{1, 1, 0, 0}, {0, 0, 0, 0, 0}, {10, 0, 0, 0, 10, 0, -1, 0, 1}};
            reject([&] { (void)DocumentMappedFixedSeamInput({tiny, ok, folded, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear}); }, "document: projective denominator not positive over the raster");
            reject([&] { (void)RenderDocumentFixedSeam(ra, ra, {tiny, ok, ok, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bicubic_catmull_rom}, 15, small); }, "document render: bicubic kernel");
        }
    } catch (const std::exception& e) { ++failures; std::cerr << e.what() << '\n'; }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
