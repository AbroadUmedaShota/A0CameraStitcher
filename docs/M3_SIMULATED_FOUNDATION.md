# M3 simulated統合基盤

## 目的と境界

実Camera Agentを待たず、.NET 10アプリ層の明示`SingleCamera`／`DualCamera`、IPC、永続transaction、失敗・再起動、operator workflowを検証する。全component、message、artifact、画面は`Simulated`と明記し、Nikon SDK、WPD、USB、実カメラへ接続しない。

この基盤の合格は、実WPF Camera Agent連携、D810一台／二台制御、実Live View、実JPEG保存、canonical original export、A0合成品質、製品MVPの合格を意味しない。別の`HardwareSingleCamera` laneと単一C++ Camera Agentのsoftware boundaryは実装・契約試験済みだが、fake laneを実機合格へ読み替えず、Phase 0A/BとM2の承認済み実機契約を別途接続・受入する。

## component

- `A0CameraStitcher.M3.Foundation`: domain、versioned JSON protocol、Named Pipe fake agent、durable fake transaction。
- `A0CameraStitcher.M3.FoundationTests`: 外部test packageを使わない自己完結契約試験。
- `A0CameraStitcher.M3.OperatorShell`: 常時`SIMULATED / 実機未接続`を表示し、起動同意、readiness、操作可否、結果確認、保守タブを検証するWPF shell。

Named Pipe request/responseは`schemaVersion: a0.camera-agent.simulated.v1`、`simulation: true`、`marker: Simulated`を必須とする。未知version、falseのsimulation、異なるmarker、未知operation、不正payloadを拒否する。application transactionは`SingleCamera`または`DualCamera`を明示し、接続台数からmodeを推定しない。fake agentが返すのはstatus、匿名camera inventory、非実画像preview placeholderだけである。

## durable transaction

```text
SingleCamera: Idle -> CaptureSelected -> PersistSelected -> Complete (Stitch NotApplicable)
DualCamera:   Idle -> CaptureA -> PersistA -> CaptureB -> PersistB -> Complete
                                        \-> FailedPartial
```

- fake原本はJPEGに見せず、`Simulated`本文を持つ`.simulated`として`.partial`経由で原子的に保存する。
- 同一transaction IDの再開・再試行を拒否し、`AutomaticRetryCount`は0に固定する。
- CAM-B失敗時も取得済みCAM-Aを保持する。
- SingleCameraでは選択aliasだけを処理し、他aliasとstitch jobを開始しない。DualCameraのrequired camera不足をSingleCameraへ自動降格しない。
- 未完journalや残留`.partial`は再起動時に`FailedPartial`へ閉じ、撮影を再実行しない。
- root単位のOS file lockにより、別service／processからの同時transactionを拒否する。

WPF shellのlocal stateは`%LOCALAPPDATA%\A0CameraStitcher\m3-simulated`へ置く。これはgitignored runtime dataであり、実写や実識別子を含まない。明示保存デモはJPEGを生成せず、OS一時フォルダ配下へ`.simulated-export.txt`を出力する。

## operator workflow

`ReadinessSnapshot`は明示mode、required aliases、camera、identity、profile、setup、card、保存先、blocker/warningを集約する。`OperatorActionAvailability`がmode変更、撮影、Live View、保存、再合成、新規撮影準備、保守画面移動の可否と無効理由を一元管理する。撮影・合成・保存結果は`CaptureOutcome`、`StitchOutcome`、`ExportOutcome`として別契約にする。`SingleCamera`では`StitchOutcome=NotApplicable`、再合成不可、canonical original相当の明示exportとする。

起動時同意は永続化しない。`Ready`と`ReadyWithCorrection`だけが追加ダイアログなしの撮影を許可し、処理開始後はcommandとaction contractの両方で二重開始を拒否する。詳細な操作順、警告、禁止操作、失敗復旧は`docs/OPERATOR_UI_SPEC.md`を正とする。

## 実行と検証

```powershell
dotnet build .\A0CameraStitcher.M3.slnx -c Release
dotnet run --project .\tests\m3\FoundationTests\A0CameraStitcher.M3.FoundationTests.csproj -c Release
pwsh -NoProfile -File .\scripts\Test-M3Simulated.ps1
dotnet run --project .\src\m3\OperatorShell\A0CameraStitcher.M3.OperatorShell.csproj -c Debug
```

契約試験はprotocol serialization／拒否、実Named Pipe round-trip、明示mode、Single選択aliasだけの成功、Single stitch N/Aと明示export、Dual CAM-A→CAM-B 100/100、no-auto-fallback、片側失敗と原本保持、Live View停止前durable failure、no-retry、crash/restart、別coordinator排他、残留partial回復に加え、起動同意、補正三状態、mode別Blocker、active transaction中のmodeを含む操作ロックを検証する。2026-08-10のfresh Release実行はsolution build 0 warning/0 error、Foundation 19/19、Operator Shell 15/15で、`pwsh -NoProfile -File .\scripts\Test-M3Simulated.ps1 -Configuration Release`も`M3 simulated foundation plus SingleCamera hardware software boundary passed validation.`として完了した。

WPF shellではmode別正常系、Live View停止、required camera撮影、cleanup、Dual合成、撮影後状態、途中擬似crashを診断シナリオとして確認する。Live View欄は一台選択式placeholderで、原画像や合成入力ではない。2026-08-10のheadless/static Operator Shell 15/15はSingle mode、mode lock、stitch N/A、no-auto-fallback、起動時操作gateをfresh検証した。2026-08-08のWindows UI Automationは旧Dual contractで連続二回Invoke時のtransaction一件、明示保存一件、新規撮影準備後のReady復帰を確認した履歴証拠に留める。screen reader、キーボード、focus、実画面のSingle操作、実Camera AgentからD810を使うwalkthroughは未実施である。
