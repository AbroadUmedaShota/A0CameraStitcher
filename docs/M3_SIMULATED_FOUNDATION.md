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

DualCameraのapplication flowは、C++ identity proof結果を匿名JSON DTOから`Ready`、`Missing`、`Ambiguous`、`Collision`、`AliasMismatch`、`TransportMismatch`、`Expired`、`InvalidSchema`、`HardwarePending`へ変換する。`Ready`以外はWPFとflow APIの両方で撮影開始前に拒否し、SingleCameraへfallbackしない。active transactionは開始時snapshotを固定する。`TestSynthetic`だけが明示的な匿名Ready snapshotを注入し、実provider未確定の経路は`HardwarePending`を既定値として維持する。このsoftware-only adapterと契約試験は、実機identity readiness、SDK/WPD correlation、card access、capture、Live View、設定変更、削除を承認・実行するものではない。

## 疑似LVフレームソース（SIMULATED）

`src/m3/OperatorShell/Simulated/`は、CAM-A/CAM-B各1系統の疑似ライブビューフレームを実行時に描画生成する（ビットマップ資産・実写・顧客原稿は一切使わない）。`ISimulatedLiveViewFrameSource`（実装: `SimulatedTestImageFrameSource`）が正対原稿・傾き原稿（ROLL ±3°/±6°の4パターン）・ボケ→合焦遷移の各シーンをWPFの`DrawingVisual`/`RenderTargetBitmap`で描く。全フレームは`Simulation=true`/`Marker="Simulated"`を持ち、二段描画（背景シーンをレンダリング後、非ブラーの別パスで"SIMULATED"透かしとタイムスタンプ帯を上書き合成）により、どの合焦状態でも透かしが可読なまま残る。

`ISimulatedLiveViewFramePump`（実装: `SimulatedLiveViewFramePump`、`System.Threading.Timer`駆動でDispatcher非依存）はLive View ON中だけ一定間隔（既定200ms・5fps。プレビュー用途で体感十分な更新頻度とCPU負荷のバランスを取った値）で「tick」（camera alias・pattern・sequence・generation・timestampのみを持つ軽量レコード`SimulatedLiveViewFrameTick`）を発火するだけで、実際のWPF描画（`ISimulatedLiveViewFrameSource.CreateFrame`）はしない。これにより、タイマーcallback自体はほぼ一瞬で完了し（tick同士のオーバーラップやスレッドプール各スレッドへのDispatcher蓄積のリスクを回避）、実際の描画は`OperatorShellViewModel`がtickを自分のSynchronizationContext経由でUIスレッドへ運んでから行う（＝全フレームが単一のDispatcherの上で生成される）。`Start()`はgeneration番号を返し、Live View OFF→同一カメラで再ONした場合でも、古いgenerationのtickは新しいgenerationと一致しないため破棄される（aliasだけの一致判定では検出できないOFF→同一alias→ON競合のガード）。

`OperatorShellViewModel`はpumpとframe sourceの両方をコンストラクタ注入（既定null、両方揃って初めて`IsSimulatedFrameSourceAvailable=true`）で受け取り、`IsLiveViewActive`のON/OFFでStart/Stopを呼ぶだけで、タイマー自体はViewModelに持たせない。フレームは`StageSingleLiveImage`/`StageCompositeLiveImage`/`StageCompositeStillImage`へ反映され、フレーム未供給時は既存のSimulatedプレースホルダ文言を維持する。合成プレビューの非ライブ側（`StageCompositeStillImage`）は、そのaliasが直近にLive View対象だった時の最終フレームを凍結表示し、そのタイムスタンプ（capture由来の`_lastCapturedOriginalTimestamps`とLVフレーム由来の`_lastLiveFrameTimestamps`のうちより新しい方）が鮮度バッジ`StageCompositeFreshnessText`の実データ源になる。tick適用時はLive View状態・生成番号・alias一致・Simulated markerを検証し、いずれかを満たさない場合は例外を投げず（`SynchronizationContext.Post`内のthrowはWPFの未処理Dispatcher例外になるため）破棄してStatusMessageへ表示する。開発・検証用のパターン切替は「設置・校正」タブに置き、frame source未注入時は非表示になる。プレビュー専用でありoriginal/合成入力へは流用しない。

## 実行と検証

```powershell
dotnet build .\A0CameraStitcher.M3.slnx -c Release
dotnet run --project .\tests\m3\FoundationTests\A0CameraStitcher.M3.FoundationTests.csproj -c Release
pwsh -NoProfile -File .\scripts\Test-M3Simulated.ps1
dotnet run --project .\src\m3\OperatorShell\A0CameraStitcher.M3.OperatorShell.csproj -c Debug
```

契約試験はprotocol serialization／拒否、実Named Pipe round-trip、明示mode、Single選択aliasだけの成功、Single stitch N/Aと明示export、Dual CAM-A→CAM-B 100/100、no-auto-fallback、片側失敗と原本保持、Live View停止前durable failure、no-retry、crash/restart、別coordinator排他、残留partial回復に加え、CAM-A-only、30日profile承認、fixed-local preference、strict hardware Live View v2 frame/sessionを検証する。2026-08-10のfresh Release実行はsolution build 0 warning/0 error、Foundation 20/20、Operator Shell 17/17で、`pwsh -NoProfile -File .\scripts\Test-M3Simulated.ps1 -Configuration Release`も完了した。

WPF shellではmode別正常系、Live View停止、required camera撮影、cleanup、Dual合成、撮影後状態、途中擬似crashを診断シナリオとして確認する。Simulated画面のLive View欄は一台選択式placeholderで、Hardware Single画面はv2 memory-only previewを使い、どちらも原画像や合成入力ではない。2026-08-10のheadless/static Operator Shell 17/17はSingle mode、CAM-A-only、profile/export preference、mode lock、stitch N/A、no-auto-fallback、起動時操作gate、v2 stop-before-captureとsuccess-only restartをfresh検証した。screen reader、キーボード、focus、実画面のSingle操作、実Camera AgentからD810を使うwalkthroughは未実施である。
