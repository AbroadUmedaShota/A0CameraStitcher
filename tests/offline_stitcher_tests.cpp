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
