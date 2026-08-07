# M2オフラインpre-gate

## 目的と境界

二台目D810、最終リグ、実写A0チャートがなくても検証できる純粋な幾何計算と公開テスト契約を提供する。ここでの出力は候補計算であり、DPI、画角、リグ、継ぎ目、色、A0品質の承認ではない。

- 入力値はすべて操作者が明示し、未承認の焦点距離、撮影距離、crop、品質閾値を既定値として固定しない。
- CLI出力には常に`approvalState: unapproved`と`qualityDecision: not-evaluated`を含める。
- OpenCV、実写画像、Nikon SDK/WPD、実カメラ識別子には依存しない。
- M2本体は`HG-0001`（A0品質）と`HG-0002`（撮影リグ）の承認後に進める。

## 光学候補CLI

`A0CameraStitcher.OpticalPlanner.exe`は、D810 Fine Lの`7360 x 4912 px`、A0の`841 x 1189 mm`を固定事実として使い、次を必須入力とする。

- 要求DPI
- 各frameを回転するか（`landscape` / `portrait`）
- 二枚を横または縦につなぐか（`horizontal` / `vertical`）
- 重複pixel数
- 合成後の四辺crop pixel数

```powershell
build\Debug\A0CameraStitcher.OpticalPlanner.exe `
  --dpi 180 `
  --rotation portrait `
  --layout horizontal `
  --overlap-px 982 `
  --crop-left-px 0 --crop-top-px 0 `
  --crop-right-px 0 --crop-bottom-px 0
```

横配置のusable幅は`2 x frame幅 - overlap - left crop - right crop`、縦配置のusable高は`2 x frame高 - overlap - top crop - bottom crop`で求める。A0要求pixel数は各辺について`ceil(mm x DPI / 25.4)`、有効DPIは幅と高さの低い方である。0 DPI、frame以上の重複、画像を消失させるcrop、整数overflowは入力エラーとして拒否する。

上記の仮定だけを置いた計算例は次のとおりである。

| 要求DPI | usable px | 必要px | 幾何候補 |
|---:|---:|---:|---|
| 180 | 8842 x 7360 | 8426 x 5960 | 寸法上は不足なし |
| 200 | 8842 x 7360 | 9363 x 6623 | 幅521 px不足 |

この表は、982 px重複、cropなし、frameを90度相当に回転して横へ並べる仮入力の結果にすぎない。実際の重複率、レンズ画角、歪み、解像力、被写界深度、照明、registration余白は評価していない。

## 公開テスト資産

- `samples/public/a0-synthetic-chart.svg`: 841 x 1189 mmの完全syntheticチャート。実写・顧客原稿・第三者チャートを含まない。
- `samples/public/m2-fixtures/`: 左右pixel pair、luma offset破損、source shift破損と、raw overlapの決定論的metric oracle。すべて`test-only` / `not-evaluated`である。
- `docs/schemas/rig-profile.schema.json`: draft/approvedを分離し、shapeと型を検査するDraft 2020-12 schema。schema単独で表現できないcross-field順序と評価時点での期限切れはruntime contractで検査する。
- `samples/public/rig-profile.draft.example.json`: 未承認値を`null`のまま保持する例。実識別子は保存できない。

`scripts/Test-M2PreGateAssets.ps1`はPowerShell組込みの`Test-Json`でschemaを正負例へ実適用し、draft/approved round-trip、version拒否、bounds拒否、draft非null／approved null拒否を検査する。production profile validator相当のruntime contractでは、provenance、`validUntil > measuredAt`、`measuredAt <= assessedAt < validUntil`、`targetMax <= autoCorrectionMax`も検査する。さらに、左右pairと二つの破損variantからoverlap、mean absolute error、max absolute errorを独立再計算し、manifestの期待値と照合する。検証前後のSHA-256比較によりfixture原本が変更されないことも確認する。

## 自動検証

### Setup-assistと自動補正範囲の契約

`a0_m2_setup`は画像処理や実機操作を行わない純粋判定境界である。profileのstatus、supported schema version、provenance、校正日時、有効期限、評価日時、全画角、カメラ設定整合、重複率と、位置・回転・倍率・露出・色の測定値、目標値、自動補正上限をすべて明示入力として受け取る。

- すべて目標内なら`ready`。
- 目標を超えても自動補正上限内なら`ready-auto-correction`。
- draft、unsupported version、provenance欠落、時刻順序不整合、期限切れprofile、画角不足、設定不整合、重複不足、自動補正上限超過は`physical-adjustment-required`。
- 閾値の既定値は持たず、NaN、無限値、負数、逆転した上限を拒否する。
- 判定は入力profileを変更せず、補正結果を固定profileへ自動反映しない。

このcontractの合格は判定ロジックだけを証明する。実際の画像から測定値を取得する処理、補正画像の生成、最終閾値、実写A0品質、二台固定transformの承認ではない。

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`m2_optical_contracts`が150/180/200 DPI、横／縦配置、crop、shortfall、invalid/overflow境界を、`m2_pregate_assets`が公開資産のguardを、`m2_setup_assessment_contracts`が三段階判定、上限境界、fail-closed入力、決定性、入力不変を検証する。これらの合格はM2 pre-gateソフトウェアの合格だけを意味する。
