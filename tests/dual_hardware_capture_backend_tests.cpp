#include "a0/phase0/phase0.hpp"
#include "dual_hardware_capture_backend_internal.hpp"

#include <cstdint>
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
    status.image_size.probe_state = "available";
    status.image_size.cap_type = "enum";
    status.image_size.value_type = "string";
    status.image_size.current_label = "Large";
    // file_type and compression_level are left at their defaults
    // (available=false, probe_state="not-advertised", no label), matching
    // the production invariant that available == (probe_state=="available")
    // and that an unavailable setting carries no value (phase0.cpp
    // validate_setting). Individual tests set them explicitly.
    return status;
}

// A setting the SDK advertised with a single string label (the
// packed-string/string probe branches in nikon_sdk_transport.cpp).
void SetAdvertisedLabel(
    SdkCameraStatus::SettingCapability& setting, std::string_view label) {
    setting.available = true;
    setting.probe_state = "available";
    setting.cap_type = "enum";
    setting.value_type = "string";
    setting.current_label = std::string(label);
}

// A setting the SDK advertised but that carries no label because it is a
// plain numeric capability (the kNkMAIDCapType_Unsigned branch in
// ReadSettingCapability, nikon_sdk_transport.cpp ~2414-2421): available=true
// but current_label stays unset.
void SetAdvertisedNumericWithoutLabel(
    SdkCameraStatus::SettingCapability& setting, std::uint32_t value) {
    setting.available = true;
    setting.probe_state = "available";
    setting.cap_type = "unsigned";
    setting.value_type = "unsigned";
    setting.current_value = value;
}

// A setting the SDK failed to probe (any ReadSettingCapability failure path
// other than "the capability does not exist at all"): available=false, no
// label, but probe_state records the specific failure instead of
// "not-advertised".
void SetProbeFailure(
    SdkCameraStatus::SettingCapability& setting, std::string_view probe_state) {
    setting.available = false;
    setting.probe_state = std::string(probe_state);
}

std::string RequireProfileFailureCategory(const SdkCameraStatus& status) {
    try {
        detail::RequireReadOnlyDualCaptureProfile(status);
    } catch (const TransportError& error) {
        return error.Category();
    }
    return {};
}

void CheckPasses(const SdkCameraStatus& status, std::string_view message) {
    bool threw = false;
    try {
        detail::RequireReadOnlyDualCaptureProfile(status);
    } catch (const TransportError&) {
        threw = true;
    }
    Check(!threw, message);
}

// D810: fileType does not exist as a capability at all (available=false,
// no label, probe_state=="not-advertised"), and compressionLevel reports
// an exact "JPEG Fine" label. This is the real capture-session profile from
// the #222 field failure and must be accepted.
void TestNotAdvertisedFileTypeWithExactJpegFineCompressionPasses() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");

    CheckPasses(status,
        "a fileType capability that does not exist (not-advertised) with an "
        "exact JPEG Fine compressionLevel label must be accepted");
}

// fileType not-advertised + a combined "RAW + JPEG Fine" compressionLevel
// profile must still be rejected: the normalized label ("rawjpegfine") is
// not an exact match for "jpegfine".
void TestNotAdvertisedFileTypeWithRawJpegFineCompressionFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "RAW + JPEG Fine");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "fileType not-advertised with a RAW + JPEG Fine compressionLevel "
        "label must still be rejected");
}

// fileType not-advertised + "JPEG Normal" must be rejected.
void TestNotAdvertisedFileTypeWithJpegNormalCompressionFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "JPEG Normal");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "fileType not-advertised with a JPEG Normal compressionLevel label "
        "must still be rejected");
}

// fileType not-advertised + "TIFF-RGB" must be rejected.
void TestNotAdvertisedFileTypeWithTiffRgbCompressionFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.compression_level, "TIFF-RGB");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "fileType not-advertised with a TIFF-RGB compressionLevel label "
        "must still be rejected");
}

// fileType not-advertised + compressionLevel also carries no label at all
// (also not-advertised) must be rejected: nothing confirms JPEG Fine.
void TestNotAdvertisedFileTypeWithNoCompressionLabelFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    // compression_level left at its MakeReadOnlyStatus() default:
    // available=false, probe_state="not-advertised", no label.

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "fileType not-advertised with no compressionLevel label at all must "
        "still be rejected");
}

// fileType failed to probe (a transport/shape error, not "the capability
// does not exist"): available=false, no label, but probe_state is e.g.
// "read-error" rather than "not-advertised". This must NOT fall through to
// the not-advertised acceptance path even though compressionLevel reports
// an exact JPEG Fine label; a probe failure is not a confirmed profile.
void TestFileTypeProbeErrorFailsEvenWithExactJpegFineCompression() {
    for (const std::string_view probe_state :
         {"read-error", "invalid-shape", "get-not-supported",
          "get-array-not-supported", "unsupported-type"}) {
        SdkCameraStatus status = MakeReadOnlyStatus();
        SetProbeFailure(status.file_type, probe_state);
        SetAdvertisedLabel(status.compression_level, "JPEG Fine");

        Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
            "a fileType probe failure must not be treated as the "
            "not-advertised acceptance shape");
    }
}

// fileType is advertised (available=true) but carries no label because the
// SDK reports it as a plain numeric capability rather than a string/enum
// one. This must be rejected even though compressionLevel reports an exact
// JPEG Fine label: an advertised-but-unlabeled fileType cannot be confirmed
// as JPEG.
void TestAdvertisedNumericFileTypeWithoutLabelFailsEvenWithExactJpegFineCompression() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedNumericWithoutLabel(status.file_type, 3);
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "an advertised fileType capability with no label must still be "
        "rejected");
}

// fileType advertised with an exact "JPEG" label + compressionLevel
// advertised with an exact "JPEG Fine" label: the single-camera-equivalent
// confirmed-JPEG case must pass.
void TestAdvertisedExactJpegFileTypeWithExactJpegFineCompressionPasses() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.file_type, "JPEG");
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");

    CheckPasses(status,
        "an advertised exact JPEG fileType with an exact JPEG Fine "
        "compressionLevel label must be accepted");
}

// fileType advertised with an exact "JPEG" label + compressionLevel
// "JPEG Normal" must be rejected.
void TestAdvertisedExactJpegFileTypeWithJpegNormalCompressionFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.file_type, "JPEG");
    SetAdvertisedLabel(status.compression_level, "JPEG Normal");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "an advertised exact JPEG fileType with a JPEG Normal "
        "compressionLevel label must be rejected");
}

// fileType advertised with a single non-JPEG label (e.g. "RAW") must be
// rejected regardless of compressionLevel.
void TestAdvertisedNonJpegFileTypeFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.file_type, "RAW");
    SetAdvertisedLabel(status.compression_level, "JPEG Fine");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "an advertised non-JPEG fileType must be rejected even when "
        "compressionLevel reports an exact JPEG Fine label");
}

// Regression guard for the substring-match bug this gate must not
// reintroduce: fileType advertised as the combined label "RAW + JPEG" together
// with a compressionLevel label of plain "Fine" would both satisfy a
// substring match ("jpeg" is contained in "raw+jpeg", "fine" is contained in
// "fine") even though neither is an exact confirmed-JPEG-Fine profile. Both
// the fileType and compressionLevel checks are exact-match, so this must be
// rejected.
void TestAdvertisedCombinedRawPlusJpegFileTypeWithPlainFineCompressionFails() {
    SdkCameraStatus status = MakeReadOnlyStatus();
    SetAdvertisedLabel(status.file_type, "RAW + JPEG");
    SetAdvertisedLabel(status.compression_level, "Fine");

    Check(RequireProfileFailureCategory(status) == "dual_jpeg_fine_not_confirmed",
        "a combined RAW + JPEG fileType label with a plain Fine "
        "compressionLevel label must be rejected, not accepted by a "
        "substring match");
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
    status.image_size.probe_state = "not-advertised";
    status.image_size.current_label.reset();

    Check(RequireProfileFailureCategory(status) == "dual_image_size_l_not_confirmed",
        "image size L not confirmed must still be rejected");
}

// Not exercised as a fixture: a fileType with available=false but a
// surviving current_label (e.g. "RAW") is impossible to produce through the
// real SDK probe and is rejected by the production invariant enforced in
// phase0.cpp's validate_setting ("unavailable SDK setting capability must
// not include a value"), which every SdkCameraStatus on the capture path is
// checked against before this gate runs. There is no SettingCapability
// shape that is both !available and carries a current_label, so this case
// is not representable as a test fixture.

} // namespace

int main() {
    try {
        TestNotAdvertisedFileTypeWithExactJpegFineCompressionPasses();
        TestNotAdvertisedFileTypeWithRawJpegFineCompressionFails();
        TestNotAdvertisedFileTypeWithJpegNormalCompressionFails();
        TestNotAdvertisedFileTypeWithTiffRgbCompressionFails();
        TestNotAdvertisedFileTypeWithNoCompressionLabelFails();
        TestFileTypeProbeErrorFailsEvenWithExactJpegFineCompression();
        TestAdvertisedNumericFileTypeWithoutLabelFailsEvenWithExactJpegFineCompression();
        TestAdvertisedExactJpegFileTypeWithExactJpegFineCompressionPasses();
        TestAdvertisedExactJpegFileTypeWithJpegNormalCompressionFails();
        TestAdvertisedNonJpegFileTypeFails();
        TestAdvertisedCombinedRawPlusJpegFileTypeWithPlainFineCompressionFails();
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
