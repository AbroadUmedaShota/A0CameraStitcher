#include "a0/phase0/phase0.hpp"
#include "dual_hardware_capture_backend_internal.hpp"
#include "hardware_replay_fixtures.hpp"

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
    status.image_size.value_type = "packed-string";
    status.image_size.current_index = 0;
    status.image_size.current_label = "L(7360*4912)";
    status.image_size.string_values = {"L(7360*4912)"};
    // The real-D810 imageSize label and shape above are taken from
    // docs/evidence/phase0/run-1791263056167-1/sdk-status-summary.json.
    //
    // file_type and compression_level are left at their default-constructed
    // shape (available=false, probe_state="not-advertised", no label). This
    // is exactly what ReadSettingCapability (nikon_sdk_transport.cpp) returns
    // when the capability does not exist at all: `if (cap == nullptr) return
    // result;` with `result` default-constructed, before any probe_state or
    // current_label field is touched. Individual tests set these settings
    // explicitly for the shapes they exercise.
    return status;
}

// A setting the SDK advertised with a single packed-string label (the
// kNkMAIDArrayType_PackedString probe branch in ReadSettingCapability,
// nikon_sdk_transport.cpp), matching the shape real D810 enum settings use
// (see docs/evidence/phase0/run-1791263056167-1/sdk-status-summary.json,
// e.g. compressionLevel/imageSize).
void SetAdvertisedLabel(
    SdkCameraStatus::SettingCapability& setting, std::string_view label) {
    setting.available = true;
    setting.probe_state = "available";
    setting.cap_type = "enum";
    setting.value_type = "packed-string";
    setting.current_index = 0;
    setting.current_label = std::string(label);
    setting.string_values = {std::string(label)};
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
// "not-advertised". cap_type follows phase0.cpp's validate_setting mapping
// for these probe_state values (lines ~1799-1805): "unsupported-type" pairs
// with cap_type "unsupported"; every other probe failure here uses "enum"
// (one of the two capability types validate_setting accepts for those
// probe_state values).
void SetProbeFailure(
    SdkCameraStatus::SettingCapability& setting, std::string_view probe_state) {
    setting.available = false;
    setting.probe_state = std::string(probe_state);
    setting.cap_type = (probe_state == "unsupported-type") ? "unsupported" : "enum";
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
    status.image_size.cap_type = "unsupported";
    status.image_size.value_type = "unsupported";
    status.image_size.current_index.reset();
    status.image_size.current_label.reset();
    status.image_size.string_values.clear();

    Check(RequireProfileFailureCategory(status) == "dual_image_size_l_not_confirmed",
        "image size L not confirmed must still be rejected");
}

// Not exercised as a fixture: a fileType with available=false but a
// surviving current_label (e.g. "RAW") is impossible to produce through the
// real SDK probe. In ReadSettingCapability (nikon_sdk_transport.cpp
// ~2388-2548), every branch that assigns a current_label also sets
// available=true in the same branch (e.g. the packed-string/string
// branches), and the single catch-all failure path resets `result = {}`
// before setting probe_state (the ~2543-2548 "fail-closed per setting"
// comment) rather than leaving a previously-assigned label in place. There
// is no path through that function returning available=false with a
// current_label set, so this shape is not representable as a test fixture.
// (phase0.cpp's validate_setting lambda inside PersistSdkStatusSummary
// encodes this same "unavailable implies no value" rule, but it is not the
// reason this shape is unreachable here: that lambda runs only on the
// sdk-status CLI path, called solely from main.cpp:1036, and is never
// invoked on the dual-camera capture path this gate runs on.)


void ApplyObservedSetting(
    SdkCameraStatus::SettingCapability& setting, const replay::Json& observed) {
    setting.available = replay::Field(observed, "available").boolean;
    if (replay::HasField(observed, "capType")) setting.cap_type = replay::Field(observed, "capType").string;
    if (replay::HasField(observed, "probeState")) {
        setting.probe_state = replay::Field(observed, "probeState").string;
    }
    if (replay::HasField(observed, "valueType")) {
        setting.value_type = replay::Field(observed, "valueType").string;
    }
    if (replay::HasField(observed, "currentValue")) {
        setting.current_value = static_cast<std::uint32_t>(
            std::stoul(replay::Field(observed, "currentValue").string));
    }
    if (replay::HasField(observed, "currentIndex")) {
        setting.current_index = static_cast<std::uint32_t>(
            std::stoul(replay::Field(observed, "currentIndex").string));
    }
    if (replay::HasField(observed, "currentLabel")) {
        setting.current_label = replay::Field(observed, "currentLabel").string;
        setting.string_values = {*setting.current_label};
    }
}

// GitHub Issue #231 (replay of #222): the settings the real D810 reported when its first
// dual-camera attempt stopped at dual_jpeg_fine_not_confirmed. The nine settings come from the
// real approved profile (the app stores exactly what the body reported); fileType is the shape
// the body really returns, {"available": false} with every other descriptor left at its
// "not advertised" default. Before the #222 fix this status was rejected, so a body that is
// perfectly configured could never take the pair.
void TestReplayRealD810ObservedSettingsAreAccepted() {
    const replay::Json profile =
        replay::ParseJson(replay::ReadFixtureText("single-camera/approved-capture-profile.json"));
    const replay::Json& expected = replay::Field(profile, "expectedSettings");

    SdkCameraStatus status;
    status.live_view_status_available = true;
    status.live_view_status = "off";
    ApplyObservedSetting(status.file_type, replay::Field(expected, "fileType"));
    ApplyObservedSetting(status.compression_level, replay::Field(expected, "compressionLevel"));
    ApplyObservedSetting(status.image_size, replay::Field(expected, "imageSize"));
    ApplyObservedSetting(status.exposure_mode, replay::Field(expected, "exposureMode"));
    ApplyObservedSetting(status.shutter_speed, replay::Field(expected, "shutterSpeed"));
    ApplyObservedSetting(status.aperture, replay::Field(expected, "aperture"));
    ApplyObservedSetting(status.sensitivity, replay::Field(expected, "sensitivity"));
    ApplyObservedSetting(status.wb_mode, replay::Field(expected, "whiteBalanceMode"));
    ApplyObservedSetting(status.focus_mode, replay::Field(expected, "focusMode"));

    // The facts about the real body this test stands on.
    Check(!status.file_type.available && status.file_type.probe_state == "not-advertised" &&
              !status.file_type.current_label && !status.file_type.current_value &&
              !status.file_type.current_index,
        "the real D810 does not advertise fileType at all");
    Check(status.compression_level.available && status.compression_level.current_label == "JPEG Fine",
        "the real D810 reports compressionLevel as the label JPEG Fine");
    Check(status.image_size.current_label == "L(7360*4912)",
        "the real D810 reports imageSize as L(7360*4912)");
    Check(status.focus_mode.available && status.focus_mode.current_value == 1U &&
              !status.focus_mode.current_label,
        "the real D810 reports focusMode only as the opaque unsigned value 1");

    CheckPasses(status,
        "the settings the real D810 reported (fileType not advertised, compressionLevel JPEG Fine, "
        "imageSize L) must pass the dual-camera shutter gate");
}

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
        TestReplayRealD810ObservedSettingsAreAccepted();
        std::cout << "Dual hardware capture backend contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dual hardware capture backend contract failed: "
                  << error.what() << '\n';
        return 1;
    }
}
