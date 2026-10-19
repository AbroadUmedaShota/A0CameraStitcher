# A/B別 AF・ピント微調整 (#331)

開発時の本人の選択「AFと微調整の2種類でok」に基づく、開発用WPD二台ライブビューの操作追加。各画面にAF・近くへ微調整・遠くへ微調整の3ボタンを置く。後ろ2つは同じ微調整機能の方向であり、AF位置や露出・AFモード設定を変更しない。

通常の `scripts/Start-WpdDualLivePreview.ps1` で表示する。初回フレームが来るまで、操作中、映像が2秒以上古い、停止中はボタンを無効化する。微調整は公開MIT実装で小として使われる10ステップに固定し、1クリックで1回だけ送る。物理距離の保証はしない。AF設定・接続レンズの対応が必要で、カメラが拒否した場合はその応答を表示し、自動的な設定変更や駆動の再試行をしない。

通信はカメラごとのworkerだけが行う。UIからは各側に最大1件だけ渡し、workerがフレーム転送とENDを完了してからフォーカス操作を処理する。反対側のworkerは継続する。AF-onlyは0x90C1（空パラメーター）、微調整は0x9204（方向1=近、2=遠と10ステップ）だけ。撮影併用AFは許可しない。送信直前allowlistで他のdirection/stepや操作を拒否する。DeviceReady 0x90C8を前後に読み、busyだけを期限内で問い合わせる。AF/微調整本体は再送しない。

操作全体の予算は10秒。同期呼び出しには従来の12秒Cancel watchdogも適用する。HRESULT/command status/最終MTPを検査し、完了不明なら両側停止へ進む。MTPで明確に拒否された操作は画面に表示して、DeviceReady確認後に映像取得へ戻る。AF完了応答は光学的な合焦の保証ではない。DeviceReadyのOutOfFocusも成功へ変換しない。通常起動で自動フォーカス操作をしない。

開発時の再現用 `--seconds 60 --focus-smoke` は、同じUI受付経路でA AF / B AF / A近 / A遠 / B近 / B遠を各1回だけ要求する明示テスト。これはレンズを動かす実機試験であり、通常利用の起動オプションには含めない。実画像は保存せず、匿名のコマンド応答、取得・描画数、Stop/Closeだけをignored build配下へ記録する。

コマンドの出典はthird_party/digicamcontrol-NOTICE.txtの固定commitの公開MIT NikonBase.cs。Nikon SDKや同梱仕様書は利用しない。二台表示と同じHardwareProcessLeaseを全工程で保持する。

追加の読み取り専用診断は0xD061（ライブビューのフォーカスモード）。公開実装の値0=AF-S、2=AF-F、3/4=MFを使い、MFではAFコマンドが受け付けられても駆動成功と表示しないよう送信前に止める。微調整はAF-Sでだけ送る。操作直前に再読するため、本体・レンズ側のスイッチ変更を反映できる。設定書き込みはしない。

二台のLive View sessionは両方開いて継続させ、WPD API呼び出しだけ共有mutexで順に送る。AF完了待ちの間はmutexを解放し、反対側の映像取得を進める。待機後にも期限・停止・session所有を再確認する。期限後でも、所有が確認できるEND/Stopは許可する。従来の並列呼び出しで起きた転送・sessionエラーの原因そのものが確定したわけではなく、変更後の実機結果を別々に記録する。

前回の失敗でLive Viewが残った場合の明示的な開発用停止操作は `--seconds 30 --stop-live-only`。登録済み二台の状態読取、必要な場合のStop一回、状態0の確認、Closeだけを行う。Start/AF/微調整は行わず、通常起動から自動で呼ばない。

終了処理は両workerの転送・操作終了をbarrierで待ち、各側でDeviceReady確認後にStopを一度だけ試み、両側のStop試行が終わる第二barrierを通過してからCloseする。開始前/COM初期化失敗もbarrierへ到達する。第二workerのOS threadを作れない場合は欠員を通知し、先行workerの終了を待つ。停止専用モードの診断・Stopの一連の操作もカメラ間で順序を付ける。

実機run 3では二台表示が約59秒継続し、AのAF/近/遠とBのAFは完了したが、Bの微調整は0xA004で拒否された。読取でBの0xD061=3（物理MF）を確認し、本人がAFへ切り替えた。run 4では両側0となりAF/近/遠すべてが完了応答を返したが、終了時にAのENDが0x800704C7、Stopが0x8007001Fとなったため、試験全体は不合格と記録した。片側Closeが他方の転送終了より先行できる実装を修正した上で再検証する。詳細な匿名ログはignored build/reference配下に保持する。

最終run 5（60秒）は合格。両側0xD061=0（AF-S）、各側AF/近/遠の3操作がそれぞれ最終MTP=0x2001、DeviceReady=0x2001で完了。A1190取得/873描画/約20.20fps、B1162取得/870描画/約19.64fps、同時期間58.874430秒。両側Stop=0x2001、Close成功、failed=0、exit 0。最終exe SHA-256: `f2849d672cefb34a2ffd881c047c63587c959c2658cf6990625aa394ce14a429`。匿名実機ログ: `build/reference/wpd-preview-focus-331-hardware-05.log`（ignored）。

Release/Debugとも関連6/6 CTests合格。codec115、転送170、送信allowlist/期限/cleanup69、GDI603、focus mailbox/AF設定55、shutdown9 checks、合計1021 checksで失敗0。これはコマンド完了・映像継続・終了処理の確認であり、ピント精度の光学的評価や長時間耐久・USB抜去受入は含まない。
