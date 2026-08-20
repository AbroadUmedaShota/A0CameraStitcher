# 合成アルゴリズム推奨事項 実行backlog

更新日: 2026-08-19

対象開始点: `codex/single-camera-product` / `a58f6562f5bd015e2d8f044fe49f322b0074319c`

## 1. 位置付け

本書は`docs/STITCHING_VALIDATION_AND_EXPERT_REVIEW.md`の推奨事項を、依存順、gate、実装単位、完了証拠へ分解した実行補助資料である。正式な要求と順序は次を優先し、本書だけでhuman gate、`Ready`、製品合格を確定しない。

1. `docs/PRODUCT_REQUIREMENTS.md`
2. `.autodev/requirements/normalized.json`
3. `.autodev/plan.json`
4. `.autodev/human-gates/open.json`

状態の意味:

- `Done-local`: 現working treeで実装し、記載したsoftware-only検証がfreshに合格。未commit・未統合であり、L1や製品合格ではない
- `Ready`: 現行承認の範囲内で次に着手可能
- `Decision-required`: 技術案またはproduct decisionが先
- `Authorization-required`: 対象と禁止操作を明示したoperatorまたはengineer-adminの個別承認が先
- `Blocked`: human gateまたは明示的な実機再開が先

## 2. 推奨実行順

`安全なfixed-warp強化 -> artifact/metric契約 -> HG-0001/0002 -> corpus/profile -> 完成pipeline -> offline受入 -> HG-0003B -> hardware E2E -> HG-0005/release`

## 3. Action register

| ID | 優先度 | 状態 | Gate・依存 | 対応内容 | 完了証拠 |
| --- | --- | --- | --- | --- | --- |
| SW-COV-001 | P0 | Done-local | なし | crop後の全画素についてCAM-A/B由来bitを画素値と独立に評価し、両方無効ならencode前にfail closed | 真黒の有効画像は成功、shearによるwedgeは拒否、wedgeを完全に除くcropは成功、JPEG/partial公開0、原本不変 |
| SW-HOM-001 | P0 | Done-local | SW-COV-001 | source矩形内をprojective denominatorの0線が横切るfixed homographyをprofile preflightで拒否。inverse poleがCAM-B外なら無効寄与として扱う | crossingはjob作成前に拒否、負のhomogeneous scaleは成功、CAM-A-only画素は成功 |
| SW-JPG-001 | P0 | Ready | SW-HOM-001 | stitch中のcanonical入力をimmutable locked snapshotにし、encode partialをfull decode・寸法・frame数・hashでpublish前に検証する | input同時変更、truncated/short write、decode失敗、rename競合で完成物0。原本不変。生成outputのsize ceilingは別decisionまで変更しない |
| SW-FAULT-001 | P0 | Ready | SW-JPG-001 | encode、flush、rename、process termination、disk full、既存partial/outputのfault seamとnegative testを追加 | 破損success 0、既存置換0、orphan状態が一意、retry 0 |
| DEC-ART-001 | P0 | Decision-required | EXP-05 | immutable `StitchJob` directoryの単一commit pointを、hash結合manifestまたはatomic markerの一方へ固定 | flush/publish/state順序、crash後terminal判定、retentionをdecision record化 |
| ART-001 | P0 | Decision-required | DEC-ART-001 | `CaptureTransaction`、`StitchJob`、`ReviewRecord`、`ExportRecord`、`DiagnosticBundle`を別artifactとして永続化 | restitchは新job、file存在だけでsuccessにしない、reviewはmetric/hard gateを変更しない |
| MET-001 | P0 | Decision-required | EXP-01からEXP-05 | metric definition、job metric result、statistical plan/resultのversioned schemaを定義 | unit、座標、mask、sampling、aggregation、丸め、境界、confidence、uncertainty、no-result、failure codeをmachine-readableに検証 |
| DEC-QUAL-001 | P0 critical path | Decision-required | EXP-01からEXP-05 | A0品質、最終rig、DPI/crop、flatness/parallax、residual、色、seam、p95/memory、corpus/statisticsを決定 | product ownerが根拠付きで`HG-0001`、`HG-0002`をcloseし、`WI-0020`完了 |
| CORP-001 | P1 | Blocked | DEC-QUAL-001 / `WI-0021` | 権利処理済みD810 corpusと独立oracleをcalibration/development/locked holdout/releaseへ分離 | manifest/hash/権利/撮影条件/期待結果、set間重複0、failure-sensitivity証拠 |
| PROF-001 | P1 | Blocked | CORP-001 / `WI-0022` | lens/rig calibration generatorとcanonical profile hash、provenance、expiry、schema/engine compatibilityを実装 | round-trip、再生成再現性、C++/adapter/.NET意味一致、profile mutation 0 |
| ALG-001 | P1 | Blocked | PROF-001 / `WI-0023` | lens correction、fixed warp、bounded residual、exposure/color、seam/blend、cropを一回のresamplingへ接続 | approved fixtureだけ成功。free homography、clamp、profile learning 0。over-limit/no-resultは拒否 |
| QUAL-001 | P1 | Blocked | ALG-001、MET-001 / `WI-0024` | pre/post-encode quality、coverage、crop、sharpness、色、seamとresource metricを接続 | good pass、damaged fail、raw値・版・補正・理由を記録。独立oracleと一致 |
| PERF-001 | P1 | Blocked | ALG-001、DEC-QUAL-001 | 実寸D810用benchmark、tile/memory計測、determinism、長時間反復を追加 | 16/32 GB基準機でp50/p95/max、peak working set、leak、codec/versionを記録し承認値内 |
| M3-STI-001 | P1 | Blocked | QUAL-001 / `WI-0034` | versioned adapterへartifact/state/metricを接続し、quality reject、stitch failure、export failureを分離 | frozen snapshot、stitch一回、read-only same-ID recoveryだけ、retry/fallback 0 |
| ID-001A | P0 parallel | Ready | EXP-06、vendor/Windows正式資料 | documentedなSDK/WPD same-body候補、strict proof、旧map migration/invalidationをsoftware-onlyで検討・negative test化 | provider/schema/version/expiry、匿名projection、missing/duplicate/collision、旧map拒否を検証。物理操作0。安全な候補がなければ`No safe candidate` |
| ID-001B | P0 parallel | Authorization-required | ID-001A、個別read-only試験承認 | gateを閉じる目的だけの接続順変更、port swap、再接続を匿名read-onlyで検証 | capture/Live View/card/settings/delete/format/vendor operation/retry 0を記録し、engineer-adminが`HG-0003B`をcloseまたはBlock維持 |
| L2-ACC-001 | P1 | Blocked | QUAL-001 | locked holdoutによるoffline実画像受入 | 正常が承認基準内、境界外/no-resultが全件fail closed。hardware合格へ読み替えない |
| L3-OFF-001 | P2 | Blocked | L2-ACC-001、DEC-QUAL-001 | 権利処理済み既存pair、または本アプリ外の別途承認済み取得だけで、profile、意図的ずれ、日間差をoffline評価 | geometry/color/seam/crop/memory/timingが基準内、独立review Pass。ただし`HG-0003B`、製品capture、L4証拠へ読み替えない |
| L3-RIG-001 | P2 | Blocked | L3-OFF-001、`HG-0003B`、明示的実機再開 | 本repository/appのDualCamera laneで承認rigの再設置・意図的ずれ・日間差を評価 | frozen body/profile対応、誤pair0、禁止操作0を含めて独立review Pass |
| L4-E2E-001 | P2 | Blocked | L3-RIG-001、M3-STI-001、明示的実機再開 | 実WPF pair capture、stitch、review、restitch、exportとfault matrix | 原本消失、誤pair、retry、fallback、SDK/WPD overlapが0。承認p95/memory、100/100と別の統計試験に合格 |
| L5-REL-001 | P3 | Blocked | L4-E2E-001、`HG-0005` | clean-PC package、license、accessibility、diagnostics、retention、rollback、full regression | fresh L4証拠、package integrity、独立release review、権限者decision |

## 4. 今回実施したsoftware-only slice

変更範囲:

- `src/m2/offline_stitcher.cpp`
- `tests/offline_stitcher_tests.cpp`

新しいhard invariant:

- crop後に未被覆画素を一つでも含む場合は`stitched.jpg`をpublishしない
- validityはRGB値ではなくsampling可否で決める
- source画像矩形内をprojective denominatorの0線が横切るprofileは、path検査、decode、job作成より前に拒否する
- inverse transformがCAM-B外で未定義でも、それだけでCAM-A-only有効画素を失敗させない
- 一貫して負のhomogeneous scaleは同じ有効homographyとして許可する

このsliceが証明しないもの:

- 実写A0品質、承認済みrig、DPI、色、seam、registration、p95、memory
- L1からL5のいずれかの完了
- DualCamera identity、実機capture、hardware `Ready`
- `HG-0001`、`HG-0002`、`HG-0003B`、`HG-0005`のclose

## 5. 次の実装候補

次は`SW-JPG-001`を、一つの独立sliceとして行う。ただし、次を先に設計reviewする。

1. canonical入力をWICへ渡すlocked snapshot方式
2. generated outputに現行64 MiB入力上限を流用しないこと
3. full decode、frame数、寸法検証とhash計算の順序
4. fault injection seamと、publish前失敗時のpartial cleanup
5. `DEC-ART-001`前に導入してよい構造的検証と、commit protocol決定後まで待つ部分の境界
