#include "a0/m2/offline_stitcher.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <chrono>
#include <array>
#include <atomic>
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
void PublishValidatedGeneratedJpeg(
    const std::filesystem::path& partial,
    const std::filesystem::path& destination,
    std::uint32_t expected_width,
    std::uint32_t expected_height);
void SetInputLocksHeldTestHook(void (*hook)()) noexcept;
void SetBeforePublishRenameTestHook(void (*hook)()) noexcept;
}

namespace {

int failures = 0;
HANDLE input_locks_held_event = nullptr;
HANDLE release_input_locks_event = nullptr;
std::filesystem::path publish_race_partial;
std::filesystem::path publish_race_renamed;
std::vector<std::uint8_t> publish_race_replacement;

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
            (void)a0::m2::StitchCanonicalPair({camera_a, camera_b, job, SnapshotStressProfile()});
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
        [&] { (void)a0::m2::StitchCanonicalPair(
            {camera_a, camera_b, locked_job, ApprovedProfile()}); },
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
        [&] { (void)a0::m2::StitchCanonicalPair(
            {camera_a, camera_b, truncated_job, ApprovedProfile()}); },
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
        [&] { (void)a0::m2::StitchCanonicalPair(
            {camera_a, camera_b, conflict_job, ApprovedProfile()}); },
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

    const auto first = a0::m2::StitchCanonicalPair({camera_a, camera_b, root / "stitch-job-001", ApprovedProfile()});
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

    const auto second = a0::m2::StitchCanonicalPair({camera_a, camera_b, root / "stitch-job-002", ApprovedProfile()});
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
    CheckRejected([&] { (void)a0::m2::StitchCanonicalPair({camera_a, camera_b, root / "reject-draft", draft}); },
        "draft profile must be rejected before output");

    auto singular = ApprovedProfile();
    singular.camera_b_to_camera_a = {};
    CheckRejected([&] { (void)a0::m2::StitchCanonicalPair({camera_a, camera_b, root / "reject-singular", singular}); },
        "singular fixed transform must be rejected without estimation fallback");

    CheckRejected([&] { (void)a0::m2::StitchCanonicalPair({camera_a, camera_b, a_directory, ApprovedProfile()}); },
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

    const auto valid_black = a0::m2::StitchCanonicalPair(
        {camera_a, camera_b, root / "coverage-valid-black", ApprovedProfile()});
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
        [&] { (void)a0::m2::StitchCanonicalPair({camera_a, camera_b, rejected_job, uncovered}); },
        "uncovered output pixel",
        "an approved crop containing a pixel from neither source must fail closed");
    Check(!std::filesystem::exists(rejected_job / "stitched.jpg")
            && !std::filesystem::exists(rejected_job / "stitched.jpg.partial"),
        "coverage rejection must not publish a stitched JPEG or partial");
    Check(ReadBytes(camera_a) == a_before && ReadBytes(camera_b) == b_before,
        "coverage validation must preserve both canonical originals byte-for-byte");

    auto cropped_wedge = uncovered;
    cropped_wedge.crop = {0, 0, 4, 0};
    const auto cropped = a0::m2::StitchCanonicalPair(
        {camera_a, camera_b, root / "coverage-cropped-wedge", cropped_wedge});
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
        [&] { (void)a0::m2::StitchCanonicalPair({camera_a, camera_b, rejected_job, crossing}); },
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
    const auto accepted = a0::m2::StitchCanonicalPair(
        {camera_a, camera_b, root / "projective-negative-scale", negative_homogeneous_scale});
    Check(accepted.width == 26 && accepted.height == 6
            && std::filesystem::is_regular_file(accepted.stitched_jpeg),
        "a valid fixed transform with consistently negative homogeneous scale must remain accepted");

    auto inverse_pole_outside_b = ApprovedProfile();
    inverse_pole_outside_b.camera_b_to_camera_a = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.1, 0.0, 1.0,
    };
    const auto a_only_accepted = a0::m2::StitchCanonicalPair(
        {camera_a, camera_b, root / "projective-inverse-pole-a-only", inverse_pole_outside_b});
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
    const auto below = a0::m2::StitchCanonicalPair(
        {camera_a, camera_b, root / "size-below-job", ApprovedProfile()});
    Check(std::filesystem::is_regular_file(below.stitched_jpeg),
        "compressed JPEG one byte below the limit must reach WIC and remain accepted");

    std::filesystem::resize_file(camera_b, a0::m2::kMaximumCompressedJpegBytes);
    const auto exact = a0::m2::StitchCanonicalPair(
        {camera_a, camera_b, root / "size-exact-job", ApprovedProfile()});
    Check(std::filesystem::is_regular_file(exact.stitched_jpeg),
        "compressed JPEG exactly at the limit must remain accepted");

    std::filesystem::resize_file(camera_b, a0::m2::kMaximumCompressedJpegBytes + 1);
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair({camera_a, camera_b, root / "size-over-job", ApprovedProfile()}); },
        "compressed JPEG byte size",
        "compressed JPEG one byte above the limit must fail before WIC decoder creation");

    WriteOversizedMetadataJpeg(metadata_jpeg, valid_jpeg);
    Check(std::filesystem::file_size(metadata_jpeg) > a0::m2::kMaximumCompressedJpegBytes,
        "generated metadata JPEG must exceed the compressed byte limit");
    CheckRejectedContains(
        [&] { (void)a0::m2::StitchCanonicalPair({camera_a, metadata_jpeg, root / "size-metadata-job", ApprovedProfile()}); },
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

    const auto corrupt_partial = directory / "corrupt.jpg.partial";
    const auto corrupt_destination = directory / "corrupt.jpg";
    WriteBytes(corrupt_partial, {0xff, 0xd8, 0x00, 0xff, 0xd9});
    CheckRejected(
        [&] { a0::m2::detail::PublishValidatedGeneratedJpeg(
            corrupt_partial, corrupt_destination, 16, 8); },
        "a generated JPEG with markers but failed full decode must not publish");
    Check(!std::filesystem::exists(corrupt_destination),
        "generated decode failure must leave completed output zero");

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

} // namespace

int main() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        std::cerr << "COM initialization failed\n";
        return 2;
    }
    const bool uninitialize = SUCCEEDED(com_result);
    const auto root = std::filesystem::temp_directory_path()
        / ("a0-offline-stitcher-tests-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()));
    try {
        if (!std::filesystem::create_directory(root)) {
            throw std::runtime_error("unique synthetic test directory already exists");
        }
        TestStitchRecomposeAndExport(root);
        TestFailClosedContracts(root);
        TestCoverageMaskContracts(root);
        TestProjectiveDomainContracts(root);
        TestCompressedJpegByteLimit(root);
        TestExportValidationFailures(root);
        TestInputHandlesRemainImmutableThroughPublish(root);
        TestStitchSnapshotFailurePreservation(root);
        TestGeneratedPartialValidationAndNonReplacingPublish(root);
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
