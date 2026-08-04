# アーキテクチャ

## 設計方針

MVPは「Windowsアプリ」「単一カメラ制御エージェント」「画像合成エンジン」を分離する。Phase 0は.NETへ依存しないC++20コンソールとして、D810用Nikon Camera Remote SDKの順次制御を検証する。

```text
┌────────────────────────────────────────────┐
│ A0CameraStitcher.App (.NET 10 / WPF, M3)  │
│ UI / CaptureCoordinator / Session / Store │
└───────────────────┬────────────────────────┘
                    │ Named Pipe
┌───────────────────▼────────────────────────┐
│ CameraAgent (C++20, one active session)    │
│ CAM-A capture/download -> close            │
│ CAM-B capture/download -> close            │
└───────────────────┬────────────────────────┘
                    │ Nikon Remote SDK / USB
              Nikon D810 A / B

┌────────────────────────────────────────────┐
│ StitchEngine (C++20 / OpenCV, M2)         │
│ calibration / warp / color / seam / blend │
└────────────────────────────────────────────┘
```

## Phase 0ツール

`A0CameraStitcher.Phase0.exe` は次の境界を持つ。

- `ICameraTransport`: 列挙、セッション開始、基準点取得、撮影、JPEG取得、セッション終了。
- `CaptureCoordinator`: 単一進行トランザクションと`CAM-A → CAM-B`の状態遷移。
- `EvidenceWriter`: 原画像の原子的保存、SHA-256、JSONLイベント、匿名化レポート。
- `NikonSdkTransport`: 正規取得したD810用SDKをリポジトリ外から読み込むadapter。
- `WpdTransport`: 承認済みfallback。Windows Portable Device APIで撮影前後のJPEG Object差分を検出し、カメラ側を削除せずPCへ取得するadapter。
- `FakeCameraTransport`: SDK不要のtransaction・失敗系テスト用。

SDK APIは、本人同意後に正規取得したlocal資料と公式sampleで確認した範囲だけをadapterへ反映する。SDK binaryは配置元から動的loadし、copy・link・再配布しない。

## 撮影トランザクション

```text
Idle
 -> CaptureA: open CAM-A / baseline / capture / exactly-one JPEG
 -> PersistA: .partial / validate / SHA-256 / atomic rename / close
 -> CaptureB: open CAM-B / baseline / capture / exactly-one JPEG
 -> PersistB: .partial / validate / SHA-256 / atomic rename / close
 -> Paired
 -> Complete
```

どの段階でもtimeout、切断、複数候補、既存・遅延画像を検出した場合は`FailedPartial`へ遷移する。自動再試行・同一トランザクションの再開・曖昧画像の自動帰属は行わない。取得済み原画像と曖昧画像は削除しない。

画像帰属は、active Source sessionで撮影前`Children`をbaseline化し、撮影開始後かつon-card完了を示す`CaptureComplete(data=1)`より前に現れた新規Itemだけを候補とする。完了後のItem、消失したItem、完了通知なし、候補0件・複数件は採用しない。完了後500msは追加eventを監視し、遅延候補を検出する。

camera aliasは列挙順で決めず、local identity mapから解決する。現在のidentity候補はSDK Source IDをhash化したものであり、再接続・port変更後の永続性はNikon資料で保証されていない。Phase 0A/B実測に合格するまで暫定方式とする。

## timeout初期値

- SDK open: 10秒
- image event: 15秒
- JPEG download: 60秒
- SDK close: 10秒
- 二台トランザクションwatchdog: 180秒

timeoutは安全停止値であり、Phase 0性能合否値ではない。すべて実行レポートへ記録する。

## 保存

- ローカル実識別子対応表: `%LOCALAPPDATA%\A0CameraStitcher\phase0\camera-map.json`
- raw証拠: `artifacts/phase0/<run-id>/`
- 曖昧画像: `artifacts/phase0/quarantine/<run-id>/<transaction-id>/<alias>/`
- commit可能レポート: `docs/evidence/phase0/<run-id>/report.md`、`summary.json`、`transaction-events.jsonl`

実識別子はローカル対応表だけに保存し、ログ・レポート・fixtureでは`CAM-A`、`CAM-B`へ置換する。JPEGは`.partial`へ保存し、JPEG構造・サイズ・SHA-256確認後に`original.jpg`へ原子的にrenameする。

## 合成処理

M2はD810の7360×4912 JPEGを前提に、150/180/200 DPI候補の光学成立性を計算してから、Planar Homographyと固定キャリブレーションを使用する。

```text
JPEG decode
 -> per-camera lens correction
 -> precomputed planar warp
 -> constrained residual alignment
 -> exposure/color compensation
 -> fixed seam or Graph Cut seam
 -> multi-band blending
 -> crop
 -> quality checks
 -> JPEG export
```

撮影ごとに自由なホモグラフィを再推定せず、残差補正量に上限を設ける。

## 技術スタック

- Phase 0: C++20 / CMake / CTest / Nikon D810 Camera Remote SDK
- M2画像処理: C++20 / OpenCV
- M3 UI・調整: .NET 10 / WPF
- M3 IPC: Named Pipe
- メタデータ: SQLite候補
- ログ: JSONLおよび構造化ログ

2026-08-04、SDKと公式sampleの双方でSDRAM Itemが生成されない証拠を確認し、product ownerが`REVISE-WPD`を承認した。WPD adapterは明示的な`--transport wpd`としてSDK adapterと分離し、カメラ側Objectの削除を行わない。
