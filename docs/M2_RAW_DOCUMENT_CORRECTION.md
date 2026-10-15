# raw対応画素から文書補正への接続（#317、#46先行）

対応済みのCAM-A/CAM-Bの画素を固定校正で公称文書mmへ戻し、[#316の上限つき相似補正](M2_DOCUMENT_CORRECTION.md)へ渡すpure計算部。保存画像の合成を先に進める#306の続き。入力側の開発用マッチャーは[#318の固定写像に誘導されたpatch照合](M2_IMAGE_PATCH_CORRECTION.md)に追加した。profileの承認、製品adapterへの接続は含まない。

既存`a0_m2_document_correction`へ`raw_document_correction.cpp`を追加した。公開headerは`a0/m2/raw_document_correction.hpp`。STLとpure rendererだけに依存し、画像を読む処理、OpenCV、校正solver、SDK/WPD、ファイル、時刻には依存しない。独立画像生成器へのlink/source混入も禁止する。

## 入力と座標

`RawPixelCorrespondence`の両側は**保存JPEGの格納順における、rendererと同じcorner model**の画素座標。pixel `(i,j)`の中心は`(i+.5,j+.5)`。表示用のEXIF回転・反転は適用しない。異なる座標系の検出器は呼出側で変換し、点の対応IDと来歴も呼出側で保持する。

`RawImageExtent`は両画像それぞれの実際の寸法を必須とする。画素の範囲は`[0,width) × [0,height)`で、寸法は既存rendererの32768/200M pixel制限で検査する。このpure部自体はD810以外の寸法でも計算できるが、製品側の7360×4912条件を変更するものではない。

`OutputRaster`の**ceil端を含む閉じた外周矩形**を、両側の逆変換の許容文書domainとする。名目上のregionを超えるceil部分も含む。対象カメラの公称文書座標へ戻した結果がdomain外なら拒否する。domain外の点をclampしたり、別のdomainを自動推測しない。補正で画像端の点がdomainを出る場合、入力の作成側で初めからdomain内の対応点を選ぶ必要がある。

## 逆変換

`MapRawPixelToDocument(camera, raster, extent, raw_pixel, policy)`は次の順序で計算する。

1. camera/raster/寸法/policyを検査し、非有限・画像外のraw画素を拒否。
2. 固定HとKから、文書domainが写る正規化座標の四角形を包含する、原点中心の円盤を求める。四隅の計算は基本演算を外向きに丸めた区間で包む。
3. 円盤全域でレンズのJacobianが正定値になる保守的な十分条件を検査。証明できなければ`lens-domain-unavailable`。
4. 原点を初期値とし、円盤内に留まりraw画素単位の残差が厳密に減るNewton stepだけを採用。必要ならstepを半分にする。指定回数内に収束しなければ失敗。
5. 歪みなし画素から固定Hの逆を解き、許容文書domainを検査。二本の同次直線を個別に正規化して解くため、legacy pixel-spaceの絶対determinant閾値`1e-12`は使わない。
6. **既存製品投影関数**`TryProjectDocument`でraw画素へ戻し、実際のEuclidean往復誤差をもう一度検査。

逆変換で自由なHやK/歪み係数を再推定しない。候補解の分岐を恣意的に選ばない。

`RawInversePolicy`は全引数が必須で、既定値を持たない。

| 欄 | 意味 |
| --- | --- |
| `maximum_newton_updates` | 更新回数の上限、1〜1000。最後の更新結果も評価する |
| `maximum_backtracking_steps` | 各更新で試すstepの数、1〜64。最初は全step、その後1/2ずつ |
| `maximum_round_trip_error_pixels` | raw画素単位のEuclidean往復誤差の上限。有限・正 |

これは明示的な**数値計算policy**で、光学品質の受入値ではない。上限にepsilonを足さず、未収束点も近似値として公開しない。許容誤差が機械精度より小さい場合、妥当なモデルでも非収束や往復誤差超過になり得る。境界を丸めて内側へ移すこともしない。

## 一意性の保守的な検査

円盤半径を`R`、`t=R²`とする。radial Jacobianの二つの固有値は`1+k1*r²+k2*r⁴+k3*r⁶`と`1+3*k1*r²+5*k2*r⁴+7*k3*r⁶`。その両方に対して、下限として次を使える。

```text
1 - 3*max(0,-k1)*t - 5*max(0,-k2)*t² - 7*max(0,-k3)*t³
```

tangential Jacobianのspectral normは`6*hypot(p1,p2)*R`以下。その項も引いて**厳密に正**なら、対称Jacobianは凸な円盤全域で正定値となり、レンズ写像はその円盤内で単射になる。損失の各項も外向きに丸め、非有限や不確かな正の分母を許可しない。Newtonの試行点をこの円盤外へ出さない。

正のradial係数が負の係数を相殺する効果は十分条件に利用していない。このため、実際には単射でも拒否されるモデルがある。拒否は「レンズが実際に壊れている」という診断ではなく、この入力経路では一意性を確認できないという意味。既存rendererのforward投影や校正solverの逆変換の条件は変更していない。

## 両側の接続と結果

`EstimateRawPairDocumentCorrection(camera_a, camera_b, raster, extent_a, extent_b, matches, inverse_policy, correction_policy)`で両側を接続する。

- `points[i][0/1]`に、入力順どおりCAM-A/Bの理由・文書mm・往復誤差・更新回数を返す。文書mmは成功時のみ。
- 片側が失敗しても、他側と後続点の診断は続ける。**点を間引かず、全点成功まで`correction`を作らない。**
- 成功時、Aの公称文書座標を出力基準、Bの公称文書座標を対象として#316へ渡す。点数不足は既存の理由を返し、4096点超は変換前に拒否する。
- `correction->corrected_camera`がある時だけ描画可能な候補で、それはBのみの置換。A、固定H、K/5係数、入力対応点・画像・profileは変えない。
- 逆変換成功後も、縮退・反射・fit残差・変位/回転/倍率の上限超過・補正後の不成立な投影は#316の失敗理由と診断を保持する。

`RawDocumentOutcomeCode`は、非有限/画像外、レンズdomain未確認、非収束、数値破綻、文書domain外、往復誤差超過を区別する。無効なcamera/raster/extent/policyは`invalid_argument`。

候補の存在は対応点の正しいID、画像被覆・色/露出、重複幅、実写品質、approved profile/setup/撮影jobへの使用許可を証明しない。#316と同様、既存profileの`registrationErrorOutputPixels`を全域変位の意味に読み替えない。

## 再現と検証

```powershell
cmake --build build/reference --config Release --target m2_raw_document_correction_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_raw_document_correction_contracts$' -V
```

試験ではproductionの逆変換や投影に依存しないforward oracleから入力を作る。projective Hと5つすべてが非zeroの歪みを持つ両カメラ計234点で逆変換を検算し、別の63対応から既知の相似を復元、117点の独立投影でH*Tの向きを確認する。

実rendererの既知BGR配列で、両kernel・両layoutの既知画素、未補正の負の対照、反復byte一致と両原本bufferの不変を確認する。NaN/画像外/domain外、閉じたdomain端とceil端、1e-14 scaleのH、片側と複数点の失敗、過大な数値、radial折返し、tangential折返し、一意性検査の保守的拒否、更新/line-search回数、補正上限超過、重複、点数境界と4096点全数も確認する。

本試験の数値policyと既知データは合成試験用で、実写の品質受入値ではない。画像の特徴点検出/マッチング、対応の来歴、既存profileの承認値との対応、製品経路・実写の受入は後続。

2026-10-15、MSVC x64、SDKなし: 新規411チェックはRelease/Debugとも成功。独立真値からの逆変換の最大差は2.79359e-10 mm、実投影の往復誤差は8.11456e-10 px、raw対応から復元した補正の独立投影との差は2.19035e-11 px（両構成同値）。既存のrender・document render・MTF・v2 readerを含む関連CTestはRelease 7/7（11.55秒、生成器の依存分離22ケースを含む）、Debug 6/6（4.86秒）成功。全体suite・重い合成JPEG校正工程・実写は再実行していない。
