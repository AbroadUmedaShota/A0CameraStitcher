# 合成JPEGからdraft校正と評価合成まで（#314、#277段階6）

開発用の明示実行試験。7360×4912の合成JPEGを各カメラ9枚（training 6、validation 2、固定reference 1）作り、検出器の公開入力から観測session、レンズ推定、固定文書写像、既存profile v2のdraft＋詳細レポート、既存レンダラーのA0評価プレビューまで通す。

## 実行

`A0_BUILD_FIDUCIAL_DETECTOR=ON`と`A0_BUILD_DRAFT_CALIBRATOR=ON`、`BUILD_TESTING=ON`が必要。いずれの開発ツールも既定OFF。OpenCVの配置は[検出器の文書](M2_FIDUCIAL_DETECTOR.md)を参照する。

```powershell
cmake --build build/reference --config Release --target m2_fiducial_detector_contracts --parallel 1
build/reference/Release/m2_fiducial_detector_contracts.exe --full-resolution-calibration build/reference/synthetic-full-NEW
dotnet run --project tests/m3/RigProfileV2Tests -c Release -- --generated-artifacts build/reference/synthetic-full-NEW
```

出力先は親が存在する新規フォルダに限る。失敗しても途中の画像とレポートを残す。既存フォルダを削除・上書きしない。重い画像生成を通常のCTestには追加しない。

## 独立した照合

- 画像は合成生成器（#269）で作る。レンズ歪み、露出、白バランス、周辺減光、ノイズ、JPEG品質0.95・4:4:4を含む。supersampleは1
- チャートは1189×841 mm。referenceのCAM-A/Bは文書の左右を担当し、各カメラのセンサー長辺が文書の縦になる向き（roll 90度）。referenceのチャートmmと文書mmは一致する既知配置を明示
- 入力に渡す真値由来の値は、operatorの代わりに与える3つの粗いanchorだけ。節点ID、12 px単位に丸めた位置、探索半径30 pxをjobとreportに残す。これはanchorを人手で与える検出器の試験で、自動ID決定の実証ではない
- 検出器へK・歪み・pose・真値写像を渡さない。writerへは検出したsessionと明示したjobだけを渡す
- 全18viewの検出点を生成器の真値と照合。出力profileを既存readerで再読し、その製品側投影を、検出点と異なる密な文書点グリッドで生成器の真値と照合する。Kの4値と歪みの5係数についても真値・推定値・差をreportに残す。係数ごとの品質閾値は置かない
- 試験の許容値はrms 0.5 px・max 1.5 px。実行前に試験内へ定めた値で、製品の光学品質閾値ではない。校正writerへは渡さない。reportの品質は常に`not-assessed`
- 評価合成は両referenceの実JPEGを再読したBGR bufferと生成draftを使う。全域が覆われるA0の4 dpi（188×133）プレビューを保存。出力品質や製品のdpiを決定するものではない

## 保存するもの

`view-*`に18枚のJPEGと生成器の真値記録。加えて`detection-job.json`、`detection-report.json`、`session.json`、`job.json`、`profile.json`、`calibration-report.json`、`truth-comparison.json`、`evaluation-preview.png`。実機のserial、SDK、実写は含まない。生成物はignoreされた`build/`内に置く。

## 2026-10-15の結果

Windows / MSVC x64 Release / OpenCV 4.14.0。最終実行は117秒、27チェック・失敗0。18枚すべてのidentity照合が通り、検出した3882点のID取り違えは0。検出点の真値との差はview別rms 0.0546〜0.0701 px、全viewのmaxは0.1825 px。

| referenceの固定写像 | 独立照合点数 | rms | max |
| --- | --- | --- | --- |
| CAM-A | 1764 | 0.0076025 px | 0.0160265 px |
| CAM-B | 1764 | 0.0062110 px | 0.0175993 px |

`truth-comparison.json`にはK・5係数の真値・推定値・差も保存。両referenceの実JPEGから全域のA0評価プレビューを描けた。既存profile/sessionのJSON schema、生成profileの.NET互換性7チェック、統合後の関連Release CTest 7/7（144.66秒）も通過。成果物はローカルの`build/reference/synthetic-full-calibration-20261015-final`に保持した。

この重い試験をDebugでは実行していない。数値計算とwriterのDebug試験は個別文書の通りで、全工程の結果と混ぜない。

## 限界

同じ理想的なBrown–Conradyモデルを使った合成入力でのソフト接続試験。実レンズのぼけ、照明応答、チャートの印刷誤差、紙の浮き、リグ安定性、実写の色・継ぎ目・解像品質は未評価。approvedの発行・期限・再校正triggerは#45、実写リグと品質は#42/#43/#49、撮影ごとの補正は#46に残る。Live Viewの取得・合成はこの試験に含めない。
