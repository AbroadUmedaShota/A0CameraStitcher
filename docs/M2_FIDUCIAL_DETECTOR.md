# JPEGからの基準点検出（#310、#277段階3b）

自社チャートのリング基準点をJPEGから見つけ、[観測session](M2_CALIBRATION_OBSERVATION_SESSION.md)（schema v1）を書き出す開発用のexe。段階3a（[レンズ歪みの推定](M2_CALIBRATION_LENS.md)、#290）に渡す入力を、解析的な観測ではなく実際の画像から作る。[設計案](M2_DRAFT_CALIBRATION_IO_PLAN.md)の§2と§6-3に沿う。

製品ではない。OpenCVを使うのはこのexeとその試験だけで、製品のlibrary・exe・試験、校正のpure libraryにはリンクしない（所有者の決定、2026-10-14）。OpenCVを製品へ同梱するかどうかの判断（`HG-0005`）は別で、この文書は何も決めない。

| 成果物 | パス |
| --- | --- |
| library | `a0_m2_fiducial_detector`（`src/m2/fiducial_detector.cpp`、`src/m2/include/a0/m2/fiducial_detector.hpp`） |
| exe | `A0CameraStitcher.FiducialDetector`（`src/m2/fiducial_detector_main.cpp`） |
| 試験 | `m2_fiducial_detector_contracts`（`tests/fiducial_detector_tests.cpp`）、`m2_fiducial_detector_link_isolation`（`tests/fiducial_detector_isolation_tests.cmake`） |
| CMake option | `A0_BUILD_FIDUCIAL_DETECTOR`（既定OFF） |

校正（K・歪み・pose）、reference pair、文書写像、draft profile、真値との比較は作らない。段階4以降。

印刷して実写に使うチャート（この検出器の規則に合わせた等倍のベクターPDF、chart定義、検出jobの雛形）は `scripts/Export-DetectorChartPdf.ps1` で作る。寸法の根拠と校正撮影の手順は [CALIBRATION_CAPTURE_PROCEDURE.md](CALIBRATION_CAPTURE_PROCEDURE.md)（#371）。

## OpenCVの入手と記録

ソースツリーには入れない。`.gitignore` 済みの `.tools/` に置く。

| 項目 | 内容 |
| --- | --- |
| 入手元 | 公式のGitHub release（`opencv/opencv` の `4.14.0`、`opencv-4.14.0-windows.exe`）。取得日 2026-10-14 |
| 版 | 4.14.0（4.xの最新の安定版。2026-07-19公開。`CV_VERSION` は試験が出力する） |
| 配布物のSHA-256 | `5f266a8b73bed535962d7e861a6457e32a0dd5f463ad0a7cf8707a135469be63`（203,030,799 byte）。GitHubが同じassetに表示するdigestと一致することを確かめた |
| 形態 | 公式のビルド済みバイナリ（MSVC vc16、x64、動的リンク）。VS2022のtoolset v143から使う。`opencv_world`（全モジュールを1つのDLLにしたもの）の1ライブラリだけ |
| 使うモジュール | `core`、`imgproc`（`GaussianBlur`、`threshold`、`findContours`、`contourArea`、`fitEllipse`、`cvtColor`）、`imgcodecs`（`imdecode`）。いずれも`opencv_world`に含まれる |
| 置き場所 | リポジトリ直下の `.tools/opencv-4.14.0/opencv/`（`build/include`、`build/x64/vc16/lib`、`build/x64/vc16/bin`、`LICENSE.txt`）。約230 MB |
| `opencv_world4140.dll` | `a192cbf006faf48c054468fa2a2e56e3f680b401c23b8f7fb24373ff75654c46` |
| `opencv_world4140.lib` | `ccbbb6223096a0d4cc2cbc0f12babcf6a5d6ee1a1918a16e5da058c144a0fad9` |
| `opencv_world4140d.dll`（Debug用） | `09d4facfdc12388620e7fa39302bcf97b9a61485a29c181bf3f22bd35d2da148` |
| `LICENSE.txt` | `3ddf9be5c28fe27dad143a5dc76eea25222ad1dd68934a047064e56ed2fa40c5` |
| ライセンス | Apache License 2.0（`LICENSE.txt` の冒頭で確認）。同梱の第三者の通知は配布物の `opencv/build/etc/licenses` にある |

取得と展開の手順（実際に行ったもの）。配布物のexeは実行しない。

```powershell
gh release download 4.14.0 -R opencv/opencv -p opencv-4.14.0-windows.exe -D <新しい空のフォルダ>
# SHA-256 を上の表と照合する
tar -xf opencv-4.14.0-windows.exe -C .tools/opencv-4.14.0 `
  --exclude=*.exe --exclude=*.pdb --exclude=opencv/sources --exclude=opencv/build/java --exclude=opencv/build/python `
  opencv/build opencv/LICENSE.txt
```

`tar` はWindows標準のbsdtarで、このexe（7-Zipの自己展開形式）を展開できる。`*.exe` を除外したので、OpenCV付属のツールとffmpegのプラグインは入らない（`imdecode` と `findContours` には要らない）。

## ビルドと実行

```powershell
cmake -S . -B build/fiducial -G "Visual Studio 17 2022" -A x64 `
  -DA0_BUILD_FIDUCIAL_DETECTOR=ON `
  -DOpenCV_DIR=<リポジトリ>/.tools/opencv-4.14.0/opencv/build/x64/vc16/lib
cmake --build build/fiducial --config Release --target A0CameraStitcher.FiducialDetector m2_fiducial_detector_contracts --parallel 1
ctest --test-dir build/fiducial -C Release -R m2_fiducial --output-on-failure
```

- `A0_BUILD_FIDUCIAL_DETECTOR` が既定のOFFなら、OpenCVを探さない。OpenCVが無い環境でもconfigureは通る（2026-10-14、OpenCVを指定しない新規のbuild directoryで確認）。ONでOpenCVが見つからない時は、`OpenCV_DIR` を求めるメッセージで止まる
- `opencv_world4140.dll` はexeと試験の隣へ、ビルド後に自動でコピーする
- 大きな試験はReleaseだけで回す。Debugは試していない

```
A0CameraStitcher.FiducialDetector.exe --job <job.json> --output <出力フォルダ>
```

`<出力フォルダ>` に `session.json`（全viewが解決した時だけ）と `detection-report.json` を新規に書く。同名のファイルがあれば上書きせず、exit 2で止まる。exitは、0=session作成、2=入力・IOのエラー、3=解決できないviewがありsessionを作らなかった。

## ジョブ（入力）

`schema` は `a0.fiducial-detector-job/1`。未知のキーと欠けたキーは拒否する（`std::invalid_argument`）。画像のパスはジョブファイルのあるフォルダからの相対で、出力にはファイル名だけを書く。

```json
{
  "schema": "a0.fiducial-detector-job/1",
  "sessionId": "...", "chartId": "...", "measuredAtUtc": "YYYY-MM-DDThh:mm:ssZ",
  "chart": { "columns": 22, "rows": 16, "originXMm": 20, "originYMm": 20, "pitchMm": 50 },
  "cameras": { "CAM-A": { "widthPixels": ..., "heightPixels": ..., "lensLabel": "...", "focalLengthLabel": "...", "focusLabel": "..." }, "CAM-B": { ... } },
  "views": [ { "viewId": "...", "camera": "CAM-A", "role": "training", "image": "a/original.jpg",
               "anchors": [ { "column": 8, "row": 7, "xPx": 888, "yPx": 624, "radiusPx": 30 }, ... ] } ]
}
```

- `chart` は印刷したチャートの定義。節点（column, row）の中心は `origin + pitch × (column, row)` mm、点IDは `F-<column 3桁>-<row 3桁>`（生成器のチャートと同じ書式）。節点は `(columns+1) × (rows+1)` 個。生成器の真値ではなくチャートの設計値
- `measuredAtUtc` は必須の入力で、時計を読まない。同じジョブから同じバイト列が出る
- `role` は `training`・`validation`・`reference`。validationは、検出した節点を `(column + row)` が偶数なら `posePointIds`、奇数なら `measurementPointIds` に振る（市松の固定規則）。sessionの契約（2つの列は共通点なし、測定側は1点以上）を満たさなければ、readerが拒否してsessionは出ない
- 画像の寸法が `cameras` の寸法と違うviewは `dimension-mismatch` として記録し、sessionを作らない
- 画像はEXIFの向きを適用せずに読む（`IMREAD_IGNORE_ORIENTATION`）。画像のSHA-256は、読み込んだファイルのバイト列から求める

## 点IDの対応づけ

見た目が同じリングなので、IDは画像だけでは決まらない。設計案§2の「ID付きROI等を使う場合も、その入力と手順を記録する」に従い、**operator anchor** を使う。

1. operatorがviewごとに3つ以上のリングについて、節点（column, row）と、おおよその画素位置（`xPx`、`yPx`）、探索半径（`radiusPx`）を与える
2. 探索半径の中に、検出したリングがちょうど1つあれば、そのリングをその節点に結びつける。0個は `anchor-not-found`、2個以上は `anchor-ambiguous`、同じ節点や同じリングへの重複は `anchor-duplicate`、チャートの外の節点は `anchor-outside-chart`、格子の2方向に張らない（共線）は `anchor-collinear`、3つ未満は `anchor-count`
3. anchorの非共線な三つ組をすべて調べ、チャートを表から見た向きと逆なら `anchor-mirrored` で拒否する。最初の三点だけで決めない
4. 解決済みの節点に隣り合う節点を、近くの解決済みの節点（格子の距離3以内、最大12個）の最小二乗のアフィン写像で予測する。予測位置から格子間隔の30%（`acceptFraction`）以内に、未使用のリングがちょうど1つあれば結びつける。2つ以上は `ambiguous`、0個は `missing`。向きが逆になる写像と、画像の縁から（白リング半径の2.2倍）以内の予測は調べない
5. 新しく結びつく節点が無くなるまで繰り返す。どの節点にも結びつかなかったリングは、reportに数だけ残す
6. 四隅が検出されたセルの模様とグレーパッチを、チャートの設計と照合する（下記）。照合に失敗したviewの観測は出さない

生成器の真値は経路に入らない。anchorは**operatorの入力**で、`detection-report.json` に節点ID・指定した位置・探索半径・結びついたリングの位置をviewごとに記録する。

試験では、operatorの役を、真値の投影位置を12 pxの倍数に丸めた位置と、半径30 pxで代行する（`OperatorAnchors`）。真値を渡すのは、この3つの節点のanchorだけで、丸めと半径は報告に残る入力である。

### 目印の取り違えの照合（#311）

格子が整合していても、節点の名前をずらすと誤ったposeが小さな残差で推定される。残差の大小をIDの証明にせず、印刷されたセルの模様を使う。

- 四隅からセル内だけの射影写像を作り、中央・模様の外側・白紙の帯を標本化する。この局所写像は開発用の照合だけに使い、製品の合成や固定リグ写像の推定には使わない
- `(3 × column + 5 × row) mod 8` の模様（白紙、暗/明の斜辺、色、グレー、縦線、横線）を、白紙に対する明るさ・変動幅・セルの軸方向の明暗の交差数で照合する
- 斜め `(1,1)` のずれは模様の種類が同じになるため、グレーの6段階 `(column + row) mod 6` も照合する
- `identityMaxMismatchFraction` と `identityMinMismatches` を超える不一致は `identity-mismatch`。セルが小さい（辺80 px未満）などの理由で測れず、`identityMinCells`（既定8）・`identityMinGrayCells`（既定2）を満たさない場合は `identity-unverifiable`。どちらもviewの観測を破棄し、job全体のsessionを出さない
- 各viewの `identity` に、照合結果・三つ組の数・セルとグレーの測定数/不一致数・測れないセル数・最初の最大16件の不一致と測定値を残す。設定値もreportに残す

**限界**: このチャートの模様には周期があり、すべての取り違えを一意に検出するものではない（例: `(3,3)` のずれは種類とグレーが繰り返す）。operatorの入力は人も見直す。明るさの比の照合は線形の画像応答を仮定し、実写の照明・印刷・JPEGの応答は未検証。これらの数値は開発用の合成チャートに対する設定で、製品の光学・品質合格線ではない。

## 検出

1. グレースケールで読み、1・99パーセンタイルの中間（`darkFraction` = 0.5）を暗の閾値にして、ガウスぼかし（σ=1）の後に二値化する
2. 輪郭を階層つきで取り（`findContours`、`RETR_TREE`）、暗い成分の「穴」（白リング）で、穴の中に島（中央の点）が1つだけあるものを候補にする
3. 候補の条件: 穴の等価半径4〜600 px、楕円近似の短径/長径 0.4以上、輪郭の面積と楕円の面積の差15%以内、中央の点の面積が穴の面積の7〜30%（設計値は17.4%。輪郭の多角形の面積は小さく出る）、点の中心と穴の中心の差が穴の半径の15%以内、穴の外側の黒リングの24点のうち85%以上が暗い
4. 中心は、白リングの領域（楕円の半径の0.62〜0.78倍）に平面を当てて背景とし、その背景に対する暗さを重みにした点の重心。楕円の窓の中で3回、中心を更新する。窓が画像の外に出る、点のコントラストが足りない、重心が大きく動く候補は捨てる（画像の縁で切れたリングは検出しない）

閾値、面積比、許容はすべて `DetectorParams`（`detection-report.json` の `params` に記録）で、製品の閾値ではない。光学系の既定値ではなく、この合成チャートの比率（黒リング0.6R〜R、白リング0.25R〜0.6R、点<0.25R）から決めた。

## 出力

- `session.json`: schema v1。`chartPoints` は格子の全節点。views は入力の順。`observations` はそのviewで調べた節点（`detected`、`missing`、`ambiguous`）で、調べていない節点は載せない。出す前に `CalibrationObservationSession::Parse` で自分の出力を検証する（拒否されたらsessionを出さず、reportに理由を残す）。`provenance.tool` は `a0-fiducial-detector` / `1.1.0`（#311）
- `detection-report.json`: schema `a0.fiducial-detector-report/1`。ID対応づけの方法、検証の振り分け規則、`params`、viewごとの画像名・SHA-256・寸法・状態（`ok` か失敗コード）・暗の閾値・候補数・検出/missing/ambiguousの数・未使用のリングの数・anchorとidentityの記録。絶対パス、PC名、ユーザー名は書かない

exeは処理の前に両出力名の存在を確認する。どちらかが既存ならexit 2で止まり、既存内容を変更せず、もう一方も新規作成しない。この事前検査は複数プロセスによる同時書込みやディスク障害時の二ファイルのatomic commitを保証するものではない。出力先は一つのjobだけで使う。

## 試験と結果

`m2_fiducial_detector_contracts`（Release）。合成画像は#269の生成器（1800×1200、supersample 2、リングの外半径は約15〜25 px（代表の1視点の輪郭からの見積もりで、全viewの範囲は集計していない））。傾いた視点の作り方は`H = K [r1 r2 t]`（[生成器](SYNTHETIC_PAIR_GENERATOR.md)の限界の節）。許容値は試験の入力で、製品の閾値ではない（`kUndistorted`、`kDistorted`、`kNoisyJpeg`、`kHarshJpeg`、`kMapping`）。

以下の数値は#310の記録（実行日 2026-10-14、Windows、Release、`ctest -C Release -R m2_fiducial`、82チェック・失敗0）。#311の変更後の結果とは区別する。

2026-10-15の#311では、±1格子ずれ（横・縦・対角）、IDの入れ替え・誤記・循環、後続のanchor三角形だけの鏡映、identity不足、出力名の衝突を追加確認。検出器の全148チェックがRelease/Debugとも成功し、関連CTestは各14/14成功した。D810寸法でdraft・合成まで通す明示試験は[全工程の試験](M2_FULL_CALIBRATION_PIPELINE.md)（#314）に分けて記録する。

### 検出した中心と真値の差（1つの視点、25° / −10° のあおり）

| 段階 | 条件 | 検出数 | 必須のリング（縁から60 px以上）と検出 | 取り違え | rms | max |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 歪みなし、無ノイズ、無圧縮 | 137 | 121 / 121 | 0 | 0.0419 px | 0.1015 px |
| 2 | 歪みあり（k1 −0.09、k2 0.02、p1 4e-4、p2 −3e-4）、無ノイズ、無圧縮 | 147 | 130 / 130 | 0 | 0.0398 px | 0.1025 px |
| 3a | 歪みあり、ノイズ、周辺減光、露出・白バランス、JPEG品質0.95・4:4:4 | 147 | 130 / 130 | 0 | 0.0404 px | 0.0987 px |
| 3b | 同上、JPEG品質0.75・4:2:0 | 147 | 130 / 130 | 0 | 0.0439 px | 0.0842 px |
| 3c | 同上、JPEG品質0.40・4:2:0 | 147 | 130 / 130 | 0 | 0.0570 px | 0.1316 px |

許容値は、段階1・2がrms 0.08・max 0.20 px、3a・3bが0.09・0.25 px、3cが0.12・0.30 px。測定値の約2倍の余裕で、通ることを確かめた値であり、品質の合格基準ではない。

### 3aのLMへ渡した写像の差（16枚のJPEG、各カメラ6つのtrainingと2つのvalidation）

16枚から作ったsessionを `EstimateLensIntrinsics`（policy `{2, 1e-2, DistortedAnalytic, 100, 1e-10, 1e-12, 1e-3}`）に渡した。比べたのは、推定した文書→画素の写像（各viewのpose、K、5係数）と、生成器の真の写像で、画像内の格子の節点（縁から30 px以上）について。

| カメラ | status | 訓練viewの写像の差 rms / max | validationの写像の差 rms / max | 検出した点の再投影 rms（訓練） |
| --- | --- | --- | --- | --- |
| CAM-A | `lens-intrinsics-estimated` | 0.0084 / 0.0269 px（875点の訓練で6view） | 0.0113 / 0.0313 px（2view） | 0.0398 px |
| CAM-B | `lens-intrinsics-estimated` | 0.0085 / 0.0237 px | 0.0078 / 0.0223 px | 0.0407 px |

推定したK（CAM-A）は fx 1499.96（真1500）、cx 899.54（真899.5）、k1 −0.09017（真−0.09）。CAM-Bもfx 1520.04（真1520）、k1 −0.06013（真−0.06）。許容値は rms 0.05・max 0.15 px。

### そのほか

- 同じジョブで2回走らせると、sessionとreportが同じバイト列になる。exeの出力は、ライブラリの出力と同じバイト列。出力先が既にあれば上書きしない。解決できないviewがあればexit 3でsessionを作らない
- 失敗系: anchorの不足・探索半径に無い・半径が広すぎ・重複・共線・チャートの外、リングの無い画像、画像寸法の不一致、未知のキー・不正なJSON・未知のカメラ/role
- #310での検出した16枚のJPEG（1800×1200）の所要は約1.0〜1.4秒（ジョブ全体、Release）。7360×4912の合成JPEGは後続#314で検証（[全工程の試験](M2_FULL_CALIBRATION_PIPELINE.md)）
- 配線の試験: `m2_fiducial_detector_link_isolation`（21件）が、OpenCVを許す対象を列挙したものだけに限ること、検出器が生成器と製品の合成にリンクしないことを、`cmake -P` の小さなプロジェクトで確かめる

## configureの検査

`a0_assert_opencv_only_in_allowed_targets`（`cmake/a0_link_isolation.cmake`）が、top-levelの全targetのうち、許可した3つ（`a0_m2_fiducial_detector`、`A0CameraStitcher.FiducialDetector`、`m2_fiducial_detector_contracts`）以外について、OpenCVのリンク（`opencv_*`、`libopencv_*`、`cv2`、imported targetを含む推移的なもの）とOpenCVのinclude directoryを拒否する。configureの最後に走るので、後から足したtargetも対象になる。OPTIONがOFFでも走る。製品のlibrary・exe・試験、`a0_m2_calibration_*` と各試験が対象に入る。

2026-10-14に、OPTIONをONにして、一時的に次の4つを足すとconfigureが止まることを確かめた（確認後に戻した）: `a0_m2_calibration_lens` に `OpenCV_LIBS` をリンク、`A0CameraStitcher.Phase0` に同じ、`a0_m2_render` にOpenCVのinclude directory、`m2_calibration_lens_contracts` に `opencv_world` をリンク。

`a0_assert_fiducial_detector_independent` は、検出器のlibraryとexeが `a0_m2_synthetic_pair`・`a0_m2_render`・`a0_m2_offline_stitcher`・`a0_m2_rig_profile_v2` と対応するsourceを持たないことを検査する。試験のexeだけが生成器にリンクする（真値は試験だけが見る）。

## 置いた仮定と限界

| # | 内容 |
| --- | --- |
| 1 | 模様とグレーで取り違えを照合するが、周期的に同じ模様になる大きなずれは区別できない（上記）。実写ではanchorの記録も人が見直す |
| 2 | チャートは生成器のチャートの比率（白リングと中央の点を持つリング）と点IDの書式。別のチャートには `DetectorParams` と `ChartDefinition` の見直しが要る |
| 3 | 合成画像だけの結果。実レンズのぼけ・照明のむら・紙の浮き・焦点ずれは入っていない。実写の検出精度はこの試験から言えない（段階6） |
| 4 | #310は外半径約15〜25 pxの画像。より小さいリングでは暗の閾値の側の効きが大きくなる（面積比の下限7%に近づく）。D810寸法の合成JPEGは#314で追加検証し、実写は未検証 |
| 5 | 中心は点の重心で、リング中心の像とは一致しない（遠近・歪みによる差。[生成器](SYNTHETIC_PAIR_GENERATOR.md)の限界の節）。この差も上のrms・maxに含まれる |
| 6 | 縁で切れたリングは検出しない。縁の近くの節点は `missing` にも載せないことがある（調べないため） |
| 7 | 3aのcutoff（`relative_singular_cutoff` = 1e-2）は、この試験の検出誤差（約0.04 px）で通った値。実写の検出誤差での再検討は段階6 |
| 8 | #311の全試験は2026-10-15にRelease/Debugとも実行済み。CIの対象ではない（OPTION既定OFF） |
