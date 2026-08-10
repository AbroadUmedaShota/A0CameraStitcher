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
