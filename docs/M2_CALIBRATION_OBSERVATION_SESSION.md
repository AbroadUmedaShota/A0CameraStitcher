# 校正の観測session（#286、#277段階1）

[入出力設計案](M2_DRAFT_CALIBRATION_IO_PLAN.md)の§2「観測session」と§6-1を実装したもの。推定側が受け取る入力を、pure C++20の厳格なreader/validatorとJSON Schema v1として確定する。段階2以降の推定コードは、この型だけを入力にして作る。

校正・profile・レンズ係数は作らない。#283の`AssessViewGeometry`もここでは呼ばない。

| 成果物 | パス |
| --- | --- |
| library | `a0_m2_calibration_observation_session`（`src/m2/calibration_observation_session.cpp`） |
| public header | `src/m2/include/a0/m2/calibration_observation_session.hpp` |
| schema | `docs/schemas/calibration-observation-session.v1.schema.json` |
| fixture | `tests/fixtures/calibration-session/`（手書きの合成データ。[README](../tests/fixtures/calibration-session/README.md)） |

libraryが使うのはSTLと共有の`protocol_json.hpp`だけ。Win32・OpenCV・製品のrender/stitch・生成器・profile readerには依存しない。configure時に`a0_assert_no_calibration_view_dependency`で確かめている。

## 範囲外

| 項目 | 行き先 |
| --- | --- |
| CLI、画像hashの照合、ファイルIO | IO段階 |
| numerical policy、K/pose推定、#283への接続 | 段階2 |
| レンズ推定、検出器、OpenCV | 段階3 |
| reference placement | 段階4 |
| rigMeasurements、provenanceId、report/draft writer | 段階5 |
| 真値との比較 | 段階6 |
| approvedの発行 | #45 |

消費者のいない欄は持たない。後の段階で欄が要る時は版を上げる。v1はpushした時点で固定する。

## 入力の形

JSONの最上位は`schemaVersion`、`sessionId`、`chartId`、`provenance`、`chartPoints`、`cameras`、`views`の7欄。すべてのobjectは未知の欄を拒否する。欄の表と制約はschemaにある。読み手が知っておくことだけを書く。

- **座標**: chart点はチャート面のmm。観測のpixelは歪みを含む保存順の座標で、整数indexが画素の中心。EXIFの向きは適用しない。有効範囲は閉区間で、`-0.5 <= x <= 幅 - 0.5`、`-0.5 <= y <= 高さ - 0.5`
- **camera**: `CAM-A`と`CAM-B`の2つだけ。レンズ・焦点距離・フォーカスのラベルは自己申告の識別子で、物理的に同じレンズである証明にはならない
- **view**: 役割を1つだけ持つ。`training`と`reference`は`usedPointIds`、`validation`は`posePointIds`（姿勢の推定に使う点）と`measurementPointIds`（誤差の測定に使う点）を持つ。後者は1点以上で、2つの列に共通の点があってはならない。referenceはcameraごとに1件まで
- **観測の状態**: `detected`の時だけpixelがobjectで、`missing`と`ambiguous`ではnull。未検出を(0,0)などで埋めた入力は拒否する。chart点が観測に載っていないのは「そのviewでは探していない」、`missing`は「探したが見つからない」を表す。段階1では両方を同じ欠損として扱う
- **measuredAt**: 観測（検出結果）を確定した時刻。撮影時刻ではない。形式は`YYYY-MM-DDThh:mm:ssZ`で、暦として正しい日付に限る
- **ID**: sessionの識別子とラベルは`[A-Za-z0-9][A-Za-z0-9_-]{0,127}`（点なし）。`provenance.tool`の2欄だけrig profile v2の`identifier`と同じ規則で、点を許す
- **画像hash**: 64文字の小文字16進。session内で一意。fixtureの値は合成で、実画像のhashではない

pixel欄が`null`であることと欄がないことは別で、欄は常に必須。

## 拒否と診断

2段に分かれる。混ぜない。

**拒否（Parse）**: `CalibrationObservationSession::Parse(json)`が読み込みと構造の検証を1回で行い、違反があれば`std::invalid_argument`を投げる。構造的に不正なsessionはオブジェクトとして存在しない。messageは次の形で、入力の値や未知のキー名は含めない。

```
calibration observation session: <category> at <json-pointer>
```

| category | 意味 |
| --- | --- |
| json-byte-limit | 0 byte、または上限超（parseの前に判定） |
| limit-json-values | JSONの値の個数の上界が上限超（parseの前に、本文を1回走査して判定。下の「資源上限」） |
| json-syntax | 構文違反。深さ超過、BOM、末尾の余分なデータ、`NaN`などの字句を含む |
| json-duplicate-key | 同じobject内のキー重複（エスケープを解いた後で比較） |
| truth-field | 真値や投影モデルのキー（下記）。形の検査より先に判定する |
| unknown-field / missing-field / type | objectの形の違反 |
| schema-version | `schemaVersion`の不一致 |
| identifier / integer / number / utc / sha256 | 値の書式・範囲の違反 |
| camera-alias / role / state | 列挙値の違反 |
| duplicate-id / duplicate-sha256 / duplicate-chart-position | 一意性の違反 |
| chart-degenerate | chart全体に、どの3点も同一直線上にない4点が無い（4点未満を含む） |
| limit-chart-points / limit-views-per-camera / limit-observations | 資源上限 |
| dimension-mismatch | viewの寸法がcameraの寸法と違う |
| undefined-point | chartに無い点、またはそのviewの観測に無い点の参照 |
| state-pixel | stateとpixelのnullの組み合わせの違反 |
| pixel-range | pixelが有効範囲の外 |
| used-not-detected | 使う点が`detected`以外の観測を指す |
| validation-measurement-empty / validation-overlap | validationの点の分離の違反 |
| reference-duplicate | 同じcameraにreferenceが2件以上 |

真値のキーは`groundTruth`、`ground_truth`、`intrinsics`、`distortion`、`projection`、`documentToImage`、`image_px`、`document_mm`、`reference_points`、`generator_version`。位置を問わず、1つでもあればtruth-fieldで拒否する。生成器のground-truth文書は「schemaVersionがない」ではなくこの理由で拒否される。categoryの語彙はpublic APIではないが、試験の契約として固定している。

**診断（DiagnoseObservationSession）**: 受理済みのsessionに対する純関数で、拒否はしない。`std::vector<SessionDiagnostic>`（code、camera、view_id）を次の順で返す。

1. CAM-A、CAM-Bの順に、camera単位のcode（referenceが先、validationが後）。view_idは持たない
2. viewを入力順に、点数・縮退のcode、続いて曖昧のcode

| code | 条件 |
| --- | --- |
| reference-view-missing | そのcameraにreferenceのviewが無い |
| validation-view-missing | そのcameraにvalidationのviewが無い |
| insufficient-observations | 使う点（validationは`posePointIds`）が4点未満。`measurementPointIds`は数えない |
| degenerate-point-configuration | 使う点が4点以上あるが、どの3点も同一直線上にない4点が無い。4点未満の時は併記しない |
| ambiguous-correspondence | 同じviewの`detected`観測のうち、別IDの2件以上が同じpixelにある。使う点に限らず、detectedの観測すべてが対象 |

診断が空でも、校正できる・成功したという意味ではない。5種類の条件が見つからなかったという以上の情報はなく、総合の合否やboolは意図して持たない。

## 資源上限

各値で「Nを受理し、N+1を拒否する」ことを試験している。

| 項目 | 上限 | 超えた時のcategory |
| --- | --- | --- |
| JSONのバイト数 | 8 MiB（8,388,608） | json-byte-limit |
| JSONの値の個数 | 1,048,576（2^20）。本文の走査で数えた上界 | limit-json-values |
| JSONの深さ | 32（rootを含む。最内が空でない入れ子の段数） | json-syntax |
| chart点の数 | 16,384 | limit-chart-points |
| cameraごとのview数 | 128 | limit-views-per-camera |
| 1 viewの観測数 | 16,384 | limit-observations |
| sessionの観測の合計 | 65,536 | limit-observations |
| 画像の辺 | 1〜32,768 | integer |
| IDとラベルの長さ | 1〜128文字 | identifier |

共有のJSON parserは、値1つにつき`std::map`と`std::vector`と`std::string`を持つ節を1つ作る。メモリを決めるのは文書のバイト数ではなく値の個数なので、バイト上限（8 MiB）だけでは足りない。`{"x":[0,0,0,...]}`は8 MiB以内に約400万個の値を詰められ、Releaseで1 GB超になると見込まれる（※推定。この形の文書そのものは構築していない）。そこでParseは、木を作る前に本文を1回走査して値の個数の上界を数え、上限を超えたら`limit-json-values`で拒否する。共有parser（`protocol_json.hpp`）は変えていない。parserの`JsonValue`のmoveが`noexcept`でなく配列の拡張で深いコピーが起きうる件は#287で扱う。

### 値の個数の数え方

走査は`src/m2/calibration_session_json_scan.hpp`の`JsonValueCountUpperBound`で、文字列の中（`\`の次の1文字を含む）を飛ばし、`,`・`[`・`{`を数えて1を足す。値はrootと、すべての容器の要素またはメンバーなので、構文として正しい本文では次が成り立つ。

```
値の個数 = 1 + (コンマの数) + (空でない容器の数)
        <= 1 + (コンマの数) + ('[' と '{' の数)
```

m個（m >= 1）の要素を持つ容器にはコンマがm - 1個あるので、容器1つにつき「コンマ + 1」がその要素数になる。上界は空の容器1つにつき1だけ多く数える。構文として正しくない本文でも走査は数えるだけで、その後のparserが拒否する。

### 上限値の計算

合法なsessionが持ちうる値の最大数を、上限の定数から数える。

| 項目 | 式 | 値 |
| --- | --- | --- |
| root、`schemaVersion`・`sessionId`・`chartId`、`provenance`（5）、`cameras`（13） | 1 + 3 + 5 + 13 | 22 |
| `chartPoints`の配列と、点ごとの object と3メンバー | 1 + 4 × 16,384 | 65,537 |
| `views`の配列と、viewごとの object・6つの値・`observations`の配列 | 1 + 8 × 256 | 2,049 |
| 点リストの配列（validation viewは2本） | 2 × 256 | 512 |
| 観測。`detected`が最大で、object・`pointId`・`state`・`pixel`のobject・`x`・`y` | 6 × 65,536 | 393,216 |
| 点リストの要素。各要素はそのviewの別々の`detected`観測を指し、validationの2本は共通の点を持てない | 65,536 | 65,536 |
| 合計 | 上の和 | 526,872 |

空の容器は1つにつき上界を1つ増やす。training viewでは空のリストの分だけ要素が減るので差し引きは変わらないが、validation viewは`posePointIds`を空にしても要素を`measurementPointIds`へ寄せられるので減らない。viewの枠は役割によらず最大11（validationで`posePointIds`が空なら値10＋空の容器1、trainingで`observations`と点リストが空なら値9＋空の容器2）なので、上界の最大は 22 + 65,537 + 1 + 11 × 256 + 7 × 65,536 = 527,128（値の最大526,872より256多い）。上限は527,128以上で最小の2の冪、2^20 = 1,048,576（約2倍の余裕。2^19 = 524,288は足りない）。試験が両方の式を`static_assert`で固定している。最悪ケースの文書（試験のW）の上界は524,345。

### 実測

2026-10-13、Windows、Releaseと一部Debug。いずれも空きメモリが少ない（物理メモリの空き約300 MB）状態での計測を含み、静かな状態の値は別に記す。

| 入力 | 構成 | 結果 |
| --- | --- | --- |
| 上限+1（1,048,577個、2,097,157 byte） | `{"x":[0,0,...]}` | 拒否16 ms（Release）、48 ms（Debug） |
| バイト上限いっぱい（4,194,302個、8,388,607 byte） | 同上 | 拒否66 ms（Release）、143 ms（Debug） |
| ちょうど上限（1,048,576個、2,097,155 byte） | 同上。木を作り、形で拒否する | Releaseで5.3 s。`bad_alloc`なし |
| `limits`試験全体（Release） | 上記と、観測65,536件の文書（3,997,248 byte）、最悪ケース（5,969,469 byte） | ピークのワーキングセット340 MB、所要16 s |
| 最悪ケースW（chart点16,384件、4 view、値524,345個） | 試験のW | Parse 1.07 s・Diagnose 0.18 s（静かな時、Release）。負荷時は2.6 s（あやめ） |
| 同上（Debug） | 同上 | 26〜43 s、ピーク約550 MB（あやめ） |

Debugの`limits`試験は、この変更の後には走らせていない（空きメモリが600 MB未満のため）。Debugの数字は変更前のWに対するあやめの計測。

JSONの深さは共有parserの固定値（`kMaximumProtocolJsonDepth`）で、template引数では変えられない。`static_assert`で定数を一致させている。

## 一般位置の判定

`degenerate-point-configuration`と`chart-degenerate`は、許容値を使わない正確な述語で判定する。入力のbinary64を有限長の整数として扱い、3点の向き（`[[ax,ay,1],[bx,by,1],[cx,cy,1]]`の行列式）が0かどうかを誤差なしで決める。#283の特異性判定と同じ関数で、内部header`src/m2/calibration_exact_predicates.hpp`にある。

例として、`(0.5, 0.5000000000000001)`、`(12, 12)`、`(24, 24)`、`(36, 36)`は、1点が直線から1 ulp外れているため、正確には同一直線上にない。この4点に`(1, 0)`を足した5点は診断が空になる。`(0.5, 0.5)`に替えると4点が直線上に乗り、`degenerate-point-configuration`が出る。`(bx-ax)*(cy-ay)-(by-ay)*(cx-ax)`をdoubleで計算する素朴な式は、前者を0と判定する。

判定は点数Nに対して線形の回数（述語の呼び出しは最大4N回）で終わる。相異なる点の集合で「どの3点も同一直線上にない4点が無い」ことは、「ある直線Lについて、L上にない点が1個以下」と同値になる。L外が1個以下なら、どの4点もL上に3点を含む。逆に、どの直線でもL外が2点以上なら、点を最も多く含む直線Lを取る。L上が2点だけなら3点共線が無いので任意の4点でよく、3点以上なら、L外の2点p、qと、直線pq上にないL上の2点で条件を満たす4点が作れる。この同値により、p0、p1と、p0p1上にない最初の点pkの3点が作る3本の直線だけを調べればよい。試験では、4×4格子上の乱択した点集合で総当たり（O(N⁴)）と結果を比べている。

## 数値の扱い

- 数値は`std::from_chars`でbinary64に変換する。字句の途中で止まる、overflow、**0へのunderflow**はすべて`number`で拒否する。rig profile v2のreaderは、0に潰れる値を0に丸めて受理する。この点がv2と違う。校正座標で0に潰れる値に意味はないため、厳しい側に倒した。非正規数（例`5e-324`）は受理し、ビット値を保つ
- 整数（寸法）は数字だけの字句に限る。`1.0`、`1e0`、`-0`、符号付き、`uint32_t`に収まらない値は`integer`で拒否する
- `-0`の座標は`+0`に置き換える。座標の重複検出、pixelの衝突検出、2回読んだ結果の一致がこの正規化に依存する
- UTCの書式と暦の検査、数値変換はrig profile v2のreaderと同じ考え方で、この翻訳単位に別に書いた。隔離policyのため`rig_profile_v2`をリンクできないことによる。2か所に分かれるので、将来のずれを避ける共通化は#288で扱う

## 置いた仮定

| # | 仮定 | 外れた時 |
| --- | --- | --- |
| 1 | 観測に載らないchart点と`missing`を、段階1では区別しない | 段階2で区別が要るなら版を上げて欄を足す |
| 2 | `measuredAt`は観測の確定時刻 | 意味の記述だけが変わる。型と書式は変わらない |
| 3 | validation viewの測定点は1点以上を必須にする（構造の拒否） | 0点を診断にしたい場合は下限を0にして診断codeを足す |
| 4 | underflowは拒否する | v2に揃えるならv2のDecimal判定を移植する |
| 5 | 1つのviewは役割を1つだけ持ち、画像hashはsession全体で一意 | |
| 6 | ID・ラベルの規則はrig profile v2の`identifier`の部分集合（点なし）。段階5でchartIdをprofileへ写しても、段階2でviewIdを`ViewHomography.id`へ渡しても規則を満たす | |

## 試験

| CTest | 内容 |
| --- | --- |
| `m2_calibration_observation_session_contracts` | 正例、構造の拒否（表の71件＋真値キー60件ほか）、診断、資源上限（軽い項目）、正確な述語の直接試験、値の個数の数え方（小さな入力）と上限±1（木を作らずに拒否されること） |
| `m2_calibration_observation_session_limits` | 重い項目。バイト上限の境界、観測の合計の境界。Releaseだけ、ちょうど上限の木（D9）と最悪ケース（W）。Debugでは後者2つを飛ばす（実測は「資源上限」の節） |
| `m2_calibration_observation_session_schema` | schemaの形、正例と負例の判定、rig profile v2との`provenance`の同一性（drift検査） |
| `m2_calibration_observation_session_link_isolation` | 隔離policyの正の対照1件と、synthetic_pair・rig_profile_v2・bcrypt・opencvの直接・推移・source混入の拒否 |

負例は`tests/fixtures/calibration-session/negative-cases.json`の変更表で、C++の試験とPowerShellのschema試験が同じ表を読む。各caseの`schema`列で、schemaが拒否するか、readerだけが拒否するか、PowerShellが数値の字句を保てないため試験しないかを固定している。表から行が消えるとC++の試験が落ちる。

fixtureはすべて合成で、実画像・実機のhash・識別子を含まない。寸法64×48は計算試験用の小さな値で、実機のサイズではない。

#283の`m2_calibration_view_geometry_contracts`は、述語を内部headerへ移した後も`checks=607 failures=0`のまま。

```
ctest -C Debug -R "^m2_calibration_|^m2_link_isolation_contracts$" --output-on-failure
ctest -C Release -R "^m2_calibration_|^m2_link_isolation_contracts$" --output-on-failure
```
