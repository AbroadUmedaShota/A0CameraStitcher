# 現在の開発状況

更新日: 2026-08-07

## 総合判定

`in-progress`。MVP要件31件の完全検証はまだ0件である。ただしプロジェクト全体が停止しているわけではなく、`software-active / hardware-capture-paused / dual-camera-waiting`の三状態で進行する。

## 確認済み

- 正式対象はNikon D810二台、USB、固定平面A0原稿、順次撮影。
- D810一台を`CAM-A`としてSDK/WPD双方で復元し、電源再投入後のPnP再列挙を匿名証拠化。
- 一台Live Viewを5分04秒・2,424 frame継続し、停止、SDK close、preview非保存を確認。
- read-only setting runでJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得。`FileType`はnot-advertised。
- 2026-08-07にD810一台を`CAM-A`として再列挙し、設定read-only、10 frame Live View、停止、SDK close、直後のOFF再確認に合格。設定変更、preview保存、撮影、WPD、deleteは0件。
- direct WPD/SDK capture入口をfake-onlyへ閉じ、承認済みhardware経路を`hybrid-capture-single`へ限定。
- 実SDK/WPDコマンドを同じWindowsログオンsession内の二つのPhase 0 processから同時実行しないnamed OS leaseを追加し、別process保持中の拒否を自動試験で確認。別ユーザーsession／serviceはMVP運用外。
- pair/hybrid transactionへ180秒の全体watchdogを適用し、SDK撮影が期限をまたいだ場合もWPD回収・保存・削除・成功状態へ進まないこと、PC保存中に期限をまたいだ場合は`.partial`を保持して`original.jpg` rename・削除・成功状態へ進まないことを確認。
- 最新ソースをSDK有効／なしの両構成でクリーンビルドし、各CTest 5/5を確認。M3 simulated Release buildも警告0、契約11/11を確認。
- M2Pの光学計算、rig-profile trust、三状態setup-assessment contractがsoftware-only合格。
- M3PのNamed Pipe、durable simulated transaction、起動同意・readiness・操作ロック・失敗復旧を含むWPF shellがsimulation-only合格。

## Partial

- setting readback: native MAID command trace、focus値の意味、FileType未広告の扱いが残る。
- Live View: standaloneは合格だが、実撮影を含むhandoff 10回は未実施。
- identity: CAM-Aの電源再投入後復元は合格。物理cable/port変更とCAM-Bは未実施。
- setup/correction: parameter contractは合格。画像から測定値を生成して補正候補へ接続する処理は未実装。
- M3P: 実Camera Agent、実JPEG、実D810との統合証拠ではない。WPF連打防止・明示保存・Ready復帰はUI Automation合格だが、screen reader、keyboard/focus walkthroughは残る。

## Deferred / Waiting

| 項目 | 理由 | 再開条件 |
|---|---|---|
| M1A one-shot、10/10、fault、handoff | 操作者がカード作業を保留。最後の証拠は90 payload | empty cardへの交換または手動backup/clearの報告と明示再開 |
| M1B二台試験 | 二台目D810なし | `HG-0003B` |
| 実M2 | リグ・A0品質契約未承認 | `HG-0001/0002` |
| 配布 | native dependency再配布未承認 | `HG-0005` |

既知の90 payload状態は、物理状態が変わるまで再確認しない。撮影、削除、format、USB切断、電源操作も自動では行わない。

## 次の安全な順番

1. `WI-0010A`: SDK setting readbackのnative command trace
2. `WI-0022C`: synthetic measurementからbounded setup decisionへの接続
3. simulated journalと実Camera Agent置換差分の強化
4. 操作者再開時だけM1A one-shot
5. 二台目入手後にM1B

## Open human gates

- `HG-0001`: A0品質・補正上限
- `HG-0002`: 最終リグ・光学条件
- `HG-0003B`: 二台目D810と二台試験
- `HG-0005`: Nikon SDK/OpenCV等の再配布

## 証拠の読み方

- `Pass`: その項目の明示contractをfresh evidenceで満たす。
- `Partial`: 一部のlayerまたは環境だけが確認済み。
- `Unverified`: 必要な実行証拠がない。
- software-only、one-camera、simulatedの結果を二台実機・A0品質・MVP受入へ読み替えない。
