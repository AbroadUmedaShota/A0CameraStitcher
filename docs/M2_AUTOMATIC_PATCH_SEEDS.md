# 画像被覆と二次元textureによるseed自動選択（#319）

[#318の画像patch照合](M2_IMAGE_PATCH_CORRECTION.md)へ渡す文書位置を、手指定せずに決めるpure開発工程。固定したgridの候補を**照合前**に評価・選択し、その集合を固定して#318→#317→#316へ一度だけ渡す。製品adapter、approved profile/setup/撮影jobへの接続ではない。

公開headerは`a0/m2/image_patch_seed_selection.hpp`。既存`a0_m2_image_patch_correction`へsourceを追加した。ファイル、OpenCV、機器、SDK/WPD、時計、校正solverに依存しない。#318の画像buffer/policy検査、文書patch sampling、強度平均とnormはprivate `image_patch_detail.hpp`へそのまま移し、同じ実装を共有する。

## 候補と被覆

`SelectImagePatchSeeds`は両BGR、固定K/5歪み/H、raster、選択/照合/補正policyを受け取る。ceil端を含む文書rasterを指定行列数で分け、各cellの中心を候補とする。候補はrow-majorで全件を保持する。重複や表現不能な位置を補完しない。

選択は**Aの画像内容と、両カメラの固定投影・画像寸法だけ**で決める。Bの画像内容、NCC、対応結果、実際の補正fitを見て候補を選び直す処理はない。Bのbuffer長は通常どおり検査する。

patch半幅を`P`、片方向探索の半幅を`S`とすると、Aにはseed周りの`2*S+P`、Bには`S+P`のsupportを要求する。Bで見つかる位置が最大Sずれ、そこからAへさらにS探索するreverse全域も含めるためである。

浮動小数点のnested加算を外向き区間で包み、文書domain内を確認。固定H、正の分母、K、radial/tangentialの5係数を通した連続矩形のraw座標も区間で囲み、全域が`[0,width) × [0,height)`内だと確認できる候補だけを残す。四隅だけの検査や、探索windowの切詰めではない。非有限・不確かな分母・境界の不確かさは被覆未確認として拒否する。

これは保守的な十分条件。区間依存性が強いモデルや端では、実際のsampleが全て見える場合でも除外し得る。選択後も#318の実sampleの被覆、両方向照合、#317の逆変換の一意性・往復誤差を省略しない。

## Textureと選択

Aのpatchを#318と同じ固定投影・bilinear・BGR算術平均でsampleする。population標準偏差を求め、照合policyのcontrast下限で検査する。各sample cellの左上からの前進差分`gx, gy`を使い、次の平均structure tensorを求める。

```text
G = mean([ gx*gx  gx*gy ])
         [ gx*gy  gy*gy ]
major = (trace(G) + hypot(Gxx-Gyy, 2*Gxy)) / 2
minor = det(G) / major   (major > 0)
axisRatio = minor / major
```

gradientはDN/sample間隔の差で、mmで割らない。固有値の単位はDN²。zero tensorはminor/major/ratioを0とする。微小な負の数値上のminorを正へclampしない。

minorとaxisRatioの両方に明示下限を要求し、一方向の縞・単一edgeなどを避ける。これは二次元の模様の尺度であり、反復模様や対応IDの一意性を証明しない。それらは後段で拒否される場合がある。

適格候補をminor降順、完全同点なら候補index昇順で並べる。既に選んだ点から明示最小距離以上なら、最大点数までgreedyに選択。間隔不足・点数上限で選ばれなかった候補も理由を残す。最適な可行部分集合を探し尽くすアルゴリズムではない。

選んだ文書点と同一文書点のidentity対応を既存#316へ一時的に渡し、最低点数・RMS spread・共分散のaxis ratioを検査する。**identityの投影を補正結果として公開しない。** 幾何的に縮退したら、別の点でやり直さず拒否する。

## 明示policy・結果

`PatchSeedSelectionPolicy`は全引数必須、既定値なし。

| 欄 | 意味 |
| --- | --- |
| `grid_columns`, `grid_rows` | 各1〜64、候補最大4096 |
| `maximum_seeds` | 最大選択点数、3〜256 |
| `minimum_spacing_mm` | 点間の最小文書mm距離。有限・正 |
| `minimum_minor_gradient_eigenvalue` | 二方向の弱い側のgradient energy下限。有限・正 |
| `minimum_gradient_axis_ratio` | minor/majorの下限。有限、0より大きく1未満 |

contrastは`ImagePatchPolicy`、点数と空間的な広がりは`DocumentCorrectionPolicy`をそのまま使う。等号は通し、epsilonを加えない。これらは開発用の入力で、既存profileの光学的承認値ではない。次段の16M candidate sample evaluations上限も継続して検査し、選択数を自動縮小して通さない。

結果は全候補のcolumn/row・文書mm・理由・texture測定値と、選択index列、selection/geometryの終了理由を保持する。選択geometryが失敗した場合も診断の点集合は保持するが、usable warpは一切返さない。

`EstimateAutoImagePatchCorrection`はinverse policyも最初に検査し、selection成功時だけ選択済みseedを#318へ渡す。照合失敗点を捨てたり、他候補への差替え・再選択・再試行はしない。全選択点が後段の全検査を通った時だけ、一時的なBのH*T候補が得られる。A、K/歪み/固定H、原本/profileは不変。

## 再現と限界

```powershell
cmake --build build/reference --config Release --target m2_image_patch_correction_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_image_patch_correction_contracts$' -V
& 'build/reference/Release/m2_image_patch_correction_contracts.exe' 'build/reference/自動照合 [319] Release'
```

明示した新規フォルダへの証拠保存modeでは、JPEGを保存・再読込みし、既存4 JPEG＋reportに`auto-corrected.jpg`と`auto-selection-report.json`を加えた計7ファイルを残す。auto reportは全候補の選択/除外理由と測定値、選択index、選択policy、補正後の平均画素値誤差を記録し、`synthetic=true / productApproved=false / qualityAssessment=not-assessed`を明記する。これは試験用の証拠保存で、製品のpublisherではない。既存フォルダは上書きしない。

検証は手指定seedなしの正しい変位と実renderer、異なる固定写像/歪み、保存・再読込みした合成JPEG、白紙/軸方向と斜めの縞/二次元反復模様、片側被覆不足、選択後の一部照合失敗と選択不変、点数/間隔/縮退/最大4096候補・256点を含む。独立3×3 sampleのtensor `[[8200,100],[100,8200]]`の固有値8100/8300も検算する。完全同点の順位、間隔・minor・axis ratioの等号/nextafterを確認する。

汎用画像でのseed選択の成功率、subpixel精度、回転/倍率を変えた探索、実写での品質、製品adapter/setup/job/承認profileへの接続、露出/色補正は後続。固定gridが有用な模様を拾えなければ、その画像では不成立になる。実機操作・製品の品質承認は追加しない。

2026-10-15、MSVC x64、SDKなし: 既存90＋追加78の計168チェックはRelease/Debugとも成功。関連CTestはRelease 8/8（13.62秒、生成器依存分離28ケースを含む）、Debug 7/7（7.66秒）成功。共有sampling関数の移動後も既存#318のJPEG測定値は不変。

保存・再読込みした合成JPEG二枚（128×112）では20候補から6点を自動選択し、既知の`(+0.3,-0.2) mm`を復元。CAM-A基準の描画に対する平均画素値誤差は自動選択で0.07725 DN、未補正は10.3465 DN。両構成の計7成果物はSHA-256比較でbyte一致し、`build/reference/自動照合 [319] Release`と`build/reference/自動照合 [319] Debug`に保持した。実写の光学品質評価や、全体suite・7360×4912の重い校正工程の再実行ではない。
