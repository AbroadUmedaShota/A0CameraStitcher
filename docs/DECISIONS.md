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
- timeout: pairとhybridは共通の180秒transaction watchdogを持ち、各open、capture、download、delete、closeへ残時間以下を渡す。PC保存では`.partial`書込み後とcanonical rename直前にも期限を確認し、期限切れ後は次のtransport、canonical rename、delete、成功状態へ進まない。期限前に確定済みのPC原本または未確定`.partial`は保持し、安全なcloseは期限後も試行できる。両台撮影の確認用合成（JPEGと報告）は原本とは別に扱う: JPEGを保存して検証するまでに期限を過ぎれば合成は作らず原本を残し、保存して検証した後に期限を過ぎても出来上がったJPEGと報告は消さず、画面に期限を過ぎたことを出す。検査（ゲート）が間に合わなければ未実施（not-run）として報告を書く（所有者の決定、#417）。
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
- 追記（2026-11-19、ADR-0046・Issue #437）: 不変条件6はADR-0046で改定（Accepted、2026-11-19）。開発用プレビューの固定の合成条件v3に限り、ライブビューのframeから推定したCAM-Bの位置（幾何で、画素ではない）が、原寸の推定が当たらない両台撮影のJPEGの位置になってよい。frameは今までどおり保存・公開せず、合成の画素は確かめた原画像2枚だけから作る。本文の書き換えは後の変更で行い、それまでは本追記とADR-0046が優先する。

## ADR-0036: ライブビューを承認済みrig profileで描く表示専用の合成Cにし、WPD経由のライブビュー3命令をvendor operationの例外にする

- 状態: 決定1 Accepted（2026-11-04、#396）／決定2 Proposed（改訂案。#326・#351の所有者の決定「本番でもWPDを使ってよい」に合わせて書き直した下書きで、所有者の承認待ち。元の前提条件だったNikon SDK使用許諾の読み直しは、改訂案では前提にしない）。決定1の実装は、#398・#399が関数と記録、#400（JPEG）と#401（開発用ビルドのC）は経路に接続済み、#402・#403は未実装。承認済みのrig profileも無いので、操作者の画面にCはまだ出ない（開発用ビルドのCだけが条件で描く）。決定2に関わる改定の対象は、今の文面がそのまま効く。AGENTS.mdは人が書き換えるので、本ADRでは本文を変えない。
- 起票日: 2026-10-14
- 決定日: 決定1 2026-11-04（#396）。決定2は未決定
- 関連: #307（本ADR）、#306（所有者の決定と技術の道筋）、#396（2026-11-04の所有者の決定）、#397〜#403（決定1の文書と実装。#403はCの画面で継ぎ目の列を決めて保存する操作）、ADR-0037（撮影ごとの位置・回転の補正をやめる。Accepted）、#292（B案）、#293（今のスコープ）、#291・#295（ADR-0035）、#280・#281・#282（合成のレンダラー・reader・製品の入口）、#274・#277（profileの選択と校正）、#46（撮影ごとの補正）、#43・#42・#45（リグ・品質基準・承認済みprofile）、#233（pilot）、`HG-0010`・`HG-0011`、#326・#351（決定2の改訂案の元になった所有者の決定）、#328〜#331（開発用のWPD二台同時ライブビュー）、#440（水準器）、ADR-0038（CAM-Aの露出の設定をCAM-Bへ写す書き込み。Accepted）、ADR-0039（製品の経路でのWPD経由の撮影。Proposed）、ADR-0044（ライブを開いたまま撮る探り）、ADR-0045（カメラと継ぎ目の呼び方）、ADR-0050（いつもの撮影用アプリを開発用の画面と同じ仕組みにする。Proposed）
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

### ADR-0036の所有者の決定（#326・#351）

所有者は#326と#351に同じ文面で次を決めた。

- 本番でもWPDを使ってよい。本番の経路は、開発用プレビュー（#330〜#343、#389、#405）と同じくWPD経由で撮影・2台同時のライブ・合成をそろえる方向で組む。
- 決定2（WPD経由のライブビュー命令、保留）を、この決定に沿って改定する。案を作り、所有者の確認を取る。
- 実装の根拠は公開の資料（libgphoto2・digiCamControlの固定commit、MicrosoftのWPDの資料）だけを使い、Nikon SDK同梱の資料は使わない今の約束は変えない。#326の論点のうち、所有者の決定で進めるのはこの範囲。
- 製品の経路で合成をつなぐのは、合成の品質（#385・#417）が出た後（所有者の指示「まず継ぎ目をしっかり合わせる」）。この項は、その後の所有者の回答（#351。ADR-0050の「所有者の要件と決定」）で置き換わる案: 正式な校正の前でも、合成を「未確認」と数付きで出す。#385の線は変えない。

所有者は#351で補足した。「本番でもWPDを使ってよい」は、ライブビューだけでなく撮影も含む。撮影はADR-0039で扱う。

Issue #326の残りの論点（SDK取得時に適用された契約の版、同梱の仕様書を独自の実装に使う権利、仕様由来のソースの公開の条件）は未解決のまま残る。改訂案は、同梱の資料を使わないことで、元の前提の(a)（同梱の仕様書を実装に使ってよい範囲）には依存しない。公開は、決定2の3の出典の記録による。決定2の本文はこの決定に合わせて書き直した改訂案で、元の文面（起票時の案）はgitの履歴に残る。改訂の要点は決定2の頭の表にある。

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

追記（2026-11-19、ADR-0046・Issue #437）: 決定1の5と、10の2つ目の副項（写像を撮影やframeの内容で動かさない）は、ADR-0046で改定（Accepted、2026-11-19）。開発用プレビューの固定の合成条件v3に限り、Cは確定した位置（CAM-Aに対するCAM-Bの姿勢と紙面。層2を含まない）を1つ持ち、その位置はCの推定（決めた3つの時だけ）、人の手の押下（条件の範囲の内だけ）、撮影の結果、条件の変化でだけ変わる。両台撮影のJPEGは、原寸の推定が当たる時はその推定で、当たらない時と手で直した時はこの位置で作る。Cの画像は今までどおり保存せず、合成の入力にしない。位置は窓の中だけで持ち、固定の合成条件とprofileには人が明示して保存した時だけ書く。本文の書き換えは後の変更で行い、それまでは本追記とADR-0046が優先する。

### 決定2: B案 — WPD経由のライブビュー3命令の例外（Proposed、改訂案。所有者の承認待ち）

保留（2026-11-04、#396）: 所有者は元の決定2を承認していない。前提条件の3（Nikon SDK使用許諾の読み直し）が済んでいなかったため。

追記（ADR-0038・Issue #416）: 下の2の「カメラの設定を書き換える命令…も禁止のまま」は変えない。ADR-0038（Accepted）のCAM-Bへの露出の5項目の書き込みは、標準のPTPのSetDevicePropValue（`0x1016`）を専用のgateで送るもので、vendor operationではなく、ライブビューの例外を広げない。ADR-0038の例外はADR-0038だけで扱う。

改訂案: 所有者の決定（#326・#351）「本番でもWPDを使ってよい」に合わせて、下の本文を書き直した。改訂案が承認されるまでは、改定の対象（ADR-0016・0017・0020・0028・0031・0035、`CON-003`、`FR-CAP-005`、`FR-LV-003`など）の今の文面が効く。開発用プレビューのWPD二台同時ライブビュー（#328〜#331）は、決定2が保留の間に、AGENTS.mdの所有者の指示（開発用に限ったWPD二台同時ライブビュー）で実装・実機確認されたもので（#351の表）、改訂案の承認はその経路の受入を変えない。いつもの撮影用アプリ（OperatorShell）に何をどの順で載せるかはADR-0050で決め、本決定はライブビューの命令の例外だけを扱う。

改訂の要点（元の文 → 改訂案）:

| 項 | 元の文の要旨 | 改訂案 | 変える理由 |
|---|---|---|---|
| 1 | ライブビューの3命令（開始・画像の取得・終了）だけをWPD経由で送ってよい例外にする。対象はD810の写真ライブビュー | 3命令の例外を、開発用プレビューと製品の経路の両方に効かせる。命令は開始`0x9201`・画像の取得`0x9203`・終了`0x9202`。前提の問い合わせとしてDeviceReady `0x90C8`も同じ例外に入れる（※要確認、所有者の確認事項） | 所有者の決定「本番でもWPDを使ってよい」。元の2の「前提として別のvendor operationが要ると分かったら止めて判断を仰ぐ」に当たる。開発用の実装で、開始の前に`0x90C8`の応答を確かめている（`docs/WPD_DUAL_PREVIEW.md`） |
| 1-a（新） | なし | 製品の経路のライブは、新しい専用のgateとchannelから送る。通す物を一覧で挙げ、ほかはすべて拒否する。このchannelはカードの列挙・削除の能力を持たない | securityの確認。今の製品のWPDのtransport（`src/phase0/wpd_transport.cpp`）にはMTPの命令の許可リストが無く、同じobjectがカードの画像を消す`DeleteRecoveredObject`を持つ |
| 1-b（新） | なし | 水準器の値は、`0x9203`で受け取ったframeのheaderから読む。水準器のために命令を足さない | 水準器を製品にも載せる（ADR-0050、#440）。開発用の水準器も同じframeから読む（AGENTS.mdの#342の行） |
| 2 | 撮影とカードに触る命令（`0x9207`を含む）、設定を書き換える命令、3命令以外のvendor operationは禁止のまま | 残す。WPD経由の撮影と設定の書き込みは本決定に含めない。製品の経路のWPD経由の撮影はADR-0039、カメラの設定の書き込みはADR-0038・0041。AF（`0x90C1`）は製品の経路に入れる（2-a）。近遠の微調整`0x9204`、AF枠の移動`0x9205`、ライブを開いたまま撮る探り（ADR-0044）は入れない | 所有者の決定は撮影も含む（#351の補足）。撮影の経路はADR-0020・0023・0024・0027・0028と`CON-003`の契約に触るので、ライブビューの例外と一緒にせずADR-0039に分ける。AFは所有者の決定（#416・#351。本体に手が届かないため）。AF枠と探りは開発用だけの指示（AGENTS.mdの#426の行、ADR-0044） |
| 3 | 前提条件は所有者による使用許諾の読み直し。済むまで実装と実機の試作に着手しない | 前提条件を「所有者が、公開のリポジトリで進めることを判断済み（#326）」に置き換える。実装の時の決まりとして、公開の資料だけを根拠にし、Nikon SDK同梱の資料を使わず、出典を固定のcommitで記録する | 所有者の決定（#326・#351）。同梱の資料を使わなければ、同梱の仕様書を使う範囲の論点には依存しない。公開は出典の記録による |
| 4 | 実機の試作の前に、対象・回数・中止の条件を別に承認する | 開発用の試作は済んだ（#328〜#331）。製品の経路の実機の受入の前に、対象・回数・中止の条件を別に承認する | 試作の段は過ぎた。製品の受入は開発用の確認と分ける（#351のR3） |
| 5 | B案のframeがSDK経由のframeと同じ範囲を写すかは試作で確かめ直す | 製品のCの比較の相手をSDK経由のframeから撮影画像に改める。WPDのframeが撮影画像の範囲を拡縮と平行移動だけで写すことを確かめる前は、製品の操作者の画面のCを「未確認」と数付きで出し、合格に数えない（ADR-0050） | 製品の経路でSDKのライブビューを使わなくなれば、比べる相手は撮影画像になる。確かめる前でも合成を出すことは所有者の回答（#351、ADR-0050） |
| 6 | 成り立たなかった場合の扱いは未決。それまではADR-0035の形 | 製品の経路の受入に通らなかった場合の扱いは未決のまま。通るまでは、製品の二台のライブビューはADR-0035の形（SDK、選んだ1台） | 変えない（※未決） |
| 7（新） | なし | 登録済みのカメラ（今はCAM-AとCAM-Bの2台）に1つずつのWPD sessionを同時に開くことを、ライブビューの間に限って認める。`FR-CAP-005`の「一つのWPD sessionだけ」を改定する | 2台とも動く映像には2つのWPD sessionが要る。#326の残作業にも「`FR-CAP-005`のWPD session上限を改定する」とある |
| 8（新） | なし | 製品の経路の各部（ライブ、撮影、合成）は別々の変更で入れる。合成は正式な校正の前でも「未確認」と数付きで出してよい。#385のゲートの線は変えない（ADR-0050） | 元の案は「合成をつなぐのは#385・#417の品質の後」。所有者の回答（#351）で置き換わる案 |

1. vendor operationのうち、D810の写真ライブビューの3命令（開始`0x9201`・画像の取得`0x9203`・終了`0x9202`）を、WPD経由で送ってよい例外にする。前提の問い合わせとしてDeviceReady `0x90C8`（データ相なし・引数なし）も同じ例外に含める（※要確認、所有者の確認事項）。例外は開発用プレビューと製品の経路の両方に効く。製品の経路で使ってよいのは、本決定の承認の後に、1-aのgateとchannelを作ってから。
   - 1-a（製品のライブのgateとchannel）: 製品の経路のライブは、新しい専用のgateとchannelだけから送る。今の製品のWPDのtransport（`src/phase0/wpd_transport.cpp`）には足さない。このtransportにはMTPの命令の許可リストが無く（`SendCommand`はvendor opcodeの問い合わせと標準の撮影命令の2か所だけ）、同じobjectがカードの画像を消す`DeleteRecoveredObject`（cleanup tokenの持ち主が`Content()->Delete`で消す）を持つため。gateが通すのは次の物だけで、ほかはすべて送る前に拒否する。
     - 上の4命令（`0x9201`・`0x9203`・`0x9202`・`0x90C8`）。引数の数と値、命令の種類（データ相なし／読み取り）をoperationごとに固定し、32 bit全体で比べる。
     - `0x1015`のうち、一覧で名前を挙げたcodeだけ。開発用のライブのgateは11個のcodeを読む（`src/phase0/include/a0/phase0/experimental/d810_preview.hpp`の`D810PreviewPropertyBytes`）。製品で読むcodeの値の一覧は別のIssueで決める。「別のIssueで決める」のはこの一覧だけで、一覧に無いcodeは拒否する。
     - 自分の転送のREAD_DATAとEND。gateがbeginの応答から持つ自分のcontextだけを通す（開発用のgateはcontextを見ずに通している。`d810_preview.hpp`の`AllowedD810PreviewRequest`）。
     - 2-aのAFは、2-aの案1ならこのgateの中で、案2なら別のgateで通す（※要確認）。
     - 拒否する物の例: `0x1016`、WITH_DATA_TO_WRITE、WRITE_DATA、`0x90C0`（ADR-0044の探りのように、ライブのsessionから撮る送り方を含む）、`0x9204`、`0x9205`、`0x9207`、`0x90C2`、`0x90CB`、2-aの条件の外の`0x90C1`。
     - このchannelは、カードの列挙・削除・objectの作成・propertyの書き込みを構造上呼べない作りにする。identityの読み取りのほかに、WPDのContent・Properties・ResourcesのAPIを使わない。cleanup tokenを持たない。
     - カメラはカメラの一覧（登録済みのalias）から扱い、gate・channel・記録の欄に台数を2に固定した書き方や、位置入りの名前を増やさない（ADR-0045）。
   - 1-b（水準器）: 製品の画面の水準器の値は、1-aの`0x9203`で受け取ったframeのheaderから、映像と同じframeに結び付けて読む。水準器のために命令もpropertyの読み取りも足さない。読み方と見せ方（向きと「今は平行」）は#440の決まりに従い、開発用と製品で同じ部品を使う（ADR-0050）。
2. 撮影とカードに触る命令（`0x9207`を含む）は引き続き禁止する。カメラの設定を書き換える命令と、上の4つと2-aのAF以外のvendor operationも、本決定では禁止のまま。開発用プレビューで使っている命令のうち、no-AFの撮影（`0x90C0`）を製品の経路に入れるかはADR-0039で扱う。近遠の微調整（`0x9204`）とAF枠の移動（`0x9205`）は製品の経路に入れない（入れるかは※要確認）。ライブを開いたまま撮る探り（ADR-0044）は製品の経路に入れない。カメラの設定の書き込みはADR-0038・0041で扱い、本決定の例外を広げない。上の4つとAFの前提として、設定の書き込みや別のvendor operationが要ると分かった場合は、例外を広げずにそこで止め、改めて判断を仰ぐ。
   - 2-a（AF、所有者の決定、#416・#351）: 本体に手が届かないので、製品の経路でもPCからAF（AFだけの命令`0x90C1`、引数なし）を送ってよい。AFと撮影を一緒にする命令は使わない。撮影はno-AFの`0x90C0`のまま（ADR-0039）。条件は次のとおり。
     - 排他: 撮影のtransactionの間、設定の書き込みの操作の間（ADR-0038決定6の持ち主の値）、そのカメラのframeの転送の途中には送らない。1台に同時に1件だけで、要求を溜めない。再送しない。
     - 送る前にライブのフォーカスモード（`0xD061`）を読み、MF（公開の実装の値3・4）なら送らずに理由を出す。前後にDeviceReady `0x90C8`を読む。
     - 結果: AFの完了の応答は、光学的に合ったことの保証にしない。DeviceReadyのOutOfFocusを成功にしない。完了が分からない時は、再送せず、両方のライブを止める（開発用のAFとAF枠の移動と同じ扱い。AGENTS.mdの#426の行。開発用と同じ部品で作るため、止める範囲もそろえる）。起動の時に自動でAFを送らない。
     - 期限: 開発用は操作の予算10秒と、同期の呼び出しの12秒のCancelのwatchdog（`docs/WPD_PREVIEW_FOCUS.md`）。製品の値は※要確認。
     - 開発用の経路で分かっている事実は`docs/WPD_PREVIEW_FOCUS.md`にある（カメラごとのworkerがframeの転送とENDを終えてからAFを処理する、反対側のworkerは続ける、MFなら送る前に止める、AFの完了を待つ間は共有のmutexを放す）。製品の経路では未実装・未受入。
     - gateとchannel（※要確認。どちらかを所有者が選ぶ）:
       - 案1: 1-aのライブのgateに、1回ごとのArmを付けた`0x90C1`を足す。Armは操作者の1回の操作で1つだけ立ち、送る前に消費する（撮影の`capture_armed`と同じ形）。ライブの間に、そのカメラのworkerがframeの転送とENDを終えてから送る。開発用の形に近く、AFの間も反対側の映像が続く。ただし、ライブのworkerの許可リストにframeの取得以外の命令が入る。
       - 案2: AFだけを通す別のgateとchannelを作る。AFの間はカメラを使う処理の持ち主（ADR-0038決定6の1つのatomic）を「フォーカス」に移し、そのカメラのライブのworkerは何も送らない。AFとDeviceReadyが終わったら持ち主をライブに戻す。ライブのgateは1-aの一覧のまま閉じたままで、ADR-0038決定9（ライブのworkerの許可リストを広げない）と同じ考え方になる。持ち主を移す分、作りが増える。
     - AFを使うには、レンズと本体をAFにしておく必要がある。今の運用は「2台ともMF」（ADR-0033の6節、`docs/OPERATOR_GUIDE.md`）で、MFのままならAFは送られない（上のとおり送る前に止まる）。この運用の改定は下の「決定2の改訂案で影響する対象」の表に置いた。
     - 2-aを製品で使い始めるのは、案1・案2のどちらかが決まり、そのgateがsecurityのレビューを通った後。それまでは、製品の画面のAFは無効の表示と理由のまま（`docs/OPERATOR_UI_SPEC.md`の「フォーカス操作」）。
3. 前提条件: 所有者が、公開のリポジトリで進めることを判断済み（#326）。実装・試験・文書の決まりは次のとおり（#326・#351の所有者の決定）。
   - (a) 命令・イベント・プロパティの番号と意味、送る順序・期限・応答の扱いなどの手順は、公開の資料（固定commitのlibgphoto2・digiCamControl、MicrosoftのWPDの資料）に同じ番号・意味・手順が載っている物だけを使う。公開の資料に見つからない物と、別の製造者の同じ番号と取り違えうる物は入れない。
   - (b) Nikon SDK同梱の資料（MTP仕様書を含む）は、実装・試験・文書の根拠に使わない（#326の既存の約束）。
   - (c) 出典は`third_party/PTP_PROTOCOL_REFERENCES.md`に、番号・意味・手順ごとに固定のcommit（タグではなく40桁のcommit）・パス・行で記録する。記録に無い物は実装に入れない。コード、コメント、識別子名、定数の並びは写さず、資料を転載しない。
   - 同梱の資料を使う必要が出た時は、#326の論点を先に決める。
   - 参照元のライセンスの表示は、確かめた形のまま写す。libgphoto2はリポジトリの`COPYING`がLGPL 2.1で、`ptp.h`の表示は「version 2 or any later version」（どちらの版かは文面の上であいまい）。digiCamControlはMIT（`third_party/digicamcontrol-NOTICE.txt`）。番号と意味の参照に留め、コードを写さずリンクもしない。
4. 製品の経路の実機の受入の前に、対象（機体と系列）、回数、中止の条件を別に承認する（ADR-0030・0032と同じ扱い）。開発用プレビューの実機の確認（#328〜#331）は、製品の経路の受入に数えない。本ADRの承認は実機の受入の承認を含まない。
5. 製品のCにWPD経由のframeを使ってよい。ただし、そのframeが撮影画像の範囲を拡縮と平行移動だけで写すこと（決定1の4、#309）を確かめるまでは、製品の操作者の画面のCは「未確認」と数付きで出し、合格・受入の数に入れない（ADR-0050）。確かめた後も、Cは表示専用のまま（決定1の5）。
6. WPD経由のライブビューが製品の経路の受入に通らなかった場合の扱いは決まっていない（※未決、所有者の確認事項）。通るまでは、製品の二台のライブビューはADR-0035の形（SDKで選んだ1台）のまま。
7. 登録済みのカメラ（今はCAM-AとCAM-Bの2台）に、それぞれ1つずつのWPD sessionを同時に開くことを、ライブビューの間に限って認める（`FR-CAP-005`の改定、※案）。同じカメラでSDKとWPDのsessionを重ねないこと、撮影の前にすべてのライブビューを閉じて確かめることは変えない（不変条件1・2）。開くのはoperator-session-wideのleaseの下で、登録済みのidentityと照合した後だけ。撮影のtransactionの間に2つのWPD sessionを開くこと（開発用の#389の形）はADR-0039で扱い、本決定の7には含めない。
8. 製品の経路は、ライブ、撮影、合成を別々の変更で入れる。合成は、正式な校正（承認済みのprofile）の前でも「未確認」と数付きで操作者の画面に出してよい（ADR-0050）。#385のゲートの線と、書き出しの止め（ADR-0037の不変条件9）は変えない。WPD経由の撮影を製品の経路に入れることは、本決定ではなくADR-0039（Proposed）で決める。ADR-0039はADR-0020・0023・0027・0028と`CON-003`の改定を伴う。

決定2の改訂案の実機の受入（4の別の承認の後）で必ず確かめる項目:

1. 製品のライブのgateが、1-aの一覧（4命令、決めた`0x1015`のcode、自分の転送のREAD_DATAとEND）と2-aのAFのほかを送らないこと（記録で0件）。
2. 2つのsessionを開閉でき、撮影の前に両方を閉じたことを確かめていること。
3. WPDのframeが、撮影画像の範囲を拡縮と平行移動だけで写すこと（5）。確かめるまでは、画面のCが「未確認」と出ていること。
4. AF: MFの時は送らずに理由が出ること。撮影・設定の書き込みの間は拒否されること。完了の応答とDeviceReadyが記録に残り、OutOfFocusが成功と出ないこと。
5. 水準器: 値を読むために、1-aの一覧の外の命令が0件であること（1-b）。

### 守る不変条件

決定1の承認（2026-11-04）で、4〜6はAcceptedとして効く。1〜3は既存のADR（0006・0028・0032・0035）の規則を写したもので、そのまま効く。1〜3のうち決定2の例外（WPD経由のライブビュー）に関わる部分は、決定2と一緒にProposedのまま。4の「撮影ごとの」補正をさらに狭め、撮影ごとの位置・回転の補正もやめる（倍率だけは上限以内で書き出すJPEGに当てる）改定はADR-0037（Accepted、2026-11-04）にある。4の本文の書き換えは後の変更で行う。

1. SDKとWPDのsessionを同じカメラで重ねない。別々のカメラの間でSDKとWPDを同時に開いてよいかは本ADRでは決めない（※未決）。決めるまでは、ADR-0028の「WPD open中はSDK operationを行わない」をそのまま守る。
2. 撮影の前に、すべてのライブビュー（WPD経由とSDK経由）を閉じ、閉じたことを確かめる。確かめられなければ試行に数えず、撮影を始めない（ADR-0035の不変条件3、ADR-0032の1-A）。
3. no retry。ライブビューの失敗で自動の開き直しをしない。原本を保持する（ADR-0006・0018・0020）。
4. 撮影ごとのhomographyの推定はしない（ADR-0021を維持）。Cでもframeごとに推定しない。profileを実行時に変更・学習しない。
5. ライブビューのframeとCを保存・公開しない。例外は所有者が認めたpilotの間の記録だけで、操作PC-1の中に限り、公開リポジトリとIssueに載せない。この記録を有効にする手段（既定OFFの開発用の設定など）とholdoutに使わないことは、承認後にNFR-SEC-001の改定とあわせて決める（※案）。
6. 画面に「同時」「同期」と書かない。シャッターの同期を保証しない（`CON-002`）。

決定2の改訂案との関係（Proposed）: 不変条件1〜3は改訂案でも文面を変えない。1の「同じカメラでSDKとWPDのsessionを重ねない」は、製品の経路をWPDだけで組んでも、SDKを使う経路（ADR-0025・0028・0035）が残る間はそのまま効く。登録済みのカメラに1つずつのWPD sessionを同時に開くことは決定2の7で扱い、SDKとWPDの重なりの規則とは別にする。「別々のカメラの間でSDKとWPDを同時に開いてよいか」は改訂案でも決めない（※未決）。決まるまでは、別々のカメラの間でも重ねない（AGENTS.mdの「Never overlap SDK and WPD sessions」）。`SingleCamera`と`DualCamera`を切り替える時は、片方のsessionがすべて閉じたのを確かめてから、もう片方を開く。

### 決定2の改訂案で影響する対象

改訂案がProposedの間は、どの対象にも追記を置かない（※案）。承認の時に、下の表の対象に「ADR-0036決定2で改定」の追記を置き、本文の書き換えは後の変更で行う。下の「改定の対象と、承認後の差し替えの案」の表の決定2の行（ADR-0016・0017・0020・0028、`CON-003`）は、改訂案でも方向を変えない（ライブビューの命令だけを例外とし、`0x9207`の禁止は残す）。改訂案で足す・変えるのは次の対象。

| 対象 | 今の文面の要旨 | 改訂案での改定の方向 |
|---|---|---|
| ADR-0031の実装前gate（1）と凍結の追記 | ライセンス下のSDK資料が二つの独立session／sourceによる同時Live Viewを許可すること。2プロセス×2 moduleのPoCは凍結 | 二台とも動く映像の実現方式をWPD（決定2）にし、gate（1）を「公開の資料だけを根拠にする」（決定2の3）に置き換える。SDKを2つ読み込む案の凍結はそのまま |
| ADR-0035の範囲外 | 二台とも動く映像、vendor operation | 決定2の承認で、WPD経由のライブビューの命令に限って範囲に入る。製品の受入までは本文の形 |
| `FR-CAP-005` | 同時に一つのWPD sessionだけを開く | ライブビューの間に限り、登録済みのカメラに1つずつのWPD sessionを認める（決定2の7） |
| `FR-LV-003` | SDK vendor documentationと実機PoCで二つの独立したsource/capture session | 実現方式をWPDにし、SDK資料の条件を外す |
| `FR-UI-003`、PRODUCT_REQUIREMENTSの「二台同時Live Viewは、ADR-0031のSDK capability・安全性gateを満たすまで有効化しない」の行 | SDKのgateが未達の間は二台同時を見せない | gateを決定2の受入（決定2の4）に置き換える |
| `docs/D810_PC_CONTROL_CAPABILITIES.md`の「二台同時Live View」の行 | 将来目標・ADR-0031のSDK capabilityの技術gate待ち | 開発用プレビューは実装・実機確認済み（#328〜#331）、製品は決定2の受入待ち |
| AGENTS.md（開発用のWPD二台同時ライブビューの文、`Never send vendor operation 0x9207 ...`の文） | 開発用に限る | 製品の経路でもライブビューの4つの命令を許す文を足す。文の案はADR-0050の「AGENTS.mdに入れる文」（確認点C4のとおり、今のAGENTS.mdに合わせて作り直した） |
| 受入の範囲（ADR-0030・0032、#227） | 製品の二台の受入はSDK撮影・WPD回収の経路 | WPD経由のライブビューの製品の受入を新しい系列として別に承認する（決定2の4）。開発用の確認と既存の系列には数えない |
| `HG-0010` | 二台とも動くライブビューをMVPに含めるか、実現方式 | 実現方式はWPD（所有者の決定、#326・#351）と記録する。MVPに含めるかは※要確認 |
| `FR-LV-001` | 「Nikon SDK経由の対話的Live View開始、継続frame取得、停止」 | 受入の後に製品のライブをWPDへ移すなら、実現方式の文をWPD（本決定）に直す。受入までは本文のまま |
| ADR-0033の6節（撮影設定。フォーカスは2台ともMF）、`docs/OPERATOR_GUIDE.md`の「2台とも、ピントを手動（MF）にする」と「アプリはピントの設定を確認も変更もしません」 | 2台ともMF。アプリはピントの設定を確かめも変えもしない | 2-aのAFを使う時は、レンズと本体をAFにする運用に改める。アプリはAFの命令を送るが、AFモードの設定は書かない（ADR-0038決定2のとおりAFモードは書かない）。改め方は所有者の承認の点 |
| `docs/PRODUCT_REQUIREMENTS.md`の対象外の「自動露出、オートISO、AFを使った撮影」 | AFを使った撮影は対象外 | 撮影はno-AFのまま（ADR-0039）なので「AFを使った撮影」は対象外のまま。撮影の前に別の操作として送るAF（2-a）を対象に足す（※読み方、所有者の承認の点） |
| `docs/WPD_PREVIEW_FOCUS.md` | 開発用のAF・微調整 | 製品のAFの節を足すか、別の文書にする。微調整は開発用のまま |
| `docs/OPERATOR_UI_SPEC.md`の「フォーカス操作」「常時禁止」 | 実機のフォーカス操作は承認まで無効。vendor operationは`0x9207`を含め禁止 | 2-aのgateが決まるまで無効のまま。決定2の承認の後、vendor operationの例外を本決定の4命令とAFに限って書く（ADR-0050の書き換わる所の一覧） |

承認後に確かめる対象にADR-0022の「承認済み実機撮影入口は…に限定する」（`docs/DECISIONS.md`のADR-0022の決定）を足す。製品のCamera AgentのWPDの撮影の入口にも及ぶかを確かめる（※要確認。撮影の入口はADR-0039）。

改訂案で変えないもの: ADR-0020の`0x9207`とspoolの規則、ADR-0023・0024の製品のSingleCameraの契約、ADR-0027・0028のCaptureRecoveryOnlyの経路（製品の経路のWPD経由の撮影はADR-0039、決定2の8）、`CON-004`（Nikon SDKを同梱しない）。

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

- B案の実現性: 起票の時は実機で確かめていなかった（工数は10〜20人日の見込み、※推定、#292の机上検討）。その後、開発用プレビューで実装・実機確認された（#328〜#331、#351の表）。製品の経路では未実装・未受入。
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

決定2の改訂案（#326・#351）で承認していただく点:

8. 決定2の改訂案の1〜8。特に、例外を開発用プレビューと製品の経路の両方に効かせること（1）、DeviceReady `0x90C8`を例外に含めること（1）、製品のライブを専用のgateとchannelから送り、通す物を一覧で閉じること（1-a）、水準器をframeのheaderから読み命令を足さないこと（1-b）、微調整・AF枠の移動・no-AFの撮影の命令とライブを開いたまま撮る探りを本決定では製品の経路に入れないこと（2）、前提条件を「公開の資料だけを根拠にし、Nikon SDK同梱の資料を使わない」に置き換えること（3）、正式な校正の前のCを「未確認」と数付きで出すこと（5・8、ADR-0050）。AFを製品の経路に入れることは所有者が決めた（2-a、#416・#351）。残るのは、AFをどのgateから送るか（2-aの案1・案2）、2台ともMFの運用の改め方、製品のAFの期限の値。
9. 登録済みのカメラに1つずつのWPD sessionを、ライブビューの間に限って同時に開く`FR-CAP-005`の改定（決定2の7）。
10. WPD経由の撮影を製品の経路に入れる判断を、本ADRではなくADR-0039にすること（決定2の8）。
11. 「決定2の改訂案で影響する対象」の表と、追記を承認の時に置く扱い。
12. 決定2の3の前提条件と、実装の時の決まり(a)〜(c)。ADR-0038・0039も同じ決まりに従う。

### 承認後の作業

- 状態をAcceptedにし、決定日を記入する。決定1は2026-11-04に済み。決定2は改訂案が所有者の承認待ち。承認されたら「決定2の改訂案で影響する対象」に追記を置き、製品の経路の命令の許可リスト（1-aの`0x1015`のcodeの一覧）と受入の系列を別のIssueで決める。
- 改定の対象の本文を、表の方向で書き換える。AGENTS.mdは人が差し替える。決定1に関わる対象は、2026-11-04（#397）に追記だけを「決定1で改定（Accepted）」に更新した。本文の書き換えは残っている。
- `HG-0011`の問いにCを含めて回答を求める。`HG-0010`との関係を所有者に確かめる。
- `.autodev`の要件と計画を正規化し直す。
- `third_party/libgphoto2-NOTICE.txt`を新しく置く（リポジトリ名、commit、`COPYING`がLGPL 2.1であること、`ptp.h`の著作権の表示者、番号・意味・解析の手順の確かめにだけ使いコードを写さずリンクもしないこと）。digiCamControlの`NikonBase.cs`のほかに引くファイルにもハッシュを付ける。READMEに商標の注記を1行足す。リポジトリのライセンスが未設定であることを書く（ライセンスは所有者が決める）。

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

追記（2026-11-19、ADR-0046・Issue #437）: 決定1・2はADR-0046で改定（Accepted、2026-11-19）。開発用プレビューの固定の合成条件v3に限り、両台撮影のJPEGの幾何は、原寸の推定が当たる時はその推定（#417の今の作り）、当たらない時と手で直した時はライブの確定した位置にする。ライブのCの位置は、決めた3つの時の推定、人の手の押下（条件の範囲の内だけ）、撮影の結果、条件の変化でだけ変わる。推定の始め・事前分布・範囲の基準は条件の基準の姿勢のままで、継ぎ目の列は変えない。本文の書き換えは後の変更で行い、それまでは本追記とADR-0046が優先する。

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

追記（2026-11-19、ADR-0046・Issue #437）: 不変条件1・2・4はADR-0046で改定（Accepted、2026-11-19）。開発用プレビューの固定の合成条件v3に限り、JPEGの写像の姿勢は撮影ごとに原寸の推定かライブの確定した位置になり、ライブのCの位置は上の決定の追記の時に変わる。frameごとには変えず、継ぎ目の列も変えない。撮影の結果は、ライブの確定した位置として窓の中でだけ次の撮影へ持ち越し、写像・固定の合成条件・profileに書き戻さず、推定の基準にも入れない。本文の書き換えは後の変更で行い、それまでは本追記とADR-0046が優先する。

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

### ADR-0038の改訂: 「写す」の項目にホワイトバランスと測光を足す（所有者が決定。案2と案4の3は保留、#416）

- 状態: **所有者が、WBと測光を写す項目に足すと決めた**（所有者の言葉は次の行。#416への記録の形は※要確認）。実装は段1: AGENTS.mdの例外の文に反映し、コードの定数`kD810Adr38RevisionAccepted`を真にして、製品の表のCAM-BのWBと測光の行を有効にした。決定2の「WB（`0x5005`）、測光（`0x500B`）…は読むだけ」の行のうちWBと測光は、本節で置き換わる。**案2（WBがプリセットの時の扱い）と案4の3は保留・未決のまま**で、今の動きは「写さず、理由を出す」側。
- 発端: 所有者の言葉（#416）「5 は足しましょう。左右差が生まれてしまいます」。実機の読み取り（読むだけ）で、両台のWBと測光が違う項目として出た。実機の値は本書に書かない。
- 実装の状態: 計画・確定の画面・読み戻しの照合・gateは7項目で動く。製品の表のCAM-BのWBと測光の行は有効（段1。偽の本体で確かめた）。確定の画面のWBの行の注記は「種類だけを写します。微調整・自動の型・蛍光灯の型は写りません」。実機では、CAM-BのISO・ホワイトバランス・測光・絞り・シャッター速度を1回に1項目ずつ書き、どれも読み戻しが一致した。段2で絞りの行、段3でシャッター速度の行も有効にした（ADR-0038が承認した5項目のうち）。段3は、実機で本体の表示とPCの値を見比べる確認と、0xD100を読むだけの確認を済ませてからmainへ入れた。見比べでは、有限の値は本体の表示と同じ値で、BulbとTimeは名前で読めた。本体のX250は0x500Dでは1/250秒と同じ値で返るので、PCの表示と読み戻しの照合では普通の1/250秒と区別できない（0xD100は別の値を返すが、書き込みの経路は0xD100を読まない）。1/1250秒より速い値は分子が1でない分数で表示され、本体の表示とは書き方が違う。

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
- 写すのは「種類」だけで、WBの微調整と測光の付随設定（中央重点の範囲）は写らない。確定の画面のWBの行に「種類だけを写します。微調整・自動の型・蛍光灯の型は写りません」と出す（行数は増やさない）。
- 写さない物（所有者の決定: 写せる値の一覧にある Automatic と Fluorescent は残す）: Automaticの時のオートの型（`0xD141`、ptp.h の `PTP_DPC_NIKON_WBAutoType`）と、Fluorescentの時の蛍光灯の型（`0xD14F`、同 `PTP_DPC_NIKON_FlourescentType`）も写らない。同じ数の種類でも、型が違えば色が違いうる。番号は公開の出典（libgphoto2 の ptp.h、固定commitの 2562・2571 行）の定義のとおりで、D810がこの2つを持つか・どう答えるかは確かめていない（※未検証）。確定の画面の注記にこの2つを足した（WBの行を有効にする前に入れる、という所有者の決定による）。

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
| 露出補正 | `0x5010` INT16、値/1000 EV（照合済み） | 読む・書く行あり（無効。ADR-0041の項目に入らず、別の決定が要る） | 内側 |
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
| 長秒時ノイズ低減 | `0xD06B` UINT8、On・Off（config.c#L12137） | 行なし | 入れない（露光時間が伸びる。ADR-0041の「増える危険」の撮影の前提との衝突と同じ） |
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
3. 書く順番: 台ごとに、Bを先、Aを最後（Aは合成とprofileの基準なので、最後に変える）。台の中は、表の順（露出モード → ISO自動OFF → ISO → シャッター → 絞り → WB → 測光 → ほか）。所有者はBが先・Aが最後に決めた（ADR-0041決定6）。
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
3. **カメラごとに値を選ぶ**（CCP2に近い一覧）: ISO・シャッター・絞り・露出補正・測光などを、台ごとに選べるようにする。台ごとに違う値を書く計画は、gateの照合が重くなる（ADR-0041決定7）ので、項目ごとに承認。
4. **色温度の数値と微調整、プリセットの番号選択**: 案Eで型・単位が確かめられた後に、項目ごとに承認。
5. **アプリから白を測る**（案Cの4）: 決まりの改訂のADRを別に通した後。通らなければやらない。

各段に、承認（項目ごと）、securityの確認、偽の本体の試験、実機の別承認が要る。

**案G ほかのADRとの整理**

- 上の改訂案（5→7項目、Aから写す。Aは読むだけ）は、そのまま別に承認できる。Aにも書く部分は、ADR-0041（値を選んで両台に書く）の側に置く。ADR-0041は本書の末尾にある（Accepted。表の行は段ごとに開く。どの段まで開いているかは同ADRの冒頭に書く）。
- 本案の段2以降は、ADR-0041の「項目ごとに新しいADR」に当たる。項目ごとの案は、WBの種類（段2）、WB以外の写すだけの項目（段1の結果の後）、色温度と微調整（段4）、白を測る操作（段5、別のADR）に分ける。
- AGENTS.mdの例外の文は、各ADRの承認ごとに人の手の改定が要る。

**所有者に決めてもらう事**

1. 目標の形: 「CCP2のように、PCの画面でカメラごとに設定を選べる」を最終形とし、「Aから写す」をその上の便利な操作として残すこと。
2. **WBがプリセットの時の扱い**（案C）。上の改訂案の案2の(a)(b)(c)と、案Cの1〜5のどれで進めるか。既定の案は置いていない。
3. 段の順番（案F）。特に、段1（読むだけの表を広げる）を先に進めてよいか。
4. 書く順番: Bが先・Aが最後（本案）か、Aが先か。→ 所有者はBが先・Aが最後に決めた（ADR-0041決定6）。
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
| `No exposure/AF-mode setting writes, capture, or AF-point movement is included.`（#331のフォーカスの文。#426で、AF点の移動の部分は別の項目に分けた） | AGENTS.md | 外す範囲は露出モードをMにする書き込みだけ。AFモードの書き込みは禁止のまま。AF点の移動（0x9205）はこの行の範囲ではなく、#426の項目（AGENTS.md）が、クリックで置いた1か所への枠の移動だけを、専用のgateで1クリックにつき最大1回認める | 当たらない（開発用の文） |
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
| シャッター速度 | `0x500D`（Nikon独自の`0xD100`もある） | UINT32 | 1/10000秒。`0xFFFFFFFF`=Bulb、`0xFFFFFFFD`=Time | 有限の値だけ書く。この機種では `0x500D` に確定した（固定の表のCAM-Bの行は段3で有効にした。CAM-Aの行は、値を選ぶ経路の段T3（ADR-0041）で有効にした。写す経路はCAM-Aに書かず、計画がAの値を読み、Bulb・Time・1秒超を拒むために使う）。`0xD100` は符号化の出典が無く、表に入れられない（決定13） |
| 絞り | `0x5007` | UINT16 | F値×100 | 書く |
| ISO | `0x500F` | UINT16 | ISOの値 | 書く |
| ISO感度自動制御 | `0xD054`（候補`0xD16A`） | UINT8 | 0=OFF | OFFだけ書く。この機種では `0xD054` に確定した（`0xD16A` は使わない。固定の表の2台分の行は無効のままで、ISOの行を書く前にAとBの値を読むために使う）（決定13） |
| WB | `0x5005` | UINT16 | | 名前を確かめた種類だけCAM-Bへ写す（ADR-0038の改訂。製品の表のCAM-Bの行は有効。写す経路で、実機で1項目ずつ書いて読み戻しが一致した） |
| 測光 | `0x500B` | UINT16 | | 値1〜4をCAM-Bへ写す（同上） |
| 露出補正 | `0x5010` | INT16 | | 読むだけ |
| ピクチャーコントロール | `0xD200` | UINT8（※未照合） | | 読むだけ |
| アクティブD-ライティング | `0xD14E` | UINT8 | | 読むだけ |

**codeの選び方と、この機種での確定**（決定13）: 公開の資料が名前を挙げている候補のうち、段Aの実機の読み取りで型・GetSet・形が合った1つだけを固定の表に入れる。資料に符号化の出典が無い候補は、表に入れられない（シャッター速度の `0xD100` は、入れるとbuildが通らない）。実行時に探すことも、切り替えることもしない。この機種では、ISO感度自動制御を `0xD054`（`0xD16A` は、実機の読み取りでbeginの失敗とENDの`0x200A`で答えた）、シャッター速度を `0x500D` に確定した。ADR-0038（写す経路）で有効にした行はCAM-BのISO・シャッター速度・絞り・WB・測光の5行だけ（選んだ値の経路で段ごとに開くCAM-Aの行は、ADR-0041の「有効にする順」に従う。WBと測光は本ADRの改訂による。絞りは段2、シャッター速度は段3）。ISO感度自動制御の行は、codeを持つが無効のまま。シャッター速度の段3は、mainへ入れる前に、実機で本体の表示とPCの値を3点見比べる確認と、0xD100を読むだけの確認をする決まり（所有者の指示）。

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

## ADR-0039: 製品の経路で、WPD経由のno-AFのSDRAM撮影とPC保存を認める

- 状態: Proposed（下書き。所有者の承認待ち）。所有者の決定（#351）に基づく。製品の経路のWPD経由の撮影は未実装・未受入。あるのは開発用の経路（#332・#343・#345・#381・#389）の実装と、開発用の実機の撮影だけ。承認までは、下の「変える既存の決定」の表の対象の今の文面が効く。securityの確認の指摘を決定2・4・6・8と変更表に入れ、所有者の決定（#416・#351）で範囲を`DualCamera`に限り、カードの扱いとAFを決めた。最初の下書きの後に、ADR-0038の照合の項目の増加（#416）、撮影をライブと同じプロセスで行う事（#437）、撮った組を校正に使う事（#434）を入れ、合成の時期の参照を取り下げた下書き（ADR-0040）からADR-0050に移した。
- 起票: #351の所有者の補足を受けて起票した
- 決定日: 未決定
- 関連: #351（所有者の決定と補足）、#326（Nikon SDK同梱の資料の論点）、#416（ADR-0038・0041）、#437（ADR-0046、確定した位置）、#434（撮影の1組からの校正の取り直し）、#332・#336・#343・#345・#381・#389（開発用のWPD撮影）、#385・#417（合成の品質）、#227（製品の二台の受入）、ADR-0002・0006・0015・0016・0017・0018・0020・0022・0023・0024・0025・0027・0028・0030・0032・0033・0036・0038・0041・0044・0045・0046・0050、`CON-002`・`CON-003`・`CON-004`、`FR-CAP-001`〜`FR-CAP-007`、`FR-LV-002`、`docs/WPD_STILL_CAPTURE.md`、`docs/WPD_STILL_REVIEW.md`、`docs/WPD_PREVIEW_FOCUS.md`、`src/phase0/include/a0/phase0/experimental/wpd_still_capture.hpp`、`third_party/digicamcontrol-NOTICE.txt`、`third_party/PTP_PROTOCOL_REFERENCES.md`、AGENTS.md
- 背景: 製品の撮影の経路は、SDKでカードに1枚撮り、WPDでその1枚を回収し、PCの原本を確かめた後にカードのそのobjectだけを消す形（ADR-0020、`FR-CAP-003`・`FR-CAP-004`）。二台はこの形をCaptureRecoveryOnlyとして評価している（ADR-0027・0028、`FR-CAP-007`）。標準のWPDの撮影（`0x100E`）は、D810で`0x2002`を返して成り立たなかった（ADR-0015）。開発用プレビューでは、公開の資料を根拠にしたno-AFのSDRAM撮影（`0x90C0`、引数`0xFFFFFFFF`）で、A・Bの撮影とPC保存を実装し、開発用に実機で撮っている（`docs/WPD_STILL_CAPTURE.md`。保存したprivateな組は#336で使っている。合否は開発状況記録にあり、本ADRでは製品の証拠に数えない）。所有者は、製品の経路の撮影もWPDにそろえると決めた。この撮影は、vendor operationを送らないと定めたADR-0016・0020・0028と`CON-003`、SDKで撮る前提のADR-0002・0023・0024・0027と要件に当たる。例外と置き換えの範囲を明文にする必要がある。

### ADR-0039の所有者の決定（#351）

所有者は#351で次を決めた。

- 本番でもWPDを使ってよい。本番の経路は、開発用プレビュー（#330〜#343、#389、#405）と同じくWPD経由で撮影・2台同時のライブ・合成をそろえる方向で組む。
- 実装の根拠は公開の資料（libgphoto2・digiCamControlの固定commit、MicrosoftのWPDの資料）だけを使い、Nikon SDK同梱の資料は使わない今の約束は変えない。
- 製品の経路で合成をつなぐのは、合成の品質（#385・#417）が出た後（所有者の指示「まず継ぎ目をしっかり合わせる」）。この項は、その後の所有者の回答（#351、ADR-0050）で置き換わる案: 正式な校正の前でも合成を「未確認」と数付きで出す。#385の線は変えない。
- 補足: 「本番でもWPDを使ってよい」は、ライブビューだけでなく撮影も含む。本番の経路でWPD経由の撮影を認めるADRを続けて下書きする（ADR-0020・0023・0024・0027・0028と`CON-003`に触れる）。

所有者は、下書きに残っていた未決について次を決めた（#416・#351）。

- 本番でもPCからピント合わせ（AF `0x90C1`）をできるようにする（本体に手が届かないため）。AFは撮影の命令ではなく、ライブの間の別の操作として扱う（ADR-0036決定2の2-a）。近遠の微調整`0x9204`は決まっていないので入れない。
- 一台だけの撮影（`SingleCamera`）はWPDに切り替えない。本ADRの範囲は`DualCamera`だけ。
- WPDの撮影の経路では、撮影の前にカードが空であることを求めない。カードに記録されないことは、実機の受入で1回確かめる。

#293のリリースの条件D1（「合成なしの二台は製品として出さない。合成ができるまでリリースしない。二台は今の評価経路（CaptureRecoveryOnly）のままとし、製品の位置づけを変えるADRは作らない」）のうち、後半（二台を評価経路のままにし、製品の位置づけを変えるADRを作らない）は、#351の所有者の決定で置き換わったと読む（※読み方。ADR-0050で所有者に見せる）。前半の「合成ができるまでリリースしない」と、配布は社内のPCだけ（D2）は変えない。

撮影の時に合成の位置を動かさず、ボタンで決めた位置（ライブで見ていた位置）のままJPEGを作る事（#351に記録した所有者の回答）は、別の変更でADR-0046とAGENTS.mdを改める。本ADRはその決まりを製品の経路でもそのまま使い、ここでは書き直さない。

ライブビューの命令の例外とAFはADR-0036決定2の改訂案で、カメラの設定の書き込みはADR-0038・0041で扱う。下の「決定」は、撮影について所有者の決定を規則の形に落とした案で、承認を要する。

### 開発用の経路で分かっている事実

どれも開発用の経路の事実で、製品の経路では未実装・未受入。

| 項目 | 事実 | 根拠 |
|---|---|---|
| 撮影の命令 | no-AFのSDRAM撮影`0x90C0`（引数`0xFFFFFFFF`、データ相なし）を、撮影を許した時（armed）だけ1回送る。AFと撮影を一緒にする命令は使わない | `docs/WPD_STILL_CAPTURE.md`の冒頭の2段落、`wpd_still_capture.hpp`の`BuildWpdStillRequest`・`AllowedWpdStillRequest`、`third_party/digicamcontrol-NOTICE.txt`（digiCamControlの固定commit `a95ee52`の`NikonBase.cs`） |
| ほかに送る命令 | DeviceReady `0x90C8`、イベントの読み取り`0x90C7`、`0x1015`で`0xD1A2`（Live状態）・`0x5004`（画質）・`0x5003`（画像サイズ）を読む。撮影後のイベントに結び付いたhandleだけに`0x1008`（ObjectInfo）・`0x1009`（Object）を送る。撮影の許可リストはこれだけ | `wpd_still_capture.hpp`の`AllowedWpdStillRequest` |
| 撮影先とカード | 引数`0xFFFFFFFF`は公開の資料でSDRAMへの撮影。画像は撮影後の新しいSDRAMのobjectのイベント1件に結び付けて取り込む。カードの列挙・削除・formatは送らない。カードに画像が記録されないことを実機で確かめた記録は、本書と`docs/WPD_STILL_CAPTURE.md`には無い（※要確認） | `docs/WPD_STILL_CAPTURE.md`の2・3・4段落 |
| 撮影前の条件 | LiveViewStatus=0、JPEG Fineだけ、7360×4912を読み、違えば設定を変えずに拒否する。2回の事前のイベントの読み取りで未回収の画像があれば拒否する。StoreAdded（`0x4004`）などの状態の変化の後は撮影条件を1回だけ読み直し、さらに2回イベントを確かめる。両台撮影では、両Liveを止めてCloseした後に、A・Bの非撮影のpreflightとCloseを先に行い、実際の撮影の時にも読み直す。Aのシャッターの後のDeviceReadyでOutOfFocusが分かったら止める | `docs/WPD_STILL_CAPTURE.md`（#343・#345の段落、#389の節）、AGENTS.mdの#343の行、`third_party/PTP_PROTOCOL_REFERENCES.md` |
| 期限 | 両台撮影は1つの180秒のtransaction（1つのdeadline）。期限の後は次のtransport、rename、成功状態を始めない。取得済みのpartialとoriginalは消さない | `docs/WPD_STILL_CAPTURE.md`の#389の節、AGENTS.md（#389の行と180秒のwatchdogの行）、ADR-0022 |
| sessionの持ち方 | SDKのsessionは開かない。両台撮影では、A・Bの2つのWPD sessionを照合から完了イベントの見届けまで開いたままにし、WPDの呼び出しは1つのスレッドから順に送る。leaseは全sessionのCloseまで持つ | `docs/WPD_STILL_CAPTURE.md`の#389の節 |
| 順番 | Aのシャッター → Bのシャッター → Aの取り込みと保存 → Bの取り込みと保存。Bのシャッターは、Aの受付を確かめた後だけ送る | `docs/WPD_STILL_CAPTURE.md`の#389の節、AGENTS.mdの#389の行 |
| 再試行しない | シャッター・取り込み・保存・期限のどれかが失敗したら、再送・再試行・画像の差し替えをしない。停止要求は未送信のシャッターを止め、送信済みの1枚の取り込みと保存は続ける。Bが露光済みなら、Aの取り込みが失敗してもBを1回だけ取り込む。結果の分からないシャッターの候補はprivateなpartialに留め、originalにしない | `docs/WPD_STILL_CAPTURE.md`の#389の表、AGENTS.mdの#389の行 |
| 完了イベント | 取り込みの後、同じsessionで完了イベント（`0xC102`か`0x400D`）を最大5秒（推定の設計定数）見届けてからCloseする。観測であり再試行ではない | `docs/WPD_STILL_CAPTURE.md`の#381の節 |
| PCの保存 | privateな`*-transport.jpg.partial`に逐次書き、最終転送、JPEGの全画素の復号と実寸、SHA-256、atomicな`original.jpg`のpublish、再読み込みのsize・hashを確かめる | `docs/WPD_STILL_CAPTURE.md`の5段落 |
| シャッターの時刻差 | `shutter_gap_ms`を測って記録するだけで、同時の露光も上限も保証しない。目標の値は所有者が決める | `docs/WPD_STILL_CAPTURE.md`の#389の節、`CON-002` |
| 根拠の資料 | 公開の資料（digiCamControl・libgphoto2の固定commit）だけ。Nikon SDK同梱の仕様書は使わない | `docs/WPD_STILL_CAPTURE.md`の2段落、`third_party/` |
| 撮影のgateが見る物 | 許可リストが見るのはMTPの命令（`WPD_COMMAND_MTP_EXT_*`）だけ。READ_DATAとENDはcontextを見ずに通す。WPDのContent・Properties・ResourcesのAPI（削除、列挙、objectの作成、propertyの書き込み）はgateを通らない。製品のspoolの経路は`Content()->Delete`でカードの画像を消し、それをcleanup token（回収の後にだけ発行）が止めている | `wpd_still_capture.hpp`の`AllowedWpdStillRequest`、`src/phase0/wpd_transport.cpp`の`DeleteRecoveredObject` |
| 取り込みが終わらなかった画像 | 期限切れや取り込みの失敗の時、画像はカメラのSDRAMに残り、保存されない。帰属は撮影の後の新しい`0xC101`のイベント1件とhandleの対応だけで決まる。読み終えたイベントは次のpreflightで「未回収」として見つからない。次の撮影で返るhandleの中身が新しい画像か残った画像かは、確かめた記録が無い（※未検証） | `docs/WPD_STILL_CAPTURE.md`の#389の表、`wpd_still_capture.hpp`の`WpdStillEventLedger` |
| 記録先 | 開発用のライブのgateは記録先のproperty（`0xD10B`。注記ではrecording media）を読むが、撮影のpreflightは読まない。記録先がカードの時の`0x90C0`の振る舞いは確かめていない（※未検証）。カードの画像のイベント`0x4002`は撮影を終端の失敗にする | `d810_preview.hpp`の`D810PreviewPropertyBytes`、`wpd_still_capture.hpp`の`WpdStillEventLedger` |
| 確定した位置の受け渡し | 開発用の窓は、撮影を頼んだ時のライブの確定した位置Pを、同じプロセスの中で撮影の合成へ渡す | ADR-0046、`docs/WPD_DUAL_PREVIEW.md`の#437の節 |

### 開発用の命令のうち製品の経路に入れる物・入れない物（案）

| 命令 | 開発用での使い方 | 製品の経路（案） | 根拠と状態 |
|---|---|---|---|
| `0x90C0`（引数`0xFFFFFFFF`） | 撮影 | 入れる。1回の撮影でカメラごとに1回まで、armedの時だけ。ライブを閉じた後のtransactionからだけ送る | 開発用で使っている撮影の命令はこれだけで、標準の`0x100E`は成り立たなかった（ADR-0015）。ライブを開いたまま撮る送り方（ADR-0044の探り）は入れない。※所有者の承認の点 |
| DeviceReady `0x90C8` | 撮影の前後の確認 | 入れる | ADR-0036決定2の改訂案の1でも、ライブビューの前提として例外に入れる案。※所有者の承認の点 |
| イベントの読み取り`0x90C7` | 事前の確認と画像の帰属 | 入れる | 画像の帰属と撮影前の条件に要る（上の表）。※所有者の承認の点 |
| `0x1015`（`0xD1A2`・`0x5004`・`0x5003`） | 撮影前の読み取り | 入れる | 標準の命令。上の表の「撮影前の条件」 |
| `0x1008`・`0x1009`（イベントに結び付いたhandleだけ） | 取り込み | 入れる | 標準の命令 |
| `0x1015`（`0x500D`・`0x500A`・`0x5001`） | #343の読み取りの診断 | ※要確認（製品の撮影の前の確認に使うか、診断だけにするか） | AGENTS.mdの#343の行 |
| AF `0x90C1` | 撮影の前の別の操作（#331） | 製品の経路に入れる（所有者の決定、#416・#351）。ただし撮影の経路のgateには入れない。ライブの間の別の操作として、ADR-0036決定2の2-aの条件とgateで送る | 開発用の事実は`docs/WPD_PREVIEW_FOCUS.md`。2台ともMFにする今の運用（ADR-0033の6節、`docs/OPERATOR_GUIDE.md`）と、製品の要件の対象外の「AFを使った撮影」の扱いは、ADR-0036決定2の「影響する対象」の表 |
| 近遠の微調整`0x9204` | 同上 | 入れない（決まっていない。※要確認） | 開発用だけの指示（AGENTS.md、`docs/WPD_PREVIEW_FOCUS.md`） |
| AF枠の移動`0x9205` | ライブの画面のクリック（#426） | 入れない | 開発用だけの指示（AGENTS.mdの#426の行） |
| 標準の撮影`0x100E` | 送らない | 入れない | ADR-0015。今のhybridの経路でも送らない |
| `0x9207` | 送らない | 送らない（変えない） | ADR-0016・0020、`CON-003`、AGENTS.md |
| ChangeCameraMode `0x90C2` | 通常のgateで拒否 | 送らない（変えない） | AGENTS.mdの#337の行、ADR-0038の決定14 |
| SetDevicePropValue `0x1016` | 撮影の経路では送らない | 撮影の経路では送らない。設定の書き込みはADR-0038・0041のgateだけ | ADR-0038の決定9 |
| カードの列挙・削除・format | 送らない | 送らない | 上の表の「撮影先とカード」 |

### 決定（案）

1. **範囲**: 製品の`DualCamera`の撮影を、WPD経由のno-AFのSDRAM撮影とPC保存で行ってよい。`SingleCamera`はWPDに切り替えない（所有者の決定、#416・#351。今は二台だけ）。`SingleCamera`の契約（ADR-0023・0024の`SingleCamera`の部分）は変えない。
2. **命令**: 製品の撮影の経路で送ってよい命令は、上の表で撮影の経路に「入れる」とした物だけにする。許可リストは送る直前のgateで固定し、operation番号と引数を32 bit全体で比べる（開発用の`AllowedWpdStillRequest`と同じ考え方、※案）。`0x9207`、`0x90C2`、`0x90C1`、`0x9204`、`0x9205`、`0x1016`は撮影のgateで拒否する。表で※要確認とした命令は、所有者が決めるまで入れない。次も守る（securityの確認）。
   - 撮影のchannelは、identityの読み取りのほかに、WPDのContent・Properties・ResourcesのAPIを使わない。列挙・削除・objectの作成・propertyの書き込み（`IPortableDeviceContent::Delete`・`EnumObjects`・`CreateObjectWithPropertiesAndData`、`IPortableDeviceProperties::SetValues`、`IPortableDeviceResources`の書き込み）は構造上呼べない作りにする。撮影の経路を今の製品のspoolのtransport（`src/phase0/wpd_transport.cpp`）の上に作らない。
   - SDRAMの経路ではcleanup tokenを発行しない。
   - READ_DATAとENDは、gateがbeginの応答から持つ自分のcontextだけを通す（開発用のgateはcontextを見ずに通している）。
   - 偽のtransportの試験で次を固定する（ADR-0038段Bの前提条件5と同じ形）: armedでない`0x90C0`、2回目の`0x90C0`、結び付けていないhandleと0・`0xFFFFFFFF`、`AuthorizeObject`の前と2回目、シャッターの前の`0xC101`、2件の`0xC101`、別の台のイベントの取り違え、`0x4002`が終端になること、`Delete`と`SetValues`の呼び出しが0回、期限の後のrenameが0回。
3. **手順**: 両台撮影は、開発用の#389の順（両台の非撮影のpreflightとClose → 両台のsessionを開いて照合と撮影直前の再確認 → Aのシャッター → Bのシャッター → Aの取り込みと保存 → Bの取り込みと保存 → 完了イベントの見届けとClose）を、1つの180秒のdeadlineで行う。CAM-AからCAM-Bの順（ADR-0023）は、シャッターの順と取り込みの順として保つ。撮影の前にすべてのライブを止めて閉じ、閉じた事を確かめる（ADR-0036の不変条件2）。ライブを開いたまま撮る順（ADR-0044）は本ADRに含めない。
4. **原本とカード**: PCの`original.jpg`を唯一の正本にする（ADR-0018は変えない）。この経路はカードを使わない前提なので、ADR-0020のsingle-slot spool（撮影前のカードの空の確認、回収した1件の削除、空の再確認）を適用せず、カードの削除は0にする（※案）。撮影の前にカードが空であることを求めない（所有者の決定、#416・#351）。カードに記録されないことは、製品の実機の受入で1回確かめる（下の「実機の受入で必ず確かめる項目」の1）。preflightで記録先のproperty（`0xD10B`）を読み、期待の値でなければ撮らない（値は書かない）案は※要確認（値の意味と期待の値が公開の資料で確かめられていない）。この確認が無いと、本体の設定が変わってカードに記録されるようになっても気づけず、製品の撮影の画像がカードに残りうる。そのため、値の意味と期待の値を公開の資料で確かめ、この読み取りをpreflightに入れる事を、Acceptedにする前提条件にする（前提条件の8）。
   - SDRAMの経路では、PCの保存が終わるまで、カメラの側に控え（spoolの経路のカード）が残らない。シャッターの後の失敗は、撮り直しで回復する（原稿は静止しているので撮り直せる）。操作者向けの文書にもこの扱いを書く。
5. **sessionとidentity**: SDKのsessionを開かない。両台撮影の間は登録済みのカメラ（今はCAM-AとCAM-B）に1つずつのWPD sessionを開く（`FR-CAP-005`の改定）。WPDの呼び出しは1つのスレッドから順に送る。カメラは登録済みのWPD serial digestで照合し、開くたびに照合し直す（開発用と同じ、※案）。カメラはカメラの一覧から扱い、台数を2に固定した書き方を新しい欄・gate・試験に増やさない（ADR-0045）。ADR-0025のSDKの候補を操作者が割り当てる照合は、WPDの経路では使わない（※案、所有者の承認の点）。ADR-0025の照合はSDKのModuleの保持（ADR-0028）を要し、WPDの経路の「SDKのModuleを保持しない」（変更表のADR-0028の行）と両立しないため。operator-session-wideのleaseは全sessionのCloseまで持つ。
   - WPD serial digestは公開の固定の文字列・長さ・serialのSHA-256で、鍵が無い（`src/phase0/wpd_transport.cpp`の`DeriveWpdStableIdentity`）。serialの取りうる値が少なければ、digestから総当たりでserialに戻せる（※想定シナリオ。D810のserialの桁数は確かめていない）。digestもserialと同じ扱いにする（不変条件8）。手元にだけ置く鍵を使うHMACにするかは、実装の段で検討する（※案）。
6. **再試行しない**: 上の表の「再試行しない」のとおり。原本の扱い（partialを残す、結果の分からない候補を昇格させない、期限の後に成功状態へ進まない）は変えない。完了イベントの見届けは観測として残す。
   - 取り込みが途中で失敗した後の止め（securityの確認）: `AuthorizeObject`の後に取り込みか保存が終わらなかった側は、そのカメラの電源を入れ直したことを確かめるまで撮影を止める（`CameraStateRequiresInspection`と同じ扱い）。SDRAMに残った画像が次の撮影の原画像になると、別の原稿の画像を組にする曖昧な画像の採用になり、`NFR-REL-001`の0件に反するため。電源の入れ直しでSDRAMが空になるか、残った画像が次の取り込みに出るかは、製品の受入の前に、別に承認した1回で確かめる（※未検証）。
7. **capture profileと設定の照合**: 撮影の直前の承認済みのcapture profileとの照合（ADR-0024、`FR-CAP-007`）を残す。今のprofileの項目（シャッター速度・絞り・ISO・WBなど）はSDKで読んでいて、開発用のWPDの撮影は画質・画像サイズ・Live状態しか読んでいない。WPDの経路で同じ項目を読む方法は、ADR-0038の段Aの読み取り（`0x1014`・`0x1015`）で決める（※要確認）。読む項目には、ADR-0038・0041で両台を比べる項目（露出の5項目と、行が有効な間のホワイトバランスの種類と測光）を両台とも含める（ADR-0038段Cの前提条件の(e)。今の画面の`SettingsMismatch`のBlockerをWPDの経路でも効かせるため）。
   - 設定の書き込みが確かめた一致で終わらなかった時の撮影の止め（AGENTS.mdの#416段Bの行。読み直しで一致を確かめるまで撮らない、閉じても再起動しても解けない）を、製品の撮影の経路でも守る。製品の経路が止めの記録を読む作りは※要確認（書き込みのoptionの無いbuildは記録を読まないので、そのbuildでも撮らない、というAGENTS.mdの文と同じ扱いにする案）。
8. **経路の選び方**: 製品のWPDの撮影の経路が受入を通るまで、今のSDKの撮影とカードのspoolの経路を残す。2つの経路を自動で切り替えない（ADR-0030の不変条件のno fallback）。SDKの経路を外す時期は、受入の後に別に決める（※案）。次も守る（securityの確認）。
   - 経路はtransactionを作る時に固定し、記録に残し、transactionの間は変えない。
   - 既定値を持たない。設定が無い・読めない・知らない値の時は撮影を拒否する。
   - 受入を通っていない経路は、製品のbuildで選べないようにする。
   - ライブ（ADR-0035のSDK／ADR-0036決定2のWPD）と撮影（spool／SDRAM）の組み合わせも同じ扱いにし、承認した組み合わせだけを選べるようにする。
9. **入れる順**: ライブ（ADR-0036決定2）、撮影（本ADR）、合成を別々の変更で入れる。いつもの撮影用アプリに載せる順と、合成を正式な校正の前に「未確認」と数付きで出す扱いはADR-0050で決める。
10. **根拠**: 実装・試験・文書の根拠は公開の資料だけにし、Nikon SDK同梱の資料を使わない（#326・#351）。実装の時の決まりはADR-0036決定2の3の(a)〜(c)に従う（撮影前の確認の読み取りの手順も出典の記録の対象）。`CON-004`は変えない。
11. **ライブと同じプロセスで撮る**（#437）: 製品の両台撮影は、ライブを持つのと同じCamera Agentのプロセスの中で行う。撮影を頼んだ時のライブの確定した位置P（ADR-0046）は、そのプロセスの中で撮影の合成へ渡し、WPFを経由して写し直さない。撮影の前にライブを止めて閉じる順（3）は変えない。SDKを使うCamera Agentとは別のプロセスにし、1つのプロセスでSDKとWPDを混ぜない（※案。ビルドの組み合わせはADR-0050の7）。
12. **撮った組を校正に使う**（#434）: 製品の両台撮影で確かめた原画像2枚は、ワンボタン校正（撮影の1組からの校正の取り直し）の入力にしてよい。使い方・範囲・断る条件は、開発用の側で先に決める決まり（ADR-0048の案。mainに未登録）に従い、製品の画面で使い始めるのはその決まりがAcceptedになった後（ADR-0050の5）。校正の取り直しは新しい撮影をしない。原画像は消さず書き換えない。校正の取り直しが作る切り出し・中間の画像・報告も、原画像と同じくprivateに置き、commitしない（AGENTS.mdの「Never commit … real customer originals, or captured production images」）。

### 実機の受入で必ず確かめる項目

1. カードに画像が記録されないこと。系列の前後でカードのpayloadを数えて確かめる。カードの列挙は撮影の許可リストの外なので、別に承認した読むだけの確認として行う（決定4、所有者の決定）。
2. 1回のシャッターにつき各台の`0xC101`が1件で、別の台に数えないこと。完了イベントの見届けが記録に残ること。
3. 保存の完全性: 全画素の復号、SHA-256、上書きしないrename、読み直し。失敗した時に`.partial`が残り、期限の後にrenameしないこと。
4. 取り込みの途中の失敗（別の系列。ADR-0032のUSB抜去の扱い）の後、SDRAMに残った画像が次の撮影の取り込みに混ざらないこと。混ざる余地があるなら、決定6の止めが働くこと。
5. AとBのシャッターの間に停止要求を入れた時、Bが露光しないこと。
6. WPDのtransactionの間、SDKの操作が0回であること（`NFR-OBS-001`の記録の形で）。
7. `shutter_gap_ms`が記録され、画面に「同時」「同期」と出ないこと。
8. 撮影の直前のprofileの照合が、決定7で決めた項目をWPDで読んで行われること。
9. 撮影に渡した位置が、頼んだ時のライブの確定した位置と同じで、記録とライブのログに同じ数で残ること（決定11、ADR-0046）。

### 製品の受入

- 製品の経路の実機の受入は、開発用の確認（#332・#343・#345・#389の実機の撮影、#336のprivateな組）と別の系列にする。開発用の撮影は製品の受入に数えない。
- 系列は新しく始め、ADR-0030の最大5組とADR-0032の数え方（試行の定義、ソフト起因でシャッター0の拒否、USB抜去の別系列、保存の合格線）に従う。最初のone-shotを5組に含める。最初の失敗・未確定で止め、補充と自動の再試行をしない。
- 対象（機体と系列）、回数、中止の条件、カードの扱いを、受入の前に所有者が別に承認する。本ADRの承認は受入の承認を含まない。
- CaptureRecoveryOnly（ADR-0027）の系列の記録と、WPDの経路の系列の記録を混ぜない（※案）。

### Acceptedにする前提条件

1. 上の命令の表で※要確認とした命令（`0x9204`・#343の読み取りの診断）を所有者が決めている。決めない物は「入れない」として承認してよい。AF`0x90C1`は所有者が決めた（ADR-0036決定2の2-a。どのgateから送るかの案1・案2はADR-0036で決める）。
2. ADR-0025の照合をWPDの経路で使わないこと（決定5の※案）を所有者が承認している。`SingleCamera`を含めないこと（決定1）とカードの空を求めないこと（決定4）は所有者が決めた（#416・#351）。
3. ADR-0036決定2の改訂案（WPD経由のライブビュー）が、本ADRと同時かそれより先にAcceptedになっている。撮影の前後にライブビューを止めて開き直す手順が、決定2の例外に依存するため。
4. securityの設計レビューを、製品の撮影のgate（許可リスト、armedの持ち方、handleの結び付け、2つのsessionの持ち方）に加えて、WPDのAPIのほかの面とcleanup token（決定2）、SDRAMに残った画像（決定6）、経路の選び方（決定8）で通している。
5. 下の「変える既存の決定」の表を承認し、AGENTS.mdの改定の文を人が承認している（文の案は、AGENTS.mdのADR-0050の項）。
6. 決定7で、profileの照合に読む項目（ADR-0038・0041で比べる項目を含む）を決めている（ADR-0038段Cの前提条件の(e)と同じもの）。
7. 撮影の経路で使う番号・意味・手順（撮影前の確認の読み取りの手順を含む）の出典が、ADR-0036決定2の3の(c)のとおり記録されている。
8. 記録先のproperty（`0xD10B`）の値の意味と期待の値を公開の資料で確かめ、preflightで読んで期待の値でなければ撮らない（値は書かない、fail-closed）形を決定4に入れている。公開の資料で確かめられない時は、カードの扱いを所有者が改めて決めている。

実装に進むのはAcceptedの後、実機に進むのは上の「製品の受入」の別の承認の後。

### 守る不変条件

1. `0x9207`と`0x90C2`を送らない。
2. 撮影のtransactionは同時に1つだけ。シャッターはカメラごとに1回まで。no retry（ADR-0006）。
3. PCの`original.jpg`を唯一の正本にし、検証の前に昇格させない（ADR-0018）。
4. 同じカメラでSDKとWPDのsessionを重ねない。WPDの経路の撮影のtransactionの間はSDKのsessionを開かない。別々のカメラの間でSDKとWPDを同時に開いてよいかは本ADRでも決めない（ADR-0036の不変条件1）。決まるまでは、別々のカメラの間でも重ねない（AGENTS.mdの「Never overlap SDK and WPD sessions」）。`SingleCamera`と`DualCamera`を切り替える時は、片方のsessionがすべて閉じたのを確かめてから、もう片方を開く。
5. 撮影の前に、すべてのライブビューを閉じて確かめる（ADR-0036の不変条件2）。
6. シャッターの同期を保証しない（`CON-002`）。
7. カードの一括削除・formatをしない。
8. 実カメラの識別子を記録・公開しない（`CON-004`、AGENTS.md）。WPD serial digestもserialと同じ扱いにし、記録・ログ・画面・Issue・公開リポジトリに出さない。登録の記録は`%LOCALAPPDATA%\A0CameraStitcher`の下に置く（push前の漏れの検査が、その下の64桁のhexとNikonのUSBのserialを針にするため）。

### 変える既存の決定（案）

本ADRがProposedの間は、どの対象の本文も変えず、追記も置かない（承認の時に置く、※案）。表の「変えない部分」は、SDKの経路が残る間そのまま効く。

| 対象 | 今の文（要旨か引用） | 変える範囲 | 変えない部分 |
|---|---|---|---|
| ADR-0002 | 「`DualCamera`は`CAM-A`の撮影・回収・close後に`CAM-B`へ進む」 | WPDの経路では、シャッターをA→Bの順に送り、両方の後にA→Bの順で取り込む（#389の順）。2つのWPD sessionを開く | 同時に開くSDK sessionは1つ。SDKの経路の順 |
| ADR-0016 | 「撮影commandとvendor operationは送らない」 | 製品のWPDの撮影の経路で、上の表の「入れる」のvendor operation（`0x90C0`・`0x90C8`・`0x90C7`）を送ってよい例外を足す | `0x9207`の扱い、query-onlyの確認 |
| ADR-0017（Superseded） | 「vendor operationを実装・送信せず」 | ADR-0016と同じ方向の追記 | hybridの経路の判断 |
| ADR-0020 | 「SDK exactly-one capture後にWPDで唯一のexact objectを回収」「vendor operation（`0x9207`を含む）は禁止する」 | WPDの経路はspoolを使わない別の経路とし（決定4）、本ADRの命令だけをvendor operationの禁止の例外にする | spoolの規則、`0x9207`の禁止、カードの一括削除・formatの禁止 |
| ADR-0019 | 影響の「承認済みADR-0020のsingle-slot spool経路を除き、captureをfail-closedにする」 | 本ADRの`DualCamera`のWPDのSDRAMの経路を、fail-closedの例外に足す | device datetimeとobject dateのcutoffを製品の帰属の契約に使わないこと。ほかの撮影はfail-closedのまま |
| ADR-0023 | 「両modeでdedicated single-slot spool、SDK/WPD session非重複…」 | `DualCamera`のWPDの経路ではspoolを求めない（決定4） | `SingleCamera`の部分は変えない（決定1。`SingleCamera`はWPDに切り替えない）。mode境界、自動降格の禁止、lease、180秒、canonical原本、no retry。撮影設定のread-onlyの文はADR-0038で扱う |
| ADR-0025、`NFR-SEC-001` | 「Dual bindingのpreview、raw identifier、serial、source objectまたはcandidate ordinalを保存・公開しない」。SDKの候補を一台ずつ表示し、操作者が`CAM-A`・`CAM-B`に割り当てる | WPDの経路では、ADR-0025の照合を使わず、登録済みのWPD serial digestで照合する（決定5の※案）。`DualCamera`でもdigestを手元に保存することになるので、digestを保存してよい物として足す。保存の場所は`%LOCALAPPDATA%\A0CameraStitcher`の下に限り、公開はしない（不変条件8） | serialとraw identifierを保存・公開しないこと、誤割当のリスクの記録、previewを保存しない |
| `NFR-OBS-001` | 「WPD capture command未送信」を記録の項目にする | spoolの経路に限る文に直す。WPDの経路では、送った撮影の命令（`0x90C0`の回数と結果）、経路の種類（決定8）、SDKの操作が0回であることを記録する | 匿名の記録、ほかの項目 |
| `FR-UI-001`、`docs/OPERATOR_UI_SPEC.md`のカードの確認（「modeが要求するcameraの接続、identity、read-only設定整合、card、Live View」と、mode変更の時のcardの再検査）、画面の`CardNotKnownEmpty`のBlocker（`OperatorWorkflowModel.cs`） | 撮影の前にカードが空であることを確かめ、確かめられなければ撮影のBlocker | `DualCamera`のWPDの経路では、カードの空を求めない（決定4、所有者の決定）。この経路では`CardNotKnownEmpty`をBlockerにしない。カードの欄は「この経路はカードに記録しない」の表示にする（表示の形は※案）。決定6の止め（取り込みが終わらなかったカメラの電源の入れ直し）は`CameraInspectionRequired`と同じ扱いのBlockerにする | spoolの経路では今のBlockerのまま |
| ADR-0027 | 「DualのSDK撮影・WPD回収・canonical PC原本保存だけを評価する`CaptureRecoveryOnly`」「CAM-A失敗時はCAM-Bへ進まず」 | WPDの経路の撮影を、CaptureRecoveryOnlyとは別の製品の経路として足す。Aが失敗した時の扱いは#389の表に合わせる（Aのシャッターが拒否・未確認ならBを露光しない。Bが露光済みなら1回だけ取り込む） | CaptureRecoveryOnlyの契約とschema |
| ADR-0028 | 「WPD open中はSDK API…を一切行わず」、vendor operationの禁止 | WPDの経路ではSDK Moduleを保持しない。vendor operationの禁止に本ADRの例外を足す | SDKの経路のModule保持の規則 |
| ADR-0036 決定2の7（改訂案） | 2つのWPD sessionはライブビューの間に限る | 撮影のtransactionの間の2つのsessionは本ADRで認める | ライブビューの例外の範囲 |
| `CON-003` | 「専用empty/cleared card single-slot spool以外の帰属・削除経路を実装・実行しない」「vendor operation（`0x9207`を含む）…は禁止する」 | WPDのSDRAMの撮影の帰属（撮影後の新しいobjectのイベント1件に結び付ける）を、spoolとは別の帰属の経路として認める。本ADRの命令だけをvendor operationの禁止の例外にする | `0x9207`、カードの一括削除・format、retry、SDKとWPDの重なりの禁止、削除はPC原本の検証の後だけ |
| `FR-CAP-001`・`FR-CAP-002` | SDKの候補の割り当てとWPDの登録 | `DualCamera`のWPDの経路では、登録済みのWPD serial digestで照合する（決定5の※案） | fail closed、alias不足の拒否、`SingleCamera`の文 |
| `FR-CAP-003`・`FR-CAP-004` | SDKでカードに撮影 → WPDで回収 → 削除 → 空の確認 | WPDの経路では、SDRAMの撮影 → 取り込み → PC保存とし、カードの削除を行わない | SDKの経路の文、PC原本の検証 |
| `FR-CAP-005` | 「同時に…一つのWPD sessionだけを開く」 | 撮影のtransactionの間、登録済みのカメラに1つずつのWPD sessionを認める（ADR-0036決定2の7のライブビューの改定と合わせる） | 撮影のtransactionは1つ、SDKとWPDの非重複 |
| `FR-CAP-007` | CaptureRecoveryOnly | 変えない。WPDの経路は別の要件として足す（※案） | 全文 |
| `FR-LV-002` | spoolのtransactionの後にSDKのLive Viewを再開 | WPDの経路では、保存とsessionのCloseを確かめた後にWPDのライブビューを再開する（開発用と同じ） | 自動で再開しない |
| `docs/PRODUCT_REQUIREMENTS.md`の対象外 | 「vendor operation（`0x9207`を含む）の実装・送信」 | 本ADRとADR-0036決定2の命令を除く | `0x9207` |
| `docs/PRODUCT_REQUIREMENTS.md`のMVP受入条件2・4・8 | SDK撮影とWPD回収、spoolの言葉 | WPDの経路の言葉を足す | 原画像を失わない、no retry、lease、180秒 |
| AGENTS.md | `Never overlap SDK and WPD sessions. Stop and fully close SDK Live View before the hybrid transaction; ...`、#332・#389の開発用の行、``Never send vendor operation `0x9207` or retry the transaction.`` | 製品の経路でもWPDの撮影を認める文を足す（文の案は、AGENTS.mdのADR-0050の項） | `0x9207`とretryの禁止、SDKとWPDを重ねない |
| `docs/WPD_STILL_CAPTURE.md` | 開発用の経路 | 製品の経路の節を足すか、別の文書にする（※案） | 開発用の記録 |
| `docs/D810_PC_CONTROL_CAPABILITIES.md` | 撮影の行 | WPDの撮影を製品の経路として書く | Phase 0のCLIの行 |
| #293のリリースの条件D1 | 二台は評価経路（CaptureRecoveryOnly）のまま、製品の位置づけを変えるADRは作らない | 後半は#351の決定で置き換わったと記録する（※読み方） | 「合成ができるまでリリースしない」、D2（配布は社内のPCだけ） |

変えないもの: ADR-0006・0018・0022（no retry、PCの原本、180秒とlease）、ADR-0024（`SingleCamera`の契約。`SingleCamera`はWPDに切り替えない、決定1）、ADR-0029（SDKのPC直接保存の評価）、ADR-0030・0032（回数と数え方。製品の受入は新しい系列）、ADR-0044（ライブを開いたまま撮る探り。開発用だけ）、`CON-002`・`CON-004`。

承認後に確かめる対象（追記は置かない）: ADR-0022の「承認済み実機撮影入口は…に限定する」が製品のCamera AgentのWPDの撮影の入口にも及ぶか（※要確認）、`HG-0008`（spoolの承認をSDKの経路に限るか）、`docs/PHASE0_TEST_PLAN.md`、`docs/HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md`、`docs/OPERATOR_GUIDE.md`・`docs/PILOT_CAPTURE_PROCEDURE.md`（カードの扱い）、`.autodev/requirements/normalized.json`。

### 範囲外と未決

- SDKの撮影とカードのspoolの経路を外す時期（決定8）。
- 微調整・#343の読み取りの診断を製品の経路に入れるか（命令の表）。AFは入れる（所有者の決定）。
- ADR-0025の照合を使わないこと（決定5の※案）。`SingleCamera`を含めないこととカードの空を求めないことは所有者が決めた。
- カードに画像が記録されないことの確認（実機の受入の1）。preflightで記録先を読む案（決定4、※要確認。Acceptedにする前提条件の8）。
- 電源の入れ直しでSDRAMに残った画像が消えるか（決定6、※未検証）。
- capture profileの項目をWPDで読む方法と、設定の書き込みの止めの記録を製品の経路が読む作り（決定7）。
- 完了イベントの待ちの上限5秒は推定（`docs/WPD_STILL_CAPTURE.md`）。`shutter_gap_ms`の目標の値は所有者が決める。

### ADR-0039で所有者に承認していただく点

1. 決定1〜12。特に、命令の表の撮影の経路に「入れる」（`0x90C0`・`0x90C8`・`0x90C7`と、標準の読み取り・取り込み）と、`0x9207`・`0x90C2`を送らないこと。撮影のchannelがWPDのAPIのほかの面を使わずcleanup tokenを発行しないこと（2）、取り込みが終わらなかったカメラを電源の入れ直しまで止めること（6）、設定の照合の項目と書き込みの止め（7）、経路に既定値を持たないこと（8）、ライブと同じプロセスで撮ること（11）、撮った組を校正に使うこと（12）。
2. 命令の表の※要確認の命令（`0x9204`・#343の読み取りの診断）と、決定4の記録先を読む案、決定5の※案。
3. 製品の受入を開発用の確認と分けること、Acceptedにする前提条件。
4. 変える既存の決定の表と、追記を承認の時に置く扱い。#293のD1の後半の読み方。

### 承認後の作業

- 状態をAcceptedにし、決定日を記入する。表の対象に「ADR-0039で改定」の追記を置く。AGENTS.mdは人が承認した文を入れる。
- 製品の撮影のgateを設計し、securityのレビューを通してから実装する。
- 製品の受入の系列を別に承認してから実機に進む。
- ADR-0038の段Cの前提条件2（本番の経路の受入）が満たされたかを、受入の後に記録する。

## ADR-0041: PCでカメラごとに選んだ撮影の設定の値を、両台のカメラに書く（CAM-Bを先に、CAM-Aを最後に）

- 状態: Accepted（#416、所有者）。所有者は、AGENTS.mdの決まりの文（#416の段Bの項の(b)と、ADR-0041の例外の項）と、下の「ADR-0041の所有者の決定」を承認した。**承認は書いてよい範囲を決めたもので、書き込みを有効にしたものではない。** 段T3のコードは、CAM-Bに選んだ値と、CAM-Aに選んだ値（CAM-Aは5項目すべて）を書けるようにしてある（製品の表の`chosen_values`と定数`kD810Adr41Accepted`は真。CAM-Aの版の欄はCAM-Bの欄を参照で持つ。有効な行の集合は`static_assert`が見張る）。実機では、T1（CAM-Bに選んだ値）、T2（CAM-AのISO。CAM-Aの本体の照合は表と一致）、T3（CAM-Aの残り4項目を1回の確定で）を試して、どれも読み戻しと読み直しが一致した（下の「実機の事実」）。有効にするのは、下の「有効にする順」の段ごとに、前の段が実機で一致してから使う、表を開く別のcommitだけ（使う＝起動する・mainへ入れるのは、前の段が一致した後）。本ADRの承認は、開発用プレビューの段B（ADR-0038の段の呼び名）に限る。
- 決定日: #416の所有者の承認による（日付はIssueの記録による）
- 関連: #416、ADR-0038（本ADRが広げる元。範囲の原則、決定1〜14、ADR-0038の改訂）、ADR-0024（承認済みのcapture profileとの照合）、#415（色と明るさの合わせ）、AGENTS.md、`docs/WPD_DUAL_PREVIEW.md`（「#416 カメラごとに値を選んで書く画面」の節と、書き込みの「実行」）、`docs/D810_PC_CONTROL_CAPABILITIES.md`、`third_party/PTP_PROTOCOL_REFERENCES.md`、`src/phase0/include/a0/phase0/experimental/d810_settings_choice.hpp`、`src/phase0/include/a0/phase0/experimental/d810_settings_plan_chosen.hpp`
- 背景: ADR-0038は、CAM-Aから読んだ値をCAM-Bへ写すことだけを認め、操作者が値を選ぶ機能を「入れるなら将来の別のADR」とした（ADR-0038の「未決への回答」の1）。吊り下げたリグでは本体のボタンに手が届かないので、CAM-Aの値そのものを変えたい時や、左右で違う値にしたい時に、PCから変えられないと作業が止まる。所有者は、最終的にソフトの側から設定を変えられる仕組みを持つ方向を示した（ADR-0038の「改訂案その2」の発端）。本ADRがその「別のADR」に当たる。安全の条件はADR-0038の承認に付いた条件のまま（カメラを壊しうる設定は変えられないようにし、一般的なカメラの設定と同じ範囲にとどめる）。
- 実機の事実（値は書かない）: ADR-0038とその改訂でCAM-AからCAM-Bへ写す5項目（ISO・WB・測光・絞り・シャッター速度）は、実機で1回に1項目ずつ書き、どれも読み戻しが一致した。選んだ値の経路は、T1（CAM-Bの1項目）とT2（CAM-Aの1項目）を実機で試して、読み戻しと読み直しが一致した。その後、CAM-Bの残り4項目（シャッター速度・絞り・WBの種類・測光）を1回の確定で書き、4項目とも読み戻しが一致し、読み直しで撮影の止めが残らず、CAM-Aへの書き込みは無かった。続けてT3として、CAM-Aの同じ4項目を1回の確定で表の順に書き、4項目とも読み戻しが一致し、読み直しで撮影の止めが残らず、CAM-Bへの書き込みは無かった。4項目は書く段の期限に余裕を持って収まった。さらに、両台に3項目（ISO・シャッター速度・測光）を、CAM-B、CAM-Aの順に台ごとに1回の確定で書き、どちらも一致した。確定と設定の画面の操作は所有者が行った。実機で試していないのは、CAM-BとCAM-Aの項目を1回の確定に混ぜた計画、5項目すべてを1回の確定で書く計画、途中で不一致になった時の止まり方（偽のaccessの試験だけ。※未検証）。

### ADR-0041の所有者の決定（#416）

1. 目標: PCでカメラごとに値を選べる（例: CAM-AはISO 100、CAM-BはISO 200）。CAM-Aにも書く。
2. 書く順: CAM-Bが先、CAM-Aが最後。
3. 項目: ISO・シャッター速度・絞り・WBの種類・測光。WBの自動と蛍光灯は一覧に残すが、その型（自動の型・蛍光灯の型）は写さない・書かない。プリセットの中身・色温度・微調整は書かない。測光は値を選ぶ画面で選べる。
4. ISOの幅: 標準の範囲（64〜12800）だけ。Lo・Hiは出さない。値の並びは本体が返す一覧を使う。
5. 露出は1秒以下。Bulb・Timeは書かない。露出モードとISO感度自動制御の書き込みは広げない。
6. CAM-Bの書き込みが途中で失敗した時は、まだ送っていないCAM-Aの値も、両台を合わせるまで撮影を止める対象に含める。
7. 止めが掛かっている間に設定の画面を閉じても、ライブは自動で再開しない（手の「ライブ再開」では再開できる）。
8. 「Aと同じにする」を使う時に、CAM-AがMである事までは求めない。
9. 再送しない。自動の入口（コマンドライン、UIの自動操作）は無い。確定は人が押す。
10. 有効にする順: CAM-Bに選んだ値（T1）→ CAM-AのISO（T2）→ CAM-Aの残り4項目（T3）。T1とT2は、実機で1項目を確かめてから次へ進む。T3は、CAM-Aの最初の1項目（ISO）がCAM-Aで単独で一致した後なら、同じ機種・同じ版のCAM-Bで単独で一致している項目を、CAM-Aにまとめて書いてよい（確定は1回）。
11. CAM-Aの本体の版の確かめ方: 表のCAM-Aの版の欄にはCAM-Bの欄を参照で入れる（版の文字列を増やさない）。所有者がCAM-Aを読んだ時の照合（ログの`CAM-A settings_write_body_check`の`firmware_match`）が1なら、CAM-AもCAM-Bと同じ機種・同じ版の扱いになる。0なら、CAM-Aには何も送られない。その時だけ、元の手順（所有者がOFFのbuildでCAM-Aの版を読み、表の1行にだけ入れる。T2・T3のexeは作り直す）に戻す。

### 決定

1. **書く相手と値の出どころ**: 操作者が台ごとに選んだ値を、CAM-BとCAM-Aに書く。台ごとに違う値でよい。ADR-0038の「AからBへ写す」は別の操作として残し、本ADRはその文面を変えない。
2. **項目**: ISO感度（`0x500F`）・シャッター速度・絞り（`0x5007`）・WBの種類（`0x5005`）・測光（`0x500B`）の5つ。codeは製品の表（ADR-0038決定13）のものを使う。露出モード・ISO感度自動制御・露出補正は選べない。値を選ぶ計画は露出モードとISO感度自動制御を書かず、書く台はどれも、既にMとISO自動OFFを読んでいる事を求める。露出補正、ピクチャーコントロール、アクティブD-ライティングなど、ここに無い項目と、ここに無い計画の種類は無効のままで、入れるなら項目ごとに別の決定が要る（ADR-0038の範囲の原則）。
3. **値**: 書く相手の本体が自分のPropDescで広告した値のうち、次を満たす値だけを、一覧から選ぶ（自由入力は置かない）。
   - ISO: 64〜12800。Lo・Hiは出さない。
   - シャッター速度: 1秒以下の有限の値。Bulb・Time・1秒を超える露出は書かない。
   - 絞り: 本体の一覧の値。
   - WB: 名前を確かめた種類だけ（Automatic・Daylight・Fluorescent・Tungsten・Flash・Cloudy・Shade）。Manual・One-push Automatic・Color Temperature・Preset は本体の別の中身に頼るので、選ばない。種類ごとの微調整、Automaticの時の型（`0xD141`）、Fluorescentの時の型（`0xD14F`）、プリセットの中身、色温度は書かない。
   - 測光: 中央重点・マルチパターン・スポット（値2〜4）。値1は公開の資料に名前が無いので選べない（「写す」では1〜4のまま）。
   - ISOの幅と1秒の上限はコンパイル時の定数で、`static_assert`が上限を見張る。広げるのは決定とレビュー付きのcommitで、実行時の入力では変わらない。
4. **計画**: 「N 件を書き込む…」を押すと、書き込みの経路が自分で両台を読み直し、その最後の完全な読み取りから計画を作る。1つでも拒否があれば計画全体を拒否する（一部だけ通さない）。今の値と同じ選びは書かない。確定の画面に台ごとの「今の値 → 書く値」と書く順を出し、人が確定を押した時だけ書く（ADR-0038決定5の確定の手順）。
5. **「Aと同じにする」**: CAM-Bの行で選べる。計画を作る時に、CAM-Aの選び（無ければCAM-Aの今の値）で解く。CAM-Aの値を元にするだけで、CAM-Aに書かない時は、CAM-AがMである事までは求めない（書く台がMとISO自動OFFを読む事は変わらない）。CAM-Aの今の値で解いた時は、その値が実行の前に変われば送らない。
6. **書く順**: CAM-Bの項目を全部書いて読み戻してから、CAM-Aの項目を送る。台の中は表の順。CAM-AとCAM-Bを同時に開かない（ADR-0038決定6）。
7. **gateと記録**: gateは、そのsessionを開いた1台の、確定した計画の項目にだけ標準の`0x1016`を通す（ADR-0038決定9のArmを台ごとにした形）。どちらかの台で1項目でも失敗・不明になれば、操作の掛け金が落ち、どちらの台にも残りを送らない。最初の`0x1016`の前に、計画と書く相手すべての前の値を記録に書いてflushし、書けなければ1件も送らない（ADR-0038決定11を両台に広げた形）。書く直前に、書く相手ごとにLive状態（`0xD1A2`が0）・露出モード・ISO自動・その項目のPropDescを読み直す。期限は両台の残りの項目の全部で数える（秒数は`docs/WPD_DUAL_PREVIEW.md`。※仮）。
8. **1回だけ、巻き戻さない**: 各命令は1回だけ送り、再送しない。前の値への自動の書き戻しはしない（ADR-0038決定7）。
9. **撮影の止め**: 1件以上送った書き込みが一致で終わらなければ、撮影を止める。止めが値を選ぶ計画を含む履歴の時、解けるのは、読み直しで、計画した項目（写す操作の項目も含む）がそれぞれの台で最後に計画した値を返し、それらの台がMとISO自動OFFを読む時だけ。CAM-Bの書き込みが途中で失敗・不明になった時も、まだ送っていないCAM-Aの値は止めの対象に残る（解くには、CAM-Aを本体でその値にする）。記録の一部が読めない時、または送った可能性のある操作のstepの行が計画の行と合わない時は、ADR-0038決定8の比較（止めの対象の項目がCAM-AとCAM-Bで一致）でだけ解ける。記録があって読めない時は解かない。本体の別の中身に頼るWBの値は一致と数えない。窓を閉じても再起動しても解けない。不一致だけの止めの間は、それを直す計画を通す。それ以外の止め（不明・終わりの記録が無い・記録が読めない）の間は、計画を作らず実行もしない。
10. **ライブの再開**: 設定の画面はライブの領域と入れ替わる。画面を閉じた時にライブを自動で再開するのは、画面自身がライブを止めた時だけで、撮影の止めが掛かっている間、撮影の失敗の後、切断の失敗の後、窓を閉じる途中、読み取り・読み直し・実行が終わらなかった時は、自動では再開しない。手の「ライブ再開」では再開できる。
11. **入口**: 画面の操作だけ。コマンドラインの入口とUIの自動操作の入口は無い。AIは、所有者がその1回を明示して許した時（書き込みは計画を見た後）を除き、設定の画面の開閉・値の選択・書き込みと確定のボタンを、UIの自動操作やscriptで押さない。
12. **有効にする手段**: 有効な行・計画の種類・property code・本体の機種と版は、リポジトリの中の固定の表（コンパイル時の定数）だけで決め、レビュー付きのcommitでだけ変える。ファイル・環境変数・レジストリ・コマンドラインの引数・UIの自動操作では何も有効にならない（ADR-0038の範囲の原則と決定13）。CAM-Aには、表がCAM-Aのその行を有効にし、本体が表の名前のCAM-Aの機種と版を返す時だけ書く。本体の版の文字列は本書に書かない。
13. **使わないもの**: `0x90C2`（ChangeCameraMode）、`0x9207`、ほかのvendor operation、firmware・カード・記録先・カスタムメニューの変更は、本ADRでも認めない。書いたことを承認済みのcapture profileとの照合の代わりにしない（ADR-0038不変条件6）。

### 有効にする順

承認の後も、段を1つずつ進め（段T3のコードはCAM-Aの残り4項目まで開く）、どの段も実機の書き込みの前に、所有者の別の承認（対象・項目・回数・止める条件）を取る。複数項目の計画は、その各項目がその台で単独で一致した後にだけ試す。ただしCAM-Aは、最初の1項目がCAM-Aで単独で一致した後なら、同じ機種・同じ版のCAM-Bで単独で一致している項目をCAM-Aにまとめて書いてよい（AGENTS.mdの決まりの文）。

| 段 | 開くもの | 次の段へ進む条件 |
|---|---|---|
| T1 | CAM-Bに選んだ値（`kD810Adr41Accepted`と`chosen_values`を真にする。表の行は変えない） | CAM-Bで1項目を書き、読み戻しと読み直しが一致した |
| T2 | CAM-AのISOの行と、CAM-Aの本体の版の欄（CAM-Bの欄を参照で入れる。所有者の決定11） | 所有者がCAM-Aを読んだ時の照合で`firmware_match=1`になり、CAM-AのISOを書いて、読み戻しと読み直しが一致した |
| T3 | CAM-Aの残りの行（シャッター速度・絞り・WBの種類・測光）の4行を、1 commitで開く。確定は1回で、4項目を表の順に書く。途中で一致以外になれば、その項目で止まり、残りは送らない。4項目を1回の確定にしてよいのは、表のCAM-Aの版の欄がCAM-Bの欄の参照のままで、T2の読み取りでCAM-Aの本体の照合が一致した時だけ。版が違う時は、CAM-Aの行を1段に1行ずつ開く | 4項目が読み戻しと読み直しで一致した（途中で止まった時は、読み直しの後、別の承認で残りを選び直す） |

T3の確定の前の条件（所有者の承認に入れる）: (1) 計画はCAM-Aだけの4項目で、CAM-Bの項目を同じ確定に混ぜない。(2) 直前のT2の結果がCAM-Aの読み取りで`firmware_match=1`、書き込みが一致、読み直しで止めが無い。(3) 両台に同じ型のレンズが付いている。(4) WBは自動・蛍光灯以外の名前のある種類にする（自動・蛍光灯の型は書かないので、色まで揃うとは限らない。型の差を承知の上で選ぶ時は、その旨を承認に書く）。(5) 4項目が書く段の期限に収まる事。T2の確定から結果までの秒数の4倍が40秒を超えるなら、T3を2回に分ける（実機のT3では、4項目は1回の確定で書く段の期限に余裕を持って収まった）。

各段のcommitの前の作業（レビュー・独立の試験・漏えいの照合など）は、securityの確認（#416）の申し送りに従う。中身は※要確認（本書には写していない）。

### 増える危険と、それへの手当て

- CAM-Aへの書き込み: CAM-Aは合成と校正の基準で、承認済みのprofileとも照合される。手当て: CAM-Aは最後に書き、CAM-Bで失敗・不明になればCAM-Aへは1件も送らない（決定6・7）。CAM-Aに書く計画の確定の画面には、基準のカメラである事と、profileと違う値になると撮影の前の照合で止まる事を出す。CAM-Aの行は段T2（ISO）とT3（残り4項目）で開く。
- 片方だけ書けた状態: CAM-Bだけが変わってCAM-Aが変わらない状態が残りうる。手当て: 計画したすべての値（送っていないCAM-Aの値も含む）を読み直しで確かめるまで撮影を止める（決定9）。巻き戻しはしない。
- 台ごとに違う値: 左右の露出が違うまま撮れる。手当て: 違う値は操作者が選んだ時だけで、確定の画面に台ごとの値を並べる。左右の明るさの比は合成の側（#415）で扱う。
- 値が同じでも色が合わない: WBのPreset・Color Temperatureや、自動・蛍光灯の型は本体の中身に頼る。手当て: 選ばない・書かない。止めの比較では一致と数えない（決定3・9）。
- 有効にする手段の抜け道: 手当て: 固定の表とレビュー付きのcommitだけ（決定12）。承認や「実機で確かめた」に見えるファイルを置いても何も有効にならない事を試験で固定している（`docs/WPD_DUAL_PREVIEW.md`の試験の項）。
- 撮影の前提との衝突: 画質・画像サイズ・フォーカス・記録先、撮影や時間の動きを起こす設定（インターバル、ブラケティング、セルフタイマー、長秒時ノイズ低減など）は項目に入れない（決定2）。

### 改定の対象

| 対象 | 状態 |
|---|---|
| AGENTS.md（#416の段Bの項と、ADR-0041の例外の項） | 所有者が承認した文面が入っている。複数項目の計画の条件に、CAM-Aについての例外を1文足した（所有者の決定10） |
| ADR-0038 決定2・3、不変条件2（CAM-Bだけに書く、Aから写すだけ、Aには書かない） | 段Bの値を選ぶ計画については本ADRが上書きする。ADR-0038の本文の書き換えは別の変更で行う（※要確認） |
| ADR-0038 決定8（止めを解く比較） | 値を選ぶ計画を含む履歴では決定9の規則。写すだけの履歴はADR-0038のまま |
| `docs/WPD_DUAL_PREVIEW.md` | 値を選ぶ画面と計画の節がある（表の行は段ごとに開く。今の段は同ADRの冒頭） |
| `docs/D810_PC_CONTROL_CAPABILITIES.md` | 段T1以降で行を開いた時に追従する |

### 範囲外と未決

- 本番の画面（段C）へ広げる事。本ADRは段Bに限る（ADR-0038の段Cの前提条件は変えない）。
- 露出補正・ピクチャーコントロール・アクティブD-ライティング・色温度・微調整・プリセットの番号の選択・白を測る操作。どれも別の決定が要る（ADR-0038の改訂案その2の案B〜F）。
- 承認済みのcapture profileの照合は測光を比べない（`src/m3/OperatorShell/Hardware/HardwareSingleCaptureProfileStore.cs`）。測光の食い違いを撮影の前に見つけるのは決定9の止めだけ（※要確認: profileの照合に足すかは段Cの論点）。
- 実機のPropDescが列挙か範囲か、ISOのLo・Hiの符号化、日本語の表示名、書く段の期限（※未検証・※仮。`docs/WPD_DUAL_PREVIEW.md`の※未検証の項）。

## ADR-0043: 継ぎ目の色の補正は、値の推定をPythonの道具で行い、画像に当てる式はC++の1つにする

- 状態: Accepted（#429、所有者）。承認の中身は下の「ADR-0043の所有者の決定」の1〜6。本ADRは置き場所と境目を決めたもので、計画を画像に当てる機能を有効にしたものではない。C++の計画の読み手と当てる関数は未実装で、アプリからPythonを呼ぶ所は無い。
- 決定日: #429の所有者の決定による（日付はIssueの記録による）
- 関連: #429、#415（今の色の合わせ方）、#434（校正の流れ）、ADR-0036 決定5（OpenCVを開発ツールだけで使い、製品に同梱しない）、`HG-0005`、`python/README.md`、`python/requirements.txt`、`python/requirements-dev.txt`
- 背景: これまでのコードはC++（合成、カメラの通信）とC#（操作画面）だけで、Pythonは無かった。継ぎ目の色は、合成のたびにC++が測る今の合わせ方（#415）で揃えている。実機では、継ぎ目に残る色の差の中心は、照明の映り込みが左右のカメラで違う場所に出る事だった。計算だけの補正では継ぎ目が分からない所までは届かず、値の求め方はまだ変わりうる。一方で、ライブの合成Cと撮った画像の合成Cで色の当て方が違えば、ライブを見て確かめる意味が無くなる。

### ADR-0043の所有者の決定（#429）

1. Pythonをリポジトリに入れてよい。
2. アプリ本体（画面、カメラの通信、撮影、設定の書き込み）はPythonに移さず、今の言語のままにする。
3. 映像の合成（ライブのC）はC++のまま。
4. 色合わせはPythonで作ってよい。
5. ライブのCと撮った画像のCで、継ぎ目の場所、位置の合わせ方、手順を食い違わせない。
6. 色合わせのPythonは、校正の時と開発の時だけ動く道具にする。撮影のたびには呼ばない。アプリにPythonとnumpyを同梱しない。

### 決定

1. **役割の分け方**: 継ぎ目の色の補正の値は、Python（numpy）の道具`python/a0seam`が求め、計画のファイル`a0.seam-color-plan.v1`に書く。Pythonが書くのは計画だけで、画像を描かない・保存しない・変えない。計画を画像に当てるのはC++の1つの関数とする。
2. **同じ計画、同じ式**: ライブのCと撮った画像のCは、同じ計画を同じC++の関数で当てる。結び目の間の補間（文書のy（mm）について線形、両端は端の値を保つ）も、その関数の中の1か所だけに置く。
3. **境目は計画の書式**: C++が読むのは計画の契約の欄だけで、`diagnostics`（Pythonの証拠）は解釈しない。推定の方法を変えても、後で推定をC++に移しても、計画の書式が同じなら描く側とライブは変わらない。
4. **Pythonが動く時**: 校正の時（色の基準を作る段）と開発の時（保存済みの組から計画を作り、C++で描き直して比べる）だけ。撮影とライブのCはPythonを起動しない。今のアプリにはPythonを呼ぶ所が無い。校正の流れから呼ぶのは#434の後の段で、別の作業とする。
5. **Pythonが無いPC**: 撮影はでき、色は今の合わせ方（#415）のまま。計画が無い時、計画を当てられない時も同じで、撮影もライブも止めない。
6. **同梱しない**: アプリはPythonとnumpyを同梱しない。道具を使うPCには、Python 3.12と`python/requirements.txt`のパッケージを入れておく。
7. **依存**: 実行時の依存はnumpyだけとし、`python/requirements.txt`で版を固定する。試験の依存（pytestとその依存）は`python/requirements-dev.txt`に分け、これも版を固定する。

### 理由

- 撮影のたびに推定すると、値が撮影ごとに変わり、ライブのCと撮った画像のCの色が食い違う元になる。Pythonの有無で撮影の結果が変わる事にもなる。
- 推定を1か所（Python）、当て方を1か所（C++）に置けば、静止画とライブが同じ式を通る事が、運用の注意ではなく仕組みで決まる。
- 推定もC++に書くと、同じ方法を2つの言語で持ち、方法を変えるたびに2か所を直す事になる。所有者は色合わせをPythonで作る事を選んだ。配るPCにPythonを入れられないと分かった時は、計画の書式を変えずに推定をC++へ移せる。

### やらない事

- 撮影のたびの推定（撮影ごとにPythonを呼ぶ事）。
- Pythonで画像を描く事。試験の基準と、開発の道具で比べるための描画は除く（どちらも製品の出力には使わない）。
- 製品の計算にOpenCVのPython版を入れる事。`a0seam`はOpenCVを使わない。

### 影響

- リポジトリに`python/`が入った: `a0seam`（推定の部品）、起動口`estimate_seam_row_gain.py`、合成した画像だけを使う試験（`python/tests/`）、`requirements.txt`、`requirements-dev.txt`、`README.md`。C++とC#のコード、ビルド、製品の出力は変わらない。
- 依存の扱い: ADR-0036 決定5は変えない。PythonとnumpyもOpenCVと同じ扱いで、リポジトリは同梱せず、requirementsで版を指すだけにする。アプリと一緒に配る事になったら、`HG-0005`の範囲にPython本体とnumpyのnativeの部品を足して別に決める。その時はライセンスのファイル一式も一緒に配る（numpyのWindows向けwheelは、OpenBLAS、LAPACK、libgfortranなど別のライセンスの部品を含む。一覧は`python/README.md`）。
- 文書: `docs/ARCHITECTURE.md`の技術スタックと、`docs/CURRENT_STATUS.md`の配布の行・`HG-0005`の行・Partialに追従した。`AGENTS.md`のDevelopmentの行は、所有者が差分を見て承認してから直す（未反映）。

### 未決（※要確認）

- 計画を読むC++の読み手と、計画を当てる関数（未実装）。読み手が拒否する条件と、書式の文書もその作業で決める。
- 校正の流れのどこで推定を動かすか（#434の後の段）。アプリがPythonを子プロセスとして起動する時の決まりは、securityのレビューを通す。
- 基準の撮影（白と黒）から値を求める方式への移行。同じ計画の書式に、閉じた符号を足して入れる見込み。
- Pythonとnumpyを配る事になった時の扱い（`HG-0005`）。

## ADR-0044: 開発用プレビューの撮影を、ライブビューを開いたまま AF なしの SDRAM 撮影で行う（段P・段1・段2）

- 状態: Proposed（#426 段階4）。段Pの決まりの文だけを先に承認してもらい、段1・2は段Pの実機の結果を見てから決める。
- 決定日: 未定
- 関連: #426（段階4）、#389（二台撮影の1取引）、#381（完了通知の観測）、#343・#345（撮影前の点検）、#430（未分類の知らせ0x4005）、#373（ライブの連続時間）、#331（AF・微調整）、ADR-0036（不変条件2）、ADR-0035（不変条件3）、ADR-0032（1-A）、ADR-0038・ADR-0041（決定13）、`docs/WPD_STILL_CAPTURE.md`、`docs/WPD_PREVIEW_FOCUS.md`、`third_party/PTP_PROTOCOL_REFERENCES.md`
- 背景: 開発用プレビューは、撮る前に両方のライブを止めて閉じ、AFなしのSDRAM撮影（`0x90C0` / `0xFFFFFFFF`）を送っている。両台のフォーカスモードセレクターがAFのまま、PCのAFで両台とも合焦の応答を得た後に二台撮影をすると、片方は保存され、もう片方は撮影の後のDeviceReadyでOutOfFocus（`0xA002`）を返して止まった。アプリはAFを送っていないので、ライブを閉じた後に本体が自分の合焦の判定でレリーズを止めている見立て（※未検証）。セレクターをMにすれば撮れるが、AFのたびに本体に触る事になる。公開の2つの実装（digiCamControl、libgphoto2）は、ライブ中の撮影ではライブを止めずにAFなしで撮る。メーカーのPC制御ソフトもライブビューの窓から撮る。D810がライブ中の`0x90C0`を受けるか、その時にAFが走るか、撮影の後にライブが続くかは、公開の資料に無い。

### ADR-0044の所有者の要件と選択（#426）

1. 要件: 「AF と シャッターボタンは別で考える必要があります。カメラコントロールプロで撮影するときは、撮影ボタン押下したら AF→シャッターとはなりません。AF(手動)→シャッター（手動）で動作しなければいけない。」
2. 運用: 両台のフォーカスモードセレクターはAFのまま。PCのAFボタンで合わせ、PCの撮影ボタンで撮る。本体には触らない。
3. 選択: 本体のカスタムメニューa2を変える案は採らず、ライブを開いたまま`0x90C0` / `0xFFFFFFFF`で撮る案で進める（「２で進めて」）。`0x9207`は使わない。

### 決定

1. **段P（探り）**: 既定OFFのbuild option `A0_BUILD_WPD_LIVE_CAPTURE_PROBE` の実行ファイルだけに、画面の操作を1つ足す。固定の定数が名指す1台（CAM-Bが先、CAM-AはCAM-Bの結果を見たレビュー付きのcommitの後）に、1プロセスで1回だけ、ライブを開いたまま`0x90C0` / `0xFFFFFFFF`を送る。応答、撮影の後のDeviceReady、シャッターの前後のeventのcodeと数、objectの有無、完了通知、撮影の後のライブの状態を記録する。画像は1回だけ取り込み、本体に残さない。シャッターが拒まれた時、またはシャッターの後に想定の順序から外れた事（結果が不明、撮影の後または取り込み前のDeviceReadyがbusyの後の`0x2001`でない、想定外のevent、objectが2個、`0x4002`、完了通知の観測中のeventの読みの失敗など）が起きた時は、そこで止まる。その後に送るのは、既存の上限の内で帰属した1個を探すためのeventの読みと、その1個の`0x1008`・`0x1009`を1回（partialへ）だけで、完了通知の観測、`0xD1A2`の読み直し、ライブの1枚読みは送らない（ログに`live_after=not_read`と残す）。最後は両方のライブを「ライブ停止」と同じ経路で止め、Startは送らない。撮影の経路は変えない。
2. **段1（単台）**: 段Pで両台とも撮影が通り、AFが走らず、撮影の後のライブの状態が分かった後に、単台の撮影ボタンをライブを開いたままの経路に替える。
3. **段2（二台）**: 段1の後に、#389の1取引をライブを開いたままの順に替える。両方のworkerを止める → A、Bの順にそれぞれのsessionで点検 → Aのシャッター → Bのシャッター → Aの取り込みと保存 → Bの取り込みと保存 → 両方の完了の観測 → ライブの扱い。
4. **sessionの持ち主は1人**: 撮影は、workerが今のフレームの転送をENDまで終えて止まった後に、そのsessionを借りる。1つのsessionに送るのは、その時の持ち主（workerか、借りた撮影）だけ。持ち主の切り替えは1つのatomicで行う。
5. **専用のgate**: 借りたsessionでだけ、静止画の撮影と同じ命令（DeviceReady、撮影の設定の3つの読み取り、CheckEvent、1回のシャッター、認めたobjectの情報とデータ、自分の転送のREAD・END）を通す。ライブ・静止画・設定の既存のgateは変えない。
6. **ライブの条件**: 撮る前に`0xD1A2`が1である事を求める。ほかの点検（JPEG Fine、FX L、eventの基準線、DeviceReady）は今のまま。
7. **eventとobject**: ledgerは今の`WpdStillEventLedger`の規則。足してよいのは、`third_party/PTP_PROTOCOL_REFERENCES.md`の公開の出典に名前があり、段Pで観察したcodeだけで、レビュー付きのcommitで足す。名前の無いcodeはシャッターの後なら止める。画像の持ち主は、そのsessionでその取引のシャッターの後に来た`0xC101` 1個だけ。
8. **撮影の後のライブ**: `0xD1A2`を1回読む。1で次のフレームが普通に読めればそのまま続ける（Startなし）。0なら両方を止めて閉じ、今の撮影の後と同じく新しいchannelで開き直す。それ以外、または撮影の失敗・不明では、両方を止めて自動では開き直さない。段Pでは、想定外の後はこの読み直しも行わない。段Pの結果に合わせて、使わない分岐は決定から外す。
9. **ライブの連続時間**: 撮影を始めるのは、撃つカメラのライブの開始から段Pで5分、段1・2で7分（どちらも※仮）を過ぎていない時だけ。本体のc4（10分）の自動終了と撮影の取引を重ねない。
10. **使わない物**: `0x9207`、`0x90CB`、撮影の取引の中でのAF・レンズ駆動・AF枠の移動・ライブの開始と終了・フレームの取得、設定の書き込み（a2、AFモードを含む）、カード・記録先の操作、`0x90C3`とobjectの削除。
11. **入口**: 画面の操作だけ。人が押す。コマンドライン、ファイル、環境変数、UIの自動操作では有効にならない。AIはUIの自動操作やscriptで押さない。実機で使う時は、そのたびに所有者が対象・回数・止める条件を承認する。
12. **今の経路を残す**: ライブを閉じてから撮る経路は、コマンドラインの撮影の診断（`--capture-a`・`--capture-b`・`--capture-smoke`）の経路として残す。ボタンをどちらの経路に向けるかは、レビュー付きのcommitだけで変える。

### 守る不変条件

1. `0x90C0` / `0xFFFFFFFF`は1台に1回。再送しない。channelとledgerは取引ごとに作り直す。
2. 1つのsessionに同時に送る者は1人。持ち主の切り替えはフレームの転送の境目でだけ。
3. 撮影の取引の中でAFもレンズ駆動もAF枠の移動もしない。AFと撮影を1つの操作にまとめない（#426の段階5の「枠＋AF」に撮影は入れない）。
4. 本体の設定を書かない。`0x9207`を送らない。
5. 画像の持ち主が曖昧なら`original.jpg`にしない（partialだけ）。古い画像を代わりに使わない。
6. SDKのsessionを開かない。WPDのsessionは2つまで。leaseを全sessionのCloseまで持つ。
7. 期限を過ぎたら、次のtransport・保存・成功状態を始めない。取得済みのpartialとoriginalは消さない。
8. 本体の応答と光学的な合焦を同じものとして扱わない。

### 退ける案

| 案 | 退ける理由 |
|---|---|
| ライブを開いたまま`0x9207`で撮る | AGENTS.mdの36・63行、ADR-0036決定2、ADR-0041決定13が禁止している。digiCamControlはD810の型でも、ライブ中は`0x90C0`だけで撮っている（NikonBase.csの`CapturePhotoNoAf`、D810の型はこれを上書きしない）。段Pで`0x90C0`が通れば要らない。通らなかった時は、所有者の判断の材料として出す |
| 所有者が本体でa2（AF-S時の優先）をレリーズに1回だけ変える | 所有者が本体の操作をしないと決めた。撮影の時にAFが走る場合には効かない |
| PCからa2（`0xD049`）を書く | AGENTS.mdの49行が名指しで禁じている。カスタムメニューの変更はADR-0041決定13の外。値の名前は他機種の表にしか無い |
| 撮る前にAFモード（`0xD161`）をMFに書き、撮った後に戻す（digiCamControlのライブの外の方法） | 撮影の取引の中に書き込みが2回入る。戻しに失敗するとMFのまま残る。AGENTS.mdの35・49・58行、ADR-0041決定2に当たる |
| a4をAF-ONのみにする | 説明書では、a2がフォーカスのままなら合焦できない時は切れない。単独では効かない |
| AF-Cにしてa1をレリーズにする | 本体の操作が要る。AF-Cは押している間合わせ続けるので、撮影の時にAFが走る危険が大きい |
| 今のまま（AFで合わせた後に本体でMにする） | AFのたびに本体に触る。Mの運用でピントがずれた記録がある |
| 撮影の段をworkerに頼む形（sessionを貸さない） | #331の「workerだけが送る」は守れるが、#389の取引のコードを作り直す事になり、A・Bのシャッターの間にスレッドの受け渡しが入る |
| 撮影の後は毎回ライブを閉じて開き直す（今と同じ） | 段Pでライブが続くと分かれば要らない。毎回のStartは、Startが`0x2019`を返した記録があるので失敗の元を増やす。ライブが撮影で終わる本体では、この形を決定8の分岐として残す |

### 実機の受入

- 段P（CAM-B、次にCAM-A。それぞれ別の承認で1回）: 押す前にPCのAFで合わせ、PCの「近」でわざとぼかす。記録が揃うこと（シャッターの応答、撮影の後のDeviceReady、eventのcodeと数、objectの取り込みの結果、`0xD1A2`、撮影の後の1枚、AF枠の言葉の前後）。再送が0、設定の書き込みが0、Startが0、ほかのカメラのI/Oが止めの間に0。保存した画像がぼけたままなら、撮影の時にAFは走っていないと読む。
- 段1（各カメラ1回ずつ、合焦させた状態で）: 保存され、OutOfFocusが出ない。撮影の後のライブが決定8のとおりに扱われる。続けて同じカメラでもう1回撮り、前の撮影の完了通知が残って止まらない事を見る。
- 段2: 二台撮影を1組。両方が保存され、再送が0、`shutter_gap_ms`を記録する。その後、同じ窓で2組目（#343と同じく、表示された結果を見てから）。
- 品質はこれまでどおり#385のゲートで見る。本ADRの受入は、撮影が要件どおりに動く事の確認で、合成の品質の受入ではない。

### 影響と改定の対象

| 対象 | 扱い |
|---|---|
| AGENTS.md | 段Pの項を足す（この枝に入れてある。所有者が差分を見て承認するまでmainに入れない）。段1・2は36・40・47・49・52行の差分と新しい項 |
| ADR-0036 不変条件2（撮影の前にすべてのライブを閉じる） | 開発用のWPDプレビューに限って本ADRが例外にする。製品の経路（WPF・SDK）には効かせない |
| ADR-0035 不変条件3、ADR-0032 1-A | 製品の経路の決まりなので変えない |
| `docs/WPD_STILL_CAPTURE.md` | 段Pの節、段1・2の順序と表 |
| `docs/WPD_PREVIEW_FOCUS.md` | 「通信はカメラごとのworkerだけ」を「その時の持ち主だけ」に直す（段P） |
| `third_party/PTP_PROTOCOL_REFERENCES.md`、`third_party/digicamcontrol-NOTICE.txt` | 段Pの出典の節 |

### 未決（※要確認）

- D810がライブ中の`0x90C0`を受けるか、AFが走るか、撮影の後にライブが続くか、どのeventが来るか（段Pで確かめる）。
- ライブ中の撮影で、本体のc4の時計が戻るか。
- 取り込んだ後のSDRAMの画像を、削除の命令なしで本体がどう扱うか。
- 撮影の後に、AF枠の位置やライブの拡大の状態が戻るか。
- 期限と時間の上限の値（60秒、5分、7分、3秒）。

### 段Pの実装の置き場所

- 許可リストと手順: `src/phase0/include/a0/phase0/experimental/wpd_live_capture_probe.hpp`（このheaderはoptionが無いと`#error`で止まる）。sessionの持ち主の状態: `preview_session_owner.hpp`。画面とCameraの側: `src/phase0/experimental/wpd_dual_preview_live_probe*.hpp`。
- 試験: `wpd_live_capture_probe_contracts`（許可リストと手順を偽の本体で）、`preview_session_owner_contracts`、`preview_live_capture_probe_ui_contracts`（optionがONのbuildだけ。カメラを使わない）、`preview_live_capture_probe_binary_contracts`（OFFのexeに入口が無いこと）、`preview_live_capture_probe_source_contracts`（シャッターの組み立てと送信が1か所、ボタンへの合成クリックが試験の中だけ）。
- 手順と実機の操作: `docs/WPD_STILL_CAPTURE.md`の「ライブを開いたまま撮る探り」。

## ADR-0045: 4台（2×2）の配置に備えて、カメラの名前・位置・継ぎ目と、基準と対象のカメラの欄の呼び方を先に決める（文書だけ）

- 状態: Proposed（#438）。2026-11-18に所有者が、3台目・4台目のカメラの名前、合成の新しい呼び名、その呼び名に直す時期の3点を決めた（下の「ADR-0045の所有者の決定」）。ほかの点は決定待ち。文書だけの案で、コード・書式・試験は変えない。ただし#434の記録3種だけは、2026-11-19の所有者の決定により、本ADRの採用を待たずに先に形を変えた（下の「ADR-0045の所有者の決定」、今やる物の2）。
- 決定日: 未定（上の3点は2026-11-18）
- 関連: #438、#434（撮影の1組からの校正の取り直しと、その記録3種）、#429（継ぎ目の色の計画）、#437（ライブのCの位置P）、ADR-0030、ADR-0031、ADR-0034（rig profile 2.0.0）、ADR-0036（表示専用の合成C）、ADR-0037（縦一直線の継ぎ目）、ADR-0038・ADR-0041（設定の書き込み）、ADR-0043（色の計画）、ADR-0044（不変条件6）、`docs/design/rig-profile-v2.md`、`docs/design/fixed-composite-registration-formats.md`、`docs/design/recalibration-from-pair.md`
- 背景: 今はD810を2台、左右に並べて使っている（CAM-Aが左、CAM-Bが右。吊り下げで、合成の前に両方の画像を180°回す）。上段に2台・下段に2台の2×2、合わせて4台の配置が前提になる可能性が高い。まだ確定ではない。確定してから書式を直すと、公開済みの書式の版上げと、ライブのCとJPEGのCの一致の試験の書き直しが要る。確定の前に位置合わせや画面を4台用に作り変えると、使わないかもしれない作りを抱える。そこで呼び方だけを先に決め、これから足す書式・記録の欄を、4台になっても名前を変えずに済む形にする。

### ADR-0045の所有者の決定（#438）

2026-11-18に、所有者が次の3点を決めた。

1. 3台目・4台目のカメラの名前は`CAM-C`・`CAM-D`とする（決定1の表の案2）。今の2台は`CAM-A`・`CAM-B`のまま。
2. 合成（今「C」と呼んでいる、ライブの合成の画面と、保存する合成の画像）の新しい呼び名は「M」（MergeのM）とする。
3. 合成の呼び名を実際に直す作業（画面の題と見出し、その文字を見る試験、docs）は、4台が確定してから行う。それまでは今の「C」のまま。

この3点を、決定1と決定6、守る不変条件6、今やる物の4、待つ物に入れた。ほかの点（下の「所有者に確かめる事」の表）は決定待ちで、状態はProposedのまま。

2026-11-19に、所有者が#434の記録3種（推定の記録・校正の報告・確定の記録）について次の3点を決めた。決定4・5を、この3種の分だけ先に決めた事になる。ADR全体の状態はProposedのまま。

1. 形は「揃える」案（基準・対象のカメラの欄を足し、欄の名前にA・Bを埋めている所をカメラの名前で引く形に改める）。
2. 版はv1のまま。公開した後に形を変える時はv2にする。
3. 校正をONにしたビルドを実機で動かした事は無い。

これに合わせて決めた事（設計とレビュー。所有者の決定ではない）:

- 欄の名前: `referenceCamera`・`targetCamera`を3種に足し、推定の記録の`inputs`と校正の報告の`originals`を`originalSha256`（aliasを鍵にしたオブジェクト）に、`cameraBResidualPx`を`targetResidualPx`に、校正の報告の`edgeShareA`・`edgeShareB`を`edgeShare`（aliasを鍵にしたオブジェクト）に改めた。
- 版の決まりの書き方: 公開のmainに入った後に、欄を足す・除く、欄の名前・型・意味を変える時は新しい版（v2）にする。`docs/design/recalibration-from-pair.md`の6節に書いた。
- 書式の文書（`docs/design/fixed-composite-registration-formats.md`）の1節の決まり8は条件v3・報告v8の決まりで、記録3種には及ぼさない（未決の1）。
- 決定3から、実機に改める前の形の記録は無い前提とし、改める前の形を読む仕組みは作らない。

所有者の決定とこれに合わせて決めた事を、決定5の表、今やる物の2、影響と改定の対象、未決の1、所有者に確かめる事の表に入れた。

### 今の呼び方（コードで確かめた事）

2026-11-18に、手元のmain（ee77d69）の木をgrepとファイルの読み取りで確かめた。行番号はこのcommitの物。「C」と「M」の数は、ee77d69に本ADRを足しただけの2f93cccの木で数え直した（コード・試験・ほかの文書はee77d69と同じ）。

- カメラの名前はalias `CAM-A`・`CAM-B`。本体とaliasの対応は照合のファイル（`IdentityMap`）に置き、リポジトリには本体の識別子を書かない。
- 書式の中でカメラを書く形が4つ混ざっている。
  1. aliasを鍵にしたオブジェクト: rig profile 2.0.0の`cameras`・`calibration.inputSha256`・`calibration.fitResidualPixels`（`docs/schemas/rig-profile.v2.schema.json`）、照合のファイル。
  2. aliasを値に持つ欄: 報告v1〜v8の`inputs[].alias`、`a0.saved-review-evaluation-inputs.v1`の`cameraAlias`。
  3. 欄の名前や符号に記号を埋めた物: #434の記録の`cameraASha256`・`cameraBSha256`・`cameraBResidualPx`（2026-11-19に決定4の形に改めた）、色の計画v1の`bindings[].kind`の`capture-original-a`・`capture-original-b`。
  4. 名前と位置を1つにした符号: profileの`documentPlane.cameraOrder`（`CAM-A-left-CAM-B-right`・`CAM-A-top-CAM-B-bottom`）と、コードの`StitchLayout`。
- 左右は文書の座標（x右・y下、profileの`conventions.documentPlane`）で、取付の向きを当てた後の向き。吊り下げでは画像の左右が部屋と逆に写るので、`rotated-180`では両方の画像を180°回してからAを左、Bを右に並べる（`docs/WPD_PREVIEW_MERGE.md`の#361の節）。取付の向き（条件v3の`mountOrientation`）はリグ全体で1つの値で、片側だけの回転は対象外。
- 基準と対象: 条件v3の`basePose`、報告v8の`registration.pose`、ライブの位置P（`live_placement.hpp`）、#434の推定の記録の`estimate.pose`は、どれもCAM-Aに対するCAM-Bの姿勢で、どちらのカメラかを書く欄が無い。色もAを基準にBへ倍率を掛ける（`seam_blend.hpp`の#415の注、ADR-0043）。
- 継ぎ目: 条件v3と報告v8の`seam`は出力の列（`column`）1つで、CAM-AとCAM-Bの間の縦の継ぎ目1本を暗に指す。色の計画v1の結び目は、継ぎ目に沿った位置`yMm`。
- 「C」: ライブの合成と保存の合成を「C」と呼んでいる。ADR-0036の題（表示専用の合成C）、画面の題「A/B Live + C（…）」（`preview_window_title.hpp`）、見出し「C 仮Merge（未校正）」（`preview_merge.hpp`）、書式の文書の「LC＝ライブの C の担当」「C の px」、開発用プレビューのログの行の頭の「C 」（`C live_placement serial=…`など）、試験の名前`preview_fixed_c_ui_contracts`・`preview_live_c_ui_contracts`とそのsmokeの出力の鍵`fixed_c_ui_*`・`live_c_ui_*`。docsでは`docs/`の24のファイルと`AGENTS.md`が「ライブのC」「合成C」「Cの画素」などの形でCを合成の意味に使う（一覧と数え方は「待つ物」の「合成の呼び名をCからMに直す範囲」）。一方で文の中のカメラは、単独の「A」「B」で書いている（「Bの姿勢」）。
- `CAM-C`は今、試験で「受け付けないalias」の例に使っている（docsを除き11ファイル18行。`tests/phase0_tests.cpp`、`tests/dual_bound_live_view_tests.cpp`、`tests/fixtures/calibration-session/negative-cases.json`、`scripts/Test-RigProfileV2Schema.ps1`など。`git grep -c CAM-C`）。docsでは本ADRだけが`CAM-C`を書く。
- 単独の「C」には合成のほかの意味もある。ADR-0038の段C、回復コマンドC（`docs/design/dual-preview-topology-diag.md`、`docs/DUAL_LIVE_WORKER_POC.md`、`docs/MACHINE_OPERATION.md`）、確認点・案の記号（「確認点 A・B・C」「案C」）、式の中のC（`docs/M2_CALIBRATION_LENS.md`の費用C）。

### 「M」の今の使われ方（grepで確かめた事）

2026-11-18に2f93cccの木で、単独の「M」と、「M」に数字や記号が続く形を探した（コードは文字列の中、文書は`` ` ``の外の文。数は本ADRの行を除く）。合成をMと呼ぶ所有者の決定は変えない。直す作業の時に書き分けが要る所として書く。

| 今の「M」の意味 | 場所 | 合成のMとの関係 |
|---|---|---|
| 露出モードのM（マニュアル露出。`0x500E`の値1） | 画面の文字: `d810_settings.hpp` 251（設定の表の値「M」）、`d810_settings_write_screen.hpp`の6行（109・115・210・230・393・683。「CAM-A が M（マニュアル露出）ではありません」「本体を M に」など）、`wpd_settings_write_ui.hpp` 1729・1734（「本体を M に」「M（読むだけ）」）。試験: testsの5ファイル34行（`d810_settings_write_tests.cpp` 19行、`hardware_camera_agent_tests.cpp` 9行と`phase0_tests.cpp` 3行は撮影モードの値「M」、`d810_settings_choice_tests.cpp` 2行、`d810_settings_tests.cpp` 1行）。文書: ADR-0038・ADR-0041（「AがMモード」「AがM」「Mだけ書く」）、`WPD_DUAL_PREVIEW.md`、`CALIBRATION_CAPTURE_PROCEDURE.md`、`PILOT_CAPTURE_PROCEDURE.md`、`D810_PC_CONTROL_CAPABILITIES.md`、`CURRENT_STATUS.md`、引き継ぎの文書（`docs/HANDOFF_*.md`の1つ）、`AGENTS.md`（`exposure mode (M only)`）、`third_party/PTP_PROTOCOL_REFERENCES.md` | 一番近い。設定の書き込みの表（ADR-0038・0041）は開発用プレビューの同じ窓に出て、出ている間は合成の欄の場所を使う（`wpd_dual_preview_ui.hpp` 1777〜1782の`in_place_of_c`の注）。直した後は、同じ窓の同じ場所に、合成の「M」と露出モードの「M」が入れ替わりで出る |
| フォーカスモードとレンズの切り替えのM（マニュアルフォーカス） | `PILOT_CAPTURE_PROCEDURE.md` 79・83、`OPERATOR_GUIDE.md` 109・110（「M（マニュアル）にする」）、ADR-0044の本文（「セレクターをMにすれば撮れる」）、`WPD_PREVIEW_FOCUS.md` 31・64（「露出をMのまま」「本体がMの時」） | 手順書と文書の文。画面の文字には無い |
| 画像サイズのM（D810のL・M・S） | `tests/pc_direct_capture_tests.cpp` 388（「M(5520*3680)」を拒む試験）。製品はLだけ（`dual_hardware_camera_agent.cpp` 270） | 試験の値だけ |
| 計画の段とモジュールの名前（Mと数字） | 段M0〜M4（`docs/ROADMAP.md`）、M7（`CI_COST_PROFILE.md`）、ディレクトリ`src/m2`・`src/m3`・`tests/m3`、名前空間`a0::m2`、`A0CameraStitcher.M3.slnx`、文書`docs/M2_*.md`・`docs/M3_*.md`の35ファイル、設計の実測の項目M1（`design/fixed-composite-registration.md`の11.12節）。文書の中の「M」と数字の形は34ファイルで約155か所（M3 55、M2 52、M1 28、M4 13、M0 5、M7 2） | 数字が続くので、単独のMとは字面が違う。「M2」「M3」が合成の2つ目・3つ目に見えるおそれは残る（※推定） |
| 指摘と確認の番号 M-1〜M-8 | ADR-0038の設計レビューの指摘の番号（`DECISIONS.md` 20行）、コードの注（`d810_settings_write.hpp` 2行、`wpd_settings_write.hpp` 2行、`wpd_settings_write_ui.hpp` 1行）、`tests/m3`の3ファイル26行、`design/dual-preview-topology-diag.md`の手動確認M-1 | ハイフンと数字が付く。合成のMに番号を付ける時は、この形を使わない |
| 作業の大きさの目安 S・M・L | `M2_SEAM_QUALITY_GATE.md`（「G3 評価経路への接続（M）」など12か所）、`M2_AUTO_OVERLAP.md` 7か所、`design/fixed-composite-registration.md`と`design/fixed-composite-lens.md`の表の大きさの列 | 見出しの末尾の「（M）」が、合成の事に見えるおそれがある（※推定） |
| 文と式の変数のM | `WPD_DUAL_PREVIEW.md` 54（「最大 M px」）・169（「違う M」）、`DUAL_LIVE_WORKER_POC.md` 171〜196と`CURRENT_STATUS.md` 115（余裕の時間M。D = W + M）、`worker_preview_dispatcher_tests.cpp`の試験の文3行 | 文の中に単独のMとして出る |
| Mergeの語 | 画面の「両台撮影 → 仮Merge保存」（`preview_window_title.hpp` 81）と見出し「C 仮Merge（未校正）」、`PRODUCT_REQUIREMENTS.md` 49（「映像をMergeする（C）」）、文書の「未校正 Merge」「画像Merge」 | 取り違えではない。MがMergeのMである事と合う |

### 決定（案）

決定1の3台目・4台目の名前と合成の呼び名、決定6の直す時期（4台が確定してから）は、所有者が2026-11-18に決めた。決定6の書き方と読み方、ほかの決定は案。

1. **カメラの名前**: 名前はaliasで、付けた順の記号とする。位置（上下・左右）も役（基準・対象）も名前に入れない。位置はrig profileに、役は記録の欄に書く。今の2台は`CAM-A`・`CAM-B`のまま変えない。3台目・4台目は`CAM-C`・`CAM-D`とする（下の表の案2。所有者が2026-11-18に選んだ）。合成の呼び名は「C」から「M」（MergeのM）に改める。直す作業は4台が確定してから行い、それまでとその後の書き方・読み方は決定6による。文の中の3台目・4台目は、単独の「C」「D」で書かず、`CAM-C`・`CAM-D`か「カメラC」「カメラD」と書く。画面の「CAM-A（左）」の括弧の位置は名前の一部でなく、profileの位置から出す表示とする（今は`wpd_dual_preview_ui.hpp`の719・820行に直書き）。

| 案 | 3台目・4台目 | 合成の呼び名 | 良い所 | 困る所 |
|---|---|---|---|---|
| 1 | `CAM-D`・`CAM-E` | Cのまま | 今の画面・ログ・文書のCが全部そのまま正しい。過去の文書を読む時に、Cがカメラか合成かを迷わない（前の版の案は、この理由で案1を推していた） | Cが抜けて見えるので、理由をprofileの文書と画面の説明に1行書く。DがD810のDと並ぶ（`CAM-`を付けて書けば分かれる） |
| 2（所有者が選んだ案。2026-11-18） | `CAM-C`・`CAM-D` | 「M」（MergeのM）に改める。直すのは4台が確定してから | 記号が続いて自然。Mは画面の「仮Merge保存」の語と合う | 画面の題と見出し、その文字を見る試験、docsのCを直す（範囲は「待つ物」の表）。ADRの本文など直さない文書ではCが合成のままなので、決定6の読み方で読む。Mは露出モードのM（開発用プレビューの同じ窓の設定の表に出る）、作業の大きさの目安「（M）」、段とモジュールの名前（M2・M3）にも使っている（「「M」の今の使われ方」）。試験の11ファイル18行が`CAM-C`を「受け付けないalias」の例に使っているので、4台で`CAM-C`を受け付ける時に替える |
| 3 | `CAM-C`・`CAM-D` | Cのまま。カメラは必ず`CAM-C`と書く | 直す物が無い | 今の文は単独のA・Bでカメラを指すので、「Cの姿勢」「Cの画素」がどちらを指すかが書き手の注意に頼る。ログの行の頭の「C 」とカメラCの欄が並ぶ |
| 4 | 番号（`CAM-1`〜`CAM-4`） | Cのまま | 位置を番号で話す時に数が合う | 今のCAM-A・CAM-Bと同じ本体に2つの名前が付く。番号が位置の番号に見え、カメラを付け替えると番号と位置がずれる |

2. **位置の呼び方**: 位置はカメラの段（上段・下段）とカメラの列（左・右）の組で呼び、4つを「上段・左」「上段・右」「下段・左」「下段・右」とする（短くは左上・右上・左下・右下）。数で書く時は(カメラの段, カメラの列)の組で、上・左から1で数える（左上が(1, 1)）。番号1つで呼ぶ時は左上から1・2、左下から3・4とし、これは表示の呼び名でカメラの名前ではない。上下左右は文書の座標（x右・y下）で、取付の向きを当てた後、つまり合成の画面とJPEGで見た向きとする。部屋で見た向きと、カメラの画像のままの向きでは呼ばない。「段」「列」だけで書かず「カメラの段」「カメラの列」と書く（「段」はADR-0044の段P・段1や推定の段G1〜G4に、「列」は継ぎ目の列（出力の画素の列）に使っている）。
   - 名前と位置の対応はrig profileにだけ書く。照合のファイル（本体とalias）には位置を書かない。画面・ログ・報告の位置の表示はprofileから引く。
   - 記録に書く形（案。profile 3.0.0で決める）: `cameras.<alias>.position`に`{"cameraRow": 1, "cameraColumn": 2}`。`row`・`column`だけの名前は出力の画素の行と列（`seam.column`・`rowsPx`）に、`grid`は層2の格子に使っているので避ける。
   - 下段の取付の向きは未定とする。上段と下段で向きが違えば、リグ全体で1つの`mountOrientation`では書けず、カメラごとの向きが要る。置き場所（profileのカメラごとか、条件か）はprofile 3.0.0で決める。
   - 今の2台は、カメラの段が1つ・列が2つの場合として読む（決定5）。

3. **継ぎ目の呼び方**: 隣り合う2台の間の継ぎ目を、その2台の組で呼ぶ。縦の継ぎ目（上下に走る線で、左右の2台を分ける）は左のカメラ、右のカメラの順に、横の継ぎ目（左右に走る線で、上下の2台を分ける）は上のカメラ、下のカメラの順に書く。文では「縦の継ぎ目CAM-A–CAM-B」「横の継ぎ目CAM-A–CAM-C」（後者は例。どのカメラがどこに入るかは未定）。
   - 記録の欄（案）: `{"direction": "vertical", "cameras": ["CAM-A", "CAM-B"]}`。`direction`は`vertical`か`horizontal`、`cameras`は2つのaliasを上の順で。
   - 2×2では継ぎ目が4区間になる。上段の縦の継ぎ目（左上–右上）、下段の縦の継ぎ目（左下–右下）、左側の横の継ぎ目（左上–左下）、右側の横の継ぎ目（右上–右下）。
   - 4台が集まる所は「継ぎ目の交点」と呼ぶ。欄（案）は`{"cameras": [左上, 右上, 左下, 右下]}`（それぞれalias）。上段と下段の縦の継ぎ目が同じ出力の列に並ぶか（1本の直線か）はADR-0037を4台へ広げる時の決定で、ここでは決めない。並ばない時は交点が1点にならず、横の継ぎ目の上で3台が集まる点が2つできる。
   - 今の2台の`seam.column`は、縦の継ぎ目CAM-A–CAM-Bの列として読む。交点は無い。

4. **基準と対象のカメラの欄**: 2台の関係を表す値（姿勢、位置のずれ、色の倍率、継ぎ目のずれ）には、`referenceCamera`（基準のカメラ。その値を測る時に動かさない側）と`targetCamera`（対象のカメラ。その値で動かす・補正する側）を付け、値はaliasにする。
   - 星形（今）では、どの対も`referenceCamera`が`CAM-A`。輪で閉じる形では、継ぎ目ごとの測定はその継ぎ目の2台を基準と対象にし、輪を閉じた後の各カメラの姿勢は、全体で動かさない1台を`referenceCamera`、そのカメラを`targetCamera`にする。どちらの形でも欄の名前は変わらない。
   - 1台ごとの値を並べる時は、aliasを鍵にしたオブジェクトにする（profile 2.0.0の`calibration.inputSha256`と同じ形。例`{"CAM-A": "<sha256>", "CAM-B": "<sha256>"}`）。対象のカメラの値を1つだけ持つ欄は、名前を役で始める（例`targetResidualPx`）。欄の名前や符号に記号を埋める形（`cameraASha256`、`capture-original-a`）は新しく作らない。
   - ログ（`key=value`の行）は同じ語のsnake_case（`reference_camera=CAM-A target_camera=CAM-B`）、継ぎ目は`seam_direction=vertical seam_cameras=CAM-A,CAM-B`の形にする。
   - 値はaliasだけ。本体の識別子は書かない（今の決まりのまま）。

5. **今の2台の書式との対応**: 今ある書式は（#434の記録3種を除き）変えずに、次のとおり読む。基準・対象・継ぎ目・位置の欄が無い記録は、2台の既定（CAM-Aが基準、CAM-Bが対象、縦の継ぎ目CAM-A–CAM-Bが1本、カメラの段が1つで列が2つ）として読む。4台の記録は版の違う書式になるので、欄の無い記録を4台の記録として読む事は無い。

| 今の書式と欄 | この案での読み方 |
|---|---|
| profile 2.0.0 `cameraOrder`: `CAM-A-left-CAM-B-right` | カメラの段が1つ・列が2つ。CAM-Aが(1, 1)、CAM-Bが(1, 2) |
| profile 2.0.0 `cameraOrder`: `CAM-A-top-CAM-B-bottom` | カメラの段が2つ・列が1つ。CAM-Aが(1, 1)、CAM-Bが(2, 1)。継ぎ目は横の継ぎ目CAM-A–CAM-B |
| 条件v3 `basePose`、報告v8 `registration`・`geometryLevel.mapping.pose`、ライブのP（`LivePlacementFields`の`tx_mm=`ほか） | 基準CAM-A、対象CAM-B |
| 条件v3・報告v8 `seam.column` | 縦の継ぎ目CAM-A–CAM-Bの出力の列 |
| 条件v3 `mountOrientation` | 全カメラに同じ向き |
| 報告の`inputs[].alias`、`cameraAlias` | そのまま（値がalias） |
| #434の記録3種の`referenceCamera`・`targetCamera`、`originalSha256`・`edgeShare`（aliasが鍵）、`targetResidualPx`（2026-11-19に決定4の形に改めた） | 役の欄のとおり読む。改める前の形（`cameraASha256`・`cameraBSha256`・`cameraBResidualPx`など）の記録は無い前提で、読まない（所有者の決定3による） |
| 色の計画v1 `globalGainBgr`・`knots[].yMm`、`capture-original-a`・`capture-original-b` | 基準CAM-A、対象CAM-B、縦の継ぎ目CAM-A–CAM-Bに沿った位置 |

6. **合成の呼び名の移行（CからMへ）**: 直す作業（「待つ物」の「合成の呼び名をCからMに直す作業」）の前と後で、次のように書き、読む。直す時期は所有者の決定3（4台が確定してから）。
   - 前も後も: 単独の「C」はカメラを指さない。カメラは必ず`CAM-C`（文では「カメラC」）と書く。4台目も`CAM-D`か「カメラD」と書く。
   - 直す作業が済むまで: 単独の「C」は今までどおり合成を指す（画面の題「A/B Live + C（…）」、見出し「C 仮Merge（未校正）」、ログの行の頭の「C 」、文書の「ライブのC」）。新しく書く文書でも、合成を「C」と書いてよい。文では「合成」「ライブの合成」「保存の合成」と言葉で書く方を勧める（直す前にも後にも正しく読め、直す時の行が増えない）。「M」は、本ADRの決定の説明を除き、まだ合成の名前に使わない（同じ物に2つの名前を並べない）。
   - 直した後: 合成は「M」。単独の「C」を合成の意味で新しく書かない（ログの行の頭の印・試験の名前・コードの識別子は未決の6による）。Mにはほかの意味があるので（「「M」の今の使われ方」）、次の書き分けを案とする（※要確認、未決の5）。
     - 文では合成である事が分かる語を前に付ける（「合成M」「ライブのM」「保存のM」）。語の付かない単独の「M」で合成を指さない。
     - 露出モードは「Mモード」か「露出モードM」と書き、文の中の「AがM」の形を新しく書かない。フォーカスは「フォーカスのM」と書く。設定の表の値の「M」は、列の見出し（「露出モード」）が付くのでそのままでよい。
     - 画面の合成の欄の見出しは「M」1字で始めず、「合成M」のように語を付ける（設定の書き込みの表が同じ場所に出るため）。
     - 作業の大きさの目安は「規模M」と書くか、小・中・大で書く。段とモジュールの名前（M2・M3）と指摘の番号（M-1）は数字が付くのでそのまま。
   - 直す前に書かれた文書（過去のADR、日付つきの記録の文書、Issueとcommitの文、直す前に書かれたログ）: 単独の「C」は合成と読む。単独の「C」がカメラを指す事は前も後も無いので、文書がいつ書かれたかを確かめずに読める。直す前の文書の単独の「M」は合成を指さない（露出モード、大きさの目安など）。上の書き分けを採れば、語の付かない単独の「M」は、いつ書かれた文書でも合成を指さない。

### 理由

- 名前に位置を入れると、カメラを付け替えた時や取付の向きを変えた時に、照合のファイル（本体とalias）と過去の記録の名前が合わなくなる。位置をprofileに置けば、付け替えはprofileの改訂で済む。
- 欄の名前に記号を埋める形（`cameraBResidualPx`）は、輪で閉じる形で対象がBでない時に名前と中身が食い違い、4台では台数の分だけ欄が増える。役の欄（`referenceCamera`・`targetCamera`）とaliasを鍵にしたオブジェクトなら、台数と解き方が変わっても欄の名前は変わらない。
- 継ぎ目を2台の組で呼べば、継ぎ目の場所はprofileの位置から決まり、継ぎ目の番号と場所の対応表が要らない。
- 欄の無い記録を2台の既定として読む決まりを先に置けば、今の書式を版上げせずに読み続けられる。
- 呼び方だけなら、4台が来なくても失う物が無い。版上げと作り変えは4台が確定してからにする。
- 移行（決定6）は「単独の「C」はカメラに使わない」の1つを軸にした。こうすれば単独の「C」は、いつ書かれた文書でも合成と読める。Issueとcommitの文と直す前に書かれたログは直せず、過去のADRも書き換えない案（未決の7）なので、読み方を書かれた時期で変えない形にした。直す時期で読み分ける形（ある日より前の文書のCは合成、後はカメラ）は、文書ごとに日付を確かめる手間が残り、日付の無い文やログの断片では決められないので採らない。
- 合成の呼び名を直すのを4台の確定まで待つのは、所有者の決定3による。4台が来なければ`CAM-C`は使われず、合成のCとぶつからないので、待つ間に失う物は無い。

### 守る不変条件

1. 1つの本体には1つのalias。aliasは位置で変えない。
2. 位置の正本はrig profile。画面・ログ・報告は位置をprofileから引き、文字列に直書きしない。
3. 上下左右は文書の座標で、取付の向きを当てた後の向き。
4. 2台の関係を表す値には、基準と対象のカメラを欄で書く。欄の無い今の書式は決定5の既定で読む。
5. ライブのログとJPEGの報告は、同じカメラ・同じ継ぎ目に同じ語と同じaliasを使う。
6. 単独の「C」はカメラを指さない。3台目・4台目は`CAM-C`・`CAM-D`（文では「カメラC」「カメラD」）と書く。単独の「C」は、合成の呼び名を直す前は合成の呼び名で、直した後は新しく書かない（直す前の文書の単独の「C」は合成と読む。ログの行の頭の印・試験の名前・コードの識別子は未決の6による）。合成の呼び名「M」は、本ADRの決定の説明を除き、直す作業が済むまで、画面・ログ・新しい文書の合成の名前に使わない（決定6）。

### 退ける案

| 案 | 退ける理由 |
|---|---|
| 3台目・4台目を`CAM-D`・`CAM-E`にして、合成をCのまま残す（決定1の表の案1。前の版の案で推していた） | 所有者が案2を選んだ（2026-11-18）。推していた理由は、今の画面・ログ・文書のCを直さずに済み、過去の文書を読む時にCがカメラか合成かを迷わない事。案2では、後者を決定6の読み方で補う |
| 合成をCのまま残し、カメラは必ず`CAM-C`と書く（案3）、番号で呼ぶ（案4） | 所有者が案2を選んだ（2026-11-18）。案3は単独の「C」がどちらを指すかが書き手の注意に頼る。案4は同じ本体に名前が2つ付く |
| 移行で、単独の「C」の読み方を直した日の前と後で分ける（前は合成、後はカメラC） | 文書ごとに日付を確かめる手間が残り、日付の無い文やログの断片では決められない。決定6は、単独の「C」をカメラに使わない事で、読み方を1つにした |
| 名前に位置を入れる（`CAM-TL`など） | 付け替えや取付の向きの変更のたびに、照合と記録の名前が変わる |
| 今すぐN台の書式を作る（profile 3.0.0、条件v4、報告はv9の次の版。v9はADR-0046が2台のまま使った） | 4台が来なければ、版と読み手と一致の試験が増えるだけになる |
| 継ぎ目を番号で呼ぶ（継ぎ目1、継ぎ目2） | 番号と場所の対応表が別に要り、配置が変わると番号の意味が変わる |
| 基準と対象を記号の欄で書く（`cameraA*`・`cameraB*`） | 輪で閉じる形では、基準がCAM-Aとは限らない |
| 位置の欄を`row`・`column`にする | 出力の画素の行と列（`seam.column`など）と読み違える |
| 位置を0から数える | 文と画面の呼び名（1から）と記録の数が1つずれる。数え方の変換は書き手と読み手の1か所に置く |

### 2台を決め打ちにしている場所

ee77d69で確かめた（製品の層の`src/m3`の行は2026-11-19に同じ木で確かめた）。直すのは4台が確定した後（下の「待つ物」）。

| 場所 | 2台の決め打ち | 4台で要る物 |
|---|---|---|
| `src/phase0/include/a0/phase0/phase0.hpp` 418〜440、472〜477、502〜503 | `IdentityMap`がaliasごとの欄`cam_a_`・`cam_b_`を持ち、照合と取り込みの数もA・Bごとの欄 | aliasの表で持つ |
| `src/phase0/hardware_camera_agent.cpp` 394〜395、1381〜1399 | 受け付けるaliasは`CAM-A`か`CAM-B`だけ。照合のファイルの鍵はちょうどこの2つ（`RequireExactFields`） | aliasの表、照合のファイルの書式の版 |
| `src/phase0/experimental/wpd_dual_preview_ui.hpp` 2〜10、484、719、820 | カメラ・worker・ID・撮影ボタンが`std::array<…,2>`。aliasを`i==0?"CAM-A":"CAM-B"`で決め、表と窓の見出しに「CAM-A（左）」「CAM-B（右）」を直書き | 台数と位置をprofileから取る |
| `src/phase0/experimental/wpd_dual_preview_main.cpp` 72〜77、115〜132、713〜716 | 照合でCAM-AとCAM-Bの2つを探す。両方の点検の待ち合わせが`ready_count<2`。全カメラのWPDの呼び出しを1つのmutexで順に通す | 待ち合わせの数。4台を同じmutexで回すと1台あたりのフレームの間隔が延びる（※推定） |
| `src/phase0/include/a0/phase0/experimental/still_pair.hpp` 58〜66、82〜83、118、185〜196 | 撮影の1取引がAのシャッター、Bのシャッターの2台で、`shutter_gap_ms`は2台の差 | 4台の順と180秒の配分、シャッターの間隔の記録 |
| `src/phase0/include/a0/phase0/experimental/d810_settings_write.hpp` 38〜39、222 | `enum class D810Camera {A, B}`、Aの露出をBへ写す`copy_a_to_b` | 写す元と先の組、書く順 |
| `src/m2/include/a0/m2/render.hpp` 27〜30 | `StitchLayout`が「A左・B右」と「A上・B下」の2つ | カメラの段と列の並び |
| `src/m2/rig_profile_v2.cpp` 316〜335 | `cameraOrder`の2つの符号と、`cameras`の鍵がちょうど`CAM-A`・`CAM-B` | profile 3.0.0 |
| `docs/schemas/rig-profile.v2.schema.json` 64〜65、79〜86、108〜126 | 同上（schemaの`enum`と`required`）。`calibration.inputSha256`・`fitResidualPixels`の必須の鍵も`CAM-A`・`CAM-B` | profile 3.0.0 |
| `src/m2/include/a0/m2/experimental/seam_blend.hpp` 44〜56、215〜218 | 合成の入力が`camera_a`（左）と`camera_b`（右）。色はAを基準にBへ掛け、右の端へ向けて薄める | 横の継ぎ目と交点の混ぜ方、色の基準 |
| `src/m2/content_registration.cpp` 1783〜1796、2216〜2217 | 推定はA左・B右だけを受け付け、Bの姿勢だけを動かす（星形）。残差は縦の継ぎ目の列（`seam_column`）の近くで見る | 継ぎ目ごとの推定と、輪で閉じる解き方 |
| `src/m2/include/a0/m2/seam_gate.hpp` 72〜74 | ゲートは出力の行ごとに継ぎ目の列（`seam_x`）を測る。縦の継ぎ目だけ | 横の継ぎ目のゲート |
| `src/phase0/include/a0/phase0/experimental/live_placement.hpp` 18〜26、101〜114 | ライブのCの位置PはカメラB 1台の位置。ログの欄はどのカメラの値かを書かない | 対象のカメラごとのP、ログの基準と対象の欄 |
| `src/m2/fixed_composite_recalibration.cpp` 420〜432、507〜524 | 取り直すのはCAM-Bの写像だけ。記録の欄が`cameraASha256`・`cameraBSha256`・`cameraBResidualPx` | 決定4の形（記録の欄は2026-11-19に決定4の形に改めた） |
| `src/phase0/include/a0/phase0/experimental/recalibration_job.hpp` 389 | 校正の報告の`originals`が`cameraASha256`・`cameraBSha256` | 決定4の形（2026-11-19に改めた） |
| `src/m2/still_review_report_impl.hpp` 533〜535、680 | 報告の`inputs`がちょうど2つで、順にCAM-A・CAM-B | 報告のv9の次の版でN個（v9はADR-0046が2台のまま使った） |
| `python/a0seam/record.py` 35 | 色の計画v1の`bindings`の符号がA・Bの原画像（`capture-original-a`・`capture-original-b`）。結び目は縦の継ぎ目に沿った`yMm` | 継ぎ目と基準・対象の欄 |
| `src/m3/Foundation/DualCamera/DualCameraProductModel.cs` 92、98、132〜134 | 製品のrig profile（`DualCameraRigProfile`、`SchemaVersion` 1.1.0）の`CameraAliases`の既定が`CAM-A`・`CAM-B`で、`Validate`が数2とこの並びちょうどを求める。`Layout`は「A左・B右」と「A上・B下」の2つ、姿勢は`CameraBToCameraA`の1つ | aliasの表、カメラの段と列の並び、対ごとの姿勢 |
| `src/m3/Foundation/DualCamera/RigProfileV2.cs` 10〜15、150、152〜153、277、385〜387 | rig profile 2.0.0の.NETの読み手。`cameraOrder`の2つの符号と、`cameras`と対の欄（`Pair`）の鍵がちょうど`CAM-A`・`CAM-B`で、値は`RigPairV2<T>`の`CameraA`・`CameraB`に入る。指紋の正規形（`Canonical`）の鍵の順も`CAM-A`・`CAM-B` | profile 3.0.0。C++の読み手（`src/m2/rig_profile_v2.cpp`）と同じ版で直す |
| `src/m3/Foundation/Hardware/HardwareCameraAgentProtocol.cs` 1255、1640〜1642 | 撮影のprofileのaliasも、カメラのaliasも、受け付けるのは`CAM-A`か`CAM-B`だけ | aliasの表 |
| 製品のmode`DualCamera`（`src/m3/Foundation/Transactions/SimulatedTransactionModel.cs` 12〜16の`CameraOperatingMode`）と名前空間`A0CameraStitcher.M3.Foundation.DualCamera`（`src/m3`の9ファイルが宣言） | modeと名前空間の名前が2台を指す。mode`DualCamera`はCAM-A → CAM-Bの順に2台で撮って合成する（ADR-0023） | 4台をどのmodeで撮るか（ADR-0023の改訂）。名前を変えるかはその時に決める |
| `?"CAM-A…":"CAM-B…"`の形の直書き全体 | srcの16ファイル33行、testsの8ファイル19行（grepで数えた） | aliasの表を引く1か所 |
| `src/m3`の`"CAM-A"`・`"CAM-B"`の直書き全体 | 31ファイル117行（`git grep -l -E '"CAM-[AB]"' -- src/m3`と`git grep -c`で数えた。XAMLの画面の文字を含む） | aliasの表を引く1か所 |
| `AGENTS.md` 6・34・42行 | 範囲が「Nikon D810 x2」。登録したA/Bの照合。撮影の1取引がA、Bの2台で、WPDのsessionが2つ | 所有者の決定で範囲を改める |
| ADR-0023（製品のmode`DualCamera`はCAM-A → CAM-Bの順の2台）、ADR-0030（不変条件のCAM-A→CAM-B）、ADR-0031（左右二画面）、ADR-0037（縦一直線の継ぎ目）、ADR-0038（AからBへ写す）、ADR-0041（Bを先に、Aを最後に書く）、ADR-0043（Aを基準にBの色を合わせる）、ADR-0044 不変条件6（WPDのsessionは2つまで） | 2台を前提にした決定 | 4台が確定した時に改訂する |

### 今やる物と待つ物

今やる物（4台が来なくても損が出ない物。どれも本ADRが採用された後に、それぞれのIssueで行う。2だけは2026-11-19の所有者の決定により、採用の前に#434で先に行った）:

1. 新しいコードと書式に、`i==0?"CAM-A":"CAM-B"`の形、欄の名前や符号に記号を埋める形、名前に位置を添えた直書きの文字列を増やさない。今ある物は直さない。
2. #434の記録3種（推定の記録`a0.pair-recalibration-estimate.v1`、校正の報告`a0.pair-recalibration-report.v1`、確定の記録`a0.pair-recalibration-confirmation.v1`。2026-11-18の時点で公開のmainには無く、本ADRと同じpushで公開される）に`referenceCamera`・`targetCamera`を足す。`inputs`・`originals`を`originalSha256`（aliasを鍵にしたオブジェクト）に、`cameraBResidualPx`を`targetResidualPx`に、校正の報告の`edgeShareA`・`edgeShareB`を`edgeShare`（aliasを鍵にしたオブジェクト）に改める。2026-11-19の所有者の決定1〜3により、この3種の分だけ先に行い、版はv1のままにした（未決の1）。
3. #429の色の計画（`a0.seam-color-plan.v1`）に、継ぎ目（`direction`と2台のalias）と`referenceCamera`・`targetCamera`を入れる。v1は公開のmainにある（`python/a0seam`）。計画を読むC++の読み手と書式の文書はまだ無い（ADR-0043の未決）ので、読み手を作る前に足すのが一番安い。`bindings`の`capture-original-a`・`capture-original-b`は今の意味のまま残す。v1に足すかv2にするかは未決の2。
4. 新しく書く文書では、3台目・4台目のカメラを単独の記号で書かず、`CAM-C`・`CAM-D`か「カメラC」「カメラD」と書く。合成は、直す作業が済むまで今の「C」のままで、文では「合成」「ライブの合成」「保存の合成」と言葉で書く方を勧める（画面やログの文字を引く時は「C」）。「M」は、直す作業が済むまで合成の名前に使わない（決定6）。カメラの位置は決定2の向きで書く。

待つ物（4台が確定してから、別のIssueで）:

- rig profile 3.0.0（N台の`cameras`、`position`、カメラごとの取付の向き、`cameraOrder`の置き換え）。profileの読み手はC++（`src/m2/rig_profile_v2.cpp`）と.NET（`src/m3/Foundation/DualCamera/RigProfileV2.cs`）の2つあるので、3.0.0でも2つが食い違わない事を試験で固定する（2.0.0では両方の試験が`tests/fixtures/rig-profile-v2`の同じ正規形とSHA-256を読む）。
- 条件v4と、報告のv9の次の版（継ぎ目ごとの位置、基準と対象の欄、交点）。報告v9は、ADR-0046が2台の確定した位置の欄（`confirmedPosition`、基準と対象のカメラは`CAM-A`・`CAM-B`）のために先に使った。
- 推定器の輪で閉じる解き方。今はCAM-Aを中心にCAM-Bだけを動かす星形で、2×2では対角の2台が交点の近くでしか重ならない（※推定）ので、星形のままでは対角のカメラを測りにくい。
- 継ぎ目のゲートと合成の、横の継ぎ目と交点（ADR-0037の縦一直線を4台へ）。
- ライブのCの位置PのN台化と、色合わせを4台で閉じる形（ADR-0043の基準はA）。
- 撮影の順と180秒の配分、WPDのsessionの数（ADR-0044の不変条件6）、ADR-0030の不変条件のCAM-A→CAM-B。
- 照合のファイルの4台化。受け付けるaliasに`CAM-C`・`CAM-D`を足す時、試験の11ファイル18行が「受け付けないalias」の例に使っている`CAM-C`を、範囲の外の名前（例`CAM-Z`。※案）に替える。
- 画面の2×2化（ADR-0031の左右二画面）。
- 設定の書き込みの4台化（ADR-0038の写す元と先、ADR-0041の書く順）。
- ログに`reference_camera`・`target_camera`を足す事。足す時は、ライブとJPEGの両方の行を書く1つの関数（`LivePlacementFields`）に足す。
- 合成の呼び名をCからMに直す作業（所有者の決定3。4台が確定してから）。直す範囲は下の表。書き方と読み方は決定6。

#### 合成の呼び名をCからMに直す範囲

2026-11-18に2f93cccの木をgrepで数えた。コードは文字列の中の単独の「C」（`C:\`、`C++`、`C#`、`CAM-C`を除く）を、日本語を含む文字列、ログの行の頭の「C 」、そのほかに分け、別の意味の行（`content_registration_tests.cpp`の「C below the minimum」の変数C、`render_tests.cpp`の「C runtime」、`calibration_observation_session_tests.cpp`のチャートの点の名前など）を除いた。文書は`` ` ``の外の単独の「C」の行を数え、ファイルごとに中を見て合成の意味があるかを分けた。数は直す時に数え直す。

| 種類 | 場所と件数 | 決まり（版上げ）との関係 |
|---|---|---|
| 画面の文字 | srcの7ファイル49行。`live_registration_display.hpp` 19行（「Cは条件の位置で描いています」など）、`fixed_composite_preview.hpp` 16行（「C 固定の合成条件 v3（…）」など）、`wpd_dual_preview_ui.hpp` 8行（「ライブCに戻す」など。うち2行はsmokeの確認）、`wpd_recalibration_ui.hpp` 2行、`fixed_composite_tool_v3.hpp` 2行、`preview_merge.hpp` 1行（見出し「C 仮Merge（未校正）」）、`preview_window_title.hpp` 1行（題「A/B Live + C（…）」） | 所有者の決定3の範囲。書式ではない |
| その文字を見る試験 | testsの6ファイル27行（`live_registration_display_tests.cpp` 14、`live_registration_runtime_tests.cpp` 7、`fixed_composite_preview_tests.cpp` 2、`fixed_composite_tool_v3_tests.cpp` 2、`fixed_composite_preview_v2_tests.cpp` 1、`preview_merge_tests.cpp` 1）と、上のsmokeの2行。窓の題のsmoke（`wpd_dual_preview_ui.hpp` 2338・2356）は`PreviewTitleFrame`を通して比べるので、題の文字は1か所で変わる | 所有者の決定3の範囲 |
| docs | `docs/`の24ファイル: `DECISIONS.md`、`WPD_PREVIEW_MERGE.md`、`WPD_STILL_REVIEW.md`、`WPD_DUAL_PREVIEW.md`、`WPD_PREVIEW_FOCUS.md`、`WPD_LIVE_VIEW_CODEC.md`、`WPD_LIVE_VIEW_TRANSFER.md`、`M2_AUTO_OVERLAP.md`、`M2_SEAM_QUALITY_GATE.md`、`M2_EVALUATION_RENDER_CONDITIONS.md`、`M2_DRAFT_STITCH_EVAL.md`、`M2_REVIEW_EVAL_INTEGRATION.md`、`M2_SAVED_EVALUATION_CONDITIONS.md`、`PRODUCT_REQUIREMENTS.md`、`OPERATOR_UI_SPEC.md`、`CALIBRATION_CAPTURE_PROCEDURE.md`、`CURRENT_STATUS.md`、`DEVELOPMENT_STATUS_20261015.md`、`DUAL_LIVE_LICENSE_REVIEW_20261016.md`、`design/fixed-composite.md`、`design/fixed-composite-lens.md`、`design/fixed-composite-registration.md`、`design/fixed-composite-registration-formats.md`、`design/recalibration-from-pair.md`。ほかに`AGENTS.md`の6行（29・37・40・41・50・55。英文の「live C」「A/B/C」）。候補の行は24ファイルで約490行（本ADRの行を含む）で、段C・確認点C・案C・式の中のCなど別の意味の行を含むので、直す時に1行ずつ見る。合成でないCだけのファイル: `design/dual-preview-topology-diag.md`・`DUAL_LIVE_WORKER_POC.md`・`MACHINE_OPERATION.md`（回復コマンドC）、`M2_CALIBRATION_LENS.md`（費用C）、`D810_PC_CONTROL_CAPABILITIES.md`（段C）、`TEST_READINESS_2026-08-26.md`（区分C） | 所有者の決定3の範囲。ADRの本文と日付つきの記録の文書を書き換えるかは未決の7。`AGENTS.md`は、2台の範囲（6・34・42行）を改める時に合わせて直す |
| ログの行の頭の「C 」 | 書く所: srcの2ファイル22行（`wpd_dual_preview_ui.hpp` 17、`live_registration_runtime.hpp` 5。`C live_placement`、`C composite mode=`、`C auto_overlap_v2`など）。読む試験: testsの2ファイル11行（`live_registration_runtime_tests.cpp` 10、`live_registration_estimator_tests.cpp` 1）とsmokeの1行（`wpd_dual_preview_ui.hpp` 2686）。文書の引用: `WPD_PREVIEW_MERGE.md` 3行、`WPD_DUAL_PREVIEW.md` 2行。ログの鍵（`live_placement`など）にはCが無い | このログには版の決まりが無い（`docs/design/fixed-composite-registration-formats.md`の1節の決まり8が名指すのは条件v3と報告v8）。ライブとJPEGを突き合わせる欄（`LivePlacementFields`が書く部分）に印は入らず、JPEGの側の行`pair_review_placement`には印が無いので、印を変えても突き合わせる欄は変わらない（印を含めて比べる試験の行は直す）。変えるか据え置くかは未決の6 |
| 試験の名前、smokeの鍵、起動の引数 | ctestの名前`preview_fixed_c_ui_contracts`・`preview_live_c_ui_contracts`（`CMakeLists.txt` 1557〜1562の4行）、起動の引数`--fixed-c-ui-smoke`・`--live-c-ui-smoke`（`wpd_dual_preview_main.cpp` 1019・1020）、smokeの出力の鍵`fixed_c_ui_synthetic`・`fixed_c_ui_failed_check`・`fixed_c_ui_checks`と`live_c_ui_*`の同じ3つ（`wpd_dual_preview_ui.hpp`）。文書の引用は`WPD_DUAL_PREVIEW.md` 58・317、`WPD_PREVIEW_MERGE.md` 94 | 書式ではない。CIの`ctest -R`の絞り込み（`.github/workflows/software-ci.yml`）にこの名前は無い。変えるかは未決の6 |
| コードの識別子と注 | `PreviewCMode`（srcの3ファイル33行、testsの3ファイル27行）、`RunFixedCUiSmoke`・`RunLiveCUiSmoke`（`wpd_dual_preview_ui.hpp` 2143・2383）、`in_place_of_c`（`wpd_dual_preview_ui.hpp` 3行）、smokeの中の変数`in_c`・`c_has`。注（`//`の行）では、src・testsの37ファイル約300行に単独の「C」がある（別の意味を含む） | 書式ではない。変えるかは未決の6 |
| 書式（schema）と記録の欄 | `docs/schemas`の25ファイルと、記録を書くコードのJSONの欄の名前に、合成の意味のCは無い（0件）。書式の文書`docs/design/fixed-composite-registration-formats.md`の説明の語（「LC＝ライブの C の担当」の札3か所、`registration.live.limits`の単位「C の px」）だけがある。製品の画面（`src/m3`）とpythonの文字列にも合成のCは無い | 欄の名前を変えないので、直す作業で書式の版は上がらない。説明の語を直すのは文書の直し。欄の名前を変える事になった時は、条件v3・報告v8は同じ文書の1節の決まり8（mainに入った後の変更は新しい版）、rig profileは版（ADR-0034の2.0.0の次）、#434の記録3種は未決の1による |

### 製品の条件との関係

所有者の製品の条件は、ライブのCと書き出すJPEGのCの位置合わせが食い違わない事（映像の側でずらした量が、JPEGに同じだけ効く事）。今はライブの位置Pと撮影の記録の位置を同じ関数（`live_placement.hpp`の`LivePlacementFields`、101〜114行）が書き、2つのログの行を文字列で比べられる。4台でもこれを保つため、呼び方と欄の名前はライブのログとJPEGの報告で同じ物を使う。カメラはalias、役は`referenceCamera`・`targetCamera`（ログでは`reference_camera`・`target_camera`）、継ぎ目は向きと2台の組。片方だけが別の名前を使うと、同じカメラ・同じ継ぎ目の値をライブとJPEGで突き合わせられなくなる。本ADRは文書だけで、ライブのCにもJPEGのCにも、位置合わせの結果にも触れない。

### 所有者に確かめる事

| 確かめる事 | 答えで変わる所 |
|---|---|
| 3台目・4台目の名前（決定1の案1〜4） | 決定済み（2026-11-18、案2）。`CAM-C`・`CAM-D`、合成の呼び名はM、直すのは4台が確定してから。決定1・決定6、不変条件6、今やる物の4、待つ物に入れた |
| 合成のMと露出モードのMなどの書き分け（決定6の案。文の「合成M」「Mモード」、画面の見出しの「合成M」） | 直す作業の画面の見出しと、文書の書き方（未決の5） |
| #434の記録に基準・対象のカメラの欄を足すのを、手元のmainを公開する前にするか | 決定済み（2026-11-19）。公開する前に、3種の分だけ先に決めて、欄を足し、名前をaliasで引く形に揃える。版はv1のままで、公開した後に形を変える時はv2。実機に古い形の記録は無い前提。今やる物の2・未決の1に入れた |
| 位置を番号1つで呼ぶ時の並び（左上から1・2、左下から3・4でよいか）と、それが合成の画面で見た並びか、部屋で見た並びか | 決定2の番号の対応 |
| 今の2台（CAM-A・CAM-B）が上段・下段のどちらに入るか | profileでの2台の位置（名前は変わらない） |
| 下段の取付の向き（上段と同じく吊り下げで180°回すか、別の向きか） | カメラごとの向きの欄が要るか（profile 3.0.0） |
| 紙の大きさと向き（A0横のままか、より大きい紙か） | profileの`documentPlane.paper`（今は`ISO-216-A0`だけ）、出力のラスタ |
| 要る細かさ（DPI。今の候補は180 DPI） | 1台が写す範囲と重なりの幅。4台にする目的が広さか細かさか |
| 撮影にかけられる時間（今は2台で1つの180秒） | 撮影の順と時間の配分 |
| 4台の重なりの幅（縦の継ぎ目と横の継ぎ目のそれぞれ） | 推定器の帯の幅、交点の扱い、作る時の検査 |
| ライブのCも4台で見るか、撮影の合成だけを4台にするか | 待つ物の順番（ライブのPのN台化を先にするか） |

### 影響と改定の対象

| 対象 | 扱い |
|---|---|
| `docs/DECISIONS.md` | 本ADRを足す（Proposed）。2026-11-18の所有者の決定と、2026-11-19の所有者の決定（とそれに合わせて決めた事）を入れた。ほかのADRの本文は変えない |
| コード・試験・書式・`AGENTS.md` | 変えない（#434の記録3種を書くコードとその試験、記録を説明する文書を除く。次の行） |
| #434の記録3種 | 2026-11-19の所有者の決定により、本ADRの採用を待たずに、今やる物の2として#434のcommitで変えた。版はv1のまま |
| 色の計画 | 採用された後に、今やる物の3として別のcommitで変える |
| ADR-0023・0030・0031・0037・0038・0041・0043・0044、`AGENTS.md`の6・34・43行（上の「2台を決め打ちにしている場所」の表はee77d69の行番号で、同じ項を42行と書く） | 4台が確定した時に改訂の対象になる。今は変えない |
| 合成のC（画面の文字、その文字を見る試験、docs、ログの行の頭の「C 」、試験の名前、コードの識別子） | 4台が確定した後、待つ物の「合成の呼び名をCからMに直す作業」で変える。今は変えない。範囲は待つ物の表 |
| ADR-0036の題（表示専用の合成C）など、Cを合成の意味で使うADRの本文 | 変えず、決定6の読み方で読む（※要確認。未決の7） |
| 試験の`CAM-C`（受け付けないaliasの例。11ファイル18行） | 4台で`CAM-C`を受け付ける時に、範囲の外の名前に替える |

### 未決（※要確認）

1. **書式の決まりの「main」**: `docs/design/fixed-composite-registration-formats.md`の1節の決まり8は「main に入った後の変更は新しい版（v4・v9）にし、古い読み手を残す」、同じ文書の状態の節は「v3 と v8 は main に入るまで凍結しない」と書く。
   - 読める事: 決まりが名指すのは条件v3と報告v8。#434の記録3種はこの文書の表に無く、記録を説明する`docs/design/recalibration-from-pair.md`の6節と`docs/WPD_STILL_REVIEW.md`にも版の決まりは無い。条件v3と報告v8は公開のmainにもあるので、mainをどちらに読んでも凍結済み。3種のうち推定の記録と校正の報告には製品の読み手が無く（試験だけが読む）、確定の記録は`wpd_recalibration_ui.hpp`の`RecalibrationRecordedAsUnchecked`が`schema`・`revisionSha256`・`unverified`の3つだけを読むので、欄を足しても今の読み手は読める。推定の記録のSHA-256は条件v3の`reportSha256`とprofileの`provenanceId`に入るので、欄を足すと、これから作る記録のSHA-256の値が変わる（既にある記録は変わらない）。
   - 読めない事: 決まり8の「main」は、手元のmainに統合した時点を指すか、公開した時点を指すか。決まり8が条件v3・報告v8以外の記録にも及ぶか。pushの後は、記録3種がどちらの読み方でもmainに入っているので、残る問いは「決まり8が条件v3・報告v8以外の記録に及ぶか」の1つになる。
   - 所有者の決定（2026-11-19）: 版はv1のまま。公開した後に形を変える時はv2にする。
   - これに合わせて決めた事（設計とレビュー）: 公開のmainに入った後に、欄を足す・除く、欄の名前・型・意味を変える時は新しい版（v2）にする。この決まりは`docs/design/recalibration-from-pair.md`の6節に書いた。決まり8は条件v3・報告v8の決まりのままで、記録3種には及ぼさない。校正をONにしたビルドを実機で動かした事は無い（所有者の決定3）ので、実機に改める前の形の記録は無い前提とし、改める前の形を読む仕組みは作らない。
2. 色の計画v1に欄を足す時の扱い。v1は公開のmainにあり、書式の文書と読み手がまだ無い。v1のまま足すかv2にするかは、計画の書式の文書を書く時に決める。
3. 2×2の対角の2台がどれだけ重なるか（交点の近くだけか）と、星形のまま測れる範囲（※推定。配置と重なりの幅が決まってから確かめる）。
4. 位置の欄の名前（`cameraRow`・`cameraColumn`）と、カメラごとの取付の向きの置き場所は、profile 3.0.0で決める。
5. **「M」の書き分け**: 決定6の案（文では「合成M」「ライブのM」「保存のM」、露出モードは「Mモード」「露出モードM」、画面の合成の欄の見出しは「合成M」、作業の大きさは「規模M」）でよいか。合成をMと呼ぶ所有者の決定は変えず、書き方だけを決める。
   - 読める事: 露出モードのMは、開発用プレビューの同じ窓の設定の表に出て、その表は合成の欄の場所を使う（「「M」の今の使われ方」）。直した後に見出しを「M」1字にすると、同じ場所に意味の違う「M」が入れ替わりで出る。
   - 答えで変わる所: 直す作業の画面の見出しと文書の書き方、決定6の書き分け。
6. **ログの行の頭の「C 」、試験の名前、コードの識別子**: 直す作業で「M」に変えるか、据え置くか。
   - 読める事: 開発用プレビューのログには版の決まりが無く、書式（schema）と記録の欄の名前には合成のCが無い（待つ物の表）。ライブとJPEGを突き合わせる欄には印が入らない。
   - 据え置く時は、ログの「C 」を合成の印として決定6の読み方で読み、画面と文書だけがMになる。変える時は、ログを読む試験と、ログの行を引く文書（`WPD_PREVIEW_MERGE.md`・`WPD_DUAL_PREVIEW.md`）を同じcommitで直し、古いログの「C 」は合成と読む。
   - 答えで変わる所: 直す作業の範囲（待つ物の表の「ログの行の頭」「試験の名前」「コードの識別子」の行）。
7. **直さない文書の範囲**: ADRの本文（ADR-0036の題を含む）と、ファイル名に日付がある記録の文書（`DEVELOPMENT_STATUS_20261015.md`、`DUAL_LIVE_LICENSE_REVIEW_20261016.md`など）を、直す作業で書き換えずに残すか。案は、残して決定6の読み方で読む事（ADRとその時点の記録は、書いた時の言葉のまま残す方が、後から経緯を追える）。
   - 答えで変わる所: 待つ物の表の「docs」の行の範囲と件数。

## ADR-0046: 開発用プレビューのライブの合成とJPEGの合成が、1つの確定した位置を共有する（#437 段2）

- 状態: Accepted（2026-11-19、#437 段2）。所有者が本ADRの決まりの変更と、下の「AGENTS.mdに入れた文」を承認し、ボタンの名前、推定の途中でライブを止めた時の扱い、手の刻みを決めた（下の「ADR-0046の所有者の要件と決定」の5）。段1の前に所有者が決めた3点（Issueのコメント「設計の要点と、所有者の決定」）と、2026-11-19の所有者の指示も、同じ節に書いた。本ADRの変更は文書だけで、コード・試験・書式は変えない。`AGENTS.md`には、承認された文を別のcommitで入れた。段2の実装は、「段の切り分け」の順で行う。同じ日に、承認された文と本ADRの決定が1つの答えになるように、技術の側で決めた事を足し（「技術の側で決めた事」）、決定3〜5・7を直して決定9・10を足し、試験の計画と不変条件を直した。所有者の決定は変えていない。所有者に聞いていない事と、残った技術の側の問いは、※案・※要確認のまま残した（「所有者に確かめる事」）。同じ日の再レビューの後に、所有者が3つを決めた（「ADR-0046の所有者の要件と決定」の6）。これに合わせて決定2〜7・9・10を直して決定11を足し、技術の側で決めた事・保証・試験の計画・不変条件を直し、ADR-0035・0036・0037に改定の追記を置いた（「影響と改定の対象」）。
- 決定日: 2026-11-19（所有者の承認。段1の前の3点は、Issueのそのコメントの日）
- 関連: #437、#417（撮影ごとの位置合わせ、報告v8）、#418（撮影の後の確認画面、手直し）、#434（撮影の1組からの校正の取り直し）、#438（ADR-0045）、#396、#403、#309（ライブの枠と撮影画像の対応S）、#304（後回しにした指摘）、ADR-0021、ADR-0035（不変条件6）、ADR-0036（決定1の5・10、確認点C1、不変条件4）、ADR-0037（決定1・2、不変条件1〜4・7）、ADR-0043、ADR-0045、`docs/design/fixed-composite-registration.md`（11.9・11.19・11.21・11.28）、`docs/design/fixed-composite-registration-formats.md`、`docs/WPD_DUAL_PREVIEW.md`（#437の節）
- 背景: 条件v3（`registration.live`がある時）の開発用プレビューでは、段1（手元のmain）で、ライブの合成がCAM-Bの位置を1つだけ持ち、推定は窓で最初にライブを始めた時と「自動位置合わせ」ボタンを押した時だけ走るようになった。撮影の後は、ライブの合成をJPEGの合成が当てた写像で描く。一方でJPEGの合成は今も、撮影ごとに条件の基準の姿勢p_bから原寸で推定し、ライブの位置を受け取らない（設計11.21の決定「Cの推定はJPEGに渡さない」）。原寸の推定が決まらない撮影（繰り返し模様、模様が足りない）では、JPEGは粗の段の目安（G3）か条件の位置（G4）で作られ、ライブで確定していた位置とは別になる。この画面から手で位置を直す道も無い（条件v3の間、重なり・上下のボタンは`ui_position_request_ignored`で断る）。所有者は、手で直した分もJPEGに同じだけ効く事を求めた。ライブの映像から求めた位置や手で決めた位置をJPEGの位置に使うのは、今の決まり（下の「変わる決まり」）の変更になる。

### ADR-0046の所有者の要件と決定（#437）

1. 要件（Issueの本文）: 「自動マージについては頭の１回＋手動の”自動調整ボタンを押下”した時のみにしたいな。（シャッター後は自動調整が１回入る）」「勿論その映像は出力されたjpgと同じでないといけない（手動調整した分はjpg側でも同じ表示されていないといけない）」
2. 受入の条件（Issueのコメント、所有者の指示）: ライブの合成と書き出すJPEGの合成が食い違わない事（継ぎ目の場所、手順、位置、色の補正。手で直した分はJPEGにも同じく反映する）を、設計と試験で保証する。解像度の違いで保証できない物は、大きさを数字で示す。位置や補正の値とその出どころは、ライブのログとJPEGの報告に同じ書き方で残す。一致の試験が無い変更はmainに入れない（レビューでHIGHとして扱う）。
3. 段1の前の決定（Issueのコメント「設計の要点と、所有者の決定」）:
   1. 手で直した後に撮った時、シャッターの後の自動は手の位置を変えない。手の位置でJPEGを作り、自動で測った位置との差を画面に出す。
   2. 撮影の後の自動で位置が決まらない時は、ライブで確定していた位置をJPEGに使う。今の決まりの変更なので、ADRとAGENTS.mdの文の承認を実装の前に別に取る（本ADR）。
   3. 手で直した位置は、アプリを閉じるまで覚える。次回も使う時は、保存の操作で残す（保存は段3）。
4. 2026-11-19の指示: 手で直した位置がJPEGに同じく反映されなければ調整する意味が無いので、段2を実装する。
5. 2026-11-19の決定（本ADRへの回答）:
   1. 決まりの変更（本ADR）を承認する。`AGENTS.md`に足す文（新しい項1つと、2つの行の末尾への追記。下の「AGENTS.mdに入れた文」）も承認する。
   2. ボタンの名前: 固定の合成条件v3がある時は「自動調整」にする。条件が無い時の未校正の合成のボタンは「自動位置合わせ」のまま（「所有者に確かめる事」の1の(b)）。
   3. 推定の途中でライブを止めた時: 頼みを残し、再開の時にその1回だけ続きを行う（同2の(a)。段1の今の作りのまま）。
   4. 手の刻み: 左右・上下は12 pxと4 px、回りは0.2°と0.05°（同5の(a)）。1 pxは、原寸の継ぎ目の確認画面（#418）ができてから足す。
6. 2026-11-19の決定（本ADRの再レビューの後の問いへの回答）:
   1. 手で位置を直した後に合成の条件が替わった時は、手で直した位置を捨てる（新しい条件の位置に戻し、自動を1回走らせ、画面でそう言う）。「アプリを閉じるまで覚える」の例外。
   2. 手で動かして入れた量の合計がちょうど0に戻った時は、「手で直していない」に戻す（手の印を消し、手で直す前の状態に戻す。出どころも手で直す前に戻す。撮影の後は今までどおり自動が位置を決める。画面でそう言う）。手で直す前の出どころによらず同じ扱いに揃える。
   3. ライブの画面が確定した位置でまだ描けていない間（描き直しの途中、描き直しに失敗した時）は、両台撮影を受け付けない。描き直しが失敗したままの時は、「自動調整」を押すかライブを止めて再開するよう画面で案内する。
7. 段2の実装の途中の決定（実装の途中の問いへの回答）:
   1. 手で動かして入れた量の合計がちょうど0に戻った時は、手で直す前の位置が範囲の外の自動の位置でも、止めずに「手で直していない」に戻す（問い「合計 0 に戻ったら」への選択「『手で直していない』に戻す」と、その後の「範囲の外の自動の位置にも戻す」の了承。6の2と同じ扱い）。
   2. 撮影した原画像から測ったCAM-Bの位置が範囲の外なのに、ライブで決めていた位置でJPEGを作った撮影は、判定の見出しを「基準内」ではなく「確かめが要る」（判定しない）にする。JPEGは今までどおりライブで決めていた位置で作る（実装のレビューの後の問いへの選択「はい、『確かめが要る』にする」）。

6の1は決定10、6の2は決定3、6の3は決定11に入れた。6の1と6の2は、それまで技術の側で決めていた事（前の「技術の側で決めた事」の7と、5の一部）を所有者が決めた形で、6の2で、手で直す前が条件の位置でない時の扱いが変わった（前は手の位置のまま）。6の3は、前の「技術の側で決める問い」の「ライブの表がまだPで焼かれていない間の撮影」への答え。

7の1は決定3と技術の側で決めた事の16に、7の2は技術の側で決めた事の13と「報告v9の差分」に入れた。7の1は、それまで技術の側で決めた事の16に「所有者が了承した」と書いていた部分（範囲の外の自動の位置にも戻す）を、所有者の決定の節に移した物。7の2で、前は`notice`で判定が「基準内」になりえた撮影が、「確かめが要る」（`not-judged`）になった。

所有者に聞いていない事: 候補として挙がった「次の撮影の前（AFの後）の自動の1回」（「所有者に確かめる事」の3）。承認された文は自動が走る時を3つに限るので、足さずに候補として残す。所有者が足すと決めた時は本ADRを改める。範囲の端で断る事（同4）は個別には聞いていないが、承認された決定4と`AGENTS.md`の文（`a step past the envelope is refused, never clamped into success`）に含まれる。技術の側で決めた事（所有者の決定の趣旨に合わせた物）は、所有者の決定と分けて「技術の側で決めた事」に書いた。残りの問いは「所有者に確かめる事」の後の「技術の側で決める問い」で、未決のまま。

### 今の作り（コードで確かめた事）

2026-11-19に、手元のmain（b081023）の木をgrepとファイルの読み取りで確かめた。行番号はこのcommitの物。本ADRの枝のcommitは文書だけなので、コードの行番号は枝の上でも同じ。ビルドと試験は走らせていない（※未検証）。その後に#434の記録の欄の変更が手元のmainに入った。その変更が触ったファイルのうち、本ADRがコードの行を引くのは`recalibration_job.hpp`だけで、その所は関数の名前で指した（「段の切り分け」の後の「ほかの作業との重なり」）。ほかの引いた行は、その変更で変わらない。

- 確定した位置: `src/phase0/include/a0/phase0/experimental/live_placement.hpp`の`LivePlacementMachine`。値はCAM-Bの姿勢6数と紙面3数（`RegisteredMapping`、層2なし）。ほかに出どころ（`fixed-condition`・`auto-start`・`auto-button`・`after-capture`、32〜41行）、通し番号`serial`、条件のSHA-256を持つ。手の出どころは無い。
- 描く写像: 位置が変わる時は必ずCAM-Bの表を焼き直してから切り替える（`docs/WPD_DUAL_PREVIEW.md` 48行）。ライブの写像`LiveViewRegisteredOutputToCamera`とJPEGの写像`RegisteredDocumentOutputToCamera`の一致は、試験`SameConventionAsTheJpeg`（`tests/fixed_composite_preview_v3_tests.cpp` 252〜279行）が、紙面のある写像も含めて1e-6ライブ画素以内に縛っている。
- 撮影へ渡る物: `StillReviewRequest`（`src/m2/include/a0/m2/experimental/still_review.hpp` 328〜378行）には、#334の未校正の合成の幾何の写し`geometry`（331行の`PreviewReviewGeometry`。ライブの枠の大きさ・重なり・上下のずれ）があるが、条件v3の部分`FixedCompositeRegistration`（370〜376行）には位置の欄が無い。撮影の推定の始め・事前分布・範囲の基準・G4の写像は、どれも条件の`basePose`（`src/m2/capture_registration.cpp` 1311行・1392行）。段を選ぶ`SelectGeometryLevel`（同1030〜1071行）の候補はG1〜G4だけ。
- 段の候補の作り方（同1036〜1054行）: G1・G2の候補は、推定器が位置を出して状態が`verified`か`out_of_envelope`で（`Trusted()`、266〜268行）、さらに推定の姿勢と紙面が範囲の内か、条件の`applyBeyondEnvelope`がtrueの時だけできる。G3の候補は`Trusted()`でない時だけできる。したがって、状態が`out_of_envelope`で`applyBeyondEnvelope`がfalseの撮影は、G3も作られずG4になる。候補は順に安い検査に通し（1056〜1065行）、G1・G2が検査に落ちた時もG3は作られずG4になる。
- 安い検査は写像と条件だけで決まる: 撮影の時の`RegisteredMappingCoverOnThreads`（`src/m2/uncalibrated_review_impl.hpp` 40〜67行）は、`CheckRegisteredMappingCover`（`src/m2/include/a0/m2/experimental/fixed_composite_creation_v3.hpp` 68行）と同じ答えを返す。答えはprofileの値・写像・継ぎ目の列・混ぜ幅（条件の値ではなく設計の定数`kSeamBlendDesign`、`src/m2/include/a0/m2/experimental/seam_blend.hpp` 38行）で決まり、画素を読まない。撮影の時の関数は、原画像が原寸（`kD810SensorWidthPixels`×`kD810SensorHeightPixels`）の時だけ自分の数え方を使い、ほかの大きさでは`CheckRegisteredMappingCover`そのものを呼ぶ。2つが同じ答えを返す事を縛る試験は、今は無い。条件v3はprofileをファイルのSHA-256と指紋で名指す（`fixed_composite_condition_v3.hpp` 108〜110行）。
- 機械の値（`live_placement.hpp`）: `Placement()`（275行）と`Serial()`（277行）が描いているPで、`Wanted()`（222行）は焼き直しを待っている位置があればそれを返す。待っている位置は、焼き直しが通った後の`Committed()`（227〜237行）でPになる。`ApplyCapture`（251〜267行）が照合するのは条件のSHA-256だけ。同じ条件で始めたセッションは、走っていた推定を頼みに戻す（`BeginSession`、163〜166行。所有者の決定5の3の「頼みを残す」）。
- 撮影の要求と推定・焼き直し: 推定と焼き直しの働き手はライブが動いている間だけ走り、`stopping`で止まる（`src/phase0/experimental/wpd_dual_preview_ui.hpp` 385〜389行）。`RequestCapture`（889〜907行）が`stopping`を立てる。ただし、働き手は焼く前には止まりの印を見るが、焼き終えた後は見ず、基準の表と`WantedSerial()`だけを比べてPを置き換える（`live_registration_runtime.hpp`の`Rebake`、577行と584〜590行）。撮影を頼んだ時に焼き直しの途中だと、頼んだ後にPが変わりうる。撮影の結果は、窓が`ApplyCaptureToLivePlacement`を呼んでから次のライブを始める（`wpd_dual_preview_ui.hpp` 1096〜1097行）。
- Pで描けていない間: セッションの始めに基準の表が置かれると、焼いた表を捨てる（`live_registration_runtime.hpp`の`PublishBase`、441行）。Pが条件の位置でなければ、Pで焼き終えるまでは基準の表（条件の位置）で描く。撮影の結果を当てた後もPが先に変わり、表は次のライブで焼き直す。1回の働きは、頼みがあれば推定を先に走らせ、その後に焼き直す（`RunPass`、462行と518行）。焼き直しが落ちた時、待っている位置があれば捨て（`DrawFailed`）、Pそのものの焼き直しなら同じ写像を焼き直さず、待つ写像が変わるか基準の表が置き直されるまで待つ（`Rebake`、567行と597〜598行）。この間、ライブは前の表か基準の表で描き、描いている物とPが違う。
- 撮影の後: 窓が`ApplyCaptureToLivePlacement`（`src/phase0/experimental/wpd_dual_preview_ui.hpp` 189〜198行）で、記録の段の写像をライブの位置にする。ログの2行（ライブの`C live_placement …`と撮影の側の`pair_review_placement …`）は同じ関数`LivePlacementFields`（`live_placement.hpp` 110〜116行）が書く。ただし撮影の側の行は窓が記録から写した値で、通し番号を持たない。
- 数の書き方: ログは`LivePlacementNumber`（同104〜109行）、報告は`Num`（`capture_registration.cpp` 28〜31行）。どちらも有効数字17桁・classicのlocale・-0を0と書くが、関数が2つあり、有限でない数の扱いが違う（ログは`nan`と書き、報告は例外を投げる）。
- 手直しの式: 条件の改訂`manual-adjustment`の`SolveManualChange`（`src/phase0/include/a0/phase0/experimental/fixed_composite_tool_v3.hpp` 127〜147行）。紙面を持たない姿勢だけの式で、継ぎ目の列の中央の行で、入れたdx・dy（出力px）になるtx・tyを解く。範囲の成分は`RegistrationComponentsFromBase`（`capture_registration.cpp` 766〜777行）。位置は|(tx, ty)|、回りは|rz|で、どちらもp_bからの差。線を超えた時だけ外。
- 偽の本体の差し込み口（`wpd_dual_preview_ui.hpp`）: `pair_capture_backend`・`pair_settings_folder_override`・`pair_review_root_override`（910〜912行）、`live_estimator_override`・`live_design_override`（110〜111行）。これらを使う窓の試験は`preview_live_c_ui_contracts`（`--live-c-ui-smoke`）と`preview_pair_v3_ui_contracts`（`--pair-v3-ui-smoke`）で、`CMakeLists.txt` 1561行・1568行にある。手のボタンはまだ無いので、その押下の差し込みは段2で作る（既存のsmokeと同じく、試験の中だけで窓の自分のボタンへ押下を送る）。
- 報告v8は公開のmainにある。書式の文書の1節の決まり8（`docs/design/fixed-composite-registration-formats.md` 16行）により、欄を足すならv9にして、v8の読み手を残す。

### 決定

1. **確定した位置**: 窓は確定した位置Pを1つ持つ。Pは、CAM-A（基準のカメラ）に対するCAM-B（対象のカメラ）の姿勢6数（rx・ry・rz度、tx・ty mm、tz %）と紙面3数（h0 mm、slope_x、slope_y）の9数で、層2を含まない。ほかに出どころ、通し番号、条件のSHA-256を持つ。出どころが手の時は、手で直す前の位置の出どころと、入れた量の合計（継ぎ目でのdx・dyを出力px、回りを度）も持つ。出どころの符号に`manual`を足し、ほかの符号は段1のまま。ライブの合成はPで描き、Pを置き換えるのはCAM-Bの表の焼き直しが通った後。
2. **自動が走るのは3つの時だけ**: 窓で最初にライブを始めた時（条件のファイルのSHA-256が変わった時も1回）、ボタンを押した時、撮影の後に原画像2枚で1回。裏の追従、変化の検出、時間で条件の位置へ戻る事、再試行はしない（段1のまま）。推定が決まらなければPはそのままで、理由を画面に出す。ライブの推定の結果は、描く前（待つ位置にする前）に、手の押下と同じ安い検査（決定4）に当て、落ちれば決まらなかった扱いにする（技術の側で決めた事の8）。推定の途中でライブを止めた時は、頼みを残し、再開の時にその1回だけ続きを行う（段1のまま。所有者の決定）。ライブを始めた時（再開を含む）は、残した頼みの推定より先にPで表を焼く。焼き直しが落ちても、残した頼みの推定は走らせる（技術の側で決めた事の11）。ボタンの名前は、条件v3の時は「自動調整」、条件が無い時の未校正の合成では「自動位置合わせ」のまま（所有者の決定）。
3. **手で動かす**: 動かせるのは面内の3つ（継ぎ目に沿う左右＝基線の向き、上下、光軸まわりの回り）。傾き・高さ・紙面は動かさない（設計11.19.3の`manual-adjustment`と同じ）。量は撮影の出力の画素と度で表す（`manual-adjustment`の`entered`と同じ単位。窓の中での持ち方は下の1つ目）。刻みは、左右・上下が12 px（ライブの合成の約1画素）と4 px、回りが0.2°と0.05°（所有者の決定）。1 pxの刻みは、原寸の継ぎ目の確認画面（#418）ができてから足す。
   - 手の位置は、手で直す前の位置に、入れた量の合計を1回で解いて足した物とする。押すたびに前の値へ積み上げない。式が線形でないので、積み上げると押した順で値が変わりうり、段3で保存した改訂の値ともずれる。入れた量は、最小の刻み（左右・上下は4 px、回りは0.05°）の整数の数で持ち、合計と「ちょうど0」の判定はその数で行う。式と記録に渡す量は、その数から1回で作る（技術の側で決めた事の15）。入れた量の合計が3つとも0の時は、解かずに手で直す前の位置そのものを返す（技術の側で決めた事の5）。
   - 手の押下を描く前に次の押下が来た時は受け付け、合計を足し直して手で直す前の位置から解き直し、待っている位置を置き換える。待っている位置が捨てられた時（撮影の要求、焼き直しの失敗）は、入れた量の合計もPの物に戻す（技術の側で決めた事の9）。
   - 式は`SolveManualChange`を紙面のある写像へ広げた1つの関数にする。紙面が0の時は今の式と同じ値を返す。
   - 推定の途中で手で動かすと、その推定の結果は捨てる。手の押下は、残していた自動の頼み（頼んだだけでまだ走っていない物、ライブを止めた時に残した物を含む）も取り消す。取り消した頼みは、ライブを再開した後も走らず、手の位置を上書きしない。ボタンを押せば、また1回走る（技術の側で決めた事の3）。押下を確かめている間に推定の結果が届き、描かれるのを待っていても、押下が勝ち、その結果は捨てる。確かめの間にPか、描かれるのを待つ手の位置が変わった時だけ、押下は古い扱いで断る（窓は量を足し直して確かめ直す）。
   - 入れた量の合計が3つともちょうど0に戻った時は、手で直す前の出どころによらず、「手で直していない」に戻す（所有者の決定6の2・7の1）。Pを手で直す前の位置（ビットで同じ）にし、出どころを手で直す前の出どころに戻し、手の印（手で直す前の位置の出どころと入れた量）を消す。撮影の後は、戻した出どころの決定5の表の行のとおり、今までどおり自動が位置を決める。状態の行で「手で動かした量が 0 に戻ったので、手で直す前の位置に戻しました。撮影の後の自動調整で位置が変わることがあります」と言う。手で直す前が条件の位置なら、決定5の補足の1により表の1行目になる。
   - 合計が0でないのに、結果のPが条件の位置とビットで同じになった時も、出どころを`fixed-condition`にして手の印を消し、表の1行目にする（技術の側で決めた事の5。起きる見込みは小さい）。
   - 手の押下は推定を使わないので、`registration.live`が無い条件でも受け付ける。窓は条件v3でライブの合成の位置合わせを始められた時は、`registration.live`の有無によらず手のボタンを出す。
4. **範囲の端で断り、丸めない**: 手で動かした後の姿勢が、条件の`registration.envelope`の内（p_bからの差。`RegistrationComponentsFromBase`の判定）にある時だけ受け付ける。超える押下は断ってPを変えず、端の値に丸めて通す事もしない。断った時は、範囲の端である事と、設置が動いた時は校正の取り直し（#434）で直す事を画面に出す。次の4つは技術の側で決めた事（「技術の側で決めた事」の2・3・4）:
   - 見るのは姿勢の範囲（`registration.envelope`の位置・回り・傾き・高さの4成分）だけで、紙面の範囲（`surfaceEnvelope`）と層2の線は見ない。手は紙面を動かさず、紙面は手で直す前の位置のまま残る（`AGENTS.md`の"the condition's pose envelope"）。
   - 自動の推定が範囲の外のまま当てられた（`applyBeyondEnvelope`）Pからも、結果の姿勢が範囲の内に入る押下だけを受け付ける。結果が範囲の外に残る押下は、超えている成分を減らす向きでも断り、Pを変えない。断った時は、範囲の外の位置を直すのは「自動調整」のボタンか校正の取り直し（#434）である事を画面に出す。手は傾きと高さを動かさないので、傾きか高さが範囲の外のPからは、どの押下も断る。
   - 範囲とは別に、結果の写像に安い検査（決定5の補足の3と同じ`CheckRegisteredMappingCover`を、条件のprofileと継ぎ目の列、設計の定数の混ぜ幅`kSeamBlendDesign`で。混ぜ幅は条件の値ではない）を当てる。落ちる押下は断ってPを変えず、継ぎ目が重なりの外に出る事（または出力の端が覆われない事）を画面に出す。手の式が解けない（収束しない）押下も断る。
   - 受け付けるのは、ライブの合成がPで描かれている時（ライブが動いていて、使っている表がPで焼かれている時）だけ。ライブを止めている間、止める途中、セッションの始めの焼き直しが済む前、撮影の結果を当てた後の焼き直しが済む前、Pの焼き直しが落ちた後は断り、状態の行で言う（両台撮影と同じ判定。決定11）。Pの置き換えは焼き直しが通った後で（決定1）、ライブを止めている間は焼き直しが走らない（「今の作り」）ので、受け付けても描けず、画面に出ないPが撮影へ渡りうるため。撮影を頼んでから撮影の結果を当てるまでも断る（決定9）。
5. **JPEGの位置の選び方**: 撮影の要求の時に、ライブの合成が描いているP（機械の`Placement()`と`Serial()`。焼き直しを待っている位置の`Wanted()`ではない）を値で写し、通し番号と条件のSHA-256と一緒に撮影へ渡す。両台撮影を受け付けるのは、ライブの合成がPで描けている時だけなので（決定11）、写したPは画面に出ていた合成の位置と同じ。撮影の推定は今までどおりp_bから原寸で行う（Pを推定の始め・事前分布・範囲の基準にしない）。段は、Pの出どころで決まる候補を順に安い検査に通し、最初に通った物にする（下の表）。

「原寸の推定が当たる」の定義（技術の側で決めた事の1）: 今の`SelectGeometryLevel`がG1かG2を選ぶ事。つまり、推定器が位置を出して状態が`verified`か`out_of_envelope`で（`Trusted()`）、推定の姿勢と紙面が範囲の内か条件の`applyBeyondEnvelope`がtrueで、そうしてできたG1かG2の候補が安い検査に通る事（「今の作り」の段の候補の作り方）。設計11.18の「確か」とコードの`Trusted()`は、このうち推定器の状態だけを言い、本ADRの「当たる」より広い。`AGENTS.md`の新しい項の"trusted estimate"は、「当たる」の意味で読む。手の行では、G1・G2の候補を同じ作り方で作って安い検査に当て、当たるかどうかを決める（JPEGには使わない）。`Trusted()`の名前と意味は変えない。

| 撮影の前のPの出どころ | 候補の順（最初に安い検査を通った物を使う） | JPEGの写像 | 撮影の後のP |
|---|---|---|---|
| 条件の位置（`fixed-condition`）、補足の1・2に当たるP、Pを渡されない合成 | 今と同じ: G1・G2（推定が当たる時）、G3（`Trusted()`でなく、粗の段の目安がある時）、G4 | 選んだ段の写像（今と同じ） | JPEGの写像（層2を除く）、`after-capture` |
| 自動（`auto-start`・`auto-button`）、前の撮影（`after-capture`） | G1・G2（今と同じ作り方）、新しい段`confirmed`（写像はP、層2なし）、G4。G3は候補にしない | 選んだ段の写像 | JPEGの写像（層2を除く）。G1・G2・G4では`after-capture`。`confirmed`ではPそのもので、出どころも変えない |
| 手（`manual`） | `confirmed`、G4。G1・G2は候補にしない | `confirmed`ならP、G4なら条件の位置 | Pのまま（`manual`のまま。G4に下がっても動かさない） |

手の行では、原寸の推定が当たる時、その写像（層2を除く）とPの継ぎ目での差を測って記録する（`confirmedPositionCheck`）。

3つの場面の答え（技術の側で決めた事の1・6）:

- (a) 推定器の状態が`out_of_envelope`で、条件の`applyBeyondEnvelope`がfalse: G1・G2の候補ができないので、推定は当たらない。自動のPなら`confirmed`でJPEGを作り、撮影の後もPのまま。条件の位置のPなら今と同じG4。手のPなら`confirmed`。
- (b) G1・G2の候補はあるが、安い検査に落ちる: 推定は当たらない。自動のPなら`confirmed`。`confirmed`も落ちればG4で、各段の理由を残し、撮影の後のPはG4の写像（条件の位置、`after-capture`）になる。今のコードはこの場面で、G1・G2の後にG3を作らないのでG4にする。`confirmed`が落ちると、撮影の前に見ていたライブ（P）とJPEG（条件の位置）が違うので、撮影の後の文と報告でそう言う（「保証する物としない物」の「ライブとJPEGが違う撮影」の2）。ライブの推定の結果には描く前に同じ検査を当てるので（決定2）、同じ条件ではこれが起きるのは、前の撮影がG1で、層2を除いた写像（撮影の後のP）が検査に落ちる時に限られる見込み。
- (c) 手のPが、撮影の時に安い検査に落ちる: JPEGはG4（条件の位置）で作り、撮影の後のPは手のまま（`AGENTS.md`の"a hand-set position stays"）。この場面では、撮影の後のライブ（手の位置）とJPEG（条件の位置）が違う（「ライブとJPEGが違う撮影」の1）。撮影の後の文と報告で、「手で直した位置では継ぎ目が重なりの外に出るため、JPEG は条件の位置で作りました。ライブは手で直した位置のままです」（出力の端が覆われない時は「出力の端が覆われないため」）と言う。手の押下に同じ検査を当てるので（決定4）、同じ条件（SHA-256が同じで、同じprofileを名指す）ならこの場面は起きない見込み（「今の作り」のとおり、検査は写像と条件と設計の定数だけで決まる。押下の時と撮影の時の検査が同じ答えを返す事は、試験の5で縛る）。

決定5の補足:

- Pが条件の位置とビットで同じ（姿勢がp_b、紙面が0）なら、出どころによらず表の1行目として扱う（`AGENTS.md`の"a position that is still the condition's own keeps the existing geometry levels"）。手で動かしてこうなったPは、押下の時に手の印を消す（決定3）。
- Pの条件のSHA-256が、撮影の時に読んだ条件と違えばPを使わず、表の1行目として扱い、使わなかった理由を記録し、撮影の後の文でも言う（「ライブとJPEGが違う撮影」の3）。
- 撮影の結果を当てる時、`serial`は、Pの9数か出どころが変わった時だけ進める。`confirmed`の時と、手の行でG4に下がった時は進めない（技術の側で決めた事の10）。
- `confirmed`の写像が安い検査（継ぎ目が重なりの内、出力が覆われる。設計11.23.4）に落ちた時はG4に下げ、理由（`seam-outside-overlap-at-level`・`output-uncovered-at-level`）を残す。撮影の後のPは、自動の行ではG4の写像（`after-capture`）、手の行では手のまま（上の(c)）。手の押下にも同じ検査を当てる（決定4）。
- 撮影の要求の時に推定が走っていたら、その結果は捨て、描いているPを渡す。焼き直しを待っている位置（推定の結果か手の押下）も捨てる（決定9）。

6. **窓の中だけで持つ**: Pは窓（プロセス）の中の値で、ライブを止めて再開しても、撮影の後も続く。窓を閉じると捨てる。条件が変わった時は、手で直した位置も捨てる（決定10。所有者の決定6の1）。ファイルに書くのは、Pを使った合成の記録（報告とログ）だけ。条件とprofileに書くのは人が明示して保存した時だけで、段3で`manual-adjustment`か`saved-estimate`の新しい改訂として書く。自動で保存しない。学習しない。
7. **記録**: 報告をv9（`a0.uncalibrated-still-review.v9`）にし、Pの値・出どころ・通し番号・条件のSHA-256・手で入れた量・原寸の推定との差を残す（欄の案は「報告v9の差分（案）」）。ライブのログと撮影の側のログは、同じ関数で同じ17桁の書き方にする。撮影の側の行は、窓が写した値でなく、JPEGの作り手が書いた記録（読み直した報告）から書く。ログの基準と対象のカメラの欄（`reference_camera=CAM-A target_camera=CAM-B`）は、v9と同じcommitで、ライブの行と撮影の側の行の両方を書く1つの関数に足す（技術の側で決めた事の7、「ADR-0045との関係」の3）。
8. **表示と品質**: ライブの合成は目安で、「基準内」と書かない。手で直した事、ライブで決めた事は、品質の証拠にも不合格の理由にもしない。ゲートは`confirmed`の写像もほかの段と同じように測り、`assessment`の導き方は変えない。`productQualityAccepted`は常にfalse。画面と報告には、手で直した位置で作った事、ライブで決めた位置で作った事を、ゲートの結果と分けて出す。
9. **撮影の間はPを動かさない**（技術の側で決めた事の2・12）: ここでの撮影は両台撮影。撮影を頼んだ時に、走っている推定の結果と、焼き直しを待っている位置（推定の結果と手の押下）を捨て、撮影へ渡したPのままにする。待っている位置を捨てる時は、待つ写像の通し番号（`WantedSerial()`）を進めて、焼き直しの途中の働き手にその結果を捨てさせ、手の押下の入れた量の合計もPの物に戻す（技術の側で決めた事の9）。撮影を頼んでから撮影の結果を当てるまで（JPEGができなかった時は、その知らせが戻るまで）は、手の押下と「自動調整」のボタンを断り、状態の行で「撮影中は位置を動かせません。撮影が終わると動かせます」と言い、「自動調整」を押した時は「撮影中は「自動調整」を押せません。撮影が終わってから押してください」と言う。JPEGがP(n)、ライブがP(n+1)という食い違いを作らないため。焼き直しの途中の働き手は、撮影を頼んだ後でもPを置き換えうる（「今の作り」）ので、止めるのは機械の側で行う。推定の頼みそのものは、ライブを止めた時と同じく残し、JPEGができて結果を当てた時に取り消す（段1のまま）。JPEGができなかった時は、再開の時にその1回だけ続ける（所有者の決定5の3と同じ扱い）。片方だけの撮影は合成のJPEGを作らず、Pを渡さず、撮影の結果でPを変えないので、ライブを止めた時と同じ扱いにする（頼みを残し、手の押下はライブを止めている間として断る）。
10. **条件が変わった時**（所有者の決定6の1）: ライブを始める時に読んだ条件のSHA-256が、Pの持つSHA-256と違えば、Pは手の位置でも捨てて新しい条件の位置に戻し（手の印と入れた量も捨てる）、推定を1回頼む（段1の`BeginSession`のまま。新しい条件に`registration.live`が無い時は頼まない）。「アプリを閉じるまで覚える」（段1の前の決定の3）の例外。Pが手の位置だった時は、状態の行の前に「固定の合成条件が変わったので、手で直した位置を捨て、新しい条件の位置に戻しました。自動調整を 1 回行います。」を付ける（新しい条件に`registration.live`が無い時は「自動調整を 1 回行います。」を付けない）。Pが手の位置でなかった時は、この文を付けず、状態の行は推定の間の文と推定の結果の文になる。
11. **Pで描けている時だけ両台撮影を受け付ける**（所有者の決定6の3）: 両台撮影の要求は、画面に出ているライブの合成の絵を描いた表が、Pで焼いた表である時（その表の焼いた写像がPとビットで同じ時）だけ受け付ける。セッションの始めと撮影の結果を当てた後の焼き直しの途中と、Pの焼き直しが落ちた後は、断って撮影を始めず、Pも`serial`も変えず、状態の行で「ライブの合成がまだ確定した位置で描けていないので、撮影できません。少し待ってからもう一度押してください」と言う。Pの焼き直しが落ちたまま（同じ写像は焼き直さない。「今の作り」）の時は、「ライブの合成がまだ確定した位置で描けていないので、撮影できません。「自動調整」を押すか、ライブを止めて再開してください」と案内する。「自動調整」の推定が位置を出せば待つ写像が変わり、ライブを始め直せば基準の表が置き直されるので、どちらでも焼き直しがもう一度試される。
   - 所有者の決定6の3の「描き直しの途中」は、Pが変わった後か表が置き直された後に、Pで表を焼いている間と読む（技術の側で決めた事の14）。手の押下か推定の結果が焼き直しを待っている間は、ライブはまだ前のPで描けているので受け付け、待っている位置は捨てる（決定9）。
   - 条件のためにライブの合成に絵が無い時（今の作りで撮影を止めない場面。`RequestCapture`の`no_picture_by_condition`）は、今のまま受け付け、Pを渡さない（表の1行目。描いている位置が無い）。
   - この決定により、撮影の要求の時に画面に出ていたPと、JPEGに渡すPが同じと言える（保証1、不変条件2）。手の押下も同じ判定で断る（決定4）。

#### 報告v9の差分（案）

- `schema`は`a0.uncalibrated-still-review.v9`。ほかの欄はv8と同じ名前・型・並び。v8の読み手は残し、v8の報告を今までどおり読む。
- `fixedCompositeRegistration.confirmedPosition`（`geometryLevel`の前）: nullか`{referenceCamera, targetCamera, serial, source, baseSource, conditionSha256, pose{rxDeg, ryDeg, rzDeg, txMm, tyMm, tzPercent}, surface{h0Mm, slopeX, slopeY}, manualEntered}`。`referenceCamera`は`CAM-A`、`targetCamera`は`CAM-B`。`baseSource`は出どころが`manual`の時だけ符号で、ほかはnull。`manualEntered`は`{seamDxPx, seamDyPx, rotationDeg}`かnull。窓から位置を渡されなかった合成（校正の取り直しの見本の合成など）はnull。
- `fixedCompositeRegistration.confirmedPositionCheck`: nullか`{used, seamDifferencePx}`。`used`は`confirmed`の段で作ったか。`seamDifferencePx`は、原寸の推定が当たる時（決定5の定義）のその写像（層2を除く）とPの、継ぎ目での差の最大（出力px）で、当たらない時はnull。測り方は1つの関数にし、画面の文に出す数と同じ値にする。
- `geometryLevel.level`に`confirmed`を足す。読み手は、写像の姿勢と紙面が`confirmedPosition`とビットで同じで、`parallax`がnullである事を確かめる。`applied.pose`はtrue。`beyondEnvelopeApplied`（派生値）は、`confirmed`ではPの成分（p_bからの差）と紙面で見る。`camera-*`と`surface-*`の警告も、`confirmed`ではPの成分から導く（G1・G2の推定の成分と同じ導き方）。`confirmed`の段で、原寸の推定が確か（`Trusted()`）で、推定の姿勢の成分か紙面が線を超えた時は、別に`unverified`の警告を1つ出す（符号は`estimate-beyond-envelope`、`interpretations`に超えた成分の`camera-*`の符号と、推定した紙面が範囲の外なら`surface-height`・`surface-tilt`、`remedy`は`check-camera-mount`）。`unverified`なので`assessment`は`not-judged`になり、ゲートが通っても`within-criteria`にならない。JPEGはPで作る（所有者の決定7の2、技術の側で決めた事の13）。
- 警告の符号に`position-by-hand`（notice、`remedy`は`none`）と`position-from-live`（notice、`none`）を足す。成分の符号は`confirmed-position`。量の無い符号として書く。
- 新しい欄の名前に`placement`を使わない。v8の`gate.placement`（大域的な配置の確かめ方）と、警告の`placement-unverified`・`placement-mismatch`が別の意味で使っている。コードの識別子（`LivePlacement*`）とログの行の名前（`C live_placement`）は変えない。

### 技術の側で決めた事（所有者の決定の趣旨に合わせた）

2026-11-19に、承認された`AGENTS.md`の文と本ADRの決定が1つの答えになるように、技術の側で決めた。所有者の決定（「ADR-0046の所有者の要件と決定」）ではない。所有者が違う形を選んだ時は、本ADRを改める。同じ日に所有者が決めた物（条件が変わった時に手の位置を捨てる事、合計が0に戻った時の扱い）は、所有者の決定6の1・2に移し、この表から外した（前の7と、前の5の一部）。前の8は7にした。8〜15は再レビューの後に足した。

| # | 決めた事 | 理由 | 合わせた所有者の決定と`AGENTS.md`の文 |
|---|---|---|---|
| 1 | 「原寸の推定が当たる」を、`SelectGeometryLevel`がG1かG2を選ぶ事と定め、出どころごとの候補の順を決めた（決定5） | `Trusted()`の意味で読むと、範囲の外で`applyBeyondEnvelope`がfalseの撮影と、G1・G2が安い検査に落ちる撮影で、JPEGが今のコードのままG4になり、ライブで見ていた位置と違うJPEGができる | 段1の前の決定の2。"when the estimate is not trusted, or the position was set by hand, it uses the confirmed position" |
| 2 | 両台撮影を頼んでから結果を当てるまで、手の押下と「自動調整」のボタンを断り、走っている推定と焼き直しを待っている位置を捨てる（決定9） | JPEGがP(n)、ライブがP(n+1)になるのを防ぐ。今の作りでは、焼き直しの途中の働き手が、撮影を頼んだ後にPを置き換えうる | 受入の条件（ライブとJPEGが食い違わない）。"Every pair composite records the confirmed position it was given" |
| 3 | 手の押下で、残していた自動の頼みも取り消す。ライブがPを描いていない間の手の押下は断る（決定3・4） | 手で直した後に、前に頼んだ推定が手の位置を上書きしない。ライブを止めている間は焼き直しが走らず、手の位置を描けない | 段1の前の決定の1。所有者の決定5の3（所有者に示した推しの文に「その間に手で動かせば、その推定は捨てる」がある。「所有者に確かめる事」の2） |
| 4 | 手の押下に安い検査を当てて断る。範囲の外のPからは、結果が範囲の内に入る押下だけを受け付ける。見るのは姿勢の範囲だけ（決定4） | 撮影の時に手の位置が検査に落ちてJPEGが条件の位置になる場面（決定5の(c)）を、押下の時に防ぐ。範囲の外に残る押下を通すと、範囲の外の手の位置ができる | 所有者の決定5の1（決定4の承認）。"only while the result stays inside the condition's pose envelope … a step past the envelope is refused, never clamped into success" |
| 5 | 入れた量の合計が0の時は、解かずに手で直す前の位置そのものを返す。合計が0でないのに結果が条件の位置とビットで同じになったPは、出どころを`fixed-condition`にして手の印を消し、表の1行目にする（決定3） | 所有者の決定6の2の「手で直す前の状態に戻す」を、式の丸めによらずビットで決める。条件の位置と同じPに手の印を残すと、表の1行目と"a hand-set position stays"が食い違う | 所有者の決定6の2。"a position that is still the condition's own keeps the existing geometry levels"、"a hand-set position stays" |
| 6 | 撮影の時に手の位置が安い検査に落ちたら、JPEGはG4、Pは手のまま、そう文で言う（決定5の(c)） | 手の位置を自動で動かさない。ライブとJPEGが違う事を隠さない | 段1の前の決定の1。"a hand-set position stays" |
| 7 | ログの基準と対象のカメラの欄を、v9と同じcommitで、両方の行を書く1つの関数に足す（決定7） | 撮影の側の行は、段2で読み直した報告v9から新しく書く行で、v9の`confirmedPosition`は`referenceCamera`・`targetCamera`を持つ。同じ欄が無ければログと報告を突き合わせられない | 受入の条件（値と出どころを同じ書き方で残す）。"written with the same numbers as the live log" |
| 8 | ライブの推定の結果にも、描く前に手の押下と同じ安い検査を当て、落ちれば決まらなかった扱いにする（決定2） | 検査を通さずに受け付けると、撮影の時に`confirmed`が落ちてG4になり、撮影の前に見ていたライブとJPEGが違う（決定5の(b)）。手の押下と同じく、描く前に防ぐ | 要件1（その映像はJPEGと同じ）。段1の前の決定の2 |
| 9 | 手の押下を描く前に次の押下が来た時は受け付け、待っている位置を、合計から解き直した物に置き換える。待っている位置を捨てた時は、入れた量の合計もPの物に戻す（決定3・9） | 続けて押した時に断ると押下が抜ける。合計を戻さないと、記録の`manualEntered`とPの写像が合わない | 不変条件12（合計を1回で解く） |
| 10 | 撮影の結果を当てる時、Pの9数か出どころが変わった時だけ`serial`を進める（決定5の補足） | `serial`はPの変化の数。値が同じPで進めると、報告の`serial`と撮影の後のライブの`serial`が同じPで食い違う | 不変条件1。"a change counter (`serial`)" |
| 11 | ライブを始めた時（再開を含む）は、残した頼みの推定より先にPで表を焼く。焼き直しが落ちても、残した頼みの推定は走らせる（決定2） | 今の`RunPass`は推定を先に走らせる（「今の作り」）。Pを焼く前に推定が走ると、その間は手の押下を断るので（決定4）、所有者に示した「その間に手で動かせば、その推定は捨てる」が成り立たない。焼き直しが落ちたまま推定も走らないと、頼みが残り続けて「自動調整」のボタンも断られる | 所有者の決定5の3（所有者に示した推しの文。「所有者に確かめる事」の2）。所有者の決定6の3の案内 |
| 12 | 決定9の撮影は両台撮影だけ。片方だけの撮影は、ライブを止めた時と同じ扱いにする（決定9） | 片方だけの撮影は合成のJPEGを作らずPを渡さず、撮影の結果でPを変えない。凍結する物が無い | "Every pair composite records the confirmed position it was given" |
| 13 | `confirmed`の段の`camera-*`・`surface-*`はPの成分から導き、原寸の推定が確かでその姿勢の成分か紙面が線を超えた時は、別に`unverified`の警告を1つ出す（報告v9の差分）。判定は`not-judged`になる | Pの成分だけから導くと、場面(a)（推定は確かだが範囲の外）で、設置が動いたかもしれない合図が警告から消える。`notice`にすると、写真が言う設置の動きと、ライブで決めた位置で作ったJPEGの食い違いを「基準内」と言いうる | 所有者の決定7の2。決定8（ゲートの結果と分けて出す）。ADR-0037の決定5（設置が変われば校正をやり直す） |
| 14 | 所有者の決定6の3の「まだ描けていない間」を、画面に出ている絵を描いた表の焼いた写像がPとビットで同じでない間と読む。待っている位置の焼き直しの間は受け付け、待っている位置を捨てる（決定11） | 待っている位置はまだ確定していない。その間もライブは前のPで描けていて、渡すPは画面のPと同じ。判定を`CurrentPlan()`の今の値でなく画面に出ている絵の表で行えば、表が替わった直後の1枚でも食い違わない | 所有者の決定6の3。決定9 |
| 15 | 入れた量は最小の刻み（4 px・0.05°）の整数の数で持ち、合計と「ちょうど0」の判定をその数で行う。式と記録に渡す量は、その数から1回で作る（決定3） | 倍精度で足し引きすると、0.2°＋0.05°−0.2°−0.05°が0にならない（−1.39e-17。2026-11-19にPythonの倍精度で`0.2+0.05-0.2-0.05`を計算した）。押した順で「ちょうど0」かが変わる。1 pxの刻みを足す時は、最小の刻みを1 pxにする | 所有者の決定6の2（ちょうど0）。所有者の決定5の4（刻み） |
| 16 | 入れた量の合計が0に戻る押下は、決定4の「押下」に数えない。手で直す前の位置が範囲の外の自動の位置でも、その位置に戻す（所有者の決定7の1） | 戻る先は手で直す前に描いていた位置そのもので、手で動かして範囲の外へ出る押下ではない。範囲の端で断る決まりは、後者のためにある。合計が0の押下を断ると、範囲の外の自動の位置から動かした後で元に戻れなくなる | 所有者の決定7の1・6の2。`AGENTS.md`の"a step past the envelope is refused, never clamped into success"（決定4の読み方） |
| 17 | `confirmed`でも、ゲートの広い探索の診断（`wide_search`）をG3・G4と同じに走らせる（書式の文書4.13.3） | `confirmed`は推定が裏に付かない段で、G3・G4と同じく推定の帯を使わない | 決定8（ゲートは`confirmed`の写像もほかの段と同じように測る） |

### 保証する物としない物

縮尺の数は今のコードから出す。ライブの合成の格子の比sは、ライブの枠の幅wを撮影の幅7360（`kD810SensorWidthPixels`、`src/m2/include/a0/m2/optical_planner.hpp` 9行）で割った値で（`src/phase0/include/a0/phase0/experimental/fixed_composite_preview.hpp` 320〜323行の`PlanLensAwarePreview`）、格子のピッチは出力のピッチの1/s（同222行の`MakeFixedPreviewGrid`）。したがってライブの合成の1画素は、出力の7360/w画素に当たる。ライブの枠の幅を640とすると（ライブの縮小の長辺の上限。`docs/M2_AUTO_OVERLAP.md`のF7と`docs/M2_SEAM_QUALITY_GATE.md`が使う値。縮小の前の枠の画素数は#309で測っておらず、縮小の後の幅が640になるかは※仮定）、11.5 pxで、180 dpiの出力では図面の上の約1.6 mm。本ADRの11.5と、それから出した数（約6 px、0.09画素、0.35画素）は、この仮定による。

| # | 保証する事 | 大きさ | 確かめ方（「一致の試験の計画」の番号） |
|---|---|---|---|
| 保証1 | 両台撮影の要求の時にライブの合成が描いているP、JPEGに渡したP、JPEGが`confirmed`で当てた写像は同じ値。両台撮影を受け付けるのはライブの合成がPで描けている時だけなので（決定11）、描いているPはPそのもの | 9数がビットで一致（-0と+0は同じと数える） | 1・3・4・10・11 |
| 保証2 | 同じPから作るライブの写像は、JPEGの写像をsで縮めた物 | 1e-6ライブ画素以内 | 既存の`SameConventionAsTheJpeg`に手の位置を足す（1） |
| 保証3 | 撮影の後、ライブの合成はJPEGと同じ写像で描く | `confirmed`では層2の違いも無い。G1では層2だけが違い、その最大を記録の値で出す（段1のまま） | 3・6 |
| 保証4 | 手で入れた量が、ライブとJPEGに同じだけ効く | 式の上で、継ぎ目の列の中央の行の変位が入れたdx・dyと1e-6 px以内 | 5 |
| 保証5 | 位置の値と出どころが、ライブのログとJPEGの報告で同じ書き方 | 同じ関数が書いた17桁の文字列が一致 | 4 |

保証しない事:

- **見た目の細かさ**: ライブの合成の1画素は出力の11.5 px。継ぎ目のずれが目で分かるのをライブで0.5画素とすると（※経験則）、ライブで合って見えてもJPEGでは約6 px（0.5 × 11.5）までずれうる（※推定）。ゲートの線（出力1 px）はライブの0.09画素で、ライブでは見えない。1 pxの精度を言えるのは原寸のゲートだけ。手の刻みの4 pxはライブの約0.35画素で、押してもライブではほとんど動いて見えない。
- **ライブの推定の精度**: ライブの解像度で推定したPは、原寸の推定より粗い（不確かさがライブの画素で出るので、出力の画素では約11.5倍になる）。`confirmed`の段は、その粗さのままJPEGに入る。そのずれは報告とゲートが測って出す。
- **ライブの枠と撮影画像の対応S**（#309）: 実機で測っていない（※未検証）。Sが外れていれば、ライブで推定したPにも、ライブを見て手で直したPにも、その分が入る。
- **時刻の違い**: 推定に使うCAM-AとCAM-Bのライブの2枚は同時ではない。最後のライブの枠からシャッターまでの間に紙や台が動いた分は、Pに入らない。
- **紙を替えた時**: Pは前の紙の紙面を持ったままで、ボタンを押すまで新しい紙には合わない（画面で押すように案内する）。
- **ライブとJPEGが違う撮影**: 次の3つの場面では、撮影の前に見ていたライブか、撮影の後のライブが、JPEGと違い、保証1・3の外になる。どれも撮影の後の文と報告で言う（撮影の側の文は`ConfirmedPositionText`）。
  1. 手のPが、撮影の時に安い検査に落ちる（決定5の(c)）: JPEGは条件の位置（G4）、撮影の後のライブは手の位置。文は「手で直した位置では継ぎ目が重なりの外に出るため、JPEG は条件の位置で作りました。ライブは手で直した位置のままです」（出力の端が覆われない時は「出力の端が覆われないため」、段の理由が無い時は「安い検査に通らないため」）。ライブの状態の行は、記録の理由を持たないので両方を言い、「手で直した位置では継ぎ目が重なりの外に出るか出力の端が覆われないため、JPEG は条件の位置で作りました。ライブは手で直した位置のままです」と言う。
  2. 自動の行のPで、原寸の推定が当たらず、`confirmed`も安い検査に落ちる（決定5の(b)）: JPEGと撮影の後のライブは条件の位置（G4）で、撮影の前に見ていたライブ（P）と違う。文は「撮影の後の自動調整では位置を決められず、ライブで決めた位置では継ぎ目が重なりの外に出るため、JPEG は条件の位置で作りました。ライブも条件の位置にしました」（理由の部分は1と同じ替え方）。
  3. 撮影の時に読み直した条件のファイルのSHA-256が、Pの物と違う（決定5の補足の2）: Pは使わず、JPEGは新しい条件の段で作る。撮影の結果はライブに当てられず（`ApplyCapture`が別の条件を断る）、次にライブを始めた時に、Pは新しい条件の位置に戻り、自動調整を1回行う（決定10）。文は「撮影の前に固定の合成条件が変わったので、ライブの位置を使わずに、新しい条件で JPEG を作りました。ライブは新しい条件の位置に戻し、自動調整を 1 回行います」（手で直した位置だった時は、2文目が「手で直した位置を捨て、ライブは新しい条件の位置に戻し、自動調整を 1 回行います」）。
  - 1と2は、手の押下とライブの推定の結果に、描く前に同じ安い検査を当てるので（決定2・4）、同じ条件では起きない見込み（押下の時と撮影の時の検査が同じ答えを返す事は、試験の5で縛る）。2が残るのは、前の撮影がG1で、層2を除いた写像（撮影の後のP）が検査に落ちる時。3は、ライブを始めた後に条件のファイルが変わった時だけ起きる。G1の層2の違いは保証3のとおりで、この3つには数えない。
- **ライブの表がまだPで焼かれていない間**: セッションの始めと撮影の結果を当てた後の焼き直しが済む前と、Pの焼き直しが落ちた後は、ライブが描いている物（基準の表か前の表）とPが違う。この間は両台撮影と手の押下を断るので（決定11・4）、この違いはJPEGに入らない。画面のライブがPと違うのは、この間だけ。

### 一致の試験の計画

どれもカメラを使わない。期待値は試験の側の式か別の経路の出力から作り、確かめる関数そのものを呼んで期待値を作らない。

| # | 固定する事 | 置き場所（※案） | 落ちるべき変異 |
|---|---|---|---|
| 1 | 手の押下でPが変わり、ライブの合成のCAM-Bの表が、その写像（`RegisteredDocumentOutputToCamera`をsで縮めた物）と1e-6ライブ画素以内で一致する。`SameConventionAsTheJpeg`の写像の組に手の位置を足す | `preview_live_registration_runtime_contracts`、`preview_fixed_composite_v3_contracts` | 手の押下で焼き直さない |
| 2 | Pが勝手に動かない: 偽の時計でフレームと時刻を流しても、頼んだ推定の結果・手の押下・撮影の結果・条件の変化のほかでは`serial`が変わらない。推定は頼んだ回数だけ走る。推定の途中の手の押下は、その推定の結果を捨てる。押下の確かめの間に推定の結果が届き、描かれるのを待っていても、押下は受け付けられてその結果を捨てる（Pか、描かれるのを待つ手の位置が変わった時だけ、押下は古い扱いで断られる）。推定の途中でライブを止めると、その推定の位置は安い検査に当てられず、描かれず、頼みが残る。手の押下は、残していた頼み（頼んだだけでまだ走っていない物と、走っている物）を取り消し、その後に推定が走らない（`counters.jobs`が増えない）。推定の途中でライブを止めて頼みを残し、再開の後、推定が始まる前に手で押すと、その頼みは取り消され、推定は走らず、Pは手の位置のまま。再開の時は、残した頼みの推定より先にPで表が焼かれ（焼き直しの行が推定の行より先）、その推定の途中に手で押すと、推定の結果は捨てられ、頼みも取り消される。偽の焼き直しで落とした時も、残した頼みの推定は走る。ライブの推定の結果のうち安い検査に落ちる物（試験の側の検査の関数で落とす）は、決まらなかった扱いで、Pと`serial`が変わらず、理由が残る。ライブを止めている間と、セッションの始めの焼き直しの前と、Pの焼き直しが落ちた後の手の押下は断られ、Pと`serial`が変わらない。手の押下を描く前の次の押下は受け付けられ、待っている位置が置き換わり、描いた後のPは2つの押下の合計から解いた物とビットで同じ | `preview_live_placement_contracts`（偽の推定器）、`preview_live_registration_runtime_contracts` | 推定の結果で手の位置を上書きする。手の押下の後も頼みを残す。止めている間の押下を受け付ける。再開の時に推定を先に走らせる。安い検査に落ちる推定の結果を描く |
| 3 | JPEGの段: 決定5の表の3行、補足の4つ、場面(a)(b)(c)。(a) 状態が`out_of_envelope`で`applyBeyondEnvelope`がfalseの推定で、自動のPは`confirmed`、条件の位置のPはG4。(b) G1・G2の候補を安い検査で落とすと（試験の側の検査の関数で落とす）、自動のPは`confirmed`、`confirmed`も落とすとG4で、撮影の後のPはG4の写像（`after-capture`）。(c) 手のPで`confirmed`を落とすとG4で、撮影の後のPは手のまま（出どころ`manual`、9数がビットで同じ）。`confirmed`の撮影と、手の行でG4に下がった撮影の後は`serial`が撮影の前と同じで、G1・G2と自動の行のG4の後は1つ進む。撮影の時に読み直した条件のSHA-256がPの物と違うと、Pは使われず（`confirmedPositionCheck.used`がfalse）、使わなかった理由が残る。`confirmed`の写像がPとビットで同じ。手の時は、原寸の推定が当たっても写像がPのまま。Pが条件の位置の時、合成の画素のバイトがPを渡さない時と同じ。Pが条件の位置でない時も、原寸の推定の結果（`registration`）がPを渡さない時とビットで同じ（Pが推定に入らない事）。条件の位置でないPの`confirmed`の合成の画素が、同じPを別の経路（試験の中で`RegisteredDocumentMappedFixedSeamInput`と`RenderMappedFixedSeam`を原画像2枚に直接当てる）で描いた画素とバイトで同じ | `m2_capture_registration_contracts`（既存のG3・G4の合成の場面を使う）、`still_review_condition_v3_contracts`、`preview_live_placement_contracts`（撮影の後のP） | Pを推定の始めにする。原寸の推定で手の位置を上書きする。(a)(b)でG4にする。(c)でPを条件の位置にする。`confirmed`で層2を当てる |
| 4 | ログと報告: 偽の撮影（`pair_capture_backend`）で撮り、ライブの行`C live_placement …`と、読み直した報告v9から書いた撮影の側の行の`serial`・`source`・基準と対象のカメラ・9数の文字列が一致する。報告のJSONの数を読み直すと、渡したPとビットで同じ | `preview_pair_v3_ui_contracts` | 数の書き方を片方だけ変える。撮影の側の行を窓の写しから書く |
| 5 | 手の式と押下の検査: 紙面のあるPで、継ぎ目の列の中央の行の変位が入れたdx・dyと1e-6 px以内。紙面が0の時は今の`SolveManualChange`と同じ値。押した順を変えても同じ値。合計が0の時は手で直す前の位置とビットで同じ。範囲のちょうど端は受け付け、越える押下は断ってPを変えない（丸めない）。範囲の外のPから、結果が範囲の内に入る押下は受け付け、結果が範囲の外に残る押下は（超えている成分を減らす向きでも）断ってPと`serial`を変えない。傾きか高さが範囲の外のPからは、どの押下も断る。紙面が紙面の範囲の外でも、姿勢が範囲の内なら受け付ける（見るのは姿勢の範囲だけ）。安い検査に落ちる押下と、式が解けない押下は断る。同じ条件で、押下の時の検査（`CheckHandStep`）、ライブの推定の結果の検査、撮影の時の検査（`RegisteredMappingCoverOnThreads`）が、境の近くの写像（ちょうど通る物と1刻み先で落ちる物を、継ぎ目が重なりの外に出る向きと出力の端が覆われない向きの両方で）に同じ答えを返す。3つとも同じ1つの関数`CheckRegisteredMappingCover`を呼び、その答えは、原寸（`kD810SensorWidthPixels`×`kD810SensorHeightPixels`）の枠を確保して数えた前の作りの答えと同じで、走査のスレッドの数によらない | 新しい純粋な部品の試験（m2）、`preview_live_placement_contracts`、3つの検査の一致は`pair_review_cover_check_contracts`、枠の版と大きさだけの版の一致は`m2_seam_blend_mapped_contracts` | 範囲で断らない。端に丸める。範囲の外のPで、範囲の外に残る押下を通す。紙面の範囲で断る。撮影の時の検査だけを変える |
| 6 | 画面の流れ: 開始 → 確定 → 手の押下 → 撮影 → 撮影の後もPが手のまま、文が「手で直した位置でJPEGを作りました（自動で測ると継ぎ目で最大N px違います）」の形。ボタンを押して推定が通ると手の印が消える。条件の位置からと自動の位置（`auto-button`）から、それぞれ+12 pxの後に−12 pxを押すと（0.2°と0.05°を混ぜて押して戻した時も）、Pが手で直す前の位置とビットで同じになり、出どころが手で直す前の物に戻り、手の印が消え、状態の行がそう言う。自動の位置から戻した時は、次の撮影の後に自動が位置を決める（表の2行目）。刻み: 1回の押下で増える最小の刻みの数が、12 pxは3、4 pxは1、0.2°は4、0.05°は1で、刻みはこの4つのほかに無い。`manualEntered`は、その数から作った値とビットで同じ。撮影の後の文が、手のPがG4に下がった時、自動のPがG4に下がった時、撮影の前に条件が変わった時に、保証しない事の「ライブとJPEGが違う撮影」の文の形になる。手のボタンが窓の幅1000・1024・1056・1400・1920で外へはみ出さず、ほかのボタンと重ならない | `preview_live_c_ui_contracts`、`preview_pair_v3_ui_contracts`、既存の`CheckButtonLayouts`、`preview_live_placement_contracts`（刻みと0に戻る場面） | 撮影の後にPを推定の写像にする。0に戻ったPに手の印を残す。手で直す前が自動の時に手の位置のままにする。刻みの値を変える。入れた量を浮動小数で足し引きする |
| 7 | 条件のSHA-256が変わると、Pは手の位置でも条件の位置に戻り、手の印と入れた量が消え、推定を1回頼み、状態の行が手の位置を捨てた事を言う（所有者の決定6の1）。別の条件のPは、JPEGにもライブにも使わない。撮影の時に読み直した条件が変わっていた時は、撮影の結果がライブに当てられず、次にライブを始めた時にPが新しい条件の位置に戻る | `preview_live_placement_contracts`、`m2_capture_registration_contracts`、`preview_live_c_ui_contracts`（文） | 条件の変化でPや手の印を持ち越す |
| 8 | 書式: v9の読み手が厳格（未知の欄・欠けた欄、`confirmed`の写像が`confirmedPosition`と違う、知らない符号を拒否）。v8の読み手が今のv8を前と同じに読み、既存の試験が試験のコードを変えずに通る。v9の文字列は`tests/registration_fixtures.hpp`の関数で作る | `m2_capture_registration_contracts`ほか、v8を読む既存の試験 | 読み手が`confirmed`の写像を確かめない |
| 9 | 変わらない物: 条件が無い時・v1・v2のライブの合成。未校正のライブの合成のボタン300〜304の働き | `preview_fixed_c_ui_contracts`ほか既存の試験 | |
| 10 | 撮影の要求の時のP: 焼き直しの途中（推定の結果か手の押下が描かれるのを待っている間）と推定の途中に撮影を頼み、報告v9の`confirmedPosition.serial`と9数が、頼んだ時に描いていたP（`Placement()`・`Serial()`）と一致し、`Wanted()`の値でない。走っていた推定の結果と、待っていた位置は捨てられ、撮影の後のPに入らない。機械の試験で、待っていた位置の`Committed()`を撮影の要求の後に呼んでも、Pが変わらない。撮影を頼んでから結果を当てるまでの手の押下と「自動調整」のボタンは断られ、Pも`serial`も変わらず、状態の行がそう言う。待っている位置を捨てる時に`WantedSerial()`が進む。待っていた手の押下を捨てた時、報告の`manualEntered`・`baseSource`と、撮影の後のライブの入れた量が、Pの物（捨てた押下を含まない）と同じ。片方だけの撮影では、残した頼みが再開の後に1回だけ走る | `preview_live_placement_contracts`（機械）、`preview_pair_v3_ui_contracts`（偽の撮影の画面） | 撮影へ`Wanted()`を渡す。撮影の要求で待っている位置を捨てない。撮影の途中の手の押下を受け付ける。捨てた押下の量を報告に残す |
| 11 | Pで描けていない間の両台撮影: セッションの始めの焼き直しの前、撮影の結果を当てた後の焼き直しの前、Pの焼き直しが落ちた後（焼き直しは、表を焼けない位置を当てて落とす）に両台撮影を頼むと断られ、撮影は始まらず（偽の撮影の呼ばれた数が増えない）、Pも`serial`も変わらず、状態の行がそう言う。焼き直しが落ちたままの時は、状態の行が「自動調整」とライブの再開を案内する。「自動調整」の推定の結果が描かれた後と、ライブを再開してPが描かれた後は受け付ける。判定は画面に出ている絵を描いた表で行い、表が替わった直後でも、渡したPは画面のPと同じ。条件のためにライブの合成に絵が無い時は今のまま受け付け、`confirmedPosition`がnull | `preview_pair_v3_ui_contracts`（偽の撮影）、`preview_live_registration_runtime_contracts`（描けているかの判定） | 描けていない間の撮影を受け付ける。`CurrentPlan()`の今の値で判定する。案内を出さない |

### 理由

1. 所有者の要件（手で直した分をJPEGに同じだけ効かせる、ライブとJPEGを食い違わせない）は、ライブとJPEGが同じ1つの値を使わなければ守れない。値を1つにすれば、一致を9数のビットの比較として試験に固定できる。
2. 原寸の推定が当たる時（決定5の定義）はそれを使う。原寸の推定はライブの約11.5倍細かい画素で測るので（縮尺の比）、自動の位置しか無い撮影でJPEGの精度を落とさない。撮影の後はライブもその写像に移るので、撮影の後の「同じ」は保たれる（段1）。
3. 原寸の推定が決まらない撮影で条件の位置（G4）や目安（G3）に落ちると、ライブで見ていた位置と違うJPEGができる。ライブで確定していた位置を使えば、ライブで見た物とJPEGが同じになる（所有者の決定2）。
4. 手で直すのは、自動が決まらない場面（繰り返し模様、無地）がほとんどで、そこでは原寸の推定も決まらない事が多い（※推定）。手の位置を原寸の推定で上書きしなくても失う物は小さく、所有者が嫌った「手で直しても勝手にまた自動が効く」を避けられる（所有者の決定1）。差は数で出す。
5. 範囲と推定の基準をいつも条件のp_bに置けば、Pが撮影やボタンのたびに変わっても基準が歩かない。これが「自動で学習しない」の守りになる。
6. 窓の中だけで持ち、保存を人の明示にすれば、条件とprofileの改訂の決まり（ADR-0037の不変条件3、設計11.19.4の改訂の列）を変えずに済む。

### 退ける案

| 案 | 退ける理由 |
|---|---|
| 裏の追従を残す（段1の前の形） | 所有者が嫌った「手で直しても勝手にまた自動が効く」の元。静かな場面でも揺れ、推定と焼き直しがCPUを使う |
| ライブのPをいつもJPEGに使い、原寸の推定は確かめだけにする | 自動の位置しか無い撮影でも、JPEGの継ぎ目の精度がライブの解像度まで落ちる。所有者の「シャッター後は自動調整が1回入る」とも合わない |
| 今のまま、JPEGをp_bから独立に作る | 手で直した分がJPEGに効かない（所有者の2026-11-19の指示）。撮影の前のライブとJPEGが別の値になる |
| 手で直した後も、原寸の推定が当たるならJPEGをその推定で作り、「手の位置に戻す」ボタンを置く | 所有者の決定1 |
| 範囲の外のPから、超えている成分を増やさない押下だけを通す（前の※案） | `AGENTS.md`の"only while the result stays inside the condition's pose envelope"と合わない。範囲の外の手の位置ができる |
| 撮影の途中も手の押下を受け付け、撮影の結果を当てる時に`serial`を照合して食い違いを言う | 食い違いを作ってから言う形になる。断れば作らない（決定9） |
| 手の位置が撮影の時に安い検査に落ちたら、撮影の後のPをG4の写像にする | 手で直した位置が自動で動く（段1の前の決定の1、`AGENTS.md`の"a hand-set position stays"） |
| 条件が変わっても手の位置を持ち越す | Pは条件のp_bとprofileに結び付いた値で、別の条件では別の位置になる（不変条件11）。所有者の決定6の1 |
| 手で直す前が条件の位置でない時は、合計が0に戻っても手の位置のままにする（前の案） | 所有者の決定6の2（手で直す前の出どころによらず「手で直していない」に戻す） |
| Pで描けていない間の両台撮影も受け付け、描いている表の写像か`Wanted()`を渡す | 描いている表の写像は、出どころと`serial`を持つ確定した位置Pではなく、`Wanted()`は画面に出ていない。どちらを渡しても、画面か記録のどちらかと合わない。所有者の決定6の3 |
| 手の押下を描く前の次の押下を断る | 続けて押した押下が抜け、押した回数と動いた量が合わない（技術の側で決めた事の9） |
| 手の値を範囲の端に丸めて通す | `AGENTS.md` 39行（`clamp an over-limit result into success`の禁止）、ADR-0037の不変条件7 |
| 範囲の外の手の値も、警告つきで当てる（推定の`applyBeyondEnvelope`と同じ扱い） | 範囲の外の手直しの改訂は、撮影とライブの合成の読み手が使わない（設計11.29のN1、所有者の決定）。窓の中でだけ通すと、保存した時と使った時で扱いが変わる。範囲の外が要るのは設置が動いた時で、その直し方は校正の取り直し（#434） |
| 窓を閉じる時にPを条件やprofileに書く。Pを次の推定の基準にする | 自動の学習に当たる（`AGENTS.md` 39行、ADR-0037の不変条件2・3）。所有者の決定3（残す時は保存の操作） |
| Pを撮影の推定の始めにする | 収束は速くなる見込み（※推定）だが、ライブの値が原寸の推定に入り、Pの誤りが推定に移る。決まりの変更も広がる |
| 層2もライブの合成に描く | ライブの解像度では層2の点が足りない。ライブの写像の作り手（`LiveDocumentHomography`）は層2を受け付けない |
| 手の押下ごとに前の値へ足し込む | 押した順で値が変わりうる。段3で保存した改訂の値と、使った値がずれる |
| 段2を報告v8のまま、符号を足すだけで済ます（書式の決まり8の例外） | 例外は既にある欄だけを使う変更に限る。出どころ・通し番号・手で入れた量には新しい欄が要る |
| 新しい欄の名前を`placement`にする | v8の`gate.placement`と警告の`placement-*`が別の意味で使っている |
| 手の量をライブの画素で持つ | ライブの枠の大きさで値の意味が変わる。`manual-adjustment`（出力px）にそのまま保存できない |

### 守る不変条件

1. Pが変わるのは、頼んだ推定の結果、手の押下、撮影の結果、条件のSHA-256の変化の4つだけ。時刻とフレームの内容では変わらない。
2. ライブの合成のCAM-Bの表の写像はP。ただし、セッションの始めと撮影の結果を当てた後は表を焼き終えるまで、Pの焼き直しが落ちた後は次に焼けるまで、Pで描けていない。Pの置き換え（推定の結果と手の押下）は焼き直しが通った後。両台撮影と手の押下を受け付けるのは、画面に出ている合成の絵がPで焼いた表で描かれている時だけ（決定11・4）。JPEGに渡すPは、撮影の要求の時に描いていたP（機械の`Placement()`と`Serial()`。`Wanted()`ではない。`serial`で照合する）で、画面に出ていた合成の位置と同じ。
3. 推定の始め・事前分布・ゲージ・範囲の基準は、条件のp_b。Pにしない。
4. ライブの推定が決まらない時（安い検査に落ちた時を含む）はPを保つ。条件の位置に戻さず、再試行しない。撮影の時の原寸の推定が決まらない時は、決定5の表のとおり。
5. 手の押下は、結果の姿勢が範囲の内で（見るのは姿勢の範囲だけ）、結果の写像が安い検査に通り、ライブがPを描いている時だけ受け付ける。範囲の外のPからも、結果が範囲の内に入る押下だけ。越える押下は断り、丸めない。
6. Pはファイルに書かない（Pを使った合成の記録を除く）。条件とprofileへの書き込みは、人の明示の保存だけ（段3）。
7. JPEGの画素は、確かめた原画像2枚だけから作る。Pは幾何で、画像ではない。
8. ライブとJPEGは、同じPから、同じ作り手の写像・同じ継ぎ目の列・同じ混ぜ方（mmで同じ幅）で描く。違ってよいのは解像度とG1の層2だけで、その大きさは記録に残す。
9. 手で直した事、ライブで決めた事を品質の証拠に数えない。ゲートは同じように測る。
10. Pが条件の位置の時、合成の画素は今と同じバイト。条件が無い時と、v1・v2の条件の時の合成とライブの合成は変えない。
11. 別の条件のSHA-256のPは、JPEGにもライブにも使わない。
12. 手の位置は、手で直す前の位置に、入れた量の合計を1回で解いて足した物。入れた量は最小の刻みの整数の数で持ち、「ちょうど0」はその数で決める。
13. ライブのログと報告は、Pを同じ関数の同じ書き方（17桁）で書き、同じ`serial`・`source`・基準と対象のカメラを書く。
14. 両台撮影を頼んでから撮影の結果を当てるまで、Pは変わらない。手の押下と「自動調整」のボタンを断り、走っている推定と焼き直しを待っている位置を捨てる。
15. 手で直した位置は自動では動かない。手の位置を変えるのは、手の押下（合計が0に戻って「手で直していない」に戻る時と、条件の位置に戻った時を含む）、ボタンで頼んだ推定の結果、条件の変化だけ。撮影の後も手のまま（G4に下がった時も）。
16. 手の押下は、残していた自動の頼みを取り消す。

### 変わる決まり

2026-11-19にb081023の木で行番号を取り直した。`docs/DECISIONS.md`の行番号だけは、ADR-0035・0036・0037に本ADRの改定の追記を置いた後の木の物（同じ日。#434の記録の欄の変更はこの3つの節より後のADR-0045の節を触るので、手元のmainに統合してもこの行番号は変わらない）。`AGENTS.md`の行番号は、承認された文を入れる前の物。入れた後は、新しい項が41行に入り、入れる前の41行から後が1つずつ下がる（39・40行と34・37行は変わらない）。

| 決まり | 今の文（要旨） | 段2で当たる所 | 本ADRでの扱い |
|---|---|---|---|
| ADR-0036 決定1の5（`docs/DECISIONS.md` 667行） | Cは撮影、保存の合成、書き出し、補正の推定、profileの選択の入力にしない | ライブの映像から推定したPを、JPEGの位置に使う | 開発用プレビューの条件v3に限って例外にする。合成の画像そのものは、今までどおり入力にしない。手の位置には確認点C1（652行・673行。人の操作で決めた値は当たらない）の読み方を当てる |
| ADR-0036 決定1の10（674行）、ADR-0037 決定1・2（793行・794行）、ADR-0037 不変条件1（831行） | 写像を撮影やframeの内容で動かさない。撮影ごとの推定とライブの自動位置合わせは診断に限る | #417の原寸の推定（G1・G2）が、既にJPEGの写像を撮影ごとに変えている（設計11.14・11.16）。ADR-0037の決定6を置き換えるADRは未起票（設計11.29のU8）。本ADRは、そこにライブのPを足す | 本ADRは#417の今の作り（G1〜G4）を前提にし、U8のADRより先に例外を書いた。承認された`AGENTS.md`の新しい項が、撮影ごとに原画像で原寸の推定を使う事（"A pair composite uses a trusted estimate on the originals"）と、Pを使う事を先に書いたので、U8のADRは、この項と本ADRを前提に書く |
| ADR-0037 不変条件4（834行） | 撮影ごと（Cではframeごと）に変えてよいのは、継ぎ目に沿った明るさの比と上限以内の倍率（JPEGだけ）だけで、平行移動・回転・継ぎ目の列を動かさない | JPEGの写像の姿勢が、撮影ごとに原寸の推定かPになる（G1・G2は#417で既にそう）。ライブの合成の姿勢が、決定2の3つの時・手の押下・撮影の結果・条件の変化で変わる | 上の行と同じ扱い。frameごとには変えない（不変条件1）。継ぎ目の列は変えない |
| ADR-0035 不変条件6（608行。ADR-0036 決定1で「保存の合成の入力にしない」に絞った。715行） | previewのframeを保存・公開せず、撮影や合成の入力にしない | ライブの映像から推定したPが、JPEGの位置になる | ADR-0036 決定1の5の行と同じ扱い（開発用プレビューの条件v3に限る例外）。frameの画素は保存せず、合成の入力にしない。合成は原画像2枚だけから作る |
| ADR-0037 不変条件2（832行） | 撮影ごとの推定の結果を、写像・条件・profileに書き戻さない。次の撮影に持ち越さない | 撮影の後のPが、次の撮影で原寸の推定が決まらない時のJPEGの位置になる（窓の中の持ち越し） | 窓の中で持ち越すだけの例外にする。条件・profileには書かず、推定の基準にも入れない |
| 設計11.21の決定（`docs/design/fixed-composite-registration.md` 818行）、11.9の「ライブの C」（423行）、11.28の不変条件18（1075行） | Cの推定はJPEGに渡さない | 同上 | 「ライブの確定した位置は、原寸の推定が当たらない時（本ADRの決定5の定義）と手で直した時にJPEGの位置になる。推定の始め・事前分布・範囲の基準にはしない。層2はライブに当てない」に改める。11.21の追従・保つ時間・τ_bakeの記述（820行と836〜844行）は段1で使われなくなったので、同じ変更で今の形に書き直す |
| `docs/WPD_DUAL_PREVIEW.md` 53行・56行 | JPEGの位置は変えていない。位置を合成の写像に書き戻さない | 同上 | 段2の節を足して書き直す |
| `AGENTS.md` 39行 | 自動補正の範囲。上限を超えた結果をclampして合格にしない。rig profileを自動で学習・更新しない | 窓の中のPが、profileや条件の更新ではない事を明文にする | 末尾に1文を足した（下の「AGENTS.mdに入れた文」） |
| `AGENTS.md` 40行 | ボタンの推定は、未校正のCの平行移動の候補だけ | 条件v3の時は、ボタンが姿勢と紙面の推定を1回走らせ、その結果をライブとJPEGに使う | 末尾に1文を足し、後に新しい項を入れた（下の「AGENTS.mdに入れた文」） |
| `AGENTS.md` 34行（`preview-as-stitch-input`）・37行 | ライブをstitchの入力にしない。開発用のCはライブの枠をメモリの中で使う | Pはライブの枠から求めた幾何で、画素ではない | 新しい項に「幾何で、画像でない。合成は原画像2枚だけから作る」と書く。34行・37行は変えない |
| `AGENTS.md` 41行（入れる前の行番号。#334の項） | Cの幾何を写して記録する。ライブからJPEGへの対応は未検証の仮定 | 条件v3では、撮影へ写す物がPになる | 変えない。未検証の仮定である事は、新しい項でも言う |
| 書式の文書の1節の決まり8（`fixed-composite-registration-formats.md` 16行） | mainに入った後の変更は新しい版にする | 報告にPの欄を足す | 決まりのとおりv9にする。決まりは変えない |

変えない決まり: 設計11.19.2の表（推定の始め・事前分布・ゲージ・範囲の基準はp_b）、11.19.3の改訂の種類、ADR-0021の安全境界（自由なhomographyを推定しない、学習しない）、ADR-0036の不変条件4（frameごとに推定しない）。

### AGENTS.mdに入れた文

2026-11-19に所有者が承認し、`AGENTS.md`に入れた文。本ADRに決定を書き入れたcommitとは別のcommitで入れ、`AGENTS.md`のほかの行は変えていない。文は、本節の引用をファイルから取り出してそのまま入れた（入れた後の39〜41行を本節の文と`cmp`で比べ、バイトで一致した。2026-11-19）。行番号は入れる前の`AGENTS.md`（b081023の木）の物で、入れた後は新しい項が41行になる。

40行（入れる前の行番号。以下同じ）の後に足した項:

> - The owner's request in #437 (ADR-0046) narrows the automatic registration of the development composite view drawn with a fixed composite condition v3 to three events: once when Live View first starts in the window (and once when the condition file changes), once per press of its estimate button, and once on the two verified originals after a pair capture. No background, periodic or retried estimate runs, and nothing else moves the live composite. The window keeps one confirmed position of CAM-B relative to CAM-A (its pose and the paper surface, never layer 2) with its source and a change counter (`serial`); the live composite is drawn with it and is re-baked before it changes. A person may move it by bounded in-plane steps (along and across the seam and about the optical axis, in output pixels and degrees) only while the result stays inside the condition's pose envelope measured from the condition's base pose; a step past the envelope is refused, never clamped into success. A pair composite uses a trusted estimate on the originals unless the confirmed position was set by hand; when the estimate is not trusted, or the position was set by hand, it uses the confirmed position (a position that is still the condition's own keeps the existing geometry levels). After the capture the confirmed position becomes the mapping the composite used, without layer 2, while a hand-set position stays. The estimate's start, prior and envelope stay measured from the condition's base pose, never from the confirmed position. Every pair composite records the confirmed position it was given, its source and serial and its seam difference to a trusted estimate, written with the same numbers as the live log. The position lives in memory only and is discarded when the window closes; it changes a fixed composite condition only when the operator explicitly saves it as a new revision. A hand-set or Live-View-derived position is never quality evidence, and the seam gate measures it like any other mapping. It is geometry, not an image: composites are still rendered only from the two verified originals, and the Live View-to-still correspondence it depends on stays unverified (#309).

39行の末尾に、空白1つの後に足した: `The #437 confirmed position of the development preview is neither a rig profile nor a fixed composite condition update.`

40行の末尾に、空白1つの後に足した: `With a fixed composite condition v3 the button follows the #437 entry below instead.`

「次の撮影の前（AFの後）の自動」は所有者に聞いておらず、文は3つの時だけのまま。所有者が足すと決めた時は、本ADRを改め、1文目の「three events」とその列挙を直す（所有者に確かめる事の3）。ボタンの名前（条件v3の時は「自動調整」。所有者の決定）は`AGENTS.md`に書かない（画面の文字の正本はコード）。

#### 本節の文の読み方

承認された文の字面のうち、次の5か所は読み違えの余地がある。本ADRの決定は、次の読み方で書いた。字面を直すのは、次に`AGENTS.md`を改める時（所有者の承認を取り直す時）とし、それまでは本節の読み方で読む。

- "No background, periodic or retried estimate runs, and nothing else moves the live composite.": 自動の推定の話で、「3つの時のほかに、自動の推定でライブの合成を動かさない」の意味。後の文の手の押下と撮影の結果（"After the capture …"）、条件の変化で条件の位置に戻す事（決定10）は、これに含まない。
- "(and once when the condition file changes)": 条件のファイルを見張る事ではない。窓はライブを始める時に条件を読み直し（`wpd_dual_preview_ui.hpp` 483行の`LoadFixedSource`・`PrepareLiveRuntime`）、SHA-256をPの物と比べる（`live_placement.hpp`の`BeginSession`）。今の作りのままで、ライブを動かしている間に変えた条件は、次にライブを始めた時に効く。
- "retried": 結果を出した推定（位置を決められなかった物を含む）を、もう一度走らせない事。推定の途中でライブを止めた時に残した頼みを、再開の時に1回だけ続ける事（所有者の決定5の3）と、画像が揃わず何も推定しなかった頼みを、揃った時に走らせる事（`live_placement.hpp`の`Release`、213〜219行）は含まない。
- "Every pair composite records the confirmed position it was given": 条件v3で作る組の合成（報告v9）の話。窓からPを渡されなかった合成（校正の取り直しの見本の合成）は、`confirmedPosition`をnullで書く。条件が無い時と、v1・v2の条件の時の報告は変えない。
- 40行の末尾の"instead": 条件v3の時は、ボタンが平行移動の候補を出す代わりに、新しい項の推定（姿勢と紙面、押すたびに1回）を走らせる、の意味。手で動かす時も、40行の"manual pixel refinement"ではなく、新しい項の刻み（範囲の内だけ）に従う。40行の禁止（frameごとのhomographyの推定、rig profileの学習、光学の合否の線、最終の校正の承認）は条件v3の時も効く。

### ADR-0045との関係

1. 報告v9で足す`confirmedPosition`は2台の関係を表す値なので、ADR-0045の決定4のとおり`referenceCamera`（`CAM-A`）と`targetCamera`（`CAM-B`）を付け、欄の名前にA・Bの記号を埋めない。v9は2台のままで、`inputs`はv8と同じくちょうど2つ（N台の書式は作らない。ADR-0045の退ける案のとおり）。
2. ADR-0045の「待つ物」と「2台を決め打ちにしている場所」の表は、4台の書式を「報告v9」と書いていた。本ADRが2台のv9を先に使ったので、4台の報告はその次の版になる。段2の文書のcommitで、ADR-0045の3か所（「退ける案」の表、「2台を決め打ちにしている場所」の表、「待つ物」）を「v9の次の版」に直した。ADR-0045の未決の1が引く書式の決まり8の「（v4・v9）」は、決まりの文の引用なので変えていない。
3. ログの`reference_camera=CAM-A target_camera=CAM-B`は、ADR-0045（Proposed）では待つ物（4台が確定してから`LivePlacementFields`に足す）。本ADRでは、段2でv9と同じcommitに入れ、ライブの行と撮影の側の行の両方を書く1つの関数に足す（決定7、技術の側で決めた事の7）。理由: 撮影の側の行は、段2で読み直した報告v9から新しく書く行で、v9の`confirmedPosition`は`referenceCamera`・`targetCamera`を持つ。新しく書く行に同じ欄が無ければ、ログと報告を同じ欄で突き合わせられない。値は2台のまま（`CAM-A`・`CAM-B`）で、欄の名前はADR-0045の決定4の形（snake_case）なので、4台の書式（継ぎ目ごとの組）はADR-0045の待つ物のまま残る。
4. 継ぎ目はv8と同じ`seam.column`で、ADR-0045の決定5の読み方では縦の継ぎ目CAM-A–CAM-B。差もこの列に沿って測る。継ぎ目の新しい欄は足さない。
5. 文の書き方: 本ADRは、合成を「ライブの合成」「JPEGの合成」と言葉で書き、カメラを`CAM-A`・`CAM-B`と書いた（ADR-0045の決定6、今やる物の4）。コード・ログ・画面の文字を引く所と、ほかのADRの文を要約した所だけ「C」のまま（`C live_placement`など）。

### 段の切り分け（段2）

着手の前の承認は済んだ。本ADRと上の`AGENTS.md`の文は、2026-11-19に所有者が承認した。`AGENTS.md`には、承認された文を、本ADRに決定を書き入れたcommitとは別のcommitで入れた。段2の実装は、次の順にcommitを分けて行う。大きさは※推定。段1は、設計の時の見積り約1,000行に対し、追加2,030行・削除868行だった（2026-11-19に`git diff --shortstat 6738bd1^1 6738bd1`で数えた。12ファイル）。

| # | 中身 | 触るファイル（※案） | 試験の計画の番号 | 大きさ（※推定） |
|---|---|---|---|---|
| 1 | 純粋な部品: 数の書き方を1つにする（報告とログ）。手の式（紙面つき）。継ぎ目での差の測り方 | m2の新しいheader（例`confirmed_position.hpp`）、`capture_registration.cpp`の`Num`、`live_placement.hpp`の`LivePlacementNumber`、`fixed_composite_tool_v3.hpp`の`SolveManualChange`（新しい式を呼ぶ。値は変えない） | 5 | 400〜700行 |
| 2 | JPEGの側: 撮影の要求のPの欄、段`confirmed`と出どころごとの候補の順（決定5）、報告v9の書き手と読み手、警告の符号と文、v9の組み立て、ログの基準と対象のカメラの欄（決定7）。書式の決まり1により1つのcommit | `still_review.hpp`、`capture_registration.hpp`・`.cpp`、`still_review_report_impl.hpp`、`uncalibrated_review_impl.hpp`、`pair_review_registration.hpp`、`fixed_composite_registration_text.hpp`、`live_placement.hpp`の行を書く関数、`tests/registration_fixtures.hpp`、書式の文書 | 3・7・8 | 1,500〜2,200行 |
| 3 | 機械: 手の出どころ、手の押下（刻みの整数の数、範囲と安い検査、描いている時だけ、頼みの取り消し、続けて押した時、合計が0に戻った時、条件の位置に戻った時）、ライブの推定の結果の安い検査、再開の時にPを先に焼く順、Pで描けているかの判定、撮影の間の扱い（決定9）、撮影の結果が`confirmed`とG4の時の扱いと`serial` | `live_placement.hpp`、`live_registration_runtime.hpp` | 2・3・5・6・7・10・11 | 500〜900行 |
| 4 | 窓: 手のボタン、撮影へPを渡す口、Pで描けていない間の両台撮影を断る判定と案内（決定11）、状態の文、ログの行 | 新しい`live_placement_controls_ui.hpp`（`wpd_dual_preview_ui.hpp`へは数か所の呼び出しだけ）、`live_registration_display.hpp`、`preview_button_layout.hpp`を使うボタンの組の一覧 | 1・4・6・7・9・10・11 | 900〜1,400行 |
| 5 | 文書: 設計11.9・11.21・11.28、`WPD_DUAL_PREVIEW.md`の段2の節、本ADRの状態（ADR-0035・0036・0037の追記は2026-11-19に置いた） | docsだけ | | 100〜200行 |

合わせて約3,400〜5,400行（※推定）。

段3に残す物:

- 「この位置を保存」。Pが条件の位置に手の量だけを足した物なら`manual-adjustment`、直前の撮影がG1・G2（推定が当たった）で基準内なら`saved-estimate`の改訂にする。自動の紙面を含むPは、改訂が紙面を持たないのでそのまま保存できず、画面でそう言う。
- `saved-estimate`の元にv9の報告を読む読み手。
- 撮影の後の原寸の継ぎ目の確認画面への入口（#418）と、手で直した位置で撮った合成の作り直し（#418の段2）。

ほかの作業との重なり: #434の記録の欄の変更（基準と対象のカメラの欄）は、本ADRの直しより先に手元のmainに入った。段2の文書のcommit（上の表の5）が触る`docs/WPD_DUAL_PREVIEW.md`と重なる所は、その変更の後の形に合わせる。本ADRの`docs/DECISIONS.md`の変更（本ADRの節と、ADR-0035・0036・0037の追記）は、その変更が触ったADR-0045の節を触らない。本ADRが引くほかのファイルの行のうち、その変更でずれた物は関数の名前で指した。#434の`tests/registration_fixtures.hpp`の変更（改訂の種類`recalibrated`）は前からmainにある。校正の取り直しの見本の合成（`src/phase0/include/a0/phase0/experimental/recalibration_job.hpp`の、見本の工程`RecalibrationStage::sample`で`ComposePairWithSelection`を呼ぶ所）は窓のPを受け取らないので、v9では`confirmedPosition`をnullで書く。

### 所有者に確かめる事

2026-11-19に、所有者が1・2・5を決め、本ADRと`AGENTS.md`の文を承認した。3は聞いていない。4は個別には聞いていないが、承認に含まれる。表の「推し」は、所有者に示した時のまま残した。同じ日の再レビューの後に、所有者は、条件が変わった時の手の位置、合計が0に戻った時、Pで描けていない間の両台撮影の3つも決めた（「ADR-0046の所有者の要件と決定」の6。この表の外）。

| # | 確かめる事 | 選択肢 | 推し | 所有者の決定 |
|---|---|---|---|---|
| 1 | ボタンの名前 | (a) 今の「自動位置合わせ」のまま。(b) 条件v3の時だけ「自動調整」にする。条件が無い時の未校正の合成のボタンは働きが違う（平行移動の候補を出す）ので「自動位置合わせ」のまま | (b)。所有者の言葉が「自動調整ボタン」で、撮影の後の文も既に「撮影の後の自動調整で…」と言っている。手のボタン（左右・上下・回り）と並んだ時に、「自動」と「手」の対で読める | (b)。条件v3の時は「自動調整」、条件が無い時の未校正の合成のボタンは「自動位置合わせ」のまま（決定2） |
| 2 | 推定の途中でライブを止めた時 | (a) 頼みを残す。再開の時に、その1回だけ続きを行う（段1の今の作り）。(b) 捨てる。再開の後は、押すまで位置を変えない | (a)。押した1回や開始時の1回が、結果を出さずに消えない。自動が走る時を新しく増やさない。再開の後は状態の行が「自動合成中です…」と出し、その間に手で動かせば、その推定は捨てる | (a)。段1の今の作りのまま（決定2） |
| 3 | 次の撮影の前（AFの後）に自動を1回足すか | (a) 足さない。AFの後の状態の行で「紙を替えた時、ピントを合わせ直した時は、ボタンを押してください」と案内する。(b) 足す。両台のAFが応答した後に1回走らせ、手で直した位置の時は測るだけで動かさない。(c) 足し、手の位置も動かす | (a)。AFはカメラごとに押すので、2回走らせるか、両方のAFを待つ時間の決まりが要る（時間で動く決まりは不変条件1と合わない）。手で直した位置とぶつかる（(c)は所有者の決定1の考えと合わない）。AFの直後のライブの枠はレンズが動いて揺れ、推定が決まりにくい（※推定）。シャッターの後に原寸の推定が毎回走るので、JPEGに効くのは原寸の推定が決まらない撮影の時だけ。(b)の利点は、撮る前のライブの合成が今の紙とピントに合う事と、原寸の推定が決まらない時のJPEGの位置が新しくなる事。(b)を選ぶ時は`AGENTS.md`の文の「three events」を4つに直す | 聞いていない（※要確認）。承認された文は自動が走る時を3つに限るので、自動の1回は足さない。(b)・(c)は候補として残す。所有者が足すと決めた時は本ADRを改める（その時は`AGENTS.md`の文の「three events」とその列挙も直す）。(a)の案内の文は決めていない |
| 4 | 手で直す量を、条件の範囲の端で断ってよいか | (a) 断る（決定4）。(b) 範囲の外も警告つきで当てる | (a)。所有者の決定9（手直しの量に上限を置かない）は量の上限の話で、範囲の外の手直しは撮影とライブの合成が使わない（設計11.29のN1）。範囲の外が要るのは設置が動いた時で、校正の取り直し（#434）で直す | (a)。個別には聞いていないが、承認された決定4と`AGENTS.md`の文（`a step past the envelope is refused, never clamped into success`）に含まれる |
| 5 | 手の刻み | (a) 左右・上下が12 px（ライブの約1画素）と4 px、回りが0.2°と0.05°（決定3の案）。(b) これに1 pxを足す | (a)。12 pxはライブで1画素動いて見える。4 pxはライブの約0.35画素で、補間で継ぎ目の見え方が少し変わる程度（※推定）。1 pxはライブの0.09画素で、画面の上ではほぼ変わらないので、原寸の継ぎ目の確認画面（#418）から使えるようにしてから足す | (a)。1 pxは原寸の継ぎ目の確認画面ができてから足す（決定3） |

技術の側で決める問い: 段2の実装で残りはなくなった（下の「決まった物」）。

決まった物: #417のADR（設計11.29のU8）との順は、`AGENTS.md`の新しい項が先に例外を書いたので本ADRが先（「変わる決まり」）。範囲の外のPからの押下と、手の押下の安い検査は決定4に書いた（技術の側で決めた事の4）。ライブの表がまだPで焼かれていない間の両台撮影は、所有者が決めた（所有者の決定6の3、決定11）。段2の実装で次の5つを決めた（書式の文書4.13.2・4.13.4、`docs/WPD_DUAL_PREVIEW.md`の#437段2の節）。報告の読み手は`seamDifferencePx`を作り直さず（作り直すにはprofileの写像が要る）、記録の値として、有限で0以上か、条件が同じで推定が確かな時だけ数があるかを確かめる。警告の符号は`position-by-hand`・`position-from-live`・`estimate-beyond-envelope`（`unverified`）、成分の符号は`confirmed-position`にした。StillReviewがv9の報告を開いた時は、v7・v8と同じく手の再Mergeを使えなくした。撮影の後の文と状態の行の文は、決定3・9〜11と「保証しない事」に引いた形にした（撮影の側は`fixed_composite_registration_text.hpp`の`ConfirmedPositionText`、ライブの側は`live_registration_display.hpp`）。安い検査は`CheckRegisteredMappingCover`の1つにまとめた。手の押下・ライブの推定の結果・撮影の候補のどれもこれを呼び、画素を読まず、枠を確保しない（「今の作り」に書いた、撮影の時の関数が原寸の原画像で自分の数え方を使う作りは、段2でなくした）。

### 影響と改定の対象

| 対象 | 扱い |
|---|---|
| `docs/DECISIONS.md` | 本ADRを足した（Proposed）。2026-11-19に所有者の決定を書き入れ、Acceptedにした。同じ日に、技術の側で決めた事を足し、決定3〜5・7・9・10、試験の計画、不変条件を直した（所有者の決定は変えていない）。同じ日の再レビューの後に、所有者の決定6を書き入れ、決定2〜7・9・10を直して決定11を足し、技術の側で決めた事・保証・試験の計画・不変条件を直した。ほかのADRには追記だけを置き（次の行）、本文は変えない |
| コード・試験・書式 | 本ADRのcommitでは変えない。段2のcommitで変える |
| `AGENTS.md` | 本ADRのcommitでは変えない。承認された文を、別のcommitで入れた（「AGENTS.mdに入れた文」）。ほかの行は変えていない |
| ADR-0036 決定1の5・10、ADR-0037 決定1・2、不変条件1・2・4、ADR-0035 不変条件6 | 本ADRが採用されたので、2026-11-19に各ADRへ「ADR-0046で改定」の追記を置いた（本文は書き換えていない）。ADR-0035は箇条の最後の追記の列、ADR-0036は決定1の10の後、ADR-0037は決定の6の後と守る不変条件の9の後。本文の書き換えは、各ADRの承認後の作業と一緒に後の変更で行い、それまでは追記と本ADRが優先する |
| 設計11.9・11.21・11.27・11.28の18、`docs/WPD_DUAL_PREVIEW.md`の#437の節、`docs/WPD_STILL_REVIEW.md`の条件v3の保存の流れ | 段2の文書のcommitで書き直した |
| `docs/design/fixed-composite-registration-formats.md` | 段2のcommit 2でv9の節（4.13）を足した。段2の文書のcommitで、5.1の表の`estimate-beyond-envelope`の行と、中身が決まった予約の欄を直した |
| ADR-0045の「報告v9」の文 | 段2の文書のcommitで、3か所を「v9の次の版」に直した（「ADR-0045との関係」の2） |

### 未決（※要確認）

1. 手のボタンのIDは、段2の実装で320〜326に決めた（320 左、321 右、322 上、323 下、324 反時計回り、325 時計回り、326 刻みの切り替え。未校正の合成のボタン300〜304とは別の範囲）。幅と並べ方は`preview_button_layout.hpp`の`LowerButtonItems`。刻みの値は所有者が決めた（決定3）。
2. 実機で見る事（段2の実装の後、所有者の承認を得て行う）: Pを固定して撮り、ライブの継ぎ目のずれ（ライブの画素 × 11.5）とJPEGのゲートの最大を比べ、「ライブで合って見える時のJPEGのずれは約6 px」の見積りを確かめる。同じ撮影で、ライブの推定と原寸の推定の継ぎ目での差を見ると、Sの外れも一緒に見える。
3. Pythonは位置の経路に入れない。解析でPythonを使う時は、ADR-0043と#429の決まり（定数の正本を1か所、共通の試験ベクトル、丸めの決め）を位置にも当て、Pythonが出した値をPや条件に直接入れない。

## ADR-0050: いつもの撮影用アプリ（OperatorShell）を、開発用プレビュー（WpdDualPreview）と同じ仕組みにする

- 状態: Proposed（下書き。所有者は向きに「はい」と答えた。本文と`AGENTS.md`に入れる文は、所有者に見せてから入れる）。本ADRの変更は文書だけで、コード・試験・書式は変えない。いつもの撮影用アプリの画面は、本ADRのAcceptedの後に段ごとに載せる。
- 決定日: 未決定
- 関連: #351（ロードマップと所有者の決定）、#29〜#34（撮影ダッシュボードの合意済みの配置）、#437（ADR-0046、確定した位置）、#434（ワンボタン校正）、#440（水準器）、#385（継ぎ目のゲート）、#293（リリースの条件D1）、#309（ライブの枠と撮影画像の対応S）、ADR-0033（4節）、ADR-0034（不変条件1）、ADR-0036（決定1の1・4・7・9、決定2の改訂案）、ADR-0037（不変条件9）、ADR-0039、ADR-0044、ADR-0045、ADR-0046、`docs/OPERATOR_UI_SPEC.md`、`docs/WPD_DUAL_PREVIEW.md`、AGENTS.md
- 背景: 開発用プレビュー（`src/phase0/experimental/wpd_dual_preview_*`）では、2台の映像、つないだ映像（C）、自動調整、手で位置を動かすボタン、ワンボタン校正（既定OFF）、水準器が動いている。いつもの撮影用アプリ（`src/m3/OperatorShell`）は、SDKで選んだ1台のライブ（ADR-0035）とCaptureRecoveryOnlyの撮影（ADR-0027）のままで、これらは1つも載っていない。載せるのを止めているのは次の決まり。2台のライブと撮影はADR-0036決定2（保留）とADR-0039（下書き）。合成を操作者の画面に出す事は、ADR-0036決定1の1・7（承認済みのprofileだけで描き、無い間は出さない）、ADR-0033の4節（製品の合成はapprovedだけ、draftは別のexe）、AGENTS.mdの#334の行（承認済みのprofileとproduct-successのStitchJobを作らない）。自動調整と位置のボタンは、ADR-0046とAGENTS.mdの#437の行が開発用プレビューに限っている。水準器はAGENTS.mdの#342の行（開発用）。計算の置き場所は、ADR-0036決定1の9がDLLをWPFから呼ぶ案を採っている。

### ADR-0050の所有者の要件と決定（#351）

1. 所有者がはじめから求めている事（所有者の言葉）:
   > ・合成が正しくできること（映像マージでずらしたら、画像マージもその分ずれるという点も含む）
   > ・UI上で正しく動作すること
   > ・水準器が現状あまり正しくないので、カメラ内部の水準器を正しく機能させ、カメラがどのくらい傾いているのか視覚的にわかるようにすること

   「UI」は、いつもの撮影用アプリの画面を指す。水準器で満たす線は「どちらに傾いてるかと今並行だよ　が出せれば満足です」。
2. 問い「いつもの撮影用アプリを、開発用の画面と同じ仕組みにしてよいですか。」への回答: 「はい　そのまま進めて　UI案は確か渡してるはずだよね？」。問いの中身（所有者に見せた文のまま）: 「開発用の画面で動いている 2 台の映像・つないだ映像・自動調整・位置のボタン・ワンボタン校正・水準器を、同じ部品のまま、いつもの撮影用アプリに載せる。正式な校正の前でも合成は「未確認」と数字付きで出す。変えない物: 約 0.11 mm の線、止める線 3.5°・3.5 cm・3 mm、一台での撮影。決まりの英文は入れる前に見せる。」（止める線の3つは、カメラの傾き3.5°、図面の下が削られる幅3.5 cm、右の端の食い違い3 mm。約0.11 mmの線は#385。）
3. 調整が入る場所（所有者の言葉）: 「ワンボタン調整→（ここではもう自動で調整されない）→撮影準備　のフローだからね。　映像や画像が常に動いちゃったら意味がないから、ある程度決まった場所で調整が入らないとだめだと思う。」
4. 載せる画面: 所有者の画面の案は#29に記録した撮影ダッシュボード（左に大きなステージ、右に拡大・フォーカス・アクション、上にタイトルバーとメニューバー）で、#29〜#34で実装済み。仕様は`docs/OPERATOR_UI_SPEC.md`の「画面構成」。

撮影の時に写真の合成を動かさず、ボタンで決めた位置（ライブで見ていた位置）のまま作る事（#351に記録した所有者の回答）は、別の変更でADR-0046とAGENTS.mdを改める。本ADRはその決まりをいつもの撮影用アプリでもそのまま使う。

### 決定

1. **載せる先**: いつもの撮影用アプリの撮影ダッシュボード（#29〜#34、`docs/OPERATOR_UI_SPEC.md`の「画面構成」）に載せる。新しい見た目は起こさない。ステージの「合成プレビュー」がC、「CAM-A live」「CAM-B live」がカメラごとの表示（ADR-0036決定1の6の技術担当者向けの表示）になる。開発用プレビューで正しく動いた物から、段ごとに載せる（下の「載せる順」）。
2. **同じ部品で作る**（ADR-0036決定1の9を改める）: 2台の映像、C、確定した位置、自動調整、手のボタン、ワンボタン校正、水準器、画面に出す文と数は、開発用プレビューと同じC++の部品を、SDKを使わない新しいCamera Agentの中で動かす。WPFは表示と操作の受け渡しだけを行い、合成・位置・判定・画面の文をC#で書き直さない。SDKを使う今のCamera Agentを広げず、1つのプロセスでSDKとWPDを混ぜない。映像と写真（JPEG）の一致の試験（ADR-0046の試験の計画）は、いつもの撮影用アプリの経路でも同じ試験を通す。一致の試験が無い変更はmainに入れない。
3. **2台の映像**: ADR-0036決定2（改訂案）の例外で、両方のカメラを動く映像で出す。撮影はADR-0039のWPD経由の撮影で行う。どちらもAcceptedになるまでは、今のSDKの1台のライブとCaptureRecoveryOnlyの撮影のまま。
4. **正式な校正の前の合成**（ADR-0036決定1の1・4・7、ADR-0033の4節、AGENTS.mdの#334の行を改める）: 承認済みのrig profileが無い間も、固定の合成条件（未承認、#399）で描いたCと、撮影の後のJPEGの合成を、いつもの撮影用アプリの画面に出してよい。条件は次のとおり。
   - 画面と記録にいつも「未確認」と出す。承認済みのprofileではない事と、ライブの枠と撮影画像の対応（S、#309）が確かめられていない事を伝える。
   - 数を付けて出す。数は今ある測り（継ぎ目の差、#385のゲートの値、カメラの傾き）から出し、どの数をどこに出すかは段2の設計で決める（※要確認）。届いていない線は「届いていない」と出し、黙って合っているように見せない。
   - 合格・受入の数に入れない。承認済みのprofile、product manifest（ADR-0026）、product-successのStitchJob、`HG-0001`の証拠に使わない。製品のM2Adapterの「approvedのprofileだけを受け付ける」拒否（ADR-0033の4節）は変えず、未確認の合成はCamera Agentの中の開発用と同じ部品で作り、未校正の記録の書式（ADR-0046の報告）で残す。
   - 固定の合成条件で描いた合成には、いつも「未確認」と出す。「未確認」の表示を外せるのは、製品の関門が受け付ける承認済みのrig profileで描いた合成だけで、ADR-0036決定2の5の確かめ（#309。ライブの枠が撮影画像の範囲を拡縮と平行移動だけで写すこと）が済んだ後に限る。承認済みのprofileがあるだけでは外れない。ファイル・環境変数・レジストリの値・コマンドラインのoption・UIの自動操作では外せない。
   - #385のゲートの線と、線を超えた撮影を書き出さない決まり（ADR-0037の不変条件9）は変えない。線の内側の未確認の合成を書き出してよいかは、その段で所有者が決める。決まるまでは書き出さず、未確認の合成はprivateな未校正の記録（ADR-0046の報告の書式）にだけ残す。
   - Cは表示専用のまま（ADR-0036決定1の5）。Cの画素を保存せず、撮影・保存の合成の入力にしない。
   - 製品のbuildに入れる時期と、既定OFFのoption（ADR-0033の4節、ADR-0036決定1の2の副項）の扱いは、段1の設計で決める（※要確認）。buildに入っても、#293のD1（合成ができるまでリリースしない）とD2（配布は社内のPCだけ）は変えない。
5. **自動調整と位置のボタン**（ADR-0046とAGENTS.mdの#437の行の範囲を広げる）: ADR-0046の確定した位置、自動調整（窓で最初にライブを始めた時と、ボタンを押した時）、手の刻みのボタンを、いつもの撮影用アプリでも同じ決まりで使う。自動の調整は決めた時だけ走り、裏の追従・時間での戻り・再試行はしない。流れは「ワンボタン調整 → （ここでは自動で調整しない）→ 撮影準備」。撮影の時の扱いは、#437の別の変更で決まった形に従う。
6. **ワンボタン校正**: 開発用プレビューと同じjobと同じ範囲の決まり（#434。ADR-0048の案、mainに未登録）で、いつもの撮影用アプリにも置く。範囲の外では作り直さず、戻す向きと量を出す。開発用の決まりは校正の側で先に決め、実機で初めて押す前に所有者に声をかける。いつもの撮影用アプリで使い始めるのは、その決まりがAcceptedになった後。
7. **水準器**（AGENTS.mdの#342の行を製品にも広げる）: 開発用プレビューと同じ部品で、どちらへ傾いているかと、今は平行かを出す。値はライブのframeのheaderから読み、命令を足さない（ADR-0036決定2の1-b）。読み方と見せ方は#440の決まり（AGENTS.mdの#342の行を改める変更）に従う。その変更がmainに入るまで、いつもの撮影用アプリは水準器の状態（向きと平行）を出さない。撮影してよいかの判定に使うかは本ADRでは決めない（※要確認）。
8. **変えない物**: 約0.11 mmの線（#385）の数。止める線（カメラの傾き3.5°、図面の下が削られる幅3.5 cm、右の端の食い違い3 mm。所有者の決定）。線を超えたら黙って補正せず、知らせて止める。一台での撮影（`SingleCamera`。ADR-0023・0024、ADR-0039の決定1）。受入の回数と数え方（ADR-0030・0032）。いつもの撮影用アプリの受入は、開発用の確認と別の系列にし、撮影の段の前に所有者に聞く。シャッターの同期を保証しない（`CON-002`）。
9. **入れない物**: AF枠の移動（`0x9205`、AGENTS.mdの#426の行）、近遠の微調整（`0x9204`）、ライブを開いたまま撮る探り（ADR-0044）、カメラの設定の書き込み（ADR-0038・0041。段Cは別に決める）、色の推定（ADR-0043）。どれも開発用プレビューの決まりのまま。AF（`0x90C1`）は、ADR-0036決定2の2-aのgateの案1・案2のどちらかを所有者が選び、そのgateがsecurityのレビューを通るまで、いつもの撮影用アプリでは無効の表示と理由のまま（2-aの最後の項と同じ条件）。使い始めた後も2-aの条件に従う。
10. **カメラの数**: 部品はカメラの一覧から作り、2台に固定した書き方を新しい部品・欄に増やさない（ADR-0045）。
11. **入口と記録**（AGENTS.mdの#426・ADR-0044の行と同じ扱い。#416段Bの行にある、所有者が1回だけ許した操作を自動で押してよい例外は持ち込まない）: いつもの撮影用アプリのカメラの操作（実機にsessionを開く操作と実機に何かを送る操作のすべて。ライブの開始と停止、AF、撮影など。あわせて自動調整、位置のボタン、ワンボタン校正）に、コマンドラインと自動の入口を作らない。新しいCamera Agentは、開発用プレビューの診断用のコマンドラインのmodeを持たない。担当は、実機につながった状態でこれらをUIの自動操作・合成した窓メッセージ・スクリプトで押さない。合成した入力を送るのは、カメラを使わない試験だけ。ログ・画面の文・合成の記録は、カメラをaliasだけで呼び、実カメラの識別子とWPD serial digestを出さない（ADR-0039の不変条件8）。

### 載せる順

| 段 | いつもの撮影用アプリに増える物 | 前提 |
|---|---|---|
| 下ごしらえ | 増えない（開発用プレビューの流れを窓から切り離し、同じ進行役を呼ぶ形にする） | 本ADRと関係なく進めてよい |
| 1 | 2台とも動く映像 | ADR-0036決定2がAccepted。実機は所有者に声をかけてから |
| 2 | 「未確認」と数付きのC、自動調整と位置のボタン、水準器の向きと平行 | 開発用プレビューで、cmのずれが消えた合成を所有者が見た後。水準器は#440の変更がmainに入った後（決定7） |
| 3 | 撮影と保存。手で動かした分が写真の合成にも出る | ADR-0039がAccepted。受入の計画を所有者が承認 |
| 4 | ワンボタン校正 | 校正の決まりがAccepted、校正の枝がmainに入っている |

各段の`docs/OPERATOR_UI_SPEC.md`の書き換えは、その段の変更で行う。

実機で使う時は、担当が始める回ごとに、また誰が始めるかによらず各段を実機で初めて使う時に、機体・回数・止める条件を挙げて所有者の合図を受ける。ライブと撮影の製品の受入は、これとは別に、ADR-0036決定2の4とADR-0039の「製品の受入」の承認を受ける。

### #29の設計原則のうち置き換わる文

| #29の文 | 置き換える決定 | 置き換えた後 |
|---|---|---|
| 一台選択式 Live View（二台同時LVは対象外） | #351の所有者の決定（本番でもWPD、2台同時のライブ）、ADR-0036決定2の改訂案、本ADRの決定3 | 2台とも動く映像を出す。CAM-A・CAM-Bの表示は技術担当者向けに残る |
| LV切替は手動のみ。自動交互切替は実装しない | 置き換えない | 両方が動くので切り替えは表示の選択だけになる。自動の交互表示はしない |
| LVフレームは原画像・合成の入力に使用しない | ADR-0036決定1の5（Cは表示専用）、ADR-0046（ライブで決めた位置を写真の合成に使う）、本ADRの決定4・5 | ライブの画素は今までどおり原画像・合成の入力にしない。ライブで決めた位置（画素ではなく位置の値）は写真の合成に使う |
| 「プレビュー表示のみ・原画像／合成には不使用」を常設表示 | 同上 | 文を直す（ライブの画素は使わず、位置だけを使う事が分かる文。文は段2で決める。※要確認） |
| ステージ「合成プレビュー」= ライブ側=動画、非ライブ側=最終フレームの静止画 | 本ADRの決定1・3 | 両側とも動く映像のC。「未確認」と数を付ける |
| fail-closed／自動リトライ0／`OperatorActionAvailability`による操作可否の一元管理 | 置き換えない | そのまま。自動調整・位置のボタン・ワンボタン校正・水準器の操作も同じ一元管理に入れる |
| 配色テーマ、SIMULATEDバッジ等の既存表示契約 | 置き換えない | そのまま |

### 影響と改定の対象

本ADRがProposedの間は、どの対象にも追記を置かない（※案）。承認の時に、下の対象に「ADR-0050で改定」の追記を置き、本文の書き換えは段ごとの変更で行う。

| 対象 | 今の文の要旨 | 改定の方向 |
|---|---|---|
| ADR-0036決定1の1・7 | 製品のCは承認済みのprofileだけで描き、無い間は操作者の画面に出さない | 決定4の条件で、承認済みのprofileの前でも「未確認」と数付きで出す |
| ADR-0036決定1の4 | Sを確かめた後でなければ「保存される合成と同じ見た目」と言わない。確かめられなければCを出さない | 「同じ見た目」とは言わないまま、確かめる前は「未確認」と出して表示する |
| ADR-0036決定1の9 | 計算は小さなDLLにしてWPFから呼ぶ | 決定2のとおり、計算はCamera Agentの中、WPFは表示だけ |
| ADR-0033の4節 | 製品の合成はapprovedだけ。評価専用の合成は製品と別のexe・既定OFFのoption | M2Adapterの拒否は変えない。未確認の合成をCamera Agentの中で作る事を例外に足す。buildの扱いは段1で決める |
| ADR-0034の不変条件1 | 製品の合成はapprovedのprofileだけ | 未確認の合成は製品の合成（product manifest）に数えない、と読む（※読み方） |
| ADR-0046 | 開発用プレビューの固定の合成条件v3に限る | いつもの撮影用アプリにも広げる |
| #293のD1 | 二台は評価経路のまま、製品の位置づけを変えるADRは作らない | 後半は#351の決定で置き換わったと記録する（ADR-0039）。「合成ができるまでリリースしない」は残す |
| `FR-UI-003`、`FR-LV-001`、`FR-LV-003` | 一台選択式のライブ、SDK経由 | ADR-0036決定2の改訂案の表と合わせて直す |
| `docs/OPERATOR_UI_SPEC.md` | 一台選択式、実機のフォーカス無効、承認済みのprofileが無い間はCを出さない、ほか | 段ごとに書き換える。書き換わる所の一覧は本ADRの承認の時に#351の子のIssueに置く |
| AGENTS.md | 開発用に限った行（#331〜#342、#437ほか） | 新しい項を1つ足し、関わる行の末尾に参照を足す（文の案は別のcommit） |

### 範囲外と未決

- 出す数の種類と置き場所（決定4、段2の設計）。
- 線の内側の未確認の合成を書き出してよいか（決定4。その段で所有者が決める。決まるまでは書き出さない）。
- 製品のbuildにCamera Agentと未確認の合成を入れる時期とoption（決定4）。
- 水準器を撮影してよいかの判定に使うか（決定7）。
- 「非原画像・非合成入力」の常設の文の直し方（#29の表）。
- いつもの撮影用アプリの受入の系列（撮影の段の前に所有者に聞く）。
- カメラごとの映像の置き場所（技術担当者向けの表示の置き場所、ADR-0036決定1の6。段1の前に写しを見せて聞く）。

### ADR-0050で所有者に承認していただく点

1. 本ADRの本文と、`AGENTS.md`に入れる文（別のcommit）。向きは所有者が「はい」と答えた（#351）。
2. #293のD1の後半を置き換わったと読む事（ADR-0039）。

### 承認後の作業

- 状態をAcceptedにし、決定日を記入する。上の対象に追記を置く。`AGENTS.md`の文を入れる。
- ADR-0036決定2の改訂案とADR-0039を、同じ時か先にAcceptedにする（段1・段3の前提）。
- 段ごとのIssueを#351の子として立てる（文面は所有者に見せてから）。
