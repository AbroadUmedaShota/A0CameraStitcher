# A/B同時Live View用WPDメッセージ部品（#328）

最初の到達点は、1台のWindows PCでA/Bの二画面がともに継続更新されること。その後に表示専用Merge C、撮影、保存JPGのMergeと進む。今回の部品は、その最初の段階で使うWPDメッセージの構築・検査を担う。デバイスの列挙・open・送信・GUIへの接続はまだ実装していない。

## 公開された根拠

- [Microsoft MTP extensions](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/supporting-mtp-extensions)と、[データなし命令](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/wpd-command-mtp-ext-execute-command-without-data-phase)、[read開始](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/wpd-command-mtp-ext-execute-command-with-data-to-read)、[chunk read](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/wpd-command-mtp-ext-read-data)、[transfer終了](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/wpd-command-mtp-ext-end-data-transfer)の公開仕様からWPD envelopeを構築した。
- 3種類のLive View operation識別子は、digiCamControlの[固定commitのNikonBase.cs](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonBase.cs)と照合した。対象ファイルのSHA-256は`4328ec2fb6f31fc4b1cd04d643d2dd8aa2e7ade72c9bd4f37a083e9b0975abbc`。[MIT表記を保持](../third_party/digicamcontrol-NOTICE.txt)する。元のdriver class、retry loop、設定・撮影・カード処理は移植していない。
- Nikon SDK同梱通信仕様の内容を今回のコードの根拠にはしていない。SDK資料を使う経路の未確認事項と、公開実装を参照したこの部品を区別する。Nikonの使用許諾全体や製品再配布の条件を確認済みとする判断ではない。

## 実装の境界

[`wpd_live_view_codec.hpp`](../src/phase0/include/a0/phase0/experimental/wpd_live_view_codec.hpp)のtyped enumはStart/Frame/Stopだけ。任意opcodeやパラメータを受け取らない。開始・停止はdataなし、frame取得はdata-to-readで、空のoperation params collectionを必ず設定する。

read/end-transferには所有するtransfer contextを明示する。カメラ間で共有するmutable stateはない。入力contextは非空・最大4096文字・埋め込みnullなし。read requestには初期化済みbufferを付ける。

応答はdispatch HRESULT、WPD共通HRESULT、MTP responseの3層を分ける。dispatchや共通HRESULTだけの成功をカメラ命令の成功としない。dataなし操作とtransfer終了の最終応答ではMTP responseが必須で、明示的なOKだけを成功とする。data phaseで未到着の最終応答を補完せず、応答が既にあれば型・成功値を検査する。busyや不明なresponseをretryしない。

転送開始ではcontext、64bit total size、任意のchunk hintを読む。既知のpayloadは1 byte〜4 MiB、chunkは1〜64 KiBへ制限する。大きいhintはbuffer割当上限で制限する。これは開発時のメモリ上限で、画質やfpsの承認値ではない。未知sizeは未知のまま返し、値を推測しない。size/hint不正時も有効に取得済みのcontextを保持して、将来の呼出側が転送を終了できるようにする。

chunkは報告された受信数がbuffer長と要求数以下であることを検査し、受信数だけを独立したbyte列へコピーする。bufferの余白を画像として返さない。このbyte列は不透明な転送断片であり、JPEGや表示可能なframeではない。**総量・終端・END_DATA_TRANSFERの成功確認とD810のframe解析を済ませるまで画面へ公開しない処理は、後続の通信部に実装する。** 未知sizeの累積量制限も後続の責任として残る。

コードはBUILD_TESTING内の独立したstatic libraryと試験だけに接続し、phase0 core・製品CLI・Camera Agent・WPFへはリンクしない。この部品にはIPortableDevice::SendCommandやカメラを開く処理がない。今回の試験は本物のWindows COM valuesを使用するが、実カメラは操作しない。ADR-0036全体、二台WPD session上限、実機試作の承認を完了扱いにしない。

## 次の接続

二台分の独立したWPD session/transfer contextを所有する通信部から、この部品を呼ぶ。SDKと重ねず、operator-sessionのleaseの下で動かし、全close確認前に撮影を許可しない。[SendCommandの仕様](https://learn.microsoft.com/en-us/windows/win32/api/portabledeviceapi/nf-portabledeviceapi-iportabledevice-sendcommand)では呼び出しは同期式でtimeout引数がなく、別threadからCancelを試みる形になる。片側の停止で両方のUIや取得loopを止めないworker配置と、Cancel/close失敗時に撮影を拒否する処理を先に決める。

返すCOM values自体はmutableであり、このcodecを送信時の権限制御の代わりにはしない。実通信を追加する時は、送信直前の3命令allowlist、session/alias照合、lease・deadline・capture排他を別に検査する。

参照した公開driverの開始/停止methodには、Live Viewの状態取得、DeviceReadyの呼び出し、zoom値の操作等も含まれる。今回参照したのは3命令の識別子で、元の手順全体を採用したものではない。3命令だけでD810実機の開始・終了が成立することは未検証。ほかのvendor命令や設定変更が必須と判明した場合は、現在の例外範囲を自動で拡大しない。

```powershell
cmake --build build/reference --config Release --target a0_wpd_live_view_codec_tests --parallel 1
ctest --test-dir build/reference -C Release -R '^wpd_live_view_codec_contracts$' --output-on-failure
# Debugでも同じbuild/CTestを実行する。
```

実機の二台Live View、USB帯域/fps、機体binding、D810のJPEG抽出、表示、C、撮影handoffはまだこの試験の対象ではない。

後続の[#329](WPD_LIVE_VIEW_TRANSFER.md)で、codecを使う転送全体の累積量・回数制限、最終END応答、独立した二channelの並行動作を実装した。実デバイスへの接続とD810のJPEG解析はその後の対象として残る。

## 開発時の確認結果

Windows/MSVC x64、Nikon SDKなしでRelease/Debugのbuildが成功し、codec各115チェックが失敗0で完了した。実COM valuesによるenvelope/型/サイズ/response/二context分離/byte所有を検査した。関連CTestは各2/2成功（Release 12.07秒、Debug 17.80秒）。内訳はcodec 115チェックと既存観測ログ検証421チェック。今回product/native全suiteは再実行していない。

証拠はignoredな`build/reference/wpd-live-view-codec-328-release-ctest.log`と`wpd-live-view-codec-328-debug-ctest.log`に保持する。初回buildでWindows APIのbuffer引数のconst不一致を検出して修正し、修正後に両構成を確認した。commit・push・main反映は未完了。
