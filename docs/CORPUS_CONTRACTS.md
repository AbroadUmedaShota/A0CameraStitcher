# M2 corpus contract（software-only）

## 目的

Issue #44のうち、実画像を取得する前に確定できるcorpus管理契約を定義する。対象は、権利記録、匿名manifest、4種類のsplit、独立oracle、fail-closed validationである。

この契約の合格は、D810実写、A0品質、最終rig、合成品質、releaseの合格を意味しない。数値のA0品質基準はIssue #42、撮影rigはIssue #43の承認後に別途適用する。

## 保存境界

- リポジトリへ保存できるのは、自社作成のvector spec、匿名metadata、schema、testだけである。
- 顧客原稿、production画像、実カメラ識別子、serial、credential、Nikon SDK配布物を保存しない。
- 将来のD810 JPEG pairは権利確認済みの外部保管へ置き、manifestには匿名`externalObjectId`、SHA-256、rig／camera configuration／lighting profileの匿名IDとhash、撮影時刻だけを記録する。画像blob自体はcommitしない。
- `contentSha256`はassetのbyte列へ結び付ける。公開vector specはvalidatorが実ファイルから再計算する。

## 3つの契約

| 契約 | 内容 |
| --- | --- |
| `corpus-rights-record.schema.json` | 権利根拠、用途、review role、asset hash、禁止contentを記録する |
| `corpus-manifest.schema.json` | 匿名asset参照、split、汚染防止group、撮影条件profileのID/hash、期待結果、oracle参照を記録する |
| `corpus-oracle.schema.json` | 製品実装・製品metric・学習feature・製品出力から独立した期待結果を記録する |

すべて`additionalProperties: false`であり、未知fieldは受理しない。schemaで表現しにくいID重複、cross-reference、hash一致、split間group重複、role独立性はruntime validatorで拒否する。

## split分離

| split | 用途 | 製品調整への使用 | 変更 |
| --- | --- | --- | --- |
| `calibration` | profile calibration | 可 | 可 |
| `development` | 実装・failure sensitivity | 可 | 可 |
| `locked-holdout` | blind acceptance | 不可 | 不可 |
| `release` | release confirmation | 不可 | 不可 |

同じ`originalMasterGroupId`、`captureSessionGroupId`、`rigStateGroupId`、`derivationFamilyId`を異なるsplitへ置けない。派生fixtureは同一split内に閉じる。locked holdoutを見て製品を調整した場合、そのsetはlocked holdoutとして失効し、新しい独立setが必要になる。

## 独立oracle

oracleは期待結果を製品実行より前に宣言し、次をすべて`false`に固定する。

- production implementationの利用
- production metric出力の利用
- training featureの利用
- product出力から期待値を生成すること

例はcontract sensitivity専用で、正常specを`Accept`、宣言済みdamageを含むspecを`Reject`とする。A0品質の数値閾値や、実画像の合否を定義していない。

## 自動検証

```powershell
pwsh -NoProfile -File scripts/Test-M2CorpusContracts.ps1
```

検証対象は、Draft 2020-12 schema適合、権利とhash、split policy、cross-split contamination 0、oracle cross-reference、damaged fixtureのfailure sensitivity、privacy field除外である。uncleared rights、serial field、hash不一致、holdout tuning、rights欠落、production依存oracle、oracle不一致をnegative caseとして拒否する。

## 実画像を追加する前の残件

1. Issue #42でquality metric、判定方法、境界、sample数を承認する。
2. Issue #43でrig、lens、距離、角度、照明、再設置条件を承認する。
3. 権利確認済みの自社vector masterを実D810で撮影し、匿名化した外部objectとSHA-256を登録する。
4. calibration/development/locked-holdout/releaseのgroup重複が0であることを再検証する。
5. 実装担当と独立したreviewerがoracle、hash、rights、splitを確認する。

この文書とexampleだけではIssue #44全体を完了扱いにしない。実D810 corpusと独立した実画像oracleの証拠が追加されるまで`software-only / quality not evaluated`である。
