# pilot 用チャート（A0 版下 2 系統）と印刷

Issue #276。道筋は #278、撮影の許可と上限は #233（所有者の決定、2026-10-08）。撮影の手順は [PILOT_CAPTURE_PROCEDURE.md](PILOT_CAPTURE_PROCEDURE.md)。

この文書とチャートは撮影の準備物であり、合成品質・校正精度・A0 品質の合格を意味しない。

## 1. 版下

| 項目 | development 用 | holdout 用 |
| --- | --- | --- |
| 版下（正本） | `samples/public/pilot-chart/a0-pilot-chart-development-v1.svg` | `samples/public/pilot-chart/a0-pilot-chart-holdout-v1.svg` |
| vector spec | `samples/public/corpus-contracts/vector-specs/pilot-chart-development-v1.json` | `samples/public/corpus-contracts/vector-specs/pilot-chart-holdout-v1.json` |
| 印字する ID | `A0CS-PILOT-DEV`、VERSION 1（`A0CS-PILOT-DEV v1`） | `A0CS-PILOT-HOLD`、VERSION 1（`A0CS-PILOT-HOLD v1`） |
| corpus の split | `development` | `locked-holdout` |
| 用途 | pilot（較正 12 組・開発 24 組）と以降の開発 | パイプライン凍結後の holdout 撮影だけ。pilot では使わない |

どちらも `samples/public/a0-synthetic-chart.svg` を元に自社で作図した。座標の原点は左上、単位は mm、縦置き（portrait）、用紙は A0（841 x 1189 mm）。SVG の `width`/`height` は `841mm`/`1189mm`、`viewBox` は `0 0 841 1189` で、SVG の 1 単位がそのまま 1 mm になる。

### 共通の要素

- 外枠（x 20〜821、y 20〜1169）と格子（細線 0.25 mm・太線 0.6 mm）
- 基準点（fiducial）: 四隅の大きい基準点 F1〜F4（外径 32 mm、十字 44 mm）と、左上だけに置いた非対称用の F5。重なり帯の小さい基準点 B01〜（外径 20 mm、十字 28 mm）。各基準点の脇に ID を印字
- 重なり帯の目印: 上下の外枠の内側に、x 350・491（重なり 141 mm、#43 の v0.1 候補）と x 360.5・480.5（最小 120 mm）の目盛り。2 台を portrait で左右に置き、基線 534 mm で中央に重なる想定の位置
- 向きと左右: 左上の L 字の黒い角マーク、上辺中央の三角と `TOP`、左に `L`・右に `R` の大きな文字。180 度回すと角マークが右下へ、左右反転すると右上へ移り、文字も裏返るので見分けられる
- 円（同心円と中心点）、斜めエッジ（数度傾けた黒い正方形）、細線群（線幅 0.1・0.2・0.3・0.5・1.0 mm、各 3 本、線幅と同じ間隔）、重なり帯を横切る直線（水平と斜め）
- グレー 6 段（#000000〜#ffffff）と色 6 色。重なり帯の中にも 1 組ずつ置き、2 台が同じパッチを写すようにした（2 台の露出・色の差を見るため）
- 寸法検証用: 200 mm の目盛り（1 mm 刻み）を横と縦に 1 本ずつ、基準点の中心間の公称距離を下辺に印字
- 版の ID を左右の半分のそれぞれに入れた（1 台の画像にも必ず ID が写る）

### 2 系統の違い

同じ版を split をまたいで使わないため、要素の配置を変えた。`scripts/Test-M2CorpusContracts.ps1` は、2 つの版の chart ID・SVG のハッシュ・基準点の配置が互いに違うことを確かめる。

| 項目 | development（A0CS-PILOT-DEV v1） | holdout（A0CS-PILOT-HOLD v1） |
| --- | --- | --- |
| 四隅の基準点 F1〜F4 | (50,110) (791,110) (50,1139) (791,1139) | (110,100) (771,100) (110,1124) (771,1124) |
| 非対称の基準点 F5 | (125,110)、F1 の右 | (110,175)、F1 の下 |
| 公称の中心間距離 | F1-F2 741.0 mm、F1-F3 1029.0 mm、対角 1268.0 mm | F1-F2 661.0 mm、F1-F3 1024.0 mm、対角 1218.8 mm |
| 重なり帯の基準点 | 12 個。2 列（x 383 と 458）を交互に、y 150〜1120 にほぼ 90 mm おき | 13 個。3 列（x 372・420.5・469）。中央列は y 170〜1120 にほぼ 160 mm おき、左右の列はその間に交互 |
| 格子 | 10 mm／50 mm | 8 mm／40 mm |
| 重なり帯の中の要素 | 斜めエッジ 3 個（26 mm 角）、グレーと色は縦に並べる | 斜めエッジ 2 個（24 mm 角）、グレーと色は横に並べる |
| 帯を横切る線 | 水平 5 本（y 457〜473）と 2 本（y 1072・1080）、斜め 1 本 | 水平 5 本（y 530〜546）、斜め 2 本（下り・上り） |
| 帯の外の要素 | 円 4 組、斜めエッジ 4 個、細線群 3 組、グレー 2 組・色 1 組 | 位置・大きさ・向きを変えた円 4 組、斜めエッジ 4 個、細線群 3 組、グレー 2 組・色 1 組 |
| 文字 L／R の位置 | どちらも y 640 | L は y 800、R は y 420 |
| 題字の位置 | 左上 | 右下 |

基準点の座標の正本は各 vector spec の `printMaster.fiducials`。試験は SVG に描かれた基準点（`data-fiducial-id`）と spec の一覧が一致することも確かめる。

holdout 用の版下は公開リポジトリにあるが、holdout で隠すのは撮影した画像と期待値であって版の図柄ではない（既存の `locked-holdout-edge` と同じ扱い）。holdout の画像は、パイプライン凍結後に別の撮影日・別のリグ状態で撮る（#278）。

### 版を変えるとき

版下を 1 か所でも変えたら版を上げる（ファイル名・印字する VERSION・spec の `chartVersion` を同時に変える）。印刷済みの紙と版下の対応は、版の ID と PDF の SHA-256 で取る。版下を変えたら、spec の `masterSha256`、権利記録と manifest の `contentSha256` を更新し、`pwsh -NoProfile -File scripts/Test-M2CorpusContracts.ps1` を通す。

## 2. 印刷用 PDF

PDF はリポジトリに入れない。中身はベクタで 1 枚あたり約 86 KB と小さいが、ブラウザが作る PDF は作成日時などで毎回バイト列が変わり（同じ版下から 2 回作って SHA-256 が別になった、2026-10-08）、ハッシュで版を固定できないため。版の正本は SVG とそのハッシュで、PDF は印刷のたびに作り、その SHA-256 を印刷の記録に残す。

```powershell
pwsh -NoProfile -File scripts/Export-PilotChartPdf.ps1 -Chart development -OutputPath <リポジトリの外のフォルダ>\a0-pilot-chart-development-v1.pdf
```

スクリプトがすること:

1. SVG のハッシュが vector spec の `masterSha256` と一致するか確かめる（違えば止める）
2. Google Chrome（無ければ Microsoft Edge）を headless で動かし、`@page { size: 841mm 1189mm; margin: 0 }` の HTML に SVG を埋め込んで PDF にする。SVG を `<img>` で読ませると Chrome は全体を 862 x 1218 画素の画像にしてしまうので、HTML に直接書き込む
3. PDF を確かめる: 1 ページ、ページ寸法が A0 から 0.5 mm 以内、画像オブジェクトが無い（ベクタのまま）
4. 版下と PDF の SHA-256、ページ寸法、使ったブラウザを表示する。出力先がリポジトリの中なら止める

確認した結果（2026-10-08、Chrome）: 両系統とも exit 0、1 ページ、ページ寸法 840.99 x 1188.89 mm、画像オブジェクトなし、約 86 KB。ページの高さは A0 より 0.11 mm 短く、下端の余白が 0.11 mm 削られるだけ。中身の変換行列（0.24 x 11.811 = 2.8346 pt/mm = 72/25.4）から、版下の 1 mm が PDF でも 1 mm であることを確かめた。Edge は同じ引数で PDF を書かずに終わったため、スクリプトは Chrome を先に試す。

文字は Arial（無ければ Helvetica 系）で描く。PDF には実行した PC の書体が埋め込まれる。PDF は配布せず印刷所に渡すだけとする。

## 3. 印刷仕様

印刷所に PDF と一緒に渡す。許容差は撮影に使えるかどうかの目安で、品質の基準ではない（※想定。所有者が決める）。実測した値は必ず記録し、較正では公称値ではなく実測値を使えるようにする。

| 項目 | 指定 |
| --- | --- |
| 用紙 | A0（841 x 1189 mm）、縦。厚手のマット紙（つや消し、170 g/m² 以上が目安）。光沢紙・光沢ラミネートは使わない（照明が映り込む） |
| 倍率 | 100 %。用紙に合わせた拡大縮小、余白の追加、トンボの追加はしない。フチなしは不要（外枠の外に 20 mm の余白がある） |
| 解像度 | 0.1 mm の細線が途切れずに出ること（大判インクジェットで 600 dpi 以上が目安） |
| 色 | 自動の色補正・画質補正を切る。黒は K 単色で刷れるか印刷所に確認する（4 色の黒は版ずれで細線がにじむ）。色パッチは 2 台の差を比べるためのもので、色の絶対値の基準にはしない |
| 寸法の許容差（※想定） | 基準点の中心間（F1-F2・F1-F3）の実測が公称値の ±0.2 % 以内（development で ±1.5 mm・±2.1 mm）。対角 2 本（F1-F4 と F2-F3）の差が 2 mm 以内。200 mm の目盛りが ±0.5 mm 以内 |
| 断裁 | 841 x 1189 mm、±2 mm。断裁のずれは外枠の外の余白で吸収する |
| 枚数 | development 用 1 枚（予備 1 枚は任意）。holdout 用は holdout を撮る時期に同じ仕様で刷る（※要確認: 今まとめて刷るか） |
| 取り扱い | 丸めて受け取る場合は太い芯で、折らない |

## 4. 所有者の作業（印刷と寸法検証）

撮影より前に済ませる。記録はリポジトリの外に置き、Issue に書くときは PC 名・氏名・店舗名を書かない。

1. 刷る版を決める（既定は development 用 1 枚。holdout 用を今刷るか、予備を刷るかを決める）
2. `scripts/Export-PilotChartPdf.ps1` で PDF を作り、表示された版下と PDF の SHA-256 を控える
3. 印刷所に PDF と上の印刷仕様を渡す。色補正を切れるか、黒を K 単色で刷れるかを確かめる。どちらかができないなら、その旨を記録して進める
4. 受け取ったら、用紙を逆向きに巻くか平らな場所に置いて巻き癖を取る（目安 24 時間）
5. 目視で確かめる: 版の ID（`A0CS-PILOT-DEV v1` など）が上下左右の 4 か所以上に読めること、左上に L 字の角マークがあること、0.1 mm の細線が途切れていないこと、汚れ・折れ・傷が無いこと
6. 寸法を測る。平らな床か机に置き、鋼製の巻尺（1 mm 目盛り）で基準点の中心から中心までを 0.5 mm 単位で読む
   - F1-F2（横）、F1-F3（縦）、F2-F4（縦）、F3-F4（横）
   - 対角 F1-F4 と F2-F3
   - 横・縦の 200 mm 目盛りを鋼製の直尺と比べる
   - 測った日時、室温（分かれば）、測った道具を記録する
7. 公称値（チャートの下辺に印字）と比べて許容差の内か外かを書く。外なら刷り直すか、そのまま使って実測値を記録するかを決める
8. 印刷の記録を残す: 版の ID、版下と PDF の SHA-256、印刷日、用紙の種類、色補正の有無、寸法の実測値と判定
9. holdout 用を刷った場合は、測った後に封をして撮影場所と別に保管し、pilot の撮影に持ち込まない

## 5. 権利と corpus の記録

- 権利: 2 つの版下はどちらも自社作成（`in-house-original-work`）。第三者のチャート・顧客原稿・写真を含まない。権利記録 `samples/public/corpus-contracts/rights-record.example.json` に `asset-pilot-chart-development` と `asset-pilot-chart-holdout` を追加した
- manifest: `samples/public/corpus-contracts/manifest.example.json` に `vector-pilot-chart-development`（split `development`）と `vector-pilot-chart-holdout`（split `locked-holdout`）を追加した。汚染防止の 4 つの group は版ごとに別の値で、同じ版が 2 つの split に入ると検証で拒否される
- oracle: 両方とも `Accept`（宣言した損傷なし）
- 検証: `pwsh -NoProfile -File scripts/Test-M2CorpusContracts.ps1`。vector spec に `printMaster` がある場合は、SVG のハッシュ、A0 の寸法、版の ID と VERSION の印字、基準点の一覧、最小の重なり帯（x 360.5〜480.5）に入る基準点が 8 個以上あること、画像や外部リソースを埋め込んでいないこと、版を作った split にだけ置かれていることを確かめる

pilot の 36 組（較正 12・開発 24）は、corpus の split ではすべて `development` に入れる（ADR-0033 3 節、※要確認: Proposed）。較正用か開発用かは撮影の記録の用途欄で分ける。corpus の契約では同じ版（`originalMasterGroupId`）を `calibration` と `development` の両方に置けないため、較正用の画像を `calibration` の split に入れるなら較正専用の版が別に要る。
