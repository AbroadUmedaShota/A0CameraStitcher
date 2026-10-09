#include "a0/m2/offline_stitcher.hpp"

#include "a0/m2/render.hpp"
#include "a0/m2/rig_profile_v2.hpp"
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
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
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
// was computed over, so a caller building a manifest record can source both
// `sha256` and `encoded_size_bytes` from the same validated byte string
// instead of pairing an in-memory (pre-rename) hash with a fresh (post-rename)
// filesystem stat of a different observation.
struct PublishedGeneratedJpeg {
    StitchJobSha256Hex sha256;
    std::uint64_t encoded_size_bytes{};
};

} // namespace a0::m2::detail

namespace a0::m2 {
namespace {

using render::Invert;
using render::PixelCount;
using render::ValidateProjectiveDomain;

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

using Image = render::BgrImage;

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

struct EncodedJpegIdentity {
    BY_HANDLE_FILE_INFORMATION file{};
    std::array<std::uint8_t, 32> sha256{};
    std::uint64_t size{};
};

bool SameFileIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber
        && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

class LockedReadFile final {
public:
    explicit LockedReadFile(const std::filesystem::path& path, const bool allow_rename = false,
        const bool reject_reparse = false) {
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ | (allow_rename ? DELETE : 0),
            FILE_SHARE_READ | (allow_rename ? FILE_SHARE_DELETE : 0),
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN
                | (reject_reparse ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::invalid_argument("JPEG source cannot be locked for immutable read");
        }
        if (reject_reparse) {
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(handle_, &information)
                || (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
                throw std::invalid_argument("v2 source handle is not an ordinary immutable file");
            }
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

    bool SameFileAs(const LockedReadFile& other) const {
        BY_HANDLE_FILE_INFORMATION a{}, b{};
        if (!GetFileInformationByHandle(handle_, &a) || !GetFileInformationByHandle(other.handle_, &b))
            throw std::runtime_error("v2 immutable source identity inspection failed");
        return SameFileIdentity(a, b);
    }

    void ValidateIdentity(const EncodedJpegIdentity& expected) const {
        BY_HANDLE_FILE_INFORMATION actual{};
        if (!GetFileInformationByHandle(handle_, &actual) || !SameFileIdentity(actual, expected.file))
            throw std::invalid_argument("v2 generated candidate file identity changed after encoding");
    }

    [[nodiscard]] std::vector<std::uint8_t> ReadAll(
        const std::uint64_t maximum = kMaximumCompressedJpegBytes) const {
        const auto size = Size();
        if (size == 0 || size > maximum) {
            throw std::invalid_argument(maximum == kMaximumCompressedJpegBytes
                ? "compressed JPEG byte size is empty or exceeds the 64 MiB limit"
                : "locked profile byte size is empty or exceeds the 256 KiB limit");
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

// A v2 encoder owns a CREATE_NEW handle throughout encoding and metadata
// stamping. It cannot replace a pre-existing candidate or patch a substituted
// path; WIC writes and the JFIF correction address this exact same object.
class NewJpegStream final : public IStream {
public:
    explicit NewJpegStream(const std::filesystem::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("v2 JPEG partial exclusive creation failed");
    }
    ~NewJpegStream() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id == IID_IUnknown || id == IID_ISequentialStream || id == IID_IStream) {
            *result = static_cast<IStream*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --references_; if (!count) delete this; return count;
    }
    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG count, ULONG* read) override {
        DWORD actual{};
        if (!ReadFile(handle_, buffer, count, &actual, nullptr)) return STG_E_READFAULT;
        if (read) *read = actual;
        return actual == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* buffer, ULONG count, ULONG* written) override {
        LARGE_INTEGER zero{}, position{};
        if (written) *written = 0;
        if (!SetFilePointerEx(handle_, zero, &position, FILE_CURRENT)
            || position.QuadPart < 0
            || static_cast<std::uint64_t>(position.QuadPart) + count > kMaximumCompressedJpegBytes)
            return STG_E_MEDIUMFULL;
        DWORD actual{};
        if (!WriteFile(handle_, buffer, count, &actual, nullptr)) return STG_E_WRITEFAULT;
        if (written) *written = actual;
        return actual == count ? S_OK : STG_E_WRITEFAULT;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER distance, DWORD origin, ULARGE_INTEGER* result) override {
        if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER position{};
        if (!SetFilePointerEx(handle_, distance, &position, origin)) return STG_E_SEEKERROR;
        if (result) result->QuadPart = static_cast<ULONGLONG>(position.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        if (size.QuadPart > kMaximumCompressedJpegBytes) return STG_E_MEDIUMFULL;
        LARGE_INTEGER zero{}, previous{}, end{};
        end.QuadPart = static_cast<LONGLONG>(size.QuadPart);
        if (!SetFilePointerEx(handle_, zero, &previous, FILE_CURRENT)
            || !SetFilePointerEx(handle_, end, nullptr, FILE_BEGIN)
            || !SetEndOfFile(handle_)
            || !SetFilePointerEx(handle_, previous, nullptr, FILE_BEGIN)) return STG_E_WRITEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; } // shared durable flush follows
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* result, DWORD) override {
        if (!result) return E_POINTER;
        *result = {}; LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size) || size.QuadPart < 0) return STG_E_READFAULT;
        result->type = STGTY_STREAM; result->cbSize.QuadPart = static_cast<ULONGLONG>(size.QuadPart);
        result->grfMode = STGM_READWRITE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
    std::vector<std::uint8_t> Snapshot() {
        STATSTG information{}; CheckHresult(Stat(&information, STATFLAG_NONAME), "v2 partial size");
        if (!information.cbSize.QuadPart || information.cbSize.QuadPart > kMaximumCompressedJpegBytes)
            throw std::runtime_error("v2 generated JPEG exceeds its byte bound");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(information.cbSize.QuadPart));
        LARGE_INTEGER start{}; CheckHresult(Seek(start, STREAM_SEEK_SET, nullptr), "v2 partial rewind");
        ULONG read{}; CheckHresult(Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "v2 partial snapshot");
        if (read != bytes.size()) throw std::runtime_error("v2 partial snapshot was short");
        return bytes;
    }
    void ReplaceOwnedBytes(const std::vector<std::uint8_t>& bytes) {
        LARGE_INTEGER start{}; CheckHresult(Seek(start, STREAM_SEEK_SET, nullptr), "v2 metadata rewind");
        ULONG written{}; CheckHresult(Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written), "v2 metadata write");
        if (written != bytes.size()) throw std::runtime_error("v2 metadata write was short");
        ULARGE_INTEGER size{}; size.QuadPart = bytes.size();
        CheckHresult(SetSize(size), "v2 metadata final length");
    }
    EncodedJpegIdentity Identity(const std::vector<std::uint8_t>& verified) const {
        EncodedJpegIdentity identity;
        if (!GetFileInformationByHandle(handle_, &identity.file))
            throw std::runtime_error("v2 encoded candidate identity inspection failed");
        Sha256Provider provider; identity.sha256 = provider.Compute(verified); identity.size = verified.size();
        return identity;
    }
private:
    std::atomic<ULONG> references_{1};
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct JpegMetadataSegment { std::size_t offset, size; bool jfif; };
std::vector<JpegMetadataSegment> JpegMetadataSegments(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8)
        throw std::invalid_argument("v2 JPEG metadata has no SOI");
    std::vector<JpegMetadataSegment> segments;
    std::size_t position = 2;
    while (position < bytes.size()) {
        const auto beginning = position;
        if (bytes[position++] != 0xff) throw std::invalid_argument("v2 JPEG metadata marker is invalid");
        while (position < bytes.size() && bytes[position] == 0xff) ++position;
        if (position >= bytes.size()) throw std::invalid_argument("v2 JPEG metadata marker is truncated");
        const auto marker = bytes[position++];
        if (marker == 0xda || marker == 0xd9) return segments;
        if (marker == 0x00 || marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            throw std::invalid_argument("v2 JPEG contains an invalid header marker");
        if (position + 2 > bytes.size()) throw std::invalid_argument("v2 JPEG metadata length is truncated");
        const auto length = (static_cast<std::size_t>(bytes[position]) << 8U) | bytes[position + 1];
        if (length < 2 || length > bytes.size() - position)
            throw std::invalid_argument("v2 JPEG metadata segment is truncated");
        const auto payload = position + 2;
        if (marker == 0xe1 && length >= 8 && std::memcmp(bytes.data() + payload, "Exif\0\0", 6) == 0)
            throw std::invalid_argument("v2 generated JPEG must not carry EXIF orientation metadata");
        const bool jfif = marker == 0xe0 && length >= 7 && std::memcmp(bytes.data() + payload, "JFIF\0", 5) == 0;
        segments.push_back({beginning, position + length - beginning, jfif});
        position += length;
    }
    throw std::invalid_argument("v2 JPEG has no scan header");
}

void ValidateJpegDpi(const std::vector<std::uint8_t>& bytes, const std::uint32_t dpi) {
    unsigned count = 0;
    for (const auto& segment : JpegMetadataSegments(bytes)) {
        if (!segment.jfif) continue;
        ++count;
        // Canonical APP0: marker(2), length(2), identifier(5), version(2), units,
        // Xdensity(2), Ydensity(2), thumbnail dimensions(2).
        const auto p = segment.offset;
        if (segment.size != 18 || bytes[p + 2] != 0 || bytes[p + 3] != 16
            || bytes[p + 11] != 1
            || ((static_cast<unsigned>(bytes[p + 12]) << 8U) | bytes[p + 13]) != dpi
            || ((static_cast<unsigned>(bytes[p + 14]) << 8U) | bytes[p + 15]) != dpi
            || bytes[p + 16] || bytes[p + 17])
            throw std::invalid_argument("v2 generated JPEG JFIF density does not match the approved raster");
    }
    if (count != 1) throw std::invalid_argument("v2 generated JPEG requires exactly one JFIF density record");
}

EncodedJpegIdentity EncodeJpegPartialV2(IWICImagingFactory* factory, Image& image,
    const std::filesystem::path& partial, const std::uint32_t dpi) {
    if (!dpi || dpi > 65535) throw std::invalid_argument("v2 JPEG DPI is outside JFIF density range");
    ComPtr<NewJpegStream> stream(new NewJpegStream(partial));
    IWICBitmapEncoder* encoder_raw{};
    CheckHresult(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder_raw), "v2 JPEG encoder creation");
    ComPtr<IWICBitmapEncoder> encoder(encoder_raw);
    CheckHresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "v2 JPEG encoder initialization");
    IWICBitmapFrameEncode* frame_raw{}; IPropertyBag2* properties_raw{};
    CheckHresult(encoder->CreateNewFrame(&frame_raw, &properties_raw), "v2 JPEG frame creation");
    ComPtr<IWICBitmapFrameEncode> frame(frame_raw); ComPtr<IPropertyBag2> properties(properties_raw);
    CheckHresult(frame->Initialize(properties.get()), "v2 JPEG frame initialization");
    CheckHresult(frame->SetSize(image.width, image.height), "v2 JPEG dimensions");
    CheckHresult(frame->SetResolution(dpi, dpi), "v2 JPEG resolution");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    CheckHresult(frame->SetPixelFormat(&format), "v2 JPEG pixel format");
    if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("v2 JPEG encoder rejected BGR");
    CheckHresult(frame->WritePixels(image.height, image.width * 3U,
        static_cast<UINT>(image.bgr.size()), image.bgr.data()), "v2 JPEG pixels");
    CheckHresult(frame->Commit(), "v2 JPEG frame commit");
    CheckHresult(encoder->Commit(), "v2 JPEG commit");
    frame.reset(); properties.reset(); encoder.reset();
    const auto original = stream->Snapshot();
    const auto segments = JpegMetadataSegments(original);
    std::vector<std::uint8_t> corrected{0xff, 0xd8, 0xff, 0xe0, 0, 16,
        'J', 'F', 'I', 'F', 0, 1, 2, 1,
        static_cast<std::uint8_t>(dpi >> 8U), static_cast<std::uint8_t>(dpi),
        static_cast<std::uint8_t>(dpi >> 8U), static_cast<std::uint8_t>(dpi), 0, 0};
    std::size_t cursor = 2;
    for (const auto& segment : segments) {
        if (!segment.jfif) continue;
        corrected.insert(corrected.end(), original.begin() + cursor, original.begin() + segment.offset);
        cursor = segment.offset + segment.size;
    }
    corrected.insert(corrected.end(), original.begin() + cursor, original.end());
    if (corrected.size() > kMaximumCompressedJpegBytes) throw std::runtime_error("v2 JPEG metadata exceeds byte bound");
    stream->ReplaceOwnedBytes(corrected);
    const auto verified = stream->Snapshot();
    if (verified != corrected) throw std::runtime_error("v2 JPEG metadata write verification failed");
    ValidateJpegDpi(verified, dpi);
    return stream->Identity(verified);
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

void FlushGeneratedPartial(const std::filesystem::path& partial, const bool reject_reparse = false,
    const EncodedJpegIdentity* expected = nullptr) {
    HANDLE handle = CreateFileW(
        partial.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | (reject_reparse ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("generated JPEG partial flush open failed");
    }
    if (reject_reparse) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            CloseHandle(handle);
            throw std::invalid_argument("v2 generated JPEG partial is redirected or nonregular");
        }
        if (expected && !SameFileIdentity(info, expected->file)) {
            CloseHandle(handle);
            throw std::invalid_argument("v2 generated JPEG partial identity changed before flush");
        }
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

// V2 paths are local absolute ordinary paths. Pin every existing ancestor
// without write/delete sharing so path inspection cannot be invalidated by a
// junction/rename while profile, inputs and the commit pipeline are active.
class V2PathPins final {
public:
    ~V2PathPins() { for (const auto handle : handles_) CloseHandle(handle); }
    void Pin(const std::filesystem::path& path, const bool leaf_is_directory = false) {
        const auto text = path.native();
        if (text.size() < 3 || text[1] != L':' || (text[2] != L'\\' && text[2] != L'/')
            || !((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'))
            || text.find(L':', 2) != std::wstring::npos
            || GetDriveTypeW(path.root_path().make_preferred().c_str()) != DRIVE_FIXED)
            throw std::invalid_argument("v2 paths require an ordinary local absolute fixed-drive path");
        for (const auto& component : path.relative_path()) {
            auto name = component.native();
            if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' '
                || name.find_first_of(L"<>\"|?*:") != std::wstring::npos
                || std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
                throw std::invalid_argument("v2 path component is ambiguous or redirected");
            const auto dot = name.find(L'.');
            name.resize(dot == std::wstring::npos ? name.size() : dot);
            for (auto& c : name) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
            if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL"
                || name == L"CONIN$" || name == L"CONOUT$" || name == L"CLOCK$"
                || (name.size() == 4 && (name.starts_with(L"COM") || name.starts_with(L"LPT"))
                    && ((name[3] >= L'1' && name[3] <= L'9')
                        || name[3] == L'\u00b9' || name[3] == L'\u00b2' || name[3] == L'\u00b3')))
                throw std::invalid_argument("v2 path contains a device component");
        }
        const auto leaf_attributes = GetFileAttributesW(path.c_str());
        if (leaf_attributes != INVALID_FILE_ATTRIBUTES && (leaf_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::invalid_argument("v2 path is redirected");
        auto directory = leaf_is_directory ? path : path.parent_path();
        std::vector<std::filesystem::path> ancestors;
        while (!directory.empty()) {
            ancestors.push_back(directory);
            const auto parent = directory.parent_path();
            if (directory == parent) break;
            directory = parent;
        }
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
            HANDLE handle = CreateFileW(it->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
                throw std::invalid_argument("v2 path ancestor cannot be pinned");
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info)
                || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                CloseHandle(handle);
                throw std::invalid_argument("v2 path ancestor is not an ordinary directory");
            }
            handles_.push_back(handle);
        }
    }
private:
    std::vector<HANDLE> handles_;
};

void ValidateV2Identity(const OfflineStitchV2Request& request) {
    const auto hex = [](std::string_view value, std::size_t size) {
        return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (!hex(request.stitch_job_id, 32) || !hex(request.capture_transaction_id, 32)
        || request.stitch_job_id == std::string(32, '0') || request.capture_transaction_id == std::string(32, '0')
        || !hex(request.expected_profile_sha256, 64))
        throw std::invalid_argument("v2 job identity or expected profile fingerprint is invalid");
    const auto& time = request.completed_at_utc;
    if (time.size() != 20 || time[4] != '-' || time[7] != '-' || time[10] != 'T'
        || time[13] != ':' || time[16] != ':' || time[19] != 'Z')
        throw std::invalid_argument("v2 completion time must be whole-second UTC");
    for (std::size_t i = 0; i < time.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19) continue;
        if (time[i] < '0' || time[i] > '9') throw std::invalid_argument("v2 completion time is invalid");
    }
    const auto number = [&](std::size_t start, std::size_t length) {
        unsigned n = 0; for (auto i = start; i < start + length; ++i) n = n * 10 + time[i] - '0'; return n;
    };
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(number(0, 4))},
        std::chrono::month{number(5, 2)}, std::chrono::day{number(8, 2)}};
    if (!number(0, 4) || !date.ok() || number(11, 2) > 23 || number(14, 2) > 59 || number(17, 2) > 59)
        throw std::invalid_argument("v2 completion time is not a valid Gregorian UTC instant");
    if (time < request.assessed_at_utc) throw std::invalid_argument("v2 completion time precedes assessment");
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
// file a second time. GitHub Issue #102 (item 3, follow-up): also returns the
// exact byte count that SHA-256 was computed over (see PublishedGeneratedJpeg),
// so the manifest's sha256 and encoded_size_bytes can be sourced from the same
// validated byte string instead of two different observations.
PublishedGeneratedJpeg PublishValidatedGeneratedJpeg(
    const std::filesystem::path& partial,
    const std::filesystem::path& destination,
    const std::uint32_t expected_width,
    const std::uint32_t expected_height,
    const std::optional<std::uint32_t> expected_dpi,
    const EncodedJpegIdentity* expected_candidate = nullptr) {
    // Keep the partial immutable while it is snapshotted, validated, hashed,
    // and renamed. FILE_SHARE_DELETE permits this process's atomic rename
    // only; writers remain excluded and a competing rename/delete makes ours
    // fail.
    LockedReadFile locked_partial(partial, true, expected_dpi.has_value());
    if (expected_candidate) locked_partial.ValidateIdentity(*expected_candidate);
    ComApartment apartment;
    auto factory = CreateFactory();
    JpegDimensions dimensions{};
    const auto snapshot = ReadJpegSnapshotStructureOnly(
        factory.get(), locked_partial, "generated JPEG partial", true, &dimensions);
    if (dimensions.width != expected_width || dimensions.height != expected_height) {
        throw std::invalid_argument("generated JPEG partial dimensions do not match the stitched result");
    }
    if (expected_dpi) ValidateJpegDpi(snapshot.compressed, *expected_dpi);
    if (expected_candidate && (snapshot.sha256 != expected_candidate->sha256
        || snapshot.compressed.size() != expected_candidate->size))
        throw std::invalid_argument("v2 generated candidate bytes changed after encoding");
    ValidateSnapshotHash(snapshot, locked_partial, "generated JPEG partial");
    InterruptIfFault(OfflineStitchFaultPoint::interrupt_before_publish);
    ThrowIfFault(OfflineStitchFaultPoint::publish_failure, "publish-failed");
    InvokeTestHook(before_publish_rename_hook);
    locked_partial.RenameToWithoutReplace(destination);
    return {ToLowerHex(snapshot.sha256), static_cast<std::uint64_t>(snapshot.compressed.size())};
}

PublishedGeneratedJpeg PublishValidatedGeneratedJpeg(
    const std::filesystem::path& partial, const std::filesystem::path& destination,
    const std::uint32_t expected_width, const std::uint32_t expected_height) {
    return PublishValidatedGeneratedJpeg(partial, destination, expected_width, expected_height, std::nullopt);
}

} // namespace detail

namespace {
OfflineStitchResult StitchCanonicalPairImpl(const OfflineStitchRequest& request,
    const render::DocumentRenderParameters* v2_parameters,
    const std::string& v2_fingerprint, const std::string& engine_version,
    V2PathPins* v2_pins, const std::function<void()>& verify_profile) {
    if (!v2_parameters) ValidateProfile(request.profile);
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
    LockedReadFile locked_camera_a(camera_a_path, false, v2_parameters != nullptr);
    LockedReadFile locked_camera_b(camera_b_path, false, v2_parameters != nullptr);
    if (v2_parameters && locked_camera_a.SameFileAs(locked_camera_b))
        throw std::invalid_argument("CAM-A and CAM-B canonical originals must be distinct locked files");
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
    // Pixel computation (inverse mapping, bilinear sampling, feather, crop) lives
    // in the I/O-free a0_m2_render library. Everything that follows this call is
    // publish and manifest work.
    const render::PairRenderParameters render_parameters{
        request.profile.camera_b_to_camera_a,
        request.profile.layout,
        request.profile.crop,
    };
    auto rendered = v2_parameters ? render::RenderDocumentPair(camera_a, camera_b, *v2_parameters)
                                 : render::RenderPair(camera_a, camera_b, render_parameters);
    render::BgrImage& output = rendered.image;
    const render::SeamNavigationCandidate& seam_navigation = rendered.seam_navigation;

    std::error_code parent_error;
    std::filesystem::create_directories(job_path.parent_path(), parent_error);
    if (parent_error) {
        throw std::runtime_error("output job parent creation failed");
    }
    if (v2_pins) v2_pins->Pin(job_path.parent_path(), true);
    std::error_code reservation_error;
    const bool reserved = std::filesystem::create_directory(job_path, reservation_error);
    if (!reserved) {
        if (!reservation_error) {
            throw std::invalid_argument("output job is already reserved or contains an unknown result");
        }
        throw std::runtime_error("output job reservation failed");
    }
    if (v2_pins) v2_pins->Pin(job_path, true);

    PartialFileGuard partial_guard(partial);
    // GitHub Issue #102 (item 2): captured from PublishValidatedGeneratedJpeg,
    // which already computed this SHA-256 (and the byte count it was computed
    // over, see PublishedGeneratedJpeg / item 3 below) to verify the partial
    // before renaming it into place. Reused for the manifest below instead of
    // re-reading and re-hashing the published file a third time -- safe
    // because RenameToWithoutReplace renames through the same open handle
    // (SetFileInformationByHandle's FileRenameInfo), a metadata-only
    // operation that never touches file content, so the bytes hashed here are
    // byte-identical to the bytes at `destination` afterward.
    detail::PublishedGeneratedJpeg published;
    std::optional<EncodedJpegIdentity> encoded_identity;
    try {
        InterruptIfFault(detail::OfflineStitchFaultPoint::interrupt_before_encode);
        ThrowIfFault(detail::OfflineStitchFaultPoint::encode_failure, "encode-failed");
        InvokeTestHook(before_encode_hook);
        if (v2_parameters) encoded_identity = EncodeJpegPartialV2(factory.get(), output, partial, v2_parameters->raster.dpi);
        else EncodeJpegPartial(factory.get(), output, partial);
        InterruptIfFault(detail::OfflineStitchFaultPoint::interrupt_after_partial_write);
        if (TriggerFault(detail::OfflineStitchFaultPoint::partial_short_write)) {
            TruncateGeneratedPartialForFault(partial, false);
        }
        if (TriggerFault(detail::OfflineStitchFaultPoint::disk_full)) {
            TruncateGeneratedPartialForFault(partial, true);
        }
        ThrowIfFault(detail::OfflineStitchFaultPoint::partial_flush_failure, "partial-flush-failed");
        InvokeTestHook(before_partial_flush_hook);
        FlushGeneratedPartial(partial, v2_parameters != nullptr, encoded_identity ? &*encoded_identity : nullptr);
        InterruptIfFault(detail::OfflineStitchFaultPoint::interrupt_after_flush);
        if (verify_profile) verify_profile();
        published = detail::PublishValidatedGeneratedJpeg(partial, destination, output.width, output.height,
            v2_parameters ? std::optional<std::uint32_t>{v2_parameters->raster.dpi} : std::nullopt,
            encoded_identity ? &*encoded_identity : nullptr);
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
    //
    // GitHub Issue #102 (item 3): the manifest's encoded_size_bytes below is
    // sourced from `published.encoded_size_bytes` (the pre-rename byte count
    // PublishValidatedGeneratedJpeg already validated and hashed), not from
    // this file_size(destination) call, so that it and the manifest's sha256
    // describe the exact same observed byte string rather than two different
    // reads. The file_size(destination) call itself is kept -- it is a fresh,
    // independent, post-rename observation from the filesystem (distinct from
    // "RenameToWithoutReplace did not throw") that the published artifact is
    // really there and non-empty at the expected path, in keeping with this
    // commit protocol's insistence on verifying state rather than trusting the
    // absence of an exception. Its result is now used only as a cross-check
    // against the pre-rename size, rather than as the manifest's size source.
    std::error_code published_size_error;
    const auto observed_destination_size = std::filesystem::file_size(destination, published_size_error);
    if (published_size_error || observed_destination_size == 0) {
        throw std::runtime_error("the published stitched JPEG could not be measured for the manifest");
    }
    if (observed_destination_size != published.encoded_size_bytes) {
        throw std::runtime_error(
            "the published stitched JPEG size does not match the bytes that were hashed before publish");
    }

    StitchJobManifest manifest;
    manifest.stitch_job_id = request.stitch_job_id;
    manifest.capture_transaction_id = request.capture_transaction_id;
    manifest.inputs[0] = {"CAM-A", ToLowerHex(camera_a_snapshot.sha256), camera_a_snapshot.compressed.size()};
    manifest.inputs[1] = {"CAM-B", ToLowerHex(camera_b_snapshot.sha256), camera_b_snapshot.compressed.size()};
    manifest.rig_profile = {
        request.profile.profile_id,
        request.profile.trust.schema_version,
        v2_parameters ? v2_fingerprint : ProfileFingerprint(request.profile),
    };
    manifest.engine = {
        std::string(kOfflineStitcherEngineId),
        engine_version,
    };
    manifest.output = {
        "stitched.jpg",
        published.sha256,
        output.width,
        output.height,
        published.encoded_size_bytes,
    };
    manifest.seam_navigation = StitchJobSeamNavigationRecord{
        seam_navigation.available,
        seam_navigation.x,
        seam_navigation.y,
    };
    manifest.completed_at_utc = request.completed_at_utc;
    if (verify_profile) verify_profile();
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

} // namespace

OfflineStitchResult StitchCanonicalPair(const OfflineStitchRequest& request) {
    return StitchCanonicalPairImpl(request, nullptr, {}, std::string(kOfflineStitcherEngineVersion), nullptr, {});
}

OfflineStitchResult StitchCanonicalPairV2(const OfflineStitchV2Request& request) {
    ValidateV2Identity(request);
    V2PathPins pins;
    pins.Pin(request.profile_path);
    pins.Pin(request.camera_a_original);
    pins.Pin(request.camera_b_original);
    pins.Pin(request.output_job_directory, true);
    // V2 does not create a missing parent tree: a new intermediate junction
    // could otherwise redirect recursive creation before the next pin. The
    // existing, pinned parent must already be present, as in the .NET entry.
    if (!std::filesystem::is_directory(request.output_job_directory.parent_path()))
        throw std::invalid_argument("v2 output job parent must already exist");
    LockedReadFile profile_file(request.profile_path, false, true);
    constexpr std::uint64_t maximum_profile_bytes = 256U * 1024U;
    const auto profile_bytes = profile_file.ReadAll(maximum_profile_bytes);
    const std::string text(profile_bytes.begin(), profile_bytes.end());
    const auto profile = RigProfileV2::Parse(text);
    const auto parameters = profile.ValidateApprovedForUse(request.assessed_at_utc, request.resampling);
    const auto fingerprint = profile.FingerprintSha256();
    if (fingerprint != request.expected_profile_sha256)
        throw std::invalid_argument("native approved profile fingerprint differs from the expected fingerprint");
    if (parameters.raster.dpi > 65535) throw std::invalid_argument("approved output DPI exceeds JFIF density range");
    const auto verify_profile = [&] {
        if (profile_file.ReadAll(maximum_profile_bytes) != profile_bytes)
            throw std::runtime_error("locked approved profile changed before completion");
    };
    // Only dimensions/identity feed the shared I/O pipeline; v2 pixels are
    // produced exclusively by its separately validated document parameters.
    const FixedRigStitchProfile identity{*profile.ProfileId(), {}, 7360, 4912, {}, parameters.layout, {}};
    const OfflineStitchRequest common{request.camera_a_original, request.camera_b_original,
        request.output_job_directory, identity, request.stitch_job_id,
        request.capture_transaction_id, request.completed_at_utc};
    auto applied = common;
    applied.profile.trust.schema_version = "2.0.0";
    const auto version = request.resampling == render::Resampling::bilinear
        ? kOfflineStitcherV2BilinearVersion : kOfflineStitcherV2BicubicVersion;
    // Reuse the manifest's exact field validation before reserving any job.
    // Placeholder input/output hashes only validate shape and are never saved.
    const std::string placeholder(64, '0');
    StitchJobManifest preflight;
    preflight.stitch_job_id = request.stitch_job_id;
    preflight.capture_transaction_id = request.capture_transaction_id;
    preflight.inputs = {{{"CAM-A", placeholder, 1}, {"CAM-B", placeholder, 1}}};
    preflight.rig_profile = {*profile.ProfileId(), "2.0.0", fingerprint};
    preflight.engine = {std::string(kOfflineStitcherEngineId), std::string(version)};
    preflight.output = {"stitched.jpg", placeholder, parameters.raster.width_pixels, parameters.raster.height_pixels, 1};
    preflight.seam_navigation = StitchJobSeamNavigationRecord{false, 0, 0};
    preflight.completed_at_utc = request.completed_at_utc;
    (void)SerializeStitchJobManifest(preflight);
    return StitchCanonicalPairImpl(applied, &parameters, fingerprint, std::string(version), &pins, verify_profile);
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
