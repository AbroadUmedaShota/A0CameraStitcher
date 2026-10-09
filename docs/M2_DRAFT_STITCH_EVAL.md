# calibrated-draft の評価合成（#273）

`A0CameraStitcher.StitchEval` は ADR-0033 の段階0/1用の評価専用exe。schema 2.0.0 の校正済みdraft、二つのD810サイズJPEG、明示した評価用ラスタとkernelを受け取る。profileの所有者決定欄はnullのまま保持する。製品adapterにdraftを許す分岐は追加しない。

## ビルドと呼出し

`A0_BUILD_STITCH_EVAL` は既定OFF。評価exeは製品adapter・アプリ・配布targetの依存先にしない。使用するときは専用build directoryで明示的にONにする。

```powershell
cmake -S . -B build/eval-stub -G "Visual Studio 17 2022" -A x64 -DA0_BUILD_STITCH_EVAL=ON
cmake --build build/eval-stub --config Release --target A0CameraStitcher.StitchEval
```

CLIは次の13個をすべて要求する。未知・重複・不足、整数の小数/指数/符号/先行zero表記を拒否する。

| option | 値 |
| --- | --- |
| `--camera-a` / `--camera-b` | 別々の7360×4912 JPEG。保存された画素順で読み、EXIF Orientationを適用しない |
| `--profile-file` | 校正済みdraftのschema 2.0.0 JSON |
| `--product-root` | 今回の製品保存ルート。既存の通常directory |
| `--output-directory` | 未使用の評価directory。親は既存の通常directory |
| `--left-um` / `--top-um` / `--right-um` / `--bottom-um` | 評価領域の端。文書左上を原点とする整数µm |
| `--dpi` | 1〜65535の整数 |
| `--width-pixels` / `--height-pixels` | 領域とDPIからceilで定まる画素数 |
| `--resampling` | `bilinear` / `bicubic-catmull-rom` |

以下は共通の合成fixtureの被覆部分を4×4へ出力する例。`$evaluationDirectory` は製品ルート・原本directoryと重ならない、未使用の通常パスを操作者が指定する。

```powershell
$draftProfile = (Resolve-Path tests/fixtures/rig-profile-v2/calibrated-draft.json).Path
& build/eval-stub/Release/A0CameraStitcher.StitchEval.exe `
  --camera-a $cameraA --camera-b $cameraB `
  --profile-file $draftProfile `
  --product-root $productRoot --output-directory $evaluationDirectory `
  --left-um 565000 --top-um 400000 --right-um 566000 --bottom-um 401000 `
  --dpi 100 --width-pixels 4 --height-pixels 4 --resampling bilinear
```

このcropは保存・記録の契約を検証するための例で、A0全領域の品質や実機受入の証拠ではない。

## 保存境界

profileは256 KiB、入力と出力のJPEGは64 MiBを上限とする。領域・DPI・ceil寸法、射影分母、放射単調性、被覆は既存のstrict readerと純粋な文書rendererで検証する。approved、雛形draft、所有者欄が非nullのdraftを拒否する。撮影ごとに新しいhomographyを推定しない。

通常のlocal fixed-drive absolute pathだけを受け付ける。祖先と入力をhandleで固定し、reparse・device・UNC・ADS・曖昧なcomponentを拒否する。製品ルートと入力directoryのどちらについても、評価出力との祖先/子孫の重なりを拒否する。短縮名やSUBSTの別名を含め、実体パスで比較する。明示した製品ルートとは別に、Windows既知folderのローカルアプリデータ内の製品ルートも除外する。

入力とprofileは処理中ずっとwrite/deleteを許さないread lockを保持する。出力directoryを排他的に予約し、自分が作成したpartialのhandleを公開まで保持する。既存fileを置換せず、flush、同じhandleの検証、非置換rename、再読を終えてから記録を確定する。失敗したcandidateは削除せず保持する。

## 評価記録

JPEG名は `evaluation.jpg`。JFIF units=1、X/Y densityは指定DPI、EXIF Orientationは書かない。JPEGのCOM markerに、ASCIIの `profileStatus=draft;quality=not-evaluated` を書く。同じバイナリ・WIC環境・入力・profile・ラスタ・kernelならJPEG byteが一致する。

記録は `stitch-eval.manifest.json`、schemaは [`a0.stitch-eval-manifest.v1`](schemas/stitch-eval-manifest.v1.schema.json)。nativeが適用したdraft snapshotのfingerprint、別入力のラスタ、kernel、入力alias/hash/サイズ、出力相対path/hash/サイズ/寸法、評価engineの版を記録する。絶対パスや撮影識別子は書かない。成功記録はJPEGの公開と再読が完了した後だけ作る。

ADR-0033§4で未決だった評価記録の欄名は `quality` とし、値は必ず `not-evaluated`。既存のcorpus recordの `qualityDecision` はそのschemaの契約を維持する。製品の `a0.stitch-job-manifest.v1/v2` は作らない。評価結果をprofile承認、品質合格、HG-0001の証拠として使わない。

成功stdoutは4行の `result=evaluated`、`profileStatus=draft`、`quality=not-evaluated`、`manifest=stitch-eval.manifest.json`。失敗はexit 2、stdoutは空、stderrに段階名だけを返す。

## 検証

```powershell
$env:A0_LEASE_TEST_STRICT = '1'
cmake --build build/eval-stub --config Debug
ctest --test-dir build/eval-stub -C Debug --timeout 1800 --output-on-failure
cmake --build build/eval-stub --config Release
ctest --test-dir build/eval-stub -C Release --timeout 1800 --output-on-failure
```

`m2_stitch_eval_default_off` は毎回新しいcacheで実際のprojectをoption未指定でconfigureし、全configurationのFile API codemodelに評価targetが無いことを製品targetの正常対照とともに確認する。`m2_stitch_eval_contracts` は製品/評価exeを別processで呼び、JPEG marker/DPI、記録、反復hash、入力不変、draft拒否と保存境界を独立に確認する。

SDKなしの全target buildと厳格lease付き全CTestはDebug/Releaseとも65/65 PASS。初期の既定OFF構成でも全targetのDebug buildが成功し、評価exeの成果物は0件。新しいcacheを使うFile API検査は両構成で通過し、全configurationに評価targetが無いことを確認した。

評価CLIの独立統合は両構成779項目PASS。両kernelの各2反復でJPEGとmanifestが一致し、原本とdraftのhashは不変。4件の実際のmanifestについて、Debugはschema正例と13種の負例変異を含む56項目、Releaseは正例4件がPASS。Debug/Release間でも4件のmanifest byteが一致した。

既知folderの製品ルート除外を外した試験用コピーでは、保護対象の子パス検査が狙った理由で失敗した。破損した所有fixtureのprofileを併用し、ガードが無い場合もprofile段階で停止するため、保護先を作成・削除せずに検出感度を確認できる。通常版はPASS、独立レビューに阻害事項はない。

詳細は#273の完了記録に残す。実写の校正・独立品質評価は#277/#275、圧縮後基準点のp95と画面接続は#274、補正pipelineは#46に残る。
