# 保存JPEGの微小回転・倍率差の検証（#325）

[#324の細探索つき評価CLI](M2_REFINEMENT_EVAL.md)に対し、端数移動と微小な回転・倍率差を同時に与える検証を追加する。今回はprocess-only harness・schema検証・試験来歴を拡張する。補正アルゴリズム、製品の閾値、job/report/manifestの形式は変更しない。

## 独立した画像・変換の基準

WICで7360×4912の人工JPEGを保存し、実CLIを子processとして呼ぶ。二つのシナリオを用いる。

| シナリオ | raw画像上の回転 | 倍率 | raw画像上の端数移動 |
| --- | --- | --- | --- |
| positive | +0.0025 rad（約+0.143239°） | 1.001（+0.1%） | (+2.35,-1.65)画素 |
| negative | -0.0025 rad（約-0.143239°） | 0.999（-0.1%） | (+2.35,-1.65)画素 |

回転・倍率の中心はraw位置(3332,240)。`a=scale*cos(angle)`、`b=scale*sin(angle)`とし、対応位置の基準は`q=c+[a -b; b a]*(p-c)+d`。画像生成器はこの式の逆をscalar演算で直接計算し、連続三角関数の模様を元画素位置にsamplingする。productionのprojection、逆変換、sampler、補正推定器で元画像をwarpしない。

固定profileは従来と同じ人工profileで、両cameraのraw投影を`x=8*documentY+100`、`y=-8*documentX+4800`とする。処理領域は文書(565,400)〜(575,408) mm、100 DPI・40×32出力画素。ceilを含む実際の領域は10.16×8.128 mm。**D810寸法の画像全域・A0全域の精度試験ではなく、この小領域での接続確認**である。

JPEG encoderの指定はquality 1・4:4:4。grayと既知BGR gain/offsetの二種類、両kernel・各2反復を使う。計16件の成功と、同じgray JPEGに対して回転上限だけを0.01°、または倍率差上限だけを0.01%に下げた計4件の拒否を確認する。これらのpolicy値・許容差は人工入力の試験条件で、承認済み光学値ではない。

## 検査項目

- 全6seedの両方向細探索が完了し、raw対応が独立した既知の変換と0.15画素未満で一致する。
- 推定された文書変換を固定投影へ合成した位置が、評価領域の四隅で独立したraw基準と0.15画素未満で一致する。軸の取り違え、回転の符号、拡大/縮小、原点の離れた文書座標への変換を確認する。
- reportの最大変位がceilを含む四隅から算出した値と出力画素単位で一致する。
- 最終JPEGが未移動JPEG対の描画へ近づき、同じ入力JPEG対を固定profileだけで描画した負対照より平均画素差を4倍以上減らす。gray/色差ありでそれぞれ同じ入力の補正前後を比べる。画像全体を中間warpして再samplingする経路は追加しない。
- 色補正併用、report/manifest/JPEGのhash結合、反復byte一致、原画像/profile/job不変を維持する。
- 回転/倍率上限を厳しくした場合は該当する違反が一つだけ残り、推定値は成功時と数値のJSON表記まで一致する。再推定・clamp・成功JPEG/manifestの公開を行わない。

## 来歴・再検証

geometry report v2 / manifest v4 / color report v1を維持する。試験用の[`similarity-oracle.json`](schemas/synthetic-similarity-jpeg-test.v1.schema.json)には、各試験のraw相似係数`[a,b,dx,dy,cx,cy]`、encoder設定、入力とgeometry reportのhash、独立基準との最大誤差、最終JPEGとの差を記録する。固定の`synthetic=true / productApproved=false / quality=not-evaluated`を付け、製品用の成果物として扱わない。

schema検証では新しい20 geometry report、16 manifest、8 color report、oracleの型・必須欄・負例に加え、oracleの全16組の重複がないことと、入力/reportの実hashへ結び付いていることを検査する。形式上正しい別hashや重複caseも拒否する。

```powershell
cmake --build build/reference --config Release --target m2_stitch_eval_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_stitch_(eval_contracts|correction_schema_contracts)$' --output-on-failure
# Debugでも同じbuild/CTestを行う。
```

既存harnessの省略可能な`--similarity-only`は、指定した新規の試験証拠directoryへ今回のシナリオだけを保存する診断用。既定のharness/CTestは従来試験も含めて実行する。

一般の模様、JPEG圧縮条件の広い範囲、実写、光学解像度、最終seam、承認済みprofileの閾値との対応、通常アプリ・Live Viewへの接続は未検証または後続。

## ローカル検証時の確認結果

Windows/MSVC x64、SDKなしのRelease/Debugで、従来分を含む実CLI harness各22,492チェック、schema各582チェックが失敗0で完了した。成功16件・上限拒否4件を含む。独立基準との最大raw対応誤差は0.0342183画素、評価領域四隅の最大誤差は0.0482727画素だった。

同じ入力JPEG対の固定profileだけの負対照と比較し、最終JPEGの平均絶対差はgrayで6.16406〜6.41484 DNから0.375〜0.471875 DNへ、色差ありで17.6339〜17.6948 DNから0.5〜0.5515625 DNへ減少した。全144成果物はRelease/Debug間でbyte一致し、従来83成果物も変更前と一致した。

最終証拠はignoredな`build/reference/similarity-325-release-verified`、`similarity-325-debug-verified`、対応するnative/schemaログと`similarity-325-byte-comparison.json`に保持する。中間の`first`・`final`という名前のdirectoryは最終証拠ではない。今回の結果で実写品質・全画面品質・製品受入を承認しない。commit・push・main反映は未完了。
