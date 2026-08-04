# MVPロードマップ

## M0: D810/SDK準備

- D810二台、Nikon SDK優先、排他的順次撮影へ要件を再基準化する。
- MSVC x64とCMakeを検証する。.NET 10はM3まで保留する。
- SDK使用許諾を本人が確認し、正規取得物をリポジトリ外へ配置する。
- Phase 0 CLI、fake transport、transaction・証拠契約を実装する。

完了条件: SDK未接続でも全自動テストが通り、本人同意後に実SDKadapterを接続できる。

## M1A: D810一台 Phase 0

- SDK列挙と匿名別名`CAM-A`
- 10回連続撮影、JPEG回収、原子的保存、SHA-256
- USB切断、電源断、アプリ再起動からの復旧
- SDK不成立時の匿名化報告

完了条件: `PHASE0_TEST_PLAN.md` のPhase 0Aが合格、またはSDK不成立証拠が確定する。

## M1B: D810二台 Phase 0

- 二台目`CAM-B`と接続順・ポート非依存の割当て
- `CAM-A → CAM-B`順次transaction 10件
- 100件連続、全異常系、匿名化レポート
- `GO-SDK-SEQUENTIAL` / `REVISE-WPD` / `STOP`

完了条件: Phase 0判定をproduct ownerが承認する。二台目が揃うまでは`WAITING-HARDWARE`。

## M2: D810オフライン合成PoC

- 7360×4912を基に150/180/200 DPIの光学成立性を計算
- 権利確認済みA0人工チャートと品質オラクル
- カメラ別歪み補正、固定平面warp、残差補正
- 露出・色差補正、seam、multi-band blending、crop
- 品質指標、拒否動作、処理時間計測

完了条件: `HG-0001`、`HG-0002`承認済み条件を満たす。

## M3: 撮影・合成統合MVP

- .NET 10 / WPFアプリ
- 単一C++ Camera AgentとNamed Pipe
- 排他的順次撮影、durable transaction、原画像保持
- 自動合成、再合成、部分失敗・切断復旧

完了条件: 1操作で順次撮影から合成結果まで完了し、失敗時も原画像が保持される。

## M4: 受入・配布

- 統合100件試験、USB異常系、容量不足、クリーンPC導入
- installer、診断手順、操作手順
- Nikon SDKとOpenCVの利用・再配布条件確認

完了条件: product ownerがMVPリリースを承認する。

## MVP後

- TIFF/16-bit、NEF/RAW、Live View、GPU、遠景パノラマ、三台以上
