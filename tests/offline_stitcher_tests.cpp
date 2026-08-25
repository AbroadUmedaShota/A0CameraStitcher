#include "a0/m2/offline_stitcher.hpp"
#include "a0/m2/stitch_job_manifest.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace a0::m2::detail {
enum class OfflineStitchFaultPoint : std::uint32_t {
    none = 0,
    encode_failure = 1,
    partial_short_write = 2,
    partial_flush_failure = 3,
    disk_full = 4,
    publish_failure = 5,
    interrupt_before_encode = 6,
    interrupt_after_partial_write = 7,
    interrupt_after_flush = 8,
    interrupt_before_publish = 9,
    interrupt_after_publish = 10,
};

// GitHub Issue #102 (item 3, follow-up from item 2): pairs the SHA-256
// PublishValidatedGeneratedJpeg already computed with the exact byte count it
// was computed over. Mirrors the real a0::m2::detail::PublishedGeneratedJpeg
// definition in offline_stitcher.cpp field-for-field -- the two definitions
// must stay in sync for this declaration to link correctly.
struct PublishedGeneratedJpeg {
    a0::m2::StitchJobSha256Hex sha256;
    std::uint64_t encoded_size_bytes{};
};

// GitHub Issue #102 (item 2): now returns the SHA-256 hex digest (and, since
// item 3, the byte count it was computed over) instead of void.
PublishedGeneratedJpeg PublishValidatedGeneratedJpeg(
    const std::filesystem::path& partial,
    const std::filesystem::path& destination,
    std::uint32_t expected_width,
    std::uint32_t expected_height);
void SetInputLocksHeldTestHook(void (*hook)()) noexcept;
void SetBeforeEncodeTestHook(void (*hook)()) noexcept;
void SetBeforePartialFlushTestHook(void (*hook)()) noexcept;
void SetBeforePublishRenameTestHook(void (*hook)()) noexcept;
void SetOfflineStitchFaultForTest(OfflineStitchFaultPoint point) noexcept;
std::uint32_t GetOfflineStitchFaultTriggerCountForTest() noexcept;
}

namespace {

int failures = 0;
HANDLE input_locks_held_event = nullptr;
HANDLE release_input_locks_event = nullptr;
std::filesystem::path publish_race_partial;
std::filesystem::path publish_race_renamed;
std::vector<std::uint8_t> publish_race_replacement;
HANDLE actual_io_blocking_handle = INVALID_HANDLE_VALUE;
std::filesystem::path actual_io_partial;
std::filesystem::path actual_io_destination;
std::vector<std::uint8_t> actual_io_destination_sentinel;

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);

void SignalInputLocksHeldAndWait() {
    if (!SetEvent(input_locks_held_event)
        || WaitForSingleObject(release_input_locks_event, 5'000) != WAIT_OBJECT_0) {
        throw std::runtime_error("input lock synchronization failed");
    }
}

void RenameVerifiedPartialAwayAndReplacePath() {
    if (!MoveFileExW(publish_race_partial.c_str(), publish_race_renamed.c_str(), 0)) {
        throw std::runtime_error("publish race could not rename verified partial away");
    }
    WriteBytes(publish_race_partial, publish_race_replacement);
}

// Fills the StitchJob identity every stitch now has to record. The values are
// unique per call so no two jobs in one test run claim to be the same job, which
// is also what a restitch does for real.
a0::m2::OfflineStitchRequest WithRecordedIdentity(a0::m2::OfflineStitchRequest request) {
    static std::atomic<unsigned int> sequence{};
    const auto ordinal = ++sequence;
    char job_id[33]{};
    char transaction_id[33]{};
    std::snprintf(job_id, sizeof(job_id), "%032x", ordinal);
    std::snprintf(transaction_id, sizeof(transaction_id), "%032x", 0xC0FFEEU + ordinal);
    request.stitch_job_id = job_id;
    request.capture_transaction_id = transaction_id;
    request.completed_at_utc = "2026-08-21T00:00:00Z";
    return request;
}

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Callable>
void CheckRejected(Callable&& callable, const std::string& message) {
    try {
        callable();
        Check(false, message);
    } catch (const std::exception&) {
    }
}

template <typename Callable>
void CheckRejectedContains(Callable&& callable, const std::string& expected, const std::string& message) {
    try {
        callable();
        Check(false, message);
    } catch (const std::exception& error) {
        Check(std::string(error.what()).find(expected) != std::string::npos,
            message + " (unexpected rejection: " + error.what() + ")");
    }
}

void CheckHr(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

void WriteSolidJpeg(
    const std::filesystem::path& path,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint8_t red,
    const std::uint8_t green,
    const std::uint8_t blue) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    try {
        CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "factory");
        CheckHr(factory->CreateStream(&stream), "stream");
        CheckHr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "open");
        CheckHr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder), "encoder");
        CheckHr(encoder->Initialize(stream, WICBitmapEncoderNoCache), "encoder init");
        CheckHr(encoder->CreateNewFrame(&frame, &properties), "frame");
        CheckHr(frame->Initialize(properties), "frame init");
        CheckHr(frame->SetSize(width, height), "size");
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        CheckHr(frame->SetPixelFormat(&format), "format");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
        for (std::size_t index = 0; index < pixels.size(); index += 3) {
            pixels[index] = blue;
            pixels[index + 1] = green;
            pixels[index + 2] = red;
        }
        CheckHr(frame->WritePixels(height, width * 3, static_cast<UINT>(pixels.size()), pixels.data()), "pixels");
        CheckHr(frame->Commit(), "frame commit");
        CheckHr(encoder->Commit(), "encoder commit");
    } catch (...) {
        if (properties) properties->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        throw;
    }
    properties->Release();
    frame->Release();
    encoder->Release();
    stream->Release();
    factory->Release();
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("test byte output open failed");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("test byte output write failed");
    }
}

void WriteOversizedMetadataJpeg(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& valid_jpeg) {
    if (valid_jpeg.size() < 4 || valid_jpeg[0] != 0xff || valid_jpeg[1] != 0xd8) {
        throw std::runtime_error("metadata test requires a valid JPEG prefix");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("metadata JPEG output open failed");
    }
    output.put(static_cast<char>(0xff));
    output.put(static_cast<char>(0xd8));
    std::vector<char> app1_segment(65'537, 0);
    app1_segment[0] = static_cast<char>(0xff);
    app1_segment[1] = static_cast<char>(0xe1);
    app1_segment[2] = static_cast<char>(0xff);
    app1_segment[3] = static_cast<char>(0xff);
    while (static_cast<std::uint64_t>(output.tellp())
        + app1_segment.size() + valid_jpeg.size() - 2 <= a0::m2::kMaximumCompressedJpegBytes) {
        output.write(app1_segment.data(), static_cast<std::streamsize>(app1_segment.size()));
    }
    output.write(app1_segment.data(), static_cast<std::streamsize>(app1_segment.size()));
    output.write(
        reinterpret_cast<const char*>(valid_jpeg.data() + 2),
        static_cast<std::streamsize>(valid_jpeg.size() - 2));
    if (!output) {
        throw std::runtime_error("metadata JPEG output write failed");
    }
}

struct DecodedJpeg {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgr;
};

DecodedJpeg DecodeJpeg(const std::filesystem::path& path) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    try {
        CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "decode factory");
        CheckHr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder), "decode open");
        CheckHr(decoder->GetFrame(0, &frame), "decode frame");
        UINT width = 0;
        UINT height = 0;
        CheckHr(frame->GetSize(&width, &height), "decode size");
        CheckHr(factory->CreateFormatConverter(&converter), "decode converter");
        CheckHr(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom), "decode conversion");
        DecodedJpeg result{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
        CheckHr(converter->CopyPixels(nullptr, width * 3, static_cast<UINT>(result.bgr.size()), result.bgr.data()), "decode pixels");
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        return result;
    } catch (...) {
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
        throw;
    }
}

std::array<std::uint8_t, 3> PixelAt(const DecodedJpeg& image, const std::uint32_t x, const std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 3;
    return {image.bgr[offset], image.bgr[offset + 1], image.bgr[offset + 2]};
}

a0::m2::FixedRigStitchProfile ApprovedProfile() {
    using namespace std::chrono;
    return {
        "synthetic-approved-rig-v1",
        {
            a0::m2::ProfileStatus::approved,
            "1.1.0",
            "anonymous-generated-test-fixture",
            sys_seconds{seconds{100}},
            sys_seconds{seconds{300}},
            sys_seconds{seconds{200}},
        },
        16,
        8,
        {1.0, 0.0, 12.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0},
        a0::m2::StitchLayout::camera_a_left_camera_b_right,
        {1, 1, 1, 1},
    };
}

a0::m2::FixedRigStitchProfile SnapshotStressProfile() {
    auto profile = ApprovedProfile();
    profile.expected_input_width = 2048;
    profile.expected_input_height = 1024;
    profile.camera_b_to_camera_a = {1.0, 0.0, 1536.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    profile.crop = {};
    return profile;
}

void TestInputHandlesRemainImmutableThroughPublish(const std::filesystem::path& root) {
    const auto a_directory = root / "snapshot-lock-a";
    const auto b_directory = root / "snapshot-lock-b";
    const auto job = root / "snapshot-lock-job";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 2048, 1024, 120, 40, 20);
    WriteSolidJpeg(camera_b, 2048, 1024, 20, 40, 120);
    const auto a_before = ReadBytes(camera_a);
    const auto b_before = ReadBytes(camera_b);

    input_locks_held_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_input_locks_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (input_locks_held_event == nullptr || release_input_locks_event == nullptr) {
        if (input_locks_held_event != nullptr) CloseHandle(input_locks_held_event);
        if (release_input_locks_event != nullptr) CloseHandle(release_input_locks_event);
        throw std::runtime_error("input lock synchronization event creation failed");
    }
    a0::m2::detail::SetInputLocksHeldTestHook(&SignalInputLocksHeldAndWait);
    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, job, SnapshotStressProfile()}));
        } catch (...) {
            worker_error = std::current_exception();
        }
    });

    if (WaitForSingleObject(input_locks_held_event, 5'000) != WAIT_OBJECT_0) {
        SetEvent(release_input_locks_event);
        worker.join();
        a0::m2::detail::SetInputLocksHeldTestHook(nullptr);
        CloseHandle(input_locks_held_event);
        CloseHandle(release_input_locks_event);
        throw std::runtime_error("stitch did not reach the input-lock barrier");
    }
    HANDLE mutation = CreateFileW(
        camera_a.c_str(),
        GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const bool mutable_handle_acquired = mutation != INVALID_HANDLE_VALUE;
    if (mutable_handle_acquired) CloseHandle(mutation);
    SetEvent(release_input_locks_event);
    worker.join();
    a0::m2::detail::SetInputLocksHeldTestHook(nullptr);
    CloseHandle(input_locks_held_event);
    CloseHandle(release_input_locks_event);
    input_locks_held_event = nullptr;
    release_input_locks_event = nullptr;

    Check(!mutable_handle_acquired,
        "CAM-A must remain locked against shared write and delete through completed publish");
    Check(worker_error == nullptr,
        "denied concurrent mutation must not prevent a valid immutable snapshot stitch");
    Check(ReadBytes(camera_a) == a_before && ReadBytes(camera_b) == b_before,
        "immutable snapshot stitching must preserve both originals byte-for-byte");
    Check(std::filesystem::is_regular_file(job / "stitched.jpg")
            && !std::filesystem::exists(job / "stitched.jpg.partial"),
        "immutable snapshot stitching must publish exactly one completed output");
}

void TestStitchSnapshotFailurePreservation(const std::filesystem::path& root) {
    const auto a_directory = root / "snapshot-failure-a";
    const auto b_directory = root / "snapshot-failure-b";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 90, 30, 10);
    WriteSolidJpeg(camera_b, 16, 8, 10, 30, 90);
    const auto original_a = ReadBytes(camera_a);
    const auto original_b = ReadBytes(camera_b);

    HANDLE mutation = CreateFileW(
        camera_b.c_str(),
        GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (mutation == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("snapshot failure test mutation handle creation failed");
    }
    const auto locked_job = root / "snapshot-failure-locked-job";
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
            {camera_a, camera_b, locked_job, ApprovedProfile()})); },
        "cannot be locked",
        "an input already open for shared write/delete must fail before snapshot or output");
    CloseHandle(mutation);
    Check(!std::filesystem::exists(locked_job / "stitched.jpg")
            && !std::filesystem::exists(locked_job / "stitched.jpg.partial")
            && ReadBytes(camera_a) == original_a && ReadBytes(camera_b) == original_b,
        "input lock failure must publish zero and preserve both originals byte-for-byte");

    auto truncated_b = original_b;
    truncated_b.pop_back();
    WriteBytes(camera_b, truncated_b);
    const auto truncated_job = root / "snapshot-failure-truncated-job";
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
            {camera_a, camera_b, truncated_job, ApprovedProfile()})); },
        "complete JPEG",
        "a truncated canonical input snapshot must fail full JPEG validation");
    Check(!std::filesystem::exists(truncated_job / "stitched.jpg")
            && !std::filesystem::exists(truncated_job / "stitched.jpg.partial")
            && ReadBytes(camera_a) == original_a && ReadBytes(camera_b) == truncated_b,
        "truncated input rejection must publish zero and leave both observed originals unchanged");

    WriteBytes(camera_b, original_b);
    const auto conflict_job = root / "snapshot-failure-existing-output";
    std::filesystem::create_directories(conflict_job);
    const auto conflict_output = conflict_job / "stitched.jpg";
    const std::vector<std::uint8_t> sentinel{'k', 'e', 'e', 'p'};
    WriteBytes(conflict_output, sentinel);
    CheckRejected(
        [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
            {camera_a, camera_b, conflict_job, ApprovedProfile()})); },
        "an existing completed stitch output must reject a non-replacing publish");
    Check(ReadBytes(conflict_output) == sentinel
            && !std::filesystem::exists(conflict_job / "stitched.jpg.partial")
            && ReadBytes(camera_a) == original_a && ReadBytes(camera_b) == original_b,
        "existing output rejection must preserve the output sentinel and both originals");
}

void TestStitchRecomposeAndExport(const std::filesystem::path& root) {
    const auto a_directory = root / "capture-a";
    const auto b_directory = root / "capture-b";
    const auto export_directory = root / "export";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    std::filesystem::create_directories(export_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 220, 20, 20);
    WriteSolidJpeg(camera_b, 16, 8, 20, 20, 220);
    const auto a_before = ReadBytes(camera_a);
    const auto b_before = ReadBytes(camera_b);

    const auto first = a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, root / "stitch-job-001", ApprovedProfile()}));
    Check(first.width == 26 && first.height == 6, "fixed translation, overlap, and crop must define output dimensions");
    Check(first.profile_id == "synthetic-approved-rig-v1", "result must retain the approved profile ID");
    Check(std::filesystem::is_regular_file(first.stitched_jpeg), "first job must atomically publish stitched.jpg");
    Check(!std::filesystem::exists(first.stitched_jpeg.parent_path() / "stitched.jpg.partial"), "successful job must leave no partial");
    const auto decoded = DecodeJpeg(first.stitched_jpeg);
    const auto left = PixelAt(decoded, 1, 2);
    const auto overlap = PixelAt(decoded, 13, 2);
    const auto right = PixelAt(decoded, 24, 2);
    Check(left[2] > 170 && left[0] < 70, "CAM-A-only region must retain the red source");
    Check(right[0] > 170 && right[2] < 70, "CAM-B-only region must retain the blue source");
    Check(overlap[0] > 65 && overlap[2] > 65,
        "fixed overlap seam must feather contributions from both sources");

    // GitHub Issue #102 (item 2) regression pin: PublishValidatedGeneratedJpeg
    // now returns the SHA-256 it already computed while validating the
    // partial, and StitchCanonicalPair records that reused value in the
    // manifest instead of recomputing ComputeFileSha256Hex(destination). If
    // that reuse were ever wrong -- e.g. the hash were captured for different
    // bytes than what RenameToWithoutReplace actually publishes --
    // VerifyPublishedStitchJob would independently recompute
    // ComputeFileSha256Hex on the published artifact and throw
    // ArtifactHashMismatch here. This is the same-ID read-only recovery path
    // (GitHub Issue #40's terminal-success check), so calling it here also
    // pins that the manifest and the published file agree end to end, not
    // just that the reused hash happens to equal itself.
    const auto recovered_first = a0::m2::VerifyPublishedStitchJob(
        first.stitched_jpeg.parent_path(), first.stitch_job_id);
    Check(recovered_first.output.sha256 == a0::m2::ComputeFileSha256Hex(first.stitched_jpeg)
            && recovered_first.output.width_pixels == first.width
            && recovered_first.output.height_pixels == first.height,
        "reused publish-time SHA-256 recorded in the manifest must match the published artifact");

    const auto second = a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, root / "stitch-job-002", ApprovedProfile()}));
    Check(second.stitched_jpeg != first.stitched_jpeg && std::filesystem::is_regular_file(second.stitched_jpeg),
        "recomposition must publish into a distinct output job");
    Check(ReadBytes(first.stitched_jpeg) == ReadBytes(second.stitched_jpeg),
        "same originals and fixed profile must recompose deterministically");
    Check(ReadBytes(camera_a) == a_before && ReadBytes(camera_b) == b_before,
        "stitch and recompose must retain both canonical input JPEGs byte-for-byte");

    const auto exported = export_directory / "explicit-result.jpg";
    a0::m2::ExportStitchedJpeg(first.stitched_jpeg, exported);
    Check(ReadBytes(exported) == ReadBytes(first.stitched_jpeg), "explicit export must be byte-identical");
    Check(!std::filesystem::exists(export_directory / "explicit-result.jpg.partial"), "successful export must leave no partial");
    CheckRejected([&] { a0::m2::ExportStitchedJpeg(first.stitched_jpeg, exported); },
        "explicit export must never overwrite an existing destination");
}

// GitHub Issue #86: 非整数の平行移動を含むリグプロファイルでは、被覆判定(pixel-center)と
// キャンバス寸法/バウンディング(pixel-corner)の不整合により、幾何学的には全面被覆されて
// いても縁 1px 強が uncovered pixel と誤判定され stitch 全体が例外で失敗していた。整数
// フィクスチャ(+12.0)だけを使う既存テストでは再現しなかった回帰。
void TestNonIntegerTransformStitchesWithoutUncoveredPixel(const std::filesystem::path& root) {
    const auto a_directory = root / "noninteger-a";
    const auto b_directory = root / "noninteger-b";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 220, 20, 20);
    WriteSolidJpeg(camera_b, 16, 8, 20, 20, 220);
    auto profile = ApprovedProfile();
    // +12.5px の純平行移動。b_bounds x=[12.5,28.5] → canvas_width=29、最右列(global_x=28)は
    // CAM-A の範囲外かつ inverse_b で source_x=15.5(=width-0.5)となり、従来の [0,width-1] 判定で
    // has_a/has_b とも false になって uncovered pixel 例外を投げていた。
    profile.camera_b_to_camera_a = {1.0, 0.0, 12.5, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    profile.crop = {0, 0, 0, 0};
    const auto result = a0::m2::StitchCanonicalPair(
        WithRecordedIdentity({camera_a, camera_b, root / "noninteger-job", profile}));
    Check(result.width == 29 && result.height == 8,
        "non-integer +12.5 translation must size the canvas to 29x8 without an uncovered-pixel failure");
    Check(std::filesystem::is_regular_file(result.stitched_jpeg),
        "non-integer transform must publish a stitched result instead of throwing uncovered-pixel");
}

void TestFailClosedContracts(const std::filesystem::path& root) {
    const auto a_directory = root / "reject-a";
    const auto b_directory = root / "reject-b";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 1, 2, 3);
    WriteSolidJpeg(camera_b, 16, 8, 4, 5, 6);

    auto draft = ApprovedProfile();
    draft.trust.status = a0::m2::ProfileStatus::draft;
    CheckRejected([&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, root / "reject-draft", draft})); },
        "draft profile must be rejected before output");

    auto singular = ApprovedProfile();
    singular.camera_b_to_camera_a = {};
    CheckRejected([&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, root / "reject-singular", singular})); },
        "singular fixed transform must be rejected without estimation fallback");

    CheckRejected([&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, a_directory, ApprovedProfile()})); },
        "output must not share a canonical input job directory");
}

void TestCoverageMaskContracts(const std::filesystem::path& root) {
    const auto a_directory = root / "coverage-a";
    const auto b_directory = root / "coverage-b";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 0, 0, 0);
    WriteSolidJpeg(camera_b, 16, 8, 0, 0, 0);
    const auto a_before = ReadBytes(camera_a);
    const auto b_before = ReadBytes(camera_b);

    const auto valid_black = a0::m2::StitchCanonicalPair(WithRecordedIdentity(
        {camera_a, camera_b, root / "coverage-valid-black", ApprovedProfile()}));
    Check(std::filesystem::is_regular_file(valid_black.stitched_jpeg),
        "valid black source pixels must not be mistaken for uncovered output");

    auto uncovered = ApprovedProfile();
    uncovered.camera_b_to_camera_a = {
        1.0, -0.5, 12.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    };
    const auto rejected_job = root / "coverage-reject-uncovered";
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, rejected_job, uncovered})); },
        "uncovered output pixel",
        "an approved crop containing a pixel from neither source must fail closed");
    Check(!std::filesystem::exists(rejected_job / "stitched.jpg")
            && !std::filesystem::exists(rejected_job / "stitched.jpg.partial"),
        "coverage rejection must not publish a stitched JPEG or partial");
    Check(ReadBytes(camera_a) == a_before && ReadBytes(camera_b) == b_before,
        "coverage validation must preserve both canonical originals byte-for-byte");

    auto cropped_wedge = uncovered;
    cropped_wedge.crop = {0, 0, 4, 0};
    const auto cropped = a0::m2::StitchCanonicalPair(WithRecordedIdentity(
        {camera_a, camera_b, root / "coverage-cropped-wedge", cropped_wedge}));
    Check(cropped.width == 24 && cropped.height == 8
            && std::filesystem::is_regular_file(cropped.stitched_jpeg),
        "a fixed crop that removes the complete uncovered wedge must remain accepted");
}

void TestProjectiveDomainContracts(const std::filesystem::path& root) {
    const auto a_directory = root / "projective-a";
    const auto b_directory = root / "projective-b";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 10, 20, 30);
    WriteSolidJpeg(camera_b, 16, 8, 30, 20, 10);
    const auto a_before = ReadBytes(camera_a);
    const auto b_before = ReadBytes(camera_b);

    auto crossing = ApprovedProfile();
    crossing.camera_b_to_camera_a = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.2, 0.0, -1.0,
    };
    const auto rejected_job = root / "projective-reject-crossing";
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, rejected_job, crossing})); },
        "projective denominator crosses the input image",
        "a fixed transform with an infinity line inside the input rectangle must fail preflight");
    Check(!std::filesystem::exists(rejected_job),
        "projective-domain preflight rejection must happen before output job creation");
    Check(ReadBytes(camera_a) == a_before && ReadBytes(camera_b) == b_before,
        "projective-domain preflight must preserve both canonical originals byte-for-byte");

    auto negative_homogeneous_scale = ApprovedProfile();
    for (double& value : negative_homogeneous_scale.camera_b_to_camera_a) {
        value = -value;
    }
    const auto accepted = a0::m2::StitchCanonicalPair(WithRecordedIdentity(
        {camera_a, camera_b, root / "projective-negative-scale", negative_homogeneous_scale}));
    Check(accepted.width == 26 && accepted.height == 6
            && std::filesystem::is_regular_file(accepted.stitched_jpeg),
        "a valid fixed transform with consistently negative homogeneous scale must remain accepted");

    auto inverse_pole_outside_b = ApprovedProfile();
    inverse_pole_outside_b.camera_b_to_camera_a = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.1, 0.0, 1.0,
    };
    const auto a_only_accepted = a0::m2::StitchCanonicalPair(WithRecordedIdentity(
        {camera_a, camera_b, root / "projective-inverse-pole-a-only", inverse_pole_outside_b}));
    Check(a_only_accepted.width == 14 && a_only_accepted.height == 6
            && std::filesystem::is_regular_file(a_only_accepted.stitched_jpeg),
        "an inverse pole outside CAM-B coverage must not reject an otherwise valid CAM-A output pixel");
}

void TestCompressedJpegByteLimit(const std::filesystem::path& root) {
    const auto a_directory = root / "size-a";
    const auto b_directory = root / "size-b";
    const auto metadata_directory = root / "size-metadata";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    std::filesystem::create_directories(metadata_directory);
    const auto camera_a = a_directory / "original.jpg";
    const auto camera_b = b_directory / "original.jpg";
    const auto metadata_jpeg = metadata_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 20, 40, 60);
    WriteSolidJpeg(camera_b, 16, 8, 60, 40, 20);
    const auto valid_jpeg = ReadBytes(camera_b);

    std::filesystem::resize_file(camera_b, a0::m2::kMaximumCompressedJpegBytes - 1);
    const auto below = a0::m2::StitchCanonicalPair(WithRecordedIdentity(
        {camera_a, camera_b, root / "size-below-job", ApprovedProfile()}));
    Check(std::filesystem::is_regular_file(below.stitched_jpeg),
        "compressed JPEG one byte below the limit must reach WIC and remain accepted");

    std::filesystem::resize_file(camera_b, a0::m2::kMaximumCompressedJpegBytes);
    const auto exact = a0::m2::StitchCanonicalPair(WithRecordedIdentity(
        {camera_a, camera_b, root / "size-exact-job", ApprovedProfile()}));
    Check(std::filesystem::is_regular_file(exact.stitched_jpeg),
        "compressed JPEG exactly at the limit must remain accepted");

    std::filesystem::resize_file(camera_b, a0::m2::kMaximumCompressedJpegBytes + 1);
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, root / "size-over-job", ApprovedProfile()})); },
        "compressed JPEG byte size",
        "compressed JPEG one byte above the limit must fail before WIC decoder creation");

    WriteOversizedMetadataJpeg(metadata_jpeg, valid_jpeg);
    Check(std::filesystem::file_size(metadata_jpeg) > a0::m2::kMaximumCompressedJpegBytes,
        "generated metadata JPEG must exceed the compressed byte limit");
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, metadata_jpeg, root / "size-metadata-job", ApprovedProfile()})); },
        "compressed JPEG byte size",
        "oversized APP1 metadata JPEG must fail before WIC decoder creation");
}

void TestExportValidationFailures(const std::filesystem::path& root) {
    const auto directory = root / "export-negative";
    std::filesystem::create_directories(directory);

    const auto valid_source = directory / "valid-source.jpg";
    WriteSolidJpeg(valid_source, 16, 8, 30, 60, 90);

    const auto non_jpeg = directory / "non-jpeg.jpg";
    WriteBytes(non_jpeg, {'n', 'o', 't', '-', 'j', 'p', 'e', 'g'});
    const auto non_jpeg_destination = directory / "non-jpeg-export.jpg";
    CheckRejected([&] { a0::m2::ExportStitchedJpeg(non_jpeg, non_jpeg_destination); },
        "explicit export must reject a non-JPEG source");
    Check(!std::filesystem::exists(non_jpeg_destination),
        "non-JPEG rejection must not publish a destination");

    const auto corrupt_jpeg = directory / "corrupt.jpg";
    auto corrupt_bytes = ReadBytes(valid_source);
    corrupt_bytes.pop_back();
    WriteBytes(corrupt_jpeg, corrupt_bytes);
    const auto corrupt_destination = directory / "corrupt-export.jpg";
    CheckRejected([&] { a0::m2::ExportStitchedJpeg(corrupt_jpeg, corrupt_destination); },
        "explicit export must reject a truncated JPEG");
    Check(!std::filesystem::exists(corrupt_destination),
        "corrupt JPEG rejection must not publish a destination");

    const auto partial_destination = directory / "existing-partial-export.jpg";
    const auto existing_partial = directory / "existing-partial-export.jpg.partial";
    const std::vector<std::uint8_t> sentinel{'k', 'e', 'e', 'p'};
    WriteBytes(existing_partial, sentinel);
    CheckRejected([&] { a0::m2::ExportStitchedJpeg(valid_source, partial_destination); },
        "explicit export must reject an existing partial");
    Check(ReadBytes(existing_partial) == sentinel,
        "existing partial rejection must preserve the pre-existing file");

    const auto mutable_source = directory / "mutable-source.jpg";
    std::filesystem::copy_file(valid_source, mutable_source);
    HANDLE mutation_handle = CreateFileW(
        mutable_source.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (mutation_handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("source mutation test handle creation failed");
    }
    const auto mutable_destination = directory / "mutable-export.jpg";
    CheckRejectedContains(
        [&] { a0::m2::ExportStitchedJpeg(mutable_source, mutable_destination); },
        "cannot be locked",
        "a source open for mutation must block explicit export");
    CloseHandle(mutation_handle);
    Check(!std::filesystem::exists(mutable_destination)
            && !std::filesystem::exists(directory / "mutable-export.jpg.partial"),
        "source lock rejection must not leave export artifacts");

    const auto oversized_source = directory / "oversized-source.jpg";
    std::filesystem::copy_file(valid_source, oversized_source);
    std::filesystem::resize_file(oversized_source, a0::m2::kMaximumCompressedJpegBytes + 1);
    const auto oversized_destination = directory / "oversized-export.jpg";
    CheckRejectedContains(
        [&] { a0::m2::ExportStitchedJpeg(oversized_source, oversized_destination); },
        "compressed JPEG byte size",
        "explicit export must independently enforce the compressed JPEG byte limit");
}

void TestGeneratedPartialValidationAndNonReplacingPublish(const std::filesystem::path& root) {
    const auto directory = root / "generated-publish";
    std::filesystem::create_directories(directory);
    const auto source = directory / "source.jpg";
    WriteSolidJpeg(source, 16, 8, 80, 40, 20);
    const auto valid_bytes = ReadBytes(source);

    const auto valid_partial = directory / "valid.jpg.partial";
    const auto valid_destination = directory / "valid.jpg";
    WriteBytes(valid_partial, valid_bytes);
    a0::m2::detail::PublishValidatedGeneratedJpeg(valid_partial, valid_destination, 16, 8);
    const auto valid_decoded = DecodeJpeg(valid_destination);
    Check(valid_decoded.width == 16 && valid_decoded.height == 8
            && !std::filesystem::exists(valid_partial),
        "validated generated JPEG must publish atomically with fixed dimensions");

    auto truncated_bytes = valid_bytes;
    truncated_bytes.pop_back();
    const auto truncated_partial = directory / "truncated.jpg.partial";
    const auto truncated_destination = directory / "truncated.jpg";
    WriteBytes(truncated_partial, truncated_bytes);
    CheckRejectedContains(
        [&] { a0::m2::detail::PublishValidatedGeneratedJpeg(
            truncated_partial, truncated_destination, 16, 8); },
        "complete JPEG",
        "a short write that leaves a truncated generated partial must fail before publish");
    Check(!std::filesystem::exists(truncated_destination),
        "truncated generated partial rejection must leave completed output zero");

    // GitHub Issue #99: PublishValidatedGeneratedJpeg no longer runs a full
    // pixel decode -- it validates via ReadJpegSnapshotStructureOnly, which
    // still opens the WIC decoder/frame and parses the frame header (SOF) to
    // read declared dimensions. `0x00` right after the SOI marker is not a
    // valid marker byte, so this fixture still fails during that structural
    // parse, not during pixel decode. This does NOT exercise -- and this
    // lightweight path does not catch -- corruption confined to the
    // entropy-coded scan data of an otherwise well-formed frame header; that
    // detection was traded away deliberately (see ValidateJpegDimensions).
    const auto corrupt_partial = directory / "corrupt.jpg.partial";
    const auto corrupt_destination = directory / "corrupt.jpg";
    WriteBytes(corrupt_partial, {0xff, 0xd8, 0x00, 0xff, 0xd9});
    CheckRejected(
        [&] { a0::m2::detail::PublishValidatedGeneratedJpeg(
            corrupt_partial, corrupt_destination, 16, 8); },
        "a generated JPEG with markers but failed structural validation must not publish");
    Check(!std::filesystem::exists(corrupt_destination),
        "generated structural-validation failure must leave completed output zero");

    const auto wrong_dimensions_partial = directory / "wrong-dimensions.jpg.partial";
    const auto wrong_dimensions_destination = directory / "wrong-dimensions.jpg";
    WriteSolidJpeg(wrong_dimensions_partial, 8, 8, 10, 20, 30);
    CheckRejectedContains(
        [&] { a0::m2::detail::PublishValidatedGeneratedJpeg(
            wrong_dimensions_partial, wrong_dimensions_destination, 16, 8); },
        "dimensions",
        "generated JPEG dimensions must match the fixed stitched result before publish");
    Check(!std::filesystem::exists(wrong_dimensions_destination),
        "generated dimension mismatch must leave completed output zero");

    const auto oversized_partial = directory / "oversized.jpg.partial";
    const auto oversized_destination = directory / "oversized.jpg";
    WriteBytes(oversized_partial, valid_bytes);
    std::filesystem::resize_file(oversized_partial, a0::m2::kMaximumCompressedJpegBytes + 1);
    CheckRejectedContains(
        [&] { a0::m2::detail::PublishValidatedGeneratedJpeg(
            oversized_partial, oversized_destination, 16, 8); },
        "compressed JPEG byte size",
        "generated partial must retain the existing compressed JPEG size ceiling");
    Check(!std::filesystem::exists(oversized_destination),
        "oversized generated partial must leave completed output zero");

    const auto conflict_partial = directory / "conflict.jpg.partial";
    const auto conflict_destination = directory / "conflict.jpg";
    const std::vector<std::uint8_t> sentinel{'k', 'e', 'e', 'p'};
    WriteBytes(conflict_partial, valid_bytes);
    WriteBytes(conflict_destination, sentinel);
    CheckRejectedContains(
        [&] { a0::m2::detail::PublishValidatedGeneratedJpeg(
            conflict_partial, conflict_destination, 16, 8); },
        "publish",
        "generated JPEG publish must reject an existing output or rename conflict");
    Check(ReadBytes(conflict_destination) == sentinel,
        "non-replacing publish must preserve the existing completed output byte-for-byte");

    const auto race_partial = directory / "race.jpg.partial";
    const auto race_renamed = directory / "race-renamed-away.jpg.partial";
    const auto race_destination = directory / "race.jpg";
    const std::vector<std::uint8_t> unverified_replacement{'n', 'o', 't', '-', 'j', 'p', 'e', 'g'};
    WriteBytes(race_partial, valid_bytes);
    publish_race_partial = race_partial;
    publish_race_renamed = race_renamed;
    publish_race_replacement = unverified_replacement;
    a0::m2::detail::SetBeforePublishRenameTestHook(&RenameVerifiedPartialAwayAndReplacePath);
    try {
        a0::m2::detail::PublishValidatedGeneratedJpeg(
            race_partial, race_destination, 16, 8);
    } catch (...) {
        a0::m2::detail::SetBeforePublishRenameTestHook(nullptr);
        throw;
    }
    a0::m2::detail::SetBeforePublishRenameTestHook(nullptr);
    Check(ReadBytes(race_destination) == valid_bytes,
        "publish must rename the exact verified handle, never an unverified same-path replacement");
    Check(ReadBytes(race_partial) == unverified_replacement
            && !std::filesystem::exists(race_renamed),
        "unverified same-path replacement must remain unpublished and the verified handle must move atomically");

    const auto export_source = directory / "race-export-source.jpg";
    const auto export_destination = directory / "race-export.jpg";
    const auto export_partial = directory / "race-export.jpg.partial";
    const auto export_renamed = directory / "race-export-renamed-away.jpg.partial";
    WriteBytes(export_source, valid_bytes);
    publish_race_partial = export_partial;
    publish_race_renamed = export_renamed;
    publish_race_replacement = unverified_replacement;
    a0::m2::detail::SetBeforePublishRenameTestHook(&RenameVerifiedPartialAwayAndReplacePath);
    try {
        a0::m2::ExportStitchedJpeg(export_source, export_destination);
    } catch (...) {
        a0::m2::detail::SetBeforePublishRenameTestHook(nullptr);
        throw;
    }
    a0::m2::detail::SetBeforePublishRenameTestHook(nullptr);
    Check(ReadBytes(export_source) == valid_bytes && ReadBytes(export_destination) == valid_bytes,
        "explicit export race must preserve its original and publish only the verified bytes");
    Check(ReadBytes(export_partial) == unverified_replacement
            && !std::filesystem::exists(export_renamed),
        "explicit export must not publish an unverified same-path partial replacement");
}

void LockNewPartialBeforeWicOpen() {
    actual_io_blocking_handle = CreateFileW(
        actual_io_partial.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (actual_io_blocking_handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("actual WIC-open failure setup failed");
    }
}

void LockPartialAgainstFlushWrite() {
    actual_io_blocking_handle = CreateFileW(
        actual_io_partial.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (actual_io_blocking_handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("actual flush-open failure setup failed");
    }
}

void CreateCompetingOutputBeforeExactHandlePublish() {
    WriteBytes(actual_io_destination, actual_io_destination_sentinel);
}

void CloseActualIoBlockingHandle() noexcept {
    if (actual_io_blocking_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(actual_io_blocking_handle);
        actual_io_blocking_handle = INVALID_HANDLE_VALUE;
    }
}

std::string FaultCode(const a0::m2::detail::OfflineStitchFaultPoint point) {
    using Point = a0::m2::detail::OfflineStitchFaultPoint;
    switch (point) {
    case Point::encode_failure: return "encode-failed";
    case Point::partial_short_write: return "partial-short-write";
    case Point::partial_flush_failure: return "partial-flush-failed";
    case Point::disk_full: return "disk-full";
    case Point::publish_failure: return "publish-failed";
    default: return "unexpected-fault";
    }
}

std::filesystem::path CurrentExecutable() {
    std::wstring buffer(32'768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("test executable path lookup failed");
    }
    buffer.resize(length);
    return buffer;
}

DWORD RunFaultChild(
    const a0::m2::detail::OfflineStitchFaultPoint point,
    const std::filesystem::path& case_root) {
    std::wstring command = L"\"" + CurrentExecutable().wstring() + L"\" --fault-child "
        + std::to_wstring(static_cast<std::uint32_t>(point)) + L" \"" + case_root.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        throw std::runtime_error("fault child process creation failed");
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 10'000);
    if (wait != WAIT_OBJECT_0) {
        (void)TerminateProcess(process.hProcess, 254);
        (void)WaitForSingleObject(process.hProcess, 5'000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        throw std::runtime_error("fault child process did not terminate within the bound");
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        throw std::runtime_error("fault child exit inspection failed");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code;
}

void PrepareFaultCase(
    const std::filesystem::path& case_root,
    std::filesystem::path& camera_a,
    std::filesystem::path& camera_b,
    std::vector<std::uint8_t>& original_a,
    std::vector<std::uint8_t>& original_b) {
    const auto a_directory = case_root / "a";
    const auto b_directory = case_root / "b";
    std::filesystem::create_directories(a_directory);
    std::filesystem::create_directories(b_directory);
    camera_a = a_directory / "original.jpg";
    camera_b = b_directory / "original.jpg";
    WriteSolidJpeg(camera_a, 16, 8, 160, 40, 20);
    WriteSolidJpeg(camera_b, 16, 8, 20, 40, 160);
    original_a = ReadBytes(camera_a);
    original_b = ReadBytes(camera_b);
}

std::size_t CountJobArtifacts(const std::filesystem::path& job) {
    if (!std::filesystem::is_directory(job)) return 0;
    return static_cast<std::size_t>(std::count_if(
        std::filesystem::directory_iterator(job),
        std::filesystem::directory_iterator{},
        [](const std::filesystem::directory_entry& entry) {
            return entry.is_regular_file();
        }));
}

void TestDeterministicPublishFaultMatrix(const std::filesystem::path& root) {
    using Point = a0::m2::detail::OfflineStitchFaultPoint;
    struct FaultCase {
        Point point;
        bool expect_partial;
    };
    for (const auto& fault : {
             FaultCase{Point::encode_failure, false},
             FaultCase{Point::partial_short_write, true},
             FaultCase{Point::partial_flush_failure, true},
             FaultCase{Point::disk_full, true},
             FaultCase{Point::publish_failure, true}}) {
        const auto case_root = root / ("fault-" + std::to_string(static_cast<std::uint32_t>(fault.point)));
        const auto job = case_root / "job";
        const auto partial = job / "stitched.jpg.partial";
        const auto output = job / "stitched.jpg";
        std::filesystem::path camera_a;
        std::filesystem::path camera_b;
        std::vector<std::uint8_t> original_a;
        std::vector<std::uint8_t> original_b;
        PrepareFaultCase(case_root, camera_a, camera_b, original_a, original_b);

        a0::m2::detail::SetOfflineStitchFaultForTest(fault.point);
        std::string error;
        try {
            (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, job, ApprovedProfile()}));
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        const auto trigger_count = a0::m2::detail::GetOfflineStitchFaultTriggerCountForTest();
        a0::m2::detail::SetOfflineStitchFaultForTest(Point::none);

        Check(error == "m2_fault:" + FaultCode(fault.point),
            "injected failure response must be a bounded typed code without path or OS detail");
        Check(error.size() <= 64 && error.find(case_root.string()) == std::string::npos,
            "fault response must not leak a job path or unbounded detail");
        Check(trigger_count == 1,
            "each injected boundary must trigger exactly once with no internal retry");
        Check(!std::filesystem::exists(output)
                && std::filesystem::exists(partial) == fault.expect_partial
                && CountJobArtifacts(job) == (fault.expect_partial ? 1U : 0U),
            "failed publish must leave completed output zero and one deterministic diagnostic orphan at most");
        Check(ReadBytes(camera_a) == original_a && ReadBytes(camera_b) == original_b,
            "fault injection must preserve both canonical originals byte-for-byte");

        if (fault.expect_partial) {
            const auto orphan_before_restart = ReadBytes(partial);
            CheckRejected(
                [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
                    {camera_a, camera_b, job, ApprovedProfile()})); },
                "restart must not treat or clean an orphan partial as success");
            Check(ReadBytes(partial) == orphan_before_restart && !std::filesystem::exists(output)
                    && CountJobArtifacts(job) == 1,
                "restart must retain the exact diagnostic partial with cleanup and retry zero");
        } else {
            Check(std::filesystem::is_directory(job) && CountJobArtifacts(job) == 0,
                "a zero-file failure boundary must retain the durable job reservation");
            CheckRejected(
                [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
                    {camera_a, camera_b, job, ApprovedProfile()})); },
                "restart must reject a reserved empty job instead of retrying encode");
            Check(std::filesystem::is_directory(job) && CountJobArtifacts(job) == 0
                    && !std::filesystem::exists(partial) && !std::filesystem::exists(output),
                "zero-file restart rejection must preserve the unknown reservation without retry or cleanup");
        }
    }

    const auto existing_root = root / "fault-existing-artifacts";
    std::filesystem::path camera_a;
    std::filesystem::path camera_b;
    std::vector<std::uint8_t> original_a;
    std::vector<std::uint8_t> original_b;
    PrepareFaultCase(existing_root, camera_a, camera_b, original_a, original_b);
    for (const auto name : {L"stitched.jpg.partial", L"stitched.jpg"}) {
        const auto job = existing_root / std::filesystem::path(name).stem();
        std::filesystem::create_directories(job);
        const auto artifact = job / name;
        const std::vector<std::uint8_t> sentinel{'k', 'e', 'e', 'p'};
        WriteBytes(artifact, sentinel);
        CheckRejected(
            [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
                {camera_a, camera_b, job, ApprovedProfile()})); },
            "existing partial or output must reject before encode");
        Check(ReadBytes(artifact) == sentinel && CountJobArtifacts(job) == 1,
            "existing output must never be replaced and existing partial must never be cleaned");
    }
    Check(ReadBytes(camera_a) == original_a && ReadBytes(camera_b) == original_b,
        "existing artifact rejection must preserve both originals");
}

void TestCrashRestartFaultMatrix(const std::filesystem::path& root) {
    using Point = a0::m2::detail::OfflineStitchFaultPoint;
    struct CrashCase {
        Point point;
        bool expect_partial;
        bool expect_output;
    };
    for (const auto& crash : {
             CrashCase{Point::interrupt_before_encode, false, false},
             CrashCase{Point::interrupt_after_partial_write, true, false},
             CrashCase{Point::interrupt_after_flush, true, false},
             CrashCase{Point::interrupt_before_publish, true, false},
             CrashCase{Point::interrupt_after_publish, false, true}}) {
        const auto case_root = root / ("crash-" + std::to_string(static_cast<std::uint32_t>(crash.point)));
        const auto job = case_root / "job";
        const auto partial = job / "stitched.jpg.partial";
        const auto output = job / "stitched.jpg";
        std::filesystem::path camera_a;
        std::filesystem::path camera_b;
        std::vector<std::uint8_t> original_a;
        std::vector<std::uint8_t> original_b;
        PrepareFaultCase(case_root, camera_a, camera_b, original_a, original_b);

        Check(RunFaultChild(crash.point, case_root) == 197,
            "process interruption must terminate at the selected bounded publish boundary");
        Check(std::filesystem::exists(partial) == crash.expect_partial
                && std::filesystem::exists(output) == crash.expect_output
                && CountJobArtifacts(job) == (crash.expect_partial || crash.expect_output ? 1U : 0U),
            "crash boundary must leave one deterministic orphan state at most");
        Check(ReadBytes(camera_a) == original_a && ReadBytes(camera_b) == original_b,
            "process interruption must preserve both canonical originals byte-for-byte");

        const auto artifact = crash.expect_partial ? partial : output;
        if (crash.expect_partial || crash.expect_output) {
            const auto artifact_before_restart = ReadBytes(artifact);
            CheckRejected(
                [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
                    {camera_a, camera_b, job, ApprovedProfile()})); },
                "restart without a manifest must not infer success from an orphan file");
            Check(ReadBytes(artifact) == artifact_before_restart && CountJobArtifacts(job) == 1,
                "restart must keep automatic cleanup and retry at zero while retaining diagnostics");
        } else {
            Check(std::filesystem::is_directory(job) && CountJobArtifacts(job) == 0,
                "pre-encode interruption must leave a durable zero-file reservation");
            CheckRejected(
                [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
                    {camera_a, camera_b, job, ApprovedProfile()})); },
                "restart after a zero-file interruption must not retry the same job");
            Check(std::filesystem::is_directory(job) && CountJobArtifacts(job) == 0,
                "zero-file crash restart must retain its unknown reservation unchanged");
        }
    }
}

void TestActualIoFailuresUseSameOrphanPolicy(const std::filesystem::path& root) {
    struct ActualFailureCase {
        const char* name;
        void (*install_hook)();
        void (*clear_hook)() noexcept;
        bool expect_partial;
        bool expect_destination;
    };
    const auto clear_encode = []() noexcept {
        a0::m2::detail::SetBeforeEncodeTestHook(nullptr);
    };
    const auto clear_flush = []() noexcept {
        a0::m2::detail::SetBeforePartialFlushTestHook(nullptr);
    };
    const auto clear_publish = []() noexcept {
        a0::m2::detail::SetBeforePublishRenameTestHook(nullptr);
    };
    const auto install_encode = [] {
        a0::m2::detail::SetBeforeEncodeTestHook(&LockNewPartialBeforeWicOpen);
    };
    const auto install_flush = [] {
        a0::m2::detail::SetBeforePartialFlushTestHook(&LockPartialAgainstFlushWrite);
    };
    const auto install_publish = [] {
        a0::m2::detail::SetBeforePublishRenameTestHook(&CreateCompetingOutputBeforeExactHandlePublish);
    };

    for (const auto& failure : {
             ActualFailureCase{"wic-open", install_encode, clear_encode, true, false},
             ActualFailureCase{"flush-open", install_flush, clear_flush, true, false},
             ActualFailureCase{"exact-handle-publish", install_publish, clear_publish, true, true}}) {
        const auto case_root = root / (std::string("actual-io-") + failure.name);
        const auto job = case_root / "job";
        const auto partial = job / "stitched.jpg.partial";
        const auto output = job / "stitched.jpg";
        std::filesystem::path camera_a;
        std::filesystem::path camera_b;
        std::vector<std::uint8_t> original_a;
        std::vector<std::uint8_t> original_b;
        PrepareFaultCase(case_root, camera_a, camera_b, original_a, original_b);
        actual_io_partial = partial;
        actual_io_destination = output;
        actual_io_destination_sentinel = {'c', 'o', 'n', 'f', 'l', 'i', 'c', 't'};

        failure.install_hook();
        std::string error;
        try {
            (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({camera_a, camera_b, job, ApprovedProfile()}));
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        failure.clear_hook();
        CloseActualIoBlockingHandle();

        Check(!error.empty() && error.rfind("m2_fault:", 0) != 0,
            std::string("actual ") + failure.name + " failure must come from the real WIC/Win32 call");
        Check(std::filesystem::exists(partial) == failure.expect_partial
                && std::filesystem::exists(output) == failure.expect_destination,
            std::string("actual ") + failure.name + " failure must retain its deterministic diagnostic state");
        if (failure.expect_destination) {
            Check(ReadBytes(output) == actual_io_destination_sentinel,
                "exact-handle non-replacing publish must preserve the competing destination");
        }
        Check(ReadBytes(camera_a) == original_a && ReadBytes(camera_b) == original_b,
            std::string("actual ") + failure.name + " failure must preserve both originals byte-for-byte");

        const auto partial_before_restart = ReadBytes(partial);
        CheckRejected(
            [&] { (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity(
                {camera_a, camera_b, job, ApprovedProfile()})); },
            std::string("actual ") + failure.name + " orphan must reject same-job restart");
        Check(ReadBytes(partial) == partial_before_restart
                && std::filesystem::exists(output) == failure.expect_destination,
            std::string("actual ") + failure.name + " restart rejection must not clean or retry");
    }
}

int RunFaultChildMode(const int argc, char* argv[]) {
    if (argc != 4 || std::string(argv[1]) != "--fault-child") return -1;
    const auto value = std::stoul(argv[2]);
    if (value < static_cast<std::uint32_t>(a0::m2::detail::OfflineStitchFaultPoint::interrupt_before_encode)
        || value > static_cast<std::uint32_t>(a0::m2::detail::OfflineStitchFaultPoint::interrupt_after_publish)) {
        return 196;
    }
    const auto point = static_cast<a0::m2::detail::OfflineStitchFaultPoint>(value);
    const std::filesystem::path case_root = argv[3];
    a0::m2::detail::SetOfflineStitchFaultForTest(point);
    try {
        (void)a0::m2::StitchCanonicalPair(WithRecordedIdentity({
            case_root / "a" / "original.jpg",
            case_root / "b" / "original.jpg",
            case_root / "job",
            ApprovedProfile(),
        }));
    } catch (...) {
        return 195;
    }
    return 194;
}

} // namespace

int main(const int argc, char* argv[]) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        std::cerr << "COM initialization failed\n";
        return 2;
    }
    const bool uninitialize = SUCCEEDED(com_result);
    if (const int child_result = RunFaultChildMode(argc, argv); child_result >= 0) {
        if (uninitialize) CoUninitialize();
        return child_result;
    }
    const auto root = std::filesystem::temp_directory_path()
        / ("a0-offline-stitcher-tests-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()));
    try {
        if (!std::filesystem::create_directory(root)) {
            throw std::runtime_error("unique synthetic test directory already exists");
        }
        TestStitchRecomposeAndExport(root);
        TestNonIntegerTransformStitchesWithoutUncoveredPixel(root);
        TestFailClosedContracts(root);
        TestCoverageMaskContracts(root);
        TestProjectiveDomainContracts(root);
        TestCompressedJpegByteLimit(root);
        TestExportValidationFailures(root);
        TestInputHandlesRemainImmutableThroughPublish(root);
        TestStitchSnapshotFailurePreservation(root);
        TestGeneratedPartialValidationAndNonReplacingPublish(root);
        TestDeterministicPublishFaultMatrix(root);
        TestCrashRestartFaultMatrix(root);
        TestActualIoFailuresUseSameOrphanPolicy(root);
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << "UNEXPECTED: " << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        if (uninitialize) CoUninitialize();
        return 2;
    }
    if (uninitialize) CoUninitialize();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All M2 offline stitcher tests passed\n";
    return 0;
}
