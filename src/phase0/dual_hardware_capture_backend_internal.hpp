#pragma once

namespace a0::phase0 {
struct SdkCameraStatus;

namespace detail {

// Private production boundary shared with contract tests, not a public API.
//
// Gate evaluated immediately before the DualCamera shutter command. Throws
// TransportError with one of:
//   "dual_live_view_not_off"          Live View was not confirmed OFF.
//   "dual_jpeg_fine_not_confirmed"     JPEG Fine could not be confirmed.
//   "dual_image_size_l_not_confirmed"  Image size L could not be confirmed.
//
// fileType/compressionLevel handling: when the camera advertises fileType
// (available and a current label), fileType must indicate JPEG and
// compressionLevel must contain "fine", matching the single-camera approved
// profile check. Some bodies (e.g. Nikon D810) do not advertise fileType at
// all; in that case the gate instead requires compressionLevel's normalized
// label to be an exact match for "jpegfine" so that combined profiles such
// as "RAW + JPEG Fine" are still rejected.
void RequireReadOnlyDualCaptureProfile(const SdkCameraStatus& status);

} // namespace detail
} // namespace a0::phase0
