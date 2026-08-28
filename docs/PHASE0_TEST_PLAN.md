# Phase 0 実機成立性検証計画

## 目的

完成アプリの前に、Windows 11 x64とNikon D810をUSB接続し、PC原本を安全に確定できる経路を判定する。attempted hybridのdatetime cutoffはRejectedである。`HG-0008`は2026-08-06に承認済みであり、single-slot spool経路を実装・実機評価する。

## 開始条件

- Phase 0A前: `HG-0003A`（D810一台、MSVC/CMake、対象PC・USB構成・実行許可）と`HG-0006`（SDK使用許諾の本人同意と内部評価）が解消済み。
- Phase 0B前: 恒久SDK body identityを前提とした旧bindingは無効であり、`HG-0003B`はADR-0025のsession-local operator binding承認で解消済みである。ただし過去の二台検出・空カード・割当結果は現在のReady証拠へ流用しない。実行のたびに、現行software SHA、licensed SDK build、同一Agent session内のCAM-A/B明示割当、Live View停止とSDK完全終了、SDK/WPD各二台、両専用カードpayload 0件を再確認し、操作者が一回撮影を明示承認した場合だけPhase 0Bを開始する。前提不一致時は`HardwarePending`または`Blocked`で停止する。
- `HG-0001`と`HG-0002`はM2のA0品質・最終リグgateであり、通信専用チャートを使うPhase 0を止めない。
- Phase 0ツールはカメラ設定とfirmwareを変更しない。

## Phase 0共通撮影プロファイル

- JPEG Fine L、FX
- 固定露出、Auto ISO無効
- manual focus、固定white balance、VR無効
- single frame、bracketing無効
- 権利確認済みの固定静止チャート
- 実行前のカメラ設定値、firmware、USBポート・ハブ構成を記録
- 実SDK/WPDへ触れるCLIは同じinteractive Windows logon sessionで共有するnamed OS leaseを一件だけ取得し、別processが保持中ならcamera open前に失敗する。別user session／serviceからの実機操作はMVP運用外とする。
- 旧`capture-single`、`capture-pair`、`stability`はfake-onlyとし、実SDK/WPD撮影に使用しない。
- pairとhybridのtransaction全体に180秒watchdogを適用し、期限切れ後は成功状態、次のtransport、canonical rename、deleteへ進まない。`.partial`または期限前に確定済みのPC原本は保持し、安全なsession closeだけは期限後も試行できる。

## Phase 0A: 一台先行

ADR-0024以後、Phase 0AはCAM-A専用`SingleCamera`実機transport受入の先行証拠として使う。登録済みWPD serial digestとSDK/WPD双方のexactly-one current-session projectionをidentity-v3で要求し、衝突するSDK Name/Interfaceを永続identityにしない。既存証拠をidentity-v3、実WPF Camera Agent、canonical original明示export、一台製品受入へ読み替えない。

### P0-A1: SDKとD810列挙

- SDK版、OS、MSVC、CMakeを記録する。
- 物理的にD810一台だけを接続することは、二台の恒久的区別を証明しない。documentedな本体固有SDK propertyまたは安全なSDK/WPD相関方法がhuman gateで承認されるまで、実機の再登録・bindingは行わない。
- 取得可能なcapabilityとfirmwareを匿名化して記録する。
- Phase 0Aのidentity合格には、承認済みidentity strategyによる再列挙復元を要求する。旧identity-v2を一台状態で再開すること、物理cableの抜き差し、接続順変更、USB port交換だけでは二台の個体区別を証明しない。

合格: D810を安定して選択aliasとして識別でき、実識別子がcommit対象へ出ない。

2026-08-05のreadiness、SDK inventory、WPD inventoryはいずれもD810一台を列挙した。読み取り専用`run-1785903488159-1`はレリーズ`S`、静止画／動画セレクター`photo`、Live View `off`、prohibit mask `0`、SDK session close、設定変更なしを匿名記録し、firmwareはWPD標準propertyから`V1.14`を取得した。電源再投入後の[identity continuity summary](evidence/phase0/run-1785917466375-1/identity-continuity-summary.json)はWPD側CAM-A continuityの履歴として保持するが、SDK側は後にephemeral MAID source IDを使用していたと判明したため無効である。二台接続時のidentity-v2衝突とread-only SDK資料診断を受け、P0-A1のSDK identity continuityは再接続・port試験ではなく、まずdocumented identity strategyと明示migration/invalidationの決定が必要である。実識別子とmap hashはcommit対象へ含めない。

2026-08-07の設定read-only [run-1786040075194-1](evidence/phase0/run-1786040075194-1/report.md)は、SDKが返した値としてJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を匿名記録した。FileTypeはnot-advertisedだったが、CompressionLevelとImageSizeは取得できた。撮影設定write、capture、Live View開始、WPD、deleteは行わず、sessionを閉じた。MAID session確立時のcontrol-plane callback登録とModuleModeは既存`CapSet`を使い得るため、設定read-onlyは「撮影設定capabilityを書き換えない」という意味である。native command-trace自動試験とfocus値の意味確定が未完了のため、設定比較項目はPartialとする。

WI-0010A revise loop後のsoftware contractは実装済みである。MAID traceは唯一のMAID entry boundaryで全`CapStart`を計数・分類し、photographic-setting、storage-routing、Live View-controlの各`CapSet`、capture、unknown/non-capture CapStart、session状態、counter整合性をfail closedする。fake entryを通る共有boundary testで一回計数と分類を検証し、`sdk-status`のLive View API非公開もcompile-time negative testで固定した。CAM-Aの`sdk-status`は、strict SingleCamera identity-v3 file load→WPD enumerator生成／read-only列挙→exactly-one D810 model／登録digest一致→SDK executor生成／列挙／open-time exactly-one／read-only probeの順に固定する。production routing seamのfake counterはmissing／malformed時にWPDとSDKが各0、digest不一致／WPD 0台／複数台／非D810時にWPD列挙1回かつSDK factory・enumerate・probe 0を要求する。明示`--camera-map`だけが従来のlegacy/Dual v2 routeを選び、CAM-BへSingle identityを流用しない。identity列挙、SDK列挙・read-only probe、禁止対象WPD/capture/deleteはMAID proofと分離したprocess routing proofとして`phase0.sdk-status-summary.v5`へ保存する。過去evidenceとv4 summaryは書き換えない。操作者報告のexactly-oneとCAM-A identity-v3登録はPassしたが、修正後の実D810 v5再実行は未検証である。

### P0-A2: one-shot hybrid単体撮影・回収

- attempted hybridは`run-1785914842210-1`でbaseline timeoutとなり、SDK open/capture前に`FailedPartial`となった。device datetime cutoffは3/3 WPD session closeを記録した`run-1785917005306-1`によりRejectedである。
- 承認済み試験は、専用empty/cleared cardをsingle-slot spoolとして、撮影前にJPEG以外も含むcamera payload objectが0件であることを確認し、SDK exactly-one capture、WPD唯一のexact JPEG object recovery、PC `.partial`、JPEG・size検証、SHA-256、atomic `original.jpg`確定、再読込検証、今回objectだけのdelete、全payload 0件のempty-after確認を行う。
- existing cardのbulk delete/format、vendor operation、retryは禁止する。
- baseline、SDK capture、recoveryの各失敗、候補0件・複数件・遅延・曖昧画像、JPEG/size検証、download、persist、delete、empty-after確認の失敗は`FailedPartial`にする。削除はせず、PC原本が確定済みなら保持し、自動再試行しない。
- まずone-shotを1回だけ証明し、その合格後に10回連続を自動再試行なしに実行する。

現在判定: `HG-0008`は2026-08-06に承認済みでsoftware contractは実装済み。ただし[run-1786014841232-1](evidence/phase0/run-1786014841232-1/report.md)が撮影前に90 payload objectを検出し、SDK/shutter/delete/retry 0で安全停止した。read-only [run-1786015997366-1](evidence/phase0/run-1786015997366-1/report.md)と[run-1786017282044-1](evidence/phase0/run-1786017282044-1/report.md)も全payload 90件、WPD close 1/1、capture/delete/vendor operation 0件を確認した。同じ空spool阻害条件が3回連続したため、M1Aは専用empty cardへの交換または操作者によるbackup・手動clear待ちとしてブロック判定する。解消後に新規one-shotを開始する。合格条件はone-shot proof 1/1後、10/10でsingle-slot spoolのSDK one capture、WPD exact-one recovery、PC原本の再読込検証までの確定、single-object delete、全payload empty-after確認が成功すること。

標準WPD失敗は履歴として保持する。attempted hybrid [run-1785914842210-1](evidence/phase0/run-1785914842210-1/report.md)はbaseline timeout、SDK open/capture 0、`FailedPartial`、retry/delete 0である。read-only [run-1785917005306-1](evidence/phase0/run-1785917005306-1/report.md)は3/3 WPD session closeを記録しつつclock cutoffを支持しない。後続の2026-08-26専用empty spool試験ではCamera Agent one-shot 1/1と10/10を完了した。A3の物理異常系とA4のhandoff 10回は未完了である。

### P0-A3: 単体異常系

- idle中のUSB切断・再接続
- active transaction中のUSB切断
- アプリ再起動後の新規transaction
- active transaction中の物理的な電源断・電源再投入: **N/A / Skip**（2026-08-09 operator判断。実行不能のため合否項目から除外）

active transactionのUSB切断は`hybrid-fault-single`で実行する。empty-before確認、SDK one capture、SDK full close後かつWPD recovery open前に匿名operator gateを出す。操作者が指定異常を発生させてcontinue markerを作成した後、WPD open失敗を同一runの`FailedPartial`として確定する。この経路はPC original未確定のためdelete 0、自動retry 0とし、残ったcard objectを自動帰属・自動削除しない。復旧後にread-only `spool-status`を実行し、必要なら操作者が画像をbackupして手動clearした後、別run IDの新規one-shotだけを許可する。アプリ再起動は正常終了した別processから新規transactionを開始する試験とし、active processの強制終了やM3のdurable transactionをPhase 0へ拡張しない。`power-off` CLI contractは安全回帰試験として保持するが、実機実行は必須にしない。

合格: USB切断でoperator gate後のWPD `open_failed`、`FailedPartial`、PC original 0、delete 0、retry 0を匿名summaryへ記録する。復旧後はread-only spool確認を経て別run IDの新規transactionが成功する。アプリ再起動後も別processの新規transactionが成功する。物理電源断・再投入には合否を要求しない。

### P0-A4: 一台選択式Live Viewとhybrid handoff

- Phase 0Aでは物理D810を一台だけ接続し、二台目は接続しない。これによりSDKとWPDが同じ実機を指す条件を固定する。
- 選択alias一台だけをSDK Live Viewで開始し、プレビュー画像を10 frame取得する。プレビューは`artifacts`の診断用途に限り、原画像・合成入力・transaction JPEG候補にしない。
- 製品UI受入では、開始後に明示停止までframeを継続取得するversioned long-lived session、heartbeat、bounded lifetime、backpressureを検証する。`hardware.v2` software contractはstart/frame/heartbeat/stop/close、20秒idle、600秒max、単列request、memory-only verified JPEG、stop-before-capture、成功後だけrestartを実装済み。`hardware.v1`の1..30 frame有限probeを継続Live View合格へ読み替えず、実D810の10回handoffは別途要求する。
- `live-view --duration-seconds 300`で5分間継続し、有効JPEG frame数、停止、SDK closeを匿名summaryへ記録する。
- Live View停止、SDK session close、WPD baseline/full close、SDK one card capture/full close、WPD recovery（capture commandなし）、PC JPEG保存、SDK Live View再開を一連のhandoffとして実行する。
- 自動handoffを1回以上実行し、SDK/WPDが重複せず各closeが次のopen前に完了したtraceを確認する。
- WPD recovery timeout、Live View再開hang、SDK close timeoutは失敗として記録し、自動再試行やプレビュー画像の流用を行わない。
- `handoff-summary.json`へ要求数、試行数、完了数、失敗数、最後のhandoff状態・error category、`Complete`または`FailedPartial`を保存する。実識別子とpreview frameは含めない。

Standalone Live Viewは実機確認済みである。[run-1785917554163-1](evidence/phase0/run-1785917554163-1/report.md)は5分04秒で2,424 frameを取得し、停止、SDK session close、preview非保存を確認した。続く別プロセスの[run-1785917887961-1](evidence/phase0/run-1785917887961-1/report.md)も1 frame取得、停止、close、preview非保存に成功し、最終`run-1785917904556-1`はLive View `off`とSDK session closeを確認した。ただしこれらは承認済みspool handoffの合格証拠ではない。10回連続handoffは未実施のため、P0-A4全体はPartial/Hardware Pendingとする。

合格: 10回連続で、SDK Live View停止・close後だけhybrid transactionを開始し、PC JPEG保存後に選択中の一台Live Viewを再開できる。失敗時はtransactionと診断を保持し、新規操作でのみ再開する。

### P0-A5: SingleCamera製品characterizationと100件受入

- one-shot合格後、CAM-A、承認済み30日profile、empty dedicated spool、no-retryで10 transactionを実行し、capture開始からbyte-identical `7360×4912`製品JPEG確定までのp50/p95/maxを記録する。
- 10回はcharacterizationであり、`HG-0009`でproduct ownerが実測p95を承認するまで性能Passにしない。
- 承認後、同じ契約で100件連続の初回成功を要求する。失敗、original損失、曖昧採用、cleanup不整合、自動retryを0件とする。

実施結果（2026-08-26）: Camera Agent実機経路でone-shot 1/1、10/10 characterization、p50 `14.036秒`、p95/max `14.643秒`を確認し、Product Ownerがp95を承認した。その後100/100を全件初回成功で完了し、p50 `14.204秒`、p95 `14.430秒`、max `14.692秒`だった。計111原画像のJPEG寸法・size・SHA-256再検証に合格し、原画像消失、誤削除、曖昧採用、自動retry、復旧不能停止は各0件だった。実WPF UI操作、Live View handoff 10回、物理USB切断・保存先障害はこの結果に含まない。詳細は[SingleCamera実機結果](SINGLE_CAMERA_HARDWARE_RESULTS_2026-08-26.md)を参照する。

## SDK単独PC転送経路の不成立判定（判定済み）

次のいずれかでSDK経路を停止する。

1. D810を列挙できない。
2. 撮影命令を実行できない。
3. `SaveMedia=SDRAM`または`Card + SDRAM`でPC転送用Itemを一意に検出または転送できない。
4. SDK adapterと公式sampleの双方でPC転送用Itemが生成されない。
5. 文書化された再接続手順で復帰できない。

上記のSDK SDRAM不成立と標準WPD失敗を踏まえ、product ownerはone-shot hybridを承認した。SDKは一回のcard captureに、WPDはbaseline/recoveryに限定し、sessionを重複させない。

## Phase 0B: 二台順次撮影

### P0-B1: 二台識別

- binding開始前のSDK台数確認には`A0CameraStitcher.DualCameraAgent --read-only-sdk-probe`を使う。このprobeは恒久identityや候補tokenを生成せず、D810 source数だけを読み取り、SDK process claim・source・moduleをすべて終了してから匿名結果を返す。D810が二台、`cleanupState=ended`、`terminalState=Pass`のすべてを満たす場合だけ次へ進む。
- 同型D810二台ではMAID Name/Interface由来のgeneric inventoryが`identity_collision`で安全停止し得るため、その結果をDualの台数確認や個体対応付けへ流用しない。generic inventoryの衝突防止自体は維持する。
- SDK-only probeの完全終了を確認した後に、`A0CameraStitcher.DualCameraAgent --read-only-wpd-probe --wpd-camera-map PATH`を別processで一回だけ実行し、WPDのD810二台、既存CAM-A/B alias map各一台、両専用カードpayload 0件、各WPD session close、topology不変を確認する。このWPD-only gateはSDK、capture、delete、camera settings、vendor operation、自動retryを行わない。SDK moduleを保持したままWPDを開く`--read-only-coexistence-probe`は、SDK/WPD非同時open要件の受入証拠にしない。
- ADR-0025に従い、恒久的なSDK識別子ではなく、同一Agent session内で操作者が二台を`CAM-A`と`CAM-B`へ明示割当する。候補数が二台以外、二重割当、割当漏れは`HardwarePending`として停止する。
- Live Viewは候補一台ずつ表示し、候補切替時と割当完了時にLive View停止とSDK session完全終了を確認する。確認できなければbindingを無効化し、撮影へ進まない。
- binding完了後は、撮影中にSDK候補を再列挙しない。Agent再起動、USB再接続、台数またはtopology変更、SDK manager再生成、SDK errorではbindingを無効化し、操作者へ再割当を要求する。
- 実撮影の直前に、SDKとWPDでD810が各二台だけであること、CAM-A/Bが各一台であること、各専用カードのpayloadが0件であることを読み取り専用で確認する。SDKとWPDは同時に開かない。
- カメラ設定、firmware、カードformat、一括削除は変更しない。匿名alias以外の実機識別情報を証跡へ残さない。

### P0-B2: 順次二台transaction

Dual専用schema `a0.camera-agent.hardware-dual.v2`は、capabilities、予約、通常開始、CaptureRecoveryOnly開始、結果照会、取消の六操作を提供する。実SDK/WPD backendはbinding modeのproduction Agent hostへ接続済みである。通常の`start-reserved-pair`は従来どおり承認済みrig profileを必須とする。`start-reserved-capture-recovery-only`は、明示承認されたtransport検証専用であり、rig profileを受け付けず、合成・再合成・合成画像exportを行わない。WPFからのCaptureRecoveryOnly操作は別途未検証であり、このPhase 0ではnative Agent入口だけを使用する。

#### 一回撮影

操作者の明示承認後、最大一pairだけ実行する。

1. current Ready binding、承認済み読み取り専用capture profile、Live View停止とSDK完全終了、両専用カードpayload 0件、`captureRecoveryOnlyApproved=true`を確認する。
2. `CAM-A`をSDKで一回だけ撮影し、SDKを完全終了する。WPDで今回のJPEG一件だけを回収し、PCへ`.partial`保存する。
3. JPEG形式、7360x4912、file size、SHA-256を検証し、atomic renameしたcanonical originalを再読込した後だけ、直前に取得したWPD object一件を削除する。カードpayload 0件への復帰を確認する。
4. CAM-Aが完全に成功した場合だけ、`CAM-B`へ同じ手順を一回実行する。CAM-A失敗時はCAM-Bを開始しない。CAM-B失敗時はCAM-Aのcanonical originalを保持する。
5. 自動retry、別alias探索、曖昧画像採用、未検証object削除を行わない。pair全体は共有180秒watchdog内で実行する。
6. 両原画像が確定した場合も合成せず、結果へ`capturePurpose=CaptureRecoveryOnly`、`stitchOutcome=Pending`、`a0QualityApproval=Unapproved`を記録する。実シャッター同期も保証しない。

合格: 初回試行の一pairでCAM-A/B原画像各一枚が確定・再読込でき、両カードpayloadが0件へ戻り、誤pair、消失、曖昧採用、retryが各0件である。失敗時は発生位置と原因に対応する`Failed`、`FailedPartial`、`WatchdogExpired`、または`HardwarePending`として停止し、成功やretryとして扱わず、同じtransactionや代替pairで上書きしない。

#### 10件性能計測

- 一回撮影合格後、同じ条件・同じcurrent bindingでCAM-A→CAM-Bを10pair実行する。SDK候補を再列挙しない。
- 各pairの開始・完了時刻、結果、CAM-A/Bのfile sizeとSHA-256、error、retry countを保存する。p50、nearest-rank p95、maxを計算する。
- 10/10を初回試行で完了し、誤pair、原画像消失、曖昧採用、誤削除、自動retryが各0件であることを合格条件とする。一件でも失敗した時点で停止し、失敗回を再実行で置き換えない。
- 実測p95は製品責任者の明示承認対象として記録する。承認前にP0-B3を開始しない。p95承認はtransport耐久試験の継続許可であり、A0品質、合成品質、release、実シャッター同期の承認ではない。

### P0-B3: 100件安定性

- 10件の実測p95を製品責任者が明示承認した後だけ、CaptureRecoveryOnlyを100pair連続で実行する。
- transactionごとに両original、file size、SHA-256、各状態・時刻、error、retry count、匿名診断情報を保持する。合成は行わず、全結果を`StitchOutcome=Pending`、`A0QualityApproval=Unapproved`とする。
- 一件でも失敗した時点で停止し、そのrunを`Fail`として確定する。再試行で100/100へ見せない。

合格:

- transaction 100/100
- CAM-A撮影・回収 100/100
- CAM-B撮影・回収 100/100
- 初回試行失敗、自動再試行、誤ペア、原画像消失、誤削除、曖昧画像の自動採用、回復不能停止が各0件

### P0-B4: 二台異常系

P0-B3完了後、通常runと分けて、可能な範囲のsoftware timeout、SDK error、Agent再起動、CAM-A完了後かつCAM-B開始前の中断、binding無効化と再binding要求を確認する。取得済みoriginalを保持し、未検証のcamera objectを削除せず、自動retryせず、同じtransactionを再開せず、新しいbindingとtransactionだけで復旧することを合格条件とする。

物理USB切断、再接続、電源、カード操作は、各試験の直前に操作者が明示承認した場合だけ実施する。承認がない項目は`NotRun`または`Blocked`として残し、software fault testで代用しない。異常時は発生位置と原因に対応する`Failed`、`FailedPartial`、`WatchdogExpired`、または`HardwarePending`として停止し、成功やretryとして扱わず、失敗を成功runで上書きしない。

### Phase 0B 証跡

各stageは別runとして、匿名化した`report.md`、`summary.json`、`transaction-events.jsonl`、テスト結果一覧、p50/p95/max、両originalのSHA-256、実施日時、CAM-A/B alias、`Pass`／`Partial`／`Fail`／`Blocked`を保存する。原画像そのもの、camera serial、SDK配布物、顧客画像はcommitしない。最終判定は`GO-SDK-WPD-SEQUENTIAL`、`REVISE`、`STOP`のいずれかとし、CaptureRecoveryOnlyの成功だけでA0画質、継ぎ目、色差、approved rig、releaseを合格扱いしない。

## M2P: software-only pre-gate

`WI-0021A`と`WI-0022B`の依存を満たした`WI-0022C`は、実機を使わないbounded software sliceとして完了した。rights-clearedな合成画像fixtureからshift、rotation、scale、exposure、colorを決定的に測定し、測定値とfixture/profile provenanceを保持するproposalへ接続する。承認済みprofile envelope内の一時補正だけを受理し、target／automatic-correction上限のboundary、over-limit、malformed、profile-mismatch、unapproved profile、入力不整合をfail closedする。profileの自動学習・更新はない。

software-only回帰はFoundation 22/22、DualCamera 18/18、Operator Shell 22/22、SDK-less／licensed Debug/Release全CTest各10/10、`Test-M3Simulated.ps1` Release/Debug、正式DualCamera WPF flowに合格した。このslice自体はカメラ、WPD、カード、Live View、設定write、delete、format、`0x9207`、retryを実行していない。`HG-0003B`は後にADR-0025のsession-local bindingで解消され、SingleCamera実機1/10/100は2026-08-26に別証跡で合格したが、Dual実機、最終リグ、A0閾値、実写品質は未検証である。

## P0判定

1. `GO-HYBRID-SEQUENTIAL`: Phase 0A/Bの全条件とLive View handoff条件を満たした。
2. `REVISE-WPD`: SDK不成立証拠をproduct ownerが確認し、WPD一台試験への切替を承認した（2026-08-04解消済み）。
3. `STOP`: USBのみでは必要な運用を満たせないとproduct ownerが判断した。

この判定は匿名化レポートを添えてproduct ownerが承認し、ADRへ反映する。

`REVISE-WPD`は2026-08-04の履歴判断である。attempted hybridはRejectedであり、2026-08-06に承認されたsingle-slot spoolがP0-A2の一回証明、10/10、A3、A4へ進む経路である。

## コミット禁止データ

- 実カメラの完全なシリアル・SDK識別子
- Nikon SDKアーカイブ、DLL、LIB、ヘッダー、仕様資料
- 顧客原稿、個人情報、機密画像
- 実写JPEG/NEFとrawイベントログ

権利確認済み人工チャートだけを `samples/public` の許可範囲へ追加できる。
