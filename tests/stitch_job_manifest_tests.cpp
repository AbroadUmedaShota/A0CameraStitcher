// StitchJob manifest contracts (GitHub Issue #40, decision on Issue #39).
//
// The property under test throughout is one sentence: an output file existing on
// disk is not a successful job. Everything here is an attempt to make that
// sentence false, and to check that it stays true.

#include "a0/m2/stitch_job_manifest.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace a0::m2;

namespace {

int failures = 0;

void Check(const bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Callable>
void CheckRejected(std::string_view expected_code, Callable&& callable, std::string_view message) {
    try {
        callable();
        Check(false, message);
    } catch (const StitchJobManifestError& error) {
        Check(error.Code() == expected_code, std::string(message) + " (code " + error.Code() + ")");
    } catch (...) {
        Check(false, message);
    }
}

fs::path MakeTempRoot() {
    static std::atomic<unsigned long long> sequence{};
    const auto root = fs::temp_directory_path() /
        ("a0-stitch-job-manifest-" + std::to_string(++sequence) + "-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&sequence)));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void WriteBytes(const fs::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

constexpr std::string_view kJobId = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kTransactionId = "fedcba9876543210fedcba9876543210";
constexpr std::string_view kOutputName = "stitched.jpg";
constexpr std::string_view kOutputBytes = "not-a-real-jpeg-but-a-stable-byte-sequence";

// Builds a manifest whose output record actually describes `job_directory/stitched.jpg`,
// so the recovery path has something real to verify against.
StitchJobManifest ManifestFor(const fs::path& job_directory) {
    WriteBytes(job_directory / fs::path(kOutputName), kOutputBytes);
    StitchJobManifest manifest;
    manifest.stitch_job_id = std::string(kJobId);
    manifest.capture_transaction_id = std::string(kTransactionId);
    manifest.inputs[0] = {"CAM-A", std::string(64, 'a'), 1024};
    manifest.inputs[1] = {"CAM-B", std::string(64, 'b'), 2048};
    manifest.rig_profile = {"rig-profile-synthetic", "1.1.0", std::string(64, 'c')};
    manifest.engine = {"a0.m2.offline-stitcher", "1.0.0"};
    manifest.output = {
        std::string(kOutputName),
        ComputeFileSha256Hex(job_directory / fs::path(kOutputName)),
        7360,
        4912,
        kOutputBytes.size(),
    };
    manifest.completed_at_utc = "2026-08-21T00:00:00Z";
    return manifest;
}

void RoundTripPreservesEveryRecordedField() {
    const auto root = MakeTempRoot();
    const auto manifest = ManifestFor(root);
    const auto parsed = ParseStitchJobManifest(SerializeStitchJobManifest(manifest));

    Check(parsed.stitch_job_id == manifest.stitch_job_id, "the StitchJob ID round-trips");
    Check(
        parsed.capture_transaction_id == manifest.capture_transaction_id,
        "the CaptureTransaction ID round-trips");
    Check(
        parsed.inputs[0].camera_alias == "CAM-A" && parsed.inputs[1].camera_alias == "CAM-B",
        "the inputs stay in rig order");
    Check(
        parsed.inputs[0].sha256 == manifest.inputs[0].sha256 &&
            parsed.inputs[1].sha256 == manifest.inputs[1].sha256,
        "both input hashes round-trip");
    Check(
        parsed.rig_profile.profile_id == manifest.rig_profile.profile_id &&
            parsed.rig_profile.version == manifest.rig_profile.version &&
            parsed.rig_profile.sha256 == manifest.rig_profile.sha256,
        "the rig profile record round-trips");
    Check(
        parsed.engine.engine_id == manifest.engine.engine_id &&
            parsed.engine.version == manifest.engine.version,
        "the engine record round-trips");
    Check(
        parsed.output.relative_path == manifest.output.relative_path &&
            parsed.output.sha256 == manifest.output.sha256 &&
            parsed.output.width_pixels == manifest.output.width_pixels &&
            parsed.output.height_pixels == manifest.output.height_pixels &&
            parsed.output.encoded_size_bytes == manifest.output.encoded_size_bytes,
        "the output record round-trips");
    Check(
        parsed.completed_at_utc == manifest.completed_at_utc,
        "the completion time round-trips");

    const auto document = SerializeStitchJobManifest(manifest);
    Check(
        document.find("\"schemaVersion\":\"a0.stitch-job-manifest.v1\"") != std::string::npos,
        "the document names its schema version first");
    Check(
        document.find("\"automaticRetryCount\":0") != std::string::npos,
        "the document records that nothing was retried");
    Check(
        document.find("\"terminalResultState\":\"Succeeded\"") != std::string::npos,
        "the document records the terminal state");
    // Absolute paths would tie the record to one machine and leak its layout.
    Check(
        document.find(root.string()) == std::string::npos &&
            document.find(":\\") == std::string::npos,
        "no absolute path appears anywhere in the manifest");

    fs::remove_all(root);
}

void PublishIsAtomicNonReplacingAndVerified() {
    const auto root = MakeTempRoot();
    const auto manifest = ManifestFor(root);
    PublishAndVerifyStitchJobManifest(root, manifest);

    const auto published = root / fs::path(kStitchJobManifestFileName);
    Check(fs::is_regular_file(published), "the manifest is published beside the output");
    Check(
        !fs::exists(root / fs::path(std::string(kStitchJobManifestFileName) + ".partial")),
        "no partial is left behind by a successful publish");

    // A terminal manifest is immutable. Re-publishing is refused rather than
    // overwriting: rewriting it would turn one job's evidence into another's.
    CheckRejected(
        "ManifestAlreadyPublished",
        [&] { PublishAndVerifyStitchJobManifest(root, manifest); },
        "a published manifest is never replaced");

    const auto recovered = VerifyPublishedStitchJob(root, kJobId);
    Check(recovered.stitch_job_id == manifest.stitch_job_id, "recovery reads the same job back");
    Check(
        recovered.output.sha256 == manifest.output.sha256,
        "recovery verifies the artifact hash the manifest recorded");

    fs::remove_all(root);
}

void AFileWithoutAManifestIsNotSuccess() {
    const auto root = MakeTempRoot();
    // The output is right there, complete, and readable. Under the pre-v1 rule
    // that alone was success.
    WriteBytes(root / fs::path(kOutputName), kOutputBytes);
    Check(
        fs::is_regular_file(root / fs::path(kOutputName)),
        "the output file exists for this test to be meaningful");

    CheckRejected(
        "ManifestMissing",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "an output file with no manifest is refused, not read as a success");

    fs::remove_all(root);
}

void ACrashBetweenPublishAndCommitIsNotSuccess() {
    const auto root = MakeTempRoot();
    const auto manifest = ManifestFor(root);
    // A process that died after writing the manifest partial but before
    // publishing it leaves exactly this.
    WriteBytes(
        root / fs::path(std::string(kStitchJobManifestFileName) + ".partial"),
        SerializeStitchJobManifest(manifest));

    CheckRejected(
        "ManifestMissing",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "a manifest that only ever reached its partial is not a completed job");

    // And the evidence of where it stopped is preserved rather than cleaned up,
    // so a later publish attempt refuses instead of silently overwriting it.
    CheckRejected(
        "ManifestPartialCreateFailed",
        [&] { PublishAndVerifyStitchJobManifest(root, manifest); },
        "an existing partial is never overwritten");

    fs::remove_all(root);
}

void ATruncatedManifestIsNotSuccess() {
    const auto root = MakeTempRoot();
    const auto manifest = ManifestFor(root);
    auto document = SerializeStitchJobManifest(manifest);
    document.resize(document.size() / 2);
    WriteBytes(root / fs::path(kStitchJobManifestFileName), document);

    CheckRejected(
        "MalformedEnvelope",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "a half-written manifest is refused");

    fs::remove_all(root);
}

void AMismatchedHashIsNotSuccess() {
    const auto root = MakeTempRoot();
    const auto manifest = ManifestFor(root);
    PublishAndVerifyStitchJobManifest(root, manifest);

    // Same length, different bytes: the size check alone would not notice.
    std::string tampered(kOutputBytes);
    tampered[0] = static_cast<char>(tampered[0] ^ 0x01);
    WriteBytes(root / fs::path(kOutputName), tampered);
    CheckRejected(
        "ArtifactHashMismatch",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "an output that no longer hashes to the recorded value is refused");

    // And a different length is refused before the hash is even computed.
    WriteBytes(root / fs::path(kOutputName), "short");
    CheckRejected(
        "ArtifactSizeMismatch",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "an output of the wrong size is refused");

    fs::remove(root / fs::path(kOutputName));
    CheckRejected(
        "ArtifactMissing",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "a manifest whose output is gone is refused");

    fs::remove_all(root);
}

void ADifferentJobIsNotThisJob() {
    const auto root = MakeTempRoot();
    PublishAndVerifyStitchJobManifest(root, ManifestFor(root));

    CheckRejected(
        "StitchJobIdMismatch",
        [&] { (void)VerifyPublishedStitchJob(root, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); },
        "a manifest belonging to another StitchJob is refused");

    fs::remove_all(root);
}

void PreV1AndUnknownSchemasAreRefusedRatherThanMigrated() {
    const auto root = MakeTempRoot();

    // A plausible pre-v1 record: it names the output and looks complete. There
    // is no migration path on purpose -- reading it would make an unverified
    // artifact look terminal.
    WriteBytes(
        root / fs::path(kStitchJobManifestFileName),
        R"({"schemaVersion":"a0.stitch-job.v0","output":"stitched.jpg","result":"ok"})");
    CheckRejected(
        "UnsupportedManifestSchema",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "a pre-v1 record is refused rather than migrated");

    fs::remove(root / fs::path(kStitchJobManifestFileName));
    WriteBytes(
        root / fs::path(kStitchJobManifestFileName),
        R"({"schemaVersion":"a0.stitch-job-manifest.v2","stitchJobId":"x"})");
    CheckRejected(
        "UnsupportedManifestSchema",
        [&] { (void)VerifyPublishedStitchJob(root, kJobId); },
        "a future schema version is refused rather than partially read");

    fs::remove_all(root);
}

void EveryFieldIsRequiredAndNoExtraIsTolerated() {
    const auto root = MakeTempRoot();
    const auto document = SerializeStitchJobManifest(ManifestFor(root));

    for (const std::string_view field : {
             "\"stitchJobId\"", "\"captureTransactionId\"", "\"inputs\"", "\"rigProfile\"",
             "\"engine\"", "\"output\"", "\"completedAtUtc\""}) {
        const auto position = document.find(field);
        Check(position != std::string::npos, "the test can locate the field to remove");
        // Removing the key alone leaves malformed JSON, which is also a
        // rejection -- either way the manifest does not read as valid.
        auto broken = document;
        broken.erase(position, field.size());
        bool rejected = false;
        try {
            (void)ParseStitchJobManifest(broken);
        } catch (const StitchJobManifestError&) {
            rejected = true;
        }
        Check(rejected, std::string("a manifest missing ") + std::string(field) + " is refused");
    }

    auto extra = document;
    extra.insert(extra.size() - 1, ",\"operatorNote\":\"anything\"");
    CheckRejected(
        "UnexpectedField",
        [&] { (void)ParseStitchJobManifest(extra); },
        "an unknown field is refused rather than ignored");

    fs::remove_all(root);
}

void ValuesOutsideTheContractAreRefused() {
    const auto root = MakeTempRoot();
    auto manifest = ManifestFor(root);

    manifest.output.relative_path = "..\\outside.jpg";
    CheckRejected(
        "InvalidManifestField",
        [&] { (void)SerializeStitchJobManifest(manifest); },
        "an output path that escapes the job directory is refused");

    manifest = ManifestFor(root);
    manifest.output.relative_path = "C:\\absolute\\stitched.jpg";
    CheckRejected(
        "InvalidManifestField",
        [&] { (void)SerializeStitchJobManifest(manifest); },
        "an absolute output path is refused");

    manifest = ManifestFor(root);
    std::swap(manifest.inputs[0], manifest.inputs[1]);
    CheckRejected(
        "InvalidManifestField",
        [&] { (void)SerializeStitchJobManifest(manifest); },
        "inputs out of rig order are refused, because they record a different pairing");

    manifest = ManifestFor(root);
    manifest.completed_at_utc = "2026-08-21 00:00:00";
    CheckRejected(
        "InvalidManifestField",
        [&] { (void)SerializeStitchJobManifest(manifest); },
        "a completion time that is not a UTC timestamp is refused");

    manifest = ManifestFor(root);
    manifest.stitch_job_id = "not-a-32-character-identifier";
    CheckRejected(
        "InvalidManifestField",
        [&] { (void)SerializeStitchJobManifest(manifest); },
        "a malformed StitchJob ID is refused");

    manifest = ManifestFor(root);
    manifest.output.width_pixels = 0;
    CheckRejected(
        "InvalidManifestField",
        [&] { (void)SerializeStitchJobManifest(manifest); },
        "a zero output dimension is refused");

    fs::remove_all(root);
}

void ARecordedRetryIsRefused() {
    const auto root = MakeTempRoot();
    auto document = SerializeStitchJobManifest(ManifestFor(root));
    const auto position = document.find("\"automaticRetryCount\":0");
    Check(position != std::string::npos, "the test can locate the retry count");
    document.replace(position, std::string("\"automaticRetryCount\":0").size(),
                     "\"automaticRetryCount\":1");

    CheckRejected(
        "AutomaticRetryRecorded",
        [&] { (void)ParseStitchJobManifest(document); },
        "a manifest claiming an automatic retry is not one this product produced");

    fs::remove_all(root);
}

void ANonSucceededStateIsRefused() {
    const auto root = MakeTempRoot();
    auto document = SerializeStitchJobManifest(ManifestFor(root));
    const auto position = document.find("\"terminalResultState\":\"Succeeded\"");
    Check(position != std::string::npos, "the test can locate the terminal state");
    document.replace(
        position,
        std::string("\"terminalResultState\":\"Succeeded\"").size(),
        "\"terminalResultState\":\"FailedPartial\"");

    // A failed job leaves no manifest at all. A manifest that claims a non-success
    // state is a contradiction, so it is refused rather than believed.
    CheckRejected(
        "UnsupportedTerminalState",
        [&] { (void)ParseStitchJobManifest(document); },
        "a manifest recording anything but Succeeded is refused");

    fs::remove_all(root);
}

void RestitchIsANewJobRatherThanAnUpdate() {
    const auto root = MakeTempRoot();
    const auto first = root / "job-1";
    const auto second = root / "job-2";
    fs::create_directories(first);
    fs::create_directories(second);

    auto original = ManifestFor(first);
    PublishAndVerifyStitchJobManifest(first, original);

    // A restitch of the same transaction: same inputs, same profile, new job ID
    // and its own directory and manifest. Nothing about the first job changes.
    auto restitched = ManifestFor(second);
    restitched.stitch_job_id = "11111111111111111111111111111111";
    restitched.capture_transaction_id = original.capture_transaction_id;
    restitched.inputs = original.inputs;
    restitched.rig_profile = original.rig_profile;
    PublishAndVerifyStitchJobManifest(second, restitched);

    const auto reread_first = VerifyPublishedStitchJob(first, kJobId);
    Check(
        reread_first.stitch_job_id == original.stitch_job_id,
        "the first job's manifest is untouched by the restitch");
    const auto reread_second =
        VerifyPublishedStitchJob(second, "11111111111111111111111111111111");
    Check(
        reread_second.capture_transaction_id == original.capture_transaction_id,
        "the restitch records the same CaptureTransaction");
    Check(
        reread_second.stitch_job_id != reread_first.stitch_job_id,
        "a restitch is a different StitchJob, not an update to the first");
    Check(
        ReadText(first / fs::path(kStitchJobManifestFileName)) !=
            ReadText(second / fs::path(kStitchJobManifestFileName)),
        "the two jobs have distinguishable manifests");

    fs::remove_all(root);
}

} // namespace

int main() {
    RoundTripPreservesEveryRecordedField();
    PublishIsAtomicNonReplacingAndVerified();
    AFileWithoutAManifestIsNotSuccess();
    ACrashBetweenPublishAndCommitIsNotSuccess();
    ATruncatedManifestIsNotSuccess();
    AMismatchedHashIsNotSuccess();
    ADifferentJobIsNotThisJob();
    PreV1AndUnknownSchemasAreRefusedRatherThanMigrated();
    EveryFieldIsRequiredAndNoExtraIsTolerated();
    ValuesOutsideTheContractAreRefused();
    ARecordedRetryIsRefused();
    ANonSucceededStateIsRefused();
    RestitchIsANewJobRatherThanAnUpdate();

    if (failures != 0) {
        std::cerr << failures << " StitchJob manifest contract failures\n";
        return 1;
    }
    std::cout << "StitchJob manifest contracts passed\n";
    return 0;
}
