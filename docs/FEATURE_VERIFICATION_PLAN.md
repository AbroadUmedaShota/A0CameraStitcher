# アプリ機能検証計画

## 目的

アプリに必要な機能を、`実機不要`、`D810一台で自動実行可能`、`操作者の物理操作が必要`、`D810二台が必要`に分ける。実機不要と一台の非破壊検証を先行し、カード交換、カメラ移動、USB切断、電源操作、二台目準備が必要な項目は保留しても、独立した後続項目を止めない。

結果は`Pass`、`Partial`、`Fail`、`Deferred`で記録する。software-onlyまたは一台の結果を、二台固定リグ、実写A0品質、MVP受入の合格へ読み替えない。

`Pass`項目は、関係コード、設定、接続状態、または受入contractが変わった時だけ再実行する。既知の物理阻害条件を確認するだけの反復実行は行わず、次の独立したsoftware-only項目へ進む。

## 実行優先順位

1. 実機不要の純粋計算、schema、契約、failure path
2. D810一台の読み取り専用状態確認
3. D810一台をSDKで一時使用するが、操作者の物理操作を必要としない確認
4. 専用空カード、校正chart、カメラ移動、USB切断、電源操作が必要な試験
5. 二台目D810が必要な試験

## 検証マトリクス

| ID | 対象 | 観測する結果 | 必要環境 | 現在 |
| --- | --- | --- | --- | --- |
| V-SAFE-001 | 旧撮影入口 | direct `capture-single/pair/stability`が実SDK/WPDをcamera open前に拒否 | 実機不要 | Pass 2026-08-07、fake-only validatorと実CLI negative test |
| V-SAFE-002 | operator session内process横断排他 | 実SDK/WPD commandが同じWindowsログオンsessionのnamed OS leaseを一件だけ保持 | 実機不要 | Pass 2026-08-07、別process保持中の拒否、正常終了後の再取得contract。別user session／serviceはMVP運用外 |
| V-SAFE-003 | transaction watchdog | pair/hybridが180秒deadlineを共有し、期限切れ後に成功処理・次transport・canonical rename・deleteへ進まない | 実機不要 | Pass 2026-08-07、zero-budget、SDK capture途中超過、original rename直前超過のnegative contract |
| V-SW-001 | Phase 0 transaction | success、partial、ambiguity、保存、redaction、no retry | 実機不要 | Pass 2026-08-07、SDKあり／なしのPhase 0 contract suite内 |
| V-SW-002 | A0光学候補 | 150/180/200 DPI、回転、重複、crop、shortfall、境界拒否 | 実機不要 | Pass 2026-08-07、SDKあり／なし各CTest 5/5内 |
| V-SW-003 | rig profile資産 | draft/approved、version、provenance/validity、correction envelope、fixture不変 | 実機不要 | Pass 2026-08-07、schema 1.1のshape/typeとruntime cross-field/expiry contract |
| V-SET-001 | setup-assist判定 | `ready`、`ready-auto-correction`、`physical-adjustment-required` | 実機不要 | Pass 2026-08-07、SDKあり／なし両構成 |
| V-SET-002 | 補正安全境界 | draft、unsupported version、provenance欠落、期限切れ、時刻順序不整合、画角不足、設定不整合、重複不足、上限超過をfail closed | 実機不要 | Pass 2026-08-07、全hard blocker contract |
| V-SET-003 | 補正契約の純粋性 | 入力profile不変、閾値既定値なし、invalid値拒否、決定性 | 実機不要 | Pass 2026-08-07、境界・invalid・immutability contract |
| V-SET-004 | synthetic画像補正 | known shift/rotation/scale/exposure/color差を測定し、上限内だけ補正 | 実機不要/OpenCV | `HG-0001/0002`を確定しないpre-gateとして後続 |
| V-M3P-001 | simulated app | IPC、永続transaction、crash/restart、画面上のSimulated表示 | 実機不要/.NET 10 | Pass 2026-08-07、Release build警告0・Foundation 11/11・検証script合格 |
| V-M3P-002 | operator workflow | 起動同意、全readiness blocker、Ready/補正範囲内、active時全操作ロック、失敗別結果、別job再合成、明示保存、新規撮影準備 | 実機不要/.NET 10 | Pass 2026-08-07、contract・static shell・Windows UI Automationで連続二回Invoke時transaction一件、明示保存一件、Ready復帰。screen reader/keyboard/focusはPartial |
| V-1CAM-001 | CAM-A接続状態 | SDK statusが一台を解決し、Live View状態等を取得後session close | D810一台、読取専用 | Pass、2026-08-07に`run-1786077278290-1`で再確認。設定変更0・Live View開始0・session close |
| V-1CAM-001W | CAM-A WPD状態 | functional targetを一意に選択し、capture/vendor operationを送らない | D810一台、読取専用 | Partial `run-1786036293601-1`、target選択成功、vendor queryはread-only権限拒否、変更操作0 |
| V-1CAM-002 | 一台Live View | frame取得、stop、SDK close、preview非保存 | D810一台、SDK一時操作 | Pass、長時間`run-1786036360495-1`に加え、2026-08-07の`run-1786077291889-1`で10 frame・stop/close・preview保存0、直後`run-1786077302493-1`でOFF再確認 |
| V-1CAM-003 | カメラ設定比較用情報 | JPEG Fine L、露出、ISO、WB、focus等を撮影設定へ書き込まず取得 | D810一台、設定read-only | Partial `run-1786040075194-1`。JPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1を取得。focusはopaque値1、FileTypeはnot-advertised。SDK control-plane callback登録は既存`CapSet`を使い得るが撮影設定write・capture・Live View開始・WPD・deleteは0。native command-trace testは未実装 |
| V-1CAM-004 | 一台レンズ校正案内 | target検出、レンズ歪み候補、profile provenance | D810一台、校正chart | `HG-0001/0002`の数値承認前は候補・Partialのみ |
| V-CARD-001 | one-shot回収 | empty-before、SDK一回撮影、WPD回収、PC原本、exact delete、empty-after | 専用空カード | 操作者が保留中のためDeferred |
| V-FAULT-001 | 切断・電源異常 | `FailedPartial`、no retry、復旧後は新規transaction | 空カード＋物理切断/電源操作 | Deferred |
| V-RIG-001 | 二台固定校正 | lens、fixed transform、overlap、seam、crop、baseline residual | D810二台＋承認chart | `HG-0001/0002/0003B`待ち |
| V-RIG-002 | 二台設置アシスタント | 自動補正可否、調整案内、profile再現性 | D810二台＋固定リグ | Deferred |
| V-PAIR-001 | 二台順次撮影 | 10件、100/100、誤pair・原本消失・自動retry 0 | D810二台＋空カード | `HG-0003B`待ち |

## Setup-assist受入contract

- 閾値は承認済みrig profileから受け取り、プログラム内に製品既定値を持たない。
- profileのstatus、supported schema version、provenance、校正日時、有効期限を判定器自身が検査し、期限切れや不整合を呼出側の単純な真偽値へ委ねない。
- 全画角、設定整合、最小重複のhard conditionを先に評価する。
- 位置、回転、倍率、露出、色は、目標上限と自動補正上限を別々に評価する。
- 目標内は`ready`、目標外かつ補正上限内は`ready-auto-correction`、補正上限外は`physical-adjustment-required`とする。
- 補正はtransaction限りで、rig profileを自動更新しない。
- 補正量と判定理由を記録する。上限外をclampして成功扱いにしない。

## 保留ルール

物理操作が必要になった時点で、その検証を`Deferred`として、必要な物、操作、再開条件を記録する。別の実機不要・一台非破壊項目へ移り、カード内既存データの削除、format、カメラ設定の無断変更、USB切断、電源操作を自動で行わない。
