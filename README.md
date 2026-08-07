# A0 Camera Stitcher

社内名称「大判撮影ツール（A0対応）」の開発リポジトリです。固定したNikon D810 2台をWindows PCへUSB接続し、静止した平面原稿を順次撮影して1枚の大画像へ合成することを目標とします。

## 現在の段階

総合状態は`in-progress`です。ソフトウェア作業と一台の非破壊検証は継続し、Phase 0Aの物理撮影だけを操作者の指示で保留しています。`HG-0008`は2026-08-06に承認され、専用empty/cleared cardをsingle-slot transient spoolに使う実装は完了しました。最初の全payload preflight [run-1786014841232-1](docs/evidence/phase0/run-1786014841232-1/report.md)は90 objectを検出し、SDK open・shutter・保存・delete・retryをすべて0のまま`FailedPartial`で安全停止しました。read-only確認は[run-1786015997366-1](docs/evidence/phase0/run-1786015997366-1/report.md)と[run-1786017282044-1](docs/evidence/phase0/run-1786017282044-1/report.md)の双方で同じpayload 90件でした。物理状態が変わるまで再確認せず、M1Aはempty cardへの交換またはbackup・手動clearの報告と明示再開を待ちます。one-shot、10/10、異常系、handoffの合格証拠は未取得です。

PCへ`.partial`、JPEG・size検証、SHA-256、atomic rename、再読込検証を完了した`original.jpg`だけを製品上の正本とします。カメラカードは一過性の転送元で、永続保持を要件にしません。承認済みの専用empty/cleared card single-slot spoolでは、撮影前にJPEG以外も含むcamera payload objectが0件であることを確認し、その後にjust-recovered WPD objectだけを削除して再びpayload 0件を確認します。候補0件・複数件・遅延・無効画像、download/persist/delete失敗では削除せず、PC原本があれば保持して`FailedPartial`にします。existing cardのbulk delete/format、vendor operation、retryは禁止です。

## MVPの前提

- 対象: 静止したA0級の平面原稿
- カメラ: Nikon D810 2台、固定リグ（現在利用可能なのは1台）
- 接続: Windows 11 x64 PCへUSB接続
- 制御: WPD baseline/recoveryと、カメラカードへ一回撮影するNikon SDK、および一台選択式SDK Live View
- 入力: FX JPEG Fine L
- 出力: 合成JPEG
- 時間目標: 撮影開始から出力完了までp95 10秒以内（暫定、Phase 0では測定のみ）
- 保存: PCへ確定・再読込検証済みの原画像を保持。カメラカードは一過性の転送元で、承認済みsingle-slot spoolではexact WPD objectだけを削除して空状態を確認する

## ドキュメント

- [現在の開発状況](docs/CURRENT_STATUS.md)
- [製品要件](docs/PRODUCT_REQUIREMENTS.md)
- [アーキテクチャ](docs/ARCHITECTURE.md)
- [Phase 0実機検証計画](docs/PHASE0_TEST_PLAN.md)
- [Phase 0 readiness](docs/PHASE0_READINESS.md)
- [D810 PC制御項目](docs/D810_PC_CONTROL_CAPABILITIES.md)
- [M2オフラインpre-gate](docs/M2_PRE_GATE.md)
- [アプリ機能検証計画](docs/FEATURE_VERIFICATION_PLAN.md)
- [M3 simulated統合基盤](docs/M3_SIMULATED_FOUNDATION.md)
- [ロードマップ](docs/ROADMAP.md)
- [意思決定記録](docs/DECISIONS.md)
- [旧D750参照会話（履歴のみ）](docs/REFERENCE_CONVERSATION.md)

## Phase 0 CLI

```powershell
pwsh -File .\scripts\Test-Phase0Readiness.ps1 -Stage Single
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
build\Debug\A0CameraStitcher.Phase0.exe sdk-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe wpd-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe spool-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe wpd-correlation-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe live-view --alias CAM-A --duration-seconds 300
build\Debug\A0CameraStitcher.Phase0.exe live-view-handoff --alias CAM-A --count 10 --frames 1
```

`spool-status --alias CAM-A`はread-only WPD sessionを一回だけ開き、folder/functional nodeを除く全payload件数だけを匿名保存して閉じます。Object ID・名前・実識別子は保存せず、capture、vendor operation、settings、deleteは0件です。`wpd-correlation-status --alias CAM-A`はread-onlyでWPD full close/reopenとdevice/object datetimeを診断しますが、clock cutoffは帰属根拠に使いません。

`hybrid-fault-single`は空spoolでone-shotが合格した後だけ使用します。SDK one captureとSDK close後、WPD recovery open前にoperator gateを出し、`usb-disconnect`または`power-off`を一回だけ試験します。異常後は`FailedPartial`、delete/retry 0、新規transactionでのみ復旧する契約です。

旧`capture-single`、`capture-pair`、`stability`はfake contract専用です。`--transport sdk`または`wpd`はcamera sessionを開く前に拒否し、実機経路は確認付き`hybrid-capture-single`だけに限定します。実SDK/WPDへ触れるコマンドはoperator-session-wide named OS leaseを保持するため、同じWindowsログオンsession内の別processとの同時実行もfail closedになります。別ユーザーsessionやserviceからの起動はMVP運用外とし、installer／運用policyで禁止します。

一台構成の`CAM-A` identity continuityは電源再投入後もSDK/WPD双方で確認済みです。Standalone Live Viewは5分04秒・2,424 frame、停止、SDK close、preview非保存に成功し、別プロセスでの再起動後も1 frame取得と正常終了を確認しました。撮影を含むone-shot、10/10、handoff 10回は承認済みspool経路で今後実施します。

一台の設定read-only診断 [run-1786040075194-1](docs/evidence/phase0/run-1786040075194-1/report.md)では、SDKが返した値としてJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得しました。FileTypeはnot-advertisedです。撮影設定write、capture、Live View開始、WPD、deleteは行わずSDK sessionを閉じました。MAID control-plane callback登録は既存`CapSet`を使い得るため、証拠上で撮影設定writeと区別しています。native command-trace testとfocus値の意味確定が残るため、この検証はPartialです。

Nikon SDKは本人同意済みで、`.tools/nikon/d810-remote-sdk`へローカル隔離配置し、CMake変数`NIKON_D810_SDK_ROOT`で参照します。SDK配布物、実カメラ識別子、実写画像はcommitしません。

## M2 pre-gate CLI

`A0CameraStitcher.OpticalPlanner.exe`は、明示入力されたDPI、frame回転、二枚の配置、重複pixel、cropからA0幾何候補を計算します。`a0_m2_setup`は、明示入力された目標値・自動補正上限から`ready`、`ready-auto-correction`、`physical-adjustment-required`を判定します。どちらも未承認の数値を既定値にせず、最終リグやA0品質の承認には使いません。詳細は[M2オフラインpre-gate](docs/M2_PRE_GATE.md)と[アプリ機能検証計画](docs/FEATURE_VERIFICATION_PLAN.md)を参照してください。

## M3 simulated shell

```powershell
pwsh -NoProfile -File .\scripts\Test-M3Simulated.ps1
dotnet run --project .\src\m3\OperatorShell\A0CameraStitcher.M3.OperatorShell.csproj -c Debug
```

このWPF shellとNamed Pipe agentは実機非接続のfakeです。画面、IPC、保存物に`Simulated`を常設し、実D810、Nikon SDK、WPDへ接続しません。詳細は[M3 simulated統合基盤](docs/M3_SIMULATED_FOUNDATION.md)を参照してください。

## 公式根拠

- [Nikon SDK downloads](https://sdk.nikonimaging.com/apply/)
- [Nikon SDK information/FAQ](https://sdk.nikonimaging.com/information/en/)
- [Nikon D810 specifications](https://nij.nikon.com/products/lineup/slr/d810/spec.html)

## 取扱い

社内用privateリポジトリです。Nikon SDK、ライセンス対象資料、カメラ固有識別子、実写サンプル、顧客原稿、生成物、認証情報はコミットしません。
