# 位置合わせの模型を当てる開発用の道具（#417 の P0）

保存済みの 2 枚（A と B の原画像）について、ゲートが測る「B が A に対してどれだけずれて見えるか」の場に、位置合わせの模型を当てて比べる。B の姿勢（6 自由度）の小さな変化で説明できるのか、校正の外挿や紙の浮きが残るのかを、数字で決めるための道具で、製品には入れない。

- 親: #417（図面の中身で撮影ごとに B の姿勢を推定して合わせる）。P2 の推定器を作る前に、模型の形を決める段
- 実機は使わない。合成も書き出しもしない。入力の画像と値は非公開で、結果は基準セットの外（リポジトリの外）に置く

## ビルドと呼び出し

道具の本体は既定 OFF の `A0_BUILD_REGISTRATION_PROBE`。計算の部分（`a0_m2_registration_probe`）は常にビルドされ、`m2_registration_probe_contracts` が ctest に入る。

```powershell
cmake -S . -B build/p0-417 -A x64 -DA0_BUILD_REGISTRATION_PROBE=ON
cmake --build build/p0-417 --config Release --target A0CameraStitcher.RegistrationModelProbe
```

測る（A と B の原画像は保存された画素順のまま読み、EXIF の向きは当てない）。

```text
A0CameraStitcher.RegistrationModelProbe --condition <固定の合成条件 v2> --image-a <A の JPEG> --image-b <B の JPEG> --out <結果の folder>
    [--label <名前>] [--spacing 64] [--patch 64] [--search 16] [--prior dx,dy] [--global-evaluations 256] [--threads N]
    [--inject-pose rx,ry,rz,tx,ty,tz] [--crop x,y,size]
```

2 回の測定の差に当てる（同じ設定で測った 2 つの `field.csv` を、標本の位置で結ぶ）。

```text
A0CameraStitcher.RegistrationModelProbe --condition <固定の合成条件 v2> --out <結果の folder> --spacing <測った間隔>
    --field-before <前の field.csv> --field-after <後の field.csv>
```

## 測り方

- 条件（v2）が名指す draft rig profile で、A と B を倍率 1・補正なしの固定の写像で文書の出力格子に別々に置く。取り付けの向きに関わらず、写像は保存順の画素に対して定義されているので、画像は回さない
- 重なりの帯の全域に、ゲートの測定器（`seam_gate::MeasureSeam`）を疎な格子（既定 64 px おき）で当てる。点の有効性、往復照合、周期の検査、残りの大きい点の再測定による確認はゲートのまま。数えるのはゲートが「数える」とした点だけ
- 変位は「B が A のパッチを見せる位置 − その点」（出力 px）。線が一方向だけの点は、線に直交する成分だけを使う
- 探す半径はゲートの既定が 16 px。真の変位がこれを超える組（B を動かした後など）では、点がほとんど有効にならない。`--prior dx,dy` で B を見る位置を変位の見込みだけずらし、真の変位が窓に入るようにする。帯の位置は prior なしの写像で決めるので、同じ設定の測定は同じ格子になる。`field.csv` の変位には prior を足し戻してある
- `--patch` は照合のパッチの一辺。大きいほど 2 方向の構造を拾いやすいが、帯の端が削られる

## 模型

| 名前 | 自由度 | 内容 |
|---|---|---|
| `translation-2` | 2 | 平行移動 |
| `similarity-4` | 4 | 平行移動・回転・倍率（データの中心まわり） |
| `similarity-vertical-scale-5` | 5 | 相似＋縦の倍率 |
| `pose-6` | 6 | B の姿勢の小さな変化。下に書く |
| `projective-8` | 8 | 文書面の射影 |
| `polynomial2-12` | 12 | 2 次の曲面。候補ではなく、滑らかな残りがどれだけあるかを見る参考 |

`pose-6` は、校正の射影 H_B を B の内部行列（公称）で K⁻¹H_B と分け、文書面を B のカメラ座標の剛体運動で動かして作る。文書面は、光軸が文書に当たる点（pivot）のまわりに (rx, ry, rz) だけ回り、(tx, ty, tz) だけ動く。tz は pivot の深さに対する %（高さ）。内部行列、レンズの 5 係数、A の写像は動かさない。補正は G = H_B⁻¹·H(p) という文書面の射影になり、p = 0 で恒等。符号は B のカメラ座標（保存順の画素）での向きで、大きさで比べる。

当てはめは重み付き最小二乗（点ごとの不確かさ、下限 0.1 px）に Tukey の重み付けをかけた反復で、乱数は使わない。パラメータの不確かさは正規方程式から出す形式的な値で、周期の取り違えなど外れ値による偏りは含まない。

## 出力（結果の folder）

- `summary.txt`: 点の数と内訳、模型ごとの残り、継ぎ目の列の上下の形、`pose-6` の推定値
- `models.json`: 同じ内容（模型ごとに継ぎ目の列、帯の全域、形、パラメータと形式的な不確かさ）
- `field.csv`: 点ごとの変位、不確かさ、各模型の残りと最終の重み。2 回の測定の差を当てる入力にもなる
- `seam_residual.csv`: 出力の全行について、継ぎ目の列に沿った残り（模型ごと）

継ぎ目の列に沿った残りは、各格子行について継ぎ目の列から格子 1 つ分以内の最も近い点の残りを取り、間が 2 格子以内の格子行どうしを直線でつないで全行に延ばしたもの。外挿はしない。測れた行（`rows`）だけで平均・p95（最近順位）・最大を出す。紙が白く測れない行は入らない。

`--inject-pose` は検証用で、B の画像を「B のカメラ座標で文書面が p だけ動いた時の画像」に作り直してから測る（実画像の質感と雑音のまま、写像だけを既知の量だけ動かす）。推定器が既知の動きを戻すことの確認に使う。測る窓の prior は、利用者が渡した値に、注入した動きが継ぎ目の列の中央の高さで起こす変位を足したものになる。2 回の測定の差を当てる方法（上）の確認には、同じ組の原画像に動きを注入して測り、注入しない測定との差に当てる。

## 限界

- 模型の違いを区別できる量は、点の数と向きで決まる。横罫だけの紙では dx がほとんど見えない
- B の平行移動（カメラ座標）と、紙が校正の平面より高いこと（視差）は、場の上では区別できない。厚い冊子は B の平行移動として吸収される。紙の高さ h は、2 台の光軸の間隔（基線 b）と深さ Z に対し、約 h·b/Z の平行移動に当たる
- 継ぎ目の列に沿った残りの測れる行は、紙の中身で決まる

## 推定器を当てる（P2、`--estimate`）

同じ入口に、P2 の推定器（`content_registration`、設計は `docs/design/fixed-composite-registration.md`）を保存済みの組へ当てる動作を足した。姿勢の模型（`PoseModel` ほか）は `registration_pose.hpp` へ移し、この道具の `registration_probe` 名前空間からは同じ名前で見える。

```text
A0CameraStitcher.RegistrationModelProbe --estimate --envelope <位置mm,回転度,傾き度,高さ%> --condition <固定の合成条件 v2>
    --image-a <A の JPEG> --image-b <B の JPEG> --out <結果の folder> [--label <名前>] [--threads N]
    [--mixing-width 8] [--omega-margin 8] [--guard 72] [--band-half-width N] [--min-unique 2]
    [--max-prediction-sigma 1] [--max-rms 3] [--max-p95 6] [--max-bias 4] [--convergence 2] [--start-agreement 2]
    [--fine-spacing 96] [--fine-search 24] [--fine-max-search 48]
```

- `--envelope` は必須（範囲の既定は無い）。上の閾値は開発用の仮の値で、製品の合否の線ではない。V5 と収束の値は、実機の組で測った残りの大きさに合わせて緩めてあるだけで、所有者が決める
- 状態（verified / undetermined / out-of-envelope）、理由、超過した成分、p̂ とその一標準偏差、画面向けの表示（位置・回転・傾き・高さ）、粗のブロックの結果、細の点数、検査の値、所要時間を `estimate.txt` に書く。点ごとの変位と残りと重みは `observations.csv`
- 最後に、p̂ で B を置いた時の Ω（継ぎ目の列 ± ゲートの範囲）の点を、ゲートの測定器で測り直して残り（平均・p95・最大、数えた点数）を出す。Ω の点は推定に使っていない。状態が `out-of-envelope` や `undetermined` でも、当てた場合に何が残るかを見るために測る
- 実画像の入力と結果はリポジトリの外に置く
