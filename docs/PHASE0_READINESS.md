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

Phase 0は通信専用チャートを使うため、`HG-0001`と`HG-0002`がopenでもPhase 0A/Bを止めない。

## 現在のpreflight証拠

確認日: 2026-08-04

- 正式対象: Nikon D810 2台
- 利用可能台数: 1台。preflightは識別情報を出力せずD810候補PnP nodeを1件検出し、SDK inventoryも`CAM-A / Nikon D810 / 1台`を確認した。
- OS: Windows x64（build 26200）
- MSVC: 14.44.35207がVisual Studio Build Tools配下に存在
- CMake: 3.31.6-msvc6がVisual Studio同梱パスに存在
- .NET SDK: Phase 0では不要。M3前に準備する。
- Nikon SDK: 2026-08-04に本人同意後、公式D810 SDK（2024-05-15版）を`.tools/nikon/d810-remote-sdk`へ隔離配置。archive SHA-256はignored receiptへ記録済み。
- SDK実機確認: 公式x64 sampleでD810を1台列挙し、Source open/closeがexit 0で完了。Phase 0 CLIもlicensed adapterでsingle preflightと匿名inventoryに成功した。SDK内部IDは表示しない。
- SDK撮影結果: D810のCaptureとカメラ側保存は成立したが、SDK adapterと公式sampleの双方でPC転送用SDRAM Itemが生成されなかった。JPEG Fine L、7360×4912、S、M、1/6秒、F8、15秒・60秒待機、強制EnumChildrenを確認してSDK経路を停止した。
- transport決定: 2026-08-04にproduct ownerが`REVISE-WPD`を承認した。
- WPD撮影結果: `run-1785826415773-1`が`Complete`。CAM-Aの新規JPEGをカメラ側から削除せずPCへ保存し、7360×4912、18,107,696 bytes、SHA-256 `09292cecaa9d4f1e9f92dc1367d681070c95510653a0645d93585411ef0ea3e5`を確認した。
- 現在判定: `PHASE0A-WPD-IN-PROGRESS`。一台10回、再接続・電源断・アプリ再起動試験は未完了。

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
| D810 | PnP node 1件、SDK inventory 1台を確認済み |
| 別名 | 実識別子をローカルだけで`CAM-A`へ対応付ける |
| USB構成 | 現在の接続でSDK inventory成功。port情報と実識別子はcommitしない |
| MSVC/CMake | MSVC 14.44.35207、CMake 3.31.6でSDK有効build・CTest成功 |
| Phase 0A実行許可 | 計画実装と最初の実機操作を承認済み。失敗後の次回撮影は新規transactionとして再指示を受ける |

`HG-0003A`は2026-08-04に解消済み。再接続による`CAM-A`維持、10回撮影、切断・電源断・アプリ再起動試験はgate解消条件ではなく、引き続きM1Aの未完了試験として扱う。

## HG-0003B: 二台環境

二台目D810の準備、`CAM-B`割当て、100件撮影、USB切断、電源断、ポート交換を実行できる時点で解消する。現在は`WAITING-HARDWARE`。

## HG-0006: SDK内部評価

2026-08-04、本人がNikon D810 SDK使用許諾への同意を明示した。Codexが公式D810 SDKをダウンロードし、`.tools/nikon/d810-remote-sdk`へ隔離配置した。公式sampleによる一台列挙とSource open/closeも成功した。

`HG-0006`は解消済み。SDK固有のheader、sample、binary、資料およびarchive hash receiptはgitignored領域だけに保持する。

次は内部評価に限定し、別途再配布承認まで禁止する。

- SDKアーカイブ、DLL、LIB、ヘッダー、資料のcommit・push
- SDK配布物を含むinstallerの作成・配布
- 契約内容を推測した公開API・仕様の転記
