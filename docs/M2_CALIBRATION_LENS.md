# 歪みを含む解析的観測からのレンズ歪みと内部パラメータの推定（#290、#277段階3a）

設計案の[入出力設計案](M2_DRAFT_CALIBRATION_IO_PLAN.md)の第6節3番のうち、OpenCVを使わない前半（3a）を実装した。入力は[観測session](M2_CALIBRATION_OBSERVATION_SESSION.md)、初期値は[歪みなしの閉形式推定](M2_CALIBRATION_INTRINSICS.md)（#289）。profile・draft・report・fileは作らない。既存のAPI、schema、profile契約も変更しない。

`a0_m2_calibration_lens` は、カメラ1台ごとに、傾いた複数のtraining viewから内部パラメータK（skew=0）とBrown–Conradyの5係数（k1・k2・k3・p1・p2）、viewごとのposeをLevenberg–Marquardt（LM）で求め、未使用の点で検算する純粋なC++20 library。依存はSTLと#289（とその先の#283・#286）のlibraryだけで、Win32・OpenCV・生成器・profile readerは使わない。

## 結果の読み方

成功の status は `lens-intrinsics-estimated`。これは **歪みを含む解析的観測で、推定が最後まで計算できた** という意味で、校正の成功ではない。

- pixelはmodelから計算した値で、実写の検出結果ではない。JPEGからの検出と実写の評価は段階3bに残る
- 残差に閾値を持たない。大きくても小さくても生の値を返し、品質の合否は判定しない
- 重み付け・外れ値の除去・ロバスト損失・係数の固定マスクを使わない
- profileや文書面写像は作らない。このIssueを閉じても#277は閉じない
- 係数はノイズがあると定まりにくい。ノイズ（σ=0.25 px）ありの試験でも、比べているのは係数ではなく、未使用点での写像の差（試験N1）

`DistortedAnalytic` を選んだことは、検出済みpixelを歪み込みの保存順pixelとして読むという宣言で、結果にそのまま写される（`observation_model`）。

## 入力

```
LensIntrinsicsResult EstimateLensIntrinsics(
    const CalibrationObservationSession& session, const LensIntrinsicsPolicy& policy);
```

引数はsessionとpolicyだけ。真値・生成器・reference view・既定のpolicyは経路に入らない。

| policyの欄 | 内容 |
| --- | --- |
| minimum_views | 最低視点数。2〜128。段階2（#289）へそのまま渡す |
| relative_singular_cutoff | 特異値の相対判定値。有限で厳密に0より大きく1より小さい。段階2へそのまま渡す |
| observation_model | `DistortedAnalytic` のみ。`UndistortedAnalytic` を含む、ほかの値は不正 |
| max_iterations | 試行の上限。1〜1000。採否とCholesky分解の失敗を問わず、試行1回を1と数える |
| relative_cost_tolerance | 相対コストの停止判定値。有限で厳密に0より大きく1より小さい |
| relative_step_tolerance | 相対ステップの停止判定値。有限で厳密に0より大きく1より小さい |
| initial_damping | LMの初期damping。有限で0より大きい |

すべて必須で既定値はなく、sessionにも入れない。不正なpolicyは `std::invalid_argument("lens intrinsics: invalid policy")`（メッセージに値を含めない）。ほかに投げるのは `std::bad_alloc` だけ。

`max_iterations` の上限1000は、既定値ではなく受け付ける範囲。未知数が最大の777（128 view）のとき、1回の試行は密なCholesky分解が支配的で、Releaseの試験では全体で数秒だった（試験L1、8試行）ため、1000回でも現実的な時間に収まると見ている（※1000回の実測なし）。

使うviewは段階2と同じ（cameraごとにroleがtrainingとvalidationのもの。referenceは読まない）。CAM-AとCAM-Bは互いに影響せず、結果は常に2要素（`[0]` がCAM-A、`[1]` がCAM-B）。

### cutoffは観測誤差に合わせるpolicy

段階2の `relative_singular_cutoff` を、歪みなしの試験と同じ値（`1e-4`）にすると、歪み込みのpixelでは使えない。歪み込みのpixelから求めたHは、Zhangの制約を満たさない。試験E1の配置（6つのtraining view）で、制約行の6番目の相対特異値は `6.8e-4` まで上がる（歪み0なら丸め誤差の水準）。cutoffが `1e-4` だとrank 6（不整合）になり、段階2が止まって `initialization-unavailable` になる。5番目の相対特異値は `5.75e-2` で、cutoff `1e-2` は上下とも5倍以上離れる（試験A2が値を info 行に出して確かめる。2026-10-13、Release）。試験は `1e-2` を使う。

cutoffは観測誤差に合わせて呼出側が決める値であり、このlibraryは「正しい値」を持たない。大きくすると視点の多様性が足りないことを見逃しやすくなる。実写の歪み込みpixelでの扱いは3bの課題。

## 手順

cameraごとに次の順で進む。前の段階が止まると、それ以降は計算しない。

1. **initialization**。段階2（`EstimateUndistortedIntrinsics`）をそのまま呼び、結果を `initialization` に載せる。`undistorted-intrinsics-estimated` 以外なら `initialization-unavailable` で止める
2. **solver**。段階2のKと各training viewのpose、歪み0を初期値にして、LMでK・5係数・全training viewのposeを同時に最小化する
3. **intrinsics**。収束したときだけ、Kが物理的か検査する
4. **distortion**。収束したときだけ、歪みの物理性を検査する（順方向のみ）
5. **pose**。training viewのposeと残差を確定する
6. **held-out**。validation viewごとにposeを求め、`measurementPointIds` の誤差を測る。ここまでの成否を変えない

### 歪みの式

```
P  = R·[X, Y, 0]ᵀ + t                      # カメラ座標。Rは行優先、チャートmm
x  = P0/P2,  y = P1/P2                      # 歪みのない正規化座標（P2 > 0 のときだけ定義）
r2 = x² + y²
L  = 1 + r2 (k1 + r2 (k2 + r2 k3))          # 放射成分。Hornerの形
xd = x L + 2 p1 x y + p2 (r2 + 2 x²)
yd = y L + p1 (r2 + 2 y²) + 2 p2 x y
u  = fx xd + cx,  v = fy yd + cy            # 生の保存順pixel。整数が画素中心
```

適用の順は、pose → 透視除算 → 歪み → K。[rig-profile-v2の設計](design/rig-profile-v2.md)の写像と同じ向き（歪みのない → 歪んだ）で、逆歪みは計算しない。接線の2係数はOpenCVの配列順ではなく、名前のついた欄 `p1`・`p2` だけで扱う。

式は[生成器](SYNTHETIC_PAIR_GENERATOR.md)の `DistortNormalized` と演算の順まで同じに書く。libraryは生成器に依存できないので、別の実行ファイル `m2_calibration_lens_formula`（試験F1）が両者を同じ入力に当ててビット単位の一致を確かめる。

### LM

未知数は `θ = [fx, fy, cx, cy, k1, k2, k3, p1, p2, (ωx, ωy, ωz, tx, ty, tz)_v …]`。個数は `n = 9 + 6V`（Vはtraining viewの数。最大128で777）。ωは回転の増分で、受理するたびに `R ← Exp(ω)·R`、`t ← t + δt` とし、ωは毎回0から始める。`Exp(ω)` は `A = sin θ/θ`、`B = 2 (sin(θ/2)/θ)²` を使い、`R = I + A[ω]ₓ + B[ω]ₓ²`（小さい角に閾値を置かない）。

- 残差は点ごとに `(u − u_obs, v − v_obs)` の2成分（raw pixel）。コストは `C = Σ (Δu² + Δv²)`（px²、1/2は付けない）。Jacobianは解析式で、列ごとの式は内部header `src/m2/calibration_lens_model.hpp` に置く（試験J1が中心差分と照合する）
- 点の順は、training viewをsessionの順、その中は `usedPointIds` の順。足す順は固定
- 正規方程式 `A = JᵀJ`、`g = Jᵀr` は密な行列（行優先の1次元配列）で組む
- Marquardtの対角スケーリング: `S_j = √A_jj`、`Â = S⁻¹AS⁻¹`（対角は1）、`ĝ = S⁻¹g`。`(Â + λI) ŝ = −ĝ` を密なCholesky分解で解き、`δ = S⁻¹ŝ`
- dampingは固定の規則で動かす。受理で `λ ← max(λ/10, DBL_MIN)`、棄却で `λ ← 10λ`（Marquardt 1963）。この10倍・1/10倍と `DBL_MIN` は調整用の閾値ではなく規則なので、policyに入れない
- 候補が深度0以下の点を含む、または非有限なら棄却（コストの計算で弾く）
- 停止理由は4種
  - `relative-step-size`: `‖δ‖₂ ≤ tol_step (‖θ‖₂ + tol_step)`。δは適用しない。`‖θ‖₂` はfx・fy・cx・cy・5係数・全viewのtの2ノルム（回転は増分なので入れない。単位が混ざるが、Ceresのparameter_toleranceと同じ扱い）
  - `relative-cost-reduction`: 受理した後に `C − C' ≤ tol_cost · C`（Cは受理前）
  - `maximum-iterations`: 上限まで使った
  - `numerical-failure`: `S_j` が0か非有限、`λ` が非有限、組み直した `A`・`g` が非有限
- 収束（最初の2つ）のときだけ次の段階へ進む。`maximum-iterations` は `solver-nonconvergence`、`numerical-failure` は `numerical-failure` になり、K・係数・poseは出さない（最後の反復値を出さない）
- Cholesky分解のピボットが0以下か非有限のときは、`λ ← 10λ` として次の試行へ進む（この試行も1回と数える）

`SolverRecord` は `iterations`、`accepted_steps`、`initial_cost`（段階2の初期値でのC）、`final_cost`（最後に受理したθでのC。受理が1度も無ければ初期値のC）、`stop_reason`。コストはどれも有限。`training_residual` のrmsとNから `rms² × N = final_cost` が成り立つ（hypotの丸めの分だけ違う）。

**偽の収束に注意**。`initial_damping` を大きく与えるとステップが小さくなり、相対ステップの条件を満たして初期値のまま「収束」する。`accepted_steps` が0で `final_cost == initial_cost` なら、その状態。試験DMが `initial_damping = 1e12` で固定している。結果の `training_residual` を見れば判別できる。

### 物理性の検査

毎反復では検査せず、**収束した後に1回だけ**行う。違反する候補を毎回棄却すると、LMは境界で止まって相対ステップの条件を満たし、最小点ではない境界の値を「収束」として返してしまう。最もよく合う解そのものが物理的でないとき（試験PEの真値がこれ）は、`invalid-distortion` と正直に返す。深度が正であることは、投影が定義できる範囲の条件なので、別扱いで毎回確かめる。

検査の対象は、training viewの使った点を最終のθで評価した値。

- **内部パラメータ**（`nonphysical-intrinsics`）: fx・fy・cx・cyがすべて有限で、`fx > 0`、`fy > 0`。clampしない
- **半径方向の単調性**: `s_max = max r2` として、`g(s) = 1 + 3k1 s + 5k2 s² + 7k3 s³` の `[0, s_max]` での最小値が厳密に正であること。最小値は閉形式で求める。候補は `g(0)=1`、`g(s_max)`、`g′(s) = 3k1 + 10k2 s + 21k3 s²` の根のうち開区間 `(0, s_max)` にあるもの。`k3 ≠ 0` なら二次方程式の安定な解の公式（`q = −½(b + sign(b)√D)` の根 `q/a`・`c/q`）、`k3 = 0` かつ `k2 ≠ 0` なら一次方程式の根、どちらも0なら根なし
- **歪み写像のJacobianの行列式**: 使った各点で `a·c − b²`（`a`・`b`・`c` は上の式の `∂xd/∂x`・`∂xd/∂y`・`∂yd/∂y`）が厳密に正であること。`p1 = p2 = 0` なら `L·g(r2)` に一致する
- 2つの値のどちらかが厳密に正でなければ `invalid-distortion`（0と−0と負とNaNはすべて不正）。値は `DistortionCheck{max_radius_squared, minimum_radial_slope, minimum_jacobian_determinant}` として結果に載せる
- 検査の入力が有限でなければ `numerical-failure`

逆歪みは使わない。validationの点がtrainingの `s_max` より外にあると、歪みを外挿した値になる。それは検査せず、残差に現れる。

### held-out

`lens-intrinsics-estimated` になったcameraだけで行う。validation viewごとに次の順。

1. 段階2の同じviewの結果が `evaluated` でない、またはposeが無い → `initialization-unavailable`（理由は `initialization` の段階2のview結果で読める）
2. 初期値は段階2が求めたそのviewのpose。Kと5係数は最終値に固定し、`posePointIds` だけで、poseの6つをLMで動かす。policyのtrainingと同じ欄を使う。記録はviewの `solver` に入る。上限に達すれば `solver-nonconvergence`、`numerical-failure` なら `numerical-failure`
3. `measurementPointIds` のどれかの深度が0以下 → `chart-not-in-front`
4. `measurementPointIds` の残差だけを測る（poseの推定に使った点は含めない）。`evaluated`

coverageと集計の規則は段階2と同じ（`not-reached`・`no-validation-view`・`all-validation-views-evaluated`・`some-validation-views-not-evaluated`）。validationの失敗はcameraのstatusを変えない。

## statusと欄のnull規則

`FailedStage(status)` で止まった段階（`initialization`・`solver`・`intrinsics`・`distortion`・`pose`）を引ける。成功ではnull。止まった段階の値と、それより後の値はnullにする。例外は `solver` の記録で、solver段階で止まっても残す。

| status | 止まった段階 | solver | intrinsics | distortion | distortion_check | trainingのpose・residual | training_residual | held-out |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| initialization-unavailable | initialization | null | null | null | null | null（not-reached） | null | not-reached |
| solver-nonconvergence | solver | あり | null | null | null | null | null | not-reached |
| numerical-failure | solver | あり | null | null | null | null | null | not-reached |
| nonphysical-intrinsics | intrinsics | あり | null | null | null | null | null | not-reached |
| invalid-distortion | distortion | あり | あり | null | あり | null | null | not-reached |
| pose-unavailable | pose | あり | あり | あり | あり | 成功したviewだけ | null | not-reached |
| lens-intrinsics-estimated | なし | あり | あり | あり | あり | 全view | あり | coverageに従う |

`initialization` はどのstatusでも段階2のcameraの結果をそのまま持つ。viewの `outcome` は、training viewは成功したとき `evaluated`、それ以外は `not-reached`。validation viewは上のheld-outの結果。結果に載るviewは段階2と同じ並び（sessionの順、referenceなし）。

**到達しないstatusについて**。`pose-unavailable` はenumとコード文字列に残すが、3aでは到達する経路が無い。LMは深度0以下の候補を棄却するので、最終のposeで深度が0以下になることはない。防御のための判定で、到達を試験で強制していない。`numerical-failure` のうち物理性の入力が非有限になる経路と、`nonphysical-intrinsics` も、end-to-endでは試験していない。後者は判定関数 `PhysicalIntrinsics` の単体試験（試験PH）で境界を確かめる。solverの `numerical-failure`（`S_j` が0・damping非有限）をend-to-endで起こす入力も作れていない（※未試験）。

## 決定性と真値の分離

- thread・乱数・時刻・fileを使わない。同じsessionとpolicyは、何回呼んでもビット単位で同じ結果になる（試験D1）。`sin`・`cos`・`sqrt` は同じ実行ファイルなら同じ値を返す
- K・係数・training viewのposeを決めるのは、training viewの使った点のpixelとチャート座標、画像寸法、policyだけ。validation・reference・使わなかった点を動かしても、これらはビット単位で変わらない（試験S-b、TS）
- 1つのcameraに何が入っていても、もう一方のcameraの結果は変わらない（試験C1）

## 規模

未知数777（128 view）、点数はsessionの上限65,536まで。メモリは正規方程式の行列と分解用の行列で約10 MB（`777² × 8 B` が2つ）、点のデータが2 MB程度。1回の試行は、密なCholesky分解（`n³/6 ≈ 7.8e7` 回の積和）と、`A` の組み立て（1点あたり約450回の積和）が主。Debugのbuildは非常に遅いので、大きな試験（L1）はReleaseだけで流す。

## 範囲外と段階3b以降への申し送り

今回含めないもの: JPEGからの検出、生成画像を使ったend-to-end、OpenCVと再配布の判断（HG-0005）、reference pairと文書写像（段階4）、writerとprovenance（段階5）、重み付け・外れ値の除去・ロバスト損失・係数の固定マスク・複数seedの統計。

3bへ申し送る。

- **cutoffと視点の多様性**。上の通り、歪み込みのpixelでは段階2のrankが上がる。実写で5番目と6番目の特異値がどこに来るかを測り、cutoffの決め方（観測誤差からの導出）を決める
- **局所解と初期値**。初期値は歪み0の閉形式。強い歪みではこの初期値が悪く、段階2が `nonphysical-intrinsics` や `inconsistent-linear-constraints` で止まる（Node.jsでの検算。接線歪みp2を0.35〜0.5にして、チャートを画像の片側に寄せた配置。C++の試験にはしていない）。複数の初期値や段階的な係数の解放は今回やらない
- **係数の同定**。k3・p1・p2はノイズに敏感で、係数そのものの誤差が大きくても写像の差は小さい。係数の差だけを合否にしない（設計案の第2節）
- **gather（点の取り出し）の重複**。`usedPointIds` の読み取り（約15行）は段階2のcppと同じ規則を複製した。段階2のcppを動かさないための選択で、段階4で共有のheaderにまとめる候補
- **Schur補元**。pose6つのブロックを先に消去して9×9に縮める方法は、計算量が `O(V·6³ + 9³)` と軽い。今回は密なCholeskyで777元を許容時間に収めている。Vの上限を128より上げるときに見直す

## 試験

`tests/calibration_lens_tests.cpp`（CTest `m2_calibration_lens_contracts`）。K・係数・R・tから独自に投影してsessionを組み、推定器にはpixelだけを渡す。試験側の投影は、本番のHornerの形ではなく項を展開した別の書き方（J1とPHだけが内部headerの関数を直接呼ぶ）。画像は1280×960、チャートは17列×13行（ピッチ25 mm、原点(−200, −150) mm、221点）、Kの真値は {1000, 1040, 651.8, 470.8}、係数の真値は k1=−0.1、k2=0.05、k3=0.01、p1=1e-3、p2=−5e-4。training viewはT1〜T6（T1は最後の5点が検出済みで使わない）、validation viewはV1・V2（偶数番目の点が111点のpose点、奇数番目が110点のmeasurement点）、reference viewはR1。許容値は試験の入力であり、製品の閾値ではない。policyは {2, 1e-2, DistortedAnalytic, 100, 1e-10, 1e-12, 1e-3}。

| ID | 内容 |
| --- | --- |
| A0 | 不正なpolicy（最低視点数 0・1・129、cutoff 0・1・負・NaN・無限大、modelが歪みなしか範囲外、max_iterations 0・1001、両方の判定値の0・1・負・NaN・無限大、dampingの0・負・NaN・無限大）が `invalid_argument`。範囲の端は受理。メッセージはカテゴリだけ。全enumのコード文字列と範囲外の空 |
| S-a | 引数が2つだけ（`static_assert`）、policyの欄が7つだけ |
| A2 | cutoffの感度。E1の配置で、cutoff `1e-2` はrank 5、5番目の相対特異値が `5e-2` 以上、6番目が `1e-4` より大きく `2e-3` 以下。cutoff `1e-4` では段階2が `inconsistent-linear-constraints`（rank 6）で止まり、`initialization-unavailable` になる |
| E1 | 歪みあり・ノイズなし。estimated、収束で停止。sessionに無い格子点192点（半ピッチずらした点）を、推定と真値で投影した差が6 viewとも最大 1e-6 px 以下。V1・V2のmeasurement残差のmaxも 1e-6 px 以下。係数の差は info 行に出すだけ（合否に使わない） |
| RS | 残差を試験側で計算し直す。view別のrms・max・N、全体の `√(Σe²/N)`、`final_cost`、validationはmeasurementの点（poseの点ではない）で |
| TS | 点の役割の取り違えを検出する。(a) V1のmeasurement点を動かしてもheld-outのposeとsolverの記録はビット単位で同じで、残差だけが変わる (b) V1のpose点を動かすとposeが変わる (c) T1の使わない点を動かしても結果全体がビット単位で同じ (d) T2の使う点を動かすとKが変わる |
| N1 | σ=0.25 px、seed 290（SplitMix64とBox–Muller）。training rmsが `σ√2·√(1 − P/2N)` の±10%以内、held-out rmsが `σ√2` の0.85〜1.20倍、未使用点の差のrmsが0.5σ以下 |
| M1・M2・M3 | 上限1ならsolver-nonconvergence（K・係数・checkはnull、solverは受理1回・上限停止）。E1の試行数nちょうどなら同じtraining結果、n−1ならsolver-nonconvergence。M3は相対コストの規則が、受理前のコストとの比であることを、最初のステップの削減率の上下の判定値で確かめる |
| HL | trainingは1試行で収束し、異なるレンズで作ったvalidationのpose推定が上限に届かない。`solver-nonconvergence` が出て、coverageが `some-validation-views-not-evaluated`、trainingの結果は変わらない |
| HV | validation viewのpose点が3つでは、そのviewは `initialization-unavailable`。cameraはestimatedのまま、集計は残りのviewだけ。validation viewが無ければ `no-validation-view` |
| PH | 内部headerの判定関数を境界で直接試験する。放射の傾き: `(0,−0.05,0)` の `s_max=2` が厳密に0、`s_max=1` で0.75、`s_max=4` で−3。端点がどちらも正で内部の最小が負の例（`(−1,0.4,0)` で−0.125、端点だけを見る実装はここで落ちる）。臨界点が区間外の例、負の臨界点を無視する例。3次式は5例を2e5点の密なサンプリングと照合（差1e-9以下、符号一致）。2つの根（`q/a`・`c/q`）と一次の根のそれぞれが最小になる例。行列式: `p1=−0.5` で点(0,1)が厳密に0（−0も不正）、y=0.5で−0.25、y=0.25で0.1875。`p1=p2=0` なら `L·g` と1e-14以内。`PhysicalIntrinsics`・`PhysicalDistortion` の0・負・NaN・無限大 |
| PE | end-to-end。真値を k1=0、k2=2.4、k3=−12、p1=p2=0 にして、E1と同じ配置で生成。`invalid-distortion`、傾きと行列式がどちらも負、Kはあり、係数とposeはnull、held-outはnot-reached |
| I1 | 正面だけの3 viewで `initialization-unavailable`。`initialization` が、段階2を直接呼んだ結果とビット単位で同じ |
| C1 | CAM-Bの入力が良くても正面だけでも、CAM-Aがビット単位で同じ。逆向きも |
| D1 | 同じJSONを2回Parseし、それぞれ2回推定して4回とも同じ |
| Z1 | 歪み0・ノイズなし。段階2の写像との差が検査点で 1e-6 px 以下、係数の絶対値が 1e-9 以下。`initialization` は段階2そのもの |
| DM | `initial_damping = 1e12`。ステップを採らず、`accepted_steps` が0、`final_cost == initial_cost`、歪み0のまま |
| J1 | 解析Jacobianと、試験側の投影の中心差分（刻み `1e-6·max(1,|θ_j|)`、回転は試験側のRodriguesで `Exp(±h·e_k)·R`）を照合。5点・15列とも相対誤差1e-6以内。あわせてpixelが試験側の投影と1e-9 px以内、`Exp(ω)` がRodriguesの式と1e-14以内 |
| L1 | Releaseのみ。128 view・777未知数（13×9格子）で収束、試行50回以内、未使用点の差1e-6 px以下 |
| INV | すべての結果に対して、statusと欄のnull規則、`FailedStage` との対応、成功ならすべての欄がそろうこと、reference非掲載、viewの順、`iterations ≤ max_iterations` と `iterations ≥ accepted_steps`、`final_cost ≤ initial_cost`、残差の要約の妥当性、Rが回転行列であること、pooled残差がviewごとの値から再現できること |
| F1 | 別の実行ファイル `m2_calibration_lens_formula`。生成器の `DistortNormalized` とlibraryの同名の関数を、[−0.8, 0.8]² の41×41点で3組の係数について比べ、ビット単位で一致すること |

`tests/calibration_lens_isolation_tests.cmake`（`m2_calibration_lens_link_isolation`）は23件。pure libraryの正の対照2件と、`synthetic_pair`・`rig_profile_v2`・Win32系・OpenCVの直接・推移の拒否、importedのOpenCV、禁止sourceの推移混入の拒否を確認する。

段階2の試験（`m2_calibration_intrinsics_contracts`）には、`DistortedAnalytic` を拒否する1行（A0）だけを足した。既存の行は変えていない。

## 置いた仮定

- 検出済みpixelを歪み込みの保存順pixelとみなす（`DistortedAnalytic`）
- 初期値は歪み0の閉形式（段階2）。held-outのpose初期値は、段階2の同じviewのpose
- LMはMarquardtの対角スケーリング、密なCholesky分解、dampingの10倍・1/10倍。停止判定はCeres型の相対ステップと相対コスト
- 物理性の検査は収束した後に1回だけ
- `ObservationModel::DistortedAnalytic` の値は2。1は割り当てない（段階2の試験が範囲外の値の例として使うため）
- `pose-unavailable` は3aで到達しない（上記）
