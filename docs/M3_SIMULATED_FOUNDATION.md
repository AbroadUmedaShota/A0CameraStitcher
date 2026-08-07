# M3 simulated統合基盤

## 目的と境界

二台目D810や実Camera Agentを待たず、.NET 10アプリ層のIPC、永続transaction、失敗・再起動、operator workflowを検証する。全component、message、artifact、画面は`Simulated`と明記し、Nikon SDK、WPD、USB、実カメラへ接続しない。

この基盤の合格は、D810二台制御、実Live View、実JPEG保存、A0合成品質、製品MVPの合格を意味しない。M3本体ではfake Camera Agentを単一C++ Camera Agentへ置換し、Phase 0BとM2の承認済み契約を接続する。

## component

- `A0CameraStitcher.M3.Foundation`: domain、versioned JSON protocol、Named Pipe fake agent、durable fake transaction。
- `A0CameraStitcher.M3.FoundationTests`: 外部test packageを使わない自己完結契約試験。
- `A0CameraStitcher.M3.OperatorShell`: 常時`SIMULATED / 実機未接続`を表示し、起動同意、readiness、操作可否、結果確認、保守タブを検証するWPF shell。

Named Pipe request/responseは`schemaVersion: a0.camera-agent.simulated.v1`、`simulation: true`、`marker: Simulated`を必須とする。未知version、falseのsimulation、異なるmarker、未知operation、不正payloadを拒否する。fake agentが返すのはstatus、匿名`CAM-A/B` inventory、非実画像preview placeholderだけである。

## durable transaction

```text
Idle -> CaptureA -> PersistA -> CaptureB -> PersistB -> Complete
                              \-> FailedPartial
```

- fake原本はJPEGに見せず、`Simulated`本文を持つ`.simulated`として`.partial`経由で原子的に保存する。
- 同一transaction IDの再開・再試行を拒否し、`AutomaticRetryCount`は0に固定する。
- CAM-B失敗時も取得済みCAM-Aを保持する。
- 未完journalや残留`.partial`は再起動時に`FailedPartial`へ閉じ、撮影を再実行しない。
- root単位のOS file lockにより、別service／processからの同時transactionを拒否する。

WPF shellのlocal stateは`%LOCALAPPDATA%\A0CameraStitcher\m3-simulated`へ置く。これはgitignored runtime dataであり、実写や実識別子を含まない。明示保存デモはJPEGを生成せず、OS一時フォルダ配下へ`.simulated-export.txt`を出力する。

## operator workflow

`ReadinessSnapshot`はcamera、identity、profile、setup、card、保存先、blocker/warningを集約する。`OperatorActionAvailability`が撮影、Live View、保存、再合成、新規撮影準備、保守画面移動の可否と無効理由を一元管理する。撮影・合成・保存結果は`CaptureOutcome`、`StitchOutcome`、`ExportOutcome`として別契約にする。

起動時同意は永続化しない。`Ready`と`ReadyWithCorrection`だけが追加ダイアログなしの撮影を許可し、処理開始後はcommandとaction contractの両方で二重開始を拒否する。詳細な操作順、警告、禁止操作、失敗復旧は`docs/OPERATOR_UI_SPEC.md`を正とする。

## 実行と検証

```powershell
dotnet build .\A0CameraStitcher.M3.slnx -c Release
dotnet run --project .\tests\m3\FoundationTests\A0CameraStitcher.M3.FoundationTests.csproj -c Release
pwsh -NoProfile -File .\scripts\Test-M3Simulated.ps1
dotnet run --project .\src\m3\OperatorShell\A0CameraStitcher.M3.OperatorShell.csproj -c Debug
```

契約試験はprotocol serialization／拒否、実Named Pipe round-trip、順次成功、片側失敗と原本保持、no-retry、crash/restart、別coordinator排他、残留partial回復に加え、起動同意、補正三状態、全Blocker、active transaction中の操作ロックを検証する。一括scriptはsolution build、11/11 test、WPFのtarget／依存、常設banner、警告とaccessibility live region、no-auto-retry表示、simulation flag拒否を確認する。

WPF shellでは正常、Live View停止、CAM-A/B撮影、cleanup、合成、Live View再開、CAM-A保存後の擬似crashを診断シナリオとして確認できる。Live View欄は一台選択式placeholderで、原画像や合成入力ではない。Windows UI Automationで連続二回Invoke時のtransaction一件、明示保存一件、新規撮影準備後のReady復帰を確認済み。screen reader、キーボード、focus walkthroughは未実施である。
