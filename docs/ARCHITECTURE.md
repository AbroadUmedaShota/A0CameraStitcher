# アーキテクチャ

## 設計方針

MVPは「Windowsアプリ」「カメラ制御エージェント」「画像合成エンジン」を分離する。最初から全体を作らず、Phase 0でUSB制御の成立性を確認してから統合へ進む。

```text
┌────────────────────────────────────────────┐
│ A0CameraStitcher.App (.NET 10 / WPF)      │
│ UI / CaptureCoordinator / Session / Store │
└───────────────────┬────────────────────────┘
                    │ Named Pipe
          ┌─────────┴─────────┐
          │                   │
┌─────────▼─────────┐ ┌───────▼───────────┐
│ CameraAgent.Left  │ │ CameraAgent.Right │
│ C++ / WPD / PTP   │ │ C++ / WPD / PTP   │
└─────────┬─────────┘ └───────┬───────────┘
          │ USB               │ USB
      Nikon D750 L        Nikon D750 R

┌────────────────────────────────────────────┐
│ StitchEngine (C++20 / OpenCV)              │
│ calibration / warp / color / seam / blend │
└────────────────────────────────────────────┘
```

## カメラ制御

### Phase 0の順序

1. WPDでD750を2台列挙し、取得可能な機能・プロパティ・永続IDを記録する。
2. 標準 `WPD_COMMAND_STILL_IMAGE_CAPTURE_INITIATE` の対応可否を1台で確認する。
3. 標準機能が不足する場合、承認済みの公開資料または正規取得したSDK資料の範囲でPTP vendor extensionを調査する。
4. 二台へ撮影命令を発行し、ObjectAdded相当の検出と画像回収を検証する。
5. 100回連続撮影とUSB再接続を検証する。

標準WPDがD750の撮影を公開しない可能性がある。これは不具合ではなく、ドライバ能力として判定する。

### プロセス分離

カメラごとに専用プロセスとCOMスレッドを持つ。これはNikon SDKの制限回避ではなく、WPD/PTP処理の障害分離と再接続単位の明確化が目的である。

### 撮影トランザクション

```text
Create transaction
  -> validate both cameras and rig profile
  -> arm left and right agents
  -> dispatch capture commands concurrently
  -> detect left and right image objects
  -> transfer left image
  -> transfer right image
  -> atomically persist originals and metadata
  -> run stitch engine
  -> persist output and quality metrics
```

## 合成処理

MVPは平面原稿に限定し、Planar Homographyと固定キャリブレーションを使用する。

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

撮影ごとに自由なホモグラフィを再推定すると誤対応で大きく変形するため、残差補正量に上限を設ける。

## 保存

- `CaptureTransaction`: ID、開始・終了、リグ、結果、エラー
- `CapturedFrame`: 左右、カメラ別名、時刻、パス、サイズ、ハッシュ
- `StitchResult`: 出力パス、処理時間、品質値、警告
- `RigProfile`: 左右識別子、内部パラメータ、変換、シーム、クロップ

メタデータはSQLite候補、画像本体はファイルシステムへ保存する。実カメラシリアルはログ表示時にマスクし、コミット可能なテストデータでは架空IDを使用する。

## 技術スタック候補

- UI・調整: .NET 10 / WPF
- カメラ制御: C++20 / Windows WPD・PTP
- 画像処理: C++20 / OpenCV
- IPC: Named Pipe
- メタデータ: SQLite
- ログ: Serilogまたは同等の構造化ログ
- ビルド: CMake + dotnet CLI

採用はPhase 0後にADRで確定する。
