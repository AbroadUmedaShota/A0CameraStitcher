# 画像Mergeと縮小Cの共通描画条件（#353、#351 R2先行）

`experimental::EvaluationRenderConditions` は、固定rig投影と適用する撮影ごとの有界補正結果をコピーして保持する、camera-freeの評価専用API。既存の `StitchEval` は、補正jobを選択した場合はその評価成功後に、この条件を一度作り `RenderStill` で保存画像を描く。補正jobなしの従来経路も維持する。推定器、補正の受入基準、出力JPEG、manifest、終了コードは従来どおり。

条件は文書crop、DPI/ceil寸法、A/Bの補正済み投影、配置、kernel、元画像寸法、任意のCAM-B BGR gain/offsetと来歴を保持する。来歴はprofileのfingerprintとfile SHA-256、適用した位置/色補正のjob/report SHA-256。色変換と色来歴の有無を一致させ、色来歴には位置補正来歴を要求する。アクセサはconst参照で、呼出元の変数を変更しても条件は変わらない。

hashラベル自体は画像byteの認証ではない。入力/profile/jobのロック・再読、受入済み補正だけの適用、manifestへのhash記録は既存のCLIが引き続き担当する。この純粋APIはprofileを読み込んだり、draftを承認したりしない。

## 明示したLive/JPEG対応

`RenderPreviewForEvaluation` は、同じ文書crop、同じ投影・色・配置・kernelを使い、明示した低い/同じDPIの出力rasterで描く。A/Bそれぞれの `StillToLiveMapping` を必須とし、原画像/Liveの寸法と次の4値を受け取る。

```text
live_x = scale_x * still_x + offset_x
live_y = scale_y * still_y + offset_y
```

座標は保存順の整数sample index。scaleは正の有限値、offsetは有限値。寸法比からscaleを推測せず、半画素のoffsetも自動で追加しない。例えば画素中心に基づく縮小なら、その対応を測定またはfixtureで定義したoffsetに含める。

出力sample `(i,j)` から、既存rendererと同じ `left_mm + (i+0.5)*25.4/dpi`、`top_mm + (j+0.5)*25.4/dpi` で文書座標を求める。元の補正済み投影とBrown-Conrady歪みを通し、そのraw座標へ明示対応を適用する。元のsensor範囲外は、Liveのpaddingで被覆を増やさず無効とする。中間の縮小合成画像や再投影画像を作らず、既存 `RenderMappedPair` にLive入力を一度だけ標本化させる。CAM-Bの色補正は標本化後、feather前に適用する。

対応には条件と一致するprofile fingerprint、通常倍率の明示、`MappingEvidence::synthetic_fixture` を要求する。未検証・未知の区分、拡大表示、寸法/対象profileの不一致、crop変更、密度の増加、不正なceil寸法、非有限値/overflow、どちらの入力でも覆えないsampleは拒否する。低DPIのceil領域が公称cropの外へ少し延びる場合も、実際のsampleと投影域を検証する。

現段階で実測対応を受け入れる区分はない。このAPIの成功は常に人工対応の評価で、実機Live/JPEG一致や製品Cの承認を意味しない。現在の未校正開発C、approved-v2の製品stitch入口、profileの承認/期限/fingerprint検証は変更しない。

## 検証

`m2_evaluation_render_conditions_contracts` は、A/B別の寸法・非ゼロoffset・異方縮小、両配置、両kernel、非ゼロ歪み、色補正、同一crop、片側被覆と重なり、ceil端を人工ramp画像で検証する。独立した投影多項式と線形rampの期待値を使い、fixtureの量子化誤差を1 DN以内とする。恒等対応では保存描画とのbyte一致も要求する。この数値許容はfixture用で、光学的な受入閾値ではない。

```powershell
cmake -S . -B build/r2 -G "Visual Studio 17 2022" -A x64 -DA0_BUILD_STITCH_EVAL=ON
cmake --build build/r2 --config Debug --target m2_evaluation_render_conditions_contracts m2_stitch_eval_contracts
ctest --test-dir build/r2 -C Debug -R '^(m2_evaluation_render_conditions_contracts|m2_stitch_eval_contracts)$' --output-on-failure
```

新しいソースも独立したsynthetic生成器・品質評価器への混入をリンク隔離規則で拒否する。描画libraryは従来どおりWin32/COM/WIC/BCryptへ依存しない。

Windows SDK-free対象検証は、Release関連9/9 CTests（183.37秒）、Debugは校正/品質評価の依存分離検査も含む15/15（352.05秒）が成功。新規の人工描画試験は両構成14,517項目。製品側のdraft・期限切れ拒否、評価CLIの既存JPEG/manifest/補正成功・拒否契約も成功した。#352 の未変更の保存ペア接続から今回のRelease評価exeを呼ぶ別process試験も137項目成功した。全software-only CI、製品Cの性能、実機/図面品質は今回の検証範囲に含めない。

## 後続

#352 のファイル選択を通常UIへ接続する作業、共有条件の永続化/再選択、期限切れ・承認・対応の実測証跡の製品側検証、#309 の実リグ画角測定、#308 のDLL/WPF/製品C・性能受入が残る。実機の明るさ/色応答がLiveとJPEGで一致する保証も別途検証が必要で、このfixtureから推論しない。吊り下げ後の校正値、HG-0001/HG-0002、図面品質を今回の合成データで承認しない。
