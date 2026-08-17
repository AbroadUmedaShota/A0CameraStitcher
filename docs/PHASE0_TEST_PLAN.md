# Phase 0 実機成立性検証計画

## 目的

完成アプリの前に、Windows 11 x64とNikon D810をUSB接続し、PC原本を安全に確定できる経路を判定する。attempted hybridのdatetime cutoffはRejectedである。`HG-0008`は2026-08-06に承認済みであり、single-slot spool経路を実装・実機評価する。

## 開始条件

- Phase 0A前: `HG-0003A`（D810一台、MSVC/CMake、対象PC・USB構成・実行許可）と`HG-0006`（SDK使用許諾の本人同意と内部評価）が解消済み。
- Phase 0B前: `HG-0003B`の解消が必要。2026-08-08にD810 PnP、SDK inventory、WPD inventoryを各2台確認した。17:31 JSTのCAM-A SDK bindingは、物理入替後もCAM-Aへ誤一致したため無効化した。原因のephemeral MAID source IDを排除しSource `Name`/`Interface` identity-v2へ修正、SDK有無各CTest 5/5とCAM-B一台checkpointは合格したが、二台接続時のSDK identity-v2は`identity_collision`となった。licensed SDK headers/docsとWPD相関設計のread-only診断ではdocumentedな本体固有propertyまたは安全なcross-transport anchorを確認できなかったため、identity strategyがsoftware-blockedである。CAM-A SDK v2再登録、抜線、再接続、port交換、二台同時readiness、実機Phase 0B撮影はhuman decisionまで開始しない。
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

標準WPD失敗は履歴として保持する。attempted hybrid [run-1785914842210-1](evidence/phase0/run-1785914842210-1/report.md)はbaseline timeout、SDK open/capture 0、`FailedPartial`、retry/delete 0である。read-only [run-1785917005306-1](evidence/phase0/run-1785917005306-1/report.md)は3/3 WPD session closeを記録しつつclock cutoffを支持しない。P0-A2は承認済みspool経路の実機one-shotから開始し、A3/A4はその後に進める。いずれの合格証拠も未取得である。

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

- 旧`bind-cross-transport-identity`／`bind-identity --transport`とidentity-v2 mapは匿名診断・明示的なinvalid化判断のためだけに残す。現行SDKの衝突するName/Interface digest、enumeration order、USB port、ephemeral Source IDを恒久bindingまたは同一実機相関の証拠へ読み替えない。
- `verify-dual-identity`、`verify-dual-spools`、`hybrid-capture-pair`は同じproduction preflightを使用する。provider configは`--dual-identity-provider`、CAM-A/B proofは`--dual-identity-proof`を2回指定する明示opt-inで、いずれもstrictなfixed-local file loaderを通す。未指定、legacy identity-v2 map、未実装のNikon production inventory providerからは`Ready`へ昇格せず、default/legacyは`Blocked/identity_strategy_unresolved`・exit 5を維持する。
- 将来の合格候補は、一台だけを接続した状態で操作者がCAM-A/Bごとに作成したlocal-only proof、documented provider ID/version、proof schema/version・有効期間・confirmation、二台inventoryのeach alias exactly once、unbound/duplicate/collision各0がすべて一致し、typed `DualIdentityReady`を返すsoftware contractに限定する。この契約試験はproviderの承認・接続または実機Readyを意味しない。
- typed Readyはproduction public callerへ接続済みだが、現時点のReady証拠はapproved anonymous provider/proof/exactly-two fake inventoryによるsoftware-only contract testに限定する。実CLIはproduction inventory projectionをraw identifier、serial、USB port、enumeration order、legacy mapから生成せず、Nikon providerは`Vendor clarification required`のままとする。したがって実CLIのpreflight Ready、hardware Ready、実体同一性、card/spool/capture受入れを証明したとは扱わない。Block時はcard inspection、WPD/SDK session、capture、Live View、設定write、delete、format、vendor operation、retryをすべて0のまま停止する。
- reparse path negativeは、Windowsがunprivileged symlink作成を許可する環境では公開`LoadDualIdentityBindingProof` seamで拒否を直接確認する。権限またはDeveloper Mode不足時は、同じ公開seamのrelative/UNC/device/non-fixed-local拒否と既存fixed-local path-chain契約をsoftware evidenceとし、reparse実体作成を未検証として残す。権限回避やproduction policy緩和は行わない。
- documented provider実装・proof作成手順・production caller接続が承認された後に限り、各aliasのLive View停止・SDK close後のhybrid transactionが同じ物理D810のシャッターとPC原本になることを一回ずつ確認する。
- 接続順変更3回、各カメラのUSBポート交換後も別名が維持されることを確認する。

### P0-B2: 順次二台transaction

アプリ側のsoftware-only準備として、Dual専用schema `a0.camera-agent.hardware-dual.v2`の4操作、durable pair store、予約済みpair開始、同一ID結果照会、.NET Reserved／terminal recovery、strict semantic preflightを実装済みである。fake backend限定ではCAM-A→CAM-Bを各最大一回、共有180秒deadline、自動retry 0で実行し、A失敗時B 0、B失敗時A原本保持、transaction ID別terminal journalの再起動照会まで合格した。これはproduction Dual Named Pipe／Agent host、実SDK・WPD・camera backend、製品composition／WPF実撮影の接続証拠ではない。既定経路は`PairDispatcherUnavailable`／`HardwarePending`であり、`HG-0003B`解消前に実機Readyへ昇格しない。

- `CAM-A`でWPD baseline/close、SDK one card capture/close、WPD recovery、PC保存を完了する。
- 次に`CAM-B`で同じhybrid処理を完了する。
- `hybrid-capture-pair`を`--dual-dedicated-spools-confirmed`を含む全安全確認付きで使用する。コマンド自身も共通dual identity検証を先頭で実行し、SDK/WPD各2台、CAM-A/B各1、unbound 0でなければ匿名証跡を残してcard access・capture前にexit 5とする。合格後はpair全体で一つの180秒deadline、operator-session-wide lease、CAM-A→CAM-Bの固定順序を維持する。
- CAM-Aのverified canonical PC original、exact-object delete、empty-afterが完了した場合だけCAM-Bを開始する。CAM-A失敗時はCAM-Bを開始しない。
- CAM-B失敗時はCAM-Aの確定済みPC原本を保持し、pairを`FailedPartial`として次pairを開始しない。
- 両方が確定した場合だけpairを`Complete`とする。順次撮影であり、実シャッター同期は保証しない。
- 10件を自動再試行なしで実行する。

合格: 10/10 transaction、CAM-A/B各10枚、誤ペア・消失・曖昧画像採用0件。

### P0-B3: 100件安定性

- 二台transactionを100件連続で実行する。
- transactionごとに両ファイルのサイズ、SHA-256、各状態・時刻を記録する。
- p50、p95、maxを集計するが、Phase 0の合否には使用しない。

合格:

- transaction 100/100
- CAM-A撮影・回収 100/100
- CAM-B撮影・回収 100/100
- 初回試行失敗、自動再試行、誤ペア、原画像消失、曖昧画像の自動採用、回復不能停止が各0件

### P0-B4: 二台異常系

通常100件とは別に次を実行する。

- CAM-A/Bそれぞれのactive中USB切断を各1回
- CAM-A/Bの物理的な電源断・電源再投入: **N/A / Skip**（2026-08-09 operator判断）
- `hybrid-interrupt-pair`でCAM-A保存・exact cleanup後、CAM-B開始前の専用gate readyを確認してPhase 0 processを終了する試験を1回。continue markerは作らない
- 接続順変更3回
- CAM-A/BのUSBポート交換を各1回

合格: 対象transactionを失敗確定し、取得済み原画像と曖昧画像を保持し、復旧後の新規transactionが成功する。

pair software fault contractは、CAM-A SDK close失敗でWPD recoveryとCAM-Bを開始しないこと、およびCAM-B WPD recovery open失敗でCAM-A原本を保持しCAM-B原本・cleanup・retryを生成しないことに合格済みである。これは実USB切断の代替証拠ではない。物理電源断は人判断により実機合否対象外である。

active中の実USB切断には`hybrid-fault-pair --alias CAM-A|CAM-B --scenario usb-disconnect`を使う。dual identity未Readyではcardを開かず、alias省略、operator gate省略、両専用spool確認省略もcamera open前に拒否する。選択bodyのSDK capture/full close後かつWPD recovery open前のready markerを確認してから指定異常を発生させる。CAM-AではCAM-B未開始、CAM-BではCAM-A verified original保持を必須とし、未確定bodyのdeleteとautomatic retryは0、新run IDを要求する。匿名`hybrid-pair-fault-summary.json`を証拠とする。`power-off` scenarioはsoftware safety contractとして残すが実機実行は任意である。

`report --run-id`はdurable event logにpair開始後のterminal stateがない場合、匿名`hybrid-pair-recovery-summary.json`を生成する。`CAM-A-active`、`after-CAM-A-before-CAM-B`、`CAM-B-active`を区別し、完了済み原本保持、automatic retry禁止、新規transaction必須を記録する。terminal failureも新規transactionを要求し、pair開始が重なる不整合証跡は`EvidenceInvalid`としてfail closedにする。reportはactive hardware evidence writerと競合しないよう、camera sessionを開かなくてもoperator-session camera-control leaseを保持する。software contractは合格済みだが、実プロセス終了と復旧後の新規実機transactionはP0-B4で別途実施する。

`hybrid-interrupt-pair`はdual identity、両専用empty spool、安全確認、operator-session leaseを要求する。CAM-A完了後の中断専用gateはprocess終了以外で正常復帰せず、誤ってcontinue markerを作った場合やtimeoutでもpairをterminal failureとして閉じてCAM-Bを開かない。実process終了時はterminal pair eventがない状態を意図的に残し、再起動後の`report --run-id`だけが匿名recovery summaryを生成する。

## M2P: software-only pre-gate

`WI-0021A`と`WI-0022B`の依存を満たした`WI-0022C`は、実機を使わないbounded software sliceとして完了した。rights-clearedな合成画像fixtureからshift、rotation、scale、exposure、colorを決定的に測定し、測定値とfixture/profile provenanceを保持するproposalへ接続する。承認済みprofile envelope内の一時補正だけを受理し、target／automatic-correction上限のboundary、over-limit、malformed、profile-mismatch、unapproved profile、入力不整合をfail closedする。profileの自動学習・更新はない。

最新のsoftware-only回帰はFoundation 22/22、DualCamera 18/18、Operator Shell 22/22、SDK-less／licensed Debug/Release全CTest各10/10、`Test-M3Simulated.ps1` Release/Debug、正式DualCamera WPF flowに合格した。このsoftware contractは最終リグ、承認済みA0閾値、実写品質、実機性能、identity strategyの解決を証明しない。identity strategyはBlocked、`HG-0003B`は未解消、実D810 v5 runと実機capture 1/10/100は未検証のままである。カメラ、WPD、カード、Live View、設定write、delete、format、`0x9207`、retryはこのsliceで実行していない。

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
