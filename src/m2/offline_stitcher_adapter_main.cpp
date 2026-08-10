#include "a0/m2/offline_stitcher.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <typename Interface>
class ComPtr final {
public:
    explicit ComPtr(Interface* value = nullptr) noexcept : value_(value) {}
    ~ComPtr() { if (value_) value_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    [[nodiscard]] Interface* get() const noexcept { return value_; }
private:
    Interface* value_{};
};

void CheckHr(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return output;
}

std::map<std::wstring, std::wstring> ParseOptions(const int argc, wchar_t* argv[], const int first) {
    std::map<std::wstring, std::wstring> options;
    for (int index = first; index < argc; index += 2) {
        if (index + 1 >= argc || std::wstring_view(argv[index]).find(L"--") != 0) {
            throw std::invalid_argument("every option requires a --name and value");
        }
        const std::wstring name = argv[index] + 2;
        if (name.empty() || !options.emplace(name, argv[index + 1]).second) {
            throw std::invalid_argument("option names must be non-empty and unique");
        }
    }
    return options;
}

const std::wstring& Required(
    const std::map<std::wstring, std::wstring>& options,
    const std::wstring& name) {
    const auto found = options.find(name);
    if (found == options.end() || found->second.empty()) {
        throw std::invalid_argument("missing required option: " + Utf8(name));
    }
    return found->second;
}

template <typename Integer>
Integer ParseInteger(const std::wstring& value, const char* name) {
    const auto utf8 = Utf8(value);
    Integer parsed{};
    const auto result = std::from_chars(utf8.data(), utf8.data() + utf8.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != utf8.data() + utf8.size()) {
        throw std::invalid_argument(std::string("invalid integer: ") + name);
    }
    return parsed;
}

std::vector<double> ParseDoubles(const std::wstring& value) {
    std::vector<double> values;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(L',', start);
        const auto token = value.substr(start, end == std::wstring::npos ? value.size() - start : end - start);
        const auto utf8 = Utf8(token);
        std::size_t consumed = 0;
        const auto parsed = std::stod(utf8, &consumed);
        if (consumed != utf8.size()) throw std::invalid_argument("invalid floating point list");
        values.push_back(parsed);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return values;
}

std::vector<std::uint32_t> ParseUnsignedList(const std::wstring& value) {
    std::vector<std::uint32_t> values;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(L',', start);
        values.push_back(ParseInteger<std::uint32_t>(
            value.substr(start, end == std::wstring::npos ? value.size() - start : end - start), "unsigned list"));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return values;
}

void GenerateSyntheticJpeg(
    const std::filesystem::path& destination,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::array<std::uint8_t, 3>& rgb) {
    if (width == 0 || height == 0 || width > 10'000 || height > 10'000) {
        throw std::invalid_argument("synthetic image dimensions are invalid");
    }
    if (destination.filename() != L"original.jpg" || !std::filesystem::is_directory(destination.parent_path()) ||
        std::filesystem::exists(destination)) {
        throw std::invalid_argument("synthetic capture requires a new canonical original.jpg in an existing directory");
    }

    const auto partial = destination.wstring() + L".partial";
    if (std::filesystem::exists(partial)) {
        throw std::invalid_argument("synthetic capture partial already exists");
    }

    {
        IWICImagingFactory* raw_factory = nullptr;
        CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&raw_factory)), "WIC factory creation");
        ComPtr<IWICImagingFactory> factory(raw_factory);
        IWICStream* raw_stream = nullptr;
        CheckHr(factory.get()->CreateStream(&raw_stream), "WIC stream creation");
        ComPtr<IWICStream> stream(raw_stream);
        CheckHr(stream.get()->InitializeFromFilename(partial.c_str(), GENERIC_WRITE), "synthetic partial open");
        IWICBitmapEncoder* raw_encoder = nullptr;
        CheckHr(factory.get()->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &raw_encoder), "JPEG encoder creation");
        ComPtr<IWICBitmapEncoder> encoder(raw_encoder);
        CheckHr(encoder.get()->Initialize(stream.get(), WICBitmapEncoderNoCache), "JPEG encoder initialization");
        IWICBitmapFrameEncode* raw_frame = nullptr;
        IPropertyBag2* raw_properties = nullptr;
        CheckHr(encoder.get()->CreateNewFrame(&raw_frame, &raw_properties), "JPEG frame creation");
        ComPtr<IWICBitmapFrameEncode> frame(raw_frame);
        ComPtr<IPropertyBag2> properties(raw_properties);
        CheckHr(frame.get()->Initialize(properties.get()), "JPEG frame initialization");
        CheckHr(frame.get()->SetSize(width, height), "JPEG frame size");
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        CheckHr(frame.get()->SetPixelFormat(&format), "JPEG pixel format");
        if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("JPEG encoder changed pixel format");
        const auto pixel_count = static_cast<std::uint64_t>(width) * height;
        if (pixel_count > std::numeric_limits<std::uint32_t>::max() / 3U) throw std::invalid_argument("synthetic image is too large");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(pixel_count) * 3U);
        for (std::size_t index = 0; index < pixels.size(); index += 3) {
            pixels[index] = rgb[2];
            pixels[index + 1] = rgb[1];
            pixels[index + 2] = rgb[0];
        }
        CheckHr(frame.get()->WritePixels(height, width * 3U, static_cast<UINT>(pixels.size()), pixels.data()), "JPEG pixels");
        CheckHr(frame.get()->Commit(), "JPEG frame commit");
        CheckHr(encoder.get()->Commit(), "JPEG encoder commit");
    }
    if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("synthetic canonical rename failed");
    }
}

void ValidateCanonicalJpeg(
    const std::filesystem::path& input,
    const std::uint32_t expected_width,
    const std::uint32_t expected_height) {
    if (!std::filesystem::is_regular_file(input) || expected_width == 0 || expected_height == 0) {
        throw std::invalid_argument("canonical JPEG validation input is invalid");
    }
    std::ifstream jpeg_stream(input, std::ios::binary);
    const auto encoded_size = std::filesystem::file_size(input);
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(encoded_size));
    jpeg_stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!jpeg_stream || jpeg_stream.gcount() != static_cast<std::streamsize>(encoded.size()) ||
        encoded.size() < 8U || encoded[0] != 0xffU || encoded[1] != 0xd8U) {
        throw std::runtime_error("canonical JPEG byte stream is malformed");
    }
    std::size_t position = 2U;
    std::size_t entropy_start = 0U;
    while (position + 4U <= encoded.size()) {
        if (encoded[position] != 0xffU) throw std::runtime_error("canonical JPEG marker is malformed");
        while (position < encoded.size() && encoded[position] == 0xffU) ++position;
        if (position >= encoded.size()) throw std::runtime_error("canonical JPEG marker is truncated");
        const auto marker = encoded[position++];
        if (marker == 0xd9U || marker == 0x00U || (marker >= 0xd0U && marker <= 0xd7U)) {
            throw std::runtime_error("canonical JPEG ended before a complete scan");
        }
        if (position + 2U > encoded.size()) throw std::runtime_error("canonical JPEG segment is truncated");
        const auto length = static_cast<std::size_t>(encoded[position] << 8U) | encoded[position + 1U];
        if (length < 2U || position + length > encoded.size()) {
            throw std::runtime_error("canonical JPEG segment length is invalid");
        }
        if (marker == 0xdaU) {
            entropy_start = position + length;
            break;
        }
        position += length;
    }
    if (entropy_start == 0U || entropy_start >= encoded.size()) {
        throw std::runtime_error("canonical JPEG scan is missing or truncated");
    }
    bool entropy_byte_seen = false;
    position = entropy_start;
    while (position < encoded.size()) {
        if (encoded[position] != 0xffU) {
            entropy_byte_seen = true;
            ++position;
            continue;
        }
        while (position < encoded.size() && encoded[position] == 0xffU) ++position;
        if (position >= encoded.size()) throw std::runtime_error("canonical JPEG entropy marker is truncated");
        const auto marker = encoded[position++];
        if (marker == 0x00U || (marker >= 0xd0U && marker <= 0xd7U)) continue;
        if (marker == 0xd9U && position == encoded.size() && entropy_byte_seen) break;
        throw std::runtime_error("canonical JPEG entropy stream contains an invalid marker");
    }
    if (!entropy_byte_seen || position != encoded.size()) {
        throw std::runtime_error("canonical JPEG entropy stream is incomplete");
    }
    IWICImagingFactory* raw_factory = nullptr;
    CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&raw_factory)), "WIC factory creation");
    ComPtr<IWICImagingFactory> factory(raw_factory);
    IWICBitmapDecoder* raw_decoder = nullptr;
    CheckHr(factory.get()->CreateDecoderFromFilename(input.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &raw_decoder), "canonical JPEG decoder creation");
    ComPtr<IWICBitmapDecoder> decoder(raw_decoder);
    GUID container{};
    CheckHr(decoder.get()->GetContainerFormat(&container), "canonical JPEG container query");
    if (container != GUID_ContainerFormatJpeg) throw std::runtime_error("canonical image container is not JPEG");
    UINT frame_count = 0;
    CheckHr(decoder.get()->GetFrameCount(&frame_count), "canonical JPEG frame count");
    if (frame_count != 1U) throw std::runtime_error("canonical JPEG must contain exactly one frame");
    IWICBitmapFrameDecode* raw_frame = nullptr;
    CheckHr(decoder.get()->GetFrame(0, &raw_frame), "canonical JPEG frame read");
    ComPtr<IWICBitmapFrameDecode> frame(raw_frame);
    UINT width = 0;
    UINT height = 0;
    CheckHr(frame.get()->GetSize(&width, &height), "canonical JPEG dimensions");
    if (width != expected_width || height != expected_height) {
        throw std::runtime_error("canonical JPEG dimensions do not match the fixed profile");
    }
    IWICFormatConverter* raw_converter = nullptr;
    CheckHr(factory.get()->CreateFormatConverter(&raw_converter), "canonical JPEG format converter creation");
    ComPtr<IWICFormatConverter> converter(raw_converter);
    CheckHr(converter.get()->Initialize(frame.get(), GUID_WICPixelFormat24bppBGR,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
        "canonical JPEG full pixel decoder initialization");
    const auto stride64 = static_cast<std::uint64_t>(width) * 3U;
    const auto buffer64 = stride64 * height;
    if (stride64 > std::numeric_limits<UINT>::max() || buffer64 > std::numeric_limits<UINT>::max()) {
        throw std::runtime_error("canonical JPEG decoded pixel buffer is too large");
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(buffer64));
    CheckHr(converter.get()->CopyPixels(nullptr, static_cast<UINT>(stride64),
        static_cast<UINT>(buffer64), pixels.data()), "canonical JPEG complete pixel decode");
}

a0::m2::FixedRigStitchProfile ParseProfile(const std::map<std::wstring, std::wstring>& options) {
    const auto matrix_values = ParseDoubles(Required(options, L"matrix"));
    const auto crop_values = ParseUnsignedList(Required(options, L"crop"));
    if (matrix_values.size() != 9 || crop_values.size() != 4) {
        throw std::invalid_argument("fixed profile matrix or crop cardinality is invalid");
    }
    std::array<double, 9> matrix{};
    std::copy(matrix_values.begin(), matrix_values.end(), matrix.begin());
    const auto status = Required(options, L"status");
    const auto layout = Required(options, L"layout");
    if (status != L"approved" && status != L"draft") {
        throw std::invalid_argument("fixed profile status is invalid");
    }
    if (layout != L"camera-a-left-camera-b-right" && layout != L"camera-a-top-camera-b-bottom") {
        throw std::invalid_argument("fixed profile layout is invalid");
    }
    using namespace std::chrono;
    return {
        Utf8(Required(options, L"profile-id")),
        {
            status == L"approved" ? a0::m2::ProfileStatus::approved : a0::m2::ProfileStatus::draft,
            Utf8(Required(options, L"schema-version")),
            Utf8(Required(options, L"provenance")),
            sys_seconds{seconds{ParseInteger<std::int64_t>(Required(options, L"measured-at"), "measured-at")}},
            sys_seconds{seconds{ParseInteger<std::int64_t>(Required(options, L"valid-until"), "valid-until")}},
            sys_seconds{seconds{ParseInteger<std::int64_t>(Required(options, L"assessed-at"), "assessed-at")}},
        },
        ParseInteger<std::uint32_t>(Required(options, L"width"), "width"),
        ParseInteger<std::uint32_t>(Required(options, L"height"), "height"),
        matrix,
        layout == L"camera-a-left-camera-b-right"
            ? a0::m2::StitchLayout::camera_a_left_camera_b_right
            : a0::m2::StitchLayout::camera_a_top_camera_b_bottom,
        {crop_values[0], crop_values[1], crop_values[2], crop_values[3]},
    };
}

} // namespace

int wmain(const int argc, wchar_t* argv[]) {
    try {
        if (argc < 2) throw std::invalid_argument("operation is required");
        const std::wstring operation = argv[1];
        const auto options = ParseOptions(argc, argv, 2);
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) throw std::runtime_error("COM initialization failed");
        try {
            if (operation == L"generate-test-synthetic") {
                const auto alias = Required(options, L"alias");
                if (alias != L"CAM-A" && alias != L"CAM-B") throw std::invalid_argument("synthetic alias is invalid");
                GenerateSyntheticJpeg(
                    Required(options, L"output"),
                    ParseInteger<std::uint32_t>(Required(options, L"width"), "width"),
                    ParseInteger<std::uint32_t>(Required(options, L"height"), "height"),
                    alias == L"CAM-A" ? std::array<std::uint8_t, 3>{220, 20, 20} : std::array<std::uint8_t, 3>{20, 20, 220});
                std::cout << "result=generated-test-synthetic-jpeg\n";
            } else if (operation == L"validate-canonical-jpeg") {
                ValidateCanonicalJpeg(
                    Required(options, L"input"),
                    ParseInteger<std::uint32_t>(Required(options, L"width"), "width"),
                    ParseInteger<std::uint32_t>(Required(options, L"height"), "height"));
                std::cout << "result=validated-canonical-jpeg\n";
            } else if (operation == L"stitch") {
                const auto result = a0::m2::StitchCanonicalPair({
                    Required(options, L"camera-a"),
                    Required(options, L"camera-b"),
                    Required(options, L"job-directory"),
                    ParseProfile(options),
                });
                std::cout << "result=stitched\nwidth=" << result.width << "\nheight=" << result.height
                    << "\nprofileId=" << result.profile_id << '\n';
            } else if (operation == L"export") {
                a0::m2::ExportStitchedJpeg(Required(options, L"source"), Required(options, L"destination"));
                std::cout << "result=exported-byte-identical\n";
            } else {
                throw std::invalid_argument("unsupported operation");
            }
        } catch (...) {
            CoUninitialize();
            throw;
        }
        CoUninitialize();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "error=" << exception.what() << '\n';
        return 2;
    }
}
