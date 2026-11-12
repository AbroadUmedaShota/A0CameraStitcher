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

## #416 white balance and metering in the settings copy (ADR-0038 revision)

The ADR-0038 revision, which the owner decided to adopt, adds white balance (0x5005) and the metering mode (0x500B) to the items the settings copy plan covers. The fixed product table enables CAM-B's rows for both (CAM-A's rows stay disabled). Neither has been written on a real body (未検証). The numbers below were read in the same public libgphoto2 sources at the fixed commit `92171a382f185f617e95cb9acecf2d43688b19ab` as the table above: [ptp.h](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h), [config.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c) and [ptp.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.c). No implementation, header body or linked library is copied or used; these are protocol identifiers and value meanings only. No Nikon SDK material is used.

| Fact | Code / value | Source lines |
| --- | --- | --- |
| Both properties are offered as settable UINT16 enumerations in the upstream configuration table (each entry names a put function) | 0x5005, 0x500B | [config.c#L12097](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12097), [config.c#L12248](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12248) |
| White balance kinds a copy may carry (named in the upstream table; their meaning on a D810 is not established, 未照合) | 0x0002 Automatic, 0x0004 Daylight, 0x0005 Fluorescent, 0x0006 Tungsten, 0x0007 Flash; Nikon 0x8010 Cloudy, 0x8011 Shade | [config.c#L1456-L1463](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1456-L1463) |
| White balance kinds that lean on content held in the body, never copied | 0x0001 Manual, 0x0003 One-push Automatic; Nikon 0x8012 Color Temperature, 0x8013 Preset | [config.c#L1455](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1455), [config.c#L1457](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1457), [config.c#L1464-L1465](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1464-L1465) |
| Named upstream, not named by the application, not copied | Nikon 0x8014 Off, 0x8015 Flash, 0x8016 Natural light auto | [config.c#L1466-L1468](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1466-L1468) |
| Metering modes the copy may carry, by the generic libgphoto2 names (other makers' values share the table and are not copied; the names shown on the screen are the Nikon names in "Metering mode names" below) | 0x0001 Average, 0x0002 Center Weighted, 0x0003 Multi Spot, 0x0004 Center Spot | [config.c#L5068-L5072](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L5068-L5072) |
| Nikon properties tied to a white balance kind, not copied (identifiers only; nothing is read or written for them, and whether a D810 has them or how it answers is not established, 未検証) | type of Automatic 0xD141 (`PTP_DPC_NIKON_WBAutoType`), type of Fluorescent 0xD14F (`PTP_DPC_NIKON_FlourescentType`) | [ptp.h#L2562](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2562), [ptp.h#L2571](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2571) |
| Nikon properties around white balance (identifiers only) | colour temperature 0xD01E; preset number 0xD01F (UINT8, settable upstream); preset values 0xD025-0xD029 (UINT32, no put function upstream); fine adjustment of each kind 0xD017-0xD01D (INT8, UINT8 or UINT16, depending on the body) | [ptp.h#L2331-L2349](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2331-L2349), [config.c#L12331-L12353](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12331-L12353), [ptp.c#L6543-L6580](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.c#L6543-L6580) |
| Nikon property around centre-weighted metering (area size; the value table differs by body) | 0xD059 (UINT8) | [ptp.h#L2402](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2402), [config.c#L6516-L6532](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L6516-L6532), [config.c#L12355](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12355) |
| Nikon vendor operations that measure a white (not used) | 0x9402-0x9406 | [ptp.h#L623-L627](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L623-L627) |

What these sources do not establish:

- A way to copy the content of a registered white (Preset). The properties that hold it (0xD025-0xD029) have no put function upstream and no stated unit; 0xD01F is a slot number; the operations that measure a white are vendor operations, which the application does not send. So the content of a Preset cannot be copied on the strength of public sources (未成立).
- The scalar type and unit of colour temperature 0xD01E: it has a name in ptp.h and ptp.c but no entry in config.c (未照合). It is not written.
- Whether a D810 answers to these properties, and with which GetSet: not established by these sources (未検証 until a read on the real bodies; the stage A read already shows it per property).
- What the spot metering position follows: no property for it was found in these sources, so a metering mode copy does not carry a position.

## #416 further image-look properties (identifiers for a Proposed design, nothing is read or written for them)

The ADR-0038 revision draft that lists the settings of a Camera Control Pro 2 style screen names the properties below. They are cited from the same fixed commit as above ([ptp.h](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h), [config.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c)). The upstream tables are per vendor, and the value tables of Active D-Lighting and High ISO noise reduction are filled in with another body's values (config.c#L12560-L12565, the generic list), so none of these is established for a D810 (未照合). No implementation is copied or used.

| Setting | Code | Type and values (upstream) | Source lines |
| --- | --- | --- | --- |
| Colour space | 0xD032 | UINT8: 0 sRGB, 1 AdobeRGB | [ptp.h#L2361](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2361), [config.c#L12107](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12107), [config.c#L4974-L4978](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L4974-L4978) |
| Vignette control | 0xD0F7 | UINT8: 0 High, 1 Normal, 2 Moderate, 3 Off | [ptp.h#L2516](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2516), [config.c#L12373](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12373), [config.c#L8092-L8098](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L8092-L8098) |
| Auto distortion control | 0xD0F8 | UINT8: On/Off | [ptp.h#L2517](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2517), [config.c#L12372](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12372) |
| HDR mode | 0xD130 | UINT8: On/Off | [ptp.h#L2556](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2556), [config.c#L12234](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12234) |
| Long exposure noise reduction | 0xD06B | UINT8: On/Off | [ptp.h#L2422](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2422), [config.c#L12137](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12137) |
| High ISO noise reduction | 0xD070 | UINT8: 0 Off, 1 Low, 2 Normal, 3 High (another body's table) | [ptp.h#L2427](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2427), [config.c#L12565](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12565) |
| Active D-Lighting values | 0xD14E | UINT8; the value table differs by body (0 Extra high to 5 Auto in the other body's table) | [config.c#L7589-L7597](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L7589-L7597), [config.c#L12467](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12467) |
| ISO auto upper limit, minimum shutter speed | 0xD183, 0xD164 | UINT8 (another body's table) | [ptp.h#L2618](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2618), [ptp.h#L2589](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2589), [config.c#L12561-L12563](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12561-L12563) |
| Picture Control selection, change | 0xD200, 0xD201 | name only: no entry in config.c, so type and values are not established | [ptp.h#L2702-L2703](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2702-L2703) |
| Live view white balance | 0xD1A7 | name only | [ptp.h#L2637](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2637) |

The range of settings a PC control program offers was taken from the manufacturer's public Camera Control Pro 2 online help (English), pages 08_camera_controls_01 to 08_camera_controls_05 and 10_camera_menu_01 under https://nikonimglib.com/ccp2/onlinehelp/en/. Only setting names and the cameras a page names are used, to decide which settings are ordinary shooting settings. The help does not name protocol operations, so it says nothing about how the program talks to the camera (未確認).



## #416 sources for the settings write (ADR-0038 stage B prerequisite 8)

This section records where the numbers, the response meaning and the procedures behind the settings write come from. The public sources are the libgphoto2 files at the fixed commit `92171a382f185f617e95cb9acecf2d43688b19ab` ([ptp.h](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h), [config.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c), [ptp.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.c), [ptp-pack.c](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp-pack.c)) and the digiCamControl [NikonBase.cs](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs) at the fixed commit `a95ee52bbc1e192a7d1da038f3af6561b185d312` (file SHA-256 in [digicamcontrol-NOTICE.txt](digicamcontrol-NOTICE.txt)). No implementation, header body or linked library is copied or used. Nothing here is taken from Nikon SDK material. Where a line says 未確認 (not checked), no public source recorded here establishes it.

### Rule for a body's answer

A read on the real body may only choose one code from the candidates that a public source above names, and confirm that its type, GetSet and form fit. It never adds a code, a type or a meaning that no public source names, and the application never switches to the other candidate at run time (ADR-0038 decision 13).

### Response code 0x200A

| Fact | Source lines |
| --- | --- |
| 0x200A is `PTP_RC_DevicePropNotSupported`, in the run of standard PTP response codes (0x2002 GeneralError to 0x200B InvalidObjectFormatCode). 0x2019 is `PTP_RC_DeviceBusy` | [ptp.h#L1119-L1129](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1119-L1129) (0x200A at [ptp.h#L1128](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1128)), [ptp.h#L1143](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1143) |

- The name and the number come from the upstream header. The clause of the PTP standard (ISO 15740) that defines the code was not checked: 未確認.
- The application's own policy, not an upstream statement: a code in 0x2002-0x2FFF or 0xA000-0xAFFF (0x2019 excluded) is an explicit refusal. That a body means "this property is not offered" when it refuses the END of a transfer whose begin was accepted with a size of 0 is an inference from the name and the shape. No public source recorded here describes that shape: 未確認.

### ISO auto control: two candidates, and how one is chosen

| Fact | Source lines |
| --- | --- |
| Two property codes are named for Nikon ISO auto control: 0xD054 (`PTP_DPC_NIKON_ISOAuto`) and 0xD16A (`PTP_DPC_NIKON_ISO_Auto`) | [ptp.h#L2397](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2397), [ptp.h#L2596](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2596) |
| Both have their own entry in the upstream configuration table, with the same UINT8 type and the same on/off get and put functions (a nonzero value shows as On; the get function requires a range form) | [config.c#L12088-L12089](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12088-L12089), [config.c#L1228-L1256](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L1228-L1256) |
| Upstream offers an entry only when the body's DeviceInfo lists the property code, so a body can offer one of the two | [config.c#L57-L92](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L57-L92), [config.c#L12717-L12718](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L12717-L12718) |

- Selection on this model: the stage A read (read only) got a response to 0xD054, and 0xD16A was answered with a failed begin and an END refused with 0x200A (the size of 0 is an inference). 0xD100 has not been read on a body: 未確認. The write path reads neither of the two. The fixed product table confirms 0xD054 (never 0xD16A) for ISO auto control, with its rows disabled. The choice follows the rule above: the code has a name in a public source and the body answered it.
- Not established by public sources: that 0xD054 is the body's ISO auto setting (the name `ISOAuto` and the on/off mapping are upstream's), and which Nikon bodies use 0xD054 or 0xD16A. Comparing the body's own menu with the reading is the only check, and no result of it is recorded here: 未確認.
- 0xD100 (shutter speed, Nikon): the code is named at [ptp.h#L2522](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L2522), but the encoding of its value (Bulb and Time marks, unit) is not recorded in any source here. Only 0x500D, whose encoding is in the stage A table above, may be confirmed for writing; the fixed product table confirms it. CAM-B's row is enabled in stage 3 (CAM-A's row stays disabled), and 0xD100 is never in the table.

### ChangeCameraMode (0x90C2) is not sent

| Fact | Source lines |
| --- | --- |
| Name and number: `PTP_OC_NIKON_ChangeCameraMode` 0x90C2, one parameter, no data | [ptp.h#L583](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L583), [ptp.h#L4975](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L4975) |
| Upstream sends it with the given mode in a "Nikon control mode" setting, and with mode 1 before it sets the exposure program to M and the exposure time to Bulb | [config.c#L8512-L8527](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L8512-L8527), [config.c#L9735-L9745](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L9735-L9745) |
| digiCamControl names it the same way and sends 1 (lock) and 0 (unlock) | [NikonBase.cs#L66](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L66), [NikonBase.cs#L2321-L2331](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L2321-L2331) |

- These sources show that other programs send 0x90C2 before some writes. They do not say what the parameter does on a D810, or whether a D810 needs it before the exposure mode is written: 未確認.
- ADR-0038 does not permit it: [DECISIONS.md](../docs/DECISIONS.md), the scope principle ("command" item), decision 2 and decision 14. The write gate admits 0x90C8, 0x1014, 0x1015 and 0x1016 only, and the contract tests send 0x90C2 to both gates and expect a refusal. If a write turns out to need 0x90C2 or another vendor operation, the copy stops and the owner decides.

### Write order is a design decision

- No public source recorded here defines an order across properties. The order is a decision of ADR-0038 (decisions 4, 5 and 14). The table order of `kD810WriteItems` in the code is the write order: exposure mode, ISO auto control (Off), ISO, shutter speed, aperture, and then, under the ADR-0038 revision, white balance and metering.
- Reason given in the ADR: the exposure mode and the ISO auto control come first because ISO, shutter speed and aperture are written only after the body is read as M with ISO auto Off (decision 4). No dependency is recorded between ISO, shutter speed and aperture, so their relative order is a convenience, not a requirement.
- The one upstream sequence seen writes the exposure mode before the exposure time ([config.c#L9735-L9745](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L9735-L9745)). It is a single Bulb routine and is not the source of the order.
- The per-camera order of the Proposed settings screen (CAM-B first, CAM-A last) is a design proposal with no upstream source. It is not implemented: the copy writes CAM-B only.

### SetDevicePropValue (0x1016): the data encoding

| Fact | Source lines |
| --- | --- |
| 0x1016 is `PTP_OC_SetDevicePropValue` | [ptp.h#L245](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L245) |
| The operation carries the property code as its one parameter, and the value goes to the body in a data phase | [ptp.c#L2823-L2837](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.c#L2823-L2837) |
| The value is packed at the width of its declared data type: 1 byte for INT8 and UINT8, 2 for INT16 and UINT16, 4 for INT32 and UINT32 | [ptp-pack.c#L1090-L1120](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp-pack.c#L1090-L1120) |
| PTP over USB is little endian | [ptp.h#L51-L58](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L51-L58) |

- The application writes exactly the width of the type that the body's own GetDevicePropDesc returned for the code in the same operation, little endian, two's complement for signed types (`EncodeD810Value`). The gate compares the byte count and the bytes with the confirmed plan (ADR-0038 decision 9).
- Whether a D810 accepts 0x1016 for each property, and the GetSet value that allows it, are read from the body's description and not assumed. No write has been made on a body: 未検証.

### DeviceReady (0x90C8) as the readiness check, if the owner allows it

| Fact | Source lines |
| --- | --- |
| `PTP_OC_NIKON_DeviceReady` 0x90C8, no parameters, no data; 0x2019 is DeviceBusy ("not yet") | [ptp.h#L589](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L589), [ptp.h#L5217](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L5217), [ptp.h#L1143](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/ptp.h#L1143) |
| Upstream repeats the call while the answer is DeviceBusy before it goes on | [config.c#L650-L668](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L650-L668) |
| digiCamControl defines the same number and calls it as a readiness query, repeating while the answer is "busy" | [NikonBase.cs#L65](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L65), [NikonBase.cs#L2576-L2598](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L2576-L2598) |

- The write channel sends 0x90C8 to wait for readiness (OK or 0x2019 returns to idle). The owner allowed using it for the readiness check; this entry is its source.

### Metering mode names

| Fact | Source lines |
| --- | --- |
| The Nikon driver's own name table for 0x500B: 2 Center-weighted, 3 Multi-pattern, 4 Spot, 0x8010 Highlight-weighted. It has no entry for 1; a value without an entry is shown as its number | [NikonBase.cs#L259-L265](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L259-L265), [NikonBase.cs#L1776-L1795](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L1776-L1795), property code at [NikonBase.cs#L88](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L88) |
| The generic libgphoto2 table behind the names above (1 Average, 2 Center Weighted, 3 Multi Spot, 4 Center Spot) is shared with other makers' values | [config.c#L5068-L5085](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/config.c#L5068-L5085) |

- The application shows the Nikon names above. 1 is shown as a raw value because no Nikon source here names it. The two tables disagree on the names for 3 and 4, and the Nikon-specific one is used. That a D810 itself uses these names is not checked on the body: 未確認.
- The manufacturer's Camera Control Pro 2 online help (Exposure 2 tab) says only that the metering mode is chosen there and does not name the modes, so it is not a source for the names.
