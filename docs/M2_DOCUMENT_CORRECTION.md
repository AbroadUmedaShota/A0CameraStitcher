# 文書座標の上限つき相似補正（#316、#46先行）

撮影ごとの差を、平行移動・回転・等方倍率の4自由度だけで表し、固定文書写像へ合成するpure計算部。入力は**対応済みの文書mm座標**で、画像から点を検出する処理ではない。既存の製品adapterや撮影の入口にはまだ接続しない。

`a0_m2_document_correction`（`document_correction.cpp`、公開header `a0/m2/document_correction.hpp`）はSTLと既存のpure rendererだけを使う。ファイル、OpenCV、Nikon SDK、機体、時計、profileの読込み・書換えには依存しない。

## 座標と合成の向き

`DocumentCorrespondence`の一組は次の意味。

- `document_mm`: 出力文書における対応点
- `camera_document_mm`: 補正対象カメラの固定写像・レンズモデルの逆を通した、公称文書座標における同じ点

raw camera pixelをこの欄へそのまま渡さない。[#317のraw画素→文書座標の接続](M2_RAW_DOCUMENT_CORRECTION.md)が、対応済みの両画像の画素からこの入力を作る。画像上の対応点検出は後続の入力側の責務。座標だけから論理的な点IDの正しさは証明できず、対応の来歴も呼出側で保持する。

推定するのは`q = T*p`。

```text
T = [ a  -b  tx ]
    [ b   a  ty ]
    [ 0   0   1 ]
scale = hypot(a,b), angle = atan2(b,a)
```

全点の平均を引き、座標の大きさを正規化して、反射を含まない相似の最小二乗解を求める。自由なhomographyや非等方の拡大・shearは推定しない。RANSACや外れ値除去、欠損点の補完もしない。

固定の文書→歪みなし画素の写像を`H`とすると、補正後は**`H*T`**。画素に左からTを掛ける向きや逆相似ではない。KとBrown–Conradyの5係数は保持する。結果は右下を1に正規化した一時的な`CameraProjection`で、既存`RenderDocumentPair`の1回のsamplingへ渡せる。profileの値を更新・保存しない。

## 必須policy

`DocumentCorrectionPolicy`は全引数を要求し、既定値を持たない。

| 欄 | 意味 |
| --- | --- |
| `minimum_points` | 最低点数。3〜4096 |
| `minimum_rms_spread_mm` | 両点集合に必要な、重心からのRMS距離。有限・正 |
| `minimum_axis_variance_ratio` | 両集合の共分散の小固有値/大固有値の下限。有限、0より大きく1未満 |
| `maximum_fit_residual_output_pixels` | 全対応点の最大fit残差の上限。文書mmの差を出力pixelへ換算 |
| `maximum_displacement_output_pixels` | 出力raster全域における`T*p-p`の長さの最大値の上限 |
| `maximum_rotation_degrees` | 回転の絶対値の上限。0〜180度 |
| `maximum_scale_difference_percent` | `abs(scale-1)*100`の上限 |

予算は有限・非負。mmから出力pixelへの換算は、1 inch = 127/5 mmの厳密な単位関係から`dpi*5/127`を使う。pixel pitchを一度丸めて逆数を取る誤差を避け、境界の判定へepsilonを足さない。

変位の長さはaffine変位のノルムなので凸であり、矩形上の最大値は四隅で求まる。`DocumentRasterCorners`は既存rendererのdomain検査と同じ矩形を返し、指定regionを整数ceilした際に広がる端も含む。fitした点や指定region内だけを見て上限内にしない。

これは開発用計算の明示policy。**既存profileの`registrationErrorOutputPixels`を、この新しい「全域の最大変位」と同じ意味だと読み替えていない。** 製品側の各profile欄・setup判定・品質gateとの対応づけ、承認済み値の取得は後続。試験用の予算を実機の承認値として使わない。

## 結果と失敗

`EstimateBoundedDocumentCorrection(fixed_camera, raster, correspondences, policy)`は以下を返す。

- `outcome`: 計算の終了理由。成功候補は`correction-estimated`
- `estimate`: 算出できた場合の符号つき相似係数。上限超過時も診断として保持
- `metrics`: 全点のfit残差rms/max・点数、全域の最大変位、回転、倍率差
- `violations`: 同時に超過したfit・変位・回転・倍率の理由を保持
- `corrected_camera`: **すべての予算と補正後の投影検査が通った時だけ**存在

上限と等しければ通し、超えた値をclamp・丸め・微小なidentityへ置換して通さない。入力点が完全に同じ場合だけ、厳密なidentity解を保持し、ゼロ予算でも不用な丸め誤差を作らない。identityでも点集合の広がり・縮退の確認は省かない。

点数不足、4096点の資源上限超過、非有限/重複座標、共線・広がり不足、反射対応、数値破綻は理由を返し、適用可能な写像を返さない。無効なpolicy・元の投影・rasterは`invalid_argument`。補正後に分母が0/負、非有限・特異、レンズ写像が折り返す場合も`projection-unavailable`等で拒否する。既存rendererの正規化とdomain検査を緩めない。

`estimate`があっても、`corrected_camera`が無ければ描画へ使わない。候補の存在は、画像の被覆、重複幅、色/露出、品質、approved profileの使用許可、撮影・jobの来歴を判定したという意味ではない。

## 検証

```powershell
cmake --build build/reference --config Release --target m2_document_correction_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_document_correction_contracts$' --output-on-failure
```

試験は独立に作った既知の変換で4係数と単位を検算する。正負の回転、倍率、translation、厳密identity、1e-160〜1e160の座標、4096点境界、反射/縮退/重複/欠損/外れ値/数値破綻、各上限の等号と1 representable stepの超過、同時超過、ceil端・分母・レンズの折返しを含む。

歪みとprojective Hを含む231点の独立投影oracle、および実rendererの既知画素で合成方向と単一samplingへの接続を確認する。既知画素の試験は両kernel・両layoutで行い、未補正の負の対照、反復byte一致、入力画像と固定写像の不変も確認する。生成器にこの補正コードが混ざらない依存検査も追加する。

2026-10-15: MSVC x64、SDKなし。新規304チェックはRelease/Debugとも成功。231点の独立oracleとの最大差は約1.4e-12 px（両構成同値）。既存のrender・document render・MTF・v2 readerを含む関連CTestはDebug 5/5（7.94秒）、Release 6/6（106.64秒、生成器の依存分離20ケースを含む）成功。実写や全体suiteを再実行した記録ではない。

raw→公称文書の逆変換と両側の接続は#317で追加。実画像の対応点検出、外れ値選別、承認profile/setup/撮影jobへの接続、露出/色、実写での受入は#46の後続として残る。
