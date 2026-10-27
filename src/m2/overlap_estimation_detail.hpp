#pragma once

// Shared private gradient planes for the uncalibrated overlap estimators:
// whole-frame translation (#364) and still tile similarity (#365).
#include <cmath>
#include <cstddef>
#include <vector>

namespace a0::m2::overlap_detail {
struct Plane {
    int width{}, height{};
    std::vector<double> v;
    double At(int x, int y) const { return v[static_cast<std::size_t>(y)*width+x]; }
};
// Forward-difference gradient magnitude, (w-1)x(h-1). Removes the smooth
// brightness field (vignetting, shading, gain offset) before any averaging.
// Row pointers keep full-size stills affordable in unoptimized builds.
inline Plane Gradient(const Plane& l) {
    Plane g{l.width-1,l.height-1,{}};
    g.v.resize(static_cast<std::size_t>(g.width)*g.height);
    for (int y = 0; y < g.height; ++y) {
        const double* const row = l.v.data()+static_cast<std::size_t>(y)*l.width; const double* const below = row+l.width;
        double* const out = g.v.data()+static_cast<std::size_t>(y)*g.width;
        for (int x = 0; x < g.width; ++x) {
            const double base = row[x], gx = row[x+1]-base, gy = below[x]-base;
            out[x] = std::sqrt(gx*gx+gy*gy);
        }
    }
    return g;
}
// 2x2 mean of the magnitude: a thin line keeps its edge energy at every level.
inline Plane Half(const Plane& p) {
    Plane h{p.width/2,p.height/2,{}};
    h.v.resize(static_cast<std::size_t>(h.width)*h.height);
    for (int y = 0; y < h.height; ++y) {
        const double* const top = p.v.data()+static_cast<std::size_t>(2*y)*p.width; const double* const bottom = top+p.width;
        double* const out = h.v.data()+static_cast<std::size_t>(y)*h.width;
        for (int x = 0; x < h.width; ++x) out[x] = (top[2*x]+top[2*x+1]+bottom[2*x]+bottom[2*x+1])/4;
    }
    return h;
}
} // namespace a0::m2::overlap_detail
