# Camera Control Pro 2公開仕様・公開実装との設計比較

調査日: 2026-09-19  
比較基準: `main` `6132f12c3d619a3fa193ff9bead3e972d7d9fe66`、診断改善PR #210 `d767dda8f761af1afacdd419d7395ecd6c3588df`

## 位置づけ

本書はCamera Control Pro 2（CCP2）の内部実装解析ではない。Nikon公式ヘルプで確認できる外部仕様と、公開ソースであるdigiCamControlおよびlibgphoto2を、A0の設計比較材料として整理したものである。公開実装の採用、ライブラリ追加、制御方式変更、実機試験を決定する文書ではない。

確認事実と推論を分離し、A0固有のUSB-only、D810、排他的な単一SDK session、順次撮影、no retry、原本保持、安全なfail-closed契約を優先する。

## 根拠

### Nikon公式（確認事実）

- CCP2は、対応カメラ接続中の撮影画像をPCへ保存し、機種によってはPCとカードへの同時記録も選べる。保存先folderと命名を設定できる。PC+CARD時はカード側の名前に合わせ、重複時はsuffixを付ける。保存後に別アプリ表示等を行う。  
  https://nikonimglib.com/ccp2/onlinehelp/en/02_transfer_options_01.html
- UIは接続中camera name、撮影操作、Live View等を示す。ただし、この外部仕様から内部のthread、callback、転送API、atomic保存方法は確認できない。  
  https://nikonimglib.com/ccp2/onlinehelp/en/07_ccp_window_01.html
- D810がCCP2の制御対象に含まれることは公式ヘルプの機種別操作表から確認できる。ただし、CCP2がD810でどの内部transportや保存検証を使うかは未確認。

### digiCamControl（公開実装。CCP2内部実装ではない）

固定commit: `9269e7851e5130f7d2278cc9942eccde0fd5e593`

- `Capture()`は`CaptureInRam` capabilityを確認し、対応時はSDRAM captureを選び、撮影要求を発行する。  
  https://github.com/dukus/digiCamControl/blob/9269e7851e5130f7d2278cc9942eccde0fd5e593/CameraControlCmd/Program.cs#L461-L476
- `InitApplication()`はcamera接続後に`PhotoCaptured`と`CaptureCompleted`へhandlerを登録する。  
  https://github.com/dukus/digiCamControl/blob/9269e7851e5130f7d2278cc9942eccde0fd5e593/CameraControlCmd/Program.cs#L512-L543
- `DeviceManager_PhotoCaptured()` / `PhotoCaptured()`は画像通知を別threadへ渡し、`TransferFile()`で一時fileへ受信後、出力先へcopyし、session登録後にbusyを解除する。  
  https://github.com/dukus/digiCamControl/blob/9269e7851e5130f7d2278cc9942eccde0fd5e593/CameraControlCmd/Program.cs#L577-L679
- 同CLIには既存出力fileの削除、card format、全camera並列撮影も存在する。これらはA0の既存契約と衝突するため参照対象外であり、取り込まない。

### libgphoto2（公開比較候補。A0の採用決定ではない）

固定commit: `5672d510447bed26aa4c5d646665e768281bc680`

- PTP device infoのoperation一覧を取得し、Nikon vendor extensionと機種群に応じてGetEvent、DeviceReady、Live View、SDRAM/media capture等のoperationを扱う。  
  https://github.com/gphoto/libgphoto2/blob/5672d510447bed26aa4c5d646665e768281bc680/camlibs/ptp2/library.c#L469-L635
- 公開実装は「機種名だけでなくadvertised operationとvendor差異を確認する」比較例になる。一方、Windows 11 x64の現行Nikon SDK経路を置き換える根拠、D810のA0受入証拠、SDK/WPD alias照合の代替証拠にはならない。
- `ptp.h`のvendor opcode/event定義はprotocol調査の索引にはなるが、A0へ直接コピーまたは依存追加する場合はライセンス・対応範囲・Windows運用を別途評価する。  
  https://github.com/gphoto/libgphoto2/blob/5672d510447bed26aa4c5d646665e768281bc680/camlibs/ptp2/ptp.h

## 状態分離による比較

| 状態 | A0現状 | CCP2・公開実装から確認できる比較点 | 判定 |
|---|---|---|---|
| 検出 | Nikon SDK source列挙とWPD D810列挙を別経路で実装。PR #210はWPD列挙数とstill-image compatible数を匿名記録する。 | CCP2は接続camera nameを表示。libgphoto2はdevice infoとoperationsを確認する。 | 実装済み。実機WPD分類は未受入。 |
| 個体照合 | SDK/WPD identity map、alias、binding proofを使用。PR #210はunbound / unavailable / ambiguousを分離する。 | CCP2公式外部仕様から個体照合方式は不明。digiCamControlはcamera propertyとsessionを関連付けるが、A0のcross-transport binding相当の証拠ではない。 | A0固有として実装済み。現在の主要未確認点。 |
| 能力確認 | Live View、SaveMedia、JPEG Fine L、SDRAM empty、WPD still-image capability等を事前確認し、未確認はfail-closed。 | digiCamControlは`CaptureInRam`を確認。libgphoto2はadvertised/vendor operationsを補正・確認。 | 実装済み。capability不一致の匿名診断をPR #210で補強。 |
| 撮影要求 | `ExecutePcDirectCaptureOnce()`からexactly oneのSDK captureを要求し、180秒watchdog、no retry、single session leaseを維持する。 | CCP2はShoot/AF and Shootを提供。digiCamControlは撮影要求を別threadから発行するが、全camera並列経路もある。 | 実装済み。並列設計は不採用。 |
| 画像通知・帰属 | `NikonPcDirectEventWindow`がbaseline、AddChild、CaptureComplete、forced enumeration、candidate除外を記録し、exactly oneへ帰属できる場合だけ進む。 | digiCamControlは`PhotoCaptured` notificationからtransfer handlerへ渡す。 | A0の方がMVP安全契約に対する帰属条件を明示。実機通知順の受入が未完了。 |
| 転送 | SDK itemをPCへ取得するpc-direct経路と、承認済みcard spoolのWPD回収経路を分離。fallbackしない。 | CCP2はPC/CARD/PC+CARDを選択可能。digiCamControlは`TransferFile()`でtempへ取得する。 | 実装済み。CCP2の選択肢を理由にfallbackを追加しない。 |
| 原本保存検証 | `.partial`、JPEG/size、SHA-256、atomic rename、`original.jpg`再読込、byte/hash再検証後に完了。 | Nikon公式はdisk保存後に表示・連携すると説明。公開資料から同等のatomic/hash検証は確認できない。digiCamControlはtemp→copyだがhash再読込検証は確認できない。 | A0で実装済み。維持する。 |
| 後処理 | SaveMedia復元/readback、SDK close、WPD close、process残留確認。card spool時のみ厳格条件後のexact object delete。 | digiCamControlは成功/失敗時にbusyを解除する。利用者報告には転送失敗後のSDRAM残留例があるがD810同一現象の証拠ではない。 | 実装済み。実機復元証拠は未受入。 |
| 下流表示・合成 | canonical original確定後だけ表示・合成へ渡す。 | CCP2はdisk保存後に表示/他アプリ連携する。 | 設計原則が整合。 |

## A0コードの比較起点

`main` `6132f12c...`:

- PC-direct orchestration: `src/phase0/pc_direct_capture.cpp` `ExecutePcDirectCaptureOnce()`  
  https://github.com/AbroadUmedaShota/A0CameraStitcher/blob/6132f12c3d619a3fa193ff9bead3e972d7d9fe66/src/phase0/pc_direct_capture.cpp#L458
- SDK event window: `BeginBaseline()`、`Observe()`、`CanAttributeExactlyOne()`  
  https://github.com/AbroadUmedaShota/A0CameraStitcher/blob/6132f12c3d619a3fa193ff9bead3e972d7d9fe66/src/phase0/nikon_sdk_transport.cpp#L326-L579
- SaveMedia選択・復元とSDK item取得: `OpenPcDirect()`およびpc-direct transport処理  
  https://github.com/AbroadUmedaShota/A0CameraStitcher/blob/6132f12c3d619a3fa193ff9bead3e972d7d9fe66/src/phase0/nikon_sdk_transport.cpp#L815-L1337

PR #210 `d767dda8...`:

- anonymous WPD identity resolution: `ResolveMappedCamera()`  
  https://github.com/AbroadUmedaShota/A0CameraStitcher/blob/d767dda8f761af1afacdd419d7395ecd6c3588df/src/phase0/phase0.cpp#L1952-L1982
- read-only spool diagnosis: `InspectWpdSpoolStatusReadOnly()`  
  https://github.com/AbroadUmedaShota/A0CameraStitcher/blob/d767dda8f761af1afacdd419d7395ecd6c3588df/src/phase0/phase0.cpp#L1984-L2037

## 実装済み・不足・対象外

### 実装済み

- 検出、identity/alias照合、事前能力・状態確認、撮影要求、callback/event window、画像取得、原本確定、復元・closeを別段階として扱う。
- PC直接保存とcard spoolを別transport契約とし、暗黙fallbackを禁止する。
- Live Viewをcapture/WPD sessionと重ねず、選択中の一台だけを扱う。
- exactly-one帰属、`.partial`、SHA-256、atomic publish、再読込検証、no retryを持つ。
- PR #210により、WPD検出・capability・identity照合失敗と、後段spool inspection失敗を区別できるsoftware evidenceを追加した。

### 不足・未確認

- PR #210の匿名count/categoryが実機D810で期待どおり出ること。
- PC-directでのD810実機1回について、通知順、exact-one SDK item、完全JPEG、SaveMedia復元、session close、card payload 0を一つのtransaction evidenceとして完了すること。
- CCP2内部がどのAPI・thread・atomic保存方式を使うか。公開情報だけでは確認不能。
- libgphoto2のD810/Windows適合と、現行Nikon SDKより安全または有利であること。今回は評価対象外。

### MVP対象外または不採用

- CCP2相当の任意PC/CARD/PC+CARDモード切替UI。
- digiCamControlの全camera並列capture、既存出力file削除、card format、無条件busy解除だけを成功証拠にする設計。
- libgphoto2/PTPへのtransport置換、依存追加、vendor opcode直接利用。
- RAW/NEF、同時2台Live View、自動retry、自動fallback、card bulk delete/format。

## 採否理由

- **採る知見**: 検出、能力確認、撮影要求、通知、転送、保存、後処理を独立状態にすること。保存完了後だけ下流へ渡すこと。能力を機種名だけでなく実際のcapability/operationから確認すること。
- **A0ですでに強化済みの知見**: transaction ID、exact-one帰属、watchdog、atomic/hash/reread、exclusive session、no retry、fail-closed。公開比較実装より緩めない。
- **採らない知見**: 並列capture、format、上書き削除、暗黙fallback、公開PTP libraryへの即時置換。A0の安全境界、Windows/Nikon SDK前提、受入済み設計と衝突するため。

## 次の設計判断に使える検証項目

1. PR #210統合後の固定候補で、CAM-A単体read-only spool statusを一度だけ実行し、`enumeratedD810Count`、`stillImageCompatibleCount`、identity category、inventory closeを確認する。
2. 事前条件が全て成立した場合だけ、既承認の1回PC-direct transactionで「撮影要求」と「画像通知・転送・原本確定・復元」を別々に証拠化する。
3. 実機結果がNikon SDK capability/event固有の不足を示した場合にのみ、digiCamControl/libgphoto2の固定commitを比較材料として再参照する。transport変更は別判断とする。

## 未確認事項

- CCP2の内部設計、D810固有のcallback順序、atomic保存・hash検証の有無は不明。
- digiCamControlとlibgphoto2の現在のlicense適用・依存導入条件は、コード利用を検討する時点で別途確認する。今回はURLと設計比較のみ。
- WPD failureのA/B分類はPR #210のsoftware testで確認済みだが、実機確認は未実施。
