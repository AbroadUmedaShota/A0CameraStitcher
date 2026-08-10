# 製品要件

## 1. 目的

製品は、固定したNikon D810一台で静止平面原稿を一枚の原画像として撮影・保存する`SingleCamera`と、D810二台でA0級原稿を分割撮影して一枚の大画像へ合成する`DualCamera`を提供する。二台A0合成を主用途として維持しつつ、一台だけを用意できる環境でも撮影、Live View、read-only設定確認、結果確認、明示保存を安全に行える状態を目指す。

## 2. MVP利用条件

- 操作者は社内の電子化作業担当者。
- 被写体は平面に近い静止原稿。
- カメラ、レンズ、原稿台、照明は固定する。
- 操作者はactive transaction外で`SingleCamera`または`DualCamera`を明示選択する。接続台数からmodeを推定せず、`DualCamera`の一台不足を`SingleCamera`へ自動降格しない。
- 初期`SingleCamera`運用は、SDKとWPDの双方で同じ登録済みD810が厳密に一台だけ列挙される構成に限定する。`DualCamera`は登録済みD810二台をWindows 11 x64 PCへUSB接続する。
- 撮影中に他のテザー撮影ソフトはカメラを占有しない。
- active transaction中は操作者も物理シャッターを操作せず、active modeがその段階で処理するcameraをPhase 0ツールが排他的に使用する。
- JPEG Fine Lを使用する。
- ハードウェア同期装置は使用しない。
- Nikon Camera Remote SDKのセッションは同時に1台だけ開く。

## 3. 機能要件

### カメラ制御

- `FR-MODE-001`: active transaction外で`SingleCamera`または`DualCamera`を明示選択し、mode、required camera aliases、profile IDをtransaction開始時に固定できる。active中のmode変更、接続台数からのmode推定、および`DualCamera`から`SingleCamera`への自動降格を禁止する。
- `FR-CAP-001`: 接続されたD810を列挙し、active modeが要求する登録済みaliasをローカルの安定識別情報から解決できる。`SingleCamera`は選択した`CAM-A`または`CAM-B`一台、`DualCamera`は`CAM-A`と`CAM-B`を要求する。
- `FR-CAP-002`: profileに必要なcamera aliasを登録し、SDKとWPDそれぞれのlocal identityを同じ物理D810へ対応付け、接続順やUSBポートに依存せず復元できる。
- `FR-CAP-003`: 2026-08-06に承認されたdedicated single-slot spoolをactive modeが要求する各cameraで使い、`SDK exactly-one card capture → WPD exact-one recovery → PC原本の再読込検証までの確定 → just-recovered object delete → empty-after確認`を完了する。`SingleCamera`は選択alias一台で完了し、`DualCamera`は`CAM-A`後に`CAM-B`を順次処理する。
- `FR-CAP-004`: device datetime cutoffによる画像帰属は採用しない。専用empty/cleared cardをsingle-slot transient spoolとして使い、撮影前にJPEG・NEF・動画・generic fileなど全camera payload objectが0件であることを確認し、SDK one capture後の唯一のexact JPEG objectをWPDで回収できる。PC `.partial`、JPEG・size検証、SHA-256、atomic rename、`original.jpg`再読込検証後に限り、そのobjectだけを削除して全payload 0件を再確認する。候補0件・複数件・遅延・曖昧画像、download/persist/delete失敗は削除せず、PC原本があれば保持して`FailedPartial`にする。
- `FR-CAP-005`: modeにかかわらず同時に一つのcapture transaction、一つのSDK session、または一つのWPD sessionだけを開く。SDK/WPDを重複させず、SDK card captureとWPD recoveryの間には必ずfull closeを完了する。
- `FR-CAP-006`: タイムアウト、USB切断、片側失敗、曖昧画像を検出し、自動再試行せず、取得済み原画像と診断情報を保持して`FailedPartial`で終了できる。
- `FR-LV-001`: 操作者が選択した`CAM-A`または`CAM-B`一台だけについて、Nikon SDK経由のLive View開始、プレビュー画像取得、停止を実行できる。プレビュー画像は原画像・合成入力・撮影transactionの候補に使用しない。
- `FR-LV-002`: 承認済みspool transaction前に選択中Live Viewの停止とSDK session closeを確認する。SDK one capture、WPD recovery、PC `original.jpg`の再読込検証までの確定、single-object delete、empty-after確認が成功した後だけ、操作者が選択していた一台のSDK Live Viewを再開できる。

### キャリブレーションと合成

- `FR-STI-001`: active modeで必要な各cameraのレンズ歪み補正値を保存できる。
- `FR-STI-002`: `DualCamera`固定リグの幾何変換、重複領域、seam、crop領域をprofileとして保存できる。
- `FR-STI-003`: `DualCamera`では承認済みrig profileの固定変換を適用し、撮影ごとの差が承認済み自動補正範囲内の場合だけ一時的な残差補正を適用できる。撮影ごとに自由なhomographyを再推定せず、補正結果でprofileを自動更新しない。初期`SingleCamera`は画像処理を行わず、将来処理は`HG-0009`で承認する。
- `FR-STI-004`: `DualCamera`では左右の露出・色差を補正し、継ぎ目を目立ちにくく合成できる。
- `FR-STI-005`: mode別の自動補正量または補正後品質が承認済み範囲を超えた場合、成功扱いにせず、物理調整または再キャリブレーションが必要な理由と測定値を出せる。初期`SingleCamera`の補正は`NotApplicable`とする。
- `FR-STI-006`: 設置アシスタントは、active modeに対応する承認済みprofileを使って、`補正不要`、`自動補正して続行`、`物理調整が必要`を判定できる。`DualCamera`では全画角、重複、カメラ設定整合、および位置・回転・倍率・露出・色を評価する。`SingleCamera`では選択cameraのidentity、設定、画角、cropおよび承認済み一台品質条件だけを評価し、二台間の重複・seam・相対補正を要求しない。閾値を内蔵せず承認済みprofileから受け取り、draft・不整合・期限切れのprofileでは本番撮影を許可しない。

### 保存と操作

- `FR-DATA-001`: PCに確定・再読込検証済みの、active modeで取得した各cameraの`original.jpg`を成功・失敗にかかわらず保持する。PC原本が唯一の製品正本であり、カメラカードは一過性の転送元とする。承認済みsingle-slot spoolではPC原本確定後にexact just-recovered WPD objectだけを削除し、empty-afterを確認する。existing cardのbulk delete/formatは禁止する。
- `FR-DATA-002`: 撮影トランザクション、カメラ別時刻、状態、ファイルサイズ、SHA-256、結果、エラーを記録する。
- `FR-EXP-001`: `DualCamera`では合成JPEGを指定フォルダへ保存できる。`SingleCamera`では検証済みcanonical `original.jpg`を画像処理せず単一撮影出力として明示保存でき、合成済みとは表示しない。
- `FR-UI-001`: 一画面の撮影ダッシュボードで、明示選択したmode、起動セッションの排他同意、modeが要求するcameraの接続・identity・設定・card・Live View状態、profile ID・版・期限、設置と自動補正可否、mode別処理進捗、撮影・合成・保存を分離した結果、赤Blocker・黄Caution・青Infoを表示できる。`SingleCamera`で非required cameraの不在をBlockerにしない。
- `FR-UI-002`: `Ready`または`ReadyWithCorrection`の場合だけ追加確認なしの一回操作で撮影し、active transaction中のmode変更、競合操作、二重開始を禁止できる。設置・校正、read-onlyカメラ設定、結果確認後の明示保存、read-onlyの新規撮影準備を提供する。別job再合成は`DualCamera`だけに提供し、`SingleCamera`では`StitchOutcome=NotApplicable`として理由を表示する。
- `FR-UI-003`: 一台選択式Live Viewの開始・停止、対象カメラ、接続状態、撮影前停止と撮影後再開の失敗を表示できる。previewを原画像・合成入力として扱わない。

## 4. 非機能要件

- `NFR-PLAT-001`: Windows 11 x64で動作する。
- `NFR-PERF-001`: mode別に撮影開始から製品JPEG確定までのp95を測定する。`DualCamera`の暫定目標は10秒、`SingleCamera`の目標は`HG-0009`で確定する。Phase 0では撮影・回収時間を測定するが合否には使わない。
- `NFR-REL-001`: `DualCamera`ではPhase 0Bで100件連続の二台撮影transactionを初回試行で完了し、失敗・誤pair・原画像消失・曖昧画像の自動採用・回復不能停止を0件とする。`SingleCamera`の製品耐久基準は`HG-0009`で確定し、二台結果から代用しない。
- `NFR-OBS-001`: mode、required aliases、Live View開始・初回frame・停止、WPD baseline/open/close/reopen、SDK card capture/open/close、画像検出、転送、`.partial`、JPEG・size検証、SHA-256、原子的保存、再読込検証、single-object delete、empty-after、Live View再開、合成または`NotApplicable`理由、export、エラーの時刻を記録する。datetime診断結果、handoffの要求数・試行数・完了数・終端状態、WPD capture command未送信に加え、profile ID・版、baseline校正残差、提案・適用・拒否した一時補正量、拒否理由を確認可能にする。
- `NFR-SEC-001`: 原画像と履歴をローカル保存し、MVPではクラウド送信しない。実識別子とSDK配布物をコミットしない。
- `NFR-MEM-001`: 推奨32GB、最低16GBのPCでピークメモリを計測し、上限を実機PoCで確定する。

## 5. 制約

- `CON-001`: USB接続したNikon D810を、明示modeに応じて厳密に一台または二台使用する。初期`SingleCamera`はSDK/WPD双方でexactly-one physical D810を要求し、`DualCamera`は二台を要求する。三台以上は対象外とする。
- `CON-002`: ハードウェアシャッター同期を追加せず、実シャッター開口時刻差の上限を保証しない。
- `CON-003`: attempted hybridのdevice datetime cutoffはhardware evidenceでRejectedであり、2026-08-06に承認された専用empty/cleared card single-slot spool以外の帰属・削除経路を実装・実行しない。SDK/WPD sessionは重複させず、vendor operation（`0x9207`を含む）、existing cardのbulk delete/format、retryは禁止する。deleteはPC原本の再読込検証後のexact just-recovered WPD objectだけとし、zero/multiple/late/invalid/download/persist/delete failureでは実行しない。二台接続時はSDK/WPD identityの同一実機bindingが完了するまでhandoffを許可しない。
- `CON-004`: Nikon SDK、ライセンス対象バイナリ・資料、実カメラ識別子をリポジトリへ含めない。内部評価利用と製品再配布を別々に承認する。
- `CON-005`: 動体、手持ち、近距離立体物、リアルタイム動画は対象外。

## 6. MVP対象外

- NEF/RAW現像
- 16-bit TIFF/BigTIFF出力
- Live Viewの二台同時表示
- Live Viewプレビュー画像の原画像・合成入力への使用
- GPU必須処理
- 遠景・球面・円筒パノラマ
- 三台以上のカメラ
- 自動露出、オートISO、AFを使った撮影
- カメラファームウェア変更
- 同一トランザクションの自動再試行
- 接続台数によるmode自動選択、`DualCamera`から`SingleCamera`への自動降格
- 初期`SingleCamera`でD810二台を接続したまま片方だけを操作すること
- 別契約が承認される前のアプリからの撮影設定write
- existing cardのbulk deleteまたはformat
- vendor operation（`0x9207`を含む）の実装・送信
- 撮影ごとの自由なhomography再推定、補正結果によるリグプロファイルの自動学習・自動更新
- モーター等を使ったカメラ位置・角度・撮影距離の自動物理調整

## 7. MVP受入条件

MVPの受入は、以下をすべて満たした時点で人間が判定する。

1. 操作者が`SingleCamera`または`DualCamera`を明示選択でき、active中に変更されず、`DualCamera`が一台不足しても自動降格しない。
2. 共通安全契約として、原画像を失わず、失敗時も取得済み画像を保持し、SDK/WPD非重複、exact-object cleanup、no retry、operator-session排他、180秒watchdogを満たす。
3. `SingleCamera`ではSDK/WPD双方で同じ登録済みD810一台だけを解決し、一操作でcanonical original一件を確定する。非選択aliasの処理とstitch jobを開始せず、`StitchOutcome=NotApplicable`を表示してcanonical originalを明示保存できる。
4. `DualCamera`では登録済みD810二台をSDK/WPD双方で同じ物理実機の`CAM-A`、`CAM-B`として識別し、一操作で両画像を順次取得して同一transactionとして確定できる。
5. `DualCamera`では承認済みテスト原稿で位置ずれ、二重像、色差、継ぎ目が合意済み基準内であり、再合成できる。
6. mode別p95と耐久試験が合意済み目標内であり、USB再接続試験の結果が記録されている。二台の100件結果を一台の合格へ読み替えない。
7. 未解決の安全・SDK利用・再配布・mode別品質・リリースゲートがない。
8. 一台選択式Live Viewが表示でき、撮影時にSDKとWPDを重複させず、安全に停止、WPD baseline、SDK一回card capture、WPD recovery、再開できる。
9. active modeに対応する承認済みprofileを使って設置状態を判定し、範囲内の差だけを自動補正し、範囲外では成功扱いにせず物理調整または再キャリブレーションを案内できる。

二台A0の数値品質基準は`HG-0001/0002`、一台出力の対象原稿・DPI・crop・耐久・性能基準は`HG-0009`で確定する。
