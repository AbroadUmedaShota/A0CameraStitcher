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
// fileType/compressionLevel handling: this predicate is the same one the
// single-camera gate uses (RequirePcDirectCaptureProfile in
// pc_direct_capture.cpp). fileType must either be confirmed JPEG by an exact
// label match, or be the specific not-advertised shape some bodies (e.g.
// Nikon D810) report for fileType (available=false, no current_label,
// probe_state=="not-advertised"); any other way fileType fails to confirm
// JPEG is rejected. compressionLevel must always be an exact "JPEG Fine"
// label match. This does not consult or compare against an approved capture
// profile document; that is a separate mechanism used by the single-camera
// hardware_camera_agent.cpp path (ValidateApprovedCaptureProfileForShutterSession).
void RequireReadOnlyDualCaptureProfile(const SdkCameraStatus& status);

} // namespace detail
} // namespace a0::phase0
