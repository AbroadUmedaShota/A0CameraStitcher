# 歪みなしの観測からの内部パラメータとposeの閉形式推定（#289、#277段階2）

設計案の[入出力設計案](M2_DRAFT_CALIBRATION_IO_PLAN.md)の第6節2番目を実装した。入力は[観測session](M2_CALIBRATION_OBSERVATION_SESSION.md)、途中で[線形制約の検査](M2_CALIBRATION_VIEW_GEOMETRY.md)（#283）を使う。profile・draft・report・fileは作らない。既存のAPI、schema、profile契約も変更しない。

`a0_m2_calibration_intrinsics` は、カメラ1台ごとに傾いた複数のtraining viewから内部パラメータKを閉形式で求め、viewごとのposeと再投影誤差を返す純粋なC++20 library。依存はSTL、#283、#286のlibraryだけで、Win32・OpenCV・生成器・profile readerは使わない。

## 結果の読み方

成功の status は `undistorted-intrinsics-estimated`。これは **解析的観測・歪みなしの前提で、閉形式の推定が最後まで計算できた** という意味で、校正の成功ではない。

- レンズ歪みを扱わない。sessionの `PixelPosition` は歪み込みと注記されているが、この段階では検出済みpixelを歪みなしの保存順pixelとして読む（`ObservationModel::UndistortedAnalytic`）。実写の成功には数えない
- 非線形の微調整・重み付け・外れ値除去をしない
- 残差に閾値を持たない。大きくても小さくても生の値を返し、品質の合否は判定しない
- profileや文書面写像は作らない。内部パラメータの来歴の判断も未決のまま（設計案の第4節）

## 入力

```
UndistortedIntrinsicsResult EstimateUndistortedIntrinsics(
    const CalibrationObservationSession& session, const UndistortedIntrinsicsPolicy& policy);
```

引数はsessionとpolicyだけ。真値・reference view・既定のpolicyは経路に入らない。

| policyの欄 | 内容 |
| --- | --- |
| minimum_views | 最低視点数。2〜128。#283へそのまま渡す |
| relative_singular_cutoff | 特異値の相対判定値。有限で厳密に0より大きく1より小さい。#283へそのまま渡す |
| observation_model | `UndistortedAnalytic` のみ。範囲外の値は不正 |

すべて必須で既定値はなく、sessionにも入れない。不正なpolicyは `std::invalid_argument("undistorted intrinsics: invalid policy")`（メッセージに値を含めない）。ほかに投げるのは `std::bad_alloc` だけ。sessionはParseを通った値なので、入力が原因の例外は構成上ない。投げた場合は欠陥として扱う。

使うviewはcameraごとにroleがtrainingとvalidationのもの。referenceは読まない。CAM-AとCAM-Bは互いに影響せず、結果は常に2要素（`[0]`がCAM-A、`[1]`がCAM-B）。viewを持たないcameraも省略せず、`insufficient-views` で返す。

## 手順

cameraごとに次の順で進む。前の段階が止まると、それ以降は計算しない。

1. **homography**。training viewごとに、チャートmmからpixelへのHを推定する。使う点はdetectedだけで、trainingは `usedPointIds`、validationは `posePointIds`
2. **view-geometry**。training viewのHを `AssessViewGeometry` に渡す。status・rank・特異値は結果にそのまま載せる
3. **intrinsics**。rankが5のときだけ、Zhangの閉形式でK（skew=0）を出す
4. **pose**。training viewごとに回転と並進を求め、使った点の再投影誤差を測る
5. **held-out**。validation viewごとに `posePointIds` でposeを求め、`measurementPointIds` の誤差を測る。ここまでの成否を変えない

### 1. homography

事前に `DiagnoseObservationSession` を1回呼び、そのviewが `insufficient-observations`（点が4未満）または `degenerate-point-configuration`（どの3点も共線にならない4点が無い）なら、同じ名前をoutcomeにして計算しない。この判定は厳密述語で、許容値を使わない。判定の対象はチャート座標だけで、pixel側の縮退（detectedのpixelがほぼ共線など）はこの段階では止めない。そうした入力から求めたほぼ特異なHは、行列式が丸め誤差の分だけ非零になれば次へ進み、後段の判定か残差に現れる。後段で必ず止まるとは限らない（カメラ中心がチャート面上にある真横のviewでは、Zhangの制約が整合して#283を通りうる。※試験なし）。全pixelが1点に重なる場合は、次のHartley正規化のscaleが有限にならず `numerical-failure` になる（試験NF2）。

DLTはHartley正規化つき。チャートmmとpixelのそれぞれで重心を引き、重心からの距離の平均が√2になるよう等方scaleをかける。点ごとに2行 `[−x,−y,−1,0,0,0,ux,uy,u]` と `[0,0,0,−x,−y,−1,vx,vy,v]` ができる。

2N×9の行列は、Givens回転で1行ずつ9×9の上三角に縮約する。この行列のまま特異値分解し、`AᵀA` を作らないので条件数を二乗しない。分解は#283と同じone-sided Jacobi（内部header `src/m2/calibration_jacobi.hpp` に移した）で、右特異ベクトルの最小のものがHの正規化座標での値になる。正規化のscaleが有限にならない場合、収束しない場合、または8番目の特異値が最大の `16ε` 倍以下なら、そのviewは `numerical-failure`。9番目は零方向なので見ない。

Hは正規化を戻してフロベニウスノルム1にし、使った点の重心でw > 0になるよう符号を決める。wが0、非有限、または行列式が厳密に0のHは `numerical-failure`。1つでも失敗したviewがあれば、そのcameraは `homography-unavailable` で止める。失敗したviewを黙って外さない。

### 2. view-geometry

#283の結果をそのまま `view_geometry` に載せる。`AssessViewGeometry` が `invalid_argument` を投げる条件（ID、寸法、view数、option、H）は、上の段階とsession側の検証で先に除かれている。投げた場合は欠陥として扱い、握りつぶさない。

#283側には、末尾に `conditioned_conic`（`std::optional<ConditionedConic>`）を足した。既存の5欄の値は変えていない。

- 中身は6本の制約行の最小特異値に対応する右特異ベクトル `b=[B11,B12,B22,B13,B23,B33]`（2ノルム1、全体の符号は不定）と、条件付けに使った画像の中心 `((w−1)/2,(h−1)/2)` と scale `max(w,h)/2`
- 値が入るのは `linear-constraints-sufficient` のときだけ。rankが5なら、最小の特異値だけが判定値以下なので、この列は一意に決まる
- 判定に使った分解の結果をそのまま返すので、判定と解が食い違わない

### 3. 内部パラメータ

条件付けの座標は `x' = T x`、`T = [[1/S,0,−c_x/S],[0,1/S,−c_y/S],[0,0,1]]`。`K' = T K` なので、`K = T⁻¹ K'` で戻す。`b` の符号を `B11` が正になるよう揃えた後、

```
μ  = B33 − B13²/B11 − B23²/B22
f'x = √(μ/B11),  f'y = √(μ/B22),  u'0 = −B13/B11,  v'0 = −B23/B22
fx = S·f'x,  fy = S·f'y,  cx = S·u'0 + c_x,  cy = S·v'0 + c_y
```

`B12` は使わない（skew=0のmodelで、#283の制約でほぼ0に拘束済み）。次のどれかなら `nonphysical-intrinsics` とし、値を返さない。clampはしない。

- `B11` が0または非有限
- `B22 ≤ 0` または `μ ≤ 0`（`B12=0` としたBが正定値でないことと同じ）
- 4つの値のどれかが非有限、または `fx ≤ 0`、`fy ≤ 0`

この場合も `view_geometry` と生の `conditioned_conic` は残る。

### 4. pose

`H` と `K` から `m_k = K⁻¹ h_k`（H の第k列）を作り、`λ = 2/(‖m1‖+‖m2‖)`、`r1=λm1`、`r2=λm2`、`r3=r1×r2`、`t=λm3`。Hの符号を `w>0` に揃えてあるので、`λ>0` が前の向き。`Q=[r1 r2 r3]` に3×3の同じJacobi分解を当て、特異値が `Q = UΣVᵀ` のとき `R = UVᵀ`（最も近い回転）とする。分解が収束しない、または最小の特異値が最大の `16ε` 倍以下なら、そのviewは `numerical-failure`。

そのviewで使った点（training は usedPointIds、validation は posePointIds と measurementPointIds）の深度 `R₆X + R₇Y + t_z` が正であることを確かめる。0以下の点が1つでもあれば `chart-not-in-front`。1つでも失敗したviewがあれば、cameraは `pose-unavailable`。成功したviewの `pose` と `residual` は残し、cameraの `training_residual` は出さない。

座標の規約は、`カメラ座標 = rotation × [x_mm, y_mm, 0]ᵀ + translation_mm`（行優先、チャート面のmm）。

### 残差

`p = K(R[X,Y,0]ᵀ + t)` を投影し、`e_i = hypot(u_p − u, v_p − v)`。`rms = √(Σe_i² / N)`、`max = max e_i`、`N = point_count`。Nは1以上で、N=0は0を入れた値ではなくnull。和はlistの順、training全体はviewをsessionの順に足す。

- training: view ごとは使った点（`usedPointIds`）、全体は全training viewの合算
- validation: `measurementPointIds` だけ。poseの推定に使った `posePointIds` は含めない。poseを求める点と誤差を測る点が分かれているので、この値は推定に使っていないpixelでの誤差になる。全体は評価できたviewの合算で、1つも無ければnull

### 5. held-out

training側が最後まで成功したときだけ行う。validation viewごとに、1〜4と同じ手順でHとposeを求める。深度は `posePointIds` と `measurementPointIds` の両方で確かめる。validationの失敗はcameraの status を変えない。

| coverage | 意味 |
| --- | --- |
| not-reached | training側が止まった |
| no-validation-view | validation viewが無い。residualはnull |
| all-validation-views-evaluated | すべてevaluated |
| some-validation-views-not-evaluated | 1つ以上が評価できなかった。評価できた分だけで集計し、0件ならnull |

## statusと欄のnull規則

`FailedStage(status)` で止まった段階（`homography`・`view-geometry`・`intrinsics`・`pose`）を引ける。statusと段階は1対1なので欄は持たない。成功では null。

| status | 止まった段階 | view_geometry | intrinsics | trainingのpose | training_residual | held-out |
| --- | --- | --- | --- | --- | --- | --- |
| homography-unavailable | homography | null | null | null | null | not-reached / null |
| insufficient-views | view-geometry | あり | null | null | null | not-reached / null |
| insufficient-view-diversity | view-geometry | あり | null | null | null | not-reached / null |
| inconsistent-linear-constraints | view-geometry | あり | null | null | null | not-reached / null |
| numerical-failure | view-geometry | あり（特異値とrankはnull） | null | null | null | not-reached / null |
| nonphysical-intrinsics | intrinsics | あり（生の `conditioned_conic` を含む） | null | null | null | not-reached / null |
| pose-unavailable | pose | あり | あり | 成功したviewだけ | null | not-reached / null |
| undistorted-intrinsics-estimated | なし | あり | あり | 全view | あり | coverageに従う |

viewごとの `outcome` は、最初に失敗した手順の理由（`insufficient-observations`・`degenerate-point-configuration`・`numerical-failure`・`chart-not-in-front`）。すべて済めば `evaluated`、cameraが先に止まって計算していなければ `not-reached`。Hを出せたviewは、後の段階でcameraが止まっても `chart_to_image` を残す。たとえば正面だけのviewでは、Hはあり、poseはnull、outcomeは `not-reached`。

次の対応が常に成り立つ。

- 成功statusであることと、内部パラメータ・全trainingのpose・`training_residual` がそろっていることは同値
- `conditioned_conic` があるのは #283 が `linear-constraints-sufficient` のときだけ
- `AmbiguousCorrespondence` は判定に使わない。Hは決まり、誤対応は残差に現れる。診断は段階5のreportが `DiagnoseObservationSession` から別に載せる

## 決定性と真値の分離

- thread・乱数・時刻・fileを使わない。同じsessionとpolicyは、何回呼んでもビット単位で同じ結果になる
- Kを決めるのは、training viewの使った点のpixelとチャート座標、画像寸法、policyだけ。validation・reference・使わなかった点のpixelを変えても、Kと各training viewの結果はビット単位で変わらない
- trainingの使った点を1つ動かすと、そのviewのHと#283の特異値が変わる
- 1つのcameraに何が入っていても、もう一方のcameraの結果は変わらない

## 範囲外と段階3以降への申し送り

今回含めないもの（interfaceも置いていない）: レンズ歪み、非線形の微調整（LM）、重み付け、外れ値除去、`UndistortedAnalytic` 以外のmodel、残差の閾値、JPEGの検出、OpenCV。

段階3で扱う前提として、次を申し送る。

- **cutoffは観測誤差に合わせる呼出側のpolicy**。合成値でも、training の1点を0.25 px動かすと最小特異値が最大の約 `1e-6` 倍になる（試験 S-b が値を info 行に出す）。cutoffが `1e-8` だとrank 6（不整合）、`1e-4` だとrank 5になる。試験は `1e-4` を使う。ノイズのある実写では、不整合が多発しうる
- **sessionのpixelは歪み込みと注記されている**。段階3でレンズ歪みを推定するときは、歪みを取り除いた値をこの段階に渡す形にする。この段階の入力を実写の歪み込みpixelにしても、結果はレンズ歪みの影響を含む
- **B12を使わない**。ノイズがあると、Zhangの一般式より主点の縦位置がわずかに違う。skew=0のmodelの選択で、精度は非線形化で扱う
- **trainingのposeは固定リグの写像ではない**。段階4のreference pairの扱いで、trainingのposeを文書面のHにコピーしない
- 誤対応（ambiguous）の疑いは、残差とdiagnosticsを並べて段階5のreportで見る

## 試験

`tests/calibration_intrinsics_tests.cpp`（CTest `m2_calibration_intrinsics_contracts`）。K・R・tから独自に投影してsessionを組み、推定器にはpixelだけを渡す。本番のJacobi・DLT・Zhangの行・pose計算は試験に使わない。画像は1280×960、チャートは13×9格子（ピッチ20 mm）。許容値は試験の入力であり、製品の閾値ではない。

| ID | 内容 |
| --- | --- |
| A0 | 不正なpolicy（最低視点数 0・1・129、cutoff 0・1・負・NaN・無限大、範囲外のmodel）が `invalid_argument`。範囲の端は受理 |
| G1・G2 | 正面のみ、同じ傾きの繰返し。`insufficient-view-diversity`（rank 2、3）。Kとposeと残差はnull、Hは残る |
| G3 | 異なる傾き。rank 5、`conditioned_conic` あり。sessionに無い格子96点と面外18点（Z=±40 mm）を、推定と真値で投影した差が最大 1e-6 px 以下。validationのmeasurement 58点のrms・maxも 1e-6 px 以下。係数の差は情報として出すだけで、合否には使わない |
| G4 | 焦点距離の違うKを混ぜる。`inconsistent-linear-constraints`、rank 6、Kはnull |
| V1a・V1b・V2 | 視点不足、最低視点数4、validation無し |
| V3・V4 | 使う点が3点／同一行の5点で `homography-unavailable`。validationの点が3つなら、cameraは成功のまま `some-validation-views-not-evaluated` |
| N1 | 縦を0.5倍した観測。rank 5だが `nonphysical-intrinsics` |
| P1 | チャートがカメラの前後にまたがるview。`pose-unavailable`、そのviewは `chart-not-in-front` |
| C1・C2 | CAM-AとCAM-Bの独立。一方に何を入れても他方がビット単位で変わらない |
| S-a・S-b・S-b'・S-c | 引数が2つだけ、policyの欄が3つだけ。使った点を動かすとHと特異値が変わる。使わない点・validation・referenceを動かしても、Kとtrainingがビット単位で変わらない |
| D1 | 同じJSONを2回Parseし、2回ずつ推定して4回とも同じ |
| L1 | Releaseのみ。1 viewあたり16,384点（128×128）の3 view |
| R-probe | チャートの範囲外の検査点24点でも、推定と真値の投影の差が 1e-6 px 以下 |
| NF1・NF2 | training 2 viewの構造上の零列とcutoff 1e-17で#283が `numerical-failure`。全pixelが1点に重なるviewはHartley正規化で `numerical-failure` |
| C3 | 7種類の失敗を、CAM-A側とCAM-B側の両方向に入れても、もう一方のcameraの結果がビット単位で変わらない |
| VF | validation viewだけの失敗（measurement点がカメラの後ろ、pose点の縮退）でcameraの成否が変わらず、held-outの残差は成功したviewだけから出る |
| SG | チャート全体がカメラの後ろにある配置で、深度が正になる向きのposeを選ぶ |
| TS | validationのpose点を動かすとそのviewのHが変わり、measurement点を動かしてもHは変わらず残差だけが変わる |
| RS | 残差のrms・max・Nを試験側で計算し直して突き合わせる |

不変条件の検査（statusと欄のnull規則、成功の同値、reference非掲載など）は、全部の結果に対して実行する。

CMakeの実configure試験（`m2_calibration_intrinsics_link_isolation`）は23件。pure libraryの正の対照2件と、`synthetic_pair`・`rig_profile_v2`・Win32系（bcrypt・ole32・windowscodecs・shell32）・OpenCVの直接・推移の拒否、importedのOpenCV、禁止sourceの推移混入の拒否を確認する。

## 置いた仮定

- 検出済みpixelを歪みなしとみなす（上記）
- `B12` は使わない
- `AmbiguousCorrespondence` は判定に使わない
- `λ` は第1・第2列の長さの平均の逆数（2列の大きさが完全には揃わない入力に対する選択）
- `pose-unavailable` でも、成功したtraining viewの `pose` と `residual` は残す
