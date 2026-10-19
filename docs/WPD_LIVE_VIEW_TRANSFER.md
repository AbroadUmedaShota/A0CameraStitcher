# WPD Live Viewのpayload転送（#329）

最優先の到達点はA/Bの両方が継続更新されるLive View。その後、映像Merge C、撮影、保存JPGのMergeへ進む。本部品は[#328のcodec](WPD_LIVE_VIEW_CODEC.md)を使い、一つのchannelで一回分の転送を完了させる。実機の列挙・openやIPortableDeviceへの接続、Live Viewの開始/停止、JPEG解析、画面表示はこの部品の外にある。

## 実行する処理

[`ReadWpdLiveViewPayload`](../src/phase0/include/a0/phase0/experimental/wpd_live_view_transfer.hpp)は、注入された一台分のchannelに対し、Frame取得開始、READ_DATA、END_DATA_TRANSFERを発行する。任意opcode、設定変更、撮影、カード操作、data-to-writeの経路を追加しない。実COM message/resultを扱うが、今回のchannel実装はsoftware試験用のみで、実デバイスを操作しない。

- 既知sizeは指定byte数まで読む。短いchunkでも進捗があれば継続し、途中の0 byteは不足として拒否する。
- 未知sizeは[Microsoft仕様](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/wpd-command-mtp-ext-execute-command-with-data-to-read)に従ってshort readを終端とする。ただし累積量とread回数を制限し、無限取得にはしない。chunkの整数倍なら終端の0 byte readが必要。
- payload上限は最大4 MiB、chunk上限は最大64 KiB、read回数は最大4096。所有者はこれ以下の値を明示できる。メモリ・仕事量の上限であり、画質/fpsの受入値ではない。未知sizeがちょうど上限に達した場合は1 byteの終端確認を行い、1 byteでも追加で来れば拒否する。超過byteをpayloadへ追加しない。
- 有効なtransfer contextが得られたら、終了処理の所有権が許す場合、途中失敗・中断時も終了処理を一度だけ試みる。失敗理由と終了結果は別に保持し、再送しない。contextが得られない失敗では推測したcontextへ終了を送らず、channelを廃止して上位のsession closeに委ねる。
- 最終ENDのWPD/MTP成功と、最後の継続許可を確認した時だけpayloadを返す。失敗時は部分データを返さない。

`may_continue`と`may_cleanup`は必須。呼出側は前者で単調時計のdeadline、停止要求、制御lease、撮影との排他を検査する。開始前、read前後、開始応答後、最終END後に検査し、遅れて戻った結果を成功にしない。messageを作った後、dispatchの直前にも確認する。既に失敗した操作に対してはデータ取得を続けず、cleanupへ進む。

後者では終了処理に必要なlease・session所有権と排他を別に検査し、期限切れ・停止後でも安全な終了を一度だけ許せるようにする。message作成後にも再確認する。所有権確認がfalseまたは例外ならENDを送らず、確認不能・channel廃止として上位へ返す。期限切れを理由に取得を止めても、所有権を失ったままcleanup命令を送ることはしない。

これは同期dispatchを外から中断する仕組みではない。実接続時は別thread/processによるCancelと、戻らない呼び出し・確認できないcloseの扱いが別途必要。callbackが返るまでの強制timeoutを保証しない。

## channelの状態と二台

一台につき一つのchannelを所有する。同じchannelへの重複転送は、commandを送らず`ChannelBusy`で拒否する。AとBの間でmutex・context・payloadを共有しないため、一方のdispatch待ちだけで他方の転送を止めない。

開始dispatch後に失敗したchannelは`Retired`となり、その後のpayload取得は無送信で拒否する。手動操作を装った再試行で同じchannelを復活させない。成功したchannelは次の通常frameを取得でき、新しいtransfer contextを使う。

**END_DATA_TRANSFERの確認は、Live Viewの停止やWPD session closeの確認ではない。** `end_confirmed`だけで撮影を許可しない。上位のcontrollerが別途、二台のLive View停止・device close・binding・lease・撮影排他を確認する。受信payloadもD810のJPEGとして検証済みではなく、原画像・保存の合成入力ではない。

## 検証と再実行

試験はWindows COM valuesと偽channelを使い、既知/未知size、累積上限、short/zero read、回数上限、三段階それぞれのdispatch失敗、終了応答の欠落/拒否、例外、中断、廃止後の無送信、通常の次frameを検証する。Aを明示的な待機点で止め、同じchannelの重複を拒否しつつBが完了できることを、sleepを使わず検査する。

```powershell
cmake --build build/reference --config Release --target a0_wpd_live_view_transfer_tests --parallel 1
ctest --test-dir build/reference -C Release -R '^wpd_live_view_(codec|transfer)_contracts$' --output-on-failure
# Debugでも同じbuild/CTestを行う。
```

BUILD_TESTING内の独立library/試験であり、製品CLI・Camera Agent・WPFには未接続。これを二台の実機Live View完成、物理identity確認、同期、C、JPG Mergeの完成として数えない。

## 開発時の確認結果

Release/Debugのbuildが成功し、転送処理各167チェックが失敗0。codec各115・既存観測ログ各421チェックを含むCTest各3/3も成功した（Release 7.90秒、Debug 7.94秒）。未実装状態では成功経路の試験が明示的に失敗することを先に確認した。追加レビューで、message構築後の送信直前の再確認と、終了処理の所有権確認を補強した。`git diff --check`と新規ファイルの空白検査も成功。

最終software証拠はignoredな`build/reference/wpd-live-view-transfer-329-release-verified-ctest.log`と`wpd-live-view-transfer-329-debug-verified-ctest.log`。`verified`を含まない先行ログは最終証拠ではない。未コミット・main未反映。

所有者がこのPCにD810二台を接続・電源ONと確認した後、PnPの最新snapshotでNikon WPD二台OK・USB error 0を確認した。続いてSDKなしで新しくbuildしたPhase0の`inventory --transport wpd`を一回だけ実行し、exit 0、CameraCount 2、BoundCameraCount 2、UnboundCameraCount 0、IdentityMapChanged falseを確認した。既存のoperator-session-wide leaseの下で読み取り専用に列挙し、コード上は各sessionのcloseを確認してから結果を返す。

その証拠はignoredな`build/reference/wpd-readonly-inventory-329-summary.json`と非公開の対応ログ。11:40:50 JSTの観測で、使用exeのSHA-256と前後の不変を記録した。これは既存identity mapとの一致確認であり、新しいAgentのbinding完了・Live Viewの開始・撮影・品質受入ではない。シリアル番号や画像を公開しない。なお、この既存Phase0のbuildでは既存ファイルにコードページのC4819警告が出たがbuildは成功し、今回その周辺のソースは変更していない。
