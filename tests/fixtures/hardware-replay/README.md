# 実機応答の再生 fixture（Issue #231）

実機（Nikon D810）が実際に返した応答を匿名化して置き、アプリの保存経路と Agent の応答生成へ流すための golden fixture。#216（原画像フォルダ名）・#222（fileType を広告しない）・#224（bindingInvalidationReason の空文字）はどれも「fake と実機の応答の差」で、SDK なし構成の ctest と `Test-M3Simulated.ps1` では出なかった。ここの値は fake を作るときの正解として使う。

このフォルダの内容は公開される。実機の識別子（シリアル番号・WPD の機体 ID・PnP ID・ユーザー名・ローカルの絶対パス）と撮影した原画像は置かない。試験は識別子の形（32/64 桁 16 進、13 桁エポック、ユーザー領域のパス、PnP ID 形式、シリアル風の文字列、許可外の日付）を毎回検査するが、これは形の検査にすぎない。本体シリアルなど形では検出できない値は試験で保証できないため、push 前に元データから抜いた実値とローカルで照合する（#241）。

## 元データの出所

操作 PC の app データフォルダに残っていた記録ファイルを、読み取り専用で写して匿名化した。

| 元の記録 | fixture での位置 | 中身 |
| --- | --- | --- |
| 一台撮影（V-1CAM-005 合格、#216 修正後のビルド） | `single-camera/` | Agent の transaction journal、event log、summary、アプリが保存した承認 profile |
| 二台撮影の 1 回目（#221、Failed / CaptureCameraA、シャッター 0 回） | `dual-camera/failed-cam-a/` | pair journal、CAM-A leg の診断 |
| 二台撮影の完了した 1 組（#221、Succeeded） | `dual-camera/succeeded/` | pair journal、CAM-A / CAM-B leg の診断 |
| 二台モードの operator 入力と app の永続状態 | `dual-camera/operator-inputs/`、`dual-camera/recovery-state/` | 承認 profile、identity proof のメモ、完了後の pending snapshot |

使っていないもの: 別の PC で採った 10-05 の一台撮影（この PC に無い）、WPD の camera map と一台用 identity ファイル（機体 ID のハッシュを含む）、保存先設定（ユーザー名入りのパス）、レビュー状態、ロックファイル、撮影した原画像。

## 匿名化規則（置換表）

| 元データの項目 | 識別子にあたるか | 置換 |
| --- | --- | --- |
| 絶対パス中のユーザープロファイル名 | あたる | パスのルートを `C:/a0-replay-fixture/` に置換（以降の相対構成は実物のまま） |
| transaction ID（3 個、32 桁 16 進） | 実機の記録を特定できる | `f231a001…a1`（一台）、`f231b002…b2`（二台 Succeeded）、`f231b003…b3`（二台 Failed）。`f231` で始まる合成値 |
| run ID・hybrid transaction ID・leg の run ID（13 桁のエポック ms を含む） | 撮影時刻を特定できる | `run-231001-1`、`hybrid-tx-231002-3`、`dual-leg-CAM-A-run-231002-2` など連番の合成値 |
| 原画像の SHA-256 とサイズ | 撮影した画像を特定できる | 合成ダミー JPEG（後述）の SHA-256 とサイズ |
| 承認 profile の ID（採取日入り）・承認参照・profile の SHA-256 | あたる | `single-cam-a-fixture`、`app:f231…`、`f231…f1` の合成値（SHA-256 は再計算していない） |
| 日付（`yyyy-MM-dd`） | 採取日 | 一律 278 日戻して 2026-01 内へ。時刻と相互の間隔（5 分の照合期限、180 秒の watchdog など）は実物のまま。再現のための正規化で、秘匿のためではない（ずらした日付は実際の採取日ではない） |
| WPD の機体 ID のハッシュ、シリアル番号、PnP ID、PC 名 | あたる | ファイルごと使わない。fixture に一切現れない |
| カメラ機種名、画像形式、画像サイズ、firmware 欄（`unknown`） | あたらない | そのまま |
| 撮影した原画像 | あたる | 使わない。`images/*.jpg.b64`（下記）に置き換え |

合成ダミー JPEG（`images/single-cam-a.jpg.b64`、`images/dual-cam-a.jpg.b64`、`images/dual-cam-b.jpg.b64`）は 1×1 画素の JPEG の宣言サイズだけを 7360×4912 に書き換え、COM セグメントに「合成画像で写真ではない」と書いたもの（各 700 バイト前後）。アプリの原画像検査（SOI・EOI・SOF のサイズ）を通す最小限の形で、EXIF・撮影情報は持たない。リポジトリには画像ファイルとして置かず、base64 の文字列だけを置く（`.gitignore` が `*.jpg` を除外しているのと同じ理由）。

## 実物と再構成の区別

| ファイル | 区別 |
| --- | --- |
| `dual-camera/*/pair-journal.json` | 実物（匿名化後）。`terminalResultHex` は Agent が書いた terminal result の UTF-8 を 16 進にしたもの。匿名化後の JSON を同じ形式で符号化し直した |
| `dual-camera/*/terminal-result.decoded.json` | 上の `terminalResultHex` を復号しただけの写し（レビュー用）。試験が一致を検査する |
| `dual-camera/*/diagnostics-*.events.jsonl`・`*.summary.json` | 実物（匿名化後）。Persisted 行の bytes・sha256 はダミー JPEG の値 |
| `single-camera/events.jsonl`・`summary.json`・`hybrid-capture-summary.json`・`transaction.json` | 実物（匿名化後）。transaction.json の originalSizeBytes・originalSha256 はダミー JPEG の値 |
| `single-camera/approved-capture-profile.json` | 実物（匿名化後）。実機が返した 9 設定（fileType は未広告、focusMode は不透明値 1）を含む |
| `dual-camera/operator-inputs/*`・`dual-camera/recovery-state/*` | 実物（匿名化後） |
| `single-camera/agent-capture-response.json` | 再構成。Agent は応答の生バイトを保存していないため、実物の transaction.json を `SerializeHardwareCameraAgentResponse` の項目順に並べ直して作った。外側（`requestId` は `req-replay-1`、`resultCode`、`marker`）は合成。C++ 試験が「実物の journal を Agent に読ませると、この応答と一致する」ことを確かめている |

二台の Agent 応答の封筒（`schemaVersion`、`requestId`、`resultCode`）も保存されていないので、試験側が合成する。`result` の中身だけが実物（pair journal の terminal result）。

## 再生時に試験が行う置換

fixture は匿名化した値のまま置き、次の項目だけ試験が差し替える。それ以外のバイトは動かさない。

- transaction ID: アプリが撮影のたびに新しく作る ID に置換（`f231…` → アプリの ID）。原画像の置き場所もその ID のフォルダ
- ルートパス: `C:/a0-replay-fixture/…` を試験用の一時フォルダに置換
- 一台撮影の profile 期限: 準備の検査が実時計と比べるため、期限だけ 2099 年へ置換
- 二台撮影の時刻: 画面（shell）経由の再生だけ、アプリ自身が作る照合時刻・watchdog 時刻に合わせて Agent の応答内の時刻を置換（Agent 自身の時刻間隔は保つ）。ワークフロー直の再生は実物の時刻のまま、時計を固定して流す

## どの試験が使うか

- C#（`tests/m3/OperatorShellTests/HardwareReplayRunner.cs`、`HardwareReplayFixtures.cs`、`Program.cs` 末尾の 1 試験と 1 シナリオ。`--hardware-replay` で単独実行できる）
  - 匿名化の検査: 許可した拡張子だけ・実画像ファイルなし、絶対パスは合成ルートのみ、32/64 桁 16 進は合成値かダミー JPEG の SHA-256 のみ、13 桁エポック・ユーザー領域・シリアル・PnP ID 形式なし、日付は 2026-01 内だけ。検査が効くことを、偽の識別子を使った例で確かめる
  - 実物の承認 profile から app の承認処理が同じ設定を作ること、一台撮影の保存（`HardwareSingleCameraViewModel`）、旧配置（`hybrid-tx-` フォルダ）の拒否
  - 二台撮影の Succeeded と Failed を `HardwareDualCaptureRecoveryOnlyWorkflow` と `HardwareOriginalExporter.ExportDualOriginalsAsync`、画面経由（`OperatorShellViewModel` の CaptureRecoveryOnly 保存）に流す
- C++（SDK なし構成の ctest）
  - `tests/dual_hardware_capture_backend_tests.cpp`: 実機の設定が二台のシャッター直前の検査（#222）を通る
  - `tests/hardware_camera_agent_tests.cpp`: 実物の event log・journal・応答と Agent の出力が一致する（#216）
  - `tests/dual_hardware_camera_agent_store_tests.cpp`: 実物の pair journal を読み戻し、同じ terminal result から同じバイト列を書く（#224 の空文字の書式）
  - `tests/wpd_dual_read_only_probe_tests.cpp`: firmware が異なる 2 台でも probe が通る

D810 の既知の性質と、fake の対応箇所の表は `docs/D810_PC_CONTROL_CAPABILITIES.md` にある。
