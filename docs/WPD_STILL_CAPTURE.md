# 開発用WPD撮影・PC保存 (#332)

開発時の所有者の「それぞれに撮影コマンドを送れてそれぞれ保存」を根拠にした開発用経路。SDKなし、登録済みD810 A/B二台を厳密照合する。通常のネイティブプレビューの各側に撮影・保存ボタンを置く。撮影前に両側の転送終了、両側のStop、両側のCloseを順に確認する。対象一台だけを新しく開き、identityを再確認する。成功した場合だけ新しい二台分のchannelでLive Viewを再開する。旧channel・失敗channelを再利用しない。カメラ制御leaseは主threadが列挙前から保存と全session終了まで保持する。

公開MITの固定commitを根拠にしたno-AF SDRAM capture `0x90C0 / 0xFFFFFFFF`を一度だけ送る。AFは撮影前の別操作であり、撮影時のAFやシャッター同期は保証しない。`0x9207`、AF-and-capture、設定write、カード削除・format、シャッターretryは許可しない。利用する公開識別子とライセンスは `third_party/digicamcontrol-NOTICE.txt` に記録する。Nikon SDK同梱仕様書は利用しない。

事前にLiveViewStatus=0、JPEG Fine only、7360×4912を読み取る。設定が違えば変更せず拒否する。二回の事前イベント読み取りに未回収画像があれば拒否する。撮影後の新規SDRAM objectイベント一件にだけobject handleを結び付け、JPEG ObjectInfo、exact size、分割転送と最終ENDを確認する。追加・重複objectや未知イベントを成功扱いにしない。イベントコードだけを診断し、object handleやカメラ内ファイル名を公開ログに出さない。

継続開発時の#343修正: 事前のStoreAdded（0x4004）は新規画像と区別し、DeviceInfoChanged/LiveViewStateChangedと同様に撮影条件の再読取を要求する。その後、もう二回の事前イベント確認を行い、未回収画像/完了通知や再度の状態変化があれば撮影せず失敗する。再読取は一回だけで、安定するまで待ち続けない。撮影後のStoreAdded、StoreRemoved、reset、未知イベントは停止のまま。通知の意味は[公開資料](../third_party/PTP_PROTOCOL_REFERENCES.md)、この限定した再確認方針は本アプリ独自の安全条件であり、通知発生の原因を確定したものではない。カード列挙/削除・設定write・追加シャッターは送らない。

同日の#345では両台撮影に限り、両Liveの停止・Close後に、A/B両方の非撮影preflightとCloseを先に行う。どちらかが拒否/Close未確認/期限/停止ならAの露光も0。実際のA/B撮影時にもpreflightを再実行するため、先の観測を撮影許可tokenや画像帰属へ置き換えない。全工程は元の一組180秒deadlineを共有し、読み取り専用経路の最後の送信境界でもシャッター/object取得を拒否する。単独撮影のAPIは維持する。

`--pair-preflight-diagnostics` は同じ登録A/Bを非撮影で確認してCloseする明示診断。原画像やtransport画像partialを作らず、private `wpd-preflights` に状態journalだけを保持する。ウィンドウ終了要求は未送信の露光を止め、既に送信済み一枚は元のdeadline内で回収・保存を続ける。次のカメラは撮らない。事前確認で拒否した場合は画面で「A/Bの撮影0」と区別し、撮影失敗後の自動再開は行わない。

取得したchunkはprivateな `*-transport.jpg.partial` に逐次保持する。最終転送、JPEG全画素復号と実寸、SHA-256、atomic original.jpg publish、再読込size/hashを検証する。保存先は `%LOCALAPPDATA%/A0CameraStitcher/wpd-captures/<run>/capture/CAM-A|CAM-B/original.jpg`。実画像・EXIFはリポジトリやIssueに載せない。成功時もtransport partialを診断用に保持する。180秒を越えて次工程・成功状態へ進めず、取得済みのpartial/originalを消さない。未確定シャッター後に帰属可能な候補を回収できた場合もprivate partialに留め、canonical originalには昇格させない。

12秒停滞したI/OはCancelを一度試み、Closeが返るまでleaseを保持する。Open/identityのdriver内部停止やCancel自体の停止を強制終了で安全と扱わない。保存成功とsession終了の両方を確認できない場合、プレビューは再開しない。

`--seconds 30 --capture-a` / `--capture-b` は両側のoff/close確認後に一台だけ撮影する実機診断。`--seconds 60 --capture-smoke` は実際のUIコマンドを経由してA撮影・再開・B撮影・再開を行う明示テスト。通常起動では自動撮影しない。ソフトウェア契約検査は `wpd_still_capture_contracts`、既存転送検査は `wpd_live_view_transfer_contracts`。実機合格の有無は開発状況記録を参照し、偽channel合格を実機成功へ読み替えない。

## 両台撮影の順番とセッション (#389)

両台撮影（開発用プレビューの「両台撮影 → 仮Merge保存」）は、次の順を一つの180秒transaction（一つのdeadline）として行う。SDKのセッションは開かない。WPDのみ。

1. 両台の事前確認: A、Bの順に、開いて非撮影のpreflightを読み、Closeする（#345）。どちらかが拒否・Close未確認・期限・停止ならAの露光も0。Aが本体自身の状態（残っていた通知、撮影設定、準備中、ピント）で止まり、AのCloseを確認できた時は、Bも同じ確認とCloseを行う（#430、後述）
2. A、B両方のWPDセッションを開き、aliasを照合し、撮影直前の状態（Ready、撮影設定、イベントの基準線）を再確認する。ここまで露光しない
3. Aのシャッター（`0x90C0 / 0xFFFFFFFF`）を一度だけ送る。受付を確認したら、すぐにBのシャッターを一度だけ送る
4. Aの画像を取り込み、PCへ保存して検証する。続けてBを同じ手順で行う
5. 両方の完了イベントを見届け（後述）、両方のセッションをCloseする

**セッションの持ち方**: 手順2から5まで、A、Bの二つのセッションを開いたままにする。理由は、シャッターの間に入る時間を減らすため。Bを開く、aliasを照合する、事前確認を読むは、それぞれ数百ミリ秒かかる。Aのシャッターの後にこれらを回すと、その分だけ露光がずれる。Aを送ってからCloseすると、Closeの待ちも入る。画像はカメラのSDRAMに残るので、取り込みは両方のシャッターの後に回してよい。WPDの呼び出しは一つのスレッドから順に送り、二台へ同時にI/Oしない。12秒の停滞watchdogは各セッションが持つ。カメラ制御leaseは従来どおり全セッションのCloseまで保持する。

**時刻差**: `shutter_gap_ms` は、Bのシャッターをtransportへ渡した時刻から、Aのシャッターを渡した時刻を引いた整数ミリ秒（steady clock）。Aのコマンド応答の待ちを含む。ログは `pair_shutters shutter_gap_ms=<n>`。片方しか送っていない時は `pair_shutters shutter_gap_ms=none a_dispatched=<0|1> b_dispatched=<0|1>`。これは測った値であり、同時露光や上限を保証しない。目標の値は所有者が決める。

**途中で止まった時の原画像の扱い**（従来と同じ規則。partialを残し、canonicalに昇格させず、再試行しない）:

| 止まった場所 | A | B |
|---|---|---|
| 再確認（手順2）の拒否 | 露光なし | 露光なし |
| Aのシャッターが拒否・未確認 | 送信済みで未確認ならprivate partialだけ回収 | 露光しない |
| Bのシャッターが未送信（停止・期限・拒否） | 通常どおり取り込み・保存 | 露光しない |
| Bのシャッターが送信済みで未確認 | 通常どおり取り込み・保存 | Aの後に候補をprivate partialだけ回収。originalにしない |
| Aの取り込み・保存が失敗 | partialを残し、originalにしない | 露光済みなので一回だけ取り込む。再試行ではない |
| Bの取り込み・保存が失敗 | 保存済みのoriginalを保持 | partialを残し、originalにしない |
| 停止要求 | 未送信なら露光しない。送信済みなら取り込み・保存を続ける | 同左 |
| 期限切れ | 以後のtransport、rename、成功状態を始めない。取得済みのpartial、originalは消さない | 同左。両方のシャッターの後に切れた場合、取り込めなかった画像はカメラのSDRAMに残り、保存されない |

AのOutOfFocusは、撮影前の確認で気付けなかった場合にだけ、Aのシャッターの後のDeviceReadyで分かる。この確認はBのシャッターの後（Aの取り込みの先頭）になるため、その時点でBは露光済みになる。Bの取り込みは一回だけ試み、両台撮影は失敗として止まる。

## 完了イベントの見届け (#381)

完了イベント（`0xC102` または `0x400D`）が画像の取り込みより後に届くと、カメラのキューに残り、次の撮影の事前確認が読んで `pending capture completion` で止まる。このため、各カメラの取り込みが終わった後、同じセッションでそのカメラの完了イベントが届くまでイベントを読み続けてからCloseする。シャッターを送ったのに取り込みへ進めなかったカメラ（ピントが合わない、取り込みの失敗など）も、Closeの前に同じ見届けを一度行う（#430、後述）。

- 待つ上限は設計定数 `kWpdStillCompletionWaitMax` = 5秒、間隔は100ミリ秒（`kWpdStillCompletionPoll`）。カメラごとに数える。両台で最大10秒で、180秒の期限の十分内側に収まる。SDRAMのイベント待ち（15秒）と同じ撮影の完了通知なので、その三分の一を上限とした。完了イベントの遅れを測った値はまだなく、この値は推定である。実機のログの `waited_ms` を見て見直す
- これは観測であり、再試行ではない。何も再送せず、見届けられなくても検証済みの撮影を失敗にしない。待ちの途中でdeadlineや停止要求でI/Oが許可されなくなったら、待ちを打ち切って（`aborted`）撮影の成否は変えない
- 待ちの間に届いた他のイベントは従来の規則で扱う。二つ目の完了イベント、記憶領域・メタデータの変化、未知のイベントは、撮影後なので終端で、取得済みのpartialを残す
- 既存の観測ログ `<alias> capture_completion shutter_attempted=<0|1> complete_event=<0|1> complete_event_code=<0xNNNN|none>` と、失敗時の `code=0x%04x phase=before_shutter|after_shutter` は変えない。待ちの結果は別の行 `<alias> completion_wait result=<already_seen|seen|timeout|aborted> waited_ms=<n> max_ms=<n>` に出す。`timeout` が、完了イベントを見届けられなかった撮影
- 失敗した撮影でも見届けが完了イベントを読むことがある（#430）。その時は `capture_completion` の行に `complete_event=1`、`completion_wait` の行に `result=not_reached` が並ぶ。後者は成功した撮影の待ちに入らなかったという意味で、完了を読めたかどうかは前者と `completion_after_failure` の行で読む
- 続けて二台を切っても、完了イベント、SDRAMの画像、観測はカメラ（セッション）ごとに持つ。各カメラは自分のセッションのキューだけを読むので、別のカメラのイベントを数えない。二台のobject handleが同じ値でも、取り込みはそのセッションが認めたhandleだけに結び付く。万一、同じカメラの別の完了イベントや画像イベントが混ざれば、重複・曖昧として終端になる

## 撮影前の点検の記録、A が止まった時の B、失敗した撮影の見届け (#430)

撮影前の点検がカメラに残っていた通知で止まり、アプリの起動し直しが何度も要る問題への対応。AGENTS.md の規則は変えない。シャッターは点検のどの段でも送らず、新しい命令の種類も足さない（通知の読み取り `0x90C7` とCloseは従来から使っている）。通知の番号の出典は[公開資料](../third_party/PTP_PROTOCOL_REFERENCES.md)。

**点検が読んだ通知の記録（C1）**: 点検は通知を `0x90C7` で2回読み、撮影前に来てはいけない通知が一つでもあれば止める。止めるかどうかの判定は従来のまま、最初に引っかかった通知で止まる。判定の前に、その回の全ての通知を番号ごとの数で数え、止まった時も含めて次の行に残す。通知の引数（object handle、storage ID）と機体の識別子は数えず、書かない。成功した点検にも同じ行が出る。

`<alias> preflight_events stage=<check|recheck> polls=<n> codes=<0xNNNN:n,...|none> refresh=<0|1> result=<ready|refused> refused_code=<0xNNNN|none> kind=<none|event|live_view_on|quality|size|not_settled|not_ready|out_of_focus|identity|transport|deadline_stop>`

`check` は露光の前の非撮影の点検、`recheck` は露光の直前の確かめ直し。`polls` は `0x90C7` を読んだ回数（読み直しを含む）、`refresh` は撮影条件の読み直しが一度要求されたか。

**A が止まった時の B（C2）**: Aの点検が止まっても、次の全てを満たす時は、Bも同じ非撮影の点検とCloseを行い、両台の残りを一度の押しで読み切る。(1) Aの止まり方がカメラ自身の状態: 残っていた通知（`kind=event`）、撮影条件（`live_view_on`/`quality`/`size`）、状態が落ち着かない（`not_settled`）、準備中のまま（`not_ready`）、ピント（`out_of_focus`）。(2) AのCloseを確認できている。(3) 一組180秒の期限と停止の要求がまだ許している。別名の照合の失敗、開けない、転送の失敗、Close未確認、期限切れ、停止の要求の時はBを開かない。どちらが止まっても、A・Bどちらにもシャッターは送らず、露光前の確かめ直しにも進まない。Bの点検は繰り返しではなく、一回の押しで各カメラを一回ずつ読むだけ。

止まった組は、本体ごとの結果を次の行に出す。状態は `ready`（通った）、`refused`、`not_checked`（開いていない）。

`pair_preflight_result a=<ready|refused|not_checked> b=<...> a_kind=<kind> b_kind=<kind> a_code=<0xNNNN|none> b_code=<0xNNNN|none> shutter_commands=0`

画面には、1行目に「撮影前の点検で止めました。A・B どちらもシャッターは切っていません。」、2行目に次の操作、続けてA、Bの順に1行ずつ出す。次の操作を2行目に置くのは、状態欄が4行分の高さで、窓が狭いと最後の行が切れるため。文は、本体ごとの理由が (a) 前の撮影の完了通知（`0xC102`/`0x400D`）、(b) 記録先が外れた通知（`0x4005`）、(c) PCに取り込んでいない画像の通知（`0xC101`）、(d) 状態が続けて変わった、ライブビューが止まり切っていない、準備中のまま、(e) 画質・画像サイズの設定、(f) ピント、(g) 想定していない通知、(h) 接続を確かめられなかった・期限・停止、のどれかを、通知の番号を括弧で添えて書く。次の操作は、(a)(b)(d) だけが理由の時は「アプリを閉じて起動し直してください。残っていた知らせは読み取ったので、次は通る見込みです。」、(f) が理由の時（(a)(b)(d) との組み合わせを含む）は「アプリを閉じて起動し直し、AF で確かめてから撮影してください。」、(e) だけの時は本体の設定を直して起動し直す、(c) と (h) と点検していない本体がある時、および (e) と他の理由の組み合わせは「この画面を閉じて、担当者に知らせてください。」。次の操作は常に一文にまとめる。状態欄に入るかどうかは、考えうる全ての組み合わせの文を幅1400と1064で測る画面の検査（`pair_status_width` のログ行）で確かめる。読んだ通知がカメラの列から消えるという前提は、公開資料に記述が無く、本アプリの観察に基づく（「見込み」と書く理由）。撮影失敗の後の「ライブ再開」は従来どおり無効のまま。

**失敗した撮影の見届け（C3）**: 完了通知を読み残して閉じると、次の押しの点検がそれを見つけて止まる。シャッターを送ったのに、ピントが合わない、取り込みが失敗した、シャッターの受付を確認できなかった、などで完了通知を読んでいないカメラは、Closeの直前に、成功時の見届けと同じ上限（`kWpdStillCompletionWaitMax`）・間隔で、そのカメラ自身のセッションの通知を `0x90C7` だけで読む。二台撮影では両方の取り込みが終わった後の `close_all` の中で行うので、Bの取り込みを遅らせない。

- 観測であり再試行ではない: シャッターは再送せず、画像の情報も中身も読まず、画像の取り込みもしない。完了が来なくても撮影の結果は変わらない（記録だけ）
- 画像の通知（`0xC101`）を読んだら `late_object_event=1` として記録して止める。画像は取り込まない。カメラに取り込んでいない画像が残っている可能性があるため、画面に知らせる
- 送信済みのシャッターの見届けは、停止の要求では打ち切らない（#381 と同じ。AGENTS.md の規則）。期限切れとキャンセルでI/Oが許可されなくなったら、読まずに打ち切る（`aborted`）。台帳が通知を拒んだ時は番号を記録して止め（`rejected`）、例外は外に出さない。成功した撮影が既に完了を待った場合は、行わない
- 画像の通知（`0xC101`）は、同じ回に台帳が拒む別の通知（`0x4005` や重複した完了通知）が来ていても記録する。台帳に渡す前に、その回の全ての通知から探す
- 既存の `capture_completion` と `completion_wait` の行は変えない。見届けが行われた時だけ、その後に次の行を出す

`<alias> completion_after_failure result=<seen|timeout|aborted|rejected|read_failed|object_event> waited_ms=<n> max_ms=<n> late_object_event=<0|1> rejected_code=<0xNNNN|none> codes=<0xNNNN:n,...|none>`

`timeout` は上限内に完了が届かなかったこと。届くまでの時間の実測はまだ無く、`waited_ms` で見直す（上限は推定）。
