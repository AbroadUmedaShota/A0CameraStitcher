#include "a0/phase0/phase0.hpp"
#include "dual_hardware_capture_backend_internal.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace a0::phase0;

namespace {

void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

SdkCameraStatus MakeReadOnlyStatus() {
    SdkCameraStatus status;
    status.live_view_status_available = true;
    status.live_view_status = "off";
    status.image_size.available = true;
    status.image_size.current_label = "Large";
    return status;
}

void SetAdvertisedLabel(
    SdkCameraStatus::SettingCapability& setting, std::string_view label) {
    setting.available = true;
    setting.current_label = std::string(label);
}

std::string RequireProfileFailureCategory(const SdkCameraStatus& status) {
    try {
        detail::RequireReadOnlyDualCaptureProfile(status);
    } catch (const TransportError& error) {
        return error.Category();
    }
    return {};
}

// D810: fileType is not advertised at all (available=false, no label), and
// compressionLevel reports "JPEG Fine". This is the real capture-session
// profile from the #222 field failure and must now be accepted.
void TestUnadvertisedFileTypeWithJpegFineLabelPasses() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");

    bool threw = false;
    try {
        detail::RequireReadOnlyDualCaptureProfile(status);
    } catch (const TransportError&) {
        threw = true;
    }
    Check(!threw,
        "unadvertised fileType with an exact JPEG Fine compressionLevel "
        "label must be accepted");
}

// fileType not advertised + a combined "RAW + JPEG Fine" profile must still
// be rejected: the normalized label ("rawjpegfine") is not an exact match
// for "jpegfine".
void TestUnadvertisedFileTypeWithRawJpegFineLabelFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "RAW + JPEG Fine");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "unadvertised fileType with a RAW + JPEG Fine compressionLevel "
        "label must still be rejected");
}

// fileType not advertised + "JPEG Normal" must be rejected.
void TestUnadvertisedFileTypeWithJpegNormalLabelFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "JPEG Normal");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "unadvertised fileType with a JPEG Normal compressionLevel label "
        "must still be rejected");
}

// fileType not advertised + "TIFF-RGB" must be rejected.
void TestUnadvertisedFileTypeWithTiffRgbLabelFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "TIFF-RGB");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "unadvertised fileType with a TIFF-RGB compressionLevel label "
        "must still be rejected");
}

// fileType not advertised + compressionLevel also carries no label must be
// rejected (nothing to confirm JPEG Fine against).
void TestUnadvertisedFileTypeWithNoCompressionLabelFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    // compression_level left at its default: available=false, no label.

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "unadvertised fileType with no compressionLevel label at all must "
        "still be rejected");
}

// fileType advertised as JPEG + compressionLevel Fine: unchanged legacy
// behavior must keep passing.
void TestAdvertisedJpegFileTypeWithFineLabelPasses() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.file_type, "JPEG");
    SetAdvertisedLabel(status.compression_level, "Fine");

    bool threw = false;
    try {
        detail::RequireReadOnlyDualCaptureProfile(status);
    } catch (const TransportError&) {
        threw = true;
    }
    Check(!threw,
        "an advertised JPEG fileType with a Fine compressionLevel label "
        "must keep passing");
}

// fileType advertised as JPEG + compressionLevel Normal: unchanged legacy
// behavior must keep rejecting.
void TestAdvertisedJpegFileTypeWithNormalLabelFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.file_type, "JPEG");
    SetAdvertisedLabel(status.compression_level, "Normal");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "an advertised JPEG fileType with a Normal compressionLevel label "
        "must keep being rejected");
}

// fileType advertised but indicating something other than JPEG (e.g. RAW)
// must keep rejecting regardless of compressionLevel.
void TestAdvertisedNonJpegFileTypeFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.file_type, "RAW");
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "an advertised non-JPEG fileType must keep being rejected even "
        "when compressionLevel reports JPEG Fine");
}

// Live View not confirmed OFF must still fail before the JPEG Fine check,
// unaffected by this change.
void TestLiveViewNotOffFailsBeforeJpegFineCheck() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");
    status.live_view_status = "on";

    Check(RequireProfileFailureCategory(status) == "dual_live_view_not_off",
        "Live View not confirmed OFF must still be rejected first");
}

// Image size L not confirmed must still fail after the JPEG Fine check,
// unaffected by this change.
void TestImageSizeNotConfirmedFailsAfterJpegFineCheck() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");
    status.image_size.available = false;
    status.image_size.current_label.reset();

    Check(RequireProfileFailureCategory(status) == "dual_image_size_l_not_confirmed",
        "image size L not confirmed must still be rejected");
}

} // namespace

int main() {
    try {
        TestUnadvertisedFileTypeWithJpegFineLabelPasses();
        TestUnadvertisedFileTypeWithRawJpegFineLabelFails();
        TestUnadvertisedFileTypeWithJpegNormalLabelFails();
        TestUnadvertisedFileTypeWithTiffRgbLabelFails();
        TestUnadvertisedFileTypeWithNoCompressionLabelFails();
        TestAdvertisedJpegFileTypeWithFineLabelPasses();
        TestAdvertisedJpegFileTypeWithNormalLabelFails();
        TestAdvertisedNonJpegFileTypeFails();
        TestLiveViewNotOffFailsBeforeJpegFineCheck();
        TestImageSizeNotConfirmedFailsAfterJpegFineCheck();
        std::cout << "Dual hardware capture backend contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dual hardware capture backend contract failed: "
                  << error.what() << '\n';
        return 1;
    }
}
