#include "a0/m2/render.hpp"
#include "a0/m2/stitch_metrics.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using a0::m2::render::BgrImage;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kInputLimit = 64ULL * 1024 * 1024;

void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void Hr(HRESULT result) { Require(SUCCEEDED(result), "WIC/COM operation failed"); }
void Nt(NTSTATUS result) { Require(result >= 0, "SHA-256 operation failed"); }

struct ComSession {
    ComSession() { Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComSession() { CoUninitialize(); }
};

struct InputFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    explicit InputFile(const std::wstring& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "input open/lock failed");
        if (!GetFileInformationByHandle(handle, &identity) ||
            (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw std::runtime_error("input is not a readable file");
        }
    }
    ~InputFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;
    std::vector<std::uint8_t> Read() const {
        LARGE_INTEGER size{}, zero{};
        Require(GetFileSizeEx(handle, &size) && size.QuadPart > 0 &&
            static_cast<std::uint64_t>(size.QuadPart) <= kInputLimit, "input size outside 1..64 MiB");
        Require(SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN), "input seek failed");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD count = 0;
        Require(ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
            count == bytes.size(), "input read failed");
        return bytes;
    }
};

std::string Sha256(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    Nt(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    struct AlgorithmGuard { BCRYPT_ALG_HANDLE value; ~AlgorithmGuard() { BCryptCloseAlgorithmProvider(value, 0); } } ag{algorithm};
    BCRYPT_HASH_HANDLE hash = nullptr;
    Nt(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    struct HashGuard { BCRYPT_HASH_HANDLE value; ~HashGuard() { BCryptDestroyHash(value); } } hg{hash};
    Require(bytes.size() <= MAXDWORD, "hash input too large");
    Nt(BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0));
    std::array<std::uint8_t, 32> digest{};
    Nt(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

BgrImage Decode(IWICImagingFactory* factory, const std::vector<std::uint8_t>& bytes,
    std::uint32_t width, std::uint32_t height) {
    Require(bytes.size() <= MAXDWORD, "JPEG stream too large");
    ComPtr<IWICStream> stream;
    Hr(factory->CreateStream(&stream));
    Hr(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    Hr(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
    GUID container{};
    Hr(decoder->GetContainerFormat(&container));
    Require(IsEqualGUID(container, GUID_ContainerFormatJpeg), "input must be JPEG");
    UINT frames = 0;
    Hr(decoder->GetFrameCount(&frames));
    Require(frames == 1, "JPEG must have one frame");
    ComPtr<IWICBitmapFrameDecode> frame;
    Hr(decoder->GetFrame(0, &frame));
    UINT actual_width = 0, actual_height = 0;
    Hr(frame->GetSize(&actual_width, &actual_height));
    Require(actual_width == width && actual_height == height, "JPEG dimensions differ from fixed parameters");
    a0::m2::render::PixelCount(width, height);
    ComPtr<IWICFormatConverter> converter;
    Hr(factory->CreateFormatConverter(&converter));
    Hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    BgrImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    Hr(converter->CopyPixels(nullptr, width * 3, static_cast<UINT>(image.bgr.size()), image.bgr.data()));
    return image;
}

std::vector<std::uint8_t> Encode(IWICImagingFactory* factory, const BgrImage& image) {
    ComPtr<IStream> stream;
    Hr(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
    Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    Hr(encoder->CreateNewFrame(&frame, &properties));
    // Match the current product: initialize the default property bag without
    // overriding ImageQuality or JpegYCrCbSubsampling. Only the stream differs.
    Hr(frame->Initialize(properties.Get()));
    Hr(frame->SetSize(image.width, image.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    Hr(frame->SetPixelFormat(&format));
    Require(IsEqualGUID(format, GUID_WICPixelFormat24bppBGR), "encoder changed BGR format");
    Hr(frame->WritePixels(image.height, image.width * 3,
        static_cast<UINT>(image.bgr.size()), const_cast<BYTE*>(image.bgr.data())));
    Hr(frame->Commit());
    Hr(encoder->Commit());
    STATSTG stat{};
    Hr(stream->Stat(&stat, STATFLAG_NONAME));
    Require(stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart <= MAXDWORD, "encoded stream too large");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    Hr(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
    ULONG count = 0;
    Hr(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &count));
    Require(count == bytes.size(), "encoded stream truncated");
    return bytes;
}

std::string ComponentVersion(IWICImagingFactory* factory) {
    ComPtr<IWICBitmapEncoder> encoder;
    Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
    ComPtr<IWICBitmapEncoderInfo> info;
    Hr(encoder->GetEncoderInfo(&info));
    UINT count = 0;
    Hr(info->GetVersion(0, nullptr, &count));
    Require(count > 0 && count <= 128, "WIC component version unavailable");
    std::vector<wchar_t> version(count);
    Hr(info->GetVersion(count, version.data(), &count));
    std::string result;
    for (auto ch : version) {
        if (ch == 0) break;
        Require((ch >= L'0' && ch <= L'9') || ch == L'.', "unexpected WIC version format");
        result += static_cast<char>(ch);
    }
    return result;
}

std::string WicDllVersion() {
    const auto module = GetModuleHandleW(L"WindowsCodecs.dll");
    Require(module != nullptr, "WIC module unavailable");
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    Require(length > 0 && length < path.size(), "WIC module location unavailable");
    DWORD ignored = 0;
    auto size = GetFileVersionInfoSizeW(path.data(), &ignored);
    Require(size > 0, "WIC file version unavailable");
    std::vector<BYTE> buffer(size);
    Require(GetFileVersionInfoW(path.data(), 0, size, buffer.data()), "WIC file version read failed");
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    Require(VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) &&
        info_size >= sizeof(VS_FIXEDFILEINFO) && info->dwSignature == 0xfeef04bd,
        "WIC version resource invalid");
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionLS));
}

std::string Ascii(std::wstring_view text) {
    std::string result;
    for (auto ch : text) { Require(ch <= 127, "numeric option must be ASCII"); result += static_cast<char>(ch); }
    return result;
}
template<class T> T Number(const std::wstring& text) {
    auto ascii = Ascii(text);
    T value{};
    auto parsed = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
    Require(!ascii.empty() && parsed.ec == std::errc{} && parsed.ptr == ascii.data() + ascii.size(), "invalid numeric option");
    if constexpr (std::is_floating_point_v<T>) Require(std::isfinite(value), "numeric option must be finite");
    return value;
}

double Milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void Run(IWICImagingFactory* factory, const std::vector<std::uint8_t>& a,
    const std::vector<std::uint8_t>& b, std::uint32_t width, std::uint32_t height,
    double translation, std::uint32_t crop_top) {
    const a0::m2::render::PairRenderParameters parameters{
        {1, 0, 0, 0, 1, translation, 0, 0, 1},
        a0::m2::StitchLayout::camera_a_top_camera_b_bottom, {0, crop_top, 0, 0}};
    auto plan = a0::m2::render::PlanPairCanvas(width, height, width, height, parameters);
    std::cout << std::setprecision(17)
        << "{\"schema\":\"a0.m2.render-benchmark-run.v1\",\"inputSha256\":[\""
        << Sha256(a) << "\",\"" << Sha256(b) << "\"],\"outputWidth\":" << plan.output_width
        << ",\"outputHeight\":" << plan.output_height << ",\"runs\":[";
    std::string expected_hash;
    bool hashes_equal = true;
    for (int iteration = 1; iteration <= 10; ++iteration) {
        const auto start = Clock::now();
        auto camera_a = Decode(factory, a, width, height);
        auto camera_b = Decode(factory, b, width, height);
        const auto decoded = Clock::now();
        auto rendered = a0::m2::render::RenderPair(camera_a, camera_b, parameters);
        const auto render_end = Clock::now();
        auto jpeg = Encode(factory, rendered.image);
        const auto encoded = Clock::now();
        auto verified = Decode(factory, jpeg, plan.output_width, plan.output_height);
        Require(!verified.bgr.empty(), "output verification failed");
        auto hash = Sha256(jpeg);
        const auto end = Clock::now();
        // Sample while decoded input/output/verification buffers are still alive.
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)),
            "process memory measurement failed");
        if (iteration == 1) expected_hash = hash;
        hashes_equal = hashes_equal && hash == expected_hash;
        if (iteration != 1) std::cout << ',';
        std::cout << "{\"iteration\":" << iteration << ",\"phase\":\"" << (iteration == 1 ? "process-first" : "warm")
            << "\",\"decodeMilliseconds\":" << Milliseconds(start, decoded)
            << ",\"renderMilliseconds\":" << Milliseconds(decoded, render_end)
            << ",\"encodeMilliseconds\":" << Milliseconds(render_end, encoded)
            << ",\"verifyHashMilliseconds\":" << Milliseconds(encoded, end)
            << ",\"totalMilliseconds\":" << Milliseconds(start, end)
            << ",\"outputSizeBytes\":" << jpeg.size() << ",\"outputSha256\":\"" << hash
            << "\",\"peakWorkingSetBytes\":" << memory.PeakWorkingSetSize
            << ",\"peakCommitBytes\":" << memory.PeakPagefileUsage << '}';
    }
    // Read codec metadata after the first measurement, preserving process-first semantics.
    std::cout << "],\"allHashesEqual\":" << (hashes_equal ? "true" : "false") << ",\"wicComponentVersion\":\"" << ComponentVersion(factory)
        << "\",\"wicDllVersion\":\"" << WicDllVersion() << "\"}" << std::endl;
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--validate-metric-pair") {
            const auto result = a0::m2::ValidateStitchMetricResultJson(Ascii(argv[2]), Ascii(argv[3]));
            Require(result.valid, "Resource metric contract invalid");
            return 0;
        }
        ComSession com;
        ComPtr<IWICImagingFactory> factory;
        Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") {
            Require(Sha256({'a', 'b', 'c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA-256 known-answer failed");
            BgrImage tiny{16, 12, std::vector<std::uint8_t>(16 * 12 * 3, 150)};
            const auto jpeg = Encode(factory.Get(), tiny);
            Run(factory.Get(), jpeg, jpeg, 16, 12, -8, 0);
            return 0;
        }
        std::map<std::wstring, std::wstring> options;
        constexpr std::array names{L"--camera-a", L"--camera-b", L"--width", L"--height", L"--translate-y", L"--crop-top"};
        for (int i = 1; i < argc; i += 2) {
            Require(i + 1 < argc, "every option needs a value");
            Require(std::find(names.begin(), names.end(), std::wstring_view(argv[i])) != names.end(), "unknown option");
            Require(options.emplace(argv[i], argv[i + 1]).second, "duplicate option");
        }
        Require(options.size() == names.size(), "all fixed render options are required");
        const auto width = Number<std::uint32_t>(options.at(L"--width"));
        const auto height = Number<std::uint32_t>(options.at(L"--height"));
        const auto translation = Number<double>(options.at(L"--translate-y"));
        const auto crop = Number<std::uint32_t>(options.at(L"--crop-top"));
        a0::m2::render::PixelCount(width, height);
        InputFile a(options.at(L"--camera-a")), b(options.at(L"--camera-b"));
        Require(a.identity.dwVolumeSerialNumber != b.identity.dwVolumeSerialNumber ||
            a.identity.nFileIndexHigh != b.identity.nFileIndexHigh || a.identity.nFileIndexLow != b.identity.nFileIndexLow,
            "input pair must contain distinct files");
        const auto a_bytes = a.Read(), b_bytes = b.Read();
        Run(factory.Get(), a_bytes, b_bytes, width, height, translation, crop);
        Require(Sha256(a.Read()) == Sha256(a_bytes) && Sha256(b.Read()) == Sha256(b_bytes), "locked inputs changed");
        return 0;
    } catch (...) {
        // Never echo input paths or codec exceptions into a public measurement.
        std::cerr << "render-benchmark failed (invalid input, metric, or platform operation)\n";
        return 2;
    }
}
