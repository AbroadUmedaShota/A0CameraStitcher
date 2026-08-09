# 現在の開発状況

更新日: 2026-08-09

## 総合判定

`in-progress`。MVP要件31件の完全検証はまだ0件である。ただしプロジェクト全体が停止しているわけではなく、`software-active / hardware-capture-paused / dual-camera-waiting`の三状態で進行する。

第三者向けには[Phase 0 二台カメラ・ショーケース](PHASE0_SHOWCASE.md)を入口とする。二台順次撮影のsoftware contractと安全停止は提示可能だが、実機二台撮影とA0品質の受入完了は主張しない。

## 確認済み

- 正式対象はNikon D810二台、USB、固定平面A0原稿、順次撮影。
- D810一台の電源再投入後PnP再列挙とWPD側CAM-A continuityを匿名証拠化。SDK側の旧continuity結論はephemeral source ID使用のため無効化し、identity-v2で再検証中。
- 一台Live Viewを5分04秒・2,424 frame継続し、停止、SDK close、preview非保存を確認。
- read-only setting runでJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得。`FileType`はnot-advertised。
- 2026-08-07にD810一台を`CAM-A`として再列挙し、設定read-only、10 frame Live View、停止、SDK close、直後のOFF再確認に合格。設定変更、preview保存、撮影、WPD、deleteは0件。
- direct WPD/SDK capture入口をfake-onlyへ閉じ、承認済みhardware経路を`hybrid-capture-single`へ限定。
- 実SDK/WPDコマンドを同じWindowsログオンsession内の二つのPhase 0 processから同時実行しないnamed OS leaseを追加し、別process保持中の拒否を自動試験で確認。別ユーザーsession／serviceはMVP運用外。
- pair/hybrid transactionへ180秒の全体watchdogを適用し、SDK撮影が期限をまたいだ場合もWPD回収・保存・削除・成功状態へ進まないこと、PC保存中に期限をまたいだ場合は`.partial`を保持して`original.jpg` rename・削除・成功状態へ進まないことを確認。
- 2026-08-08の現変更はSDK有効／なしの両構成でCTest 5/5、M3 simulated Release警告0、Foundation 13/13、Operator Shell 1/1に合格した。Nikon SDKはcommit対象外の`.tools`へ再配置済み。
- M2Pの光学計算、rig-profile trust、三状態setup-assessment contractがsoftware-only合格。
- M3PのNamed Pipe、durable simulated transaction、起動同意・readiness・操作ロック・失敗復旧を含むWPF shellがsimulation-only合格。
- 2026-08-08の再起動後dual binding readinessでD810 PnP、SDK inventory、WPD inventoryを各2台匿名確認した。SDK/WPDともbound 0・unbound 2で`READY_FOR_IDENTITY_BINDING`。inventoryの列挙順自動割当を廃止し、旧SDK mapは値を読まず可逆隔離した。SDK有無各CTest 5/5も合格し、撮影、Live View、設定変更、card操作は実行していない。
- readinessは選択stageの台数とSDK・WPD・PnP件数の完全一致を要求する。現在の二台接続はDualでbinding待ち、Singleではexit 1で拒否され、余分なD810を許可しない。
- 2026-08-08 17:31 JSTのCAM-A cross-transport bindingは後続の物理入替試験で無効化した。WPDは入替後の個体を未登録として区別した一方、SDKは旧CAM-Aへ誤一致した。原因はMAID source object IDを個体IDとしていた実装であり、当該SDK mapを値を読まず可逆隔離した。過去のSDK側CAM-A continuity主張も再検証対象である。
- SDK identityをdocumented MAID Source `Name`/`Interface`の境界付きlocal-only digest v2へ変更し、source object IDを除外、欠落・不正文字列・二台衝突をfail closedにした。SDK有無Release CTestは各5/5。17:49 JST、現在のCAM-B候補をSDK/WPDへ明示bindingし、各bound 1・unbound 0、Single `READY`を確認した。CAM-Aへ戻した際の別個体判定、再接続、port交換まではcheckpointであり完了扱いにしない。
- 18:14 JSTのCAM-A swap-back申告後もSDK/WPDはともにCAM-B、WPD firmwareは直前のCAM-Bと同じ`V1.11`だったため、本体交換は証拠化できずCAM-A登録を拒否した。WPD identityはPnP device IDから本体報告`WPD_DEVICE_SERIAL_NUMBER`のlocal-only digest v2へ強化し、現在のV1.11個体をCAM-Bへ再登録した。実serial/identityは出力していない。
- SDK/WPDを別々に登録する途中で本体が変わる余地を閉じるため、`bind-cross-transport-identity`を追加した。一つのcamera-control lease内でSDK sessionを完全closeしてからWPDを列挙し、両mapの競合を事前検証後にだけ双方を登録する。競合時の片側map未変更、台数0/複数拒否、引数guardを契約化し、SDK有無Release CTest各5/5と現在のCAM-Bでidempotent実機実行に合格。撮影・Live View・設定・card操作は0件。
- `verify-dual-identity`を追加し、SDK/WPDで台数2、CAM-A/B各1、unbound 0を同時に満たす場合だけ`Ready`とする匿名durable summaryを実装した。台数不一致、未登録、alias cardinality不一致を撮影前に拒否する。現在一台の実機`run-1786182987492-1`はSDK/WPD各1、CAM-B各1を記録し、想定どおり`camera_count_mismatch` / exit 5、map変更・撮影・Live View・設定・card操作0で停止した。
- `verify-dual-spools`を追加し、dual identity `Ready`後だけCAM-A→CAM-Bの順に二つのWPD cardをread-onlyで開き、全payload数が双方0の場合だけ撮影laneを`Ready`にする。片方でも非空なら両体を`spool_not_empty`で停止し、delete/format/vendor operation/retryは行わない。現在一台の`run-1786183481065-1`は`dual_identity_not_ready` / exit 5、`CardInspectionPerformed: false`、WPD session 0、撮影・削除0で停止した。SDK有無Release CTest各5/5。
- 実機`hybrid-capture-pair`の入口も共通dual identity検証へ接続した。Readyでない試行は匿名`dual-identity-verification-summary.json`を残し、card access・capture・retryなしでexit 5とする。SDK有効/無効Release buildとCTest各5/5に合格。現在一台のため、安全確認フラグを未確認のまま実機コマンドは実行していない。
- M3PでCAM-A→CAM-B durable simulated transactionを100/100実行し、両原画像、順序、各100回、retry 0、再起動時の未完了0を確認した。
- 実機用`hybrid-capture-pair`のsoftware contractは1/10/100組、100組集計、初回失敗停止、共有180秒watchdog、p50/p95/max匿名時間集計に合格した。時間値はPhase 0合否には使わない。
- CAM-A完了後・CAM-B開始前のアプリ停止をdurable event logから匿名診断し、完了原本保持、retry禁止、新規transaction必須を復元するrecovery summary contractに合格した。実プロセス停止試験は未実施。
- pair fault contractはCAM-A SDK close失敗時のB未開始と、CAM-B WPD recovery-open失敗時のA原本保持・B原本なし・cleanup/retryなしに合格した。実USB切断・電源断は未実施。
- `hybrid-fault-pair`を追加し、CAM-A/Bを明示選択して各bodyのSDK完全close後・WPD recovery前にUSB切断または電源断を行う実機入口を用意した。共通dual identity gate、両専用spool確認、operator-session lease、明示alias、operator gateを必須とする。fake契約はA異常時B未開始、B異常時A原本保持、未確定bodyの原本・delete・retryなし、匿名pair fault summary/reportを確認。SDK有効/無効Release buildとCTest各5/5、CLI不足引数2件のcamera-open前拒否に合格。実異常注入は未実施。
- `hybrid-interrupt-pair`を追加し、CAM-A verified original保存・exact cleanup後かつCAM-B開始前だけ中断専用gateをreadyにする。gateはprocess終了以外で正常復帰せず、continue markerとtimeoutをfail closedにしてCAM-Bを開始しない。再起動後の`report --run-id`で`after-CAM-A-before-CAM-B`、A原本保持、retry禁止、新規transaction必須を復元する。fake gate/pair契約とSDK有効/無効Release build・CTest各5/5、missing gate・alias/scenario指定のcamera-open前拒否に合格。実process終了は未実施。
- 19:28 JSTの最終read-only再確認`run-1786184898081-1`でもSDK/WPD各1台、CAM-B各1、CAM-A 0、unbound 0だった。`camera_count_mismatch` / exit 5で停止し、map変更、capture、Live View、設定変更、card access、実識別子出力は0。元CAM-A本体の接続・登録なしには二台実機試験を開始できない。
- 2026-08-09のoperator判断により、物理的な電源再投入・再起動と実power-off復旧subtestは`N/A / Skip`としてPhase 0合否項目から除外した。USB切断、software process再起動／pair境界中断、接続順・port確認、二台identity、empty spool、1/10/100 pairは残る。`power-off` CLIとfake安全contractは回帰保護として保持する。
- 2026-08-09に第三者向け[Phase 0 二台カメラ・ショーケース](PHASE0_SHOWCASE.md)と非破壊検証scriptを追加した。SDK有効／なしのRelease build、CTest各5/5、厳選した匿名証拠のschema・安全フラグ検証に合格し、`SOFTWARE_CONTRACT_READY_HARDWARE_PENDING`を出力した。camera command、撮影、Live View、設定変更、card accessは実行していない。
- Live View停止失敗は撮影前の`Idle → FailedPartial`としてjournalへ永続化し、再起動復元、連打防止、明示的新規撮影準備までのロックを自動試験で確認した。

## Partial

- setting readback: native MAID command trace、focus値の意味、FileType未広告の扱いが残る。
- Live View: standaloneは合格だが、実撮影を含むhandoff 10回は未実施。
- identity: 旧SDK CAM-A復元証拠はephemeral source ID使用のため無効。WPD側CAM-Aとidentity-v2のCAM-B checkpointは保持し、CAM-A SDK v2再登録、別個体判定、再接続、物理cable/port変更が未実施。
- setup/correction: parameter contractは合格。画像から測定値を生成して補正候補へ接続する処理は未実装。
- M3P: 実Camera Agent、実JPEG、実D810との統合証拠ではない。WPF連打防止・明示保存・Ready復帰はUI Automation合格だが、screen reader、keyboard/focus walkthroughは残る。

## Deferred / Waiting

| 項目 | 理由 | 再開条件 |
|---|---|---|
| M1A one-shot、10/10、fault、handoff | 操作者がカード作業を保留。最後の証拠は90 payload | empty cardへの交換または手動backup/clearの報告と明示再開 |
| M1B二台試験 | D810 PnP/SDK/WPD各2台のread-only inventoryと実機pair CLI software contractを確認済み。SDK Name/Interface v2・WPD device-serial v2でV1.11個体のCAM-B checkpointまで完了。物理power-cycle/power-off復旧はN/A | 現在のV1.11本体を外して、履歴上V1.14の元CAM-A本体だけを接続。SDK/WPD双方の別個体判定後にCAM-A登録、接続順・port確認、二台readiness |
| 実M2 | リグ・A0品質契約未承認 | `HG-0001/0002` |
| 配布 | native dependency再配布未承認 | `HG-0005` |

既知の90 payload状態は、物理状態が変わるまで再確認しない。撮影、削除、format、USB切断、電源操作も自動では行わない。

## 次の安全な順番

1. `WI-0010A`: SDK setting readbackのnative command trace
2. `WI-0022C`: synthetic measurementからbounded setup decisionへの接続
3. simulated journalと実Camera Agent置換差分の強化
4. 操作者再開時だけM1A one-shot
5. 一台ずつ`bind-cross-transport-identity`を実行し、SDK/WPD双方のCAM-A/B binding完成後にM1B

## Open human gates

- `HG-0001`: A0品質・補正上限
- `HG-0002`: 最終リグ・光学条件
- `HG-0003B`: 二台D810のPnP存在は確認済み。SDK/WPD bindingと二台実機試験は未完了
- `HG-0005`: Nikon SDK/OpenCV等の再配布

## 証拠の読み方

- `Pass`: その項目の明示contractをfresh evidenceで満たす。
- `Partial`: 一部のlayerまたは環境だけが確認済み。
- `Unverified`: 必要な実行証拠がない。
- software-only、one-camera、simulatedの結果を二台実機・A0品質・MVP受入へ読み替えない。
