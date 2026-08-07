# Test layout reservation

Planned suites:

- Unit tests for transactions, pairing, profiles, and quality rules
- Offline integration tests using rights-cleared synthetic image pairs
- Hardware-in-loop tests for D810 SDK Live View, approved single-slot spool capture/recovery/delete/empty-after verification, exclusive SDK-to-WPD handoff, transfer, and reconnect; the software suite also checks read-only all-payload spool aggregation and the SDK-close/WPD-recovery fault gate, but no hardware one-shot, 10/10, fault, or handoff acceptance result is implied by those software tests
- Fake contract tests for SDK stop/close failure blocking WPD, WPD failure skipping resume, uncertain-dispatch candidate quarantine without retry, resume failure preserving the WPD original, preview exclusion, controlled transport-error detail, and anonymous transaction/SDK-status/Live View/handoff report export
- CLI safety contracts for fake-only legacy capture commands, real-hardware command classification, same-Windows-logon-session named mutex contention/release, and pair/hybrid transaction watchdog expiry during capture and before canonical PC rename
- Acceptance scripts implementing `docs/PHASE0_TEST_PLAN.md`
