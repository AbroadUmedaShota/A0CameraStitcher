# 保存JPEGの位置・色補正つき評価（#322）

#320の評価CLIへ[#321の色補正](M2_COLOR_CORRECTION.md)を接続する開発用経路。既存の13必須optionに`--correction-job`と`--color-job`を追加する。色だけを指定する呼出しは拒否する。既定OFFの`A0_BUILD_STITCH_EVAL`、calibrated-draft専用、製品adapterのdraft拒否は維持する。

```powershell
cmake --build build/reference --config Release --target m2_stitch_eval_contracts --parallel 1
# 既存StitchEval呼出しへ両optionを追加する:
# --correction-job $geometryJobAbsolutePath --color-job $colorJobAbsolutePath
```

## 処理と互換性

1. 両JPEG、profile、幾何job、色jobをread lock/snapshotで固定する。すべてのjobの形式を出力作成前に検証する。
2. 従来の幾何補正を実行し、`correction-report.json`を保存。不成立なら従来のexit 3で終え、色は実行しない。
3. 幾何補正が選んだ**同じ全seedを同じ順で**使い、CAM-Aと幾何補正後CAM-Bの色を原画像から観測する。色不成立点の破棄・差替え・再選択はしない。
4. `color-report.json`を保存する。色が不成立ならexit 3で終え、JPEGと成功manifestは作らない。
5. すべて成立した場合だけ元画像を一度samplingし、CAM-Bのsampleへ色補正してから既存featherで描画する。JPEGの公開・再読と全入力/両reportの再検証を終えて、最後に成功manifestを確定する。

| 指定 | 成功成果物 | manifest / engine |
| --- | --- | --- |
| なし | JPEG＋manifest | 従来のv1 / 1.0.0 |
| 幾何jobのみ | 幾何report＋JPEG＋manifest | 従来のv2 / 2.0.0 |
| 幾何job＋色job | 幾何report＋色report＋JPEG＋manifest | v3 / 3.0.0 |

profileは既存v2のまま、保存原本・校正値・所有者の承認欄は変更しない。色未指定の既存report形式や出力byteを変えない。

## 色job

[`a0.stitch-color-job.v1`](schemas/stitch-color-job.v1.schema.json)は、`schemaVersion`に加えて`minimumSamples`、`minimumSampleDn`、`maximumSampleDn`、`minimumStddevDn`、`maximumGainDifferencePercent`、`maximumOffsetDn`、`maximumResidualDn`をすべて要求する。各値の意味は#321のpolicyと同じ。閾値の既定値で補わない。[人工JPEG専用の例](../tests/fixtures/stitch-correction/color-job.json)は実写の承認値ではない。

最大64KiB、nesting最大8。未知/重複/不足のkey、型の誤り、非有限・範囲外、空のsignal区間を拒否する。点数は数字だけの整数表記とし、小数/指数/符号/overflowも拒否する。JSON schemaでは数値字句や二つの欄の大小を検証しきれないため、nativeのstrict parserを最終検証とする。

通常local fixed-drive absolute path、祖先handle固定、製品root除外、reparse/UNC/device/ADS拒否の既存規則を維持する。色jobもwrite/delete禁止のlockと再読検証の対象。JPEG/profile/幾何jobと同じ実体はhardlinkを含めて拒否する。既存の出力や`.partial`を上書き・削除しない。

## 診断と成功記録

[`color-report.json`](schemas/stitch-color-report.v1.schema.json)には次を保存する。

- 正確な色policy snapshotとfile hash、両原本のalias/hash/byte数
- profile file hashと意味的fingerprint、幾何reportのhash、評価ラスタ
- 全seedの文書位置・観測成否・元の両BGR値
- B/G/R別の両側標準偏差、gain/offset推定値、残差rms/max、違反とsample/channel index

`model=encoded-dn-diagonal-affine`、`channelOrder=BGR`、`profileStatus=draft`、`quality=not-evaluated`、`productApproved=false`、`previewRequiresManifest=true`は固定。未到達段階や測れない値はnull。絶対パス・機体の識別子は書かない。

成功manifestは[`a0.stitch-eval-manifest.v3`](schemas/stitch-eval-manifest.v3.schema.json)。v2の欄を維持し、色reportの相対path/hash/byte数と色job hashを`colorCorrection`へ追加する。幾何reportと色reportの両方が、同じ原本/profile/jobに結び付いた結果であることを確認できる。

色reportの`color-estimated`だけでは保存やプレビューの成功ではない。report公開後にrender/IOが失敗する場合もある。最終v3 manifestと、JPEG・両report・全入力のhash一致を必要とする。品質合格や製品利用の承認にはしない。

## 終了値

成功は従来のexit 0 / `result=evaluated`の4行。色の数値的な不成立はexit 3で次の4行。幾何reportと色reportを残す。

```text
result=color-rejected
profileStatus=draft
quality=not-evaluated
report=color-report.json
```

幾何不成立は従来の`result=correction-rejected`で幾何reportのみ。形式・IO等の失敗はexit 2、stdoutは空、stderrは段階名のみ。失敗時の途中成果物は診断用に保持する。

## 検証範囲

process-only harnessは補正計算/reader/rendererにlinkせず、WICで7360×4912の人工JPEGを保存し、実CLIを子processで呼ぶ。位置ずれ(+3,-2px)に加え、B/G/R別の既知gain/offsetを画像に与える。色試験は32点を使い、JPEG圧縮誤差が少数点の係数推定を支配しないようにする。許容差はテスト専用で、製品の点数や品質条件を決めるものではない。

成功側の色付きBはJPEG quality 1 / 4:4:4で作る。quality 0.98で同じ画像を圧縮した場合、赤channelの最大残差8.043 DNが試験policyの6 DNを超えるため、これを別の拒否回帰として保持する。係数の許容差や残差上限を緩めて成功させない。

両kernelの各2反復、保存JPEGの基準画像からの誤差、係数の独立真値、全観測/来歴/hash、拒否時の非公開、入力不変を検証する。schemaは実際の成功4件・色拒否5件と構造/承認/path/hash/stateの負例を確認する。

実写のモデル妥当性、全A0の色/継ぎ目/解像品質、承認済み条件への結合、通常アプリ、Live Viewは後続。今回の色試験を光学品質の合格に使わない。

2026-10-15、MSVC x64/SDKなし: Release/Debugの実CLI試験は証拠保存modeで各5118チェック成功。両構成の実report/manifestに対するschema正負例は各236チェック成功。両kernelの2反復で4成果物が一致し、成功/拒否の記録やjobを含む全43成果物も構成間でbyte一致した。色未指定の従来21成果物も変更前とbyte一致。関連CTestは別途両構成10/10成功（Release 31.03秒、Debug 21.32秒、生成器の依存分離40ケースを含む）。

保存JPEGを再読した平均画素値誤差はbilinearで12.5687→1.5 DN、bicubicで12.782→1.55859 DN。比較前は幾何のみを補正した画像。実写の光学精度・色品質の受入値ではない。全体suiteや重い18枚の校正工程は今回再実行せず、機器操作・main反映・pushも行っていない。
