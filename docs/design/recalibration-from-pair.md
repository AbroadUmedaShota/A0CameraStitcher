# 撮影の 1 組から校正を取り直す（#434）

カメラの間隔を変えた時に、レンズの校正を最初からやり直さず、1 組の撮影だけで CAM-B の写像を取り直して新しい条件の改訂を足す。この文書は計算の部分（`src/m2/fixed_composite_recalibration.*`）の設計と決まりを書く。画面と確定の書き込みは別の層の仕事で、ここでは扱わない。書式は `fixed-composite-registration-formats.md`、profile の欄は `rig-profile-v2.md` の `/calibration` の注、改訂の種類は `fixed-composite-registration.md` の 11.19.3。

## 1. 何を変え、何を変えないか

- 変えない: レンズ（K と 5 係数）、CAM-A の写像、撮影の経路、方針ファイルの値、`seam_gate.*`、前の改訂と前の profile（消さない・上書きしない）
- 変える: CAM-B の「文書 → 画像」の写像だけ。両台で撮った 1 組から B の姿勢 p-hat（6 自由度）を求め、`PoseModel(B).CameraHomography(p-hat)` を B の `documentToImage` にした新しい draft profile を作る。A のブロックは親のまま。新しい profile では p_b = 0
- 新しい条件は改訂として 1 つ足す（種類 `recalibrated`）。profile・出力ラスタ・継ぎ目の列を親と変えてよい唯一の種類

p_b に大きな値を入れずに profile を作り直す理由: p_b は「校正から範囲の内」が撮影とライブの読み手の決まりで、範囲を超える移動を p_b に入れるとこの決まりを崩す。profile に焼き込めば決まりはそのままで、範囲も新しい位置が中心になる。写像としては同じ（H0 · G(p-hat) = H(p-hat)）。

前提: 間隔を変える時、レンズ（焦点距離・ピント）と A の向きは変えない。広げるのは B を基線の向きへ動かすこと。これが崩れた時（B が振れた、高さやズームを変えた）は 5 節の線で確定させない。

## 2. 流れ

| 段 | 関数 | 中身 |
|---|---|---|
| R1 広く探す | `EstimateWideStart` | 原画像 2 枚で重なりを広く探す（`EstimateOverlapSimilarity`。重なりが幅の 10〜90 %）。合意したタイルの点を A・B それぞれの校正の写像の逆で文書の mm へ移し、文書面の剛体のずれ（tx, ty, rz）を最小二乗で求めて初期の姿勢 p0 とする。半回転の取付は画像を回してから探し、点を保存順へ戻す |
| R2 細かい合わせ | `ProposeRecalibration` の内 | `EstimateRegistration` を 6 自由度（紙面なし）で、base_pose = p0、条件の範囲・閾値・幾何で回す。`verified` が要る。継ぎ目の列は p0 での重なりの帯の中央（`keep_previous` なら親の列） |
| R3 焼き込み | `ReanchoredProfileText` | 新しい draft profile の文字列。`RigProfileV2::Parse` を通す |
| R4 作る時の検査 | `ProposeFromPose` | 出力ラスタ（`ProposeRaster`）と継ぎ目を決め、親の値を詰めた要求で `MakeFixedCompositeConditionV3`（ファイルを読まない純粋な関数）を呼び、`MakeFixedCompositeCreationCheckV3` を通す |
| 確かめ | `VerifyCandidate` | 新しい profile で製品と同じ設定の推定をもう一度回し、継ぎ目の列の上のずれを測る（5 節の 3） |
| R5 見本 | `SelectionOfCandidate` | 候補をメモリの中の selection にする。合成は既存の関数（撮影の経路が使う `ComposePairWithSelection`）に渡す |

`ProposeRecalibration` は R1 から確かめまでをつなぐ。ファイルを読まず、書かない。時刻、SHA-256 の提供、推定器の設計定数、停止の合図は呼び手が渡す。

## 3. 継ぎ目の列と出力の範囲（所有者の決定待ちの 2 つ）

どちらも切り替えられる作りで、既定は推す案。

| 選ぶ物 | 既定 | もう一方 |
|---|---|---|
| 継ぎ目（`SeamPlacement`） | `band_center`: 新しい profile での重なりの帯の中央。origin は `band-center-provisional`。撮るたびの合わせ込みが一番効く | `keep_previous`: 親の列（図面の上で同じ位置）。origin は `operator`。重なりの端に近くなると検査 (a) で落ちて確定させない |
| 出力の範囲（`RasterPlacement`） | `follow_camera_b`: 左・上・下・DPI は親のまま、右端を B の覆いの右端（全行の最小）が動いた量だけ動かす | `keep`: 親のまま（広げた分は合成に入らない） |

## 4. 画面に出す数

`Proposal::display`。確定できない時も、出せる所まで埋める。

- B の動き（図面の座標の言葉。`OutputFrameOf` を通すので取付が 180° でも向きは逆にならない）と、位置・回り・傾き・高さの大きさ
- 重なりの幅（新しいラスタの全行の最も狭い所、mm）
- 継ぎ目の列と図面の上の位置（新旧）、ラスタの大きさ（新旧）
- 継ぎ目が写真の端からどれだけの所か（A の右端と B の左端への距離。画素と幅に対する割合）。校正が確かな範囲の外へ寄ると、レンズの外挿が効いて誤差が出やすい（`rig-profile-v2.md`）ので、校正の画面に必ず出す
- 推定の質: B の画素での当てはめの残差（`ProfileFit`）、確かめの推定が残したずれ

## 5. 確定させない線

新しい閾値は作らない。線はすべて条件の欄か、推定器の既存の判定。確定させないとき `Proposal::refusals` に符号が入る（`RefusalCode`）。

1. R1 で重なりが一つに決まる（`no-overlap`）。重なりが幅の 10 % を割るほど広げた組はここで落ちる
2. R2 が `verified`（`estimate-not-verified`。理由は推定の `reasons`）
3. 確かめの推定が `verified` で、継ぎ目の列の上で適用するずれ（姿勢と、紙面を推定した時はその平面）が `registration.limits.convergencePx` 以下（`reestimate-not-verified`、`reestimate-displacement`）。焼き込んだ profile を製品と同じ経路でもう一度推定して、ほぼ 0 になる事を実行時に確かめる。R2 の始めを大きな p_b で回す経路の取り違えもここで捕まる
4. R2 の傾き |(rx, ry)| が `registration.envelope.tiltDeg` 以下、高さ |tz| が `heightPercent` 以下（`tilt-exceeded`、`height-exceeded`）。超えるのは「動かした」でなく「向きや高さが変わった」で、チャートでの校正が要る
5. 作る時の検査 (a)〜(d)（`creation-check-failed`。検査の文は `detail` に入る）
6. 出力ラスタが描画の限界以下（`raster-too-large`）で、profile の用紙の内側（`raster-outside-sheet`）

継ぎ目のずれのゲートによる確かめ（GΩ）は見本の合成の側（呼び手）で行う。

## 6. 記録

- 推定の記録 `a0.pair-recalibration-estimate.v1`（`Proposal::estimate_record_text`）: 撮影の組の識別と原画像の SHA-256、親の改訂・profile の SHA-256 と fingerprint と `provenanceId` と `toolId`（`reanchoredBefore` は親がすでに取り直した物か）、R1 の結果、R2 の結果（姿勢・各成分の標準偏差・事前分布の標準偏差・最後の当てはめの条件数・点と inlier の数・B の画素での残差）。パスを持たない。改訂の `reportSha256` はこの記録の SHA-256
- 新しい profile の `provenanceId` は `pair-reanchor-` ＋ この記録の SHA-256 の先頭 16 桁
- 確かめの結果、表示の数は `Proposal` に持つだけで記録のハッシュには入れない（記録を指す profile と循環しないため）。呼び手の校正の報告に載せる

## 7. 書き込む順（呼び手の仕事）

確定の時だけ、設定のフォルダに次の順で書く。計算の時にメモリに持った物を書く（校正用のフォルダに書いた写しを読み戻さない）。

1. 有効な改訂を読み直し、その SHA-256 が `parentSha256` と同じか確かめる
2. profile の写しを書く（`CopyFixedCompositeProfile`。名前は内容の hash）
3. 改訂を書く（`WriteFixedCompositeConditionV3`。排他の `.partial`、読み直し、置き換えなしの改名）
4. 読み直して、有効な改訂の SHA-256 が候補と同じか確かめる

2 の後で 3 が失敗すると profile の写しだけが残る。名前が内容なので害はなく、有効な改訂は前のまま。

## 8. 読み手への影響

`recalibrated` を知らない読み手（#434 より前）は改訂の列全体を拒否する。C を描かず、撮影は原画像だけを残し、合成しない。前の改訂へは倒れない。後から `values` や `rollback` を足しても古い読み手は読めないままなので、校正をやり直した後は新しい版のアプリだけを使う。

## 9. 限界と未検証

- 間隔を広げると、重なりが約 1 mm ごとに 1 mm 減る。必要な重なりは範囲の項（位置・回転・高さ・傾き・紙面・浮き）と混ぜ幅で決まり、作る時の検査が判定する。広げるほど厚い冊子の視差は間隔に比例して大きくなる
- 帯は縦に細長いので、基線の向きの平行移動 tx、高さ tz、縦軸まわりの傾き ry は帯の中でほぼ区別できない。推定器はこれを承知して、見えない向きを弱い事前分布で p_b（ここでは p0。tx, ty, rz だけ）へ引く。B が平行に動いた前提が崩れた時の台形のゆがみは継ぎ目から遠い所に残り、継ぎ目のゲートは見ない。5 節の 3 と 4 が実行時の守りで、継ぎ目から遠い所の寸法は実機で定規などを置いて確かめる
- 取り直しを重ねても、見える向きは毎回図面から決め直し、見えない向きは p0 に留まるので誤差は積み上がらない（※推定）
- A を動かした時（向きが変わった時）は、この流れでは気づけない
- 実機の K と公称の K の差が大きな移動でどう効くかは、合成の組では確かめた範囲だけ（試験の欄）。実機は未検証
