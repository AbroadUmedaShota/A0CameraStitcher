# 意思決定記録

## ADR-0001: USBのみの撮影トランザクション

- 状態: Accepted
- 決定: ハードウェア同期を追加せず、`DualCamera`では静止平面原稿を二台で順次撮影する。`SingleCamera`追加はADR-0023で扱う。
- 影響: 実シャッター時刻差は保証せず、transaction整合性と合成結果で判定する。

## ADR-0002: Nikon Camera Remote SDKの排他的順次制御

- 状態: Accepted for Phase 0
- 決定: D810用Camera Remote SDKを第一候補とし、同時に一台だけセッションを開く。
- 理由: 正式機材がD810で、対象が静止原稿のため、複数台同時制御を前提にしない順次方式を採用できる。
- 影響: modeにかかわらず同時に一台だけを制御する。`SingleCamera`は選択alias一台で完了し、`DualCamera`は`CAM-A`の撮影・回収・close後に`CAM-B`へ進む。SDK不成立時のWPD調査は別承認を必要とする。

## ADR-0003: JPEG Fine Lから開始

- 状態: Accepted
- 決定: Phase 0とMVPの初期入力をFX JPEG Fine Lとする。
- 影響: NEF、16-bit、TIFFはMVP後とする。

## ADR-0004: 固定平面A0原稿を第一対象にする

- 状態: Accepted
- 決定: 動体・遠景より先に、固定した平面A0級原稿を対象とする。
- 影響: MVPの投影はPlanar Homography。円筒・球面投影は対象外。

## ADR-0005: D750からD810へ正式変更

- 状態: Accepted; camera cardinality amended by ADR-0023
- 決定: 正式製品のcamera modelをNikon D810へ変更する。当初の二台固定cardinalityはADR-0023により明示的一台／二台modeへ拡張する。
- 根拠: 2026-08-03のproduct owner判断。
- 影響: 旧D750前提は参考履歴のみ。D810最大7360×4912をM2光学計算へ使用する。

## ADR-0006: 単一進行transactionと自動再試行禁止

- 状態: Accepted
- 決定: 未完了transactionは一件だけとし、各カメラの唯一の新規JPEGだけを採用する。曖昧・timeout・部分失敗は`FailedPartial`で確定する。
- 影響: 同一transactionを再開せず、操作者は新規transactionを開始する。取得済み画像は削除しない。

## ADR-0007: SDK内部評価と製品再配布を分離

- 状態: Accepted for workflow
- 決定: SDK使用許諾の本人同意後に内部評価を行い、installerや製品への同梱はリリース前に別途承認する。
- 影響: SDK配布物と資料はリポジトリへ含めない。

## ADR-0008: SDK USB transportを配置元から明示ロード

- 状態: Accepted for Phase 0
- 決定: `Type0014.md3`より先に、同じlicensed SDK x64 directoryの`NkdPTP.dll`を絶対pathでloadする。SDK binaryはbuild directoryへcopyしない。
- 根拠: 公式sampleはSDK binary directoryをcurrent directoryにするとD810を列挙したが、Phase 0 CLIはrepository rootから起動すると0台だった。同一moduleを使いcurrent directoryだけを変更するとCLIも1台を列挙し、明示load後はrepository rootから成功した。
- 影響: カメラ列挙はprocess current directoryに依存しない。CMakeは`NkdPTP.dll`、`NkRoyalmile.dll`、`dnssd.dll`を含む完全なignored SDK directoryだけをadapter有効条件とする。

## ADR-0009: SDK Source ID由来のcamera mappingを一台構成で採用する

- 状態: Superseded; ephemeral Source IDを使う結論は無効
- 決定: SDKのraw Source ID自体は表示・commitせず、hash化したlocal identityを`CAM-A/B` mapping候補にする。CLIは撮影時に列挙順ではなくこのmappingを必ず解決する。
- 実機証拠: [run-1785917466375-1](evidence/phase0/run-1785917466375-1/identity-continuity-summary.json)で、記録済み電源再投入arrivalより前から存在するSDK/WPD local identity mapが再列挙後も変更されず、両transportが一台のD810を`CAM-A`へ復元した。実識別子とmap hashはcommit対象に含めない。
- 制約: Nikon MAID資料はSource child IDの再接続・USB port変更後の永続性を保証していない。一台構成の電源再投入は合格したが、二台の接続順変更とUSB port変更は未証明である。
- 影響: `WI-0011`は一台構成で完了とし、`WI-0014`で接続順変更3回と両cameraのport交換を実測する。維持できなければ二台撮影へ進まずidentity方式を再設計する。

2026-08-08のbody swap証拠により、MAID source object IDは物理D810のstable identityではないと判明した。現行契約はdocumented Source `Name`/`Interface`とWPD device serialから作るlocal-only identity-v2であり、CAM-Bはcheckpoint、CAM-Aと二台復元性は未検証である。過去のPassをidentity-v2合格へ読み替えない。

## ADR-0010: Phase 0 transportをWPD/PTPへ変更

- 状態: Superseded by ADR-0017、ADR-0019、ADR-0020
- 決定: 2026-08-04、product ownerが`REVISE-WPD`を明示承認した。以後のPhase 0撮影・JPEG回収はWindows Portable Device APIを使用する。
- 根拠: Nikon SDK adapterと公式sampleの双方で、D810のCaptureとカメラ側保存は成立したが、`SaveMedia=SDRAM`および`Card + SDRAM`でPC転送用Itemが生成されなかった。WPDは同じD810を列挙し、静止画撮影コマンド、JPEG Object差分検出、PC取得に成功した。
- 証拠: `run-1785826415773-1`は1 transactionを`Complete`で終了し、7360×4912、18,107,696 bytes、SHA-256 `09292cecaa9d4f1e9f92dc1367d681070c95510653a0645d93585411ef0ea3e5`のJPEGを保存した。
- 影響: カメラ側画像は削除しない。WPD用alias台帳をSDK用と分離する。二台・100回・異常系が完了するまで最終transport GOとはしない。

## ADR-0011: 一台選択式SDK Live Viewとhybrid transactionの排他handoff

- 状態: Provisional for Phase 0A
- 決定: MVPに一台選択式のSDK Live Viewを追加する。二台同時表示とプレビューframeの原画像・合成入力への使用は対象外とする。Live ViewのstopとSDK session close後だけhybrid transactionを開始し、WPD baseline/full close、SDK one card capture/full close、WPD recovery成功後に選択中一台のLive Viewを再開する。Phase 0Aのhandoffは物理D810が一台だけ接続された場合に限定する。
- 根拠: 固定リグで原稿位置、重複、ピント、照明を確認する必要がある一方、SDKとWPDの同時カメラ占有は避ける必要がある。
- 実機証拠: `run-1785834861034-1`で10 frame取得を確認した。手動handoffはWPD撮影`run-1785834883573-1`と撮影後Live View`run-1785834891891-1`で成立し、`run-1785834999815-1`では自動handoff 1/1に成功した。`run-1785835030476-1`は最初の5回成功後にWPD `image_event_timeout`とLive View再開hangが発生したため、10回連続の合格証拠ではない。
- 影響: Phase 0AへP0-A4を追加する。既存のWPD handoff証拠はhistorical/Partialであり、hybrid handoffの合格証拠ではない。失敗frameや失敗transactionを自動再利用しない。Phase 0Bでは一台ずつ接続してSDK/WPD identityを同じaliasへ登録するcross-transport bindingを先に実装し、完了までは二台接続時のhandoffを拒否する。

## ADR-0012: WPD撮影targetを一意なfunctional objectへ固定

- 状態: Accepted for Phase 0A implementation
- 決定: WPD session開始時に`WPD_FUNCTIONAL_CATEGORY_STILL_IMAGE_CAPTURE`のfunctional object IDを取得し、一件だけの場合に`WPD_PROPERTY_COMMON_COMMAND_TARGET`へ設定する。0件、複数件、不正型は推測せず`open_failed`とする。
- 根拠: 2026-08-05の`run-1785893437110-1`はSDK close後のWPD撮影で`capture_command_failed`となった。実装をMicrosoft WPDのcommand契約と照合したところ、必須targetが未指定だった。
- 影響: `SendCommand`自体のHRESULT、戻り値のcommon HRESULT、任意driver error codeを、実識別子を含めない16進診断として保持する。修正の実機成立性は新規WPD単体transactionで確認し、成功前にhandoff連続試験へ進まない。

## ADR-0013: WPD command結果不確定時は候補を隔離して失敗確定

- 状態: Accepted for Phase 0A safety
- 決定: `SendCommand`が`S_OK`でもcommon HRESULTが失敗、欠落、読取不能の場合は、撮影commandを再送せずpost-baseline観測を一度だけ行う。得られた全候補は`artifacts/phase0/quarantine`へbytes・SHA-256付きで保持するが、`original.jpg`へ昇格せずtransactionを`FailedPartial`で確定する。
- 根拠: `S_OK`はdriverがcommand結果を返したことだけを示し、common HRESULTの`E_FAIL`を撮影成功へ読み替えられない。一方、シャッターが物理動作した場合の新規objectを観測前に捨てるとPC側診断証拠を失う。
- 影響: 候補が一件でも`Complete`、`Paired`、CAM-B、Live View resumeへ進まない。0件、複数件、観測・download失敗も分類して保持し、同一transactionの再試行・候補再利用を行わない。

## ADR-0014: 撮影前のSDK状態probeは読み取り専用とする

- 状態: Accepted for Phase 0A diagnostics
- 決定: `sdk-status --alias CAM-A`はSDK source sessionでcapabilityを`Get`するだけとし、Live View開始・停止、撮影、SaveMedia、露出・WB等のカメラ設定を変更しない。終了時にSDK sessionを閉じ、匿名summaryだけを保存する。
- 根拠: WPD撮影再試験前にLive ViewがOFFで禁止要因がないことをPC側から確認し、物理状態の推測を減らす必要がある。
- 実機証拠: Enum capability読取を追加した`run-1785903488159-1`でLive View `off`、セレクター`photo`、prohibit mask `0`、レリーズ`S`、session closeを確認した。firmwareは別の読み取り専用WPD標準propertyで`V1.14`を取得した。
- 影響: セレクターの人手確認待ちは解消した。status probe自体をP0-A2撮影成功やP0-A4 Live View成功の証拠には数えない。

## ADR-0015: WPD target互換性を撮影前に検証しMTP responseを判定証拠にする

- 状態: Accepted for Phase 0A diagnostics
- 決定: `GetCommandOptions(WPD_COMMAND_STILL_IMAGE_CAPTURE_INITIATE)`を読み取り、still-image functional objectが一件で、`WPD_OPTION_VALID_OBJECT_IDS`がある場合はその交差も一件のときだけtargetを採用する。query失敗、0件、複数件、不正値は`SendCommand`前に停止する。common HRESULTだけで原因が特定できない場合はETWを一transactionに限定して採取し、raw traceはgitignored、匿名summaryだけをcommit可能とする。
- 実機証拠: 電源再投入後の`run-1785908501354-1`は`S / photo / Live View off / prohibit 0 / session closed`を確認した。ETW付き[run-1785908732670-1](evidence/phase0/run-1785908732670-1/report.md)では、MTP `InitiateCapture`（`0x100E`、parameter `0,0`）へ4453.125ms後`0x2002 GeneralError`を受信した。`SendCommand S_OK`、common `E_FAIL`、standard ObjectAdded 0件、新規JPEG 0件、`FailedPartial`、自動再試行なしである。
- 影響: target指定、SDK session競合、Live View状態、静止画セレクターと一過性の電源状態は標準WPD失敗の説明から除外できる。この時点の「HG-0007再判断待ち」と混成経路の禁止はADR-0017で置換済みである。標準WPD capture command自体は現行hybrid経路で送らない。

## ADR-0016: Vendor opcode広告はquery-onlyで確認し実行を別判断にする

- 状態: Accepted for Phase 0A diagnostics
- 決定: Microsoft公式`WPD_COMMAND_MTP_EXT_GET_SUPPORTED_VENDOR_OPCODES`だけを独立status probeで送る。個別一覧、vendor extension文字列、実識別子は保存せず、件数と`0x9207`広告有無だけを匿名化する。撮影commandとvendor operationは送らない。既定read-only accessが拒否された場合も自動fallbackせず、明示的read/write query-only runを別証拠にする。
- 実機証拠: 電源再投入後のquery-only [run-1785908518274-1](evidence/phase0/run-1785908518274-1/report.md)は34件と`0x9207`広告有無を匿名記録した。capture command・vendor operationは未送信である。
- 影響: `0x9207`が広告されることだけを確認でき、意味、parameter、event、撮影・回収成立性、利用許可は未確認である。標準`0x100E`の広告有無もvendor APIからは推定しない。ADR-0017でhybrid経路は承認されたが、vendor-op評価・実装・実行は引き続き対象外である。

## ADR-0017: 標準WPD不成立後のone-shot hybrid評価

- 状態: Superseded by ADR-0019
- 決定: product ownerは、`WPD baseline + full close → Nikon SDK exactly-one capture to camera card + full close → WPD reopen, no capture command, recover exactly one new JPEG`をPhase 0の承認経路とした。
- 根拠: 電源再投入後も[run-1785908732670-1](evidence/phase0/run-1785908732670-1/report.md)で標準`0x100E`は`0x2002`、ObjectAdded/JPEG 0件、`FailedPartial`だった。query-only [run-1785908518274-1](evidence/phase0/run-1785908518274-1/report.md)の`0x9207`広告は、operationの意味・実行許可・成功の証拠ではない。
- 影響: vendor operationを実装・送信せず、SDK/WPD sessionを重複させない。hardware evidenceによりdatetime相関契約はADR-0019でRejectedとなり、captureは承認済みADR-0020のsingle-slot spool経路だけで評価する。

## ADR-0018: PC原本を唯一の正本とし、camera cardは一過性の転送元にする

- 状態: Accepted
- 決定: WPD recoveryで取得したJPEGはPCの`.partial`、JPEG検証、SHA-256、atomic renameを経て`original.jpg`へ確定する。このPC原本だけを製品上保持する。camera cardの永続保持は要件にしない。
- 根拠: card captureとPC回収の責務を分離し、card上の状態に製品データ保持を依存させないため。削除は回収失敗時の診断可能性も下げる。
- 影響: `HG-0008`承認後のsingle-slot spoolだけは、PC確定後にjust-recovered objectを削除できる。existing cardのbulk delete/formatは禁止する。PC確定前の候補、0件・複数件・遅延・曖昧画像は正本にせず、診断として保持して新規transactionで扱う。

## ADR-0019: WPD device clock cutoffによるhybrid画像帰属を撤回する

- 状態: Rejected by hardware evidence
- 根拠: [run-1785914842210-1](evidence/phase0/run-1785914842210-1/report.md)はWPD baselineが10.385秒で`baseline_timeout`となり、SDK open/capture前に0/1件の`FailedPartial`で終了した。read-only [run-1785917005306-1](evidence/phase0/run-1785917005306-1/report.md)はWPD full close/reopen 3 sample（1500ms）をsession close 3/3で完了し、datetime 3/3 availableだったが、advance 0/equal 2、JPEG date 240/240、latest object date > device time 3/3だった。
- 影響: device datetimeとobject dateのcutoffを製品帰属契約に使わない。承認済みADR-0020のsingle-slot spool経路を除き、captureをfail-closedにする。

## ADR-0020: dedicated empty/cleared cardをsingle-slot transient spoolとして使用する

- 状態: Accepted — `HG-0008` approved 2026-08-06
- 決定: 専用empty/cleared cardだけをsingle-slot spoolとして使う。SDK exactly-one capture後にWPDで唯一のexact objectを回収し、PC `.partial`、JPEG・size検証、SHA-256、atomic `original.jpg`確定、再読込検証を完了してからそのobjectだけを削除し、spoolが空であることを確認する。
- 失敗時: baseline、capture、recovery、候補0件・複数件・遅延・曖昧画像、JPEG/size検証、download、persist、delete、empty-after確認のいずれかに失敗した場合は削除しない。PC原本が確定済みなら保持し、transactionを`FailedPartial`で終了する。同一transactionをretryまたは再開しない。
- 制約: existing cardのbulk delete/format、vendor operation（`0x9207`を含む）は禁止する。実機one-shot、10/10、fault、handoffの合格証拠はまだない。

## ADR-0021: 設置は自動補正可能範囲までとし固定profile＋上限付き補正を使う

- 状態: Accepted for design and software pre-gate; optical thresholds pending
- 決定: 操作者へpixel単位の完全な手動位置合わせを要求しない。承認済みrig profileの固定レンズ補正・planar warpを基準とし、撮影ごとの位置・回転・倍率・露出・色の差が承認済み上限内の場合だけ一時的に自動補正する。判定は`ready`、`ready-auto-correction`、`physical-adjustment-required`の三状態とする。
- 安全境界: 撮影ごとに自由なhomographyを再推定せず、補正結果でrig profileを自動学習・更新しない。判定器はprofileのstatus、対応schema版、provenance、校正日時、有効期限を構造化入力から検証する。全画角不足、重複不足、設定不整合、上限超過、draft・不整合・期限切れprofileは成功扱いにせず、物理調整または再キャリブレーションを案内する。
- 検証順: 実機不要の純粋判定・schema・synthetic contractを先行し、一台ではレンズ校正案内・設定読取・provenance記録を検証する。左右固定transform、seam、実写A0品質は二台と承認済みchartが揃うまで合格にしない。
- 未解決: 数値閾値、校正target、profile有効期限・再校正triggerは`HG-0001`と`HG-0002`で決定する。

## ADR-0022: Phase 0実機入口を一経路へ閉じoperator session内でprocess横断排他する

- 状態: Accepted for Phase 0 safety
- 決定: 旧`capture-single`、`capture-pair`、`stability`はfake contract専用とし、実SDK/WPD transportをcamera open前に拒否する。承認済み実機撮影入口は確認flag付き`hybrid-capture-single`と、その後段の`hybrid-fault-single`／Live View handoffに限定する。
- 排他: 実SDK/WPDへ触れる全Phase 0 CLI commandは`Local\` Windows named mutexをprocess終了まで保持する。transport内のsession guardとtransaction内の`ActiveGuard`は維持し、同じinteractive Windows logon sessionの同一process内・process間で二層排他する。別user session／serviceからの実機操作はMVP運用外とし、M4のinstaller・運用policyで禁止する。
- timeout: pairとhybridは共通の180秒transaction watchdogを持ち、各open、capture、download、delete、closeへ残時間以下を渡す。PC保存では`.partial`書込み後とcanonical rename直前にも期限を確認し、期限切れ後は次のtransport、canonical rename、delete、成功状態へ進まない。期限前に確定済みのPC原本または未確定`.partial`は保持し、安全なcloseは期限後も試行できる。
- 根拠: 旧direct WPD入口が通常CLIに残り、既定transportがWPDだったこと、および従来のSDK/session guardがprocess内限定だったことを独立レビューで確認した。
- 検証: fake-only validator、実CLI negative test、別processによるnamed mutex contention/release、pair/hybrid zero-budget、SDK capture途中超過、canonical rename直前超過のwatchdog contractをSDK有無のsuiteへ追加する。これは実機one-shotや二重process実機試験の合格証拠ではない。

## ADR-0023: 明示的なSingleCameraとDualCamera製品mode

- 状態: Accepted
- 決定日: 2026-08-10
- 決定: 製品は`SingleCamera`と`DualCamera`を明示選択可能にする。`SingleCamera`は登録済み`CAM-A`または`CAM-B`一台を一transactionで撮影・検証・保存し、`DualCamera`は従来どおり`CAM-A → CAM-B`の順次撮影と合成を行う。
- mode境界: mode、required aliases、profile IDはtransaction開始時に固定する。接続台数からmodeを推定せず、`DualCamera`の一台不足を`SingleCamera`へ自動降格しない。active transaction中のmode変更は禁止する。
- 初期一台構成: `SingleCamera`はSDKとWPDの双方で同じ登録済みD810が厳密に一台だけ列挙される場合に限定する。二台接続中に片方だけを選択する運用は、別の安全判断と実機証拠が得られるまで許可しない。
- 出力: `SingleCamera`はcanonical `original.jpg`を画像処理せず単一撮影出力として明示exportし、`StitchOutcome=NotApplicable`とする。合成済みまたはA0品質合格とは表示しない。`DualCamera`の合成・再合成契約は維持する。
- 操作: Live View、接続・identity・card・設定状態確認、撮影、結果確認、明示export、診断、新規撮影準備をmode別に提供する。撮影設定はread-onlyのままとし、write操作は別承認まで追加しない。
- 安全: 両modeでdedicated single-slot spool、SDK/WPD session非重複、operator-session-wide lease、180秒watchdog、canonical PC original、exact just-recovered object cleanup、no retryを維持する。
- 未決: 一台modeの対象原稿サイズ、DPI、crop、将来のlens/crop処理、品質・性能・耐久基準は`HG-0009`で決める。software-onlyまたは既存Phase 0一台証拠を、実WPF Camera Agent連携、一台製品受入、二台実機、A0品質へ読み替えない。

## ADR-0024: SingleCamera先行製品契約をCAM-Aと原画像保存に固定する

- 状態: Accepted
- 決定日: 2026-08-10
- 対象: 最初の`SingleCamera`製品lane。`DualCamera`の二台A0契約は変更しない。
- identity: `SingleCamera`は`CAM-A`だけを対象とし、SDK/WPD双方でD810が厳密に一台のcurrent sessionであること、かつ登録済みWPD serial digestと一致することを要求する。SDKの衝突するSource Name/Interfaceを永続identityに使わず、SDK側は`exactly-one-current-session`としてのみ選択する。二台接続、missing、duplicate、digest不一致はcamera open前にfail closedする。旧identity-v2を自動移行・自動成功扱いしない。
- 出力: 検証済みcanonical originalの`7360×4912` JPEGをbyte-identicalに明示exportする。SingleCameraではcrop、lens補正、DPI metadata、物理原稿寸法、合成品質を保証せず、`StitchOutcome=NotApplicable`とする。保存先は操作者が選択するfixed-local directoryとし、network、UNC、device path、ADS、removable、reparse chainを拒否する。
- profile: アプリ内で観測済みread-only設定を操作者が承認し、30日有効のlocal profileを作成する。初期設定はJPEG Fine、L、S、1/6秒、F8、ISO 64、Preset 1、非空のopaque focus tokenとし、SDKが広告しないFileTypeは`unavailable`として記録する。アプリはcamera settingを書き換えない。承認、期限、alias、profile SHAは撮影transactionへ固定し、撮影直前にも一致を再検証する。
- Live View: 製品UIは開始、継続frame取得、停止を明示操作できる対話的Live Viewを必要とする。現行`hardware.v1`の有限probeは診断専用であり、この要件の合格証拠にしない。継続session、heartbeat、寿命、backpressure、capture handoffを持つ別versionのprotocolで実装する。
- 受入順: 実機撮影は専用empty spoolが用意され明示再開されるまで行わない。再開後はone-shot、10回のcharacterizationでp95を算出し、その実測値をproduct ownerが目標として承認してから、100件連続の初回成功をrelease acceptanceとする。自動retryは行わない。
- Dual境界: 当時の二台SDK identity collisionはDual laneだけをBlockしていた。この未決境界は2026-08-20のADR-0025でsession-local operator binding relaxationへ置換した。SingleCameraの`CAM-A` exact-one identity-v3をDualのCAM-A/B binding証拠へ読み替えない。

## ADR-0025: DualCameraをAgent session内の操作者Live View割当へ限定して再開する

- 状態: Accepted relaxation; implementation and hardware acceptance pending
- 決定日: 2026-08-20
- 決定: documentedな恒久SDK body identityが得られないため、`HG-0003B`をsession-local operator bindingの承認で解消する。Dual Camera Agent session開始時にSDK candidate source objectを厳密に二つだけ取得し、二候補を同時表示せず一台ずつLive View表示する。操作者は表示内容を確認して各candidateを`CAM-A`または`CAM-B`へexactly onceで明示割当する。candidate source object、candidate ordinal、列挙順、USB portは永続identityにせず、割当は同じAgent processのmemory-only `DualIdentitySessionBinding`としてだけ保持する。
- protocol: 安定済み`a0.camera-agent.hardware-dual.v2`は変更しない。別schema `a0.camera-agent.hardware-dual-binding.v1`に`begin-binding`、`start-candidate-live-view`、`get-candidate-live-view-frame`、`confirm-alias`、`complete-binding`を定義する。candidate ordinalはbinding session内だけで有効であり、公開・保存可能な証拠は匿名alias、provider、version、`confirmedAt`、`invalidationReason`に限定する。preview、raw identifier、serial、source object、candidate ordinalは保存・公開しない。
- 完了条件: 同一candidateの二重割当、candidate数が二以外、`CAM-A`または`CAM-B`不足、candidate Live View停止未確認、SDK session full close未確認ではbindingを拒否する。両candidateのLive View停止とSDK session full closeを確認した後だけ内部`DualIdentitySessionBinding`を`Ready`にする。
- invalidation: Agent restart、USB reconnect、camera countまたはtopology変化、SDK manager再生成、任意のSDK errorでbindingを即時invalidにし、再binding完了までは`HardwarePending`とする。撮影はbindingが保持するsource objectを再列挙せず使用する。
- recovery: SDK撮影後、対応するWPD aliasからexactly one objectを回収できなければ、取得済みPC原本を保持して`FailedPartial`にする。他aliasの探索、別objectの削除、自動retryは行わない。
- residual risk: 操作者がLive Viewを見誤ってCAM-A/Bを逆に割り当てるriskは受容して残す。transaction順序は`CAM-A → CAM-B`だが、実シャッター開口時刻の同期や時刻差上限は保証しない。
- gate境界: `HG-0003B`はこのrelaxation決定としてopen/unresolvedから除く。ただし決定だけでは`Hardware Ready`にしない。Issue #9のcore、binding protocol、WPF、capture backendと、実機one-shot、10回、100回が完了するまでDualCameraは`HardwarePending`を維持する。

## ADR-0026: StitchJobのdurable commit pointをversioned manifestの検証済みpublishに固定する

- 状態: Accepted; StitchJob manifest実装済み（Issue #40完了）／他4 artifact typeはPost-MVPへ延期（Issue #205）
- 決定日: 2026-08-20（GitHub Issue #39でuser承認・ERI20 roadmap）
- 決定: StitchJobの唯一のdurable commit pointを`a0.stitch-job-manifest.v1`のatomic・non-replacing publishと直後の再読込検証とする。file存在だけをsuccessとする判定は廃止する。
- commit順序: ①CAM-A/Bのimmutable input snapshot、rig/profile、engine情報を固定 ②output candidateを`.partial`へ生成しflush、full JPEG decode、寸法・size ceiling・SHA-256を検証 ③outputをnon-replacingでpublish ④manifestを別`.partial`へ書きflush ⑤manifestをnon-replacingでatomic publishし、再読込してschema・transaction/job ID・全hash・寸法・result stateを照合 ⑥⑤完了時点だけterminal success。
- manifest最小項目: schema/version、immutable StitchJob IDとCaptureTransaction ID、ordered CAM-A/CAM-B input hash、rig/profile ID・version・hash、engine ID・version、output relative path・SHA-256・寸法・encoded size、terminal result state、`completedAtUtc`、`automaticRetryCount = 0`。実識別子、serial、absolute path、preview、実画像は保存しない。
- crash/recovery: outputが存在してもmanifestが未確定・欠落・partial・schema不一致・hash不一致ならsuccessではない。crash前のinput/output/manifest candidateは診断用に保持し、自動cleanup・自動retry・既存成果物の置換をしない。recoveryはsame-IDのmanifestとartifactをread-only検証し、新しいstitchを自動実行しない。terminal manifestはimmutableで、restitchは新しいStitchJob IDと新manifestを作る。
- migration: v1以前の「file exists = success」は移行せずfail closed。明示migration toolを別承認しない限りlegacy artifactをterminal扱いしない。
- 実装時の補足（Issue #40）: rig/profile hashは呼び出し側が渡す値ではなく、stitcherが実際に適用したprofile値の正規化表現から算出する。渡された値と別のprofileのhashを組み合わせられると、作られていない変換を記録したmanifestができ下流で検出できないため。
- 範囲境界: これはsoftware-onlyのarchitecture decisionであり、画質閾値、A0品質、実機撮影、Hardware Ready、実シャッター同期を承認するものではない。free homography、rig自動学習、原本上書き、retryは引き続き禁止。2026-09-17のProduct Owner判断により、CaptureTransaction、ReviewRecord、ExportRecord、DiagnosticBundleのversioned artifact化は現行MVPへ追加せず、既存挙動を維持したままIssue #205でPost-MVPに追跡する。4種は未実装である。

## ADR-0027: Dual実機撮影方式の検証にCaptureRecoveryOnlyを限定許可する

- 状態: Accepted for controlled Dual hardware verification
- 決定日: 2026-08-27
- 決定: 製品責任者の明示承認に基づき、承認済みrig profileがない段階でも、DualのSDK撮影・WPD回収・canonical PC原本保存だけを評価する`CaptureRecoveryOnly`を許可する。これは製品撮影・合成・A0品質受入ではない。
- protocol: 安定済み`a0.camera-agent.hardware-dual.v2`のcapabilitiesでは`start-reserved-capture-recovery-only`をadditiveに広告するが、既存v2のcapabilities・予約・通常開始・照会・終了payload/result shapeは変更しない。CaptureRecoveryOnlyの開始・同一ID照会・終了は別schema `a0.camera-agent.hardware-dual-capture-recovery-only.v1`を使う。追加操作はrig snapshotと`rigProfileFrozen`を受け取らず、代わりに`captureRecoveryOnlyApproved=true`を厳密に要求する。
- 安全境界: current-session binding、承認済みread-only capture profile、全Live View停止・SDK full close、両card empty、CAM-A→CAM-B、SDK/WPD非重複、canonical原本の再読込検証後だけのexact-object delete、empty-after、180秒watchdog、no retryを通常経路と同じく必須とする。CAM-A失敗時はCAM-Bへ進まず、CAM-B失敗時はCAM-A原本を保持する。
- 結果境界: 追加操作の結果は`capturePurpose=CaptureRecoveryOnly`、`stitchOutcome=Pending`、`a0QualityApproval=Unapproved`を明示し、rig evidenceを生成しない。合成、再合成、合成JPEG export、A0品質合格、実シャッター同期保証を意味しない。
- integration境界: native Dual AgentとWPF製品UIの`CaptureRecoveryOnly` software経路は接続済みである。これは契約試験とローカルbuildの状態であり、実WPF・実D810操作は未検証とする。one-shot、10回、p95承認後100回、異常系の実機証拠が揃うまで撮影方式をGOにしない。

## ADR-0028: DualCameraのSDK Module保持を限定例外として許可する

- 状態: Accepted for controlled Dual `CaptureRecoveryOnly` verification
- 決定日: 2026-08-31
- 決定: 同一Agent process内のmemory-only `DualIdentitySessionBinding`を維持するため、controlled Dual `CaptureRecoveryOnly`に限りSDK Moduleをopaque binding tokenの保持だけの目的で残せる。これはSDK/WPD session同時openの許可ではない。
- 境界: WPDをopenする前に、全candidate Live View、SDK source object、SDK capture sessionをcloseする。WPD open中はSDK API、Live View、capture、candidate enumeration、camera setting writeを一切行わず、WPD closeまでSDK operationを開始しない。WPD cleanupを確認できない場合はSDK API（`End`を含む）を呼ばず、そのAgentを隔離してterminal化し、古いbindingの失効理由を上位へ返して操作者の再bindingを必須にする。ModuleはAgent/binding-session terminal teardown、Agent restart、USB reconnect、camera count/topology変化、SDK manager再生成、任意のSDK errorでunloadし、bindingをinvalidにする。確認済みWPD cleanup後に有効bindingを保持できるのは将来の同一binding連続runのための境界だけであり、現sliceはcoexistence probeとone-shot前software gateまでである。同一bindingの10/100 runnerは未実装で、固定600秒host lifetimeとの両立も未解決のため、10 pair・100 pairを開始またはPass扱いにしない。
- 不変条件: source objectの再列挙・再bindingなし、no retry、camera setting writeなし、vendor operation、existing cardのbulk delete/format禁止、PC canonical originalの再読込検証後だけのexact-object delete、CAM-A失敗時はCAM-Bを開始しない、CAM-B失敗時はCAM-A originalを保持する、を維持する。
- 実機再開前の技術gate: pair-level read-only preflightでWPD D810 exact-two、CAM-A/B map exact-one、両card payload 0、全WPD session closeを一括確認する。さらにproduction adapterのread-only coexistence probeで、WPD open前のSDK source/capture session close、Module retained、WPD session close、WPD open中SDK operation 0を確認する。focused/full回帰と独立reviewの`PASS`後だけoperator-resume済みone-shotへ進む。
- 受入境界: このADRはCaptureRecoveryOnlyのtransport検証だけを対象とし、合成、A0品質、実シャッター同期、releaseを承認しない。one-shot、10 pair、実測p95の製品責任者承認、100 pair、異常系の証跡は引き続き別gateである。

## ADR-0030: STEP4までの受入反復を最大5回へ変更する

- 決定日: 2026-09-21
- 根拠: 本人指示「STEP4までの対応を完了せてください。100回テストは多すぎるので多くても5回程度に収めてください。」
- 決定: 今回の完成範囲はソフトウェア安定化、一台実WPF受入、二台撮影受入、実写A0合成受入まで。新規の実機・実写反復試験は一つの計画された受入系列につき最大5回とし、最初のone-shotもこの5回に含める。Singleは最大5撮影、Dualは最大5組（各body最大5撮影、全体最大10シャッター）とする。成功までの補充、同じ系列を分割して回数を増やすこと、自動retryは禁止する。
- 置換範囲: ADR-0024/0025/0027/0028と旧計画の10回characterization・100回耐久・10回handoffを今回の最大5回へ置換する。既存100回内部coordinatorは過去契約の検証用として保持するが、製品UI/CLIには接続しない。過去の実機1/10/100成功記録やソフトウェアfixtureは改変しない。
- 判定: 五つの初回結果、実行回数、失敗数、全時間値・最大時間を記録する。p95を併記する場合も5標本の記述統計であり、長期信頼性や100回耐久と同等とは主張しない。最初の失敗・未確定・割当失効・時間不足で停止し、未実行は未実行と記録する。既存の性能承認は該当mode・条件だけに適用する。
- 不変条件: 原本保持、SDK/WPD非重複、CAM-A→CAM-B、exact-object cleanup、180秒watchdog、明示割当、no fallback、no retryを維持する。5回への変更はカメラ設定・カード整理・品質閾値・最終リグの承認ではない。
- 未確定時の記録: DualのHardwarePending・割当失効では成功系列の集約証跡を作らない。既存transaction/recovery記録を正本とし、5回の結果が揃ったとは扱わない。
- 実施条件: 対象PC・body・候補SHA/hash・操作・上限を対応付けた実機計画と現在の状態を確認する。本人指示によりAOPC-22-NOTEは使用しない。現在PC（AOPC-11-NOTE）のOS検出はD810一台であり、SDK/WPD readiness・撮影は未実施。実写corpusとHG-0001/0002は未充足。STEP4完了は実写品質の証拠が揃ってから判定する。配布・release（M4）は今回の範囲外。
- ADR-0032で改定／解釈を確定（2026-10-07）: 試行の数え方、ソフト起因でシャッター0の拒否の扱い（判定の例外）、系列の始まり、USB抜去の別系列、保存の合格線、想定外の扱い（割当失効の読み方を含む）はADR-0032に従う。

## ADR-0029: SDK PC直接保存を受入済み経路と分離して評価する

- 状態: Experimental / hardware acceptance failed
- 決定日: 2026-09-16
- 決定: D810のSDK PC直接保存は、dedicated single-slot spool経路の合格を変更せず、独立した候補laneとして評価する。PC直接保存が実機でPC原本を確定するまで、製品経路、MVP受入、従来spool経路の代替として扱わない。
- 安全境界: D810一台と明示alias、operator-session-wide lease、SDK/WPD非重複、最大一回capture、180秒watchdog、SaveMedia復元readback、card fallback／camera delete／format／自動retry 0を必須とする。PC原本は完全JPEG decode・寸法・SHA-256・atomic rename・再読込検証を完了した`original.jpg`だけとする。
- 実機結果: 2026-09-16の2回は、どちらも`FailedPartial / image_event_timeout`、PC原本0件だった。1回目はpost-baselineの同一Item IDを1件観測したが`CaptureComplete` 0、2回目はpost-baseline採用候補0だった。2回目の採用候補0からraw callback 0を断定しない。`cardUnchanged=false`はafter fingerprint取得不能であり、card mutationの証拠ではない。
- 仮説境界: 旧失敗のSDRAM Item残存またはItem ID再利用によるbaseline除外は有力仮説だが未確定である。仮説を決定事実として記録しない。
- 次gate: SDRAM baseline非空をcapture dispatch前に拒否し、filter前raw event／baseline-hit／Children遷移／SaveMedia readback／Capture開始結果を匿名診断するsoftware changeは、PR #202として2026-09-17に`main`へ統合した。次は固定artifactによる実機評価であり、このsoftware統合だけでは実機再開・採用を意味しない。追加撮影は毎回、対象artifact・回数・復元条件を限定した明示承認を要する。

## ADR-0031: DualCameraの最終Live View目標を左右二画面へ更新し、SDK capability gateまで現行一台表示を維持する

- 状態: Accepted product direction; implementation blocked on SDK capability and hardware safety evidence。2026-10-05に実機PoCを凍結（Blocked、末尾の状態の追記）
- 決定日: 2026-09-21
- 決定: `DualCamera`の最終operator experienceは、CAM-Aを左、CAM-Bを右に表示する横長の二pane Live Viewとする。各paneはalias、Live/映像停止、最終frame受信時刻を表示する。二paneをframe同期済み・合成済み・原画像と表示せず、previewを撮影・合成入力・永続証拠へ用いない。
- 現行との差分: 現行`a0.camera-agent.hardware-dual-binding.v1`、`NikonDualBindingSdkAdapter`、fake adapterはいずれも同時に一つのLive Viewだけを許可し、切替時に前candidateをstopしてSDK source/capture sessionをcloseする。このfail-closed制約を、SDK vendor documentationと実機証拠なしに解除しない。従って本ADRは、現行UIへ偽の二画面表示や二重session APIを追加する許可ではない。
- 2026-09-21追加調査: 正規配布SDKの `Module/ReadMe_Eng.txt` Limitations（77行）は一つのmoduleで二台以上の制御を許可しない。`Module/Documents/English/Usage of Type0014 Module(E).pdf` p.6 §8と `MAID3Type0014(E).pdf` p.171 §6.15も複数Sourceの同時openを制限する。したがって現行一module内で二本のLive Viewを開く案は非対応であり、単なる未実装ではない。別process/moduleの構成は資料で保証されておらず、既存排他契約との整合も未確認。代替構成の設計と別承認実機PoCまでは現行制限を維持する。資料・SDK binaryはrepositoryへ転載・同梱しない。
- 実装前gate: (1) ライセンス下のSDK documentationで二つの独立session/sourceによる同時Live Viewを許可すること、(2) 二台start、継続frame、片側停止、例外、USB切断で全sessionを決定的にstop/closeできること、(3) binding aliasの取り違え防止と再binding条件、(4) capture開始前に両Live Viewと全SDK source/capture sessionをcloseしWPD open中SDK operation 0を維持すること、(5) transaction排他・no retry・原本保持を維持すること、をsoftware contractと明示承認済み最大5回の実機検証で確認する。
- 失敗時: 片側のframe取得停止、session close未確認、SDK error、USB topology変化、candidate count変化は両paneをLive成功と見せず、bindingをinvalidにして`HardwarePending`へ戻す。capture transaction開始は拒否する。
- 範囲外: リアルタイム合成、自由homography、previewからのA0品質判断、hardware shutter synchronizationの保証。実機PoCはこのdecisionだけでは開始せず、対象・回数・操作を明記した別承認を要する。
- 状態の追記（2026-10-05）: 凍結（Blocked）。2プロセス×2 moduleの構成による二台Live Viewの実機PoCを、(i) ベンダーの書面確認、または (ii) 所有者が新しい実機予算を明示承認するまで止める。二画面Live Viewの目標そのものは放棄しない（PMの仮定）。根拠は4点。二台runはrun-04・run-05とも候補列挙の直後、最初の命令で失敗した（run-04の分類は記録が無く※推定、run-05はselectで `worker_selection_invalidated`）。run-05ではworker 1がSDKを読み込む前に止まったため、2 moduleの共存は試験できていない。実装前gate (1)「ライセンス下のSDK資料が独立session／sourceによる同時Live Viewを許可すること」は、手元の資料では満たせない。実機preview枠は5/5を消費した。MVPのLive Viewは一台選択式のまま（AGENTS.md）。経緯と証跡は `docs/DUAL_LIVE_WORKER_POC.md` の「二台実機run-05」節、作業順は `docs/CURRENT_STATUS.md` の2026-10-05 run-05節。計装 A（`853c075`）を 2026-10-05 に main へ入れた。同日時点でmainへは未着地）。凍結解除の判断材料はAの計装データで、データを取る次の実機1回には所有者の新たな承認が要る。

## ADR-0032: 受入試行の定義・回数の数え方・USB抜去・保存の合格線を定める

- 状態: Accepted。所有者が2026-10-07に推奨案どおり承認した（末尾の決定欄）。
- 改訂: 2026-10-08に6-Aの表の「transaction作成前の機体照合の失効」の行を実装に合わせて改訂した（末尾の改訂欄）。
- 起票日: 2026-10-07（草案）
- 決定日: 2026-10-07
- 承認者: 所有者
- 決定: 1-A、2-B（条件(i)〜(iii)付き）、3-A、4-A、5-A、6-A。決める事項5の`FR-CAP-007`の解釈（#226の原画像書き出しは「製品export」に当たらない）も承認した。2-Bにより`NFR-REL-001`、MVP受入条件6、ADR-0030の判定を改定し、その他は解釈として既存文書を補う。各文書には本ADRへの参照を付けた。
- 根拠: #230。所有者の目標（2026-10-06、#227）「合成はさておき、保存ができる・二台操作ができることを保証する」。所有者の決定（2026-10-06、#221）「二台の1回目（transaction `020a434c…`、シャッター0）もADR-0030の最大5組に数える。使用2組、残り3組」。#225のコードレビューで確認待ちになったM-6（この決定と`NFR-REL-001`の関係）。
- 背景: 次の4点が文書間で揃っていない。
  - `NFR-REL-001`（[要件](PRODUCT_REQUIREMENTS.md)「非機能要件」）は「予定5回が全て成功」「最初の失敗で停止し、不足分を補充しない」、MVP受入条件6は「失敗時はその系列を不合格として止める」、`FR-CAP-007`は「6組目・補充撮影・自動retryを行わない」、ADR-0030は「同じ系列を分割して回数を増やすこと」を禁じる。どの文書も「試行」が何を指すか（シャッター1回以上か、transaction作成か）を定めていない。
  - 二台の1回目は、CAM-Aのシャッター直前確認がソフトの不具合（#222、D810が`fileType`を広告しない）で必ず失敗したもので、シャッター0、カード操作0、原画像0だった。所有者はこれを5組に数えると決めたが、上の文面どおりに読むと二台系列は1組目で不合格・停止済みになる。#227は残り3組を続ける前提で書かれている。
  - 一台は、2026-10-05（カメラ側はComplete・シャッター1、アプリ側は`original_reread_failed`で保存不可、#216）と2026-10-06（1回目は`OutOfFocus`でシャッター0、2回目はCompleteで書き出しのSHA-256一致）の試行を系列に数えず、#227で新しく最大5回とする案になっている。二台と扱いが揃っていない。
  - #227の受入基準は「記録する」だけで合格線がない。USB抜去を二台の残り3組に含めるか、上限を6組にするかも決まっていない。
- 用語（本ADRの中だけで使う）:
  - transaction作成: 操作者が撮影の主ボタンを押し、新しいclient transaction IDがcamera access前にjournalへ永続化されたこと。同一IDの照会（例: 2026-10-06 16:06の二台の操作。pendingが残っていたため新しいtransactionは作られなかった）はtransaction作成ではない。
  - シャッター: SDK撮影命令によりカメラが実際に撮影したこと。二台では各bodyのシャッターを別に数える。証跡上でどのイベントをシャッターの根拠にするかは※要確認（候補はSDK capture成功のイベントと、撮影後のカード上のobject 1件）。
  - ソフト起因の拒否: シャッターの前に、本製品のソフトの判定（設定確認、readiness、照会の読み取り等）が撮影を拒否し、その原因が本製品のソフトの不具合としてIssueで特定されたもの。カメラ側が撮影を拒否したもの（例: AFが合焦しない`OutOfFocus`）と、操作者・環境の要因（ケーブル、カード、ピント）は含めない。
  - 系列: ADR-0030の「一つの計画された受入系列」。mode別に一つ。

### 決める事項1: 試行の定義

- 1-A: transaction作成を1試行とする。シャッター数は試行の属性として記録する。一台・二台で共通にする。
- 1-B: シャッター1回以上（二台はCAM-Aのシャッター）を1試行とする。シャッター0のtransactionは試行に数えず、記録だけ残す。一台・二台で共通にする。
- 1-C: mode別に定義する（例: 一台はシャッター、二台はtransaction作成）。
- 推奨: 1-A。journalのtransaction IDで機械的に数えられ、シャッターの判定根拠（※要確認）に依存しない。所有者の2026-10-06の決定（シャッター0の1回目を数える）とも矛盾しない。シャッター0の扱いは決める事項2で別に決められるので、定義の段階で除外する必要はない。1-Bはシャッター0のtransactionを何度でも繰り返せる余地が残り、ADR-0030の「補充しない」と緊張する。1-Cではmode間で結果を比べられなくなる。
- 既存文書との関係: 解釈にとどまる。`NFR-REL-001`の「初回試行」と`FR-CAP-007`の「組」の定義を補うだけで、上限5回と補充禁止は変えない。

### 決める事項2: シャッター0のソフト起因の拒否を、回数の予算と合否のそれぞれに数えるか

一台・二台で同じ扱いにする（どの案でも共通）。

- 2-A: 予算に数え、合否にも数える。文面どおりの読み方。二台系列は2026-10-06の1組目で不合格・停止済みとなり、残り3組は撮らない。二台の受入をやり直すには新しい系列が要り、ADR-0030の分割禁止との関係で所有者の改定判断が別に必要になる。
- 2-B: 予算に数え、合否には数えない。条件を3つ付ける。(i) 証跡でシャッター0、カード操作0、原画像0、削除0、自動再試行0が確認できる。(ii) 原因をIssueで特定し、修正がmainに入ってから同じ系列を再開する。修正前に次の試行を始めない。(iii) 同じ原因による2回目の拒否は合否に数える（失敗）。予算を消費するので回数は増えず、ADR-0030の「分割して回数を増やす」には当たらない。
- 2-C: 予算にも合否にも数えない。二台は残り4組になる。所有者の2026-10-06の決定（1回目を数える）を取り消すことになる。
- 推奨: 2-B。所有者の2026-10-06の決定は「予算に数える」までで、合否の扱いは決めていない（M-6）。2-Bはその決定をそのまま保ち、カメラにも原画像にも何も起きていないソフトの不具合を、製品の信頼性の失敗としては扱わない。予算は消費するので、不具合が出るたびに撮り直せる形にもならない。2-Aを選ぶと二台の残り3組は撮れず、#227の二台部分は新しい系列の承認待ちになる。
- 対象外: カメラ側の拒否と、操作者・環境の要因によるシャッター0（例: 一台2026-10-06 1回目の`OutOfFocus`）は、どの案でも予算と合否の両方に数える（失敗）。製品の利用者も同じ状況に遭うため。
- 既存文書との関係: 2-Aは改定なし（文面どおり）。2-Bと2-Cは`NFR-REL-001`の「予定5回が全て成功」「最初の失敗で停止」、MVP受入条件6、ADR-0030の判定に例外を設けるため改定にあたる。承認された場合は`docs/PRODUCT_REQUIREMENTS.md`の`NFR-REL-001`・`FR-CAP-007`と`.autodev/requirements/normalized.json`の同じ項目に、ADR-0032への参照と例外の条件を追記する。

### 決める事項3: 既に行った試行の扱い

| 試行 | 内容 | 3-A（推奨） | 3-B | 3-C |
|---|---|---|---|---|
| 一台 2026-10-05 | カメラ側Complete・シャッター1、アプリ側`original_reread_failed`で保存不可（#216） | 系列に数えない（V-1CAM-005の検証） | 系列の1回目（失敗。系列は不合格） | 系列に数えない |
| 一台 2026-10-06 1回目 | `OutOfFocus`、シャッター0 | 系列に数えない | 3-Bでは10-05の時点で系列は停止済み | 系列に数えない |
| 一台 2026-10-06 2回目 | Complete、書き出しのSHA-256一致、採用 | 系列に数えない | 同上 | 系列に数えない |
| 二台 2026-10-06 1回目（`020a434c…`） | `Failed / CaptureCameraA`、シャッター0、ソフト起因（#222） | 系列の1組目（合否は決める事項2に従う） | 系列の1組目 | 系列に数えない |
| 二台 2026-10-06 16:06の操作 | 同一IDの照会。新しいtransaction 0 | 試行ではない | 試行ではない | 試行ではない |
| 二台 2026-10-06 2回目（`d1d8cda5…`） | `Succeeded`、27秒、両原本保存 | 系列の2組目（成功） | 系列の2組目 | 系列に数えない |
| 以後 | | 一台は#227で新しい系列（最大5回）。二台は残り3組 | 一台の系列は不合格。二台は残り3組 | 一台・二台とも#227で新しい系列（各最大5） |

- 推奨: 3-A。系列の始まりを「所有者がその系列を数えると決めた時点」とする規則にすれば、一台と二台の違いを説明できる。二台は所有者が2026-10-06に1回目から数えると決めたので、その時点から系列が始まっている。一台は2026-10-05・10-06の試行を系列として数える決定がなく、どちらも#216の不具合の発見と修正確認（V-1CAM-005）として行ったため、#227で新しい系列を始める。
- 3-Aの注意点: 二台系列は異なるビルド（1組目はmain `2c2582b`、2組目はmain `cb3df7d`、残り3組は#225・#226の着地後のビルド）にまたがる。ADR-0030の実施条件（対象PC・body・候補SHA/hashの対応付け）を満たすため、各組の記録にビルドのcommitとexeのSHA-256を書く。ビルドをまたいだ結果を一つの系列として合否判定してよいかは、所有者の判断に含める。
- 3-B（一台も10-05から数える）は文面に最も近いが、一台系列は既に不合格になる。3-C（二台も数えない）は一台と完全に揃う一方、所有者の2026-10-06の決定を取り消すことになり、二台の実機の組も余分に使う（残り3組が5組になる）。
- 既存文書との関係: 3-Aと3-Cは解釈（ADR-0030の「計画された受入系列」の始まりを定める）。3-Cは所有者決定の変更を伴う。

### 決める事項4: USB抜去の扱い

- 4-A: USB抜去は正常系列と別の異常系列とし、mode別に1回ずつ行う（一台1回、二台1組）。正常系列の5回の予算に含めない。異常系列の上限は1回で、結果が不明でも追加の抜去はしない（追加は所有者の新たな承認を要する）。合格線は抜く段に依存しない不変条件とする。
- 4-B: 二台の残り3組のうち1組を抜去に使う（正常2組＋異常1組）。抜去の組は正常系列の失敗として数えるため、`NFR-REL-001`の「予定5回が全て成功」を満たせなくなる。
- 4-C: 二台の上限を6組にし、6組目を抜去に使う。`FR-CAP-007`の「6組目を行わない」の改定が要る。
- 推奨: 4-A。MVP受入条件6が既に「USB再接続など個別異常系の結果を記録する」と正常系列と分けて書いており、4-Aはそれを具体化するだけで済む。4-Bと4-Cでは正常系列の合否と抜去の結果が混ざる。実施順は正常系列の後とする（#235の当日の順番どおり）。
- 不変条件（4-Aの合格線。抜いた段は記録するが合否には使わない）:
  1. カード上のobjectの削除0（PC原本の再読込検証が済んでいないobjectを削除しない）
  2. 自動再試行0
  3. 抜去の前にPCで確定済みの原画像は保持される（サイズとSHA-256が抜去の前後で変わらない）。二台でCAM-A確定後にCAM-B側で抜いた場合はCAM-Aの原画像が残る
  4. 終端は`FailedPartial`または`HardwarePending`で止まり、成功を表示しない
  5. 復旧後は新しいtransactionでだけ再開し、同じIDで撮影をやり直さない。二台はUSBのtopology変化でbindingが失効するため、機体照合からやり直す
- 復旧手順: カード上に残ったobjectは自動で帰属・削除しない。必要なら操作者が退避してから手動でclearし、read-onlyの確認で空を確かめてから次へ進む（[Phase 0試験計画](PHASE0_TEST_PLAN.md)の`hybrid-fault-single`と同じ扱い）。
- ※要確認: 抜去をWPFの画面操作中に行うか、Phase 0 CLIの`hybrid-fault-single`／`hybrid-fault-pair`のoperator gate（SDK close後・WPD open前に止まる）で行うか。WPFには抜く時点で止めて待つgateがないため、抜く段は操作者の手のタイミングで決まる。#227の目的（アプリで保存できることの保証）からはWPFでの実施を推奨するが、どの段で抜けたかを証跡から特定できるかは確認が要る。二台でどちらのbodyを抜くか（CAM-A確定後にCAM-Bを抜くと不変条件3を確かめられる）も所有者の判断に含める。
- 既存文書との関係: 4-Aは解釈（MVP受入条件6、`FR-CAP-006`、ADR-0030の「一つの計画された受入系列につき最大5回」の範囲内）。4-Cは`FR-CAP-007`の改定。

### 決める事項5: 保存の合格線

- 5-A: 正常系列の全件で次の4つを満たす。
  1. 撮影結果が成功（一台は`Complete`、二台は`Succeeded`）
  2. アプリの書き出し操作で、一台は1枚、二台は2枚（CAM-A・CAM-B）の原画像を操作者が選んだ固定ローカルフォルダへ書き出し、書き出したファイルのSHA-256を`Get-FileHash`で再計算して、アプリが表示する原画像のSHA-256と一致する
  3. カード上の削除は回収した該当1件だけで、それ以外の削除0、自動再試行0
  4. 失敗時（書き出し失敗、SHA-256不一致、保存先の異常）は成功を表示せず、理由と次の操作を表示する
- 5-B: 1・3・4は全件、2は各modeで1回だけ確かめる（抜き取り）。
- 推奨: 5-A。#235のG5（保存）は「一台1枚・二台2枚を指定フォルダへbyte-identicalに書き出す」ことを求めており、抜き取りでは系列の一部でしか保証できない。書き出しは実機の追加撮影を伴わない。
- 4の確かめ方: 正常系列では失敗が起きない前提のため、実機で4を確かめられるのは失敗が起きたときとUSB抜去系列だけになる。保存先の異常（書き込み不可のフォルダ等）の実機試験は本ADRでは追加しない。書き出し失敗時の表示を固定するソフト試験があるかは※要確認（無ければ#226か別Issueで追加する）。
- 二台の原画像書き出しについての解釈: `FR-CAP-007`は`CaptureRecoveryOnly`で「製品exportを開始しない」と定め、ADR-0027は合成JPEG exportを意味しないとしている。#226の書き出しは保持済みの原画像をbyte-identicalに複製するもので、合成JPEGの製品exportではないと解釈する。この解釈も所有者の承認対象に含める。
- 既存文書との関係: 解釈。MVP受入条件3（一台のbyte-identicalな明示保存）を二台の原画像にも当てはめ、合格線を具体化する。上の`FR-CAP-007`の解釈を含む。

### 決める事項6: 実機日に想定外が起きたときの扱い

- 6-A: 次の表に従う。
- 6-B: 表を持たず、その場で所有者が判断する。
- 推奨: 6-A。実機日は照合の有効期限（確定から5分）とAgentの寿命（起動から10分）に追われるため、判断を事前に決めておく方が誤操作が少ない。

| 起きたこと | 系列の扱い | その日の扱い | 記録 |
|---|---|---|---|
| 正常系列のn回目（n組目）が失敗（`Failed`・`FailedPartial`・書き出しのSHA-256不一致・環境起因のシャッター0を含む） | その系列を停止。残りを撮らない。系列は不合格 | 別modeの系列は、原因がそのmode固有と判断できる場合だけ続けてよい。共通部分（Agent、保存、journal）が疑わしければ終了 | 原画像・カードに触らずに証跡を保存し、Issueを立てる |
| シャッター0のソフト起因の拒否 | 決める事項2に従う。2-Bなら系列を一時停止し、修正の着地後に同じ系列を再開 | 終了 | Issueを立てる |
| 撮影結果に影響しない新しい不具合（表示、終了ロック等） | その試行の判定は記録された結果に従う | 終了してIssueを立てる。続けるかは所有者がその場で判断 | Issueを立てる |
| 結果不明（pendingが解けない、`HardwarePending`） | 同一IDの照会だけを行う。新しいtransactionを始めない。確定しなければ系列を停止 | 確定しなければ終了 | 照会の結果を記録 |
| transaction作成前の機体照合の失効 | 試行に数えない。5組の実行はそこで停止する。アプリを終了して起動し直し、機体照合からやり直して新しい5組の実行で続ける（2026-10-08改訂。改訂前は「照合からやり直す」「続行」。末尾の改訂欄） | 起動し直して続ける。受入予算の残数は手順で管理する | 失効の時刻と、撮影を始めなかった組の番号を記録 |
| transaction作成前のAgentの自然終了 | 試行に数えない。照合からやり直す | 続行 | 終了の時刻を記録 |
| transaction作成後の割当失効 | ADR-0030どおり系列を停止 | 終了 | Issueを立てる |
| 撮影前の確認でカードが空でない | 撮影しない。試行に数えない | 退避・手動clearは所有者の判断。行わなければ終了 | payload件数を記録 |
| アプリが閉じられない（終了ロック） | 試行の結果には影響しない | #225の案内どおりAgentの自然終了を待ってもう一度閉じる。強制終了しない | 待った時間を記録 |
| USB抜去系列で不変条件のどれかが崩れた | 異常系列は不合格 | 終了 | P0のIssueを立てる |

- 既存文書との関係: 解釈。ADR-0030の「最初の失敗・未確定・割当失効・時間不足で停止し、未実行は未実行と記録する」を具体化する。transaction作成前の照合失効を停止条件に含めない点は、ADR-0030の「割当失効で停止」の読み方を定めるもので、所有者の承認対象に含める。

### 推奨案のまとめと承認後の作業

- 推奨: 1-A、2-B、3-A、4-A、5-A、6-A。この組み合わせでは、二台は1組目（ソフト起因・合否に数えない）と2組目（成功）を使って残り3組、一台は#227で新しく最大5回、USB抜去は各mode 1回の別系列になる。
- 承認後の作業: (1) 本ADRの状態をAcceptedにし、決定日と承認者を記入する。(2) 2-Bまたは2-Cを選んだ場合は`NFR-REL-001`・`FR-CAP-007`と`.autodev/requirements/normalized.json`の同じ項目を改定する。(3) #227の受入基準を本ADRの合格線に書き換える。(4) #225のM-6を閉じる。(5) `docs/CURRENT_STATUS.md`の#221節に決定を反映する。
- 範囲外: 合成・A0品質（`HG-0001/0002`）、電源断、二台同時Live View（ADR-0031で凍結）、性能目標の新規承認。本ADRは実機の撮影開始そのものを承認しない。実機日の実施は#227で別に承認する。

### 所有者の決定欄

| 項目 | 選択肢 | 推奨 | 所有者の選択 | 決定日 | 備考 |
|---|---|---|---|---|---|
| 1 試行の定義 | 1-A / 1-B / 1-C | 1-A | 1-A | 2026-10-07 | |
| 2 シャッター0のソフト起因の拒否 | 2-A / 2-B / 2-C | 2-B | 2-B | 2026-10-07 | 条件(i)〜(iii)付き。`NFR-REL-001`・MVP受入条件6・ADR-0030の改定 |
| 3 既に行った試行 | 3-A / 3-B / 3-C | 3-A | 3-A | 2026-10-07 | 二台はビルドをまたいで一つの系列として判定する（各組にビルドのcommitとexeのSHA-256を記録）。二台は残り3組、一台は#227で新しく最大5回 |
| 4 USB抜去 | 4-A / 4-B / 4-C | 4-A | 4-A | 2026-10-07 | 実施経路は本文の推奨どおりWPF。抜いた段を証跡から特定できるかと抜くbodyは、#227の実施計画で確かめる（※要確認のまま） |
| 5 保存の合格線 | 5-A / 5-B | 5-A | 5-A | 2026-10-07 | 二台の原画像書き出しは製品exportではないという`FR-CAP-007`の解釈を承認 |
| 6 想定外の扱い | 6-A / 6-B | 6-A | 6-A | 2026-10-07 | transaction作成前の照合失効を停止条件に含めない読み方を承認 |

- 承認者: 所有者（選択肢から「推奨案どおり承認」を選んだ）
- 承認日: 2026-10-07
- 承認後の作業の状況（2026-10-07）: (1) 状態・決定日・承認者を記入した。(2) `docs/PRODUCT_REQUIREMENTS.md`の`NFR-REL-001`・`FR-CAP-007`・MVP受入条件6、ADR-0030、`.autodev/requirements/normalized.json`の同じ項目に本ADRへの参照を付けた。(3) #227の受入基準の書き換えと(4) #225のM-6を閉じる作業はIssue側で行う。(5) `docs/CURRENT_STATUS.md`の表記を承認済みに直した。

### 改訂欄

- 改訂（2026-10-08）: 6-Aの表の「transaction作成前の機体照合の失効、Agentの自然終了」の行を2行に分け、機体照合の失効の行を実装に合わせた。改訂前の扱いは「試行に数えない。照合からやり直す。続行」。改訂後は「試行に数えない。5組の実行はそこで停止する。アプリを終了して起動し直し、機体照合からやり直して新しい5組の実行で続ける」。Agentの自然終了の行は改訂していない。
  - 理由: 実装は5組の実行を1プロセス1回に限り、機体照合のスナップショットを5組全体で共有するため、同じプロセスで照合をやり直して続ける手段がない。安全ゲート（照合が有効でない組は撮らない、1プロセス1回）を変えずに済む方を選んだ。
  - 決定: 所有者（2026-10-08、「ADR-0032を実装に合わせる」）。関連Issue: #259、#251。
  - 用語: 「5組の実行」は、`--capture-recovery-only --capture-recovery-run-count 5`で起動したアプリが主ボタン1回で行う最大5組の連続撮影を指す。ADR-0030の受入系列とは別のもので、照合の失効で5組の実行が止まっても受入系列は停止せず、不合格にもしない（決める事項6の「transaction作成前の照合失効を停止条件に含めない」は維持）。
  - 実装の事実（main `22572ce`で確認）: 主ボタンを押した時点で有効時間5分の照合スナップショットを1つ作り、5組の実行の全組に同じものを渡す（`src/m3/OperatorShell/ViewModels/OperatorShellViewModel.cs`の`RunCaptureRecoveryOnlyAsync`）。各組の開始時にスナップショットが有効でなければ、撮影IDを作る前に`IdentityNotReady`で返り、シャッターは切らない（`src/m3/OperatorShell/Hardware/HardwareDualCaptureRecoveryOnlyWorkflow.cs`の`CaptureAsync`）。5組の実行はこの結果で停止し、集約証跡は作らない。同じプロセスで2回目の5組の実行は始められない（`src/m3/OperatorShell/Hardware/CaptureRecoveryOnlyFiveRunCoordinator.cs`の`HasStarted`）。組数は5に固定で、5未満を指定する手段も、組と組の間で止める操作もない（同ファイルの`RequestedCount`、`src/m3/OperatorShell/App.xaml.cs`は`--capture-recovery-run-count`に5だけを受け付ける）。
  - 試行の数え方: 試行の正本はjournalで、transaction作成を1試行とする（1-A）。アプリ画面の撮影回数（`TransactionStartCount`）と5組の実行の技術情報の`attempted`は、transaction作成前の拒否（照合の失効、事前確認の失敗）も含むため、受入の記録には使わない。
  - 受入予算の残数: 起動し直した5組の実行はまた最大5組を撮るため、受入系列の残りの予算（例: 二台は残り3組）はアプリでは管理せず、手順で管理する。残りが5組未満のときに5組の実行を始めると予算を超えうるため、主ボタンを押す前に残数を確かめる。`--capture-recovery-run-count`を付けずに起動した1組モードは主ボタン1回で1組だけを撮り、照合スナップショットもその都度作る。
  - 確かめること: #227の前に二台1組の実際の所要時間を測り、5組が照合の有効時間5分に収まるかを確かめる。二台1組の所要時間の記録は、決める事項3の表の二台2回目（27秒）の1件だけで、5組分の所要時間は未測定。

## ADR-0033: 合成の段階0〜3を定め、pilot撮影系列と開発用draft合成を受入系列・製品経路から分ける

- 状態: Accepted（2026-10-08、所有者）。起票時は所有者の決定（#233・#42）を写した部分だけがAcceptedで、起票者が足した定義・数え方・手順はProposedだった。所有者が同日に草案のとおり承認し、6節の撮影設定とレンズの型式も決めた（末尾の決定欄）。
- 起票日: 2026-10-08（草案）
- 根拠: #272（本ADRの草案）、#278（合成の道筋。段階の表と守ること）。所有者の決定（2026-10-08）: #233「pilotを撮ってよい。最大36組（較正用12・開発用24）。ADR-0030とは別枠。holdoutに使わない。#227の後、1日以内。機材の購入はpilotの結果を見てから、A0チャートの印刷代だけ先に許可」、#42「v0.1を測り始めるための物差しとして使ってよい。最終承認はlocked holdoutの結果の後」。
- 関係するADR: ADR-0021（撮影ごとにhomographyを推定しない）、ADR-0024（アプリはカメラ設定を書き換えず、承認済みread-only capture profileとの一致を撮影直前に確かめる）、ADR-0026（StitchJobのproduct manifest）、ADR-0027（`CaptureRecoveryOnly`）、ADR-0028（Agent終了でbindingが失効する）、ADR-0030（受入系列は最大5回）、ADR-0032（受入試行の数え方）。並行のADR-0034（#271、rig profile v2）とは扱う事項を分け、本ADRはprofileの形式を定めない。
- 範囲外: 品質基準の最終承認（`HG-0001`）、承認リグと機材の購入（`HG-0002`・#43）、段階3の合格線（ADR-0027を改定する新しいADRで決める）、#227の実施承認。本ADRは実機の撮影開始そのものを承認しない。pilotの実施日は#227の完了後に別に決める。

### 1. 「合成を動かす」の段階0〜3（Proposed）

道筋の親Issue（#278）の表を本ADRで正本にする。#278の表と食い違う場合は本ADRに従う。

| 段階 | 状態 | 受入基準 | 使う入力 | 合格しても意味しないこと | 対応 |
|---|---|---|---|---|---|
| 0 | ソフトだけで、実寸の合成チャート二枚を合成して測れる | 正解つきの実寸の合成ペア（#269）を評価経路で合成し、正解に対する位置ずれ、処理時間、ピークメモリ、同じ入力での出力hashの一致を数値で記録できる。実機の操作0、実写0 | 合成ペアだけ | 実写で動くこと、品質の合格 | #268・#269・#270・#273・#274・#275 |
| 1 | 実写2枚から1枚の合成画像が出て、目で見て、ずれを数字で読める | pilotの1組（CAM-A・CAM-B各1枚）から評価専用exe（#273）が合成画像1枚を出し、記録に`profileStatus=draft`と`quality=not-evaluated`が残る。人が目で見た所見と、重なり帯のずれの数値（#275の評価器）を記録する。合否は付けない | pilotの組 | 品質、較正の妥当性、製品経路で動くこと | pilot（本ADRの2〜6節） |
| 2a | 開発用の実写の組で、v0.1の物差しで全項目を測れる | v0.1候補の全項目（#42の2026-09-17のコメントの一覧）について、pilotの開発用の組で評価器が値を出す。値を出せない項目が0件。製品の合否は決めない | pilotの開発用の組 | 品質基準への合格、承認リグの性能 | #45〜#47 |
| 2b | 承認リグのlocked holdoutで品質基準に合格する | 承認済みのリグとprofileで、パイプライン凍結後に撮ったlocked holdoutが、所有者が最終承認した品質基準に合格する | locked holdout（pilotは使わない） | アプリの撮影経路で動くこと | G4a・#49・`HG-0001` |
| 3 | アプリで撮影→合成→確認→合成JPEGの書き出しまで実機で通る | 実WPFで二台の撮影から合成JPEGの書き出しまでを実機で通す。合格線はADR-0027を改定する新しいADRで決める | 製品経路の撮影 | （新しいADRで定める） | G4b・D-4 |

- 各段階の合格を次の段階の合格に読み替えない。段階1で合成画像が出ても段階2aの「全項目を測れる」の代わりにならず、段階2aで測れても段階2bの合格の代わりにならない。
- 段階は[検証計画](STITCHING_VALIDATION_AND_EXPERT_REVIEW.md) §4.1のL1〜L5とは別の物差しで、互いに読み替えない。段階1・2aの証拠は手元の機材の暫定リグによるもので、L3（承認済み二台rig）とL4（製品経路）の証拠にならない。
- 段階1・2aの数値は実写由来の値なので、リポジトリにコミットしない（7節）。

### 2. pilot撮影系列の数え方

Accepted（2026-10-08、所有者、#233）:

- pilot（合成の開発用の試し撮り）を撮ってよい。上限は最大36組で、内訳は較正用12組・開発用24組。手元の機材の暫定リグと自社チャートで撮る。
- ADR-0030の受入系列（最大5回）とは別枠で数える。pilotの組は受入系列の予算を消費せず、受入系列の結果にも数えない。逆に受入系列の組をpilotに数えない。
- holdoutには使わない。
- #227の後に撮り、1日以内に収める。
- 機材の購入（同じ型のレンズ・照明・測定器など）はpilotの結果を見てから上限を決める。A0チャートの印刷代だけ先に許可する。

Proposed:

- 1組の定義: リグと原稿を動かさずに、CAM-A→CAM-Bの順に各bodyで1回ずつシャッターを切って得る原画像2枚。順序はADR-0027と同じ。
- 数える時点: CAM-Aのシャッターが切れた時点で1組を消費する。ADR-0032の受入試行（transaction作成を1試行とする）と数え方が違うのは、アプリ外の経路（5節）にはtransactionが無いため。上限は最大72シャッター（各body最大36）になる。
- シャッター0で終わった組（カメラ側の拒否、ソフトの拒否、照合の失効）は組数に数えず、起きた時刻と理由を記録する。シャッター0が続く場合はその日のpilotを止め、原因をIssueにする。
- 失敗した組: 片側だけ撮れた組、ピントやブレや露出が外れた組、原画像の保存に失敗した組は組数に数える。上限の範囲でなら撮り直してよいが、36組を超えて補充しない。失敗した組の画像は消さずに同じsplitに残し、失敗の種類を記録する（failure sensitivityの材料にする）。
- 内訳: 較正用12組と開発用24組は互いに融通しない。較正用が余っても開発用に回さず、逆も同じ。
- 1日で上限に届かなかった場合は、残りを撮らずにpilotを終える。別の日に続けるには所有者の新しい判断が要る。

### 3. splitとgroup、holdoutとの分離

Accepted（2026-10-08、所有者、#233）: pilotの画像をholdoutに使わない。

Proposed:

- pilotの36組はすべて`development` splitに入れる。較正用の12組はdevelopmentの中の役割（draftキャリブレーション #277 の入力）として扱い、`calibration` splitには入れない。`locked-holdout`と`release`には入れない。
- 理由: [コーパスの契約](CORPUS_CONTRACTS.md)の「split分離」は、同じ`originalMasterGroupId`、`captureSessionGroupId`、`rigStateGroupId`、`derivationFamilyId`を異なるsplitに置くことを禁じている。同じ日に同じ暫定リグで較正用と開発用を撮ると、`captureSessionGroupId`と`rigStateGroupId`が`calibration`と`development`にまたがる。両者の間でリグを組み直せばgroupは分かれるが、較正の結果を同じリグ状態の開発用の組に当てられなくなる。pilotのキャリブレーションはdraftであり、`calibration` splitの用途（profile calibration）にも当たらない。
- holdoutの分離: holdoutはパイプライン凍結後に、pilotと別の原稿の版（`originalMasterGroupId`が別）、別の撮影日（`captureSessionGroupId`が別）、別のリグ状態（`rigStateGroupId`が別）で撮る。pilotで使った版下をholdoutで使わない。版下はdevelopment用とholdout用の2系統を分けて作る（#276）。
- pilotの結果を見てパイプラインを直すのは凍結の前だけとする。holdoutを見て直した場合、そのsetはlocked holdoutとして失効する（コーパスの契約のとおり）。
- ※要確認: 承認リグで較正とholdoutを撮るとき、同じリグ状態が`calibration`と`locked-holdout`にまたがる。`rigStateGroupId`の粒度（設置1回ごとか、設置して較正した状態ごとか）は#43・#49で決める必要がある。本ADRでは決めない。

### 4. 開発用のdraft合成（評価専用exe、#273）

Proposed:

- 製品の合成はapprovedのprofileだけを受け付ける。この拒否（`src/m2/offline_stitcher.cpp`の`rig profile must be approved`）を変えない。製品のM2Adapterにdraftを許す分岐を足さない。approvedを名乗る試験用のprofileを作らない（provenanceの偽装になる）。
- 評価専用の合成は製品と別のexeにし、既定OFFのCMake optionを付けたときだけビルドする。既定のビルド成果物と配布物に含めない。
- 出力の記録に`profileStatus=draft`と`quality=not-evaluated`を残す。ADR-0026のproduct manifest（`a0.stitch-job-manifest.v1`）を書かず、別のschemaの記録を書く（名前は#273で決める）。出力先は製品のルートの外にする。
- draftの出力と記録を、approved profile、product manifest、`HG-0001`の合格の証拠に流用しない。
- ADR-0021のとおり、撮影ごとにhomographyを推定しない。推定するのは開発時の校正ツール（#277）だけで、その結果はdraftのprofileにとどまる。
- ※要確認: 既存のcorpus manifestと[M2 pre-gate](M2_PRE_GATE.md)は品質の欄を`qualityDecision: not-evaluated`と書く。評価用の記録の欄名を`quality`にするか`qualityDecision`に揃えるかは#273で決める。

### 5. 撮影経路

Proposed:

- 既定の経路: アプリの`CaptureRecoveryOnly`の1組モード（`--capture-recovery-run-count`を付けずに起動）。5組の実行は組数が5に固定で、pilotの残数の管理に合わないため使わない。
  - 実装の事実（main `b9d48cd`で確認）: 1組モードは主ボタン1回で1組を撮り、押すたびに有効時間5分の照合スナップショットを作る（`src/m3/OperatorShell/ViewModels/OperatorShellViewModel.cs`の`RunCaptureRecoveryOnlyAsync`）。組の開始時にスナップショットが有効でなければ、撮影IDを作る前に`IdentityNotReady`で返り、シャッターは切らない（`src/m3/OperatorShell/Hardware/HardwareDualCaptureRecoveryOnlyWorkflow.cs`）。起動時の組数は1か5だけを受け付ける（`src/m3/OperatorShell/MainWindow.xaml.cs`）。Agentは起動から10分で自分で終了する（`src/phase0/include/a0/phase0/agent_host_lifetime.hpp`の`kDefaultBudgetMilliseconds = 600000`。C#側の写しは`src/m3/OperatorShell/Hardware/DualCameraAgentLifecycle.cs`の`AgentMaxLifetime`）。Agentが終わるとbindingは失効し（ADR-0028）、機体照合からやり直す。
  - 36組を撮るにはAgentの起動と機体照合を何度も繰り返す。二台1組の所要時間の記録は27秒の1件（ADR-0032）だけで、Agentの起動と機体照合にかかる時間は未測定（※要確認）。
  - pilotのtransactionは受入系列と同じjournalに入る。取り違えないよう、pilotのtransactionと組番号・用途の対応をリポジトリの外の記録に書く。結果は`capturePurpose=CaptureRecoveryOnly`、`stitchOutcome=Pending`のままで、pilotの組は#227の証拠にならない。
  - 両カードが空であることが撮影の前提で、回収した1件だけを再読込検証の後に消す（ADR-0027のとおり）。
- アプリ外の経路（カメラ単体で撮り、カードから読み出す）: 機体照合5分とAgent 10分の失効のため、1日の中で予定の組数をアプリで撮り切れないと分かったときに使う。切り替えは実施前に所有者が了承する。
  - 位置付け: [検証計画](STITCHING_VALIDATION_AND_EXPERT_REVIEW.md) §4.1の注記にある「アプリ外の承認済み手順で取得された画像」と、§4.2の「本アプリ外の別途承認済み取得手順」に当たる。本節がその手順の草案で、所有者の承認で承認済みの手順になる。結果はofflineの評価にだけ使い、`HG-0003B`、製品のcapture flow、L4、#227の証拠にしない。
  - 手順:
    1. 撮影前に両カードの中身の件数を確かめて記録する。空でないカードを消さない。pilot用には空のカードを用意する。
    2. CAM-A→CAM-Bの順に各1回シャッターを切る。シャッターの切り方（リモートレリーズの有無）は※要確認。
    3. カードから原画像をPCへコピーする。移動はしない。コピー先はリポジトリの外にする。
    4. コピー元とコピー先のSHA-256を`Get-FileHash -Algorithm SHA256`で計算し、一致を記録する。
    5. 組ごとに記録する項目: 組番号、用途（較正用・開発用）、CAM-A・CAM-Bと本体の対応、カード上のファイル名、SHA-256、撮影条件（露出モード、シャッター速度、絞り、ISO、WB、フォーカスモード、画質とサイズ。EXIFから読む）、撮影時刻、原稿の版、リグ状態、取得者と取得方法。本体の識別子（serial）はリポジトリの外の記録にだけ書き、リポジトリでは別名（CAM-A・CAM-B）で書く。
    6. カード上のファイルを手で消さない。カードをフォーマットしない。どちらも所有者の明示の判断があるときだけ行う。
  - 注意: カードにpilotの画像が残っていると、アプリの経路はカードが空でないため撮影しない（ADR-0027）。アプリの経路に戻る前に、カードの扱いを所有者が決める。

### 6. 撮影設定（2026-10-08、所有者が決定）

- フォーカス: 2台ともMF。
- 絞りとcapture profile: 承認済みの設定（F8）で撮る。pilot用に別のprofileは承認しない。アプリは設定を書き換えず、撮影直前に承認済みprofileとカメラの設定の一致を確かめる（ADR-0024）ので、F8から変えるとアプリの経路では別のprofileの承認が要る。二台で使う承認済みprofileの名前と有効期限は、pilotの前に確かめる（※要確認）。
- レンズ: 手元の2本は同じ型（所有者の回答）。型式の名前は※未記録で、pilotの撮影記録に書く。

### 7. 実画像と実写由来の値をコミットしない

Proposed（#278の「守ること」を本ADRで確かめ直したもの）:

- pilotの原画像、合成画像、評価の数値、EXIF、hash、transaction ID、撮影の日時をリポジトリにコミットしない。保存先はリポジトリの外にする。
- pushの前に`pwsh -NoProfile -File scripts/Test-ReplayFixtureLeak.ps1 -Base origin/main`で、pushする内容にこのPCの実際の識別子が含まれないことを照合する（#241・#243・#260）。
- ※要確認: 段階1・2aの結果（ずれの数値など）をPUBLICのIssueにどこまで書くか。

### 所有者に確認すること

1. 1節の段階0〜3の受入基準。
2. 2節の数え方（1組の定義、CAM-Aのシャッターで1組を消費、シャッター0は数えない、失敗した組は数えて補充しない、内訳を融通しない、1日で終わらなければ打ち切る）。
3. 3節のpilotをすべて`development`に入れる扱い。
4. 4節のdraft合成の扱い。
5. 5節のアプリ外の経路の手順と、切り替えを所有者が了承する運用。
6. 6節の撮影設定（MF、capture profile、絞り）。
7. 手元のレンズの型式。

### ADR-0033の所有者の決定欄

| 項目 | 状態 | 所有者の選択 | 決定日 | 備考 |
|---|---|---|---|---|
| pilotを撮ってよい・最大36組（較正12・開発24）・ADR-0030と別枠・holdoutに使わない・#227の後1日以内 | Accepted | 承認 | 2026-10-08 | #233 |
| v0.1を測り始めるための物差しとして使う（最終承認はlocked holdoutの後） | Accepted | 承認 | 2026-10-08 | #42 |
| 機材の購入はpilotの結果を見てから・A0チャートの印刷代だけ先に許可 | Accepted | 承認 | 2026-10-08 | #233 |
| 1 段階0〜3の受入基準 | Accepted | 草案のとおり承認 | 2026-10-08 | #272 |
| 2 pilotの数え方 | Accepted | 草案のとおり承認 | 2026-10-08 | #272 |
| 3 split（すべて`development`） | Accepted | 草案のとおり承認 | 2026-10-08 | #272 |
| 4 draft合成の扱い | Accepted | 草案のとおり承認 | 2026-10-08 | #273 |
| 5 撮影経路とアプリ外の手順 | Accepted | 草案のとおり承認（アプリ外への切り替えはその都度所有者が了承） | 2026-10-08 | #272 |
| 6 撮影設定 | Accepted | 2台ともMF、承認済みの設定（F8） | 2026-10-08 | #272 |
| 7 レンズの型式 | 回答あり | 2本は同じ型（型式の名前は撮影記録に書く） | 2026-10-08 | #233 |

## ADR-0034: rig profile 2.0.0を文書平面基準にし、カメラごとのレンズ係数とmm/DPIの出力ラスタを持たせる

- 状態: Accepted（2026-10-08、所有者が草案のとおり承認。「所有者の確認が要る点」の1〜9も草案の読み方のとおり）。製品の挙動は#274の実装と最初のapproved（#45）まで変わらない。
- 起票日: 2026-10-08
- 関連: #271（本ADR）、#278（合成の道筋）、#274、#277、#273、#45、#46、#42（`HG-0001`）、#43（`HG-0002`）、ADR-0021、ADR-0026
- 設計: [rig profile 2.0.0の設計](design/rig-profile-v2.md)。schemaは`docs/schemas/rig-profile.v2.schema.json`、雛形は`samples/public/rig-profile.v2.draft.example.json`、試験は`scripts/Test-RigProfileV2Schema.ps1`（ctest `m2_rig_profile_v2_schema`）。
- 背景: 1.1.0はCAM-Aの生pixelを基準にした固定3×3（CAM-B→CAM-A）と%のcropだけを持ち、レンズ係数・DPI・出力の向きの欄が無い。C++と.NETはcropをpxで持ち、JSONの%と単位も合っていない。#42のv0.1候補（A0横8426×5960・180 DPI）を一度の写像で出せず、#46の完了条件「resamplingは1回」を満たせない。

### 決定事項（案）

1. rig profile 2.0.0を文書平面基準にする。座標は出力と同じ向きで見た用紙の左上を原点とするmm（X右・Y下）。カメラごとに内部パラメータ（fx・fy・cx・cy）、Brown–Conradyの5係数（k1・k2・k3・p1・p2。名前つきの欄で全部必須）、文書→歪みのない画像の3×3射影変換（行優先、列ベクトル、`H[2][2]` = 1）を持つ。
2. 出力ラスタを文書のmm（用紙の内側の領域、1 µm刻み）と整数のDPIで決める。画素数は`ceil(幅[µm] × DPI / 25400)`と一致させる。出力画素の中心から各カメラの生pixelまでを1本の式（文書→画像→歪み→生pixel）で求め、resamplingは1回にする。
3. 生pixelはJPEGに保存された順で使い、画素(i, j)の標本点を(i, j)に置く（OpenCVと同じ）。被覆の判定は1.1.0の`[0,w)`×`[0,h)`（#86）を引き継ぐ。EXIF Orientationは校正・生成・合成・評価のどの段でも適用しない。
4. draftは「校正の値」（用紙と向き、写像とレンズ係数、校正の記録）を全部そろえて持つか、全部nullにする。「所有者の決定」（出力ラスタ、補正上限、品質の閾値、承認の記録）はdraftに持たせない。approvedは全部を持つ。
5. 向きはリグの配置の決定（`HG-0002`）として`documentPlane`に置く。DPIと出力領域は出力品質の決定（`HG-0001`）として`outputRaster`に置く。
6. manifestの`rigProfile.sha256`は、正規化した行（数はIEEE 754 binary64のビット列）のSHA-256とする。評価時刻は入れない（1.1.0の`ProfileFingerprint`は`assessedAt`を入れていた）。
7. 1.1.0は試験専用として残す。1.1.0では実リグのprofileを作らず、承認もしない。移行はストラングラー方式（設計12節）で、製品のadapterから1.1.0を外すのは2.0.0の最初のapproved（#45）の後。

### 却下した案

- CAM-A基準のまま両カメラにレンズ係数を足す（1.1.0の拡張）: 出力の格子がCAM-Aの画素に縛られ、A0のmmとDPIに合わせるための回転・拡縮の段が後ろに残る。その段は2回目のresamplingになるか、1本の式にまとめるとCAM-Aを特別扱いした文書平面基準と同じになる。cropも%とpxのままで、出力の形の要件を満たせない。
- 校正の結果を3つ目の状態（candidate等）で表す: #272・#273・#277が「draftのprofile」と書いており、並行作業の語とずれる。draftの中で校正の値と所有者の決定を分ければ同じ安全性が得られる。
- draftの値をすべてnullに限る（1.1.0と同じ）: #277の受入基準「出力profileは常にdraftで、provenanceを持つ」と矛盾する。
- 係数をOpenCVの並びの配列で持つ、接線の係数を任意にして欠けたら0とする、fingerprintをファイルのbyte列やRFC 8785で作る: 設計15節。

### 守る不変条件

1. 製品の合成はapprovedのprofileだけを受け付ける。校正の値があるdraftも拒否する。draftでの合成は評価専用の別exeと別manifest（#273）で行う。
2. 撮影ごとにhomographyを推定しない（ADR-0021）。推定は開発時の校正ツール（#277）だけで行う。撮影ごとの補正は承認された上限の内側の相似変換に限る。
3. 製品コードに数値の既定値（閾値、レンズ係数、DPI、用紙の寸法）を置かない。欄が無ければ失敗にする。A0の寸法はschemaが固定し、製品はprofileから読む。
4. resamplingは1回。中間画像を作らない。
5. EXIF Orientationを適用しない。
6. profileを実行時に変更・学習しない。内容が変われば新しい`profileId`にする。
7. fingerprintはstitcherが自分で読んで適用した値から計算する（ADR-0026）。
8. profileに物理的な識別子と自由記述の欄を置かない。

### 所有者の確認が要る点

1. draftの定義: 校正の値（レンズ係数・写像・校正の記録）をdraftに入れてよいか。#271の受入基準の「draftなのに値が入っている」を「所有者が決める値が入っている」と読んだ。#277の受入基準と両立させるため。
2. 出力の向きをリグの決定（`HG-0002`・#43）に含めること。
3. 出力領域を用紙の内側に限ること。用紙の外に余白をつけた出力は2.0.0では作らない。
4. 画素数の丸めを切り上げにすること。A0横180 DPIで8426×5960となり、#42の候補と一致する。
5. 位置ずれとseamのpixelを「出力のpixel（`outputRaster.dpi`での）」と定めること。#42の候補「registration p95 0.5 px」がどちらのpixelかの確認を含む。
6. 重なりの下限を%からmm（文書座標）に変えること。
7. 1.1.0を試験専用とし、製品のadapterから外す時期を2.0.0の最初のapproved（#45）の後とすること。
8. 有効期限（`approval.validUntil`）と再校正のtriggerは#43・#45で決める。本ADRは欄だけを用意する。
9. `approval.decisionRef`に何を書くか（Issueのコメントの識別子など）。人名は書かない。

### 承認後の作業と範囲外

- 承認後の作業: 本ADRをAcceptedにし、決定日と承認者を記入する（2026-10-08に済み）。#274でC++と.NETのreader・実行時の検査・fingerprintを実装し、両方に同じ試験ベクトルを置く。#277・#273・#269を本ADRの規約に合わせる。`docs/M2_PRE_GATE.md`と`docs/ARCHITECTURE.md`のrig profileの記述を更新する。
- 範囲外: C++・.NETの実装（#274）、数値（DPI・閾値・レンズ係数）の決定、リグの決定（#43）、合成の段階とpilot（#272のADR草案）。
