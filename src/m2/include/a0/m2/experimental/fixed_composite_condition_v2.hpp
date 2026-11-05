#pragma once
#include "a0/m2/experimental/fixed_composite_condition.hpp"
#include "a0/m2/document_render.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace a0::m2::experimental {
// Fixed composite condition v2 (#405 W1, parent #396): the lens-aware sibling
// of the v1 record of #399. Development only and always unapproved; it holds
// no approval and creates none.
//
// v1 stores a similarity (translation, rotation, scale) taken from a saved
// review. v2 stores no mapping values at all. It names a calibrated DRAFT rig
// profile (rig profile 2.0.0, #374) beside it by the SHA-256 of the file and by
// the profile fingerprint (ADR-0034), and adds what a draft profile never
// holds because it is an owner decision there: the output raster, the seam
// column in that raster, and the largest per-capture scale difference. Here
// those are development values chosen by a person; they are not approved.
//
// Both versions live in one file name (kFixedCompositeConditionName). The
// "schema" member says which one it is; LoadFixedCompositeConditionAny reads
// either. The v1 reader, writer and report are unchanged. Switching from v1 to
// v2 is a person's action: move the v1 file away first, then write the v2
// file (the writer never replaces an existing file).
//
// What is checked where (design docs/design/fixed-composite-lens.md, 3.9):
//  - reading (every capture, inside the 180-second deadline): the record's
//    format, the profile file's SHA-256 and fingerprint, that the profile is a
//    calibrated draft with A on the left and B on the right, and that its
//    projections are valid over the recorded output raster. Cheap by design.
//  - creating (once, by a person): the above plus the checks that need the
//    mapped composite -- the output covered by A or B for every scale in the
//    allowed range, the seam (and half the blend width) inside the overlap on
//    every row, and a provisional seam equal to the band middle. Those come in
//    as FixedCompositeCreationCheck because they need the mapped renderer
//    (#405 W2); creation refuses to run without one.
inline constexpr std::string_view kFixedCompositeConditionSchemaV2="a0.fixed-composite-condition.v2";
inline constexpr std::size_t kMaximumFixedCompositeConditionV2Bytes=4096;
// Same ceiling as the rig profile reader.
inline constexpr std::size_t kMaximumFixedCompositeProfileBytes=256*1024;

// SHA-256, lower-case hex, of the bytes. The caller's provider, as in v1.
using FixedCompositeSha256=std::function<std::string(const std::vector<unsigned char>&)>;

struct FixedCompositeConditionV2 {
    // UTC, "YYYY-MM-DDTHH:MM:SSZ".
    std::string created_utc;
    // SHA-256 of the file this record replaces; none for the first record
    // (#396 A3). Which record is in force is #403's decision, not this record's.
    std::optional<std::string> parent_sha256;
    struct Source {
        // File name of the profile, in the same folder as the condition. A name
        // only: no folder part. See IsFixedCompositeProfileFileName.
        std::string profile_file;
        // SHA-256 of the profile file's bytes, and the profile fingerprint
        // (RigProfileV2::FingerprintSha256). Both must match when reading.
        std::string profile_file_sha256,profile_fingerprint_sha256;
    } source;
    // The orientation the profile was calibrated in.
    MountOrientation orientation{MountOrientation::normal};
    // Development output raster: region in micrometres of the document plane,
    // integer DPI, and the pixel counts they imply (ceiling, ADR-0034). Not an
    // owner decision (HG-0001) and not approved. No code default.
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};
    // Largest per-capture scale difference, in percent, that may be corrected.
    // Required and without a code default (see v1). Not-a-number until set, so
    // a forgotten value fails instead of meaning "never correct".
    double scale_auto_correction_max_percent{std::numeric_limits<double>::quiet_NaN()};
    struct Seam {
        // Output raster pixel column, 0 <= column < raster.width_pixels.
        std::uint32_t column{};
        FixedSeamOrigin origin{FixedSeamOrigin::band_center_provisional};
    } seam;
};

// True for a plain file name that may be stored as the profile reference:
// 6..96 characters of [A-Za-z0-9._-], starting with a letter or digit, ending
// ".json", no "..", not a Windows device name, and not the condition's own name.
// Folder separators, drive letters and parent folders are all outside this set.
bool IsFixedCompositeProfileFileName(std::string_view name);
// "wpd-fixed-composite-profile-<first 16 hex of the file SHA-256>.json". The
// name the creating entrance copies the profile to. Not required of a record:
// the SHA-256 in the record is what identifies the profile.
std::string FixedCompositeProfileFileName(const std::string& profile_file_sha256);

// Throws std::invalid_argument unless every field is in range: timestamps and
// hashes well formed, the profile name plain, the raster exact (micrometres and
// DPI against the pixel counts), the limit finite and non-negative, the seam
// column inside the raster. Checks nothing that needs the profile.
void ValidateFixedCompositeConditionV2(const FixedCompositeConditionV2& c);

// Canonical text of a validated record, no trailing newline. Always carries
// "approval":"unapproved".
std::string FormatFixedCompositeConditionV2(const FixedCompositeConditionV2& c);
// Strict reader of the format alone (no file access). Rejects malformed JSON,
// duplicate keys, a different schema, missing or extra members, values of the
// wrong kind, non-whole numbers where a whole number is required, "approved",
// and anything ValidateFixedCompositeConditionV2 refuses. Every failure is a
// std::invalid_argument.
FixedCompositeConditionV2 ParseFixedCompositeConditionV2(std::string_view text);
// The "schema" value of a condition file's text (either version), after the
// same JSON checks. Throws std::invalid_argument if the text is not a JSON
// object with a string "schema". Reads nothing else.
std::string FixedCompositeConditionSchemaOf(std::string_view text);

// A problem with the profile a v2 record names, as opposed to the record itself:
// `missing` when the file is not in the condition's folder, `differs` when it is
// there but is not the profile the record names (SHA-256, fingerprint) or cannot be
// used (not a rig profile 2.0.0, not a calibrated draft, A not left of B, projections
// invalid over the raster). It is a std::invalid_argument, so callers that only check
// for that keep working; the review writer uses the kind to say why nothing was composed.
class FixedCompositeProfileError : public std::invalid_argument {
public:
    enum class Kind {missing,differs};
    FixedCompositeProfileError(Kind kind,const std::string& what):std::invalid_argument(what),kind_(kind){}
    Kind GetKind() const noexcept {return kind_;}
private:
    Kind kind_;
};

// The referenced profile after verification, ready for the renderers.
struct FixedCompositeProfileBinding {
    // The profile's cameras and layout (A on the left, B on the right) with the
    // condition's raster and bilinear sampling. A copy; the profile is never
    // changed (ADR-0034 invariant 6).
    render::DocumentRenderParameters parameters;
    // The profile's profileId, if it has one (reports quote it).
    std::optional<std::string> profile_id;
    std::string profile_file_sha256,profile_fingerprint_sha256;
};
// Verifies the profile bytes against the record and returns the binding.
// Throws std::invalid_argument when the SHA-256 or the fingerprint differs from
// the record, the bytes are not a rig profile 2.0.0, the profile is approved or
// is not a calibrated draft, its camera order is not A left / B right, or its
// projections are not valid over the record's raster. Never changes the profile.
FixedCompositeProfileBinding BindFixedCompositeProfile(const FixedCompositeConditionV2& c,std::string_view profile_bytes,const FixedCompositeSha256& hash);

// What a person supplies when creating a v2 condition. Nothing here has a
// default that could pass for a decision.
struct FixedCompositeV2Request {
    std::string profile_file;
    // The bytes of the profile file as it will sit beside the condition.
    std::string profile_bytes;
    std::optional<std::string> parent_sha256;
    MountOrientation orientation{MountOrientation::normal};
    render::OutputRaster raster{render::DocumentRegionUm{0,0,0,0},0,0,0};
    double scale_auto_correction_max_percent{std::numeric_limits<double>::quiet_NaN()};
    std::uint32_t seam_column{};
    FixedSeamOrigin seam_origin{FixedSeamOrigin::band_center_provisional};
};
// The checks of design 3.9 items 3 to 5 that need the mapped composite. Given
// the candidate record and its verified profile, throws std::invalid_argument
// if, for the scales 1 and 1 +- limit/100, any output sample is covered by
// neither camera or the seam column (with half the blend width) leaves the
// overlap on any row, or a provisional column differs from the band middle.
// MakeFixedCompositeCreationCheck (fixed_composite_creation.hpp, #405 W6) is the check
// the creating entrance passes.
using FixedCompositeCreationCheck=std::function<void(const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&)>;
// Pure: builds the record, verifies the profile bytes (BindFixedCompositeProfile),
// runs the creation check and validates. Reads and writes no file. Throws
// std::invalid_argument for any failure, including an empty hash or check.
// created is a parameter so the time is explicit and testable.
FixedCompositeConditionV2 MakeFixedCompositeConditionV2(const FixedCompositeV2Request& request,std::chrono::system_clock::time_point created,
    const FixedCompositeSha256& hash,const FixedCompositeCreationCheck& check);

struct LoadedFixedCompositeConditionV2 {
    FixedCompositeConditionV2 condition;
    // SHA-256 of the exact condition file bytes, for reports that record which condition was used.
    std::string file_sha256;
    FixedCompositeProfileBinding profile;
};
using LoadedFixedCompositeConditionAny=std::variant<LoadedFixedCompositeCondition,LoadedFixedCompositeConditionV2>;
// Reads the condition file (either version). An absent file means there is no
// condition (std::nullopt). Everything else that is not exact throws, so a
// damaged file is never mistaken for absence: a file that is not a regular file,
// is too large or malformed, an unknown schema, and for v2 a profile that is
// missing from the folder, not a regular file, too large, or that differs from
// the record (see BindFixedCompositeProfile). The v1 branch is
// ParseFixedCompositeCondition on the same bytes, so v1 behaves as before.
std::optional<LoadedFixedCompositeConditionAny> LoadFixedCompositeConditionAny(const std::filesystem::path& file,const FixedCompositeSha256& hash);

// Writes the record as a NEW file, with the steps and guarantees of
// WriteFixedCompositeCondition (exclusive partial, read back, move without
// replacing; an existing file or partial is never overwritten). Copying the
// profile beside it is CopyFixedCompositeProfile's job.
void WriteFixedCompositeConditionV2(const std::filesystem::path& file,const FixedCompositeConditionV2& condition);

// Puts the profile bytes into `folder` under FixedCompositeProfileFileName(hash(bytes))
// and returns that name (design 3.2). A new file, with the same steps as the
// condition writer. A file of that name that already holds exactly these bytes is
// left alone and counts as done; one that holds other bytes is never replaced
// (std::invalid_argument). The profile is not parsed here: BindFixedCompositeProfile
// (through MakeFixedCompositeConditionV2) is what decides whether it is usable.
std::string CopyFixedCompositeProfile(const std::filesystem::path& folder,std::string_view profile_bytes,const FixedCompositeSha256& hash);

// SHA-256, lower-case hex, through the system cryptography provider. For entrances
// that have no provider of their own (the development StillReview does not link the
// camera core, which holds the other one). The reading and creating functions above
// still take the provider as a parameter.
std::string FixedCompositeBytesSha256(const std::vector<unsigned char>& bytes);
}
