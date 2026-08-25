#include "a0/m2/offline_stitcher.hpp"

#include "a0/m2/stitch_job_manifest.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
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

} // namespace a0::m2::detail

namespace a0::m2 {
namespace {

constexpr std::uint64_t kMaximumDecodedPixels = 200'000'000;
constexpr std::uint32_t kMaximumImageDimension = 32'768;
constexpr double kMatrixEpsilon = 1e-12;

using TestHook = void (*)();
std::atomic<TestHook> input_locks_held_hook{};
std::atomic<TestHook> before_encode_hook{};
std::atomic<TestHook> before_partial_flush_hook{};
std::atomic<TestHook> before_publish_rename_hook{};
std::atomic<detail::OfflineStitchFaultPoint> offline_stitch_fault{};
std::atomic<std::uint32_t> offline_stitch_fault_trigger_count{};

class InjectedOfflineStitchFault final : public std::runtime_error {
public:
    explicit InjectedOfflineStitchFault(const char* code)
        : std::runtime_error(std::string("m2_fault:") + code) {}
};

bool TriggerFault(const detail::OfflineStitchFaultPoint point) noexcept {
    if (offline_stitch_fault.load(std::memory_order_acquire) != point) return false;
    offline_stitch_fault_trigger_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ThrowIfFault(
    const detail::OfflineStitchFaultPoint point,
    const char* code) {
    if (TriggerFault(point)) throw InjectedOfflineStitchFault(code);
}

void InterruptIfFault(const detail::OfflineStitchFaultPoint point) noexcept {
    if (!TriggerFault(point)) return;
    (void)TerminateProcess(GetCurrentProcess(), 197);
    std::terminate();
}

void InvokeTestHook(const std::atomic<TestHook>& hook) {
    if (const auto callback = hook.load(std::memory_order_acquire); callback != nullptr) {
        callback();
    }
}

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

struct JpegSnapshot {
    std::vector<std::uint8_t> compressed;
    std::array<std::uint8_t, 32> sha256{};
    Image image;
};

// Declared pixel dimensions of a JPEG frame, without any decoded pixel data.
struct JpegDimensions {
    std::uint32_t width{};
    std::uint32_t height{};
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

class LockedReadFile final {
public:
    explicit LockedReadFile(const std::filesystem::path& path, const bool allow_rename = false) {
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ | (allow_rename ? DELETE : 0),
            FILE_SHARE_READ | (allow_rename ? FILE_SHARE_DELETE : 0),
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::invalid_argument("JPEG source cannot be locked for immutable read");
        }
    }

    ~LockedReadFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    LockedReadFile(const LockedReadFile&) = delete;
    LockedReadFile& operator=(const LockedReadFile&) = delete;

    [[nodiscard]] std::uint64_t Size() const {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size) || size.QuadPart < 0) {
            throw std::runtime_error("locked JPEG size inspection failed");
        }
        return static_cast<std::uint64_t>(size.QuadPart);
    }

    [[nodiscard]] std::vector<std::uint8_t> ReadAll() const {
        const auto size = Size();
        if (size == 0 || size > kMaximumCompressedJpegBytes) {
            throw std::invalid_argument("compressed JPEG byte size is empty or exceeds the 64 MiB limit");
        }
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(handle_, beginning, nullptr, FILE_BEGIN)) {
            throw std::runtime_error("locked JPEG seek failed before snapshot read");
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto request = static_cast<DWORD>(std::min<std::size_t>(remaining, 1024U * 1024U));
            DWORD read = 0;
            if (!ReadFile(handle_, bytes.data() + offset, request, &read, nullptr) || read == 0) {
                throw std::runtime_error("locked JPEG read failed before the declared size");
            }
            offset += read;
        }
        if (Size() != size) {
            throw std::runtime_error("locked JPEG size changed during snapshot read");
        }
        return bytes;
    }

    void RenameToWithoutReplace(const std::filesystem::path& destination) const {
        const auto absolute_destination = std::filesystem::absolute(destination).lexically_normal();
        const auto file_name = absolute_destination.native();
        const auto file_name_bytes = file_name.size() * sizeof(wchar_t);
        if (file_name_bytes > std::numeric_limits<DWORD>::max()) {
            throw std::invalid_argument("atomic JPEG publish destination is too long");
        }
        std::vector<std::uint8_t> buffer(
            sizeof(FILE_RENAME_INFO) + file_name_bytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(file_name_bytes);
        std::memcpy(rename->FileName, file_name.c_str(), file_name_bytes + sizeof(wchar_t));
        const bool renamed = SetFileInformationByHandle(
            handle_, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())) != FALSE;
        const DWORD rename_error = renamed ? ERROR_SUCCESS : GetLastError();
        if (!renamed) {
            throw std::runtime_error(
                "atomic JPEG publish failed without replacing an existing output (Win32 "
                + std::to_string(rename_error) + ")");
        }
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class Sha256Provider final {
public:
    Sha256Provider() {
        module_ = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_ == nullptr) {
            throw std::runtime_error("SHA-256 provider load failed");
        }
        open_algorithm_ = Load<OpenAlgorithm>("BCryptOpenAlgorithmProvider");
        hash_ = Load<Hash>("BCryptHash");
        close_algorithm_ = Load<CloseAlgorithm>("BCryptCloseAlgorithmProvider");
        if (!BCRYPT_SUCCESS(open_algorithm_(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw std::runtime_error("SHA-256 provider initialization failed");
        }
    }

    ~Sha256Provider() {
        if (algorithm_ != nullptr) {
            (void)close_algorithm_(algorithm_, 0);
        }
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    Sha256Provider(const Sha256Provider&) = delete;
    Sha256Provider& operator=(const Sha256Provider&) = delete;

    [[nodiscard]] std::array<std::uint8_t, 32> Compute(
        const std::vector<std::uint8_t>& bytes) const {
        if (bytes.size() > std::numeric_limits<ULONG>::max()) {
            throw std::invalid_argument("SHA-256 input exceeds the supported byte count");
        }
        std::array<std::uint8_t, 32> digest{};
        if (!BCRYPT_SUCCESS(hash_(
                algorithm_,
                nullptr,
                0,
                const_cast<PUCHAR>(bytes.data()),
                static_cast<ULONG>(bytes.size()),
                digest.data(),
                static_cast<ULONG>(digest.size())))) {
            throw std::runtime_error("SHA-256 calculation failed");
        }
        return digest;
    }

private:
    using OpenAlgorithm = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
    using Hash = NTSTATUS (WINAPI*)(
        BCRYPT_ALG_HANDLE, PUCHAR, ULONG, PUCHAR, ULONG, PUCHAR, ULONG);
    using CloseAlgorithm = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);

    template <typename Function>
    [[nodiscard]] Function Load(const char* name) {
        const auto address = GetProcAddress(module_, name);
        if (address == nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw std::runtime_error("SHA-256 provider entry point is unavailable");
        }
        return reinterpret_cast<Function>(address);
    }

    HMODULE module_{};
    BCRYPT_ALG_HANDLE algorithm_{};
    OpenAlgorithm open_algorithm_{};
    Hash hash_{};
    CloseAlgorithm close_algorithm_{};
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

// Opens the single frame of a JPEG byte stream after confirming SOI/EOI
// markers, a recognized JPEG container, and exactly one frame. Shared by the
// full pixel decode (DecodeJpegSnapshot) and the dimension-only validation
// (ValidateJpegDimensions) below, since both need the same frame handle up to
// this point and diverge only on whether pixel data is ever decoded.
ComPtr<IWICBitmapFrameDecode> OpenJpegFrame(
    IWICImagingFactory* factory,
    const std::vector<std::uint8_t>& bytes,
    const std::string& description,
    const bool require_terminal_eoi) {
    const std::array<std::uint8_t, 2> eoi{0xff, 0xd9};
    const auto eoi_position = bytes.size() >= 4
        ? std::find_end(bytes.begin() + 2, bytes.end(), eoi.begin(), eoi.end())
        : bytes.end();
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8
        || eoi_position == bytes.end()
        || (require_terminal_eoi && eoi_position + 2 != bytes.end())) {
        throw std::invalid_argument(description + " is not a complete JPEG byte stream");
    }

    IWICStream* stream_raw = nullptr;
    CheckHresult(factory->CreateStream(&stream_raw), description + " memory stream creation");
    ComPtr<IWICStream> stream(stream_raw);
    CheckHresult(stream->InitializeFromMemory(
        const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())),
        description + " memory stream initialization");

    IWICBitmapDecoder* decoder_raw = nullptr;
    CheckHresult(factory->CreateDecoderFromStream(
        stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder_raw),
        description + " decoder creation");
    ComPtr<IWICBitmapDecoder> decoder(decoder_raw);
    GUID container{};
    CheckHresult(decoder->GetContainerFormat(&container), description + " container inspection");
    if (container != GUID_ContainerFormatJpeg) {
        throw std::invalid_argument(description + " must be a JPEG container");
    }
    UINT frame_count = 0;
    CheckHresult(decoder->GetFrameCount(&frame_count), description + " frame count");
    if (frame_count != 1) {
        throw std::invalid_argument(description + " must contain exactly one frame");
    }

    IWICBitmapFrameDecode* frame_raw = nullptr;
    CheckHresult(decoder->GetFrame(0, &frame_raw), description + " frame decode");
    return ComPtr<IWICBitmapFrameDecode>(frame_raw);
}

Image DecodeJpegSnapshot(
    IWICImagingFactory* factory,
    const std::vector<std::uint8_t>& bytes,
    const std::string& description,
    const bool require_terminal_eoi) {
    const auto frame = OpenJpegFrame(factory, bytes, description, require_terminal_eoi);
    UINT width = 0;
    UINT height = 0;
    CheckHresult(frame->GetSize(&width, &height), description + " dimensions");
    const auto pixels = PixelCount(width, height);

    IWICFormatConverter* converter_raw = nullptr;
    CheckHresult(factory->CreateFormatConverter(&converter_raw), description + " pixel converter creation");
    ComPtr<IWICFormatConverter> converter(converter_raw);
    CheckHresult(converter->Initialize(
        frame.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom), description + " pixel conversion");
    Image image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(pixels * 3))};
    CheckHresult(converter->CopyPixels(
        nullptr, width * 3U, static_cast<UINT>(image.bgr.size()), image.bgr.data()),
        description + " complete pixel read");
    return image;
}

// GitHub Issue #99: confirms `bytes` is a syntactically complete, single-frame
// JPEG (SOI/EOI markers present, recognized container, one decodable frame
// header) and returns its declared dimensions, without ever decoding pixel
// data. Callers that only need the declared width/height -- not the decoded
// pixels -- use this instead of DecodeJpegSnapshot to skip the WIC format
// conversion and full-resolution CopyPixels, both of which are wasted work
// when the result is discarded immediately.
//
// What this does NOT check, relative to a full decode: entropy-coded scan
// data (the compressed pixel payload) is never decoded, so a JPEG whose frame
// header parses fine but whose Huffman-coded MCU data is corrupted will pass
// this check even though DecodeJpegSnapshot would fail on it during
// CopyPixels. Callers that need that stronger guarantee must keep using
// DecodeJpegSnapshot/ReadJpegSnapshot.
JpegDimensions ValidateJpegDimensions(
    IWICImagingFactory* factory,
    const std::vector<std::uint8_t>& bytes,
    const std::string& description,
    const bool require_terminal_eoi) {
    const auto frame = OpenJpegFrame(factory, bytes, description, require_terminal_eoi);
    UINT width = 0;
    UINT height = 0;
    CheckHresult(frame->GetSize(&width, &height), description + " dimensions");
    (void)PixelCount(width, height);
    return {width, height};
}

JpegSnapshot ReadJpegSnapshot(
    IWICImagingFactory* factory,
    const LockedReadFile& locked_file,
    const std::string& description,
    const bool require_terminal_eoi) {
    JpegSnapshot snapshot;
    snapshot.compressed = locked_file.ReadAll();
    Sha256Provider sha256;
    snapshot.sha256 = sha256.Compute(snapshot.compressed);
    snapshot.image = DecodeJpegSnapshot(
        factory, snapshot.compressed, description, require_terminal_eoi);
    return snapshot;
}

// GitHub Issue #99: lightweight sibling of ReadJpegSnapshot for callers that
// need the compressed bytes, their SHA-256, and (optionally) confirmation of
// the declared JPEG dimensions, but never touch decoded pixel data. The
// returned snapshot's `.image` field is left default-constructed (empty) --
// callers of this function must not read it. See ValidateJpegDimensions for
// exactly what validation is and is not performed.
JpegSnapshot ReadJpegSnapshotStructureOnly(
    IWICImagingFactory* factory,
    const LockedReadFile& locked_file,
    const std::string& description,
    const bool require_terminal_eoi,
    JpegDimensions* out_dimensions = nullptr) {
    JpegSnapshot snapshot;
    snapshot.compressed = locked_file.ReadAll();
    Sha256Provider sha256;
    snapshot.sha256 = sha256.Compute(snapshot.compressed);
    const auto dimensions = ValidateJpegDimensions(
        factory, snapshot.compressed, description, require_terminal_eoi);
    if (out_dimensions != nullptr) {
        *out_dimensions = dimensions;
    }
    return snapshot;
}

// GitHub Issue #102 (item 1, deliberately left unchanged): this re-reads and
// re-hashes the file the caller already read via ReadJpegSnapshot /
// ReadJpegSnapshotStructureOnly, even though the LockedReadFile handle in use
// for the whole operation excludes concurrent writers (see the FILE_SHARE_READ
// comments at each call site) and ReadAll() already re-checks the file size
// did not change during its own read. This looks redundant, but it is kept as
// an explicit, independent proof that the exact bytes snapshotted earlier are
// still the exact bytes on disk immediately before they are trusted (hashed
// into the manifest, or renamed into place) -- a second, cheap check against
// any future change to LockedReadFile/ReadAll that might weaken that
// guarantee. Not changed by the #99/#102 performance work in this file: the
// evidence needed to prove it is safe to remove was not conclusive enough to
// risk it in a safety-critical (safety:S1) path.
void ValidateSnapshotHash(
    const JpegSnapshot& snapshot,
    const LockedReadFile& locked_file,
    const std::string& description) {
    const auto reread = locked_file.ReadAll();
    Sha256Provider sha256;
    if (sha256.Compute(reread) != snapshot.sha256 || reread != snapshot.compressed) {
        throw std::runtime_error(description + " SHA-256 or bytes changed on locked reread");
    }
}

void WriteBytesToNewFile(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes) {
    HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("explicit export partial creation failed");
    }
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto request = static_cast<DWORD>(std::min<std::size_t>(remaining, 1024U * 1024U));
            DWORD written = 0;
            if (!WriteFile(handle, bytes.data() + offset, request, &written, nullptr) || written == 0) {
                throw std::runtime_error("explicit export partial write failed");
            }
            offset += written;
        }
        if (!FlushFileBuffers(handle)) {
            throw std::runtime_error("explicit export partial flush failed");
        }
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
    if (!CloseHandle(handle)) {
        throw std::runtime_error("explicit export partial close failed");
    }
}

double TransformDenominator(const std::array<double, 9>& matrix, const Point point) {
    return matrix[6] * point.x + matrix[7] * point.y + matrix[8];
}

bool TryTransform(const std::array<double, 9>& matrix, const Point point, Point& transformed) {
    const double denominator = TransformDenominator(matrix, point);
    if (!std::isfinite(denominator) || std::abs(denominator) <= kMatrixEpsilon) {
        return false;
    }
    transformed = {
        (matrix[0] * point.x + matrix[1] * point.y + matrix[2]) / denominator,
        (matrix[3] * point.x + matrix[4] * point.y + matrix[5]) / denominator,
    };
    return std::isfinite(transformed.x) && std::isfinite(transformed.y);
}

Point Transform(const std::array<double, 9>& matrix, const Point point) {
    Point transformed{};
    if (!TryTransform(matrix, point, transformed)) {
        throw std::invalid_argument("fixed transform maps a coordinate to infinity");
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

void ValidateProjectiveDomain(
    const std::array<double, 9>& matrix,
    const std::uint32_t width,
    const std::uint32_t height) {
    const std::array<Point, 4> corners{{
        {0.0, 0.0},
        {static_cast<double>(width), 0.0},
        {0.0, static_cast<double>(height)},
        {static_cast<double>(width), static_cast<double>(height)},
    }};
    bool positive{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const double denominator = TransformDenominator(matrix, corners[index]);
        if (!std::isfinite(denominator) || std::abs(denominator) <= kMatrixEpsilon) {
            throw std::invalid_argument("fixed transform projective denominator crosses the input image");
        }
        const bool current_positive = denominator > 0.0;
        if (index == 0) {
            positive = current_positive;
        } else if (current_positive != positive) {
            throw std::invalid_argument("fixed transform projective denominator crosses the input image");
        }
    }
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
    // GitHub Issue #86: 被覆判定はキャンバス寸法・バウンディング(コーナーモデル:
    // 画像は [0,width)×[0,height) を占める)と揃える。従来は pixel-center モデルの
    // [0,width-1] のみを有効域とし、コーナーモデルで採番された縁 1px 強(例: 純平行移動
    // +50.5px)が「被覆済みキャンバスなのにサンプル不可」となって uncovered pixel 例外で
    // stitch が必ず失敗していた。整数座標では x>width-1 と x>=width は同値なので既存の
    // 整数フィクスチャは不変で、非整数の (width-1, width) の縁のみ端列へクランプして
    // 被覆する(x1 = min(x0+1, width-1) の既存クランプがそのまま効く)。
    if (x < 0.0 || y < 0.0 || x >= static_cast<double>(image.width)
        || y >= static_cast<double>(image.height)) {
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

std::string ToLowerHex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        hex.push_back(digits[(byte >> 4U) & 0x0FU]);
        hex.push_back(digits[byte & 0x0FU]);
    }
    return hex;
}

// Hashes the profile as it was actually applied, field by field, rather than
// trusting a hash the caller supplies alongside the values. A caller that sent
// one profile and a hash of another would otherwise produce a manifest that
// records a transform the output was not produced with, and nothing downstream
// could notice.
std::string ProfileFingerprint(const FixedRigStitchProfile& profile) {
    std::ostringstream canonical;
    canonical << "profileId=" << profile.profile_id
              << ";schemaVersion=" << profile.trust.schema_version
              << ";status=" << (profile.trust.status == ProfileStatus::approved ? "approved" : "draft")
              << ";provenance=" << profile.trust.provenance
              << ";measuredAt=" << profile.trust.measured_at.time_since_epoch().count()
              << ";validUntil=" << profile.trust.valid_until.time_since_epoch().count()
              << ";assessedAt=" << profile.trust.assessed_at.time_since_epoch().count()
              << ";width=" << profile.expected_input_width
              << ";height=" << profile.expected_input_height
              << ";layout="
              << (profile.layout == StitchLayout::camera_a_left_camera_b_right
                      ? "camera-a-left-camera-b-right"
                      : "camera-a-top-camera-b-bottom")
              << ";crop=" << profile.crop.left << ',' << profile.crop.top << ','
              << profile.crop.right << ',' << profile.crop.bottom << ";matrix=";
    for (std::size_t index = 0; index < profile.camera_b_to_camera_a.size(); ++index) {
        if (index > 0) canonical << ',';
        // Round-trippable spelling: a shortened one would let two different
        // transforms fingerprint identically.
        canonical << std::setprecision(17) << profile.camera_b_to_camera_a[index];
    }
    const auto text = canonical.str();
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    Sha256Provider sha256;
    return ToLowerHex(sha256.Compute(bytes));
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
    ValidateProjectiveDomain(
        profile.camera_b_to_camera_a,
        profile.expected_input_width,
        profile.expected_input_height);
}

void ValidateCanonicalPath(const std::filesystem::path& path, const char* alias) {
    if (path.filename() != L"original.jpg") {
        throw std::invalid_argument(std::string(alias) + " input must be a canonical original.jpg");
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::invalid_argument(std::string(alias) + " canonical original does not exist");
    }
}

void EncodeJpegPartial(
    IWICImagingFactory* factory,
    Image& image,
    const std::filesystem::path& partial) {
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
}

void TruncateGeneratedPartialForFault(
    const std::filesystem::path& partial,
    const bool disk_full) {
    HANDLE handle = CreateFileW(
        partial.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw InjectedOfflineStitchFault(disk_full ? "disk-full" : "partial-short-write");
    }
    LARGE_INTEGER size{};
    bool truncated = GetFileSizeEx(handle, &size) != FALSE && size.QuadPart > 1;
    if (truncated) {
        LARGE_INTEGER retained{};
        retained.QuadPart = disk_full ? 1 : size.QuadPart / 2;
        truncated = SetFilePointerEx(handle, retained, nullptr, FILE_BEGIN) != FALSE
            && SetEndOfFile(handle) != FALSE;
    }
    const bool closed = CloseHandle(handle) != FALSE;
    if (!truncated || !closed) {
        throw InjectedOfflineStitchFault(disk_full ? "disk-full" : "partial-short-write");
    }
    throw InjectedOfflineStitchFault(disk_full ? "disk-full" : "partial-short-write");
}

void FlushGeneratedPartial(const std::filesystem::path& partial) {
    HANDLE handle = CreateFileW(
        partial.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("generated JPEG partial flush open failed");
    }
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    if (!flushed || !closed) {
        throw std::runtime_error("generated JPEG partial flush failed");
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

namespace detail {

void SetInputLocksHeldTestHook(void (*hook)()) noexcept {
    input_locks_held_hook.store(hook, std::memory_order_release);
}

void SetBeforeEncodeTestHook(void (*hook)()) noexcept {
    before_encode_hook.store(hook, std::memory_order_release);
}

void SetBeforePartialFlushTestHook(void (*hook)()) noexcept {
    before_partial_flush_hook.store(hook, std::memory_order_release);
}

void SetBeforePublishRenameTestHook(void (*hook)()) noexcept {
    before_publish_rename_hook.store(hook, std::memory_order_release);
}

void SetOfflineStitchFaultForTest(const OfflineStitchFaultPoint point) noexcept {
    const auto value = static_cast<std::uint32_t>(point);
    const auto bounded = value <= static_cast<std::uint32_t>(OfflineStitchFaultPoint::interrupt_after_publish)
        ? point
        : OfflineStitchFaultPoint::none;
    offline_stitch_fault_trigger_count.store(0, std::memory_order_relaxed);
    offline_stitch_fault.store(bounded, std::memory_order_release);
}

std::uint32_t GetOfflineStitchFaultTriggerCountForTest() noexcept {
    return offline_stitch_fault_trigger_count.load(std::memory_order_acquire);
}

// GitHub Issue #99: this only ever needed the partial's declared dimensions
// (to confirm they match the stitched result) and its bytes/hash, never the
// decoded pixels, so it validates via ReadJpegSnapshotStructureOnly instead of
// a full pixel decode. See ValidateJpegDimensions for exactly what that does
// and does not check.
//
// GitHub Issue #102 (item 2): returns the SHA-256 it already computed here so
// the caller can record it in the manifest without re-hashing the published
// file a second time.
StitchJobSha256Hex PublishValidatedGeneratedJpeg(
    const std::filesystem::path& partial,
    const std::filesystem::path& destination,
    const std::uint32_t expected_width,
    const std::uint32_t expected_height) {
    // Keep the partial immutable while it is snapshotted, validated, hashed,
    // and renamed. FILE_SHARE_DELETE permits this process's atomic rename
    // only; writers remain excluded and a competing rename/delete makes ours
    // fail.
    LockedReadFile locked_partial(partial, true);
    ComApartment apartment;
    auto factory = CreateFactory();
    JpegDimensions dimensions{};
    const auto snapshot = ReadJpegSnapshotStructureOnly(
        factory.get(), locked_partial, "generated JPEG partial", true, &dimensions);
    if (dimensions.width != expected_width || dimensions.height != expected_height) {
        throw std::invalid_argument("generated JPEG partial dimensions do not match the stitched result");
    }
    ValidateSnapshotHash(snapshot, locked_partial, "generated JPEG partial");
    InterruptIfFault(OfflineStitchFaultPoint::interrupt_before_publish);
    ThrowIfFault(OfflineStitchFaultPoint::publish_failure, "publish-failed");
    InvokeTestHook(before_publish_rename_hook);
    locked_partial.RenameToWithoutReplace(destination);
    return ToLowerHex(snapshot.sha256);
}

} // namespace detail

OfflineStitchResult StitchCanonicalPair(const OfflineStitchRequest& request) {
    ValidateProfile(request.profile);
    // Refused up front, before any file is created. A job that cannot be
    // recorded must not leave a stitched output behind for someone to later
    // mistake for a completed one.
    if (request.stitch_job_id.empty() || request.capture_transaction_id.empty()
        || request.completed_at_utc.empty()) {
        throw std::invalid_argument(
            "a StitchJob ID, CaptureTransaction ID and completion time are required to record the result");
    }
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
    const auto destination = job_path / L"stitched.jpg";
    const auto partial = job_path / L"stitched.jpg.partial";
    if (std::filesystem::exists(job_path)) {
        throw std::invalid_argument("output job is already reserved or contains an unknown result");
    }

    ComApartment apartment;
    auto factory = CreateFactory();
    // Acquire both handles before snapshotting either input and keep them alive
    // through publish. FILE_SHARE_READ excludes concurrent source write,
    // replacement, and deletion for the complete operation.
    LockedReadFile locked_camera_a(camera_a_path);
    LockedReadFile locked_camera_b(camera_b_path);
    InvokeTestHook(input_locks_held_hook);
    const auto camera_a_snapshot = ReadJpegSnapshot(
        factory.get(), locked_camera_a, "CAM-A canonical JPEG", false);
    const auto camera_b_snapshot = ReadJpegSnapshot(
        factory.get(), locked_camera_b, "CAM-B canonical JPEG", false);
    ValidateSnapshotHash(camera_a_snapshot, locked_camera_a, "CAM-A canonical JPEG");
    ValidateSnapshotHash(camera_b_snapshot, locked_camera_b, "CAM-B canonical JPEG");
    const Image& camera_a = camera_a_snapshot.image;
    const Image& camera_b = camera_b_snapshot.image;
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

    // GitHub Issue #102 (item 3): b_bounds is the axis-aligned bounding box, in
    // the same global/output coordinate space as global_x/global_y below, of
    // CAM-B's rectangle mapped forward through the approved fixed transform
    // (TransformedBounds already computed it above for canvas sizing). Because
    // that forward transform is a fixed projective map validated by
    // ValidateProjectiveDomain to not cross its zero-denominator line inside
    // the CAM-B rectangle, the transform is a homeomorphism there and the
    // image of the rectangle is exactly the convex quadrilateral spanned by
    // its four transformed corners -- so b_bounds, the AABB of those corners,
    // is already a true, non-lossy superset of every global coordinate CAM-B
    // can possibly cover. A global point strictly outside b_bounds can never
    // produce a has_b=true, so skipping the inverse transform and bilinear
    // sample for such points cannot change any output pixel; it only skips
    // work that was always going to end in has_b=false.
    //
    // The padding below is not required by that geometric argument, but is
    // added anyway as a second, independent safety margin against floating-
    // point drift: b_bounds is computed via the forward matrix, while the
    // per-pixel skip test below is compared against values ultimately used
    // with the separately-computed inverse matrix (inverse_b), so the two are
    // not guaranteed to agree to the last bit right at the boundary. Rounding
    // the box outward to whole pixels and then padding by an extra
    // kCameraBSkipSafetyMarginPixels on every side keeps the skip test
    // conservative: on any doubt near the edge, this does not skip, and the
    // pixel falls through to the exact same TryTransform+SampleBilinear path
    // used before this change, producing an identical result.
    constexpr double kCameraBSkipSafetyMarginPixels = 2.0;
    const double b_skip_minimum_x = std::floor(b_bounds.minimum_x) - kCameraBSkipSafetyMarginPixels;
    const double b_skip_minimum_y = std::floor(b_bounds.minimum_y) - kCameraBSkipSafetyMarginPixels;
    const double b_skip_maximum_x = std::ceil(b_bounds.maximum_x) + kCameraBSkipSafetyMarginPixels;
    const double b_skip_maximum_y = std::ceil(b_bounds.maximum_y) + kCameraBSkipSafetyMarginPixels;

    Image output{
        canvas_width - static_cast<std::uint32_t>(horizontal_crop),
        canvas_height - static_cast<std::uint32_t>(vertical_crop),
        {},
    };
    output.bgr.resize(static_cast<std::size_t>(PixelCount(output.width, output.height) * 3));
    for (std::uint32_t output_y = 0; output_y < output.height; ++output_y) {
        const double global_y = minimum_y + request.profile.crop.top + output_y;
        const bool row_may_hit_camera_b = global_y >= b_skip_minimum_y && global_y <= b_skip_maximum_y;
        for (std::uint32_t output_x = 0; output_x < output.width; ++output_x) {
            const double global_x = minimum_x + request.profile.crop.left + output_x;
            std::array<double, 3> pixel_a{};
            std::array<double, 3> pixel_b{};
            const bool has_a = SampleBilinear(camera_a, global_x, global_y, pixel_a);
            const bool may_hit_camera_b = row_may_hit_camera_b
                && global_x >= b_skip_minimum_x && global_x <= b_skip_maximum_x;
            Point source_b{};
            const bool has_b = may_hit_camera_b
                && TryTransform(inverse_b, {global_x, global_y}, source_b)
                && SampleBilinear(camera_b, source_b.x, source_b.y, pixel_b);
            if (!has_a && !has_b) {
                throw std::invalid_argument("approved crop contains an uncovered output pixel");
            }
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

    std::error_code parent_error;
    std::filesystem::create_directories(job_path.parent_path(), parent_error);
    if (parent_error) {
        throw std::runtime_error("output job parent creation failed");
    }
    std::error_code reservation_error;
    const bool reserved = std::filesystem::create_directory(job_path, reservation_error);
    if (!reserved) {
        if (!reservation_error) {
            throw std::invalid_argument("output job is already reserved or contains an unknown result");
        }
        throw std::runtime_error("output job reservation failed");
    }

    PartialFileGuard partial_guard(partial);
    // GitHub Issue #102 (item 2): captured from PublishValidatedGeneratedJpeg,
    // which already computed this SHA-256 to verify the partial before
    // renaming it into place. Reused for the manifest below instead of
    // re-reading and re-hashing the published file a third time -- safe
    // because RenameToWithoutReplace renames through the same open handle
    // (SetFileInformationByHandle's FileRenameInfo), a metadata-only
    // operation that never touches file content, so the bytes hashed here are
    // byte-identical to the bytes at `destination` afterward.
    StitchJobSha256Hex published_sha256;
    try {
        InterruptIfFault(detail::OfflineStitchFaultPoint::interrupt_before_encode);
        ThrowIfFault(detail::OfflineStitchFaultPoint::encode_failure, "encode-failed");
        InvokeTestHook(before_encode_hook);
        EncodeJpegPartial(factory.get(), output, partial);
        InterruptIfFault(detail::OfflineStitchFaultPoint::interrupt_after_partial_write);
        if (TriggerFault(detail::OfflineStitchFaultPoint::partial_short_write)) {
            TruncateGeneratedPartialForFault(partial, false);
        }
        if (TriggerFault(detail::OfflineStitchFaultPoint::disk_full)) {
            TruncateGeneratedPartialForFault(partial, true);
        }
        ThrowIfFault(detail::OfflineStitchFaultPoint::partial_flush_failure, "partial-flush-failed");
        InvokeTestHook(before_partial_flush_hook);
        FlushGeneratedPartial(partial);
        InterruptIfFault(detail::OfflineStitchFaultPoint::interrupt_after_flush);
        published_sha256 = detail::PublishValidatedGeneratedJpeg(partial, destination, output.width, output.height);
        InterruptIfFault(detail::OfflineStitchFaultPoint::interrupt_after_publish);
        partial_guard.Release();
    } catch (...) {
        // Once the durable job reservation reaches encode/write/flush/verify/
        // publish, injected and real I/O failures use the same orphan policy.
        // Preserve the exact candidate left by the failed boundary; do not
        // retry or clean it automatically.
        partial_guard.Release();
        throw;
    }
    // Steps 4-6 of the commit protocol. Until the manifest is published and read
    // back, this job is not terminal-success no matter how complete the JPEG on
    // disk looks. A crash between the two leaves both artifacts and no claim.
    std::error_code published_size_error;
    const auto published_size = std::filesystem::file_size(destination, published_size_error);
    if (published_size_error || published_size == 0) {
        throw std::runtime_error("the published stitched JPEG could not be measured for the manifest");
    }

    StitchJobManifest manifest;
    manifest.stitch_job_id = request.stitch_job_id;
    manifest.capture_transaction_id = request.capture_transaction_id;
    manifest.inputs[0] = {"CAM-A", ToLowerHex(camera_a_snapshot.sha256), camera_a_snapshot.compressed.size()};
    manifest.inputs[1] = {"CAM-B", ToLowerHex(camera_b_snapshot.sha256), camera_b_snapshot.compressed.size()};
    manifest.rig_profile = {
        request.profile.profile_id,
        request.profile.trust.schema_version,
        ProfileFingerprint(request.profile),
    };
    manifest.engine = {
        std::string(kOfflineStitcherEngineId),
        std::string(kOfflineStitcherEngineVersion),
    };
    manifest.output = {
        "stitched.jpg",
        published_sha256,
        output.width,
        output.height,
        published_size,
    };
    manifest.completed_at_utc = request.completed_at_utc;
    PublishAndVerifyStitchJobManifest(job_path, manifest);

    return {
        destination,
        output.width,
        output.height,
        request.profile.profile_id,
        job_path / std::filesystem::path(kStitchJobManifestFileName),
        request.stitch_job_id,
    };
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

    // Keep this handle alive through validation, partial verification, and
    // rename. FILE_SHARE_READ prevents source write, replacement, or deletion.
    LockedReadFile locked_source(stitched_jpeg);
    ComApartment apartment;
    auto factory = CreateFactory();
    // GitHub Issue #99: an explicit export never reads `.image` -- it copies
    // the compressed bytes verbatim and only needs confirmation that both the
    // source and the partial are complete, well-formed JPEGs (and, via the
    // hash/byte comparisons below, that they match each other). Before this
    // change, ReadJpegSnapshot's full pixel decode ran here only to be
    // discarded; ReadJpegSnapshotStructureOnly performs the equivalent
    // structural validation without it. See ValidateJpegDimensions for
    // exactly what that does and does not check.
    const auto source_snapshot = ReadJpegSnapshotStructureOnly(factory.get(), locked_source, "export source", true);
    ValidateSnapshotHash(source_snapshot, locked_source, "export source");

    PartialFileGuard partial_guard(partial);
    WriteBytesToNewFile(partial, source_snapshot.compressed);
    LockedReadFile locked_partial(partial, true);
    const auto partial_snapshot = ReadJpegSnapshotStructureOnly(factory.get(), locked_partial, "export partial", true);
    ValidateSnapshotHash(partial_snapshot, locked_partial, "export partial");
    if (partial_snapshot.sha256 != source_snapshot.sha256
        || partial_snapshot.compressed != source_snapshot.compressed) {
        throw std::runtime_error("explicit export partial reread is not byte-identical");
    }
    InvokeTestHook(before_publish_rename_hook);
    locked_partial.RenameToWithoutReplace(destination_jpeg);
    partial_guard.Release();
}

} // namespace a0::m2
