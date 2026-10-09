// Independent evaluator I/O. This translation unit has no renderer, rig
// profile, stitcher, synthetic generator, or product metric dependency.
#include "a0/m2/stitch_evaluation.hpp"
#include "a0/common/protocol_json.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace evaluation = a0::m2::evaluation;
constexpr std::uint64_t kImageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kJsonBytes = 256ULL * 1024ULL;
constexpr std::uint64_t kPixels = 200'000'000ULL;
constexpr std::uint64_t kMaskBytes = kPixels + 128ULL;
constexpr std::uint64_t kReportBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::wstring_view kReportName = L"stitch-evaluation.report.json";
[[noreturn]] void Fail() { throw std::runtime_error("evaluation boundary refused"); }
void Hr(HRESULT result) { if (FAILED(result)) Fail(); }
template<class T> struct Release { void operator()(T* object) const noexcept { if (object) object->Release(); } };
template<class T> using Com = std::unique_ptr<T, Release<T>>;
class Handle final {
public:
    explicit Handle(HANDLE value) : value_(value) { if (value_ == INVALID_HANDLE_VALUE) Fail(); }
    ~Handle() { CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return value_; }
private: HANDLE value_;
};
BY_HANDLE_FILE_INFORMATION Information(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION result{};
    if (!GetFileInformationByHandle(handle, &result)) Fail();
    return result;
}
bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh
        && a.nFileIndexLow == b.nFileIndexLow;
}
void Regular(HANDLE handle) {
    if (GetFileType(handle) != FILE_TYPE_DISK || (Information(handle).dwFileAttributes
        & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) Fail();
}
std::uint64_t Size(HANDLE handle) {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(handle, &value) || value.QuadPart < 0) Fail();
    return static_cast<std::uint64_t>(value.QuadPart);
}
std::vector<std::uint8_t> Read(HANDLE handle, std::uint64_t maximum) {
    const auto count = Size(handle);
    if (!count || count > maximum || count > std::numeric_limits<std::size_t>::max()) Fail();
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) Fail();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
    std::size_t position = 0;
    while (position < bytes.size()) {
        DWORD actual{};
        const auto wanted = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - position, 1024U * 1024U));
        if (!ReadFile(handle, bytes.data() + position, wanted, &actual, nullptr) || !actual) Fail();
        position += actual;
    }
    if (Size(handle) != count) Fail();
    return bytes;
}
std::string Hash(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > std::numeric_limits<ULONG>::max()) Fail();
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) Fail();
    std::array<std::uint8_t, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) Fail();
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (auto byte : digest) { text += digits[byte >> 4U]; text += digits[byte & 15U]; }
    return text;
}
std::wstring FinalName(HANDLE handle) {
    const auto needed = GetFinalPathNameByHandleW(handle, nullptr, 0, VOLUME_NAME_GUID);
    if (!needed || needed > 32768) Fail();
    std::wstring value(needed, L'\0');
    const auto used = GetFinalPathNameByHandleW(handle, value.data(), needed, VOLUME_NAME_GUID);
    if (!used || used >= needed) Fail();
    value.resize(used);
    if (!value.starts_with(L"\\\\?\\Volume{")) Fail();
    while (!value.empty() && value.back() == L'\\') value.pop_back();
    return value;
}
bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool Ancestor(std::wstring_view a, std::wstring_view b) {
    return Equal(a, b) || (b.size() > a.size() && b[a.size()] == L'\\' && Equal(a, b.substr(0, a.size())));
}
void Separate(std::wstring_view a, std::wstring_view b) { if (Ancestor(a, b) || Ancestor(b, a)) Fail(); }

fs::path OrdinaryPath(const std::wstring& text) {
    const fs::path path(text);
    if (text.size() < 3 || text.size() > 32767 || text[1] != L':' || (text[2] != L'\\' && text[2] != L'/')
        || !((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'))
        || text.find(L':', 2) != std::wstring::npos
        || GetDriveTypeW(path.root_path().make_preferred().c_str()) != DRIVE_FIXED) Fail();
    for (const auto& component : path.relative_path()) {
        auto name = component.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' '
            || name.find_first_of(L"<>\"|?*:") != std::wstring::npos
            || std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; })) Fail();
        const auto dot = name.find(L'.');
        if (dot != std::wstring::npos) name.resize(dot);
        while (!name.empty() && name.back() == L' ') name.pop_back();
        for (auto& c : name) if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - (L'a' - L'A'));
        if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" || name == L"CONIN$"
            || name == L"CONOUT$" || name == L"CLOCK$" || (name.size() == 4 && (name.starts_with(L"COM")
                || name.starts_with(L"LPT")) && ((name[3] >= L'1' && name[3] <= L'9')
                    || name[3] == L'\u00b9' || name[3] == L'\u00b2' || name[3] == L'\u00b3'))) Fail();
    }
    return path.lexically_normal().make_preferred();
}
// Inspect original spelling before final-name normalization: normalization
// would otherwise erase evidence of a selected junction/reparse ancestor.
class DirectoryPins final {
public:
    ~DirectoryPins() { for (const auto handle : handles_) CloseHandle(handle); }
    std::wstring Pin(const fs::path& directory) {
        std::vector<fs::path> ancestors;
        for (auto current = directory; !current.empty();) {
            ancestors.push_back(current);
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        if (ancestors.empty()) Fail();
        HANDLE leaf = INVALID_HANDLE_VALUE;
        for (auto current = ancestors.rbegin(); current != ancestors.rend(); ++current) {
            const auto handle = CreateFileW(current->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) Fail();
            handles_.push_back(handle);
            const auto info = Information(handle);
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Fail();
            leaf = handle;
        }
        return FinalName(leaf);
    }
private: std::vector<HANDLE> handles_;
};
class LockedFile final {
public:
    explicit LockedFile(const fs::path& path) : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) { Regular(handle_.Get()); }
    HANDLE Get() const noexcept { return handle_.Get(); }
    std::vector<std::uint8_t> Snapshot(std::uint64_t maximum) const { return Read(Get(), maximum); }
    void Verify(const std::vector<std::uint8_t>& expected, std::uint64_t maximum) const {
        if (Snapshot(maximum) != expected) Fail();
    }
private: Handle handle_;
};
void GuardDefaultRoot(DirectoryPins& pins, const std::wstring& candidate) {
    PWSTR raw{};
    Hr(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(raw, &CoTaskMemFree);
    if (!raw) Fail();
    const auto local = OrdinaryPath(raw);
    const auto final_parent = pins.Pin(local);
    const auto product = local / L"A0CameraStitcher";
    const auto attributes = GetFileAttributesW(product.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto reason = GetLastError();
        if (reason != ERROR_FILE_NOT_FOUND && reason != ERROR_PATH_NOT_FOUND) Fail();
        Separate(candidate, final_parent + L"\\A0CameraStitcher");
    } else {
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) Fail();
        Separate(candidate, pins.Pin(product));
    }
}
std::map<std::wstring, std::wstring> Options(int argc, wchar_t* argv[]) {
    constexpr std::array<std::wstring_view, 7> names{L"image", L"ground-truth", L"plan-file", L"thresholds-file",
        L"validity-file", L"product-root", L"output-directory"};
    if (argc != 15) Fail();
    std::map<std::wstring, std::wstring> options;
    for (int index = 1; index < argc; index += 2) {
        const std::wstring name(argv[index]);
        if (!name.starts_with(L"--") || std::wstring(argv[index + 1]).empty()) Fail();
        const auto key = name.substr(2);
        if (std::find(names.begin(), names.end(), key) == names.end() || !options.emplace(key, argv[index + 1]).second) Fail();
    }
    if (options.size() != names.size()) Fail();
    return options;
}
struct JsonFailure { [[noreturn]] static void Fail(std::string_view, std::string_view) { ::Fail(); } };
using Json = a0::common::protocol_json::JsonValue;
using Kind = a0::common::protocol_json::JsonKind;
std::string String(const Json& value, const char* field) {
    const auto& leaf = value.object.at(field);
    if (leaf.kind != Kind::string) Fail();
    return leaf.string;
}
std::uint32_t Dimension(const Json& value, const char* field) {
    const auto& leaf = value.object.at(field);
    if (leaf.kind != Kind::number || leaf.string.empty() || (leaf.string.size() > 1 && leaf.string.front() == '0')) Fail();
    std::uint32_t result = 0;
    for (auto c : leaf.string) {
        if (c < '0' || c > '9') Fail();
        const auto digit = static_cast<unsigned>(c - '0');
        if (result > 32768U / 10U || (result == 32768U / 10U && digit > 32768U % 10U)) Fail();
        result = result * 10U + digit;
    }
    if (!result) Fail();
    return result;
}
std::string_view JsonText(const std::vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void JsonEncoding(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > kJsonBytes || bytes.size() > std::numeric_limits<int>::max()
        || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()), nullptr, 0)) Fail();
}
struct DecodedImage { std::uint32_t width, height; std::vector<std::uint8_t> bgr; };
DecodedImage Decode(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4 || bytes[0] != 255 || bytes[1] != 216 || bytes[bytes.size() - 2] != 255 || bytes.back() != 217) Fail();
    struct Apartment {
        Apartment() { Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
        ~Apartment() { CoUninitialize(); }
    } apartment;
    IWICImagingFactory* factory_raw{};
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_raw)));
    Com<IWICImagingFactory> factory(factory_raw);
    IWICStream* stream_raw{};
    Hr(factory->CreateStream(&stream_raw));
    Com<IWICStream> stream(stream_raw);
    Hr(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())));
    IWICBitmapDecoder* decoder_raw{};
    Hr(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder_raw));
    Com<IWICBitmapDecoder> decoder(decoder_raw);
    GUID container{}; UINT frames{};
    Hr(decoder->GetContainerFormat(&container)); Hr(decoder->GetFrameCount(&frames));
    if (container != GUID_ContainerFormatJpeg || frames != 1) Fail();
    IWICBitmapFrameDecode* frame_raw{};
    Hr(decoder->GetFrame(0, &frame_raw));
    Com<IWICBitmapFrameDecode> frame(frame_raw);
    UINT width{}, height{};
    Hr(frame->GetSize(&width, &height));
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (!width || !height || width > 32768 || height > 32768 || pixels > kPixels) Fail();
    const auto bgr_bytes = pixels * 3ULL;
    if (bgr_bytes > std::numeric_limits<std::size_t>::max() || bgr_bytes > std::numeric_limits<UINT>::max()) Fail();
    IWICFormatConverter* converter_raw{};
    Hr(factory->CreateFormatConverter(&converter_raw));
    Com<IWICFormatConverter> converter(converter_raw);
    // WIC frame/format conversion preserves stored sample order. There is no
    // EXIF Orientation lookup, rotation, or FlipRotator in this evaluator.
    Hr(converter->Initialize(frame.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom));
    DecodedImage result{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(bgr_bytes))};
    Hr(converter->CopyPixels(nullptr, width * 3U, static_cast<UINT>(result.bgr.size()), result.bgr.data()));
    return result;
}
std::span<const std::uint8_t> Validity(const std::vector<std::uint8_t>& json, const std::vector<std::uint8_t>& mask,
    const evaluation::SourceHashes& hashes, const DecodedImage& image, const evaluation::Plan& plan) {
    JsonEncoding(json);
    const auto value = a0::common::protocol_json::BasicJsonParser<JsonFailure>(JsonText(json)).Parse();
    constexpr std::array<std::string_view, 6> fields{"schemaVersion", "imageSha256", "maskSha256",
        "widthPixels", "heightPixels", "maskRelativePath"};
    if (value.kind != Kind::object || value.object.size() != fields.size()) Fail();
    for (auto field : fields) if (!value.object.contains(std::string(field))) Fail();
    if (String(value, "schemaVersion") != "a0.stitch-output-validity.v1" || String(value, "maskRelativePath") != "validity.pgm"
        || String(value, "imageSha256") != hashes.image || String(value, "maskSha256") != hashes.mask) Fail();
    const auto width = Dimension(value, "widthPixels"), height = Dimension(value, "heightPixels");
    if (width != image.width || height != image.height || width != plan.raster.width || height != plan.raster.height) Fail();
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > kPixels) Fail();
    const auto prefix = "P5\n" + std::to_string(width) + " " + std::to_string(height) + "\n1\n";
    if (mask.size() != prefix.size() + pixels || !std::equal(prefix.begin(), prefix.end(), mask.begin())) Fail();
    const auto samples = std::span<const std::uint8_t>(mask).subspan(prefix.size());
    if (std::any_of(samples.begin(), samples.end(), [](std::uint8_t byte) { return byte > 1; })) Fail();
    // A bound sidecar is external declared coverage, not proof of its producer's
    // truthfulness. Samples pass through unchanged; no ground-truth inference.
    return samples;
}
std::unique_ptr<Handle> Publish(const fs::path& partial, const fs::path& destination, const std::wstring& expected_final,
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > kReportBytes) Fail();
    auto file = std::make_unique<Handle>(CreateFileW(partial.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Regular(file->Get());
    const auto identity = Information(file->Get());
    DWORD written{};
    if (!WriteFile(file->Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        || written != bytes.size() || !FlushFileBuffers(file->Get()) || Read(file->Get(), kReportBytes) != bytes) Fail();
    const auto name = destination.native();
    const auto length = name.size() * sizeof(wchar_t);
    if (length > std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) Fail();
    std::vector<std::uint8_t> storage(sizeof(FILE_RENAME_INFO) + length);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(length);
    std::memcpy(rename->FileName, name.c_str(), length + sizeof(wchar_t));
    if (!SetFileInformationByHandle(file->Get(), FileRenameInfo, rename, static_cast<DWORD>(storage.size()))
        || !SameFile(identity, Information(file->Get())) || !Equal(FinalName(file->Get()), expected_final)
        || Read(file->Get(), kReportBytes) != bytes) Fail();
    return file;
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const char* stage = "arguments";
    try {
        const auto options = Options(argc, argv);
        stage = "paths";
        const std::array<fs::path, 6> paths{
            OrdinaryPath(options.at(L"image")), OrdinaryPath(options.at(L"ground-truth")),
            OrdinaryPath(options.at(L"plan-file")), OrdinaryPath(options.at(L"thresholds-file")),
            OrdinaryPath(options.at(L"validity-file")),
            OrdinaryPath(options.at(L"validity-file")).parent_path() / L"validity.pgm"};
        const auto product = OrdinaryPath(options.at(L"product-root"));
        const auto output = OrdinaryPath(options.at(L"output-directory"));
        if (output.filename().empty()) Fail();
        DirectoryPins pins;
        const auto parent = pins.Pin(output.parent_path());
        const auto candidate = parent + L"\\" + output.filename().native();
        Separate(candidate, pins.Pin(product));
        for (const auto& path : paths) Separate(candidate, pins.Pin(path.parent_path()));
        GuardDefaultRoot(pins, candidate);
        if (GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES) Fail();
        const auto unavailable = GetLastError();
        if (unavailable != ERROR_FILE_NOT_FOUND && unavailable != ERROR_PATH_NOT_FOUND) Fail();
        stage = "inputs";
        std::array<std::unique_ptr<LockedFile>, 6> files;
        std::array<std::vector<std::uint8_t>, 6> snapshots;
        constexpr std::array<std::uint64_t, 6> bounds{kImageBytes, kJsonBytes, kJsonBytes, kJsonBytes, kJsonBytes, kMaskBytes};
        for (std::size_t index = 0; index < paths.size(); ++index) {
            files[index] = std::make_unique<LockedFile>(paths[index]);
            for (std::size_t earlier = 0; earlier < index; ++earlier)
                if (SameFile(Information(files[index]->Get()), Information(files[earlier]->Get()))) Fail();
            if (index != 5) snapshots[index] = files[index]->Snapshot(bounds[index]);
        }
        evaluation::SourceHashes hashes{Hash(snapshots[0]), Hash(snapshots[1]), Hash(snapshots[2]),
            Hash(snapshots[3]), Hash(snapshots[4]), {}};
        stage = "contracts";
        for (std::size_t index = 1; index < 4; ++index) JsonEncoding(snapshots[index]);
        const auto plan = evaluation::ParsePlan(JsonText(snapshots[1]), JsonText(snapshots[2]));
        const auto thresholds = evaluation::ParseThresholds(JsonText(snapshots[3]));
        stage = "jpeg";
        const auto image = Decode(snapshots[0]);
        stage = "validity";
        snapshots[5] = files[5]->Snapshot(bounds[5]);
        hashes.mask = Hash(snapshots[5]);
        const auto validity = Validity(snapshots[4], snapshots[5], hashes, image, plan);
        stage = "evaluation";
        const auto result = evaluation::Evaluate({image.width, image.height, image.bgr}, validity, plan, thresholds);
        if (result.threshold_assessment != "thresholds-met" && result.threshold_assessment != "thresholds-not-met"
            && result.threshold_assessment != "incomplete-measurements") Fail();
        const auto report = evaluation::SerializeReport(result, plan, thresholds, hashes);
        if (report.empty() || report.size() > kReportBytes) Fail();
        for (std::size_t index = 0; index < paths.size(); ++index) files[index]->Verify(snapshots[index], bounds[index]);
        stage = "output";
        if (!CreateDirectoryW(output.c_str(), nullptr)) Fail();
        if (!Equal(pins.Pin(output), candidate)) Fail();
        const std::vector<std::uint8_t> bytes(report.begin(), report.end());
        const auto published = Publish(output / L"stitch-evaluation.report.json.partial", output / kReportName,
            candidate + L"\\stitch-evaluation.report.json", bytes);
        for (std::size_t index = 0; index < paths.size(); ++index) files[index]->Verify(snapshots[index], bounds[index]);
        if (Read(published->Get(), kReportBytes) != bytes) Fail();
        std::cout << "result=recorded\nquality=not-evaluated\nthresholdAssessment=" << result.threshold_assessment
            << "\nreport=stitch-evaluation.report.json\n";
        return 0;
    } catch (...) {
        std::cerr << "error=" << stage << "-failed\n";
        return 2;
    }
}
