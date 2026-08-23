# 現在の開発状況

更新日: 2026-08-24

## 総合判定

`in-progress`。requirements 2.7.0のMVP要件32件の完全検証はまだ0件である。ADR-0024により最初の`SingleCamera`をCAM-A専用へ固定し、`single-software-active / hardware-capture-paused / dual-identity-blocked`で進行する。WPD digest＋exact-one identity-v3、30日read-only profile、fixed-local byte-identical export、継続Live View v2はsoftware実装済みだが、実D810/WPF受入は未完了である。

第三者向けには[Phase 0 二台カメラ・ショーケース](PHASE0_SHOWCASE.md)を入口とする。二台順次撮影のsoftware contractと安全停止は提示可能だが、実機二台撮影とA0品質の受入完了は主張しない。

## 確認済み

- Dual専用schema `a0.camera-agent.hardware-dual.v2`の4操作、durable pair store、予約→開始→同一ID照会、.NETのReserved／terminal typed recovery、厳密なsemantic preflightを実装済みである。ADR-0025に従う`a0.camera-agent.hardware-dual-binding.v1`のcore（#9）、Agent protocol（#61）、WPF確認UIと再binding表示（#62）もsoftware実装済みで、一台ずつのLive View確認、CAM-A/B exactly-once割当、close確認、無効化、Single fallback禁止をfake SDKで検証した。実SDK candidate provider、実SDK・WPD capture/recovery backend、実WPF撮影は未接続であり、`HardwarePending`、実カメラ操作0件である。
- 正式camera modelはNikon D810であり、明示的な`SingleCamera`または`DualCamera`をUSBで運用する。`SingleCamera`はCAM-A、WPD serial digest、SDK/WPD各exactly-one current session、canonical original一件、stitch `NotApplicable`、byte-identical `7360×4912` export、30日read-only profileとする。`DualCamera`は従来どおり固定平面A0原稿をCAM-A→CAM-Bで順次撮影・合成する。
- modeはactive transaction外で明示選択し、接続台数から推定しない。`DualCamera`の一台不足を`SingleCamera`へ自動降格せず、active中のmode変更を禁止する。
- D810一台の電源再投入後PnP再列挙とWPD側CAM-A continuityを匿名証拠化。SDK側の旧continuity結論はephemeral source ID使用のため無効化した。二台のidentity-v2衝突を恒久identityで解決する方針は採用せず、ADR-0025のsession-local operator bindingへ置換した。
- 一台Live Viewを5分04秒・2,424 frame継続し、停止、SDK close、preview非保存を確認。
- read-only setting runでJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得。`FileType`はnot-advertised。
- 2026-08-07にD810一台を`CAM-A`として再列挙し、設定read-only、10 frame Live View、停止、SDK close、直後のOFF再確認に合格。設定変更、preview保存、撮影、WPD、deleteは0件。
- direct WPD/SDK capture入口をfake-onlyへ閉じ、承認済みhardware経路を`hybrid-capture-single`へ限定。
- 実SDK/WPDコマンドを同じWindowsログオンsession内の二つのPhase 0 processから同時実行しないnamed OS leaseを追加し、別process保持中の拒否を自動試験で確認。別ユーザーsession／serviceはMVP運用外。
- pair/hybrid transactionへ180秒の全体watchdogを適用し、SDK撮影が期限をまたいだ場合もWPD回収・保存・削除・成功状態へ進まないこと、PC保存中に期限をまたいだ場合は`.partial`を保持して`original.jpg` rename・削除・成功状態へ進まないことを確認。
- 2026-08-08時点の変更はSDK有効／なしの両構成でCTest 5/5、当時のM3 simulated Release警告0、Foundation 13/13、Operator Shell 1/1に合格した履歴証拠である。Nikon SDKはcommit対象外の`.tools`へ再配置済み。
- M2Pの光学計算、rig-profile trust、三状態setup-assessment、WI-0022Cの合成画像測定からbounded correction proposalへの接続がsoftware-only合格。shift／rotation／scale／exposure／colorを決定的に測定し、profile provenance mismatch、draft、malformed、over-limitをfail closedする。最終リグ、承認済み閾値、実写A0品質は証明しない。
- StitchJobはADR-0026の`a0.stitch-job-manifest.v1`を唯一のdurable commit pointとして実装した（#40／PR #80）。出力検証後のnon-replacing publish、manifestのatomic publish・再読込検証まで成功にせず、partial、既存成果物、schema/hash不一致を自動cleanup・置換・retryしない。CaptureTransaction、ReviewRecord、ExportRecord、DiagnosticBundleのversioned artifact化は未決であり、この完了主張に含めない。
- M3PのNamed Pipe、durable simulated transaction、起動同意・readiness・操作ロック・失敗復旧を含むWPF shellは、2026-08-10のrequirements 2.6.0に対するfresh Release build 0 warning/0 error、Foundation 19/19、Operator Shell 15/15、`Test-M3Simulated.ps1` Passでsoftware-only合格した。明示Single/Dual、required aliases固定、no-auto-fallback、Single original一件、stitch `NotApplicable`、明示export、起動時操作gateを含む。
- 実機一台WPF経路は、同一transaction ID・alias・承認profile ID/version/SHA/expiry・Live View handoff intentをdurable保存して結果照会するsoftware boundary、検証済みcanonical originalの明示byte-identical export、`TransactionNotFound`時のsupport-required保持まで実装・契約試験済みである。現在の`hardware.v1` Live View handoff結果は撮影後の有限一frame probeを取得後に停止・SDK closeするもので、継続streamの「再開」を主張しない。
- requirements 2.7.0の継続Live View v2 sliceはQA revise後のfresh SDK-less／licensed-SDK-enabled Debug/Release CTest各7/7、.NET Release build警告0・エラー0、Foundation 20/20、Operator Shell 17/17、M3 boundary script、非破壊showcaseに合格した。production backendへ偽SDK/clockを注入し、512 KiB frame境界、session所有権、二重start、heartbeat timeout、単列backpressure、stop/close失敗後のcapture拒否を確認した。実Named Pipeは1 MiB未満／丁度を受理し、1 MiB超過をdispatch前に拒否する。canonical Base64と解析済みv2 rejection envelopeもnegative test済みである。camera command 0のsoftware-onlyで、実機合格ではない。
- C++ `hardware.v1`は、operator-session lease、directory作成から初期journal確定までを含むgap-free transaction reservation/mutex、exactly-one・alias/profile/expiry相関、Live View OFFのWPD前・open SDK shutter-session内再確認、SDK/WPD非重複、canonical originalを同一handleで再検証してwrite/delete禁止のままexact WPD delete・empty-afterまで保持する境界、fixed-local path、no retry、bounded pipe/journalをsoftware contract化した。WI-0022C追加後の2026-08-10 SDK-less Debug/Release全CTestは7/7で、`hardware_camera_agent_contracts`を含め0 failureだった（このsliceのlicensed SDK buildは未実行）。camera commandは送っておらず、実D810、actual JPEG、WPF hardware操作の合格証拠ではない。
- `WI-0010A`のsoftware contractを実装した。MAID `SdkCommandTrace`は唯一のMAID entry boundaryで全`CapStart`を計数・分類し、photographic-setting CapSet、storage-routing CapSet、Live View-control CapSet、capture、unknown/non-capture CapStart、session状態、counter整合性をfail closedする。`sdk-status`は列挙とread-only statusだけを公開する`NikonSdkStatusExecutor`へ分離した。CAM-Aの既定経路はSingleCamera identity-v3をstrict loadし、その合格後だけWPD enumeratorを生成してread-only列挙し、exactly-one D810とcurrent WPD digest一致を検証する。これらが全て合格した後だけSDK executorを生成・列挙・open/probeする二段階routingである。missing／malformed／digest不一致／WPD 0台／複数台ではSDK factory・enumerate・open/probeが全て0となる。明示`--camera-map`だけがlegacy/Dual v2へ入り、CAM-BへSingle identityを流用しない。process routing proofはidentity列挙と禁止対象のWPD/capture/deleteを分けてv5 summaryへ記録する。修正後の実D810 v5再実行は未検証であり、過去のv4 evidenceは変更していない。
- このidentity-v3 route修正はfocused Phase 0／CLI safety／Camera Agent contract、SDK-less Debug/Release全CTest各7/7、licensed-SDK-enabled Debug/Release全CTest各7/7、M3 Release警告0・エラー0、非破壊showcaseに合格した。licensed構成を含めcamera commandは0件である。
- 2026-08-08の再起動後dual binding readinessでD810 PnP、SDK inventory、WPD inventoryを各2台匿名確認した。SDK/WPDともbound 0・unbound 2で`READY_FOR_IDENTITY_BINDING`。inventoryの列挙順自動割当を廃止し、旧SDK mapは値を読まず可逆隔離した。SDK有無各CTest 5/5も合格し、撮影、Live View、設定変更、card操作は実行していない。
- readinessは選択stageの台数とSDK・WPD・PnP件数の完全一致を要求する。現在の二台接続はDualでbinding待ち、Singleではexit 1で拒否され、余分なD810を許可しない。
- WI-0010A完了後の履歴runでは、licensed Release binaryの`verify-dual-identity`がSDK `identity_collision`でfail closedし、恒久的なSDK/WPD相関アンカーが得られないことを確認した。WPD inventory開始、map変更、cardアクセス、capture、Live View、設定write、delete、format、0x9207、retryはなかった。この結果を受け、恒久identity方針はADR-0025のsession-local operator bindingへ置換済みである。一方、SingleCameraの操作者報告ではexactly-oneと`bind-single-identity-v3 --alias CAM-A`がcamera mutation 0でPassした。直後の`sdk-status` routing defectはsoftware修正済みだが、実機再実行までは合格扱いにしない。
- 2026-08-08 17:31 JSTのCAM-A cross-transport bindingは後続の物理入替試験で無効化した。WPDは入替後の個体を未登録として区別した一方、SDKは旧CAM-Aへ誤一致した。原因はMAID source object IDを個体IDとしていた実装であり、当該SDK mapを値を読まず可逆隔離した。過去のSDK側CAM-A continuity主張も再検証対象である。
- SDK identityをdocumented MAID Source `Name`/`Interface`の境界付きlocal-only digest v2へ変更し、source object IDを除外、欠落・不正文字列・二台衝突をfail closedにした。SDK有無Release CTestは各5/5。17:49 JST、現在のCAM-B候補をSDK/WPDへ明示bindingし、各bound 1・unbound 0、Single `READY`を確認した。CAM-Aへ戻した際の別個体判定、再接続、port交換まではcheckpointであり完了扱いにしない。
- 18:14 JSTのCAM-A swap-back申告後もSDK/WPDはともにCAM-B、WPD firmwareは直前のCAM-Bと同じ`V1.11`だったため、本体交換は証拠化できずCAM-A登録を拒否した。WPD identityはPnP device IDから本体報告`WPD_DEVICE_SERIAL_NUMBER`のlocal-only digest v2へ強化し、現在のV1.11個体をCAM-Bへ再登録した。実serial/identityは出力していない。
- SDK/WPDを別々に登録する途中で本体が変わる余地を閉じるため、`bind-cross-transport-identity`を追加した。一つのcamera-control lease内でSDK sessionを完全closeしてからWPDを列挙し、両mapの競合を事前検証後にだけ双方を登録する。競合時の片側map未変更、台数0/複数拒否、引数guardを契約化し、SDK有無Release CTest各5/5と現在のCAM-Bでidempotent実機実行に合格。撮影・Live View・設定・card操作は0件。
- `verify-dual-identity`を追加し、SDK/WPDで台数2、CAM-A/B各1、unbound 0を同時に満たす場合だけ`Ready`とする匿名durable summaryを実装した。台数不一致、未登録、alias cardinality不一致を撮影前に拒否する。現在一台の実機`run-1786182987492-1`はSDK/WPD各1、CAM-B各1を記録し、想定どおり`camera_count_mismatch` / exit 5、map変更・撮影・Live View・設定・card操作0で停止した。
- `verify-dual-spools`を追加し、dual identity `Ready`後だけCAM-A→CAM-Bの順に二つのWPD cardをread-onlyで開き、全payload数が双方0の場合だけ撮影laneを`Ready`にする。片方でも非空なら両体を`spool_not_empty`で停止し、delete/format/vendor operation/retryは行わない。現在一台の`run-1786183481065-1`は`dual_identity_not_ready` / exit 5、`CardInspectionPerformed: false`、WPD session 0、撮影・削除0で停止した。SDK有無Release CTest各5/5。
- 実機`hybrid-capture-pair`の旧入口は共通dual identity検証へ接続し、Readyでない試行をcard access・capture・retryなしで停止するsoftware contractに合格した。ただし、このprovider/proof経路はADR-0025後のproduction binding手順ではなく、#10でsession binding済みsource objectと対応WPD alias recoveryへ置換接続するまで実行対象外である。
- P3Aのprovider config/proof loaderと`VerifyDualIdentitySoftwareContract`は恒久identity案の履歴software contractとして保持する。legacy実CLI、fake inventoryの`Ready`、旧proofをhardware Readyや実体同一性の受入証拠へ読み替えない。
- M3PでCAM-A→CAM-B durable simulated transactionを100/100実行し、両原画像、順序、各100回、retry 0、再起動時の未完了0を確認した。
- 実機用`hybrid-capture-pair`のsoftware contractは1/10/100組、100組集計、初回失敗停止、共有180秒watchdog、p50/p95/max匿名時間集計に合格した。時間値はPhase 0合否には使わない。
- CAM-A完了後・CAM-B開始前のアプリ停止をdurable event logから匿名診断し、完了原本保持、retry禁止、新規transaction必須を復元するrecovery summary contractに合格した。実プロセス停止試験は未実施。
- pair fault contractはCAM-A SDK close失敗時のB未開始と、CAM-B WPD recovery-open失敗時のA原本保持・B原本なし・cleanup/retryなしに合格した。実USB切断・電源断は未実施。
- `hybrid-fault-pair`の履歴software contractはA異常時B未開始、B異常時A原本保持、未確定bodyの原本・delete・retryなし、匿名pair fault summary/reportに合格した。ADR-0025後の実異常試験は、#10でsession bindingを接続した実backendを対象に別途行い、現時点では未実施である。
- `hybrid-interrupt-pair`を追加し、CAM-A verified original保存・exact cleanup後かつCAM-B開始前だけ中断専用gateをreadyにする。gateはprocess終了以外で正常復帰せず、continue markerとtimeoutをfail closedにしてCAM-Bを開始しない。再起動後の`report --run-id`で`after-CAM-A-before-CAM-B`、A原本保持、retry禁止、新規transaction必須を復元する。fake gate/pair契約とSDK有効/無効Release build・CTest各5/5、missing gate・alias/scenario指定のcamera-open前拒否に合格。実process終了は未実施。
- 19:28 JSTの最終read-only再確認`run-1786184898081-1`でもSDK/WPD各1台、CAM-B各1、CAM-A 0、unbound 0だった。`camera_count_mismatch` / exit 5で停止し、map変更、capture、Live View、設定変更、card access、実識別子出力は0。元CAM-A本体の接続・登録なしには二台実機試験を開始できない。
- 2026-08-09のoperator判断により、物理的な電源再投入・再起動と実power-off復旧subtestは`N/A / Skip`としてPhase 0合否項目から除外した。USB切断、software process再起動／pair境界中断、接続順・port確認、二台identity、empty spool、1/10/100 pairは残る。`power-off` CLIとfake安全contractは回帰保護として保持する。
- 2026-08-09に第三者向け[Phase 0 二台カメラ・ショーケース](PHASE0_SHOWCASE.md)と非破壊検証scriptを追加した。SDK有効／なしのRelease build、CTest各5/5、厳選した匿名証拠のschema・安全フラグ検証に合格し、`SOFTWARE_CONTRACT_READY_HARDWARE_PENDING`を出力した。camera command、撮影、Live View、設定変更、card accessは実行していない。
- Live View停止失敗は撮影前の`Idle → FailedPartial`としてjournalへ永続化し、再起動復元、連打防止、明示的新規撮影準備までのロックを自動試験で確認した。
- 2026-08-10のproduct owner指示をADR-0023として記録し、要件を2.6.0へ更新した。これは一台製品modeの実装開始判断であり、実WPF Camera Agent、実JPEG、canonical original export、実機one-shotの合格証拠ではない。

## Partial

- setting readback: native MAID command-traceとSingle identity-v3対応`sdk-status` process-routing software contractは実装・fresh test済み。identity-v3登録後の実D810 v5再実行、focus値の意味、FileType未広告の扱いは未検証／未確定。
- Live View: standaloneは合格だが、実撮影を含むhandoff 10回は未実施。
- identity: 旧SDK CAM-A復元証拠はephemeral source ID使用のため無効。SingleCamera CAM-AはWPD digest＋exactly-one current SDK/WPDのidentity-v3を実装し、操作者報告の一台登録はPassした。旧v2 mapへ自動fallbackせず、欠落・破損・digest不一致・0台／複数台をstatus open前に拒否する。Dualは恒久identityを作らず、ADR-0025のmemory-only session bindingを使用する。Single identity-v3はDualへ流用せず、操作者の誤割当は残留riskである。
- setup/correction: parameter contractとWI-0022Cのsynthetic measurement seamは合格。proposalは測定値・fixture/profile provenanceを保持し、profile envelope外を拒否し、profileを変更しない。実写A0品質、承認済みDual profile、実機性能は未検証である。
- M3P: Dual binding確認UIはkeyboard、focus、読み上げ、Single fallback遮断を含めsoftware検証済みである。実D810、actual JPEG、実WPF画面操作、screen reader実機確認、Single実画面walkthroughは残る。
- SingleCamera製品mode: CAM-A identity-v3、30日profile承認、操作者選択fixed-local folder、byte-identical export、`hardware.v2`継続Live Viewをsoftware実装済み。v2はcanonical Base64を含むmemory-only frame、512 KiB frame上限、1 MiB pipe上限、20秒heartbeat timeout、600秒max lifetime、単列backpressure、session所有権、cleanup失敗時のcapture拒否、stop/SDK-close前のcapture 0、verified capture成功後だけ再startを契約化する。実D810、actual JPEG、実WPF操作、10回handoff、10回p95、100件受入は未検証である。

## Deferred / Waiting

| 項目 | 理由 | 再開条件 |
|---|---|---|
| M1A one-shot、10/10、fault、handoff | 操作者がカード作業を保留。最後の証拠は90 payload | empty cardへの交換または手動backup/clearの報告と明示再開 |
| M1B二台試験 | 二台接続時のSDK identity collisionは、2026-08-20のADR-0025でsession-local operator bindingへ置換して解消した。恒久的な機体識別は作らず、割当はAgent process内のmemory-onlyで、Single identity-v3は流用しない | core（#9）・binding Agent protocol（#61）・確認UI（#62）はsoftware実装済み。実capture backend（#10）と実機受入が残り、DualCameraは`HardwarePending` |
| 実M2 | リグ・A0品質契約未承認 | `HG-0001/0002` |
| SingleCamera製品受入 | identity/profile/export/継続Live View v2 softwareは実装済み。実D810・actual JPEG・10回handoff・10回p95・100件耐久は未実行 | empty spoolと明示再開、実Live View v2/handoff、10回後の`HG-0009`承認、100件実WPF受入 |
| 配布 | native dependency再配布未承認 | `HG-0005` |

既知の90 payload状態は、物理状態が変わるまで再確認しない。撮影、削除、format、USB切断、電源操作も自動では行わない。

## 次の安全な順番

1. 明示的な実機再開後に一台CAM-Aの`sdk-status` v5をidentity-v3経路でread-only再実行し、設定・capture・Live View・card操作0を確認する
2. empty dedicated spoolの用意と明示再開後だけ、one-shot、10回handoff／characterizationへ進む
3. 10回の実測p95を`HG-0009`で承認後、SingleCamera 100件受入を行う
4. Dualは#10の実capture backendを実装し、session binding UI・対応WPD alias proof・専用empty spoolを揃えた後だけ実機1/10/100へ進む

## Open human gates

- `HG-0001`: A0品質・補正上限
- `HG-0002`: 最終リグ・光学条件
- `HG-0005`: Nikon SDK/OpenCV等の再配布
- `HG-0009`: SingleCamera実機10回characterization後のp95目標承認

## 証拠の読み方

- `Pass`: その項目の明示contractをfresh evidenceで満たす。
- `Partial`: 一部のlayerまたは環境だけが確認済み。
- `Unverified`: 必要な実行証拠がない。
- software-only、旧one-camera、simulatedの結果を実WPF Camera Agent、一台製品受入、二台実機、A0品質、MVP受入へ読み替えない。
