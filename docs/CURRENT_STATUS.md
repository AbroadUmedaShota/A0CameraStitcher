# 現在の開発状況

更新日: 2026-08-31

## 総合判定

`in-progress / Dual hardware Blocked`。2026-08-26にNikon D810一台、`SingleCamera`、`CAM-A`のCamera Agent撮影経路でone-shot 1/1、10/10 characterization、p95 `14.643秒`のProduct Owner承認、100/100耐久を完了した。原画像111件の再検証と安全指標も合格した。ただし、実WPF画面からの100回操作、Continuous Live View handoff 10回、物理USB切断・保存先障害は未検証であり、SingleCamera全体は`Partial`である。DualCameraはproduction backendとWPF `CaptureRecoveryOnly` software経路まで実装済みだが、2026-08-31の安全レビューで「WPD前のSDK完全終了」と「session-local tokenを再列挙せず継続利用」の両立不能を確認したため、実機one-shot／10回／100回は開始せず`Blocked`としている。A0合成品質も未完了である。

第三者向けには[Phase 0 二台カメラ・ショーケース](PHASE0_SHOWCASE.md)を入口とする。二台順次撮影のsoftware contractと安全停止は提示可能だが、実機二台撮影とA0品質の受入完了は主張しない。

## 確認済み

- [2026-08-31 DualCamera安全監査](DUAL_HARDWARE_SAFETY_AUDIT_2026-08-31.md): AOPC-22-NOTEのSDK-only／WPD-only読み取り専用ゲートはD810各2台、CAM-A/B map各1、両payload 0、全session close、process 0でPassした。安全修正`29b8671`はNative CTest 20/20、Foundation 37/37、OperatorShell 63/63、独立reviewでpatch `PASS`となりpush済み。一方、production captureはSDK Moduleを保持したままWPDへ進むため現行のfull-close条件を満たさない。ライセンス済みSDKのheader 22件、sample 6件、PDF 10件の静的調査でも、Module reload後に同型D810を一意照合できる正式なper-body property/APIは確認できなかった。pair開始前の両カード一括preflightと同一bindingの10/100 runnerも未実装であり、Dual hardware readinessは`BLOCK`。実撮影、delete、設定変更、USB操作は0件。
- 2026-08-29時点で、WPFの明示起動引数、承認済み`a0.dual-capture-profile.operator-approved.v1`、同一Agent内CAM-A/B割当から`CaptureRecoveryOnly`へのactivation、予約前durable snapshot、1 reserve／1 start、曖昧時same-ID照会、再起動時の再binding、CAM-B失敗時CAM-A原本保持、JPEG 7360×4912・size・SHA-256再検証、stitch `Pending`・A0品質`Unapproved`を実装した。通常`start-reserved-pair` schemaと通常合成経路は変更していない。この項目はsoftware-onlyで、実シャッター、実WPF操作、実機one-shotの合格を意味しない。
- [2026-08-26 SingleCamera実機結果](SINGLE_CAMERA_HARDWARE_RESULTS_2026-08-26.md): one-shot 1/1、10/10、p50 `14.036秒`、p95/max `14.643秒`、HG-0009承認、100/100初回成功、100回p50 `14.204秒`・p95 `14.430秒`・max `14.692秒`。計111原画像のJPEG寸法・size・SHA-256再検証に合格し、原画像消失・誤削除・曖昧採用・自動retry・復旧不能停止は各0件だった。
- PR #163でNikon SDK非同期バッファ寿命とSingleCamera WPD identity-v3経路を修正し、Release buildとfocused contractsを確認後にmainへマージした。GitHub ActionsはBilling制限により未実行であり、CI greenとは扱わない。
- 2026-08-17時点でDual専用schema `a0.camera-agent.hardware-dual.v2`の4操作、durable pair store、予約→開始→同一ID照会、.NETのReserved／terminal typed recovery、厳密なsemantic preflightを実装済みである。fake backend限定orchestratorはCAM-A→CAM-Bを各最大一回、自動retry 0、共有180秒deadlineで実行する。A失敗時はBを開始せず、B失敗時はA原本を保持する。terminalはtransaction ID別にatomic publish・再読込検証され、その後だけactiveを削除する。過去結果を残したまま次pairを予約でき、再起動後も同一IDで照会できる。Foundation 22/22、DualCamera 18/18、Operator Shell 22/22、SDK-less／licensed Debug/Release CTest各10/10、M3 Release/Debug、正式DualCamera WPF flowが合格した。当時はproduction Dual経路が未接続だったが、この制約は上記2026-08-29のsoftware実装で更新された。実機受入が未完了で`HardwarePending`である点は変わらない。
- 正式camera modelはNikon D810であり、明示的な`SingleCamera`または`DualCamera`をUSBで運用する。`SingleCamera`はCAM-A、WPD serial digest、SDK/WPD各exactly-one current session、canonical original一件、stitch `NotApplicable`、byte-identical `7360×4912` export、30日read-only profileとする。`DualCamera`は従来どおり固定平面A0原稿をCAM-A→CAM-Bで順次撮影・合成する。
- modeはactive transaction外で明示選択し、接続台数から推定しない。`DualCamera`の一台不足を`SingleCamera`へ自動降格せず、active中のmode変更を禁止する。
- D810一台の電源再投入後PnP再列挙とWPD側CAM-A continuityを匿名証拠化。SDK側の旧continuity結論はephemeral source ID使用のため無効化され、接続中二台のidentity-v2衝突によりidentity strategyはBlockedである。
- 一台Live Viewを5分04秒・2,424 frame継続し、停止、SDK close、preview非保存を確認。
- read-only setting runでJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得。`FileType`はnot-advertised。
- 2026-08-07にD810一台を`CAM-A`として再列挙し、設定read-only、10 frame Live View、停止、SDK close、直後のOFF再確認に合格。設定変更、preview保存、撮影、WPD、deleteは0件。
- direct WPD/SDK capture入口をfake-onlyへ閉じ、承認済みhardware経路を`hybrid-capture-single`へ限定。
- 実SDK/WPDコマンドを同じWindowsログオンsession内の二つのPhase 0 processから同時実行しないnamed OS leaseを追加し、別process保持中の拒否を自動試験で確認。別ユーザーsession／serviceはMVP運用外。
- pair/hybrid transactionへ180秒の全体watchdogを適用し、SDK撮影が期限をまたいだ場合もWPD回収・保存・削除・成功状態へ進まないこと、PC保存中に期限をまたいだ場合は`.partial`を保持して`original.jpg` rename・削除・成功状態へ進まないことを確認。
- 2026-08-08時点の変更はSDK有効／なしの両構成でCTest 5/5、当時のM3 simulated Release警告0、Foundation 13/13、Operator Shell 1/1に合格した履歴証拠である。Nikon SDKはcommit対象外の`.tools`へ再配置済み。
- M2Pの光学計算、rig-profile trust、三状態setup-assessment、WI-0022Cの合成画像測定からbounded correction proposalへの接続がsoftware-only合格。shift／rotation／scale／exposure／colorを決定的に測定し、profile provenance mismatch、draft、malformed、over-limitをfail closedする。最終リグ、承認済み閾値、実写A0品質は証明しない。
- M3PのNamed Pipe、durable simulated transaction、起動同意・readiness・操作ロック・失敗復旧を含むWPF shellは、2026-08-10のrequirements 2.6.0に対するfresh Release build 0 warning/0 error、Foundation 19/19、Operator Shell 15/15、`Test-M3Simulated.ps1` Passでsoftware-only合格した。明示Single/Dual、required aliases固定、no-auto-fallback、Single original一件、stitch `NotApplicable`、明示export、起動時操作gateを含む。
- 実機一台WPF経路は、同一transaction ID・alias・承認profile ID/version/SHA/expiry・Live View handoff intentをdurable保存して結果照会するsoftware boundary、検証済みcanonical originalの明示byte-identical export、`TransactionNotFound`時のsupport-required保持まで実装・契約試験済みである。現在の`hardware.v1` Live View handoff結果は撮影後の有限一frame probeを取得後に停止・SDK closeするもので、継続streamの「再開」を主張しない。
- requirements 2.7.0の継続Live View v2 sliceはQA revise後のfresh SDK-less／licensed-SDK-enabled Debug/Release CTest各7/7、.NET Release build警告0・エラー0、Foundation 20/20、Operator Shell 17/17、M3 boundary script、非破壊showcaseに合格した。production backendへ偽SDK/clockを注入し、512 KiB frame境界、session所有権、二重start、heartbeat timeout、単列backpressure、stop/close失敗後のcapture拒否を確認した。実Named Pipeは1 MiB未満／丁度を受理し、1 MiB超過をdispatch前に拒否する。canonical Base64と解析済みv2 rejection envelopeもnegative test済みである。camera command 0のsoftware-onlyで、実機合格ではない。
- C++ `hardware.v1`は、operator-session lease、directory作成から初期journal確定までを含むgap-free transaction reservation/mutex、exactly-one・alias/profile/expiry相関、Live View OFFのWPD前・open SDK shutter-session内再確認、SDK/WPD非重複、canonical originalを同一handleで再検証してwrite/delete禁止のままexact WPD delete・empty-afterまで保持する境界、fixed-local path、no retry、bounded pipe/journalをsoftware contract化した。WI-0022C追加後の2026-08-10 SDK-less Debug/Release全CTestは7/7で、`hardware_camera_agent_contracts`を含め0 failureだった（このsliceのlicensed SDK buildは未実行）。camera commandは送っておらず、実D810、actual JPEG、WPF hardware操作の合格証拠ではない。
- `WI-0010A`のsoftware contractを実装した。MAID `SdkCommandTrace`は唯一のMAID entry boundaryで全`CapStart`を計数・分類し、photographic-setting CapSet、storage-routing CapSet、Live View-control CapSet、capture、unknown/non-capture CapStart、session状態、counter整合性をfail closedする。`sdk-status`は列挙とread-only statusだけを公開する`NikonSdkStatusExecutor`へ分離した。CAM-Aの既定経路はSingleCamera identity-v3をstrict loadし、その合格後だけWPD enumeratorを生成してread-only列挙し、exactly-one D810とcurrent WPD digest一致を検証する。これらが全て合格した後だけSDK executorを生成・列挙・open/probeする二段階routingである。missing／malformed／digest不一致／WPD 0台／複数台ではSDK factory・enumerate・open/probeが全て0となる。明示`--camera-map`だけがlegacy/Dual v2へ入り、CAM-BへSingle identityを流用しない。process routing proofはidentity列挙と禁止対象のWPD/capture/deleteを分けてv5 summaryへ記録する。修正後の実D810 v5再実行は未検証であり、過去のv4 evidenceは変更していない。
- このidentity-v3 route修正はfocused Phase 0／CLI safety／Camera Agent contract、SDK-less Debug/Release全CTest各7/7、licensed-SDK-enabled Debug/Release全CTest各7/7、M3 Release警告0・エラー0、非破壊showcaseに合格した。licensed構成を含めcamera commandは0件である。
- 2026-08-08の再起動後dual binding readinessでD810 PnP、SDK inventory、WPD inventoryを各2台匿名確認した。SDK/WPDともbound 0・unbound 2で`READY_FOR_IDENTITY_BINDING`。inventoryの列挙順自動割当を廃止し、旧SDK mapは値を読まず可逆隔離した。SDK有無各CTest 5/5も合格し、撮影、Live View、設定変更、card操作は実行していない。
- readinessは選択stageの台数とSDK・WPD・PnP件数の完全一致を要求する。現在の二台接続はDualでbinding待ち、Singleではexit 1で拒否され、余分なD810を許可しない。
- WI-0010A完了後、licensed Release binaryの`verify-dual-identity`をread-only identity確認として厳密に1回実行したが、SDK inventoryで`identity_collision`となりfail closedした。WPD inventory開始、map変更、cross-transport binding、cardアクセス、capture、Live View、設定write、delete、format、0x9207、retryはない。追加のSDK header／document調査でも、二台のD810本体を恒久的に区別するdocumented SDK propertyまたは安全なSDK/WPD相関アンカーは見つからず、Dual identity strategyはBlockedのままである。一方、操作者報告では一台exactly-oneと`bind-single-identity-v3 --alias CAM-A`はSDK/WPD各1台、camera mutation 0でPassした。その直後の`sdk-status`はlegacy mapを参照してcamera open前にalias unavailableとなったため、このsoftware defectを上記identity-v3経路へ修正した。実機での再実行までは合格扱いにしない。
- 2026-08-08 17:31 JSTのCAM-A cross-transport bindingは後続の物理入替試験で無効化した。WPDは入替後の個体を未登録として区別した一方、SDKは旧CAM-Aへ誤一致した。原因はMAID source object IDを個体IDとしていた実装であり、当該SDK mapを値を読まず可逆隔離した。過去のSDK側CAM-A continuity主張も再検証対象である。
- SDK identityをdocumented MAID Source `Name`/`Interface`の境界付きlocal-only digest v2へ変更し、source object IDを除外、欠落・不正文字列・二台衝突をfail closedにした。SDK有無Release CTestは各5/5。17:49 JST、現在のCAM-B候補をSDK/WPDへ明示bindingし、各bound 1・unbound 0、Single `READY`を確認した。CAM-Aへ戻した際の別個体判定、再接続、port交換まではcheckpointであり完了扱いにしない。
- 18:14 JSTのCAM-A swap-back申告後もSDK/WPDはともにCAM-B、WPD firmwareは直前のCAM-Bと同じ`V1.11`だったため、本体交換は証拠化できずCAM-A登録を拒否した。WPD identityはPnP device IDから本体報告`WPD_DEVICE_SERIAL_NUMBER`のlocal-only digest v2へ強化し、現在のV1.11個体をCAM-Bへ再登録した。実serial/identityは出力していない。
- SDK/WPDを別々に登録する途中で本体が変わる余地を閉じるため、`bind-cross-transport-identity`を追加した。一つのcamera-control lease内でSDK sessionを完全closeしてからWPDを列挙し、両mapの競合を事前検証後にだけ双方を登録する。競合時の片側map未変更、台数0/複数拒否、引数guardを契約化し、SDK有無Release CTest各5/5と現在のCAM-Bでidempotent実機実行に合格。撮影・Live View・設定・card操作は0件。
- `verify-dual-identity`を追加し、SDK/WPDで台数2、CAM-A/B各1、unbound 0を同時に満たす場合だけ`Ready`とする匿名durable summaryを実装した。台数不一致、未登録、alias cardinality不一致を撮影前に拒否する。現在一台の実機`run-1786182987492-1`はSDK/WPD各1、CAM-B各1を記録し、想定どおり`camera_count_mismatch` / exit 5、map変更・撮影・Live View・設定・card操作0で停止した。
- `verify-dual-spools`を追加し、dual identity `Ready`後だけCAM-A→CAM-Bの順に二つのWPD cardをread-onlyで開き、全payload数が双方0の場合だけ撮影laneを`Ready`にする。片方でも非空なら両体を`spool_not_empty`で停止し、delete/format/vendor operation/retryは行わない。現在一台の`run-1786183481065-1`は`dual_identity_not_ready` / exit 5、`CardInspectionPerformed: false`、WPD session 0、撮影・削除0で停止した。SDK有無Release CTest各5/5。
- 実機`hybrid-capture-pair`の入口も共通dual identity検証へ接続した。Readyでない試行は匿名`dual-identity-verification-summary.json`を残し、card access・capture・retryなしでexit 5とする。SDK有効/無効Release buildとCTest各5/5に合格。現在一台のため、安全確認フラグを未確認のまま実機コマンドは実行していない。
- P3Aで3 CLIの入口をstrictなprovider config/proof loaderと`VerifyDualIdentitySoftwareContract`を共有するproduction preflightへ接続し、preflight Block判定をSDK/WPD transport生成より前へ移した。approved anonymous fake inventoryだけはpublic callerでsoftware-only `Ready`を確認できるが、Nikon production providerは未実装で`Vendor clarification required`である。default/legacy実CLIは`identity_strategy_unresolved`、明示opt-inもproduction inventory不在でBlockし、hardware Readyや実体同一性の受入れ証拠ではない。
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
- 2026-08-10のproduct owner指示をADR-0023として記録し、要件を2.6.0へ更新した。これは一台製品modeの実装開始判断であり、実WPF Camera Agent、実JPEG、canonical original export、実機one-shotの合格証拠ではない。

## Partial

- setting readback: native MAID command-traceとSingle identity-v3対応`sdk-status` process-routing software contractは実装・fresh test済み。identity-v3登録後の実D810 v5再実行、focus値の意味、FileType未広告の扱いは未検証／未確定。
- Live View: standaloneは合格だが、実撮影を含むContinuous Live View handoff 10回は未実施。
- identity: 旧SDK CAM-A復元証拠はephemeral source ID使用のため無効。SingleCamera CAM-AはWPD digest＋exactly-one current SDK/WPDのidentity-v3を実装し、操作者報告の一台登録はPassした。旧v2 mapへ自動fallbackせず、欠落・破損・digest不一致・0台／複数台をstatus open前に拒否する。二台接続時のSDK Name/Interface v2衝突、旧v2 mapの明示migration/invalidation、CAM-A/Bの恒久的区別はDual laneの`HG-0003B`としてBlockedであり、Single identity-v3をDualへ流用しない。
- setup/correction: parameter contractとWI-0022Cのsynthetic measurement seamは合格。proposalは測定値・fixture/profile provenanceを保持し、profile envelope外を拒否し、profileを変更しない。実写A0品質、承認済みDual profile、実機性能は未検証である。
- M3P: requirements 2.6.0のfresh software contractは合格したが、実D810、actual JPEG、実WPF画面操作の統合証拠ではない。旧Dual UI Automationは保持し、screen reader、keyboard/focus、Single実画面walkthroughは残る。
- SingleCamera製品mode: 実D810のCamera Agent撮影、actual JPEG、one-shot、10回p95、100件耐久は合格済み。実WPF操作、10回handoff、UI経由fixed-local export、物理異常系は未検証である。

## Deferred / Waiting

| 項目 | 理由 | 再開条件 |
|---|---|---|
| M1A one-shot、10/10 | 2026-08-26に完了 | one-shot 1/1、10/10、p95承認、Camera Agent経路100/100を実績として維持 |
| M1A fault、handoff | software recoveryは合格。物理USB切断とContinuous Live View handoff 10回は未実施 | 実機操作者の明示確認後に残試験を個別実施 |
| M1B二台試験 | 二台接続時のSDK identity collisionは、2026-08-20のADR-0025でsession-local operator bindingへ置換して解消した。恒久的な機体識別は作らず、割当はAgent process内のmemory-onlyで、Single identity-v3は流用しない | core（#9）・binding Agent protocol（#61）・確認UI（#62）はsoftware実装済み。実capture backend（#10）と実機受入が残り、DualCameraは`HardwarePending` |
| 実M2 | リグ・A0品質契約未承認 | `HG-0001/0002` |
| SingleCamera製品受入 | 実Camera Agent撮影1/10/100とHG-0009は完了。実WPF 100件とLive View handoffは未完了 | WPF end-to-end one-shot後、UI操作・export・状態表示を含む100件受入 |
| 配布 | native dependency再配布未承認 | `HG-0005` |

90 payloadを検出した旧カード状態は履歴として保持する。2026-08-26の実績は別の専用empty spoolで取得しており、旧カードへ削除・formatを行ったことを意味しない。今後もUSB切断、電源操作、保存障害は自動実行しない。

## 次の安全な順番

1. SingleCameraの残作業を、Continuous Live View handoff 10回、実WPF end-to-end、物理異常系へ限定する
2. DualCameraのproduction real capture backend（Issue #10）をsoftware contractで完成させる
3. 実D810二台でone-shot、10組、100組、fault matrixを順に実施する
4. `HG-0001/0002`承認後にA0合成品質へ進む

## Human gates

- `HG-0001`: A0品質・補正上限
- `HG-0002`: 最終リグ・光学条件
- `HG-0005`: Nikon SDK/OpenCV等の再配布

解消済み:

- `HG-0003B`: ADR-0025のsession-local operator bindingとして解消。software実装済み、二台実機受入は別途未完了
- `HG-0009`: 2026-08-26にSingleCamera実測p95 `14.643秒`を承認

## 証拠の読み方

- `Pass`: その項目の明示contractをfresh evidenceで満たす。
- `Partial`: 一部のlayerまたは環境だけが確認済み。
- `Unverified`: 必要な実行証拠がない。
- software-only、旧one-camera、simulatedの結果を実WPF Camera Agent、一台製品受入、二台実機、A0品質、MVP受入へ読み替えない。
