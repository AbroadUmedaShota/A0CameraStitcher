# Additional public protocol facts

The development still-capture event ledger uses the numeric event meanings
DeviceInfoChanged (0x4008) and Nikon LiveViewStateChanged (0xC10C), cross-checked
against the upstream [libgphoto2 v2.5.31 protocol header](https://github.com/gphoto/libgphoto2/blob/v2.5.31/camlibs/ptp2/ptp.h).
No libgphoto2 implementation, header body or linked library is copied or used.
These entries are protocol identifiers, not a new transport dependency.

Before shutter dispatch these notifications require the read-only capture
profile to be checked again. They never attribute an image. After dispatch
they fail the transaction rather than silently accepting a changed state.
Unknown events, image/card object events and reset/removal notifications are
not treated as harmless property notifications.

The #343 follow-up also cross-checks StoreAdded (0x4004) against the same
header and the upstream [event handler](https://github.com/gphoto/libgphoto2/blob/v2.5.31/camlibs/ptp2/library.c#L6923-L6928).
The identifier denotes storage addition, not image arrival. This application's
policy (not an upstream guarantee) is to invalidate the pre-shutter baseline,
re-read the existing Live-off/JPEG Fine/FX L profile once, then check two more
event batches. A further invalidation, any pending image/completion, removal,
reset or unknown event stops before shutter. StoreAdded after shutter is
terminal. No storage inventory/cache, filesystem reset implementation, setting
writer or image-attribution rule is copied from upstream.

The development preview lifecycle also references the numeric Nikon response
NotLiveView (0xA00B) in that public protocol header and prohibition bits 8
(battery exhausted) and 17 (temperature) in the same version's
[library.c](https://github.com/gphoto/libgphoto2/blob/v2.5.31/camlibs/ptp2/library.c).
These are protocol facts only. No libgphoto2 implementation is copied or linked;
no setting writer, automatic retry or restart loop is adopted.

Capture diagnostics reference standard BatteryLevel (0x5001), FocusMode
(0x500A), ExposureTime (0x500D) and Nikon OutOfFocus (0xA002) identifiers
in that same public header. Read-only integer values are logged without
inferring optical focus, exposure completion or image ownership. No upstream
implementation or camera-setting writer is copied.

The explicit #337 state diagnostic adds read-only InfoDispErrStatus (0xD1B2),
ApplicationMode (0xD1F0), LiveViewSelector (0xD1A6), RecordingMedia (0xD10B)
and standard ExposureProgramMode (0x500E). Identifiers and scalar types are
cross-checked against the public libgphoto2
[protocol header](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h)
and [configuration definitions](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c).
The selector byte is also read by the fixed-commit digiCamControl NikonBase
referenced in NOTICE. These values do not identify every possible refusal;
zero error/prohibition must never authorize a refused Start. No upstream
implementation, settings writer, recovery loop or linked library is adopted.
