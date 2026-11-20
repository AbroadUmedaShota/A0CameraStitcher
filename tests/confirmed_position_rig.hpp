#pragma once
// The rig of the confirmed position's tests (#437 stage 2): a fixed composite condition v3 made from the shared rig profile v2 fixture, as the
// condition tool's own test makes one (18 mm x 8 mm inside the 39 mm overlap of the fixture cameras, 180 x 80 pixels at 254 DPI, seam column 90).
// Pure computation and one file read. Every range and limit is a value of the tests; the product code has no default for any of them.
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_creation_v3.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace a0::tests::confirmed_position_rig {
namespace m2=a0::m2::experimental;
namespace render=a0::m2::render;

inline std::string ReadAll(const std::filesystem::path& path){
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),{}};
}

struct Rig {
    m2::FixedCompositeConditionV3 condition;
    m2::FixedCompositeProfileBinding binding;
};

// The rig of the condition tool's own test, with the pose range {5 mm, 1.5 deg, 2.5 deg, 3 %}.
inline Rig MakeRig(const std::string& draft){
    using namespace std::chrono_literals;
    const m2::FixedCompositeSha256 hash=[](const std::vector<unsigned char>& bytes){return m2::FixedCompositeBytesSha256(bytes);};
    const auto now=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h};
    m2::FixedCompositeV3Request request;
    request.profile_file="wpd-fixed-composite-profile-test.json";request.profile_bytes=draft;request.policy_sha256=std::string(64,'d');
    request.sequence=1;request.change.kind=m2::FixedCompositeRevisionKind::created;
    request.raster=render::OutputRaster{render::DocumentRegionUm{580000,100000,598000,108000},254,180,80};
    request.seam_column=90;request.seam_origin=m2::FixedSeamOrigin::band_center_provisional;
    request.registration.envelope={5,1.5,2.5,3};
    request.registration.limits.brown_lowe_alpha=8;request.registration.limits.brown_lowe_beta=0.3;request.registration.limits.minimum_unique_blocks=3;
    request.registration.limits.maximum_prediction_sigma_px=0.5;request.registration.limits.maximum_residual_rms_px=0.75;request.registration.limits.maximum_residual_p95_px=1.5;
    request.registration.limits.maximum_bin_bias_px=0.4;request.registration.limits.convergence_px=0.05;request.registration.limits.start_agreement_px=0.25;
    request.registration.geometry={2,72,96};
    request.registration.surface_envelope=m2::SurfaceEnvelope{10,4};request.registration.apply_beyond_envelope=true;
    request.registration.parallax=std::nullopt;
    request.seam_gate={1.0,8.0};
    m2::CreationCheckDetailsV3 details;
    auto condition=m2::MakeFixedCompositeConditionV3(request,now,hash,m2::MakeFixedCompositeCreationCheckV3(&details,m2::CreationCheckOptionsV3{}));
    auto binding=m2::BindFixedCompositeProfileV3(condition,draft,hash);
    return {std::move(condition),std::move(binding)};
}
}
