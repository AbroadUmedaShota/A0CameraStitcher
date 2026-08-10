#include "a0/m2/offline_stitcher.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace a0::m2 {
namespace {

constexpr std::uint64_t kMaximumDecodedPixels = 200'000'000;
constexpr std::uint32_t kMaximumImageDimension = 32'768;
constexpr double kMatrixEpsilon = 1e-12;

template <typename T>
struct ComReleaser {
    void operator()(T* value) const noexcept {
        if (value != nullptr) {
            value->Release();
        }
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComReleaser<T>>;

class ComApartment final {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            uninitialize_ = true;
        } else if (result != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("COM initialization failed");
        }
    }

    ~ComApartment() {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool uninitialize_{};
};

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgr;
};

struct Point {
    double x{};
    double y{};
};

struct Bounds {
    double minimum_x{};
    double minimum_y{};
    double maximum_x{};
    double maximum_y{};
};

[[noreturn]] void ThrowHresult(const std::string& operation, const HRESULT result) {
    throw std::runtime_error(operation + " failed (HRESULT " + std::to_string(result) + ")");
}

void CheckHresult(const HRESULT result, const std::string& operation) {
    if (FAILED(result)) {
        ThrowHresult(operation, result);
    }
}

ComPtr<IWICImagingFactory> CreateFactory() {
    IWICImagingFactory* raw = nullptr;
    CheckHresult(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)),
        "WIC factory creation");
    return ComPtr<IWICImagingFactory>(raw);
}

std::uint64_t PixelCount(const std::uint32_t width, const std::uint32_t height) {
    const auto count = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > kMaximumImageDimension
        || height > kMaximumImageDimension || count > kMaximumDecodedPixels) {
        throw std::invalid_argument("JPEG dimensions are empty or exceed the offline stitch limit");
    }
    return count;
}

Image DecodeJpeg(IWICImagingFactory* factory, const std::filesystem::path& path) {
    IWICBitmapDecoder* decoder_raw = nullptr;
    CheckHresult(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder_raw),
        "JPEG decoder creation");
    ComPtr<IWICBitmapDecoder> decoder(decoder_raw);

    GUID container{};
    CheckHresult(decoder->GetContainerFormat(&container), "JPEG container inspection");
    if (container != GUID_ContainerFormatJpeg) {
        throw std::invalid_argument("canonical input must be a JPEG container");
    }

    UINT frame_count = 0;
    CheckHresult(decoder->GetFrameCount(&frame_count), "JPEG frame count");
    if (frame_count != 1) {
        throw std::invalid_argument("canonical JPEG must contain exactly one frame");
    }

    IWICBitmapFrameDecode* frame_raw = nullptr;
    CheckHresult(decoder->GetFrame(0, &frame_raw), "JPEG frame decode");
    ComPtr<IWICBitmapFrameDecode> frame(frame_raw);

    UINT width = 0;
    UINT height = 0;
    CheckHresult(frame->GetSize(&width, &height), "JPEG dimensions");
    const auto pixels = PixelCount(width, height);

    IWICFormatConverter* converter_raw = nullptr;
    CheckHresult(factory->CreateFormatConverter(&converter_raw), "JPEG pixel converter creation");
    ComPtr<IWICFormatConverter> converter(converter_raw);
    CheckHresult(converter->Initialize(
        frame.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom), "JPEG pixel conversion");

    Image image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(pixels * 3))};
    const auto stride = width * 3U;
    CheckHresult(converter->CopyPixels(nullptr, stride, static_cast<UINT>(image.bgr.size()), image.bgr.data()),
        "JPEG pixel read");
    return image;
}

Point Transform(const std::array<double, 9>& matrix, const Point point) {
    const double denominator = matrix[6] * point.x + matrix[7] * point.y + matrix[8];
    if (!std::isfinite(denominator) || std::abs(denominator) <= kMatrixEpsilon) {
        throw std::invalid_argument("fixed transform maps an image corner to infinity");
    }
    const Point transformed{
        (matrix[0] * point.x + matrix[1] * point.y + matrix[2]) / denominator,
        (matrix[3] * point.x + matrix[4] * point.y + matrix[5]) / denominator,
    };
    if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y)) {
        throw std::invalid_argument("fixed transform produces a non-finite coordinate");
    }
    return transformed;
}

std::array<double, 9> Invert(const std::array<double, 9>& matrix) {
    for (const double value : matrix) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("fixed transform values must be finite");
        }
    }
    const double determinant =
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= kMatrixEpsilon) {
        throw std::invalid_argument("fixed transform must be invertible");
    }
    return {
        (matrix[4] * matrix[8] - matrix[5] * matrix[7]) / determinant,
        (matrix[2] * matrix[7] - matrix[1] * matrix[8]) / determinant,
        (matrix[1] * matrix[5] - matrix[2] * matrix[4]) / determinant,
        (matrix[5] * matrix[6] - matrix[3] * matrix[8]) / determinant,
        (matrix[0] * matrix[8] - matrix[2] * matrix[6]) / determinant,
        (matrix[2] * matrix[3] - matrix[0] * matrix[5]) / determinant,
        (matrix[3] * matrix[7] - matrix[4] * matrix[6]) / determinant,
        (matrix[1] * matrix[6] - matrix[0] * matrix[7]) / determinant,
        (matrix[0] * matrix[4] - matrix[1] * matrix[3]) / determinant,
    };
}

Bounds TransformedBounds(const Image& image, const std::array<double, 9>& matrix) {
    const std::array<Point, 4> corners{{
        {0.0, 0.0},
        {static_cast<double>(image.width), 0.0},
        {0.0, static_cast<double>(image.height)},
        {static_cast<double>(image.width), static_cast<double>(image.height)},
    }};
    const Point first = Transform(matrix, corners[0]);
    Bounds bounds{first.x, first.y, first.x, first.y};
    for (std::size_t index = 1; index < corners.size(); ++index) {
        const Point point = Transform(matrix, corners[index]);
        bounds.minimum_x = std::min(bounds.minimum_x, point.x);
        bounds.minimum_y = std::min(bounds.minimum_y, point.y);
        bounds.maximum_x = std::max(bounds.maximum_x, point.x);
        bounds.maximum_y = std::max(bounds.maximum_y, point.y);
    }
    return bounds;
}

bool SampleBilinear(const Image& image, const double x, const double y, std::array<double, 3>& pixel) {
    if (x < 0.0 || y < 0.0 || x > static_cast<double>(image.width - 1)
        || y > static_cast<double>(image.height - 1)) {
        return false;
    }
    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = std::min(x0 + 1, image.width - 1);
    const auto y1 = std::min(y0 + 1, image.height - 1);
    const double dx = x - x0;
    const double dy = y - y0;
    for (std::size_t channel = 0; channel < pixel.size(); ++channel) {
        const auto at = [&](const std::uint32_t sx, const std::uint32_t sy) {
            return static_cast<double>(image.bgr[(static_cast<std::size_t>(sy) * image.width + sx) * 3 + channel]);
        };
        pixel[channel] = (1.0 - dy) * ((1.0 - dx) * at(x0, y0) + dx * at(x1, y0))
            + dy * ((1.0 - dx) * at(x0, y1) + dx * at(x1, y1));
    }
    return true;
}

double FeatherWeight(
    const StitchLayout layout,
    const double x,
    const double y,
    const Bounds& a_bounds,
    const Bounds& b_bounds) {
    const double start = layout == StitchLayout::camera_a_left_camera_b_right
        ? std::max(a_bounds.minimum_x, b_bounds.minimum_x)
        : std::max(a_bounds.minimum_y, b_bounds.minimum_y);
    const double end = layout == StitchLayout::camera_a_left_camera_b_right
        ? std::min(a_bounds.maximum_x, b_bounds.maximum_x)
        : std::min(a_bounds.maximum_y, b_bounds.maximum_y);
    const double coordinate = layout == StitchLayout::camera_a_left_camera_b_right ? x : y;
    if (end <= start + kMatrixEpsilon) {
        return 0.5;
    }
    return std::clamp((coordinate - start) / (end - start), 0.0, 1.0);
}

void ValidateProfile(const FixedRigStitchProfile& profile) {
    if (profile.profile_id.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument("approved rig profile ID is required");
    }
    if (profile.trust.status != ProfileStatus::approved) {
        throw std::invalid_argument("rig profile must be approved");
    }
    if (profile.trust.schema_version != kSupportedProfileSchemaVersion) {
        throw std::invalid_argument("rig profile schema version is unsupported");
    }
    if (profile.trust.provenance.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument("rig profile provenance is required");
    }
    if (profile.trust.valid_until <= profile.trust.measured_at
        || profile.trust.assessed_at < profile.trust.measured_at
        || profile.trust.assessed_at >= profile.trust.valid_until) {
        throw std::invalid_argument("rig profile validity window is not trusted at assessment time");
    }
    if (profile.expected_input_width == 0 || profile.expected_input_height == 0) {
        throw std::invalid_argument("rig profile expected input dimensions are required");
    }
    (void)Invert(profile.camera_b_to_camera_a);
}

void ValidateCanonicalPath(const std::filesystem::path& path, const char* alias) {
    if (path.filename() != L"original.jpg") {
        throw std::invalid_argument(std::string(alias) + " input must be a canonical original.jpg");
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::invalid_argument(std::string(alias) + " canonical original does not exist");
    }
}

void EncodeJpegAtomic(
    IWICImagingFactory* factory,
    Image& image,
    const std::filesystem::path& partial,
    const std::filesystem::path& destination) {
    IWICStream* stream_raw = nullptr;
    CheckHresult(factory->CreateStream(&stream_raw), "JPEG output stream creation");
    ComPtr<IWICStream> stream(stream_raw);
    CheckHresult(stream->InitializeFromFilename(partial.c_str(), GENERIC_WRITE), "JPEG partial open");

    IWICBitmapEncoder* encoder_raw = nullptr;
    CheckHresult(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder_raw), "JPEG encoder creation");
    ComPtr<IWICBitmapEncoder> encoder(encoder_raw);
    CheckHresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "JPEG encoder initialization");

    IWICBitmapFrameEncode* frame_raw = nullptr;
    IPropertyBag2* properties_raw = nullptr;
    CheckHresult(encoder->CreateNewFrame(&frame_raw, &properties_raw), "JPEG output frame creation");
    ComPtr<IWICBitmapFrameEncode> frame(frame_raw);
    ComPtr<IPropertyBag2> properties(properties_raw);
    CheckHresult(frame->Initialize(properties.get()), "JPEG output frame initialization");
    CheckHresult(frame->SetSize(image.width, image.height), "JPEG output dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    CheckHresult(frame->SetPixelFormat(&format), "JPEG output pixel format");
    if (format != GUID_WICPixelFormat24bppBGR) {
        throw std::runtime_error("JPEG encoder rejected 24-bit BGR output");
    }
    const auto stride = image.width * 3U;
    CheckHresult(frame->WritePixels(image.height, stride, static_cast<UINT>(image.bgr.size()), image.bgr.data()),
        "JPEG output pixel write");
    CheckHresult(frame->Commit(), "JPEG output frame commit");
    CheckHresult(encoder->Commit(), "JPEG output commit");
    properties.reset();
    frame.reset();
    encoder.reset();
    stream.reset();

    std::error_code error;
    std::filesystem::rename(partial, destination, error);
    if (error) {
        throw std::runtime_error("atomic JPEG publish failed: " + error.message());
    }
}

class PartialFileGuard final {
public:
    explicit PartialFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~PartialFileGuard() {
        if (active_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    void Release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

std::filesystem::path NormalizedAbsolute(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

} // namespace

OfflineStitchResult StitchCanonicalPair(const OfflineStitchRequest& request) {
    ValidateProfile(request.profile);
    ValidateCanonicalPath(request.camera_a_original, "CAM-A");
    ValidateCanonicalPath(request.camera_b_original, "CAM-B");
    const auto camera_a_path = NormalizedAbsolute(request.camera_a_original);
    const auto camera_b_path = NormalizedAbsolute(request.camera_b_original);
    const auto job_path = NormalizedAbsolute(request.output_job_directory);
    if (std::filesystem::equivalent(camera_a_path, camera_b_path)) {
        throw std::invalid_argument("CAM-A and CAM-B canonical originals must be distinct files");
    }
    const auto resolved_job_path = std::filesystem::weakly_canonical(job_path);
    if (resolved_job_path == std::filesystem::canonical(camera_a_path.parent_path())
        || resolved_job_path == std::filesystem::canonical(camera_b_path.parent_path())) {
        throw std::invalid_argument("stitched output must use a separate job directory");
    }
    std::filesystem::create_directories(job_path);
    const auto destination = job_path / L"stitched.jpg";
    const auto partial = job_path / L"stitched.jpg.partial";
    if (std::filesystem::exists(destination) || std::filesystem::exists(partial)) {
        throw std::invalid_argument("output job already contains a stitched JPEG or partial");
    }

    ComApartment apartment;
    auto factory = CreateFactory();
    const Image camera_a = DecodeJpeg(factory.get(), camera_a_path);
    const Image camera_b = DecodeJpeg(factory.get(), camera_b_path);
    if (camera_a.width != request.profile.expected_input_width
        || camera_a.height != request.profile.expected_input_height
        || camera_b.width != request.profile.expected_input_width
        || camera_b.height != request.profile.expected_input_height) {
        throw std::invalid_argument("canonical JPEG dimensions do not match the approved rig profile");
    }

    const auto inverse_b = Invert(request.profile.camera_b_to_camera_a);
    const Bounds a_bounds{0.0, 0.0, static_cast<double>(camera_a.width), static_cast<double>(camera_a.height)};
    const Bounds b_bounds = TransformedBounds(camera_b, request.profile.camera_b_to_camera_a);
    const double minimum_x = std::floor(std::min(a_bounds.minimum_x, b_bounds.minimum_x));
    const double minimum_y = std::floor(std::min(a_bounds.minimum_y, b_bounds.minimum_y));
    const double maximum_x = std::ceil(std::max(a_bounds.maximum_x, b_bounds.maximum_x));
    const double maximum_y = std::ceil(std::max(a_bounds.maximum_y, b_bounds.maximum_y));
    const double canvas_width_value = maximum_x - minimum_x;
    const double canvas_height_value = maximum_y - minimum_y;
    if (canvas_width_value > std::numeric_limits<std::uint32_t>::max()
        || canvas_height_value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("fixed transform canvas exceeds supported dimensions");
    }
    const auto canvas_width = static_cast<std::uint32_t>(canvas_width_value);
    const auto canvas_height = static_cast<std::uint32_t>(canvas_height_value);
    (void)PixelCount(canvas_width, canvas_height);
    const auto horizontal_crop = static_cast<std::uint64_t>(request.profile.crop.left) + request.profile.crop.right;
    const auto vertical_crop = static_cast<std::uint64_t>(request.profile.crop.top) + request.profile.crop.bottom;
    if (horizontal_crop >= canvas_width || vertical_crop >= canvas_height) {
        throw std::invalid_argument("approved crop removes the complete stitched canvas");
    }

    Image output{
        canvas_width - static_cast<std::uint32_t>(horizontal_crop),
        canvas_height - static_cast<std::uint32_t>(vertical_crop),
        {},
    };
    output.bgr.resize(static_cast<std::size_t>(PixelCount(output.width, output.height) * 3));
    for (std::uint32_t output_y = 0; output_y < output.height; ++output_y) {
        const double global_y = minimum_y + request.profile.crop.top + output_y;
        for (std::uint32_t output_x = 0; output_x < output.width; ++output_x) {
            const double global_x = minimum_x + request.profile.crop.left + output_x;
            std::array<double, 3> pixel_a{};
            std::array<double, 3> pixel_b{};
            const bool has_a = SampleBilinear(camera_a, global_x, global_y, pixel_a);
            const Point source_b = Transform(inverse_b, {global_x, global_y});
            const bool has_b = SampleBilinear(camera_b, source_b.x, source_b.y, pixel_b);
            const double b_weight = has_a && has_b
                ? FeatherWeight(request.profile.layout, global_x, global_y, a_bounds, b_bounds)
                : (has_b ? 1.0 : 0.0);
            const auto offset = (static_cast<std::size_t>(output_y) * output.width + output_x) * 3;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const double value = (1.0 - b_weight) * pixel_a[channel] + b_weight * pixel_b[channel];
                output.bgr[offset + channel] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }

    PartialFileGuard partial_guard(partial);
    EncodeJpegAtomic(factory.get(), output, partial, destination);
    partial_guard.Release();
    return {destination, output.width, output.height, request.profile.profile_id};
}

void ExportStitchedJpeg(
    const std::filesystem::path& stitched_jpeg,
    const std::filesystem::path& destination_jpeg) {
    if (!std::filesystem::is_regular_file(stitched_jpeg)) {
        throw std::invalid_argument("completed stitched JPEG does not exist");
    }
    if (destination_jpeg.extension() != L".jpg") {
        throw std::invalid_argument("explicit export destination must use the .jpg extension");
    }
    if (!std::filesystem::is_directory(destination_jpeg.parent_path())) {
        throw std::invalid_argument("explicit export destination directory must already exist");
    }
    if (std::filesystem::exists(destination_jpeg)) {
        throw std::invalid_argument("explicit export never replaces an existing file");
    }
    const auto partial = destination_jpeg.parent_path() / (destination_jpeg.filename().wstring() + L".partial");
    if (std::filesystem::exists(partial)) {
        throw std::invalid_argument("explicit export partial already exists");
    }
    PartialFileGuard partial_guard(partial);
    std::filesystem::copy_file(stitched_jpeg, partial, std::filesystem::copy_options::none);
    std::error_code error;
    std::filesystem::rename(partial, destination_jpeg, error);
    if (error) {
        throw std::runtime_error("atomic explicit export failed: " + error.message());
    }
    partial_guard.Release();
}

} // namespace a0::m2
