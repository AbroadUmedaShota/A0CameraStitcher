#pragma once

// Test-only picture generator for the grid sheet tilt tests (#440 stage 2): a square-grid sheet seen by a camera that
// stands in a known pose, with the lens distortion of a rig profile. It shares no code with the product: the lens
// inverse, the plane intersection, the line profile and the truth are written here. Nothing in it reads a file.
//
// Frames. The document frame has x to the right, y down and z into the table (right-handed). A camera frame has x right,
// y down and z forward. A pose maps document points to camera points: p_camera = R p_document + T. The sheet's lattice is
// the document plane turned by `mat_deg` and shifted: lattice = Rm (X, Y) / pitch + offset, in units of one pitch.

#include "a0/m2/document_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

namespace grid_scene {

namespace render = a0::m2::render;
constexpr double kDeg = std::numbers::pi / 180.0;

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>;

inline Mat3 Mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[3 * i + j] += a[3 * i + k] * b[3 * k + j];
    return r;
}
inline Vec3 Apply(const Mat3& m, const Vec3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}
inline Mat3 Transpose(const Mat3& m) { return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; }
inline Mat3 RotX(double a) { return {1, 0, 0, 0, std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a)}; }
inline Mat3 RotY(double a) { return {std::cos(a), 0, std::sin(a), 0, 1, 0, -std::sin(a), 0, std::cos(a)}; }
inline Mat3 RotZ(double a) { return {std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1}; }

// A camera of the rig: the picture size and the intrinsics scale with `scale` (1 is the full 7360 x 4912 picture).
struct Rig {
    double scale{1};
    std::uint32_t width{}, height{};
    double f{}, cx{}, cy{};
    render::LensDistortion lens{-0.09, 0.04, 0.0, 0.0004, -0.0003};
};
inline Rig MakeRig(double scale) {
    Rig r;
    r.scale = scale;
    r.width = static_cast<std::uint32_t>(std::lround(7360 * scale));
    r.height = static_cast<std::uint32_t>(std::lround(4912 * scale));
    r.f = 10250.0 * scale;
    r.cx = r.width / 2.0; r.cy = r.height / 2.0;
    return r;
}

struct Pose {
    Mat3 R{};
    Vec3 T{};
};
// The camera centre over document point (x, y) at `height` mm, its image axes turned `mount_deg` about the optical axis, then
// turned about its own centre by (rx, ry, rz) degrees about its own axes.
inline Pose MakePose(double x_mm, double y_mm, double height_mm, double mount_deg, double rx_deg, double ry_deg, double rz_deg = 0) {
    const Mat3 r0 = RotZ(mount_deg * kDeg);
    const Vec3 centre{x_mm, y_mm, -height_mm};
    const Vec3 t0 = Apply(r0, centre);
    const Mat3 p = Mul(RotX(rx_deg * kDeg), Mul(RotY(ry_deg * kDeg), RotZ(rz_deg * kDeg)));
    Pose pose;
    pose.R = Mul(p, r0);
    // p_camera = P (R0 (X - C)) = (P R0) X - P R0 C
    pose.T = Apply(p, {-t0[0], -t0[1], -t0[2]});
    return pose;
}

// The projection a rig profile would hold for a pose: document mm to the undistorted pixel, normalised to 1 in the corner.
inline render::CameraProjection ProjectionOf(const Rig& rig, const Pose& pose) {
    const Mat3 k{rig.f, 0, rig.cx, 0, rig.f, rig.cy, 0, 0, 1};
    const Mat3 plane{pose.R[0], pose.R[1], pose.T[0], pose.R[3], pose.R[4], pose.T[1], pose.R[6], pose.R[7], pose.T[2]};
    Mat3 h = Mul(k, plane);
    const double s = h[8];
    for (auto& v : h) v /= s;
    return render::CameraProjection(render::CameraIntrinsics(rig.f, rig.f, rig.cx, rig.cy), rig.lens, h);
}

// ---- the truth, from the geometry alone ------------------------------------------------------------------------------
struct Truth {
    double tilt_deg{};                    // angle between the optical axis and the sheet's normal
    render::Point lean_doc_deg{};         // angle x unit direction over the document, from the point under the camera to where the axis hits
    render::Point lean_screen_deg{};      // the same along the picture's own axes (right, down)
    double phi_deg{};                     // the picture's x axis over the document, from x toward y
    Vec3 axis_doc{};                      // optical axis, document frame, toward the table
    double height_mm{};
};
inline Truth TruthOf(const Pose& pose) {
    const Mat3 rt = Transpose(pose.R);
    const Vec3 t = pose.T;
    const Vec3 c{-(rt[0] * t[0] + rt[1] * t[1] + rt[2] * t[2]), -(rt[3] * t[0] + rt[4] * t[1] + rt[5] * t[2]), -(rt[6] * t[0] + rt[7] * t[1] + rt[8] * t[2])};
    const Vec3 d = Apply(rt, {0, 0, 1});
    Truth out;
    out.axis_doc = d;
    out.height_mm = -c[2];
    out.tilt_deg = std::acos(std::clamp(d[2], -1.0, 1.0)) / kDeg;
    const double s = -c[2] / d[2];
    const double hit_x = c[0] + s * d[0], hit_y = c[1] + s * d[1];
    const double lean_x = hit_x - c[0], lean_y = hit_y - c[1];
    const double angle = std::atan2(std::hypot(lean_x, lean_y), out.height_mm) / kDeg;
    const double n = std::hypot(lean_x, lean_y);
    out.lean_doc_deg = n > 0 ? render::Point{angle * lean_x / n, angle * lean_y / n} : render::Point{0, 0};
    const Vec3 ray = Apply(rt, {0.01, 0, 1});
    const double s2 = -c[2] / ray[2];
    out.phi_deg = std::atan2(c[1] + s2 * ray[1] - hit_y, c[0] + s2 * ray[0] - hit_x) / kDeg;
    const double cphi = std::cos(out.phi_deg * kDeg), sphi = std::sin(out.phi_deg * kDeg);
    // the lean along the image axes: x axis (cos, sin), y axis (-sin, cos) in the document plane (the pose is right handed)
    out.lean_screen_deg = {out.lean_doc_deg.x * cphi + out.lean_doc_deg.y * sphi, -out.lean_doc_deg.x * sphi + out.lean_doc_deg.y * cphi};
    return out;
}

// ---- the picture ----------------------------------------------------------------------------------------------------
struct Segment {            // a bright line segment in picture pixels
    double x0, y0, x1, y1, amplitude, sigma;
};
struct Scene {
    Rig rig;
    Pose pose;
    double mat_deg{0}, pitch_mm{10}, offset_x{3.37}, offset_y{1.91};
    double background{40}, amplitude{60}, line_sigma{1.15}, thick_sigma{1.6}, thick_gain{1.5};
    int thick_every{5};
    double noise{1.5};
    double region[4]{0, 0, 1, 1};     // left, top, right, bottom as fractions of the picture: where the sheet is visible
    bool fold{false};
    double fold_x_mm{0}, fold_deg{0}; // the sheet right of X = fold_x_mm is bent out of the table plane toward the camera
    bool lines_only_in_first_family{false};
    std::vector<Segment> extra;
    std::uint64_t seed{1};
};

class Renderer {
public:
    explicit Renderer(const Scene& scene) : s_(scene), rt_(Transpose(scene.pose.R)) {
        const auto& t = scene.pose.T;
        centre_ = {-(rt_[0] * t[0] + rt_[1] * t[1] + rt_[2] * t[2]), -(rt_[3] * t[0] + rt_[4] * t[1] + rt_[5] * t[2]), -(rt_[6] * t[0] + rt_[7] * t[1] + rt_[8] * t[2])};
        const double m = scene.mat_deg * kDeg;
        rm_ = {std::cos(m), std::sin(m), -std::sin(m), std::cos(m)};
    }

    render::BgrImage Render() const {
        const auto& rig = s_.rig;
        render::BgrImage image;
        image.width = rig.width; image.height = rig.height;
        image.bgr.assign(static_cast<std::size_t>(rig.width) * rig.height * 3, 0);
        const int w = static_cast<int>(rig.width), h = static_cast<int>(rig.height);
        std::vector<double> lx0(w + 1), ly0(w + 1), lx1(w + 1), ly1(w + 1);
        std::vector<std::uint8_t> ok0(w + 1), ok1(w + 1);
        LatticeRow(0, lx0, ly0, ok0);
        const int u_lo = static_cast<int>(s_.region[0] * w), u_hi = static_cast<int>(s_.region[2] * w);
        const int v_lo = static_cast<int>(s_.region[1] * h), v_hi = static_cast<int>(s_.region[3] * h);
        for (int v = 0; v < h; ++v) {
            LatticeRow(v + 1, lx1, ly1, ok1);
            std::mt19937_64 rng(s_.seed * 1000003ull + static_cast<std::uint64_t>(v));
            std::normal_distribution<double> noise(0.0, s_.noise);
            for (int u = 0; u < w; ++u) {
                double value = s_.background;
                const bool inside = ok0[u] && ok0[u + 1] && ok1[u] && u >= u_lo && u < u_hi && v >= v_lo && v < v_hi;
                if (inside) {
                    for (int family = 0; family < 2; ++family) {
                        if (family == 1 && s_.lines_only_in_first_family) continue;
                        const double l0 = family == 0 ? lx0[u] : ly0[u];
                        const double lu = family == 0 ? lx0[u + 1] : ly0[u + 1];
                        const double lv = family == 0 ? lx1[u] : ly1[u];
                        const double gradient = std::hypot(lu - l0, lv - l0);
                        if (!(gradient > 0)) continue;
                        const double k = std::nearbyint(l0);
                        const double distance = std::abs(l0 - k) / gradient;
                        const bool thick = s_.thick_every > 0 && (static_cast<long long>(k) % s_.thick_every == 0);
                        const double sigma = thick ? s_.thick_sigma : s_.line_sigma;
                        if (distance > 6 * sigma) continue;
                        value += (thick ? s_.amplitude * s_.thick_gain : s_.amplitude) * std::exp(-distance * distance / (2 * sigma * sigma));
                    }
                }
                for (const auto& e : s_.extra) {
                    const double dx = e.x1 - e.x0, dy = e.y1 - e.y0, len2 = dx * dx + dy * dy;
                    const double t = ((u - e.x0) * dx + (v - e.y0) * dy) / len2;
                    if (t < 0 || t > 1) continue;
                    const double d = ((u - e.x0) * dy - (v - e.y0) * dx) / std::sqrt(len2);
                    if (std::abs(d) > 6 * e.sigma) continue;
                    value += e.amplitude * std::exp(-d * d / (2 * e.sigma * e.sigma));
                }
                value += noise(rng);
                const auto byte = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 255l));
                std::uint8_t* px = image.bgr.data() + (static_cast<std::size_t>(v) * w + u) * 3;
                px[0] = px[1] = px[2] = byte;
            }
            std::swap(lx0, lx1); std::swap(ly0, ly1); std::swap(ok0, ok1);
        }
        return image;
    }

    // Lattice coordinates of the sheet point seen at picture position (u, v) (fractional allowed); false when the ray misses.
    bool LatticeAt(double u, double v, double& lx, double& ly) const {
        const auto& rig = s_.rig;
        const double xd = (u - rig.cx) / rig.f, yd = (v - rig.cy) / rig.f;
        double x = xd, y = yd;
        const auto& d = rig.lens;
        for (int i = 0; i < 25; ++i) {
            const double r2 = x * x + y * y;
            const double radial = 1 + d.k1 * r2 + d.k2 * r2 * r2 + d.k3 * r2 * r2 * r2;
            const double fx = x * radial + 2 * d.p1 * x * y + d.p2 * (r2 + 2 * x * x);
            const double fy = y * radial + d.p1 * (r2 + 2 * y * y) + 2 * d.p2 * x * y;
            const double ex = xd - fx, ey = yd - fy;
            x += ex; y += ey;
            if (std::abs(ex) < 1e-13 && std::abs(ey) < 1e-13) break;
        }
        const Vec3 dir = Apply(rt_, {x, y, 1});
        if (!(dir[2] > 1e-9)) return false;
        const double t1 = -centre_[2] / dir[2];
        double hx = centre_[0] + t1 * dir[0], hy = centre_[1] + t1 * dir[1];
        if (s_.fold && hx > s_.fold_x_mm) {
            const double b = s_.fold_deg * kDeg;
            const Vec3 n{std::sin(b), 0, std::cos(b)};
            const double den = n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2];
            if (std::abs(den) > 1e-12) {
                const double t2 = (n[0] * (s_.fold_x_mm - centre_[0]) + n[1] * (0 - centre_[1]) + n[2] * (0 - centre_[2])) / den;
                if (t2 > 0) {
                    const double px = centre_[0] + t2 * dir[0], py = centre_[1] + t2 * dir[1], pz = centre_[2] + t2 * dir[2];
                    const double s = (px - s_.fold_x_mm) * std::cos(b) + pz * -std::sin(b);
                    if (s > 0) { hx = s_.fold_x_mm + s; hy = py; }
                }
            }
        }
        lx = (rm_[0] * hx + rm_[1] * hy) / s_.pitch_mm + s_.offset_x;
        ly = (rm_[2] * hx + rm_[3] * hy) / s_.pitch_mm + s_.offset_y;
        return std::isfinite(lx) && std::isfinite(ly);
    }

private:
    void LatticeRow(int v, std::vector<double>& lx, std::vector<double>& ly, std::vector<std::uint8_t>& ok) const {
        const int w = static_cast<int>(s_.rig.width);
        for (int u = 0; u <= w; ++u) ok[u] = LatticeAt(u, v, lx[u], ly[u]) ? 1 : 0;
    }

    Scene s_;
    Mat3 rt_{};
    Vec3 centre_{};
    std::array<double, 4> rm_{};
};

}   // namespace grid_scene
