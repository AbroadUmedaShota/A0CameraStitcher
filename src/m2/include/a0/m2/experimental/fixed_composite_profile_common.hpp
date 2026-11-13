#pragma once
#include "a0/m2/document_render.hpp"
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a0::m2::experimental {
// What the fixed composite condition records of every version that names a rig profile (v2 and v3) share:
// the profile file name rules, the hash provider, the verified profile and the profile errors. Moved out of
// fixed_composite_condition_v2.hpp so that the v3 record can use them while the variant of both versions
// (LoadedFixedCompositeConditionAny) is still declared in fixed_composite_condition_v2.hpp without an include
// cycle. Declarations only; the definitions stay in fixed_composite_condition_v2.cpp, and nothing changed.

// Same ceiling as the rig profile reader.
inline constexpr std::size_t kMaximumFixedCompositeProfileBytes=256*1024;

// SHA-256, lower-case hex, of the bytes. The caller's provider, as in v1.
using FixedCompositeSha256=std::function<std::string(const std::vector<unsigned char>&)>;

// True for a plain file name that may be stored as the profile reference:
// 6..96 characters of [A-Za-z0-9._-], starting with a letter or digit, ending
// ".json", no "..", not a Windows device name, and not the condition's own name.
// Folder separators, drive letters and parent folders are all outside this set.
bool IsFixedCompositeProfileFileName(std::string_view name);
// "wpd-fixed-composite-profile-<first 16 hex of the file SHA-256>.json". The
// name the creating entrance copies the profile to. Not required of a record:
// the SHA-256 in the record is what identifies the profile.
std::string FixedCompositeProfileFileName(const std::string& profile_file_sha256);

// A problem with the profile a v2 or v3 record names, as opposed to the record itself:
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
}
