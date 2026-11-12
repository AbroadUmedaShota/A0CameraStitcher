#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/content_registration.hpp"
#include "a0/m2/experimental/fixed_composite_condition.hpp"
#include "a0/m2/experimental/fixed_composite_profile_common.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental {
// Fixed composite condition v3 (#417, docs/design/fixed-composite-registration.md 11.9, 11.19, 11.20, 11.27.1): the record
// that the registered composite is made with. Development only and always unapproved; it holds no approval and creates
// none. The field names, types and units are in docs/design/fixed-composite-registration-formats.md.
//
// What v3 adds to v2's identification of the rig (profile by hash and fingerprint, orientation, output raster, seam column):
//  - a revision: v3 is a chain of immutable files r<6 digits>.json in a folder (11.19.4), each naming its parent by SHA-256;
//  - the base pose p_b (the pose that the ranges are measured from and that the fixed level G4 uses);
//  - the ranges and lines of the registration (B's envelope, the checks' limits, the geometry of W and Omega, the paper
//    surface's range, the paper-lift line, whether an amount beyond a range is still applied, the scale-warning line), copied
//    from the registration policy file when the record is made and carrying the policy file's hash;
//  - the seam gate's line L and guard band U.
// v2's scale limit (scaleAutoCorrectionMaxPercent) is not carried: the height component of the pose does the same work and
// the mapping uses no scale k (11.20.2).
//
// There is no default for any range, line or limit in code (design 11.13 item 6): a member that holds one is NaN or
// optional, and ValidateFixedCompositeConditionV3 refuses an unset one. A layer whose range is absent is not applied, and the
// record says so by writing null (the reader does not accept a missing member).
//
// What is checked where (11.23): the record's format on reading (every capture, inside the 180-second deadline); the checks
// that need the mapped composite -- the seam and half the blend width inside B's cover over the whole parameter range, the
// strain bound of layer 2, the base pose inside B's envelope -- when the record is created and again on reading in the cheap
// form of fixed_composite_creation_v3.hpp.

inline constexpr std::string_view kFixedCompositeConditionSchemaV3="a0.fixed-composite-condition.v3";
// v3's own ceiling (not v2's 4096): the revision, the ranges and the lines are in the file. A proposal.
inline constexpr std::size_t kMaximumFixedCompositeConditionV3Bytes=16*1024;
// The folder (beside the settings) that holds the revisions, and the file name of revision n.
inline constexpr std::string_view kFixedCompositeRevisionFolderName="wpd-fixed-composite-revisions";
inline constexpr std::uint32_t kMaximumFixedCompositeRevisionSequence=999999;
// "r000003.json" for 3. Throws std::invalid_argument for 0 or more than kMaximumFixedCompositeRevisionSequence.
[[nodiscard]] std::string FixedCompositeRevisionFileName(std::uint32_t sequence);
// The sequence of a revision file name, nullopt for any other name (including "r3.json", "r000003.json.partial").
[[nodiscard]] std::optional<std::uint32_t> ParseFixedCompositeRevisionFileName(std::string_view name);

// What a revision changed (11.19.3). The members after `kind` are those of the kind and nothing else; the writer and the
// reader accept exactly the set of the kind.
enum class FixedCompositeRevisionKind {created,migrated_from_v2,saved_estimate,manual_adjustment,rollback,values};
[[nodiscard]] constexpr std::string_view FixedCompositeRevisionKindText(FixedCompositeRevisionKind k){
    switch(k){
    case FixedCompositeRevisionKind::created:return "created";
    case FixedCompositeRevisionKind::migrated_from_v2:return "migrated-from-v2";
    case FixedCompositeRevisionKind::saved_estimate:return "saved-estimate";
    case FixedCompositeRevisionKind::manual_adjustment:return "manual-adjustment";
    case FixedCompositeRevisionKind::rollback:return "rollback";
    case FixedCompositeRevisionKind::values:return "values";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<FixedCompositeRevisionKind> ParseFixedCompositeRevisionKind(std::string_view text){
    for(const auto k:{FixedCompositeRevisionKind::created,FixedCompositeRevisionKind::migrated_from_v2,FixedCompositeRevisionKind::saved_estimate,
        FixedCompositeRevisionKind::manual_adjustment,FixedCompositeRevisionKind::rollback,FixedCompositeRevisionKind::values})
        if(FixedCompositeRevisionKindText(k)==text)return k;
    return std::nullopt;
}
struct FixedCompositeRevisionChangeV3 {
    FixedCompositeRevisionKind kind{FixedCompositeRevisionKind::created};
    // migrated_from_v2: SHA-256 of the v2 file the values were taken from.
    std::optional<std::string> v2_sha256;
    // saved_estimate: the report the pose was taken from and its pair.
    std::optional<std::string> report_sha256,pair_id;
    // manual_adjustment: what the operator entered, as the displacement at the seam in output pixels and a rotation in degrees,
    // and the largest change of the seam column's displacement between the parent's base pose and the new one. There is no
    // upper bound on it (the owner's decision 9): it is recorded, never refused.
    struct Entered {double seam_dx_px=std::numeric_limits<double>::quiet_NaN(),seam_dy_px=std::numeric_limits<double>::quiet_NaN(),
        rotation_deg=std::numeric_limits<double>::quiet_NaN();};
    std::optional<Entered> entered;
    std::optional<double> maximum_seam_displacement_px;
    // rollback: SHA-256 of the revision file whose content was written again.
    std::optional<std::string> restored_sha256;
};

struct FixedCompositeConditionV3 {
    // UTC, "YYYY-MM-DDTHH:MM:SSZ".
    std::string created_utc;
    struct Revision {
        std::uint32_t sequence{};               // >= 1; equal to the number in the file name
        std::optional<std::string> parent_sha256;   // none for sequence 1 and only for it
        FixedCompositeRevisionChangeV3 change;
    } revision;
    struct Source {
        // As in v2: the profile's plain file name beside the condition's folder, the file's SHA-256 and the profile fingerprint.
        std::string profile_file;
        std::string profile_file_sha256,profile_fingerprint_sha256;
        // SHA-256 of the registration policy file (wpd-registration-policy.json) that the ranges and lines were copied from.
        std::string policy_sha256;
    } source;
    MountOrientation orientation{MountOrientation::normal};
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};
    struct Seam {
        std::uint32_t column{};
        FixedSeamOrigin origin{FixedSeamOrigin::band_center_provisional};
    } seam;
    // p_b: against the calibrated pose; degrees, mm, percent. All zero for `created` and `migrated_from_v2`.
    content_registration::PoseParameters base_pose;

    struct Registration {
        // The ranges are measured on p-hat minus p_b (11.19.2).
        content_registration::RegistrationEnvelope envelope;
        content_registration::RegistrationLimits limits;      // the nine acceptance values of the estimator's checks (development values)
        struct Geometry {                                      // output px; the blend width comes from the composite's design
            std::uint32_t omega_margin_px{},guard_px{},band_half_width_px{};
        } geometry;
        std::optional<SurfaceEnvelope> surface_envelope;      // null: the paper surface is not estimated (six degrees of freedom)
        std::optional<bool> apply_beyond_envelope;            // required; true: an amount beyond a range is applied when the estimate is trusted, and warned about
        // The scale warning line (percent). The field exists; the owner's value is null: no warning about the scale of the drawing.
        std::optional<double> surface_scale_warn_percent;
        struct Parallax {double max_lift_mm=std::numeric_limits<double>::quiet_NaN();};
        std::optional<Parallax> parallax;                     // null: layer 2 is not tried
        // Reserved for the live view (11.21): the estimator's limits in the live view's pixels. Written as null until then.
        struct Live {content_registration::RegistrationLimits limits;};
        std::optional<Live> live;
    } registration;
    struct SeamGateLine {                                      // #385 owner values
        double limit_px=std::numeric_limits<double>::quiet_NaN();
        double guard_band_px=std::numeric_limits<double>::quiet_NaN();
    } seam_gate;
    struct RevisionRules {
        // Reserved. The field exists; the owner's value is null, which means no upper bound on a manual adjustment (decision 9).
        // No processing refuses an adjustment by size.
        std::optional<double> manual_adjustment_max_seam_px;
    } revision_rules;
};

// Throws std::invalid_argument unless every field is in range: timestamps and hashes well formed, the profile name plain, the
// raster exact, the seam column inside the raster, the revision's members those of its kind, every range, line and limit set
// (finite and positive where they are ranges), and the base pose finite, and, except for a manual adjustment, the base pose inside
// registration.envelope. Checks nothing that needs the profile.
//
// A manual adjustment is not held to the envelope when it is written (the owner's decision: the size of a hand adjustment has no
// upper bound), so that it can be written, read back, loaded by the tools and rolled back. A reader that uses the condition for a
// capture or for the live view still refuses a base pose outside the envelope: RequireFixedCompositeBasePoseInEnvelopeV3, which
// LoadFixedCompositeConditionAny (and so SelectFixedComposite) applies to every record it returns.
void ValidateFixedCompositeConditionV3(const FixedCompositeConditionV3& c);
// Throws std::invalid_argument when the base pose is outside registration.envelope, whatever the kind of the revision.
void RequireFixedCompositeBasePoseInEnvelopeV3(const FixedCompositeConditionV3& c);
// Canonical text of a validated record, no trailing newline. Always carries "approval":"unapproved".
[[nodiscard]] std::string FormatFixedCompositeConditionV3(const FixedCompositeConditionV3& c);
// Strict reader of the format alone (no file access). Rejects malformed JSON, duplicate keys, a different schema, missing or
// extra members, values of the wrong kind, "approved", more than kMaximumFixedCompositeConditionV3Bytes, and anything
// ValidateFixedCompositeConditionV3 refuses. Every failure is a std::invalid_argument.
[[nodiscard]] FixedCompositeConditionV3 ParseFixedCompositeConditionV3(std::string_view text);

// The registration policy file (a0.wpd-registration-policy.v1, docs/design/fixed-composite-registration.md 11.20.2): the one place where
// the ranges, lines and limits are written down. Its members are "schema" and the condition's own "registration", "seamGate" and
// "revisionRules" (same names, same types). Only the tool that makes and revises a record reads it; the capture path reads the condition.
struct FixedCompositeRegistrationValuesV3 {
    FixedCompositeConditionV3::Registration registration;
    FixedCompositeConditionV3::SeamGateLine seam_gate;
    FixedCompositeConditionV3::RevisionRules revision_rules;
};
inline constexpr std::size_t kMaximumFixedCompositeRegistrationPolicyV3Bytes=16*1024;   // a proposal
// Strict reader of the policy text (no file access): duplicate and unknown members, a missing member, a value of the wrong kind, a schema
// other than expected_schema, and any range, line or limit that is unset or out of range (the checks of ValidateFixedCompositeConditionV3)
// are refused with std::invalid_argument. A layer that is not to be applied is null; nothing has a default.
[[nodiscard]] FixedCompositeRegistrationValuesV3 ParseFixedCompositeRegistrationPolicyV3(std::string_view text,std::string_view expected_schema);

// The profile a v3 record names, verified like BindFixedCompositeProfile does for v2. Throws FixedCompositeProfileError /
// std::invalid_argument on the same grounds.
[[nodiscard]] FixedCompositeProfileBinding BindFixedCompositeProfileV3(const FixedCompositeConditionV3& c,std::string_view profile_bytes,
    const FixedCompositeSha256& hash);

// The checks of fixed_composite_creation_v3.hpp, passed in by the creating entrance.
using FixedCompositeCreationCheckV3=std::function<void(const FixedCompositeConditionV3&,const FixedCompositeProfileBinding&)>;
// What a person supplies when making a v3 record. Nothing here has a default that could pass for a decision; the ranges and
// lines are copied from the policy file by the tool (fixed_composite_tool_v3.hpp) and arrive here as `registration`,
// `seam_gate` and `revision_rules`.
struct FixedCompositeV3Request {
    std::string profile_file;
    std::string profile_bytes;                       // the bytes of the profile file as it will sit beside the folder
    std::string policy_sha256;
    std::uint32_t sequence{};
    std::optional<std::string> parent_sha256;
    FixedCompositeRevisionChangeV3 change;
    MountOrientation orientation{MountOrientation::normal};
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};
    std::uint32_t seam_column{};
    FixedSeamOrigin seam_origin{FixedSeamOrigin::band_center_provisional};
    content_registration::PoseParameters base_pose;
    FixedCompositeConditionV3::Registration registration;
    FixedCompositeConditionV3::SeamGateLine seam_gate;
    FixedCompositeConditionV3::RevisionRules revision_rules;
};
// Pure: builds the record, verifies the profile bytes, runs the creation check and validates. Reads and writes no file. Throws
// std::invalid_argument for any failure, including an empty hash or check. `created` is a parameter so the time is explicit.
[[nodiscard]] FixedCompositeConditionV3 MakeFixedCompositeConditionV3(const FixedCompositeV3Request& request,
    std::chrono::system_clock::time_point created,const FixedCompositeSha256& hash,const FixedCompositeCreationCheckV3& check);

struct LoadedFixedCompositeConditionV3 {
    FixedCompositeConditionV3 condition;
    // SHA-256 of the exact condition file bytes, for reports that record which condition was used.
    std::string file_sha256;
    FixedCompositeProfileBinding profile;
};
// Reads one revision file: strict format, then the profile beside it (the folder of `file`) verified against the record.
// Throws like LoadFixedCompositeConditionAny does for v2 (FixedCompositeProfileError for the profile). This is the v3 branch
// of LoadFixedCompositeConditionAny, which does not call it until the capture path is wired.
[[nodiscard]] LoadedFixedCompositeConditionV3 LoadFixedCompositeConditionV3(const std::filesystem::path& file,const FixedCompositeSha256& hash);
// The revision in force in `folder` (the revisions folder): the highest number, with an unbroken chain from r000001 and each
// parent hash equal to the SHA-256 of the previous file (11.19.4). An absent folder or no revision is nullopt; a gap, a mismatch
// or a file whose name is not a revision (a `.partial` is not counted) is std::invalid_argument, never a guess.
[[nodiscard]] std::optional<LoadedFixedCompositeConditionV3> LoadActiveFixedCompositeRevisionV3(const std::filesystem::path& folder,
    const FixedCompositeSha256& hash);

// Writes the record as a NEW revision file, with the steps and guarantees of WriteFixedCompositeConditionV2 (exclusive
// partial, read back, move without replacing; an existing file or partial is never overwritten, so two writers of the same
// number cannot both succeed). The file name is FixedCompositeRevisionFileName(condition.revision.sequence), in `folder`.
void WriteFixedCompositeConditionV3(const std::filesystem::path& folder,const FixedCompositeConditionV3& condition);
}
