// Contracts of the registered mapping of camera B (#417, docs/design/fixed-composite-registration.md 11.2 to 11.4 and 11.13):
// the camera centres, Pi(pi) of the paper surface and G(p) Pi(pi), checked against a ray construction that does not use the
// code under test; the makers of fixed_composite_maps with layer 2 (w, q(y), the strain) and their bit-for-bit zero components.
// Camera, file and clock free; no random numbers.
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace cr = a0::m2::experimental::content_registration;
namespace m2x = a0::m2::experimental;
namespace render = a0::m2::render;
using render::Point;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const std::string& what) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << what << '\n'; }
}
void CheckNear(double value, double expected, double tolerance, const std::string& what) {
    ++checks;
    if (!(std::abs(value-expected) <= tolerance)) {
        ++failures;
        std::cerr << "FAIL " << what << ": " << value << " expected " << expected << " +/- " << tolerance << '\n';
    }
}
bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool SameMatrix(const render::Matrix3& a, const render::Matrix3& b) {
    for (int i = 0; i < 9; ++i) if (!SameBits(a[i], b[i])) return false;
    return true;
}
template <class F> bool Throws(F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
constexpr double kPi = std::numbers::pi;
constexpr double kDegree = kPi/180;

using Vec3 = std::array<double,3>;
using Mat3 = std::array<double,9>;
Mat3 Mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[3*i+j] += a[3*i+k]*b[3*k+j];
    return r;
}
Vec3 Apply(const Mat3& m, const Vec3& v) {
    return {m[0]*v[0]+m[1]*v[1]+m[2]*v[2], m[3]*v[0]+m[4]*v[1]+m[5]*v[2], m[6]*v[0]+m[7]*v[1]+m[8]*v[2]};
}
Vec3 Cross(const Vec3& a, const Vec3& b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
double Dot(const Vec3& a, const Vec3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Mat3 RotationX(double a) { return {1,0,0,0,std::cos(a),-std::sin(a),0,std::sin(a),std::cos(a)}; }
Mat3 RotationY(double a) { return {std::cos(a),0,std::sin(a),0,1,0,-std::sin(a),0,std::cos(a)}; }
Mat3 RotationZ(double a) { return {std::cos(a),-std::sin(a),0,std::sin(a),std::cos(a),0,0,0,1}; }
Point Homogeneous(const render::Matrix3& m, Point p) {
    const double w = m[6]*p.x+m[7]*p.y+m[8];
    return {(m[0]*p.x+m[1]*p.y+m[2])/w, (m[3]*p.x+m[4]*p.y+m[5])/w};
}

// ---- A synthetic rig whose cameras are known as rigid 3D cameras -------------------------------------------------------------
// The table frame of the test: (x, y) document mm and a height h toward the cameras. A camera sees the table point at height h
// over (x, y) at the camera-frame point r1 x + r2 y - h r3 + t, where r1 r2 r3 are the columns of a rotation (r3 = r1 x r2 points
// away from the camera). This is the test's own model; the code under test only sees the document-to-image matrix.
constexpr unsigned kWidth = 1200, kHeight = 800;
constexpr double kFocal = 1560;
const render::LensDistortion kLens{-0.035, 0.012, 0.0, 0.0005, -0.0003};

struct RigidCamera {
    Vec3 r1{}, r2{}, r3{}, t{};
    double fx{kFocal}, fy{kFocal}, cx{(kWidth-1)/2.0}, cy{(kHeight-1)/2.0};
    [[nodiscard]] Vec3 CameraPoint(double x, double y, double h) const {
        return {r1[0]*x+r2[0]*y-h*r3[0]+t[0], r1[1]*x+r2[1]*y-h*r3[1]+t[1], r1[2]*x+r2[2]*y-h*r3[2]+t[2]};
    }
    // The centre in the table frame: the point whose camera-frame position is 0.
    [[nodiscard]] Vec3 Centre() const {
        // r1 x + r2 y - h r3 = -t with orthonormal columns: x = -r1.t, y = -r2.t, h = r3.t.
        return {-Dot(r1, t), -Dot(r2, t), Dot(r3, t)};
    }
    [[nodiscard]] Point Undistorted(const Vec3& c) const { return {fx*c[0]/c[2]+cx, fy*c[1]/c[2]+cy}; }
    [[nodiscard]] Point Raw(const Vec3& c) const {
        const double x = c[0]/c[2], y = c[1]/c[2], rr = x*x+y*y;
        const double radial = 1+kLens.k1*rr+kLens.k2*rr*rr+kLens.k3*rr*rr*rr;
        const double xd = x*radial+2*kLens.p1*x*y+kLens.p2*(rr+2*x*x), yd = y*radial+kLens.p1*(rr+2*y*y)+2*kLens.p2*x*y;
        return {fx*xd+cx, fy*yd+cy};
    }
    [[nodiscard]] render::CameraProjection Projection() const {
        const Mat3 k{fx,0,cx,0,fy,cy,0,0,1};
        const Mat3 columns{r1[0],r2[0],t[0],r1[1],r2[1],t[1],r1[2],r2[2],t[2]};
        auto h = Mul(k, columns);
        const double n = h[8];
        for (auto& v : h) v /= n;
        h[8] = 1;
        return render::CameraProjection(render::CameraIntrinsics(fx,fy,cx,cy), kLens, h);
    }
};
// Over the document point (x0, y0) at the given depth, rolled about the optical axis and tilted (degrees).
RigidCamera MakeCamera(double x0, double y0, double depth, double roll, double tilt_x, double tilt_y) {
    const auto rotation = Mul(RotationZ(roll*kDegree), Mul(RotationX(tilt_x*kDegree), RotationY(tilt_y*kDegree)));
    RigidCamera c;
    c.r1 = {rotation[0], rotation[3], rotation[6]};
    c.r2 = {rotation[1], rotation[4], rotation[7]};
    c.r3 = Cross(c.r1, c.r2);
    for (int i = 0; i < 3; ++i) c.t[i] = -x0*c.r1[i]-y0*c.r2[i]+(i == 2 ? depth : 0.0);
    return c;
}
// The rigid camera behind a document-to-image matrix with exactly orthonormal columns (the camera moved by a pose):
// K^-1 H, scaled so that the first column has unit length and the plane is in front.
RigidCamera FromHomography(const render::Matrix3& h) {
    RigidCamera c;
    const Mat3 k_inverse{1/c.fx,0,-c.cx/c.fx,0,1/c.fy,-c.cy/c.fy,0,0,1};
    const auto raw = Mul(k_inverse, {h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8]});
    const Vec3 c0{raw[0],raw[3],raw[6]}, c1{raw[1],raw[4],raw[7]}, c2{raw[2],raw[5],raw[8]};
    double s = 1/std::sqrt(Dot(c0, c0));
    if (c2[2]*s < 0) s = -s;
    for (int i = 0; i < 3; ++i) { c.r1[i] = c0[i]*s; c.r2[i] = c1[i]*s; c.t[i] = c2[i]*s; }
    c.r3 = Cross(c.r1, c.r2);
    return c;
}

render::OutputRaster MakeRaster() {
    const std::int64_t left = 10000, top = 10000, right = 780000, bottom = 350000;
    const std::uint32_t dpi = 48;
    const auto pixels = [dpi](std::int64_t extent) {return static_cast<std::uint32_t>((extent*dpi+25399)/25400);};
    return render::OutputRaster(render::DocumentRegionUm(left, top, right, bottom), dpi, pixels(right-left), pixels(bottom-top));
}

struct Rig {
    render::OutputRaster raster = MakeRaster();
    // B is higher than A and offset in y, so the heights differ and the baseline is not along x.
    RigidCamera a = MakeCamera(270, 180, 700, 0.3, -0.15, 0.1);
    RigidCamera b = MakeCamera(520, 186, 712, -0.4, 0.2, -0.1);
    render::CameraProjection projection_a = a.Projection(), projection_b = b.Projection();
    std::uint32_t seam_column = 727;
};

// The paper point that camera c's ray through the table point (x, y) meets on the plane, by the test's own construction:
// the ray is C + s ((x, y, 0) - C); the height along it, H (1 - s), equals the plane's height at the ray's (x, y).
// Solved for s by bisection, so that it shares no algebra with the closed form under test.
Vec3 PaperPoint(const Vec3& centre, Point x, const m2x::SurfacePlane& plane, Point origin) {
    const auto at = [&](double s) {
        const double px = centre[0]+s*(x.x-centre[0]), py = centre[1]+s*(x.y-centre[1]), pz = centre[2]*(1-s);
        return pz-(plane.h0_mm+plane.slope_x*(px-origin.x)+plane.slope_y*(py-origin.y));
    };
    double lo = 0, hi = 2;   // at(0) = H - h(C) > 0; at(2) < 0 for the planes of these tests
    for (int i = 0; i < 200; ++i) { const double mid = 0.5*(lo+hi); (at(mid) > 0 ? lo : hi) = mid; }
    const double s = 0.5*(lo+hi);
    return {centre[0]+s*(x.x-centre[0]), centre[1]+s*(x.y-centre[1]), centre[2]*(1-s)};
}
// The table point on the ray from the centre through the paper point.
Point TablePoint(const Vec3& centre, const Vec3& p) {
    const double s = centre[2]/(centre[2]-p[2]);
    return {centre[0]+s*(p[0]-centre[0]), centre[1]+s*(p[1]-centre[1])};
}

const std::vector<m2x::SurfacePlane>& Planes() {
    static const std::vector<m2x::SurfacePlane> planes{
        {40, 0, 0}, {-6, 0, 0}, {30, std::tan(6*kDegree), 0}, {25, 0, std::tan(-4*kDegree)}, {12, -0.05, 0.03}, {0, 0.08, 0}, {0, 0, 0.06}};
    return planes;
}
std::string Name(const m2x::SurfacePlane& s) {
    return "h0 " + std::to_string(s.h0_mm) + " gx " + std::to_string(s.slope_x) + " gy " + std::to_string(s.slope_y);
}

// ---- Camera centres and the plane origin -------------------------------------------------------------------------------
void Centres() {
    const Rig rig;
    for (const auto* camera : {&rig.a, &rig.b}) {
        const auto truth = camera->Centre();
        const auto c = cr::PoseModel(camera->Projection()).Centre({});
        CheckNear(c.x_mm, truth[0], 1e-9, "calibrated centre x");
        CheckNear(c.y_mm, truth[1], 1e-9, "calibrated centre y");
        CheckNear(c.height_mm, truth[2], 1e-9, "calibrated centre height");
    }
    // A moved camera: the centre of the rigid camera behind the pose's document-to-image matrix.
    const cr::PoseModel model(rig.projection_b);
    for (const cr::PoseParameters& p : {cr::PoseParameters{0.4, -0.3, 0.9, 4.0, -3.0, 0.6}, cr::PoseParameters{1.2, 0, 0, 0, 0, 0},
             cr::PoseParameters{0, 0, 0, 0, 0, -1.5}, cr::PoseParameters{0, -0.8, 0, 2, 0, 0}}) {
        const auto truth = FromHomography(model.CameraHomography(p)).Centre();
        const auto c = model.Centre(p);
        const double error = std::max({std::abs(c.x_mm-truth[0]), std::abs(c.y_mm-truth[1]), std::abs(c.height_mm-truth[2])});
        CheckNear(error, 0, 1e-8, "the centre after a pose move is the centre of the moved camera (mm)");
    }
    // A tilt about the pivot moves the centre sideways, so Pi must use the moved centre (design 11.3, C_B from the pose).
    const auto still = model.Centre({}), tilted = model.Centre({1.0, 0, 0, 0, 0, 0});
    Check(std::hypot(tilted.x_mm-still.x_mm, tilted.y_mm-still.y_mm) > 5.0, "a 1 degree tilt moves B's centre by more than 5 mm");
    Check(Throws([&] { (void)model.Centre({0, 0, std::nan(""), 0, 0, 0}); }), "a nonfinite pose is refused by Centre");

    // The plane origin is the middle of the seam column.
    const auto frame = cr::FrameOfRaster(rig.raster);
    const auto origin = cr::SurfaceOriginMm(rig.raster, rig.seam_column);
    Check(SameBits(origin.x, frame.left_mm+(rig.seam_column+0.5)*frame.pitch_mm), "origin x is the document x of the seam column");
    Check(SameBits(origin.y, frame.top_mm+((rig.raster.height_pixels-1)/2.0+0.5)*frame.pitch_mm), "origin y is the middle row");
    Check(Throws([&] { (void)cr::SurfaceOriginMm(rig.raster, rig.raster.width_pixels); }), "a seam column outside the raster is refused");
}

// ---- Pi(pi) against the ray construction ---------------------------------------------------------------------------------
void SurfaceHomographyFollowsTheRays() {
    const Rig rig;
    const auto origin = cr::SurfaceOriginMm(rig.raster, rig.seam_column);
    const auto ca = cr::PoseModel(rig.projection_a).Centre({}), cb = cr::PoseModel(rig.projection_b).Centre({});
    const Vec3 ta = rig.a.Centre(), tb = rig.b.Centre();
    const auto frame = cr::FrameOfRaster(rig.raster);
    for (const auto& plane : Planes()) {
        const auto pi = cr::SurfaceHomography(ca, cb, origin, plane);
        double worst = 0;
        for (double y = 0; y < rig.raster.height_pixels; y += 53.5) for (double x = 400; x < 1100; x += 61.25) {
            const Point X{frame.left_mm+(x+0.5)*frame.pitch_mm, frame.top_mm+(y+0.5)*frame.pitch_mm};
            const Point truth = TablePoint(tb, PaperPoint(ta, X, plane, origin));
            const Point got = Homogeneous(pi, X);
            worst = std::max(worst, std::hypot(got.x-truth.x, got.y-truth.y));
            Check(cr::SurfaceHomographyHoldsAt(ca, cb, origin, plane, X), Name(plane) + ": Pi holds over the raster");
        }
        CheckNear(worst, 0, 1e-8, Name(plane) + ": Pi(X) is the table point of B's ray through the paper point (mm)");
        Check(std::abs(pi[8]-1) == 0, Name(plane) + ": Pi is normalized to [8] = 1");
    }

    // The departure from proportionality by 1/(H - h) is in Pi: at h0 = 40 mm the first-order displacement -b h0 / H is more
    // than one output px away from the ray construction, while Pi follows it.
    {
        const m2x::SurfacePlane plane{40, 0, 0};
        const Point X{frame.left_mm+(727+0.5)*frame.pitch_mm, frame.top_mm+(300+0.5)*frame.pitch_mm};
        const Point truth = TablePoint(tb, PaperPoint(ta, X, plane, origin));
        const double first_order_x = X.x-(tb[0]-ta[0])*plane.h0_mm/tb[2], first_order_y = X.y-(tb[1]-ta[1])*plane.h0_mm/tb[2];
        Check(std::hypot(first_order_x-truth.x, first_order_y-truth.y)/frame.pitch_mm > 1.0,
            "the first-order displacement misses the ray construction by more than 1 px at h0 = 40 mm");
        const Point got = Homogeneous(cr::SurfaceHomography(ca, cb, origin, plane), X);
        CheckNear(std::hypot(got.x-truth.x, got.y-truth.y), 0, 1e-8, "Pi keeps the 1/(H - h) term");
    }

    // Equal heights and a level surface: a pure translation by -(b - a) h0 / (H - h0) (design 11.2).
    {
        const RigidCamera a = MakeCamera(270, 180, 700, 0, 0, 0), b = MakeCamera(520, 186, 700, 0, 0, 0);
        const auto ia = cr::PoseModel(a.Projection()).Centre({}), ib = cr::PoseModel(b.Projection()).Centre({});
        const double h0 = 30;
        const auto pi = cr::SurfaceHomography(ia, ib, origin, {h0, 0, 0});
        CheckNear(pi[0], 1, 1e-12, "level surface, equal heights: no scale in x");
        CheckNear(pi[4], 1, 1e-12, "level surface, equal heights: no scale in y");
        CheckNear(std::abs(pi[1])+std::abs(pi[3])+std::abs(pi[6])+std::abs(pi[7]), 0, 1e-12, "level surface, equal heights: no shear or perspective");
        CheckNear(pi[2], -(ib.x_mm-ia.x_mm)*h0/(700-h0), 1e-9, "level surface, equal heights: the x shift is -b_x h0 / (H - h0)");
        CheckNear(pi[5], -(ib.y_mm-ia.y_mm)*h0/(700-h0), 1e-9, "level surface, equal heights: the y shift is -b_y h0 / (H - h0)");
    }

    // The zero plane is the identity up to rounding.
    const auto identity = cr::SurfaceHomography(ca, cb, origin, {});
    double off = 0;
    for (int i = 0; i < 9; ++i) off = std::max(off, std::abs(identity[i]-((i % 4) == 0 ? 1.0 : 0.0)));
    CheckNear(off, 0, 1e-12, "the zero plane gives the identity up to rounding");
    Check(m2x::SurfacePlane{}.IsZero() && !m2x::SurfacePlane{0, 0, 1e-300}.IsZero(), "the zero plane is decided on exact zeros");
}

// ---- G(p) Pi(pi): B moved and the paper raised together -------------------------------------------------------------------
void PoseAndSurfaceTogether() {
    const Rig rig;
    const cr::PoseModel model(rig.projection_b);
    const auto origin = cr::SurfaceOriginMm(rig.raster, rig.seam_column);
    const auto ca = cr::PoseModel(rig.projection_a).Centre({});
    const Vec3 ta = rig.a.Centre();
    const auto frame = cr::FrameOfRaster(rig.raster);
    const RigidCamera& calibrated_b = rig.b;
    for (const cr::PoseParameters& p : {cr::PoseParameters{0.4, -0.3, 0.9, 4.0, -3.0, 0.6}, cr::PoseParameters{1.0, 0, 0, 0, 0, 0},
             cr::PoseParameters{0, 0.7, -0.5, 0, 2.5, -0.8}}) {
        const RigidCamera moved = FromHomography(model.CameraHomography(p));
        for (const auto& plane : Planes()) {
            const auto h = cr::RegisteredHomography(model, ca, origin, p, plane);
            double worst = 0;
            for (double y = 20; y < rig.raster.height_pixels; y += 77.5) for (double x = 480; x < 1000; x += 83.0) {
                const Point X{frame.left_mm+(x+0.5)*frame.pitch_mm, frame.top_mm+(y+0.5)*frame.pitch_mm};
                // The moved B sees the paper point P; B's calibrated projection of G(p) Pi(pi) (X) must land on the same
                // undistorted pixel.
                const Vec3 paper = PaperPoint(ta, X, plane, origin);
                const Point truth = moved.Undistorted(moved.CameraPoint(paper[0], paper[1], paper[2]));
                const Point y_doc = Homogeneous(h, X);
                const Point got = calibrated_b.Undistorted(calibrated_b.CameraPoint(y_doc.x, y_doc.y, 0));
                worst = std::max(worst, std::hypot(got.x-truth.x, got.y-truth.y));
            }
            CheckNear(worst, 0, 1e-7, "pose and " + Name(plane) + ": G(p) Pi(pi) puts the moved camera's view of the paper back (undistorted px)");
        }
    }
    // Built with the calibrated centre of B instead of the moved one, the same check is off by more than 0.5 px for a tilt
    // of 1 degree and h0 = 30 mm: the centre after the move is what makes the composition exact.
    {
        const cr::PoseParameters p{1.0, 0, 0, 0, 0, 0};
        const m2x::SurfacePlane plane{30, 0, 0};
        const RigidCamera moved = FromHomography(model.CameraHomography(p));
        const render::Matrix3 wrong = Mul(model.Homography(p), cr::SurfaceHomography(ca, model.Centre({}), origin, plane));
        const Point X{frame.left_mm+(727+0.5)*frame.pitch_mm, frame.top_mm+(300+0.5)*frame.pitch_mm};
        const Vec3 paper = PaperPoint(ta, X, plane, origin);
        const Point truth = moved.Undistorted(moved.CameraPoint(paper[0], paper[1], paper[2]));
        const Point y_doc = Homogeneous(wrong, X);
        const Point got = calibrated_b.Undistorted(calibrated_b.CameraPoint(y_doc.x, y_doc.y, 0));
        Check(std::hypot(got.x-truth.x, got.y-truth.y) > 0.5, "the calibrated centre of B would leave more than 0.5 px");
    }
    // Zero components give the existing functions exactly.
    for (const cr::PoseParameters& p : {cr::PoseParameters{}, cr::PoseParameters{0.4, -0.3, 0.9, 4.0, -3.0, 0.6}}) {
        Check(SameMatrix(cr::RegisteredHomography(model, ca, origin, p, {}), model.Homography(p)), "a zero surface gives Homography(p) bit for bit");
        bool same = true;
        for (double y = 0; y < rig.raster.height_pixels; y += 91) for (double x = 0; x < rig.raster.width_pixels; x += 97) {
            const auto r = cr::RegisteredDisplacement(model, frame, ca, origin, p, {}, x, y), q = cr::PoseDisplacement(model, frame, p, x, y);
            same = same && SameBits(r.x, q.x) && SameBits(r.y, q.y);
        }
        Check(same, "a zero surface gives PoseDisplacement bit for bit");
    }
    Check(SameMatrix(cr::RegisteredHomography(model, ca, origin, {}, {12, -0.05, 0.03}), cr::SurfaceHomography(ca, model.Centre({}), origin, {12, -0.05, 0.03})),
        "the zero pose gives Pi(pi) itself");
    // RegisteredDisplacement is (G Pi (X) - X) / pitch.
    {
        const cr::PoseParameters p{0.4, -0.3, 0.9, 4.0, -3.0, 0.6};
        const m2x::SurfacePlane plane{25, 0.02, -0.03};
        const auto h = cr::RegisteredHomography(model, ca, origin, p, plane);
        const Point X{frame.left_mm+(700+0.5)*frame.pitch_mm, frame.top_mm+(250+0.5)*frame.pitch_mm};
        const Point moved = Homogeneous(h, X);
        const auto d = cr::RegisteredDisplacement(model, frame, ca, origin, p, plane, 700, 250);
        CheckNear(d.x, (moved.x-X.x)/frame.pitch_mm, 1e-9, "RegisteredDisplacement x");
        CheckNear(d.y, (moved.y-X.y)/frame.pitch_mm, 1e-9, "RegisteredDisplacement y");
    }
}

// ---- Refusals ------------------------------------------------------------------------------------------------------------------
void Refusals() {
    const Rig rig;
    const auto origin = cr::SurfaceOriginMm(rig.raster, rig.seam_column);
    const auto ca = cr::PoseModel(rig.projection_a).Centre({}), cb = cr::PoseModel(rig.projection_b).Centre({});
    const double nan = std::nan(""), inf = std::numeric_limits<double>::infinity();
    Check(Throws([&] { (void)cr::SurfaceHomography(ca, cb, origin, {nan, 0, 0}); }), "a nonfinite h0 is refused");
    Check(Throws([&] { (void)cr::SurfaceHomography(ca, cb, origin, {0, inf, 0}); }), "a nonfinite slope_x is refused");
    Check(Throws([&] { (void)cr::SurfaceHomography(ca, cb, origin, {0, 0, -inf}); }), "a nonfinite slope_y is refused");
    Check(Throws([&] { (void)cr::SurfaceHomography(ca, cb, {nan, origin.y}, {1, 0, 0}); }), "a nonfinite origin is refused");
    Check(Throws([&] { (void)cr::SurfaceHomography({ca.x_mm, ca.y_mm, 0}, cb, origin, {1, 0, 0}); }), "a centre on the table is refused");
    Check(Throws([&] { (void)cr::SurfaceHomography(ca, {cb.x_mm, nan, cb.height_mm}, origin, {1, 0, 0}); }), "a nonfinite centre is refused");
    Check(Throws([&] { (void)cr::SurfaceHomography(ca, cb, origin, {ca.height_mm+1, 0, 0}); }), "a surface above camera A is refused");
    const cr::PoseModel model(rig.projection_b);
    Check(Throws([&] { (void)cr::RegisteredHomography(model, ca, origin, {inf, 0, 0, 0, 0, 0}, {1, 0, 0}); }), "a nonfinite pose is refused with a surface");
    Check(Throws([&] { (void)cr::RegisteredHomography(model, ca, origin, {0, 0, 0, nan, 0, 0}, {}); }), "a nonfinite pose is refused without a surface");
    // A surface that A's ray through a table point does not meet in front of A does not hold at that point.
    const auto frame = cr::FrameOfRaster(rig.raster);
    const m2x::SurfacePlane steep{0, 3.0, 0};
    const Point left_edge{frame.left_mm+0.5*frame.pitch_mm, origin.y}, middle{origin.x, origin.y};
    Check(!cr::SurfaceHomographyHoldsAt(ca, cb, origin, steep, left_edge), "a surface that A's ray does not meet in front does not hold");
    Check(cr::SurfaceHomographyHoldsAt(ca, cb, origin, steep, middle), "the same surface holds where A's ray meets it");
    Check(!cr::SurfaceHomographyHoldsAt(ca, cb, origin, {nan, 0, 0}, middle), "a nonfinite surface does not hold");
}

// ---- Layer 2 pieces: w, q(y), the strain ----------------------------------------------------------------------------------------
// Samples at rows 0, 64, .. 704 (the raster of the rig has 643 rows), with a peak, a flat run and a change of sign.
m2x::ParallaxProfile MakeProfile(std::uint32_t seam_column) {
    m2x::ParallaxProfile profile;
    profile.baseline = {std::cos(1.0*kDegree), std::sin(1.0*kDegree)};   // e_b off the x axis, as with a baseline y component
    profile.row_spacing_px = 64;
    profile.first_row_px = 0;
    profile.q_px = {0.5, 1.5, 3.0, 2.0, -1.0, -1.0, 0.5, 2.5, 2.5, 1.0, 0.25, 0.75};
    profile.a_px = 12;
    profile.l_px = 40;
    profile.seam_column = seam_column;
    return profile;
}

void SeamWeight() {
    const double a = 12, l = 40;
    Check(m2x::ParallaxSeamWeight(0, a, l) == 1 && m2x::ParallaxSeamWeight(a, a, l) == 1, "w is 1 up to a");
    Check(m2x::ParallaxSeamWeight(a+l, a, l) == 0 && m2x::ParallaxSeamWeight(a+l+100, a, l) == 0, "w is 0 from a + L");
    CheckNear(m2x::ParallaxSeamWeight(a+l/2, a, l), 0.5, 1e-15, "w is 1/2 in the middle of the taper");
    bool monotone = true;
    double previous = 1, steepest = 0;
    for (double u = 0; u <= a+l+5; u += 0.01) {
        const double w = m2x::ParallaxSeamWeight(u, a, l);
        monotone = monotone && w <= previous && w >= 0 && w <= 1;
        previous = w;
    }
    Check(monotone, "w falls from 1 to 0 without rising");
    // C1: the one-sided slopes at the joins are 0 on both sides, and the steepest slope is pi / (2 L) in the middle.
    for (const double join : {a, a+l}) {
        const double e = 1e-4;
        const double left = (m2x::ParallaxSeamWeight(join, a, l)-m2x::ParallaxSeamWeight(join-e, a, l))/e;
        const double right = (m2x::ParallaxSeamWeight(join+e, a, l)-m2x::ParallaxSeamWeight(join, a, l))/e;
        CheckNear(left, 0, 1e-5, "w: the slope left of a join is 0");
        CheckNear(right, 0, 1e-5, "w: the slope right of a join is 0");
    }
    for (double u = a; u <= a+l; u += 0.05) {
        const double e = 1e-6;
        steepest = std::max(steepest, std::abs(m2x::ParallaxSeamWeight(u+e, a, l)-m2x::ParallaxSeamWeight(std::max(0.0, u-e), a, l))/(2*e));
    }
    CheckNear(steepest, kPi/(2*l), 1e-6, "w: the steepest slope is pi / (2 L)");
    Check(Throws([] { (void)m2x::ParallaxSeamWeight(std::nan(""), 1, 1); }), "w refuses a nonfinite distance");
    Check(Throws([] { (void)m2x::ParallaxSeamWeight(1, -1, 1); }), "w refuses a negative a");
    Check(Throws([] { (void)m2x::ParallaxSeamWeight(1, 1, 0); }), "w refuses L = 0");
    Check(Throws([] { (void)m2x::ParallaxSeamWeight(1, 1, std::numeric_limits<double>::infinity()); }), "w refuses an infinite L");
}

void RowShift() {
    const auto profile = MakeProfile(727);
    const auto& q = profile.q_px;
    const double spacing = profile.row_spacing_px;
    bool through = true;
    for (std::size_t i = 0; i < q.size(); ++i) through = through && SameBits(m2x::ParallaxShiftAtRow(profile, i*spacing), q[i]);
    Check(through, "q(y) passes through every sample exactly");
    Check(SameBits(m2x::ParallaxShiftAtRow(profile, -5), q.front()) && SameBits(m2x::ParallaxShiftAtRow(profile, 1e6), q.back()),
        "q(y) keeps the end samples outside the sampled rows");
    // No overshoot: between two samples q stays between them, so max |q(y)| is max |q_i|.
    double largest = 0, largest_sample = 0;
    bool bounded = true;
    for (const double v : q) largest_sample = std::max(largest_sample, std::abs(v));
    for (double y = 0; y <= (q.size()-1)*spacing; y += 0.25) {
        const double v = m2x::ParallaxShiftAtRow(profile, y);
        const auto k = std::min(static_cast<std::size_t>(y/spacing), q.size()-2);
        bounded = bounded && v >= std::min(q[k], q[k+1]) && v <= std::max(q[k], q[k+1]);
        largest = std::max(largest, std::abs(v));
    }
    Check(bounded, "q(y) stays between the neighbouring samples");
    Check(largest <= largest_sample, "max |q(y)| is max |q_i|");
    // C1 at every interior sample: the slopes from the left and from the right agree.
    double worst = 0;
    for (std::size_t i = 1; i+1 < q.size(); ++i) {
        const double y = i*spacing, e = 1e-4;
        const double left = (m2x::ParallaxShiftAtRow(profile, y)-m2x::ParallaxShiftAtRow(profile, y-e))/e;
        const double right = (m2x::ParallaxShiftAtRow(profile, y+e)-m2x::ParallaxShiftAtRow(profile, y))/e;
        worst = std::max(worst, std::abs(left-right));
    }
    CheckNear(worst, 0, 1e-5, "q(y) is C1 at the samples");
    // C1 at the first and last sample too, where the end values continue: the slope is 0 on both sides.
    for (const double y : {0.0, (q.size()-1)*spacing}) {
        const double e = 1e-4;
        CheckNear((m2x::ParallaxShiftAtRow(profile, y)-m2x::ParallaxShiftAtRow(profile, y-e))/e, 0, 1e-5, "q(y): no slope just before an end sample");
        CheckNear((m2x::ParallaxShiftAtRow(profile, y+e)-m2x::ParallaxShiftAtRow(profile, y))/e, 0, 1e-5, "q(y): no slope just after an end sample");
    }
    // The samples sit at first_row_px + i row_spacing_px.
    {
        auto shifted = profile;
        shifted.first_row_px = -10.5;
        shifted.row_spacing_px = 48.25;
        bool at = true;
        for (std::size_t i = 0; i < q.size(); ++i) at = at && SameBits(m2x::ParallaxShiftAtRow(shifted, -10.5+i*48.25), q[i]);
        Check(at, "q(y) passes through the samples at first_row_px + i row_spacing_px");
        CheckNear(m2x::ParallaxShiftAtRow(shifted, -10.5+2.5*48.25), m2x::ParallaxShiftAtRow(profile, 2.5*spacing), 1e-12,
            "q(y) depends on the position in sample units only");
    }
    Check(Throws([&] { auto p = profile; p.first_row_px = std::nan(""); (void)m2x::ParallaxShiftAtRow(p, 3); }), "q(y) refuses a nonfinite first row");
    Check(Throws([&] { auto p = profile; p.row_spacing_px = 0; (void)m2x::ParallaxShiftAtRow(p, 3); }), "q(y) refuses a zero row spacing");
    Check(Throws([&] { (void)m2x::ParallaxShiftAtRow(profile, std::nan("")); }), "q(y) refuses a nonfinite row");
    // A local extremum is flat: the sample 3.0 at row 128 is a peak.
    CheckNear((m2x::ParallaxShiftAtRow(profile, 2*spacing+1e-4)-m2x::ParallaxShiftAtRow(profile, 2*spacing-1e-4))/2e-4, 0, 1e-6, "q(y) is flat at a peak");

    // The strain: (pi / 2) max |q| / L, which is the steepest x slope of q(y) w(u) at the row of the largest sample.
    const double strain = m2x::ParallaxMaximumStrain(profile);
    CheckNear(strain, kPi/2*3.0/profile.l_px, 1e-15, "the strain is (pi / 2) max |q| / L");
    double steepest = 0;
    for (double u = profile.a_px; u <= profile.a_px+profile.l_px; u += 0.05) {
        const double e = 1e-6;
        const double f1 = m2x::ParallaxShiftAtRow(profile, 2*spacing)*m2x::ParallaxSeamWeight(u+e, profile.a_px, profile.l_px);
        const double f0 = m2x::ParallaxShiftAtRow(profile, 2*spacing)*m2x::ParallaxSeamWeight(u-e, profile.a_px, profile.l_px);
        steepest = std::max(steepest, std::abs(f1-f0)/(2*e));
    }
    CheckNear(steepest, strain, 1e-5, "the strain is the steepest shift per column");
    auto bad = profile;
    bad.q_px[3] = std::nan("");
    Check(Throws([&] { (void)m2x::ParallaxMaximumStrain(bad); }), "the strain refuses a nonfinite sample");
}

// ---- The makers -------------------------------------------------------------------------------------------------------------------
bool SameRaw(const render::OutputToCamera& f, const render::OutputToCamera& g, Point at) {
    Point a{}, b{};
    const bool ha = f(at, a), hb = g(at, b);
    return ha == hb && (!ha || (SameBits(a.x, b.x) && SameBits(a.y, b.y)));
}

void MakersBitForBit() {
    const Rig rig;
    const render::DocumentRenderParameters parameters{rig.raster, rig.projection_a, rig.projection_b, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear};
    const cr::PoseModel model(rig.projection_b);
    const auto plain = m2x::DocumentMappedFixedSeamInput(parameters);
    const auto calibrated = render::DocumentOutputToCamera(rig.raster, rig.projection_b);
    const cr::PoseParameters p_b{0.4, -0.3, 0.9, 4.0, -3.0, 0.6};
    auto zero_profile = MakeProfile(rig.seam_column);
    std::fill(zero_profile.q_px.begin(), zero_profile.q_px.end(), 0.0);
    const auto profile = MakeProfile(rig.seam_column);
    const m2x::SurfacePlane surface{25, 0.02, -0.03};

    const auto grid = [&](auto&& each) {
        for (double y = 0; y < rig.raster.height_pixels; y += 23) for (double x = 0; x < rig.raster.width_pixels; x += 29) each(Point{x, y});
        // Every column of the taper around the seam, on a few rows.
        for (double y : {0.0, 100.0, 333.0, 642.0}) for (double x = rig.seam_column-60.0; x <= rig.seam_column+60.0; x += 1) each(Point{x, y});
    };
    const auto all_same = [&](const render::OutputToCamera& f, const render::OutputToCamera& g) {
        bool same = true;
        grid([&](Point at) { same = same && SameRaw(f, g, at); });
        return same;
    };

    // pi = 0 and q = 0: the pose mapping itself; p = 0 is the calibrated mapping and p = p_b the condition's fixed mapping.
    for (const auto& p : {cr::PoseParameters{}, p_b}) {
        const auto pose_only = m2x::PoseCorrectedDocumentOutputToCamera(rig.raster, rig.projection_b, model, p);
        for (const auto& parallax : {std::optional<m2x::ParallaxProfile>{}, std::optional<m2x::ParallaxProfile>{zero_profile}}) {
            const m2x::RegisteredMapping mapping{p, {}, parallax};
            Check(mapping.IsPoseOnly(), "no surface and no or zero q is pose only");
            const auto registered = m2x::RegisteredDocumentOutputToCamera(rig.raster, rig.projection_a, rig.projection_b, model, mapping, rig.seam_column);
            Check(all_same(registered, pose_only), "pi = 0, q = 0: the pose mapping bit for bit");
            if (p.IsZero()) Check(all_same(registered, calibrated), "p = 0, pi = 0, q = 0: the calibrated mapping bit for bit");
            const auto input = m2x::RegisteredDocumentMappedFixedSeamInput(parameters, mapping, rig.seam_column);
            const auto pose_input = m2x::PoseCorrectedDocumentMappedFixedSeamInput(parameters, p);
            Check(input.width == plain.width && input.height == plain.height, "the registered pair keeps the raster");
            Check(all_same(input.camera_b, pose_input.camera_b) && all_same(input.camera_a, plain.camera_a), "the registered pair: B is the pose mapping, A untouched");
        }
    }

    // A surface without layer 2: the homography mapping of G(p) Pi(pi), and A untouched.
    const auto ca = cr::PoseModel(rig.projection_a).Centre({});
    const auto origin = cr::SurfaceOriginMm(rig.raster, rig.seam_column);
    for (const auto& p : {cr::PoseParameters{}, p_b}) {
        const m2x::RegisteredMapping mapping{p, surface, std::nullopt};
        const auto registered = m2x::RegisteredDocumentOutputToCamera(rig.raster, rig.projection_a, rig.projection_b, model, mapping, rig.seam_column);
        const auto by_homography = m2x::HomographyCorrectedDocumentOutputToCamera(rig.raster, rig.projection_b, cr::RegisteredHomography(model, ca, origin, p, surface));
        Check(all_same(registered, by_homography), "a surface: the mapping of G(p) Pi(pi) bit for bit");
        const auto input = m2x::RegisteredDocumentMappedFixedSeamInput(parameters, mapping, rig.seam_column);
        Check(all_same(input.camera_a, plain.camera_a), "a surface: A untouched");
        Check(all_same(input.camera_b, registered), "a surface: the pair holds the same B mapping");
    }

    // Layer 2: B samples at X what the mapping without layer 2 puts at X + q(y) w(u) e_b; where w = 0 it is that mapping bit for bit.
    for (const auto& p : {cr::PoseParameters{}, p_b}) for (const auto& s : {m2x::SurfacePlane{}, surface}) {
        const auto without = m2x::RegisteredDocumentOutputToCamera(rig.raster, rig.projection_a, rig.projection_b, model, {p, s, std::nullopt}, rig.seam_column);
        const auto with = m2x::RegisteredDocumentOutputToCamera(rig.raster, rig.projection_a, rig.projection_b, model, {p, s, profile}, rig.seam_column);
        bool outside = true, inside = true;
        unsigned moved = 0;
        grid([&](Point at) {
            const double w = m2x::ParallaxSeamWeight(std::abs(at.x-rig.seam_column), profile.a_px, profile.l_px);
            if (w == 0) { outside = outside && SameRaw(with, without, at); return; }
            const double shift = m2x::ParallaxShiftAtRow(profile, at.y)*w;
            const Point shifted{at.x+shift*profile.baseline.x, at.y+shift*profile.baseline.y};
            Point a{}, b{};
            const bool ha = with(at, a), hb = without(shifted, b);
            inside = inside && ha == hb && SameBits(a.x, b.x) && SameBits(a.y, b.y);
            if (shift != 0) ++moved;
        });
        Check(outside, "layer 2: outside the taper the mapping without layer 2 bit for bit");
        Check(inside, "layer 2: inside, the mapping without layer 2 at X + q w e_b bit for bit");
        Check(moved > 100, "layer 2: the shift acts on the columns around the seam");
        if (p.IsZero() && s.IsZero()) {
            bool same = true;
            grid([&](Point at) {
                if (m2x::ParallaxSeamWeight(std::abs(at.x-rig.seam_column), profile.a_px, profile.l_px) == 0) same = same && SameRaw(with, calibrated, at);
            });
            Check(same, "layer 2 alone: outside the taper the calibrated mapping bit for bit");
        }
        const auto input = m2x::RegisteredDocumentMappedFixedSeamInput(parameters, {p, s, profile}, rig.seam_column);
        Check(all_same(input.camera_a, plain.camera_a), "layer 2: A untouched");
        Check(all_same(input.camera_b, with), "layer 2: the pair holds the same B mapping");
    }
}

// The raw position of B's registered mapping is the moved camera's raw projection of the paper point A shows at the same output
// sample, through the lens (G(p) Pi(pi) without layer 2).
void MakersFollowThePaper() {
    const Rig rig;
    const cr::PoseModel model(rig.projection_b);
    const auto frame = cr::FrameOfRaster(rig.raster);
    const auto origin = cr::SurfaceOriginMm(rig.raster, rig.seam_column);
    const Vec3 ta = rig.a.Centre();
    const auto map_a = render::DocumentOutputToCamera(rig.raster, rig.projection_a);
    for (const cr::PoseParameters& p : {cr::PoseParameters{}, cr::PoseParameters{0.4, -0.3, 0.9, 4.0, -3.0, 0.6}}) {
        const RigidCamera moved = FromHomography(model.CameraHomography(p));
        for (const auto& plane : Planes()) {
            const auto map_b = m2x::RegisteredDocumentOutputToCamera(rig.raster, rig.projection_a, rig.projection_b, model, {p, plane, std::nullopt}, rig.seam_column);
            double worst_a = 0, worst_b = 0;
            for (double y = 30; y < rig.raster.height_pixels; y += 97) for (double x = 560; x < 900; x += 47) {
                const Point X{frame.left_mm+(x+0.5)*frame.pitch_mm, frame.top_mm+(y+0.5)*frame.pitch_mm};
                const Vec3 paper = PaperPoint(ta, X, plane, origin);
                Point ra{}, rb{};
                map_a({x, y}, ra);
                map_b({x, y}, rb);
                const Point truth_a = rig.a.Raw(rig.a.CameraPoint(paper[0], paper[1], paper[2]));
                const Point truth_b = moved.Raw(moved.CameraPoint(paper[0], paper[1], paper[2]));
                worst_a = std::max(worst_a, std::hypot(ra.x-truth_a.x, ra.y-truth_a.y));
                worst_b = std::max(worst_b, std::hypot(rb.x-truth_b.x, rb.y-truth_b.y));
            }
            CheckNear(worst_a, 0, 1e-6, Name(plane) + ": A shows the paper point at the output sample (raw px)");
            CheckNear(worst_b, 0, 1e-6, Name(plane) + ": B's registered mapping shows the same paper point (raw px)");
        }
    }
}

void MakerRefusals() {
    const Rig rig;
    const cr::PoseModel model(rig.projection_b);
    const double nan = std::nan(""), inf = std::numeric_limits<double>::infinity();
    const auto make = [&](const m2x::RegisteredMapping& mapping, std::uint32_t seam) {
        return Throws([&] { (void)m2x::RegisteredDocumentOutputToCamera(rig.raster, rig.projection_a, rig.projection_b, model, mapping, seam); });
    };
    const auto profile = MakeProfile(rig.seam_column);
    const auto with = [&](auto&& change) { auto q = profile; change(q); return m2x::RegisteredMapping{{}, {}, q}; };
    Check(make({{}, {nan, 0, 0}, std::nullopt}, rig.seam_column), "a nonfinite h0 is refused");
    Check(make({{}, {0, 0, inf}, std::nullopt}, rig.seam_column), "a nonfinite slope is refused");
    Check(make({{0, 0, 0, nan, 0, 0}, {5, 0, 0}, std::nullopt}, rig.seam_column), "a nonfinite pose is refused");
    Check(make({}, rig.raster.width_pixels), "a seam column outside the raster is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.q_px[2] = nan; }), rig.seam_column), "a nonfinite q sample is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.q_px[2] = inf; }), rig.seam_column), "an infinite q sample is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { std::fill(q.q_px.begin(), q.q_px.end(), 0.0); q.q_px[1] = nan; }), rig.seam_column),
        "a nonfinite sample is refused in an otherwise zero profile");
    Check(make(with([&](m2x::ParallaxProfile& q) { std::fill(q.q_px.begin(), q.q_px.end(), 0.0); q.l_px = 0; }), rig.seam_column),
        "a zero profile with an invalid L is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.a_px = nan; }), rig.seam_column), "a nonfinite a is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.a_px = -1; }), rig.seam_column), "a negative a is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.l_px = 0; }), rig.seam_column), "L = 0 is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.baseline = {nan, 0}; }), rig.seam_column), "a nonfinite baseline is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.baseline = {1, 0.01}; }), rig.seam_column), "a baseline that is not a unit vector is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.row_spacing_px = 0; }), rig.seam_column), "a zero row spacing is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.row_spacing_px = inf; }), rig.seam_column), "an infinite row spacing is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.first_row_px = nan; }), rig.seam_column), "a nonfinite first row is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.seam_column += 1; }), rig.seam_column), "a profile for another seam column is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { std::fill(q.q_px.begin(), q.q_px.end(), 0.0); q.seam_column += 1; }), rig.seam_column),
        "a zero profile for another seam column is refused");
    Check(make(with([&](m2x::ParallaxProfile& q) { q.q_px.clear(); }), rig.seam_column), "an empty profile is refused");
    Check(make({{}, {0, 3.0, 0}, std::nullopt}, rig.seam_column), "a surface that A's rays do not meet over the raster is refused");
    const render::DocumentRenderParameters parameters{rig.raster, rig.projection_a, rig.projection_b, a0::m2::StitchLayout::camera_a_left_camera_b_right, render::Resampling::bilinear};
    Check(Throws([&] { (void)m2x::RegisteredDocumentMappedFixedSeamInput(parameters, with([&](m2x::ParallaxProfile& q) { std::fill(q.q_px.begin(), q.q_px.end(), 0.0); q.baseline = {0, 0}; }), rig.seam_column); }),
        "the pair refuses an invalid zero profile too");
    Check(Throws([&] { (void)m2x::RegisteredDocumentMappedFixedSeamInput(parameters, {}, rig.raster.width_pixels); }),
        "the pair refuses a seam column outside the raster for a pose-only mapping");
    bool accepted = true;
    try { (void)m2x::RegisteredDocumentOutputToCamera(rig.raster, rig.projection_a, rig.projection_b, model, {{}, {12, -0.05, 0.03}, profile}, rig.seam_column); }
    catch (const std::exception&) { accepted = false; }
    Check(accepted, "the test profile and surface are accepted");
}
}

int main() {
    try {
        Centres();
        SurfaceHomographyFollowsTheRays();
        PoseAndSurfaceTogether();
        Refusals();
        SeamWeight();
        RowShift();
        MakersBitForBit();
        MakersFollowThePaper();
        MakerRefusals();
    } catch (const std::exception& e) {
        std::cerr << "FAIL unexpected exception: " << e.what() << '\n';
        ++failures;
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
