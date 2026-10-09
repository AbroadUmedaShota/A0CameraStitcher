#include "a0/m2/document_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using a0::m2::StitchLayout;
using namespace a0::m2::render;

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr std::uint32_t kSourceSide = 512;
constexpr std::uint32_t kOutputSide = 256;
constexpr double kSourceEdgeCentre = 224.0;
constexpr double kMapOffset = 96.0;
constexpr double kDark = 32.0;
constexpr double kLight = 224.0;
constexpr double kSigma = 0.8;
constexpr double kHalfWindow = 12.0;
constexpr double kBinWidth = 0.125;
constexpr std::size_t kBins = 192;
constexpr std::uint32_t kRoiFirstRow = 16;
constexpr std::uint32_t kRoiEndRow = 240;

int failures = 0;

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct EdgeCase {
    const char* name;
    double slope;
    double phase_x;
    double phase_y;
};

constexpr std::array<EdgeCase, 3> kCases{{
    {"slope-plus-1over8", 1.0 / 8.0, 0.37, 0.23},
    {"slope-plus-1over12", 1.0 / 12.0, 0.61, 0.47},
    {"slope-minus-1over10", -1.0 / 10.0, 0.19, 0.73},
}};

double SourceNormalDistance(const EdgeCase& edge, const double x, const double y) {
    return ((x - kSourceEdgeCentre) - edge.slope * (y - kSourceEdgeCentre))
        / std::sqrt(1.0 + edge.slope * edge.slope);
}

// The fixture is a continuous Gaussian-blurred half plane sampled at INTEGER
// camera indices, with no pixel aperture, gamma, JPEG, noise or lens model.
// The direct-output oracle evaluates this field anew; it does not interpolate
// the fixture or call either production sampler.
double AnalyticLevel(const double distance, const double sigma) {
    return kDark + (kLight - kDark) * 0.5
        * (1.0 + std::erf(distance / (std::sqrt(2.0) * sigma)));
}

std::uint8_t QuantizedLevel(const double distance, const double sigma) {
    return static_cast<std::uint8_t>(std::floor(AnalyticLevel(distance, sigma) + 0.5));
}

BgrImage MakeSource(const EdgeCase& edge, const double sigma) {
    BgrImage image{kSourceSide, kSourceSide,
        std::vector<std::uint8_t>(static_cast<std::size_t>(kSourceSide) * kSourceSide * 3)};
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto value = QuantizedLevel(SourceNormalDistance(edge, x, y), sigma);
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3;
            image.bgr[index] = image.bgr[index + 1] = image.bgr[index + 2] = value;
        }
    }
    return image;
}

double OutputNormalDistance(const EdgeCase& edge, const double x, const double y) {
    // Unit scale: one camera pixel equals one output pixel. In particular the
    // normal distance and the DFT frequency are in OUTPUT pixel units.
    return SourceNormalDistance(edge, x + kMapOffset + edge.phase_x,
        y + kMapOffset + edge.phase_y);
}

BgrImage MakeAnalyticOutput(const EdgeCase& edge, const double sigma) {
    BgrImage image{kOutputSide, kOutputSide,
        std::vector<std::uint8_t>(static_cast<std::size_t>(kOutputSide) * kOutputSide * 3)};
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto value = QuantizedLevel(OutputNormalDistance(edge, x, y), sigma);
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3;
            image.bgr[index] = image.bgr[index + 1] = image.bgr[index + 2] = value;
        }
    }
    return image;
}

struct MtfMeasurement {
    double mtf50;
    double dc_contrast;
    std::size_t minimum_bin_population;
};

// Independently defined slanted-edge estimator, not a claim of ISO-12233
// compliance: known true edge normal (no fit), integer output samples, 1/8 px
// ESF bins over [-12,12), rows [16,240), centered finite difference, Hann LSF
// window, and a complex DFT normalized by its DC. The first crossing of 0.5 is
// searched at 1/4096 cycles/output-pixel spacing and linearly interpolated.
// No derivative/window deconvolution is applied; the analytic control below
// independently bounds the small estimator bias for these finite ROIs.
MtfMeasurement MeasureMtf50(const BgrImage& image, const EdgeCase& edge) {
    if (image.width != kOutputSide || image.height != kOutputSide
        || image.bgr.size() != static_cast<std::size_t>(image.width) * image.height * 3) {
        throw std::invalid_argument("MTF fixture dimensions are inconsistent");
    }
    std::array<double, kBins> sums{};
    std::array<std::size_t, kBins> counts{};
    for (std::uint32_t y = kRoiFirstRow; y < kRoiEndRow; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const double distance = OutputNormalDistance(edge, x, y);
            if (!(distance >= -kHalfWindow && distance < kHalfWindow)) continue;
            const auto bin = static_cast<std::size_t>(std::floor((distance + kHalfWindow) / kBinWidth));
            if (bin >= kBins) throw std::runtime_error("MTF bin overflow");
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3;
            if (image.bgr[index] != image.bgr[index + 1] || image.bgr[index] != image.bgr[index + 2]) {
                throw std::runtime_error("MTF fixture must remain achromatic");
            }
            sums[bin] += image.bgr[index];
            ++counts[bin];
        }
    }
    const std::size_t minimum = *std::min_element(counts.begin(), counts.end());
    if (minimum == 0) throw std::runtime_error("An ESF bin has no observations");
    std::array<double, kBins> esf{};
    std::array<double, kBins> lsf{};
    for (std::size_t bin = 0; bin < kBins; ++bin) esf[bin] = sums[bin] / counts[bin];
    double dc = 0.0;
    for (std::size_t bin = 1; bin + 1 < kBins; ++bin) {
        const double distance = -kHalfWindow + (static_cast<double>(bin) + 0.5) * kBinWidth;
        const double window = 0.5 * (1.0 + std::cos(kPi * distance / kHalfWindow));
        lsf[bin] = (esf[bin + 1] - esf[bin - 1]) / (2.0 * kBinWidth) * window;
        dc += lsf[bin] * kBinWidth;
    }
    if (!std::isfinite(dc) || dc < 16.0) throw std::runtime_error("No positive edge contrast for MTF normalization");
    double previous_mtf = 1.0;
    double previous_frequency = 0.0;
    for (int step = 1; step <= 2048; ++step) {
        const double frequency = static_cast<double>(step) / 4096.0;
        double real = 0.0, imaginary = 0.0;
        for (std::size_t bin = 1; bin + 1 < kBins; ++bin) {
            const double distance = -kHalfWindow + (static_cast<double>(bin) + 0.5) * kBinWidth;
            const double phase = 2.0 * kPi * frequency * distance;
            real += lsf[bin] * std::cos(phase) * kBinWidth;
            imaginary -= lsf[bin] * std::sin(phase) * kBinWidth;
        }
        const double mtf = std::hypot(real, imaginary) / dc;
        if (!std::isfinite(mtf)) throw std::runtime_error("Non-finite MTF");
        if (previous_mtf > 0.5 && mtf <= 0.5) {
            const double fraction = (previous_mtf - 0.5) / (previous_mtf - mtf);
            return {previous_frequency + fraction * (frequency - previous_frequency), dc, minimum};
        }
        previous_mtf = mtf;
        previous_frequency = frequency;
    }
    throw std::runtime_error("MTF50 has no crossing below output Nyquist");
}

double GaussianMtf50(const double sigma) {
    // FT of the Gaussian line-spread function: exp(-2*pi^2*sigma^2*f^2).
    return std::sqrt(2.0 * std::log(2.0)) / (2.0 * kPi * sigma);
}

double EdgeRmsDifference(const BgrImage& image, const BgrImage& oracle, const EdgeCase& edge) {
    double square_sum = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = kRoiFirstRow; y < kRoiEndRow; ++y) {
        for (std::uint32_t x = 0; x < kOutputSide; ++x) {
            if (std::abs(OutputNormalDistance(edge, x, y)) > 8.0) continue;
            const std::size_t index = (static_cast<std::size_t>(y) * kOutputSide + x) * 3;
            const double difference = static_cast<double>(image.bgr[index]) - oracle.bgr[index];
            square_sum += difference * difference;
            ++count;
        }
    }
    if (count == 0) throw std::runtime_error("No oracle comparison samples");
    return std::sqrt(square_sum / count);
}

void TestAnalyticEstimatorControls() {
    for (const auto& edge : kCases) {
        for (const double sigma : {0.8, 1.2}) {
            const auto oracle = MakeAnalyticOutput(edge, sigma);
            const auto measured = MeasureMtf50(oracle, edge);
            const double theoretical = GaussianMtf50(sigma);
            Check(std::abs(measured.mtf50 - theoretical) < 0.012,
                std::string(edge.name) + ": independent analytic Gaussian validates the finite ESF/DFT estimator");
            Check(measured.minimum_bin_population >= 8, "Analytic edge gives populated ESF bins");
            std::cout << "mtf50-control case=" << edge.name << " sigma_input_px=" << sigma
                << " theoretical_cycles_per_output_px=" << theoretical
                << " measured_cycles_per_output_px=" << measured.mtf50
                << " minimum_bin_population=" << measured.minimum_bin_population << '\n';
        }
    }
    auto flat = MakeAnalyticOutput(kCases.front(), kSigma);
    std::fill(flat.bgr.begin(), flat.bgr.end(), std::uint8_t{128});
    bool rejected = false;
    try { (void)MeasureMtf50(flat, kCases.front()); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "A flat image cannot produce a fabricated MTF50");
}

void TestMappedKernelMtf50() {
    std::cout << "mtf50-method known-normal-esf-bin=0.125px roi-normal=[-12,12) roi-rows=[16,240)"
        " derivative=central-difference lsf-window=hann dft-dc-normalized frequency-step=1/4096"
        " correction=none crossing=first-0.5 units=cycles/output-pixel"
        " source=quantized-erf sigma-input-px=0.8 levels=32..224 scale=1"
        " input-sample=integer no-aperture no-gamma no-jpeg no-noise quality=not-evaluated\n";
    std::cout << "case,slope,phase_x,phase_y,analytic_direct_mtf50,bilinear_mtf50,catmull_rom_mtf50,"
        "bilinear_rms_8bit,catmull_rom_rms_8bit\n";
    for (const auto& edge : kCases) {
        const auto source_a = MakeSource(edge, kSigma);
        const auto source_b = source_a;
        const auto original_a = source_a.bgr;
        const auto original_b = source_b.bgr;
        const auto oracle = MakeAnalyticOutput(edge, kSigma);
        const auto analytic = MeasureMtf50(oracle, edge);
        std::array<double, 2> mtf50{}, rms{};
        for (std::size_t kernel_index = 0; kernel_index < 2; ++kernel_index) {
            const auto kernel = kernel_index == 0 ? Resampling::bilinear : Resampling::bicubic_catmull_rom;
            const OutputToCamera map = [edge](const Point output, Point& raw) {
                raw = {output.x + kMapOffset + edge.phase_x, output.y + kMapOffset + edge.phase_y};
                return true;
            };
            const MappedRenderParameters parameters{kOutputSide, kOutputSide, map, map,
                StitchLayout::camera_a_left_camera_b_right, kernel};
            // All samples and the full 4x4 Catmull-Rom support are strictly
            // inside [94,355] of the 512px source; no border clamp is exercised.
            const auto result = RenderMappedPair(source_a, source_b, parameters);
            const auto again = RenderMappedPair(source_a, source_b, parameters);
            Check(result.image.bgr == again.image.bgr, "Kernel render is byte deterministic");
            Check(source_a.bgr == original_a && source_b.bgr == original_b, "Both source images remain unchanged");
            const auto measurement = MeasureMtf50(result.image, edge);
            mtf50[kernel_index] = measurement.mtf50;
            rms[kernel_index] = EdgeRmsDifference(result.image, oracle, edge);
            Check(std::isfinite(measurement.mtf50) && measurement.mtf50 > 0.0 && measurement.mtf50 < 0.5,
                "A real finite MTF50 crossing must lie below Nyquist");
            // These are fixture/oracle sanity guards, not an optical-quality
            // threshold or a ranking of the two kernels. Both share the same
            // Gaussian input blur and must preserve this known edge signal.
            Check(std::abs(measurement.mtf50 - analytic.mtf50) < 0.08,
                "Rendered edge MTF50 remains consistent with the independently sampled smooth field");
            Check(std::isfinite(rms[kernel_index]) && rms[kernel_index] < 5.0,
                "Rendered edge signal agrees with the independent analytic field in the valid ROI");
            Check(result.seam_navigation.available, "Identical-camera overlap exercises the real feather path");
        }
        std::cout << edge.name << ',' << edge.slope << ',' << edge.phase_x << ',' << edge.phase_y
            << ',' << analytic.mtf50 << ',' << mtf50[0] << ',' << mtf50[1] << ',' << rms[0] << ',' << rms[1] << '\n';
    }
}

} // namespace

int main() {
    try {
        std::cout << std::setprecision(12);
        TestAnalyticEstimatorControls();
        TestMappedKernelMtf50();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: MTF comparison could not produce valid evidence: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) return 1;
    std::cout << "document render MTF comparisons recorded; quality=not-evaluated failures=0\n";
    return 0;
}
