# D810 PC制御項目

## 結論

コントラストや明暗は調整可能です。ただし、調整方法は二種類に分かれます。

- 撮影時の明るさは、シャッタースピード、絞り、ISO感度、露出補正で制御する。
- JPEGの見た目は、ピクチャーコントロールとその輪郭強調、明瞭度、コントラスト、明るさ、彩度、色相で調整する。

attempted hybridはWPD baseline timeoutとdatetime相関不成立によりRejectedです。`HG-0008`は2026-08-06に承認され、dedicated empty/cleared cardをsingle-slot transient spoolとして、PC原本の再読込検証後にexact just-recovered WPD objectだけを削除して空へ戻す実装・実機評価が可能です。Nikon SDKの`SDK status`は、Live View状態、禁止mask、静止画／動画セレクターに加え、主要撮影設定を撮影設定capabilityへ書き込まず匿名記録できます。MAID sessionのcontrol-plane callback登録とModuleModeには既存の`CapSet`を使い得るため、「SDK commandがGetだけ」という意味ではありません。

## 現在の実装範囲

| 操作 | 現在の状態 | 備考 |
| --- | --- | --- |
| D810列挙、匿名別名付与 | 実装済み | 実シリアルはreportへ出さない |
| PCからシャッター指示 | software implementation ready | 承認済みsingle-slot spoolの実機one-shotは未実施 |
| 新規JPEGの識別 | clock cutoff Rejected | `wpd-correlation-status`はdatetimeの診断だけを行う |
| JPEGのPC保存 | 実機確認済み | `.partial`、JPEG検証、SHA-256確定後に`original.jpg`へ移動 |
| カメラ側JPEG | 承認済みsingle-slot spool | PC `.partial`、JPEG・size検証、SHA-256、atomic `original.jpg`、再読込検証後だけexact objectを削除して空状態を確認する。失敗時は削除しない |
| Live View状態・禁止maskの取得 | SDKで実装・実機確認済み | `sdk-status`はLive Viewを開始せず、設定を変更せずsessionを閉じる |
| 静止画／動画セレクターの取得 | SDKで実装・実機確認済み | Enum表現を読み取り、`photo`を匿名記録する |
| 露出・WB・画質等の取得 | 一台でPartial | `run-1786040075194-1`でJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1を取得。focusはopaque値1、FileTypeはnot-advertised。設定writeなし、session close |
| 露出・WB・画質等の変更 | 未実装 | Phase 0では人が固定する。書込実装・実機試験は別途承認して一項目ずつ行う |
| 一台選択式Live View | Standalone合格、handoffはPartial | SDKで5分04秒・2,424 frame、停止・close・preview非保存と別プロセス再起動を確認。撮影を含む連続handoffは未合格 |
| 二台同時Live View、動画、RAW | 対象外 | MVPでは使用しない |

## PCから扱える候補

次はD810用Nikon Camera Remote SDKのcapability定義と付属サンプルに存在する代表項目です。「候補」はSDKに項目があることを示し、この製品での実装済み・全状態での書き込み可能を意味しません。

| 分類 | 取得・変更候補 | A0撮影での扱い |
| --- | --- | --- |
| 撮影 | 撮影、AF撮影、フォーカスロック、露出ロック | 撮影はSDK exactly-one card captureを採用。AFは原稿撮影では原則固定を推奨 |
| 露出 | 露出モード、シャッタースピード、絞り、ISO感度、露出補正、測光モード | 二台で固定値を一致させる主要項目 |
| 画像形式 | JPEG圧縮率、画像サイズ、RAW画像サイズ、保存先 | MVPはJPEG Fine L。RAWは対象外 |
| 色 | WBモード、WB微調整、蛍光灯種別、プリセットWB | 二台の色差を抑えるため固定・一致させる |
| JPEG仕上げ | ピクチャーコントロール、ピクチャーコントロールデータ、彩度、明るさ、Active D-Lighting、ノイズ低減 | コントラスト等を含む。実機の書込可否と値域を追加検証する |
| フォーカス | フォーカスモード、AFエリアモード、優先フォーカスポイント、AF実行 | 固定リグではMF固定または撮影前AF後に固定する方針を検討 |
| 状態確認 | バッテリー、レンズ情報、焦点距離、カメラ時刻、露出状態 | preflight/report候補。固有識別子は匿名化する |
| ライブビュー | 開始・終了、画像取得、コントラストAF、表示サイズ等 | 一台選択式をMVP対象とする。二台同時表示とpreviewの原画像・合成利用は対象外 |

## コントラスト・明暗を扱う方針

1. Phase 0ではカメラ設定を人が固定し、ツールは変更しない。
2. 設定read-onlyの`SDK status`でLive View関連状態と主要撮影設定の取得を先行した。SDKが返すUnsigned／PackedString／Stringだけを記録し、未知値へ意味を推測しない。control-plane callback登録と撮影設定writeを証拠上で区別する。
3. 書き込みはホワイトバランス、露出、JPEG Fine L、ピクチャーコントロールの順に一項目ずつ実機検証する。
4. 二台撮影では同一設定を検証し、不一致なら撮影を開始しない。
5. PC側でコントラストや明るさを画像処理する場合、`original.jpg`は変更せず派生画像を別ファイルとして保存する。
6. Live ViewはSDK sessionを一台だけ開き、hybrid transaction前に必ずstopとclose完了を確認する。WPD baseline/close、SDK capture/close、WPD recovery、PC原本確定の成功後だけ選択中のLive Viewを再開する。

「明るさ」というSDK capabilityがそのまま最終JPEGの露出補正を意味するとは限りません。撮影時の光量は露出三要素で扱い、JPEGのトーンはピクチャーコントロールで扱うのが安全です。

## 根拠

- [Nikon D810使用説明書](https://downloadcenter.nikonimglib.com/ja/products/176/D810.html): ピクチャーコントロールの輪郭強調、明瞭度、コントラスト、明るさ、彩度などを説明。
- [Nikon Camera Remote SDK information/FAQ](https://sdk.nikonimaging.com/information/en/): D810用Remote SDKの提供とWindows対応履歴を掲載。
- ローカル隔離したD810用SDKのcapability定義および付属サンプルを参照した。ライセンス対象資料そのものはリポジトリへ含めない。
