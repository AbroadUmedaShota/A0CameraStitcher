# Phase 0 二台カメラ・ショーケース

更新日: 2026-08-09

## 一行で表す現在地

**二台のD810をCAM-A→CAM-Bの順に安全に扱うsoftware contractはデモ可能。実機二台のidentity、empty spool、1/10/100組撮影の受入は未完了。**

現在接続されている実機はidentity-v2でSDK/WPD双方に登録済みの`CAM-B`一台です。`CAM-A`のidentity-v2登録がないため、実機pair入口は撮影やcard accessより前にfail closedします。

## 今見せられるもの

| 項目 | 状態 | 根拠 |
|---|---|---|
| 二台順次撮影contract | Software Pass | 1/10/100組、CAM-A→CAM-B、100/100 synthetic pair、retry 0、session overlap 0 |
| A側失敗時の停止 | Software Pass | CAM-Bを開始しない |
| B側失敗時の保持 | Software Pass | CAM-Aの検証済みPC originalを保持し、Bの未確定original化・delete・retryをしない |
| pair境界でのprocess終了 | Software Pass | CAM-A完了後・CAM-B開始前を復元し、CAM-A保持・新規transaction要求 |
| operator-session lease / watchdog | Software Pass | 同一Windows logon session内を直列化し、pair全体を180秒でfail closed |
| 実機identity gate | One-camera evidence | SDK/WPDともCAM-B一台だけと匿名確認し、撮影・card accessなしで拒否 |
| 実機二台撮影 | Unverified | CAM-A identity-v2登録、dual spool、1/10/100組が未実施 |
| A0光学品質 | Unverified | 最終リグ、補正上限、品質閾値が未承認 |

## 5分の安全なデモ

このデモはカメラを開かず、撮影、Live View、設定変更、card access、deleteを一切行いません。

```powershell
pwsh -NoProfile -File .\scripts\Test-Phase0Showcase.ps1
```

スクリプトはSDK有効・SDKなしの既存buildをReleaseで再buildし、両方のCTestを実行します。その後、公開対象の匿名証拠と「実機二台撮影未実施」という境界を検証し、次の状態を表示します。

```text
ShowcaseState                    SOFTWARE_CONTRACT_READY_HARDWARE_PENDING
RealDualCaptureExecuted          False
LatestHardwareState              SINGLE_CAM_A_IDENTITY_V3_BOUND_SDK_STATUS_RERUN_PENDING
ActualShutterSyncGuaranteed      False
PhysicalPowerCycleRequired       False
```

buildを再実行せず資料だけを確認する場合は`-SkipBuild`を指定できます。

## 厳選した証拠

- [二台capture software contract](evidence/phase0/run-1786174618989-1/hybrid-pair-contract-summary.json): 100/100 synthetic pair、初回失敗停止、retention、watchdog、非同期保証を匿名集約。
- [CAM-B cross-transport binding](evidence/phase0/run-1786182705745-1/cross-transport-binding-command-summary.json): SDKを完全closeしてからWPDを確認し、既存CAM-B bindingと一致。撮影・card accessなし。
- [dual spool gateの安全停止](evidence/phase0/run-1786183481065-1/dual-spool-verification-summary.json): dual identity未成立のためcard inspection前に停止。
- [最新dual identity確認](evidence/phase0/run-1786184898081-1/dual-identity-verification-summary.json): SDK/WPD各一台、双方CAM-B、CAM-A 0、実識別子なし、`camera_count_mismatch`。

証拠JSONにはcamera serial、SDK内部identity、object ID、画像を含めません。SDK配布物と実写画像もrepositoryへcommitしません。

## デモで主張しないこと

- 実機二台のpair撮影が完了したとは言いません。
- 二台の実シャッター同期は保証しません。設計は順次撮影です。
- software-only、simulated、一台実機の結果を、A0光学品質やMVP受入へ読み替えません。
- 旧SDK `CAM-A` continuityは有効な証拠として扱いません。ephemeral source ID依存が判明したため無効化済みです。
- Nikon SDKやlicensed binaryの再配布可否は未承認です。

## 実機二台で残る受入順序

1. 元`CAM-A`本体だけを接続し、SDK/WPD identity-v2を同一operator-session lease内で明示登録する。
2. CAM-A/Bを同時接続し、接続順とUSB portを変えてもSDK/WPD各2、CAM-A/B各1、unbound 0であることを確認する。
3. 二つの専用spoolがpayload 0であることをread-only確認する。
4. 1組、10組、100組を順に実行し、PC原本、exact-object cleanup、停止条件、180秒watchdogを記録する。
5. USB切断とsoftware process再起動／CAM-A-CAM-B境界中断を実行し、保持・no retry・新規transactionを確認する。

操作者判断`HD-20260809-001`により、物理的なcamera/PC power-cycle・rebootと実`power-off`復旧subtestはPhase 0必須合否から除外されています。USB切断、software process再起動、接続順・port確認は除外されていません。

## 安全境界

- SDKとWPD sessionを重ねません。camera sessionとcapture transactionは常に一つです。
- PCの`original.jpg`だけを製品上の正本とし、`.partial`、JPEG/size検証、SHA-256、atomic rename、再読込検証を通します。
- delete可能なのは、検証済みPC originalに対応するjust-recovered exact WPD objectだけです。
- zero/multiple/late/invalid/download/persist/delete failureでは自動retryせず、診断物を保持して`FailedPartial`にします。
- bulk delete、format、vendor operation `0x9207`は行いません。

詳細な全体状態は[現在の開発状況](CURRENT_STATUS.md)、実機手順は[Phase 0実機検証計画](PHASE0_TEST_PLAN.md)を参照してください。
