# PC直接保存 実機評価レポート

- 実行日: 2026-09-16
- source: `7bb042c4693acc40fceb59b4ffb4d76123ad9b52`
- 対象: `CAM-A`、Nikon D810一台
- 結果: `FailedPartial / image_event_timeout`
- terminal subreason: `sdram-item-missing`
- 所要時間: 18,101 ms
- local summary SHA-256: `73022B89390511E37B0DB1366F990D462803DD94C5EC99192E1CC8C6FC3022CE`

## 観測結果

- capture attempt: 1
- `CaptureComplete`: 0
- post-baseline採用候補: 0
- forced enumeration: 727/727成功、失敗0
- PC `original.jpg`／`original.jpg.partial`: 0
- SaveMedia復元: attempted／confirmed
- automatic retry、card fallback、camera delete、format: 0
- 事前／事後read-only spool: payload 0、`EMPTY`
- 最終camera-control process: 0

採用候補0はbaseline filter後の結果であり、生のSDK callbackが0だったとは断定しない。`cardUnchanged=false`はafter fingerprint取得不能を表し、card mutationの証拠ではない。独立した事後WPD確認はpayload 0だった。旧SDRAM Item残存またはItem ID再利用は仮説であり未確定である。実画像、実識別子、serial、SDK配布物、生ログはrepositoryへ保存しない。
