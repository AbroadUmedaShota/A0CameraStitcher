#include "calibration_lens_model.hpp"
#include "synthetic_pair_geometry.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

// Formula agreement test of the lens stage (Issue #290).
// The lens library must not depend on the synthetic pair generator, so the
// contract test never calls the generator. This executable is the one place
// that links both: it compares the library's distortion function with the
// generator's, bit for bit, over a grid of normalized points and three sets of
// coefficients. A mismatch means the two formulas drifted apart (or that the
// compiler contracted the arithmetic differently); it is fixed by making the
// order of operations identical, never by loosening the comparison.
namespace {
int checks = 0, failures = 0;
void Check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
bool Bitwise(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }

struct CoefficientSet {
    const char* name;
    double k1, k2, k3, p1, p2;
};
} // namespace

int main() {
    // E1 truth, the invalid-distortion truth of PE, and every coefficient nonzero with mixed signs.
    const std::vector<CoefficientSet> sets{
        {"E1", -0.1, 0.05, 0.01, 1e-3, -5e-4},
        {"PE", 0.0, 2.4, -12.0, 0.0, 0.0},
        {"all-nonzero", 0.31, -0.27, 0.19, -0.013, 0.021}};
    for (const auto& set : sets) {
        const a0::m2::calibration::BrownConradyDistortion library{set.k1, set.k2, set.k3, set.p1, set.p2};
        a0::m2::synthetic::DistortionSpec generator;
        generator.k1 = set.k1;
        generator.k2 = set.k2;
        generator.k3 = set.k3;
        generator.p1 = set.p1;
        generator.p2 = set.p2;
        int mismatches = 0;
        for (int i = 0; i <= 40; ++i) for (int j = 0; j <= 40; ++j) {
            const double x = -0.8 + 1.6 * i / 40.0, y = -0.8 + 1.6 * j / 40.0;
            const auto a = a0::m2::calibration::lens_detail::DistortNormalized(library, x, y);
            const auto b = a0::m2::synthetic::detail::DistortNormalized(generator, x, y);
            if (!Bitwise(a.x, b.x) || !Bitwise(a.y, b.y)) ++mismatches;
        }
        Check(mismatches == 0, std::string("F1: bitwise equal on the 41 x 41 grid: ") + set.name);
    }
    // The generator's own Jacobian entries (a, b, c in UndistortNormalized) are the ones the lens
    // library writes in DistortionDerivativesAt; they are not exported, so they are re-derived here
    // from the distortion by a central difference as a sanity check of the shared formula.
    {
        const a0::m2::calibration::BrownConradyDistortion d{-0.1, 0.05, 0.01, 1e-3, -5e-4};
        a0::m2::synthetic::DistortionSpec g;
        g.k1 = d.k1; g.k2 = d.k2; g.k3 = d.k3; g.p1 = d.p1; g.p2 = d.p2;
        double worst = 0;
        for (const double x : {-0.4, 0.0, 0.3}) for (const double y : {-0.35, 0.1, 0.45}) {
            const double h = 1e-6;
            const auto xp = a0::m2::synthetic::detail::DistortNormalized(g, x + h, y), xm = a0::m2::synthetic::detail::DistortNormalized(g, x - h, y);
            const auto yp = a0::m2::synthetic::detail::DistortNormalized(g, x, y + h), ym = a0::m2::synthetic::detail::DistortNormalized(g, x, y - h);
            const auto derivative = a0::m2::calibration::lens_detail::DistortionDerivativesAt(d, x, y);
            const double dxdx = (xp.x - xm.x) / (2 * h), dxdy = (yp.x - ym.x) / (2 * h);
            const double dydx = (xp.y - xm.y) / (2 * h), dydy = (yp.y - ym.y) / (2 * h);
            for (const double e : {std::abs(dxdx - derivative.a), std::abs(dxdy - derivative.b), std::abs(dydx - derivative.b), std::abs(dydy - derivative.c)}) {
                worst = std::max(worst, e);
            }
        }
        Check(worst <= 1e-8, "F1: the library derivative matches the generator's map (central difference), worst " + std::to_string(worst));
    }
    std::cout << "calibration_lens_formula checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
