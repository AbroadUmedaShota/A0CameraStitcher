# 操作者画面・警告・失敗復旧仕様

## 適用範囲

本仕様は`FR-UI-001`〜`FR-UI-003`の画面契約である。WPFには`模擬動作（実機未接続）`のmode-aware shellと、明示起動する`HardwareSingleCamera`画面がある。後者はCamera Agent protocol、local pending state、canonical original再検証、明示exportまでsoftware boundaryを実装するが、実D810、WPD/SDK、actual JPEG、実画面操作の受入証拠はまだない。

## 画面表記

画面に出す文字は操作者の言葉で書き、開発者語をそのまま置かない。内部の識別子・enum・ログは英語のままでよいが、画面表記は次に統一する。

| 内部の呼び方 | 画面表記 |
| --- | --- |
| Live View | ライブ表示 |
| canonical original | 原画像 |
| fixed-local export | このPCのフォルダへ保存 |
| identity | 機体照合 |
| card | カード |
| focus lock | ピント固定 |
| watchdog | 制限時間 |
| transaction | 撮影ID |
| `FailedPartial` | 撮影失敗（再開不可） |
| `Degraded` | 警告確認 |
| `NotApplicable` | なし |
| `SIMULATED` | 模擬動作（実機未接続） |

`Blocker`／`Caution`／`Info` の3語だけは英字のまま残す。色に依存せず重大度を文字で併記するアクセシビリティ要件が、この3語の同一性を前提にしているためである。

支援技術向けの`AutomationProperties.Name`も同じ表記に合わせる。読み上げだけが別語彙になると、画面を見ている人と読み上げを聞いている人の間で会話が成立しなくなる。

## 画面構成

日常操作は`撮影ダッシュボード`一画面に集約する。画面は 1920×1080 の固定キャンバスとして構成し、ウィンドウ側では等比縮小だけを行う。操作者ごとに画面サイズが違っても要素の相対位置が変わらず、撮影手順の記憶と一致し続けるためである。縮小で生じる余白はマスク色で塗り、スクロールは発生させない。

縦は タイトルバー40px ／ メニューバー30px ／ コンテンツ ／ ステータスバー28px の4層とする。タイトルバーはOS標準クロムではなく自前で描き、アプリ名・運用構成・総合状態・`模擬動作（実機未接続）`・設置プロファイル・ウィンドウ操作を載せる。ステータスバーには撮影ID・原画像・合成・占有状態を常時置く。コンテンツは左カラムのステージと、右カラム372px（フォーカスパネル、撮影、アクションゾーン）からなる2カラムで構成する。左ナビゲーションは設置せず、低頻度のグローバル操作はメニューバーへ集約する。`設置・校正`、`カメラ設定`、`保存・診断`はメニューから到達する保守画面とし、active transaction中は該当メニュー項目を無効化して移動を禁止する。カメラ設定はread-onlyで、実機write契約が承認されるまで変更ボタンを提供しない。フォーカス操作の扱いは「フォーカス操作」節に定める。

ダッシュボードは次を常時表示する。

- 明示選択した`SingleCamera`または`DualCamera`、required aliases、総合状態、起動セッションの排他同意、profile ID・版・期限、保存先
- タイトルバーのバッジとして、選択中mode、総合状態、`模擬動作（実機未接続）`
- modeが要求するcameraの接続、identity、read-only設定整合、card、Live View。`SingleCamera`では非required cameraの不在をBlockerにしない
- 一台選択式Live Viewと「非原画像・非合成入力」の表示
- 設置判定、予定自動補正、必要な物理調整
- ライブ表示停止、required cameraの撮影・保存、および`DualCamera`だけの合成進捗。`SingleCamera`では合成を`なし`と表示
- 撮影、保持原画像、合成、保存を分離した共通結果領域
- `Blocker`／`Caution`／`Info` の3段と、展開式のerror code・ログ情報

### メニューバー

メニューバーは`ファイル`、`カメラ`、`表示`、`ツール`、`ヘルプ`の5項目で構成する。`編集`は設置しない。原画像を無加工・byte-identicalで保存する契約のため画像編集機能は設計上存在せず、慣習で空メニューを置かない。

| メニュー | 内容 |
| --- | --- |
| `ファイル` | 保存先（このPC内のフォルダ）の指定、このPCのフォルダへ保存、終了 |
| `カメラ` | 運用構成（`SingleCamera`／`DualCamera`）の選択、カメラ設定のread-only表示、観測値の30日profile承認、identity状態、readiness再検査 |
| `表示` | オーバーレイ（グリッド、トンボ、重複帯、安全マージン）のトグル、拡大エリアの倍率、傾き読み値の表示切替 |
| `ツール` | 設置・校正、別jobでの再合成、保存・診断 |
| `ヘルプ` | 技術情報（error code、ログ位置）、バージョン |

メニューバー右端には構図グリッドの分割指定と表示設定のリセットを置く。リセットは1回目を確認待ち、2回目の押下で確定し、3秒放置で解除する。誤操作で構図やピント表示が消えると撮り直しになるためである。

全メニュー項目の有効・無効は`OperatorActionAvailability`に連動する。`Capturing`・`Stitching`中は競合操作を一括無効化し、mode変更はactive transaction外だけで有効とする。メニュー経由の保存先変更・設定操作を状態ゲートの例外にしない。

### ステージ

ステージは左カラムを占め、次の表示モードを手動で切り替える。自動切替と自動交互表示は行わない。

| 表示モード | 内容 |
| --- | --- |
| `CAM-A live`／`CAM-B live` | 選択中カメラの素のフルフレーム表示 |
| 合成プレビュー | 両カメラをA0レイアウトへマッピングした全体表示。ライブ側は選択中カメラのLive View、非ライブ側は最終取得frameの静止画。重複帯を帯と幅pxで示す |

合成プレビューはLive Viewを一台だけ開き、非ライブ側は静止画を表示する。同時に二台のLive Viewを開かないため、常時禁止の「同時二台Live View」に抵触しない。非ライブ側には鮮度バッジ（`静止画 N秒前`）を併記し、静止画であることを明示する。

`Capturing`・`Stitching`中はステージ全面を覆う進捗オーバーレイ（`ライブ表示は停止中（撮影を実行しています）`・段階ストリップ・制限時間）へ置き換え、黒画面のまま放置しない。この間に触れる操作がないことを面で示す。`Review`ではステージを合成結果ビューアへ切り替え、検証済み原本由来の表示として`合成結果`バッジを付ける。

ライブ表示の全表示モードで「非原画像・非合成入力」の注記を常設する。

操作結果の通知はステージ上端中央へ出し、3.2秒で自然に消す。操作した直後の視線の先で気づけるようにするためで、確認操作は要求しない。

### ターゲット□と拡大エリア

ターゲット□は画面に常に1つとし、拡大エリアの照準とAF優先ポイントを兼ねる。移動はドラッグだけで行い、ステージ上のドラッグを粗い移動、拡大エリア内のドラッグを細かい移動とする。固定座標への5点プリセットは提供しない。実際の原稿四隅と一致しないためである。

ターゲット□は 92×92 の四隅ブラケットと中央十字で描き、ライブ側は明るい緑、非ライブ側は無彩色にする。AFできる側がどちらかを色で示すためである。

拡大エリアはステージ右下へ 236×236 で浮かせ、□周辺を等倍でクロップ表示する。右カラム常設ではなくステージ上に置くのは、撮影対象から視線を外さずにピントを見るためである。倍率は100%と200%を切り替える。拡大エリア内にも□の枠線を描画する。表示ソースはステージの表示モードに追従し、□が非ライブ側カメラの担当域にある場合はフリーズframeを表示して鮮度バッジを併記する。ライブ表示中でない状態（進捗中・結果確認）では拡大エリアを出さない。

### 設置ガイドオーバーレイと傾き読み値

ステージへ重ねるオーバーレイは方眼グリッド、トンボ、重複帯（帯表示と幅px。値はrig profile由来）、安全マージンとし、個別にトグルする。方眼グリッドはステージ表示領域を列数×行数でちょうど等分し、列・行は1〜24で指定する。3×3／4×4／5×5のプリセットを併置する。原稿サイズや割り付けは案件ごとに違うため、分割数を固定しない。トンボは四隅の合わせマークとして描く。

傾き読み値はステージ下部の常駐行へ表示し、ライブ表示frameからの原稿エッジ検出による面内回転（`ROLL`）角と許容範囲チップを示す。撮影ダッシュボードでは表示のみとし、許容値の入力は保守画面（`設置・校正`）へ置く。判定に使わない値の入力欄を撮影導線へ置かないためである。許容値は設定値とし、本仕様では既定値を定めない。面外傾き（`PITCH`）は検出・表示しない。透視解析を要するためである。非ライブ側やframe未取得時は数値を出さず`検出不能`と表示する。

オーバーレイと傾き読み値はガイドであり、合否を判定しない。検出結果を撮影可否・自動補正へ接続しない。Go/NoGoは`ReadinessSnapshot`による設置判定が担う。

### フォーカス操作

フォーカス操作（AF実行、AFエリア・優先フォーカスポイントの指定、MFドライブ）は、露出・WB等の撮影設定writeと区別し、撮影系操作として分類する。採用範囲と安全境界は次のとおりとする。

- UIとSIMULATED（fake backend）は先行して実装する
- 実機モードでは、承認までフォーカスパネルを無効表示とし、無効の理由を併記する（fail-closed）
- 実機へのAF・MFコマンド配線は、hardware-requiredの別Issueとhuman gate承認後にだけ有効化する
- 本分類だけでは実機へのフォーカス操作を許可しない

本分類は`SingleCamera`での撮影設定write禁止を含む既存の常時禁止を緩和しない。

フォーカスパネルは対象カメラ（Live View中のカメラ）、ターゲット□位置を用いたAF実行と合焦結果、MFステップ（粗・微）、フォーカスピーキングのON/OFF、カメラごとの固定状態チップを表示する。□が非ライブ側カメラの担当域にあるときはAFを実行せず、Live View切替の導線を表示する。自動切替は行わない。固定状態チップは撮影のハードゲートにせず、未固定はCautionの表示にとどめる。

フォーカス位置の絶対値スライダーは提供しない。SDKのフォーカス値はopaqueであり、絶対位置制御の裏付けがない。表示する場合はread-onlyの相対インジケータまでとする。PCからのMFドライブ可否はcapability定義に明記がなく未検証である。SDK調査で不可と判明した場合、MF操作系は提供しない。

### アクションゾーン

アクションゾーンは右カラム下部の単一領域とし、UI状態に応じて中身を入れ替える。フェーズナビ・ウィザードは採用しない。準備作業は順序自由であり、撮影から合成までは全自動のためである。

| UI状態 | 表示 |
| --- | --- |
| `NotReady`／`Ready`／`ReadyWithCorrection` | 設置判定カード（Go/NoGoと予定補正量。値は`ReadinessSnapshot`由来）と撮影ボタン2種 |
| `Capturing`／`Stitching` | ステージ全面の進捗オーバーレイ（ライブ表示停止→CAM-A撮影→CAM-A原画像の確認→CAM-B撮影→CAM-B原画像の確認→合成の現在位置と制限時間）。右カラム側は進行中である旨だけを示す |
| `Review`／`FailedPartial`／`Degraded` | 結果パネル（結果サマリ、`このPCのフォルダへ保存`、`撮影済み画像で再合成`、`新しい撮影を準備`） |

進捗ストリップのCAM-B段と自動合成段は`DualCamera`だけに現れる。`DualCamera`では主ボタン直下に処理順の説明（`A→Bの順に撮影し、完了後に合成へ進みます`）を表示する。撮影不可時は撮影ボタンを無効にし、直下へ最初のBlocker理由を表示する。Blockerはボタン直下、Infoはステージ下部の注記行へ配置し、色に依存せず`Blocker`／`Caution`／`Info`の文字を併記する既存規約を維持する。

保存したファイルの控えは右カラムに置き、保存が実際に成功したときだけ増やす。自動保存はしないので、控えが増えていれば操作者が保存したということになる。控えには実際の出力先をそのまま記録し、表示と実ファイルがずれないようにする。

### Live View表示の実機縮退

ステージ、拡大エリア、フォーカスピーキング、原稿エッジ検出は継続Live View frameの供給を前提とする。SIMULATEDは`Simulated`透かしとタイムスタンプを持つ疑似frame sourceから供給する。

実機では継続Live View v2の受入が成立するまで、画面内previewを`hardware.v1`の有限frame probe表示へ縮退させ、継続streamが動作中とは表示しない。縮退中は継続frameを前提とする表示（拡大エリアの追従、ピーキング、傾き読み値の更新）が最後に取得したframeで固定されることを鮮度バッジで明示する。有限v1 probeは継続v2の受入へ読み替えない。

## 標準操作順

1. 起動時に未完了状態を検査し、新しい撮影を開始しない。SIMULATEDの残留journalは撮影を再実行せず`FailedPartial`へ閉じる。Hardwareではlocal pendingに固定した同じtransaction ID・required alias・profile ID/version/SHA/expiry・handoff intentで`get-transaction-result`だけを行う。既知のpre-dispatch状態だけは「撮影要求0回」として明示的に閉じられるが、dispatch済みの`TransactionNotFound`はpendingを保持してsupport-requiredとし、自動で新規撮影可能にしない。
2. 操作者は「物理シャッターを操作しない」「他のカメラアプリを使わない」に起動セッション単位で同意する。同意は起動時のモーダルで受け、2項目の両方にチェックが入るまで開始ボタンを押せない。読まずに流す操作を防ぐためである。見送った場合はモーダルを畳んで閲覧だけを許し、タイトルバーの未同意表示から開き直せる。同意は永続化せず、アプリ終了時に失効する。
3. 操作者はactive transaction外でmodeを明示選択する。mode変更時はreadinessを破棄して、接続、identity、profile、設置、card、保存先をread-onlyで再検査する。Blockerが一件でもあれば撮影ボタンを無効にし、直下へ最初の理由を表示する。
4. `Ready`または`ReadyWithCorrection`では、追加ダイアログなしに撮影ボタンの一回押下で開始する。後者は予定補正量を常時表示する。撮影ボタンは主従2種とし、序列を固定する。主ボタンは`撮影`（`DualCamera`では`2台を順次撮影する`）で、現在のフォーカス位置のまま撮影する。従ボタンは`撮影+AF`とする。原稿撮影は撮影前AF後に固定する運用方針のため、AF付きを主ボタンにしない。
5. `撮影+AF`は、modeが要求する各カメラの撮影直前へAF段を挿入する。Live View停止後に実行するため位相差AFとし、ターゲット□の位置は最寄りのAFポイントへ丸める。AFが失敗した場合はシャッターを実行しないままfail-closedで停止し、自動リトライしない。AF実行の有無と結果はjournalへ記録する。実機モードでは`撮影+AF`を実行不可とし、無効の理由を表示する。実機での有効化条件は「フォーカス操作」節に従う。
6. 直ちにmode変更を含む全競合操作をロックし、選択中Live Viewを停止してSDK session closeを確認する。停止できなければシャッター処理へ進まない。現`hardware.v1`の画面内previewは一回ごとに有限frameを取得して停止・closeするため、継続streamが動作中とは表示しない。
7. `SingleCamera`は選択alias一台だけを処理し、canonical original確定後にReviewへ進み、`StitchOutcome=NotApplicable`とする。`DualCamera`はCAM-A/Bを順次処理し、両PC原本の検証と明確な帰属が成立した場合だけ自動合成する。SIMULATED実装は同じ状態契約だけを検証する。
8. 結果を共通領域で確認し、操作者が`保存`を押した場合だけ出力する。実`SingleCamera`はcanonical originalを画像処理せず明示copyし、合成済みとは表示しない。SIMULATED版はJPEGに見せない`.simulated-export.txt`を一時フォルダへ出力する。
9. `新しい撮影を準備`はreadinessのread-only再検査だけを行う。過去の片側画像を再利用せず、次回押下時に新しいtransaction IDを生成する。

## UI状態遷移

| 状態 | 意味 | 撮影 |
| --- | --- | --- |
| `AwaitingSafetyAck` | 起動時同意待ち | 禁止 |
| `CheckingReadiness` | read-only状態検査中 | 禁止 |
| `NotReady` | Blockerあり | 禁止 |
| `Ready` | 補正不要 | 許可 |
| `ReadyWithCorrection` | 承認済み範囲内の補正予定 | 許可 |
| `Capturing` | Live View停止〜modeが要求する原本確定 | 禁止・mode変更を含む全競合操作をロック |
| `Stitching` | `DualCamera`自動合成中。`SingleCamera`では遷移しない | 禁止・全競合操作をロック |
| `Review` | 結果確認・明示保存待ち | 新規撮影は`新しい撮影を準備`後 |
| `FailedPartial` | 同じtransactionを再開しない終端失敗 | 新規撮影は`新しい撮影を準備`後 |
| `Degraded` | 結果は保持したがSDK/card状態要確認 | 安全再確認まで禁止 |

`OperatorReadinessEvaluator`が`ReadinessSnapshot`から警告とready状態を作り、`OperatorActionAvailability`が撮影、`撮影+AF`、Live View、フォーカス操作、保存、再合成、新規撮影準備、および全メニュー項目を含む保守画面移動の可否と理由を一元管理する。

## 警告

| レベル | 条件 | 動作 |
| --- | --- | --- |
| 赤 / Blocker | mode不一致、required camera不足、identity未登録、設定不整合、profile未承認・期限切れ、物理調整必要、card非empty/不明、保存先不正、active transaction、SDK/card要確認 | 撮影禁止。スクリーンリーダーへassertive通知 |
| 黄 / Caution | 自動補正範囲内、Live View停止予定、撮影後の有限probe失敗、cleanup異常、フォーカス未固定 | 予定処置を常時表示。安全な結果操作だけ許可 |
| 青 / Info | Live Viewは非原画像、PC原本を保持、実シャッター時刻差は非保証、設置ガイドと傾き読み値は判定に用いない、実機モードでフォーカス操作と`撮影+AF`が無効である理由 | 常時説明 |

操作者向け説明を先に出し、技術情報は展開領域へ分離する。色だけに依存せず`Blocker`、`Caution`、`Info`の文字を併記する。

## 失敗と戻り方

| 失敗点 | 保持 | 戻り方 |
| --- | --- | --- |
| Live View停止 | 原画像なし、シャッター未実行 | `FailedPartial`。安全停止確認後、新しいtransaction |
| `撮影+AF`の撮影直前AF | 当該カメラの原画像なし、シャッター未実行 | 自動リトライせず`FailedPartial`。AF実行の有無と結果をjournalへ記録する。確定済み原本がある場合の扱いは以下の各行に従う |
| `SingleCamera`原本確定前 | 原画像なし | `FailedPartial`。新しいsingle transaction |
| `DualCamera` CAM-A原本確定前 | 原画像なし | `FailedPartial`。両方を新規撮影 |
| `DualCamera` CAM-A確定後 | CAM-A保持 | 過去CAM-Aを再利用せず、両方を新規撮影 |
| 両原本確定後のcleanup | 左右原画像と合成結果を保持可能 | card状態を再確認するまで新規撮影禁止 |
| `DualCamera`自動合成 | 左右原画像を保持 | `再合成`を別stitch job IDで実行。撮影transactionは変更しない |
| `SingleCamera` | canonical originalを保持 | 合成・再合成を開始せず`NotApplicable`。明示保存だけを許可 |
| 明示保存 | 原画像・合成結果を保持 | 保存先を直して再度明示保存 |
| 撮影後Live View確認 | 撮影・合成結果を保持 | 現`hardware.v1`は有限一frame probe後に停止・SDK closeする。probe失敗は`FailedPartial`または`Degraded`として新規撮影を禁止し、継続stream再開の成功とは表示しない |
| アプリ終了・再起動 | 発見した確定原画像を保持 | 未完了journalを`FailedPartial`へ閉じ、自動再開しない |

## 常時禁止

- 同じtransactionの再試行・再開、過去の片側画像との自動ペア
- SDK/WPD session重複、同時二台Live View、ハードウェア同期の保証
- Live View previewの原画像・合成入力への採用
- ステージ表示モードの自動切替、ターゲット□位置によるLive Viewカメラの自動切替
- 原稿エッジ検出・傾き読み値による撮影可否の判定と自動補正への接続
- existing cardの削除、bulk delete、format、vendor operation `0x9207`
- PC原本の検証前のcamera-object削除
- identity未登録、未承認profile、補正上限超過での撮影
- hardware-required Issueとhuman gate承認前の、実機でのフォーカス操作と`撮影+AF`の実行
- active transaction中の設定、校正、保存先変更、保守操作
- active transaction中のmode変更、接続台数によるmode自動変更、`DualCamera`から`SingleCamera`への自動降格
- `SingleCamera`での撮影設定write、二台接続中の片方だけを使う初期運用、単一原画像を合成済みと表示すること
- 原画像の上書き、自動削除、自動再試行

## 自動試験と受入境界

- 2026-08-10 fresh software検証: .NET 10 Release build 0 warning/0 error、Foundation 20/20、Operator Shell 17/17、`Test-M3Simulated.ps1` Pass。CAM-A-only、起動同意、no-auto-fallback、30日read-only profile承認、fixed-local保存先、active中の操作ロック、Singleのstitch `NotApplicable`と明示保存を確認した。
- Hardware Single headless contractは、起動時全操作gate、同一transactionの結果照会、profile/alias/expiry/handoff相関、pre-dispatchと曖昧dispatchの分離、no retry、CAM-A identity-v3、30日profile、fixed-local preference、same-file-identity byte-identical exportに加え、継続Live View v2のmemory-only frame、stop-before-capture、停止不明時capture 0、verified success後だけrestartを含み、Operator Shell 17/17で確認した。C++側もSDK-less／licensed-SDK-enabled Release CTest各7/7を要求するが、camera commandは送っていない。有限v1 probeはv2受入へ読み替えない。
- 一括検証は外部NuGetなし、SIMULATED常設表示、警告レベル、accessibility live region、Foundation facade以外のcamera API不使用を静的・headlessに確認する。2026-08-08のWindows UI Automationは旧Dual contractの履歴証拠であり、requirements 2.6.0のSingle実画面受入へ読み替えない。screen reader、キーボード、focus、実WPF Hardware Single操作、実Camera Agent/D810の各失敗点は未実施である。
- 撮影画面リデザイン（4層構成、メニューバー、ステージ表示モード、ターゲット□と拡大エリア、設置ガイドオーバーレイと傾き読み値、フォーカスパネル、アクションゾーン、模擬frame source、同意モーダル、通知バー、保存控え、構図グリッドの分割指定）はSIMULATED境界まで実装済みで、`Test-M3Simulated.ps1`の静的マーカーとheadless VM検証で担保している。実画面のscreen reader・キーボード・focus検証と、実機での受入証拠は未取得である。フォーカス操作と`撮影+AF`の受入範囲はSIMULATED契約までとし、実機でのAF・MF実行、PCからのMFドライブ可否、継続Live View v2は未検証である。

## 要件追跡

| 要件 | 仕様・実装 |
| --- | --- |
| `FR-UI-001` | 4層構成（タイトルバー／メニューバー／2カラム／ステータスバー）、タイトルバーのバッジ、メニューバー5項目、camera/setup/progress/result、`ReadinessSnapshot`、警告三段階 |
| `FR-UI-002` | 一回撮影と`撮影+AF`の主従2ボタン、状態駆動のアクションゾーン3表示、設置・校正、read-only設定と撮影系操作としてのフォーカス操作、別job再合成、明示保存、物理調整案内 |
| `FR-UI-003` | 一台選択式Live View、開始停止、非原画像表示、ステージ表示モードと合成プレビュー、ターゲット□と拡大エリア、設置ガイドオーバーレイと傾き読み値、停止失敗と撮影後有限probe失敗。継続stream再開は実機受入まで未検証で、実機では有限probe表示へ縮退する |
