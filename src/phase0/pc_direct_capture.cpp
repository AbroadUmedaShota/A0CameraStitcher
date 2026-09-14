#include "a0/phase0/pc_direct_capture.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace a0::phase0 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaximumJpegBytes = 256U * 1024U * 1024U;

void CheckHr(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        throw TransportError(
            "invalid_jpeg_decode",
            std::string("WIC JPEG operation failed: ") +
                std::string(operation));
    }
}

class ComApartment final {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
            CheckHr(result, "initialize COM");
        }
        owns_initialization_ = SUCCEEDED(result);
    }

    ~ComApartment() {
        if (owns_initialization_) CoUninitialize();
    }

private:
    bool owns_initialization_{};
};

std::string NormalizeSettingLabel(std::string_view label) {
    std::string normalized;
    normalized.reserve(label.size());
    for (const unsigned char character : label) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(
                static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

bool LabelContains(
    const SdkCameraStatus::SettingCapability& setting,
    std::string_view expected) {
    return setting.available && setting.current_label &&
        NormalizeSettingLabel(*setting.current_label).find(expected) !=
            std::string::npos;
}

bool LabelEquals(
    const SdkCameraStatus::SettingCapability& setting,
    std::string_view expected) {
    return setting.available && setting.current_label &&
        NormalizeSettingLabel(*setting.current_label) == expected;
}

void RequirePcDirectCaptureProfile(const SdkCameraStatus& status) {
    if (!status.live_view_status_available ||
        status.live_view_status != "off") {
        throw TransportError(
            "pc_direct_live_view_not_off",
            "Live View OFF was not confirmed in the PC-direct capture session");
    }
    const bool file_type_is_jpeg_only =
        LabelEquals(status.file_type, "jpeg");
    const bool file_type_is_approved_unavailable =
        !status.file_type.available &&
        !status.file_type.current_label &&
        status.file_type.probe_state == "not-advertised";
    if ((!file_type_is_jpeg_only &&
         !file_type_is_approved_unavailable) ||
        !LabelEquals(status.compression_level, "jpegfine")) {
        throw TransportError(
            "pc_direct_jpeg_fine_not_confirmed",
            "JPEG Fine was not confirmed in the PC-direct capture session");
    }
    const auto image_size_label =
        status.image_size.available && status.image_size.current_label
            ? NormalizeSettingLabel(*status.image_size.current_label)
            : std::string{};
    const bool large_label = image_size_label == "l" ||
        image_size_label == "large" ||
        image_size_label == "l73604912" ||
        image_size_label == "l7360x4912" ||
        image_size_label == "large73604912" ||
        image_size_label == "large7360x4912";
    if (!large_label) {
        throw TransportError(
            "pc_direct_image_size_l_not_confirmed",
            "image size L was not confirmed in the PC-direct capture session");
    }
}

std::vector<unsigned char> ReadBoundedFile(const fs::path& path) {
    std::error_code size_error;
    const auto size = fs::file_size(path, size_error);
    if (size_error || size == 0 || size > kMaximumJpegBytes) {
        throw TransportError(
            "pc_original_verification_failed",
            "persisted PC original size is invalid");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw TransportError(
            "pc_original_verification_failed",
            "persisted PC original cannot be reopened");
    }
    std::vector<unsigned char> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad() || bytes.size() != size) {
        throw TransportError(
            "pc_original_verification_failed",
            "persisted PC original could not be read completely");
    }
    return bytes;
}

std::string ErrorCategory(const std::exception& error) {
    if (const auto* transport = dynamic_cast<const TransportError*>(&error)) {
        return transport->Category();
    }
    return "pc_direct_exception";
}

} // namespace

std::optional<std::string> PcDirectReportRelativePath(
    const fs::path& run_root,
    const fs::path& original_path) {
    if (run_root.empty() || original_path.empty()) return std::nullopt;
    const auto relative = original_path.lexically_relative(run_root);
    const bool safe_relative = !relative.empty() &&
        !relative.is_absolute() &&
        std::none_of(
            relative.begin(), relative.end(),
            [](const fs::path& component) {
                return component == "..";
            });
    if (!safe_relative) return std::nullopt;
    return relative.generic_string();
}

DecodedJpegInfo FullyDecodeJpeg(
    const std::vector<unsigned char>& bytes,
    std::uint32_t expected_width,
    std::uint32_t expected_height) {
    if (bytes.empty() || bytes.size() > kMaximumJpegBytes ||
        bytes.size() > std::numeric_limits<DWORD>::max() ||
        expected_width == 0 || expected_height == 0) {
        throw TransportError(
            "invalid_jpeg_decode", "JPEG input or expected dimensions are invalid");
    }

    ComApartment apartment;
    ComPtr<IWICImagingFactory> factory;
    CheckHr(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "create imaging factory");
    ComPtr<IWICStream> stream;
    CheckHr(factory->CreateStream(&stream), "create memory stream");
    CheckHr(stream->InitializeFromMemory(
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),
        static_cast<DWORD>(bytes.size())), "initialize memory stream");

    ComPtr<IWICBitmapDecoder> decoder;
    CheckHr(factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder),
        "create JPEG decoder");
    GUID container{};
    CheckHr(decoder->GetContainerFormat(&container), "read container format");
    if (container != GUID_ContainerFormatJpeg) {
        throw TransportError(
            "invalid_jpeg_decode", "decoded image container is not JPEG");
    }
    UINT frame_count = 0;
    CheckHr(decoder->GetFrameCount(&frame_count), "read frame count");
    if (frame_count != 1) {
        throw TransportError(
            "invalid_jpeg_decode", "JPEG must contain exactly one frame");
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(decoder->GetFrame(0, &frame), "open JPEG frame");
    UINT width = 0;
    UINT height = 0;
    CheckHr(frame->GetSize(&width, &height), "read JPEG dimensions");
    if (width != expected_width || height != expected_height) {
        throw TransportError(
            "jpeg_dimensions_mismatch",
            "decoded JPEG dimensions do not match the approved profile");
    }

    ComPtr<IWICFormatConverter> converter;
    CheckHr(factory->CreateFormatConverter(&converter),
        "create pixel converter");
    CheckHr(converter->Initialize(
        frame.Get(), GUID_WICPixelFormat24bppBGR,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom), "initialize pixel converter");
    constexpr UINT bytes_per_pixel = 3;
    if (width > std::numeric_limits<UINT>::max() / bytes_per_pixel) {
        throw TransportError(
            "invalid_jpeg_decode", "decoded JPEG row is too large");
    }
    const UINT stride = width * bytes_per_pixel;
    constexpr UINT rows_per_chunk = 32;
    if (stride > std::numeric_limits<UINT>::max() / rows_per_chunk) {
        throw TransportError(
            "invalid_jpeg_decode", "decoded JPEG chunk is too large");
    }
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(stride) * rows_per_chunk);
    for (UINT top = 0; top < height;) {
        const UINT rows = std::min(rows_per_chunk, height - top);
        WICRect rect{
            0, static_cast<INT>(top), static_cast<INT>(width),
            static_cast<INT>(rows)};
        CheckHr(converter->CopyPixels(
            &rect, stride, stride * rows, pixels.data()),
            "decode JPEG pixels");
        top += rows;
    }
    return {width, height};
}

PcDirectCaptureResult ExecutePcDirectCaptureOnce(
    IPcDirectCaptureTransport& transport,
    EvidenceWriter& evidence,
    const PcDirectCaptureRequest& request,
    const std::function<void()>& after_sdk_close_validation) {
    if (request.transaction_id.empty() ||
        (request.camera_alias != "CAM-A" &&
         request.camera_alias != "CAM-B") ||
        request.stable_identity.empty()) {
        throw std::invalid_argument(
            "PC-direct capture request is incomplete");
    }

    PcDirectCaptureResult result;
    result.transaction.run_id = evidence.RunId();
    result.transaction.transaction_id = request.transaction_id;
    const auto started = std::chrono::steady_clock::now();
    bool opened = false;

    const auto set_failure = [&](std::string category) {
        result.transaction.terminal_state = "FailedPartial";
        result.transaction.error_category = std::move(category);
    };
    const auto close_once = [&]() {
        if (!opened) return;
        opened = false;
        result.save_media_restore_attempted = true;
        try {
            transport.ClosePcDirect(request.timeouts.close);
            result.save_media_restore_confirmed = true;
        } catch (const std::exception& error) {
            set_failure(ErrorCategory(error));
            evidence.RecordState(
                request.transaction_id,
                "PcDirectSaveMediaRestoreFailed",
                request.camera_alias);
        }
    };

    evidence.RecordState(
        request.transaction_id, "PcDirectCaptureStarted",
        request.camera_alias);
    try {
        transport.OpenPcDirect(
            request.stable_identity, request.timeouts.open);
        opened = true;
        const auto status = transport.ProbeOpenCaptureSessionStatus(
            request.timeouts.open);
        RequirePcDirectCaptureProfile(status);
        const auto baseline = transport.BeginPcDirectBaseline(
            request.timeouts.open);
        result.capture_attempted = true;
        const auto candidates = transport.CaptureAndDownloadToPc(
            baseline, request.timeouts.image_event,
            request.timeouts.download,
            request.timeouts.transaction_watchdog);
        result.candidate_count = candidates.size();

        if (candidates.size() != 1 ||
            !candidates.front().attributable) {
            auto frame = evidence.PersistExactlyOne(
                request.transaction_id, request.camera_alias, candidates);
            set_failure(frame.error_category.empty()
                    ? "pc_direct_candidate_unattributable"
                    : frame.error_category);
            result.transaction.frames.push_back(std::move(frame));
        } else {
            try {
                (void)FullyDecodeJpeg(
                    candidates.front().bytes,
                    request.expected_width,
                    request.expected_height);
                result.downloaded_jpeg_fully_decoded = true;
                auto frame = evidence.PersistExactlyOne(
                    request.transaction_id,
                    request.camera_alias,
                    candidates);
                result.transaction.frames.push_back(std::move(frame));
                auto& persisted_frame = result.transaction.frames.back();
                if (!persisted_frame.success) {
                    set_failure(persisted_frame.error_category.empty()
                            ? "pc_original_persistence_failed"
                            : persisted_frame.error_category);
                } else {
                    const auto persisted = ReadBoundedFile(
                        persisted_frame.path);
                    (void)FullyDecodeJpeg(
                        persisted, request.expected_width,
                        request.expected_height);
                    if (persisted.size() != persisted_frame.bytes ||
                        Sha256Hex(persisted) != persisted_frame.sha256) {
                        throw TransportError(
                            "pc_original_verification_failed",
                            "persisted PC original changed after publication");
                    }
                    result.persisted_jpeg_fully_decoded = true;
                }
            } catch (const TransportError& error) {
                if (result.transaction.frames.empty()) {
                    auto frame = evidence.QuarantineUnconfirmed(
                        request.transaction_id,
                        request.camera_alias,
                        candidates,
                        "PC-direct JPEG decode or persistence validation failed");
                    frame.error_category = error.Category();
                    result.transaction.frames.push_back(std::move(frame));
                }
                set_failure(error.Category());
            } catch (const std::exception&) {
                if (result.transaction.frames.empty()) {
                    auto frame = evidence.QuarantineUnconfirmed(
                        request.transaction_id,
                        request.camera_alias,
                        candidates,
                        "PC-direct original persistence failed");
                    frame.error_category =
                        "pc_original_persistence_failed";
                    result.transaction.frames.push_back(std::move(frame));
                }
                set_failure("pc_original_persistence_failed");
            }
        }
        close_once();
        if (result.save_media_restore_confirmed &&
            after_sdk_close_validation) {
            try {
                after_sdk_close_validation();
            } catch (const std::exception& error) {
                set_failure(ErrorCategory(error));
                evidence.RecordState(
                    request.transaction_id,
                    "PcDirectPostCloseValidationFailed",
                    request.camera_alias);
            }
        }
        if (result.save_media_restore_confirmed &&
            result.transaction.error_category.empty() &&
            result.downloaded_jpeg_fully_decoded &&
            result.persisted_jpeg_fully_decoded &&
            result.transaction.frames.size() == 1 &&
            result.transaction.frames.front().success) {
            result.transaction.terminal_state = "Complete";
        }
    } catch (const std::exception& error) {
        set_failure(ErrorCategory(error));
        close_once();
    }

    result.transaction.duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    evidence.RecordState(
        request.transaction_id,
        result.transaction.terminal_state == "Complete"
            ? "PcDirectCaptureComplete"
            : "PcDirectCaptureFailed",
        request.camera_alias,
        result.transaction.error_category);
    evidence.RecordResult(result.transaction);
    return result;
}

} // namespace a0::phase0
