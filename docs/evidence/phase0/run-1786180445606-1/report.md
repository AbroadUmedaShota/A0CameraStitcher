# Phase 0 attempted CAM-A swap-back observation

- Run: `run-1786180445606-1`
- Checked: 2026-08-08 18:14 JST
- The operator reported a swap back to the original CAM-A body, with exactly one D810 present.
- Read-only SDK and WPD inventories both still restored CAM-B. WPD also reported firmware `V1.11`, matching the immediately preceding CAM-B checkpoint rather than the historical CAM-A `V1.14` observation.
- The evidence therefore does not establish that the physical body changed. CAM-A binding was not attempted.
- WPD identity was hardened from a PnP device ID digest to a local-only digest of the standard camera-reported `WPD_DEVICE_SERIAL_NUMBER`. Missing, malformed, or duplicate serial identities fail closed.
- SDK-enabled and SDK-less Release CTest each passed 5/5 after the WPD change.
- The former WPD map was quarantined without reading identity values. The currently observed V1.11 body was explicitly rebound to CAM-B under WPD serial identity v2. Its SDK CAM-B identity-v2 map was restored without reading identity values.
- Real serials, identity digests, map hashes, and PnP IDs were not printed or committed. No capture, Live View, setting change, card access, deletion, or vendor operation was performed.
