# Dual session binding core と WPD alias proof — 設計（Issue #9）

状態: core 実装済み（#9・PR #73）。binding protocol も実装済み（#61）。
正本: `docs/DECISIONS.md` ADR-0025（session-local operator binding）
対象 Issue: #9（core）。後続: #61（binding protocol / fake SDK・完了）、#62（確認UI）、#10（実capture backend）
protocol 側の正本: [../../HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md](../../HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md)

## 1. 何を作るか

ADR-0025 が承認した **session-local operator binding** のネイティブ core。Agent process の
一生存 session の内側だけで CAM-A / CAM-B の割当を保持する状態機械と、対応する
WPD alias の exact-one 証明を作る。

**恒久的な SDK same-body identity は作らない。** これが今回いちばん重要な境界で、
`HG-0003B` が塞がれた理由そのものである。candidate source object・candidate ordinal・
列挙順・USB port はいずれも identity として保存しない。

## 2. 既存契約との関係

| 既存 | 今回の扱い |
| --- | --- |
| `a0.camera-agent.hardware-dual.v2`（`DualHardwareCameraAgentDispatcher`） | **変更しない**。ADR-0025 が明示している |
| `DualIdentityBindingProof` / `DualIdentityCorrelationProvider`（`hardware_camera_agent.hpp`） | documented correlation provider を前提にした旧方式。**触らない**。今回の binding はこれとは別系統で、読み替えもしない |
| `SingleCameraIdentityV3` | CAM-A exact-one の identity-v3。**Dual の binding 証拠へ読み替えない**（ADR-0024 の Dual 境界） |
| binding の wire protocol（`a0.camera-agent.hardware-dual-binding.v1`） | **#61 の担当**。今回は in-process の型付き seam まで |

## 3. 状態機械

```
                begin-binding
   (none) ──────────────────────▶ CollectingCandidates
                                    │   candidate 数が 2 以外 → Rejected
                                    │
                    confirm-alias ×2（各 candidate に exactly once）
                                    ▼
                                AwaitingQuiesce
                                    │   全 candidate の Live View 停止確認
                                    │   ＋ SDK session full close 確認
                                    ▼
                                  Ready ──── invalidation ────▶ Invalid
                                                                 （再 binding まで
                                                                   HardwarePending）
```

`Ready` にするのは **両方の停止と full close を確認した後だけ**。ここを緩めると、
Live View が開いたまま撮影へ入る経路ができる。

### 拒否条件（ADR-0025 完了条件）

- candidate 数が 2 以外（0 / 1 / 3以上）
- 同一 candidate への二重割当
- 同一 alias への二重割当
- `CAM-A` または `CAM-B` の割当不足
- candidate Live View の停止未確認
- SDK session full close 未確認

### invalidation の契機

Agent restart / USB reconnect / camera count・topology 変化 / SDK manager 再生成 /
任意の SDK error。いずれも**即時** `Invalid` にして `HardwarePending` へ戻す。

## 4. 公開してよい証拠

ADR-0025 が限定している。**alias / provider / version / `confirmedAt` / `invalidationReason` のみ。**

preview・raw identifier・serial・source object・candidate ordinal は保存も公開もしない。
テストでもこの5つ以外がシリアライズ面へ出ていないことを assert する。

## 5. capture へ渡す seam

撮影側は binding が保持する source object を**再列挙せずに**使う。再列挙を許すと、
列挙順が変わったときに黙って別の個体を撮ることになる。

型付きの取得口だけを公開し、`Ready` 以外では取得できない設計にする（呼び出し側で
状態を確認させない）。

## 6. WPD alias proof

SDK 撮影後、対応する WPD alias から **exactly one** object を回収する。

- missing / multiple / mismatch は typed failure
- **別 alias の探索をしない**（他方の alias に写っていても拾わない）
- **削除しない**
- **自動 retry しない**（0回固定）
- 失敗時は取得済み PC 原本を保持したまま `FailedPartial`

既存の `DeriveWpdStableIdentity(serial)` は serial からのローカル identity 導出であり、
alias proof はその上に「この alias に対して今この session で見えている object が
ちょうど1つか」を判定する層として置く。

## 7. ファイル構成（予定）

| ファイル | 内容 |
| --- | --- |
| `src/phase0/include/a0/phase0/dual_identity_session_binding.hpp` | 状態機械・candidate・binding・invalidation reason の型 |
| `src/phase0/dual_identity_session_binding.cpp` | 実装 |
| `src/phase0/include/a0/phase0/wpd_alias_proof.hpp` | alias exact-one 証明の型と判定 |
| `src/phase0/wpd_alias_proof.cpp` | 実装 |
| `tests/dual_identity_session_binding_tests.cpp` | 状態機械の網羅 |
| `tests/wpd_alias_proof_tests.cpp` | missing / multiple / mismatch |
| `CMakeLists.txt` | 上記の登録（既存の phase0 ターゲットと ctest への追加） |

fake seam は既存の `fake_camera_transport.{hpp,cpp}` の作り方に合わせる。実 SDK・実 WPD・
実カメラへは触らない（software-only）。

## 8. 試験（Issue #9 の必須試験）

- candidate 数 0 / 1 / 2 / 3
- 同一 candidate 二重割当・同一 alias 二重割当・alias 不足
- Agent restart / USB reconnect / topology 変化 / SDK manager 再生成 / SDK error による invalidation
- Live View stop 失敗・SDK close 失敗で `Ready` にしない
- WPD missing / multiple / mismatch
- capture seam が再列挙しないこと
- camera command 0 回・card 操作 0 回・delete 0 回・retry 0 回

最後の行は **counter では確認しない**（実装時に方針変更）。core も protocol も camera・
card・WPD・SDK の型に一切依存していないため、これらの counter を増やせるコードパスが
そもそも存在しない。「0 を assert する」試験は絶対に落ちず、カバレッジがあるように
見えるだけになる。実際に動く counter（`source_object_reuse_count`、binding session 数、
Live View 開始数など）だけを持ち、依存が無いこと自体は構造的性質としてヘッダに明記する
方針にした。

この判断は #61 で mutation check により裏付けが取れている: 実装をわざと壊すと該当試験が
落ちることを確認した上で残している（詳細は protocol 側ドキュメントの「Quiesce is the
binding core's decision」節）。

## 9. 未確定・確認したいこと

1. ~~**`DualIdentitySessionBinding` の置き場所**~~ → **決着（#9 実装時）**。`a0::phase0` に
   置いた。agent 系・pipe host・fake transport がすべてこの名前空間にあり、binding だけを
   分けると #61 の protocol 層が二つの名前空間をまたぐことになる。名前空間の改称は
   binding 単独ではなく `src/phase0` 全体の話なので、やるなら別 Issue
2. **旧 `DualIdentityBindingProof` 系の扱い**。ADR-0025 で方式が置き換わったので、
   いずれ削除対象になるはず。今回は触らないが、二つの binding 概念が並存する期間が
   できる。整理を別 Issue にするか（#9・#61 とも未着手のまま。**要起票**）
3. WPD alias proof が「今この session で見えている object」を数える具体的な API 面
   （`WpdTransport` のどのメソッドを使うか）。**未決**。`ProveExactlyOneWpdAliasObject` は
   呼び出し側が alias で絞り込んだ観測列を受け取る形にしてあり、その観測をどう作るかは
   実 WPD を触る #10 の担当
