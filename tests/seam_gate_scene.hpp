#pragma once
// Test-only scene for the seam gate (#386): an analytic paper of blurred ink
// strokes, and its CAM-A and CAM-B views under a known misalignment field.
// Independent of the gate: no seam gate header is used, and the views are
// computed from the strokes, not resampled from another image.
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <vector>

namespace seam_gate_scene {
using a0::m2::render::BgrImage;

struct Stroke { double cx{}, cy{}, half_length{}, half_width{}, c{1}, s{0}, ink{}; };
struct Lcg {
    std::uint64_t state;
    unsigned Next(unsigned n) { state = state*6364136223846793005ull+1442695040888963407ull; return static_cast<unsigned>((state>>33)%n); }
    double Uniform() { return Next(1u<<30)/double(1u<<30); }
};
inline constexpr double kPaper = 232, kBlur = 0.8;
// Box of half width h convolved with a Gaussian of sigma kBlur.
inline double Profile(double t, double h) {
    const double k = 1/(kBlur*std::numbers::sqrt2);
    return 0.5*(std::erf((t+h)*k)-std::erf((t-h)*k));
}
inline double Coverage(const Stroke& s, double x, double y) {
    const double dx = x-s.cx, dy = y-s.cy;
    return Profile(dx*s.c+dy*s.s,s.half_length)*Profile(-dx*s.s+dy*s.c,s.half_width);
}
inline Stroke Segment(double x0, double y0, double x1, double y1, double width, double ink) {
    const double length = std::hypot(x1-x0,y1-y0);
    return {(x0+x1)/2,(y0+y1)/2,length/2,width/2,(x1-x0)/length,(y1-y0)/length,ink};
}

// View pixel (i,j) shows the world point at(i,j); |at(i,j)-(i+ox,j+oy)| <= bound.
// Deterministic noise of standard deviation noise_dn, then rounding to 8 bits.
inline BgrImage Render(unsigned w, unsigned h, double ox, double oy, double bound, const std::vector<Stroke>& strokes,
    const std::function<void(int,int,double&,double&)>& at, std::uint64_t seed, double noise_dn) {
    std::vector<double> dark(static_cast<std::size_t>(w)*h);
    const double margin = 4*kBlur+1;
    for (const auto& s : strokes) {
        const double ex = std::abs(s.c)*(s.half_length+margin)+std::abs(s.s)*(s.half_width+margin);
        const double ey = std::abs(s.s)*(s.half_length+margin)+std::abs(s.c)*(s.half_width+margin);
        const int i0 = std::max(0,static_cast<int>(std::floor(s.cx-ex-ox-bound))), i1 = std::min(static_cast<int>(w)-1,static_cast<int>(std::ceil(s.cx+ex-ox+bound)));
        const int j0 = std::max(0,static_cast<int>(std::floor(s.cy-ey-oy-bound))), j1 = std::min(static_cast<int>(h)-1,static_cast<int>(std::ceil(s.cy+ey-oy+bound)));
        for (int j = j0; j <= j1; ++j) for (int i = i0; i <= i1; ++i) {
            double x{}, y{}; at(i,j,x,y);
            dark[static_cast<std::size_t>(j)*w+i] += s.ink*Coverage(s,x,y);
        }
    }
    BgrImage image{w,h,std::vector<std::uint8_t>(static_cast<std::size_t>(w)*h*3)};
    Lcg r{seed};
    for (std::size_t k = 0; k < dark.size(); ++k) {
        double noise = 0;
        if (noise_dn > 0) { for (int n = 0; n < 4; ++n) noise += r.Uniform()-0.5; noise *= noise_dn*std::sqrt(3.0); }
        const auto v = static_cast<std::uint8_t>(std::clamp(std::lround(kPaper-dark[k]+noise),0L,255L));
        image.bgr[3*k] = image.bgr[3*k+1] = image.bgr[3*k+2] = v;
    }
    return image;
}

struct Rect { double x0, y0, x1, y1; };
// Lines, boxes and diagonal strokes of a drawing.
inline void Drawing(std::vector<Stroke>& out, Rect r, std::uint64_t seed, double per_square_px) {
    Lcg g{seed};
    const auto count = static_cast<long>((r.x1-r.x0)*(r.y1-r.y0)/per_square_px);
    for (long k = 0; k < count; ++k) {
        const double x = r.x0+g.Uniform()*(r.x1-r.x0), y = r.y0+g.Uniform()*(r.y1-r.y0);
        const double len = 30+g.Next(200), width = 1.5+g.Next(3)*0.5, ink = 120+g.Next(80);
        switch (g.Next(4)) {
        case 0: out.push_back(Segment(x,y,std::min(x+len,r.x1),y,width,ink)); break;
        case 1: out.push_back(Segment(x,y,x,std::min(y+len,r.y1),width,ink)); break;
        case 2: {
            const double bw = std::min(len,r.x1-x), bh = std::min(len/2,r.y1-y);
            if (bw < 8 || bh < 8) break;
            out.push_back(Segment(x,y,x+bw,y,width,ink)); out.push_back(Segment(x,y+bh,x+bw,y+bh,width,ink));
            out.push_back(Segment(x,y,x,y+bh,width,ink)); out.push_back(Segment(x+bw,y,x+bw,y+bh,width,ink));
            break;
        }
        default: {
            const double a = (15+g.Next(150))*std::numbers::pi/180, ex = std::clamp(x+len*std::cos(a),r.x0,r.x1), ey = std::clamp(y+len*std::sin(a),r.y0,r.y1);
            if (std::hypot(ex-x,ey-y) > 8) out.push_back(Segment(x,y,ex,ey,width,ink));
        }
        }
    }
}
// Rows of glyph-like strokes of varying shape and spacing.
inline void Text(std::vector<Stroke>& out, Rect r, std::uint64_t seed) {
    Lcg g{seed};
    for (double y = r.y0+6; y+16 < r.y1; y += 22+g.Next(14)) {
        double x = r.x0+g.Next(20);
        while (x+14 < r.x1) {
            const double gw = 6+g.Next(7), gh = 10+g.Next(5), ink = 130+g.Next(70);
            switch (g.Next(5)) {
            case 0: out.push_back(Segment(x,y,x,y+gh,1.8,ink)); out.push_back(Segment(x,y+gh,x+gw,y+gh,1.8,ink)); break;
            case 1: out.push_back(Segment(x,y,x+gw,y,1.8,ink)); out.push_back(Segment(x+gw/2,y,x+gw/2,y+gh,1.8,ink)); break;
            case 2: out.push_back(Segment(x,y+gh,x+gw/2,y,1.8,ink)); out.push_back(Segment(x+gw/2,y,x+gw,y+gh,1.8,ink));
                out.push_back(Segment(x+gw/4,y+gh/2,x+3*gw/4,y+gh/2,1.6,ink)); break;
            case 3: out.push_back(Segment(x,y,x,y+gh,1.8,ink)); out.push_back(Segment(x,y,x+gw,y+gh/3,1.8,ink));
                out.push_back(Segment(x,y+2*gh/3,x+gw,y+gh,1.8,ink)); break;
            default: out.push_back(Segment(x,y,x+gw,y,1.8,ink)); out.push_back(Segment(x,y+gh,x+gw,y+gh,1.8,ink));
                out.push_back(Segment(x,y,x,y+gh,1.8,ink)); out.push_back(Segment(x+gw,y+gh/2,x+gw,y+gh,1.8,ink));
            }
            x += gw+3+g.Next(6);
            if (!g.Next(7)) x += 10+g.Next(12);
        }
    }
}
// Horizontal lines only, at irregular spacing.
inline void HorizontalLines(std::vector<Stroke>& out, Rect r, std::uint64_t seed) {
    Lcg g{seed};
    for (double y = r.y0+10+g.Next(20); y < r.y1-10; y += 45+g.Next(40))
        out.push_back(Segment(r.x0,y,r.x1,y,1.5+g.Next(3)*0.5,130+g.Next(60)));
}
// Fine hatching with a fixed period.
inline void Hatching(std::vector<Stroke>& out, Rect r, double period) {
    for (double x = r.x0; x < r.x1; x += period) out.push_back(Segment(x,r.y0,x,r.y1,period/3,150));
}
// Lines of a fixed period in both directions with a small square mark at every
// crossing, the table grid of a desk. x_phase and y_phase place a crossing.
inline void Lattice(std::vector<Stroke>& out, Rect r, double period, double x_phase, double y_phase) {
    const double fx = std::fmod(x_phase, period), fy = std::fmod(y_phase, period);
    for (double x = r.x0+fx; x < r.x1; x += period) out.push_back(Segment(x, r.y0, x, r.y1, 3, 110));
    for (double y = r.y0+fy; y < r.y1; y += period) out.push_back(Segment(r.x0, y, r.x1, y, 3, 110));
    for (double x = r.x0+fx; x < r.x1; x += period) for (double y = r.y0+fy; y < r.y1; y += period)
        out.push_back(Segment(x-4, y, x+4, y, 9, 140));
}
} // namespace seam_gate_scene
