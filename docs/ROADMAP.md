# MVPロードマップ

## 現在の進め方

MVP全体は`in-progress`である。2026-08-10に製品を明示的な`SingleCamera`／`DualCamera`へ拡張した。二台A0合成を維持し、一台software workflowを先行実装するが、既存Passを新しい一台製品受入へ読み替えない。空カードが必要なM1Aは`Deferred`のままとし、既知の90 payload objectは物理状態が変わるまで再確認しない。

| 実行レーン | 現在 | 次の完了条件 |
|---|---|---|
| 安全基盤 M0 | Complete | SDK有無各CTest 5/5、旧direct実機拒否、同一operator session内の別process排他、撮影／保存watchdog途中超過が合格 |
| 一台・非破壊 M1N | Active / Partial | SDK setting command traceを自動検証し、未広告・opaque値を明示処理 |
| オフラインpre-gate M2P | Active | synthetic画像の測定値を上限付き補正判定へ接続 |
| simulated統合 M3P | Complete / Software-only | requirements 2.6.0の明示mode、no-auto-fallback、Single original一件、stitch N/A、明示exportをfresh contractで維持 |
| 一台製品mode M1A/M3 | Software Boundary Complete / Hardware Deferred | 明示再開とempty spool確認を経てexactly-one D810のone-shot 1/1、続いて実WPF受入 |
| 二台 Phase 0 M1B | Binding In Progress | CAM-B identity-v2 checkpoint済み。pair撮影、CAM-A/B別USB/電源異常、A完了後B開始前process中断CLIを実装し、dual identity未Ready時はcard/capture前停止。fake安全契約合格。CAM-A SDK/WPD v2再登録待ち |
| 実リグ・製品統合 M2/M3/M4 | Human/Hardware Gated | `HG-0001/HG-0002/HG-0003B/HG-0005/HG-0009`と先行実機証拠 |

現在の詳細は[CURRENT_STATUS.md](CURRENT_STATUS.md)、機能単位の検証キューは[FEATURE_VERIFICATION_PLAN.md](FEATURE_VERIFICATION_PLAN.md)を正本とする。

## 現在の実行順

1. `WI-0010A`: principal setting readbackのnative SDK command-trace contract
2. `WI-0022C`: synthetic shift/rotation/scale/exposure/color測定とbounded decisionの接続
3. 操作者がカード作業を明示再開した場合だけexactly-one D810のM1A one-shot
4. `HG-0009`承認後に実D810のWPF→Camera Agent→canonical original→明示export受入
5. CAM-A/B cross-transport binding後にM1B実機pair
6. `HG-0001/HG-0002`承認後にDual実M2/M3受入、続いてM4

## M0: D810/SDK安全基盤

- D810、SDK one-shot card capture、WPD single-slot recovery、一台選択式Live Viewへ再基準化
- C++20/CMake/CTest、licensed SDKのリポジトリ外配置
- fake transaction、証拠、failure contract
- 旧`capture-single`、`capture-pair`、`stability`をfake-only化し、実SDK/WPD指定をcamera open前に拒否
- 実カメラを開く全CLIへoperator-session-wide named OS leaseを追加
- pairとhybridに共通の180秒transaction watchdogを適用

完了条件: SDK有無のbuild/test、旧実機入口のnegative test、process lease contention、watchdog境界が合格する。

実装状態: 2026-08-07のfresh buildでSDK有効／なし各CTest 5/5、実CLI negative test、同一operator session内の別process contention、SDK capture途中およびcanonical rename直前のwatchdog超過を確認し、M0は完了。

## M1N: D810一台・非破壊検証

- `CAM-A`のSDK/WPD匿名inventory。物理電源再投入continuityの追加実行は2026-08-09判断でN/A
- SDK status、Live View状態、JPEG/露出/ISO/WB/focus capabilityのreadback
- 撮影設定capabilityへのwrite、capture、Live View開始、WPD、deleteがないことをnative command traceで検証

実装状態: inventoryと設定readback実機runは完了し、設定readbackは9項目中8項目を取得、`FileType=not-advertised`、focusはopaque値1である。旧CAM-A SDK continuityはephemeral Source IDのため無効で、CAM-B identity-v2はcheckpointに留まる。identity-v2 reconnect／port確認とnative command traceが未実装なのでM1NはPartial。

## M2P: オフラインpre-gate

- 150/180/200 DPI候補、回転、重複、crop、shortfallの純粋計算
- rights-cleared synthetic fixtureと品質metric contract
- versioned rig-profile schemaとruntime trust検証
- `ready`、`ready-auto-correction`、`physical-adjustment-required`の純粋判定
- known synthetic差分から測定値を生成し、profile上限内だけ補正候補にするcontract

実装状態: 光学計算、schema、fixture、三状態setup-assessmentはsoftware-only合格。画像測定から補正判定への接続`WI-0022C`が次である。最終リグ、承認済み閾値、実写A0品質は証明しない。

## M3P: simulated統合基盤

- .NET 10 domain contractとversioned Named Pipe protocol
- fake CAM-A/Bによるdurable transaction、片側失敗、crash/restart
- 全画面に`SIMULATED / 実機未接続`と`NO AUTO RETRY`を表示するWPF shell

実装状態: 2026-08-10のrequirements 2.6.0 fresh Release実行でbuild 0 warning/0 error、Foundation 19/19、Operator Shell 15/15、`Test-M3Simulated.ps1` Pass。さらにSDK-less／licensed-SDK-enabled Release CTest各6/6で`hardware_camera_agent_contracts`を含むC++ software boundaryを確認した。明示mode、Single original一件、他alias未開始、stitch N/A、明示export、mode lock、no-auto-fallbackをsoftware-onlyで確認したためM3PはCompleteとする。CTestではcamera commandを送っておらず、実D810を使うWPF Camera Agent実行、WPD/SDK、actual JPEG、製品受入とは分離する。

## M1A: D810一台・物理撮影／SingleCamera transport受入 Phase 0

- 明示再開後、SDK/WPD双方で同じ登録済みaliasのD810がexactly oneであることを確認し、専用empty/cleared cardでone-shot 1/1
- 合格後に10/10 capture/recovery/delete/empty-after
- Live View handoff 10回
- USB切断、software process再起動からの新規transaction復旧（物理power-cycle/power-off復旧は2026-08-09判断でN/A）

現在: 操作者の指示で保留中。接続中cardは最後のread-only証拠で90 payload objectだった。カード交換または手動backup/clearの報告があるまで、`spool-status`を含め再実行しない。既存Phase 0 software contractは実WPF `SingleCamera`連携、canonical original明示export、一台製品受入の証拠ではない。

## M1B: D810二台 Phase 0

- 一台ずつSDK/WPD identityを同じ`CAM-A/B`へbinding
- 接続順・USB port変更後の復元
- `CAM-A → CAM-B`順次transaction 10件
- 100/100、二台異常系、最終transport判断

現在: D810 PnP/SDK/WPD各2台、read-only inventory、実機`hybrid-capture-pair` software contractを確認済み。CAM-A→CAM-B、pair共有180秒watchdog、A失敗時B未開始、B失敗時A原本保持、retry 0、sync非保証、100組集計、p50/p95/max匿名時間統計、CAM-A後の途中停止recovery診断がSDK有無各CTest 5/5で合格した。旧SDK source-ID mapは無効化してdocumented MAID Source `Name`/`Interface` identity-v2へ修正し、WPDもPnP IDから本体報告serial identity-v2へ強化した。現在はfirmware V1.11個体のCAM-B checkpoint。物理power-cycle/rebootと実power-off復旧はN/A。履歴上V1.14の元CAM-A本体だけへ交換後に別個体判定、CAM-A登録、接続順・port確認、二台readiness、1/10/100 pairへ進む。

## M2: 実リグ・オフライン合成PoC

- 承認済みA0 chart、lens、距離、overlap、品質閾値
- lens distortion、fixed planar warp、bounded residual correction
- exposure/color、seam、blend、crop、quality/memory/timing metrics

完了条件: `HG-0001/0002`で承認した契約を実画像で満たす。

## M3: mode別撮影・合成統合MVP

- .NET 10/WPFと単一C++ Camera Agent
- 明示`SingleCamera`／`DualCamera`、no-auto-fallback、process-wide排他、Live View handoff、durable sequential transaction。現`hardware.v1` handoffは撮影後に有限一frameを確認して停止・SDK closeするprobeで、継続stream再開は今後の実機受入対象
- `SingleCamera`: exactly-one D810、canonical original一件、stitch `NotApplicable`、画像処理なしの明示export、撮影設定read-only
- `DualCamera`: CAM-A→CAM-B、setup assistant、approved rig profile、bounded correction、stitch/restitch
- 部分失敗・再起動・原本保持

実機統合開始条件: M1A/M1B transport証拠、mode別品質contract、M3P置換差分が揃う。software-only mode contractは先行できるが実機合格へ読み替えない。

## M4: 受入・配布

- 統合100件、USB異常系、容量不足、clean-PC導入
- installer、診断、操作手順
- Nikon SDK、OpenCV、native dependencyの再配布確認
- product ownerのrelease判断

## リファクタリング方針

SingleCamera software boundaryを維持しながらSingle／Dual統合製品へ移行する詳細な構成レビュー、優先度、依存関係、受入証拠は[アプリ構成レビューと実装計画](APP_ARCHITECTURE_REVIEW_AND_IMPLEMENTATION_PLAN.md)に記録する。計画はProposedであり、hardware v1を一括置換せず、共通domain・operator event schema・contract/CIを先行する。

Phase 0は安全入口を先に分離した。次の構造整理は挙動を変えない小分けで行う。

1. `cli`: option解析、command dispatch、安全分類
2. `evidence`: summary validation、JSON、atomic persistence
3. `transaction`: direct/fake、hybrid、Live View handoff
4. `test_support`: fake transportとdomain別test executable

Nikon SDK/WPD adapter内部の大規模分割は実機回帰リスクが高いため、M1A再開直前には行わない。各分割はcharacterization testを先に置き、SDK有無の全suiteを維持する。

## MVP後

- TIFF/16-bit、NEF/RAW、Live View二台同時表示、GPU、遠景パノラマ、三台以上
