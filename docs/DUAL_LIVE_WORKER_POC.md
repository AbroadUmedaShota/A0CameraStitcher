# 二プロセス Live View 試作（software-only）

## 結論

### 二台実機run-04: 終了確認不能・隔離維持（2026-09-23）

本人の二台接続復旧を受け、AOPC-11-NOTEでOS上の正常なD810二台、A0関連processなし、委譲markerなしを確認した。隔離SDK一組と付随DLL三つの存在を読取りで確認。SDK有効の `PreviewCommissioning` / `PreviewWorker` を既存Visual Studio生成projectからRelease buildし、exit 0。画面exeのSHA-256は `132FA1975F8E6EB5D074B6B5EA3403F1318D7F10A84B3DBCAC8E4E288D3F3260`、worker exeは `056E4F003362E0794FBE1080CBEA334B579B34506633CD874AB75E98906EAE0C`。実行processに限りSDK module pathを渡し、`--commission-preview-only` で画面を起動した。起動時点ではカメラ操作0回。

本人による「開始」後に二workerの起動を観測した。試験画面は候補選択欄を表示したが、その後 `終了確認が取れません。隔離を維持しています。アプリを閉じず、実機状態を確認してください。` を表示。worker二つはOS上で終了したが、委譲markerは残存し、画面processは存続した。本人は二台とも電源ON・背面ライブビュー映像なしを目視確認した。ただしこれをSDK停止・Source/Module closeの証明には使わない。明示的なCAM-A/B割当、左右同時フレーム、両workerの完全close receiptは未確認。

本人が二台の電源OFF・USB切断を実施し、OSでD810 0台・worker 0件を確認。通常のウィンドウ終了を一度要求しても隔離画面は閉じなかった。本人から画面だけの手動終了の明示承認を得て、実行ファイルpath/PID、D810 0台、worker 0件、marker 1件を再照合して画面PID 32488のみを終了した。markerは1件のまま維持した。これにより読めたignored記録 `build/worker-selection-sdk/Release/logs/preview-run-32488-434863421.jsonl` は `run_started` → `worker_a_enumerated(value=2)` → 13.75秒後に `operation_failed` → `close_unconfirmed`、後刻の通常終了要求でも `close_unconfirmed`。CAM-A/B確認・同時開始・frame受信の記録はない。失敗したSDK操作の詳細categoryとworker終了codeはこの記録では不明。再送・marker削除・撮影・設定変更・WPD・カード操作は行っていない。

**実機preview枠は累計4/5消費、残り1回。結果は未合格かつ隔離中。** 同条件の再実行はしない。次はSDK操作失敗時の匿名category・close receipt伝搬とworker終了codeの欠落をsoftware-onlyで解析する。隔離markerは正常終了の証拠なしに解除しない。

### 隔離中の記録読取りを許可（software-only、実機再実行なし）

run-04では `PreviewRunJournal` が排他書込みで開かれ、隔離中に記録を読めなかった。記録済み行をwriter存続中に別のread handleから読める契約試験を追加し、変更前は1/1 FAIL（この読取り条件のみ）、`FILE_SHARE_READ` 追加後は1/1 PASS。`CREATE_NEW`、write-through/flush、書込み独占、既存ファイル上書き禁止は維持。SDK有効の `PreviewCommissioning` Release buildもexit 0。これは今後の隔離調査の可観測性修正であり、run-04の終了証拠の欠落や残存markerを解決しない。SDK・カメラ再操作0回、実機preview枠は4/5のまま。

### run-04の失敗伝搬と隔離復旧の停止判定（software-only調査）

確定した順序はjournalの `run_started` → `worker_a_enumerated(2)` → 13.75秒後の `operation_failed` → `close_unconfirmed` である。`worker_a_observed_bytes` はなく、CAM-A/B確認も同時開始もない。`PreviewCommissioning::Preview` は候補 `select`、Live View `start`、`frame` を順に送るため、このいずれか、またはそのIPCで失敗した可能性がある。どの命令か、SDKの戻り値、workerのexit codeは記録されておらず、今回の実SDK失敗原因は**未確定**。60秒の全体期限が直接発火したとの記録もない。

コード上の伝搬欠落は別に確認できる。`WorkerPreviewDispatcher::Handle` は `TransportError` を捕捉するとSDKの一回限りの終了処理を試み、`status=failed` にエラー分類と終了receiptを入れ得る。一方、親の `preview_worker_owner.cpp::Exchange` は通常操作の `status=ok` を要求し、`failed` 応答ではpayloadのエラー分類・終了receiptを採用せず、delivery未確認のままACK前に失敗する。その後の `CloseChild` は同workerへの追加送信を拒否する。worker側は正常な明示 `close` 以外では、SDKが閉じていても正常終了code 0を保証しない。したがって、失敗応答が届いた場合でもmarker解除のための証跡が親に残らない経路がある。これは**コードからの機構上の説明**であり、run-04で実際に `failed` 応答が親まで届いた事実は未観測。通信途絶・worker先行終了も区別できない。

ソフトウェア実行証拠: SDKなしのfake候補選択失敗を `worker_preview_dispatcher_tests` へ追加し、親に返す応答の `status=failed`、固定 `error`、`safeToExit=true`、一回のclose、明示close成功ではない状態を検査した。stub Release build exit 0（既存C4819警告）、専用exeを1回実行して `failures=0`、exit 0。これはworker応答の契約確認であり、親 `Exchange` の実IPC配送、run-04の実SDK失敗category、marker解除の成立を証明しない。owner既存試験系列は5/5のため再実行していない。

非破壊の人手復旧手順は、①操作者がカメラの電源・背面Live View・USB状態を確認し、不明ならカメラを安全に電源OFF/切断する、②OS上のD810・試験画面・worker・同一sessionの委譲markerを読取りで照合する、③ignored journalとアプリ表示を保全する、④markerを**削除・編集せず**カメラ制御入口を停止したまま原因と終了証跡を評価する、である。2026-09-23の本調査時点ではmarkerは1件、v2構造は妥当、reparseでなく、試験画面/workerは0件。ただしOS上のD810は再び2台見えており、その後の物理状態や再接続の理由は未確認。接続済みであっても隔離を解除しない。秘密のnonce/epochや個体番号は記録・commitしない。

既存の `DisarmDualDelegation` は両workerの型付き完全終了証拠、保持process handleとの一致、両exit code 0、marker内容一致を要求する。run-04ではこれらが揃わず、親画面も既に終了しているため、**既存APIだけで安全にmarkerを解除する経路はない**。手動のファイル削除、再起動、旧binaryからの操作を復旧手順にしない。解除が必要なら、カメラの物理隔離、証拠保全、正確な対象確認、監査可能な一回限りの操作を含む別の人間判断・実装・software-only検証が先に必要。

残り1/5の実機preview試験は現時点で**実施不可**。実施判断の最低条件は、失敗命令・匿名category・worker終了receiptを失わない改修とfake/IPC負例、markerの承認済み回復手順、両実機の新たな接続/物理alias確認、変更後候補のexact hash、SDK/WPD非重複、1回だけの開始・左右新規frame・両側停止/closeを記録できること。どれか欠ければ中止し、SDK close不明、片側停止、通信不明、期限切れでは再試行・marker解除・撮影へ進まない。プレビュー成功でも撮影、保存原画像、光学品質、製品受入の証拠にはしない。

### 単体実機run-03: SDK読込み段階で失敗（2026-09-23）

本人の一台で進める指示を受け、既存preview枠の3回目を実施した。AOPC-11-NOTEで正常なD810一台、A0関連processなし、委譲隔離markerなしを確認。current source `319335fa96f76819b36fee7d33fd8d0e40fc8de9` の `SingleWorkerPreview` をSDK有効構成でbuild（exit 0）し、SHA-256 `E02813FD0757672271D2D206A9B07EE0EE170FDF6C5E8E85C39980CFB0279B68` のexeで `preview-single --confirm-one-physical-camera` を一回だけ実行した。目的は更新後の終了receiptを含む単体経路の確認であり、凍結済み旧workerの再合格ではない。

結果はexit 5、`status=failed`、`error=sdk_load_failed`、`frames=0`、`bytes=0`。openingからclosingへ移り、starting段階には達しなかった。`closeReceipt` はSource/Module/process claimの解放をすべてtrueと報告し、終了後のA0関連processは0。`closeConfirmed=true` だが、開始していないLive Viewのoff確認はfalseであり、Live Viewの取得・停止成功とは扱わない。エラー分類だけではDLL読み込み、Module Open、初期化callback等のどこが失敗したか確定できない。

rawログはignoredの `build/worker-selection-sdk/hardware-run-03.log` に保持した。撮影・撮影設定変更・WPD・カード操作・画像保存・自動再試行は0回。凍結済み旧workerと既存配布候補は変更していない。**実機preview枠は累計3/5消費、残り2回**。追加実行は原因切り分け後に別の有効な検証として判断し、同条件では再試行しない。以下の2/5記述は当時の履歴である。二台同時Live View、物理alias照合、撮影/品質受入は未達のまま。

### SDK初期化の匿名段階診断（software検証、実機再実行なし）

run-03の分類だけでは失敗点を特定できなかったため、次の既承認preview実行に備えて `sdkLoadStage` を追加した。SDK初期化の `sdk_load_failed` に限り、runtime配置確認、DLL directory、PTP/module library、entrypoint、Module Open、capability/callback、module mode、子列挙、version取得のどこで例外が発生したかをenumから固定tokenで返す。stageなし・未知enumは空文字。SDK戻り値・OS自由文・path・個体情報はJSONへ追加しない。既存error分類/exit/終了確認/リース/自動再試行禁止は維持する。旧run-03にはこの情報がなく、今回の原因は未確定のまま。

`build/single-worker-diagnostics-sdk` を新規生成し、SDK有効のworkerとfakeテストをbuildしてexit 0（既存C4819警告あり）。`ctest --test-dir build/single-worker-diagnostics-sdk -C Release -R '^single_worker_preview_contracts$' --output-on-failure` を1回実行、1/1 PASS、0.35秒、exit 0。sdk_load_failed段階伝搬、Open一回・Start/Frame/Stopなし・Close一回、終了receipt、未知stage空をfakeで検証した。実SDKロード/実機追加試験は0回。run-03のexeおよび旧凍結workerのhash不変を確認し、診断buildによる上書きを避けた。予算は3/5・残り2回。

### 非画像の試験記録と表示失敗の保持（2026-09-22）

実機試験画面は開始ボタン時、worker生成より前にexe隣接の `logs/preview-run-<process>-<tick>.jsonl` をCREATE_NEWで作成する。固定ASCII event・連番・単調時刻・数値だけを各行write/flushし、画像・Source token・serial・自由文例外を保存しない。通常起動/UI-only/describeでは記録を作らない。作成失敗はSDK開始を禁止、操作後の記録失敗は終了確認へ進む。終了記録の失敗もSDK閉鎖を妨げず画面で未合格とする。ログはignored build配下で保持し、既存ログの上書き・自動削除はない。

`frame_pair_received` はバイト受信だけの事実であり、表示成功、異なる物理個体、フレーム同期、製品合格の証明ではない。実WIC表示の失敗はUIで保持し、制御threadに `display_failed` を記録させる。後続のClosedでも未合格表示を消さない。終了不明時の隔離・自動再試行禁止は維持する。完全なログ、本人の物理alias確認、左右映像の実観測、両側終了確認を合わせて実機結果を判定する。

最終stub/SDK PreviewCommissioning buildはいずれもexit 0。新規2試験 `ctest --test-dir build/worker-selection-stub -C Release -R '^preview_(run_journal|display_failure)_contracts$' --output-on-failure` は一回実行、2/2 PASS（0.41秒、exit 0）。前者は記録構文・連番・既存file拒否・不正event拒否、後者は非表示の実WindowProc/WICへ破損JPEGを渡しClose後の未合格保持を確認した。Start命令なし、SDK/worker/lease開始なし。disk write/flush障害の注入と実カメラ映像表示は未検証。既存owner/lease試験は再実行していない。

同ターンのOS確認はAOPC-11-NOTEでD810 2台/正常2台、A0関連processなし。実機preview予算は **2/5消費・残り3回** のまま。本人にA/Bそれぞれだけへ映る目印の準備と割当操作が可能かを確認中で、SDK版実行はまだ行っていない。

### 委譲検証・起動失敗分類の修正（software検証済み、実機未実行）

委譲marker v2に親PID・登録済み子PID二つ・ランダムepochを永続化し、workerがSDK transport生成前と各SDK操作前後に照合する。親の生存・同一Windows session・既存mutexの占有も確認する。mutexの占有だけを所有者PIDのOS証明とは呼ばず、保持process handleと登録記録を合わせた通常起動経路の検証とする。同一アカウントによる意図的な記録改ざんへのセキュリティ境界ではない。SDK有効buildは試験用lease/root overrideを拒否する。

`PreviewWorkerStartupError` は子process未生成の確定失敗と部分起動を区別する。前者は画面を閉じられるが、既存隔離記録があっても解除しない。後者は隔離を維持する。owner試験にexe欠落と二子生成後bootstrap前の注入失敗を追加した。新規authorization試験は偽epoch・null親・未登録子・欠落記録・偽bootstrapをSDKなしの実processで検査する。

独立再レビューでは旧HIGH/MEDIUMのコード修正を確認。空authority callbackによる既存IPC試験の回帰を指摘されたため、callback引数の既定値を削除しstub専用試験だけ明示callbackを渡す修正を加えた。欠落root検証で作られる試験用空directoryの厳密cleanupは維持。実機停止・期限切れ・物理個体確認は未検証で、二台実機成立や製品受入へ昇格しない。

最終stub Release build exit 0。`ctest --test-dir build/worker-selection-stub -C Release -R '^(preview_worker_owner|worker_delegation_authorization|worker_preview_pipe)_contracts$' --output-on-failure` を一回実行し3/3 PASS、29.07秒、exit 0（authorization 15.72秒、owner 11.54秒、pipe 1.27秒）。owner試験seriesは累計5回となり、この枠で追加再実行しない。旧delegation suiteも再実行なし。SDK有効のPreviewWorker/PreviewCommissioning build exit 0、SDK版生成projectに `A0_NIKON_SDK_AVAILABLE=1` がありstub test context定義がないことを確認。既存core/headerのC4819警告は残る。SDK版実行・撮影・設定変更・WPD・カード操作は0回、preview実機予算は2/5消費・残り3回。

### 前回の実機移行判定: BLOCK（以下は修正前のレビュー記録）

独立read-onlyレビューで、workerが任意の生存parent handleと自己発行bootstrapを受理し、登録済み本番lease ownerからの委譲をworker自身で確認できない欠落を検出した。現状のparent PID/capability照合だけでは、別launcherによる隔離marker迂回を拒否できない。次は永続委譲記録をowner PID・登録worker PID・世代nonceに結び、workerのSDK入口で照合し、不一致/記録欠落/偽bootstrapをSDK open前に拒否する。同じWindowsアカウントによるbinary/markerの意図的改ざん耐性は別の非保証であり、通常の直接起動経路を許可する理由にはしない。

もう一点、owner constructorのすべての例外を部分起動として扱うため、worker exe欠落など起動前の確定失敗でも画面が閉じられない。owner側で子process生成の有無を区別して伝え、確定的な起動前失敗だけは正常な画面終了を許し、部分起動不明は隔離を維持する修正が必要。これらは未解決で、UI-only合格・build成功は実機投入許可ではない。

レビュー後の確認: AOPC-11-NOTEのOS列挙ではD810 2台/正常2台（個体IDは出力・保存なし）。SDK操作0回、preview予算2/5のまま。GUIのSDK版buildはexit 0（その後の停止要求チェック1行追加はSDK版再build待ち）。最終stub buildとdescribe再検証はexit 0、describe 1/1 PASS（0.68秒、累計2回）。混在 `--describe --commission-preview-only` と重複 `--ui-only --ui-only` はそれぞれexit 2、UI/SDK開始なし。現在の画面は未受入で、実機モードを起動しない。

### 2026-09-22 実験用の操作者画面（統合検証中、実機未実行）

`A0CameraStitcher.PreviewCommissioning` をPhase 0 C++の独立した試験画面として追加。本体.NETアプリの代替や製品受入ではない。通常起動と `--ui-only` は表示専用でSDK/lease/workerを作らない。`--describe` は副作用のないJSON発見操作で、機械操作標準は `partial` と明記する。実機操作は別の `--commission-preview-only` と操作者の開始ボタンを必要とし、そのflagだけを本人承認の証拠として扱わない。

専用threadがowner構築・SDK列挙・候補preview・物理alias観測・停止・二台開始・終了を所有する。候補はworker内の0/1として選び、初期表示は物理alias未確定。二台開始後はCAM-A/Bの順に各3frameを取得して終了を確認する限定PoCで、常時表示の完成品ではない。previewはメモリーだけ、保存/撮影/設定/WPD/カード操作はない。画像受信の時刻と古い表示を区別し、終了不明を成功やLiveとして表示しない。

統合時に、owner構築途中の失敗を「開始前の正常終了」にしない処理、停止要求後の追加frame抑止、確認画像を新しい同時frameへ偽装しない処理、同時取得中の再割当禁止、UI-onlyの操作入口拒否、SDK列挙中からの固定60秒期限表示を補強した。終了未確認は画面・制御threadと隔離を保持し、自動kill/restartしない。人手確認を含む60秒運用の実機での妥当性は未検証。

stub build exit 0。`preview_commissioning_describe` は1/1 PASS（0.33秒、exit 0）。最初の非表示UI-only起動は画面観測不可だったため、実行pathと `--ui-only` を照合した試験processだけを終了した。実workerは起動していない。その後通常起動をUI-onlyへ揃え再build exit 0（当targetの警告0）。Computer Useで対象画面を前面化して左右pane、未受信表示、開始/候補/割当/同時開始のdisabled状態を確認し、タイトルバーの閉じるで終了、対象windowとA0 processの残存0を確認した。実画像decode、SDK操作中のUI操作/取消/期限、実物理割当は未確認。独立安全レビューとSDK版buildを回収するまで実機へ進めない。

### 2026-09-22 親側commissioning命令の接続（最新、実機未実行）

`PreviewWorkerOwner` へ列挙・候補preview・操作者の物理alias確認とSource停止・同時開始・alias別frame取得を接続した。`PreviewCommissioning` はworker 0のpreview確認とsuspendが成功するまでworker 1の列挙を許可せず、二台の異なる物理aliasを明示確認して両Sourceを閉じた後だけ、一度のresume/startを許す。tokenが別workerで同じ文字列でも同じ実機とは判断せず、worker順序をCAM-A/Bへ自動対応させない。候補変更・重複割当・途中失敗・二回目startはterminalで、自動再試行しない。

親の実pipe clientは各workerの連続sequence・epoch・OS PIDを検証する。送信/応答不明なら同workerへ追加送信しない。公開APIの失敗時は両workerのCloseを試みるが、未確認応答は正常終了として扱わずmarkerを残す。正常な命令応答と、両SDK閉鎖・両process終了による解除は別判定のまま。

frame返信はraw最大256 KiBをhex化するため、親の明示的なJSON上限を512 KiB+4096へ設定した。共有parserの既定256 KiBと既存利用側は変更していない。previewはメモリー上の表示素材のみで、原画像・合成入力にはしない。

検証: stub Release build exit 0。`ctest --test-dir build/worker-selection-stub -C Release -R '^(preview_commissioning|preview_worker_owner)_contracts$' --output-on-failure` は2/2 PASS（1.70秒、exit 0）。実dispatcherとfake transportによる最大frame往復・逆順alias割当・二重割当・早期start・未知候補・二台目resume失敗の再送禁止を確認。owner側の実process/pipe終了と親死亡の回帰もPASS（owner suite累計3回）。最大frameを実pipe越しに送る試験、SDK操作後の終了、物理個体の実確認はまだ未検証である。

このAPIは内部実験用で、本体UIや操作者用CLIからはまだ呼ばない。物理aliasは操作者の映像確認であり、SDK/WPD capture bindingやシリアル照合の証拠ではない。次は操作者が一台ずつ映像を見て確認できる限定UI/CLIの接続と候補版の検証。実機枠は2/5のまま。以下は各段階の履歴であり「親controller未接続」などの記述は当時の状態を示す。

SDK有効構成もworkerとcommissioning test targetのbuild exit 0を確認。既存C4819と負例testの戻り値破棄C4834警告あり。同じfake試験のSDK側重複実行はしていない。SDK版workerの起動・カメラ操作はいずれも0回。

UI接続前にowner threadをコードでも強制した。別threadのCloseは状態を変更せずfalse、通常APIは送信前に拒否する。通常命令前には登録した両processの生存を確認し、相手worker終了後の新規SDK命令も拒否する。stub build exit 0、owner suiteの追加回帰1/1 PASS（0.92秒、exit 0、累計4回）。別threadからのClose/Enumerate拒否後も正規owner threadで正常終了でき、終了済みpairへの命令が拒否されることを確認。これはGUI responsivenessや実SDK実行中の異常の受入ではない。

### 2026-09-22 worker命令・実パイプhostの接続

`WorkerPreviewDispatcher` と内部 `RunWorkerPreviewNamedPipeServer` を追加。既存Camera Agentの同一logon SID限定・remote拒否・長さ付きJSON・delivery ACK・取消drainを使い、既存3種類のdispatcherには新条件を適用しない。新workerだけは `GetNamedPipeClientProcessId` が保持した親processのPIDと一致することを要求する。親生存/固定期限を各通常命令の前後で確認し、schema/epoch/専用capability/連続sequenceを照合する。capabilityは応答へ出さない。

命令は `enumerate/select/start/frame/suspend/resume/close` のみ。frameは最大256 KiBの受信バイト列をhex（最大512 KiB）で返すプレビュー専用で、原画像や合成入力ではない。Source候補の内容/再開条件は実Nikon transportで再検査する。close応答はepoch/worker PID/sequenceと、停止・Source・Module・SDK占有解放・safeToExitを含む。停止不明やhandoff中のSource Close不明を後のローカル解放で成功に変えない。重複/不正要求・失効・通信失敗はterminalで一回だけcloseし、自動SDK再試行や子の強制終了を行わない。安全に片付いたことと正常セッション完了は別で、明示closeと配送確認なしの期限終了をexit 0へ変えない。

これは内部hostまでの接続であり、実worker起動CLI/親controller/物理A/B確認UIはまだない。実行可能ファイルから実二台SDKを起動する入口は増やしていない。今後の親controllerはmarker arm・起動handle登録・強いランダムcapability・物理個体確認後の同時grantを担当し、`status=closed`、全close項目、`safeToExit=true`、epoch/sequence/PID/起動handleの一致と両OS正常終了をすべて照合してからのみ解除する。internal hostが `safe_to_exit=false` を返したworkerはプロセスを保持し、人の復旧まで隔離する必要がある。

検証: SDK構成のdispatcher build exit 0、fake命令試験1/1 PASS（0.12秒）。最終のquarantined応答追加後、stub build exit 0、`ctest --test-dir build/worker-selection-stub -C Release -R '^worker_preview_(dispatcher|pipe)_contracts$' --output-on-failure` は2/2 PASS（0.75秒）、exit 0。要求順序、禁止命令、capability不一致、通信/親失効、各停止失敗をfakeで確認。実パイプ試験はCMakeでSDK無効構成にだけ生成し、実Windows子processへの明示close/不正capability、応答PID、配送ACK、OS exit 0/3を確認した。SDK列挙/Source Open/実機は未実行、preview枠は2/5。今回の試験起動はSDK側1回とstub側1回（計2回）で、自動再実行なし。

最終変更後のSDK構成も同targetを再ビルドしexit 0を確認（既存C4819警告あり）。SDK側の同一fake試験は重複実行せず、最終動作は上記stub試験を根拠とする。二台同時の成功、実機通信性能、親controllerによる隔離解除は未検証のまま。

### 2026-09-22 個体確認後の同一候補handoff

実験用worker selectionに明示的な一回限りのhandoffを追加。候補を一度Openし、Live Viewを開始して個体確認した後、checked STOP成功を条件に `SuspendSelectedWorkerPreview` でSourceのみを閉じる。Module/worker-local候補generationは維持する。`ResumeSelectedWorkerPreview` は同じtoken/同じinventoryの同じSourceだけを一度再Openでき、D810型と初期Live View OFFを再確認してからプレビューへ進む。別候補・再度のhandoff・失敗後の再開・途中のAdd/Remove（同じIDのAddも含む）はterminalにする。handoffのSource CloseではZombieObjectも正常終了と扱わない。既存DualのClose契約は変更しない。

これにより次の統括実装で、Aを単独確認→AのSourceを閉じる→Bを単独確認→BのSourceを閉じる→異なる物理個体であることを明示確認→同時プレビュー、という順序を組める。現時点はtransport APIと選択状態の実装のみで、二台CLI・UI・controllerからは未接続。tokenは個体識別の証明ではなく、operator bindingの代わりにはならない。Module保持中の二プロセス共存と実Source再Openは未検証である。既存単体preview CLIはhandoffを使わず、実機preview枠も消費しない。

検証: `build/worker-selection-sdk` / `build/worker-selection-stub` のRelease `a0_worker_preview_selection_tests` ビルドはいずれもexit 0。SDK側は最終Close厳格化後に差分再ビルド済み。各構成で `ctest -C Release -R '^worker_preview_selection_contracts$' --output-on-failure` を1回ずつ実行し各1/1 PASS（SDK 0.13秒、stub 0.26秒）、exit 0。正常handoffでOpen先が83→83、別候補・inventory変更・Add・Close例外・Resume例外・2回目handoffの拒否を確認。これは選択stateのfake試験であり、実SDK Close結果や二台同時成立の実証ではない。

次の統合対象は新しい実worker host/controllerで、上記APIを世代付きIPCへ接続すること。順序はmarker arm→子プロセス起動/登録→worker-local列挙/単独確認→両Source停止・閉鎖→物理A/Bの明示確定→一回の同時プレビューgrant→両終了通知と登録process終了の照合→marker解除。SDKコマンド前の親生存・期限・grant検査、未知の通信/終了状態での隔離維持が必要。テスト専用IPCの強制終了cleanup helperは実workerへ流用しない。

### 2026-09-22 SDK終了状態のワーカー出力

単体workerの終了JSONに `closeReceipt` (`a0.worker-close.v1`) を追加した。実行ID・PIDと、Live View停止、Source解放、Module/DLL解放、プロセス内SDK占有解除を別々に出力する。Source/Module/占有状態は実transportの `InspectDualSessionExitState` から取得し、checked `Close` が例外なく戻った場合だけ成功フラグを立てる。close失敗後の後始末でローカルobjectが消えても、成功の証拠へ昇格させない。既存 `closeConfirmed` は全解放の集約として維持する。

実行IDは既存 `NewRunId` による診断用相関値で、認証nonceやcamera identityではない。二台統括のIPC受信・grant/generation照合・登録processとの統合は未実装。新出力だけで二台の操作禁止記録を解除してはならない。実機で合格済みの `build/single-worker-sdk` バイナリは変更せず、別の `build/worker-selection-sdk` でビルドする。

検証: `cmake --build build/worker-selection-sdk --config Release --target a0_single_worker_preview_tests A0CameraStitcher.SingleWorkerPreview` exit 0（既存C4819警告あり）。`ctest --test-dir build/worker-selection-sdk -C Release -R '^single_worker_preview_contracts$' --output-on-failure` は今回1回、1/1 PASS、0.49秒、exit 0。実型ExitStateを返すfakeでSource/Module/占有の各残留、close例外時の成功フラグ抑止を確認。実SDKのビルド成立とfake試験であり、実機での新JSON受信や二台解放は未検証。凍結済み単体workerのSHA-256は `EB5C29A2E87D99AE35527011EC11F2506A53190BF7716965DD46CEC6E6C7ADF7` のまま。実機preview枠は2/5。

### 2026-09-22 終了プロセス照合の追加

後続変更: `ArmDualDelegation` 後に `RegisterDualWorkers` を一度だけ呼び、CAM-A/B順でプロセスを登録する契約を追加。leaseが照会/待機権限だけの非継承handleを複製して保持し、解除時の証拠が同じ順の登録プロセスに対応することを検証する。未登録・入替・再登録では解除しない。保持handleにより登録後のPID再利用を防ぐ。登録はSDK操作の許可ではなく、起動元がSDK grantより前に正しいworkerを登録する実controllerの接続はまだ必要。SDK終了フラグとIPC generationの照合も未実装。

この後続変更の同一stub target buildはexit 0、同じCTestを1回実行し1/1 PASS（1.88秒、exit 0）。未登録・A/B入替・再登録後の解除拒否を追加確認。異常終了の検査はexit 91の子を登録してから行う。系列累計5回で終了。実機previewは未実行、2/5消費済みのまま。

永続操作禁止記録の解除APIから `worker_reaped` の自己申告を除去し、呼出元が保持する二つのプロセスhandleを受け取る。解除前に `GetProcessId` で別プロセスであること、ゼロ待機で両方が終了済みであること、`GetExitCodeProcess` が両方0であることを確認する。稼働中・非0終了・同一プロセスの重複・非プロセスhandleは解除せず、そのleaseでの再試行も拒否する。プロセスを強制終了したり、終了を待ち続けたりはしない。

これはOS終了確認の実装であり、SDK終了の証明ではない。Live View/Source/Module終了フラグは依然呼出元の申告である。実controllerによる起動handle保持、generation/個体/終了通知との照合は未接続で、無関係な正常終了processを渡せない契約も今後の統合対象。実二台SDK起動はまだ許可しない。

検証: stub構成 `cmake --build build/lease-marker-stub --config Release --target a0_hardware_process_lease_delegation_tests` exit 0（既存C4819警告あり）。`ctest --test-dir build/lease-marker-stub -C Release -R '^hardware_process_lease_delegation_contracts$' --output-on-failure` は今回1回、1/1 PASS、0.68秒、exit 0。正常終了した二つの実Windows子プロセスで解除可能、稼働中/exit 91/重複/signaled eventでは隔離維持と再試行拒否を確認。従前の欠落8項目・削除失敗・統括異常終了試験も含む。カメラ/SDK/WPDは未実行。これまでの同試験系列は計4回、実機preview枠は2/5のまま。

本試作は、統括と CAM-A/CAM-B の二 worker 間で模擬フレームを IPC する試験専用の実装である。実機 Live View、撮影、カード操作、設定変更を行わない。初期IPC試験はSDK/WPD依存なし。後続の排他統合試験は既存HardwareProcessLeaseを使うためcoreへリンクするが、SDK rootを空にしたstub構成で、SDK/WPD APIは呼ばない。SDK の同一 Module/同一 Source の制約を回避・緩和する実装ではない。製品UI・実機Agent・本番leaseへの組込みは未実施。

## 境界

- テスト統括が generation と worker 別 token を渡し、限定継承した統括プロセス監視ハンドルを持つ child だけを起動する。監視handleなしの command line 起動は pipe を開く前に拒否する。worker は専用の常駐 pipe へ generation/token/alias/sequence/偽Open trace/模擬payload を返し、統括は応答を検証して最新一枚のmailboxへ渡す。tokenはテスト固定値であり、本番認証・悪意ある同一ユーザーprocessへの耐性の証明ではない。
- token は worker 起動専用で、camera Source ID・候補 ordinal・物理個体 ID を表さない。プロセス間で SDK Source ID が安定または同一とは仮定しない。
- 統括の観測処理は実worker process handleを確認し、片worker終了時にterminalへ移して残るworkerへ明示STOPを送る。受信失敗でもterminalとする。各操作前後の観測であり、本番用の常時監視serviceは未実装。再接続、worker再起動、フレーム再送、撮影IPC送信はしない。
- coordinator は capture/WPD への遷移を常に拒否する。Live View 停止・SDK source/module 解放の実証を worker process 終了で代用しない。
- IPC の `frame` は偽transportのバイト列であり、画面・ログとも実機 Live View と表示してはならない。
- 試作 wire は最大1 KiB、各I/O待機の期限は1500 ms。取消後も同じ時間内にcompletionが収束しなければ試験processを終了し、未完了OVERLAPPEDを解放しない。試験全体のCTest期限は10秒。これは偽payload専用であり、256 KiBの実フレーム伝送、SDK撮影の180秒watchdog、実機通信性能の証明ではない。
- 子processは試験専用Jobで管理し、失敗時も外側runner終了時に回収する。親死亡検証は別の監視専用workerではなく、通常と同一のIPC workerへ注入する。強制終了の対象は試験で生成した偽processだけであり、実カメラprocessへの強制終了方針ではない。
- pipe断が親process終了通知より先に観測される場合があるため、読取り失敗後に最大1500 msだけ親終了を観測して原因を判定する。これは追加I/O・再接続ではない。各I/O期限と終了収束期限は別で、合計が1500 ms以内という主張ではない。

## 2026-09-22 検証記録

- base: `7c5c2c6`、初期試作: `183a3cc`。変更は隔離branch `codex/dual-live-worker-poc-20260922` のみ。本体アプリ・SDK/WPDは未実行。
- 初回の狭いIPC試験はPASSだったが、独立レビューでwire受信とCoordinatorの未結線、実process死亡監視不足などを発見。初回PASSを完成証拠としない。
- focused CTest実行は計5回（初回PASS、途中3回FAIL、最終PASS）。途中失敗は応答喪失、および親終了通知とpipe断・Job終了反映の競合。条件を無変更で反復せず、原因を修正して実行した。上限到達後の追加試験なし。
- 最終Release build成功。`ctest --test-dir build/dual-live-worker-poc -C Release -R '^dual_live_worker_poc_contracts$' --output-on-failure`: **1/1 PASS、5.80秒、exit 0**。ローカル詳細ログは `build/dual-live-worker-poc/Testing/Temporary/LastTest.log`（ignored）。
- 確認項目: 同時常駐するA/Bの複数wire応答→個別token/alias/generation/sequence/偽Open traceの検証→最新mailbox、古いsequenceと別worker応答の拒否、実worker終了からterminal/兄弟STOP、実parent終了から同じworkerのexit 7、通信応答失敗からterminal/兄弟STOP、wire上限超過、不正sequence/token、無通信期限、監視handleなし起動、偽Source選別、Job内残存child 0。
- 独立read-onlyレビューで重大指摘の修正を確認。ただし長期安定性・本番認証・実画像の転送・実機安全性の受入ではない。

## 次の設計条件（実機経路は無効のまま）

1. worker-local Sourceと物理カメラA/Bの対応をどう確定し、再起動・再列挙で失効させるかを設計する。番号や列挙順のprocess間一致は仮定しない。
2. 現行のoperator-session-wide leaseを統括が保持し、その配下workerに限定委譲する契約を別途設計・検証する。試作でleaseを外したり、本番を複数session可に変えたりしない。
3. SDK/driver共有の実現可能性と両側stop/close証拠が得られるまで本体へ接続しない。worker process終了はSDK解放証明ではない。

## 後続Goal: 個体割当と限定委譲（2026-09-22）

本人の「後続の作業についてもgoal化して進めてください」によりGoalを作成。基点 `33bed76`、branch `codex/dual-live-worker-poc-20260922`。実機なしの設計・契約実装・レビュー・限定検証を進める。SDK/WPD起動、撮影、設定変更、AOPC-22-NOTE、本番有効化、merge/公開は含まない。

### 今回の契約モデル

`dual_live_session_contract.hpp` と専用 `dual_live_session_contracts` は純粋なoffline契約モデルである。後続の `dual_live_session_integration` が実child processのIPCと試験専用名のHardwareProcessLeaseに結線する。製品へ組み込むruntimeではなく、software-onlyの成立性を確認する試験用hostである。

- worker内でのみSource番号を保持し、instance/epochが異なる選択、未検出・重複候補、再Openをfake transport callback前に拒否。二workerのSource番号が同じでも異なっても物理個体対応を推論しない。
- binding receiptは模擬カメラの独立したfixture identityであり、SDK Source/serialの証明ではない。同じsynthetic body、worker instance、receiptの二重割当を拒否する。実環境にはその対応入力が未確立のため、モデルのReadyは`SimulatedReady`でしかない。
- 委譲grantはcontroller/epoch/alias/worker instance/receipt/個別capability/発行時刻/期限に限定。期限到達、時刻後退、連番再送、別workerの借用、lease不在/abandoned、親終了・topology変更で全grantを失効させる。後でleaseが得られても旧sessionは復活しない。
- quiesce開始でpreview権限を止め、両workerのpreview停止・Source close・Module closeの明示receiptが揃うまでClosedにしない。process終了だけはclose証拠にしない。Closed後は旧割当を再利用せず、遅延通知はClosedを破壊しない。
- `RealSdk`、`Wpd`、`Capture`は常時拒否。lease観測・時刻・close receiptはsimulationから注入している値で、実mutex所有、OS process認証、実SDK teardownの証拠ではない。

### 検証・現在地

- 新規モデルだけをRelease buildし、focused CTestは2回、いずれも1/1 PASS（最終0.74秒）。前段の5回IPC試験は再実行していない。
- fake Open/frame callbackと認可を接続し、親失効後の両worker callback停止を確認。独立レビューの発行時刻・Closed遅延通知に関する指摘は修正し再試験した。
- コマンド: `cmake --build build/dual-live-worker-poc --config Release --target a0_dual_live_session_contract_tests`、`ctest --test-dir build/dual-live-worker-poc -C Release -R '^dual_live_session_contracts$' --output-on-failure`。
- 後続3回目は `ctest --test-dir build/dual-live-worker-poc -C Release -R '^dual_live_session_integration$' --output-on-failure`: **1/1 PASS、3.06秒、exit 0**。出力は `mode=simulation, hardwareAllowed=false, status=PASS, failures=0`。後続シリーズは計3回/上限5回（モデル2回、統合1回）。前段5回のIPC suiteは再実行していない。
- `NIKON_D810_SDK_ROOT=` のstub構成でintegration/model/旧IPCのRelease build成功。初回integration buildの既存例外API名の誤記は修正済み。旧IPCのutilityを共通headerへ機械的抽出し、utility本文と残る旧test本文の文字列同一性を確認。旧試験の実行結果を新protocolの証拠としては使わない。
- **software-only Goalの受入項目は完了**。実processのreceipt→model grant→wire→worker gate→fake frame→両mock closeを接続。統括だけが `A0.Poc.TestLease.*` の既存HardwareProcessLeaseを保持し、別process probeが保持中は正確に `camera_control_busy`、解放後は取得成功する。workerはmutex名/handleを受け取らず、production lease名は未変更。
- grant全8 fieldの改変・別worker grant・期限不正をOpen/frame callback 0で拒否。SDK/WPD/CAPTURE opcodeと連番再送を追加frame callback 0で拒否。統括共通ReceiveがDENIED観測で全grant失効→拒否側ACK終了→兄弟STOPを行い、両process非生存を確認する。両mock close receiptの前にはClosedにせず、閉鎖後もhardwareは許可しない。全試験後のJob残存childは0。
- 独立read-onlyレビューの「兄弟停止がテストからの手動呼出しに留まる」指摘は共通Receiveへの接続で修正し、再レビューでP1/P2なし。試験wireは最大1 KiB、各I/O期限1500 ms、全体CTest期限15秒。既存試験utilityのbounded I/O、限定handle継承、Job回収を再利用した。
- 限界: 新grant protocolの親死亡・OS lease abandonmentの故障注入は未実施（モデル検証と旧protocolの証拠は代用しない）。試験のbootstrap identity/tokenは固定値であり、production認証・悪意ある同一ユーザーへの耐性は証明しない。SDK/driver共有、実個体識別、実SDK stop/close、長時間連続動作、製品UIへの組込みは未実施。

### 将来の排他・個体対応設計

実機経路へ接続する場合、統括が現在のoperator-session-wide leaseを唯一保持し、workerへ渡すのは所有そのものではなく親の生存期間に限ったpreview権限だけとする。子workerは独立lease取得/解放、leaseなし起動、親喪失後の操作を行わない。実HardwareProcessLeaseの既定名・既存一台制限は今回変更しない。

worker-local映像の操作者確認は候補だが、二workerが別物理bodyを開いた証拠は別途必要。親のSource token移送・列挙順対応・同一番号一致判定は禁止。担当外カメラをOpenしない選択方法が確立しない間は実機workerを有効化しない。全module close後は新generation・再割当が必要で、自動再接続しない。

### software-only Goalの受入境界

このGoalの完了は製品の二台同時Live View完成を意味しない。次を区別して記録する。

| 項目 | このGoalの受入条件 | このGoalでは証明しないこと |
| --- | --- | --- |
| 割当 | 各workerの模擬receiptをIPCで回収し、接続世代・worker・aliasの対応を契約で検証 | Source番号からの物理個体特定、実機二台の区別 |
| 限定委譲 | IPC grantをworker側でも検証し、偽preview以外の要求を拒否 | SDKを二つのprocessから使えること、本番IPC認証 |
| 排他 | 既存HardwareProcessLeaseを試験専用名で統括が保持し、別processの取得を阻止。解放後の取得も確認 | 本番leaseの変更、別Windows logon sessionからの操作 |
| 終了 | 両workerの模擬停止・close receiptを確認してからClosedへ遷移 | Nikon callback収束、実Source/Module teardown |

実機移行前には、担当外Openを起こさない個体選択方法と、SDK/driverの共有可否について別途根拠が必要。どちらも今回のsynthetic fixtureやsoftware-only PASSで代用しない。FR-LV-003と既存の本番一session制限は変更しない。

### 別承認の実機PoC案（未実行）

まず資料と実装レビューで対象外Open防止・個体選択方法・既存排他との整合を確認する。成立しない場合は実機試験へ進まない。承認時に候補SHA、PC、対象カメラ、操作・期限・回数を固定する。以下は最大5回の上限案であり、必要な回だけ行い失敗補充しない。

1. 各workerの単体選択・映像確認・正常停止（単体A）。
2. 同じ確認を単体Bで実施。過去の撮影成功だけで新workerの合格にしない。
3. 二workerが別物理bodyを同時保持できるか、担当外Openなしで確認する。
4. 両側の新しい映像更新と、明示停止→両Source/Module closeを確認する。
5. 別に指定した正常終了または人手の接続変化シナリオで失効・両側停止・再利用禁止を確認する。故障注入内容は実行前に明示する。

全回で撮影・カード操作・WPD・カメラ設定変更をしない。識別不明、対象外Open、片側停止/close未確認、電力/温度異常は中止し、再試行・worker再起動・サービス操作・強制killを行わない。実機SDK/WPD非重複や撮影handoffは、このpreview-only試験の合格とは別工程とする。

## 実機接続の第一段階: 一台限定worker（2026-09-22、検証中）

本人の「実機検証を始めて」「進めてください」に基づく実装。PCはAOPC-11-NOTE、BのUSBを抜いて一台だけとする準備を本人が確認済み。初回は接続中の一台だけを対象とし、3フレームの取得を1回の試験として数える。最大5回、自動再試行なし。これは物理CAM-A/Bの対応証明ではない。

- 新CLI `A0CameraStitcher.SingleWorkerPreview describe` はSDKを開かない発見操作。`sdkAvailable` はSDK有効buildかつ実行時moduleパスの配置検査成功を示し、実ロード・カメラ通信成功の証拠ではない。
- `preview-single --confirm-one-physical-camera` は実機操作。SDKのraw Sourceが厳密に一つでなければSource Open前に拒否し、唯一のSourceだけを一回開いてD810であることを確認する。既存の全候補Openによる識別経路は使わない。Source IDを保存・process間転送しない。
- 開始前にLive View OFFを要求し、既にONなら自動OFF復旧しない。SDK moduleのcallback通知と列挙結果の集合を再確認してから各フレームを取得する。画像はメモリー上のpreviewのみで、撮影・保存・カード操作・WPD・撮影設定変更はない。
- この単体commissioning段階では**一台のworker自身**が既存production HardwareProcessLeaseを全期間保持する。二workerへのlease委譲は未実装であり、前段の試験用IPCへSDKを接続したという意味ではない。二台同時操作は無効のまま。
- 各SDK操作には既存の期限を渡し、新規操作の全体期限は60秒。停止とCloseは期限後も行う。SDK関数自体が戻らない場合の強制終了はしない。開始成功時のStopは一回だけ、明示Closeと `InspectDualSessionExitState().FullyEnded()` を確認する。開始結果不明、Stop/Close不成立、abandoned leaseでは同じthreadがleaseを保持したまま `quarantined` とし、人手復旧を待つ。process終了やtimeoutをClose成功に読み替えない。
- 結果はJSONの `mode=hardware`、`passed/failed/quarantined`、frame数・byte数・stop/close確認、`bindingProof=false`。例外本文・個体番号・SDKパスは出力しない。結果不明でもコマンドを再送しない。隔離されたprocessを自動killしない。
- privateなSDKの一組だけをCMake rootとし、実行processだけに `NIKON_D810_SDK_MODULE_PATH` を設定する。SDK素材はコピー・commitしない。本体UI/既存camera agentは変更しない。

機械操作標準は部分対応: 発見・限定CLI・状態/結果出力は実装、承認の自動検証・A/B binding・二worker委譲・本体UI操作は未対応。実機起動時は本人の対象/回数/禁止事項、正確な候補版とOS上一台を操作者が照合する。コマンドの確認flagそのものを承認証明とは扱わない。

### 検証記録

- 独立レビューで発見したabandoned lease解放、開始拒否後の不要Stopを修正。開始途中の状態変化でも既存ONを自動OFFにしないstrict経路、callback由来Sourceのtopology確認を追加。
- SDKなし構成のRelease build成功。`single_worker_preview_contracts` は1/1 PASS（0.13秒）。raw Source 0/2/重複でOpen 0、唯一SourceのOpen一回、3frame成功、Open/Start/topology/frame/Stop/Close失敗時no-retry、開始拒否時Stop 0、終了不明時quarantine、期限切れで開始禁止を確認。
- SDK有効構成のRelease build成功。新規lifecycleと既存dual-session adapterのsoftware-only回帰試験は2/2 PASS（0.48秒）。本変更のCTest実行は2回、前工程の5回IPC suiteは再実行なし。`describe` のSDK指標は環境設定にも依存するため `sdkBuilt` という初稿の名前を `sdkAvailable` に訂正。coreに既存の文字コード警告C4819があるがbuild errorはない。実機結果は実行後に追記する。
- 前段software-only Goalの完了記録を、本実機接続や二台動作の完成証明へ読み替えない。
- 実機候補は `e8986dc`、Release exe SHA-256 `EB5C29A2E87D99AE35527011EC11F2506A53190BF7716965DD46CEC6E6C7ADF7`。実行processへのmodule設定後の `describe` は `sdkAvailable=true, dualEnabled=false, bindingProof=false`。
- 実機1回目の事前チェックで中断。本人の一台準備後にはOSでD810一台を観測したが、実行直前は0台となったため、SDK/カメラOpenより前に拒否。実SDK起動・Live View開始・撮影・設定変更はいずれも0回、hardware-run-01.logも未作成。読取りで0台を再確認し、本人へAの電源/USB確認を依頼した。接続回復待ちであり、実機PASSではない。試験枠は0/5消費、再送なし。
- 本人の再接続連絡後、OSで正常なD810一台、関連camera processなし、上記exe hash一致、既存run-01ログなしを確認して実機1回目を実行。`preview-single --confirm-one-physical-camera` はexit 0、`status=passed`、`frames=3`、`bytes=52219`、`stopConfirmed=true`、`closeConfirmed=true`、`bindingProof=false`。終了後の関連camera processは0。rawログはignoredの `build/single-worker-sdk/hardware-run-01.log` に保持し、画像の保存・撮影・設定変更・WPD操作は行っていない。
- 実機枠は **1/5消費、単体preview 1回合格**。接続対象は本人がAとして準備した一台であり、永続個体IDやA/B bindingの証明ではない。次は本人によるAからB一台へのUSB切替待ち。二台同時Live View、二worker実SDK委譲、製品UI統合は未確認・未完了のまま。
- 本人の「接続しなおしました」を受け、Bとして準備された一台で実機2回目を実施。同じ候補exe hash、OSで正常D810一台、関連camera processなし、run-02ログ未存在を照合。exit 0、`status=passed`、`frames=3`、`bytes=122799`、`stopConfirmed=true`、`closeConfirmed=true`、`bindingProof=false`。終了後の関連camera processは0。ignoredの `build/single-worker-sdk/hardware-run-02.log` に保持。撮影・撮影設定変更・WPD操作・画像保存・再試行なし。
- 現在の実機枠は **2/5消費、操作者指定A/Bの単体preview各1回合格**。永続的な個体照合は行っていない。二台同時への移行には担当外Openを防ぐ個体選択と実worker委譲経路が必要で、単体成功を根拠に同時接続経路を有効化しない。

## 既存経路のOpen前フィルターに関する制約

現行 `NikonSdkTransport::BeginDualSession` は `D810SourceIds` が各候補を Open して機種を判定する。この処理を worker に流用すると、worker 担当外の Source を Open する。`SelectAssignedSourceBeforeOpen` は列挙済みの worker-local assigned Source だけを選択し、重複・未検出なら Open 計画を返さない。

これは実機 identity 解決ではない。将来の実機試験では、各 worker が自分の session-local 映像で担当を確認し、worker-local Source を得る方法、二つの物理個体に対する対応、driver/module 共有可否を別途証明するまで実機経路を遮断する。

### Worker-local selection のソフトウェア準備（実機未実行）

`WorkerPreviewSelection` は、一つの worker/module が列挙した **厳密に二つの異なる raw Source ID** だけから、同worker・同module世代で一回だけ使える不透明トークンを作る。これは CAM-A/CAM-B、シリアル番号、USBパス、または物理個体の証明ではない。Source を Open する前にその対応を得る公開SDK APIは確認できていない。

- inventory の差分、Add/Remove（既存IDの再Addを含む）、不正/旧世代トークン、SDK callback例外は選択を恒久的に失効させ、再試行・自動再割当をしない。
- generic `StartLiveView` を通る場合にも、選択済みSource・不変inventory・初期Live View OFFを強制する。終了時の `ReleaseSession` は選択トークンを破棄する。
- このAPIはCLI/UIへ公開しておらず、既存の production lease にも接続していない。SDK/WPD/カメラへの実行は **0回** である。
- focused software test は新規build directoryで実行し、実機試験カウンタは **2/5のまま** とする。二台同時Live Viewのidentity/lease/SDK-driver共有のgateは未解決である。
- これらのselection試験はheader-onlyの契約callbackとSDKなしの既存single-worker fake lifecycleを対象にする。実SDKが例外を返した場合のSource/Module状態を注入・証明する試験ではない。
- 新規の `build/worker-selection-stub` と `build/worker-selection-sdk` で、それぞれ `worker_preview_selection_contracts` と `single_worker_preview_contracts` は 2/2 PASS。前者はSDK stub、後者はlicensed SDK headersを有効にした**コンパイルとsoftware-only test**であり、どちらもSDK DLL/カメラを実行していない。focused CTestは有効実行2回（各2/2 PASS）。初回のstub CTestはexe生成前のNot Runで、合格・実機回数のいずれにも数えない。
- controller消滅時に既存CLIがabandoned leaseを警告だけで継続し得る経路は今回の対象外であり、二worker SDKを有効化する前に全入口を止めるquarantine/guardianが必要である。worker process終了をSource/Moduleの正常Close証明として扱わない。

### 二worker委譲の永続停止記録（実装・検証中）

- `HardwareProcessLease` の既定本番名は、同一Windows sessionの固定ローカル領域に委譲markerがあれば、SDK/WPDへ入る前に拒否する。controllerはworker起動前にmarkerを作成・flush・再読込し、destructorやprocess消滅では解除しない。旧binaryには適用されず、二worker運用前に入口を同一対応版へ揃える必要がある。
- marker解除は両側のLive View OFF・Source close・Module close・worker回収を表すtyped evidenceが必要。ただし現状はcaller assertionであり、実SDK/IPCから証拠を作るcontrollerは未接続。これだけで実機二workerを有効化しない。
- 独立レビューで固定ローカルdriveとroot自体のreparse拒否不足を検出し修正した。本番marker保存先のoverrideは拒否し、テスト専用lease名と隔離rootだけで検証する。人手復旧CLIは未実装、自動解除しない。
- 初回の専用buildは主担当がexit 0を回収。`hardware_process_lease_delegation_contracts` は1/1 PASS、0.40秒、exit 0。模擬子processの`ExitProcess(91)`によるdestructor非実行、次owner拒否、同thread再入拒否、不完全終了証拠の拒否、正常解除を確認。これは実SDK異常終了試験ではない。
- 上記PASS後に固定drive検査と機械整形を加えた最終差分も、専用build exit 0、同CTest 1/1 PASS（0.60秒、exit 0）。このseriesのCTestは計2回。既定本番名のstorage分岐を隔離環境で直接通す試験と、I/O故障注入の網羅は残る。実機操作0回、preview枠は2/5のまま。
- 追加負例をまとめた3回目もbuild exit 0、同CTest 1/1 PASS（0.69秒、exit 0）。A/B各4終了項目の一つずつの欠落、失敗後に完全証拠を渡しても解除不可、readonly markerによる削除失敗・その後の再解除拒否、本番保存先overrideと相対path拒否を確認。計3回でこの試験seriesを終了。write/flush故障、既定本番storage分岐、実controller/SDK証拠の接続は未検証として残す。

## 親processの起動・終了接続（2026-09-22、software-only）

- `PreviewWorkerOwner` と内部専用 `A0CameraStitcher.PreviewWorker` を追加。本番lease取得・marker永続化・二子process登録の後だけ、継承した匿名pipeで起動情報を渡す。capabilityはコマンドラインへ出さず、継承handleをparent参照とbootstrap readerだけに限定する。
- OSのpipe server PIDを登録済みworkerと照合し、epoch・sequence・終了応答の全5項目、ACK、両processのexit 0を確認した場合だけmarker解除へ進む。Closeは一回限り。失敗・期限切れ・destructorではmarkerを消さず、自動kill/restartもしない。
- 親側APIは起動・closeだけを公開。候補列挙・個体確認・preview grantは未接続で、実機二台操作や製品UIを有効化していない。構築・Close・破棄は同じlease所有threadで行う。
- SDK stub build exit 0。新規 `preview_worker_owner_contracts` を1回実行し1/1 PASS（0.59秒、exit 0）。実際の二子processで正常close後のmarker解除、期限切れexit 3、解除拒否・再取得拒否を確認。テスト専用lease/rootであり本番markerとカメラには触れていない。
- SDK有効構成のworker buildもexit 0。既存headerのC4819警告あり。SDK版実行、カメラOpen、撮影、WPDは0回。preview実機枠は **2/5のまま**。
- 次工程は段階的commissioning・異なる物理個体の明示確認・同時preview grant。異常な親終了・実SDK解放失敗・製品UI連携の受入は未完で、今回のprocess試験で代替しない。
- 追加の起動段階の親死亡試験: 専用stub helper内で実際の `PreviewWorkerOwner` を構築し、外側observerが二workerのprocess handleを保持した後、helperが `ExitProcess(91)` でdestructorを通らず終了する。二workerが正常完了ではないexit 2（bootstrap前の親消失）または3（hostでの親消失）で終了し、marker残存と次owner拒否を確認する。10秒のworker寿命より短い待機で親死亡への収束を検査し、両workerと親の終了が不明な場合はtest markerも消さない。テストは実SDK操作中の親死亡を証明しない。
- この追加後のstub build exit 0、`preview_worker_owner_contracts` 1/1 PASS（0.96秒、exit 0）。今回CTestは1回、当owner試験seriesの累計2回。実機操作0回、preview枠2/5のまま。カメラ操作を伴う二台同時試験はまだ実施していない。

## 実機 PoC（未実行）

別承認後でも最大 5 回。各回は、二 worker の SDK 初期化、Source Open trace（担当外 Open 0）、両 Live View の停止、source/module 解放、全体 lease が保持されたこと、capture/WPD 未遷移を記録する。今回の試作はその承認・実行を含まない。
