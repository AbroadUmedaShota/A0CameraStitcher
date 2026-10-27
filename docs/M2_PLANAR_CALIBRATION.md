# 平置きのチャートだけから歪みと文書面の写像を求める（#374）

#367 の段 2（#351 R4）。吊り下げリグではカメラを動かせず、チャートを傾ける道具も無い。図面は常に机に平らに置くので、チャートも机に平らに置いたまま位置だけ変えて撮り、その写真から各カメラのレンズ歪みと「文書の面 → 画像」の写像を求める。出力は既存の rig profile v2 の draft と詳細な報告で、承認された校正（#45・#43）ではない。

撮影の手順は [校正撮影の手順](CALIBRATION_CAPTURE_PROCEDURE.md) の「平置きの手順」の節。傾ける手順（[レンズ推定](M2_CALIBRATION_LENS.md) と [固定文書写像](M2_REFERENCE_DOCUMENT_MAPPING.md)）は代替として残る。

## 1. モデル

カメラ c、view j のチャート点 (x, y) mm から画素 (u, v) まで:

```text
文書 (X, Y)        = R(θ_j) (x, y) + (tx_j, ty_j)          置き方 j の平面内の剛体変換
歪みなしの画素 (U, V) = H_c (X, Y, 1)                          3 × 3、h22 = 1
正規化 (x_n, y_n)  = ((U − cx) / fx, (V − cy) / fy)           K_nom
画素 (u, v)        = K_nom · BrownConrady(x_n, y_n)            既存の 5 係数の式
```

- 合成に要るのは文書の面から画像への写像で、奥行き方向の内部パラメータ（焦点距離そのもの）は要らない。K は推定しない
- K_nom は公称値から作る: fx = f × 幅 px / センサー幅 mm、fy = f × 高さ px / センサー高さ mm、主点は標本格子の中心 ((幅 − 1) / 2, (高さ − 1) / 2)、skew 0。歪みの式と物理性の検査は `calibration_lens_model.hpp` のものをそのまま使う
- 真の焦点距離と主点が公称値とずれると、係数はその分だけ別の値になるが、写像（文書 → 画素）は合う。係数を他のレンズや他の K と比べない
- 文書の座標は、重なる範囲に置いた置き方（両台に写る view）で決める。その置き方を T = I とし、チャートの F-000-000 を原点、チャートの軸を文書の軸にする。2 台はこの置き方だけを共有する
- profile に書くときは、job の `overlapPlacement`（重なりのチャートが用紙のどこにあるか）で用紙の座標に移す

## 2. 未知数と最適化

| 未知数 | 数 |
| --- | --- |
| H_c（h22 = 1） | カメラごとに 8 |
| 歪み k1 k2 k3 p1 p2 | カメラごとに 5 |
| 置き方 (θ, tx, ty) | training の view ごとに 3（重なりの view は T = I で 0） |

2 台が共有するのは T = I に固定した重なりの置き方だけなので、2 台の問題は厳密に分かれる。カメラごとに 1 回の Levenberg–Marquardt で、H_c・5 係数・全 training view の置き方を同時に最小化する（raw 画素の再投影誤差の二乗和）。

- 初期値: 重なりの view の DLT（チャート → 画素、段階 2 と同じ Hartley 正規化と Jacobi）を H_c に、各 training view の DLT に H_c⁻¹ を掛けて最も近い剛体変換を置き方に、歪みは 0。乱数なし
- 反復: 既存のレンズ推定と同じ LM（`calibration_levenberg_marquardt.hpp` に切り出して共用。Marquardt の対角スケーリング、Cholesky、減衰 ×10 / ÷10）。止まる条件も同じ: コストの相対減少、ステップの相対の大きさ、反復の上限（試したステップを全部数える）、数値の失敗。ステップの大きさは H・5 係数・並進のノルムに対して測り、角度は入れない
- 物理性: 収束した値で 1 回だけ判定する。fit に使った点の範囲で `MinimumRadialSlope` > 0 と `DistortionJacobianDeterminant` > 0、H_c の行列式が厳密に 0 でないこと。落ちたら profile を出さない（`invalid-distortion`）
- validation の view は学習に使わない。H_c と 5 係数を固定して、`posePointIds` に置き方 (θ, tx, ty) だけを合わせ（初期値は歪みを除いた画素の DLT）、`measurementPointIds` の残差を測る
- 直線性: チャートの同じ行・同じ列の点（3 点以上）に、検出した画素と、推定した歪みを除いた画素のそれぞれで直線を当て、直線からの距離を出す。H は直線を直線に写すので、歪みを除いた後に曲がりが残れば歪みの推定が合っていない。validation の view では学習に使っていない点で確かめることになる

推定器は STL だけで、OpenCV・Win32・生成器・profile reader に依存しない（`a0_m2_planar_calibration`、CMake の依存検査あり）。

## 3. job（`a0.planar-calibration-job.v1`）

全キー必須、未知キー・型違いを拒否し、既定値を補わない。64 KiB、深さ 8 以下。例は [samples/public/planar-calibration-job.v1.example.json](../samples/public/planar-calibration-job.v1.example.json)（架空の値。実機の条件ではない）。

| キー | 中身 |
| --- | --- |
| `profileId`・`provenanceId`・`measuredAt` | 既存の draft job と同じ。`measuredAt` は session の観測確定時刻以降 |
| `documentPlane` | `orientation`（`landscape`/`portrait`）と `cameraOrder`（`CAM-A-left-CAM-B-right`/`CAM-A-top-CAM-B-bottom`） |
| `overlapViews` | `CAM-A`・`CAM-B` それぞれの重なりの view の ID。session で role が `reference` の view であること |
| `overlapPlacement` | 重なりのチャートの用紙上の位置。既存の `referencePlacement` と同じ向き: `chart = R(angleRadians) × document + (translationXMm, translationYMm)`。全部 0 なら用紙の原点 = F-000-000 |
| `nominalCameras` | カメラごとの `focalLengthMm`（レンズの公称値）、`sensorWidthMm`・`sensorHeightMm`（保存された向きの幅と高さ）。0 より大きく 10000 以下 |
| `numericalPolicy` | `minimumTrainingViews`（1〜128、重なりの view は数えない）、`maxIterations`（1〜1000）、`relativeCostTolerance`・`relativeStepTolerance`（0〜1 の開区間）、`initialDamping`（> 0）、`maxUndistortionIterations`（1〜1000）、`undistortionTolerancePixels`（> 0）。後の 2 つは直線性の検査で歪みを除くときにだけ使う |

数値 policy は停止条件で、光学の合格線ではない。既存の draft job の `relativeSingularCutoff`（段階 2 の閉じた式で使う）は、この推定器に段階 2 が無いので持たない。`rigMeasurements` も持たない（profile では null。公称の焦点距離は測った値ではないので `lensFocalLengthMm` に入れない）。

## 4. 出力

`A0CameraStitcher.DraftCalibrator.exe --session <session.json> --planar-job <job.json> --output <新しいフォルダ>` が、既存の reference-pair 経路（`--job`）と同じ書き方で `profile.json` と `calibration-report.json` を出す。`--job` と `--planar-job` は一方だけ。exit 0: draft、3: 報告だけ（profile なし）、2: 入力か IO の誤り。

### profile（既存の rig profile v2、`status: draft`）

| 欄 | 中身 |
| --- | --- |
| `cameras.*.projection.intrinsics` | K_nom |
| `cameras.*.projection.distortion` | 推定した 5 係数 |
| `cameras.*.projection.documentToImage` | H_c × (用紙 → 重なりのチャート)。用紙 mm → 歪みなしの画素、h22 = 1 |
| `calibration.method` | `chart-fiducial-planar`（v2 の定数） |
| `calibration.tool.toolId` | `a0-planar-calibrator`。この tool は常に公称の K を使う |
| `calibration.inputSha256` | 両台の重なりの view の画像ハッシュ（文書の座標を決めた画像） |
| `calibration.fitResidualPixels` | training と重なりの view の、fit に使った点の rms と max |
| `calibration.rigMeasurements`、`outputRaster`、`correctionEnvelope`、`qualityContract`、`approval`、`physicalIdentity` | null |

K が公称値であることを書く欄は profile v2 に無い。v2 の形（ADR-0034、[rig-profile-v2](design/rig-profile-v2.md)）を変えないため、profile では tool ID で区別し、詳細は報告に書く。v2 に欄を足すかは承認された profile（#45）を作るときに決める（※所有者判断）。

### 報告（`a0.planar-calibration-report.v1`）

`result`（`draft-produced`/`no-profile`）、`rejection`（`unsupported-image-dimensions`、`planar-calibration-unavailable`、`validation-not-evaluated`、`overlap-placement-invalid`、`profile-metadata-invalid`）、`profileFingerprintSha256`、`sessionSha256` と session そのもの、`numericalPolicy`、`model`、`intrinsicsSource: "nominal-not-estimated"`、`nominalCameras`（宣言値と、そこから作った K）、`overlapViews`、`overlapPlacement`、`documentPlane`、`quality: "not-assessed"`。

`cameras[]` はカメラごとに:

- `status`（`planar-calibration-estimated` ほか、ヘッダ `planar_calibration.hpp` の一覧）、`nominalIntrinsics`、`solver`（反復数、受け入れたステップ数、初期と最後のコスト、止まった理由）、`distortion`
- `overlapChartToImage`（重なりのチャート座標での H_c）と `documentToImage`（profile と同じ、用紙の座標）
- `distortionCheck`（fit した点の最大 r²、そこまでの半径方向の傾きの最小、歪みのヤコビアンの行列式の最小）
- `fitResidual`（training と重なり）、`overlapResidual`、`heldOutCoverage`、`heldOutResidual`（validation の `measurementPointIds`）
- `straightness.fit`・`straightness.heldOut` のそれぞれ `raw` と `undistorted`（rms、max、行と列の数、点の数）
- `views[]`: view ごとの role、重なりかどうか、`outcome`、置き方（重なりは 0, 0, 0）、残差、validation の LM の記録、直線性

profile が出るのは、両台が `planar-calibration-estimated` で、両台の validation がすべて評価できたときだけ。残差・直線性にはどの合格線も当てない。合格線は所有者が決める（#367 段 4）。

## 5. 1 コマンドで流す

```powershell
pwsh -NoProfile -File scripts/Invoke-DraftCalibrationPipeline.ps1 -Planar `
  -DetectionJob <画像の隣にあるdetection-job.json> `
  -CalibrationJob <planar-calibration-job.json> `
  -DetectorExecutable build/reference/Release/A0CameraStitcher.FiducialDetector.exe `
  -CalibratorExecutable build/reference/Release/A0CameraStitcher.DraftCalibrator.exe `
  -OutputDirectory <親が存在する新規出力フォルダ>
```

`-Planar` を付けると、検出の後に `--planar-job` で推定し、報告の schema を `a0.planar-calibration-report.v1` として確かめる。`pipeline-report.json` の `calibrationKind` が `planar-nominal-k` になる。それ以外（読み取りロック、snapshot、ハッシュの一覧、exit code）は [校正コマンド](M2_CALIBRATION_COMMAND.md) と同じ。

出来た draft は `StitchEval --profile-file` で読める（既存の評価用の読み込みと同じ検査）。

## 6. 試験

```powershell
cmake -S . -B build/planar -G "Visual Studio 17 2022" -A x64 -DA0_BUILD_DRAFT_CALIBRATOR=ON -DA0_BUILD_STITCH_EVAL=ON -DBUILD_TESTING=ON
cmake --build build/planar --config Release --target m2_planar_calibration_contracts -- /m:1 /nodeReuse:false
ctest --test-dir build/planar -C Release -j 1 -R '^m2_planar_calibration_contracts$' --output-on-failure
```

`tests/planar_calibration_tests.cpp`。観測は人工の生成器（`a0_m2_synthetic_pair` の `ProjectDocumentPoint`、画像を描くのと同じ式）で作る。推定器は真の値を見ない。

- 構成: D810 の寸法、公称 50 mm・35.9 × 24.0 mm。2 台とも 90° 回した向きで約 7 px/mm、面内で少し回し（0.4°、−0.7°）、机に対して少し傾けた H。歪みは CAM-A (k1, k2, k3, p1, p2) = (−0.045, 0.09, −0.06, 1.5e−4, −2e−4)、CAM-B (−0.052, 0.11, −0.08, −1e−4, 1.2e−4)。チャートは 11 × 15 節点、ピッチ 20 mm。各カメラに training 5（範囲の中央と四隅、少しずつ回す）、validation 1、重なり 1（2 台で同じ置き方）。画像の縁から 20 px 以内の点は使わない
- 真の K = 公称の K、揺らぎなし: 5 係数が既知の値から 1e−6 以内、H と写像全体（文書 → raw 画素）が範囲内の約 2400 点で 1e−4 px 以内、validation の残差が 1 px 未満、歪みを除いた後の行と列が直線（1e−4 px）、profile v2 の読み込みと評価用の描画が通る、同じ入力で同じバイト列、`--planar-job` の exe が同じバイト列を書く
- 公称の K がずれた場合: 真の焦点距離を 1 % 長く、主点を (±12, −8) px ずらし、画素に ±0.1 px の決まった揺らぎを足す。係数は比べず、validation の残差と写像の差が 1 px 未満であることを確かめる
- 拒否: 未知のキー、欠けたキー、別の schema、不正な policy、文字列の数値、焦点距離 0、重なりの view が reference でない・別のカメラ・無い、training が足りない、validation が無い、`measuredAt` が観測より前

試験の閾値（1e−6、1e−4 px、1 px）は試験の入力で、実写の合格線ではない。実行した結果（Debug・Release とも 48 チェック・失敗 0）:

| 項目 | CAM-A | CAM-B |
| --- | --- | --- |
| 係数の差の最大（k3） | 3.6e−12 | 8.3e−12 |
| H の要素の相対差の最大 | 1.9e−13 | 2.0e−12 |
| 写像の差の最大（raw 画素、2491 点 / 2438 点） | 7.5e−12 px | 2.2e−11 px |
| validation の残差 rms | 2.0e−12 px | 5.2e−12 px |
| 直線性 rms（検出した画素 → 歪みを除いた画素） | 0.081 → 4.0e−11 px | 0.091 → 3.9e−11 px |
| 公称の K がずれた場合: validation の残差 rms / 写像の差の最大 | 0.071 px / 0.049 px | 0.072 px / 0.065 px |

### 画像から StitchEval まで（手で流す確認）

試験の exe に `--write-fixture <新しいフォルダ>` を渡すと、上の構成の `session.json`・`job.json`、同じ真の 2 台で A0 の人工チャートを描く生成器の spec（`pair-spec.json`）、真の K・歪み・H を入れた profile（`truth-profile.json`）を書く。

```powershell
build/planar/Release/m2_planar_calibration_contracts.exe --write-fixture <f>
build/planar/Release/A0CameraStitcher.DraftCalibrator.exe --session <f>/session.json --planar-job <f>/job.json --output <d>
build/planar/Release/A0CameraStitcher.SyntheticPairGenerator.exe --spec <f>/pair-spec.json --output <p>
build/planar/Release/A0CameraStitcher.StitchEval.exe --profile-file <d>/profile.json --camera-a <p>/CAM-A/original.jpg --camera-b <p>/CAM-B/original.jpg `
  --product-root <空のフォルダ> --output-directory <e> --left-um 40000 --top-um 20000 --right-um 1150000 --bottom-um 820000 `
  --dpi 50 --width-pixels 2186 --height-pixels 1575 --resampling bilinear
```

推定した draft と `truth-profile.json` のそれぞれで StitchEval を流すと、どちらも exit 0（`result=evaluated`）で、`evaluation.jpg` の SHA-256 が一致した（同じバイト列）。用紙の全体（0〜1189 mm）を指定すると、この人工の構成では 2 台とも写さない左右の端が残り、真の profile でも `render-failed` になる。

## 7. 限界（※要確認）

- 人工の観測だけの結果。実写の検出誤差、紙の浮きとたわみ、印刷の倍率、ピントの位置による歪みの変化は入っていない。実写は #374 の検証方法（所有者の GO の後）で確かめる
- 机が平らでないと、置き方ごとに面がずれ、H と歪みに混ざる。紙は机に直に置く（板に貼ったチャートは厚みの分だけ面が変わるので使わない）
- 範囲の縁と隅に点が無いと、そこでの歪みは外挿になる。四隅の置き方が要る
- 物理性で落ちる（`invalid-distortion`）経路は、人工の正常な観測からは起きないので、この試験では通っていない。判定の式は既存のレンズ推定と同じものを使っている
- 公称の K を使うので、係数そのものは真のレンズの係数ではない。profile の K と係数は組で使う
