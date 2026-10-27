# 保存ペアの評価条件セット（#356）

カメラを使わず、明示選択した保存reviewとdraft校正・位置補正・任意の色補正・出力条件を保存し、同じペアで再評価する。#352の既存入口と#353の共通描画を使い、承認済みprofileの製品入口・未校正C・原画像・既存結果を変更しない。

## 保存と再読

従来の `scripts/Invoke-StillPairCorrection.ps1` の引数に `-SaveConditions -ConditionsRoot <既存root>` を追加すると、新規評価出力内へ `evaluation-conditions.json` を非置換で保存する。rootはprofile/jobの参照基準であり、対象のreviewは従来どおり明示する。root外の条件ファイルは保存できない。

再読は同じスクリプトへ次を渡す。profile/job・rasterを個別指定する従来の引数セットとは併用しない。

```powershell
./scripts/Invoke-StillPairCorrection.ps1 `
  -Report $SelectedReview -ConditionsFile $SelectedConditions -ConditionsRoot $SelectedRoot `
  -Inspector $Inspector -Evaluator $Evaluator -ProductRoot $ExcludedProductRoot `
  -OutputDirectory $NewEvaluationDirectory
```

実行exe・製品除外root・新規出力先は条件ファイルへ保存せず、呼出側が毎回指定する。既存出力の置換、最新ファイルの自動選択、別ペアへの条件流用はない。条件ファイルを別フォルダへ移しても、明示したroot内の同じbyte/hashを持つ参照だけを使う。

## データ契約

UTF-8のJSON、最大64 KiB、schemaVersionは `a0.saved-review-evaluation-conditions.v1`。全objectで未知・重複・欠落欄を拒否する。

| 欄 | 内容 |
|---|---|
| `productApproved`, `quality` | 必ず `false`, `not-evaluated` |
| `pairId`, `reviewReportSha256`, `reviewImageSha256` | 明示reviewの同一性 |
| `inputs` | CAM-A、CAM-Bの順でalias、原JPEGのSHA-256・encodedSizeBytes |
| `profile`, `correction`, `color` | root相対path、SHA-256、encodedSizeBytes。色なしはnull |
| `raster` | left/top/right/bottomUm、dpi、width/heightPixels、resampling |

hashは小文字hex64桁、sizeは正の整数。profileは最大256 KiB、補正jobは各64 KiB、原画像は各64 MiBの既存境界を維持する。整数は符号なし十進表記だけを許可し、小数・指数・overflowを拒否する。cropは正の面積、DPIと寸法は正、幅/高さはµmとDPIによる整数ceilingに一致させる。kernelは既存のbilinear / bicubic-catmull-romだけ。評価器の幾何・画像・補正上限等の検証も毎回実行する。

相対pathは `/` 区切りとし、絶対/UNC、親・現在成分、空成分、末尾space/dot、stream、Windows予約device名を拒否する。固定local driveの明示root内に解決し、既存のdirectory pinとread-only handleでreparse・置換・書込を拒否する。rootは明示選択であり、別ファイルや別rootへの自動探索は行わない。

保存は評価器が入力と補正指定を検証した後（数値的な補正拒否を含む）に、新規出力内だけへCREATE_NEW・flush・非置換rename・同じhandleでの再読を使う。保存成功は補正・品質の合格ではない。再読ではreviewと全原画像、profile/jobのhash/sizeを再照合し、条件だけから成功・承認状態を復元しない。

## 来歴と互換性

従来の個別引数による評価は `a0.saved-review-evaluation-binding.v1` を維持する。条件を保存/再読する場合だけbinding v2にし、`conditionSet` のsha256とencodedSizeBytesを結ぶ。保存した条件はartifactsにも追加する。読み込んだ条件は入力として保持し、以前の条件を上書き・再公開しない。どちらのbindingにも私有絶対pathを記録しない。

## 検証と残工程

`saved_review_evaluation_contracts` は別processの保存/再読を直接評価器のJPEGと比較し、固定の期待raster・入力hash・binding hashも検査する。幾何のみ/色併用、別pair、改変source、未知/重複欄、整数表記、ceil寸法、unsafe path、reparseと元データの不変を検査する。既存CIの追加接続jobでDebug/Releaseとも実行する。

通常画面への接続、承認/期限/実測対応の製品gate、実機Live/JPEG対応、設置後の校正と図面品質受入は別工程。条件保存をR2全体や製品合格として扱わない。
