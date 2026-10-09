# 独立した合成画像の評価器（#275）

`A0CameraStitcher.StitchEvaluator` は、保存済み JPEG を独立に計測する開発用 CLI。製品の renderer、stitcher、rig profile reader、合成チャート生成器にはリンクしない。期待位置の算出にも製品の投影・補正処理を使わない。品質は常に `not-evaluated`。合成試験の閾値を満たしても実写品質の承認にはならない（#42）。

## 入力と実行

SDK なしのローカルビルドで `-DA0_BUILD_STITCH_EVALUATOR=ON` を明示する。既定は OFF。Schema のローカル試験には PowerShell 7（`pwsh`）が必要。引数は以下の7個がすべて必要。

```powershell
& .\A0CameraStitcher.StitchEvaluator.exe `
  --image C:\evaluation-input\evaluation.jpg `
  --ground-truth C:\evaluation-input\ground-truth.json `
  --plan-file C:\evaluation-input\plan.json `
  --thresholds-file C:\evaluation-input\thresholds.json `
  --validity-file C:\evaluation-input\validity.json `
  --product-root C:\evaluation-product `
  --output-directory C:\evaluation-results\run-001
```

入力と product root は既存、出力は既存の通常ディレクトリの下に新しく作る子ディレクトリ。入力ディレクトリ・指定 product root・実際の LocalAppData の製品 root と重なる出力、reparse point、UNC、device path、ADS は拒否する。入力を読み取りロックし、公開前後に再読照合する。入力は書き換えない。

plan、thresholds、validity と report の JSON Schema は `docs/schemas/stitch-evaluation-*.v1.schema.json` と `stitch-output-validity.v1.schema.json`。未知・重複・不足のキーを拒否する。整数入力は `1.0` や `1e0` を認めない。入力 JSON は各256 KiB、JPEG は64 MiB、画素数は2億以下、各辺32768以下。plan の基準点・DPIペア・継ぎ目ペアは各4096以下、窓の半径512以下、計測する窓・パッチの総画素訪問数は2^24以下。

ground-truth は #269 の `/2` 注釈。利用する schema、conventions、文書寸法、基準点 ID と `document_mm` を検証する。未利用の camera projection、生成物一覧や pattern の内容まで生成器の spec として検証する入口ではない。合成画像の期待位置は、plan の文書領域・DPIから次式で求める。

```text
x = (document_mm.x − regionUm.left / 1000) × dpi / 25.4 − 0.5
y = (document_mm.y − regionUm.top  / 1000) × dpi / 25.4 − 0.5
```

`image_px` は元のカメラ画像の座標なので使わない。検出後に全体の位置合わせをしてずれを消す処理もない。各基準点の窓は期待位置を最寄り整数に丸め、その周囲の正方形を使う。全域が画像内で全マスク値が1である窓だけ計測する。

## 計測と閾値

暗い8近傍連結成分から、面積・縦横比・周囲とのコントラストを満たす中央ドットを検出する。窓端に接する成分は使わない。閾値との差を重みとする重心を求め、期待位置に最も近い候補を採る。2候補の距離差が指定値以下なら ambiguous。検出用パラメータと最低サンプル数はすべて plan に明示する。

| metricId | 生の計測値 |
| --- | --- |
| fiducial-position-median | 位置誤差の中央値（偶数は中央2値の平均）、px |
| fiducial-position-p95 | 位置誤差の LinearR7 p95、px |
| fiducial-position-max | 位置誤差の最大、px |
| fiducial-detection-coverage | 検出成功数 / 指定した全基準点数 |
| valid-pixel-coverage | マスクが1の数 / 明示した出力ラスターの全画素数 |
| seam-luma-step-max | 同じ平坦な内容と事前注釈されたパッチ対の平均輝度差の最大 |
| seam-rgb-step-max | 同パッチ対の平均RGBの各チャンネル差の最大 |
| local-dpi-error-ratio-max | `abs(実測DPI / 指定DPI − 1)` の最大 |

実測DPIは `25.4 × 検出点間距離px / 既知の文書上の距離mm`。個々の実測DPIも report に残す。継ぎ目パッチは呼出側が製品実行前に同内容の領域として指定する。実行結果から内容を探して比較対象を選び直したり、seam path を推定したりしない。

色と輝度は WIC が保存順で復号した8 bit RGBのコード値。輝度は `(0.2126 R + 0.7152 G + 0.0722 B) / 255`、RGB差も255で正規化する。ガンマ変換・ICC適用・CIEDE2000ではない。実写の色再現の合否はここでは判定しない。

外部の thresholds には8指標すべてを一度ずつ指定する。被覆だけ `AtLeast`、ほかは `AtMost`。丸め前の生値で境界を含めて比較する。閾値は組込み・既定値にしない。

## マスクの意味

JPEG の黒は有効なインクにもなりうるため、RGBから欠損を推定しない。validity sidecar は JPEG の実バイトと、sidecar と同じディレクトリにある固定名 `validity.pgm` の実バイトの SHA-256、寸法を結び付ける。PGM は正確に `P5\nWIDTH HEIGHT\n1\n` と全画素の0/1バイト。コメント・余剰バイト・別の最大値は認めない。

マスクは外から申告された有効性。ハッシュ一致はこの申告と JPEG の結合を保証するだけで、申告元が被覆を正しく計測したことの証明にはならない。被覆領域は明示した出力ラスター全域。文書全体の実機被覆を保証しない。

位置・DPI・継ぎ目は欠損した窓を計測から除外する。一方、基準点の missing / ambiguous / partial-window / invalid-mask は基準点被覆の分母に残す。無効画素0も画素被覆の分母に残す。report の `coverageTrialValidity=all-declared-trials` は、この計測試行の意味を明示する。

## 出力

成功時は `stitch-evaluation.report.json` だけを、新規 `.partial` の同一ハンドルで書込み・flush・照合して非置換 rename する。失敗時は書けた診断用ファイルを残す。JPEGや製品の manifest は作らない。report は元ファイルのハッシュを残すが、絶対パスや実撮影識別子は含めない。

既存の stitch-metric definition/result v1 を各指標に埋め込む。HalfToEven / AggregateThenRound、decimalPlaces は plan 指定の0〜9。公表する rawValue の十進表記を正確に丸め、既存 validator の丸め契約に合わせる。拡大した値が2^39を超える結果は拒否する。

閾値超過でも計測が成立すれば metric result は `Success`、`failureCode=null` で数値を保つ。足りない計測は `NoResult` と契約上の failureCode、数値は null。threshold の `met` と report の診断コードは別に記録する。最低サンプル数未達でも被覆の数値と検出状況を残す。

終了0は記録完了。`thresholdAssessment` は `thresholds-met` / `thresholds-not-met` / `incomplete-measurements`。これは品質合格ではない。入力・IOの拒否は終了2、標準エラーに `error=<stage>-failed`。公開APIの SerializeReport は形と値の契約を確認する adapter で、手作りした Evaluation の数値を画像から再計測しない。CLI は必ず Parse → Evaluate → Serialize の結果を保存する。

## ローカル試験

`m2_stitch_evaluation_contracts` は独立に描いた対称基準点の解析的な座標と比較する。実JPEGを使う `m2_stitch_evaluator_cli_contracts` は正常・位置ずれ・継ぎ目の露出差・伸縮・欠損、再実行のバイト一致、入力不変、拒否時の未公開を確かめる。別プロセスの実際の #269 生成器が作るペアも読み、明示した恒等ラスターに投影された片方のJPEGを文書座標から計測する。これは生成器の注釈との接続試験であり、製品の合成処理や実写品質の合格ではない。`m2_stitch_evaluator_schema_contracts` は実際に保存された正常・破損・計測不足・生成器の report と、変異させた不正 report の Schema 適合を確かめ、埋め込んだ既存 metric Schema の変更も検知する。`m2_stitch_evaluator_link_isolation` は renderer/stitcher/generator/profile の直接・推移・source混入を実configureで拒否させる。

fixture の0.5 pxなどは試験用の外部入力。圧縮前の解析解と JPEG 復号後の値は分けて扱い、これを #42 の品質閾値や #269 の圧縮前0.02 px許容と混同しない。保存された fixture は独立した解析解の互換注釈であり、生成器がこのカメラ座標を出力したという記録ではない。
