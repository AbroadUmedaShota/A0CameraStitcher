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

## 3. 継ぎ目の列と出力の範囲（所有者の決定）

どちらも切り替えられる作りで、既定は所有者が選んだ側（継ぎ目は重なりの真ん中、範囲は広げた分だけ広げる）。A と B を同時に動かして間隔を広げる使い方もあるので、画面と案内の文は「B を動かした時」と決め打ちせず、「カメラの間隔を変えた時」「A に対する B の位置」と書く。

| 選ぶ物 | 既定 | もう一方 |
|---|---|---|
| 継ぎ目（`SeamPlacement`） | `band_center`: 新しい profile での重なりの帯の中央。origin は `band-center-provisional`。撮るたびの合わせ込みが一番効く | `keep_previous`: 親の列（図面の上で同じ位置）。origin は `operator`。重なりの端に近くなると検査 (a) で落ちて確定させない |
| 出力の範囲（`RasterPlacement`） | `follow_camera_b`: 左・上・下・DPI は親のまま、右端を B の覆いの右端（全行の最小）が動いた量だけ動かす | `keep`: 親のまま（広げた分は合成に入らない） |

## 4. 画面に出す数

`Proposal::display`。確定できない時も、出せる所まで埋める。

- A に対する B の位置の変化（図面の座標の言葉。`OutputFrameOf` を通すので取付が 180° でも向きは逆にならない。左右と上下を分けて出す）と、位置・回り・傾き・高さの大きさ
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
7. 位置の計算が成り立つ（`estimate-failed`）。推定器が設定を断った、または例外を返した時
8. 新しい profile と記録が読み手を通る（`profile-invalid`）
9. 呼び手の中止の合図（`stopped`）。R1 と作る時の検査は合図を見ないので、待ちが数十秒になる事がある。合図には利用者の中止だけをつなぐ（期限をつなぐと `stopped` と誤って出る）

### 確かめられなかった時の扱い（所有者の決定）

上の符号は、呼び手（`recalibration_job.hpp`）が三つに分ける。「確かめられなかった」は、目印が足りなくて測れなかっただけで、合っていない印が何も見つかっていない時に限る。合っていないと測れた校正、合っていない疑いの印が 1 つでもある校正は確定させない。

| 区分 | 符号 | 扱い |
|---|---|---|
| 何も作れなかった | `no-overlap`、`estimate-failed`、`estimate-not-verified`、`stopped` | 確定させない |
| 確かめて合っていなかった・疑いがある | `tilt-exceeded`、`height-exceeded`、`creation-check-failed`、`raster-too-large`、`raster-outside-sheet`、`profile-invalid`、`reestimate-displacement`、`reestimate-not-verified`（確かめの推定が範囲の外と出た時）、`reestimate-doubtful`（確かめの推定が `undetermined` で、理由に「目印が足りない」の組の外が 1 つでもある時）、見本の推定が範囲の外（`sample-estimate-out-of-envelope`）、見本の推定が `undetermined` で理由に組の外がある時（`sample-estimate-doubtful`）、見本のゲートが `fail` で原因が `lift`・`pose`・`displacement`（`gate-fail`）、ゲートの `transition-outside-band`（`gate-transition-outside-band`）、ゲートの知らない符号と、状態が `pass` でないのに符号が無い時、知らない状態（`gate-unknown`）、ゲートの `gate-undetermined`（継ぎ目のずれを測る設定の食い違い。確かめる仕組みの故障で、目印の量の話ではない。文は「技術担当へ知らせてください」）、見本のゲートが `not_run`（`gate-not-run`。時間が足りなかった時も、処理が止まった時も確定させない。理由は finding の reason に入り、文だけが変わる。時間切れは「撮り直す」を押して撮り直す案内（続けて起きる時は技術担当へ）、処理の停止は技術担当への案内） | 確定させない |
| 確かめられなかった | `reestimate-not-verified`（確かめの推定が `undetermined` で、理由のすべてが「目印が足りない」の組）、見本の推定が `undetermined` で理由のすべてが同じ組（`sample-estimate-undetermined`）、見本のゲートの `nothing-measured`・`content-unmeasured`・`dx-unmeasured`・`placement-unverified`（符号ごとに `gate-nothing-measured` などの文） | 既定は、警告を見たうえで人が確定できる。定数 `kRecalibrationUnverifiedPolicy` で確定させない側にも切り替えられる |
| 測ったら怪しかった（所有者の答え待ち） | 見本のゲートの `unconfirmed-content`（基準を超えた点が測り直しで確かめられなかった）、`one-sided-content`（片方のカメラにだけ写る所がある） | 既定は確定させない（`gate-unconfirmed-content`、`gate-one-sided-content`）。定数 `kRecalibrationSuspectContentPolicy` を `allow_with_warning` にすると「確かめられなかった」と同じく警告つきで確定できる |

推定（確かめの推定と見本の推定）の「目印が足りない」の組は、`content_registration::AssessEvidenceShortage` が 1 か所で決める。確かめの推定、見本の推定、画面の文がこの関数だけを通る。理由は複数並ぶので、全部がこの組に入る時だけ「確かめられなかった」で、1 つでも外があれば疑いとして確定させない。理由が空の `undetermined` と、知らない符号も疑い。`unique-blocks` は推定器の 2 か所（粗い段と V2）から同じ符号で出る。`too-few-points` は細かい段で点が足りずに止まった符号で、この時は V2 が走らない。どちらも粗い段の数（一意の塊・一致した塊・目印の塊・一致した目印の塊・票で勝った位置と別の位置を指した目印の有無）で分け、数は推定のメモリ上の診断から渡す。別の位置を指した目印の数は、渡されなければ疑いになる値を持つ。v8 の報告は目印と票の数を持たないので、読み戻した記録では分けない。

| 区分 | 理由の符号 |
|---|---|
| 目印が足りない | `coarse-no-unique-blocks`、`repeating-only`、`too-few-points`（粗い段が何も数えていない時。数えた時は、その数が V2 を通る形か、下の `unique-blocks` の 2 つ目と同じく目印の数だけで落ちる形の時だけ）、`prediction-uncertain`、`too-dark`、`unique-blocks`（粗い段が塊の数で止まった時＝目印の塊が 1 以上、一意の塊が最小数未満で、一致の数がまだ数えられていない時。または V2 が目印の数だけで落ちた時＝目印の塊が 1 以上・最小数未満で、その全部が当てはめと一致し、一致した塊が最小数以上かつ使った塊の半分以上の時。どちらも、票で勝った位置と別の位置を指した目印が 1 つも無い時に限る） |
| 合っていない疑い | `coarse-ambiguous`、`coarse-inconsistent`、`fit-failed`、`not-converged`、`residual-shape`、`starts-disagree`、`no-overlap`、`estimate-failed`、`displacement-failed`、`time-budget`、`brown-lowe`（測った点が全部合っていても V1 を通らない数＝`points·(1−β) ≤ α` で、かつ `0 < inliers ≤ points` の時だけは目印が足りない側。点の数だけで落ちている時に限る。α・β は条件の `registration.limits` の値で新しい閾値ではない。α が有限で 0 以上、β が有限で 0 以上 1 未満でない時、`points` が 0 の時、`inliers` が 0 の時、`inliers` が `points` を超える時は疑い。今の値 α8・β0.3、点の最小数 12 では `points·(1−β)` は 8.4 以上なので起きず、`brown-lowe` は常に疑いになる）、`unique-blocks`（上の 2 つ以外。票で勝った位置と別の位置を指した目印が 1 つでもある時（粗い段が止まった時も、V2 で落ちた時も）、一意の塊の半分を超える数が当てはめと食い違った時、最小数以上あった目印が除かれて最小数を割った時、目印が 1 つでも除かれた時、目印の塊が 0 の時、数が欠けている・食い違う時、塊の最小数が設定されていない時。位置だけの推定では数えた塊はすべて目印なので、V2 から出る `unique-blocks` は常に疑い）、`too-few-points`（粗い段が数えた時で、上の場合以外。使った塊の半分を超える数が当てはめと食い違った時、目印が除かれて一致した目印が最小数を割った時、最小数未満の目印のうち 1 つでも除かれたか別の位置を指した時、目印の塊が 0 の時、数が食い違う時、塊の最小数が設定されていない時）、知らない符号 |

見本のゲートは状態でなく、ゲートの `gate_findings` の符号で分ける（`ClassifyGateFinding`）。状態が `fail` で原因が `unmeasurable` の時と `undetermined` の時が対象で、符号が複数あれば 1 つずつ分け、1 つでも確定させない符号があれば確定させない。基準を超えた（`lift`・`pose`・`displacement`）時は符号を見ずに `gate-fail`。

画面の継ぎ目のずれの行は、測れた値（最大・基準・測れた行の割合）だけを書き、確かめられたかどうかは書かない。それは findings の文が言う。見本の推定だけが疑いでゲートが `pass` の時は、`sample-estimate-doubtful` の文が「継ぎ目のずれが基準内でも、この校正は使えません」と添える。α・β・点の最小数・塊の最小数は開発用の仮の値で、値が変われば「目印が足りない」に入る組が出る（式はコードに残してある）。

確かめられないまま確定した改訂は、条件 v3 の書式に印を置く場所が無いので、校正の報告（`recalibration-report.json`）と確定の記録（`confirmation.json`）とログに残る。画面は「未確認の校正」と出す。見本のゲート（GΩ）は確かめの独立した守りなので、結果（測れたか、ずれの値と基準、測れた行の割合）を必ず画面と報告に並べる。

### 範囲の外に出た手直しから始める

有効な改訂の手直し（`manual-adjustment`）の基準の姿勢が範囲の外に出ていると、読み手は `pose_outside_envelope` で断り、「rollback するか、校正をやり直してください」と案内する。校正のやり直しはこの状態からも始められる。呼び手は、改訂の列を範囲を見ずに読み直し（`LoadActiveFixedCompositeRevisionV3`。列・profile・単一の出どころは範囲を見る前に検査済み）、有効な改訂を親にする。出力ラスタの決め方と「位置の変化」の基準は、親の基準の姿勢でなく profile の姿勢（0）にする（`Display::previous_outside_envelope` が立ち、画面に「前の手直しは範囲の外だったため、校正の位置からの変化を出します」と出る）。確定した `recalibrated` は基準の姿勢が 0 なので範囲の内になる。列が壊れている・profile が無い・違う・v1/v2 が並ぶ時は、これまでどおり断る。

継ぎ目のずれのゲートによる確かめ（GΩ）は見本の合成の側（呼び手）で行う。計算の側の確かめ（`VerifyCandidate`）は同じ組での焼き込みの確かめで、精度の独立な確かめではない。

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
