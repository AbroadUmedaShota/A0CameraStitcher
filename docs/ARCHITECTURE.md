# アーキテクチャ

## 設計方針

MVPは「Windowsアプリ」「単一カメラ制御エージェント」「画像合成エンジン」を分離する。Phase 0は.NETへ依存しないC++20コンソールとして、Nikon SDK Live Viewと、2026-08-06に`HG-0008`承認済みのsingle-slot spool経路の排他handoffを検証する。

```text
┌────────────────────────────────────────────┐
│ A0CameraStitcher.App (.NET 10 / WPF, M3)  │
│ UI / CaptureCoordinator / Session / Store │
└───────────────────┬────────────────────────┘
                    │ Named Pipe
┌───────────────────▼────────────────────────┐
│ CameraAgent (C++20, one active transport)  │
│ SDK Live View (one selected camera)        │
│ stop + close -> WPD baseline/close         │
│ -> SDK one card capture/close               │
│ -> WPD recover (no capture command)        │
└───────────────────┬────────────────────────┘
                    │ Nikon Remote SDK / WPD / USB
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
- `ICardCaptureTransport` / `IPostCardObservationTransport`: SDK card captureとWPD post-baseline回収を分離し、hybrid executorが各`ICameraTransport::Close`の完了後だけ次のtransportを開く。
- `EvidenceWriter`: 原画像の原子的保存、SHA-256、JSONLイベント、匿名化レポート。
- `NikonSdkTransport`: 正規取得したD810用SDKをリポジトリ外から読み込み、選択中一台のLive View、設定、およびone-shotのカメラカード撮影を行うadapter。capture後はWPD recovery前にfull closeする。
- `WpdTransport`: correlation diagnosticsと、承認済みsingle-slot spool recovery専用。attempted hybridのdatetime cutoffはRejectedであり、clock cutoffを帰属に使わない。
- `ILiveViewTransport` / `LiveViewCoordinator`: SDKで選択中一台のLive Viewだけを開始・画像取得・停止し、hybrid transactionへ渡す前にstopとsession closeの完了を保証する。fake transportで停止・close・撮影・再開失敗を契約試験する。
- `FakeCameraTransport`: SDK不要のtransaction・失敗系テスト用。
- `CliSafety`: 旧direct captureをfake-onlyへ制限し、実機を開くcommandだけを明示分類する。
- `HardwareProcessLease`: `Local\` Windows named mutexにより、同じinteractive Windows logon session内の別processを含め実SDK/WPD commandを同時に一件だけ許可する。transport内のsession guardとは別の安全層である。別ユーザーsession／serviceはMVP運用外とし、M4のinstaller・運用policyで二重起動を禁止する。

SDK APIは、本人同意後に正規取得したlocal資料と公式sampleで確認した範囲だけをadapterへ反映する。SDK binaryは配置元から動的loadし、copy・link・再配布しない。

Live Viewは一台選択式であり、二台同時表示は行わない。プレビューframeは一時表示・診断専用で、`EvidenceWriter`の原画像、JPEG帰属、合成入力に渡さない。撮影要求が来たら`LiveViewCoordinator`はSDK Live Viewを停止しsession closeの成功を待つ。次にWPD baselineを取得してfull closeし、SDKがカメラカードへ一回だけ撮影してfull closeし、WPDがreopenして撮影commandなしで回収する。成功時だけ操作者が選択していた一台のLive ViewをSDKで再開する。停止・close・再開・baseline・capture・recoveryの失敗は自動再試行せず、診断を残して明示操作を要求する。

## 撮影トランザクション

```text
Idle
 -> CaptureA: WPD baseline / close / SDK one card capture / close / WPD recover exactly-one JPEG
 -> PersistA: .partial / validate / SHA-256 / atomic rename / close
 -> CaptureB: WPD baseline / close / SDK one card capture / close / WPD recover exactly-one JPEG
 -> PersistB: .partial / validate / SHA-256 / atomic rename / close
 -> Paired
 -> Complete
```

どの段階でもtimeout、切断、複数候補、既存・遅延画像を検出した場合は`FailedPartial`へ遷移する。自動再試行・同一トランザクションの再開・曖昧画像の自動帰属は行わない。取得済み原画像と曖昧画像は削除しない。

`run-1785914842210-1`でWPD baselineが10.385秒後に`baseline_timeout`となりSDK open/capture前に終了し、read-only `run-1785917005306-1`では3/3 session close後もdevice datetimeがadvance 0/equal 2、latest object date > device time 3/3だった。このためdevice datetime cutoffを製品帰属契約から撤回する。`HG-0008`は2026-08-06に承認済みで、専用empty/cleared card single-slot spoolを実装・実機評価できる。撮影前にJPEG以外も含むcamera payload objectが0件であることを確認し、SDK one capture後の唯一JPEG objectをWPDで回収する。PC `.partial`、JPEG・size検証、SHA-256、atomic `original.jpg`確定、再読込検証後にそのexact objectだけを削除し、全payload 0件を再確認する。候補0件・複数件・遅延・無効画像、download/persist/delete失敗では削除せず、PC原本があれば保持して`FailedPartial`にする。existing cardのbulk delete/format、vendor operation、retryは禁止する。

`wpd-status`はtarget互換性に加え、Microsoftの`WPD_COMMAND_MTP_EXT_GET_SUPPORTED_VENDOR_OPCODES`だけをqueryし、個別opcode一覧を保存せず件数と`0x9207`広告有無だけを匿名化する。撮影commandとvendor operationは構築・送信しない。既定は`GENERIC_READ`で、driverがqueryを`Access denied`にした場合も自動で権限を上げない。明示的な`--wpd-status-access read-write` runだけがread/write sessionを開けるが、送るcommandは同じ非変更query一件に固定する。WPD common HRESULTだけで原因を特定できない場合は、Microsoft WPD/MTP ETWを一transactionだけ収集し、raw traceはgitignored領域へ隔離する。commit可能な証拠にはopcode、response code、時刻、候補件数だけを残し、実識別子を含めない。

`spool-status`は`GENERIC_READ`のWPD session一回で、folder/functional node以外の全payload object数だけを集計してfull closeする。Object ID・名前・拡張子・日付・実識別子は境界外へ出さず、capture、vendor operation、settings、deleteを実行しない。`hybrid-fault-single`はempty-beforeとSDK one capture/full closeの後、WPD recovery open直前にoperator gateを置く。USB切断または電源断後のWPD open failureを`FailedPartial`にし、PC original未確定時はdeleteせず、自動retryや同一transaction再開を行わない。旧direct WPD/SDK capture commandはfake-onlyへ閉じ、実機指定をparse段階で拒否する。

電源再投入後の標準WPD transaction失敗は履歴証拠として保持する。attempted hybridはADR-0019でRejectedであり、`0x9207`の広告はoperationの意味・parameter・許可・成功を示さず、vendor operationは実装・送信しない。P0-A2は承認済みsingle-slot spoolを一回証明してから10/10へ進み、A3/A4はその後に評価する。これらの実機試験は未実施である。

camera aliasは列挙順で決めず、transport別のlocal identity mapから解決する。SDKとWPDの実識別子は別台帳に保存し、reportには出さない。Phase 0Aのhandoffは物理D810が一台だけ接続された場合に限定する。Phase 0Bでは各bodyを一台ずつ接続して両transport identityを同じaliasへ明示登録し、そのbindingが完成するまで二台接続時のhandoffを拒否する。再接続・port変更後の永続性はPhase 0A/B実測に合格するまで暫定方式とする。

## timeout初期値

- SDK open: 10秒
- image event: 15秒
- JPEG download: 60秒
- SDK close: 10秒
- pair/hybridトランザクション全体watchdog: 180秒

各操作には全体watchdogの残時間以下を渡す。operator gateを含む処理が期限を超えた場合も次のtransportを開かず`transaction_watchdog`で終了する。PC保存は`.partial`書込み後とatomic rename直前にもdeadlineを確認し、期限後にcanonical rename、card delete、成功状態を開始しない。期限前に既に確定したPC原本、または未確定`.partial`は診断用に保持する。timeoutは安全停止値であり、Phase 0性能合否値ではない。すべて実行レポートへ記録する。

## 保存

- ローカル実識別子対応表: `%LOCALAPPDATA%\A0CameraStitcher\phase0\camera-map.json`
- raw証拠: `artifacts/phase0/<run-id>/`
- Live View handoff証拠: `artifacts/phase0/<run-id>/handoff-summary.json`
- 曖昧画像: `artifacts/phase0/quarantine/<run-id>/<transaction-id>/<alias>/`
- commit可能レポート: `docs/evidence/phase0/<run-id>/report.md`と、存在する匿名`summary.json`、`handoff-summary.json`、`live-view-summary.json`、`transaction-events.jsonl`

実識別子はローカル対応表だけに保存し、ログ・レポート・fixtureでは`CAM-A`、`CAM-B`へ置換する。JPEGは`.partial`へ保存し、JPEG構造・サイズ・SHA-256確認後に`original.jpg`へ原子的にrenameする。

## 合成処理

M2はD810の7360×4912 JPEGを前提に、150/180/200 DPI候補の光学成立性を計算してから、Planar Homographyと固定キャリブレーションを使用する。

設置の目的はpixel単位で人が完全一致させることではなく、承認済みprofileの自動補正範囲へ撮影条件を入れることである。設置アシスタントは、profile承認状態、全画角、重複、カメラ設定整合と、位置・回転・倍率・露出・色の測定値を評価し、次の三状態を返す。

- `ready`: 目標範囲内で補正不要。
- `ready-auto-correction`: 承認済み上限内なので、一時的な補正を適用して処理を続行できる。
- `physical-adjustment-required`: 画角・重複・設定または補正量が範囲外で、物理調整または再キャリブレーションが必要。

判定器は閾値の既定値を持たず、承認済みrig profileから明示的に受け取る。単純な`approved`フラグを信用せず、status、対応schema版、provenance、`measuredAt <= assessedAt < validUntil`を自分で検査する。draft・不整合・期限切れprofileは本番撮影に使わない。JSON Schemaはshapeと型を担当し、cross-field順序と評価時点の期限はproduction validator／判定器がfail closedにする。Live View frameは設置の目視案内に限り、正確な校正と品質測定には明示的な校正用JPEGを使う。Live View frameを原画像・合成入力・撮影候補へ昇格しない。

M2 pre-gateの`A0CameraStitcher.OpticalPlanner`はOpenCVや実画像に依存しない純粋計算境界である。全光学条件を引数で受け取り、結果へ`unapproved`と`not-evaluated`を常設する。公開synthetic chartとdraft rig-profileは契約試験専用であり、実写品質や承認済みcalibrationとして画像処理pipelineへ自動投入しない。

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

撮影ごとに自由なホモグラフィを再推定せず、固定profileに対する位置・回転・倍率・露出・色の一時的な残差補正だけを行う。補正量に上限を設け、上限超過時はclampして成功扱いにせず`physical-adjustment-required`とする。補正結果で固定profileを自動学習・自動更新しない。

各transactionはprofile ID・schema版・校正時刻、baseline残差、提案・適用・拒否した補正量、判定理由、補正後品質を記録する。実camera identityは含めない。単一cameraではレンズ校正、設定読取、設置案内と記録contractを先行検証できるが、左右固定transform、overlap、seam、A0品質の合格には二台と承認済みchartが必要である。

## 技術スタック

- Phase 0: C++20 / CMake / CTest / Nikon D810 Camera Remote SDK
- M2画像処理: C++20 / OpenCV
- M3 UI・調整: .NET 10 / WPF
- M3 IPC: Named Pipe
- メタデータ: SQLite候補
- ログ: JSONLおよび構造化ログ

## M3 pre-gateのsimulation境界

M3Pは.NET 10内の`Foundation`、実Named Pipeを使うfake agent、WPF `OperatorShell`でapplication contractだけを先行検証する。messageは`simulation=true`と`marker=Simulated`を必須とし、画面は常時`SIMULATED / 実機未接続`を表示する。fake原本は`.simulated`のplain textであり、JPEGやLive View frameとして扱わない。

M3Pのfake agentは将来のC++ Camera Agentそのものではない。M3で置換するまで、SDK/WPD transport、実camera identity、実Live View、実JPEGをこの境界へ接続しない。durable transactionのstate／no-retry／部分成功保持契約だけを共通化する。

2026-08-04の`REVISE-WPD`承認と標準WPD失敗の履歴を保持する。2026-08-06の`HG-0008`承認により、専用empty/cleared cardに限り、PC原本の再読込検証後にjust-recovered WPD objectを削除できる。bulk delete、format、vendor operation、retryは行わない。
