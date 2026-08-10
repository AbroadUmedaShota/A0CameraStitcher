#include "a0/m2/synthetic_measurement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace a0::m2 {
namespace {

constexpr double kLuminanceEpsilon = 1.0e-12;
constexpr double kMaximumNormalizedChannel = 1.0;
constexpr std::size_t kMaximumSyntheticDimension = 4096;

void RequireNonBlank(const std::string& value, const std::string_view name) {
    if (value.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument(std::string(name) + " is required");
    }
}

void RequireFiniteNonNegative(const double value, const std::string_view name) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument(std::string(name) + " must be finite and non-negative");
    }
}

void ValidateImage(const SyntheticImage& image, const std::string_view name) {
    if (image.width_pixels == 0 || image.height_pixels == 0) {
        throw std::invalid_argument(std::string(name) + " dimensions must be non-zero");
    }
    if (image.width_pixels > kMaximumSyntheticDimension || image.height_pixels > kMaximumSyntheticDimension) {
        throw std::invalid_argument(std::string(name) + " dimensions exceed the synthetic fixture limit");
    }
    if (image.width_pixels > std::numeric_limits<std::size_t>::max() / image.height_pixels
        || image.pixels.size() != image.width_pixels * image.height_pixels) {
        throw std::invalid_argument(std::string(name) + " pixel count does not match dimensions");
    }

    for (const auto& pixel : image.pixels) {
        for (const double channel : {pixel.red, pixel.green, pixel.blue}) {
            if (!std::isfinite(channel) || channel < 0.0 || channel > kMaximumNormalizedChannel) {
                throw std::invalid_argument(std::string(name) + " channels must be finite normalized values");
            }
        }
    }
}

double Luminance(const RgbPixel& pixel) {
    return 0.2126 * pixel.red + 0.7152 * pixel.green + 0.0722 * pixel.blue;
}

struct Moments {
    double total_weight{};
    double mean_luminance{};
    double centroid_x{};
    double centroid_y{};
    double covariance_xx{};
    double covariance_xy{};
    double covariance_yy{};
    double weighted_red{};
    double weighted_green{};
    double weighted_blue{};
};

Moments ComputeMoments(const SyntheticImage& image) {
    Moments moments;
    for (std::size_t y = 0; y < image.height_pixels; ++y) {
        for (std::size_t x = 0; x < image.width_pixels; ++x) {
            const auto& pixel = image.pixels[y * image.width_pixels + x];
            const double weight = Luminance(pixel);
            moments.total_weight += weight;
            moments.mean_luminance += weight;
            moments.centroid_x += static_cast<double>(x) * weight;
            moments.centroid_y += static_cast<double>(y) * weight;
            moments.weighted_red += pixel.red * weight;
            moments.weighted_green += pixel.green * weight;
            moments.weighted_blue += pixel.blue * weight;
        }
    }
    if (!std::isfinite(moments.total_weight) || moments.total_weight <= kLuminanceEpsilon) {
        throw std::invalid_argument("synthetic images must contain measurable luminance");
    }

    moments.mean_luminance = moments.total_weight
        / static_cast<double>(image.width_pixels * image.height_pixels);
    moments.centroid_x /= moments.total_weight;
    moments.centroid_y /= moments.total_weight;

    for (std::size_t y = 0; y < image.height_pixels; ++y) {
        for (std::size_t x = 0; x < image.width_pixels; ++x) {
            const double weight = Luminance(image.pixels[y * image.width_pixels + x]);
            const double dx = static_cast<double>(x) - moments.centroid_x;
            const double dy = static_cast<double>(y) - moments.centroid_y;
            moments.covariance_xx += dx * dx * weight;
            moments.covariance_xy += dx * dy * weight;
            moments.covariance_yy += dy * dy * weight;
        }
    }
    moments.covariance_xx /= moments.total_weight;
    moments.covariance_xy /= moments.total_weight;
    moments.covariance_yy /= moments.total_weight;
    moments.weighted_red /= moments.total_weight;
    moments.weighted_green /= moments.total_weight;
    moments.weighted_blue /= moments.total_weight;
    return moments;
}

double PrincipalAxisRadians(const Moments& moments) {
    const double anisotropy = std::hypot(
        moments.covariance_xx - moments.covariance_yy,
        2.0 * moments.covariance_xy);
    if (!std::isfinite(anisotropy) || anisotropy <= kLuminanceEpsilon) {
        throw std::invalid_argument("synthetic images must contain an anisotropic registration feature");
    }
    return 0.5 * std::atan2(
        2.0 * moments.covariance_xy,
        moments.covariance_xx - moments.covariance_yy);
}

double PrincipalAxisDifferenceDegrees(const Moments& first, const Moments& second) {
    const double period = std::numbers::pi;
    const double raw_difference = PrincipalAxisRadians(second) - PrincipalAxisRadians(first);
    const double wrapped = std::fmod(raw_difference + period / 2.0, period);
    const double shortest = (wrapped < 0.0 ? wrapped + period : wrapped) - period / 2.0;
    return std::abs(shortest) * 180.0 / std::numbers::pi;
}

double DeltaE76(const Moments& first, const Moments& second) {
    const auto to_lab = [](const Moments& moments) {
        const double red = moments.weighted_red <= 0.04045
            ? moments.weighted_red / 12.92
            : std::pow((moments.weighted_red + 0.055) / 1.055, 2.4);
        const double green = moments.weighted_green <= 0.04045
            ? moments.weighted_green / 12.92
            : std::pow((moments.weighted_green + 0.055) / 1.055, 2.4);
        const double blue = moments.weighted_blue <= 0.04045
            ? moments.weighted_blue / 12.92
            : std::pow((moments.weighted_blue + 0.055) / 1.055, 2.4);

        const double x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047;
        const double y = (0.2126729 * red + 0.7151522 * green + 0.0721750 * blue);
        const double z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883;
        const auto f = [](const double value) {
            constexpr double kEpsilon = 216.0 / 24389.0;
            constexpr double kKappa = 24389.0 / 27.0;
            return value > kEpsilon ? std::cbrt(value) : (kKappa * value + 16.0) / 116.0;
        };
        const double fx = f(x);
        const double fy = f(y);
        const double fz = f(z);
        return std::array<double, 3>{116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
    };

    const auto first_lab = to_lab(first);
    const auto second_lab = to_lab(second);
    return std::sqrt(
        std::pow(first_lab[0] - second_lab[0], 2.0)
        + std::pow(first_lab[1] - second_lab[1], 2.0)
        + std::pow(first_lab[2] - second_lab[2], 2.0));
}

void ValidateMeasuredValues(const ImageMeasurements& measurements) {
    RequireFiniteNonNegative(measurements.shift_pixels, "shift pixels");
    RequireFiniteNonNegative(measurements.rotation_degrees, "rotation degrees");
    RequireFiniteNonNegative(measurements.scale_difference_percent, "scale difference percent");
    RequireFiniteNonNegative(measurements.exposure_difference_ev, "exposure difference EV");
    RequireFiniteNonNegative(measurements.color_delta_e, "color DeltaE");
    RequireFiniteNonNegative(measurements.overlap_percent, "overlap percent");
    if (measurements.overlap_percent > 100.0) {
        throw std::invalid_argument("overlap percent must be within 0 through 100");
    }
}

CorrectionProposal RejectedProposal(
    const SyntheticMeasurement& measurement,
    std::vector<std::string> reasons) {
    return {
        CorrectionProposalStatus::rejected,
        measurement.fixture_id,
        measurement.profile_provenance,
        measurement.measurement_provenance,
        measurement.measurements,
        SetupReadiness::physical_adjustment_required,
        false,
        std::move(reasons),
    };
}

} // namespace

SyntheticMeasurement MeasureSyntheticPair(const SyntheticPairFixture& fixture) {
    RequireNonBlank(fixture.fixture_id, "fixture id");
    if (fixture.schema_version != kSyntheticImagePairSchemaVersion) {
        throw std::invalid_argument("synthetic fixture schema version is unsupported");
    }
    RequireNonBlank(fixture.profile_provenance, "profile provenance");
    RequireNonBlank(fixture.measurement_provenance, "measurement provenance");
    ValidateImage(fixture.camera_a, "camera A image");
    ValidateImage(fixture.camera_b, "camera B image");
    if (fixture.camera_a.width_pixels != fixture.camera_b.width_pixels
        || fixture.camera_a.height_pixels != fixture.camera_b.height_pixels) {
        throw std::invalid_argument("synthetic camera images must have matching dimensions");
    }
    RequireFiniteNonNegative(fixture.overlap_percent, "overlap percent");
    if (fixture.overlap_percent > 100.0) {
        throw std::invalid_argument("overlap percent must be within 0 through 100");
    }

    const Moments camera_a = ComputeMoments(fixture.camera_a);
    const Moments camera_b = ComputeMoments(fixture.camera_b);
    const double scale_ratio = std::sqrt(
        (camera_b.covariance_xx + camera_b.covariance_yy)
        / (camera_a.covariance_xx + camera_a.covariance_yy));
    if (!std::isfinite(scale_ratio) || scale_ratio <= 0.0) {
        throw std::invalid_argument("synthetic images have an invalid scale measurement");
    }
    // A scale change alters the covered pixel area in this synthetic raster. Remove that
    // deterministic geometric area factor before reporting the independent exposure term.
    const double exposure_ratio = camera_b.mean_luminance
        / camera_a.mean_luminance
        / (scale_ratio * scale_ratio);
    if (!std::isfinite(exposure_ratio) || exposure_ratio <= 0.0) {
        throw std::invalid_argument("synthetic images have an invalid exposure measurement");
    }

    ImageMeasurements measurements{
        std::hypot(camera_b.centroid_x - camera_a.centroid_x, camera_b.centroid_y - camera_a.centroid_y),
        PrincipalAxisDifferenceDegrees(camera_a, camera_b),
        std::abs(scale_ratio - 1.0) * 100.0,
        std::abs(std::log2(exposure_ratio)),
        DeltaE76(camera_a, camera_b),
        fixture.overlap_percent,
    };
    ValidateMeasuredValues(measurements);
    return {
        fixture.fixture_id,
        fixture.schema_version,
        fixture.profile_provenance,
        fixture.measurement_provenance,
        measurements,
    };
}

CorrectionProposal ProposeBoundedCorrection(
    const SyntheticMeasurement& measurement,
    const RigProfileContract& profile,
    const bool full_coverage,
    const bool camera_settings_compatible) {
    try {
        RequireNonBlank(measurement.fixture_id, "measurement fixture id");
        if (measurement.fixture_schema_version != kSyntheticImagePairSchemaVersion) {
            return RejectedProposal(measurement, {"measurement fixture schema version is unsupported"});
        }
        RequireNonBlank(measurement.measurement_provenance, "measurement provenance");
        RequireNonBlank(measurement.profile_provenance, "measurement profile provenance");
        RequireNonBlank(profile.trust.provenance, "profile provenance");
        ValidateMeasuredValues(measurement.measurements);
        if (measurement.profile_provenance != profile.trust.provenance) {
            return RejectedProposal(measurement, {"measurement and profile provenance do not match"});
        }

        const auto& envelope = profile.correction_envelope;
        const SetupAssessmentInput assessment_input{
            profile.trust,
            full_coverage,
            camera_settings_compatible,
            measurement.measurements.overlap_percent,
            envelope.minimum_overlap_percent,
            {measurement.measurements.shift_pixels,
             envelope.registration_pixels.target_max,
             envelope.registration_pixels.auto_correction_max},
            {measurement.measurements.rotation_degrees,
             envelope.rotation_degrees.target_max,
             envelope.rotation_degrees.auto_correction_max},
            {measurement.measurements.scale_difference_percent,
             envelope.scale_difference_percent.target_max,
             envelope.scale_difference_percent.auto_correction_max},
            {measurement.measurements.exposure_difference_ev,
             envelope.exposure_difference_ev.target_max,
             envelope.exposure_difference_ev.auto_correction_max},
            {measurement.measurements.color_delta_e,
             envelope.color_delta_e.target_max,
             envelope.color_delta_e.auto_correction_max},
        };
        const auto assessment = AssessSetup(assessment_input);
        if (assessment.readiness == SetupReadiness::physical_adjustment_required) {
            return {
                CorrectionProposalStatus::rejected,
                measurement.fixture_id,
                profile.trust.provenance,
                measurement.measurement_provenance,
                measurement.measurements,
                assessment.readiness,
                false,
                assessment.reasons,
            };
        }
        return {
            assessment.readiness == SetupReadiness::ready_auto_correction
                ? CorrectionProposalStatus::accepted
                : CorrectionProposalStatus::not_required,
            measurement.fixture_id,
            profile.trust.provenance,
            measurement.measurement_provenance,
            measurement.measurements,
            assessment.readiness,
            assessment.apply_transient_correction,
            assessment.reasons,
        };
    } catch (const std::invalid_argument& error) {
        return RejectedProposal(measurement, {std::string("malformed measurement or profile: ") + error.what()});
    }
}

} // namespace a0::m2
