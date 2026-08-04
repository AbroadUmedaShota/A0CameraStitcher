# A0 Camera Stitcher

社内名称「大判撮影ツール（A0対応）」の開発リポジトリです。固定したNikon D810 2台をWindows PCへUSB接続し、静止した平面原稿を順次撮影して1枚の大画像へ合成することを目標とします。

## 現在の段階

`phase0a-wpd-validation` — Nikon SDKは撮影できてもPC転送用SDRAM Itemを生成できず、2026-08-04にproduct ownerが`REVISE-WPD`を承認しました。WPDでD810一台を匿名`CAM-A`として列挙し、撮影、カメラ側JPEG保持、PCへの18,107,696-byte JPEG保存、7360×4912検証、SHA-256確定まで1 transaction成功しています。Phase 0Aの10回・異常系とPhase 0Bは未完了です。

Nikon Camera Remote SDKは同時に一台だけ開き、`CAM-A`の撮影・JPEG回収・close後に`CAM-B`へ進みます。実シャッター時刻差は保証しません。

## MVPの前提

- 対象: 静止したA0級の平面原稿
- カメラ: Nikon D810 2台、固定リグ（現在利用可能なのは1台）
- 接続: Windows 11 x64 PCへUSB接続
- 制御: Nikon D810 Camera Remote SDKを第一候補とする排他的順次制御
- 入力: FX JPEG Fine L
- 出力: 合成JPEG
- 時間目標: 撮影開始から出力完了までp95 10秒以内（暫定、Phase 0では測定のみ）
- 保存: 成功・失敗にかかわらず取得済み原画像を保持

## ドキュメント

- [製品要件](docs/PRODUCT_REQUIREMENTS.md)
- [アーキテクチャ](docs/ARCHITECTURE.md)
- [Phase 0実機検証計画](docs/PHASE0_TEST_PLAN.md)
- [Phase 0 readiness](docs/PHASE0_READINESS.md)
- [D810 PC制御項目](docs/D810_PC_CONTROL_CAPABILITIES.md)
- [ロードマップ](docs/ROADMAP.md)
- [意思決定記録](docs/DECISIONS.md)
- [旧D750参照会話（履歴のみ）](docs/REFERENCE_CONVERSATION.md)

## Phase 0 CLI

```powershell
pwsh -File .\scripts\Test-Phase0Readiness.ps1 -Stage Single
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Nikon SDKは本人同意済みで、`.tools/nikon/d810-remote-sdk`へローカル隔離配置し、CMake変数`NIKON_D810_SDK_ROOT`で参照します。SDK配布物、実カメラ識別子、実写画像はcommitしません。

## 公式根拠

- [Nikon SDK downloads](https://sdk.nikonimaging.com/apply/)
- [Nikon SDK information/FAQ](https://sdk.nikonimaging.com/information/en/)
- [Nikon D810 specifications](https://nij.nikon.com/products/lineup/slr/d810/spec.html)

## 取扱い

社内用privateリポジトリです。Nikon SDK、ライセンス対象資料、カメラ固有識別子、実写サンプル、顧客原稿、生成物、認証情報はコミットしません。
