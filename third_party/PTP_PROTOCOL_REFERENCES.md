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

The #416 stage A read-only settings display (`--settings-read-diagnostics` and the
"両台の設定を読む" button) reads these property descriptions with
GetDevicePropDesc (0x1014), plus the Live state with GetDevicePropValue (0x1015,
code 0xD1A2, already used above). All numbers below were checked on 2026-11-10
against the public libgphoto2 sources at the fixed commit
`92171a382f185f617e95cb9acecf2d43688b19ab`: protocol header
[ptp.h](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h),
[config.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c)
and [ptp-pack.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp-pack.c).
No implementation, header body or linked library is copied or used; these are
protocol identifiers and value meanings only. This step writes nothing: 0x1016
(SetDevicePropValue, ptp.h#L245) stays refused by the read-only gate.

| Setting | Code | Scalar type | Source lines |
| --- | --- | --- | --- |
| Exposure program (M=1, P=2, A=3, S=4) | 0x500E | UINT16 | [ptp.h#L1954](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1954), [config.c#L4439-L4443](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L4439-L4443), [config.c#L12223](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12223) |
| Exposure time (unit 1/10000 s; Nikon 0xFFFFFFFF = Bulb, 0xFFFFFFFD = Time) | 0x500D | UINT32 | [ptp.h#L1953](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1953), [config.c#L4137-L4161](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L4137-L4161), [config.c#L12263](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12263) |
| F-number (value / 100) | 0x5007 | UINT16 | [ptp.h#L1947](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1947), [config.c#L3961-L3977](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L3961-L3977), [config.c#L12192](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12192) |
| ISO (ExposureIndex) | 0x500F | UINT16 | [ptp.h#L1955](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1955), [config.c#L12076](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12076) |
| Nikon ISO auto control (0 = Off, else On) | 0xD054 (`PTP_DPC_NIKON_ISOAuto`) and 0xD16A (`PTP_DPC_NIKON_ISO_Auto`) | UINT8 | [ptp.h#L2397](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2397), [ptp.h#L2596](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2596), [config.c#L12088-L12089](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12088-L12089), [config.c#L1229-L1240](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1229-L1240) |
| White balance (1 Manual to 7 Flash; Nikon 0x8010 Cloudy, 0x8011 Shade, 0x8012 Color Temperature, 0x8013 Preset) | 0x5005 | UINT16 | [ptp.h#L1945](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1945), [config.c#L1454-L1465](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1454-L1465), [config.c#L12097](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12097) |
| Exposure metering mode (1 Average, 2 Center Weighted, 3 Multi Spot, 4 Center Spot) | 0x500B | UINT16 | [ptp.h#L1951](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1951), [config.c#L5068-L5072](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L5068-L5072), [config.c#L12248](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12248) |
| Exposure bias compensation (value / 1000 EV) | 0x5010 | INT16 | [ptp.h#L1956](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1956), [config.c#L12152](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12152) |
| Nikon Active D-Lighting | 0xD14E | UINT8 | [ptp.h#L2570](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2570), [config.c#L12467](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12467) |
| Nikon active Picture Control item | 0xD200 (`PTP_DPC_NIKON_ActivePicCtrlItem`) | UINT8 assumed, not cross-checked (config.c has no entry for it) | [ptp.h#L2702](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2702) |
| Nikon shutter speed (diagnostic line only) | 0xD100 (`PTP_DPC_NIKON_ExposureTime`) | UINT32 | [ptp.h#L2522](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2522), [config.c#L12267](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12267) |

The DevicePropDesc layout parsed by `ParseD810PropDesc` (property code u16, data
type u16, get/set u8, default, current, form flag u8, then range min/max/step or an
enumeration count u16 and values, little endian; get/set 0 = Get, 1 = GetSet;
form flag 0 = none, 1 = range, 2 = enumeration) follows
[ptp-pack.c#L660-L734](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp-pack.c#L660-L734),
the data type numbers [ptp.h#L1911-L1935](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1911-L1935)
and the flags [ptp.h#L4123-L4139](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L4123-L4139).
Unlike the upstream unpacker, which tolerates short or surplus data, this
application requires the exact computed length and refuses strings, arrays and
other widths.

Not established by these sources: which of 0xD054 and 0xD16A a D810 answers to,
whether 0xD100 exists on it, and the real scalar type of 0xD200 (no
cross-check: 未照合). Whether each source line really belongs to the D810 rather
than another Nikon body is likewise not established: the upstream tables are
per-vendor, not per-model. The diagnostic prints each code's answer for a person
to compare with the body.
