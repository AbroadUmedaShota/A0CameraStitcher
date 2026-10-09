#pragma once

#include "a0/m2/document_render.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace a0::m2 {

// Strict ADR-0034 reader. Parsed values and their fixed-order canonical text
// are one immutable snapshot; neither the input JSON nor an assessment time
// is retained as an alternative source of applied projection values.
class RigProfileV2 final {
public:
    static RigProfileV2 Parse(std::string_view json);

    [[nodiscard]] const std::string& CanonicalFingerprintText() const noexcept;
    [[nodiscard]] std::string FingerprintSha256() const;
    [[nodiscard]] bool IsApproved() const noexcept;
    [[nodiscard]] bool IsCalibrated() const noexcept;
    [[nodiscard]] const std::optional<std::string>& ProfileId() const noexcept;

    // Product-use gate: calibrated approved only, with measuredAt <= approvedAt
    // <= assessedAt < validUntil. The sampling kernel has no implicit default.
    [[nodiscard]] render::DocumentRenderParameters ValidateApprovedForUse(
        std::string_view assessed_at_utc, render::Resampling kernel) const;

    // Evaluation gate: calibrated DRAFT only. Owner-decision blocks remain
    // null in the profile; the evaluation caller explicitly supplies its raster.
    [[nodiscard]] render::DocumentRenderParameters ValidateCalibrationForEvaluation(
        const render::OutputRaster& raster, render::Resampling kernel) const;

private:
    struct Data;
    explicit RigProfileV2(std::shared_ptr<const Data> data);
    std::shared_ptr<const Data> data_;
};

} // namespace a0::m2
