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
- 追記（2026-10-14、ADR-0036・Issue #307）: ADR-0036で改定（Proposed）。ライブビューの3命令（開始・画像の取得・終了）だけをWPD経由で送る例外を提案している。撮影とカードに触る命令（`0x9207`を含む）の禁止は変えない。承認までは本ADRの文面が効く。

## ADR-0017: 標準WPD不成立後のone-shot hybrid評価

- 状態: Superseded by ADR-0019
- 決定: product ownerは、`WPD baseline + full close → Nikon SDK exactly-one capture to camera card + full close → WPD reopen, no capture command, recover exactly one new JPEG`をPhase 0の承認経路とした。
- 根拠: 電源再投入後も[run-1785908732670-1](evidence/phase0/run-1785908732670-1/report.md)で標準`0x100E`は`0x2002`、ObjectAdded/JPEG 0件、`FailedPartial`だった。query-only [run-1785908518274-1](evidence/phase0/run-1785908518274-1/report.md)の`0x9207`広告は、operationの意味・実行許可・成功の証拠ではない。
- 影響: vendor operationを実装・送信せず、SDK/WPD sessionを重複させない。hardware evidenceによりdatetime相関契約はADR-0019でRejectedとなり、captureは承認済みADR-0020のsingle-slot spool経路だけで評価する。
- 追記（2026-10-14、ADR-0036・Issue #307）: ADR-0036で改定（Proposed）。vendor operationを送らない点について、ライブビューの3命令だけを例外とする案がある。hybridの経路の判断は変えない。承認までは本ADRの文面が効く。

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
- 追記（2026-10-14、ADR-0036・Issue #307）: ADR-0036で改定（Proposed）。禁止するvendor operationのうち、ライブビューの3命令だけを例外とする案がある。`0x9207`とspoolの規則は変えない。承認までは本ADRの文面が効く。

## ADR-0021: 設置は自動補正可能範囲までとし固定profile＋上限付き補正を使う

- 状態: Accepted for design and software pre-gate; optical thresholds pending
- 決定: 操作者へpixel単位の完全な手動位置合わせを要求しない。承認済みrig profileの固定レンズ補正・planar warpを基準とし、撮影ごとの位置・回転・倍率・露出・色の差が承認済み上限内の場合だけ一時的に自動補正する。判定は`ready`、`ready-auto-correction`、`physical-adjustment-required`の三状態とする。
- 安全境界: 撮影ごとに自由なhomographyを再推定せず、補正結果でrig profileを自動学習・更新しない。判定器はprofileのstatus、対応schema版、provenance、校正日時、有効期限を構造化入力から検証する。全画角不足、重複不足、設定不整合、上限超過、draft・不整合・期限切れprofileは成功扱いにせず、物理調整または再キャリブレーションを案内する。
- 検証順: 実機不要の純粋判定・schema・synthetic contractを先行し、一台ではレンズ校正案内・設定読取・provenance記録を検証する。左右固定transform、seam、実写A0品質は二台と承認済みchartが揃うまで合格にしない。
- 未解決: 数値閾値、校正target、profile有効期限・再校正triggerは`HG-0001`と`HG-0002`で決定する。
- 追記（2026-11-04、ADR-0037・Issue #396・#397）: ADR-0037で改定（Accepted、2026-11-04）。撮影ごとの位置（平行移動）・回転の補正をやめ、設置ごとに固定した写像と縦一直線の継ぎ目で合成する。撮影ごとの推定はずれの診断とゲート（#385）の入力に使う。撮影ごとに変えるのは、継ぎ目に沿った明るさの比と、上限以内の倍率（ピントの合わせ直しや高さの違いによる左右の倍率の差。上限は所有者の決定で1%から始める。上限を超えたら補正せず不合格）だけ。縦一直線の継ぎ目の列は人がCの画面で決めて固定の条件に保存する（#403）。判定は`ready`と`physical-adjustment-required`の二状態にし、`ready-auto-correction`を廃止する。本文の書き換えは後の変更で行い、それまでは本追記とADR-0037が優先する。

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
- 追記（ADR-0038・Issue #416）: ADR-0038で改定（Accepted）。操作の「write操作は別承認まで追加しない」の「別承認」をADR-0038とし、`DualCamera`の本番の画面に、CAM-Aの露出の5項目をCAM-Bへ写す「写す」操作だけを足す（ADR-0038の段C）。`SingleCamera`の撮影設定はread-onlyのまま。段Cの前提条件が揃うまでは本文が効く。本文の書き換えは段Cを始める変更で行う。

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
- 追記（2026-10-14、ADR-0035・Issue #291）: bindingが`Ready`の間、割り当て済みのaliasについて、操作者が選んだ1台のLive Viewを開いてよい。候補選択の`start-candidate-live-view`は再開せず、別schema `a0.camera-agent.hardware-dual-bound-live-view.v1`で行う（既存schemaは変えない追加）。同時に開くSDK sourceとLive Viewは1つまでで、切り替えのたびに前のLive Viewの停止とSDK sourceのcloseを確認してから次を開く。操作者が1回押したときだけ動く「両方を更新」（CAM-A、CAM-Bの順に1 frameずつ取り直し、選んでいた側のLive Viewに戻す）は自動の繰り返しではない。撮影のtransaction IDを作る前に、すべてのLive ViewとSDK sourceが閉じたことを確認する。SDK errorと失効の事象はbindingを失効させ、開き直しも再試行もしない。previewの保存・公開・撮影入力への使用の禁止は変えない。

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
- 追記（2026-10-14、ADR-0035・Issue #291）: Moduleを残す目的に「`Ready`の後のbound Live Viewのsourceを開くこと」を足す。WPDをopenする前に全Live ViewとSDK sourceをcloseする規則、WPD open中にSDK operationを行わない規則は変えない。
- 追記（2026-10-14、ADR-0036・Issue #307）: ADR-0036で改定（Proposed）。WPD経由のライブビューの3命令の例外を提案している。SDKとWPDを同じカメラで重ねないこと、撮影の前にすべてのライブビュー（WPD・SDK）を閉じて確かめることは保つ。別々のカメラの間で重ねてよいかは決めておらず、それまでは本ADRの「WPD open中はSDK operationを行わない」が効く。

## ADR-0030: STEP4までの受入反復を最大5回へ変更する

- 決定日: 2026-09-21
- 根拠: 本人指示「STEP4までの対応を完了せてください。100回テストは多すぎるので多くても5回程度に収めてください。」
- 決定: 今回の完成範囲はソフトウェア安定化、一台実WPF受入、二台撮影受入、実写A0合成受入まで。新規の実機・実写反復試験は一つの計画された受入系列につき最大5回とし、最初のone-shotもこの5回に含める。Singleは最大5撮影、Dualは最大5組（各body最大5撮影、全体最大10シャッター）とする。成功までの補充、同じ系列を分割して回数を増やすこと、自動retryは禁止する。
- 置換範囲: ADR-0024/0025/0027/0028と旧計画の10回characterization・100回耐久・10回handoffを今回の最大5回へ置換する。既存100回内部coordinatorは過去契約の検証用として保持するが、製品UI/CLIには接続しない。過去の実機1/10/100成功記録やソフトウェアfixtureは改変しない。
- 判定: 五つの初回結果、実行回数、失敗数、全時間値・最大時間を記録する。p95を併記する場合も5標本の記述統計であり、長期信頼性や100回耐久と同等とは主張しない。最初の失敗・未確定・割当失効・時間不足で停止し、未実行は未実行と記録する。既存の性能承認は該当mode・条件だけに適用する。
- 不変条件: 原本保持、SDK/WPD非重複、CAM-A→CAM-B、exact-object cleanup、180秒watchdog、明示割当、no fallback、no retryを維持する。5回への変更はカメラ設定・カード整理・品質閾値・最終リグの承認ではない。
- 未確定時の記録: DualのHardwarePending・割当失効では成功系列の集約証跡を作らない。既存transaction/recovery記録を正本とし、5回の結果が揃ったとは扱わない。
- 実施条件: 対象PC・body・候補SHA/hash・操作・上限を対応付けた実機計画と現在の状態を確認する。本人指示により操作 PC-3は使用しない。現在PC（操作 PC-4）のOS検出はD810一台であり、SDK/WPD readiness・撮影は未実施。実写corpusとHG-0001/0002は未充足。STEP4完了は実写品質の証拠が揃ってから判定する。配布・release（M4）は今回の範囲外。
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
- 状態の追記（2026-10-13の所有者の決定、2026-10-14記録、Issue #291）: 二台のライブビューは、当面「選んだ側をLive、反対側はその最終frameの静止画」と「両方を更新」ボタンで進める。最終目標の二台とも動く映像は放棄せず、別Issue（#292）で追う。所有者が確かめたいのは、設置時の構図合わせ・ピント・原稿ごとの収まり・2台が生きていることの4つで、当面の構成で4つとも満たせる。実装前gate (1)〜(5)と2026-10-05の凍結は#292の範囲にだけかかる。#291は一module・一Live Viewのままで、上の「1 moduleで複数台は不可」の制限に触れない。詳細はADR-0035。
- 追記（2026-10-14、ADR-0036・Issue #307）: ADR-0036で改定（Proposed）。所有者の決定（2026-10-14、#306）に基づき、最終の操作者の画面を、承認済みrig profileで描く表示専用の合成Cにし、左右の二paneを技術担当者向けへ移す案。「保存した合成と表示しない」「同期と表示しない」は残す。承認までは本ADRの文面が効く。
- 追記（2026-11-04、ADR-0036決定1・Issue #396・#397）: ADR-0036決定1で改定（Accepted）。最終の操作者の画面は、承認済みのrig profileで描く表示専用の合成Cにする。左右の二paneは技術担当者向けへ移す（置き場所は※未決）。「保存した合成と表示しない」「同期と表示しない」は残す。Cは写像と縦一直線の継ぎ目の列を固定で描く（列は人がCの画面で決めて保存する、#403）。明るさはframeごとに継ぎ目に沿って合わせる。二台とも動く映像の実現方式（B案）は決定2で保留（Proposed）。本文の書き換えは後の変更で行う。承認済みのprofileが無い間は、操作者の画面は本文の形のまま。

## ADR-0032: 受入試行の定義・回数の数え方・USB抜去・保存の合格線を定める

- 状態: Accepted。所有者が2026-10-07に推奨案どおり承認した（末尾の決定欄）。
- 改訂: 2026-10-08に6-Aの表の「transaction作成前の機体照合の失効」の行を実装に合わせて改訂した（末尾の改訂欄）。2026-10-13に「照合の確定から5分」の誤りに訂正注記を足した（#294、末尾の改訂欄）。
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
  - 訂正（2026-10-13、#294）: 「照合の有効期限（確定から5分）」は誤り。照合には確定からの期限は無く、失効するのは事象のときだけ（FR-CAP-002、ADR-0025）。5分は撮影の主ボタンを押した時点で作る撮影開始スナップショットの期限。実機日の時間を決めるのはAgentの寿命（起動から10分）と撮影の受付の締切（起動から6分55秒、残り185秒が要る）。定義は[二台の撮影にかかわる時間の定義](HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md#二台の撮影にかかわる時間の定義)。推奨の6-Aは変えない。

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
    - 訂正（2026-10-13、#294）: ここの「照合の有効時間5分」は、主ボタンを押した時点から5分の撮影開始スナップショットを指す（上の実装の事実のとおり）。照合そのものには期限が無い。5組の実行が収まるべき時間は、このスナップショットの5分と、撮影の受付の締切（Agentの起動から6分55秒）の両方。定義は[二台の撮影にかかわる時間の定義](HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md#二台の撮影にかかわる時間の定義)。
- 改訂（2026-10-13、#294）: 決める事項6の推奨の理由にある「照合の有効期限（確定から5分）」を訂正した（同所の訂正注記）。照合には確定からの期限は無く、5分は主ボタンを押した時点で作る撮影開始スナップショットの期限。コードは2026-08-29の`39ab449`から押した時点で作っており、誤っていたのは文書。6-Aの表と推奨は変えない。

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
  - 訂正（2026-10-13、#294）: 「機体照合5分」は誤り。機体照合には期限が無い。5分は主ボタンを押した時点で作る撮影開始スナップショットの期限で、1組モードでは押すたびに作る。アプリで撮れる組数を決めるのは、Agentの寿命（起動から10分）と撮影の受付の締切（起動から6分55秒、残り185秒が要る）。定義は[二台の撮影にかかわる時間の定義](HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md#二台の撮影にかかわる時間の定義)。アプリ外の経路に切り替える条件の考え方は変えない。
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

追記（2026-11-04、ADR-0037・Issue #396・#397）: 不変条件2はADR-0037で改定（Accepted、2026-11-04）。撮影ごとの平行移動・回転の補正を合成に使わず、撮影ごとの補正は継ぎ目に沿った明るさの比と、承認された上限以内の倍率に限る。倍率の推定では回転を固定の値に縛り、補正の中心はBの画像の中心を固定の写像で出力へ移した点に置き、推定できない時は補正しない（所有者の決定（2026-11-04、#396））。本文の書き換えは後の変更で行い、それまでは本追記とADR-0037が優先する。

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

## ADR-0035: 照合がReadyの間、選んだ1台のLive Viewを開けるようにする

- 状態: Proposed。前半（binding host側）の実装は書き終えたが、ビルドも実機の確認もしていない（※未検証）。所有者の承認が要るかは※要確認。製品の方向は2026-10-13の所有者の決定で決まっており、要件はFR-LV-001（Dualで選んだ1台の継続Live View）がすでに許している。
- 決定日: 2026-10-14（案）
- 関連: Issue #291（前半）、#295（後半）、ADR-0025・0028・0031、`docs/HARDWARE_CAMERA_AGENT_DUAL_BOUND_LIVE_VIEW_V1.md`、`docs/OPERATOR_UI_SPEC.md`の「実機の二台構成のステージ」
- 決定: 照合（binding）が`Ready`の間、割り当て済みのaliasについて、操作者が選んだ1台のLive Viewを開けるようにする。Live Viewを受け持つ部品はAgentの中に1つだけ置き、binding hostから呼ぶ。schemaは`a0.camera-agent.hardware-dual-bound-live-view.v1`で、命令は`start-bound-live-view`、`get-bound-live-view-frame`、`stop-bound-live-view`の3つ。「両方を更新」の命令は作らず、WPFが3命令を組み合わせる。封筒の`marker`は受ける側のhostのもの（binding pipeは`HardwareBinding`）で、各hostは自分のmarkerの要求だけを受ける。既存のbinding v1・Dual v2の命令と形は変えない。
- 範囲: 前半（#291）はbinding host（`Ready`から`activate-capture`まで）と、WPFのステージ・撮影前に閉じる処理。撮影の後のLive View（capture host側の受付判定、`OnIdle`の接続、`activate-capture`後のWPDの振り分け、古い静止画の警告）は後半（#295）。前半だけが着地している間は、撮影の後の画面に「撮影後は機体照合のやり直しが必要です」を出す。
- 守る不変条件:
  1. SDKのsourceとLive Viewは、2つのhostを通して同時に1つまで。開いているかどうかの正はadapterの報告で、部品の記録から推測しない。
  2. 開けるのは、bindingが`Ready`で、予約・dispatch中・未確定のtransactionが無いときだけ（後半）。
  3. WPFはLive Viewが閉じたことを確かめてから撮影のtransaction IDを作る。確かめられなければ試行に数えずに止まる（ADR-0032の1-A）。何も開いたことがなければ、Agentへ何も送らない。
  4. WPDを開くのは、sourceもLive Viewも開いていないときだけ（ADR-0028のまま）。`activate-capture`は、開いているLive Viewがあれば`BoundLiveViewOpen`で拒否し、bindingは`Ready`のまま残す。
  5. SDKの例外と失効の事象はbindingを失効させる。開き直しも再試行もしない。復旧は照合のやり直しだけ。
  6. previewのframeは保存・公開せず、撮影や合成の入力にも使わない。
  7. 既存の命令と形、capabilitiesを変えない。Live Viewを使わない撮影では、Agentへ送る命令の列が今と同じ。
  8. Live ViewでAgentの寿命を延ばさない。keep-aliveは作らない。クライアントが黙った場合に備え、Agent側が一定時間で自分で止めて閉じる（`OnIdle`）。
- 時間の定義: 照合そのものに「確定から何分」という期限は無い。画面に出す時間は「撮影できる残り」で、Agentの起動時刻＋600秒−185秒（watchdog 180秒、応答3秒、後始末2秒）から導く。WPFが読むプロセスの起動時刻はnativeの時計より早いので、値は短めに出る（失敗側に倒れる）。Agentが応答に載せる`captureAdmissionRemainingMs`は、受け取ってからの経過を引いて、短い方を使う。起動時刻が分からないときは「残り時間を確認できません」と出し、切り替えと「両方を更新」を押せなくする。5分は、主ボタンを押したときにWPFが作る撮影開始snapshotの有効時間で、照合の期限ではない。
- 選ばなかった案: (B) 撮影の後にbinding hostへ戻す。受入済みの一方向のactivate契約と終了処理（#225・#228）を作り直すことになり、往復の途中でどちらのpipeが生きているかをWPFが取り違える。(C) 1組ごとにAgentを作り直す。capture hostは自然終了（最長10分）するまで次を起動できず、照合のやり直しとキャップの付け外しが組の数だけ増える。
- 影響: 受入済みの撮影経路に入る変更は、撮影前のstop（使っていなければ何も送らない）と`activate-capture`の確認だけ。1回の照合で撮れる組数はLive Viewを使うと3〜4組の見込み（※推定）。
- 実機で確かめる点（実機日の別系列、シャッターを切らないものと切るものがある）: Ready後のLive（A・B各60秒連続のframe率とSDKエラー頻度）、切り替えと「両方を更新」の所要時間、Live View後の撮影（撮影前に閉じる処理から1組）、Live中のUSB抜去での失効、Live中にアプリを閉じたときのstop。どれも※未検証。回数と系列は実機日の前に所有者の承認を取る。
- 範囲外: 二台とも動く映像（#292）、vendor operation、SDKの2重読み込み、HDMIの取り込み、previewを撮影や合成の入力にすること。
- 追記（2026-10-14、ADR-0036・Issue #307）: ADR-0036で改定（Proposed）。主画面を表示専用の合成Cにし、A・Bの表示を技術担当者向けへ移す案。不変条件6（previewを合成の入力にしない）は「保存の合成の入力にしない」に絞る案で、Cは保存せず、撮影・保存の合成の入力にもしない。範囲外の「二台とも動く映像」「vendor operation」はB案（ライブビューの3命令の例外）で改定する案。承認までは本ADRの文面が効く。
- 追記（2026-11-04、ADR-0036決定1・Issue #396・#397）: ADR-0036決定1で改定（Accepted）。主画面を表示専用の合成Cにし、A・Bの表示を技術担当者向けへ移す。不変条件6は「保存の合成の入力にしない」に絞る。Cは保存せず、撮影・保存の合成・補正の推定・profileの入力にしない。範囲外の「二台とも動く映像」「vendor operation」は決定2で保留（Proposed）のまま、本ADRの文面が効く。本文の書き換えは後の変更で行う。承認済みのprofileが無い間は、主画面は本文の形のまま。

## ADR-0036: ライブビューを承認済みrig profileで描く表示専用の合成Cにし、WPD経由のライブビュー3命令をvendor operationの例外にする

- 状態: 決定1 Accepted（2026-11-04、#396）／決定2 Proposed（保留。所有者によるNikon SDK使用許諾の読み直しが前提）。決定1の実装は、#398・#399が関数と記録、#400（JPEG）と#401（開発用ビルドのC）は経路に接続済み、#402・#403は未実装。承認済みのrig profileも無いので、操作者の画面にCはまだ出ない（開発用ビルドのCだけが条件で描く）。決定2に関わる改定の対象は、今の文面がそのまま効く。AGENTS.mdは人が書き換えるので、本ADRでは本文を変えない。
- 起票日: 2026-10-14
- 決定日: 決定1 2026-11-04（#396）。決定2は未決定
- 関連: #307（本ADR）、#306（所有者の決定と技術の道筋）、#396（2026-11-04の所有者の決定）、#397〜#403（決定1の文書と実装。#403はCの画面で継ぎ目の列を決めて保存する操作）、ADR-0037（撮影ごとの位置・回転の補正をやめる。Accepted）、#292（B案）、#293（今のスコープ）、#291・#295（ADR-0035）、#280・#281・#282（合成のレンダラー・reader・製品の入口）、#274・#277（profileの選択と校正）、#46（撮影ごとの補正）、#43・#42・#45（リグ・品質基準・承認済みprofile）、#233（pilot）、`HG-0010`・`HG-0011`
- 背景: ADR-0031は、CAM-Aを左・CAM-Bを右に置く二paneを最終の操作者の画面とし、二paneを合成済みと見せないこと、リアルタイム合成を範囲外とすることを決めた。ADR-0035（#291）は、当面の形として選んだ1台をLive、反対側を静止画にした。所有者は2026-10-14に、操作者の画面ではA・Bの区別を見せず、2台の映像を合成した1枚（以下C）を出すこと、Cの両側を動かすためにB案（WPD経由でPTPのライブビュー命令を送る、#292）を今のスコープに入れることを決めた（#306）。B案は、vendor operationを送らないと定めたADR-0016・0017・0020・0028と`CON-003`に当たるため、例外を明文にする必要がある。

### ADR-0036の所有者の決定（2026-10-14、#306）

所有者が決めたのは次の6点。下の「決定1」「決定2」「不変条件」は、これを規則の形に落とした案で、案の部分は承認を要する。

1. 時期: 今のスコープ（#293）に入れる。進め方は、先に合成の本体を作り、それを映像に反映する。
2. Cの動き: 両側とも動くC。B案（#292）を今のスコープに入れる。
3. 見た目: 保存される合成と同じにする。位置・継ぎ目（幾何）に加えて、明るさと色も合わせる（撮影ごとの補正#46をCにも当てる）。
4. A・Bのカメラごとの表示: 技術担当者向けに残す（設置・ピント合わせ・不具合の切り分け）。操作者の通常の画面はCだけ。
5. 校正（#277）の基準点の検出: OpenCVを開発ツールだけで使い、製品には同梱しない。製品への同梱の判断（`HG-0005`）は別。
6. pilotの間、ライブビューのframeを操作PC-1の中に記録してよい。公開リポジトリとIssueには載せない。

### ADR-0036の所有者の決定（2026-11-04、#396）

所有者は2026-11-04に#396で次を決めた。

- 決定1だけを承認する。決定2は保留する。保留の理由は、決定2の前提条件（決定2の3）であるNikon SDK使用許諾の読み直しがまだ済んでいないため。決定2の本文は変えず、Proposedのまま残す。
- ライブのCは、書き出すJPEGの合成に合わせる。見え方を同じにし、撮影ごとに合成点（幾何と継ぎ目の位置）を変えない。幾何と継ぎ目は固定で合成する（Q1〜Q4の前文）。
- Q1 明るさ: 撮影ごとに合わせてよい。今のJPEG（#368）と同じく継ぎ目に沿ってBに比を掛ける方法で、Cでも同じ計算をする。合成の位置は動かさない。
- Q2 #362: 「設置が変わったら校正をやり直して、固定の位置を更新する」と読み替える。重なりの自動推定（#364・#365）は、ずれの検出と再校正の案内にだけ使い、合成の位置には使わない。撮影ごとの位置・回転・倍率の補正をやめることはADR-0021の改定になるので、ADR-0037に分ける（2026-11-04に確認点B7の修正つきでAccepted。倍率だけは上限以内で撮影ごとに補正する）。
- Q4 継ぎ目（同日に2回改定。下は最後の決定）: 縦にまっすぐな1本にする。列は基本固定で、設置ごとの固定の合成条件（#399）に持たせ、すべての撮影で同じ列を使う。撮影ごとに自動で列を選ぶことはしない。列は人が決め、Cの画面で継ぎ目の線をマウスでドラッグして、重なりの帯の余裕の内側で左右に動かす。CとJPEGは同じ列を使い、Cで動かした列がその後の撮影のJPEGに使われる。選んだ列は報告に記録し、検品の時に一目で分かるようにする（StillReviewの継ぎ目の表示など）。行ごとに曲がる道筋（#368のseam-cutの形）は使わない。写像（幾何）を撮影ごとに変えないことは変わらない。操作と保存は#403で設計し、所有者に見せてから実装する。
  - 経緯: 最初の決定は「列は校正の時にずれが最も小さかった位置で決め、校正データに持たせる」。1回目の改定は「撮影ごとに帯の余裕の中で最も良い列を探してよい」。どちらも最後の決定で置き換わった。
  - 所有者の※仮定（再開後に設計で確かめる）: 列をドラッグする場所はライブのCの画面。保存済みの結果（StillReview）で列を変えて作り直すのは、親を残した別保存として扱う。自動で良い列を探す処理は、作るとしても候補の表示だけにする。この※仮定は、下の確認点Aの回答で設計の案とともに承認された。
- 確認点A・B・Cの回答（2026-11-04、#396）: 所有者はB7だけを取り上げて直し、ほかは案のとおり承認した（※ほかは案のとおりと読んだ）。本ADRに関わるのは次のとおり。
  - A（#403の設計の案1〜7）: 承認。[設計](design/fixed-composite.md)の5節に決定として書いた。
  - C1: Cの画面で人が動かした列を固定の合成条件に保存することは、決定1の5（Cを補正の推定やprofileの入力にしない）に当たらない。列はライブビューの画素から推定した値ではなく、人の操作で決めた値だから。決定1の10に書いた。
  - C2: 製品の経路で継ぎ目の列を承認済みのrig profileに持たせるかは、#45の時に決める。
  - C3: 「承認していただく点」の7（未決は別のIssueで決める）を、決定1の範囲でも承認済みとして扱う。
  - C4: AGENTS.mdの差し替えの案のうち、決定2に関わる文は、決定2の判断の時に今のAGENTS.mdに合わせて作り直す。
  - B7（ADR-0037）: 書き出すJPEGだけ、上限以内の倍率を撮影ごとに補正する。Cは撮影の前に倍率を知らないので固定の写像で描き、CとJPEGの幾何の違いは上限以内の倍率の差だけになる。決定1の10に書いた。
- 決定1の2・3は、この決定に合わせて2026-11-04に書き直し、継ぎ目の10を足した。元の文面（撮影ごとの補正#46をCにも当てる）はgitの履歴に残る。設計は[固定の合成条件と縦一直線の継ぎ目による合成](design/fixed-composite.md)。

### 決定1: 表示専用の合成C（Accepted、2026-11-04、#396）

1. 製品の経路では、Cは承認済み（approved）のrig profileだけで描く。draft、期限切れ、fingerprintが一致しないprofileでは描かない。開発経路は2の副項のとおり。
2. Cの写像は、approved profileの写像をライブビューの画素へ縮めたものとする（`K_lv = S·K`、`H_lv = S·H`、歪み係数はそのまま）。描画は保存の合成と同じレンダラー（#280）を通す。`S`はライブビューの画素と撮影画像の画素の対応（拡縮と平行移動）で、測定で決めた固定の値とし、frameごとに推定しない。`S`の値と置き場所（機種の定数か、profileの欄か）は測定の後に別に承認する。
   - 承認済みのprofileが出るまでの開発経路では、設置ごとに一度作る「固定の合成条件（未承認）」（#399）を、同じように`S`でライブビューの画素へ縮めて使う。この条件は承認済みのprofileではなく、approvedを名乗らない。描くCは表示専用で、保存しない（5のとおり）。画面には未校正・未承認であることと、`S`が※未検証の仮定であること（#309）を出す。
   - この開発経路は、ADR-0033の4節の評価専用の入口と同じ扱いにする。既定OFFのCMake option（今は`A0_BUILD_WPD_DUAL_PREVIEW`）を付けたビルドだけに入れ、製品の配布物に入れない。製品の合成がapprovedのprofileだけを受け付ける規則（ADR-0033の4節、ADR-0034の不変条件1）は変えない。7の「承認済みのprofileが無い間は操作者の画面にCを出さない」もそのまま効く。
3. 明るさは保存の合成に合わせる。継ぎ目に沿った窓でAとBの輝度の比を求め、行方向にならしてBに掛ける（#368と同じ計算。#396のQ1）。JPEGでは撮影ごと、Cではframeごとに比を求める。比を求めても合成の位置（写像と継ぎ目の列）は動かさない。色の補正（#46のチャンネルごとの補正）は本ADRの範囲外のまま（※未決）。どの方法でも、ライブビューのframeから自由なhomographyを推定しない。
4. 「保存される合成と同じ見た目」と言えるのは、ライブビューが撮影画像の範囲を拡縮と平行移動だけで写していることを確かめた後とする。確かめ方は、ライセンス下の資料を読む（リポジトリには要旨だけを書く）、実機でシャッターを切らずに測る、pilotで原画像と比べる、の3段。確かめられなければCを出さない。拡大表示中など、撮影範囲の全体を写していないframeでは描かない。
5. Cは表示専用とする。保存しない。画素をログ・journalに書かない。撮影、保存の合成、書き出し、補正の推定、profileの選択の入力にしない。Cの画像は、保存と書き出しのAPIが受け付けない型で持つ。
6. A・Bのカメラごとの表示は技術担当者向けに残す。操作者の通常の画面はCだけで、CAM-A・CAM-Bの名前を出さない。技術担当者向けの表示を置く場所（保守画面か、表示の切り替えか）は`docs/OPERATOR_UI_SPEC.md`の改定で決める（※未決）。
7. 承認済みのprofileが無い間は、操作者の画面にCを出さず、出せない理由を表示する。draftのprofileでCを描くことは、AGENTS.md（承認済みの固定rig profileを使う）、ADR-0033の4節、ADR-0034の不変条件1に反するため、本ADRでは認めない。#306の依存の順序5にある「開発用の画面で下書きのCを所有者に見せる」は、ADR-0033の4節の評価専用の入口の範囲で行い、製品の配布物に入れない（※この読み方は所有者の確認事項）。
8. 片側のframeが全く無い時は描かない。片側が最終frameの静止画のまま描くCの扱い（B案が成り立つまで、ADR-0035の形のframeで描く場合）は`HG-0011`に含める。画面に「同時」「同期」と書かず、2台のframeの時刻が違うことを常設の注記で伝える。
9. 計算は、保存の合成と同じC++を小さなDLLにしてWPFから呼ぶ案を採る（#306の技術の道筋）。1枚35〜65 ms、20 MB未満の見込み（※推定。実装の前に操作PC-1で計測する）。描画はUIスレッドの外で行い、最新のframeだけを描く。
10. 継ぎ目は縦にまっすぐな1本で、出力の画素の1列に置く（#396のQ4の最後の決定）。行ごとに曲がる道筋は使わない。列は写像と一緒に固定の条件（開発経路では固定の合成条件#399。製品経路で承認済みのprofileに持たせるかは#45の時に決める）に持たせ、すべての撮影とframeで同じ列を使う。列を動かすのは人だけで、Cの画面で継ぎ目の線をドラッグし、帯の余裕の内側で左右に動かす。「この列で固定」を押した時だけ、固定の合成条件の新しい改訂として保存し、以後のJPEGとCの両方に使う。開発経路の初期値は帯の中央（※仮）。使った列とその出どころは報告に記録し、検品の時に一目で分かるように示す。CとJPEGは、列を入力に取る同じ合成の関数（#398）を通り、Cの列は原寸の列を`S`で縮めたもの（保存するのは原寸の列を丸めた値）。操作と保存の細部は[設計](design/fixed-composite.md)の5節（#403）。
    - 人が動かした列の保存は、5（Cを補正の推定やprofileの入力にしない）に当たらない（C1、2026-11-04）。列はライブビューの画素から推定した値ではなく、人の操作で決めた値だから。Cの画素は保存せず、保存するのは列の値だけ。
    - 写像と列は撮影やframeの内容で動かさない。例外はADR-0037の決定6で、書き出すJPEGだけ、左右の倍率の差が上限以内なら撮影ごとに倍率を当てる。Cは撮影の前に倍率を知らないので、固定の写像のまま描く。CとJPEGの幾何の違いは、`S`の縮尺を除くと、上限以内の倍率の差だけになる。

### 決定2: B案 — WPD経由のライブビュー3命令の例外（Proposed、保留）

保留（2026-11-04、#396）: 所有者は決定2を承認していない。前提条件の3（Nikon SDK使用許諾の読み直し）が済んでいないため。以下の本文は2026-10-14の案のまま変えていない。読み直しが済むまで、B案の実装と実機の試作に着手しない。

追記（ADR-0038・Issue #416）: 下の2の「カメラの設定を書き換える命令…も禁止のまま」は変えない。ADR-0038（Accepted）のCAM-Bへの露出の5項目の書き込みは、標準のPTPのSetDevicePropValue（`0x1016`）を専用のgateで送るもので、vendor operationではなく、ライブビューの例外を広げない。ADR-0038の例外はADR-0038だけで扱う。

1. vendor operationのうち、ライブビューの3命令（開始・画像の取得・終了）だけを、WPD経由で送ってよい例外にする。対象はD810の写真ライブビュー。
2. 撮影とカードに触る命令（`0x9207`を含む）は引き続き禁止する。カメラの設定を書き換える命令と、3命令以外のvendor operationも禁止のまま。3命令のどれかの前提として、設定の書き込みや別のvendor operationが要ると分かった場合は、例外を広げずにそこで止め、改めて判断を仰ぐ。
3. 前提条件: 所有者がNikon SDKの使用許諾を読み直し、次の2点を確かめる（#292、所有者の作業）。(a) 同梱のMTP仕様書を実装に使ってよい範囲、(b) 命令の値と手順を公開リポジトリに書いてよいか。確認が済むまで、B案の実装と実機の試作に着手しない。資料は転載せず、参照は節の番号と要旨にとどめる。
4. 実機の試作の前に、試作の対象（機体と系列）、回数、中止の条件を別に承認する（ADR-0031の「対象・回数・操作を明記した別承認」と同じ扱い）。本ADRの承認は試作の承認を含まない。
5. B案で得るframeが、SDK経由のframeと同じ範囲を写すかは、試作で確かめ直す（決定1の4）。
6. B案が実機で成り立たなかった場合の扱いは決まっていない（※未決、所有者の確認事項）。それまでの二台のライブビューはADR-0035の形のまま。

### 守る不変条件

決定1の承認（2026-11-04）で、4〜6はAcceptedとして効く。1〜3は既存のADR（0006・0028・0032・0035）の規則を写したもので、そのまま効く。1〜3のうち決定2の例外（WPD経由のライブビュー）に関わる部分は、決定2と一緒にProposedのまま。4の「撮影ごとの」補正をさらに狭め、撮影ごとの位置・回転の補正もやめる（倍率だけは上限以内で書き出すJPEGに当てる）改定はADR-0037（Accepted、2026-11-04）にある。4の本文の書き換えは後の変更で行う。

1. SDKとWPDのsessionを同じカメラで重ねない。別々のカメラの間でSDKとWPDを同時に開いてよいかは本ADRでは決めない（※未決）。決めるまでは、ADR-0028の「WPD open中はSDK operationを行わない」をそのまま守る。
2. 撮影の前に、すべてのライブビュー（WPD経由とSDK経由）を閉じ、閉じたことを確かめる。確かめられなければ試行に数えず、撮影を始めない（ADR-0035の不変条件3、ADR-0032の1-A）。
3. no retry。ライブビューの失敗で自動の開き直しをしない。原本を保持する（ADR-0006・0018・0020）。
4. 撮影ごとのhomographyの推定はしない（ADR-0021を維持）。Cでもframeごとに推定しない。profileを実行時に変更・学習しない。
5. ライブビューのframeとCを保存・公開しない。例外は所有者が認めたpilotの間の記録だけで、操作PC-1の中に限り、公開リポジトリとIssueに載せない。この記録を有効にする手段（既定OFFの開発用の設定など）とholdoutに使わないことは、承認後にNFR-SEC-001の改定とあわせて決める（※案）。
6. 画面に「同時」「同期」と書かない。シャッターの同期を保証しない（`CON-002`）。

### 改定の対象と、承認後の差し替えの案

本ADRがProposedの間は、各対象に「ADR-0036で改定（Proposed）」の追記だけを置き、本文は変えない。承認後に次の方向で本文を書き換える。

2026-11-04（#396・#397）: 決定1の承認を受け、決定1に関わる行を「決定1承認により改定」とした。この変更では各対象の本文を書き換えず、既存の「ADR-0036で改定（Proposed）」の追記の後ろに「決定1で改定（Accepted）」の追記を足した。本文の書き換えは、技術担当者向けの表示の置き場所（決定1の6、※未決）と`.autodev`の要件の正規化をあわせて行う時に回す（承認後の作業）。決定2に関わる行と部分はProposedのまま。

| 対象 | 今の文面の要旨 | 承認後の改定の方向 | 扱い（2026-11-04） |
|---|---|---|---|
| ADR-0016 | vendor operationは送らない | ライブビューの3命令だけを例外とし、`0x9207`の禁止は残す | 決定2。Proposedのまま |
| ADR-0017（Superseded） | vendor operationを実装・送信しない | 同上。hybridの経路の判断は変えない | 決定2。Proposedのまま |
| ADR-0020 | `0x9207`を含むvendor operationを禁止 | ライブビューの3命令だけを例外とする。spoolの規則は変えない | 決定2。Proposedのまま |
| ADR-0028 | WPD open中はSDK operationを行わない。vendor operation禁止 | ライブビューの3命令の例外を足す。別カメラ間の重なりは不変条件1のとおり未決 | 決定2。Proposedのまま |
| ADR-0031 | 最終の操作者の画面は二pane。合成済みと表示しない。リアルタイム合成は範囲外 | 最終の操作者の画面をCにし、二paneは技術担当者向けへ移す。「保存した合成と表示しない」「同期と表示しない」は残す | 決定1承認により改定（Accepted）。追記を更新、本文は未改定 |
| ADR-0035 | 主画面はA・Bの固定配置。previewを合成の入力にしない（不変条件6） | 主画面をCにし、A・Bは技術担当者向けへ。不変条件6を「保存の合成の入力にしない」に絞る | 決定1承認により改定（Accepted）。範囲外の「二台とも動く映像」「vendor operation」は決定2でProposedのまま。追記を更新、本文は未改定 |
| `FR-LV-001` | プレビュー画像を合成入力に使用しない | 対象を「保存する合成の入力」に絞り、表示専用のCを除く | 決定1承認により改定（Accepted）。追記を更新、本文は未改定 |
| `FR-LV-003` | 実現方式は未決 | B案を今のスコープとし（所有者の決定）、操作者の画面はCにする | 操作者の画面をCにする部分は決定1承認により改定（Accepted）。B案の部分は決定2でProposedのまま。追記を更新 |
| `FR-LV-004` | 当面の形（選んだ側がLive、反対側は静止画） | B案が成り立つまでのCのframeの出どころとして残す。自動の交互表示はしないを維持 | Cのframeの出どころとして残す部分は決定1（8）に当たりAccepted。B案が成り立つかは決定2でProposedのまま。追記を更新 |
| `CON-003` | vendor operation（`0x9207`を含む）は禁止 | ライブビューの3命令だけを例外とする | 決定2。Proposedのまま |
| `docs/OPERATOR_UI_SPEC.md` | ステージ、実機の二台構成のステージ、常時禁止 | 主画面をC、A・Bを技術担当者向けにする。半分を「左側／右側」など位置の言葉で呼び、古い半分は画素を変えない重ね表示で示す | 主画面をCにし合成入力を「保存する合成の入力」に絞る部分は決定1承認により改定（Accepted）。vendor operationの部分は決定2でProposedのまま。追記を更新、本文は未改定 |
| AGENTS.md | 下の2文 | 下の差し替えの案（承認後に人が変える） | 下の注記のとおり、今のAGENTS.mdに1文目は無い。案は作り直しが要る（人が書き換える） |
| `HG-0011` | 静止画が古い時の扱い | 片側が静止画のCを含める | 決定1（8）に当たる。問いの更新は未着手 |

承認後に確かめる対象（本ADRでは追記を置かない）: `FR-UI-003`、`NFR-SEC-001`（pilotの記録）、PRODUCT_REQUIREMENTSの「MVP対象外」のLive Viewプレビューの行、`.autodev/requirements/normalized.json`、`HG-0010`、`docs/HARDWARE_CAMERA_AGENT_DUAL_BOUND_LIVE_VIEW_V1.md`（全体を写すframeかを示す欄）。

AGENTS.mdの差し替えの案（承認までは今の文言が効く）:

- 今: `One selected-camera Live View is in MVP; simultaneous two-camera Live View, preview-as-original/preview-as-stitch-input, RAW/NEF, GPU acceleration, and hardware shutter synchronization are deferred.`
- 案: `One selected-camera Live View is in MVP. A display-only composite Live View may render Live View frames through an approved rig profile only (ADR-0036); it is never saved and never used as an original, stitch input, correction estimate, or profile input. Two-camera Live View through the WPD Live View exception follows ADR-0036. Preview-as-original/preview-as-stitch-input, RAW/NEF, GPU acceleration, and hardware shutter synchronization are deferred.`
- 今: ``Never send vendor operation `0x9207` or retry the transaction.``
- 案: ``The only permitted vendor operations are the three Live View operations (start, get frame, end) over WPD under ADR-0036. Never send any capture- or card-related vendor operation, including `0x9207`, and never retry the transaction.``

注記（2026-11-04、#397）: 1文目の「今」の文（`One selected-camera Live View is in MVP; ...`）は、2026-11-04のAGENTS.mdには無い。開発用の二台のライブビューを認めた文（`Start with FX JPEG Fine L. The user's instruction authorizes a development-only WPD two-camera simultaneous Live View preview ...`）と、その後の開発用の指示の文に置き換わっている。2文目の`Never send vendor operation `0x9207` or retry the transaction.`は今もある。

- 1文目の案は、決定2の判断の時に、今のAGENTS.mdの文面に合わせて作り直す（確認点C4で承認、2026-11-04）。
- 決定1に関わる部分（表示専用のCを承認済みのprofileか固定の合成条件だけで描き、保存しない）は、撮影ごとの補正の文と同じ所に関わるので、ADR-0037の差し替えの案にまとめた。AGENTS.mdは人が書き換える。

### 範囲外と未決

- B案の実現性: 実機で確かめていない。工数は10〜20人日の見込み（※推定、#292の机上検討）。
- 承認済みのprofileの時期: #43（最終のリグ）と#42の一部の後、#45で決まる。Cが操作者の画面に出る時期はこれに依存する。
- Cに色の補正を当てる方法（#46）。明るさは決定1の3で決めた（2026-11-04）。
- `S`の値、機種の定数かprofileの欄か、2台で同じ値か。
- 技術担当者向けの表示の置き場所。
- 別々のカメラの間でSDKとWPDを同時に開いてよいか。
- B案が成り立たなかった場合の扱い。
- 本ADRの決定が`HG-0010`（二台とも動くライブビューをMVPに含めるか、実現方式）の回答に当たるか。
- HDMIの取り込みの案と、SDKを2つ読み込む案（ADR-0031の凍結）は本ADRで扱わない。

### ADR-0036で所有者に承認していただく点

1. 決定1（表示専用の合成C）の1〜9。特に、承認済みのprofileが出るまで操作者の画面にCを出さないこと（7）と、開発用の画面で下書きのCを見せる手順をADR-0033の4節の範囲で行う読み方（7）。
2. 決定2（ライブビューの3命令の例外）の1〜6。例外の範囲を3命令に限り、撮影とカードに触る命令（`0x9207`を含む）を禁止のまま残すこと。
3. B案の前提条件として、使用許諾の確認（所有者の作業）を置くこと。
4. 実機の試作の前に、対象・回数・中止の条件を別に承認する手順。
5. 守る不変条件の1〜6。特に、別カメラ間のSDKとWPDの重なりを未決のまま残し、ADR-0028の規則を守ること（1）。
6. 改定の対象の表と、AGENTS.mdの差し替えの案。
7. 未決の項目を、本ADRの承認の後に別のIssueで決めること。

回答（2026-11-04、#396）: 1は承認（2・3は#396の決定で書き直した）。2〜4は保留（決定2）。5は決定1にかかる4〜6が承認、決定2の例外に関わる部分は保留。6は決定1に関わる行が承認、AGENTS.mdは人が書き換える。7は承認の範囲の未決について同じ扱いとする（2026-11-04の確認点C3で、案のとおり承認と読んだ。※読み方）。

### 承認後の作業

- 状態をAcceptedにし、決定日を記入する。決定1は2026-11-04に済み。決定2は保留。
- 改定の対象の本文を、表の方向で書き換える。AGENTS.mdは人が差し替える。決定1に関わる対象は、2026-11-04（#397）に追記だけを「決定1で改定（Accepted）」に更新した。本文の書き換えは残っている。
- `HG-0011`の問いにCを含めて回答を求める。`HG-0010`との関係を所有者に確かめる。
- `.autodev`の要件と計画を正規化し直す。

## ADR-0037: 撮影ごとの位置・回転の補正をやめ、設置ごとに固定した写像と縦一直線の継ぎ目で合成する（倍率だけは上限以内で撮影ごとに補正する）

- 状態: Accepted（2026-11-04、#396。B7は所有者の修正のとおり。倍率の上限は所有者の決定で1%から始める（実機で測った後に見直す前提）。倍率の補正の細部3点も同日に所有者が決定。明るさの比の上限値は未決）。所有者は確認点B7だけを取り上げて直したので、ほかの確認点は案のとおり承認と読んだ（※読み方）。実装は#398・#399が関数と記録まで。経路への接続（#400〜#403）は未実装。ADR-0021の決定と安全境界、ADR-0034の不変条件2、ADR-0036の不変条件4の本文の書き換えは残っており（承認後の作業）、それまでは各対象の追記が本ADRへの改定を示す。AGENTS.mdは人が書き換えるので本文を変えず、下の差し替えの案を人が反映する。
- 起票日: 2026-11-04
- 決定日: 2026-11-04（#396の所有者の回答）
- 関連: #396（所有者の決定Q1〜Q4と確認点A・B・Cの回答）、#397（本ADR）、#398（縦一直線の継ぎ目の合成関数。列を入力に取る）、#399（固定の合成条件）、#400（JPEGの経路）、#401（Cの経路）、#402（自動推定を診断と再校正の案内に）、#403（Cの画面で継ぎ目の線をドラッグして列を決め、固定の合成条件に保存する）、#385（継ぎ目のズレのゲート）、#394・WP-04（ゲートを開発経路へつなぐ前の修正と接続）、#362・#364・#365・#368、#374（平置きの校正）、#45（承認済みprofile）、#46（撮影ごとの補正）、ADR-0021・0033・0034・0036
- 設計: [固定の合成条件と縦一直線の継ぎ目による合成](design/fixed-composite.md)
- 背景: ADR-0021は、承認済みrig profileの固定の写像を基準にし、撮影ごとの位置・回転・倍率・露出・色の差を承認済みの上限の内側で一時的に補正すると決めた。開発経路はこれに沿って、両台撮影のたびにJPEGで相似を推定して合成の幾何に当て（#365）、継ぎ目の道筋を画像の内容で選んでいる（#368）。ライブのCは自動位置合わせv2（#364）か手動の値で描き、帯の全体を線形featherで混ぜる。このため、CとJPEGで合成点が違い、撮影ごとにも合成点が変わる。所有者は2026-11-04に、Cを書き出すJPEGに合わせ、撮影ごとに合成点（幾何と継ぎ目の位置）を変えず、幾何と継ぎ目を固定で合成すると決めた（#396）。継ぎ目の列は同じ日に2回改められ、最後の決定では、縦一直線の列を人がCの画面で決めて固定の合成条件に保存し、すべての撮影で同じ列を使う。確認点の回答（B7）で、ピントの合わせ直しと高さの違いによる倍率の差だけは、上限以内なら撮影ごとに補正することになった。撮影ごとの位置・回転の補正をやめることは、ADR-0021の決定と、ADR-0034の不変条件2（撮影ごとの補正は承認された上限の内側の相似変換に限る）の改定になる。

### ADR-0037の元になった所有者の決定（2026-11-04、#396）

- ライブのCは書き出すJPEGの合成に合わせ、見え方を同じにする。撮影ごとに合成点を変えない。幾何と継ぎ目は固定で合成する。
- Q1 明るさ: 撮影ごとに合わせてよい。継ぎ目に沿ってBに比を掛ける今の方法で、Cでも同じ計算をする。合成の位置は動かさない。
- Q2 #362: 「設置が変わったら校正をやり直して、固定の位置を更新する」と読み替える。重なりの自動推定（#364の推定器など）は、ずれの検出と再校正の案内にだけ使い、合成の位置には使わない。
- Q4 継ぎ目（同日に2回改定。下は最後の決定）: 縦にまっすぐな1本。列は基本固定で、設置ごとの固定の合成条件（#399）に持たせ、すべての撮影で同じ列を使う。撮影ごとに自動で列を選ぶことはしない。列は人が決め、Cの画面で継ぎ目の線をマウスでドラッグして、重なりの帯の余裕の内側で左右に動かす。CとJPEGは同じ列を使い、Cで動かした列がその後の撮影のJPEGに使われる。選んだ列は報告に記録し、検品の時に一目で分かるようにする。行ごとに曲がる道筋（#368のseam-cutの形）は使わない。写像（幾何）を撮影ごとに変えないことは変わらない。
  - 経緯: 最初の決定は「列は校正の時にずれが最も小さかった位置で決め、校正データに持たせる」。1回目の改定は「撮影ごとに帯の余裕の中で最も良い列を探してよい」。どちらも最後の決定で置き換わった。
- #368の内容で道筋を選ぶseam-cutと、#365の撮影ごとの相似の補正は、書き出す合成の幾何から外れる。診断とゲートの入力としては残す。
- 確認点A・B・Cの回答（2026-11-04）: B7以外は案のとおり承認。B2は二状態にする（`ready-auto-correction`を廃止）。B5は明るさの比に上限を置く（値は実機で測ってから）。C1は「人の操作で決めた列の保存はADR-0036決定1の5に当たらない」と読む。
- B7の決定: ピントは撮影のたびに合わせ直してよく、設置の時に固定する運用にはしない。ピントの合わせ直しや高さの違いで左右のカメラの写る大きさ（倍率）に差が出た時は、差が上限以内なら撮影ごとに補正する。上限を超えたら補正せず、ゲートで不合格にして「ピントか高さを確かめてください」と案内する。回転と平行移動は撮影ごとに直さない。継ぎ目の列は変えない。
- B7の上限（2026-11-04、#396）: 倍率の補正の上限は1%で始める。左右の倍率の差が1%以内なら撮影ごとに補正し、超えたら補正せずゲートで不合格にして案内する。値は実機で測った後に見直す。
- B7の細部（2026-11-04、#396、所有者の言葉「推奨どおりで」）: (1) 回転は固定の値に縛り、撮影ごとに変えるのは倍率だけ。(2) 倍率の補正の中心は継ぎ目の列の上に置き、継ぎ目の線を動かさない（同日の後の決定で置き換わった。決定6 の中心の項）。(3) 倍率を推定できない時は補正しない。固定の写像のまま合成し、ゲートの判定に任せる。

### 決定

1. **固定の写像で合成する**: 書き出す合成の幾何は、設置ごとに固定した写像で決める。製品経路では承認済みrig profileの写像（ADR-0034）。承認済みのprofileが出るまでの開発経路では、設置ごとに一度作る「固定の合成条件（未承認）」（#399）。どちらも撮影ごとに作り直さない。撮影ごとの位置（平行移動）と回転の補正は合成の幾何に当てない。例外は決定6の倍率だけ。
2. **撮影ごとの推定は診断に限る**: 撮影JPEGでの相似の推定（#365）、ライブの自動位置合わせv2（#364）、ゲート（#385）の測定は、固定の写像からのずれを測って報告に残し、ゲートの入力と再校正の案内（#402）にだけ使う。例外は決定6の倍率の補正。推定の方法は#365の推定器の倍率の部分を使う案で、#400の実装に合わせて書く（※案）。合成の写像、継ぎ目の列、固定の合成条件、profileには書き戻さない。
3. **明るさは撮影ごとに合わせる**: 継ぎ目に沿った窓でAとBの輝度の比を行ごとに求め、行方向にならしてBに掛ける（#368と同じ計算）。JPEGは撮影ごと、Cはframeごとに求める。比を求めても合成の位置は動かさない。比には上限を置く（B5）。上限の値は実機で測ってから決める（※未決）。上限を超えた時の扱いも値と一緒に決める（※未決）。色の補正（#46）は本ADRの範囲外（※未決）。
4. **継ぎ目は縦一直線の1本**: 継ぎ目は出力の画素の1列で、すべての行で同じ列に置く。行ごとに曲がる道筋（#368の`RenderPairSeamCut`の形）は書き出す合成にもCにも使わない。列は固定の合成条件（#399）に持たせ、すべての撮影で同じ列を使う。製品経路で承認済みのprofileに持たせるかは#45の時に決める（C2）。撮影ごとに自動で列を選ばない。列を動かすのは人だけで、Cの画面で継ぎ目の線をドラッグし、帯の余裕の内側で左右に動かす。「この列で固定」を押した時だけ、固定の合成条件の新しい改訂として保存し、以後の撮影のJPEGとCの両方に使う。開発経路の列の初期値は帯の中央で、記録と画面に※仮と出す。操作と保存の細部は[設計](design/fixed-composite.md)の5節（#403、2026-11-04に承認）。使った列とその出どころは報告に記録し、検品の時に一目で分かるように示す。CとJPEGは、列を入力に取る同じ合成の関数（#398）を通る。列がどこかの行でAとBの重なり（と混ぜ幅）から外れる時は合成せずに失敗にする。
5. **設置が変われば校正をやり直す**: 固定の合成条件とprofileの写像を更新するのは、設置が変わった時の再校正だけ（#362の読み替え）。設置の時の推定（#364・#365）から作った条件は、人が見て確かめた上で使ってよい（B3）。継ぎ目の列は、人がCの画面で動かして保存した時に更新する（決定4）。どちらの更新も人が明示して行い、撮影の結果から自動では更新しない。台の揺れやシャッターの振動で組ごとにAとBが動いた撮影は、固定の写像からずれた分がゲートで測られ、合格線を超えれば書き出さない（原画像は残す、#385）。揺れない取り付けを前提にする（B1）。
6. **倍率だけは上限以内で撮影ごとに補正する（B7）**: ピントは撮影のたびに合わせ直してよい。左右の倍率の差が上限以内なら、撮影ごとにBに倍率だけを当てる。上限を超えたら補正せず（倍率1）、ゲートで不合格にして「ピントか高さを確かめてください」と案内する。上限で切った部分的な補正はしない。上限は所有者の決定で1%（初期値。実機で測った後に見直す前提）。倍率の差は「倍率を%にした値と100の差の絶対値」で比べ、1%ちょうどは補正する（#365の倍率の判定と同じ比べ方、※案）。値はコードに既定値として埋め込まず、方針ファイル（`wpd-merge-policy.json`の`scaleAutoCorrectionMaxPercent`）か固定の合成条件の記録（開発経路。値は方針ファイルから条件を作る時に写す、未承認）から読む。どちらにも値が無ければ倍率を補正しない（倍率1）。
   - 倍率の推定では、回転を固定の写像の値に縛る。撮影ごとに変えるのは倍率だけ（所有者の決定（2026-11-04、#396））。推定の方法は#365の推定器の倍率の部分を使う案で、#400の実装に合わせて書く（※案）。
   - 倍率の補正の中心は、Bの画像の中心（原寸の (幅/2, 高さ/2)、写像の `anchor:"image-center"` と同じ取り方）を固定の写像で出力へ移した点にする（所有者の決定（2026-11-04、#396、所有者の言葉「Bの中心に改める」）。同日の最初の決定の「継ぎ目の列の上」を置き換える）。ピントや高さの変化はBの光学中心のまわりにかかるので、この中心なら倍率でないずれが残らない。継ぎ目の線は動かない: 継ぎ目は出力の列で決まり、合成の窓の左端を保つので、Bをどこを中心に拡大しても列は変わらない。
   - 倍率を推定できない時（繰り返し模様、候補が定まらないなど）は補正しない。固定の写像のまま合成し、ゲートの判定に任せる（所有者の決定（2026-11-04、#396））。報告に「倍率を推定できなかった」と残す（記録の書き方は#400）。
   - 推定した倍率と当てた倍率、上限の値と出どころは報告に記録する。
   - ライブのCは撮影の前に倍率を知らないので、固定の写像のまま描く。CとJPEGの幾何の違いは、上限以内の倍率の差だけになる。

### ADR-0021の三状態の扱い（決定、B2）

ADR-0021は、設置の判定を`ready`、`ready-auto-correction`、`physical-adjustment-required`の三状態にした（コードでは`src/m2/include/a0/m2/setup_assessment.hpp`の`SetupReadiness`）。B2の回答で、`ready-auto-correction`を廃止し、`ready`と`physical-adjustment-required`（再校正か物理調整の案内）の二状態にする。

- 固定の条件からのずれの大きさは状態にせず、診断の値として表示と報告に出す。明るさの比と上限以内の倍率は毎回当てるので、状態に数えない。
- 書き出してよいかはゲート（#385）が決め、設置の判定は案内にとどまる。
- 再校正を案内する境目の値は所有者が決める（#402、B4）。決まるまではずれの値を表示するだけで、状態を分けない。
- `SetupReadiness`と表示の変更は別の実装のIssueで行う（※未着手）。

### ADR-0034の不変条件2との関係

- 今: 「撮影ごとにhomographyを推定しない（ADR-0021）。推定は開発時の校正ツール（#277）だけで行う。撮影ごとの補正は承認された上限の内側の相似変換に限る。」
- 改定（B9で承認した案をB7と2026-11-04の決定に合わせて直した。本文の書き換えは承認後の作業）: 「撮影ごとにhomographyも相似も推定して合成に当てない。合成に使う写像の推定は、開発時の校正ツール（#277）と、設置時に固定の合成条件を作る時だけで行う。撮影ごとの推定は、ずれの診断とゲートの入力に使う。撮影ごとの補正は、継ぎ目に沿った明るさの比と、承認された上限以内の倍率に限る。倍率の推定では回転を固定の値に縛り、補正の中心はBの画像の中心を固定の写像で出力へ移した点に置く。上限を超えた倍率と、推定できない倍率は当てない。」（細部の3点は所有者の決定（2026-11-04、#396））
- profile 2.0.0の`correctionEnvelope`（[rig profile 2.0.0の設計](design/rig-profile-v2.md)の2.4節）: B8の案のとおり、回転の欄は補正の上限から再校正の案内の境目に読み替える。倍率の欄はB7で撮影ごとの補正の上限として残ると読む（※読み方。B8の案は回転・倍率の両方を読み替える案だったが、B7で倍率の補正が戻ったため）。本ADRではschemaを変えない。

### #385の決定との関係

- ゲートの量は、書き出す合成と同じ写像でAとBを別々に置いて測る（[品質ゲート](M2_SEAM_QUALITY_GATE.md)の3.2節）。写像が固定になってもゲートの作りは変わらない。決定6で倍率を当てた撮影は、当てた後の写像で測る。遷移帯は、固定の合成条件の列と混ぜ幅から決まり、`SeamCutTransitionBand`にすべての行で同じ列の道筋を渡せる。
- D2（継ぎ目のそばの局所合わせ）: 取り下げる（B6）。撮影ごとに写像を変え、Cには当てない設計なのでCとJPEGの見え方も変わり、#396の決定と食い違うため。品質ゲートの5.6節に追記を置いた。
- D3（AF-Sを前提にする）: B7で、撮影ごとのピントの合わせ直しで変わる倍率は、上限以内なら決定6で補正する。上限を超えた撮影はゲートで不合格にし、「ピントか高さを確かめてください」と案内する。歪みのわずかな変化は残差としてゲートに出る（※推定、実機で未確認）。
- 大域的な配置の確認（品質ゲートの16.11節、#394）: 撮影ごとの推定が周期1つ分ずれた位置を合成に使う危険は無くなり、固定の条件を作る時の確認へ役目が移る。決定6の倍率の推定は繰り返し模様で誤ることがあり、その扱いは#400で決める（推定できない時は補正しない、※案）。詳細は#394・WP-04の再開時に見直す。

### 守る不変条件

1. 合成の写像（平行移動と回転）と継ぎ目の列は、撮影やframeの内容で変えない。同じ固定の条件からは、どの撮影でも同じ写像と同じ列を使う。例外は決定6の上限以内の倍率だけ。
2. 撮影ごとの推定の結果を、合成の写像、継ぎ目の列、固定の合成条件、profileに書き戻さない。自動で学習・更新しない。撮影ごとに当てた倍率も次の撮影に持ち越さない。
3. 固定の合成条件の更新は、人が明示した再校正（写像）と、人がCの画面で列を動かして「この列で固定」を押す操作（継ぎ目の列）だけで行う。更新は新しい改訂として保存し、前の記録を消さずに残し、新しい記録は前の記録のSHA-256を持つ（A3）。
4. 撮影ごと（Cではframeごと）に変えてよいのは、継ぎ目に沿った明るさの比と、上限以内の倍率（JPEGだけ）だけ。どちらも平行移動・回転・継ぎ目の列を動かさない。継ぎ目は行ごとに曲げない。
5. CとJPEGは同じ固定の条件と、列を入力に取る同じ合成の関数（#398）を通り、同じ列を使う。幾何の違いは、Cをライブビューの画素へ縮める`S`（ADR-0036決定1の2）と、JPEGだけに当てる上限以内の倍率。このほか、Cでの混ぜ幅と明るさの窓の扱い（設計の3.4節、※未決）によって、画素の見え方も違いうる。
6. 使った継ぎ目の列とその出どころ、当てた倍率は報告に記録し、検品の時に一目で分かるように示す。
7. ADR-0021の安全境界は残す。自由なhomographyを推定しない。上限を超えた値をclampして合格にしない。未承認の閾値をコードに置かない。
8. 固定の合成条件（未承認）を承認済みのprofileとして扱わない。製品の合成はapprovedのprofileだけを受け付ける（ADR-0033の4節、ADR-0034の不変条件1）。
9. ゲートの合格線を超えた撮影は書き出さない。どの状態でも原画像は残す（#385）。

### 改定の対象と差し替えの案

本ADRはAcceptedだが、改定の対象の本文の書き換えは別の変更で行う（承認後の作業）。それまでは各対象の追記が本ADRへの改定を示す。AGENTS.mdは人が書き換えるので、追記も置かず、下に案だけを置く。

| 対象 | 今の文面の要旨 | 改定の方向 | 2026-11-04の追記 |
|---|---|---|---|
| ADR-0021 決定 | 撮影ごとの位置・回転・倍率・露出・色の差を上限の内側で一時的に補正する。三状態 | 幾何は固定の写像。継ぎ目は固定の縦一直線の列。撮影ごとに変えるのは継ぎ目に沿った明るさの比（色は※未決）と上限以内の倍率だけ。二状態 | 置いた |
| ADR-0021 安全境界 | 撮影ごとに自由なhomographyを再推定しない。profileを自動学習・更新しない | 残す。「撮影ごとの平行移動・回転を合成に当てない」「固定の合成条件を自動で更新しない」を足す | 同上 |
| ADR-0034 不変条件2 | 撮影ごとの補正は承認された上限の内側の相似変換に限る | 上の改定 | 置いた |
| ADR-0036 不変条件4 | 撮影ごとのhomographyの推定はしない | 撮影ごとの平行移動・回転を合成に当てない、に揃える | 不変条件の前文に置いた |
| `FR-STI-003` | 撮影ごとの差が上限の内側なら一時的な残差補正 | 幾何は固定の変換。継ぎ目は固定の縦一直線の列。撮影ごとに変えるのは明るさの比と上限以内の倍率だけ | 置いた |
| `FR-STI-005` | 自動補正量が範囲を超えたら成功扱いにしない | 倍率は上限を超えたら補正せず不合格。位置・回転は固定の条件からのずれ（診断の値）に読み替える | 置いた |
| `FR-STI-006` | `補正不要`・`自動補正して続行`・`物理調整が必要` | 二状態（`自動補正して続行`を廃止） | 置いた |
| `docs/WPD_STILL_REVIEW.md` の#365・#368の節 | 撮影ごとの相似を合成に当てる。内容で道筋を選ぶ | #400で書き換える。相似と道筋は診断として報告に残し、倍率だけを上限以内で当てる | 節の頭に注記を置いた |
| `docs/WPD_PREVIEW_MERGE.md` の冒頭・#364の節 | 自動位置合わせv2と手動の調整がCの位置を変える。線形feather | #401（Cの節。書き換え済み）・#402（v2の節）で書き換える | 節の頭に注記を置いた |
| `docs/M2_SEAM_QUALITY_GATE.md` の5.6節 | 撮影ごとの局所合わせのADR草案（D2） | D2を取り下げる（B6） | 置いた |
| AGENTS.md | 下の3文 | 下の差し替えの案（人が反映する） | 置かない |

書き換えの時に確かめる対象（本ADRでは追記を置かない）: `FR-UI-001`（設置と自動補正可否の表示）、PRODUCT_REQUIREMENTSの受入の9（範囲内の差だけを自動補正する）、`docs/ARCHITECTURE.md`・`docs/M2_PRE_GATE.md`・`docs/ROADMAP.md`・`docs/FEATURE_VERIFICATION_PLAN.md`・`docs/STITCHING_VALIDATION_AND_EXPERT_REVIEW.md`の三状態の記述、`docs/M2_AUTO_OVERLAP.md`（相似の保持`T_hold`の節と、撮影ごとの補正を前提にした節）、`docs/M2_SEAM_QUALITY_GATE.md`の0節・6節・7節、`docs/design/rig-profile-v2.md`の`correctionEnvelope`、`src/m2/include/a0/m2/setup_assessment.hpp`、`.autodev/requirements/normalized.json`。

AGENTS.mdの差し替えの案（B9で承認した案をB7と2026-11-04の決定に合わせて直した。人が反映するまでは今の文言が効く）:

- 今: `Treat setup as an automatic-correction envelope, not pixel-perfect manual placement: use an approved fixed rig profile plus transient bounded per-capture correction. Do not freely re-estimate a homography, hard-code unapproved optical thresholds, clamp an over-limit result into success, or automatically learn/update the rig profile.`
- 案: `Treat setup as a calibrated fixed installation, not pixel-perfect manual placement: compose every capture with one fixed geometry per installation (an approved rig profile, or on development paths an unapproved fixed composite condition) and one straight vertical seam column stored with it (ADR-0037). Only a person may move the seam column, by dragging it in the C view within the overlap margins and pressing the fix button; save it as a new revision of the fixed condition and use it for every later JPEG and C. Record the column used in every report. Per-capture position and rotation estimates are diagnostics and seam-gate inputs only; they never move the composite geometry or the seam. Per capture, only brightness (the A/B ratio along the seam applied to B) and a left/right scale difference within the owner-approved limit may be corrected; read the limit from the policy file or the fixed composite record, never from a code default. Estimate scale with rotation held at the fixed value, centre the scale correction on the centre of B's image moved into the output by the fixed mapping (the seam is a column of the output window and does not move), and do not apply a scale that is over the limit or cannot be estimated (compose with the fixed mapping and leave the decision to the seam gate). Never bend the seam row by row. Recalibrate and replace the fixed condition when the installation changes. Do not freely re-estimate a homography, hard-code unapproved optical thresholds, clamp an over-limit result into success, or automatically learn/update the rig profile or the fixed composite condition.`
- 今: `A provisional fixed-parameter C review view may consume live frames in memory and must be labelled uncalibrated.`
- 案: `A development C view may consume live frames in memory only through the same fixed composite condition and straight-seam function as the saved JPEG, scaled to Live View pixels (ADR-0036 decision 1, ADR-0037); it does not apply the per-capture scale correction. It must be labelled uncalibrated and unapproved, and the Live View-to-still mapping must be labelled unverified until measured (#309).`
- 今: `The later request for rough automatic merge plus manual refinement also authorizes a button-triggered translation proposal in the uncalibrated development C view. Keep the search bounded to the visible manual translation range, identify the result as a proposal requiring visual review, and permit manual pixel refinement. No per-frame homography estimation, rig-profile learning, optical acceptance threshold or final calibration approval is implied.`
- 案: `The button-triggered estimate in the development C view reports the offset from the fixed composite condition and may advise recalibration; it never moves C. Manual or automatic alignment may be used only by explicit operator action to create or replace the fixed composite condition when the installation is set up. No per-frame homography estimation, rig-profile learning, optical acceptance threshold or final calibration approval is implied.`

書き換えの時に確かめるAGENTS.mdの文: #334の文の`snapshot C geometry`（JPEGの幾何はCの値でなく固定の条件から取るので、記録するものが変わる）、#336・#338の文（StillReviewの手動の再Merge。B9で、保存済みの組を調べる道具として残すことが承認された）。

### 範囲外と未決

- 実装（#398〜#403）。
- 値: 明るさの比の上限（実機で測ってから）、再校正の案内の境目（#402）。
- 倍率の推定の方法は#400で決めた（#365の推定器が返した合意タイルから、回転を固定の値に縛り、推定の中だけ平行移動を自由にして倍率を求め直す。当てるのは倍率だけ）。回転を縛ること、補正の中心をBの画像の中心を固定の写像で出力へ移した点に置くこと、推定できない時は補正しないことは所有者の決定（2026-11-04、#396）。
- 製品経路で継ぎ目の列と倍率の上限を承認済みのprofileに持たせるか（#45の時）。
- 色の補正（#46）。承認済みのprofile（#45）と最終のリグ（#43）。Cのframeの出どころ（ADR-0036決定2、保留）。#394・WP-04の見直しの詳細。

### 所有者の回答の記録（2026-11-04、#396）

確認点Bの番号は#396のコメント「所有者に確認していただきたい点」のもの。

| 確認点 | 回答 |
|---|---|
| B1 撮影ごとの位置・回転・倍率の補正を合成に当てない。揺れて動いた撮影は不合格 | 案のとおり承認（※読み方）。ただし倍率はB7で例外 |
| B2 三状態の扱い | 二状態にする（`ready-auto-correction`を廃止） |
| B3 設置時の推定から作った条件を、人が確かめて使ってよいか | 案のとおり承認（※読み方） |
| B4 再校正の案内の境目。決まるまでは値を表示するだけ | 案のとおり承認（※読み方）。値は未決 |
| B5 明るさの比に上限を置くか | 置く。値は実機で測ってから |
| B6 #385のD2（局所合わせ）を取り下げるか | 取り下げる（案のとおり、※読み方） |
| B7 #385のD3（AF-S）の倍率 | 所有者の修正: ピントは都度合わせてよい。倍率の差は上限以内なら撮影ごとに補正、超えたら不合格で案内。上限は1%で始める（同日の決定。実機で測った後に見直す）。細部の3点（回転を固定の値に縛る、推定できない時は補正しない、中心）も同日に決定。中心は最初「継ぎ目の列上」（所有者の言葉「推奨どおりで」）としたが、同日の後の決定で「Bの画像の中心を固定の写像で出力へ移した点」に改めた（所有者の言葉「Bの中心に改める」） |
| B8 `correctionEnvelope`の回転・倍率の欄の読み替え | 案のとおり承認（※読み方）。倍率はB7で補正の上限として残ると読む |
| B9 ADR-0034の不変条件2の改定とAGENTS.mdの差し替えの案、StillReviewの手動の再Merge | 案のとおり承認（※読み方）。改定と案はB7と同日の細部の決定に合わせて直した |

### 承認後の作業

- ADR-0021の決定と安全境界、ADR-0034の不変条件2、ADR-0036の不変条件4の本文を書き換える。AGENTS.mdは人が差し替える。
- 改定の対象の表の本文と、書き換えの時に確かめる対象を書き換える。二状態に合わせて`SetupReadiness`と表示を直す（別Issue）。
- 倍率の上限（1%）を実機で測った後に見直す。明るさの比の上限の値を所有者が決めたら、本ADRと設計に記録する。
- `.autodev`の要件と計画を正規化し直す。

## ADR-0038: CAM-Aの露出の設定（露出モード・シャッター速度・絞り・ISO）をCAM-Bへ写す書き込みを、開発用プレビューから始めて本番の画面でも、カメラの設定を書かない規則の例外にする

- 状態: Accepted（#416、所有者）。所有者は、書けるのはCAM-Bの5項目だけであること、Aが M・ISO感度自動制御OFF・Bulbでない時だけ写すこと、各命令は1回だけで再送しないこと、一致を確かめられない間は撮影を止め再起動しても解けないことを示した確認に承認し、段Bを継ぎ目合わせと並行で進めてよいとした。同じ承認に、カメラを壊しうる設定を変えられないようにし、対象を一般的なカメラの設定と同じ範囲にとどめる条件が付いた（下の「所有者の承認」と、決定の「範囲の原則」）。securityの確認（判定は条件付き、HIGH 0件）のMEDIUM 1〜5は決定3・5・6・8・9・10・11と段Cの前提条件に、段Aの安全の門の変更を見た時のsecurityの注意は決定9に入れた。段A（読むだけの表示）の実装はブランチ`claude/issue-416-h1`にあり、独立の試験に合格したが、本ADRの承認の時点ではmainに未統合で、実機でも動かしていない（※未検証）。段B（開発用プレビューでの書き込み）と段C（本番の画面での書き込み。段の呼び名は本書のもの）は、本ADRの承認の時点では未着手。プロパティのcodeは段Aの実機の読み取りで確定する（※未検証）。実機への書き込みは、そのcodeを1つに確定した後にだけ行う。なお、WBと測光を「写す」項目に足す改訂案は、下の「ADR-0038の改訂案」の節にProposedとして書いてあり、所有者の承認までは上の5項目のままが効く。
- 決定日: #416の所有者の承認による（日付はIssueの記録による）
- 関連: #416（本ADR。設計、所有者の決定と回答と承認、securityの設計レビューM-1〜M-8）、#415（合成の側の明るさと色の合わせ）、#374（Bが関わる平置きの校正の撮り直し）、#292・#326（Nikon SDK同梱の資料を使わない約束）、#351（本番でもWPDを使う所有者の決定）、ADR-0014・0016・0023・0024・0028・0030・0032・0036、本番の経路をWPDへ移すADR（未承認の下書き、#351。本ADRの段Cの前提）、AGENTS.md、`docs/WPD_DUAL_PREVIEW.md`、`docs/D810_PC_CONTROL_CAPABILITIES.md`、`docs/OPERATOR_UI_SPEC.md`、`docs/OPERATOR_GUIDE.md`、`src/phase0/include/a0/phase0/experimental/d810_preview.hpp`、`src/m3/OperatorShell`
- 背景: 二台のD810を吊り下げたリグでは本体のボタンに手が届かない。#416の原画像のEXIFでは、両台の露出モードと絞りは同じで、シャッター速度とISOが違い、BがAより明るく写っていた（#416）。左右の明るさが違うと合成の色が合わず、校正の目印の検出も落ちる（#416では、Bの重なりの一部がチャートの灰色の照合を通らなかった）。合成の側の明るさの比（#415、ADR-0037の決定3）は設定をそろえた後も使うが、設定の違いから来る差を比だけで埋める前提にはしない。今のリポジトリは、開発用プレビューでも製品でも「アプリはカメラの設定を書かない」を安全の規則にしている（下の「外す規則と外す範囲」）。PCからBの設定を書くには、この規則の例外を明文にする必要がある。

### ADR-0038の所有者の決定（#416）

所有者の言葉（#416）: 「左右でカメラの明るさが違うので、合うようにしてほしい。つまりある程度のカメラ設定をpc側から操作できるようにするべき」「カメラコントロールプロ２でいじれる範囲はいじれるのが良い」。

#416で所有者が決めたのは次のとおり。下の「決定」は、これを規則の形に落とした案。

1. PCから書く項目は段1のうちWBを除いたもの。露出モード、シャッター速度、絞り、ISO（ISO感度自動制御のOFFを含む）。段1全部とWBを書かないことの組み合わせをこう読んだ（※読み方。#416で所有者に伝えてある）。
2. WBは今は書かない。表示だけ。色は#415と一緒に扱う。
3. 開発用プレビュー（`wpd_dual_preview`）に限る。Liveの停止を確かめた後、撮影のtransactionの外で、許可した項目だけ、Aから読んだ値だけをBに書く。値はBが広告する値の一覧にあるものに限る。書いた後に読み直し、合わなければ撮影を止める。再試行しない。記録を残す。製品の経路（ADR-0023・0024、PRODUCT_REQUIREMENTSの対象外の行）は変えない。（範囲は、下の「下書きを見ての回答」の1で本番の画面にも広げた。書き方の条件はそのまま効く。）
4. 写す条件: AがMモード・ISO感度自動制御がOFF・シャッター速度がBulb／Timeでない時だけ写す。満たさなければ何も書かずに理由を出す。
5. 書き込みがうまくいかなかった時（読み直しの不一致・途中の失敗・結果が分からない）は撮影ボタンを止め、操作者が読み直して一致を確かめるまで戻さない（ウィンドウを閉じると解除）。
6. 露出モードをPCから書けない時は本体で合わせる。
7. 書く順番（露出モード → ISO自動OFF → ISO → シャッター速度 → 絞り）、期限（読む30秒・写す60秒、※仮）、写す時にAも読み直すこと、記録の項目は設計の案のとおり。
8. 段の分け方: 段Aは読むだけ（表示と`--settings-read-diagnostics`）。段Bが書き込みで、本ADRのAccepted、AGENTS.mdの改定、securityのgateのレビューの後に着手する。

段2（露出補正、測光、ピクチャーコントロール、画質・サイズ、AFモード）の書き込みは、#416の所有者の方向には挙がっているが本ADRに含めない。含めるなら本ADRを改定する。

### ADR-0038の所有者の決定（下書きを見ての回答、#416）

所有者は本ADRの下書きを見て、#416で次を決めた。

1. カメラの設定は操作者も変えられるようにする。開発用の画面だけに留めず、本番の画面からも書けるようにする。
2. 撮影ボタンを戻す条件は「読み直して一致を確かめた時」だけ。「もう一度写して一致した時」は入れない。

同じ回答で所有者は次も示した。本番の画面から書く段は、本番の経路のWPDへの移行と実機の受入の後に置く（※仮定。所有者は下書きで案を出すように求めた。本書では段Cと呼ぶ）。操作者がAからBへ写す以外に値を選べるようにするか（段1の5つのプロパティの外を含む）は未決。

1は上の決定3の「開発用プレビューに限る」を置き換える。2は上の決定5の文面を所有者が確かめたもので、本文の決定8はこの文面のまま。

### ADR-0038の所有者の決定（未決への回答、#416・#351）

所有者は、下書きに残っていた未決について次を決めた。

1. 操作者が設定の値そのものを選ぶ機能は入れない。まず「AからBへ写す」だけにする。値を選ぶ機能は、入れるなら将来の別のADRで扱う（決定3）。上の「操作者がAからBへ写す以外に値を選べるようにするか」はこれで閉じる。
2. 設定の書き込みが一致で終わらなかった時の撮影の止めは、OperatorShellとCamera Agentの再起動をまたいで持ち越す。解けるのは「読み直して5項目が一致した時」だけ（段Cの前提条件の(b)〜(d)）。開発用プレビューの「ウィンドウを閉じると解除」（上の決定5）は段Bだけに限る（決定8）。

### ADR-0038の所有者の承認（#416）

所有者は#416で次を承認した。

1. 本ADRをAcceptedにする。確認に示した要点は次の4つ。書けるのはCAM-Bの5項目だけ（決定2）。Aが M・ISO感度自動制御OFF・Bulbでない時だけ写す（決定4）。各命令は1回だけ送り、再送しない（決定7）。一致を確かめられない間は撮影を止め、再起動しても解けない（決定8。段Bの「ウィンドウを閉じると解除」は上の未決への回答のとおり段Bだけ）。
2. 段Bを、継ぎ目の位置合わせの作業と並行して進めてよい。実機への書き込みは、段Aの実機の読み取りで決定13のcodeを1つに確定した後に行う（段Bの前提条件3）。

同じ承認に、所有者は条件を付けた。所有者の言葉（#416）: 「カメラを壊すような設定は変えられないようにね。あくまで一般的なカメラ設定と同じ。」。続けて、範囲の目安をメーカーの撮影用の制御ソフトで変えられる設定と同じにしてもよい、と述べた。本書はこれを、対象を足す時の範囲の目安と読んだ（※読み方。所有者に確認中）。どちらも決定の「範囲の原則」に入れた。本ADRの対象は5項目のままで、この目安の内側にある。

### ADR-0038の改訂案: 「写す」の項目にホワイトバランスと測光を足す（Proposed、所有者の承認待ち、#416）

- 状態: **Proposed。所有者の承認待ち**。承認されるまで、本節は決定ではない。上の決定2・4・5と「外す規則と外す範囲」の表の本文は、Acceptedのまま効く。承認された時に、決定2の「WB（`0x5005`）、測光（`0x500B`）…は読むだけ」の行のうちWBと測光を置き換え、AGENTS.mdの例外の文（5つのプロパティ）を人の手で改定する。その後に、コードの定数`kD810Adr38RevisionAccepted`を真にするレビュー付きcommitで、初めてWBと測光の行を有効にできる。
- 発端: 所有者の言葉（#416）「5 は足しましょう。左右差が生まれてしまいます」。実機の読み取り（読むだけ）で、両台のWBと測光が違う項目として出た。実機の値は本書に書かない。
- 実装の状態: 計画・確定の画面・読み戻しの照合・gateは7項目で動く作りまで入っている（試験用の表だけで確かめた）。製品の表のホワイトバランスと測光の行は無効のままで、上の定数が偽の間は有効にできない。実機で書いたのはCAM-BのISOの1回だけで、ホワイトバランスと測光は実機で書いていない（※未検証）。

**調べた事**（出典は`third_party/PTP_PROTOCOL_REFERENCES.md`の末尾の節。固定commitの行のアンカー。公開の資料だけで、Nikon SDK同梱の資料は使っていない）

| 項目 | code | 型 | 値 | 出典での扱い |
|---|---|---|---|---|
| WB | `0x5005` | UINT16、列挙 | 標準の1〜7（Manual・Automatic・One-push Automatic・Daylight・Fluorescent・Tungsten・Flash）、Nikonの`0x8010`〜`0x8016`（Cloudy・Shade・Color Temperature・Preset・Off・Flash・Natural light auto） | 上流の設定の表に書き込みの関数がある（書ける項目として載っている） |
| 測光 | `0x500B` | UINT16、列挙 | 汎用の表（他社の値も載る）は1 Average・2 Center Weighted・3 Multi Spot・4 Center Spot。Nikon用の名前の表は2 Center-weighted・3 Multi-pattern・4 Spot・`0x8010` Highlight-weighted（1の名前は無い）。画面と文書にはNikon用の名前を出し、1は値のまま出す（名前の食い違いは出典の記録の「Metering mode names」） | 同上 |

- 標準のPTPの番号（`0x5005`・`0x500B`）に、Nikon独自の値（WBの`0x8010`〜）が乗る形。どちらも標準の設定の命令`0x1016`で書き、vendor operationは要らない。
- GetSet: アプリは読む時に`GetSet`とFormFlagを読み、書く直前に毎回、`GetSet`が1で、列挙（値あり）か範囲（stepが正）の時だけ書く（決定13）。この2項目が実機で`GetSet=1`かは、本書の作業では確かめていない（※未検証。段Aの読み取りの行で人が確かめる）。
- 値の意味はメーカーごとの表の意味で、D810でその意味かは確かめていない（※未照合。出典の記録のとおり、公開の表は機種ごとではない）。画面は、名前を確かめた値だけ名前で出し、それ以外は`raw 0x…`で出す（段Aと同じ）。
- WBの付随する設定（出典の末尾の節）: 種類ごとの微調整（`0xD017`〜`0xD01D`、本体により型が違う）、色温度（`0xD01E`。名前だけで、型と単位の出典が無い）、プリセット番号（`0xD01F`）、プリセットの値（`0xD025`〜`0xD029`、UINT32。上流は書き込みの関数を持たず、単位の記述も無い）。
- 測光の付随する設定: 中央重点の範囲（`0xD059`、UINT8。値の表は本体で違う）。スポット測光の位置を持つプロパティは、見た範囲では見つからなかった（AF点に連動すると思われるが、※未確認）。
- 登録した白（プリセット）の**中身**を写す公開の手段は、確かめられなかった。中身を持つ`0xD025`〜`0xD029`は上流で書けない扱い、`0xD01F`は番号、白を測る操作（`0x9402`〜`0x9406`）はvendor operationで、アプリは送らない。→ 下の(c)は成り立たない（※未成立）。

**案1 項目と値の集合**

- 写す項目を5項目から7項目にする。順番は、既存の5項目の後に WB → 測光（表の順＝書く順）。向きはAからBだけ、書く相手はCAM-Bだけ（決定2・3のまま）。値を選ぶ入力は作らない。
- WBで写す値は、名前を確かめた種類だけ: `0x0002` Automatic、`0x0004` Daylight、`0x0005` Fluorescent、`0x0006` Tungsten、`0x0007` Flash、`0x8010` Cloudy、`0x8011` Shade。本体の別の中身に頼る値（`0x0001` Manual、`0x0003` One-push Automatic、`0x8012` Color Temperature、`0x8013` Preset）と、名前を確かめていない値（`0x8014`〜）は写さない。
- 測光で写す値は1〜4だけ。
- どちらも、CAM-Bが自分の一覧にその値を広告している時だけ書く（決定の「値」）。1項目1回、再送なし、巻き戻しなし、書いた後に読み戻して一致／不一致／不明（決定7・8）。前提は既存の表のとおり（Bが M・ISO感度自動制御OFF。Aは写す条件で既に M・OFF）。
- 2項目は、計画全体を拒否させない。Bが値を広告していない・書けない・読めない時は、その項目だけを「外した項目」に理由つきで出し、露出の5項目の計画は作る（5項目の失敗が従来どおり計画を拒否する扱いは変えない）。
- 写すのは「種類」だけで、WBの微調整と測光の付随設定（中央重点の範囲）は写らない。確定の画面のWBの行に「種類だけを写します。微調整は写りません」と出す。
- 写さない物（所有者の決定: 写せる値の一覧にある Automatic と Fluorescent は残す）: Automaticの時のオートの型（`0xD141`、ptp.h の `PTP_DPC_NIKON_WBAutoType`）と、Fluorescentの時の蛍光灯の型（`0xD14F`、同 `PTP_DPC_NIKON_FlourescentType`）も写らない。同じ数の種類でも、型が違えば色が違いうる。番号は公開の出典（libgphoto2 の ptp.h、固定commitの 2562・2571 行）の定義のとおりで、D810がこの2つを持つか・どう答えるかは確かめていない（※未検証）。確定の画面の注記にこの2つを足すかは、WBの行を有効にする前に決める。

**案2 WBがプリセットの時の扱い（保留。所有者と相談。既定の案は置かない）**（Aの登録した白は、BがPresetになってもBに登録された白が使われる見込みで、色が揃うとは限らない。所有者の言葉は「要相談。一旦カメラコントロールプロと同じように」。Camera Control Pro 2の扱いは下の「改訂案その2」の案Cに書いた）

| 案 | 内容 | 長所 | 短所 |
|---|---|---|---|
| (a) | モード（Preset）だけ写す。確定の画面に「Bの登録済みの白が使われます。色が揃わない事があります」と必ず出す | 左右のモードは揃う | Bに白が登録されていない・古い時、Autoより悪い色になりうる。登録の有無は読めない。画面の警告に頼る |
| (b) | AがPresetの時は、WBを「写せない項目」として外し、理由を出す。Bの値は変えない | 登録した白の中身を写せないのに、写したように見せない。Bを悪化させない。実装は既存の「内容に頼る値は書かない」に沿う | WBは操作者が本体で合わせる必要がある（画面がそう案内する） |
| (c) | 登録した白の中身まで写す | 色まで揃う | 公開の資料で書けると確かめられなかった（上の「調べた事」）。成り立たない |

判断材料（どの案も、所有者と相談して決める。上の表のどれを既定にするかは、本書では決めない）: (a)は「写した」ように見えて色が揃わず、Bのプリセットが未登録・古い時は元より悪くなりうる。(b)は何もしない側に倒れ、既存の規則（本体の別の中身に頼る値は書かない、止めを解く比較では不明）と矛盾しないが、WBは操作者が本体で合わせることになる。(c)は、Aの登録した白の中身を写す手段が公開の資料では確かめられておらず、今は成り立たない。Camera Control Pro 2は、プリセットの番号を選び、PCから白を測って登録し、コメントを編集できる（下の案C）。それと同じ形でBの白を整えるには、白を測る操作を送る決まりの改訂が要る。

部品の今の動きは、暫定的に(b)と同じ「写さず、理由を出す」にしてある。これは最小の（何も書かない側の）動きとして入れただけで、既定の案として選んだものではない。行は無効の表の中にあり、承認までは何も書かない。(a)に変える時の変更は小さい（`excluded`からPresetを外し、確定の画面に警告を足す）。(c)や、白を測る操作を使う案は、別のADRになる。

(b)の動きの細部（暫定）: 外すのは、AのWBが写せない値の時（Manual・One-push・Color Temperature・Preset・名前のない値）。Bが同じ数値を読んでいても、白の中身が同じとは限らないので、「外した項目」に載せて、比べられないことを画面に出す。Bが今Presetで、Aが名前のある種類（例: Daylight）なら、Bにその種類を書く（Bの登録した白は本体に残る）。

**案3 一般的なカメラ設定と同じ範囲に入るかの見立て**

- 所有者の条件（承認の条件）「カメラを壊すような設定は変えられないように。あくまで一般的なカメラ設定と同じ」と、範囲の目安（メーカーの撮影用の制御ソフトで撮影者が変えられる撮影の設定）に照らす。
- WBと測光のモードは、本体のボタンとメニューで撮影者がふつうに変える撮影の設定で、本体の動作・寿命・firmware・記録先・カード・時計・カスタムメニューの保存に関わらない。値の一覧はBが広告した物に限り、名前を確かめた値だけを書く。よって範囲の内側と見立てる（※見立て。メーカーの制御ソフトで変えられるかは、公開の一次情報を確かめていない。所有者の方針の目安に当たるかは所有者の判断）。
- 測光モードが写る明るさに効くかは、露出モードによる。Mでは測光は露出計の表示に効くだけで、写る明るさは変えないのがふつうと考えられるが、出典は確かめていない（※要確認）。左右の本体の設定をそろえる目的（所有者の方針）には効く。

**案4 変わる物と、所有者に決めてもらう事**

1. 決定2の行の置き換えと、AGENTS.mdの例外の文（5つ→7つ）の人の手の改定。
2. 撮影の止めを解く比較（5項目が両台で同じ）は、WBと測光の行が有効になった時に7項目になる（行が無効の間は5項目のまま）。
3. **未決（所有者）**: WBの行が有効で、AがPresetの時、止めを解く比較は「不明」になって解けない（今の実装の、安全側の動作。試験で固定した）。止めが掛かるのは書き込みの失敗の後だけだが、解くにはAの本体でWBを名前のある種類にするしかなくなる。案(i) 今のまま。案(ii) 書かない値（写せない種類）は、止めを解く比較から外す（書き込みの失敗の対象になりえないため。比べられないことは画面に出す）。(ii)は、決定の「不明を一致と数えない」を緩めるので、採るならsecurityの確認を通してから。どちらにするかはWBのプリセットの扱い（案2）と一緒に、所有者と相談して決める。(ii)を採るなら、securityの確認を通してから。行を有効にする前に決める。
4. **窓の大きさ（実装済み）**: 7項目の確定の画面は最大11行（外した項目があれば最大12行）。開発用プレビューの窓は、12行が入る大きさ（クライアント領域で幅1240・高さ931 px）より小さくできない。作業領域がそれより小さい画面で行が入らない時は、「窓を大きくしてください」と出て書き込めない（何も書かない側）。行の詰め方を変えるかは、必要になった時に決める。
5. 実機で確かめる事（承認と別に承認を取る。ADR-0030・0032と同じ扱い）: 2項目の`GetSet`と一覧、Bが広告する値、書き込みと読み戻しが1項目ずつ一致するか。最初は1項目（例: 測光）から始める案（※案）。

**承認後の作業**: 決定2の行の置き換え、AGENTS.mdの人の手の改定、`kD810Adr38RevisionAccepted`を真にするcommit、案4の3の決定、`docs/WPD_DUAL_PREVIEW.md`・`docs/OPERATOR_GUIDE.md`の追従、実機の受入の別承認。

### ADR-0038の改訂案その2: PCの画面でカメラごとに設定を選べる形にする（Camera Control Pro 2に近い形。Proposed、所有者の承認待ち、#416）

- 状態: **Proposed。所有者の承認待ち**。上の改訂案（5項目→7項目、Aから写す）とは別の案で、どちらも承認までは決定ではない。承認までは、Acceptedの決定（書くのはCAM-Bだけ、Aから写すだけ）が効く。コードは書いていない（案だけ）。
- 発端: 所有者の言葉（#416）「WB もさわれるようにしてほしい」「アプリ上で色味補正が入るのは良いんだけど、最大限カメラ側で揃える努力はしたいからね」「ニコンのカメラコントロールプロと同じ設定を引っ張ってくる、でもいいよ」「最終的にソフト側から変えられる仕組みにしてないと詰む場面が出てくる」。条件は変わらない: 「カメラを壊すような設定は変えられないように。あくまで一般的なカメラ設定と同じ」。WBがプリセットの時の扱いは「要相談。一旦カメラコントロールプロと同じように」（所有者）。
- 目標: Nikon Camera Control Pro 2（以下CCP2）のように、PCの画面で**カメラごと（CAM-A・CAM-B）に**一般的な撮影の設定を選んで書ける形。「AからBへ写す」は、その上の便利な操作（Aの値をBにも入れる）として残す。実機で分かった事実は、実機の値として本書には書かない。
- 実装の状態: 無し。7項目の「写す」（上の改訂案）の部品が、試験用の表で動くところまで。製品の表の行は無効のまま。

**案A CCP2が変えられる設定（メーカーの公開の説明で確かめた範囲）**

出典は、メーカーの公開のオンラインヘルプ（英語版）のページだけ。SDKの同梱資料は使っていない。D810の欄は、ページの記述にD810が名指しされているか、除外の一覧に無いことによる。ページに書いていないことは「未確認」。ページの文章は写さず、設定の名前と対象機種だけを書く。

| タブ・メニュー | 設定（CCP2の名前） | D810についてページに書いてあること | 出典 |
|---|---|---|---|
| Exposure 1 | Exposure Mode | 現在のモードを表示する。本体で変えるしかない機種が名指しされているが、D810は名指しされていない。PCから変えられるかは**未確認** | [08_camera_controls_01](https://nikonimglib.com/ccp2/onlinehelp/en/08_camera_controls_01.html) |
| Exposure 1 | Shutter Speed、Bulb、Aperture | シャッター速度はM・Sモード、絞りはM・Aモードで調整できる。Bulbは、D3系・D700・D300系・D90・D7000・D5100・D5000を除く機種で使える（D810は除外されていない）。シャッター速度・絞りには、本体側のロックの設定がある | 同上 |
| Exposure 1 | Exposure Comp.、Flash Comp. | 露出モードによって使えるかが変わる | 同上 |
| Exposure 1 | Exposure Preview、AE Lock | D810でExposure Previewが使える | 同上 |
| Exposure 2 | Metering | D810は、CCP2から変えられる機種に挙がっている（D3系・D800・D700・D300系・Dfは、本体側の設定で決まる） | [08_camera_controls_02](https://nikonimglib.com/ccp2/onlinehelp/en/08_camera_controls_02.html) |
| Exposure 2 | ISO Sensitivity、Auto Control（Detail: 最大感度・最低シャッター速度） | ISOの自動制御を入れ切りできる。詳細で最大感度と最低シャッター速度を決める | 同上 |
| Exposure 2 | White Balance: 種類 | 蛍光灯は電球の型の一覧が出る。オートは、D810でオートの種類の一覧が出る | 同上 |
| Exposure 2 | White Balance: 色温度（Choose color temp.） | D810は専用のダイアログ。2500 Kから10,000 Kまで10 K刻み。緑-マゼンタの調整つき | 同上 |
| Exposure 2 | White Balance: Fine Tune | D810は、琥珀-青の軸が0.5刻み、緑-マゼンタの軸が0.25刻み。各軸で両方向に6段 | 同上 |
| Exposure 2 | White Balance: プリセット | プリセットを選び、コメント（36文字まで）をEditで編集できる。プリセットが保護されていると、Fine tune・Editが使えない。**番号の名前（d-0〜d-4など）はページに書いていない（未確認）** | 同上 |
| Exposure 2 | White Balance (LV) | D810で使える。PCのモニターのためのWBで、写真には影響しない | 同上 |
| Camera メニュー | Measure White Balance | PCの側からダイアログを開き、本体を取扱説明書のとおりに構えてOKを押すと、プリセットの白を測れる。複数のプリセットを持つ機種は、測った値の保存先をダイアログの上のメニューで選べ、保護もできる。使えるのはP・S・A・Mモードのみ。本体の操作が要る範囲（白い物を写す構え）は、ページに詳しく書いていない | [10_camera_menu_01](https://nikonimglib.com/ccp2/onlinehelp/en/10_camera_menu_01.html) |
| Image Processing | Picture Control | 選ぶか、別ソフト（Picture Control Utility 2）で編集する | [08_camera_controls_05](https://nikonimglib.com/ccp2/onlinehelp/en/08_camera_controls_05.html) |
| Image Processing | Color Space、Active D-Lighting | ページに挙がっている（機種の除外の記述は無い） | 同上 |
| Image Processing | HDR Mode | D810で使える。詳細ボタンで調整する。「On (single photo)」は、1枚撮ると自動で終わる | 同上 |
| Image Processing | Long exposure noise reduction、High ISO NR | ページに挙がっている | 同上 |
| Image Processing | Vignette Control、Auto distortion control | D810で使える（Auto distortion controlはD3系・D700・D300系・D90が除外で、D810は除外されていない） | 同上 |
| Image Processing | Diffraction compensation | D810は挙がっていない | 同上 |
| Storage | Image Quality、Image Area、Image Size、記録先、スロットの設定ほか | D810でJPEG/TIFFとRAWのサイズ、Primary Slot Selection、Secondary Slot Functionが使える | [08_camera_controls_03](https://nikonimglib.com/ccp2/onlinehelp/en/08_camera_controls_03.html) |
| Mechanical | Release Mode、ブラケティング、AFエリア、フォーカスモード、フリッカー低減、レンズ、セルフタイマー、ミラーアップほか | D810で使える（Release Modeは「本体で操作を有効にする」設定の時は、本体でしか変えられない） | [08_camera_controls_04](https://nikonimglib.com/ccp2/onlinehelp/en/08_camera_controls_04.html) |
| Camera メニュー | カスタム設定、日時、著作権と画像コメント、IPTC、撮影バンク、ブラケティング、インターバルタイマー、ライブビュー、フラッシュ、HDMI、本体の操作など | メニューの項目名が、1〜2文の説明つきで並ぶ | [10_camera_menu_01](https://nikonimglib.com/ccp2/onlinehelp/en/10_camera_menu_01.html) |

- 一般的なカメラ設定にあたるのは、Exposure 1・Exposure 2・Image Processingの上の行。Storage（画質・サイズ・記録先）、Mechanical（AF・ドライブ・ブラケティング・レンズ）、Cameraメニュー（カスタム設定・日時・著作権・IPTC・インターバルなど）は、承認の条件（本体の動作や寿命に関わりうる設定は変えない）とADR-0038の範囲の原則に照らして、入れない（撮影の前の検査がJPEG Fine・FX Lを求めることも理由）。
- CCP2の設定とPTPのプロパティの対応は、名前による推定で、D810での対応は未照合（※）。CCP2が内部でどの命令を使うかは、公開の説明からは分からない（※未確認）。

**案B 番号と値を公開の資料で確かめられるものと、今の仕組みで扱えるか**

「番号・型・値」は、`third_party/PTP_PROTOCOL_REFERENCES.md`の固定commit（libgphoto2）の行のアンカー。「今の仕組み」は、段Aの読む表（`kD810SettingSpecs`）と、書く表（`kD810WriteItems`）の今の行。

| 設定 | 番号・型・値（公開の資料） | 今の仕組み | 一般の範囲の見立て |
|---|---|---|---|
| 露出モード | `0x500E` UINT16、1 M・2 P・3 A・4 S（照合済み） | 読む・書く行あり（Mだけ固定） | 内側。Mだけに固定のまま（合成の前提） |
| シャッター速度 | `0x500D` UINT32、1/10000秒単位（照合済み） | 読む・書く行あり。Bulb・Time・1秒超は拒否 | 内側 |
| 絞り | `0x5007` UINT16、値/100（照合済み） | 読む・書く行あり | 内側 |
| ISO、ISO自動制御 | `0x500F` UINT16、`0xD054`か`0xD16A` UINT8（照合済み） | 読む・書く行あり（自動制御はOFFだけ） | 内側 |
| ISO自動制御の最大感度・最低シャッター速度 | `0xD183`・`0xD164`（名前のみ: ptp.h#L2618・#L2589）。値の表はconfig.c#L12561-L12563（D90の値で埋めた表。D810は未照合） | 行なし | 内側の見込み。ISO自動OFFが前提なので、揃える必要は薄い |
| 露出補正 | `0x5010` INT16、値/1000 EV（照合済み） | 読む・書く行あり（ADR-0041側） | 内側 |
| 測光モード | `0x500B` UINT16、1〜4（照合済み） | 読む・書く行あり（上の改訂案） | 内側 |
| WBの種類 | `0x5005` UINT16（照合済み。値は上の改訂案のとおり） | 読む・書く行あり（上の改訂案） | 内側。蛍光灯の電球の型とオートの種類を決める別のプロパティは、公開の資料で確かめていない（**未確認**） |
| WBの色温度 | `0xD01E`。名前だけで、型と単位の出典なし（ptp.h#L2338、ptp.c#L6557）。CCP2のD810の説明は2500〜10,000 K・10 K刻み | 行なし | 内側。型・単位を実機の読み取りで確かめるまで書かない（案E） |
| WBの微調整 | `0xD017`〜`0xD01D`。型は本体ごとにINT8／UINT8／UINT16（config.c#L12331-L12347）。CCP2のD810の説明は琥珀-青0.5刻み・緑-マゼンタ0.25刻み・各方向6段 | 行なし | 内側。D810の型は未確認。型を実機の読み取りで確かめるまで書かない |
| WBのプリセットの選択 | 番号`0xD01F` UINT8、範囲、上流は書ける扱い（ptp.h#L2339、config.c#L12348）。意味（スロット番号か）は**未確認** | 行なし | 番号の選択は内側の見込み |
| WBのプリセットの白の中身 | `0xD025`〜`0xD029` UINT32。上流は書き込み関数なし、単位の記述なし（config.c#L12349-L12353） | 行なし。書く手段は確かめられていない | 書かない |
| WBのプリセットの名前（コメント） | `0xD020`〜`0xD024`、文字列（ptp.h#L2340-L2344） | 文字列は読み取りが拒否する | 入れない |
| WBを測る（Measure White Balance） | 公開の資料にあるのは操作の名前だけ: `0x9402`〜`0x9406`（ptp.h#L623-L627、vendor operation）。CCP2がこれを使うかは**未確認** | 送らない決まり（vendor operationを送らない） | 案Cで相談 |
| Picture Control（選択中の項目） | `0xD200`。番号のみ（ptp.h#L2702）、config.cに項目なし。型・値は**未確認** | 読む行あり（型はUINT8と仮定） | 選択は内側の見込み。型を確かめるまで書かない。編集は入れない |
| 色空間 | `0xD032` UINT8、0 sRGB・1 AdobeRGB（config.c#L12107、表はconfig.c#L4974-L4978） | 行なし | 内側 |
| Active D-Lighting | `0xD14E` UINT8。値の表は本体ごとに違う（D90系はconfig.c#L7589-L7597。D850は別の関数: config.c#L12467）。D810は**未照合** | 読む行あり | 内側。型と値を確かめるまで書かない |
| HDR | `0xD130` UINT8、On・Off（config.c#L12234） | 行なし | 入れない（撮影の動作が変わる）。Offのままを読んで確かめる側 |
| 長秒時ノイズ低減 | `0xD06B` UINT8、On・Off（config.c#L12137） | 行なし | 入れない（露光時間が伸びる。ADR-0041の下書きの2-3と同じ） |
| 高感度ノイズ低減 | `0xD070` UINT8、Off・Low・Normal・High（config.c#L12565。D90の値で埋めた表。D810は未照合） | 行なし | 内側の見込み。型と値を確かめるまで書かない |
| ヴィネットコントロール | `0xD0F7` UINT8、High・Normal・Moderate・Off（config.c#L12373、表はconfig.c#L8092-L8098） | 行なし | 内側。周辺の明るさに効く |
| 自動歪曲補正 | `0xD0F8` UINT8、On・Off（config.c#L12372） | 行なし | 内側。**幾何に効く**ので、校正した時と同じ値であることを前提にする |
| WB (LV)（モニター用） | `0xD1A7`（名前のみ: ptp.h#L2637） | 行なし | 入れない（写真に影響しない） |
| 画質・サイズ・記録先、フォーカス、ブラケティング、セルフタイマー、カスタム設定、日時、著作権、IPTCなど | — | 表に無い | 入れない（案Aの理由） |

- 「写すだけで足りる」物（測光、色空間、ヴィネット、自動歪曲補正、ADL、高感度NR、WBの種類で名前のある物）と、「PCで値を選ぶ必要がある」物（WBがプリセットの時の種類、色温度、微調整の値、プリセットの選択）がある。前者は、読むだけの表で両台が違うと分かった物から、「Aから写す」の枠に足せる（項目ごとに承認が要る）。後者は、本案の「選ぶ」の枠が要る。
- 番号の一覧表は上流のもので、D810の表ではない（出典の記録と同じ）。D810の型・値は、実機の読み取り（読むだけ）で確かめるまで書かない。

**案C WBのプリセットの扱い（所有者と相談。既定の案は置かない）**

CCP2はWBのプリセットを、**PCから白を測って保存先を選べる**形で扱う（案Aの表の「Measure White Balance」）。測る時は、本体を取扱説明書のとおりに構える操作が要る。プリセットのコメントの編集と保護もできる。アプリで同じことをするには、白を測る操作（vendor operation）を送る必要があり、今の決まり（`0x90C2`・`0x9207`ほかのvendor operationは送らない）の改訂が要る。

「両台で同じ白にそろえる」ための現実的な手順の案（どれもまだ決めない）:

1. **名前のある固定の種類にそろえる**: 両台をDaylight・Fluorescent・Tungsten・Flash・Cloudy・Shadeのどれかに書く（案D）。登録した白は使わない。今の仕組みで書ける。微調整が両台で違うと色は少し違う（微調整は別のプロパティ）。
2. **色温度の数値でそろえる**: 両台を同じ色温度にする。ケルビンで決められるので、1より揃えやすい見込み。型と単位の確認が先に要る（案E）。
3. **プリセットは各台で本体で測り、アプリは番号の選択だけ**: 白の測定は本体の操作にして、アプリは「どのプリセットを使うか」の番号（`0xD01F`）を両台で合わせる。vendor operationは要らない。ただし、同じ白を写して測った両台のプリセットが、同じ色になる保証はない。番号の意味と書けるかは未確認。
4. **アプリから白を測る（CCP2と同じ）**: 白い紙に両台を向け、アプリが両台に測定の命令を送る。同じ被写体を同時に測れるので、3より揃いやすい見込み。vendor operation（`0x9402`〜`0x9406`の名前が公開の資料にある。CCP2がこれを使うかは未確認）を送る決まりの改訂が要る。`0x9207`などと同じ種類の危険の議論を、別のADRで通す話になる。
5. **Aのプリセットの中身をBに写す**: 中身の`0xD025`〜`0xD029`は、公開の資料では書けない扱いで、単位も不明。成り立たない（※未成立）。

判断材料（見立て。決めるのは所有者）: 1・2は今の決まりの内側で、先に進められる。3は番号の意味が確かめられれば内側に入る。4は決まりの改訂が要るが、CCP2と同じ使い勝手で、Aの白を活かしたまま両台を揃えられる手段になりうる。5は成り立たない。上の改訂案の案2（AがPresetの時の(a)(b)(c)）は、この手順の選択と一緒に決める。

**案D 画面の形と、安全の決まりを両台に掛ける時の扱い**

1. 画面の形（案）: カメラごとの一覧。行は設定、列はCAM-AとCAM-B。各セルに「今の値」（読める形と生の値）と、選べる値の一覧。選べる値は、そのカメラが広告した値の一覧と、名前を確かめた値の一覧の両方に入るものだけ。自由入力は置かない。行ごとに「AをBにも入れる」の操作（今の「Aから写す」）を残す。変える予定の行には印を付ける。
2. 確定の画面: 台ごとに「今の値 → 書く値」を並べる。ADR-0038決定5の注意（自動では元に戻らない、1項目1回で再送しない）を最初の行に出す。Aを変えるので、基準のカメラが変わること、承認済みのcapture profileと食い違うと撮影が止まることも出す。**今の確定の画面は、窓の高さに入る行数（窓の最小の大きさで12行）に収める作りで、CCP2のように多くの設定を並べるには足りない**。一覧と確定の画面は、別の描画領域（スクロールのある一覧）にする必要がある（実装の論点）。
3. 書く順番: 台ごとに、Bを先、Aを最後（Aは合成とprofileの基準なので、最後に変える）。台の中は、表の順（露出モード → ISO自動OFF → ISO → シャッター → 絞り → WB → 測光 → ほか）。ADR-0041の下書きは「Aが先」と書いてあり、この点は食い違う。どちらを採るかは所有者の判断。
4. 今の安全の決まりを両台に掛ける: 命令は標準の`0x1016`だけ。各命令1回、再送なし、巻き戻しなし。書く前に、計画と**両台の前の値**を記録に書いてflushする（書けなければ1件も送らない）。両台とも、書く直前にLive停止・型・`GetSet`・広告された値を読み直す。書いた後に読み戻して一致／不一致／不明を出す。1件以上送った操作が一致で終わらなければ、撮影を止める。解けるのは、読み直して両台の止めの対象の項目がそろって一致した時だけ。再起動をまたぐ止めの扱いはADR-0038のとおり。台をまたぐ掛け金（先の台で失敗・不明になったら、後の台は書き込みで開かない）を使う。
5. **片方だけ変わった時**: Bが書けてAが失敗・不明なら、Bだけが変わった状態になる。画面は台ごとの結果を出す（「CAM-B: 一致（書いた）／CAM-A: 書いていない（または不明）」）。撮影を止める。自動では戻さない（前の値は記録にある）。解くのは、読み直して両台が揃った時だけ。続けるには、同じ操作をもう一度確定するか、本体で合わせる。Bの書き込みに失敗した時は、Aに1件も送らない。
6. 承認済みのcapture profileとの関係: 今のprofileの照合はWBを比べる（※要確認: `HardwareSingleCaptureProfileStore.cs`の記述による）。AのWBがPresetから別の種類に変わると、profileとの照合で撮影が止まる見込み。書いたことをprofileの承認の代わりにしない（ADR-0038不変条件6）ので、profileを承認し直す手順が要る。段Cの論点と同じ話で、本案では解かない。
7. 実機への書き込みは、承認と別に、対象（A・B）・項目・回数・中止の条件を承認してから行う（ADR-0030・0032と同じ扱い）。

**案E 色温度の数値指定（`0xD01E`）の型と単位を、読むだけで確かめる手順**

公開の資料には番号と名前しか無い。**番号は作らない・型は推測で確定しない**。すべて読むだけ（`0x1014`のPropDesc読み取り。書き込みは0）。

1. 段Aの読む表に、診断の行だけの項目として`0xD01E`（型は仮にUINT16、※仮定）を足す。型が違えば、本体が返した型つきで`type_mismatch`として出る（`0xD200`と同じ。合わせ込まない）。同じ形で`0xD01F`（UINT8）と、微調整の`0xD017`〜`0xD01D`も足す（型は本体ごとに違うので、実測の型つきで出す）。
2. 所有者が、**本体の操作で**WBを色温度の指定にして、本体の表示のケルビンを控える（アプリは書かない）。段Aの読み取りを1回流し、`0xD01E`の型・`GetSet`・形式・現在値・最小・最大・stepを見る。
3. 本体の色温度を別の値に変えて、もう1回読む。本体の表示（K）と現在値の対応から、単位と刻みを推定する。CCP2の説明（D810: 2500〜10,000 K、10 K刻み）と比べて、現在値がその範囲にあり、stepが10に当たるなら、単位はKと見てよい見込み（※推定。「確定」とは書かない）。
4. 微調整は、本体で琥珀-青と緑-マゼンタを動かして読み、範囲（各方向6段の見込み）と型を見る。
5. 両台で同じ手順を行い、同じ型・単位であることを確かめる。
6. 結果は、実機の値を書かずに、「型・単位・GetSet・書けるか」の結論だけを出典の記録に残す。公開の出典が無いまま、実機の測定だけで単位を確定する扱いにするかは、所有者の判断（承認の条件の「公開の資料だけ」との関係）。

色温度の数値指定を書く案は、型と単位が確かめられた後に、別の項目として改めて提案する。

**案F 段階の案（どの順で広げるか）**

0. **最初の段**: 「AからBへ写す」を使える所まで（上の改訂案の7項目。AがPresetの時の扱いは案2のとおり保留）。製品の表の行を有効にするのは、改訂の承認と実機の別承認の後。
1. **読むだけの表を広げる**（書き込み0）: 色空間・ヴィネット・自動歪曲補正・HDR・高感度NR・WBの微調整・プリセット番号・色温度・中央重点の範囲を、診断の行として足し、両台のどれが違うかを実機で読む。型は実測の型つきで出す。違う項目が分かってから、どれを揃えるかを決められる（案E）。
2. **WBの種類を選んで両台に書く**（案Dの画面の最小版: 一覧は1項目、両台に同じ値）。ADR-0041の最初の項目として扱う。案Cの1（名前のある固定の種類）で、Aがプリセットでも揃えられる。
3. **カメラごとに値を選ぶ**（CCP2に近い一覧）: ISO・シャッター・絞り・露出補正・測光などを、台ごとに選べるようにする。台ごとに違う値を書く計画は、gateの照合が重くなる（ADR-0041の下書きの「gateの変更」）ので、項目ごとに承認。
4. **色温度の数値と微調整、プリセットの番号選択**: 案Eで型・単位が確かめられた後に、項目ごとに承認。
5. **アプリから白を測る**（案Cの4）: 決まりの改訂のADRを別に通した後。通らなければやらない。

各段に、承認（項目ごと）、securityの確認、偽の本体の試験、実機の別承認が要る。

**案G ほかのADRとの整理**

- 上の改訂案（5→7項目、Aから写す。Aは読むだけ）は、そのまま別に承認できる。Aにも書く部分は、ADR-0041（値を選んで両台に書く）の側に置く。ADR-0041の下書きは本書にまだ入っていない（別の作業用の枝にあり、リモートには無い）ので、承認の前にmainへ入れる必要がある。
- 本案の段2以降は、ADR-0041の「項目ごとに新しいADR」に当たる。項目ごとの案は、WBの種類（段2）、WB以外の写すだけの項目（段1の結果の後）、色温度と微調整（段4）、白を測る操作（段5、別のADR）に分ける。
- AGENTS.mdの例外の文は、各ADRの承認ごとに人の手の改定が要る。

**所有者に決めてもらう事**

1. 目標の形: 「CCP2のように、PCの画面でカメラごとに設定を選べる」を最終形とし、「Aから写す」をその上の便利な操作として残すこと。
2. **WBがプリセットの時の扱い**（案C）。上の改訂案の案2の(a)(b)(c)と、案Cの1〜5のどれで進めるか。既定の案は置いていない。
3. 段の順番（案F）。特に、段1（読むだけの表を広げる）を先に進めてよいか。
4. 書く順番: Bが先・Aが最後（本案）か、ADR-0041の下書きの「Aが先」か。
5. 白を測る操作（vendor operation）を送る決まりの改訂を、別のADRで検討するか（案Cの4）。
6. 色温度の型・単位を実機の測定で確かめる案（案E）で進めてよいか。
7. 自動歪曲補正・ヴィネットのように幾何や周辺光量に効く項目を、揃える対象に入れるか（入れる場合は、校正した時と同じ値であることを前提にする）。
8. 一般的なカメラの設定と同じ範囲の見立て: 案Aの上の3タブの項目は、撮影者が本体でふつうに変える設定で、メーカーの制御ソフトで変えられる範囲の目安に入ると見立てる（CCP2の公開の説明で確かめた範囲。D810の可否が未確認の物は別）。本体の動作・寿命・firmware・記録先・カードには関わらない。Aに書くことで増える危険は、基準のカメラが変わること、承認済みのprofileと食い違って撮影が止まることで、壊す危険ではない。最終判断は所有者。
9. 実機の確認の別承認（対象・項目・回数・中止の条件）。最初はBだけの1回（※案）。

### 外す規則と外す範囲

本ADRが外すのは、下の表で「外す」とした行の、CAM-Bの5つのプロパティ（決定2）に書く部分だけ。段B（開発用プレビュー）と段C（本番の画面）で外す範囲が違うので、列を分けた。ほかの行の文面はそのまま効く。段Cの列は、段Cの前提条件が揃うまで効かない（今の文面が効く）。

| 規則 | 場所 | 段B（開発用プレビュー） | 段C（本番の画面） |
|---|---|---|---|
| `Do not change firmware or camera settings outside the documented test profile.` | AGENTS.md | 外す。開発用プレビューの中で、5つのプロパティをCAM-Bに書くことだけを例外にする | 外す。本番の画面の「写す」操作で、同じ5つをCAM-Bに書くことだけを例外にする。どちらの段もfirmwareとほかの設定は変えない |
| `This does not authorize capture, camera-setting writes, ...`（開発用のWPD二台ライブビューの文） | AGENTS.md | 外す。同上。ライブビューの経路（`AllowedD810PreviewRequest`）からは今までどおり書けない | 当たらない（開発用の文） |
| `No exposure/AF-mode setting writes, capture, or AF-point movement is included.`（#331のフォーカスの文） | AGENTS.md | 外す範囲は露出モードをMにする書き込みだけ。AFモードの書き込みとAF点の移動は禁止のまま | 当たらない（開発用の文） |
| `it does not permit 0x9207, camera-setting writes, card deletion/format, ...`（AGENTS.mdのno-AFのSDRAM撮影を認めた文。前の下書きでは#343の文と書いたが、文の中にIssueの番号は無く、`docs/WPD_STILL_CAPTURE.md`の#332の文と読める。※要確認） | AGENTS.md | 外す範囲は5つのプロパティだけ。撮影の経路の中では書かない（決定6）。`0x9207`、カードの削除・フォーマット、画像の再利用、シャッターの自動の再試行は禁止のまま | 当たらない（開発用の文。製品の経路の撮影の命令は、本番の経路をWPDへ移すADR（未承認の下書き）で扱う） |
| `This adapter can never dispatch capture/property writes.`（`AllowedD810PreviewRequest`の注記） | `d810_preview.hpp` | 外さない。ライブビューのgateは`0x1016`を拒否したまま。書き込みは別のgateだけが通す（決定9） | 外さない。本番の経路のライブビューと撮影のgateも`0x1016`を拒否する作りにする（※案） |
| 段Aの読むだけのgate（`D810SettingsReadGate`。`0x1016`・WITH_DATA_TO_WRITE・WRITE_DATAを常に拒否） | 段AのAGENTS.mdの1行と`wpd_settings_read.hpp` | 外さない。段Aのgateは段Bの後も書けない作りのまま | 外さない |
| 「Phase 0ではカメラ設定を人が固定し、ツールは変更しない」と、表の「露出・WB・画質等の変更: 未実装。書込実装・実機試験は別途承認して一項目ずつ行う」 | `docs/D810_PC_CONTROL_CAPABILITIES.md` | 本ADRが表の行の「別途承認」に当たる。Phase 0のCLI（`sdk-status`など）は変更しないまま | 同じ。Phase 0のCLIは変更しないまま |
| 「アプリからカメラの設定を書かない。案内は人が設定を確かめる形にする」ほか3か所 | `docs/M2_AUTO_OVERLAP.md`（8.2節の表、9.2節のP3、11節の12、16節の採らなかった案の表） | 開発用プレビューでは「Aの設定をBに写す」を案内の先にしてよい、と読み替える | 製品の案内でも「写す」を案内の先にしてよい。文面は段Cの設計で作る |
| ADR-0036決定2（Proposed、保留）の2「カメラの設定を書き換える命令と、3命令以外のvendor operationも禁止のまま」 | 本書 | 外さない。ライブビューの例外を広げない。本ADRの書き込みは標準のPTPの`0x1016`で、vendor operationではない。別の例外として本ADRだけで扱う | 同じ |
| ADR-0014（`sdk-status`は読むだけ） | 本書 | 外さない | 外さない |
| ADR-0023の「撮影設定はread-onlyのままとし、write操作は別承認まで追加しない」 | 本書 | 外さない | 外す。本ADRを「別承認」とし、`DualCamera`の本番の画面に「写す」操作だけを足す。`SingleCamera`では書かない（写す相手が無いため、※案） |
| ADR-0024の「アプリはcamera settingを書き換えない」 | 本書 | 外さない | 外さない（`SingleCamera`の`CAM-A`の契約で、書かない）。capture profileとの照合との関係は段Cの論点 |
| ADR-0028の「camera setting writeなし」 | 本書 | 外さない | 外さない。SDKのCaptureRecoveryOnlyの経路の規則で、本ADRの書き込みは撮影のtransactionの外でWPDの経路だけで行う |
| `FR-CAP-005`の「WPD open中のSDK API、Live View、capture、enumeration、settings writeはすべて禁止」 | `docs/PRODUCT_REQUIREMENTS.md` | 外さない | 外さない。ADR-0028のSDK Module保持中の規則として読む（※読み方、所有者の確認事項）。本ADRの書き込みはWPDのsessionを1台ずつ開いて行う（決定6） |
| 対象外の「別契約が承認される前のアプリからの撮影設定write」 | `docs/PRODUCT_REQUIREMENTS.md` | 外さない（製品の経路の行） | 本ADRを「別契約」とし、5つのプロパティの「写す」だけを対象外から外す |
| `FR-UI-002`の「read-onlyカメラ設定」 | `docs/PRODUCT_REQUIREMENTS.md` | 外さない | 外す範囲は`DualCamera`の「写す」操作だけ |
| 「カメラ設定はread-onlyで、実機write契約が承認されるまで変更ボタンを提供しない」 | `docs/OPERATOR_UI_SPEC.md`（画面の構成の節） | 外さない | 外す。「写す」操作だけを置く。置き場所（`カメラ設定`の保守画面か、撮影ダッシュボードか）は※要確認 |
| 「本分類は`SingleCamera`での撮影設定write禁止を含む既存の常時禁止を緩和しない」 | `docs/OPERATOR_UI_SPEC.md`（フォーカス操作の節） | 外さない | 外さない |
| 「アプリはカメラの設定を書き換えません」、10節の表の「カメラの設定を、アプリから変える｜禁止」 | `docs/OPERATOR_GUIDE.md` | 外さない | 外す。段Cの開始と同じ変更で、操作者の手順を書き直す |
| 「アプリはカメラの設定を書き換えず、撮影直前に承認済みの capture profile との一致を確かめる」 | `docs/PILOT_CAPTURE_PROCEDURE.md` | 外さない | 前半だけ外す。capture profileとの一致の確認は残す |

### 決定

**範囲の原則**（所有者の承認の条件、#416）: 下の決定1〜14は、すべてこの原則の内側で読む。

- 対象: 書き込みの対象は、カメラ本体の操作で撮影者がふつうに変える露出の設定（露出モード・ISO感度自動制御・ISO感度・シャッター速度・絞り）に限る。本体の動作や寿命に関わりうる設定、サービス用・調整用の項目、firmware、カスタムメニューの保存や初期化、時計、記録先、カードのフォーマット、清掃・ミラーアップ、そのほかここに挙げない項目は、書き込みの対象にしない。プロパティごとの扱いは決定2の表で固定する。
- 対象を足す時: 範囲の目安は、メーカーの撮影用の制御ソフトで撮影者が変えられる撮影の設定とする（所有者の方針、#416。※読み方、所有者に確認中）。目安の内側の項目でも、本体の動作や寿命に関わりうる項目は足さない。足す時は項目ごとに新しいADRにし、決定3〜14をすべて当てる。項目の番号と意味の根拠は、これまでどおり公開の資料だけとする（決定13、不変条件8）。
- 有効にする手段: 書き込みを有効にするか、どの宛先・項目・codeに書けるかは、リポジトリの中の固定の表（コンパイル時の定数）だけで決める（決定13）。表を変える手段は、レビューを通したcommitだけ。実行時のファイル・環境変数・レジストリ・コマンドラインの引数は、絞る方向（止める）にだけ効かせ、宛先・項目・値・codeを広げる向きには効かせない。絞る側の入力が読めない・壊れている時は、止める側に倒す。承認や「実機で確かめた」に見えるファイルを置いても何も有効にならないことを、試験で固定する（securityの確認、HIGH 1）。
- 値: 書くのは、CAM-B自身が受け付けると広告した値の一覧（PropDescのEnumなら値の一覧、Rangeならmin〜maxでstepに乗る値）にある値だけ。Rangeの`step`が0以下の時と、FormFlagがNoneの時は書けない扱いにする。一覧に無い値、範囲の外の値、シャッター速度のBulb（`0xFFFFFFFF`）・Time（`0xFFFFFFFD`）、長時間の露光になる値は、gateが送る前に拒否する（送信0）。長時間の露光とみなす上限の秒数は所有者が決める（※要確認）。決まるまでの既定はコードの定数で置き、未定を上限なしと扱わない（※案: 1秒）。Bulb・Timeは、決定13で選んだcodeの符号化で判定する。決定2〜4はこの値の条件を引く。
- 命令: 書き込みに使う命令は、標準のPTPの設定の命令（SetDevicePropValue `0x1016`）だけ。`0x90C2`（ChangeCameraMode）を含むvendor operationと`0x9207`は送らない（決定2・14）。書くために別の命令が要ると分かった時は、例外を広げずに止める（決定14）。

1. **範囲と段**: 書き込みは次の2つの段で入れる。どちらの段も、同じgate（決定9）、同じ5つのプロパティ（決定2）、同じ条件（範囲の原則と決定3〜14）で書き、段ごとに条件を緩めない。
   - 段B（開発用プレビュー）: 既定OFFのCMake option `A0_BUILD_WPD_DUAL_PREVIEW`を付けたビルドの開発用プレビュー`A0CameraStitcher.WpdDualPreview`の中で書く。書き込みの経路（設定の読み書きのchannel、決定9のgate、「写す」の操作）は、`A0_BUILD_WPD_DUAL_PREVIEW`とは別の既定OFFのbuild optionの内側に置く。OFFのbuildには、書き込みのchannelも「写す」の操作も入らない（securityの確認。optionの名前は段Bの設計で決める、※要確認）。実機に向けるbuildでこのoptionをONにするのは、段Bの前提条件の「実機に書く前」が揃った後だけ。
   - 段C（本番の画面）: 本番の画面（OperatorShell）の`DualCamera`から、操作者が同じ「写す」操作で書く。書く処理は本番の経路のWPDの実装の中に置く（※前提: 別のADRの承認。本番の経路をWPDへ移すADRは未承認の下書き、#351）。段Cは、段Bを終え、本番の経路がWPDに移り、製品の経路の実機の受入を通った後に始める（※仮定。所有者の回答（#416）で下書きの案として示された順）。gateとカメラのI/Oは、leaseを持つprocess（Camera Agent）だけに置く（段Cの前提条件の(a)）。
   - どちらの段も、SDKの経路、Phase 0のCLI、`SingleCamera`には入れない（`SingleCamera`は※案）。書き込み用のコマンドラインの入口は作らない。段Bの入口は画面の「写す」操作だけ。段Cでは、画面の「写す」操作がCamera Agentのprotocolを通るので、Agentのprotocolも入口になる。段Cの操作は新しいschemaにし、確定した計画の識別子を必須にし、Agentが受けた時に決定3〜14の条件をすべて見直す（Agentのpipeは対話ログオンのSIDだけに開き、remoteを拒否するが、同じログオンのprocessからは呼べるため）。
2. **書いてよいプロパティと値**（code ごとにgateで固定する。M-3）:
   - 露出モード（`0x500E`）: M（値1）だけ。Bが`GetSet=1`を返した時だけ書く。
   - ISO感度自動制御（`0xD054`、候補`0xD16A`）: OFF（値0）だけ。
   - ISO（`0x500F`）、シャッター速度（`0x500D`）、絞り（`0x5007`）: Aから読んだ値で、範囲の原則の値の条件に合うものだけ（シャッター速度のBulb・Time・長時間の露光は拒否）。
   - WB（`0x5005`）、測光（`0x500B`）、露出補正（`0x5010`）、ピクチャーコントロール（`0xD200`）、アクティブD-ライティング（`0xD14E`）は読むだけ。
   - 上の表に無いプロパティと任意のプロパティは書かない（範囲の原則の対象）。カードの削除・フォーマット、firmware、通信の設定、カスタムメニュー全般（c4を含む）の命令、`0x9207`、ChangeCameraMode（`0x90C2`）は送らない。
3. **向きと値の出どころ**: AからBへだけ写す。B→Aの向きと、操作者が項目ごとに値を選ぶ入力は作らない（所有者の決定、#416・#351。値を選ぶ機能は、入れるなら将来の別のADRで扱う）。書く値は、確定した計画（決定5）の値だけ。計画はAから読んだ値から作り、範囲の原則の値の条件（Bが返したPropDescの一覧にある値）に合う値に限る。確定の後、最初の`0x1016`の前に、同じ操作の中でAの値とBのPropDescを読み直す。読み直した値が計画と1つでも違えば、1件も送らずに止め、計画を作り直す（操作者がもう一度確定する）。読み直した新しい値を書くことはしない（操作者が確かめていない値になるため）。
4. **写す条件**: AがM（`0x500E`=1）、ISO感度自動制御がOFF、シャッター速度がBulb・Timeでない時だけ写す。満たさなければ1件も送らず、満たさない項目を画面に出す。ISO・シャッター速度・絞りをBに書くのは、Bの露出モードがMで、BのISO感度自動制御がOFFになっていることを読み直して確かめた後だけ（M-3）。
5. **書く順番**: 露出モード → ISO自動OFF → ISO → シャッター速度 → 絞り。差のある項目だけを書き、差が無ければ何も送らない。送る前に、項目・Bの今の値・書く値の一覧を操作者に見せ、操作者が確定した時だけ書く。自動では前の値に戻らないことを、確定の画面に必ず出す。gateが持つのは、確定した計画（codeと値の組）だけで、Armできるのは確定した計画にある(code, 値)の組だけ（M-3）。計画に無い組のArmは送信0で拒否する。
   - 読む段と書く段を分ける。読む段（期限30秒、※仮）でAとBを読んで計画を作り、sessionを閉じる。操作者が確定するのを待つ間はsessionを開いておかない。確定の後に書く段（期限60秒、※仮）を始め、決定3の読み直しから進める。
6. **書いてよい時**: 両台のLiveが止まりworkerがjoinしてsessionがClose済みで、Bの`0xD1A2`（Live状態）を読んで0の時だけ。撮影のtransactionが無く、停止要求が無く、期限内であること。`Capturing`・`Stitching`の間は「写す」を無効にする。カメラを使う処理の持ち主（なし／Live／撮影／設定）を1つのatomicで表す（M-4）。持ち主が移る時は、今の値を比べてから入れ替える（compare-and-swap）。持ち主になれなかった要求は送信0で拒否する。設定の操作中はLiveの再開、Discover、撮影、AFを受け付けない。段Cでは、この値をカメラのI/Oを持つprocess（Camera Agent）の1か所だけに置き、2つの経路（SDKとWPD）が残る間はSDKのModuleの保持（ADR-0028）も持ち主に含める（段Cの前提条件の(a)・(f)）。AとBを同時に開かない。lease の下で、Aを開いて読み直して閉じ、それからBを開く。開く時は登録済みのidentityと照合し直す。
7. **1回だけ送る**: 各命令は1回だけ送る。再送、自動の巻き戻し（前の値に書き戻すこと）はしない。途中の項目で失敗したら、残りの項目は送らない。1回の「写す」操作で、1つのcodeに書くのは1回まで（M-1）。
8. **読み直しと撮影の停止**: 各項目を書いた後にBのPropDescを読み直し、今の値が書いた値と一致するかを確かめる。結果は一致／不一致／不明の3つにする（M-2）。「送った」は、`0x1016`のbeginをtransportに渡した時点とする。渡す処理が例外で終わった場合も送ったに数える（撮影のchannelが`0x90C0`をExchangeの前に「試みた」にするのと同じ作り。`wpd_still_capture.hpp`の`Dispatch`）。1件以上送った操作が一致で終わらなければ、二台撮影とA・Bの単独撮影のボタンを無効にし、撮影の要求（`RequestCapture`）でも拒否する。戻すのは、操作者が「読み直す」を押して、5項目すべて（決定2で書く5つ。計画にあった項目だけではない）がAとBで一致した時だけ（所有者が下書きを見て確かめた条件、#416。「次の『写す』が一致で終わった時」は戻す条件に入れない）。結果が「不明」「未確認」の間は「写す」を無効にし、「読み直す」で状態が分かった後だけ押せるようにする。
   - 段B: ウィンドウを閉じると停止は解ける（所有者の決定5。段Bだけに限る、#416・#351）が、終わりの記録が無い操作は次の起動で「未確認」と出す（決定11、M-8）。
   - 段C: 止めはOperatorShellとCamera Agentの再起動、Agentの寿命切れ、照合の失効をまたいで持ち越す。解けるのは読み直して5項目が一致した時だけ（所有者の決定、#416・#351。段Cの前提条件の(b)〜(d)）。
9. **gateを分ける**: 書き込みは、設定の読み書き専用のchannelと、新しいgate（設計の案の名前は`D810SettingsWriteGate`）だけが通す。既存の許可リスト`AllowedD810PreviewRequest`は広げず、ライブビューのworkerは構造上`0x1016`を送れないままにする。段Aの`D810SettingsReadGate`も書けない作りのまま残す。段Cでも同じgateを使い、本番の経路の撮影とライブビューのgateに`0x1016`を足さない。gateは1つのsessionに1つの状態機械（Idle／ReadOpen／WriteBegun／WriteSent／Retired）とし、次を守る（M-1）。
   - `0x1016`のbeginを渡す前にArmを消費して世代を進める。書き込みのENDの後は世代を進め、次の`0x1016`の前にLiveの状態とPropDescの読み直しをgate自身が強制する。
   - transfer の context は、gateがbeginの応答から持つ。WRITE_DATAは、通した`0x1016`の直後の1回だけ受け付け、NUM_BYTES_TO_WRITE、TRANSFER_DATAの要素数、中身の3つを、計画の値の符号化と完全一致で比べる。データ長は型（UINT8・UINT16・UINT32）と一致させる。
   - operation番号と引数は32 bit全体で比べ、命令の種類（データ相なし／読み取り／書き込み）をoperationごとに固定する。
   - 失敗、結果不明、Cancelの後は、そのsessionで`0x1016`を送らない（Retired）。
   - 段Aの読み取りのgateの「本体が明示に拒否した時はIdleに戻って次へ進む」は、読むだけの命令の性質で、書き込みのgateに持ち込まない（securityの注意、#416）。書き込みでは、本体の拒否・応答の欠落・例外・タイムアウトのどれも「適用されなかった」を意味しない。そうなった項目の結果は「不明」とし（決定8）、gateはRetiredにする。その操作の残りの項目は送らない（決定7）。読み直しは結果を確かめるためだけに行い、読み直した後も次の項目へは進まない。失敗した項目を飛ばして次の項目を書くこと（項目をまたぐ前進）は許さない。AとBが食い違ったまま残るため（決定7）。
   - WRITE_DATAで1 byteでもデータを送った後の拒否は、応答のcodeにかかわらず常に閉じる（読み取りのgateの、データの無いtransferの拒否を終わりとみなす扱いと逆）。拒否を判定する関数は、読み取りのgateのもの（段Aの`IsExplicitMtpRejection`）と共有しない。
   - 書き込みのchannelは、identityの読み取りのほかにWPDのContent・Properties・ResourcesのAPIを使わない。WPDのpropertyのAPI（`IPortableDeviceProperties::SetValues`）で設定を書く道を作らない。カードの列挙・削除・objectの作成を構造上呼べない作りにする。
10. **期限**: 読む段を30秒、書く段を60秒（どちらも※仮）の期限で囲む（決定5）。操作者が確定するのを待つ時間は、どちらの期限にも含めない（待つ間はsessionを開いていないため）。`0x1016`のbeginは、書く段の残り時間が、残りの項目の数だけの書き込みと読み直しに余裕を足した長さ以上ある時だけ通す。この余裕の値は段Aの実機の読み取りの時間から決める（※要確認）。beginを通した後のWRITE_DATAとENDは操作の期限で切らず、I/Oのwatchdogだけで縛る（M-6。データ相の途中で切ると本体が次の命令を受け付けなくなることがあるため。`ContinuePreviewFrame`と同じ考え方）。watchdogは、開発用の撮影と同じく、I/Oが12秒止まったらCancelを1回送る（`wpd_dual_preview_main.cpp`。値は※仮）。watchdogが働いた項目の結果は「不明」とする。
11. **記録**: 最初の`0x1016`の前に、計画（項目と書く値）とBの前の値を操作PC-1の中のprivateな記録に書き、OSのバッファの先まで書く（`FlushFileBuffers`かwrite-through）。この記録が書けなければ1件も送らない。各項目の後に、読み直しの値と結果（一致／不一致／不明）を足し、操作の終わりの行を書く。途中の項目や終わりの行が書けなければ、その操作の結果を「不明」とし、撮影を止める（決定8）。終わりの行が無い操作は「未確認」とし、次の起動時と読む画面に出す（M-8）。記録の項目はalias、項目、前の値、書いた値、読み直しの値、結果、時刻、操作ID。device ID、serial、digest、transferのcontext、生のbytesは書かない。記録は公開リポジトリとIssueに載せない。置き場所は`%LOCALAPPDATA%\A0CameraStitcher`の下にする（push前の漏れの検査`scripts/Test-ReplayFixtureLeak.ps1`がその下の記録を針にするため）。段Bと段Cで同じ記録を共有する（段Cの前提条件の(d)）。下の段は、その下のどのフォルダにするかを段Bの設計で決める（※要確認）。
12. **終わり方**: ウィンドウを閉じる時は、停止の旗を立て、設定のworkerの終わりとBのCloseを待ってからleaseを手放す（M-5）。停止の旗は操作ごとの専用のもので、立てたら戻さない（M-4）。
13. **codeの固定と、有効にする表**: ISO感度自動制御のcode（`0xD054`か`0xD16A`）と、シャッター速度のcode（標準の`0x500D`かNikon独自の`0xD100`）は、段Aの実機の読み取りで確定した1つだけを、リポジトリの中の固定の表（コンパイル時の定数）に入れる。実行時に候補を探したり、片方が失敗したらもう片方へ切り替えたり、ファイルや引数でcodeを選んだりしない（M-7、securityの確認のHIGH 1）。
   - 書き込みを有効にするか、どの行を有効にするかも、この表だけで決める。有効にする手段は、表を変えるcommit（レビュー付き）だけ（範囲の原則の「有効にする手段」）。
   - codeが確定して表にcommitされるまで、ISO感度自動制御とシャッター速度の行は無効にし、仮の既定のcodeを置かない。決定4はBのISO感度自動制御を確定したcodeで読むので、それまではISO・シャッター速度・絞りも書けない状態が正しい。
   - 表の各行は、宛先（CAM-Bだけ）、code、型、書いてよい値の条件、確かめた機種とfirmwareの版を持つ（機種と版は※案。版の文字列は機体を特定しない）。本体の機種か版が表と違えば書かない。
   - 項目が本体で書けるかは、記録やファイルで判断しない。書く直前の読み直し（決定3）で毎回、PropDescの型が表と一致すること、`GetSet=1`であること、FormFlagがEnumかRangeであることを確かめ、1つでも外れれば送信0にする。書く値と型は、同じ操作の中で読み直したPropDescからだけ取る。
   - 公開の資料の表はメーカーごとで、機種ごとではない（`third_party/PTP_PROTOCOL_REFERENCES.md`）。取り違えたcodeがD810では別の意味を持つおそれがある（※未検証）。codeを実機の読み取りで1つに確定させるのはこのため。
   - `0xD100`の値の符号化（Bulb・Timeの印と値の単位）は、出典の記録に無い（※要確認）。`0xD100`を選ぶ時は、その出典を記録（段Bの前提条件8）してから有効にする。
14. **露出モードを書けない時**: Bが`0x500E`に`GetSet=0`を返した時や、書き込みを拒んだ時は、露出モードを書かずに操作を止め、「本体でMにしてください」と案内する（所有者の決定6）。公開の資料（digiCamControl、libgphoto2の`_put_Nikon_Bulb`）では、Nikonは露出モードを書く前にChangeCameraMode `0x90C2`を送っている（#416の設計）。`0x90C2`は送らない。書くために`0x90C2`や別のvendor operationが要ると分かっても、例外を広げずにそこで止め、改めて所有者の判断を仰ぐ。

### 段Bの前提条件

段Bの前提条件は、下の1〜8を3つの時点に分けて求める（securityの確認、MEDIUM 1）。所有者の承認の2（段Bを継ぎ目の位置合わせと並行して進めてよい。gate、撮影の止め、偽の本体を使う試験までを先に実装する）は、時点(i)の後の実装と試験を認めたものと読む。所有者の決定の8（本ADRのAccepted、AGENTS.mdの改定、securityのgateのレビューの後に着手する）は、この承認で、実装と偽の本体の試験については改められたと読む（※読み方。所有者に確認する。確認が取れるまでは、この安全側の読み方で扱う）。どの時点でも、前の時点の条件は満たしたままにする。

- (i) 実装と偽の本体の試験を始める前: 1と5。5は、偽のtransportの試験で固定する受入基準で、試験は実装と一緒に作る。
- (ii) mainに入れる前: 4（gateの設計レビュー。統合前のコードのレビューで兼ねてよい）。加えて、2（AGENTS.mdの改定）か、書き込みの経路を既定OFFのbuild option（決定1）の内側に閉じることの、どちらか。
- (iii) 実機に書く前: 2・3・7・8のすべて。(ii)でbuild optionを選んだ場合も、実機に書く前には2が要る。6は、試験の部分を(ii)までに、実機の部分を実機の受入で確かめる。

1. 本ADRがAcceptedになっている（#416で満たした）。
2. AGENTS.mdが、下の「改定の対象」の方向で人の手で改定されている。
3. 段Aがmainに統合され、実機の読み取り（`--settings-read-diagnostics`）で次を確かめている。`0x1014`の応答が`0x2001`か、`0x500E`・`0x500D`・`0x5007`・`0x500F`の`GetSet`と一覧、ISO感度自動制御が`0xD054`と`0xD16A`のどちらに答えるか、`0xD100`の有無、ISO・シャッター速度が本体の表示（EXIF）と一致するか。決定13のcodeはこの結果で決める。
4. securityの設計レビューの指摘M-1〜M-8が、gateの設計と試験に入っている（対応は決定の各項に書いた）。LOWの10件（撮影の起動点でも停止を確かめる、ボタンの処理の中で条件を見直す、Aを読むだけにする仕組みを型で固定、長い露光を写さない、受け付けない状態、試験の掃引、記録に出さないもの、段Aの分のAGENTS.mdの1行、実機の受入は1項目から、書き込みを外せるbuild option）は段Bの中で扱う。
5. 偽のtransportの試験で、#416の受入基準を固定している。許可外のプロパティ、PropDescに無い値、Live中・停止未確認・撮影中、単独や2回目のWRITE_DATA、データ長違いを、送る前に拒否する（送信0）。読み直しの不一致で撮影ボタンが無効になり、再試行しない。途中の失敗で残りを送らない。期限切れ・停止要求の後に新しい`0x1016`を送らない。差が無ければ送信0。ログに識別子が出ない。`AllowedD810PreviewRequest`は`0x1016`を拒否したまま。既存のCTestがRelease・Debugで合格する。
6. 書いた後に本体が送る`DevicePropChanged`（`0x4006`）は、今の`wpd_still_capture.hpp`のまま読み飛ばしてよい。書いた後にBの値が変わったことを見つける手段にはしない。それは撮影の直前の照合で5項目を読むこと（段Cの前提条件の(e)）を正とする。段Bでは、`0x4006`で次の撮影のpreflightが拒否しないことを試験と実機で確かめる。
7. 実機の受入の前に、対象（CAM-B）、項目、回数、中止の条件を別に承認する（ADR-0030・0032と同じ扱い）。LOWの指摘のとおり、最初は1項目から始める案（※案）。本ADRの承認は実機の受入の承認を含まない。
8. 本ADRで使う番号・意味・手順（`0xD054`か`0xD16A`、`0xD100`、`0x90C2`の扱い、書く順番、PropDescの読み方）の出典が、`third_party/PTP_PROTOCOL_REFERENCES.md`に公開の資料の固定のcommitの行のアンカーで記録されている。段Aのブランチの版には番号・型とPropDescの読み方の分があり、`0x90C2`の扱いと書く順番の出典はまだ無い（※要確認）。

### 段Cの前提条件と論点

段Cは次がすべて揃うまで始めない（※案。順番は所有者の回答の※仮定による）。

1. 段Bが上の前提条件のとおりに実装され、開発用プレビューで、別に承認した実機の確認を終えている。
2. 本番の経路の撮影とライブビューをWPDへ移すADRがAcceptedになり（※前提: 別のADRの承認。本ADRの承認の時点では未承認の下書き、#351）、本番の経路の撮影とライブビューがWPDに移り、製品の経路の実機の受入を通っている。
3. 下の「改定の対象」の段Cの行を承認し、AGENTS.mdを人が改定し、操作者向けの文書（`docs/OPERATOR_GUIDE.md`ほか）を書き直している。
4. securityの設計レビューを、本番の画面に広げた設計（下の論点を含む）でもう一度通している。M-1〜M-8の対応は、段Bと同じgateをそのまま使うことで引き継ぐ。
5. 本番の画面からの書き込みの実機の受入を、対象・項目・回数・中止の条件を決めて別に承認している（ADR-0030・0032と同じ扱い）。段Bの実機の確認は段Cの受入に数えない。
6. 次の(a)〜(h)を設計と試験に入れている（securityの確認、MEDIUM 5）。どの論点をどう解いても、この安全側の要件は守る。
   - (a) 書き込みのgateとカメラのI/Oは、leaseを持つprocess（Camera Agent）だけに置く。WPFはWPDを開かず、確定した計画の識別子だけを送る。
   - (b) 撮影の止めの正本は、ディスクに残る記録（決定11）とする。Agentは起動の時と撮影の要求を受けるたびに記録を読み、未解決の書き込みがあれば撮影を拒否する。記録が読めない・壊れている時も拒否する。記録を消すと止めが解ける作りにしない（securityの確認、MEDIUM 2）。記録が無いことを「未解決なし」と読まない。例えば、未解決の印を記録とは別の所（製品のjournalなど）にも残し、どちらかが未解決を示すか、どちらかが読めない時は拒否する（※案）。撮影の直前の照合(e)は、記録が消えたり書き換えられたりした時の最後の守りになるので、必須のまま残す。WPFのBlockerは表示のための写しとする（今の`CameraStateRequiresInspection`と同じ扱い。`src/m3/Foundation/OperatorWorkflow/OperatorWorkflowModel.cs`）。
   - (c) 止めは、Agentの寿命切れ（最長10分。`DualCameraAgentLifecycle.cs`の`AgentMaxLifetime`）、AgentやOperatorShellの再起動、照合の失効（`FR-CAP-002`）では解けない。解くのは読み直して5項目が一致した時だけ（所有者の決定、#416・#351）。止めをAgentのmemoryだけに持つと、10分以内に消えるため。
   - (d) 段Bと段Cで記録を共有する。開発用プレビューで未解決の書き込みを残したまま本番の画面を開いても、撮影は止まったままにする。
   - (e) 撮影の直前のprofileの照合（ADR-0024。WPDの経路での照合の作りは、本番の経路をWPDへ移すADRで決める）で、書き込みの対象の5項目を両台とも読む。Aの値がprofileから外れていた時に、その誤りをBへ写したまま撮影に進まないため。今の画面の`SettingsMismatch`のBlockerを、WPDの経路でも効かせるための条件。
   - (f) 2つの経路（SDKとWPD）が残る間は、持ち主の値（決定6）にSDKのModuleの保持（ADR-0028）も含める。
   - (g) 書き込みの操作は、段Cの受入を通るまで、製品のbuildとAgentのcapabilitiesで既定OFFにする。
   - (h) Agentの残りの寿命が、書く段の期限（決定10）に余裕を足した長さより短い時は受け付けない（撮影の受付に残り185秒を求める`FR-CAP-002`と同じ考え方。余裕の値は※要確認）。
   - 操作者の誤操作については、`Capturing`・`Stitching`の間は「写す」を無効にし（決定6）、自動では元に戻らないことを確定の画面に必ず出す（決定5）。
7. 段Bの前提条件の8（出典の記録）を満たしている。

本番の画面に広げた時の残りの論点（段Cの設計で決める。どれも※要確認）:

- 名前の出し方: 操作者の通常の画面はCだけで、CAM-A・CAM-Bの名前を出さない（ADR-0036決定1の6）。「AからBへ写す」を操作者にどう示すか（左・右など位置の言葉にするか、`カメラ設定`の保守画面にだけ置くか）。
- 状態ゲートへのつなぎ方: (b)の記録の写しを、`OperatorActionAvailability`と`FR-UI-002`の状態ゲートにどうつなぐか。終わりの記録が無い操作の「未確認」（M-8）を本番の画面のどこに出すか。
- 承認済みのcapture profileとの関係: Bへ写した後のBの値がprofileと違えば、撮影は止まる。写す操作とprofileの承認の順番、写した後にprofileを承認し直すか（不変条件6のとおり、書いたことをprofileの承認の代わりにしない）。
- 写す間の表示: 写す間はLiveを止めるので、製品のC（ADR-0036決定1）の表示が古くなる。その間の表示の扱い。
- 承認済みのcapture profileの露出モード: 文書の間で書き方が割れている。本書のADR-0024の周りの記述とコードの注記（`src/m3/OperatorShell/Hardware/HardwareSingleCaptureProfileStore.cs`の`ExposureMode(S)`）はS、`docs/CURRENT_STATUS.md`と`docs/`の引き継ぎの文書の1つはM。コードはSDKの経路の値を比べていて、その値の意味は確かめていない（※要確認）。段Cの前に確かめる。profileがSなら、決定4により段Cの「写す」は使えない。
- 記録: (b)の記録を製品の記録（journal）と同じ所に置くか、別のprivateな記録にするか。どちらも`%LOCALAPPDATA%\A0CameraStitcher`の下（決定11）。

### 実機の受入で必ず確かめる項目

段B（前提条件7の別の承認の後）:

1. 段Aの読み取り（前提条件3の項目: `0x1014`の応答、`GetSet`、ISO感度自動制御のcode、`0xD100`の有無、EXIFとの一致）。
2. 1項目から始める。書いた後のPropDescの読み直しが一致し、次の二台撮影のEXIF（ExposureTime・FNumber・ISO・露出モード）がAと一致すること。
3. 露出モードを`0x90C2`なしで書けるか（`GetSet`と応答のcode）。書けない時は、ほかの項目を1件も送らずに止まり、案内が出ること。
4. 書いた後の`0x4006`で、次のpreflightが拒否しないこと。
5. 書いた値が、電源を入れ直した後も残るか（※未検証）。
6. 書き込みのデータ相の途中での失敗（別に承認した1回。例えばUSBを抜く）で、結果が「不明」になって撮影が止まること。終わりの行の無い操作が、次の起動で「未確認」と出ること。その後、カメラが次の命令を受け付けるか。
7. 設定の操作中はLiveの再開と撮影が拒否され、Live中と撮影中は「写す」が拒否されること。
8. 記録にdevice ID・serial・digest・context・生のbytesが無いこと。push前の漏れの検査が通ること。

段C（段Bの項目に加えて）:

9. 未解決の書き込みがある時、Agentの寿命切れ・Agentの再起動・OperatorShellの再起動の後も撮影が止まったままであること。
10. 開発用プレビューで未解決の書き込みを残した後に本番の画面を開いても、撮影が止まること。
11. Bの5項目がprofileから外れていれば、撮影の直前の照合で撮影が止まること。

### 守る不変条件

1. ライブビューのgate（`AllowedD810PreviewRequest`）と段Aの読むだけのgateは`0x1016`・WITH_DATA_TO_WRITE・WRITE_DATAを拒否したまま。書き込みを通すのは決定9のgateだけ。段Cの本番の経路のライブビューと撮影のgateも`0x1016`を通さない（※案）。
2. 書くのはCAM-Bの決定2のプロパティだけ。Aには書かない。Aは読むだけで、読むだけであることを型で固定する（LOWの指摘、※案）。
3. Liveの最中と撮影のtransactionの中では書かない。設定の操作中はLiveも撮影も始めない。
4. no retry。命令を再送せず、自動で前の値へ戻さず、失敗した項目の後を送らない。
5. 一致を確かめるまで撮影しない。不一致・不明は撮影の停止で終える。
6. 設定を書いたことを、合成の品質、校正の合格、承認済みのcapture profile（ADR-0024の30日profile）の代わりにしない。製品の撮影の前の照合（ADR-0024）は今のまま。
7. 実カメラの識別子を記録・画面・ログ・公開リポジトリ・Issueに出さない。
8. 識別子と符号化は公開の資料（libgphoto2・digiCamControlの固定commit）で確かめ、コードを写さない。Nikon SDK同梱のMTP仕様書は使わない（#292・#326・#351の約束）。

### PTPの命令とプロパティ（出典と確定の状態）

命令は標準のPTPのGetDevicePropDesc（`0x1014`）、GetDevicePropValue（`0x1015`）、SetDevicePropValue（`0x1016`）。`0x1016`はvendor operationではない。プロパティの値は#416の設計が公開のlibgphoto2（固定commit `92171a3`の`ptp.h`・`config.c`・`ptp-pack.c`）で照合したもので、行のアンカーは段Aのブランチ`claude/issue-416-h1`の`third_party/PTP_PROTOCOL_REFERENCES.md`にあり、本ADRの承認の時点でmainに未統合。書く順番（決定5）、露出モードの前の`0x90C2`（決定14）など手順の出典も、番号と同じく出典の記録（段Bの前提条件8）の対象にする。`0xD200`の型はブランチの版でも照合していない（下の表の※未照合）ので、出典がそろうまで使わない。D810の実機で読んだ値は無い（※未検証）。

| 項目 | code | 型 | 符号化 | 本ADRでの扱い |
|---|---|---|---|---|
| 露出モード | `0x500E` | UINT16 | 1=M、2=P、3=A、4=S | Mだけ書く（決定2・14） |
| シャッター速度 | `0x500D`（Nikon独自の`0xD100`もある） | UINT32 | 1/10000秒。`0xFFFFFFFF`=Bulb、`0xFFFFFFFD`=Time | 有限の値だけ書く。この機種では `0x500D` に確定した（固定の表の2台分の行は無効のままで、計画がAの値を読み、Bulb・Time・1秒超を拒むために使う）。`0xD100` は符号化の出典が無く、表に入れられない（決定13） |
| 絞り | `0x5007` | UINT16 | F値×100 | 書く |
| ISO | `0x500F` | UINT16 | ISOの値 | 書く |
| ISO感度自動制御 | `0xD054`（候補`0xD16A`） | UINT8 | 0=OFF | OFFだけ書く。この機種では `0xD054` に確定した（`0xD16A` は使わない。固定の表の2台分の行は無効のままで、ISOの行を書く前にAとBの値を読むために使う）（決定13） |
| WB | `0x5005` | UINT16 | | 読むだけ |
| 測光 | `0x500B` | UINT16 | | 読むだけ |
| 露出補正 | `0x5010` | INT16 | | 読むだけ |
| ピクチャーコントロール | `0xD200` | UINT8（※未照合） | | 読むだけ |
| アクティブD-ライティング | `0xD14E` | UINT8 | | 読むだけ |

**codeの選び方と、この機種での確定**（決定13）: 公開の資料が名前を挙げている候補のうち、段Aの実機の読み取りで型・GetSet・形が合った1つだけを固定の表に入れる。資料に符号化の出典が無い候補は、表に入れられない（シャッター速度の `0xD100` は、入れるとbuildが通らない）。実行時に探すことも、切り替えることもしない。この機種では、ISO感度自動制御を `0xD054`（`0xD16A` は、実機の読み取りでbeginの失敗とENDの`0x200A`で答えた）、シャッター速度を `0x500D` に確定した。確定したcodeを持つ行のうち、有効にしたのはCAM-BのISOの1行だけ。ISO感度自動制御とシャッター速度の行は、codeを持つが無効のまま。

### 選ばなかった案

- 既存の`AllowedD810PreviewRequest`に`0x1016`を足す案。ライブビューのworkerが書ける作りになり、frameの取得の途中や停止の処理から書き込みが出る道ができる。gateを分けて、ライブビューのworkerからは構造上書けないままにする。
- 任意のプロパティを書ける汎用の書き込みの案。カードの削除・フォーマット、通信、カスタムメニューにつながる値を1つのgateで見分けることになる。項目ごとの表に絞り、足す時は範囲の原則のとおり項目ごとに新しいADRにする。
- 承認済みのcapture profileの値をBに書く案（#416の本文の案）。profileは製品の経路の照合（ADR-0024）のためのもので、書く値の出どころにすると、照合する値をアプリが自分で書くことになり、照合の意味が変わる。段Bでも段Cでも、Aから読んだ値だけにする。
- 合成の側の明るさの比（#415）だけで差を埋める案。比は使い続けるが、両台の差はシャッター速度とISOの違いから来ていて、設定をそろえる方が校正の目印の検出に直接効く（※推定。#374の撮り直しで確かめる）。
- 本体で人が合わせる案。吊り下げたままでは手が届かない。露出モードだけは書けない場合にこの方法へ戻す（決定14）。
- 書き込みを開発用プレビューだけに留める案（前の下書きの範囲）。所有者の回答（#416）の1で採らない。
- 段Cを段Bと同時に始める案。製品の経路はまだSDKの撮影とカードのspoolで、WPDの書き込みを足すとSDKとWPDの経路が混ざる。本番の経路がWPDに移って受入を通った後に置く（※仮定）。

### 改定の対象

本ADRはAcceptedだが、改定の対象の本文の書き換えは別の変更で行う（承認後の作業）。それまでは各対象の追記が本ADRへの改定を示す。AGENTS.mdは人が書き換えるので、追記も置かず、本文も変えない（書き換えの案は#416で所有者に示し、所有者が見てから反映する）。`.autodev/requirements/normalized.json`も本ADRの変更では書き換えず、要件の正規化で直す。段Cの行は、段Cの前提条件が揃うまで今の文面が効き、本文の書き換えは段Cを始める変更で行う。

| 対象 | 段 | 今の文面の要旨 | 改定の方向 | 追記 |
|---|---|---|---|---|
| AGENTS.md（上の表の開発用の3文。#331のフォーカスの文、二台ライブビューの文、no-AFのSDRAM撮影の文） | B | 開発用プレビューでもカメラの設定を書かない | 開発用プレビューの「Aの設定をBに写す」だけを例外として足す。文面は人が書き換える | 置かない |
| AGENTS.md（段Aの1行、段Aのブランチにある） | B | 段Aは書き込みを含まない | 段Aの文は残し、段Bの文を別に足す | 置かない |
| AGENTS.md（`Do not change firmware or camera settings ...`） | B・C | 記録された試験のprofileの外でカメラの設定を変えない | 開発用プレビューと本番の画面の「写す」の5つのプロパティを例外として足す。firmwareとほかの設定は変えないまま。文面は人が書き換える | 置かない |
| `docs/D810_PC_CONTROL_CAPABILITIES.md` | B・C | 露出・WB・画質等の変更は未実装、別途承認 | 露出の5項目（露出モード・ISO感度自動制御・ISO・シャッター速度・絞り）は本ADRで承認。段Bは開発用プレビュー、段Cは本番の画面。WBとほかの項目は未承認のまま | 表の後に置いた |
| `docs/M2_AUTO_OVERLAP.md`（8.2節の表、9.2節のP3、11節の12、16節の採らなかった案の表） | B・C | アプリは設定を書かない | 段Bで開発用プレビューの案内に、段Cで製品の案内に「Aの設定をBに写す」を足す | 11節の12の後に置いた |
| `docs/WPD_DUAL_PREVIEW.md` | B | 段Aの節（段Aのブランチ） | 段Bの節を足す（gate、記録、撮影の停止と解除） | 冒頭に置いた |
| ADR-0036 決定2（Proposed、保留）の2 | — | 設定を書き換える命令は禁止のまま | 変えない。本ADRへの参照だけを足す | 決定2の保留の注記の後に置いた |
| ADR-0023（操作の「撮影設定はread-onlyのままとし、write操作は別承認まで追加しない」） | C | 撮影設定はread-only | `DualCamera`の「写す」の5つのプロパティだけを足す。`SingleCamera`はread-onlyのまま | 置いた |
| `docs/PRODUCT_REQUIREMENTS.md`（対象外の「別契約が承認される前のアプリからの撮影設定write」、`FR-UI-002`の「read-onlyカメラ設定」） | C | 撮影設定のwriteは対象外、カメラ設定はread-only | 本ADRの「写す」だけを除く | 両方の行の後に置いた |
| `docs/OPERATOR_UI_SPEC.md`（画面の構成の節の「カメラ設定はread-onlyで…変更ボタンを提供しない」） | C | 変更ボタンを置かない | 「写す」操作だけを置く。置き場所は※要確認 | 置いた |
| `docs/OPERATOR_GUIDE.md`（「アプリはカメラの設定を書き換えません」、10節の表） | C | アプリは設定を読むだけ | 「写す」操作の手順と、書き込みがうまくいかなかった時の撮影の止め（再起動をまたいで残ること）と戻し方を書く。「5つのプロパティをBに書く以外は書き換えない」は残す | 置かない。操作者の手順なので、段Cを始める変更で書き直す |
| `docs/PILOT_CAPTURE_PROCEDURE.md`（アプリの経路の記述） | C | アプリは設定を書き換えない | 前半を改め、capture profileとの一致の確認は残す | 置かない。段Cを始める変更で書き直す |
| `.autodev/requirements/normalized.json`（「camera-setting writes without a separately approved contract」を対象外とする行、「Keep camera settings read-only」） | B・C | 別に承認した契約が無いカメラの設定の書き込みは対象外。カメラの設定は読むだけ | 本ADRを「別に承認した契約」とし、5つのプロパティの「写す」だけを除く。本体は要件の正規化で直す | 置かない |

承認後に確かめる対象（追記は置かない）: ADR-0014・0024・0028、`FR-CAP-005`（上の表の読み方）、`FR-STI-006`（カメラ設定の整合の判定）、`docs/PHASE0_TEST_PLAN.md`、`docs/APP_ARCHITECTURE_REVIEW_AND_IMPLEMENTATION_PLAN.md`、`docs/HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md`（段CでCamera Agentに操作を足す場合）。ADR-0014・0024・0028はPhase 0のCLIか`SingleCamera`かSDKの経路の文で、本ADRでは変えない。

### 範囲外と未決

- 段Cの時期と順番。本番の経路のWPDへの移行と実機の受入の後に置く案は※仮定（所有者の回答、#416）。
- 操作者が値そのものを選ぶ機能（5つのプロパティの外を含む）。入れない（所有者の決定、#416・#351）。入れるなら将来の別のADR。
- `SingleCamera`で書かないこと（※案）。
- WBとほかの項目の書き込み。範囲の原則のとおり、足すなら項目ごとに新しいADR。WBは#415で色と一緒に決める。
- 段Cの残りの論点（上の「段Cの前提条件と論点」）。
- 期限の値（読む段30秒・書く段60秒・watchdog 12秒）は※仮。beginを通す余裕の値と、Agentの残りの寿命の余裕の値は※要確認。長時間の露光とみなす上限の秒数は※要確認で、決まるまではコードの定数の既定（※案: 1秒）で縛る（範囲の原則）。記録の置き場所は`%LOCALAPPDATA%\A0CameraStitcher`の下で、その下のフォルダは※要確認。
- 書いた設定が本体の電源を切った後も残るか、本体の表示とEXIFがいつ変わるかは確かめていない（※未検証）。
- 範囲の目安（メーカーの撮影用の制御ソフトで撮影者が変えられる撮影の設定）の読み方は、所有者に確認中（※読み方）。

### 承認後の作業

- 改定の対象のうち追記を置いた対象（上の表の「追記」の列）は、本ADRの承認の変更で置いた。本文の書き換えは別の変更で行う。
- AGENTS.mdは人が改定する（段Bの前提条件2）。書き換えの案は#416で所有者に示す。`.autodev`の要件は正規化で直す。
- 段Aをmainに統合し、実機の読み取りで決定13のcodeを確定して本ADRの表に記録する。
- 段Bは、段Bの前提条件の3つの時点のとおりに進める。実装と偽のtransportの試験は時点(i)の後に並行で進め、gateの設計レビュー（前提条件4）と統合前のレビュー、独立の試験を経てmainに入れ、時点(iii)が揃った後に、別に承認した実機の受入に進む。その後に#374の撮り直し。
- 段Cは、段Cの前提条件が揃ってから、論点を設計で決め、securityのレビューを通して実装する。段Cの行の改定の対象の本文は、段Cを始める変更で書き換える。
