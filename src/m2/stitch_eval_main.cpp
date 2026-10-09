// Evaluation-only I/O boundary. Deliberately independent of the product
// stitcher and product manifest: only the pure renderer and strict v2 reader
// are shared. A DRAFT output can never acquire a product success record here.
#include "a0/m2/document_render.hpp"
#include "a0/m2/rig_profile_v2.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace render = a0::m2::render;
constexpr std::uint64_t jpeg_limit = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t profile_limit = 256ULL * 1024ULL;
constexpr std::string_view label = "profileStatus=draft;quality=not-evaluated";
constexpr std::wstring_view jpeg_name = L"evaluation.jpg";
constexpr std::wstring_view manifest_name = L"stitch-eval.manifest.json";
[[noreturn]] void Fail() { throw std::runtime_error("evaluation operation refused"); }
void Hr(HRESULT result) { if (FAILED(result)) Fail(); }
template<class T> struct Release { void operator()(T* value) const noexcept { if (value) value->Release(); } };
template<class T> using Com = std::unique_ptr<T, Release<T>>;
class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) { if (value_ == INVALID_HANDLE_VALUE) Fail(); }
    ~Handle() { CloseHandle(value_); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return value_; }
private: HANDLE value_;
};
class Apartment final {
public:
    Apartment() { Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~Apartment() { CoUninitialize(); }
};

std::array<std::uint8_t, 32> Sha(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > std::numeric_limits<ULONG>::max()) Fail();
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) Fail();
    std::array<std::uint8_t, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) Fail();
    return digest;
}
std::string Hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (auto b : bytes) { result += digits[b >> 4U]; result += digits[b & 15U]; }
    return result;
}
BY_HANDLE_FILE_INFORMATION Information(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) Fail();
    return info;
}
bool SameIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh
        && a.nFileIndexLow == b.nFileIndexLow;
}
void RequireRegular(HANDLE handle) {
    if (GetFileType(handle) != FILE_TYPE_DISK
        || (Information(handle).dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) Fail();
}
std::uint64_t Size(HANDLE handle) {
    LARGE_INTEGER size{}; if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) Fail();
    return static_cast<std::uint64_t>(size.QuadPart);
}
std::vector<std::uint8_t> Read(HANDLE handle, std::uint64_t maximum) {
    const auto size = Size(handle); if (!size || size > maximum) Fail();
    LARGE_INTEGER start{}; if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) Fail();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size)); std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD actual{}; const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024U * 1024U));
        if (!ReadFile(handle, bytes.data() + offset, count, &actual, nullptr) || !actual) Fail();
        offset += actual;
    }
    if (Size(handle) != size) Fail();
    return bytes;
}
std::wstring FinalName(HANDLE handle) {
    const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, VOLUME_NAME_GUID);
    if (!required || required > 32768) Fail();
    std::wstring value(required, L'\0');
    const auto count = GetFinalPathNameByHandleW(handle, value.data(), required, VOLUME_NAME_GUID);
    if (!count || count >= required) Fail(); value.resize(count);
    if (!value.starts_with(L"\\\\?\\Volume{")) Fail();
    while (!value.empty() && value.back() == L'\\') value.pop_back();
    return value;
}
bool EqualPath(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool Ancestor(std::wstring_view a, std::wstring_view b) {
    return EqualPath(a, b) || (b.size() > a.size() && b[a.size()] == L'\\' && EqualPath(a, b.substr(0, a.size())));
}
void Separate(std::wstring_view a, std::wstring_view b) { if (Ancestor(a, b) || Ancestor(b, a)) Fail(); }

fs::path OrdinaryPath(const std::wstring& text) {
    const fs::path path(text);
    if (text.size() < 3 || text[1] != L':' || (text[2] != L'/' && text[2] != L'\\')
        || !((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'))
        || text.find(L':', 2) != std::wstring::npos
        || GetDriveTypeW(path.root_path().make_preferred().c_str()) != DRIVE_FIXED) Fail();
    for (const auto& part : path.relative_path()) {
        auto name = part.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' '
            || name.find_first_of(L"<>\"|?*:") != std::wstring::npos
            || std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; })) Fail();
        name.resize(name.find(L'.') == std::wstring::npos ? name.size() : name.find(L'.'));
        while (!name.empty() && name.back() == L' ') name.pop_back();
        for (auto& c : name) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
        if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL"
            || name == L"CONIN$" || name == L"CONOUT$" || name == L"CLOCK$"
            || (name.size() == 4 && (name.starts_with(L"COM") || name.starts_with(L"LPT"))
                && ((name[3] >= L'1' && name[3] <= L'9') || name[3] == L'\u00b9'
                    || name[3] == L'\u00b2' || name[3] == L'\u00b3'))) Fail();
    }
    return path.lexically_normal().make_preferred();
}

// Root-to-leaf OPEN_REPARSE_POINT inspection preserves the user's spelling:
// final-name normalization alone would hide a junction. No write/delete
// sharing pins the ordinary ancestors until both artifacts are committed.
class Directories final {
public:
    ~Directories() { for (const auto handle : handles_) CloseHandle(handle); }
    std::wstring Pin(const fs::path& directory) {
        std::vector<fs::path> chain; auto current = directory;
        while (!current.empty()) {
            chain.push_back(current); const auto parent = current.parent_path();
            if (parent == current) break; current = parent;
        }
        if (chain.empty()) Fail(); HANDLE leaf = INVALID_HANDLE_VALUE;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto handle = CreateFileW(it->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) Fail();
            handles_.push_back(handle); const auto info = Information(handle);
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Fail();
            leaf = handle;
        }
        return FinalName(leaf);
    }
private: std::vector<HANDLE> handles_;
};
class ImmutableFile final {
public:
    explicit ImmutableFile(const fs::path& path) : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) { RequireRegular(handle_.Get()); }
    HANDLE Get() const { return handle_.Get(); }
    std::vector<std::uint8_t> Snapshot(std::uint64_t bound) const { return Read(Get(), bound); }
    void Verify(const std::vector<std::uint8_t>& expected, std::uint64_t bound) const {
        if (Snapshot(bound) != expected) Fail();
    }
private: Handle handle_;
};

// Output stream owns its CREATE_NEW object through encode, metadata editing,
// flush, verification, exact-handle rename and reread. No pathname reopening
// can substitute a different candidate and no failure deletes evidence.
class OutputStream final : public IStream {
public:
    explicit OutputStream(const fs::path& path, std::uint64_t bound)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)), bound_(bound) { RequireRegular(Get()); }
    HANDLE Get() const { return handle_.Get(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id == IID_IUnknown || id == IID_ISequentialStream || id == IID_IStream) { *out = static_cast<IStream*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG count, ULONG* read) override {
        DWORD actual{}; if (!ReadFile(Get(), data, count, &actual, nullptr)) return STG_E_READFAULT;
        if (read) *read = actual; return actual == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG count, ULONG* written) override {
        if (written) *written = 0; LARGE_INTEGER zero{}, position{};
        if (!SetFilePointerEx(Get(), zero, &position, FILE_CURRENT) || position.QuadPart < 0
            || static_cast<std::uint64_t>(position.QuadPart) > bound_
            || count > bound_ - static_cast<std::uint64_t>(position.QuadPart)) return STG_E_MEDIUMFULL;
        DWORD actual{}; if (!WriteFile(Get(), data, count, &actual, nullptr)) return STG_E_WRITEFAULT;
        if (written) *written = actual; return actual == count ? S_OK : STG_E_WRITEFAULT;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER distance, DWORD origin, ULARGE_INTEGER* position) override {
        LARGE_INTEGER value{}; if (origin > STREAM_SEEK_END || !SetFilePointerEx(Get(), distance, &value, origin)) return STG_E_SEEKERROR;
        if (position) position->QuadPart = static_cast<ULONGLONG>(value.QuadPart); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        if (size.QuadPart > bound_) return STG_E_MEDIUMFULL;
        LARGE_INTEGER zero{}, previous{}, end{}; end.QuadPart = static_cast<LONGLONG>(size.QuadPart);
        if (!SetFilePointerEx(Get(), zero, &previous, FILE_CURRENT) || !SetFilePointerEx(Get(), end, nullptr, FILE_BEGIN)
            || !SetEndOfFile(Get()) || !SetFilePointerEx(Get(), previous, nullptr, FILE_BEGIN)) return STG_E_WRITEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* info, DWORD) override {
        if (!info) return E_POINTER; *info = {}; LARGE_INTEGER size{};
        if (!GetFileSizeEx(Get(), &size) || size.QuadPart < 0) return STG_E_READFAULT;
        info->type = STGTY_STREAM; info->cbSize.QuadPart = static_cast<ULONGLONG>(size.QuadPart); info->grfMode = STGM_READWRITE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
    std::vector<std::uint8_t> Snapshot() const { return ::Read(Get(), bound_); }
    void ReplaceBytes(const std::vector<std::uint8_t>& bytes) {
        if (bytes.empty() || bytes.size() > bound_ || bytes.size() > std::numeric_limits<ULONG>::max()) Fail();
        LARGE_INTEGER zero{}; Hr(Seek(zero, STREAM_SEEK_SET, nullptr));
        ULONG actual{}; Hr(Write(bytes.data(), static_cast<ULONG>(bytes.size()), &actual)); if (actual != bytes.size()) Fail();
        ULARGE_INTEGER size{}; size.QuadPart = bytes.size(); Hr(SetSize(size));
        if (Snapshot() != bytes) Fail();
    }
    void Flush() { if (!FlushFileBuffers(Get())) Fail(); }
    void Publish(const fs::path& destination, const std::vector<std::uint8_t>& expected, const std::wstring& final_expected) {
        const auto identity = Information(Get());
        if (Snapshot() != expected) Fail();
        const auto name = destination.native(); const auto name_bytes = name.size() * sizeof(wchar_t);
        if (name_bytes > std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) Fail();
        std::vector<std::uint8_t> storage(sizeof(FILE_RENAME_INFO) + name_bytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists = FALSE; rename->RootDirectory = nullptr; rename->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(rename->FileName, name.c_str(), name_bytes + sizeof(wchar_t));
        if (!SetFileInformationByHandle(Get(), FileRenameInfo, rename, static_cast<DWORD>(storage.size()))) Fail();
        if (!SameIdentity(identity, Information(Get())) || !EqualPath(FinalName(Get()), final_expected) || Snapshot() != expected) Fail();
    }
private: Handle handle_; std::uint64_t bound_; std::atomic<ULONG> references_{1};
};

Com<IWICImagingFactory> Factory() {
    IWICImagingFactory* raw{}; Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)));
    return Com<IWICImagingFactory>(raw);
}
Com<IWICBitmapFrameDecode> Frame(IWICImagingFactory* factory, const std::vector<std::uint8_t>& bytes, bool terminal_eoi) {
    if (bytes.size() < 4 || bytes[0] != 255 || bytes[1] != 216 || bytes.size() > jpeg_limit) Fail();
    const std::array<std::uint8_t, 2> eoi{255, 217};
    if (std::find_end(bytes.begin() + 2, bytes.end(), eoi.begin(), eoi.end()) == bytes.end()
        || (terminal_eoi && (bytes[bytes.size() - 2] != 255 || bytes.back() != 217))) Fail();
    IWICStream* stream_raw{}; Hr(factory->CreateStream(&stream_raw)); Com<IWICStream> stream(stream_raw);
    Hr(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())));
    IWICBitmapDecoder* decoder_raw{}; Hr(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder_raw));
    Com<IWICBitmapDecoder> decoder(decoder_raw); GUID format{}; Hr(decoder->GetContainerFormat(&format));
    UINT frames{}; Hr(decoder->GetFrameCount(&frames)); if (format != GUID_ContainerFormatJpeg || frames != 1) Fail();
    IWICBitmapFrameDecode* frame_raw{}; Hr(decoder->GetFrame(0, &frame_raw)); return Com<IWICBitmapFrameDecode>(frame_raw);
}
Com<IWICFormatConverter> Converter(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame) {
    IWICFormatConverter* raw{}; Hr(factory->CreateFormatConverter(&raw)); Com<IWICFormatConverter> converter(raw);
    // Stored-order pixels: no orientation transform is inserted.
    Hr(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    return converter;
}
render::BgrImage DecodeInput(IWICImagingFactory* factory, const std::vector<std::uint8_t>& bytes) {
    const auto frame = Frame(factory, bytes, false); UINT width{}, height{}; Hr(frame->GetSize(&width, &height));
    if (width != 7360 || height != 4912) Fail();
    const auto pixels = render::PixelCount(width, height); const auto converter = Converter(factory, frame.get());
    render::BgrImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(pixels * 3))};
    Hr(converter->CopyPixels(nullptr, width * 3U, static_cast<UINT>(image.bgr.size()), image.bgr.data())); return image;
}
void VerifyOutputPixels(IWICImagingFactory* factory, const std::vector<std::uint8_t>& bytes, const render::OutputRaster& raster) {
    const auto frame = Frame(factory, bytes, true); UINT width{}, height{}; Hr(frame->GetSize(&width, &height));
    if (width != raster.width_pixels || height != raster.height_pixels) Fail();
    const auto converter = Converter(factory, frame.get());
    const auto stride = width * 3U; std::vector<std::uint8_t> rows(static_cast<std::size_t>(stride) * std::min(height, 64U));
    for (UINT y = 0; y < height; y += 64) {
        const auto count = std::min(height - y, 64U); const WICRect rect{0, static_cast<INT>(y), static_cast<INT>(width), static_cast<INT>(count)};
        Hr(converter->CopyPixels(&rect, stride, stride * count, rows.data()));
    }
}

struct Segment { std::size_t offset, size, payload; std::uint8_t marker; bool jfif; };
std::vector<Segment> Segments(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4 || bytes[0] != 255 || bytes[1] != 216) Fail();
    std::vector<Segment> result; std::size_t p = 2;
    while (p < bytes.size()) {
        const auto start = p; if (bytes[p++] != 255) Fail(); while (p < bytes.size() && bytes[p] == 255) ++p;
        if (p >= bytes.size()) Fail(); const auto marker = bytes[p++];
        if (marker == 218) return result;
        if (marker == 217 || marker == 0 || marker == 216 || marker == 1 || (marker >= 208 && marker <= 215)) Fail();
        if (p + 2 > bytes.size()) Fail(); const auto length = static_cast<std::size_t>(bytes[p]) * 256 + bytes[p + 1];
        if (length < 2 || length > bytes.size() - p) Fail(); const auto payload = p + 2;
        // The evaluator writes no EXIF block, including Orientation.
        if (marker == 225 && length >= 8 && std::memcmp(bytes.data() + payload, "Exif\0\0", 6) == 0) Fail();
        const bool jfif = marker == 224 && length >= 7 && std::memcmp(bytes.data() + payload, "JFIF\0", 5) == 0;
        result.push_back({start, p + length - start, payload, marker, jfif}); p += length;
    }
    Fail();
}
void VerifyMetadata(const std::vector<std::uint8_t>& bytes, unsigned dpi) {
    unsigned jfif_count = 0, comment_count = 0;
    for (const auto& segment : Segments(bytes)) {
        const auto p = segment.payload;
        if (segment.jfif) {
            ++jfif_count;
            if (segment.size != 18 || bytes[p + 7] != 1 || bytes[p + 8] * 256U + bytes[p + 9] != dpi
                || bytes[p + 10] * 256U + bytes[p + 11] != dpi || bytes[p + 12] || bytes[p + 13]) Fail();
        }
        if (segment.marker == 254) {
            ++comment_count;
            if (segment.size != label.size() + 4 || std::memcmp(bytes.data() + p, label.data(), label.size()) != 0) Fail();
        }
    }
    if (jfif_count != 1 || comment_count != 1) Fail();
}
std::vector<std::uint8_t> Encode(IWICImagingFactory* factory, render::BgrImage& image,
    OutputStream* stream, const render::OutputRaster& raster) {
    IWICBitmapEncoder* encoder_raw{}; Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder_raw));
    Com<IWICBitmapEncoder> encoder(encoder_raw); Hr(encoder->Initialize(stream, WICBitmapEncoderNoCache));
    IWICBitmapFrameEncode* frame_raw{}; IPropertyBag2* bag_raw{}; Hr(encoder->CreateNewFrame(&frame_raw, &bag_raw));
    Com<IWICBitmapFrameEncode> frame(frame_raw); Com<IPropertyBag2> bag(bag_raw); Hr(frame->Initialize(bag.get()));
    Hr(frame->SetSize(image.width, image.height)); Hr(frame->SetResolution(raster.dpi, raster.dpi));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR; Hr(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat24bppBGR) Fail();
    Hr(frame->WritePixels(image.height, image.width * 3U, static_cast<UINT>(image.bgr.size()), image.bgr.data()));
    Hr(frame->Commit()); Hr(encoder->Commit()); frame.reset(); bag.reset(); encoder.reset();
    const auto original = stream->Snapshot(); const auto segments = Segments(original); const auto dpi = raster.dpi;
    std::vector<std::uint8_t> stamped{255, 216, 255, 224, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 2, 1,
        static_cast<std::uint8_t>(dpi >> 8), static_cast<std::uint8_t>(dpi),
        static_cast<std::uint8_t>(dpi >> 8), static_cast<std::uint8_t>(dpi), 0, 0,
        255, 254, static_cast<std::uint8_t>((label.size() + 2) >> 8), static_cast<std::uint8_t>(label.size() + 2)};
    stamped.insert(stamped.end(), label.begin(), label.end()); std::size_t cursor = 2;
    for (const auto& segment : segments) {
        if (!segment.jfif && segment.marker != 254) continue;
        stamped.insert(stamped.end(), original.begin() + cursor, original.begin() + segment.offset); cursor = segment.offset + segment.size;
    }
    stamped.insert(stamped.end(), original.begin() + cursor, original.end());
    if (stamped.size() > jpeg_limit) Fail(); stream->ReplaceBytes(stamped);
    VerifyMetadata(stamped, dpi); VerifyOutputPixels(factory, stamped, raster); stream->Flush();
    if (stream->Snapshot() != stamped) Fail(); return stamped;
}

using Options = std::map<std::wstring, std::wstring>;
Options ParseOptions(int argc, wchar_t* argv[]) {
    constexpr std::array<std::wstring_view, 13> allowed{L"camera-a", L"camera-b", L"profile-file", L"product-root", L"output-directory",
        L"left-um", L"top-um", L"right-um", L"bottom-um", L"dpi", L"width-pixels", L"height-pixels", L"resampling"};
    if (argc != 27) Fail(); Options options;
    for (int i = 1; i < argc; i += 2) {
        const std::wstring_view arg(argv[i]);
        if (!arg.starts_with(L"--") || arg.size() <= 2 || !*argv[i + 1]) Fail();
        const std::wstring name(arg.substr(2));
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()
            || !options.emplace(name, argv[i + 1]).second) Fail();
    }
    return options;
}
std::uint64_t Integer(const std::wstring& value, std::uint64_t maximum) {
    if (value.empty() || (value.size() > 1 && value[0] == L'0')) Fail(); std::uint64_t number = 0;
    for (const auto c : value) {
        if (c < L'0' || c > L'9') Fail(); const auto digit = static_cast<unsigned>(c - L'0');
        if (number > maximum / 10 || (number == maximum / 10 && digit > maximum % 10)) Fail();
        number = number * 10 + digit;
    }
    return number;
}
struct InputRecord { std::string sha; std::uint64_t size; };
std::string Manifest(const std::string& fingerprint, std::string_view kernel, const render::OutputRaster& raster,
    const std::array<InputRecord, 2>& inputs, const std::string& output_sha, std::uint64_t output_size) {
    const auto& r = raster.region_um; std::ostringstream json; json.imbue(std::locale::classic());
    json << "{\"schemaVersion\":\"a0.stitch-eval-manifest.v1\",\"profileStatus\":\"draft\",\"quality\":\"not-evaluated\","
        << "\"profileVersion\":\"2.0.0\",\"profileFingerprintSha256\":\"" << fingerprint << "\",\"resampling\":\"" << kernel
        << "\",\"outputRaster\":{\"regionUm\":{\"left\":" << r.left << ",\"top\":" << r.top << ",\"right\":" << r.right
        << ",\"bottom\":" << r.bottom << "},\"dpi\":" << raster.dpi << ",\"widthPixels\":" << raster.width_pixels
        << ",\"heightPixels\":" << raster.height_pixels << "},\"inputs\":[{\"cameraAlias\":\"CAM-A\",\"sha256\":\""
        << inputs[0].sha << "\",\"encodedSizeBytes\":" << inputs[0].size << "},{\"cameraAlias\":\"CAM-B\",\"sha256\":\""
        << inputs[1].sha << "\",\"encodedSizeBytes\":" << inputs[1].size << "}],\"output\":{\"relativePath\":\"evaluation.jpg\","
        << "\"sha256\":\"" << output_sha << "\",\"encodedSizeBytes\":" << output_size << ",\"widthPixels\":" << raster.width_pixels
        << ",\"heightPixels\":" << raster.height_pixels << "},\"engine\":{\"name\":\"a0.m2.stitch-eval\",\"version\":\"1.0.0\"}}\n";
    return json.str();
}
void GuardDefaultProductRoot(Directories& pins, const std::wstring& candidate) {
    PWSTR raw{}; Hr(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> known(raw, &CoTaskMemFree);
    if (!raw) Fail(); const auto local = OrdinaryPath(raw); const auto final_parent = pins.Pin(local);
    const auto product = local / L"A0CameraStitcher";
    const auto attributes = GetFileAttributesW(product.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError(); if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) Fail();
        Separate(candidate, final_parent + L"\\A0CameraStitcher");
    } else {
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) Fail();
        Separate(candidate, pins.Pin(product));
    }
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const char* stage = "arguments";
    try {
        const auto options = ParseOptions(argc, argv);
        const auto& resampling = options.at(L"resampling");
        if (resampling != L"bilinear" && resampling != L"bicubic-catmull-rom") Fail();
        const auto kernel = resampling == L"bilinear" ? render::Resampling::bilinear : render::Resampling::bicubic_catmull_rom;
        const auto um = [&](const wchar_t* name) { return static_cast<std::int64_t>(Integer(options.at(name), std::numeric_limits<std::int64_t>::max())); };
        const auto u32 = [&](const wchar_t* name) { return static_cast<std::uint32_t>(Integer(options.at(name), std::numeric_limits<std::uint32_t>::max())); };
        const render::OutputRaster raster{{um(L"left-um"), um(L"top-um"), um(L"right-um"), um(L"bottom-um")},
            u32(L"dpi"), u32(L"width-pixels"), u32(L"height-pixels")};
        render::ValidateOutputRaster(raster); if (raster.dpi > 65535) Fail();
        stage = "paths";
        const auto a_path = OrdinaryPath(options.at(L"camera-a")), b_path = OrdinaryPath(options.at(L"camera-b"));
        const auto profile_path = OrdinaryPath(options.at(L"profile-file")), product_root = OrdinaryPath(options.at(L"product-root"));
        const auto output_path = OrdinaryPath(options.at(L"output-directory"));
        if (output_path.filename().empty()) Fail();
        Directories pins;
        const auto output_parent = pins.Pin(output_path.parent_path());
        const auto candidate = output_parent + L"\\" + output_path.filename().native();
        Separate(candidate, pins.Pin(product_root));
        Separate(candidate, pins.Pin(a_path.parent_path())); Separate(candidate, pins.Pin(b_path.parent_path()));
        (void)pins.Pin(profile_path.parent_path()); GuardDefaultProductRoot(pins, candidate);
        const auto occupied = GetFileAttributesW(output_path.c_str());
        if (occupied != INVALID_FILE_ATTRIBUTES) Fail();
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) Fail();
        stage = "inputs";
        ImmutableFile a_file(a_path), b_file(b_path), profile_file(profile_path);
        if (SameIdentity(Information(a_file.Get()), Information(b_file.Get()))) Fail();
        const auto a_bytes = a_file.Snapshot(jpeg_limit), b_bytes = b_file.Snapshot(jpeg_limit);
        const auto profile_bytes = profile_file.Snapshot(profile_limit);
        const std::array<InputRecord, 2> input_records{{{Hex(Sha(a_bytes)), a_bytes.size()}, {Hex(Sha(b_bytes)), b_bytes.size()}}};
        stage = "profile";
        const auto profile = a0::m2::RigProfileV2::Parse(std::string_view(reinterpret_cast<const char*>(profile_bytes.data()), profile_bytes.size()));
        const auto parameters = profile.ValidateCalibrationForEvaluation(raster, kernel);
        const auto fingerprint = profile.FingerprintSha256();
        stage = "jpeg"; Apartment apartment; auto factory = Factory();
        const auto a = DecodeInput(factory.get(), a_bytes), b = DecodeInput(factory.get(), b_bytes);
        a_file.Verify(a_bytes, jpeg_limit); b_file.Verify(b_bytes, jpeg_limit); profile_file.Verify(profile_bytes, profile_limit);
        stage = "render"; auto rendered = render::RenderDocumentPair(a, b, parameters);
        stage = "output";
        if (!CreateDirectoryW(output_path.c_str(), nullptr)) Fail();
        if (!EqualPath(pins.Pin(output_path), candidate)) Fail();
        Com<OutputStream> jpeg(new OutputStream(output_path / L"evaluation.jpg.partial", jpeg_limit));
        const auto generated = Encode(factory.get(), rendered.image, jpeg.get(), raster);
        const auto generated_sha = Hex(Sha(generated));
        a_file.Verify(a_bytes, jpeg_limit); b_file.Verify(b_bytes, jpeg_limit); profile_file.Verify(profile_bytes, profile_limit);
        jpeg->Publish(output_path / jpeg_name, generated, candidate + L"\\evaluation.jpg");
        const auto json = Manifest(fingerprint, kernel == render::Resampling::bilinear ? "bilinear" : "bicubic-catmull-rom",
            raster, input_records, generated_sha, generated.size());
        const std::vector<std::uint8_t> record(json.begin(), json.end());
        Com<OutputStream> manifest(new OutputStream(output_path / L"stitch-eval.manifest.json.partial", profile_limit));
        manifest->ReplaceBytes(record); manifest->Flush();
        // All verification that can fail is completed before the final commit.
        // Held handles exclude external mutation through the entire operation.
        if (jpeg->Snapshot() != generated || Hex(Sha(jpeg->Snapshot())) != generated_sha) Fail();
        VerifyMetadata(generated, raster.dpi); profile_file.Verify(profile_bytes, profile_limit);
        manifest->Publish(output_path / manifest_name, record, candidate + L"\\stitch-eval.manifest.json");
        std::cout << "result=evaluated\nprofileStatus=draft\nquality=not-evaluated\nmanifest=stitch-eval.manifest.json\n";
        return 0;
    } catch (...) {
        // Stable phase-only diagnostics contain no absolute path, original
        // filename, physical camera identifier or profile/free-form contents.
        std::cerr << "error=" << stage << "-failed\n"; return 2;
    }
}
