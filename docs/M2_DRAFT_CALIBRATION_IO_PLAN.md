# draft校正の入出力設計案（#285、#277前段）

この文書は次の実装を分けるための設計案。観測reader、検出器、solver、report/schema、draft writerはまだ実装していない。既存の拘束は[profile v2設計](design/rig-profile-v2.md)、[v2 schema](schemas/rig-profile.v2.schema.json)、[ADR-0034](DECISIONS.md#adr-0034-rig-profile-200を文書平面基準にしカメラごとのレンズ係数とmmdpiの出力ラスタを持たせる)（Accepted）に従う。新しい欄名や失敗コードはこの文書だけでは製品契約にならない。

## 1. 既存契約と今回の境界

- 製品はapproved profileの値を適用する。撮影ごとの自由なhomography推定、自動profile更新はしない。
- 校正の出力は両カメラ分の値がそろったdraftのみ。文書面・projection・calibrationをすべて入れ、所有者の決定であるoutputRaster、correctionEnvelope、qualityContract、approvalはすべてnull。physicalIdentityもnull。
- 同一カメラの内部パラメータを傾いた複数視点から求める。一枚の正対画像や同じ傾きの繰返しを「枚数が足りた」で成功にしない。[線形制約検査](M2_CALIBRATION_VIEW_GEOMETRY.md)は必要な前段であり、校正成功の証明ではない。
- approved発行・期限・再校正triggerは#45、実写リグ決定は#43。rigMeasurementsブロックと必須欄はdraftでも用意し、未実測の各測定値だけをnullにする。
- 今回は入出力と試験を設計する。既存schema/fingerprint、製品経路、実機条件を変更せず、OpenCVも導入しない。

## 2. 入力を三つに分ける

### 観測session（推定側）

sessionには画像・チャートの対応と検出結果を置く。次の表は論理的な欄の案で、JSONの名前・版・資源上限はreader/schema実装時に定義する。

| 入力 | 意味と検証 |
| --- | --- |
| sessionId / chartId | 公開してよい論理ID。PC名・serial・実撮影IDを使わない |
| chart points | fiducial IDとチャート面上の既知のmm座標。印刷/生成したチャートの定義に由来する。ID重複・非有限値を拒否 |
| camera groups | CAM-A/Bを明示。各groupで寸法・レンズ・焦点距離・フォーカスなど内部パラメータを変える条件を混ぜない。自己申告だけで物理同一性を証明したと扱わない |
| views | view ID、camera alias、画像のSHA-256、保存順寸法、利用する観測点ID、役割。学習用・独立検証用・固定リグ基準を明示 |
| observations | 検出した歪み込み保存順pixel座標。整数indexを中心とし、EXIF向きを適用しない。点ごとにdetected/missing/ambiguous等の状態を保持 |
| reference pair | 両カメラ各1枚の固定リグ基準view ID。同じ静止文書面・リグ状態の組で、文書写像をここで決める。学習viewとは役割を区別 |
| reference placement | 基準viewのチャート座標とA0文書座標の関係。未知の配置を自動で文書原点とみなさない |
| provenance | 測定日時UTC、tool ID/version、rigMeasurementsの欄。手測定値と推定値を混同しない |
| numerical policy | 最低視点数、rank判定値、solver停止条件などを外部から明示。品質合格の閾値と区別 |

各viewの点IDと座標を対にする。欠損をpixel (0,0)や推定位置で補わない。点数が足りても共線・重複で写像を決められない配置なら理由を残す。点対応の規則は検出器の公開入力として記録する。見た目が同じリングのIDは画像だけで自明ではないので、operatorによるID付きROI等を使う場合も、その入力と手順を記録する。

生成器の真値pixelを検出済み座標として渡したり、真値projectionから作った探索窓を無記録で検出器に渡したりしない。#275の期待位置付き評価窓は合成済み画像を測る入力であり、生カメラ画像のblind detectorの実装ではない。

初期のpure計算試験では画像検出と分けて、独立の解析的観測点を使ってよい。その結果には「解析的観測」と明記し、JPEGからの検出合格には数えない。draftのD810出力は7360×4912。小寸法の計算試験を実寸profileに偽って書かない。

### 真値/比較plan（評価側）

[生成器](SYNTHETIC_PAIR_GENERATOR.md)のground-truthは推定APIに渡さない。推定が完了してから別の評価adapterが、入力画像hashでsessionと結合する。学習側に渡せるのはチャートの既知mm座標であり、真のintrinsics、歪み係数、projection、image_pxは比較側だけに置く。

係数差だけを合否にしない。固定リグ基準の文書点について、推定値を合成した歪み込み保存順pixel写像と真値の写像を比較する。使った点・見えなかった点・比較不能点の数、pixel誤差のrms/max等を残す。閾値を内蔵せず、未検証・数値失敗を0誤差に置き換えない。

内部パラメータの学習に使わないviewを用意する。独立検証viewのposeを評価用に推定する場合は、そのpose推定に使う点と誤差を測る点も分ける。点ID集合と画像hashの重複を検査し、同じJPEGを別IDで登録しただけで独立にしない。同じfiducial IDが別viewに写ること自体は許すが、分離の単位（viewまたはview内の点）をreportに明示する。

これは#49のlocked holdoutではなく、開発用合成試験の分離。検証結果を見て再学習したら新しい試行として記録する。

### ローカルIO指定

画像の場所と出力先はローカルの指定として扱い、公開session例/reportに絶対パスを入れない。公開fixtureはsyntheticだけ。実画像・実画像hash・機体識別は公開repo/Issueに書かない。

将来のCLIは画像/設定を読み取り照合して、hashに結合した結果だけを保存する。既存製品rootや入力を上書きせず、新しい出力先で失敗時も診断を保つ。詳細なpath/file-lock/再読/公開契約は既存の[評価合成exe](M2_DRAFT_STITCH_EVAL.md)を参照して別のIO実装試験を設ける。

## 3. 多視点のposeと固定リグの写像

内部パラメータの学習用viewではチャートが傾いたり移動したりする。そこで求めたviewごとのposeを、そのまま固定リグのdocumentToImageにコピーしない。両カメラの内部パラメータを求めた後、明示されたreference pairのチャートと文書面の関係を使って固定写像を決める。

referenceのチャートは文書面と同じ平面上にあることを要件とする。mm座標間の既知配置は面内の回転と並進で表し、任意の射影変換で配置誤差を吸収しない。基準チャートを傾けたviewしかない場合は固定文書面への配置が不明なので、profileを発行しない。

行優先・列ベクトルで、Tを文書mm→referenceチャートmm、Hをreferenceチャートmm→歪みなし保存順pixelとする。profileの写像はH×Tを右下要素で正規化したもの。有限・非特異、右下を1にできることを確認する。Brown–Conradyをfx/fyで正規化して適用し、raw pixelへの再投影残差を測る。未知の文書位置や用紙の向きを推測で埋めない。

#283が受け取るのは歪みなしH。歪み込みの観測点へ直接homographyを当てた値は、その入力契約を満たさない。歪みなしの解析試験から開始し、歪みありでは候補のレンズ値によるundistortionとpose/Hの推定を明示した段階で検査する。初期化用の暫定Hでrankが5でも校正成功にしない。逆歪みが求まらない点を黙って除外せず、理由と点数を残す。

## 4. 出力とprovenanceの未決事項

論理出力は「診断report」と「成功した場合のみdraft profile」。reportはCAM-A/B各viewの画像hash、役割、使用/除外点、観測方法、数値policy、推定した内部パラメータとview pose、rank検査、残差、独立検証、真値との差、出力profileのfingerprintを結合する。失敗したstage以降の値はnull/未実施で表し、片方だけのprofileは作らない。

残差の集計は2D Euclidean pixel誤差e_iを使い、rms=sqrt(sum(e_i²)/N)、max=max(e_i)。Nと集計対象のview/点を必ず残す。profileのfitResidualPixelsはraw camera pixelのfit残差であり、独立検証や真値との差で代用しない。N=0なら0ではなく未計測。

**現行v2はinputSha256をCAM-A/B各1文字列しか持たない。多視点すべての画像hashをその欄に列挙できない。** schemaは未知欄も拒否する。この差を隠したdraft writerは実装しない。

| 選択肢 | 利点 | 必要な判断 |
| --- | --- | --- |
| 現行v2にはreference pairの実画像hashを残し、provenanceIdで全viewを含む別reportに結合 | profile構造と既存fingerprintを変えずに多視点の来歴を残せる | inputSha256の意味をreference pairに限定すること、別reportの版/結合/保管/移管を設計正本で確定する |
| profileの校正入力欄を拡張する新しい版を定義 | 単体profileに複数入力の来歴を持たせられる | schema/reader/C++/.NET/fingerprintの互換性と移行方針を決める |

前者を次の設計候補とするが、今回承認済みと扱わない。画像hash欄へsession manifestのhash・複合hashを代入する案は、現行の「画像のSHA-256」と違うので採らない。provenanceIdは照合の論理IDであり、文字列が一致しただけでreportの内容・画像の真実性を保証しない。結合方法を決めるまで完成したprovenanceとしてprofileを発行しない。

同様にfitResidualPixelsの集計対象を、全学習viewかreference pairかで曖昧にしない。全view別の値はreportに残し、profileへ入れる集合は上記の来歴の判断と一緒に正本へ記録してからwriterを実装する。今回既存fixture/readerの意味を変更しない。

## 5. 失敗と成功の区別（コード案）

| 診断 | profile |
| --- | --- |
| 不正なsession、画像hash不一致、未知/重複点ID、非有限値、同一画像の独立検証への流用 | 作らない |
| insufficient-observations / ambiguous-correspondence | 作らない。view/点の欠損理由を残す |
| #283の視点不足・縮退・不整合・numerical-failure | 作らない。特異値/rankの有無を保持 |
| solver-nonconvergence / nonphysical-intrinsics / invalid-distortion / reference-placement-missing | 作らない。成功値にclampしない |
| validation-not-evaluated / provenance-contract-unresolved | この案では作らない。検証や来歴を省いて成功にしない |
| draft-produced | 両カメラの校正値と記録がそろい、既存strict reader/schemaに通るdraftのみ |

検証が成立して誤差が大きい場合は生の数値を残す。外部の品質判定が未指定なら品質はnot-assessedであり、draftができても品質合格ではない。閾値超過を計測失敗と混同しない。資源上限・数値停止条件・品質判定の閾値は別の入力/診断として扱う。

## 6. 次の実装と受入試験

依存順に一つずつIssue化する。各段階で実装済みの範囲だけを完了とする。

1. 観測sessionのpure reader/validatorとschemaを定義。有限値、点対応、役割と画像結合、共線/重複、資源上限、校正と検証の分離を正負fixtureで検査。真値に依存しないpublic APIを作る。
2. 歪みなしの解析的な多視点観測からのK/pose推定を独立の既知幾何で検算。#283へ観測Hを接続し、正面/同じ傾き/異なる傾き/不整合を区別。真値Hの注入を経路分離で防ぐ。
3. 採用する開発用ライブラリの版/module/ライセンスと製品非同梱の境界を具体化してから、レンズ推定とJPEG検出を接続。pure入力の試験とJPEG end-to-end試験を区別。歪みなし→歪みあり→ノイズありの順で未使用点への再投影を測る。
4. reference pair/配置から固定文書写像を求める。異なる学習poseを固定写像に選んだ誤り、reference欠損、片方の失敗を負の対照にし、いずれもprofile非発行を確認。
5. 上記のprovenance/残差集計の判断を正本へ反映した後、report/draft writerを実装。既存C++ strict reader、schema、.NET fingerprint、#273との接続と非上書きIOを検証。approved欄の混入は拒否。
6. #277の受入に対応する合成試験を実施し、真値写像との差・失敗理由・draft provenanceを記録。これで実写や#45/#49まで完了したとしない。

今回の確認は既存schema/API/文書との照合、相対リンク、差分、独立レビューのみ。新しい計算・検出・校正の試験を実行した記録ではない。
