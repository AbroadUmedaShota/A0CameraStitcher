# rig profile 2.0.0（文書平面基準・レンズ係数・mm/DPI 出力）— 設計（Issue #271）

状態: 草案。判断は `docs/DECISIONS.md` ADR-0034（Proposed、承認は所有者）
対象 Issue: #271。親: #278。後続: #274（レンズ補正と写像を 1 回の再サンプリングで）、#277（チャートからの draft 校正）、#273（評価専用の合成 exe）、#45（approved の発行）、#46（pipeline の完成）
正本: schema `docs/schemas/rig-profile.v2.schema.json`、draft 例 `samples/public/rig-profile.v2.draft.example.json`、試験 `scripts/Test-RigProfileV2Schema.ps1`（ctest 名 `m2_rig_profile_v2_schema`）
範囲外: C++・.NET の実装（#274）、数値（DPI・閾値・レンズ係数）の決定（#42・#43）、リグの決定（#43）

## 0. 前提と置いた仮定

- 1.1.0 の approved profile は試験用の synthetic だけで、実リグのものは無い（.NET の `DualCameraRigProfile.ApprovedSynthetic()`、C++ の契約試験の fixture）。移すデータは無い
- 製品は今、profile を JSON から読んでいない。.NET の `DualCameraRigProfile` の値を `M2OfflineStitcherProcessAdapter` が CLI 引数（`--matrix`、`--crop` など）に直し、C++ の `M2Adapter`（`src/m2/offline_stitcher_adapter_main.cpp` の `ParseProfile`）へ渡している
- 二台とも D810、JPEG Fine L（7360×4912）。レンズは同じ型式を想定するが、係数はカメラごとに持つ
- 非機能の目標（※仮定）: A0 横・180 DPI（#42 の v0.1 候補、8426×5960）を 1 回の再サンプリングで出す。時間とメモリの目標は #270 の実測で決め、本書では置かない
- データ整合性: profile は不変の文書で、内容を変えたら新しい `profileId` にする。合成 1 回の中で「読む → 検証 → 合成 → manifest」が閉じ、manifest は適用した値の fingerprint を持つ（ADR-0026）。profile を複数のプロセスが書き換えることは無い
- #269（正解つき合成ペアの生成器）と #277（校正ツール）は本書の規約に合わせる前提で書いた。並行作業のため、生成器の正解 JSON の形は #269 側で決まる（※要確認）

## 1. 推奨案

文書平面を基準にする。カメラごとに「文書平面（mm）→ 歪みのない画像座標」の射影変換とレンズモデルを持ち、出力ラスタを文書平面の mm と DPI で決める。出力の画素ごとに、文書の点から各カメラの生 pixel までを 1 本の式で求め、1 回だけ標本化する。

理由:

1. 出力の形: #42 の候補（A0 横 8426×5960、180 DPI）は文書の mm で決まっている。文書平面を基準にすれば、出力の画素格子をそのまま mm で書ける。CAM-A の生 pixel を基準にすると出力は CAM-A の画素格子に縛られ、A0 の mm に合わせるには後ろに回転と拡縮の段（2 回目の再サンプリング）が要る
2. 再サンプリング 1 回（#46 の完了条件）: レンズの歪み、カメラの配置、出力の DPI と向きが、出力画素ごとの 1 本の逆写像にまとまる。中間画像を作らない
3. 二台の対称性: CAM-A と CAM-B が同じ形の値を持つ。v1 は CAM-A を基準にして CAM-B だけが行列を持つので、CAM-A のレンズ歪みを直す場所が無かった
4. 校正との親和性: チャートの基準点は文書平面上の mm で既知なので、平面チャートからの校正（#277）はカメラごとの「文書 → 画像」の写像とレンズ係数をそのまま出す。OpenCV の `calibrateCamera` の出力（内部パラメータ・歪み係数・外部パラメータ）と同じ形で、変換の段が要らない

## 2. 座標系と単位

### 2.1 文書座標

- 原点は、出力の向きで見た用紙の左上の角。X は右、Y は下、単位は mm
- 用紙は ISO 216 の A0。横（landscape）は 1189×841 mm、縦（portrait）は 841×1189 mm
- 向きはリグの配置の決定（HG-0002・#43 の placement/orientation）で、`documentPlane.orientation` に書く。文書座標は出力画像と同じ向きにとり、向きを別の回転の段として持たない
- チャートの座標から文書座標への対応（チャートを台のどこに、どの向きで置いたか）は校正ツール（#277）が持つ。profile には文書座標で書いた結果だけが入り、使ったチャートは `calibration.chartId` に残る
- 文書は 1 枚の平面とみなす。原稿の浮き（視差）は profile では表さない（#43 の原稿の押さえ方で抑える）

### 2.2 カメラの生 pixel 座標

- JPEG を復号したときの画素の並び（保存されている順）を使う。列 i、行 j の画素の標本点を (u, v) = (i, j) に置く
- EXIF Orientation は見ない（4 節）
- これは OpenCV の内部パラメータと同じ約束（画素の中心が整数座標）なので、OpenCV で校正した cx, cy をずらさずに使える

### 2.3 正規化カメラ座標

歪みのない画像座標 (u, v) から x = (u − cx) / fx、y = (v − cy) / fy。無次元で、レンズモデルはこの座標で働く。

### 2.4 単位の一覧

| 量 | 単位 | 欄の例 |
| --- | --- | --- |
| 文書座標・用紙・出力領域 | mm（領域は 1 µm 刻み） | `documentPlane.sheetWidthMm`、`outputRaster.regionMm.left` |
| 生 pixel・内部パラメータ | カメラの pixel | `intrinsics.fxPixels`、`intrinsics.cxPixels` |
| 歪み係数 | 無次元（正規化座標に対して） | `distortion.k1` |
| 出力の解像度 | DPI（1 inch = 25.4 mm ちょうど） | `outputRaster.dpi` |
| 出力の寸法 | 出力の pixel | `outputRaster.widthPixels` |
| 位置ずれ・seam の上限 | 出力の pixel（`outputRaster.dpi` での） | `correctionEnvelope.registrationErrorOutputPixels` |
| 校正の残差 | カメラの pixel | `calibration.fitResidualPixels` |
| 重なりの下限 | mm（文書座標） | `correctionEnvelope.minimumOverlapMm` |
| 回転・倍率差・露出・色 | 度・%・EV・ΔE（v1 と同じ） | `correctionEnvelope.rotationErrorDegrees` |
| 時刻 | UTC、秒まで、末尾 `Z` | `calibration.measuredAt` |

pixel が 2 種類（出力の pixel とカメラの pixel）あるので、出力側の欄には `OutputPixels` を付けて名前で分けた。色差の式（CIEDE2000 か）は v1 と同じく未定（#42、※要確認）。

## 3. 画素のモデル

### 3.1 v1（今の main）

`src/m2/offline_stitcher.cpp` の `SampleBilinear`（646-672 行付近、Issue #86）の約束:

- 画素 i の値は座標 i に置く。`x0 = floor(x)`、`dx = x - x0` の双一次補間で、x = i なら画素 i の値がそのまま出る
- 被覆の判定は [0, w) × [0, h)。[w−1, w) は端の列にクランプして被覆とみなし、x < 0 は被覆しない
- 出力画素 (x, y) は大域座標 (minimum_x + crop.left + x, minimum_y + crop.top + y) の整数点を標本化する（1112-1117 行付近）

標本点は OpenCV と同じ整数座標だが、被覆の範囲は標本点から見た [−0.5, w−0.5) ではなく [0, w) で、右と下へ半画素ずれている。

### 3.2 v2 の生 pixel

- 標本点の約束と双一次補間の式は v1 と同じ
- 被覆の判定も v1 の [0, w) × [0, h) を引き継ぐ。#86 の修正と #268 の fixture（非整数の平行移動）を変えずに済み、#274 の「歪み 0 で I-1 と画素一致」の検算も壊さない。影響はフレームの右端と下端の半画素の帯だけで、その帯は重なりの feather の中か出力領域の外にある
- 被覆を [−0.5, w−0.5) に揃えるかは、engine の版を上げる別の判断にする（本 schema の範囲外）

### 3.3 v2 の出力ラスタ

- 出力画素 (i, j) は文書上の矩形 [left + i·p, left + (i+1)·p) × [top + j·p, top + (j+1)·p) を表し、その中心で標本化する。p = 25.4 / dpi（mm）
- 計算の順は固定する。`p = 25.4 / dpi` を倍精度で 1 回だけ求め、`X = left + (i + 0.5) * p`、`Y = top + (j + 0.5) * p`。順を変えると丸めが変わり、出力の byte 一致が崩れる
- 出力の画素数は `widthPixels = ceil((right − left)[µm] × dpi / 25400)` を整数で計算する。高さも同じ。`src/m2/optical_planner.cpp` の `RequiredPixels`（mm × dpi × 10 / 254 の整数の切り上げ）を µm に広げたもので、A0 横・180 DPI で 8426×5960 になる（1189000 × 180 / 25400 = 8425.98…、841000 × 180 / 25400 = 5959.84…）
- ラスタの実寸は widthPixels × p なので、領域より 1 画素未満（180 DPI で 0.141 mm 未満）だけ右と下へはみ出す。はみ出した部分にもカメラの被覆が要る

### 3.4 v1 と同じ画素を出すとき（#274 の検算）

整数の dpi では 25.4 / dpi（= 127 / (5·dpi)）が 2 進の有限小数にならない。mm/DPI の段を通すと標本点が整数から倍精度の丸めの分だけずれ、[0, w) の境界（たとえば 0 が −4e-17 になる）で被覆の判定が変わりうる。そのため、#274 の「歪み 0・単位写像で I-1 と画素一致」は、出力格子から文書 mm への段を通さず、画素空間の写像を render の関数へ直接渡して確かめる。内部パラメータは fx = fy = 1、cx = cy = 0 にする（(u − cx) / fx × fx + cx が丸めなしで u に戻る）。

## 4. EXIF Orientation を無視する

- 校正ツール、生成器、合成、評価器のどの段でも、画素は JPEG に保存された順のまま使い、EXIF の Orientation タグを適用しない。profile の `conventions.exifOrientation` は定数 `ignored`
- 理由: D810 は本体の傾きセンサーから Orientation を書く。下向きに据えたカメラでは値が撮影ごとに変わりうる（※推定。実機では未確認）。どこか 1 段でも適用すると、同じリグなのに撮影ごとに画素の枠が回り、固定の profile が合わなくなる
- 今の製品の復号（WIC の `IWICBitmapFrameDecode`）は Orientation を適用しないので、変更は要らない。気をつけるのは開発ツール側で、OpenCV の `cv::imread` は既定で Orientation を適用する。校正ツール（#277）と評価器（#275）は `cv::IMREAD_IGNORE_ORIENTATION` を付ける
- 出力 JPEG には Orientation を書かない。読む側は 1 とみなすので、#42 の候補の「EXIF orientation 1」と同じ扱いになる

## 5. レンズモデル

Brown–Conrady の 5 係数（放射 k1・k2・k3、接線 p1・p2）を使う。式は OpenCV の `projectPoints` と同じ。

```text
r2     = x*x + y*y
radial = 1 + k1*r2 + k2*r2*r2 + k3*r2*r2*r2
xd     = x*radial + 2*p1*x*y + p2*(r2 + 2*x*x)
yd     = y*radial + p1*(r2 + 2*y*y) + 2*p2*x*y
```

(x, y) は歪みのない正規化座標、(xd, yd) は歪んだ正規化座標。この向き（歪みのない → 歪んだ）は逆写像の合成でそのまま使う向きなので、反復計算が要らない。

- 係数は名前つきの欄（`k1`、`k2`、`k3`、`p1`、`p2`）で持ち、並び順に意味を持たせない。OpenCV の `distCoeffs` は [k1, k2, p1, p2, k3] の順なので、校正ツールは名前で写す（k3 は 5 番目）
- 接線の係数も必須。欄が無いときに 0 とみなすと、製品コードに係数の既定値を置くことになる。接線を使わない校正は 0 を明示して書く
- `distortion.model` は定数 `brown-conrady-k1k2k3-p1p2`。k4〜k6（有理式）や魚眼は 2.0.0 では受け付けない。歪みの強いレンズに替えるときは model を足す（schema の minor 版）。拡張点はここだけにし、ほかの欄は拡張を見込まない
- 内部パラメータは fx・fy・cx・cy（生 pixel）。skew は 0 で固定し（OpenCV の既定と同じ）、欄を持たない
- 実行時の検査（#274）: 出力領域が使う半径の範囲で、放射の写像 r → r·radial(r) が単調に増えること（1 + 3·k1·r² + 5·k2·r⁴ + 7·k3·r⁶ > 0）。単調でないと 2 つの文書点が同じ生 pixel に写り、像が折り返す。調整する閾値ではなく数学上の条件なので、製品コードに数値の既定値を足すことにはならない

## 6. 文書→画像の写像と行列の向き

- `projection.documentToImage` は 3×3 の射影変換 H。行の順に入れ子の配列で書く（行優先）。列ベクトルの約束で [u', v', w']ᵀ = H [X, Y, 1]ᵀ、(u, v) = (u'/w', v'/w')。(X, Y) は文書の mm、(u, v) は歪みのない画像座標
- 向きは「文書 → 画像」。平面チャートの校正では H = K [r1 r2 t]（K は内部パラメータ、r1・r2・t は mm 単位の外部パラメータ）で、校正の結果をそのまま書ける。出力画素ごとの計算もこの向きなので、合成時に逆行列を取らない
- 正規化: `H[2][2]` = 1（schema で固定）。射影変換は定数倍しても同じ写像なので、正規化しないと同じ写像で fingerprint が変わる。`H[2][2]` は文書の原点での奥行きにあたり、カメラの前にある原点なら正なので 1 に割り戻せる
- 実行時の検査: 出力領域の四隅で `w' = H[2][0]*X + H[2][1]*Y + 1 > 0`。w' は X・Y の一次式なので、四隅で正なら領域全体で正（v1 の `ValidateProjectiveDomain` と同じ考え方）
- v1 との違い: v1 の `calibration.homography`（C++ の `camera_b_to_camera_a`）は「CAM-B の生 pixel → CAM-A の生 pixel」の向きで、9 要素の平らな配列だった。合成時に逆行列を取っていた。要素の並び（h00, h01, h02, h10, …）は同じ行優先

## 7. 1 回の再サンプリング（逆写像の式）

出力画素 (i, j) ごとに、カメラ c（CAM-A、CAM-B）について次を計算する。

```text
p  = 25.4 / dpi                          # 合成 1 回につき 1 回だけ
X  = left + (i + 0.5) * p
Y  = top  + (j + 0.5) * p
u' = H[0][0]*X + H[0][1]*Y + H[0][2]
v' = H[1][0]*X + H[1][1]*Y + H[1][2]
w' = H[2][0]*X + H[2][1]*Y + 1
u  = u' / w';   v = v' / w'              # 歪みのない画像
x  = (u - cx) / fx;   y = (v - cy) / fy  # 正規化
(xd, yd) = distort(x, y)                 # 5 節の式
us = fx * xd + cx;   vs = fy * yd + cy   # 生 pixel
0 <= us < 7360 かつ 0 <= vs < 4912 のときだけ双一次で標本化（3.2 節）
```

- 両方のカメラが被覆する点は feather で混ぜる。feather の座標は文書座標（`documentPlane.cameraOrder` が左右なら X、上下なら Y）で、重なりの帯の端は出力格子上の各カメラの被覆から決める。画像の枠を歪みの逆変換（反復）で文書へ写す方法はとらない
- どちらのカメラも被覆しない出力画素があれば失敗にする（v1 と同じく、黙って埋めない）
- 撮影ごとの上限つき補正（ADR-0021、#46）は、補正するカメラの (X, Y) に文書座標での相似変換（平行移動・回転・倍率）を先にかけてから上の式に入れる。式は 1 本のままで、再サンプリングは増えない。自由な射影変換は推定しない
- 露出と色の補正（#46）は標本化した値に掛けるだけで、幾何の段を増やさない

## 8. 出力ラスタ（A0 の mm・DPI・向き）

- 向き: `documentPlane.orientation`（リグの決定）。用紙の寸法は向きから決まり、schema が A0 の値に固定する。製品コードは A0 の寸法を持たず、profile から読む
- DPI: `outputRaster.dpi`。1〜65535 の整数（JPEG の JFIF の密度欄が 16 bit のため）。値は所有者の決定（HG-0001・#42）
- 領域: `outputRaster.regionMm`（left, top, right, bottom）。用紙の内側に置く（0 ≤ left < right ≤ 用紙の幅。上下も同じ）。用紙全体なら横で (0, 0, 1189, 841)。原稿の押さえ具が写る縁を切るときは内側へ寄せる
- 画素数: `widthPixels`・`heightPixels` を profile に書き、実行時に 3.3 節の式と一致するかを確かめる。承認する人が画素数を目で確かめられ、DPI の打ち間違い（18 と 180 など）を実行時に捕まえられる
- 出力 JPEG: JFIF の密度を `dpi` にする（印刷すると A0 の実寸になる）。EXIF Orientation は書かない。圧縮後 64 MiB の上限（`kMaximumCompressedJpegBytes`）と、画素数の上限（1 辺 32,768、全体 2 億画素。`offline_stitcher.cpp` の `PixelCount`）は今のまま。schema の上限 65535 は JPEG の形式の上限で、engine の上限はそれより小さい。8426×5960 は約 5,000 万画素で、どちらの内側にも入る
- 用紙の外へ余白をつけた出力（領域が用紙より大きい）は 2.0.0 では受け付けない（所有者の確認事項）

## 9. schema 2.0.0

### 9.1 draft と approved

| 状態 | 校正の値（`documentPlane`、`cameras.*.projection`、`calibration`） | 所有者の決定（`outputRaster`、`correctionEnvelope`、`qualityContract`、`approval`） | 使う場所 |
| --- | --- | --- | --- |
| draft（雛形） | すべて null | すべて null | `samples/public/rig-profile.v2.draft.example.json` |
| draft（校正の結果） | すべて値あり（`rigMeasurements` の各値だけは null 可） | すべて null | #277 の出力、#273 の評価 exe の入力 |
| approved | すべて値あり | すべて値あり | 製品の合成（これだけを受け付ける） |

- 校正の値は全部そろうか全部 null。片方のカメラだけの写像や、校正の記録が無い写像は draft でも不可
- draft に所有者の決定の値が入っていたら不可（受入基準の「draft なのに値が入っている」）。#277 の受入基準は「出力 profile は常に draft で、provenance を持つ」なので、draft に入れてはいけない値を「所有者が決める値」と定義した。この読み方は所有者の確認事項に含める
- 評価 exe（#273）は出力ラスタを profile からではなく自分の入力から受け取り、評価用の manifest に記録する
- `profileId` は draft では null を許す。approved では必須

### 9.2 欄の一覧

この表の順が fingerprint（10 節）の行の順の正本で、schema の `properties` の順と同じ。型の列の「整数」「数」「文字列」は fingerprint の `i:`・`f:`・`s:` に対応する。

| JSON Pointer | 型 | draft | approved | 意味 |
| --- | --- | --- | --- | --- |
| `/schemaVersion` | 文字列 | `2.0.0` | `2.0.0` | schema の版 |
| `/status` | 文字列 | `draft` | `approved` | |
| `/profileId` | 文字列 | null 可 | 必須 | 内容ごとの識別子。版の欄は持たない |
| `/conventions/documentPlane` | 文字列 | 定数 | 定数 | `mm-origin-sheet-top-left-x-right-y-down` |
| `/conventions/cameraPixel` | 文字列 | 定数 | 定数 | `stored-order-sample-at-integer-index` |
| `/conventions/matrix` | 文字列 | 定数 | 定数 | `row-major-column-vector` |
| `/conventions/exifOrientation` | 文字列 | 定数 | 定数 | `ignored` |
| `/cameraModel/manufacturer`、`/model` | 文字列 | 定数 | 定数 | `Nikon`、`D810` |
| `/cameraModel/sensorWidthPixels`、`/sensorHeightPixels` | 整数 | 定数 | 定数 | 7360、4912 |
| `/documentPlane` | ブロック | null か値 | 値 | 用紙と向き |
| `/documentPlane/paper` | 文字列 | | | 定数 `ISO-216-A0` |
| `/documentPlane/orientation` | 文字列 | | | `landscape` / `portrait` |
| `/documentPlane/sheetWidthMm`、`/sheetHeightMm` | 数 | | | 向きに対応する A0 の寸法（schema が固定） |
| `/documentPlane/cameraOrder` | 文字列 | | | `CAM-A-left-CAM-B-right` / `CAM-A-top-CAM-B-bottom` |
| `/cameras/CAM-A/physicalIdentity` | null | null | null | 物理的な識別子を書かない |
| `/cameras/CAM-A/projection` | ブロック | null か値 | 値 | 適用する値 |
| `…/projection/intrinsics/fxPixels`、`fyPixels`、`cxPixels`、`cyPixels` | 数 | | | fx・fy は正 |
| `…/projection/distortion/model` | 文字列 | | | 定数 `brown-conrady-k1k2k3-p1p2` |
| `…/projection/distortion/k1`、`k2`、`k3`、`p1`、`p2` | 数 | | | 5 つとも必須 |
| `…/projection/documentToImage/0/0` 〜 `/2/2` | 数 | | | 行優先の 3×3、`/2/2` は 1 |
| `/cameras/CAM-B/…` | | | | CAM-A と同じ形 |
| `/calibration` | ブロック | null か値 | 値 | 校正の記録（合成には使わない） |
| `/calibration/method` | 文字列 | | | 定数 `chart-fiducial-planar` |
| `/calibration/chartId`、`/provenanceId` | 文字列 | | | 識別子 |
| `/calibration/measuredAt` | 文字列 | | | UTC、秒まで、`Z` |
| `/calibration/tool/toolId`、`/tool/version` | 文字列 | | | 校正ツールの識別 |
| `/calibration/inputSha256/CAM-A`、`/CAM-B` | 文字列 | | | 校正に使った画像の SHA-256（画像そのものは置かない） |
| `/calibration/fitResidualPixels/CAM-A/rms`、`/max`、`/CAM-B/…` | 数 | | | 基準点の再投影残差（カメラの pixel）。測った値で、閾値ではない |
| `/calibration/rigMeasurements/baselineMm`、`/overlapMm` | 数 | null 可 | 必須 | 手で測ったリグの値（#276 の実測項目） |
| `/calibration/rigMeasurements/cameraToDocumentMm/CAM-A`、`/CAM-B` | 数 | null 可 | 必須 | 同上 |
| `/calibration/rigMeasurements/lensFocalLengthMm/CAM-A`、`/CAM-B` | 数 | null 可 | 必須 | 同上 |
| `/outputRaster` | ブロック | null | 値 | 所有者の決定 |
| `/outputRaster/dpi` | 整数 | | | 1〜65535 |
| `/outputRaster/regionMm/left`、`top`、`right`、`bottom` | 数 | | | 用紙の内側、1 µm 刻み |
| `/outputRaster/widthPixels`、`/heightPixels` | 整数 | | | 3.3 節の式と一致 |
| `/correctionEnvelope` | ブロック | null | 値 | 所有者の決定（ADR-0021 の上限） |
| `/correctionEnvelope/minimumOverlapMm` | 数 | | | 重なりの下限（文書の mm） |
| `/correctionEnvelope/registrationErrorOutputPixels/targetMax`、`/autoCorrectionMax` | 数 | | | 出力の pixel |
| `/correctionEnvelope/rotationErrorDegrees/…`、`scaleDifferencePercent/…`、`exposureDifferenceEv/…`、`colorDeltaE/…` | 数 | | | v1 と同じ |
| `/qualityContract` | ブロック | null | 値 | 所有者の決定 |
| `/qualityContract/seamErrorOutputPixelsMax`、`registrationErrorOutputPixelsMax`、`colorDeltaEMax` | 数 | | | 出力の pixel・ΔE |
| `/approval` | ブロック | null | 値 | 承認の記録 |
| `/approval/decisionRef` | 文字列 | | | 所有者の決定の参照（Issue のコメントなど）。人名は書かない |
| `/approval/approvedAt`、`/validUntil` | 文字列 | | | UTC、秒まで、`Z` |

v1 にあった自由記述の `qualityContract.notes` は持たない。公開リポジトリで PC 名などが紛れ込む入口になり、機械が読む意味も無い。

### 9.3 schema が検査すること、実行時に検査すること

schema（`Test-Json`）が検査する:

- 形・型・必須・未知の欄の拒否・定数（版、規約、D810 の寸法、向きと A0 の寸法の組）
- draft と approved の区別（9.1 節の表）
- 係数の欠落（内部パラメータ 4 つと歪み係数 5 つは全部必須。3×3 以外の行列は不可）
- DPI は 1 以上の整数
- 出力領域が用紙の内側にあること（向きごとの上限）、1 µm 刻み
- `H[2][2]` = 1
- 時刻の書式（UTC、秒まで、`Z`）

JSON Schema は欄どうしの算術を書けないので、次は実行時（製品の validator、#274）に検査する。`scripts/Test-RigProfileV2Schema.ps1` の `Assert-RigProfileV2RuntimeContract` が参照実装で、単調性の検査だけは入っていない。

- status が approved、schemaVersion が 2.0.0
- left < right、top < bottom
- 画素数が 3.3 節の式と一致する（受入基準の「出力寸法と crop の矛盾」のうち算術の部分）
- DPI と画素数が整数の表記で書かれている（JSON Schema の integer は `180.0` も通す）
- 出力領域の四隅で w' > 0（両カメラ）
- 放射の写像の単調性（5 節）
- 補正上限の targetMax ≤ autoCorrectionMax
- measuredAt ≤ approvedAt ≤ 評価時刻 < validUntil
- 入力 JPEG が 7360×4912（v1 と同じ）

## 10. manifest の fingerprint の正規化規則

目的: StitchJob manifest（ADR-0026、`a0.stitch-job-manifest.v1`/`v2`）の `rigProfile.sha256` を、合成に使った profile に一意に結びつける。stitcher は自分が読んで適用した値から計算し、呼び出し側が渡した hash を使わない（ADR-0026 の実装時の補足）。manifest の `rigProfile.version` には今の C++ と同じく `schemaVersion`（`2.0.0`）を書く。規則は schemaVersion の major で決まり、2.x は本節の規則に従う。

正規化の手順:

1. 1 行目は `a0.rig-profile-fingerprint.v2`
2. 以降、9.2 節の表の順で、葉の値を 1 行に 1 つ、`<JSON Pointer>=<値>` の形で書く。カメラは CAM-A、CAM-B の順（ファイルの中の順によらない）。行列は行、列の添字の順
3. 値の書き方
   - 文字列: `s:` に続けてそのまま書く。schema の pattern と定数で、文字列は制御文字・改行・`=` を含まない ASCII に限られている
   - 整数（9.2 節の型が整数の欄）: `i:` と 10 進
   - 数（型が数の欄）: `f:` と、IEEE 754 binary64 のビット列を小文字の 16 進 16 桁で。JSON の 10 進表記を最近接偶数丸めで binary64 に直した値を使い、−0 は +0（`0000000000000000`）にする
   - null: `null`。ブロック全体が null のときは子の行を出さず、`/<ブロック>=null` の 1 行だけ書く
4. 各行は LF で終える（最終行も）
5. その byte 列の SHA-256 を小文字 16 進 64 桁で manifest に書く

入れないもの: 評価時刻。v1（`ProfileFingerprint`、`offline_stitcher.cpp` 750-781 行付近）は `assessedAt` を入れていたので、同じ profile でも評価のたびに hash が変わっていた。評価時刻は profile の値ではないので外す。

入れるもの: それ以外のすべての欄。approval の記録、閾値、校正の記録も入る。適用した値だけでなく profile 全体の同一性を表す。

数を 10 進の文字列にせずビット列で書く理由: C++ の `std::to_chars` と .NET の `double.ToString("R")` は最短表記の書き方（指数の表し方、大文字・小文字）が違い、同じ値から違う文字列ができる。ビット列ならどちらでも同じになり、JSON 側の表記の揺れ（`1` と `1.0`、`1e3` と `1000`）も同じ行になる。

例（雛形 draft の先頭と、数の書き方）:

```text
a0.rig-profile-fingerprint.v2
/schemaVersion=s:2.0.0
/status=s:draft
/profileId=null
/conventions/documentPlane=s:mm-origin-sheet-top-left-x-right-y-down
/conventions/cameraPixel=s:stored-order-sample-at-integer-index
/conventions/matrix=s:row-major-column-vector
/conventions/exifOrientation=s:ignored
/cameraModel/manufacturer=s:Nikon
/cameraModel/model=s:D810
/cameraModel/sensorWidthPixels=i:7360
/cameraModel/sensorHeightPixels=i:4912
/documentPlane=null
/cameras/CAM-A/physicalIdentity=null
/cameras/CAM-A/projection=null
…
```

値が入ったときの例: `/documentPlane/sheetWidthMm=f:4092940000000000`（1189.0）、`/cameras/CAM-A/projection/documentToImage/2/2=f:3ff0000000000000`（1.0）。

試験ベクトル（雛形 draft と試験用の approved の 2 つの fingerprint）は #274 で C++ と .NET の両方に置き、同じ値になることを確かめる。

## 11. C++ と .NET の対応表

今（v1）の列は main の実装、v2 の列は #274 で作る名前の案。

| v2 の JSON | v1 の JSON（1.1.0） | C++ 今 | .NET 今 | C++ 案（#274） | .NET 案（#274） |
| --- | --- | --- | --- | --- | --- |
| `schemaVersion` | `schemaVersion` | `ProfileTrust::schema_version`、`kSupportedProfileSchemaVersion`（`setup_assessment.hpp`） | `DualCameraRigProfile.SchemaVersion`（`Validate` が `1.1.0` を要求） | `RigProfileV2::schema_version`、対応版 `2.0.0` | `RigProfileV2.SchemaVersion` |
| `status` | `status` | `ProfileTrust::status`（`ProfileStatus`） | `Status`（`DualCameraProfileStatus`） | 同じ列挙 | 同じ列挙 |
| `profileId` | 無し | `FixedRigStitchProfile::profile_id` | `ProfileId` | `profile_id` | `ProfileId` |
| 無し | 無し | 無し | `Version`（`"1"`） | 無し（新しい内容は新しい profileId） | 無し |
| `conventions.*` | 無し | 無し | 無し | 定数として検査だけ | 同左 |
| `cameraModel.*` | `cameraModel.*` | `expected_input_width`・`expected_input_height` | `ExpectedInputWidth`・`ExpectedInputHeight` | `sensor_width_pixels`・`sensor_height_pixels` | `SensorWidthPixels`・`SensorHeightPixels` |
| `documentPlane.orientation` | `layout.orientation` | 無し | 無し | `DocumentPlane::orientation` | `DocumentPlane.Orientation` |
| `documentPlane.sheetWidthMm`・`sheetHeightMm` | 無し | 無し | 無し | `sheet_width_mm`・`sheet_height_mm` | `SheetWidthMm`・`SheetHeightMm` |
| `documentPlane.cameraOrder` | `layout.cameraOrder` | `StitchLayout`（`layout`） | `Layout`（`"camera-a-left-camera-b-right"` 等） | `StitchLayout` を流用 | `Layout` |
| `cameras.*.physicalIdentity` | `cameraSlots[].physicalIdentity` | 無し | 無し（`CameraAliases` は別名の並びだけ） | null の検査だけ | 同左 |
| 無し | `cameraSlots[].rotationDegrees` | 無し | 無し | 無し（写像に含まれる） | 無し |
| `cameras.*.projection.intrinsics` | 無し | 無し | 無し | `CameraProjection::fx`・`fy`・`cx`・`cy` | `CameraProjection.FxPixels` 等 |
| `cameras.*.projection.distortion` | 無し | 無し | 無し | `LensDistortion::k1`〜`p2` | `LensDistortion.K1`〜`P2` |
| `cameras.*.projection.documentToImage` | `calibration.homography`（向きと意味が違う） | `camera_b_to_camera_a`（`std::array<double, 9>`、CAM-B → CAM-A） | `CameraBToCameraA`（9 要素） | `document_to_image`（`std::array<double, 9>`、行優先） | `DocumentToImage`（9 要素、行優先） |
| 無し | `layout.overlapPercent` | 無し | 無し | 無し（写像から求める。下限は `minimumOverlapMm`） | 無し |
| `outputRaster.regionMm` | `layout.crop.*Percent`（%） | `StitchCropPixels`（px。v1 の JSON の % と単位が合っていない） | `Crop`（int ×4、px） | `region_um`（µm の整数に直して持つ） | `RegionMm` |
| `outputRaster.dpi` | `optics.targetDpi` | 無し | 無し | `OutputRaster::dpi` | `OutputRaster.Dpi` |
| `outputRaster.widthPixels`・`heightPixels` | 無し（canvas − crop で決まる） | 出力は canvas から導出 | 無し | 検査に使う | 同左 |
| `calibration.measuredAt` | `calibration.measuredAt` | `ProfileTrust::measured_at` | `MeasuredAtUtc` | 同じ | 同じ |
| `calibration.provenanceId` | `calibration.provenanceId` | `ProfileTrust::provenance` | `Provenance` | 同じ | 同じ |
| `calibration` の残りの欄 | `optics.focalLengthMm`・`optics.cameraToOriginalMm` が近い | 無し | 無し | 記録として読む（合成には使わない、fingerprint には入る） | 同左 |
| `approval.validUntil` | `calibration.validUntil` | `ProfileTrust::valid_until` | `ValidUntilUtc` | 同じ | 同じ |
| `approval.approvedAt`・`decisionRef` | 無し | 無し | 無し（`HardwareDualCaptureProfile.ApprovedAtUtc` は撮影 profile の欄で別物） | `approved_at`・`decision_ref` | `ApprovedAtUtc`・`DecisionRef` |
| （評価時刻） | — | `ProfileTrust::assessed_at`（fingerprint に入っていた） | `AssessedAtUtc` | 入力のまま。fingerprint から外す | 同左 |
| `correctionEnvelope.*` | `correctionEnvelope.*`（`registrationErrorPixels`、`minimumOverlapPercent`） | `SetupAssessmentInput`（`registration_pixels`、`minimum_overlap_percent` 等） | 無し | `minimum_overlap_mm`、`registration_output_pixels` 等 | 同名の PascalCase |
| `qualityContract.*` | `qualityContract.*`（`notes` あり） | 無し | 無し | 読むだけ | 同左 |
| fingerprint | — | `ProfileFingerprint`（`offline_stitcher.cpp`） | 無し（C++ が計算） | 10 節の規則 | 同じ規則で計算して照合 |

受け渡しの方法も変える。v1 は .NET が値を 1 つずつ CLI 引数にし、C++ が `ParseProfile` で読み直していた。v2 では .NET が approved の profile のファイルの場所を渡し、C++ がファイルを読んで検証・fingerprint・合成まで行う。値の写し替えが .NET と C++ の 2 か所に分かれていると、片方だけ欄を足したときにずれる。fingerprint も C++ が自分で読んで適用した値から作るので、適用した値との対応が切れない。.NET は同じファイルを表示と setup assessment のために読み、同じ規則で fingerprint を計算して manifest と照合する。M2Adapter の引数が変わるので、変更は #274 で行う（#268 は引数を変えない）。

## 12. v1.1.0 の扱いと移行

- 1.1.0（`docs/schemas/rig-profile.schema.json`、`samples/public/rig-profile.draft.example.json`）は試験専用として残す。既存の試験（ctest の `m2_pregate_assets`、C++ の 16×8・2048×1024・非整数の平行移動の fixture、.NET の `ApprovedSynthetic`）はそのまま通す
- 1.1.0 では実リグの profile を作らず、承認もしない
- 移行はストラングラー方式で進める
  1. #271（本書）: 2.0.0 の schema と試験を足す。製品は変えない
  2. #274: 製品の合成に 2.0.0 の経路を足す。1.1.0 の経路は既存の試験のために残す
  3. 2.0.0 の approved が初めて出た後（#45）: 製品の adapter から 1.1.0 を受け付ける経路を外し、1.1.0 は試験のコードからだけ使う。ファイル名の入れ替え（2.0.0 を `rig-profile.schema.json` にするか）はこの時に決める
- データの移行は無い（実リグの v1 profile が無い）。v1 から v2 への変換の道具も作らない。v1 の値にはレンズ係数が無く、CAM-A 基準の行列から文書への写像は作れない
- 互換: 2.0.0 の reader は 1.1.0 を拒否し、1.1.0 の reader は 2.0.0 を拒否する（どちらも試験で確かめている）
- ロールバック: #274 の変更を戻す。製品で使っている approved の v1 profile は無いので、戻しても失うものは無い。戻す条件は、2.0.0 の合成が同じ入力で byte 一致しないこと、または approved を経ずに合成できる経路が見つかること

## 13. 実装ガイドライン

**#274（C++ の合成）**:

- 型の名前は `RigProfileV2`、`CameraProjection`、`LensDistortion`、`OutputRaster` とし、欄は JSON の名前を snake_case にする
- JSON の reader は、未知の欄、重複したキー、型の違い、整数の欄の小数表記を拒否する（manifest の reader と同じ厳しさ）
- 9.3 節の実行時の検査を、合成の前に全部行う。数値の既定値を持たない（欄が無ければ失敗）
- 7 節の式は 1 か所（#268 の `a0_m2_render`）に置き、評価 exe（#273）と製品が同じものを呼ぶ。独立した評価器（#275）は呼ばない
- 3.3 節の計算順を固定する。並列化しても行ごとに独立に計算する（#274 の受入基準）
- fingerprint は 10 節の規則で計算し、C++ と .NET に同じ試験ベクトルを置く
- engine の版（`kOfflineStitcherEngineVersion`）を上げる。出力 JPEG の JFIF の密度を dpi にし、Orientation を書かない

**#277（校正ツール）**:

- 出力は draft（校正の値あり、所有者の決定は null）。内容ごとに新しい `profileId` を付ける
- OpenCV を使うなら、`cv::IMREAD_IGNORE_ORIENTATION` で読み、`distCoeffs` を名前で写し、H = K [r1 r2 t] を mm で作って `H[2][2]` で割る
- チャートの座標から文書座標への対応はツールが持ち、`calibration.chartId` に記録する
- 残差は `fitResidualPixels` に書くだけで、閾値で判定しない

**#273（評価 exe）**: 校正の値がある draft を受け付け、出力ラスタは自分の入力から受け取る。評価用の manifest に、draft の fingerprint と使った出力ラスタを記録する。

**#269（生成器）**: 正解 JSON のカメラのパラメータを本書の規約（文書 → 画像、行優先、`H[2][2]` = 1、正規化座標での Brown–Conrady、生 pixel の約束）で書けば、#274 の検算でそのまま profile に写せる。生成器の歪みは放射 k1〜k3 だけなので、p1 = p2 = 0 を明示して写す。

.NET: `DualCameraRigProfile` の v2 版は profile のファイルを読み、表示と setup assessment に使う。合成へは値ではなくファイルの場所を渡す（11 節）。

## 14. 守るべき不変条件

1. 製品の合成は、status が approved で schemaVersion が対応版の profile だけを受け付ける。校正の値がある draft も拒否する（評価は別 exe・別 manifest、#273）
2. 撮影ごとに射影変換を推定しない（ADR-0021）。写像は profile の値だけを使う。撮影ごとの補正は承認された上限の内側の相似変換で、上限を超えたら失敗にする
3. 製品コードに数値の既定値（閾値、レンズ係数、DPI、用紙の寸法）を置かない。欄が無ければ失敗にする。25.4（mm/inch）は単位の定義で、既定値ではない
4. 再サンプリングは 1 回。中間画像を作らない
5. EXIF Orientation をどの段でも適用しない
6. profile を実行時に変更・学習しない。内容を変えたら新しい profileId にする
7. fingerprint は stitcher が自分で読んで適用した値から計算する
8. draft は所有者の決定の値を持たない
9. profile に物理的な識別子（serial など）と自由記述の欄を置かない

## 15. 代替案と不採用の理由

| 案 | 不採用の理由 |
| --- | --- |
| CAM-A の生 pixel 基準のまま、両カメラにレンズ係数を足す（v1 の拡張） | 出力の格子が CAM-A の画素に縛られる。A0 の mm と DPI で出力を決めるには、回転と拡縮の段（2 回目の再サンプリング）を後ろに足すことになる。それを 1 本の式にまとめると、CAM-A を特別扱いした文書平面基準と同じになり、v1 との互換も残らない（crop も % と px のまま） |
| 写像を「画像 → 文書」の向きで持つ | 出力画素ごとの計算は文書 → 生 pixel の向き。逆向きで持つと、歪みの逆（反復計算）か行列の逆が要る |
| 接線の係数を任意にし、無ければ 0 とする | 係数の既定値 0 を製品に置くことになる |
| 係数を OpenCV と同じ順の配列で持つ | k3 と p1 の取り違えが型で捕まらない。名前つきの欄なら schema が欠落も捕まえる |
| 校正の結果を 3 つ目の状態（candidate など）で表す | #272・#273・#277 が「draft の profile」と書いており、並行作業の文書と語がずれる。draft の中で「校正の値」と「所有者の決定」を分ければ同じ安全性が得られる |
| draft の値をすべて null に限る（v1 と同じ） | #277 の受入基準（draft に校正の値と provenance を持たせる）と矛盾する |
| 向きを出力ラスタ（所有者の HG-0001 の決定）に置く | 文書座標が向きに依存するので、校正の時点で向きが決まっていないと写像を書けない。向きはリグの配置（HG-0002）で決まる |
| 出力の画素数を書かず、領域と DPI から導くだけにする | 承認する人が画素数を確かめられない。書いておけば打ち間違いを実行時に捕まえられる |
| fingerprint を profile のファイルの byte 列の SHA-256 にする | 空白や欄の順が変わるだけで hash が変わる。.NET が書き直すと別物になる |
| fingerprint を RFC 8785（JCS）にする | 数を ECMAScript の規則の 10 進で書く必要があり、C++ の `std::to_chars` の表記と合わせる実装が要る。ビット列なら短く書けて揺れない |
| 生 pixel の被覆を [−0.5, w−0.5) に揃える | #86 の修正、#268 の fixture、#274 の検算の結果が変わる。得られる差は端の半画素だけ |

## 16. リスク

- 校正の精度: 平面チャートからの推定では k3 と fx の相関が強く、残差が小さくてもフレームの端（重なりの帯）で誤差が出やすい（※経験則）。#277 で検証用の基準点を分けて測る（#42 の calibration residual）
- 視差: 文書を 1 枚の平面とみなしている。原稿が浮くと重なりの帯でずれ、profile では直せない（#278 の見込みで 0.2 mm の浮きが約 0.4 px、※推定）
- pixel の取り違え: 出力の pixel とカメラの pixel が混ざると閾値の意味が変わる。欄名で分けたが、#42 の「p95 0.5 px」がどちらの pixel かは所有者の確認事項
- 並行作業とのずれ: #269 の正解 JSON の形、#272 の ADR 草案の `profileStatus=draft` の語と、本書の draft の定義がずれる可能性。本書の draft は「校正の値はあってよく、所有者の決定は無い」で、#273 の評価 exe が受け付ける draft と同じものを指す
- 決定性: 25.4 / dpi の丸めは同じバイナリなら再現するが、3.3 節の計算順を変えると出力が変わる
- JSON Schema の限界: 算術の関係（画素数、領域の順、時刻の順、分母の符号）は実行時に任せている。製品の validator が参照実装と同じ規則を持たないと、schema を通った不正な profile が通る。#274 で同じ試験ケースを C++ と .NET に置く

## 17. 所有者の確認が要る点

ADR-0034 の「所有者の確認が要る点」と同じ。

## 18. 試験

`scripts/Test-RigProfileV2Schema.ps1`（ctest `m2_rig_profile_v2_schema`）。`Test-Json` で schema を正例と負例へ当て、算術の規則は参照実装の `Assert-RigProfileV2RuntimeContract` で確かめる。実行時に任せた例は schema を通ることも確かめ、規則が層の間で黙って移ったら気づけるようにしている。

受入基準の負例 4 種との対応:

| 受入基準の負例 | schema で拒否する例 | 実行時に拒否する例 |
| --- | --- | --- |
| 係数の欠落 | approved の k3 の欠落、p2 が null、校正済み draft の k1 の欠落、cy の欠落、2 行の行列 | — |
| DPI 0 | 0、180.5、null | `180.0` と書いた DPI |
| 出力寸法と crop の矛盾 | 横の用紙より高い領域、縦の用紙より広い領域、用紙の端から始まる領域、向きと用紙の寸法の不一致、幅 0、1 µm 未満の端 | 画素数が領域と DPI に合わない（1 画素足りない・多い）、左右が逆の領域 |
| draft なのに値が入っている | 雛形 draft に出力ラスタ、校正済み draft に出力ラスタ・承認の記録・補正上限・品質の閾値、片方のカメラだけの写像、校正の記録が無い写像 | — |

ほかに、版の取り違え（1.1.0 と 2.0.0 の相互拒否）、approved の null のブロック、物理的な識別子、`H[2][2]` ≠ 1、3 台目のカメラ、未知の欄、時刻の書式、分母が領域の中で 0 を跨ぐ写像、補正上限の逆転、承認と期限の順序を試験している。
