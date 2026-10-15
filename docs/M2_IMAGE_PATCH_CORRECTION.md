# 固定写像に誘導された画像patch照合（#318、#46先行）

保存画像から補正用の対応点を得る、開発用pure計算部。指定した文書mm位置（seed）でCAM-Aの小領域を取り、固定校正付近のCAM-Bの画像内容を探索する。全seedの両方向照合が成立した場合だけ[#317のraw対応→文書補正](M2_RAW_DOCUMENT_CORRECTION.md)へ接続し、Bの一時的なH*T候補を返す。

公開headerは`a0/m2/image_patch_correction.hpp`、libraryは`a0_m2_image_patch_correction`。STL、pure renderer、既存のpure補正libだけに依存する。ファイル、JPEG decoder、OpenCV、機器、SDK/WPD、時計、校正solverには依存しない。製品adapter・アプリへはlinkしない。独立画像生成器へこのlibrary/sourceが混ざることもconfigure時に拒否する。

## 入力と処理

`EstimateImagePatchCorrection`はimmutableなdecoded BGR二枚、固定K/5歪み/H、出力raster、文書mmのseed列、照合/逆変換/相似補正の三つの必須policyを受け取る。JPEGの格納順のcorner modelは既存rendererと同じ。画像全体の特徴点検出やseedの自動配置・選別は行わない。

1. Aのseed周辺の正方形patchを文書mmで定義し、各sampleを固定投影でraw画素へ写す。既存bilinear samplingを使い、BGR三成分の算術平均を8-bit DNの強度とする。線形radianceや色の品質を推定したという意味ではない。
2. Bの同じ文書位置の周辺を、明示した文書mmの平行移動gridで探索。各候補のpatchもBの固定写像を通すので、両カメラの倍率・姿勢・歪みの既知の違いを利用する。
3. 各patchから平均を引き、L2 normで正規化した内積（zero-mean normalized correlation）を比較。sourceのpopulation標準偏差と、候補の標準偏差が明示下限未満なら利用しない。候補のcontrast不足は数を記録し、相関scoreを捏造しない。
4. 最大相関が下限以上、最大と次点の差が下限以上、かつ最大が探索gridの外周でない場合だけforward成立。比較できる候補が二つ未満なら曖昧として拒否する。同点を探索順で成功へ変えない。
5. 見つかったBの位置からAへ同じ探索を行い、戻った位置と元seedの文書mm距離を検査。reverseでもscore・contrast・次点・外周・被覆条件を省略しない。
6. 両方向成立時だけ両側の位置を固定写像でraw画素へ写し、#317へ渡す。全seedの成功が必要。逆変換/相似fit/各補正上限/補正後投影の既存検査もすべて通す。

すべてのpatch sampleがceil端を含む文書domain内で、raw画像の範囲内にあることが必要。探索途中に未被覆の候補があれば**そのseedを拒否**し、見えている候補だけの切り詰め探索を成功扱いにしない。画像端のrenderer既存bilinear規約は変更しない。

照合用sample列は最終画像として保存・再warpしない。最終描画はimmutableな元BGRと一時補正後のB投影を既存`RenderDocumentPair`へ渡し、一度のsamplingで行う。A、固定K/5歪み/H、profileを変更しない。

## 明示policyと資源上限

`ImagePatchPolicy`は全引数必須、既定値なし。光学的な受入値ではなく、開発計算の設定。既存profileの承認値との対応は未実装で、試験値を本番へ流用しない。

| 欄 | 単位・意味 |
| --- | --- |
| `patch_radius_samples` | patchの半径。1〜8、sideは`2*r+1` |
| `patch_spacing_mm` | patch sampleの間隔。有限・正の文書mm |
| `search_radius_steps` | 探索gridの半径。1〜32 |
| `search_step_mm` | 探索候補の間隔。有限・正の文書mm |
| `minimum_intensity_stddev` | population標準偏差の下限。有限・正の8-bit DN |
| `minimum_correlation` | NCC下限。有限、-1〜1 |
| `minimum_best_second_gap` | 最大scoreと次点との差の下限。有限、0より大きく2以下 |
| `maximum_reverse_error_mm` | 往復照合位置の距離上限。有限・非負の文書mm |

各比較の等号は通し、epsilonを足したり相関・補正値をclampしない。点ごとの最大scoreは浮動小数点の生の計算値として保持する。照合位置は**指定grid上のみ**で、subpixel最適化、回転/倍率を変えたpatch探索、自由H推定、RANSACはしない。小さな回転/倍率差でもgrid量子化・patch変形で不成立になる場合があり、一般画像での頑健性や精度を保証する実装ではない。

seed上限は256。候補sampleの仕事量を`seed数 * 2方向 * (2*r+1)² * (2*s+1)²`で上から見積もり、16,000,000超なら探索前に拒否する。source patchの読取りは別に各方向1回以内。この上限は資源制限であり光学品質条件ではない。

## 結果と拒否

`ImagePatchCorrectionResult`は入力順の`points`と、全seed成功時だけ`correction`を持つ。点数不足だけは既存のcount拒否を`correction`へ保持する。上限超過・不正policy/image/projection等も区別する。

各点はseed、最終理由、forward/reverseの理由・最大位置・最大/次点score・source標準偏差・探索先の最大標準偏差・比較/利用候補数・往復距離を持つ。forwardだけ成立しても、使えるraw対応`correspondence`は返さない。あるseedが失敗した後も、後続seedの診断を続ける。失敗seedや外れ値を捨てて残りをfitする処理はない。

`corrected_camera`が使用可能なのは、外側が`Estimated`で、`correction->correction->corrected_camera`があるときだけ。この投影は**Bだけ**の一時的な置換であり、Aは固定のまま。入れ子の拒否理由と測定量は#317/#316のまま保持する。

白紙、候補のcontrast不足、相関不足、反復模様、探索端、source未被覆、探索window未被覆、逆照合不一致、重複/非有限/domain外seedを区別する。無効な数値policy、画像buffer寸法不一致、無効な固定投影/rasterは`invalid_argument`。NCCと逆照合の通過だけで論理的な対応IDの正しさを証明するものではない。

## 再現

```powershell
cmake --build build/reference --config Release --target m2_image_patch_correction_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_image_patch_correction_contracts$' -V
```

テストexeへ**新しい**フォルダを一つ渡すと、合成JPEG二枚を保存・再読込みして照合する工程を実行し、補正前後のJPEGと`matching-report.json`を残す。日本語・空白・角括弧を含むパスにも対応する。出力先が既存なら上書きせず失敗する。これは内部試験用で、productionのtransaction publisherやprofile writerではない。

```powershell
& 'build/reference/Release/m2_image_patch_correction_contracts.exe' 'build/reference/画像照合 [318] Release'
```

reportは`synthetic=true / productApproved=false / qualityAssessment=not-assessed`を明記し、照合policy、既知/推定変位、score、往復距離、raw対応、JPEG試験の平均誤差を保存する。画像の撮影、機器操作、profile承認は行わない。

試験は独立した画素パターン、正負の既知変位、露出gain/offset、別々のaffine固定写像とradial歪みを持つ画像で対応を取得する。画像生成はproduction投影・逆変換に依存せず、後者はscalar bisectionと明示的なaffine逆を使う。誤った固定写像の負の対照も確認する。

実rendererでは両kernel・両layoutの修正結果、未補正の負の対照、反復byte一致、原本buffer不変を検査する。unsnappedな浮動小数点fitでcubic量子化の半整数境界を跨ぐ可能性があるため、基準画像との差は1 DN以内かつ未補正より100倍以上小さいことを要求する。repeat同士は厳密なbyte一致を要求する。合成JPEGの試験は別途、誤差が未補正の1/10未満になることを要求する。

seed自動配置/選定・汎用特徴点検出、探索精度の実写評価、承認profile値との対応、setup/撮影job来歴・アプリ本体への接続、露出/色補正、実写での受入は後続。

2026-10-15、MSVC x64、SDKなし: 新規90チェックはRelease/Debugとも成功。関連CTestはRelease 8/8（13.05秒、生成器依存分離26ケースを含む）、Debug 7/7（3.88秒）成功。明示した日本語・空白・角括弧の出力先へ保存したJPEGの再読込みも両構成で成功し、4 JPEG＋reportの全5ファイルがSHA-256比較でbyte一致した。

合成JPEG二枚（128×112）の6対応から既知の`(+0.3,-0.2) mm`を復元。補正後とCAM-Aだけから描画した基準との平均絶対誤差は0.077375 DN、未補正は10.3465 DN。これはこの合成試験のpixel差で、光学解像度・実写seam・色品質の受入ではない。記録は`build/reference/画像照合 [318] Release`と`build/reference/画像照合 [318] Debug`に残した。全体suite、7360×4912の重いJPEG校正工程、実写は今回再実行していない。
