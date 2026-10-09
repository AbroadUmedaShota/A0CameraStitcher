# 校正入力の多視点・線形制約の検査（#283、#277前段）

`a0_m2_calibration_view_geometry` はカメラ1台分の複数視点について、平面校正の線形制約を調べる純粋なC++20 library。画像処理・OpenCV・Win32・製品renderer/stitcher/生成器/profile readerには依存しない。profileは作らない。#277の検出・レンズ校正・draft出力の前段で使うための検査であり、その作業全体の完了ではない。

## 数学と入力の意味

Zhang の [A Flexible New Technique for Camera Calibration](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/tr98-71.pdf) の3.1節に従う。入力Hは文書mmから歪みのない保存順pixelへの3×3 homography（行優先）。同一カメラ・同一の内部パラメータの視点をまとめる。接続時は検出点から求めた観測Hを渡し、生成器の真値Hで代用しない。このlibraryは入力Hの来歴や観測誤差を検証しない。

Hの第1・第2列を h1,h2 とし、6未知数を `[B11,B12,B22,B13,B23,B33]` の順で持つ。各視点から `v12` と `v11−v22` の2行を積み、ADR-0034のskew=0から `[0,1,0,0,0,0]` を1行追加する。入力がN視点なら行数は `2N+1`。十分な独立制約がある場合、6未知数の同次解はscaleを除いて一意となり、数値rankは5になる。

skew=0が明示されているため、異なる傾きの2視点でも足りる場合がある。ただし2枚あれば足りるという条件ではない。同じ平面の法線を保つ平行移動・回転や、同じ傾きの繰返しは独立制約を増やさない。実際のrankで検査する。

全視点に同じ画像座標の正規化を適用する。中心は `((width−1)/2,(height−1)/2)`、等方scaleは `max(width,height)/2`。これは数値計算の条件を整える変換で、カメラの焦点距離や主点を仮定する値ではない。視点ごとに別の正規化をすると共通の内部パラメータの問題が変わるので行わない。h1,h2は共通scaleで扱い、`v11−v22` を作った後に各制約行をL2正規化する。

特異値はVを直接処理するone-sided Jacobi SVDで求める。`VᵀV`の固有値にして条件数を二乗しない。反復は上限付き。収束判定には浮動小数点の丸め精度だけを使い、呼出側のrank判定値とは分離する。丸め精度付近で収束のために無視した列が残る場合、指定した判定値でその列の特異値を確かめられるかも検査する。極小の判定値で未確定の誤差成分をrankに数える代わりに `numerical-failure` とする。値を0に丸めてrankを小さく見せる処理はしない。

## 必須の外部入力と結果

`AssessViewGeometry(views, options)` に画像寸法、最低視点数、特異値の相対判定値を明示する。最低視点数は2〜128、相対判定値は有限で厳密に0より大きく1より小さい。qualityの閾値や既定値は置かない。特異値を降順で返し、`singularValue > cutoff × largestSingularValue` の数をrankとする。境界と等しい値は数えない。

| status code | 意味 |
| --- | --- |
| insufficient-views | 指定した最低視点数に達しない |
| insufficient-view-diversity | 数値rankが5未満で、一意性を示す線形情報が足りない |
| inconsistent-linear-constraints | 数値rankが6。指定した許容の下では共通の同次解が残らない |
| linear-constraints-sufficient | 最低視点数を満たし、数値rankが5 |
| numerical-failure | 有限で信頼できる特異値を求められない、または反復が収束しない |

視点不足でも計算できた特異値とrankを残す。視点0の場合も既知のskew制約の1行があるのでrankは1。数値失敗のときだけ特異値とrankをnullにする。入力検証は最低枚数の判定より先に行う。不正なHを「画像が少ない」で隠さない。

128視点以下、各辺32768以下。IDは128文字以下の英数字・`_`・`-`で重複禁止。非有限・零・特異なHや不正なoptionは `invalid_argument`。Hの特異性は、入力binary64の仮数と指数を有限長整数として扱い、3×3行列式の6項の和を正確に比較する。浮動小数点のpivot誤差で、行列式0の入力が通ることを防ぐ。任意の非零scaleや文書単位の等方scaleで表される同じHは同じ線形制約を持つ。binary64への丸めが加わるため、数値判定は指定した許容の範囲で検証する。文書の異方scaleは別の問題なので、この不変性の対象にしない。

`linear-constraints-sufficient` はレンズ校正の成功を意味しない。Bの正定値性、物理的な内部パラメータ、レンズ係数、未使用点の再投影誤差、被覆、JPEG後の品質、rig/provenanceの妥当性は後続で検証する。異なるレンズや焦点距離を混ぜた入力が常にrank6になる保証もない。#277ではカメラ・撮影条件の同一性を別に確認する必要がある。

## 検証と利用条件

独立に `H=K[r1 r2 t]` を組み立てた解析的なカメラで、正面・同じ傾き・異なる傾き・不整合を試す。任意scale、入力順、等方文書単位の変更、相対判定値への応答、不正入力も試す。合成値の判定値は試験に明示した入力で、実写の推奨閾値ではない。

CMakeの実configure試験はpure libraryの正の対照と、Win32、製品計算、OpenCV（version suffix/imported target/defaultlibを含む）の直接・推移・source混入の拒否を確認する。共有したリンク検査の既存generator/evaluatorの試験も再実行する。

#277の実装方式は引き続き別の作業。今回OpenCVをインストール・リンクしていない。利用条件の一次資料は [OpenCV License](https://opencv.org/license/) と [商用利用に関する公式説明](https://opencv.org/blog/opencv-is-to-change-the-license-to-apache-2/)。4.5以降はApache 2.0。リポジトリの `HG-0005` は `blocking_scope=release`、`Before packaging` の製品再配布判断であり、この検査で承認済みに変更しない。#277に残る開発利用の確認事項と、製品へ同梱する判断は別々に記録する。
