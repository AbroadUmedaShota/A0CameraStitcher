#include "a0/m2/offline_stitcher.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

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
        TestCompressedJpegByteLimit(root);
        TestExportValidationFailures(root);
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
