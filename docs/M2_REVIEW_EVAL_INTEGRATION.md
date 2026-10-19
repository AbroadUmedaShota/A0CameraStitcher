# 保存ペア接続と共通描画条件の統合（#354）

#352 と #353 を main `aec3edd` に対して一つのソースへ取り込む。保存済みreviewから検証したA/B原画像を選び、明示したdraft校正・位置補正・任意の色補正で評価し、共通の不変描画条件でJPEGを生成してreviewとのhash結合を保存する。

元の2つの作業ツリー、原画像、以前のreview・補正結果は保持する。通常の未校正C、製品のapproved-profile必須条件、profile承認/有効期限、実機の制御や撮影は変更しない。

## レビューと検証の対応

| 要件・境界 | 確認する証拠 |
|---|---|
| v1/v2/v3と同じpairの原画像だけを使う | strict readerと `still_review_contracts`、別processの `saved_review_evaluation_contracts` |
| 原画像・review・条件の保持と来歴 | 入力/親reportのhash・size照合、読取lock、別bindingのartifact hash、終了後の不変検査 |
| 既存ファイルを上書きしない | 新規directoryとCREATE_NEW、flush・非置換rename・同じhandleでの再読、既存output拒否試験 |
| 成功と位置/色補正の拒否を区別する | CLIのexit 0/3/2、数値拒否時に成功JPEG/manifestがないこと、既存CLI/schema契約 |
| 保存画像と縮小評価が同じ条件を使う | `m2_evaluation_render_conditions_contracts` の独立投影/ramp oracleと恒等対応のbyte一致 |
| draft/人工対応を製品承認にしない | approved-v2/rig readerの拒否試験、評価flags、未検証/拡大/不一致/未被覆の拒否 |
| カメラ制御や独立評価器に混入しない | StillReviewと純粋renderのリンク隔離、生成器/品質評価器の分離試験 |
| 既存画面の起動と保存作業の再開 | 人工画像を明示した `StillReview --report ... --smoke` と `--session ... --resume-smoke` |

## CIの接続不足と修正

統合前のWindows CIは製品用の既定構成だけをconfigureし、既定OFFの評価exeと開発用inspectorの接続試験を登録しなかった。通常CIの成功だけでは #352 の回帰を検出できない。

`.github/workflows/software-ci.yml` に、別runnerの独立jobで `build/review-eval-ci` を作り、`A0_BUILD_STITCH_EVAL=ON` と `A0_BUILD_WPD_DUAL_PREVIEW=ON` を明示する工程を追加する。既存jobの前回実績が約41分で45分制限に近いため、追加検査を同じjobへ直列に積まない。両構成で以下の5契約を実行する。

- `m2_stitch_eval_contracts`
- `m2_stitch_correction_schema_contracts`
- `saved_review_evaluation_contracts`
- `still_review_contracts`
- `m2_evaluation_render_conditions_contracts`

必要なtargetだけをbuildし、実カメラ用のpreviewを起動しない。通常の製品/M3用build directoryとoptionは保ち、default-OFF検査も維持する。Debug失敗でReleaseを隠さず、CIのsummaryへ両方の結果を追加する。

bindingに絶対pathがないことを確認する試験も、特定の `C:` だけを見る検査から、任意ドライブとUNCの表現を拒否する検査へ変更する。CIと操作PCの作業ドライブが異なっても、同じ来歴保持の契約を検査する。

## ローカル検証手順

```powershell
cmake -S . -B build/integration -G "Visual Studio 17 2022" -A x64 -DA0_BUILD_STITCH_EVAL=ON -DA0_BUILD_WPD_DUAL_PREVIEW=ON
cmake --build build/integration --config Release --parallel 1
$env:A0_LEASE_TEST_STRICT = '1'
ctest --test-dir build/integration -C Release -j 1 --output-on-failure
cmake --build build/integration --config Debug --parallel 1
ctest --test-dir build/integration -C Debug -j 1 --output-on-failure
```

実機画角・Live/JPEG間の色応答・図面品質の受入、共有条件の永続化/再選択、製品C/DLL/WPFへの接続は #351 の後続工程に残す。
