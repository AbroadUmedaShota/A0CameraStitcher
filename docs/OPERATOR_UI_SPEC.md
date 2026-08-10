# 操作者画面・警告・失敗復旧仕様

## 適用範囲

本仕様は`FR-UI-001`〜`FR-UI-003`の画面契約である。WPFには`SIMULATED / 実機未接続`のmode-aware shellと、明示起動する`HardwareSingleCamera`画面がある。後者はCamera Agent protocol、local pending state、canonical original再検証、明示exportまでsoftware boundaryを実装するが、実D810、WPD/SDK、actual JPEG、実画面操作の受入証拠はまだない。

## 画面構成

日常操作は`撮影ダッシュボード`一画面に集約する。`設置・校正`、`カメラ設定`、`保存・診断`は保守タブに分け、active transaction中は移動を禁止する。カメラ設定はread-onlyで、実機write契約が承認されるまで変更ボタンを提供しない。

ダッシュボードは次を常時表示する。

- 明示選択した`SingleCamera`または`DualCamera`、required aliases、総合状態、起動セッションの排他同意、profile ID・版・期限、保存先
- modeが要求するcameraの接続、identity、read-only設定整合、card、Live View。`SingleCamera`では非required cameraの不在をBlockerにしない
- 一台選択式Live Viewと「非原画像・非合成入力」の表示
- 設置判定、予定自動補正、必要な物理調整
- Live View停止、required cameraの撮影・保存、および`DualCamera`だけの自動合成進捗。`SingleCamera`では合成を`NotApplicable`と表示
- 撮影、保持原画像、合成、保存を分離した共通結果領域
- 赤Blocker、黄Caution、青Infoと、展開式のerror code・ログ情報

## 標準操作順

1. 起動時に未完了状態を検査し、新しい撮影を開始しない。SIMULATEDの残留journalは撮影を再実行せず`FailedPartial`へ閉じる。Hardwareではlocal pendingに固定した同じtransaction ID・required alias・profile ID/version/SHA/expiry・handoff intentで`get-transaction-result`だけを行う。既知のpre-dispatch状態だけは「撮影要求0回」として明示的に閉じられるが、dispatch済みの`TransactionNotFound`はpendingを保持してsupport-requiredとし、自動で新規撮影可能にしない。
2. 操作者は「物理シャッターを操作しない」「他のカメラアプリを使わない」に起動セッション単位で同意する。同意は永続化せず、アプリ終了時に失効する。
3. 操作者はactive transaction外でmodeを明示選択する。mode変更時はreadinessを破棄して、接続、identity、profile、設置、card、保存先をread-onlyで再検査する。Blockerが一件でもあれば撮影ボタンを無効にし、直下へ最初の理由を表示する。
4. `Ready`または`ReadyWithCorrection`では、追加ダイアログなしに撮影ボタンの一回押下で開始する。後者は予定補正量を常時表示する。
5. 直ちにmode変更を含む全競合操作をロックし、選択中Live Viewを停止してSDK session closeを確認する。停止できなければシャッター処理へ進まない。現`hardware.v1`の画面内previewは一回ごとに有限frameを取得して停止・closeするため、継続streamが動作中とは表示しない。
6. `SingleCamera`は選択alias一台だけを処理し、canonical original確定後にReviewへ進み、`StitchOutcome=NotApplicable`とする。`DualCamera`はCAM-A/Bを順次処理し、両PC原本の検証と明確な帰属が成立した場合だけ自動合成する。SIMULATED実装は同じ状態契約だけを検証する。
7. 結果を共通領域で確認し、操作者が`保存`を押した場合だけ出力する。実`SingleCamera`はcanonical originalを画像処理せず明示copyし、合成済みとは表示しない。SIMULATED版はJPEGに見せない`.simulated-export.txt`を一時フォルダへ出力する。
8. `新しい撮影を準備`はreadinessのread-only再検査だけを行う。過去の片側画像を再利用せず、次回押下時に新しいtransaction IDを生成する。

## UI状態遷移

| 状態 | 意味 | 撮影 |
| --- | --- | --- |
| `AwaitingSafetyAck` | 起動時同意待ち | 禁止 |
| `CheckingReadiness` | read-only状態検査中 | 禁止 |
| `NotReady` | Blockerあり | 禁止 |
| `Ready` | 補正不要 | 許可 |
| `ReadyWithCorrection` | 承認済み範囲内の補正予定 | 許可 |
| `Capturing` | Live View停止〜modeが要求する原本確定 | 禁止・mode変更を含む全競合操作をロック |
| `Stitching` | `DualCamera`自動合成中。`SingleCamera`では遷移しない | 禁止・全競合操作をロック |
| `Review` | 結果確認・明示保存待ち | 新規撮影は`新しい撮影を準備`後 |
| `FailedPartial` | 同じtransactionを再開しない終端失敗 | 新規撮影は`新しい撮影を準備`後 |
| `Degraded` | 結果は保持したがSDK/card状態要確認 | 安全再確認まで禁止 |

`OperatorReadinessEvaluator`が`ReadinessSnapshot`から警告とready状態を作り、`OperatorActionAvailability`が撮影、Live View、保存、再合成、新規撮影準備、保守画面移動の可否と理由を一元管理する。

## 警告

| レベル | 条件 | 動作 |
| --- | --- | --- |
| 赤 / Blocker | mode不一致、required camera不足、identity未登録、設定不整合、profile未承認・期限切れ、物理調整必要、card非empty/不明、保存先不正、active transaction、SDK/card要確認 | 撮影禁止。スクリーンリーダーへassertive通知 |
| 黄 / Caution | 自動補正範囲内、Live View停止予定、撮影後の有限probe失敗、cleanup異常 | 予定処置を常時表示。安全な結果操作だけ許可 |
| 青 / Info | Live Viewは非原画像、PC原本を保持、実シャッター時刻差は非保証 | 常時説明 |

操作者向け説明を先に出し、技術情報は展開領域へ分離する。色だけに依存せず`Blocker`、`Caution`、`Info`の文字を併記する。

## 失敗と戻り方

| 失敗点 | 保持 | 戻り方 |
| --- | --- | --- |
| Live View停止 | 原画像なし、シャッター未実行 | `FailedPartial`。安全停止確認後、新しいtransaction |
| `SingleCamera`原本確定前 | 原画像なし | `FailedPartial`。新しいsingle transaction |
| `DualCamera` CAM-A原本確定前 | 原画像なし | `FailedPartial`。両方を新規撮影 |
| `DualCamera` CAM-A確定後 | CAM-A保持 | 過去CAM-Aを再利用せず、両方を新規撮影 |
| 両原本確定後のcleanup | 左右原画像と合成結果を保持可能 | card状態を再確認するまで新規撮影禁止 |
| `DualCamera`自動合成 | 左右原画像を保持 | `再合成`を別stitch job IDで実行。撮影transactionは変更しない |
| `SingleCamera` | canonical originalを保持 | 合成・再合成を開始せず`NotApplicable`。明示保存だけを許可 |
| 明示保存 | 原画像・合成結果を保持 | 保存先を直して再度明示保存 |
| 撮影後Live View確認 | 撮影・合成結果を保持 | 現`hardware.v1`は有限一frame probe後に停止・SDK closeする。probe失敗は`FailedPartial`または`Degraded`として新規撮影を禁止し、継続stream再開の成功とは表示しない |
| アプリ終了・再起動 | 発見した確定原画像を保持 | 未完了journalを`FailedPartial`へ閉じ、自動再開しない |

## 常時禁止

- 同じtransactionの再試行・再開、過去の片側画像との自動ペア
- SDK/WPD session重複、同時二台Live View、ハードウェア同期の保証
- Live View previewの原画像・合成入力への採用
- existing cardの削除、bulk delete、format、vendor operation `0x9207`
- PC原本の検証前のcamera-object削除
- identity未登録、未承認profile、補正上限超過での撮影
- active transaction中の設定、校正、保存先変更、保守操作
- active transaction中のmode変更、接続台数によるmode自動変更、`DualCamera`から`SingleCamera`への自動降格
- `SingleCamera`での撮影設定write、二台接続中の片方だけを使う初期運用、単一原画像を合成済みと表示すること
- 原画像の上書き、自動削除、自動再試行

## 自動試験と受入境界

- 2026-08-10 fresh software検証: .NET 10 Release build 0 warning/0 error、Foundation 19/19、Operator Shell 16/16、`Test-M3Simulated.ps1` Pass。CAM-A-only、起動同意、no-auto-fallback、30日read-only profile承認、fixed-local保存先、active中の操作ロック、Singleのstitch `NotApplicable`と明示保存を確認した。
- Hardware Single headless contractは、起動時全操作gate、同一transactionの結果照会、profile/alias/expiry/handoff相関、pre-dispatchと曖昧dispatchの分離、no retry、有限Live View previewの非原本性、CAM-A identity-v3、30日profile、fixed-local preference、same-file-identity byte-identical exportを含み、Operator Shell 16/16で確認した。C++側もSDK-less／licensed-SDK-enabled Release CTest各7/7に合格したが、camera commandは送っていない。有限probeは製品の継続Live View受入ではない。
- 一括検証は外部NuGetなし、SIMULATED常設表示、警告レベル、accessibility live region、Foundation facade以外のcamera API不使用を静的・headlessに確認する。2026-08-08のWindows UI Automationは旧Dual contractの履歴証拠であり、requirements 2.6.0のSingle実画面受入へ読み替えない。screen reader、キーボード、focus、実WPF Hardware Single操作、実Camera Agent/D810の各失敗点は未実施である。

## 要件追跡

| 要件 | 仕様・実装 |
| --- | --- |
| `FR-UI-001` | ヘッダー、camera/setup/progress/result、`ReadinessSnapshot`、警告三段階 |
| `FR-UI-002` | 一回撮影、設置・校正、read-only設定、別job再合成、明示保存、物理調整案内 |
| `FR-UI-003` | 一台選択式Live View、開始停止、非原画像表示、停止失敗と撮影後有限probe失敗。継続stream再開は実機受入まで未検証 |
