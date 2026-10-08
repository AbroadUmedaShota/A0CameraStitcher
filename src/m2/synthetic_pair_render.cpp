#include "synthetic_pair_chart.hpp"
#include "synthetic_pair_geometry.hpp"

#include <cmath>
#include <stdexcept>

namespace a0::m2::synthetic {

namespace {

constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ULL;
constexpr double kUnitVarianceScale = 1.7320508075688772;  // sqrt(3): four uniforms have variance 1/3

// SplitMix64 finalizer. A counter based generator keeps every pixel's noise
// independent of the order in which rows are rendered.
std::uint64_t Mix(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return value;
}

// Approximately normal, unit variance: sum of four 16 bit uniforms from one 64 bit word.
double GaussianNoise(const std::uint64_t key, const std::uint64_t counter) noexcept {
    const std::uint64_t word = Mix(key + (counter + 1U) * kGoldenGamma);
    double sum = 0.0;
    for (unsigned shift = 0; shift < 64U; shift += 16U) {
        sum += (static_cast<double>((word >> shift) & 0xFFFFU) + 0.5) / 65536.0;
    }
    return (sum - 2.0) * kUnitVarianceScale;
}

// NaN and values below 0 give 0, values above 1 give 255, so no NaN reaches the integer cast.
std::uint8_t Quantize(const double value) noexcept {
    if (!(value > 0.0)) return 0;
    if (value >= 1.0) return 255;
    return static_cast<std::uint8_t>(static_cast<int>(value * 255.0 + 0.5));
}

} // namespace

CameraRenderer::CameraRenderer(const PairSpec& spec, const std::size_t camera_index)
    : spec_(&spec), camera_index_(camera_index) {
    ValidatePairSpec(spec);
    if (camera_index >= spec.cameras.size()) throw std::invalid_argument("camera index is out of range");
    if (!detail::InvertMatrix3(spec.cameras[camera_index].document_to_image, image_to_document_)) {
        throw std::invalid_argument("documentToImage is not invertible");
    }
    camera_key_ = Mix(spec.seed + (static_cast<std::uint64_t>(camera_index) + 1U) * kGoldenGamma);
}

void CameraRenderer::RenderRows(
    const std::uint32_t first_row, const std::uint32_t row_count, std::uint8_t* const bgr_output) const {
    const auto& spec = *spec_;
    const auto& camera = spec.cameras[camera_index_];
    if (first_row > spec.image_height_px || row_count > spec.image_height_px - first_row) {
        throw std::invalid_argument("render rows are outside the image");
    }
    if (bgr_output == nullptr) throw std::invalid_argument("render output is null");

    const detail::ChartSampler chart(spec.chart);
    const Rgb table = detail::ChartSampler::Table();
    const auto& k = camera.intrinsics;
    const auto& hi = image_to_document_;
    const std::uint32_t samples = spec.supersample;
    const double sample_step = 1.0 / static_cast<double>(samples);
    const double sample_weight = 1.0 / static_cast<double>(samples * samples);

    std::uint8_t* out = bgr_output;
    for (std::uint32_t row = first_row; row < first_row + row_count; ++row) {
        for (std::uint32_t column = 0; column < spec.image_width_px; ++column) {
            double sum_r = 0.0;
            double sum_g = 0.0;
            double sum_b = 0.0;
            // Pixel (column, row) has its sample at (column, row) and covers
            // [column - 0.5, column + 0.5), so sub-sample s sits at column - 0.5 + (s + 0.5) / S.
            for (std::uint32_t sy = 0; sy < samples; ++sy) {
                const double py = static_cast<double>(row) - 0.5 + (static_cast<double>(sy) + 0.5) * sample_step;
                for (std::uint32_t sx = 0; sx < samples; ++sx) {
                    const double px = static_cast<double>(column) - 0.5 + (static_cast<double>(sx) + 0.5) * sample_step;
                    Vec2 ideal;
                    if (!detail::UndistortPixel(camera, px, py, ideal)) {
                        throw std::runtime_error("the lens distortion could not be inverted at a render sample");
                    }
                    // The third row of the inverse is 1 / w' of the document point; a
                    // point at w' <= 0 is behind the camera and sees the table.
                    const double w = hi[6] * ideal.x + hi[7] * ideal.y + hi[8];
                    Rgb color = table;
                    if (w > 0.0) {
                        const double doc_x = (hi[0] * ideal.x + hi[1] * ideal.y + hi[2]) / w;
                        const double doc_y = (hi[3] * ideal.x + hi[4] * ideal.y + hi[5]) / w;
                        if (std::isfinite(doc_x) && std::isfinite(doc_y)) color = chart.Sample(doc_x, doc_y);
                    }
                    sum_r += color.r;
                    sum_g += color.g;
                    sum_b += color.b;
                }
            }

            const double nx = (static_cast<double>(column) - k.cx_pixels) / k.fx_pixels;
            const double ny = (static_cast<double>(row) - k.cy_pixels) / k.fy_pixels;
            const double rho2 = nx * nx + ny * ny;
            double vignette = 1.0 + rho2 * (camera.vignette[0] + rho2 * camera.vignette[1]);
            if (vignette < 0.0) vignette = 0.0;
            const double scale = camera.exposure_gain * vignette;

            const double base[3] = {
                sum_r * sample_weight * scale * camera.white_balance_gain[0],
                sum_g * sample_weight * scale * camera.white_balance_gain[1],
                sum_b * sample_weight * scale * camera.white_balance_gain[2],
            };
            const std::uint64_t pixel_index = static_cast<std::uint64_t>(row) * spec.image_width_px + column;
            double encoded[3]{};
            for (unsigned channel = 0; channel < 3U; ++channel) {
                const double signal = base[channel] > 0.0 ? base[channel] : 0.0;
                const double sigma = camera.noise.read_sigma + camera.noise.shot_sigma * std::sqrt(signal);
                const double noise = sigma > 0.0 ? sigma * GaussianNoise(camera_key_, pixel_index * 3U + channel) : 0.0;
                encoded[channel] = base[channel] + noise;
            }
            out[0] = Quantize(encoded[2]);
            out[1] = Quantize(encoded[1]);
            out[2] = Quantize(encoded[0]);
            out += 3;
        }
    }
}

} // namespace a0::m2::synthetic
