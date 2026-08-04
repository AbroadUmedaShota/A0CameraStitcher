# Phase 0 実機成立性検証計画

## 目的

完成アプリの前に、Windows 11 x64とNikon D810をUSB接続し、正規取得したNikon Camera Remote SDKで一台ずつ排他的に撮影・JPEG回収できるかを判定する。

## 開始条件

- Phase 0A前: `HG-0003A`（D810一台、MSVC/CMake、対象PC・USB構成・実行許可）と`HG-0006`（SDK使用許諾の本人同意と内部評価）が解消済み。
- Phase 0B前: `HG-0003B`（二台目D810と二台試験許可）が解消済み。
- `HG-0001`と`HG-0002`はM2のA0品質・最終リグgateであり、通信専用チャートを使うPhase 0を止めない。
- Phase 0ツールはカメラ設定とfirmwareを変更しない。

## Phase 0共通撮影プロファイル

- JPEG Fine L、FX
- 固定露出、Auto ISO無効
- manual focus、固定white balance、VR無効
- single frame、bracketing無効
- 権利確認済みの固定静止チャート
- 実行前のカメラ設定値、firmware、USBポート・ハブ構成を記録

## Phase 0A: 一台先行

### P0-A1: SDKとD810列挙

- SDK版、OS、MSVC、CMakeを記録する。
- D810一台を列挙し、実識別子をローカルで`CAM-A`へ対応付ける。
- 取得可能なcapabilityとfirmwareを匿名化して記録する。
- 切断・再接続後も`CAM-A`を復元する。

合格: D810を安定して`CAM-A`として識別でき、実識別子がcommit対象へ出ない。

### P0-A2: 単体撮影・回収

- セッション開始前に画像Object/eventの基準点を記録する。
- 撮影命令後に唯一の新規JPEGを検出し、PCへ転送する。
- `.partial`保存、JPEG検証、サイズ・SHA-256、原子的renameを確認する。
- 10回連続で自動再試行なしに実行する。

合格: 10/10で撮影・回収・JPEG検証・ハッシュ確定が成功する。

### P0-A3: 単体異常系

- idle中のUSB切断・再接続
- active transaction中のUSB切断
- active transaction中の電源断
- アプリ再起動後の新規transaction

合格: 失敗transactionが`FailedPartial`で確定し、取得済み画像を保持し、復旧後の新規transactionが成功する。

## SDK不成立判定

次のいずれかでSDK経路を停止する。

1. D810を列挙できない。
2. 撮影命令を実行できない。
3. 新規JPEGを一意に検出または転送できない。
4. P0-A2が10/10を満たさない。
5. 文書化された再接続手順で復帰できない。

OS、SDK版、firmware、エラー、再現手順、匿名化ログをまとめ、product ownerがWPD切替を承認するまでWPD調査・実装を開始しない。

## Phase 0B: 二台順次撮影

### P0-B1: 二台識別

- 二台目を`CAM-B`として登録する。
- 接続順変更3回、各カメラのUSBポート交換後も別名が維持されることを確認する。

### P0-B2: 順次二台transaction

- `CAM-A`を開き、撮影・回収・保存・closeを完了する。
- 次に`CAM-B`を開き、同じ処理を完了する。
- 両方が確定した場合だけ`Paired`、`Complete`とする。
- 10件を自動再試行なしで実行する。

合格: 10/10 transaction、CAM-A/B各10枚、誤ペア・消失・曖昧画像採用0件。

### P0-B3: 100件安定性

- 二台transactionを100件連続で実行する。
- transactionごとに両ファイルのサイズ、SHA-256、各状態・時刻を記録する。
- p50、p95、maxを集計するが、Phase 0の合否には使用しない。

合格:

- transaction 100/100
- CAM-A撮影・回収 100/100
- CAM-B撮影・回収 100/100
- 初回試行失敗、自動再試行、誤ペア、原画像消失、曖昧画像の自動採用、回復不能停止が各0件

### P0-B4: 二台異常系

通常100件とは別に次を実行する。

- CAM-A/Bそれぞれのactive中USB切断と電源断を各1回
- CAM-A保存後、CAM-B開始前のアプリ終了を1回
- 接続順変更3回
- CAM-A/BのUSBポート交換を各1回

合格: 対象transactionを失敗確定し、取得済み原画像と曖昧画像を保持し、復旧後の新規transactionが成功する。

## P0判定

1. `GO-SDK-SEQUENTIAL`: Phase 0A/Bの全条件を満たした。
2. `REVISE-WPD`: SDK不成立証拠をproduct ownerが確認し、WPD一台試験への切替を承認した。
3. `STOP`: USBのみでは必要な運用を満たせないとproduct ownerが判断した。

この判定は匿名化レポートを添えてproduct ownerが承認し、ADRへ反映する。

## コミット禁止データ

- 実カメラの完全なシリアル・SDK識別子
- Nikon SDKアーカイブ、DLL、LIB、ヘッダー、仕様資料
- 顧客原稿、個人情報、機密画像
- 実写JPEG/NEFとrawイベントログ

権利確認済み人工チャートだけを `samples/public` の許可範囲へ追加できる。
