#include "a0/m2/synthetic_pair.hpp"
#include "synthetic_pair_publish.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <propidl.h>
#include <psapi.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace a0::m2::synthetic {

namespace {

template <typename Interface>
class ComPtr final {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (value_ != nullptr) value_->Release(); }
    Interface** Put() noexcept { return &value_; }
    [[nodiscard]] Interface* Get() const noexcept { return value_; }
    Interface* operator->() const noexcept { return value_; }

private:
    Interface* value_{};
};

void CheckHr(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(operation) + " failed (" + code + ")");
    }
}

class ComScope final {
public:
    ComScope() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) owns_ = true;
        else if (result != RPC_E_CHANGED_MODE) throw std::runtime_error("COM initialization failed");
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope() { if (owns_) CoUninitialize(); }

private:
    bool owns_{};
};

BYTE SubsamplingOption(const ChromaSubsampling value) noexcept {
    switch (value) {
    case ChromaSubsampling::s420: return WICJpegYCrCbSubsampling420;
    case ChromaSubsampling::s422: return WICJpegYCrCbSubsampling422;
    case ChromaSubsampling::s444: return WICJpegYCrCbSubsampling444;
    case ChromaSubsampling::s440: return WICJpegYCrCbSubsampling440;
    }
    return WICJpegYCrCbSubsampling444;
}

constexpr std::uint32_t kStripRows = 16;

// Renders strips in parallel batches and hands them to the encoder in row order.
// The bytes depend only on the spec: a strip is a pure function of its rows.
void EncodeJpeg(const std::filesystem::path& destination, const PairSpec& spec, const CameraRenderer& renderer,
    const unsigned thread_count) {
    const std::uint32_t width = spec.image_width_px;
    const std::uint32_t height = spec.image_height_px;
    const std::size_t strip_bytes = static_cast<std::size_t>(width) * kStripRows * 3U;
    const std::size_t batch_strips = static_cast<std::size_t>(thread_count) * 4U;
    std::vector<std::vector<std::uint8_t>> buffers(batch_strips, std::vector<std::uint8_t>(strip_bytes));

    ComPtr<IWICImagingFactory> factory;
    CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.Put())), "WIC factory creation");
    ComPtr<IWICStream> stream;
    CheckHr(factory->CreateStream(stream.Put()), "WIC stream creation");
    CheckHr(stream->InitializeFromFilename(destination.c_str(), GENERIC_WRITE), "JPEG output open");
    ComPtr<IWICBitmapEncoder> encoder;
    CheckHr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.Put()), "JPEG encoder creation");
    CheckHr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "JPEG encoder initialization");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    CheckHr(encoder->CreateNewFrame(frame.Put(), properties.Put()), "JPEG frame creation");

    PROPBAG2 names[2]{};
    VARIANT values[2]{};
    wchar_t quality_name[] = L"ImageQuality";
    wchar_t subsampling_name[] = L"JpegYCrCbSubsampling";
    names[0].pstrName = quality_name;
    VariantInit(&values[0]);
    values[0].vt = VT_R4;
    values[0].fltVal = static_cast<float>(spec.jpeg.quality);
    names[1].pstrName = subsampling_name;
    VariantInit(&values[1]);
    values[1].vt = VT_UI1;
    values[1].bVal = SubsamplingOption(spec.jpeg.chroma_subsampling);
    CheckHr(properties->Write(2, names, values), "JPEG encoder options");
    CheckHr(frame->Initialize(properties.Get()), "JPEG frame initialization");
    CheckHr(frame->SetSize(width, height), "JPEG frame size");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    CheckHr(frame->SetPixelFormat(&format), "JPEG pixel format");
    if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("JPEG encoder changed the pixel format");

    const std::uint32_t strip_count = (height + kStripRows - 1U) / kStripRows;
    for (std::uint32_t batch_first = 0; batch_first < strip_count; batch_first += static_cast<std::uint32_t>(batch_strips)) {
        const std::uint32_t batch_last = std::min<std::uint32_t>(
            strip_count, batch_first + static_cast<std::uint32_t>(batch_strips));
        std::atomic<std::uint32_t> next{batch_first};
        std::exception_ptr failure;
        std::mutex failure_mutex;
        auto work = [&]() {
            try {
                for (;;) {
                    const std::uint32_t strip = next.fetch_add(1U);
                    if (strip >= batch_last) return;
                    const std::uint32_t first_row = strip * kStripRows;
                    const std::uint32_t rows = std::min(kStripRows, height - first_row);
                    renderer.RenderRows(first_row, rows, buffers[strip - batch_first].data());
                }
            } catch (...) {
                const std::lock_guard<std::mutex> lock(failure_mutex);
                if (!failure) failure = std::current_exception();
                next.store(batch_last);
            }
        };
        // jthread joins when it goes out of scope, so an exception cannot leave a running
        // thread behind. A thread that cannot be created just means fewer workers: the
        // calling thread runs `work` as well and the bytes do not depend on the count.
        {
            std::vector<std::jthread> workers;
            for (unsigned index = 1; index < thread_count; ++index) {
                try {
                    workers.emplace_back(work);
                } catch (const std::system_error&) {
                    break;
                }
            }
            work();
        }
        if (failure) std::rethrow_exception(failure);

        for (std::uint32_t strip = batch_first; strip < batch_last; ++strip) {
            const std::uint32_t first_row = strip * kStripRows;
            const std::uint32_t rows = std::min(kStripRows, height - first_row);
            const UINT stride = width * 3U;
            CheckHr(frame->WritePixels(rows, stride, stride * rows, buffers[strip - batch_first].data()), "JPEG pixels");
        }
    }
    CheckHr(frame->Commit(), "JPEG frame commit");
    CheckHr(encoder->Commit(), "JPEG encoder commit");
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) throw std::runtime_error("ground truth write failed");
}

void RemoveQuietly(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

std::string RelativePath(const std::string& alias) {
    return alias + "/" + std::string(kCanonicalFileName);
}

} // namespace

std::string Sha256Hex(const std::filesystem::path& file) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("SHA-256 provider open failed");
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    try {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
            throw std::runtime_error("SHA-256 hash creation failed");
        }
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("SHA-256 input open failed");
        std::vector<char> chunk(1U << 20U);
        while (input) {
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = input.gcount();
            if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()),
                static_cast<ULONG>(count), 0) < 0) {
                throw std::runtime_error("SHA-256 update failed");
            }
        }
        if (!input.eof()) throw std::runtime_error("SHA-256 input read failed");
        std::array<UCHAR, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            throw std::runtime_error("SHA-256 finish failed");
        }
        static constexpr char kDigits[] = "0123456789abcdef";
        for (const UCHAR byte : digest) {
            hex.push_back(kDigits[byte >> 4U]);
            hex.push_back(kDigits[byte & 0x0FU]);
        }
    } catch (...) {
        if (hash != nullptr) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hex;
}

namespace detail {

void PublishAll(const std::vector<PublishItem>& items) {
    std::vector<std::filesystem::path> published;
    try {
        for (const auto& item : items) {
            // No MOVEFILE_REPLACE_EXISTING: an existing destination makes the move fail.
            if (!MoveFileExW(item.partial.c_str(), item.destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                throw std::runtime_error("output rename failed");
            }
            published.push_back(item.destination);
        }
    } catch (...) {
        for (const auto& destination : published) RemoveQuietly(destination);
        for (const auto& item : items) RemoveQuietly(item.partial);
        throw;
    }
}

} // namespace detail

namespace {

// Directories that do not exist yet, outermost first, for the path and all its parents.
std::vector<std::filesystem::path> MissingDirectories(std::filesystem::path directory) {
    std::vector<std::filesystem::path> missing;
    while (!directory.empty() && !std::filesystem::exists(directory)) {
        missing.push_back(directory);
        directory = directory.parent_path();
    }
    std::reverse(missing.begin(), missing.end());
    return missing;
}

} // namespace

GenerateResult GeneratePair(const PairSpec& spec, const GenerateOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    ValidatePairSpec(spec);
    if (options.output_directory.empty()) throw std::invalid_argument("output directory is required");
    const unsigned thread_count = options.thread_count != 0
        ? std::min(options.thread_count, 64U)
        : std::max(1U, std::min(std::thread::hardware_concurrency(), 64U));

    const auto& root = options.output_directory;
    const auto truth_path = root / std::string(kGroundTruthFileName);
    if (std::filesystem::exists(truth_path)) throw std::invalid_argument("ground truth already exists in the output directory");
    for (const auto& camera : spec.cameras) {
        if (std::filesystem::exists(root / camera.alias / std::string(kCanonicalFileName))) {
            throw std::invalid_argument("an original.jpg already exists for " + camera.alias);
        }
    }

    const ComScope com;
    GenerateResult result;

    // Everything is written next to its destination as <name>.partial. Nothing becomes
    // visible under its real name until all of it is complete.
    std::vector<detail::PublishItem> items;
    std::vector<std::filesystem::path> created_directories;
    const auto discard = [&]() noexcept {
        for (const auto& item : items) RemoveQuietly(item.partial);
        // Only empty directories go; remove() leaves a directory that holds anything.
        for (auto directory = created_directories.rbegin(); directory != created_directories.rend(); ++directory) {
            RemoveQuietly(*directory);
        }
    };
    const auto partial_of = [](const std::filesystem::path& destination) {
        return std::filesystem::path(destination.wstring() + L".partial");
    };

    try {
        created_directories = MissingDirectories(root);
        std::filesystem::create_directories(root);
        for (std::size_t index = 0; index < spec.cameras.size(); ++index) {
            const auto directory = root / spec.cameras[index].alias;
            if (!std::filesystem::exists(directory)) {
                std::filesystem::create_directory(directory);
                created_directories.push_back(directory);
            }
            const auto destination = directory / std::string(kCanonicalFileName);
            items.push_back({partial_of(destination), destination});
            RemoveQuietly(items.back().partial);
        }
        items.push_back({partial_of(truth_path), truth_path});
        RemoveQuietly(items.back().partial);

        for (std::size_t index = 0; index < spec.cameras.size(); ++index) {
            const auto& alias = spec.cameras[index].alias;
            const CameraRenderer renderer(spec, index);
            EncodeJpeg(items[index].partial, spec, renderer, thread_count);
            // The partial file is renamed without being touched again, so its hash and size are final.
            result.files.push_back({alias, RelativePath(alias), Sha256Hex(items[index].partial),
                static_cast<std::uint64_t>(std::filesystem::file_size(items[index].partial))});
        }
        WriteTextFile(items.back().partial, SerializeGroundTruth(spec, result.files));
        result.ground_truth_relative_path = std::string(kGroundTruthFileName);
        result.ground_truth_sha256 = Sha256Hex(items.back().partial);

        // Images first, the ground truth last: ground-truth.json is the completion marker.
        detail::PublishAll(items);
    } catch (...) {
        discard();
        throw;
    }

    result.elapsed_milliseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        result.peak_working_set_bytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
    return result;
}

} // namespace a0::m2::synthetic