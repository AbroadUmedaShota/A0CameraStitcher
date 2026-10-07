# D810 PC制御項目

## 結論

コントラストや明暗は調整可能です。ただし、調整方法は二種類に分かれます。

- 撮影時の明るさは、シャッタースピード、絞り、ISO感度、露出補正で制御する。
- JPEGの見た目は、ピクチャーコントロールとその輪郭強調、明瞭度、コントラスト、明るさ、彩度、色相で調整する。

attempted hybridはWPD baseline timeoutとdatetime相関不成立によりRejectedです。`HG-0008`は2026-08-06に承認され、dedicated empty/cleared cardをsingle-slot transient spoolとして、PC原本の再読込検証後にexact just-recovered WPD objectだけを削除して空へ戻す実装・実機評価が可能です。Nikon SDKの`SDK status`は、Live View状態、禁止mask、静止画／動画セレクターに加え、主要撮影設定を撮影設定capabilityへ書き込まず匿名記録できます。MAID sessionのcontrol-plane callback登録とModuleModeには既存の`CapSet`を使い得るため、「SDK commandがGetだけ」という意味ではありません。

## 現在の実装範囲

| 操作 | 現在の状態 | 備考 |
| --- | --- | --- |
| D810列挙、匿名別名付与 | 実装済み | 実シリアルはreportへ出さない |
| PCからシャッター指示 | software implementation ready | 承認済みsingle-slot spoolの実機one-shotは未実施 |
| 新規JPEGの識別 | clock cutoff Rejected | `wpd-correlation-status`はdatetimeの診断だけを行う |
| JPEGのPC保存 | 実機確認済み | `.partial`、JPEG検証、SHA-256確定後に`original.jpg`へ移動 |
| カメラ側JPEG | 承認済みsingle-slot spool | PC `.partial`、JPEG・size検証、SHA-256、atomic `original.jpg`、再読込検証後だけexact objectを削除して空状態を確認する。失敗時は削除しない |
| Live View状態・禁止maskの取得 | SDKで実装・実機確認済み | `sdk-status`はLive Viewを開始せず、設定を変更せずsessionを閉じる |
| 静止画／動画セレクターの取得 | SDKで実装・実機確認済み | Enum表現を読み取り、`photo`を匿名記録する |
| 露出・WB・画質等の取得 | 一台でPartial | `run-1786040075194-1`でJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1を取得。focusはopaque値1、FileTypeはnot-advertised。設定writeなし、session close |
| 露出・WB・画質等の変更 | 未実装 | Phase 0では人が固定する。書込実装・実機試験は別途承認して一項目ずつ行う |
| 一台選択式Live View | Standalone合格、handoffはPartial | SDKで5分04秒・2,424 frame、停止・close・preview非保存と別プロセス再起動を確認。撮影を含む連続handoffは未合格 |
| 二台同時Live View | 将来目標・技術gate待ち | ADR-0031のSDK capabilityと安全性証拠まで有効化しない |
| 動画、RAW | 対象外 | MVPでは使用しない |

## PCから扱える候補

次はD810用Nikon Camera Remote SDKのcapability定義と付属サンプルに存在する代表項目です。「候補」はSDKに項目があることを示し、この製品での実装済み・全状態での書き込み可能を意味しません。

| 分類 | 取得・変更候補 | A0撮影での扱い |
| --- | --- | --- |
| 撮影 | 撮影、AF撮影、フォーカスロック、露出ロック | 撮影はSDK exactly-one card captureを採用。AFは原稿撮影では原則固定を推奨 |
| 露出 | 露出モード、シャッタースピード、絞り、ISO感度、露出補正、測光モード | 二台で固定値を一致させる主要項目 |
| 画像形式 | JPEG圧縮率、画像サイズ、RAW画像サイズ、保存先 | MVPはJPEG Fine L。RAWは対象外 |
| 色 | WBモード、WB微調整、蛍光灯種別、プリセットWB | 二台の色差を抑えるため固定・一致させる |
| JPEG仕上げ | ピクチャーコントロール、ピクチャーコントロールデータ、彩度、明るさ、Active D-Lighting、ノイズ低減 | コントラスト等を含む。実機の書込可否と値域を追加検証する |
| フォーカス | フォーカスモード、AFエリアモード、優先フォーカスポイント、AF実行 | 固定リグではMF固定または撮影前AF後に固定する方針を検討 |
| 状態確認 | バッテリー、レンズ情報、焦点距離、カメラ時刻、露出状態 | preflight/report候補。固有識別子は匿名化する |
| ライブビュー | 開始・終了、画像取得、コントラストAF、表示サイズ等 | 一台選択式が現行対象。二台同時左右paneはADR-0031のSDK capability・安全性gate待ち。previewの原画像・合成利用は対象外 |

## コントラスト・明暗を扱う方針

1. Phase 0ではカメラ設定を人が固定し、ツールは変更しない。
2. 設定read-onlyの`SDK status`でLive View関連状態と主要撮影設定の取得を先行した。SDKが返すUnsigned／PackedString／Stringだけを記録し、未知値へ意味を推測しない。control-plane callback登録と撮影設定writeを証拠上で区別する。
3. 書き込みはホワイトバランス、露出、JPEG Fine L、ピクチャーコントロールの順に一項目ずつ実機検証する。
4. 二台撮影では同一設定を検証し、不一致なら撮影を開始しない。
5. PC側でコントラストや明るさを画像処理する場合、`original.jpg`は変更せず派生画像を別ファイルとして保存する。
6. Live ViewはSDK sessionを一台だけ開き、hybrid transaction前に必ずstopとclose完了を確認する。WPD baseline/close、SDK capture/close、WPD recovery、PC原本確定の成功後だけ選択中のLive Viewを再開する。

「明るさ」というSDK capabilityがそのまま最終JPEGの露出補正を意味するとは限りません。撮影時の光量は露出三要素で扱い、JPEGのトーンはピクチャーコントロールで扱うのが安全です。

## 根拠

- [Nikon D810使用説明書](https://downloadcenter.nikonimglib.com/ja/products/176/D810.html): ピクチャーコントロールの輪郭強調、明瞭度、コントラスト、明るさ、彩度などを説明。
- [Nikon Camera Remote SDK information/FAQ](https://sdk.nikonimaging.com/information/en/): D810用Remote SDKの提供とWindows対応履歴を掲載。
- ローカル隔離したD810用SDKのcapability定義および付属サンプルを参照した。ライセンス対象資料そのものはリポジトリへ含めない。

## 実機応答の既知の性質と fake の対応（Issue #231）

実機で見つかった #216・#222・#224 は、どれも fake と実機の応答の差だった。実機が実際に返した値は匿名化して `tests/fixtures/hardware-replay/` に置き（出所・置換規則・実物と再構成の区別は同フォルダの `README.md`）、アプリの保存経路と Agent の応答生成へ流す試験を足した。次の表は、D810 の性質ごとに「実機の観測」「fake の対応箇所」「状態」を突き合わせたもの。行番号は 2026-10 時点。

| 性質 | 実機の観測 | fake の対応箇所 | 状態 |
| --- | --- | --- | --- |
| fileType を広告しない | 承認 profile の `fileType` は `{"available": false}` だけ。SDK の記述子は既定のまま（`capType` unsupported、`probeState` not-advertised、`valueType` unsupported、値もラベルもなし）。`compressionLevel` は packed-string のラベル `JPEG Fine`（index 2）で広告される | C++: `tests/dual_hardware_capture_backend_tests.cpp:19`（`MakeReadOnlyStatus`、fileType は既定の未広告）、`:297`・`:327`（実物の 9 設定を流して二台のシャッター直前検査を通す）。C#: `tests/m3/OperatorShellTests/Program.cs:14145`〜`14192`（`HardwareTestData` の既定設定。本 Issue で未広告へ修正。以前は「広告あり・JPEG」で実機と逆だった）、`tests/m3/OperatorShellTests/HardwareReplayFixtures.cs:465`（実物 profile から作る readiness の設定）、`tests/m3/OperatorShellTests/HardwareReplayRunner.cs:205`（実機の観測からアプリの承認処理が実物と同じ設定を作る） | 対応済み |
| focus は意味の分からない値 | `focusMode` は `capType` unsigned・`valueType` unsigned・`currentValue` 1。ラベルも index もない。値の意味は推測しない（既存の方針）。承認はこの値の有無だけを条件にする（`src/m3/OperatorShell/Hardware/HardwareSingleCaptureProfileStore.cs:101`） | C#: `Program.cs:14192`（`OpaqueUnsigned(1)`。本 Issue で修正。以前はラベル付き）。C++: `tests/hardware_camera_agent_tests.cpp:695`〜`720`・`:737`（`capType` を実機の unsigned に修正。以前は generic）、`tests/dual_hardware_capture_backend_tests.cpp:327` | 対応済み。残る差: C++ の writer 形 profile の他の設定は `probeState` observed・`valueType` label（実機は available・packed-string）。比較は文字列の完全一致なので試験の意味は保たれ、実機の値そのものは再生試験が持つため、書き換えていない |
| ファームウェアが 2 台で違う（V1.11 と V1.14） | 1 台目 V1.11、2 台目 V1.14 のまま、3 種の撮影なし probe と二台撮影が通った（#221）。firmware は readiness の表示用の文字列で、撮影の判定には使われない（`src/phase0/hardware_camera_agent.cpp:3552`）。一台撮影の event log の `firmware` は `unknown` | `tests/wpd_dual_read_only_probe_tests.cpp:119`（本 Issue で追加。2 台の firmware を変えても probe が通り、出力に firmware が出ない）。既存の二台 fake は両台に同じ `fixture-firmware`（`tests/nikon_dual_session_adapter_tests.cpp:540`、`tests/wpd_dual_read_only_probe_tests.cpp:25`） | 一部対応。SDK 側の二台 adapter の fake は同一 firmware のまま（未対応。二台の撮影・Agent の経路は firmware を読まず、足しても分岐がないため） |
| 機体照合は確定から 5 分で失効 | 実物の terminal result の `identitySnapshot` は `expiresAtUtc` − `observedAtUtc` = 5 分。照合を確定してから返答を待つ間に失効した（#221 の 2 回目） | アプリが作る側: `src/m3/OperatorShell/ViewModels/OperatorShellViewModel.cs:2829`、判定: `src/m3/Foundation/DualCamera/DualCameraIdentity.cs:30`。再生: `HardwareReplayRunner.cs:449`（実物の照合値で 5 分ちょうどの拒否と 1 秒前の受理を検査）。通常フローの fake の既定は失効しない照合（`DualCameraIdentity.cs:39` の `AnonymousTestSyntheticReady`、`Program.cs` 内 7 箇所の `FixedDualCameraIdentitySnapshotSource`） | 再生側は対応済み。通常フローの既定は未対応（時計を持たない試験が多く、既定を変えるとそれぞれに時計の注入が要るため。5 分の境界は再生試験が持つ） |
| Agent は起動から 600 秒で終わる | Agent ホストの寿命は 600 秒固定で延長しない（#225）。journal にも応答にも現れず、時計で決まる性質 | native: `src/phase0/include/a0/phase0/agent_host_lifetime.hpp:15`、C#: `src/m3/OperatorShell/Hardware/DualCameraAgentLifecycle.cs:55`。fake: `tests/dual_hardware_camera_agent_pipe_tests.cpp:974`（偽の時計で 600000 ms を再現）、`Program.cs:7855`（C# の定数が native の予算と一致） | 対応済み（既存。fixture にはしない） |
| `None` は空文字で書かれる | Agent は `DualBindingInvalidationReason::None` を `""` で書く（`src/phase0/dual_identity_session_binding.cpp:29`）。実物の Succeeded と Failed の terminal result はどちらも `"bindingInvalidationReason":""`。アプリ自身の永続 snapshot は名前の `"None"` で、別の書式 | C++: `tests/dual_hardware_camera_agent_tests.cpp:310`（空文字を固定）、`tests/dual_hardware_camera_agent_store_tests.cpp:676`（実物の pair journal を読み戻し、同じ result から同じバイト列を書く）。C#: `HardwareReplayFixtures.cs:242`（実物の result を返す Agent の再生）、`HardwareReplayRunner.cs:318`・`:384`（Succeeded と Failed の流し込み）、`Program.cs` の shell シナリオ。`CaptureRecoveryOnlyFakeOperations` は型付きの結果を直接返し JSON を通らないため空文字を再現しない | 再生側は対応済み。型付き fake は未対応（JSON を経由しない設計のため。JSON を通る試験は上の再生側が持つ） |

### 変異確認（Issue #231 の受入基準）

修正を一時的に外して、再生試験が落ちることを確かめた。確認後に元へ戻し、コミットには含めていない。

| 外した修正 | 外し方 | 落ちた再生試験 |
| --- | --- | --- |
| #222（fileType 未広告の許容） | `RequireReadOnlyDualCaptureProfile` を修正前の判定（`fileType` と `compressionLevel` の部分一致）に戻す | `TestReplayRealD810ObservedSettingsAreAccepted` |
| #216（原画像フォルダ名） | 実行側（`ExecuteBoundSingleCapture` が transaction ID を渡す）と位置検査（先頭フォルダ名の一致）の両方を戻す。片方ずつでも、それぞれの再生試験が落ちる | `TestReplayRealSingleCaptureEventsAndLanding`（着地先と event）、`TestReplayRealSingleCaptureJournalAndResponse`（journal が `hybrid-tx-` フォルダの原画像を保証しない） |
| #224（`""` の読み取り） | `DualHardwareBindingInvalidationReasonConverter` の登録を外す | 二台の Succeeded・Failed の再生（ワークフロー、画面経由とも） |