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
