# 正解つき合成ペア生成器

Issue #269（親: #278 段階 0）。実写なしで、校正・レンズ補正・評価を検証するための画像と正解値を作る。製品の合成（`src/m2/offline_stitcher.cpp`）には依存しない。実機にもカメラにも触らない。

座標とレンズの約束は ADR-0034（`docs/DECISIONS.md`）と `docs/design/rig-profile-v2.md`、schema `docs/schemas/rig-profile.v2.schema.json` に従う。形式は `/2` で、ADR-0034 より前に書いた `/1`（画素 `(i, j)` の標本点が `(i + 0.5, j + 0.5)`、`homography` と `lens` の欄）は読み込まずに拒否する。

## 何を作るか

自社チャート（格子、リング基準点、斜めエッジ、細線、色パッチ、グレーパッチ）をコードで描き、カメラごとのモデルで投影して、`7360x4912` の JPEG と正解 JSON を出す。

```
<出力フォルダ>/CAM-A/original.jpg
<出力フォルダ>/CAM-B/original.jpg
<出力フォルダ>/ground-truth.json
```

`original.jpg` の名前と置き場所は製品の canonical 原本と同じ形にしてあり、`M2Adapter validate-canonical-jpeg --width 7360 --height 4912` を通る。生成した画像はコミットしない（`.gitignore` の画像規則と `scripts/Test-ReplayFixtureLeak.ps1` の対象）。試験は一時フォルダへ作って消す。

## 実行

```powershell
A0CameraStitcher.SyntheticPairGenerator.exe --spec tests/fixtures/synthetic-pair/two-camera-bodies-rotated.spec.json --output <空のフォルダ> [--threads <n>]
```

出力フォルダに `ground-truth.json` か `original.jpg` が既にあると、上書きせず exit 2 で止まる。標準出力に各ファイルの SHA-256、`elapsedMilliseconds`、`peakWorkingSetBytes` が `key=value` で出る。

C++ から使う場合は `a0_m2_synthetic_pair`（`src/m2/include/a0/m2/synthetic_pair.hpp`）をリンクする。`ParsePairSpec` → `GeneratePair`。画像をファイルにせず取り出すなら `CameraRenderer::RenderRows`（24 bit BGR）。`ValidatePairSpec` は `ParsePairSpec`・`CameraRenderer`・`GeneratePair` が自分で呼ぶので、C++ で組んだ `PairSpec`（`supersample = 0` など）も同じ規則で拒否される。

## 公開の順序

`GeneratePair` は全ファイルを、行き先の隣に `<名前>.partial` として書き終えてから、画像 → `ground-truth.json` の順にまとめて名前を付ける。途中で失敗したら、名前を付け終えたファイルを消し、`.partial` も消し、この実行で作ったフォルダ（空のもの）も消す。`ground-truth.json` が見えていれば画像は揃っている（正解 JSON が完成の目印）。`.partial` は `.gitignore` の対象。

## 入力（spec）

数値の既定値は持たない。欠けたキーも、知らないキーも、範囲外の値も `std::invalid_argument` で拒否する。例は `tests/fixtures/synthetic-pair/two-camera-bodies-rotated.spec.json`（カメラ本体を 90 度回して据えた 2 台。文書の向きは横置きのまま）。

| 項目 | 内容 |
|---|---|
| `schema` | `a0.m2.synthetic-pair-spec/2` |
| `seed` | 64 bit 整数。ノイズだけが使う（ノイズを 0 にすれば画像は seed に依らない） |
| `image` | `width_px`、`height_px`、`supersample`（1 画素あたり supersample² 点で平均する） |
| `jpeg` | `quality`（0 より大きく 1 以下）、`chroma_subsampling`（`444`/`422`/`420`/`440`） |
| `chart` | 文書の寸法（mm）、余白、格子の間隔と線幅、基準点の間隔と半径、斜めエッジの傾き（tan）、細線の基準幅 |
| `cameras[]` | 下記 |

カメラごと:

- `alias`: `CAM-A` のような名前。出力フォルダ名になる。英数字・`-`・`_`（先頭は英数字）、32 文字まで。`CON`・`NUL`・`COM1` など Windows の予約名は拒否し、大文字小文字だけが違う名前は重複として拒否する。
- `projection`: rig profile v2 の `cameras.*.projection` と同じ名前・同じ形。
  - `intrinsics`: `fxPixels`、`fyPixels`、`cxPixels`、`cyPixels`。
  - `distortion`: `model`（定数 `brown-conrady-k1k2k3-p1p2`）、`k1`、`k2`、`k3`、`p1`、`p2`。5 係数とも必須で、接線歪みを使わないなら 0 を書く。正規化は `x = (u - cx) / fx`、`y = (v - cy) / fy`。`r2 = x² + y²`、`xd = x·radial + 2·p1·x·y + p2·(r2 + 2x²)`、`yd = y·radial + p1·(r2 + 2y²) + 2·p2·x·y`、`radial = 1 + k1·r2 + k2·r2² + k3·r2³`（設計文書 5 節と同じ）。
  - `documentToImage`（3×3 を入れ子の配列で書く、行優先、列ベクトル）または `placement`（どちらか一方だけ）。文書（mm）から歪みのない画素への射影変換で、`[2][2]` は 1 だけ受け付ける。`placement` は入力の便宜で、`center_mm`（標本格子の中心 `((W-1)/2, (H-1)/2)` に写る文書上の点）、`pixels_per_mm`、`quarter_turns`（0〜3）から作る。90 度刻みでない回転やあおりは `documentToImage` を直接書く。
- `exposure_gain`、`white_balance_gain`（R, G, B）、`vignette`（`1 + v1 ρ² + v2 ρ⁴` の `v1`, `v2`。`ρ² = ((i - cx)/fx)² + ((j - cy)/fy)²`）。
- `noise`: `read_sigma`、`shot_sigma`。画素値（満点 1.0）に対する標準偏差 `read_sigma + shot_sigma × √信号`。

座標は、文書が左上原点・x 右・y 下（mm）。カメラの画素は保存された順で、画素 `(i, j)` の標本点が `(i, j)`（OpenCV と同じ。ADR-0034 決定 3）。1 画素は `[i - 0.5, i + 0.5)` を覆い、部分標本は `i - 0.5 + (s + 0.5)/S`。EXIF Orientation は扱わない。

`ValidatePairSpec` が拒否するもの（カメラごと）: `documentToImage` が特異、`[2][2]` が 1 でない、チャートの四隅の `w' <= 0`、画像の四隅（と画像全体の 33×33 の格子）を逆写像した点が `w' <= 0`、画像の四隅に届くまでの半径で放射の写像 `r → r·radial(r)` が単調でない（例: `k2 = k3 = 0`、`k1 = -0.2` で隅に逆像が無い）、歪みの逆（2 次元 Newton）が画像上で 1e-7 px 以内に収束しない。

## 出力（ground-truth.json）

```json
{
"schema":"a0.m2.synthetic-pair-ground-truth/2",
"generator_version":"a0.m2.synthetic-pair-generator/2",
"seed":269269,
"conventions":{"documentPlane":"mm-origin-sheet-top-left-x-right-y-down","cameraPixel":"stored-order-sample-at-integer-index","matrix":"row-major-column-vector","exifOrientation":"ignored"},
"spec":{ ...解決済みの入力。documentToImage は必ず入る。ParsePairSpec にそのまま渡せる... },
"output_files":[{"alias":"CAM-A","path":"CAM-A/original.jpg","sha256":"2e5a...1f03","size_bytes":11327677}],
"reference_points":[
{"id":"F-010-008","document_mm":[520,420],"image_px":{"CAM-A":[3676.38,1040.63],"CAM-B":null}}
],
"patterns":[{"kind":"slanted-edge-dark-on-light","cell":[3,0],"center_mm":[195,45],"half_size_mm":12.5}]
}
```

- `conventions`: rig profile v2 の `conventions` と同じ 4 つの定数。
- `spec.cameras[].projection`: そのまま draft profile の `cameras.*.projection` に写せる（試験が写して schema に当てている）。
- `reference_points`: リング基準点の中心。`document_mm` が文書上の位置、`image_px` が各カメラ画像上の真の位置（歪み込み）。被覆の判定は `0 <= u < width` かつ `0 <= v < height`（rig profile v2 の約束）で、外か、カメラの後ろ（`w' <= 0`）か、放射の写像が折り返す半径より外なら `null`。折り返しの外の点は、式どおりに評価すると画像の中に入ることがあるが、そこには写らない。
- `patterns`: 斜めエッジ・色・グレー・細線のブロックの位置（評価器が測る対象を探す手がかり）。
- `spec` には入力で渡した `placement` ではなく、解決済みの `documentToImage` が入る。

## 決定性

同じ spec から、スレッド数に関わらず同じバイト列が出る。理由は 3 つ。画素は互いに独立した関数で、ノイズは seed と画素番号から決まるカウンタ型の乱数（SplitMix64）。画素の計算は四則演算・`floor`・`sqrt` だけで、`sin`/`pow` などの数学関数を使わない（歪みの逆の Newton 反復も四則演算だけ）。MSVC は `/fp:precise`、GCC/Clang は `-ffp-contract=off` で融合積和を避ける。

保証の範囲は「同じ Windows の JPEG エンコーダ（WIC）で同じバイト列」まで。WIC の版が変わるとエンコード結果が変わりうる（※未検証）。その場合も画素値（エンコード前）は変わらない。バイト一致が必要なら、同じ OS 版の上で比べる。

## 後続 Issue からの使い方

- **#270（実寸の時間・メモリ・決定性の計測）**: 固定 spec で 2 回生成して SHA-256 を比べる。生成器自体の時間とメモリは標準出力の `elapsedMilliseconds`、`peakWorkingSetBytes`。製品の合成に渡す入力は `<出力>/CAM-A/original.jpg` と `CAM-B/original.jpg`。
- **#274（レンズ補正＋文書への写像）**: spec の `projection` が正解（profile にそのまま写せる）。補正後の画像上の基準点は、`document_mm` を目標の写像で送った位置と比べる。レンズなし（`k1`〜`p2` = 0）の spec で幾何だけを先に確かめられる。
- **#275（独立した評価器）**: `reference_points` の `image_px` を正解として、合成画像から検出した基準点の位置との差を測る。`patterns` が斜めエッジ・細線・色・グレーの位置を与える。ノイズ・露出・周辺減光は `noise`、`exposure_gain`、`vignette` を変えて作り分ける。
- **#277（チャートからの draft キャリブレーション）**: 既知の `projection` を持つペアを作り、推定値を正解と比べる。基準点だけを使う検出が通るかを、歪みなし → 歪みあり → ノイズありの順に確かめられる。

重なりを変えたいときは、`placement.center_mm` の 2 台の間隔（baseline）か `pixels_per_mm` を変える。fixture は baseline 541 mm、`pixels_per_mm` 7.27 で、文書中央の水平線上の重なりが約 142 mm（#42 の v0.1 候補の 141 mm を参考にした値で、承認値ではない）。

## 限界

- 文書は完全な平面で、視差・紙の浮き・ピントぼけ・順次撮影の時間差・実レンズの MTF は入っていない。ここで合格しても実写の品質合格にはならない。
- 色は 0〜1 の値を直接 8 bit にする（sRGB のガンマ変換はしない）。色の再現を測る用途には向かない。
- 歪みは Brown–Conrady の 5 係数（k1〜k3、p1、p2）まで。有理式や魚眼は無い。
- 歪んだリングの重心は、中心の像とは一致しない（レンズがリングを曲げ、画素面積が場所で変わる）。リングを順写像で密に送って重心を予測する試験では、fixture の実寸で中心の像から約 0.04 カメラ px（主点から約 3,500 px の位置）ずれた。Issue の見積もり（隅で最大約 0.17 px）とは数値が合っていない（※要確認。測れたのは窓が画像に収まるリングまでで、主点から約 3,500 px が上限）。#275・#277 の誤差予算では、基準点の位置を「リングの重心」で測る限りこの分が入る。
- 正対した 1 視点だけでは fx が決まらない（平面校正の縮退。fx と撮影距離が分離できず、画像には比 fx/Z だけが残る）。#277 には傾いた視点が要る。生成器は `documentToImage` に任意の射影変換を書けるので、`H = K [r1 r2 t]`（K は内部パラメータ、r1・r2 は回転行列の第 1・第 2 列、t は mm の平行移動を `H[2][2]` で割る）で傾いた視点を作れる。試験の傾いた視点（25 度のあおり、fx ≠ fy、p1・p2 あり）が組み立て方の例。

## 試験

`tests/synthetic_pair_tests.cpp`（ctest 名 `m2_synthetic_pair_contracts`）。

- JSON（入れ子 32 段の境界）と spec の検証（`/1` の拒否、欄の欠落、`documentToImage` の形、チャートの四隅・画像の四隅の `w'`、樽型で隅に逆像が無い spec、予約名、C++ API の `supersample = 0`）。
- 正解 JSON と独立実装（本書の式を別に書いた版。MSVC の `long double` は `double` と同じ 53 bit なので精度の検査ではなく、実装の独立性の検査）の投影の差が 1e-6 px 以内、`conventions` の 4 定数、被覆判定 `0 <= u < width`、折り返しの外の点が `null`。
- 画像全体（四隅を含む 65×65 の格子）で、画素 → 文書 → 画素の往復が 1e-6 px 以内（fixture、小さい spec、回転・射影・接線・fx≠fy のカメラ、傾いた視点）。
- `/1` からの移行: fixture の `image_px` が `/1` の値 −0.5 と 1e-9 px 以内で一致。
- 描画したリングの重心と、リングを順写像で密に送った重心の予測の比較（小さい spec 4 通りと実寸 fixture の数本の行）。今の許容は推定器の揺れで決まっている（画素位相で約 0.09〜0.18 px 動く）。
- 正解から組んだ draft profile を `rig-profile.v2.schema.json` に当てる（`Test-Json`）。欄を壊した負例が拒否されることも確かめる。
- 行の分割・スレッド数・seed による出力の違い、公開の原子性（失敗時に画像・`.partial` が残らない）、2 回生成での SHA-256 一致、`M2Adapter validate-canonical-jpeg` の合格。

製品の合成への依存は `CMakeLists.txt` の `a0_assert_no_product_stitch_dependency` が configure 時に検査する。
