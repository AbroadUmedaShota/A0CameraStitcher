#pragma once
#include "a0/m2/experimental/capture_registration.hpp"
#include "a0/m2/experimental/confirmed_position.hpp"
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/seam_blend.hpp"

#include <cstdint>
#include <vector>

namespace a0::m2::experimental {
// The checks that a hand step has to pass before the confirmed position changes (#437 stage 2, docs/DECISIONS.md ADR-0046 decision 4):
// the sum of the entered amounts is solved once from the position before the first hand press (confirmed_position.hpp), the result's
// pose is measured against the condition's pose envelope, and the result's mapping goes through the cheap cover check that a capture
// makes on the mapping it chose. Pure computation: no window, clock, file or thread. A step that is refused changes nothing and is
// never rounded to an edge.
//
// - Only the pose envelope is read (registration.envelope: position, rotation, tilt and height, measured from the condition's base
//   pose p_b by RegistrationComponentsFromBase; exactly at a limit is inside). The paper surface's range and layer 2's line are not.
//   A hand step moves position and rotation only, so a position whose tilt or height is already outside the envelope has no step
//   that leads inside, and a position whose position or rotation is outside has only the steps whose result is inside.
// - The cover check is CheckRegisteredMappingCover with the condition's profile, the seam column and the design's blend width, the
//   answer a capture's RegisteredMappingCoverOnThreads gives for the same mapping.
// - A sum of 0 in all three counts is not a step. It is accepted, gives hand_base back as it is, and is neither measured against the
//   envelope nor covered again: that position was the one drawn before the first hand press, and going back to it is always allowed.
// - Order: the solve (cheap), then the envelope (cheap), then the cover (the costly one), so a refusal by the envelope costs no cover.
// - Linking: this is in a0_m2_confirmed_position, which needs the condition library and the estimator's range check; the arithmetic of
//   confirmed_position.hpp does not.

enum class HandStepStatus {
    accepted,
    refused_envelope,       // the result's pose is outside the pose envelope (components says which)
    refused_cover,          // the result's mapping fails the cheap cover check (seam_covered / output_covered say which)
    refused_unsolvable,     // the amounts could not be turned into a pose (no convergence, a surface that cannot be drawn)
};

struct HandStepInput {
    const render::DocumentRenderParameters* parameters{};          // the condition's profile: both cameras and the output raster (required)
    std::uint32_t seam_column{};                                    // the condition's seam column
    SeamBlendSettings blend{kSeamBlendDesign};                      // the design's blend width, not a value of the condition
    content_registration::PoseParameters base_pose;                 // p_b: the condition's base pose, the origin of the range
    content_registration::RegistrationEnvelope envelope;            // the condition's registration.envelope (all four limits set)
    RegisteredMapping hand_base;                                    // the position before the first hand press: its surface is kept, no layer 2
    HandCounts total;                                               // the sum of the amounts entered since
};

struct HandStepVerdict {
    HandStepStatus status{HandStepStatus::refused_unsolvable};
    // The position the step gives. Filled when the amounts were solved (accepted, refused_envelope and refused_cover); otherwise it
    // is hand_base. A refused step's mapping is not to be used.
    RegisteredMapping mapping;
    // The four components of the result against the envelope (position, rotation, tilt, height), filled when the amounts were solved
    // (not for a sum of 0).
    std::vector<content_registration::ComponentCheck> components;
    // The cover answer, filled when the cover check ran (accepted and refused_cover). A check that threw is read the way a capture reads
    // it (SelectGeometryLevel): the seam is not covered.
    bool cover_checked{};
    bool seam_covered{};
    bool output_covered{};
};

// Throws std::invalid_argument for input that is not a request (no parameters, a seam column outside the raster, an envelope that is not
// set, a hand_base that carries layer 2 or is not finite); a step that cannot be taken is a verdict, not an exception.
[[nodiscard]] HandStepVerdict CheckHandStep(const HandStepInput& input);
}
