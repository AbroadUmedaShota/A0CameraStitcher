# Phase 0 開始判定とhuman gate回答票

## 判定ルール

| Gate | 決定者 | 必要時点 | 解消条件 |
|---|---|---|---|
| `HG-0001` A0出力品質 | product owner | M2前 | D810二台の光学成立性と品質契約を承認 |
| `HG-0002` 最終リグ | product owner | M2前 | 配置、レンズ、距離、重複、照明を承認 |
| `HG-0003A` 一台環境 | engineer-admin | Phase 0A前 | D810一台、MSVC/CMake、対象PC、USB、実行許可 |
| `HG-0003B` 二台環境 | engineer-admin | Phase 0B前 | 二台目D810と二台異常系を含む実行許可 |
| `HG-0004` transport決定 | product owner | WPD実装前 | 2026-08-04解消。SDK失敗証拠を確認し`REVISE-WPD`を承認 |
| `HG-0006` SDK内部評価 | requester | SDK取得前 | 2026-08-04解消。本人同意、公式取得、ignored local配置を確認 |
| `HG-0007` 標準WPD不成立後のtransport判断 | product owner | P0-A2 10/10前 | 解消済み。one-shot hybrid（WPD baseline/close → SDK one card capture/close → WPD recovery）を明示承認 |
| `HG-0008` dedicated spool/delete判断 | product owner | P0-A2再開前 | 2026-08-06解消。専用empty/cleared cardをsingle-slot spoolとして使い、PC原本の再読込検証後にexact just-recovered WPD objectだけを削除し、empty-afterを確認することを明示承認 |

Phase 0は通信専用チャートを使うため、`HG-0001`と`HG-0002`がopenでもPhase 0A/Bを止めない。

> **現在の経路:** 承認済みhardware captureはdedicated single-slot spoolだけである。以下に残る過去のdirect WPD成功や旧handoffは原因調査の履歴証拠であり、現在の合格経路ではない。Phase 0 CLIの旧`capture-single/pair/stability`はfake-onlyとなり、実SDK/WPD指定をcamera open前に拒否する。

## 現在のpreflight証拠

確認日: 2026-08-05

- 正式対象: Nikon D810 2台
- 利用可能台数: 1台。preflightは識別情報を出力せずD810候補PnP nodeを1件検出し、SDK inventoryも`CAM-A / Nikon D810 / 1台`を確認した。
- OS: Windows x64（build 26200）
- MSVC: 14.44.35207がVisual Studio Build Tools配下に存在
- CMake: 3.31.6-msvc6がVisual Studio同梱パスに存在
- .NET SDK: Phase 0では不要。M3前に準備する。
- Nikon SDK: 2026-08-04に本人同意後、公式D810 SDK（2024-05-15版）を`.tools/nikon/d810-remote-sdk`へ隔離配置。archive SHA-256はignored receiptへ記録済み。
- SDK実機確認: 公式x64 sampleでD810を1台列挙し、Source open/closeがexit 0で完了。Phase 0 CLIもlicensed adapterでsingle preflightと匿名inventoryに成功した。SDK内部IDは表示しない。
- SDK撮影結果: D810のCaptureとカメラ側保存は成立したが、SDK adapterと公式sampleの双方でPC転送用SDRAM Itemが生成されなかった。JPEG Fine L、7360×4912、S、M、1/6秒、F8、15秒・60秒待機、強制EnumChildrenを確認してSDK経路を停止した。
- transport決定: 2026-08-04の`REVISE-WPD`は履歴として保持する。標準WPD不成立後、product ownerはone-shot hybrid（WPD baseline/full close → SDK exactly-one card capture/full close → WPD reopen/no capture command/exactly-one JPEG recovery）を承認した。
- WPD撮影結果: `run-1785826415773-1`が`Complete`。CAM-Aの新規JPEGをカメラ側から削除せずPCへ保存し、7360×4912、18,107,696 bytes、SHA-256 `09292cecaa9d4f1e9f92dc1367d681070c95510653a0645d93585411ef0ea3e5`を確認した。
- SDK Live View実機確認: `run-1785834861034-1`で一台の10 frame取得を確認した。手動handoffはWPD撮影`run-1785834883573-1`と撮影後Live View`run-1785834891891-1`で成立し、`run-1785834999815-1`で自動handoff 1/1成功を確認した。`run-1785835030476-1`は最初の5回成功後、6回目にWPD `image_event_timeout`とLive View再開hangを記録したため、連続試験はPartialである。
- 2026-08-05復旧確認: Windows PnP、SDK、WPDの各匿名inventoryでD810一台を確認した。`run-1785893397010-1`はLive View 1 frame（17,546 bytes）、stop、SDK session closeを完了し、previewを保存していない。
- 2026-08-07一台再検証: licensed SDK inventoryでD810一台を`CAM-A`として再確認した。[設定read-only結果](evidence/phase0/run-1786077278290-1/report.md)はJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、設定変更0、Live View開始0、session closeを記録した。[短時間Live View結果](evidence/phase0/run-1786077291889-1/report.md)は10 frame、最終16,397 bytes、preview保存なし、stop、session closeを完了し、直後の[再確認](evidence/phase0/run-1786077302493-1/report.md)でLive View OFF、prohibit mask 0、設定変更0、session closeを確認した。
- 2026-08-05失敗証拠: `run-1785893437110-1`はSDK close後だけWPDへ移行したが、最初のWPD撮影が`capture_command_failed`となった。0/1件、`FailedPartial`、Live View resumeは仕様どおりskipし、自動再試行していない。
- WPD target互換性証拠: omit targetの`run-1785897041881-1`、functional targetの`run-1785898095864-1`、read-write/impersonation指定後の`run-1785901766678-1`はいずれも`SendCommand`自体は`S_OK`、common HRESULTは`E_FAIL`だった。各1件を`FailedPartial`で終了し、画像保存0、自動fallback・再送信なしである。読み取り専用`wpd-status`はcommand options query `S_OK`、functional object 1件、compatible target 1件、capture command未送信を確認した。
- 電源再投入後の状態: 2026-08-05 14:38:22+09に到着したD810を、`run-1785908501354-1`で`S / photo / Live View off / prohibit 0 / session closed`と匿名確認した。カメラ設定とLive View状態は変更していない。
- 電源再投入後のMTP root cause: ETW付き[run-1785908732670-1](evidence/phase0/run-1785908732670-1/report.md)は標準MTP `InitiateCapture`（`0x100E`、parameter `0,0`）を送信し、4453.125ms後に`0x2002 GeneralError`を受信した。`SendCommand`は`S_OK`、Windows common HRESULTは`E_FAIL`、standard ObjectAdded 0件、新規JPEG 0件、`FailedPartial`、自動再試行なしで終了した。raw ETLはgitignoredで、匿名summaryだけを保存した。
- MTP広告能力: query-only [run-1785908518274-1](evidence/phase0/run-1785908518274-1/report.md)は34件と`0x9207`広告有無を匿名集計した。撮影command、vendor operation、設定変更、画像転送・削除は0件である。標準`0x100E`の広告有無はこのvendor APIでは判定できない。
- 匿名report: [SDK状態](evidence/phase0/run-1785908501354-1/report.md)、[vendor opcode広告](evidence/phase0/run-1785908518274-1/report.md)、[電源再投入後のMTP応答付きWPD失敗](evidence/phase0/run-1785908732670-1/report.md)。過去の証拠も`docs/evidence/phase0`に保存する。実写画像、preview、実識別子、SDK配布物は含めていない。
- hybrid failure: [run-1785914842210-1](evidence/phase0/run-1785914842210-1/report.md)はWPD baselineが10.385秒後に`baseline_timeout`、SDK open/capture前、0/1件の`FailedPartial`、自動retry/delete 0だった。
- correlation診断: read-only [run-1785917005306-1](evidence/phase0/run-1785917005306-1/report.md)はWPD full close/reopen 3 sample（1500ms）、datetime 3/3 available、session close 3/3、terminal `Complete`を確認したが、advance 0/equal 2、JPEG date 240/240、latest object date > device time 3/3だった。capture、vendor operation、settings、deleteは0件で、device datetime cutoffは製品帰属契約から撤回する。先行`run-1785915695600-1`も同じdatetime結果を保持する。
- identity continuity: 電源再投入後の[run-1785917466375-1](evidence/phase0/run-1785917466375-1/identity-continuity-summary.json)は、再投入前から存在するSDK/WPD local identity mapが再列挙後も変更されず、両transportが一台のD810を`CAM-A`へ復元したことを匿名記録した。実識別子とmap hashは含めていないため、一台構成のP0-A1を合格とする。
- 5分Live View: [run-1785917554163-1](evidence/phase0/run-1785917554163-1/report.md)は304,347msで2,424 frameを取得し、停止、SDK session close、preview非保存に成功した。続く別プロセスの[run-1785917887961-1](evidence/phase0/run-1785917887961-1/report.md)も1 frame取得、停止、close、preview非保存に成功し、最終`run-1785917904556-1`はLive View `off`、session close、設定変更なしを確認した。Standalone Live Viewは合格、hybrid handoff 10回は未実施である。
- 承認済みの次経路: dedicated empty/cleared cardをsingle-slot transient spoolとして使い、PC `.partial`、JPEG・size検証、SHA-256、atomic `original.jpg`確定、再読込検証後にexact just-recovered WPD objectだけを削除し、empty-afterを確認する。候補0件・複数件・遅延・無効画像、download/persist/delete失敗では削除せず、PC原本があれば保持して`FailedPartial`にする。existing cardのbulk delete/format、vendor operation、retryは禁止する。
- 全payload preflight: [run-1786014841232-1](evidence/phase0/run-1786014841232-1/report.md)は90 camera payload objectを検出し、SDK open、shutter、PC保存、delete、retryを0のまま`spool_not_empty` / `FailedPartial`で終了した。既存内容は自動削除せず、dedicated empty cardへの交換または操作者によるbackup・手動clearを待つ。
- read-only spool status: [run-1786015997366-1](evidence/phase0/run-1786015997366-1/report.md)は全payload 90件、`NON_EMPTY`、WPD session close 1/1を匿名記録した。Object ID・名前・実識別子は含めず、capture command、camera delete、vendor operationは0件である。
- final blocked audit: read-only [run-1786017282044-1](evidence/phase0/run-1786017282044-1/report.md)も全payload 90件、`NON_EMPTY`、WPD session close 1/1、capture/delete/vendor operation 0件だった。同じ物理阻害条件が3回連続したため、専用empty cardへの交換またはbackup・手動clearまでM1Aをブロックする。
- 一台設定read-only: [run-1786040075194-1](evidence/phase0/run-1786040075194-1/report.md)はCAM-AからJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得し、撮影設定write、capture、Live View開始、WPD、deleteなしでSDK sessionを閉じた。FileTypeはnot-advertised。SDK control-plane callback登録は既存`CapSet`を使い得ることをsummaryへ明示し、撮影設定read-onlyと区別した。native command-trace testとfocus意味確定は未完了のためPartialである。
- software確認: SDK有効・SDKなしの両Debug buildと全CTestを維持する。empty-spool aggregate、SDK close後/WPD recovery前の異常系operator gate、WPD reopen失敗時の`FailedPartial`、delete/retry 0、匿名fault summaryに加え、direct hardware capture拒否、process-wide named lease、pair/hybrid watchdogを契約化した。既存のstop/close失敗時のWPD未開始、WPD失敗時のresume skip、resume失敗時の原画像保持、preview非保存、匿名error detail、uncertain dispatch候補のquarantine、SDK状態summary、Live View/handoff reportも維持する。M3 simulated Release buildは警告0・エラー0で検証済みである。
- 現在判定: `PHASE0A-DEDICATED-SPOOL-EMPTY-REQUIRED`。P0-A1とStandalone 5分Live View・別プロセス再起動は合格済み。ADR-0020のsoftware contractと全payload fail-closed preflightは合格したが、接続中cardに90 payload objectがあるためone-shot前で停止中である。P0-A2のone-shotと10/10、A3、A4の10回handoffは未実施で、M1Aは未完了。vendor operation、existing cardのbulk delete/format、retryは実装・実行しない。

```powershell
pwsh -File .\scripts\Test-Phase0Readiness.ps1 -Stage Single
```

## HG-0001: D810 A0品質契約

| 項目 | 回答 |
|---|---|
| 代表原稿と利用目的 | `PENDING` |
| 必須の向き | `PENDING` |
| 150/180/200 DPI候補のうち必要な最低実効DPI | `PENDING` |
| 最終JPEG幅×高さ | `PENDING` |
| クロップ・内容欠損許容 | `PENDING` |
| 位置ずれ・色差・継ぎ目の測定法と許容値 | `PENDING` |
| p95処理時間（暫定10秒） | `PENDING` |
| 合否判定者 | `PENDING` |

D810 JPEG Fine Lの最大記録画素は7360×4912。M2で二台の画角・重複・クロップから実効DPIを計算し、人工チャート実写と合わせて承認する。

## HG-0002: 最終リグ

| 項目 | 回答 |
|---|---|
| 配置、カメラ向き | `PENDING` |
| レンズ2本、焦点距離 | `PENDING` |
| 原稿面までの距離 | `PENDING` |
| 重複率と測定方法 | `PENDING` |
| 露出、focus、white balance、VR | `PENDING` |
| 照明と外光遮断 | `PENDING` |
| A0人工チャート | `PENDING` |

## HG-0003A: 一台環境

| 項目 | 現在値・回答 |
|---|---|
| 対象PC | このWindows x64 PC |
| D810 | 2026-08-05にPnP node 1件、SDK/WPD inventory各1台を再確認済み |
| 別名 | 一台だけ接続した状態でSDK/WPD双方の実識別子をローカルだけで同じ`CAM-A`へ対応付ける |
| USB構成 | 現在の接続でSDK inventory成功。port情報と実識別子はcommitしない |
| MSVC/CMake | MSVC 14.44.35207、CMake 3.31.6でSDK有効build・CTest成功 |
| Phase 0A実行許可 | 計画実装と最初の実機操作を承認済み。失敗後の次回撮影は新規transactionとして再指示を受ける |

`HG-0003A`は2026-08-04に解消済み。電源再投入後の`CAM-A`維持は`run-1785917466375-1`で合格した。10回撮影とactive transaction中の切断・電源断・撮影経路のアプリ再起動試験は、引き続きM1Aの未完了試験として扱う。

## HG-0003B: 二台環境

二台目D810の準備、一台ずつ接続してSDK/WPD identityを同じ`CAM-A/B`へ登録するcross-transport binding、100件撮影、USB切断、電源断、ポート交換を実行できる時点で解消する。現在は`WAITING-HARDWARE`。

## HG-0006: SDK内部評価

2026-08-04、本人がNikon D810 SDK使用許諾への同意を明示した。Codexが公式D810 SDKをダウンロードし、`.tools/nikon/d810-remote-sdk`へ隔離配置した。公式sampleによる一台列挙とSource open/closeも成功した。

`HG-0006`は解消済み。SDK固有のheader、sample、binary、資料およびarchive hash receiptはgitignored領域だけに保持する。

次は内部評価に限定し、別途再配布承認まで禁止する。

- SDKアーカイブ、DLL、LIB、ヘッダー、資料のcommit・push
- SDK配布物を含むinstallerの作成・配布
- 契約内容を推測した公開API・仕様の転記
