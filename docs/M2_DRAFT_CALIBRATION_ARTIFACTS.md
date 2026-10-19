# draft校正ファイルと詳細レポート（#313、#277 段階5）

2026-10-15の所有者の決定に従い、既存のrig profile v2と、全校正画像の来歴を持つ詳細レポートを一組で生成する。新しいprofile形式には移行しない。計算は[固定文書写像](M2_REFERENCE_DOCUMENT_MAPPING.md)を呼び、保存用の画像合成本体を先に進めるための開発ツールとする。

JPEGの検出から一度に実行する場合は[1コマンドの校正](M2_CALIBRATION_COMMAND.md)（#315）を使う。以下はwriter単体の契約。

## ビルドと実行

```powershell
cmake -S . -B build/reference -G "Visual Studio 17 2022" -A x64 -DA0_BUILD_DRAFT_CALIBRATOR=ON
cmake --build build/reference --config Release --target A0CameraStitcher.DraftCalibrator m2_draft_calibration_artifacts_contracts --parallel 1
ctest --test-dir build/reference -C Release -R '^m2_draft_calibration_artifacts_contracts$' --output-on-failure

build/reference/Release/A0CameraStitcher.DraftCalibrator.exe --session <観測session.json> --job <job.json> --output <まだ存在しない出力フォルダ>
```

`A0_BUILD_DRAFT_CALIBRATOR`は既定OFF。OpenCV、Nikon SDK、生成器に依存しない。STLだけの校正libraryとは分け、開発用writerだけが既存profile readerとWindowsのSHA-256・ファイル保存を使う。製品の起動・撮影経路には接続しない。

入力の例は[開発用job例](../samples/public/draft-calibration-job.v1.example.json)。数値・配置は架空の開発用例で、実機の光学条件や品質合格線ではない。利用するsessionの基準view ID、既知の配置、測定日時、数値policyへ明示的に置き換える。`measuredAt`は観測が確定した日時以降とする。

## jobの契約

schemaは`a0.draft-calibration-job.v1`。全キー必須、未知キー・重複・不正な型を拒否し、数値の既定値は補わない。64 KiB、深さ8以下。観測sessionは既存readerの上限（8 MiBほか）で検査する。

- `profileId`、`provenanceId`、`measuredAt`: 出力を識別する論理IDと校正確定日時。PC名や実機識別子をIDにしない。異なる内容には新しいIDを付ける
- `documentPlane`: `orientation`は`landscape`/`portrait`、`cameraOrder`は`CAM-A-left-CAM-B-right`/`CAM-A-top-CAM-B-bottom`。用紙寸法は既存v2のISO A0規則に従う
- `referencePair`、`referencePlacement`: 段階4の明示入力。配置不明ならnullのまま診断させ、勝手に恒等配置にしない
- `numericalPolicy`: 段階4とレンズ推定の全数値停止条件。観測は歪み込みraw pixelとして扱う。品質閾値ではない
- `rigMeasurements`: baseline・overlap・カメラから文書までの距離・レンズ焦点距離。未実測の各値はnullとし、推定値を手測定値として埋めない

sessionの両カメラが7360×4912でなければprofileを出さない。小寸法の合成試験をD810の校正として偽装しない。画像はこのツールで読み直さず、session内の画像hashは入力の宣言として結合する。

両カメラの独立検証viewがすべて評価済みで、未使用点の残差と点数があることも必要。検証viewが無い・検証の計算が失敗した場合は`validation-not-evaluated`でprofileを出さない。学習やreferenceのfitが成功しても代用しない。評価済みの残差そのものには品質閾値を内蔵しない。

## 二つの成果物

### profile.json

既存schema `2.0.0`、常に`draft`。documentPlane・二台分のprojection・calibrationをそろえる。`physicalIdentity`、`outputRaster`、`correctionEnvelope`、`qualityContract`、`approval`は常にnull。出力前に既存C++ strict readerで再検証し、既存方式のcanonical fingerprintを計算する。

`calibration.inputSha256`は各reference画像のhash。`fitResidualPixels`は各referenceのusedPointIdsをraw pixelへ再投影したrms/maxで、学習やvalidationとは混ぜない。既存のschema/reader/fingerprintの構造は変更しない。

### calibration-report.json

schemaVersionは`a0.draft-calibration-report.v1`。以下を記録する。

- 結果`draft-produced`/`no-profile`と拒否理由、profile検証の失敗理由（JSON欄の診断）
- profileId・provenanceId・校正日時・tool ID/version、成功profileのcanonical fingerprint SHA-256
- 読み込んだ観測sessionの元のJSONと、その**元バイト列**のSHA-256。全画像のhash・役割・使用点・観測・観測toolの来歴を保持する
- 基準画像の組、既知の配置、文書面、rigMeasurements、全数値policy
- 二台それぞれのreferenceの終了理由・pose・残差/点数・LM記録
- 学習の終了理由・K・5係数・物理性検査・視点のrank/特異値・訓練/独立検証の集計と全viewの最終値、初期化段階のview別診断
- `quality: not-assessed`。数値計算が終わっても品質合格とはしない

両ファイルは一組として保管・移管し、取り込み時にはprovenanceIdとprofileのcanonical fingerprintを両方照合する。sessionのhashは元バイト列に結び付くので、同じJSON値でも整形し直した別ファイルとは一致しない。実画像の真正性、機体同一性、所有者の承認をIDの一致だけで証明しない。承認時の来歴確認は#45に残る。

## 保存と終了コード

出力先は存在しないフォルダを指定し、親フォルダは事前に用意する。既存の空フォルダも再利用しない。専用の兄弟`.partial-<process id>`フォルダにCREATE_NEWで保存・flush・再読比較した後、上書きしないdirectory renameで一組を公開する。競合で負けた側は完了済みフォルダに触れない。

- exit 0: 両カメラがそろい、profileとreportを公開
- exit 3: 校正・reference・metadataの失敗。reportだけを公開し、profileは作らない
- exit 2: 不正な入力やIO失敗。途中の保存物があれば`.partial-...`のまま保持し、完成したフォルダとして公開しない

複数ファイルの途中書込みは完成フォルダに現れない。電源断・プロセス終了後に残ったpartialを自動採用・再試行しない。既存profileを更新・上書きするツールではない。

## 検証範囲

`m2_draft_calibration_artifacts_contracts`は、独立の解析式で作った二台分の観測を入力し、既存strict readerとの互換性、reference hashと残差の結合、全viewの保持、決定的なバイト列、独立検証の欠損/失敗、片側失敗、metadata不正、寸法偽装の拒否、CLIの非上書き・IO失敗・同時公開の競合を検証する。

テストexeの任意の第二引数に新しいフォルダを指定すると、合成の観測session・job・profile・reportを残せる。.NET側でも次の専用確認を使い、異なる実装のreader/fingerprint/SHA-256と照合する。

```powershell
dotnet run --project tests/m3/RigProfileV2Tests -c Release -- --generated-artifacts <合成テスト成果物フォルダ>
```

この試験単体は解析的観測の結果。D810寸法の**合成JPEG**を実際に読み、draftと評価合成まで通す試験は[段階6](M2_FULL_CALIBRATION_PIPELINE.md)（#314）に分ける。実写・合成品質の受入、approvedの発行は#45、#42/#43/#49に残る。

2026-10-15の結果: writerの44チェックはRelease/Debugとも成功。既存.NET共有vectorは163チェック、生成artifactの互換性は7チェック成功。既存profile/sessionのJSON schemaと独立SHA-256も一致した。
