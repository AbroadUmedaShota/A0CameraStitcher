# PC直接保存 実機評価レポート

- 実行日: 2026-09-16
- source: `f2a24b83e2dff066b32d5681cfeb18a8c738fa85`
- 対象: `CAM-A`、Nikon D810一台
- 結果: `FailedPartial / image_event_timeout`
- terminal subreason: `capture-complete-missing`
- 所要時間: 18,852 ms

## 観測結果

- capture attempt: 1
- `CaptureComplete`: 0
- `AddChild`: 785（同一Item IDの重複784を含む）
- forced enumeration: 839/839成功、失敗0
- post-baseline distinct notified/enumerated Item: 各1
- candidate: 1
- PC `original.jpg`／`original.jpg.partial`: 0
- SaveMedia復元: attempted／confirmed
- automatic retry、card fallback、camera delete、format: 0
- 事後read-only spool: payload 0、`EMPTY`

`AddChild` 785は785個の画像を意味せず、同一IDの再通知を含む。PC原本を確定できていないため、本runはPC直接保存の合格証拠ではない。実画像、実識別子、serial、SDK配布物、生ログはrepositoryへ保存しない。
