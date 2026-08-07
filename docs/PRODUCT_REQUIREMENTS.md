# 製品要件

## 1. 目的

固定したNikon D810 2台でA0級の静止した平面原稿を分割撮影し、Windows PC上で1枚の大画像へ合成する。A1より大きい原稿を、カメラ1台の画角・設置制約を超えて効率よく電子化できる状態を目指す。

## 2. MVP利用条件

- 操作者は社内の電子化作業担当者。
- 被写体は平面に近い静止原稿。
- カメラ、レンズ、原稿台、照明は固定する。
- Nikon D810 2台をWindows 11 x64 PCへUSB接続する。
- 撮影中に他のテザー撮影ソフトはカメラを占有しない。
- active transaction中は操作者も物理シャッターを操作せず、接続した一台をPhase 0ツールが排他的に使用する。
- JPEG Fine Lを使用する。
- ハードウェア同期装置は使用しない。
- Nikon Camera Remote SDKのセッションは同時に1台だけ開く。

## 3. 機能要件

### カメラ制御

- `FR-CAP-001`: 接続されたD810を列挙し、ローカルの安定識別情報から`CAM-A`と`CAM-B`を区別できる。
- `FR-CAP-002`: リグプロファイルに左右カメラを登録し、SDKとWPDそれぞれのlocal identityを同じ物理D810の`CAM-A`または`CAM-B`へ対応付け、接続順やUSBポートに依存せず復元できる。
- `FR-CAP-003`: 2026-08-06に承認されたdedicated single-slot spoolを各cameraで使い、`SDK exactly-one card capture → WPD exact-one recovery → PC原本の再読込検証までの確定 → just-recovered object delete → empty-after確認`を完了し、`CAM-A`後に`CAM-B`を順次処理できる。
- `FR-CAP-004`: device datetime cutoffによる画像帰属は採用しない。専用empty/cleared cardをsingle-slot transient spoolとして使い、撮影前にJPEG・NEF・動画・generic fileなど全camera payload objectが0件であることを確認し、SDK one capture後の唯一のexact JPEG objectをWPDで回収できる。PC `.partial`、JPEG・size検証、SHA-256、atomic rename、`original.jpg`再読込検証後に限り、そのobjectだけを削除して全payload 0件を再確認する。候補0件・複数件・遅延・曖昧画像、download/persist/delete失敗は削除せず、PC原本があれば保持して`FailedPartial`にする。
- `FR-CAP-005`: 同時に一つのcapture transaction、一つのSDK session、または一つのWPD sessionだけを開く。SDK/WPDを重複させず、SDK card captureとWPD recoveryの間には必ずfull closeを完了する。
- `FR-CAP-006`: タイムアウト、USB切断、片側失敗、曖昧画像を検出し、自動再試行せず、取得済み原画像と診断情報を保持して`FailedPartial`で終了できる。
- `FR-LV-001`: 操作者が選択した`CAM-A`または`CAM-B`一台だけについて、Nikon SDK経由のLive View開始、プレビュー画像取得、停止を実行できる。プレビュー画像は原画像・合成入力・撮影transactionの候補に使用しない。
- `FR-LV-002`: 承認済みspool transaction前に選択中Live Viewの停止とSDK session closeを確認する。SDK one capture、WPD recovery、PC `original.jpg`の再読込検証までの確定、single-object delete、empty-after確認が成功した後だけ、操作者が選択していた一台のSDK Live Viewを再開できる。

### キャリブレーションと合成

- `FR-STI-001`: 左右カメラごとのレンズ歪み補正値を保存できる。
- `FR-STI-002`: 固定リグの幾何変換、重複領域、シーム、クロップ領域をプロファイルとして保存できる。
- `FR-STI-003`: 承認済みリグプロファイルの固定変換を適用し、撮影ごとの位置・回転・倍率・露出・色の差が承認済み自動補正範囲内の場合だけ、一時的な残差補正を自動適用できる。撮影ごとに自由なhomographyを再推定せず、補正結果でリグプロファイルを自動更新しない。
- `FR-STI-004`: 左右の露出・色差を補正し、継ぎ目を目立ちにくく合成できる。
- `FR-STI-005`: 自動補正量または補正後品質が承認済み範囲を超えた場合、成功扱いにせず、物理調整または再キャリブレーションが必要な理由と測定値を出せる。
- `FR-STI-006`: 設置アシスタントは、承認済みリグプロファイル、全画角、重複、カメラ設定整合、および位置・回転・倍率・露出・色の測定値から、`補正不要`、`自動補正して続行`、`物理調整が必要`を判定できる。閾値を内蔵せず承認済みプロファイルから受け取り、draft・不整合・期限切れのプロファイルでは本番撮影を許可しない。

### 保存と操作

- `FR-DATA-001`: PCに確定・再読込検証済みの左右の`original.jpg`を成功・失敗にかかわらず保持する。PC原本が唯一の製品正本であり、カメラカードは一過性の転送元とする。承認済みsingle-slot spoolではPC原本確定後にexact just-recovered WPD objectだけを削除し、empty-afterを確認する。existing cardのbulk delete/formatは禁止する。
- `FR-DATA-002`: 撮影トランザクション、カメラ別時刻、状態、ファイルサイズ、SHA-256、結果、エラーを記録する。
- `FR-EXP-001`: 合成JPEGを指定フォルダへ保存できる。
- `FR-UI-001`: 左右カメラの接続・Ready状態、リグプロファイルID・版・校正状態、自動補正可否、処理進捗、結果を表示できる。
- `FR-UI-002`: 撮影、設置確認、キャリブレーション、再合成、保存先表示、設定を操作でき、補正範囲外では調整方向または再キャリブレーションを案内できる。
- `FR-UI-003`: 一台選択式Live Viewの開始・停止、対象カメラ、接続状態、再開失敗を表示できる。

## 4. 非機能要件

- `NFR-PLAT-001`: Windows 11 x64で動作する。
- `NFR-PERF-001`: 撮影開始から合成JPEG確定までp95で10秒以内を暫定目標とする。Phase 0では撮影・回収時間を測定するが合否には使わない。
- `NFR-REL-001`: Phase 0Bでは100件連続の二台撮影トランザクションを初回試行で完了し、撮影・回収失敗、誤ペア、原画像消失、曖昧画像の自動採用、回復不能停止がすべて0件であること。
- `NFR-OBS-001`: Live View開始・初回frame・停止、WPD baseline/open/close/reopen、SDK card capture/open/close、画像検出、転送、`.partial`、JPEG・size検証、SHA-256、原子的保存、再読込検証、single-object delete、empty-after、Live View再開、合成、エラーの時刻を記録する。datetime診断結果、handoffの要求数・試行数・完了数・終端状態、WPD capture command未送信に加え、リグプロファイルID・版、baseline校正残差、提案・適用・拒否した一時補正量、拒否理由を確認可能にする。
- `NFR-SEC-001`: 原画像と履歴をローカル保存し、MVPではクラウド送信しない。実識別子とSDK配布物をコミットしない。
- `NFR-MEM-001`: 推奨32GB、最低16GBのPCでピークメモリを計測し、上限を実機PoCで確定する。

## 5. 制約

- `CON-001`: Nikon D810 2台を使用する。
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
- existing cardのbulk deleteまたはformat
- vendor operation（`0x9207`を含む）の実装・送信
- 撮影ごとの自由なhomography再推定、補正結果によるリグプロファイルの自動学習・自動更新
- モーター等を使ったカメラ位置・角度・撮影距離の自動物理調整

## 7. MVP受入条件

MVPの受入は、以下をすべて満たした時点で人間が判定する。

1. 登録済みD810 2台を、SDKとWPDの両方で同じ物理実機の`CAM-A`、`CAM-B`として正しく識別できる。
2. 1操作で両画像を順次取得し、同一トランザクションとして確定できる。
3. 原画像を失わず、失敗時も部分取得済み画像を保持し、再合成できる。
4. 承認済みテスト原稿で、位置ずれ、二重像、色差、継ぎ目が合意済み基準内である。
5. p95処理時間が合意済み目標内である。
6. 100件連続撮影とUSB再接続試験の結果が記録されている。
7. 未解決の安全・SDK利用・再配布・リリースゲートがない。
8. 一台選択式Live Viewが表示でき、撮影時にSDKとWPDを重複させず、安全に停止、WPD baseline、SDK一回card capture、WPD recovery、再開できる。
9. 承認済みリグプロファイルを使って設置状態を判定し、範囲内の差だけを自動補正し、範囲外では成功扱いにせず物理調整または再キャリブレーションを案内できる。

数値品質基準は `unresolved_questions.json` の人間ゲートで確定する。
