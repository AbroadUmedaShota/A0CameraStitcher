# MVPロードマップ

## 現在の進め方

MVP全体は`in-progress`である。2026-08-26に`SingleCamera`のCamera Agent実機経路でone-shot、10回characterization、p95承認、100回耐久を完了した。残るSingleCamera作業は実WPF end-to-end、Continuous Live View handoff 10回、物理異常系である。2026-09-11に`main`の`56f3cb36182812969126a34cd12137105bf3840c`を照合し、Dualはsession-local operator binding、production `CaptureRecoveryOnly` backend、WPF経路、同一bindingの10回runnerが実装済みと確認した。次の主工程は実機one-shot・10組の受入と、未実装の100回runnerの開発である。Dual実機受入は未完了であり、撮影・原画像保存のみの実装を、合成・A0品質の合格とは扱わない。

| 実行レーン | 現在 | 次の完了条件 |
|---|---|---|
| 安全基盤 M0 | Complete | SDK有無各CTest 5/5、旧direct実機拒否、同一operator session内の別process排他、撮影／保存watchdog途中超過が合格 |
| 一台・非破壊 M1N | Active / Partial | identity-v3登録後のalias解決defectはsoftware修正済み。実D810 `sdk-status` v5をread-only再実行し、未広告・opaque値の扱いを確認 |
| オフラインpre-gate M2P | Active / WI-0022C software complete | `HG-0001/HG-0002`承認後に実リグ値・品質作業へ進む |
| simulated統合 M3P | Complete / Software-only | requirements 2.7.0の明示mode、CAM-A-only、no-auto-fallback、Single original一件、stitch N/A、local profile/exportをfresh contractで維持 |
| Dual software経路 | CaptureRecoveryOnly実装済み / HardwarePending | 通常`hardware-dual.v2`の4操作は維持。production binding hostではADR-0028に基づく撮影・回収専用経路を使用し、通常`start-reserved-pair`の合成経路は有効化しない。実WPF・実カメラでの受入を残す |
| 一台製品mode M1A/M3 | Camera Agent hardware Partial | one-shot 1/1、10/10、p95 `14.643秒`承認、100/100は完了。実WPF end-to-end、Live View handoff 10回、物理異常系を残す |
| 二台 Phase 0 M1B | 撮影・回収／10回runner実装済み、100回runner未実装 / HardwarePending | 二台one-shot受入→10組実測・p95承認→100回runnerを実装・検証した後に耐久試験。host lifetimeの適合確認と異常系を残す |
| 実リグ・製品統合 M2/M3/M4 | Human/Hardware Gated | `HG-0001/HG-0002/HG-0005`とDual先行実機証拠 |

現在の詳細は[CURRENT_STATUS.md](CURRENT_STATUS.md)、機能単位の検証キューは[FEATURE_VERIFICATION_PLAN.md](FEATURE_VERIFICATION_PLAN.md)を正本とする。

## 現在の実行順

継続Live View v2のprotocol、agent session、WPF開始／frame／停止／capture handoffはQA revise済みsoftware checkpointとして完了している。次工程は以下である。

1. 実績をGitHub Issueと正本文書へ同期し、SingleCamera残作業をWPF／Live View／物理異常系へ限定する
2. 実装済みCaptureRecoveryOnlyの安全条件を維持し、pair-level preflight・coexistence probeを含むDual実機one-shotを受入する
3. one-shot合格後、既存10回runnerで10組を実測し、p50/p95/maxと原画像・診断情報を記録する
4. 未実装の100回runnerを開発・ソフトウェア検証し、host lifetimeの適合を確認する。この開発は実機や本人回答待ちに依存させない。100回の実機実行は、10回結果とp95の承認後に行う。失敗時はその場で停止し、再実行で隠さずfault matrixも別に記録する
5. `HG-0001/HG-0002`承認後にA0合成品質とmode別WPF統合へ進み、最後にM4を行う

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

実装状態: inventoryと設定readback実機runは完了し、設定readbackは9項目中8項目を取得、`FileType=not-advertised`、focusはopaque値1である。WI-0010A contractに加え、SingleCamera用identity-v3を実装した。CAM-A、WPD serial digest、`exactly-one-current-session` SDK policyだけをfixed-local stateへ保存し、SDK Name/Interfaceを永続化しない。strict parser、上書き拒否、unsafe path、CAM-B、extra-camera、digest不一致をsoftware testでfail closedにする。操作者報告では実D810一台のexactly-oneとCAM-A identity-v3登録はcamera mutation 0でPassした。登録直後の`sdk-status`がlegacy mapへ誤routingしたdefectと、WPD digest照合前にSDKを列挙していたQA findingは、identity file→WPD exact-one/model/digest→SDK executor factory/enumerate/open/probeの二段階software contractへ修正済みである。前段失敗時はSDK call 0。実D810 v5再実行は未検証、Dual identity-v2 collisionは`HG-0003B`として残る。

## M2P: オフラインpre-gate

- 150/180/200 DPI候補、回転、重複、crop、shortfallの純粋計算
- rights-cleared synthetic fixtureと品質metric contract
- versioned rig-profile schemaとruntime trust検証
- `ready`、`ready-auto-correction`、`physical-adjustment-required`の純粋判定
- known synthetic差分から測定値を生成し、profile上限内だけ補正候補にするcontract

実装状態: 光学計算、schema、fixture、三状態setup-assessment、`WI-0022C`のsynthetic画像測定からprofile-bounded correction proposalへの接続はsoftware-only合格。測定値は決定的に保持され、over-limit、malformed、profile-mismatch、unapproved profileはfail closedし、profileを変更しない。最終リグ、承認済み閾値、実写A0品質、実機性能は証明しない。

## M3P: simulated統合基盤

- .NET 10 domain contractとversioned Named Pipe protocol
- fake CAM-A/Bによるdurable transaction、片側失敗、crash/restart
- 全画面に`SIMULATED / 実機未接続`と`NO AUTO RETRY`を表示するWPF shell

実装状態: 2026-08-10のrequirements 2.6.0 fresh Release実行でbuild 0 warning/0 error、Foundation 19/19、Operator Shell 15/15、`Test-M3Simulated.ps1` Pass。さらにWI-0022C追加後のSDK-less Release CTest 7/7で`hardware_camera_agent_contracts`を含むC++ software boundaryを確認した。既存のlicensed-SDK-enabled 6/6 evidenceは保持する。明示mode、Single original一件、他alias未開始、stitch N/A、明示export、mode lock、no-auto-fallbackをsoftware-onlyで確認したためM3PはCompleteとする。CTestではcamera commandを送っておらず、実D810を使うWPF Camera Agent実行、WPD/SDK、actual JPEG、製品受入とは分離する。

requirements 2.7.0のSingle-first追加後は、fresh SDK-less／licensed-SDK-enabled Release CTest各7/7、.NET Release build 0 warning/0 error、Foundation 19/19、Operator Shell 16/16、M3 boundary scriptに合格した。後続sliceで長寿命Camera Agent、strict `hardware.v2`、memory-only frame、heartbeat/max lifetime/backpressure、WPF start/stop、stop-before-v1-capture、成功後だけrestartをsoftware実装した。最終fresh結果はFoundation 20/20、Operator Shell 17/17、C++全CTest 7/7を記録し、実機受入は未検証のまま残す。

2026-08-17のDual専用schema `a0.camera-agent.hardware-dual.v2` sliceでは、4操作、durable pair store、予約済み開始、同一ID typed recovery、strict semantic preflight、fake backend限定CAM-A→CAM-B orchestrator、複数terminal journalを実装した。A失敗時B 0、B失敗時A原本保持、共有180秒deadline、自動retry 0、terminal atomic publish後だけCompleted応答を契約化している。fresh結果はFoundation 22/22、DualCamera 18/18、Operator Shell 22/22、SDK-less／licensed Debug/Release CTest各10/10、M3 Release/Debug、正式DualCamera WPF flow Passである。当時はproduction Dual経路が未接続だった。その後CaptureRecoveryOnlyのproduction backend・WPF経路・10回runnerは実装済みとなったが、実機受入は未完了である。現在の境界は上記実行レーンとM1Bを参照し、この履歴を実機Passに読み替えない。

## M1A: D810一台・物理撮影／SingleCamera transport受入 Phase 0

- 明示再開後、SDK/WPD双方で同じ登録済みaliasのD810がexactly oneであることを確認し、専用empty/cleared cardでone-shot 1/1
- 合格後に10/10 capture/recovery/delete/empty-after
- Live View handoff 10回
- USB切断、software process再起動からの新規transaction復旧（物理power-cycle/power-off復旧は2026-08-09判断でN/A）

現在: 2026-08-26に専用empty spoolを用意し、Camera Agent実機経路のone-shot 1/1、10/10、p95承認、100/100を完了した。原画像111件を再検証し、安全指標は全て0件だった。実WPF end-to-end、Continuous Live View handoff 10回、物理USB切断・保存先障害は未実施のため、M1A/M3全体は`Partial`を維持する。

## M1B: D810二台 Phase 0

- 同一Agent sessionで二台の候補を確認し、一台ずつのLive Viewで操作者が`CAM-A/B`を明示割当
- USB再接続・台数変更・Agent再起動・SDKエラーでは割当を無効化し、再bindingを要求。接続順やUSB portから自動復元しない
- CaptureRecoveryOnly one-shot受入後、同じbindingを保持する既存10回runnerで`CAM-A → CAM-B`順次transaction 10組を実測
- 100回runnerは未実装。実装・検証とhost lifetimeの適合確認、10回結果・p95承認後に100/100、二台異常系、最終transport判断

旧方式の履歴: D810 PnP/SDK/WPD各2台、read-only inventory、`hybrid-capture-pair` software contractを確認した。CAM-A→CAM-B、pair共有180秒watchdog、A失敗時B未開始、B失敗時A原本保持、retry 0、sync非保証、100組集計、p50/p95/max匿名時間統計、CAM-A後の途中停止recovery診断はSDK有無各CTest 5/5で合格した。旧SDK source-ID mapを無効化してMAID Source `Name`/`Interface` identity-v2へ変更しても二台で衝突し、本体固有propertyや安全なSDK/WPD相関は確認できなかった。旧CAM-B checkpointは恒久的なDual identity証明ではない。

現在: ADR-0025のsession-local operator bindingとADR-0028のModule保持境界を使うCaptureRecoveryOnly backend・WPF経路、および[10回runner](../src/m3/OperatorShell/Hardware/CaptureRecoveryOnlyTenRunCoordinator.cs)は実装済みである。[集計・p95承認記録処理](../src/m3/OperatorShell/Hardware/CaptureRecoveryOnlyRunEvidence.cs)は10件・100件の記録を扱うが、実機実行を証明せず、100回runnerの代わりにはならない。Dualのone-shot・10組・100組・異常系の実機受入は未完了で`HardwarePending`を維持する。合成は`Pending`、A0品質は`Unapproved`であり、実シャッター同期も保証しない。物理power-cycleと実power-off復旧のN/A判断は維持するが、Agent再起動・USB再接続時の安全停止と再binding確認は残る。

## M2: 実リグ・オフライン合成PoC

- 承認済みA0 chart、lens、距離、overlap、品質閾値
- lens distortion、fixed planar warp、bounded residual correction
- exposure/color、seam、blend、crop、quality/memory/timing metrics

完了条件: `HG-0001/0002`で承認した契約を実画像で満たす。

## M3: mode別撮影・合成統合MVP

- .NET 10/WPFと単一C++ Camera Agent
- 明示`SingleCamera`／`DualCamera`、no-auto-fallback、process-wide排他、Live View handoff、durable sequential transaction。`hardware.v1`の有限probeとは別に、継続session、heartbeat、bounded lifetime、backpressureを持つ`hardware.v2`をsoftware実装済み
- `SingleCamera`: CAM-A identity-v3、exactly-one D810、30日app-approved read-only profile、canonical original一件、stitch `NotApplicable`、操作者選択fixed-local folderへのbyte-identical `7360×4912` export
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
