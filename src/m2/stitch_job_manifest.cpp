#include "a0/m2/stitch_job_manifest.hpp"

#include "a0/common/protocol_json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace a0::m2 {
namespace {

using ::a0::common::protocol_json::JsonKind;
using ::a0::common::protocol_json::JsonValue;

[[noreturn]] void ManifestFailure(std::string code, std::string message) {
    throw StitchJobManifestError(std::move(code), std::move(message));
}

struct StitchJobManifestJsonFailure {
    [[noreturn]] static void Fail(std::string code, std::string message) {
        ManifestFailure(std::move(code), std::move(message));
    }
};

using ManifestParser =
    ::a0::common::protocol_json::BasicJsonParser<StitchJobManifestJsonFailure>;

[[nodiscard]] const JsonValue& RequireField(
    const JsonValue& object, std::string_view name, JsonKind kind) {
    return ::a0::common::protocol_json::RequireFieldWith<StitchJobManifestJsonFailure>(
        object, name, kind);
}

void RequireExactFields(const JsonValue& object, const std::set<std::string>& expected) {
    if (object.kind != JsonKind::object || object.object.size() != expected.size()) {
        ManifestFailure(
            "UnexpectedField", "the StitchJob manifest has missing or unexpected fields");
    }
    for (const auto& [name, ignored] : object.object) {
        (void)ignored;
        if (!expected.contains(name)) {
            ManifestFailure(
                "UnexpectedField", "the StitchJob manifest contains an unexpected field");
        }
    }
}

bool IsLowercaseHex(std::string_view value, const std::size_t expected_length) noexcept {
    return value.size() == expected_length &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

// The relative path is the one field a reader turns back into a filesystem
// operation, so it is the one field that has to be boring: a single path
// component of safe characters. No separators, no traversal, no drive letters,
// no device names -- a manifest can be carried between machines and must not be
// able to point anywhere except beside itself.
bool IsSafeRelativeOutputPath(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128 || value == "." || value == "..") return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '-' || character == '_';
    });
}

bool IsUtcTimestamp(std::string_view value) noexcept {
    // yyyy-mm-ddThh:mm:ssZ, fixed width. A tolerant reader here would accept a
    // local time and record it as UTC, which is unrecoverable afterwards.
    if (value.size() != 20) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        switch (index) {
            case 4: case 7: if (character != '-') return false; break;
            case 10: if (character != 'T') return false; break;
            case 13: case 16: if (character != ':') return false; break;
            case 19: if (character != 'Z') return false; break;
            default: if (character < '0' || character > '9') return false; break;
        }
    }
    return true;
}

bool IsBoundedText(std::string_view value, const std::size_t maximum) noexcept {
    return !value.empty() && value.size() <= maximum;
}

std::uint64_t ParseUnsigned(const JsonValue& value, std::string_view field) {
    if (value.kind != JsonKind::number || value.string.empty() ||
        value.string.find_first_of(".eE-+") != std::string::npos) {
        ManifestFailure(
            "InvalidManifestField",
            "StitchJob manifest field '" + std::string(field) + "' is not a whole number");
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(
        value.string.data(), value.string.data() + value.string.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.string.data() + value.string.size()) {
        ManifestFailure(
            "InvalidManifestField",
            "StitchJob manifest field '" + std::string(field) + "' is outside the supported range");
    }
    return parsed;
}

const std::string& RequireStringField(
    const JsonValue& object, std::string_view field, const std::size_t maximum) {
    const auto& value = RequireField(object, field, JsonKind::string).string;
    if (!IsBoundedText(value, maximum)) {
        ManifestFailure(
            "InvalidManifestField",
            "StitchJob manifest field '" + std::string(field) + "' is empty or unbounded");
    }
    return value;
}

StitchJobSha256Hex RequireSha256Field(const JsonValue& object, std::string_view field) {
    const auto& value = RequireField(object, field, JsonKind::string).string;
    if (!IsLowercaseHex(value, 64)) {
        ManifestFailure(
            "InvalidManifestField",
            "StitchJob manifest field '" + std::string(field) + "' is not a lowercase SHA-256");
    }
    return value;
}

std::string RequireIdField(const JsonValue& object, std::string_view field) {
    const auto& value = RequireField(object, field, JsonKind::string).string;
    if (!IsLowercaseHex(value, 32)) {
        ManifestFailure(
            "InvalidManifestField",
            "StitchJob manifest field '" + std::string(field) + "' is not a 32 character identifier");
    }
    return value;
}

class ManifestFileHandle final {
public:
    ManifestFileHandle(const std::filesystem::path& path, const bool for_write) {
        handle_ = for_write
            ? CreateFileW(
                  path.c_str(),
                  GENERIC_WRITE,
                  0,
                  nullptr,
                  // Never CREATE_ALWAYS: an existing partial is evidence of an
                  // earlier attempt, and silently overwriting it destroys the
                  // only record of where that attempt stopped.
                  CREATE_NEW,
                  FILE_ATTRIBUTE_NORMAL,
                  nullptr)
            : CreateFileW(
                  path.c_str(),
                  GENERIC_READ | DELETE,
                  FILE_SHARE_READ | FILE_SHARE_DELETE,
                  nullptr,
                  OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                  nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            ManifestFailure(
                for_write ? "ManifestPartialCreateFailed" : "ManifestOpenFailed",
                "the StitchJob manifest file could not be opened (Win32 " +
                    std::to_string(GetLastError()) + ")");
        }
    }

    ~ManifestFileHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    ManifestFileHandle(const ManifestFileHandle&) = delete;
    ManifestFileHandle& operator=(const ManifestFileHandle&) = delete;

    void WriteAllAndFlush(std::string_view bytes) const {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto request = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - offset, 1024U * 1024U));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data() + offset, request, &written, nullptr) ||
                written == 0) {
                ManifestFailure("ManifestWriteFailed", "the StitchJob manifest write failed");
            }
            offset += written;
        }
        if (!FlushFileBuffers(handle_)) {
            ManifestFailure("ManifestFlushFailed", "the StitchJob manifest flush failed");
        }
    }

    [[nodiscard]] std::string ReadAll() const {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size) || size.QuadPart <= 0 ||
            size.QuadPart > static_cast<LONGLONG>(64 * 1024)) {
            ManifestFailure(
                "ManifestSizeInvalid", "the StitchJob manifest size is empty or implausible");
        }
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD read = 0;
            if (!ReadFile(
                    handle_,
                    bytes.data() + offset,
                    static_cast<DWORD>(bytes.size() - offset),
                    &read,
                    nullptr) ||
                read == 0) {
                ManifestFailure("ManifestReadFailed", "the StitchJob manifest read failed");
            }
            offset += read;
        }
        return bytes;
    }

    void RenameToWithoutReplace(const std::filesystem::path& destination) const {
        const auto absolute = std::filesystem::absolute(destination).lexically_normal();
        const auto name = absolute.native();
        const auto name_bytes = name.size() * sizeof(wchar_t);
        if (name_bytes > (std::numeric_limits<DWORD>::max)()) {
            ManifestFailure("ManifestPathTooLong", "the StitchJob manifest destination is too long");
        }
        std::vector<std::uint8_t> buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        // Never replace. A manifest that already exists is a terminal record and
        // rewriting it would turn one job's evidence into another's.
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(rename->FileName, name.c_str(), name_bytes + sizeof(wchar_t));
        if (SetFileInformationByHandle(
                handle_, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())) == FALSE) {
            // ReplaceIfExists = FALSE is the only thing standing between a
            // re-publish and one job's evidence overwriting another's, so the
            // reason is derived from its failure rather than from a separate
            // exists() check taken earlier. A check taken earlier would also be
            // a check taken before the rename, which is a window a concurrent
            // publisher fits through.
            const DWORD error = GetLastError();
            ManifestFailure(
                error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
                    ? "ManifestAlreadyPublished"
                    : "ManifestPublishFailed",
                error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
                    ? "a terminal StitchJob manifest already exists and is never replaced"
                    : "the StitchJob manifest could not be published (Win32 " +
                          std::to_string(error) + ")");
        }
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

void ValidateManifestValues(const StitchJobManifest& manifest) {
    if (!IsLowercaseHex(manifest.stitch_job_id, 32)) {
        ManifestFailure("InvalidManifestField", "the StitchJob ID is not a 32 character identifier");
    }
    if (!IsLowercaseHex(manifest.capture_transaction_id, 32)) {
        ManifestFailure(
            "InvalidManifestField", "the CaptureTransaction ID is not a 32 character identifier");
    }
    if (manifest.inputs[0].camera_alias != "CAM-A" ||
        manifest.inputs[1].camera_alias != "CAM-B") {
        // Ordered, not just present. A manifest whose inputs are in the other
        // order records a different pairing than the one that was stitched.
        ManifestFailure(
            "InvalidManifestField", "the StitchJob inputs must be CAM-A then CAM-B, in that order");
    }
    for (const auto& input : manifest.inputs) {
        if (!IsLowercaseHex(input.sha256, 64) || input.encoded_size_bytes == 0) {
            ManifestFailure("InvalidManifestField", "a StitchJob input record is incomplete");
        }
    }
    if (!IsBoundedText(manifest.rig_profile.profile_id, 128) ||
        !IsBoundedText(manifest.rig_profile.version, 32) ||
        !IsLowercaseHex(manifest.rig_profile.sha256, 64)) {
        ManifestFailure("InvalidManifestField", "the StitchJob rig profile record is incomplete");
    }
    if (!IsBoundedText(manifest.engine.engine_id, 128) ||
        !IsBoundedText(manifest.engine.version, 32)) {
        ManifestFailure("InvalidManifestField", "the StitchJob engine record is incomplete");
    }
    if (!IsSafeRelativeOutputPath(manifest.output.relative_path) ||
        !IsLowercaseHex(manifest.output.sha256, 64) ||
        manifest.output.width_pixels == 0 || manifest.output.height_pixels == 0 ||
        manifest.output.encoded_size_bytes == 0) {
        ManifestFailure("InvalidManifestField", "the StitchJob output record is incomplete");
    }
    if (!IsUtcTimestamp(manifest.completed_at_utc)) {
        ManifestFailure(
            "InvalidManifestField", "the StitchJob completion time is not a UTC timestamp");
    }
}

std::string Escape(std::string_view value) {
    return ::a0::common::protocol_json::JsonEscape(value);
}

} // namespace

StitchJobManifestError::StitchJobManifestError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& StitchJobManifestError::Code() const noexcept { return code_; }

std::string SerializeStitchJobManifest(const StitchJobManifest& manifest) {
    ValidateManifestValues(manifest);

    std::ostringstream output;
    output << "{\"schemaVersion\":\"" << kStitchJobManifestSchemaVersion
           << "\",\"stitchJobId\":\"" << Escape(manifest.stitch_job_id)
           << "\",\"captureTransactionId\":\"" << Escape(manifest.capture_transaction_id)
           << "\",\"inputs\":[";
    for (std::size_t index = 0; index < manifest.inputs.size(); ++index) {
        if (index > 0) output << ',';
        const auto& input = manifest.inputs[index];
        output << "{\"cameraAlias\":\"" << Escape(input.camera_alias)
               << "\",\"sha256\":\"" << Escape(input.sha256)
               << "\",\"encodedSizeBytes\":" << input.encoded_size_bytes << '}';
    }
    output << "],\"rigProfile\":{\"profileId\":\"" << Escape(manifest.rig_profile.profile_id)
           << "\",\"version\":\"" << Escape(manifest.rig_profile.version)
           << "\",\"sha256\":\"" << Escape(manifest.rig_profile.sha256)
           << "\"},\"engine\":{\"engineId\":\"" << Escape(manifest.engine.engine_id)
           << "\",\"version\":\"" << Escape(manifest.engine.version)
           << "\"},\"output\":{\"relativePath\":\"" << Escape(manifest.output.relative_path)
           << "\",\"sha256\":\"" << Escape(manifest.output.sha256)
           << "\",\"widthPixels\":" << manifest.output.width_pixels
           << ",\"heightPixels\":" << manifest.output.height_pixels
           << ",\"encodedSizeBytes\":" << manifest.output.encoded_size_bytes
           << "},\"terminalResultState\":\"Succeeded\",\"completedAtUtc\":\""
           << Escape(manifest.completed_at_utc) << "\",\"automaticRetryCount\":0}";
    return output.str();
}

StitchJobManifest ParseStitchJobManifest(std::string_view json) {
    const JsonValue root = ManifestParser(json).Parse();

    // The schema version is checked before the field set, and the order matters
    // for the reason reported rather than for whether it is refused. A future
    // v2 manifest will legitimately carry a different set of fields; telling its
    // reader "unexpected field" would send them looking for a typo instead of
    // for the version mismatch that is actually in front of them.
    //
    // Not a best-effort read either way. A manifest from a version this build
    // does not know is exactly the case where guessing turns a failed job into a
    // successful-looking one, and pre-v1 artifacts have no manifest at all.
    if (RequireField(root, "schemaVersion", JsonKind::string).string !=
        kStitchJobManifestSchemaVersion) {
        ManifestFailure(
            "UnsupportedManifestSchema",
            "the StitchJob manifest schema version is not a0.stitch-job-manifest.v1");
    }

    RequireExactFields(root, {
        "schemaVersion", "stitchJobId", "captureTransactionId", "inputs", "rigProfile",
        "engine", "output", "terminalResultState", "completedAtUtc", "automaticRetryCount",
    });
    if (RequireField(root, "terminalResultState", JsonKind::string).string != "Succeeded") {
        ManifestFailure(
            "UnsupportedTerminalState",
            "only a succeeded StitchJob has a manifest; any other state is a rejection");
    }
    if (ParseUnsigned(
            RequireField(root, "automaticRetryCount", JsonKind::number), "automaticRetryCount") != 0) {
        ManifestFailure(
            "AutomaticRetryRecorded",
            "the product performs no automatic retry, so a non-zero count is not a manifest "
            "this build produced");
    }

    StitchJobManifest manifest;
    manifest.stitch_job_id = RequireIdField(root, "stitchJobId");
    manifest.capture_transaction_id = RequireIdField(root, "captureTransactionId");

    const auto& inputs = RequireField(root, "inputs", JsonKind::array);
    if (inputs.array.size() != manifest.inputs.size()) {
        ManifestFailure(
            "InvalidManifestField", "a StitchJob manifest records exactly two inputs");
    }
    for (std::size_t index = 0; index < inputs.array.size(); ++index) {
        const auto& entry = inputs.array[index];
        RequireExactFields(entry, {"cameraAlias", "sha256", "encodedSizeBytes"});
        manifest.inputs[index].camera_alias = RequireStringField(entry, "cameraAlias", 32);
        manifest.inputs[index].sha256 = RequireSha256Field(entry, "sha256");
        manifest.inputs[index].encoded_size_bytes = ParseUnsigned(
            RequireField(entry, "encodedSizeBytes", JsonKind::number), "encodedSizeBytes");
    }

    const auto& profile = RequireField(root, "rigProfile", JsonKind::object);
    RequireExactFields(profile, {"profileId", "version", "sha256"});
    manifest.rig_profile.profile_id = RequireStringField(profile, "profileId", 128);
    manifest.rig_profile.version = RequireStringField(profile, "version", 32);
    manifest.rig_profile.sha256 = RequireSha256Field(profile, "sha256");

    const auto& engine = RequireField(root, "engine", JsonKind::object);
    RequireExactFields(engine, {"engineId", "version"});
    manifest.engine.engine_id = RequireStringField(engine, "engineId", 128);
    manifest.engine.version = RequireStringField(engine, "version", 32);

    const auto& artifact = RequireField(root, "output", JsonKind::object);
    RequireExactFields(
        artifact, {"relativePath", "sha256", "widthPixels", "heightPixels", "encodedSizeBytes"});
    manifest.output.relative_path = RequireStringField(artifact, "relativePath", 128);
    manifest.output.sha256 = RequireSha256Field(artifact, "sha256");
    const auto width = ParseUnsigned(
        RequireField(artifact, "widthPixels", JsonKind::number), "widthPixels");
    const auto height = ParseUnsigned(
        RequireField(artifact, "heightPixels", JsonKind::number), "heightPixels");
    if (width == 0 || width > (std::numeric_limits<std::uint32_t>::max)() ||
        height == 0 || height > (std::numeric_limits<std::uint32_t>::max)()) {
        ManifestFailure("InvalidManifestField", "the StitchJob output dimensions are invalid");
    }
    manifest.output.width_pixels = static_cast<std::uint32_t>(width);
    manifest.output.height_pixels = static_cast<std::uint32_t>(height);
    manifest.output.encoded_size_bytes = ParseUnsigned(
        RequireField(artifact, "encodedSizeBytes", JsonKind::number), "encodedSizeBytes");

    manifest.completed_at_utc = RequireStringField(root, "completedAtUtc", 32);

    ValidateManifestValues(manifest);
    return manifest;
}

StitchJobSha256Hex ComputeFileSha256Hex(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        ManifestFailure("HashProviderUnavailable", "SHA-256 is unavailable");
    }
    DWORD object_size = 0;
    DWORD hash_size = 0;
    ULONG written = 0;
    if (BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size), &written, 0) < 0 ||
        BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
            sizeof(hash_size), &written, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        ManifestFailure("HashProviderUnavailable", "SHA-256 properties are unavailable");
    }
    std::vector<std::uint8_t> object(object_size);
    std::vector<std::uint8_t> digest(hash_size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        ManifestFailure("HashProviderUnavailable", "SHA-256 could not be started");
    }

    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        ManifestFailure("ArtifactMissing", "the recorded StitchJob artifact could not be opened");
    }

    std::vector<std::uint8_t> chunk(1024U * 1024U);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
            CloseHandle(file);
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            ManifestFailure("ArtifactReadFailed", "the recorded StitchJob artifact could not be read");
        }
        if (read == 0) break;
        if (BCryptHashData(hash, chunk.data(), read, 0) < 0) {
            CloseHandle(file);
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            ManifestFailure("HashProviderUnavailable", "SHA-256 hashing failed");
        }
    }
    CloseHandle(file);

    const bool finished = BCryptFinishHash(hash, digest.data(), hash_size, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!finished) {
        ManifestFailure("HashProviderUnavailable", "SHA-256 could not be finished");
    }

    static constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        hex.push_back(digits[(byte >> 4U) & 0x0FU]);
        hex.push_back(digits[byte & 0x0FU]);
    }
    return hex;
}

void PublishAndVerifyStitchJobManifest(
    const std::filesystem::path& job_directory,
    const StitchJobManifest& expected) {
    const auto document = SerializeStitchJobManifest(expected);
    const auto destination = job_directory / std::filesystem::path(kStitchJobManifestFileName);
    const auto partial =
        job_directory / std::filesystem::path(std::string(kStitchJobManifestFileName) + ".partial");
    {
        const ManifestFileHandle writer(partial, true);
        writer.WriteAllAndFlush(document);
    }
    {
        const ManifestFileHandle publisher(partial, false);
        publisher.RenameToWithoutReplace(destination);
    }

    // Re-read and compare. Until this succeeds the caller has not committed: a
    // manifest that cannot be read back is indistinguishable, to the next
    // process, from one that was never written.
    const ManifestFileHandle reader(destination, false);
    const auto reread = ParseStitchJobManifest(reader.ReadAll());
    // Parsing is what does the work here: it re-validates every field against the
    // schema, so a truncated, corrupt or unreadable manifest fails above. What is
    // left to check is identity -- that the document now at this path is still
    // the one just published, and not one a concurrent writer replaced it with.
    // Re-comparing the remaining fields would only restate what parsing already
    // proved about a string this function serialized moments ago.
    if (reread.stitch_job_id != expected.stitch_job_id ||
        reread.output.sha256 != expected.output.sha256) {
        ManifestFailure(
            "ManifestRereadMismatch",
            "the published StitchJob manifest does not match what was committed");
    }
}

StitchJobManifest VerifyPublishedStitchJob(
    const std::filesystem::path& job_directory,
    std::string_view expected_stitch_job_id) {
    const auto manifest_path =
        job_directory / std::filesystem::path(kStitchJobManifestFileName);
    if (!std::filesystem::is_regular_file(manifest_path)) {
        // The output may well be sitting right there. That is the whole point:
        // a file on disk with no manifest is not a successful job, and this is
        // the boundary that refuses to read it as one.
        ManifestFailure(
            "ManifestMissing",
            "the StitchJob has no manifest, so it is not terminal-success regardless of what "
            "files exist beside it");
    }

    const ManifestFileHandle reader(manifest_path, false);
    const auto manifest = ParseStitchJobManifest(reader.ReadAll());
    if (manifest.stitch_job_id != expected_stitch_job_id) {
        ManifestFailure(
            "StitchJobIdMismatch",
            "the manifest in this job directory belongs to a different StitchJob");
    }

    const auto artifact = job_directory / std::filesystem::path(manifest.output.relative_path);
    if (!std::filesystem::is_regular_file(artifact)) {
        ManifestFailure("ArtifactMissing", "the StitchJob output recorded by the manifest is gone");
    }
    std::error_code size_error;
    const auto size = std::filesystem::file_size(artifact, size_error);
    if (size_error || size != manifest.output.encoded_size_bytes) {
        ManifestFailure(
            "ArtifactSizeMismatch",
            "the StitchJob output is not the size the manifest recorded");
    }
    if (ComputeFileSha256Hex(artifact) != manifest.output.sha256) {
        ManifestFailure(
            "ArtifactHashMismatch",
            "the StitchJob output does not hash to the value the manifest recorded");
    }
    return manifest;
}

} // namespace a0::m2
