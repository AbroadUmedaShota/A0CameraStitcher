# A0 Camera Stitcher

社内名称「大判撮影ツール（A0対応）」の開発リポジトリです。固定した Nikon D750 2台をWindows PCへUSB接続し、静止した平面原稿を撮影して1枚の大画像へ合成することを目標とします。

## 現在の段階

`bootstrap-paused` — 要件・設計・Phase 0検証計画を準備済みです。アプリ実装は開始していません。

最初の成立性ゲートは、Windows 11 x64上でD750を2台同時に識別し、USBのみで撮影命令と画像回収を安定して実行できることです。Nikon Camera Remote SDKは公式FAQで複数カメラ同時制御を非対応としているため、二台制御の製品経路には採用しません。

## MVPの前提

- 対象: 静止したA0級の平面原稿
- カメラ: Nikon D750 2台、固定リグ
- 接続: Windows 11 x64 PCへUSB接続
- 同期: PCからのソフトウェア撮影トランザクション。実シャッター時刻差は保証しない
- 入力: JPEG Fine
- 出力: 合成JPEG（TIFFとRAWはMVP後）
- 時間目標: 撮影開始から出力完了までp95で10秒以内（実機検証で見直す）
- 保存: 左右の原画像を必ず保持

## ドキュメント

- [製品要件](docs/PRODUCT_REQUIREMENTS.md)
- [参照会話の整理](docs/REFERENCE_CONVERSATION.md)
- [アーキテクチャ](docs/ARCHITECTURE.md)
- [Phase 0 実機検証計画](docs/PHASE0_TEST_PLAN.md)
- [ロードマップ](docs/ROADMAP.md)
- [意思決定記録](docs/DECISIONS.md)

## 開発開始前に必要なもの

- Nikon D750 2台と使用予定レンズ
- 固定リグ、照明、A0相当テストチャート
- Windows 11 x64の検証PC
- .NET 10 SDK
- Visual Studio Build Tools（Desktop development with C++）
- CMakeとOpenCVの取得方式の確定

現PCではGit/GitHub CLIは利用できますが、.NET SDK、CMake、C++コンパイラはPATH上で未検出です。

## 公式根拠

- [Nikon SDK Information/FAQ](https://sdk.nikonimaging.com/information/en/)
- [Nikon SDK downloads](https://sdk.nikonimaging.com/apply/)
- [Nikon D750 specifications](https://nij.nikon.com/products/lineup/slr/d750/spec.html)
- [Microsoft WPD still image capture command](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/wpd-command-still-image-capture-initiate-command)
- [.NET support policy](https://dotnet.microsoft.com/en-us/platform/support/policy)

## 取扱い

社内用のprivateリポジトリです。Nikon SDK、カメラ固有資料、実写サンプル、個人情報を含む原稿、生成物、認証情報はコミットしません。
