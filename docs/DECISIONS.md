# 意思決定記録

## ADR-0001: USBのみの撮影トランザクション

- 状態: Accepted for Phase 0
- 決定: ハードウェア同期装置を追加せず、PCから2台へ撮影命令を発行する。
- 理由: ユーザーが追加ハードウェアを対象外とした。対象は静止した平面原稿である。
- 影響: 実シャッター時刻差は保証しない。品質は合成結果とトランザクション整合性で判定する。

## ADR-0002: Nikon Camera Remote SDKを二台制御へ使わない

- 状態: Accepted
- 決定: Camera Remote SDKを二台同時制御の製品経路に採用しない。
- 根拠: Nikon公式FAQは複数カメラ同時制御を非対応としている。
- 影響: WPD/PTPの実機成立性をPhase 0で確認する。

## ADR-0003: JPEG Fineから開始

- 状態: Accepted for MVP
- 決定: Phase 0とMVPの初期入力をJPEG Fineとする。
- 理由: カメラ制御、転送、合成の成立性をRAW現像・ライセンス問題から分離する。
- 影響: NEF、16-bit処理、TIFFはMVP後とする。

## ADR-0004: 平面A0原稿を第一対象にする

- 状態: Accepted for MVP planning
- 決定: 遠景より先に、固定した平面A0級原稿を対象とする。
- 理由: 棚卸し表の目的が「A1より大きいサイズの原稿を一発撮り」である。
- 影響: MVPの投影はPlanar Homography。円筒・球面投影は対象外。

## ADR-0005: 技術スタックはPhase 0後に確定

- 状態: Proposed
- 候補: .NET 10/WPF、C++20/WPD/PTP、OpenCV、Named Pipe、SQLite。
- 未確定: WPD/PTPの実装経路、OpenCV取得方法、Camera Agent分離の最終形。
- 判定: Phase 0 transport decision gateで更新する。
