# 保存JPEGの自動補正つきdraft評価（#320）

#319の自動seed選択→両方向patch照合→raw逆写像→上限つき相似補正を、既存の`A0CameraStitcher.StitchEval`へ接続する開発用経路。保存された7360×4912 JPEGを読み、補正したCAM-B写像で元画像を一度だけrenderする。CAM-Aと校正profileは変更しない。

#322で任意の`--color-job`を併用する[位置・色補正つき評価](M2_COLOR_EVAL.md)を追加した。未指定時の本書の形式と動作は維持する。

#324でcorrection job v2を明示した場合だけ[細探索つき評価](M2_REFINEMENT_EVAL.md)を追加した。本書のjob v1・report v1・manifest v2の契約は維持する。

`A0_BUILD_STITCH_EVAL`は既定OFF。製品adapterは従来どおりdraftを拒否する。今回のpolicyは開発者が明示する試験条件で、実写・製品の承認済み閾値ではない。自由homography、profileの自動学習、撮影、Live Viewは追加しない。

## 呼出しとpolicy

[既存の13必須option](M2_DRAFT_STITCH_EVAL.md)に`--correction-job <絶対パス>`を追加する。指定しない呼出しは従来の動作・stdout・v1 manifestを維持する。

```powershell
cmake -S . -B build/eval-stub -G "Visual Studio 17 2022" -A x64 -DA0_BUILD_STITCH_EVAL=ON
cmake --build build/eval-stub --config Release --target m2_stitch_eval_contracts --parallel 1
# 既存のStitchEval呼出しに、次のoptionを追加する:
# --correction-job $correctionJobAbsolutePath
```

jobは[`a0.stitch-correction-job.v1`](schemas/stitch-correction-job.v1.schema.json)。すべての値を要求し、既定値で補わない。[人工JPEG専用の例](../tests/fixtures/stitch-correction/job.json)を実写へそのまま適用しない。

| object | 設定 |
| --- | --- |
| `seedSelection` | gridの行列数、最大seed数、最小間隔、gradientの小固有値と軸比の下限 |
| `matching` | patch半径/文書mm間隔、探索半径/文書mm刻み、contrast/NCC/一位二位差/逆照合誤差 |
| `inverse` | Newton更新数、backtrack数、raw画素での往復誤差 |
| `correction` | 必要点数/広がり/二軸比、出力画素での残差と最大変位、回転度/倍率差%の上限 |

入力は最大64KiB、nestingは最大8。未知・重複・不足のkey、不適切な型、非有限数、範囲外を拒否する。整数欄は数字だけの表記を要求し、小数・指数・符号表記も拒否する。JSON schemaの整数型だけでは表記まで検証できないため、native parserを最終の入力検証とする。

grid最大64×64、選択最大256点、patch半径最大8、探索半径最大32。これらは構造/作業量の限界であり、実写の品質条件を決めるものではない。画像から選んだseed集合は固定し、照合に失敗した点を捨てて成功に変えない。補正の上限超過もclampしない。

## 成果物と失敗

数値計算を終えた場合は[`correction-report.json`](schemas/stitch-correction-report.v1.schema.json)を保存する。入力JPEGのalias/hash/byte数、profile fileのhashと意味的fingerprint、job fileのhashと正確なpolicy snapshot、評価ラスタを結び付ける。候補全点の被覆/texture/選択理由、選択index、全seedの両方向NCC/contrast/探索数、raw対応、逆写像の成否/往復誤差/更新数、相似推定値・残差・上限違反を記録する。未到達段階・取得できなかった値はnull。

`profileStatus=draft`、`quality=not-evaluated`、`productApproved=false`、`previewRequiresManifest=true`は固定。絶対パスや実機識別子は出力しない。

| 終了値 | 状態と出力 |
| --- | --- |
| 0 | 補正成立・render・保存/再読の成功。`evaluation.jpg`、`correction-report.json`、最後に`stitch-eval.manifest.json`の3ファイル |
| 3 | 数値的な補正不成立。reportのみ。JPEGや成功manifestは作らない |
| 2 | 引数/パス/入力/profile/job形式/IO/renderなどの失敗。stdoutは空、stderrは段階名のみ。途中成果物は診断用に保持 |

exit 0のstdoutは従来どおり。exit 3は次の4行。

```text
result=correction-rejected
profileStatus=draft
quality=not-evaluated
report=correction-report.json
```

補正つき成功manifestは[`a0.stitch-eval-manifest.v2`](schemas/stitch-eval-manifest.v2.schema.json)、engineは`2.0.0`。既存欄に加え、reportの相対path/hash/byte数とjob/profile file hashを記録する。補正なしはv1/engine `1.0.0`のまま。

**reportの`correction-estimated`だけでプレビュー成功と判断しない。** report公開後のrender/保存で失敗する場合もある。最終v2 manifestがあり、JPEG・report・入力・profile・jobのhashが一致した評価結果だけを成功候補として扱う。品質合格や製品利用の承認にはならない。

## 保存境界

既存CLIの通常local fixed-drive absolute path、祖先handle固定、既知製品ルート除外、reparse/ADS/UNC/device拒否を維持する。jobも祖先固定・write/delete禁止のread lock・最大byte数・再読検証の対象に加える。JPEG/profileと同じ実体のjobはhardlinkを含めて拒否する。

新規出力directoryと`.partial`を排他的に確保する。report/JPEGを同じ保持handleでflush・検証・非置換renameし、入力snapshotと公開済みreport/JPEGを再確認して最後にmanifestを確定する。既存成果物を上書きせず、原画像・profile・jobを書き換えない。

## 再検証

```powershell
cmake --build build/eval-stub --config Release --target m2_stitch_eval_contracts --parallel 1
ctest --test-dir build/eval-stub -C Release -R '^m2_stitch_(eval_contracts|correction_schema_contracts)$' --output-on-failure
# Debugでも同じbuild/CTestを行う。
```

独立したprocess-only harnessは補正core/profile reader/rendererとlinkせず、WICで人工JPEGを保存し、実CLIを子processで呼ぶ。両kernelの2反復、既知のraw移動(+3,-2px)→文書移動(+0.25,+0.375mm)、画像への補正適用、report/manifest結合、旧v1、入力不変、上限超過・白紙・被覆不足、job形式/パス/alias拒否を検証する。画像への適用は、未移動の独立JPEG対を使った基準renderとの画素差で確認する。

schema試験は実CLIの成功4件・拒否3件と、品質/承認/path/hash/構造の負例を検証する。実写・A0全域の品質、承認済みprofile値への結合、通常アプリの撮影ごとの補正経路、Live View合成は後続。

2026-10-15、MSVC x64/SDKなしでRelease/Debug build成功。process-onlyは各1747チェック、証拠保存時は各2041チェック、schemaは各104チェック成功。Releaseの関連CTestは9/9＋CLI/schema 2/2、Debugは10/10成功（既定OFFと生成器の依存分離32ケースを含む）。全体suiteや重い18枚の校正工程は今回再実行していない。

両kernelの各2反復でJPEG/report/manifestが一致。保存された成功・拒否の記録と試験用profile/jobを含む全21成果物もDebug/Release間でSHA-256が一致した。未移動JPEG対の基準renderに対する平均画素値誤差は、bilinearで15.0258→1.575 DN、bicubicで18.2148→1.48594 DN。小領域の人工画像による接続確認であり、実写の光学品質評価ではない。
