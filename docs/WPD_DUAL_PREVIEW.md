# WPD A/B 同時ライブビュー開発用表示 (#330)

開発時の本人指示「いちいち止めないで良いので、2台同時ライブビューまで走りきってください」を本工程の実機実施承認とする。既存のA/B登録を使い、同一PC上のD810二台に独立したWPD接続を持つ開発用プレビューに限定する。従来の一台Live制限およびWPD session上限はこの工程だけ二台へ拡張する。ADR-0036全体の承認、映像Cの合成、製品への統合、撮影、設定書き込み、カード操作を意味しない。Nikon SDKは使わない。

SDKなしの既存buildに `-DA0_BUILD_WPD_DUAL_PREVIEW=ON` を指定し、`cmake --build build/reference --config Release --target A0CameraStitcher.WpdDualPreview --parallel 1` でビルドする。BUILD_TESTINGも必要。既定はOFF。

実行例: `build/reference/Release/A0CameraStitcher.WpdDualPreview.exe --seconds 60`。`--seconds 0` はウィンドウを閉じるまで表示する。Escまたは閉じる操作で両側停止処理を始める。描画は各側の最新フレームだけ、2秒以上更新が途切れた映像は非表示とする。

再起動用に `powershell -ExecutionPolicy Bypass -File scripts/Start-WpdDualLivePreview.ps1` も用意した（既定は閉じるまで表示）。取り付けの向きは `-MountOrientation normal|rotated-180` で選べる（[WPD_PREVIEW_MERGE.md](WPD_PREVIEW_MERGE.md) の #361 の節）。実カメラ制御アプリを同時起動しないこと。

operator-session-wide HardwareProcessLeaseを列挙開始前から両workerの終了まで保持する。列挙順でA/Bを割り当てず、既存のWPD serial digest登録に照合し、実接続後にも再確認する。workerごとにCOM MTA / IPortableDeviceFTM / WIC decoderを持つ。WPD送信直前にStart(0x9201)/Frame(0x9203)/Stop(0x9202)およびREAD/ENDだけを許可し、取得は#329の上限・最終MTP応答検証を使う。D810の384byteヘッダー後のJPEGを厳密に検査し、WICで最大2048x2048へ制限してメモリ内で復号する。実画像ファイルは書かない。

一方が失敗したら両方を止め、自動再接続しない。Startの結果が不明でもStopを一度だけ試み、Closeを確認する。同期SendCommandの12秒停滞時は別threadからCancelし、workerが閉じるまでleaseを解放しない。WPDのOpen/identity取得とCancel自体が返らないドライバー障害には強制killで安全を偽らず、プロセスがleaseを保持したまま残る限界がある。captureへの切り替え機能は含まない。

匿名ログに取得数、実際に描画した更新数、画像内容が変化した回数、fps、両映像取得期間の重なり、Start/Stop/Closeの成否を残す。単なる転送成功や静止画像表示を二台ライブ成功と扱わない。実機結果は開発状況記録へ追記する。

開始前に読み取り専用GetDevicePropValue(0x1015)でLiveViewStatus(0xD1A2)およびLiveViewProhibitCondition(0xD1A4)だけを診断する例外を送信allowlistへ追加。任意のproperty読み取りや書き込みは許可しない。両台のidentityと事前状態確認が済むまで、どちらのStartも送らない。0x20は公開MIT実装のbit 5（最小絞り警告）に対応する。初回実機ではBがこの理由で拒否し、本人による絞りリング調整後に解除された。ニコン公式の[fEE案内](https://nij.nikon.com/support/faq/products/article?articleNo=000059982)も参照。

開発時、30秒実機run 3合格: A=755取得 / 456描画 / 約25.80fps、B=747取得 / 458描画 / 約25.56fps、同時取得期間29.190秒。両側Start/Stopの最終MTP=0x2001、両側Close成功、exit 0。A/B両方で内容変化も観測した。取得画像は保存せず、匿名ログはignored build/reference配下だけに保存。これは開発用ネイティブpreviewの達成であり、既存OperatorShell製品UIへの統合や映像Cの合成は未実装。

最終ビルドの15秒run 4も合格（A358/B356取得、A223/B220描画、同時期間14.151秒、Stop/Close成功）。Release/Debugの3/3関連CTestも合格。自動計測はGDI描画更新と転送・復号成功を証明し、画面の目視受入や長時間耐久を代替しない。

## 開発時 次工程の追加

所有者の次の依頼に基づき、この初期表示に [A/B撮影・PC保存](WPD_STILL_CAPTURE.md) と [未校正C・自動候補・手動微調整](WPD_PREVIEW_MERGE.md) を追加した。上記の初期版の「撮影/Cなし」という制限は履歴であり、追加した開発用経路の範囲は各文書に従う。通常起動で自動撮影/自動フォーカスは行わない。最終製品の承認profileゲートは変更しない。正常停止は送信済みの一枚を元のdeadline内でdrainし、新規frameを開始しない。最新実機結果・未達は開発状況記録を参照。

### #337 開始前readinessと再起動時の拒否

Start前にも既存のDeviceReady（0x90C8）を、Startと共通の10秒deadline内で確認する。準備応答は匿名の `start_ready` へ記録し、0x2001以外ではStartを送らない。問い合わせるのはreadinessのみで、Start本体を再送せず、設定write・撮影・focus・自動再接続も行わない。参照先は既存NOTICEと同じ固定commitの [NikonBase.StartLiveView](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L1758-L1774)。

今回の再起動では両側の事前status/prohibitionが0、両側readinessも0x2001だったが、CAM-AのStartだけ0x2019を返した。したがってreadiness追加だけで問題が解消したとは扱わない。CAM-B Startと両側Stop/Closeは成功、取得/描画/撮影/focusは0。両側を閉じた状態でA本体のLv表示可否を所有者に確認しており、現在のA/B/C起動は未達。これを#335の約10分後の終了と同じ原因とは断定しない。

### #342 A/B水準器表示

所有者の表示実装指示で、AF操作欄の下に左右/前後の整数参考角度と左右の傾き目盛りを追加した。停止/更新切れ/利用不可時は数値を隠し、精度未検証と大きな前後傾斜の制限を表示する。既存Live Viewヘッダーだけを読み、追加命令・設定変更はない。詳細とカメラなしの描画テストは[水準器表示](WPD_PREVIEW_LEVEL.md)を参照。上記#337は過去の失敗記録で、最新の起動/受信結果は開発状況記録に追記する。

### #335/#337 終了理由の保持と明示的な停止・再開

通常の `--seconds 0` では、カメラ側の終了や開始拒否が起きても両台を停止・切断した後に画面を残す。左右の映像と水準器は非表示となり、中央欄に理由を表示する。有限秒数の試験は従来どおり失敗終了し、CIで待ち続けない。時間の近さだけでタイムアウトと決めず、明示されたMTP 0xA00Bだけを「カメラ側でライブビューが終了」と分類する。HRESULT/command status不成功や不正な型の数値を同じ理由に変換しない。

「ライブ停止」は撮影やウィンドウ終了を伴わず、送信済みフレームをdrainして両Stop/Closeへ進む。「ライブ再開」は両workerのjoin後、開いた全sessionのCloseと試行した各Startに対するStop成功を確認できた場合だけ有効になる。片側でも未確認、撮影/保存中、撮影失敗後、終了処理中は無効。開始前に拒否された場合も、Open済みならCloseの確認が必要になる。

再開のクリックでは、同じhardware leaseの下で登録A/Bを再照合し、新しいCamera/channelで開始する。引退済みchannel・古い映像/角度・古いWPD IDの列挙順は使わない。Cの手動位置条件と保存済みJPEGの確認情報は保持する。自動再接続/Start本体の再送/撮影の再試行はしない。過去に失敗したrunは、その後の手動再開が成功しても最終集計で失敗履歴を残す。

`d810_preview_contracts` は明示応答の分類と再開許可条件を確認する。`preview_lifecycle_ui_contracts`（`--lifecycle-ui-smoke`）はカメラを列挙・接続せず、人工的な終了状態を自分の非表示windowへ与え、画面/理由の保持、再開buttonの有効/無効、有限試験の終了を実Win32 eventで確認する。人工のframes等のログを実機実績に数えない。

明示試験 `--seconds 30 --restart-smoke` は、同じbutton経路で二回の停止/手動再開を試す。三sessionそれぞれでA/B取得・描画とStop/Closeを確認する。通常起動では実行しない。撮影/AF/レンズ駆動は0。

所有者が両台の本体c4「ライブビュー」を10分と確認した。過去の598秒台の終了との対応から自動終了設定が有力だが、設定を変えた対照試験前に原因確定や長時間合格とは扱わない。アプリからc4を書き換える処理はない。本体操作で設定を変更する場合は、本人の確認と試験結果を開発状況記録へ残す。

その後の本人の選択は「設定は10分のままで進めてほしい」。したがって本工程は10分設定を維持して、終了理由を確認し明示的に再開する運用とする。無中断表示のための設定変更や自動Startは追加しない。10分後のカメラ側終了は起こり得るものとして扱い、停止・切断の未確認を成功へ変換しない。

`--seconds 720 --camera-end-restart-smoke` は、その実機確認のための明示非撮影試験。単なる経過時間ではなく明示的な0xA00Bを待ち、両workerのjoin・Stop/Close確認後に同じ再開buttonを一度だけ通す。新A/Bと新ライブCの描画で完了する。通常起動にこの動作はない。通信/cleanupの別の失敗なら止め、Startを再送しない。終了した取得sessionの `final_result=FAIL` を残し、別の `camera_end_restart_test_result` と `acquisition_history_passed=0` で復旧試験の結果を区別する。試験中の撮影/AFボタン操作も受け付けない。

Windows WPDの `E_WPD_DEVICE_NOT_OPEN` は、明示的なdispatch/command HRESULTに出た場合だけ通信session切断として表示する。カメラ側の0xA00Bや10分設定とは別に分類し、物理USB抜去・電池等の直接原因を断定しない。停止未確認なら再開は無効のまま。数値の根拠はローカルWindows SDK `PortableDevice.h` の公開identifierであり、Nikon専有資料は使用していない。

Windowsの `HRESULT_FROM_WIN32(ERROR_NO_SUCH_DEVICE)`（0x800701B1）は、別の「Windowsがカメラを利用できない」理由として表示する。明示的なdispatch失敗、または成功dispatch後のcommand失敗だけを分類し、ENDの結果は実際にENDを試行した場合に限る。成功transferやMTP拒否を同じ理由に変換しない。公開Windows SDK `winerror.h` の定数を用い、USB抜去・電池・自動終了等の直接原因や復旧成功を推定しない。従来のStop/Closeと再開禁止条件を維持し、自動再接続や命令再送は追加しない。

Start/Stopは受付ACK後にも同じ10秒deadline内でDeviceReadyを確認し、完了してからstarted/stoppedを公開する。[公開NikonBase](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs#L1758-L1785)の前後Readyという順序を照合した限定改善で、Start本体は再送しない。Stopの既存cleanup規則（OutOfFocusはfocusの状態として許容）を維持する。片側の開始失敗に伴って他方の準備待ちを中断した場合は、別の通信故障や期限切れとは区別して `start_interrupted_by_stop=1` を残し、Stop/Closeへ進む。原因がこれで修復したとは扱わず、実機結果を別に記録する。

### #346 通信ライフサイクルの時刻記録

`Log` の既存行の末尾へ `log_sequence`（プロセス内連番）、`log_mono_ms`（Windows起動後のms）、`log_utc_unix_ms`（UTC Unix時刻ms）を追加する。採時と出力を同じmutex内で行い、両workerとwatchdogの記録順を照合する。これはホストで記録した時刻であり、カメラ内の発生時刻や撮影同期ではない。UTCはOSイベントとの照合用だけで、deadlineや停止判定には使わない。以前のログに時刻を後付けしない。

Live workerではOpen、Close、watchdog Cancelの開始・完了とClose/CancelのHRESULTを記録する。送信直前gateを通過したStart/Stopのdispatch開始、および既存の応答検査で失敗したSendCommand境界にはcommand ID・opcode・dispatch/command HRESULT・MTP応答を記録する。成功した毎フレームの追加記録、transfer context・画像・device IDの出力は行わない。既存の通常集計と失敗履歴を保持する。診断専用の別Open/Close経路にLive workerの境界ログがあるとは扱わない。

新しいカメラ命令・再接続・retry・設定writeはない。追加ログは次回の切り分け用であり、既存のDeviceBusyや通信切断の根本原因修正・実機復旧の証拠ではない。

### #337 A単独の開始切り分け

明示 `--isolated-a-start-diagnostics` は、Bとの開始処理の重なりがなくてもAが拒否するか調べる非撮影診断。両登録機の読み取り診断でLive offとCloseを先に確認し、Bのsessionを閉じたままAだけを再Open・identity再確認する。AのLive off/prohibition=0を再確認して既存ControlでStartを一回試みる。BのStart、撮影、focus、設定write、画像保存、自動retryはない。leaseを全Closeまで保持する。通常の二台表示や単眼運用への自動切り替えとは接続しない。

Startが明示0x2019だった場合に限り、Startと同じ10秒deadline内でReadyとLive/prohibitionを読み取る。公開 [libgphoto2のNikon preview処理](https://github.com/gphoto/libgphoto2/blob/v2.5.31/camlibs/ptp2/library.c#L3458-L3465) にはBusy後にも待機する処理がある一方、既存参照のNikonBaseはStart応答を検査している。本ツールはBusyを受付成功にせず、Start再送やフレーム再試行も採用しない。直後の状態観測だけを独自の診断に使い、失敗はFAILのまま保持する。

Start ACK/Ready/Live onをすべて確認できた場合だけ、既存の転送・D810 envelope・WIC検証で一枚のpreviewをメモリ内で復号する。描画/保存や二台Liveの受入には数えない。この成功経路は今回実機未検証。いずれの結果でも試行したStartに対しStopを一度試みCloseし、watchdog/停止未確認を成功に変換しない。継続開発時の初回診断ではAだけのStartでも0x2019、直後Ready=0x2001/Live=0/prohibition=0で、A Stop/Close成功・診断FAIL。根本原因は未確定。

### #337 追加の読み取り状態診断

`--camera-state-diagnostics` は両登録機を再照合し、Live offを要求する。従来の状態に加え、表示エラーD1B2・application mode D1F0・Live selector D1A6・recording media D10Bを各UINT8、露出program 500EをUINT16として読み、Closeする。送信gateはスカラーreadのみを追加許可し、0x1016のproperty writeは拒否する。数値の出典は [PTP_PROTOCOL_REFERENCES.md](../third_party/PTP_PROTOCOL_REFERENCES.md)。エラー/禁止値0でもStart可能の証明にはならない。

単独診断のBusy後観測は元の10秒deadline内で最大約3秒、100ms間隔のLive/prohibition読み取りに限る。Live=0のままという観測と即座のStopを区別するための診断で、Startの再送や成功への変換はない。

継続開発時の独立比較では、別のmanaged WPD clientとWIA `IWiaItemExtras::Escape` 経由でもA Start=0x2019、Ready/Stop=0x2001だった。WIAの通信STALL解除はAPI成功でも直後の通常dual Startを修復しなかった。WPD Common ResetはE_NOTIMPLで未実行。各比較はprivate probeに隔離し、両WPD preflight/Close、登録Aの物理identity照合、同じleaseの下で実施した。WPD/WIAを重ねず、撮影0。Windowsの別APIでも再現するが、カメラ/driver/設定等の根本原因は未確定。

PC-control mode 0/1の明示比較（mode 1後は0復元確認）とWPD event subscriptionの比較も改善しなかった。これらの専用mode、90C2の許可、callbackは通常ソースから撤去し、private証拠だけ保持した。通常アプリへWIA/Reset/control-mode/自動復旧の依存を追加していない。0x90C2はパラメータ0/1とも通常gateで拒否する。

### #337 Busy後に一度だけframeを調べる診断

`--isolated-a-busy-frame-diagnostics` は前記単独診断の明示的な追加実験。両登録機Live off/CloseとA再照合後、A Start一回がtransport成功のMTP Busyだった場合だけ、元の10秒deadline内でReadyを確認してframeを最大一回取得する。根拠は公開 [libgphoto2のNikon preview経路](https://github.com/gphoto/libgphoto2/blob/92171a382f185f617e95cb9acecf2d43688b19ab/camlibs/ptp2/library.c#L3607-L3622) がBusy後もframe取得へ進むという事実であり、そのretry/restart実装は採用していない。

既存のpayload/chunk/read回数制限、END検証、D810 envelope/WIC検証、watchdog、leaseを維持する。データはメモリ内だけ。Start拒否はframeが復号できてもFAILのままとし、通常のStart成功条件を変更しない。channel retired後の追加readを禁止し、Stop/Closeを試みる。撮影/AF/設定write・B Start・画像保存・Start/frame再送はない。

初回実機ではStart2019、Ready2001でもframeは0bytes、END応答がA00B。channel retired、A Stop/Ready/Close成功でFAILだった。したがってこの系列は表示だけの問題ではなく、実際に非Liveという応答が確認できた。新旧exe比較で後にLiveが復帰した系列とは分けて保存する。

### #335/#337 相手側の自動終了をまたぐ停止候補

片側でtransport成功の明示A00Bを得た場合、両workerのtransfer drain後に、まだ正常でstarted・自分は未終了・data channel未retireの相手だけがStop前に自分のLive statusを確認する。未知の通信障害や自動終了応答のない通常停止には適用しない。新sessionは全join後に観測flagをresetする。

新readの受付を最大2秒に制限する一方、各scalar transferとそのcleanupは元のStop 10秒deadline内に収め、より短い期限で正常data phaseを途中Cancelしない。Live0を観測したら待機を終える。Live1なら最大50ms間隔で再確認する。期間を超えたら従来のStop cleanupへ進み、未知値/read失敗は失敗として残す。元deadlineを延長せず、最後は従来のReady→一回Stop→Ready、全Stop barrier後Closeを維持する。追加のStart/自動再開/設定writeはない。

修正前の実機ではB自動終了→相手A再開拒否と、A自動終了→相手B再開拒否が対応し、自動終了したAだけの再開は成功した。これはタイマー境界の競合仮説を支持するが内部原因の証明ではない。候補の実機結果は安定性報告の末尾へ記録する。

### #416 読むだけの設定の表示（段A）

左右の明るさ・色がそろわない原因を、本体を触らずに見るための表示。**読むだけ**で、設定の書き込み（0x1016）・「Aの設定をBへ写す」は含まない。実装は実機未確認（合成の本体での試験のみ）。

**入口**

- `--settings-read-diagnostics`: lease → Discover → A を開く（identityを登録と照合し直す）→ 読む → Close → B を同じ手順。両台を同時には開かない。`settings_read_diagnostics=1 shutter_commands=0 setting_writes=0 live_start_commands=0` を出し、項目ごとに `CAM-B settings_desc code=0x500d mtp=0x2001 type=0x0006 getset=1 form=2 count=N current=0x… first=0x… last=0x…` の形の行を出す。読めない項目は `unavailable reason=<token> mtp=0x…`（本体が返した型が分かれば `type=0x…` も付く）。最後に `settings_compare` 行（項目ごとの値と差）と `settings_read_diagnostics_result=PASS|FAIL` を出す。識別子・device ID・transfer context・生のbytesは出さない。
- 画面の「両台の設定を読む」ボタン: 両台のLiveが停止・切断済みで再開できる状態の時だけ有効。押した処理の中でも同じ条件（`CanReadSettings`）を見直す。読み取り中は Live の再開・撮影・フォーカス操作・ほかの設定操作を受け付けない。結果はCAM-A／CAM-Bを並べた表（項目、Aの値、Bの値、差あり／同じ／比較不可）で、Cの表示の場所に出る。「ライブ再開」で表は消える。

**読む項目**（`kD810SettingSpecs`。出典と型の照合は [PTP_PROTOCOL_REFERENCES.md](../third_party/PTP_PROTOCOL_REFERENCES.md)）

| 表示 | code | 型 |
| --- | --- | --- |
| 露出モード | 0x500E | UINT16 |
| シャッター速度 | 0x500D | UINT32 |
| 絞り | 0x5007 | UINT16 |
| ISO | 0x500F | UINT16 |
| ISO感度自動制御 | 0xD054 | UINT8 |
| ホワイトバランス | 0x5005 | UINT16 |
| 測光 | 0x500B | UINT16 |
| 露出補正 | 0x5010 | INT16 |
| ピクチャーコントロール | 0xD200 | UINT8（※未照合） |
| アクティブD-ライティング | 0xD14E | UINT8 |
| （診断の行だけ）ISO自動の候補 | 0xD16A | UINT8 |
| （診断の行だけ）Nikon のシャッター速度 | 0xD100 | UINT32 |

名前を付けるのは公開資料で値の意味を確かめたものだけ（露出モード1〜4、WB 1〜7と0x8010〜0x8013、測光1〜4、ISO自動0/1）。それ以外は `raw 0x…` で出す。シャッター速度の分数は1/10000秒の値から丸めた表示用の値で、値が10未満の時は分数にせず `n/10000 s` と出す。文字列型の項目は表に入れない。

**送信gate**（`D810SettingsReadGate`。既存の `AllowedD810PreviewRequest` などは広げていない）

- 構造上書けない。0x1016、WITH_DATA_TO_WRITE、WRITE_DATA は形の検査で常に拒否し、書き込みの分岐は置いていない。
- 通すのは 0x90C8（データ相なし・引数なし）、0x1015（引数は 0xD1A2 の1個だけ）、0x1014（引数1個で表のcode）、自分が開いた転送の context に限った READ_DATA と END_DATA_TRANSFER だけ。operation 番号と引数は32 bit全体で比べ、命令の種類（データ相なし／読み取り）も operation ごとに固定する。0x90C7 は通さない。
- 転送が開いている間は、その転送の READ_DATA／END だけを通す（context は gate が begin の応答から持つ）。別の begin、DeviceReady、END の後の READ_DATA は拒否する。transportが例外を返した後は、開いている転送の END を除いて何も通さない。READ_DATA を1回も受けていない、または受けた応答がすべて0 bytes の転送で、END が MTP の応答コードで明示的に拒否された時だけ、gate は Idle に戻る（本体が知らない code の問い合わせで起きる形）。「明示的な拒否」は、dispatch・command とも成功で、応答コードが 16 bit に収まり 0x2002〜0x2FFF または 0xA000〜0xAFFF の範囲にあるもの。0x2019（DeviceBusy）は転送が閉じたとはみなせないので含めない。範囲外の値（0、0xFFFFFFFF、16 bit を超える値など）と、データを1 byte でも受けた後の拒否、HRESULT の失敗、応答コードの欠落、transport の例外では Closed のまま。**END の拒否コードで転送が閉じたとみなすのは推定で、実機では未確認**（実機で END の拒否の後に次の項目の begin が通るかを確かめるまで、この Idle への復帰は仮の扱い）。

**PropDescの解析**（`ParseD810PropDesc`）: PTP標準の構造（code u16、型u16、GetSet u8、既定値、現在値、FormFlag u8、Range=min・max・step／Enum=個数u16と値）、little-endian。長さは計算した値と完全一致（不足・余りとも拒否）、上限4 KiB（※仮）。code は要求と一致、型は表の型と一致（違えば unavailable。合わせ込まない）。文字列型・配列型・64 bit以上は拒否。GetSet は0/1、FormFlag は0/1/2だけ。Range は min ≤ max（符号付きで比較）、step=0 は「書けない」扱い、Enum の個数は1〜1024（※仮）。

**読み取りの流れ**（`WpdSettingsReadSession::ReadAll`）: DeviceReady（Busy 0x2019 の間は50ms間隔で期限まで）→ Live状態 0xD1A2 を読み、0以外なら止める（`live_view_on`）→ 表の全項目の 0x1014。次へ進むのは、本体が明示のMTP応答コードで答えた項目と、空の答えを END が受け入れた項目だけで、`unavailable` として残す。(1) begin の拒否（dispatch・command とも成功で、応答コードが上の「明示的な拒否」の範囲）は `mtp_rejected`。範囲外のコードと 0x2019 は拒否として数えず、止める。(2) begin は成功して context が返り、READ_DATA を1回以上送っても受け取った bytes が0のまま END を送り、その応答が「明示的な拒否」だった場合は `mtp_rejected_at_end`（行に END のコードを残す）。READ_DATA を1回も送っていない場合（大きさの上限超過など）はこれに含めない。(3) 大きさ不明の転送で READ_DATA が0 bytes を返し、END が 0x2001 で成功した場合は `empty_payload`（行の mtp は END の 0x2001）。(4) 大きさの上限超過（`too_large`）は END が成功した時。それ以外の転送の失敗はそのsessionでこれ以上送らない（残りの行は `not_read`、停止の理由は `not_read_after_failure`）。begin のWPD HRESULT の失敗（コマンド応答の HRESULT、session切断など）は `command_failed` の行を残して止める。SendCommand 自体の HRESULT の失敗（dispatch）は `transfer_failed_6` になる。データを途中まで受けた後の拒否、END の応答コードの欠落、transportの例外も止める。止めた行の `mtp` は、失敗した命令の応答コードがあればそれ、無ければ END の応答コード。直後の行 `settings_stop code=0x…` に END の `end_dispatch`・`end_command`・`end_mtp`（Live と同じ書き方）と、`end_error` の番号（EndRejected なら 12）を残す。期限・停止要求の後は開いている転送のENDとCloseだけで、各命令は1回だけ送る（再送しない）。`ReadAll` は1つのsessionで1回だけで、2回目は何も送らず `already_read` の理由で全項目を `not_read` にする。

**期限と停止**: 1回の読み取りに30秒（※仮）を1つ、I/Oが12秒止まったら `Cancel`（`StillLane` と同じ）。停止の旗は読み取り1回ごとの専用のもので、立てるだけで戻さない。ウィンドウを閉じる時（WM_CLOSE、有限実行の終了）は旗を立て、読み取りの終わりとCloseを待ってから窓とleaseを手放す。Closeできなかった時は読み直しも Live の再開も出さず、窓を閉じて本体・USB接続を確認する。

**試験**: `d810_settings_contracts`（解析の境界、整形、比較、gateの掃引、転送の順序、偽の本体での読み取り）、`preview_settings_ui_contracts`（`--settings-ui-smoke`。合成のbackend）。既存の `d810_preview_contracts` にはWRITE_DATAと0x1016の拒否を足した。

**実機で確かめること**（`--settings-read-diagnostics`）: 0x1014 の応答が `mtp=0x2001` か、0x500E／0x500D／0x5007 の getset と一覧（form=2 の first／last）、ISO感度自動制御が 0xD054 と 0xD16A のどちらに答えるか、0xD100 の有無、0xD200 の型、ISO・シャッター速度・WB・測光が本体の表示（EXIF）と一致するか、Live中は読まず `live_view_on` で止まること。`reason=mtp_rejected_at_end` や `empty_payload` が出た時は、その次の項目の 0x1014 が通ったか（END の拒否で転送が閉じたという推定の確認）。途中で止まった時は、行の `mtp` と直後の `settings_stop` の行（END のコード）を保存する。

### #416 設定の書き込み（段B、ADR-0038）

CAM-Aの露出の5項目（露出モード・ISO感度自動制御・ISO・シャッター速度・絞り）をCAM-Bへ写す書き込みの部品。今の段階は、偽の本体の試験までで、画面にはつないでいない。**既定のbuildでは何も書けない**。書ける行を決めるのはリポジトリの中の固定の表だけで、その表は今、全部の行が無効になっている（下の「書ける行を決める物」）。実機にはまだ1回も書いていない（※未検証）。

**build option**: `A0_BUILD_WPD_SETTINGS_WRITE`（既定OFF。`A0_BUILD_WPD_DUAL_PREVIEW` とは別）。OFFの間は、`wpd_settings_write.hpp` をプレビューに入れるとコンパイルが止まる（`#error`）。だから、書き込みのchannelも「写す」のボタンもOFFのbuildには入らない。実機に向けるbuildは、AGENTS.mdの例外が人の手で入るまでOFFのままにする。書き込みのコマンドラインの入口は作らない。

**部品**（`d810_settings_write.hpp` は純粋な関数、`wpd_settings_write.hpp` はWPD・ファイル・画面から呼ぶ入口）

- `kD810WriteItems`: 書く可能性のある項目の表。露出モード（0x500E、Mだけ）、ISO感度自動制御（0xD054か0xD16A、OFFだけ）、ISO（0x500F）、シャッター速度（0x500Dか0xD100）、絞り（0x5007）、ADR-0041の候補のWB（0x5005）・測光（0x500B）・露出補正（0x5010、INT16）。表の順が書く順。ピクチャーコントロールとアクティブD-ライティングは型が確定していないので載せない。画質・画像サイズ・フォーカス・記録先・ほかのプロパティは表に無く、どこからも書けない。WBのManual（1）・色温度（0x8012）・Preset（0x8013）は本体の別の中身に頼る値なので書かず、止めを解く比較でも「不明」として扱う。
- `D810WriteTable` と `kD810ProductWriteTable`: 項目×カメラの行（確定したcode、有効・無効）、計画の種類（写す／値を選ぶ）、確かめた機種とfirmwareの版。
- `D810SettingsWriteGate`: 書き込み専用のgate。`WpdSettingsWriteSession`: 1台分のsession。`D810SettingsWriteController`: 画面から呼ぶ入口。
- `D810SettingsReadGate` と `AllowedD810PreviewRequest` は1行も変えていない。今までどおり0x1016・WITH_DATA_TO_WRITE・WRITE_DATAを拒否する。

**書ける行を決める物**（固定の表だけ）

- 有効にする手段は、表を変えるcommit（レビュー付き）だけ。実行時のファイル・環境変数・レジストリ・コマンドラインの引数では、行を有効にすることも、codeを確定することも、計画の種類を足すこともできない。実行時の物は止める方向にだけ効く。
- `static_assert` で縛る: 製品の表で有効にできるのはCAM-B×ADR-0038の5項目だけ。CAM-Aの行、追加の項目の行、「値を選ぶ」を有効にした表はコンパイルが通らない（ADR-0038の不変条件2「Aには書かない」をコンパイル時に成り立たせる）。今は有効な行が0であることも `static_assert` にしてある。
- ISO感度自動制御とシャッター速度のcodeは、段Aの実機の読み取りで1つに確定するまで0（未確定）。未確定の間は、Aの条件（ISO自動OFF）もBの前提（ISO自動OFF）も読めないので、ISO・シャッター速度・絞りも含めて計画を作れない。実行時に候補を探したり切り替えたりはしない。0xD100はBulb・Timeの符号化と単位の出典が無いので、表に確定させられない（`static_assert`）。
- 確かめた機種とfirmwareの版を表に持たせる（今はfirmwareが未記入で、どの本体も一致しない）。書く相手の本体がidentityで報告する機種と版が表と違えば、1件も送らない。版の文字列は記録にもログにも書かない。
- 表で有効でも、書く直前にそのsessionで読み直したPropDescで毎回確かめる: 型が表と一致、GetSet=1、FormFlagがEnum（値あり）かRange（stepが正）、書く値がその一覧・範囲にある。段Aの `D810PropDesc::Writable()` はFormFlagがNoneでも真を返すので、書き込みの判定には使わず、`D810DescWritable` を使う。

**計画**（`PlanCopy`、`PlanChosen`）

- 写す（ADR-0038）: Aが M・ISO自動OFF・シャッター速度がBulb／Time／0でなく1秒以下（`kD810MaxWritableExposureUnits`、※仮）の時だけ作る。差のある項目だけを書く。値はAから読んだ値（露出モードはM、ISO自動はOFFの固定値）。Bがその値を広告していなければ計画を作らない。行が無効な項目と、操作者が外した項目（`only`）は `skipped` に理由と一緒に載り、書かない（値を別の値に置き換えることはない）。「1項目だけ」はこの外し方で行う。Bの露出モードがMでなく、露出モードを書けない時は、計画を作らず「本体でMにしてください」の理由（`exposure_mode_not_manual_set_on_body`）を返す（決定14）。
- 値を選ぶ（ADR-0041、Proposed）: コードはあるが、製品の表で無効。有効にしても、最初の版は「両台に同じ値」だけで、両台が広告している値だけ。
- 計画は `Confirm` で確定した物だけを `Execute` できる。確定は1回限りで、実行すると消える。

**実行**（`Execute`）

1. 撮影の止めが「不明」「未確認」「記録が読めない」の間は始めない（先に「読み直す」）。
2. カメラの持ち主（なし／Live／撮影／設定）を1つのatomicでcompare-and-swapし、なしから設定に移れなければ送信0。
3. 記録に計画と各項目の前の値を書いてflushする。書けなければ送信0。
4. 計画が頼る全部のカメラを、読むだけのsession（段Aの `WpdSettingsReadSession`）で開いて読み直し、閉じる。値が1つでも計画と違う、または書く相手の機種・版が表と違えば、送信0で止める。
5. 書く相手ごとに（Aがあれば先にA）、書き込みのsessionを開き、項目ごとに: Liveの状態（0xD1A2が0）→露出モード→ISO自動（必要な項目だけ）→その項目のPropDescを読み直し、前の値が計画どおりか確かめる→Arm→begin（0x1016）→WRITE_DATA→END→読み戻し→一致／不一致／不明。期限は両台の残りの項目の全部で数え、足りなければbeginを渡さない。
6. 一致以外、送る前の拒否、途中の停止で、その操作の残りは送らない。操作の掛け金（`D810OperationLatch`）が落ち、もう片方のカメラは書き込みで開かない。再送しない。前の値に戻さない（「元に戻す」機能は作らない）。

**gateの状態機械**（`D810SettingsWriteGate`、1つのsessionに1つ）

| 状態（ADR-0038決定9の呼び名） | 受け付ける物 | 次 |
|---|---|---|
| Idle | 0x90C8（引数なし）、0x1015（0xD1A2だけ）、0x1014（計画のcodeと0x500E・確定したISO自動のcode）、Armした組の0x1016 | 各状態へ |
| ReadyPending／ReadBegun／ReadOpen／ReadEnding（ReadOpen） | 自分が開いた読み取りのREAD_DATAとEND | ENDが0x2001でデータがきれいならIdle（読んだ値を今の世代に記録）。それ以外はRetired |
| WriteBegun／WriteOpen（WriteBegun） | 応答の後、そのcontextのWRITE_DATAを1回だけ（NUM_BYTES_TO_WRITE・要素数・中身が計画の値の符号化と完全一致） | 応答にcontextが無ければRetired |
| WriteSent／WriteEnding（WriteSent） | WRITE_DATAの応答の後、そのcontextのENDだけ | ENDが0x2001でWRITE_DATAが全部書けていればIdle（世代を進める）。どんな拒否もRetired |
| Retired | 開いていた転送のENDを1回だけ（watchdogのCancelの後は何も） | なし |

- Armの鍵は（宛先、code、値）。Armできるのは、計画にある組、そのcodeをまだ書いていない、今の世代でLiveの0を読んだ、今の世代でそのcodeのPropDescを読んで値が書ける、前提（露出・ISO自動）を今の世代で読んだ時だけ。
- beginをtransportに渡す前に、Armを消費して世代を進める。「送った」はこの時点で数え、beginが例外で終わっても送ったに数える。
- 読み取りのgateの「明示の拒否ならIdleに戻る」は持ち込んでいない。拒否の判定の関数（`IsExplicitMtpRejection`）も使っていない。本体の拒否、応答の欠落、例外、watchdogは、どれも結果を「不明」にする。
- 通るopcodeは0x90C8・0x1014・0x1015・0x1016だけで、0x9207・0x90C2ほかのvendor operationを送る道は無い。

**記録と撮影の止め**

- 置き場所: `SHGetKnownFolderPath(FOLDERID_LocalAppData)` の下の `A0CameraStitcher\settings-write\journal.txt`。取れなければ書かない（cwdやtempに逃げない）。親のフォルダの各段がフォルダでreparse pointでないこと、記録が通常のファイルであることを毎回確かめる。各行はwrite-throughで書いてflushする。
- 行: 計画（時刻・種類・項目の数・digest）、項目（alias・項目・code・前の値・書く値）、結果（読み戻しの値・一致／不一致／不明）、終わり（結果・送った数・理由）、読み直し（一致／不一致）。device ID・serial・機種・版・context・生のbytesは書かない。
- 止め（`Block()`）: 記録のディスクの中身が正本。不一致・不明で終わった操作、終わりの行の無い操作（未確認）、読めない行（未確認）、読めない記録（読めない）の間は撮影を止める。プロセスの再起動、Controllerの作り直しでは解けない。後の操作が一致で終わっても解けない。
- 解き方: 「読み直す」（`Reread`）。読むだけのsessionで両台を読み、止めの対象の項目（5項目と、有効にした追加の項目）がすべてAとBで一致した時だけ、読み直しの一致の行を書いて解く。読み直して違いがあれば、止めは「不一致」のまま（「不明」「未確認」は「不一致」に変わり、次の計画を作れるようになる）。記録が読めない時は解けないので、人が記録を確かめて退避する。
- 記録のファイルを消すと止めは解ける（※段Cの前提条件(b)の論点。段Cで別の場所にも未解決の印を持つ）。

**画面への組み込み**（まだ。順番を決めて入れる）

`A0_BUILD_WPD_SETTINGS_WRITE` をONにしたbuildだけで、`wpd_dual_preview_ui.hpp` の読み取りの表の横に置く。入口は `D810SettingsWriteController`（製品の表は `kD810ProductWriteTable`、記録は `OpenProductD810SettingsJournal()`、持ち主はLiveと撮影の経路と共有する1つの `D810CameraOwnership`）。画面に出す材料は `Status()`（止めの種類、撮影を許すか、写すを許すか、有効な行の数）と、計画の `steps`・`skipped`・`reasons` の符号、実行の `verdict`・`reason`。

**試験**: `d810_settings_write_contracts`（表と値、製品の表、承認らしいファイルを置いても何も有効にならない、計画、gateの掃引（Armしたgateは全opcode×命令の種類×引数、ほかの状態は関係するopcode。Armは全code×値）、Armの条件、転送の順序と失敗、偽の本体での実行、記録と止め、読み直し）。
