# 上限つき明るさ・色補正の計算部（#321、#46先行）

幾何補正後の対応色から、CAM-Bだけの一時的なBGR別gain/offsetを求める開発用pure計算部。FR-STI-004と#306の「位置に加えて明るさ・色も合わせる」の次の工程。公開headerは`a0/m2/color_correction.hpp`、libraryは`a0_m2_color_correction`。STLとpure rendererだけを使い、OpenCV・機器・file/decoder・時計・profileへ依存しない。

## モデルと入力

格納順の8-bit BGRの数値（DN）で、各channelを独立に次の形で合わせる。

```text
CAM-A[channel] ≈ gain[channel] * CAM-B[channel] + offsetDn[channel]
```

線形radiance、ガンマ推定、ICC/色差規格、3×3色混合、空間的な周辺減光を扱うモデルではない。実写の光学品質合格や、全原稿の色再現を証明しない。

`EstimateBoundedColorCorrection`は**対応済み**の色の組を全点使う。各channelで平均を引き、両側のL2 normと正規化内積から最小二乗のgainを求め、平均からoffsetを求める。`hypot`によるnormを使い、極小信号の二乗のunderflowを避ける。非有限な計算結果は拒否する。

`EstimateImageColorCorrection`は文書mmのseed、固定CAM-A写像、**幾何補正後のCAM-B写像**から両原画像の色をbilinearで観測する。文書domainはrendererと同じceilラスタの外周まで。seedの重複・非有限・domain外・どちらかの未被覆では、残りの診断を続けるがfitへ進まない。成立した点だけを残す処理はない。最大4096点。

この工程は位置合わせや対応IDの正しさを推定しない。幾何補正が不成立だった場合に、色が似ていることを理由に続行しない。入力seedの選択とその来歴、実写での分布の妥当性は呼出側が保持する。

## 明示policy

`ColorCorrectionPolicy`は全引数が必須で、承認済み値や光学上の既定値を持たない。

| 欄 | 意味 |
| --- | --- |
| `minimum_samples` | 最低対応数。3〜4096 |
| `minimum_sample_dn` / `maximum_sample_dn` | 両側・全channelの観測を許すDN範囲。0〜255内の非空区間。飽和marginも呼出側が明示する |
| `minimum_stddev_dn` | 各channelの両側に必要なpopulation標準偏差。有限・正 |
| `maximum_gain_difference_percent` | 各channelの`abs(gain-1)*100`の上限 |
| `maximum_offset_dn` | 各channelの`abs(offsetDn)`の上限 |
| `maximum_residual_dn` | 全点・各channelの最大fit残差DNの上限 |

上限は有限・非負。等号は通し、epsilonを加えず、超過した係数をclampしない。全対応色が厳密に同じ場合だけgain=1/offset=0を保つが、contrast検査は省略しない。白一色から二つの係数を推定しない。

## 結果

`ColorCorrectionResult`は点数、channel別の両側標準偏差、推定値、残差rms/max、channel付きの全上限違反を保持する。不正な観測またはsignal範囲外は最初のsample index/channelを返し、点を捨てない。contrast不足、非正gain、数値破綻、gain/offset/残差超過を区別する。

`transform`が存在するのは**全channel・全対応・全上限が成立した時だけ**。上限超過でも推定値と測定量を診断として保持する。`transform`の存在を、profile承認や品質合格と解釈しない。

## 単一samplingの描画への接続

既存`RenderDocumentPair`と`RenderMappedPair`へ、明示的な`BgrAffineTransform`を受け取るoverloadを追加した。既存の引数だけの呼出しは従来どおり。

元画像を既存kernelでsamplingし、CAM-Bのsampleへgain/offsetを適用してから既存featherで合成する。中間の補正画像を作らず、CAM-A・固定profile・原本bufferを変更しない。CAM-Bしか寄与しない場所では補正後のB、CAM-Aだけの場所では従来のAを使う。coverage、重複band、seam位置を変更しない。

samplingと色補正によるovershootはblendまで保持し、最後に8-bitへ量子化する。これは既存の画素出力の契約で、係数の上限超過を成功に変える処理ではない。renderer単体は係数の数学的検査のみを行い、承認値を判定しない。非正/非有限のgain、非有限offset、計算のoverflowを拒否する。有限の大きな値でも`lround`の整数overflowを起こさないよう、最終量子化でdoubleのまま0〜255へ収める。

本体の承認profile・製品adapter・通常アプリ・#320のCLI/job/report形式は今回変更しない。飽和画素数・全画面のclipや色品質の受入、実写のモデル妥当性、承認profile値への結合と永続的な来歴は後続。

## 再検証

```powershell
cmake --build build/reference --config Release --target m2_color_correction_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_color_correction_contracts$' -V
# Debugも同じtarget/CTestで実行する。
```

独立した既知gain/offset、厳密identity、点数/信号/contrast/予算の境界と`nextafter`、非有限・負gain・白紙・飽和・非線形・外れ値、全違反の保持、4096点、seedの重複/domain/被覆/ceil端、入力不変を確認する。描画は両kernel・両layoutで、独立したlinear rampの既知画素と、未変更CAM-Aの基準描画へ照合する。幾何補正`H*T`との併用も確認する。

補正前のgap probeは平均誤差20.3302 DNで失敗。追加後は同じ人工画像で0 DN。これはdecoded bufferによるソフト検算で、今回の新しい色補正を保存・再読込みJPEGや実写で受入評価したものではない。

2026-10-15、MSVC x64/SDKなし: 新規152チェックはRelease/Debugとも成功。両kernel・両layoutの人工画像の平均誤差は、補正前19.851〜20.3302 DNから補正後0 DN。関連CTestは両構成12/12成功（Release 58.70秒、Debug 69.50秒）。既定OFF、生成器の依存分離36ケース、既存renderer/profile/幾何補正、実CLIとschemaを含む。

色補正を指定しない従来の評価CLIについても、成功/拒否のreport・JPEG・manifest・試験用profile/jobを含む全21成果物が、両構成とも#321変更前とbyte一致した。#320の未commit変更は保持。全体suite・重い18枚の校正・新しい実機系列の再実行、mainへの反映は行っていない。
