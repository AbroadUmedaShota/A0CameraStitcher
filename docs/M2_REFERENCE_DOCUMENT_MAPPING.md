# 基準画像の組からの固定文書写像（#312、#277 段階4）

撮影画像の合成本体を先に進めるため、明示した二台のreference viewから、文書mm座標を歪みなし保存順pixelへ写す固定写像を求める。`a0_m2_calibration_lens` の開発用pure APIとして実装し、ファイル・profile・draft・approvedは出力しない。製品の撮影ごとの写像推定には使わない。

## 入力

`EstimateReferenceDocumentMapping(session, request, policy)` は、構造検証済みの不変な観測session、CAM-A/Bの基準view ID、既知の配置、数値policyを受け取る。

- `ReferencePairRequest.view_ids`: CAM-A、CAM-Bの順に、session内のview IDを指定する。それぞれのroleは`reference`であること。学習用・validation・別カメラのviewへ代替しない
- `DocumentPlanePlacement`: 文書mmからreferenceチャートmmへの面内回転（radian）と並進（mm）。`chart = R(angle) * document + translation`。配置不明を恒等変換とみなさず、optionalが無ければ`reference-placement-missing`
- `coplanar_confirmed`: チャートが文書面と同じ平面上にあるという入力上の確認。false、非有限の角度・並進は`reference-placement-invalid`。この値だけで物理的な同一平面を証明したとは扱わない
- `ReferenceMappingPolicy.lens`: 既存のレンズ校正policy。内部パラメータと5係数は、このsessionのtraining viewだけで推定する。外から渡された別sessionの推定結果は使わない
- `max_undistortion_iterations`: 逆歪みのNewton更新回数の上限（1〜1000）。`undistortion_tolerance_pixels`は有限・正のpixel誤差。どちらも数値停止条件であり、製品の光学・品質合格線ではない。既定値は設けない

## 計算と失敗

1. 二つのreference ID・role・aliasと配置を検査する。不正ならレンズ推定も始めず、`lens`と`mapping`を空にする
2. 既存の`EstimateLensIntrinsics`でKと歪みを求める。referenceはこの学習に入らない
3. 各referenceの`usedPointIds`を全て集め、raw pixelを逆歪みで戻す。一点でも収束しない・非有限・局所Jacobianが正でない・半径方向の単調性が満たされない場合は`undistortion-unavailable`。点を黙って落とさない
4. 歪みなし点に既存の正規化DLTとpose分解を適用して初期値にする。学習viewのposeは使わない。初期値が作れない・チャートが前方に無い場合は`pose-unavailable`
5. **Kと5係数を固定**したposeだけのLMでraw pixel残差を最小化する。停止条件は入力のlens policy。反復上限なら`solver-nonconvergence`、数値失敗なら`numerical-failure`
6. 行優先・列ベクトルで、`H = K [r1 r2 t]`、`T = 文書mm→referenceチャートmm`、`documentToImage = H*T`。右下要素を1に正規化し、有限・非特異であることを確認する。正規化不能な結果を補正して成功にしない

DLTとpose分解の共通入口は内部の`calibration_planar_pose.hpp`。既存の段階2の計算手順は変えない。逆歪み・poseの最適化は開発時だけの処理で、製品レンダラーの一回の再サンプリングには入らない。

## 結果

- `cameras`: alias・reference view ID・終了理由・成功時のposeとraw pixel残差（rms/max/点数）・LMの記録。片側の成功診断は反対側が失敗しても保持する
- `lens`: 同じ入力からの学習・validationの結果。referenceのためにKや係数を再学習しない
- `mapping`: **両側が成功した時だけ**二台分を返す。各項目にalias、reference view ID、reference画像のSHA-256、K、5係数、固定写像を持つ

`mapping-estimated`は数値計算が終わったという意味で、校正の承認・draftの発行・A0品質の合格ではない。残差に品質閾値を内蔵しない。画像hashも、sessionに記載されたものを結合するだけで実ファイルの再読検証ではない。

## 検証

既存の`m2_calibration_lens_contracts`に段階4の公開APIを通る試験を追加した。独立のカメラ投影式から作った解析的観測を用い、恒等配置と回転・並進配置の文書点をraw pixelで検算する。学習poseを誤ってコピーした負の対照が検出できること、referenceの変化でK・係数が変わらないことも調べる。

異常系は、reference欠損・role違い・alias入れ替え・配置欠損・同一平面の未確認・非有限値・逆歪みの反復上限・片側の学習失敗・合成行列のoverflow・不正policyを扱う。これは合成JPEGや実写の受入の代わりにはならない。

```powershell
cmake --build build/reference --config Release --target m2_calibration_lens_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_calibration_' --output-on-failure --parallel 1
```

provenanceとwriterは段階5（[入出力設計](M2_DRAFT_CALIBRATION_IO_PLAN.md)第4節）、approvedの発行は#45、実写の受入と品質の承認は#42/#43/#49に残る。
