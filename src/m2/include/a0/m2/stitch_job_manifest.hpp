#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::m2 {

// The single durable commit point for a StitchJob (GitHub Issue #39 decision,
// implemented by Issue #40).
//
// Before this existed, a stitched output on disk was the whole answer: the file
// is there, so the job succeeded. That is wrong in every way that matters. The
// file can be there because the process died after the rename and before it
// recorded what the file was. It can be there from an earlier attempt with a
// different profile. It can be there and be a different image than the one whose
// dimensions were verified. None of those are distinguishable from success by
// looking at the file.
//
// So terminal success is a separate, versioned record that is published
// atomically and then read back and checked. Ordering matters and is fixed:
//
//   1. generate the output candidate, flush it, decode it fully, check its
//      dimensions, size ceiling and SHA-256
//   2. publish the output non-replacingly
//   3. write the manifest to its own .partial and flush it
//   4. publish the manifest non-replacingly
//   5. re-read the manifest and check schema, IDs, every hash, the dimensions
//      and the result state
//   6. only now is the job terminal-success
//
// A crash anywhere before 5 leaves a job that is not successful, and leaves the
// evidence of where it stopped. Nothing is cleaned up automatically, nothing is
// retried, and no existing artifact is ever replaced.
//
// Deliberately absent from the record: absolute paths, serials, real
// identifiers, previews and image data. The output is named relative to the job
// directory, so a manifest can be carried, read and verified without saying
// anything about the machine that produced it.

inline constexpr std::string_view kStitchJobManifestSchemaVersion =
    "a0.stitch-job-manifest.v1";
inline constexpr std::string_view kStitchJobManifestFileName = "stitch-job.manifest.json";

// A hex SHA-256, lowercase, as it appears in the manifest.
using StitchJobSha256Hex = std::string;

class StitchJobManifestError final : public std::runtime_error {
public:
    StitchJobManifestError(std::string code, std::string message);

    [[nodiscard]] const std::string& Code() const noexcept;

private:
    std::string code_;
};

struct StitchJobInputRecord {
    // "CAM-A" or "CAM-B". Kept as an ordered pair rather than a map so a reader
    // cannot treat the two bodies as interchangeable.
    std::string camera_alias;
    StitchJobSha256Hex sha256;
    std::uint64_t encoded_size_bytes{};
};

struct StitchJobProfileRecord {
    std::string profile_id;
    std::string version;
    StitchJobSha256Hex sha256;
};

struct StitchJobEngineRecord {
    std::string engine_id;
    std::string version;
};

struct StitchJobOutputRecord {
    // Relative to the job directory, never absolute.
    std::string relative_path;
    StitchJobSha256Hex sha256;
    std::uint32_t width_pixels{};
    std::uint32_t height_pixels{};
    std::uint64_t encoded_size_bytes{};
};

// Everything a terminal StitchJob records. There is no "failed" manifest: a job
// that did not succeed leaves none, which is what makes "manifest present and
// verified" mean exactly "the job succeeded".
struct StitchJobManifest {
    std::string stitch_job_id;
    std::string capture_transaction_id;
    std::array<StitchJobInputRecord, 2> inputs;
    StitchJobProfileRecord rig_profile;
    StitchJobEngineRecord engine;
    StitchJobOutputRecord output;
    std::string completed_at_utc;
};

// Serializes the manifest, rejecting anything the schema would reject.
//
// Validation happens here rather than at the call site because a manifest that
// is written and only then found to be malformed has already been published:
// the whole point of the commit point is that it is either right or absent.
[[nodiscard]] std::string SerializeStitchJobManifest(const StitchJobManifest& manifest);

// Parses and validates a manifest document.
//
// Every field is required and no unknown field is tolerated. An unrecognised
// schema version is a rejection rather than a best-effort read: a manifest from
// a version this build does not know is exactly the case where guessing turns a
// failed job into a successful-looking one.
[[nodiscard]] StitchJobManifest ParseStitchJobManifest(std::string_view json);

// Writes the manifest to `job_directory` through a `.partial`, publishes it
// without replacing anything, then re-reads it and checks it matches
// `expected` field for field.
//
// The re-read is not paranoia about the filesystem lying. It is what closes the
// window in which the manifest is written but not durable: if the re-read fails
// for any reason, the caller has not committed and must not report success.
void PublishAndVerifyStitchJobManifest(
    const std::filesystem::path& job_directory,
    const StitchJobManifest& expected);

// Reads back an already published manifest and verifies it against the artifacts
// on disk: the output file must exist, hash to the recorded SHA-256, and be the
// recorded size.
//
// This is the same-ID read-only recovery path. It re-runs no stitch, writes
// nothing, deletes nothing and replaces nothing -- a restart that finds an
// ambiguous job answers "was this job successful?" and nothing else.
[[nodiscard]] StitchJobManifest VerifyPublishedStitchJob(
    const std::filesystem::path& job_directory,
    std::string_view expected_stitch_job_id);

// SHA-256 of a file, lowercase hex. Exposed because the manifest records hashes
// the caller computed for other reasons, and both sides must agree on spelling.
[[nodiscard]] StitchJobSha256Hex ComputeFileSha256Hex(const std::filesystem::path& path);

} // namespace a0::m2
