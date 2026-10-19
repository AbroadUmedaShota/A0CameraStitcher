# 保存JPEGの細探索つき評価（#324）

#325で[微小回転・倍率差を併用した保存JPEGの独立検証](M2_SIMILARITY_JPEG_EVAL.md)を追加した。以下は#324で追加したCLIの契約と当時の試験結果。

[#323の細探索](M2_PATCH_REFINEMENT.md)を開発用`A0CameraStitcher.StitchEval`の`--correction-job`へ接続する。新しい[correction job v2](schemas/stitch-correction-job.v2.schema.json)を明示した場合だけ有効で、既存job v1の粗探索や補正未指定の動作は維持する。製品adapter・通常アプリへ接続する変更ではない。

## 設定と形式

job v2は既存の`seedSelection`、`matching`、`inverse`、`correction`に、必須の`refinement: {"levels": 1〜8}`を加える。`schemaVersion`は`a0.stitch-correction-job.v2`。v1に`refinement`を追加したり、v2で省略したりした入力は拒否する。全項目を明示し、既定値で補わない。

64KiB・nesting 8・未知/重複/不足key・有限値・型・範囲・数字だけの整数表記の検査と、入力の祖先固定・read lock・snapshot・hardlink/alias検査を維持する。`levels: 7.0`や`7e0`もnative parserが拒否する。JSON schemaの数学的な整数型だけでは表記まで検証できない。

[人工JPEG専用の例](../tests/fixtures/stitch-correction/refinement-job.json)は接続試験の条件で、実写・承認済み光学条件ではない。同じCLI引数でjob fileを切り替える。任意の`--color-job`との併用も可能で、色補正は細探索で整列したBと同じ全seed集合を使う。

| 指定 | geometry report | final manifest / engine |
| --- | --- | --- |
| 補正なし | なし | v1 / 1.0.0 |
| correction job v1 | v1 | v2 / 2.0.0 |
| correction job v1＋色補正 | v1 | v3 / 3.0.0 |
| correction job v2（色補正は任意） | v2 | v4 / 4.0.0 |

[report v2](schemas/stitch-correction-report.v2.schema.json)は既存欄とhash結合を維持し、各pointの`forward`と`reverse`に`refinement`配列を追加する。粗探索で拒否された検索は空配列。各段は間隔、成否、最大位置、最大/次点NCC、最大target標準偏差、調べた/contrastを通った候補数を記録する。失敗した段も残し、それ以降は実行しない。

検索の`bestDocumentMm`・score・contrast・候補数は粗探索の診断のまま。検索の`outcome`は細探索を含む結果。最終位置は最後の細探索段で、両方向と往復検査を通った`rawCorrespondence`だけを後段の対応点に使う。report v1へ細探索診断を捨てて保存することや、成功した検索で必要段数を省くことはserializerが拒否する。

[manifest v4](schemas/stitch-eval-manifest.v4.schema.json)はgeometry report v2を使う評価を示し、reportの相対path・hash・byte数、job/profile hash、入力とJPEGを結び付ける。`colorCorrection`は任意で、併用時のcolor report v1はgeometry report v2のhashへ結合する。

終了値・stdout・成果物名・排他的保存・再読確認は[既存補正CLI](M2_CORRECTION_EVAL.md)の規約を維持する。数値的拒否はexit 3の診断のみ、形式/IO/render失敗はexit 2。成功時はJPEG・reportを確認した後、manifestを最後に公開する。**推定成功のreportだけではプレビュー成功ではない**。draft/not-evaluatedは品質合格や製品承認を意味しない。

## 検証の範囲

process-only harnessは補正core・renderer・profile readerへlinkせず、WICで7360×4912の人工JPEGを作り、実CLIを子processで実行する。連続三角関数の模様を元の画素位置で直接計算し、Bへ独立に`(+2.35,-1.65)` raw画素の移動を与える。productionのsampling/投影を使って入力をwarpしない。JPEGはquality 1・4:4:4、背景は一定とし、試験領域だけ二次元模様を与える。

両kernel・各2反復・gray/既知BGR色差の計8成功例について、両方向7段の全診断、既知変位、最終位置と固定投影からのraw対応、同じ全seedでの色補正、report/manifest/JPEGのhash結合、反復byte一致、原本/profile/jobの不変を確認する。未移動JPEG対の描画を基準に、粗探索だけの負対照と最終JPEGの画素差を比較する。

補正量超過、逆方向の誤差、白紙、追加候補を含む仕事量超過、逆方向の粗探索が曖昧な模様、整列後の色上限超過、無効なjob v2を拒否する。最初のJPEG試験で逆方向の粗探索NCC差が約0.0000497となり、明示下限0.0001で拒否された模様は、値を緩めず負例として残した。成功側には異なる二次元模様を使う。

```powershell
cmake --build build/reference --config Release --target m2_stitch_correction_job_contracts m2_stitch_eval_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_stitch_(correction_job_contracts|eval_contracts|correction_schema_contracts)$' --output-on-failure
# Debugでも同じbuild/CTestを行う。
```

純粋なjob/parser試験と実CLIのschema試験も追加した。schema試験は新しい14 geometry report、8 manifest、5 color reportと、それぞれの版/必須欄/段数/候補数/型/未知欄/path/hash/承認状態の負例を確認する。以前の43成果物は既存形式のまま保持する。

ローカル検証時、MSVC x64/SDKなし: pure job/parser/report各33チェック、実CLI各19,492チェック、schema各525チェックがRelease/Debugで成功。関連CTestは両構成12/12成功。保存された83成果物は構成間でbyte一致し、既存43成果物も変更前と一致した。最大対応点誤差はgray 0.0267000画素、色差あり0.0198874画素。最終JPEGの平均画素差は粗探索だけの1.20938〜1.22656 DNから0.414844〜0.495312 DNへ減少した。[当日の開発状況](DEVELOPMENT_STATUS_20261016.md)を参照。

これは人工JPEGでの接続検証。実写、一般の模様や圧縮条件、光学解像度、最終seam、製品の承認profile閾値との対応・通常アプリ・Live Viewは後続。
