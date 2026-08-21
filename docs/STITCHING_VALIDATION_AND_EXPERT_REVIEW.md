# 合成アルゴリズム利用可能化の検証計画・有識者検討依頼書

更新日: 2026-08-18

調査対象: `codex/single-camera-product` / `a58f6562f5bd015e2d8f044fe49f322b0074319c`

文書状態: 検討用draft。2026-08-18に共有されたChatGPTタブ「md内容の検討」の指摘と候補値を参照したが、同タブは助言資料であり、repositoryのsource of truth、承認記録、実測証拠ではない。

承認状態: `HG-0001`、`HG-0002`、`HG-0003B`、`HG-0005`はすべてopenのままである。本書とリンク先に記載された数値候補は、権限を持つ担当者が根拠とともに承認するまで、実装既定値、合格閾値、`Ready`根拠にしてはならない。

実行状況は`docs/STITCHING_RECOMMENDATION_BACKLOG.md`で追跡する。同backlogは本書を実行単位へ分解した補助資料であり、`.autodev/plan.json`やhuman gateを置き換えない。

## 1. 目的

本書は、DualCameraの合成処理を、現在のsoftware-only固定変換から、実写A0を品質保証して運用できる状態へ進めるための検討項目、検証証拠、専門家への依頼事項をまとめる。

ここでいう「利用可能」は、単に`stitched.jpg`を生成できることではない。承認済みprofileと正しい二枚の原本を使い、合成後品質が数値基準を満たす場合だけ成功し、範囲外・測定不能・入力異常では理由を伴ってfail closedになり、原本と診断証拠が保持されることを指す。

## 2. 現状認識

### 2.1 現在実装されている範囲

- CAM-Aを基準座標とする固定3x3変換
- CAM-Bへの逆写像と双線形補間
- 左右または上下の幾何重複全体を使う線形feather
- profile指定の固定pixel crop
- WICによる24-bit BGR JPEG decode/encode
- `stitched.jpg.partial`から`stitched.jpg`へのnon-replacing atomic publish
- 原本を変更せず、再合成を別jobへ出力する契約
- approved/schema/provenance/validityと特異行列の基本検証
- synthetic画像について、位置・回転・倍率・露出・色の差を測定し、profile上限に基づいて三状態を判定するsoftware-only契約
- M3からnative adapterを起動し、capture、stitch、restitch、exportを管理するsoftware-only flow

### 2.2 製品要求に対して未完成または未接続の範囲

- 各cameraのレンズ歪み補正と実rig用profile生成
- 実画像から得た残差を固定profileへ一時適用する処理
- 測定不能または低confidenceを拒否するproduction measurement
- 露出・色差補正
- fixed seamまたはGraph Cut seamの製品選択
- multi-band等を含むblend方式の製品選択
- 合成後のregistration、色、seam、coverage、crop品質測定
- 品質上限外をsuccessにしないend-to-end quality gate
- profile schema 1.1の全情報をC++、adapter、.NET、保存済みtransactionで同一に扱う契約
- 実寸D810画像の時間・メモリ・長時間安定性
- 権利処理済み実画像fixtureと独立quality oracle
- 回転・射影時の実overlap mask、未被覆pixel、black wedgeを拒否するcoverage検証
- 合成JPEGの完全decode、実寸、hash、profile/algorithm versionを照合するdurable manifest
- 原本と合成結果をzoom、pan、seam拡大で比較し、accept/rejectできる実画像Review UI
- 校正済み二台固定rigと実写A0の合格証拠
- 二台を同じ物理bodyとしてSDK/WPD間で安定識別する実機証拠

現状は`Software fixed-warp prototype / Real-image and Hardware pending`であり、実写A0品質や製品利用可能を示すものではない。

## 3. 変更してはならない前提

- 対象はUSB接続Nikon D810二台、固定rig、静止した平面に近いA0原稿である。
- 実シャッター同期を保証しない。動体、立体、handheld、遠景panoramaは対象外とする。
- 撮影ごとに自由なhomographyを再推定しない。
- 承認済み固定profileに対する、承認済み上限内の一時的残差補正だけを許可する。
- 上限外をclampして成功扱いにしない。物理調整または再校正を要求する。
- 撮影結果からprofileを自動学習・自動更新しない。
- CAM-A、CAM-Bのcanonical `original.jpg`を不変の製品原本として保持する。
- previewを原本または合成入力に使用しない。
- `DualCamera`が成立しない場合に`SingleCamera`へ自動fallbackしない。
- hardware evidenceがない状態を`Ready`または実機合格と表示しない。

## 4. 「利用可能」の段階定義

### 4.1 maturity gate

各段階のPassを次段階へ読み替えない。

| 段階 | 状態名 | 最低限必要な証拠 |
| --- | --- | --- |
| L1 | Software-ready | 数学、profile、codec、I/O、failure pathのDebug/Release contractとproperty testがfreshに合格。hardware operationは0 |
| L2 | Offline real-image-ready | 権利処理済み実D810画像corpusで、独立oracleに対して正常画像が合格し、境界外画像が確実に拒否される |
| L3 | Calibrated-rig-ready | 承認済み二台rig、chart、profileで、再設置を含む反復結果がgeometry、色、seam、crop基準内 |
| L4 | Product-ready | 実WPFからpair capture、合成、review、restitch、export、failure/recoveryまで実機で成立し、p95、memory、durabilityを満たす |
| L5 | Release-ready | clean Windows 11 package、依存関係・再配布、運用・診断・rollback、全回帰が承認済み |

L3で既存の権利処理済み実画像や、アプリ外の承認済み手順で取得された画像を使う場合は、取得者、取得方法、body対応、hash、撮影条件をmanifestへ固定する。その結果はofflineのrig/画質評価にだけ使用でき、`HG-0003B`、製品capture flow、L4の証拠にはならない。本repositoryまたは本アプリから二台の物理操作を行う場合は、先に`HG-0003B`を解決する。

### 4.2 artifact model

再合成、export、診断出力を一つのtransactionへ混在させない。最低限、次を別artifactとしてimmutableに扱う。

| artifact | 必須内容 | 不変条件 |
| --- | --- | --- |
| `CaptureTransaction` | capture ID、CAM-A/B alias、両original path/size/hash/full-decode結果、撮影設定、redacted identity proof snapshot/hash、rig/profile snapshot | raw識別子を保存せず、canonical originalと開始時snapshotを後から置換しない |
| `StitchJob` | stitch job ID、capture ID、profile/hash、engine/codec/version、parameter、補正提案/適用/拒否、全metric、output hash、判定 | restitchごとに新規jobを作り、過去jobを上書きしない |
| `ReviewRecord` | stitch job ID/hash、reviewer、時刻、判断、理由、自動metric/hard gate snapshot | metricを書き換えず、hard gateをoverrideせず、reviewの訂正は追記する |
| `ExportRecord` | stitch job ID、source/destination path、前後hash、decode検証、時刻、結果 | 合格済みjobだけをbyte-identicalにexportし、sourceを変更しない |
| `DiagnosticBundle` | rejected candidate、validity mask、metric、log、failure code、環境情報 | 製品成果物と名前・commit pointを分け、実識別子と原画像byteをlogへ出さない |

### 4.3 state model

一つの`Succeeded/Failed`へ圧縮せず、少なくとも次の状態群を別々に永続化する。状態遷移と許可されるside effectはversioned contractにする。

| state machine | 必要な状態 |
| --- | --- |
| Setup | `Ready`、`ReadyWithBoundedCorrection`、`PhysicalAdjustmentRequired`、`MeasurementUnavailable`、`ProfileInvalid`、`HardwareBindingUnavailable` |
| Capture | `Created`、`Reserved`、`CaptureInProgress`、`ResponseUnknown`、`RecoveryPending`、`PairCaptured`、`FailedPartial`、`WatchdogExpired`、`CaptureFailed`、`Cancelled` |
| StitchJob | `Created`、`PreflightRejected`、`MeasurementRejected`、`Processing`、`StitchFailed`、`QualityRejected`、`Succeeded`、`Cancelled` |
| Review | `NotRequested`、`Pending`、`Accepted`、`Rejected` |
| Export | `NotRequested`、`Exporting`、`Succeeded`、`Failed` |

`NoResult`、low confidence、coverage不明、post-encode検証不能は成功の別名にせず、対応する拒否状態へ遷移させる。`ResponseUnknown`と`RecoveryPending`ではfrozen snapshotによるsame-ID read-only query以外を許可せず、新規reservation、capture再dispatch、次transport、canonical rename、card delete、成功遷移を禁止する。`FailedPartial`では既に確定したPC originalを診断用に保持する。operator reviewは測定値やhard gateを上書きせず、別の`ReviewRecord`として残す。

## 5. 全検討項目

### 5.1 出力・品質の受入契約

検討する内容:

- 目標DPIを150、180、200のどれにするか
- 出力orientation、pixel寸法、物理寸法の扱い
- 許容crop量、内容欠落、余白、未定義pixelの上限
- registration誤差、二重像、局所歪み、seam誤差の合否値
- exposure差、white balance差、色差の指標と合否値
- overlap内外の解像感、細線、小文字、halftoneの保持基準
- JPEG圧縮品質、chroma subsampling、metadata、ICC profileの製品契約
- 自動metricと人の目視判定の優先関係
- 合格率、false pass、false rejectの許容値

必要な証拠:

- `HG-0001`として承認された数値表
- 指標ごとの測定方法、測定位置、集計方法、境界値の丸め規則
- 正常、境界、上限超過の参照画像と期待判定

### 5.2 最終rig・光学条件

検討する内容:

- cameraの向き、左右または上下配置、camera order
- lens pair、焦点距離、focus固定方法、絞り、working distance
- 必要overlap、画角、crop後の最小有効DPI
- A0全面、overlap corridor、局所領域ごとの平面度、depth差、parallax上限
- 出力canvasのmetadata値ではなく、実入力解像度とwarpのlocal Jacobianから求める局所有効DPI
- exposure、ISO、white balance、picture control等の固定方針
- 照明の種類、位置、均一性、flicker、反射対策
- rig剛性、振動、温度変化、再設置の許容範囲
- A0 calibration chartの寸法精度、色票、権利、保管方法

必要な証拠:

- `HG-0002`として承認されたrig仕様書
- 光学成立性計算と実測の一致
- 正常設置、意図的なずれ、日を跨ぐ再設置の比較

### 5.3 実画像fixtureと独立oracle

検討する内容:

- 権利処理済みD810 JPEG corpusの構成
- 細線、小文字、格子、斜線、色票、無地、低texture、高contrast、画像端を含む原稿
- 正常、目標境界、補正上限、上限超過、overlap不足、設定不一致、露出差、色差を持つpair
- calibration set、development validation set、変更中に見ないlocked holdout、release confirmation setの四分離
- 実装と独立した基準座標、色、crop、expected resultの作成方法
- residual correctorが使うfeature、seam cost、学習用画像を、同じ欠陥の合否oracleとして再利用しないこと
- corpus間の原稿、撮影日、rig状態、派生画像の重複を検出する方法

必要な証拠:

- fixture manifest、hash、chart版、撮影条件、利用権、期待用途
- 実装と同じ式を複製しないoracle
- testが意図した欠陥を実際に検出するfailure-sensitivity証拠

### 5.4 lens calibrationとrig profile lifecycle

検討する内容:

- camera別のintrinsic、radial/tangential distortion model
- lens correctionとplanar warpの順序
- CAM-BからCAM-Aへのhomography生成方法
- 座標系、pixel center、matrix方向、単位、rotationの定義
- baseline reprojection residualとholdout residual
- profile ID、version、schema、provenance、camera applicability、設定snapshot/hash
- 全parameterのcanonical serializationとcontent hash、承認者、承認日時、承認根拠
- signatureまたは承認済みhash allowlist、schema/engine compatibility、失効list
- profile有効期間と再校正trigger
- camera/lens交換、focus変化、rig移動、衝撃、温度、品質drift時の失効方法
- draft、期限切れ、alias不一致、設定不一致、破損profileのfail-closed処理

必要な証拠:

- profile generatorのround-trip test
- 同じrigから再生成したprofileの再現性
- schema、C++、adapter、.NET、transaction snapshot間の意味一致
- profile不変と、自動更新0の証拠

### 5.5 JPEG inputと色管理

検討する内容:

- D810 `7360x4912`、JPEG Fine L以外の拒否条件
- 現行64 MiB ceilingを、canonical original入力とexport source/partialの各locked readで維持し、`limit-1`、`limit`、`limit+1`を検証すること
- engineが生成するstitched candidate/outputには別の生成後size ceilingが必要か。64 MiBと同一にするかをresource/quality根拠で決めること
- baseline/progressive、grayscale、CMYK、壊れたscan、巨大metadataの扱い
- EXIF orientationを正規化するか、非標準値を拒否するか
- ICC profile、sRGB前提、camera色処理、metadata保持・削除方針
- gamma encoded値とlinear lightのどちらで補正・blendするか
- decoder/encoder差異とWindows/WIC version差の扱い

必要な証拠:

- codec variant matrixとmalformed corpus
- decode後の完全pixel read、寸法、色空間、orientationの検証
- JPEG encode policyの固定と、再圧縮劣化の測定

### 5.6 fixed warpの数学・数値安全性

検討する内容:

- identity、integer/subpixel translation、rotation、scale、perspectiveの既知解
- forward/inverse consistency、rounding、canvas bounds、crop座標
- singularだけでなくnear-singularまたはill-conditioned matrix
- projective denominatorが画像内部で0へ近づく場合
- 極端なscale、負座標、巨大canvas、整数overflow、NaN、Inf
- 変換後四角形外の空白、hole、black wedge、片側だけのpixel
- alias入替、layout不一致、matrix方向誤りの拒否
- resamplingによるaliasing、edge、細線の劣化

必要な証拠:

- 独立計算によるunit/property/metamorphic test
- limit-1、limit、limit+1と数値epsilon周辺のtest
- randomized caseと、問題発生時に固定fixtureへ縮小できる仕組み

### 5.7 撮影ごとの残差測定と範囲限定補正

検討する内容:

- productionで測る残差modelをtranslation、similarity、affineのどこまで許すか
- fixed profileを基準にする探索範囲と、free homographyにならない制約
- 低texture、反復模様、細線、強い露出差、部分遮蔽でのconfidence
- 測定不能、複数解、low confidence、上限超過の拒否条件
- 位置、回転、倍率の補正を一回のresamplingへ統合する方法
- 補正値をtransaction限りにし、profileを変更しない方法
- 現行のglobal momentsによるsynthetic測定をproduction evidenceへ読み替えない境界

必要な証拠:

- known ground truthに対するbias、variance、worst case
- 目標値ちょうど、補正上限ちょうど、上限直上の判定
- 正常pairのfalse rejectと異常pairのfalse passの測定
- 測定不能時のstitch/capture side effectが0である証拠

### 5.8 exposure・color補正

検討する内容:

- luminance/EVと色差の測定領域
- global gain、per-channel gain、低周波field等の補正model
- ΔE76、CIEDE2000等、使用する色差指標
- clipping、black/white point、banding、gradient、noise増幅の抑制
- overlapだけに合わせて非overlap領域を悪化させない条件
- lighting driftとcamera setting mismatchの区別

必要な証拠:

- calibrated color targetと独立測色値
- 正常・境界・上限超過の補正前後metric
- 色補正でdetailや階調が悪化していない証拠

### 5.9 seam・blend・crop

検討する内容:

- fixed seamとGraph Cut等の候補比較
- linear featherとmulti-band等の候補比較
- A0文書の文字、罫線、写真、halftoneでの適性
- seamが文字や細線を横切る場合の二重像・blur
- overlap端、画像端、低texture、露出gradientでの挙動
- CAM-A有効mask、CAM-B有効mask、overlap、single-source、uncovered、crop-safeの各mask生成と保存
- black pixelの値ではなくvalidity maskによりhole、black wedge、未被覆領域を判定すること
- crop後に全内容、必要余白、最小DPIが残ること
- blendをgamma空間またはlinear lightで行う影響

必要な証拠:

- 同じholdout corpusに対する候補方式のblind比較
- seam可視性、registration、sharpness、処理時間、memoryの比較表
- 選定理由と、採用しなかった方式の棄却理由

### 5.10 合成後quality gate

検討する内容:

- registration、seam、色、coverage、crop、sharpnessをどの順に検査するか
- 平均だけでなく最大値、percentile、局所worst caseをどう扱うか
- 自動測定不能時の扱い
- 補正後に基準外なら`Succeeded`にせず、理由と測定値を返すstate model
- metricごとのID/version、定義、単位、座標系、mask、sample、集計、未丸め値での比較、閾値、境界値の包含規則
- `pass`、`fail`、`no-result`、`not-applicable`を区別するmachine-readable metric contract
- versioned metric definitionと、jobごとのraw value/confidence/uncertainty/statusを持つmetric resultを別schemaにすること
- operatorが自動合格をrejectできるか。identity、profile、coverage、crop、no-result、hard limitをoverrideできないこと

必要な証拠:

- good fixtureが合格し、各種damaged fixtureが対応する理由で不合格になるtest
- raw測定値、比較値、閾値、profile版、metric/oracle版、補正量、判定理由のjob記録
- quality不合格時も両原本と診断用outputが保持される証拠
- reviewを許す場合、reviewer、理由、時刻、対象job hashを別recordへ保存し、metricを変更しない証拠

### 5.11 file、atomicity、failure recovery

検討する内容:

- 原本のsize、SHA-256、full decodeをstitch前後に再確認
- hardlink、symlink、junction、reparse、same-file alias、path escape
- existing output、existing partial、read-only、permission、disk full、quota
- encode途中、flush、rename、process kill、app crash、Windows shutdown時の状態
- cancellation後のchild process、orphan partial、temp directoryの扱い
- publish後のJPEG再読込、hash、quality結果の永続化
- candidate生成、pre-encode quality、`.partial` encode、full decode、post-encode quality、manifest作成、product commitの厳密な順序
- immutable job directoryで、hashを結合したmanifestまたは`COMMITTED` markerのどちらか一つを唯一のcommit pointとして選ぶこと。→ **決着（[DECISIONS.md](DECISIONS.md) ADR-0026）**。`a0.stitch-job-manifest.v1`のnon-replacing atomic publishと直後の再読込検証を採用し、`stitched.jpg`の存在だけを成功としない扱いを実装済み（Issue #40）
- 選んだ単一commit pointについて、output/manifestのflush、atomic publish、state反映、crash後terminal判定の順序を固定すること → **決着（ADR-0026のcommit順序①〜⑥）**
- quality rejectは`stitched.jpg`としてpublishせず、`REJECTED`状態のdiagnostic candidateとして隔離すること
- restitchが原本や過去resultを置換しないこと
- exportが完成JPEGのbyte-identical copyであること

必要な証拠:

- fault injectionと再起動後の状態検査
- original消失0、既存file置換0、retry 0
- partialと診断artifactの明文化されたretention/cleanup policy

### 5.12 adapter、application、recovery統合

検討する内容:

- profile全項目とprofile hashをCLI引数ではなくversioned contractで渡す必要性
- engine/adapter/profile schemaのversion negotiation
- resultへoutput pathだけでなくmetric、補正、quality、failure reasonを返す型
- stderr、path、実camera identityのredaction
- process timeout、hang、crash、invalid response、oversized response
- active transactionでprofile、identity、原本、rig snapshotを固定すること
- response-unknown recoveryではfrozen transaction identity/capture/rig snapshotを使って同じtransactionのstatus/result/hashだけをqueryし、start validation、reservation、capture dispatchへ戻らないこと
- side effectを伴うcommandの自動retryを禁止し、read-only same-ID queryだけを明示的な上限・backoff付きで再試行可能にすること
- partial pairではstitchを開始しないこと
- quality不合格、stitch失敗、export失敗を別結果として表示すること

必要な証拠:

- public boundaryのcontract/integration test
- app/Agent restart、same-ID query、stitch一回、retry 0
- profile mismatch、engine mismatch、missing adapterのfail-closed test

### 5.13 performance、memory、determinism

検討する内容:

- stitch-onlyとcapture-to-product-JPEGのp50、p95、max
- provisional DualCamera 10秒目標を維持するか
- 16GB/32GB PCでのpeak working set、commit、allocation
- CPU、disk I/O、JPEG decode/encode、warp、blend別のprofile
- 連続実行時のmemory leak、COM object、handle、file lock
- 同じPCでのpixel/metric決定性と、別Windows/WIC versionで必要な再現性
- byte-identical JPEGを要求する範囲と、semantic/pixel toleranceでよい範囲
- cancellation latencyとresource ceiling
- deterministic regression corpusと、運用失敗率を推定する統計試験を分離すること
- trialの独立性、operator/日/rig再設置/原稿種別による層化、停止規則、欠測の扱い

必要な証拠:

- 実寸D810 pairを使ったrepeatable benchmark
- warm/cold条件、machine spec、build、engine/profile version付きreport
- 長時間反復後にresourceが戻ること
- 0 failure/100回でも片側95%上限は約2.95%であることを明記する。失敗率1%未満を片側95%で主張するなら、独立同条件という前提の下で0 failure/299回以上が一つの目安になる。sample数はEXP-04とproduct ownerが目的別に承認する

### 5.14 UI、observability、運用

検討する内容:

- profile ID/version/expiry、setup readiness、補正値、quality結果の表示
- `ready`、`ready-auto-correction`、`physical-adjustment-required`のoperator guidance
- lens/position/rotation/distance/lightingのどこを調整すべきかの案内
- capture、stitch、quality、exportを分けた状態表示
- failure後の再試行禁止と、新transaction開始手順
- recalibration通知、profile失効、diagnostic bundle、retention
- keyboard、focus、screen reader、色以外の警告表現

必要な証拠:

- 実WPFのoperator walkthrough
- 各failure codeから一意で安全な案内への対応表
- redacted event logと診断report

### 5.15 robustness、security、release

検討する内容:

- malformed JPEG、metadata bomb、極端profile、CLI outputに対するfuzz
- CPU、memory、diskのresource exhaustion
- untrusted path、reparse、ADS、network/removable destinationの拒否
- originalまたはpreview byte、実識別子をlogへ出さないこと
- OpenCV等を追加する場合のversion固定、脆弱性、再配布条件
- clean Windows 11でのinstall、smoke、uninstall、rollback
- engine、profile、Agent、appの互換性matrix

必要な証拠:

- bounded fuzz/negative testとcrash 0
- dependency/license inventoryと`HG-0005`承認
- clean machine package testとrelease candidate全回帰

### 5.16 evidence、version、freshness

検討する内容:

- requirement、test、build、engine、compiler/options、native dependency、codec、schema、profile、fixture、oracleのID/hash
- Windows build、machine class、camera firmware、lens/focus、rig placement、lighting、operator、実施日時
- engineまたはmetric変更、dependency/OS/codec更新、profile再生成、camera/lens/focus/rig/lighting変更時に、どの証拠を失効させるか
- raw measurement、threshold、decision、reviewerを追跡できるrequirement-to-evidence matrix
- software-only、offline real-image、calibrated rig、hardware E2Eの証拠を混在させないlabel

必要な証拠:

- 各gateについて、対象versionとartifact hashを固定したmanifest
- staleまたは対象外の証拠を自動的に`Pass`集計から外す規則
- 同じ入力から判定を再現できるraw valueとoracle/tool version

## 6. 有識者へ依頼すべき検討package

各packageは単独承認にせず、次のdecision ownerとcross-reviewを明示する。最終承認者はrepositoryのhuman gateに記録された権限者であり、有識者回答だけではgateを閉じない。

| package | 主担当 | 必須cross-review | 主な決定責任 |
| --- | --- | --- | --- |
| EXP-01 | 光学・document imaging | EXP-02、EXP-04 | rig、DPI、平面度、parallax、画質契約 |
| EXP-02 | calibration・computer vision | EXP-01、EXP-04 | 幾何model、残差補正、confidence/no-result |
| EXP-03 | color science・computational photography | EXP-01、EXP-04、EXP-05 | 色、seam、blend、JPEG contract |
| EXP-04 | 統計・test engineering | EXP-01からEXP-06 | corpus分離、sample数、false pass/reject、合否式 |
| EXP-05 | Windows C++・secure I/O | 実装責任者、EXP-03、EXP-04 | resource、commit、fault、security、dependency |
| EXP-06 | Nikon/transport担当 | architecture責任者、EXP-04 | same-body bindingとfail-closed protocol |

複数packageで値が食い違う場合は、安全側へ自動採用せず、衝突する要求、影響するgate、選択肢、追加証拠をdecision recordとしてproduct ownerまたはengineer-adminへ戻す。

### EXP-01 光学・固定rig・出力品質契約

推奨する有識者:

- document imaging、camera optics、metrologyの経験者

決めてほしい内容:

- 150/180/200 DPI候補からの製品目標
- lens、焦点距離、working distance、orientation、overlap、crop
- exposure、focus、white balance、照明
- calibration chartと撮影手順
- baseline残差、補正目標、補正hard limit
- registration、二重像、seam、色、sharpnessの合否値
- profile有効期間と再校正trigger

提出してほしい成果物:

- 数値入りrig仕様書
- 指標、単位、測定位置、集計、合否式を持つ品質契約
- 推奨chart、必要fixture、測定器、sample数
- 不確かさ、成立しない条件、再評価条件

このpackageは`HG-0001`と`HG-0002`の承認判断に必要な技術入力を作る。gateを閉じるのはproduct ownerの明示承認である。

### EXP-02 calibration・registration・残差補正

推奨する有識者:

- camera calibration、computer vision、画像registrationの経験者

決めてほしい内容:

- lens distortion modelとcalibration手順
- planar homographyの生成、holdout検証、condition判定
- fixed profileに許す残差model
- 探索範囲、confidence、ambiguity、low-texture拒否条件
- 一回resampling、interpolation、edge処理
- free homographyまたはprofile learningへ逸脱しない実装境界

比較してほしい候補:

- phase correlation、ECC、特徴点、fiducial等の残差測定
- translation、similarity、限定affine
- robust estimatorと明示的なno-result判定

提出してほしい成果物:

- 数学的な座標・matrix convention
- 候補比較、選定理由、棄却理由
- ground truth corpusと誤差分布
- target、hard limit、confidence、no-resultの判定式

### EXP-03 色管理・seam・blend・JPEG

推奨する有識者:

- color science、document imaging、computational photographyの経験者

決めてほしい内容:

- working color space、ICC、EXIF、gamma/linear処理
- exposure/color補正modelとclipping対策
- ΔE指標と合否値
- fixed seam対Graph Cut等、linear feather対multi-band等
- 文書文字・罫線・写真・halftoneでの品質優先順位
- JPEG quality、subsampling、metadata policy

提出してほしい成果物:

- 候補方式のblind比較結果
- 色、seam、sharpness、処理時間、memoryのtrade-off
- 採用parameterと許容範囲
- damaged fixtureと期待するfail判定

### EXP-04 品質評価・統計・受入試験

推奨する有識者:

- image-quality assessment、test engineering、統計的品質保証の経験者

決めてほしい内容:

- 自動metricの独立oracleとhuman reviewの関係
- 平均、最大、percentile、局所worst caseの採用
- false pass/false rejectの目標
- calibration画像とholdout画像の分離
- corpus構成、sample数、境界case、durability時の品質sampling
- 同一operatorと複数operator、日間差、再設置差の評価

提出してほしい成果物:

- requirement-to-evidence matrix
- test protocol、sample数、合否式、停止条件
- blind review用score sheet
- 正常・境界・異常fixtureの期待結果

### EXP-05 Windows画像処理・性能・堅牢性

推奨する有識者:

- Windows C++、WIC/OpenCV、画像処理性能、secure file I/Oの経験者

決めてほしい内容:

- 実寸D810 pairのmemory modelとresource上限
- WIC codec policy、version差、encode determinism
- atomic publish、cancellation、process kill、disk full、orphan partial
- reparse/hardlink/path aliasとcodec attack surface
- p95計測方法と、provisional 10秒目標の実現性

提出してほしい成果物:

- benchmark/fault/fuzz plan
- memory/time予算とinstrumentation
- threat modelとnegative test matrix
- native dependencyと再配布上の注意

### EXP-06 DualCamera same-body binding

これは画像合成方式そのものではないが、アプリで利用可能にする前の必須gateである。

推奨する有識者:

- Nikon SDK/MAIDまたはWindows camera transportの正式情報へアクセスできる担当者

決めてほしい内容:

- 二台接続時にSDK cameraとWPD deviceを同じ物理D810へ対応付ける、documentedで安定した方法
- 接続順、USB port、再接続に依存しないbinding
- property欠落、重複、変更時のfail-closed規則

提出してほしい成果物:

- 使用propertyまたは相関protocolの正式根拠
- documented候補をsoftware-onlyで検査するcontractとnegative fixture
- gateを閉じる目的だけの、三回の接続順変更と両port swapを含む匿名read-only identity test protocol
- read-only実行中のcapture、Live View、card access、settings write、delete、format、vendor operation、automatic retryがすべて0である証拠
- 安全な方法がない場合の明示的なBlock判断

このread-only identity testは、候補とprotocolを先に文書化し、operatorまたはengineer-adminが対象body、接続操作、禁止操作を明示承認した場合だけ実施できる。これはproduction operationや`Ready`証拠ではない。このpackageの証拠をengineer-adminが承認して`HG-0003B`を閉じるまでは、DualCamera実撮影、本アプリで取得したpairの製品E2E合成、Ready判定を開始しない。既存の権利処理済みpairによるoffline合成評価はL2/L3の範囲で継続できる。

## 7. 有識者へ渡す資料

- 本書
- `docs/PRODUCT_REQUIREMENTS.md`の`FR-STI-001`から`FR-STI-006`
- `docs/ARCHITECTURE.md`の合成pipelineと安全境界
- `.autodev/requirements/unresolved_questions.json`の`HG-0001`、`HG-0002`、必要に応じて`HG-0003B`
- `.autodev/plan.json`の`WI-0020`から`WI-0024`、`WI-0034`
- `docs/schemas/rig-profile.schema.json`
- 現行`src/m2/offline_stitcher.*`、`synthetic_measurement.*`、`setup_assessment.*`
- 現行testと、まだtest化されていないgap一覧
- 匿名化したrig図、lens情報、PC仕様、照明条件
- 権利処理済み画像fixture。実camera serial、SDK archive、customer originalは渡さない

## 8. 有識者回答の要求format

各packageについて、最低限次を回答してもらう。

1. 前提と対象外
2. 推奨案
3. 比較した代替案と棄却理由
4. 決定値、単位、許容範囲
5. 測定方法、器具、fixture、sample数
6. pass/fail/no-resultの判定式
7. 不確かさ、false pass、false reject
8. 再校正・再検証trigger
9. 未解決事項と追加で必要な証拠
10. 回答者、回答日、参照規格・資料
11. metric contract。metric ID/version、単位、座標、mask、sampling、aggregation、閾値、丸め、境界等号、no-result、failure code
12. artifact/state/commit pointへの影響と、許可・禁止するside effect
13. profile canonicalization/hash、schema/engine compatibility、承認・失効方法
14. requirement/test/build/profile/fixture/oracle/evidence IDとfreshness trigger
15. 数値ごとの区分。`approved`、`provisional`、`candidate`、`rejected`を混在させない

「一般に良好」「目立たない」だけの回答では受入条件にならない。数値化できない官能評価は、blind review手順、score、判定人数、合否規則を明示する。

回答本文に加え、少なくとも次のmachine-readable成果物を提出してもらう。

- metric definition: `requirement_id, metric_id, metric_version, description, unit, coordinate_space, mask, sampling, aggregation, rounding_rule, boundary_rule, pass_operator, pass_threshold, hard_limit, confidence_rule, uncertainty_method, no_result_rule, failure_code_map, oracle_id, status, decision_owner`
- metric result: `stitch_job_id, metric_id, metric_version, raw_value, comparison_value, confidence, uncertainty, result_status, failure_code, fixture_id, evidence_id, measured_at, tool_version`
- statistical plan/result: `claim_id, estimand, trial_unit, population, stratification, cluster, independence_assumption, sample_size, stopping_rule, missing_data_rule, ci_method, confidence_level, observed_trials, observed_failures, estimate, ci_lower, ci_upper, decision`

## 9. 推奨する検証順序

### Phase 0: human decision

- 有識者が`HG-0001`、`HG-0002`の技術入力を作り、product ownerが根拠付きで明示承認する。
- 出力契約、最終rig、quality oracle、profile有効期間を版管理する。

### Phase 1: software-only

- 数学、profile、codec、I/O、quality stateをunit/property/contract/fuzzで検証する。
- lens、残差、色、seam、blend、quality gateをpublic engine boundaryへ接続する。
- hardware operation 0を確認する。

### Phase 2: offline real-image

- 権利処理済み実D810 fixtureとholdoutでqualityを評価する。
- 正常が合格し、境界外・測定不能がfail closedになることを確認する。
- この段階をhardware acceptanceへ読み替えない。

### Phase 2.5: DualCamera identity gate

1. 正式資料からdocumentedな本体固有SDK propertyまたは明示的に安全なSDK/WPD相関protocol候補を作る。
2. software-only contractでmissing、duplicate、ambiguous、schema/version/expiry不一致をfail closedにし、raw identifierを保存・表示せず、旧identity mapのmigrationまたはinvalidationを明示する。
3. operatorまたはengineer-adminが別途明示承認したread-only identity testだけを実施する。接続順変更、port swap、再接続を含めるが、capture、Live View、card access、settings write、delete、format、vendor operation、automatic retryは0とする。
4. engineer-adminが正式根拠、匿名反復結果、禁止操作0をreviewし、成立しなければBlockを維持する。`HG-0003B`を閉じるまでproduction capture、product `Ready`、本アプリで取得したpairの製品E2E合成を許可しない。

### Phase 3: calibrated rig

- 承認rigでprofileを生成し、再設置・意図的ずれ・日間差を検証する。
- geometry、色、seam、crop、memory、timingを記録する。
- 独立reviewerが`Pass`または`Block`を判断する。
- Phase 2.5より前にできるのは、権利処理済み既存pairまたは本アプリ外の別途承認済み取得手順によるoffline評価だけである。この証拠は`HG-0003B`や製品captureの代用にしない。

### Phase 4: hardware end-to-end

- Phase 2.5の完了と、明示的な実機再開後だけ実施する。
- CAM-AからCAM-Bの順次撮影、canonical original、stitch、quality、restitch、exportを一transactionで確認する。
- disconnect、timeout、crash、response-unknown、disk、quality failureのfault matrixを実施する。
- 実pair characterization後、計画上の100/100を実施する。これはdeterministic release confirmationであり、別途承認した統計的失敗率の主張を単独では支えない。

### Phase 5: release

- `HG-0005`、package、clean PC、accessibility、operations、diagnostics、full regressionを完了する。
- L1からL4の証拠を相互に代用せず、release decisionへ添付する。

## 10. 最終完了条件

### 10.1 Product-ready（L4）

次をすべて満たすまで、DualCamera合成を製品利用可能と判定しない。

- `HG-0001`、`HG-0002`、`HG-0003B`が承認済み
- `FR-STI-001`から`FR-STI-006`が各々独立証拠を持つ
- approved profile以外でcapture/stitch side effect 0
- lens、fixed warp、bounded residual、exposure/color、seam/blend、crop、quality gateが接続済み
- good fixtureは全件合格し、damaged/over-limit/no-result fixtureは全件fail closed
- profile mutation 0、automatic retry 0、Single fallback 0
- CAM-A/B原本消失0、誤pair 0、既存result置換0
- `CaptureTransaction`、`StitchJob`、`ReviewRecord`、`ExportRecord`、`DiagnosticBundle`が別artifactとして永続化される
- metric contractに従うraw value、quality、補正、profile/hash、engine/oracle版、failure reasonがredacted recordとして記録される
- pre/post-encode qualityとfull decodeを通ったjobだけが、承認済みの単一commit pointに到達し、file存在だけを成功としない
- 実寸画像でp95とpeak memoryが承認値内
- 校正rigの反復、再設置、実WPF、fault matrixが合格
- 実pair 100/100、目的に対応した統計試験、独立product reviewがそれぞれ合格

### 10.2 Release-ready（L5）

次をすべて満たすまで配布可能なreleaseと判定しない。

- L4証拠が対象release build、profile、fixture、oracleに対してfreshである
- `HG-0005`が承認され、Nikon SDK、OpenCV等のnative dependency、license、installer境界が確定している
- clean Windows 11 PCでinstall、smoke、upgrade、uninstall、rollbackが合格する
- engine、profile schema、Agent、app、codec、Windows buildのcompatibility matrixが承認済みである
- accessibility、operator手順、diagnostic bundle、retention/cleanup、容量不足、support/rollback手順が検証済みである
- freshなserial full regression、package integrity確認、独立release review、権限者のrelease decisionが合格する

## 11. 共有レビュー「md内容の検討」の採否整理

### 11.1 本書へ採用した構造上の指摘

次は既存要求と矛盾せず、検証仕様を実行可能にするため、本書へ採用した。

- capture、stitch、review、export、diagnosticのartifact分離と、restitchの新規job化
- setup、capture、stitch、exportのstate machine分離
- pre-encode検査、partial encode、full decode、post-encode検査、manifest、commitの順序固定
- validity maskによるcoverage判定。画素値の黒だけでholeを判定しない
- metric ID/version、座標、mask、sampling、aggregation、境界、no-resultを持つmachine-readable contract
- calibration、development、locked holdout、release confirmationのcorpus分離
- 残差補正器、seam cost、acceptance oracleの独立性
- deterministic 100/100と統計的な失敗率評価の分離
- local Jacobianと実入力解像度に基づく局所有効DPI、平面度、depth、parallaxの評価
- profile canonicalization/hash、承認、compatibility、失効の技術契約
- requirement-to-evidence matrixとevidence freshness trigger
- `HG-0003B`を明示したPhase 2.5と、有識者package間のdecision ownership

### 11.2 未承認の数値・方式候補

リンク先で提示された次の値は、比較案を具体化するための`candidate`として有識者へ渡せる。ただし、計算書、実測、fixture、独立oracle、risk評価、権限者承認がないため、本書では採用値にしない。

| 分類 | リンク先のcandidate | 確認・承認が必要な点 |
| --- | --- | --- |
| 出力 | 180 DPI、landscape `8426x5960`、EXIF orientation 1 | A0物理寸法とのrounding、crop後局所DPI、既存要件、代表原稿の可読性 |
| camera/rig | D810 portrait二台、左右配置、同一60 mm級lens、f/5.6、ISO 64、manual focus、VR off、約1.81 m | 光学成立性、lens個体差、被写界深度、回折、固定方法、正式なcamera setting契約 |
| 幾何 | baseline `534 +/- 2 mm`、overlap `141 +/- 10 mm`、hard minimum `120 mm`、配置`+/- 0.5 mm` | 許容差の測定器、uncertainty、local DPI、全corner coverage、再設置性 |
| 原稿平面 | 全面flatness `<= 0.50 mm`、overlap corridor `<= 0.20 mm`、vacuum platen | 原稿保全、吸着可否、安全性、parallaxとの関係、測定方法 |
| 照明 | `5000 +/- 200 K`、CRI `>= 95`、R9 `>= 90`、`1500 +/- 300 lx`、min/max `>= 0.90`、flicker `<= 1%` | 測定位置、暖機、反射、紙種、環境光、計測器校正 |
| 幾何品質 | registration p95 `<= 0.5 px`、max `<= 1.0 px`、profile reprojection RMS `<= 0.25 px`、uncovered/content loss 0、edge inward `<= 0.3 mm` | metric/oracle、sampling、mask、worst-case位置、視覚品質との相関 |
| calibration/warp | Brown-Conrady 5 parameter、homographyはprofile生成時のみ、production residualはsimilarity限定、fiducial主・ECC cross-check、Lanczos3、一回inverse resampling | model adequacy、parameter observability、anti-ringing、no-result、性能、licensing |
| 色・seam | float32 linear-light sRGB、固定camera補正、bounded per-capture gain、Graph Cut seamと16 px cosine feather | ICC契約、色票実測、文字横断時の品質、determinism、fallback禁止条件 |
| JPEG | baseline JPEG、4:4:4、quality 0.95候補、quantization table hash、sRGB ICC、orientation 1、GPS/serial/MakerNote/thumbnail除去 | WICでの実現性、encoder差、再圧縮劣化、metadata privacy、byte/pixel決定性 |
| profile lifecycle | 30日または1000 pairで失効する候補 | drift data、再校正cost、期限到来時の運用、延長承認protocol |
| corpus | calibration 60、development good 120、locked holdout good 300、境界30/parameter、破損30/class、malformed 20/variant等 | 独立性、層化、power、欠測、権利、試験費用。sample数は目的ごとに再計算 |
| live評価 | 300/300、30種以上の原稿、3 operator、5日以上、5回以上の再設置候補 | `HG-0003B`、実機承認、安全な取得手順、失敗率claimとの整合 |
| performance | stitch-only p95 10秒、16 GB機12秒、capture-to-product p95 30秒または35秒候補 | reference machine、warm/cold定義、画素数、codec、quality stage、I/Oを固定 |
| memory | native `<= 2.5/3.5 GiB`、app commit `<= 4/5 GiB`等のtarget/hard候補 | measurement API、同時process、tile寸法、fragmentation、16/32 GB環境 |
| retention/fuzz | diagnostic 7日、10 GiB、partial 24時間、profile 30日、fuzz 24時間等の候補 | product policy、privacy、disk pressure、cleanup authority、試験停止条件 |

上表にない色差、MTF、seam、人手score、残差補正target/hard limitの一部は、リンク先表示から値・演算子・単位を完全には復元できない。推測で補わず、EXP-01からEXP-04へmachine-readable表の再提出を依頼する。

### 11.3 現行方針と衝突し、再決定なしには採用しない提案

| 提案または表現 | 現在の扱い | 必要な対応 |
| --- | --- | --- |
| `HG-0001/0002`を条件付き承認またはv0.1承認とみなす | 不採用。両gateはopen | 根拠付き有識者回答後、product ownerが明示承認する |
| `HG-0003B`解決前の本アプリDualCamera実撮影 | 禁止 | documented same-body binding証拠とengineer-admin承認が先 |
| `HG-0005`を条件付き承認とみなす | 不採用。release gateはopen | native dependencyと再配布境界をproduct ownerが承認する |
| JPEG入力上限を128 MiBへ変更 | 保留。現行codeは64 MiB | corpus実測、DoS/resource評価、compatibilityを伴う別decisionが必要 |
| physical power-cycle、cold boot、real power-off recoveryを必須化 | 不採用。2026-08-09のoperator decisionでN/A | 必要性と安全手順を示し、human decisionを明示的に再openする |
| controlled USB switch等によるhardware isolation | architecture変更候補 | USB-only MVP、操作権限、誤body防止、failure modeをarchitecture reviewする |
| JPEG/EXIF、接続順、index、表示名、drive letter、port単独でbody binding | 禁止 | 公式にdocumentedなbody固有propertyまたは安全なcross-transport相関が必要 |
| 「10秒」をcapture-to-product全体の承認値とする | 不採用 | stitch-onlyかend-to-endかを分け、reference machineとpercentileを承認する |
| 100/100だけで失敗率1%未満を証明する | 不採用 | deterministic確認と統計的主張を分け、EXP-04がsample数を設計する |
| リンク先の`approved`、`最終値`という表現 | repository上の承認ではない | human gate、decision record、fixture/evidence IDへ結び直す |

### 11.4 有識者へ最優先で返す質問

1. A0全面で必要な局所有効DPI、content loss、flatness、depth、parallaxを、どの座標・mask・測定器・uncertaintyで判定するか。
2. 固定profile生成と撮影ごとのbounded residualを分離したとき、許可model、target、hard limit、confidence、`NoResult`をどう定義するか。
3. lens correction、warp、color、seam、blend、cropを一回のresamplingとfail-closed quality gateへどう構成するか。
4. registration、色差、sharpness、seam、coverageの各metricが、実装のfeature/costから独立して欠陥を検出できるか。
5. pre/post-encode検査、manifest、`COMMITTED` marker、crash recoveryを含むartifact protocolがWindows filesystem上で成立するか。
6. candidate数値ごとに、根拠、代替案、感度、sample size、false pass/reject、再検証triggerを提示できるか。
7. 二台のSDK/WPD same-body bindingに、NikonまたはWindowsの正式資料で裏付けられた方法があるか。なければBlockを維持すべきか。

## 12. 主要参照

- `docs/PRODUCT_REQUIREMENTS.md`
- `docs/ARCHITECTURE.md`
- `docs/FEATURE_VERIFICATION_PLAN.md`
- `docs/ROADMAP.md`
- `docs/schemas/rig-profile.schema.json`
- `.autodev/requirements/normalized.json`
- `.autodev/requirements/unresolved_questions.json`
- `.autodev/human-gates/open.json`
- `.autodev/bootstrap/report.json`
- `.autodev/plan.json`
- `src/m2/include/a0/m2/offline_stitcher.hpp`
- `src/m2/offline_stitcher.cpp`
- `src/m2/synthetic_measurement.cpp`
- `src/m2/setup_assessment.cpp`
- `src/m3/Foundation/DualCamera/DualCameraProductFlow.cs`
- `src/m3/Foundation/DualCamera/M2OfflineStitcherProcessAdapter.cs`
- `tests/offline_stitcher_tests.cpp`
- `tests/synthetic_measurement_tests.cpp`
- `tests/setup_assessment_tests.cpp`
- `tests/m3/DualCameraFlowTests/Program.cs`
