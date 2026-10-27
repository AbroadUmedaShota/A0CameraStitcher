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
