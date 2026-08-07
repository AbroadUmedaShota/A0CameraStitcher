# MVPロードマップ

## 現在の進め方

MVP全体は`in-progress`である。空カードが必要なM1Aだけを`Deferred`とし、実機不要またはD810一台を変更せず確認できる作業を止めない。既知の90 payload objectは物理状態が変わるまで再確認しない。

| 実行レーン | 現在 | 次の完了条件 |
|---|---|---|
| 安全基盤 M0 | Complete | SDK有無各CTest 5/5、旧direct実機拒否、同一operator session内の別process排他、撮影／保存watchdog途中超過が合格 |
| 一台・非破壊 M1N | Active / Partial | SDK setting command traceを自動検証し、未広告・opaque値を明示処理 |
| オフラインpre-gate M2P | Active | synthetic画像の測定値を上限付き補正判定へ接続 |
| simulated統合 M3P | software-only完了 | 実Camera Agentとの差分を維持し、実機合格へ読み替えない |
| 一台・物理撮影 M1A | Operator Deferred | 明示再開とempty spool確認後にone-shot 1/1から開始 |
| 二台 Phase 0 M1B | Waiting Hardware | 二台目D810と`HG-0003B` |
| 実リグ・製品統合 M2/M3/M4 | Human/Hardware Gated | `HG-0001/0002/0003B/0005`と先行実機証拠 |

現在の詳細は[CURRENT_STATUS.md](CURRENT_STATUS.md)、機能単位の検証キューは[FEATURE_VERIFICATION_PLAN.md](FEATURE_VERIFICATION_PLAN.md)を正本とする。

## 現在の実行順

1. `WI-0010A`: principal setting readbackのnative SDK command-trace contract
2. `WI-0022C`: synthetic shift/rotation/scale/exposure/color測定とbounded decisionの接続
3. M3 simulatedと将来の実Camera Agentとの差分・journal強化
4. 操作者がカード作業を明示再開した場合だけM1A one-shot
5. 二台目入手後にM1B
6. `HG-0001/0002`承認後に実M2、続いてM3/M4

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

- `CAM-A`のSDK/WPD匿名inventoryと電源再投入後のPnP identity continuity
- SDK status、Live View状態、JPEG/露出/ISO/WB/focus capabilityのreadback
- 撮影設定capabilityへのwrite、capture、Live View開始、WPD、deleteがないことをnative command traceで検証

実装状態: inventory、identity continuity、設定readback実機runは完了。設定readbackは9項目中8項目を取得し、`FileType=not-advertised`、focusはopaque値1である。native command traceが未実装なのでM1NはPartial。

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

実装状態: Release build警告0、Foundation契約11/11、一括shell検証がsoftware-only合格。起動同意、readinessと操作可否、設置三状態、共通結果、失敗復旧、保守タブを実装した。実WPF accessibility walkthrough、実C++ Camera Agent、D810、WPD/SDK、実JPEG、MVP合格とは分離する。

## M1A: D810一台・物理撮影 Phase 0

- 明示再開後、専用empty/cleared cardでone-shot 1/1
- 合格後に10/10 capture/recovery/delete/empty-after
- Live View handoff 10回
- USB切断、電源断、アプリ再起動からの新規transaction復旧

現在: 操作者の指示で保留中。接続中cardは最後のread-only証拠で90 payload objectだった。カード交換または手動backup/clearの報告があるまで、`spool-status`を含め再実行しない。

## M1B: D810二台 Phase 0

- 一台ずつSDK/WPD identityを同じ`CAM-A/B`へbinding
- 接続順・USB port変更後の復元
- `CAM-A → CAM-B`順次transaction 10件
- 100/100、二台異常系、最終transport判断

現在: 二台目D810と`HG-0003B`待ち。

## M2: 実リグ・オフライン合成PoC

- 承認済みA0 chart、lens、距離、overlap、品質閾値
- lens distortion、fixed planar warp、bounded residual correction
- exposure/color、seam、blend、crop、quality/memory/timing metrics

完了条件: `HG-0001/0002`で承認した契約を実画像で満たす。

## M3: 撮影・合成統合MVP

- .NET 10/WPFと単一C++ Camera Agent
- process-wide排他、Live View handoff、durable sequential transaction
- setup assistant、approved profile、bounded correction、stitch/restitch
- 部分失敗・再起動・原本保持

開始条件: M1B transport判断、M2品質contract、M3P置換差分が揃う。

## M4: 受入・配布

- 統合100件、USB異常系、容量不足、clean-PC導入
- installer、診断、操作手順
- Nikon SDK、OpenCV、native dependencyの再配布確認
- product ownerのrelease判断

## リファクタリング方針

Phase 0は安全入口を先に分離した。次の構造整理は挙動を変えない小分けで行う。

1. `cli`: option解析、command dispatch、安全分類
2. `evidence`: summary validation、JSON、atomic persistence
3. `transaction`: direct/fake、hybrid、Live View handoff
4. `test_support`: fake transportとdomain別test executable

Nikon SDK/WPD adapter内部の大規模分割は実機回帰リスクが高いため、M1A再開直前には行わない。各分割はcharacterization testを先に置き、SDK有無の全suiteを維持する。

## MVP後

- TIFF/16-bit、NEF/RAW、Live View二台同時表示、GPU、遠景パノラマ、三台以上
