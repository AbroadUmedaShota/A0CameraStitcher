# SingleCamera実機結果（2026-08-26）

## 判定

`Partial`。Nikon D810一台、`SingleCamera`、`CAM-A`のCamera Agent撮影経路は、one-shot、10回characterization、p95承認後の100回耐久まで合格した。一方、実WPF画面を使った100回end-to-end操作、撮影を挟むContinuous Live View handoff 10回、物理USB切断・保存先障害は未実施である。

この結果を`DualCamera`、A0合成品質、実シャッター同期、RAW/NEF、設定書込み、製品配布の合格証拠へ読み替えない。

## 確認した実績

| 項目 | 結果 | 測定値・安全結果 |
|---|---|---|
| software事前確認 | Pass | Release build、focused contracts、Camera Agent経路が合格 |
| read-only事前確認 | Pass | SDK/WPD各1台、`CAM-A` identity-v3、専用empty spoolを確認 |
| one-shot | 1/1 Pass | JPEG Fine / L / `7360×4912`、`.partial`からatomic rename、再読込・SHA-256検証、exact object cleanup、empty-after |
| 10回characterization | 10/10 Pass | p50 `14.036秒`、p95/max `14.643秒`、自動retry 0 |
| HG-0009 | Approved | Product Ownerが実測p95 `14.643秒`を承認 |
| 100回耐久 | 100/100 Pass | p50 `14.204秒`、p95 `14.430秒`、max `14.692秒`、全件初回成功 |
| 原画像再検証 | Pass | one-shot、10回、100回の計111 JPEGについて寸法・size・SHA-256を再検証 |
| 安全性 | Pass | 原画像消失0、誤削除0、曖昧画像採用0、自動retry 0、復旧不能停止0 |
| software failure recovery | Pass | `FailedPartial`、取得済み原画像保持、未検証object非削除、新transaction再開を確認 |

実カメラ識別子、serial、撮影画像、SDK配布物はリポジトリへ保存していない。実行証跡は実機PC上のリポジトリ外フォルダで管理する。

## 未完了

- 実WPF画面からの100回連続操作とfixed-local exportのUI証拠
- Continuous Live View停止・SDK完全終了・撮影・WPD回収・Live View再開の10回handoff
- 操作者による物理USB切断試験
- 保存先容量不足・書込み失敗の実環境試験
- DualCameraの実機1/10/100組、二台異常系、A0合成品質
- GitHub ActionsのBilling制限解消後のCI再実行

## 関連

- GitHub PR [#163](https://github.com/AbroadUmedaShota/A0CameraStitcher/pull/163)
- GitHub Issue [#7](https://github.com/AbroadUmedaShota/A0CameraStitcher/issues/7)
- GitHub Issue [#11](https://github.com/AbroadUmedaShota/A0CameraStitcher/issues/11)
- GitHub Issue [#12](https://github.com/AbroadUmedaShota/A0CameraStitcher/issues/12)
- GitHub Issue [#13](https://github.com/AbroadUmedaShota/A0CameraStitcher/issues/13)
