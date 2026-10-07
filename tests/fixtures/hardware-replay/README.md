# 実機応答の再生 fixture（Issue #231）

実機（Nikon D810）が実際に返した応答を匿名化して置き、アプリの保存経路と Agent の応答生成へ流すための golden fixture。#216（原画像フォルダ名）・#222（fileType を広告しない）・#224（bindingInvalidationReason の空文字）はどれも「fake と実機の応答の差」で、SDK なし構成の ctest と `Test-M3Simulated.ps1` では出なかった。ここの値は fake を作るときの正解として使う。

このフォルダの内容は公開される。実機の識別子（シリアル番号・WPD の機体 ID・PnP ID・ユーザー名・PC 名・ローカルの絶対パス）と撮影した原画像は置かない。守りは 2 段で、役割が違う。

| 段 | 何を見るか | どこで動くか | 保証できないこと |
| --- | --- | --- | --- |
| 形の検査 | 識別子の「形」 | 試験（C# の `HardwareReplayAnonymizationRules.Scan`）。`OperatorShellTests` の中にあるので、`Test-M3Simulated.ps1` を実行したときだけ動く。ctest には C# の試験が登録されていないため、ctest では動かない | 形が目立たない値（7 文字の本体シリアル、中立な名前の欄に入った承認 GUID など）は見つけられない |
| push 前のローカル照合 | 操作 PC 上の実値との一致 | 手元で `scripts/Test-ReplayFixtureLeak.ps1` を実行（下記）。CI では動かせない（実値が手元にしかない） | 実値を読めない PC では何も保証できない（その場合は検証不能で止まる） |

形の検査が通っても、push してよい証拠にはならない。公開リポジトリへの push は取り消せないので、ローカル照合を通してから push する。

## 形の検査（`Test-M3Simulated.ps1` の試験が行う）

`HardwareReplayAnonymizationRules.Scan` が fixture の全ファイルに対して行う。

- ファイルの種類: `.md` `.json` `.jsonl` と、`images/*.jpg.b64`（`images/` 直下の `*.jpg.b64` だけ）のみ。実画像ファイルは置けない。`.b64` を他の場所・名前に置くと落ちる（検査を飛ばさない）
- ダミー画像（全 `.b64`）: 復号して JPEG の構造を検査する。許可するセグメントは JFIF（E0）・注記（FE）・量子化表（DB）・フレームヘッダ（C0）・ハフマン表（C4）・スキャンヘッダ（DA）だけで、長さは固定。EXIF（E1）・ICC・APPn・未知のマーカーは落ちる。COM（注記）は決まった全文（`A0 replay fixture: synthetic image, not a photograph (<ラベル>)`）と完全一致、画像データは 32 バイト以下、宣言サイズは 7360×4912。復号した中身にも下記のテキスト規則をかける
- 絶対パス: 合成ルート `C:/a0-replay-fixture/` 以外は落ちる
- 16 進の連なり（16 桁以上）: 合成値の「形全体」か、ダミー画像の SHA-256 だけ許す。合成値の形は `f231` + 4 桁 + 0 の連なり + 2〜4 桁（32 桁または 64 桁）。`f231` で始まるだけの値は落ちる。ダッシュ付き GUID も同じ基準
- 日付: 2026 年 1 月だけ許す。`yyyy-MM-dd`・`yyyy/MM/dd`・`yyyy.MM.dd`・EXIF の `yyyy:MM:dd`・`yyyyMMdd`・和文の日付をすべて見る
- エポック秒: 10 桁（秒）・13 桁（ミリ秒）・16 桁・19 桁
- メールアドレス
- 語の規則（README を除く）: `serial`・`appdata`・ユーザー領域のパス・PnP/USB の ID 形式・`XX-99-NOTE` 形の PC 名・Windows の既定のホスト名（`DESKTOP-` など）・ホスト名やアカウント名や所有者の欄名
- 中立な名前の欄に入った識別子風の値（README を除く）: 数字を含む英数字 5〜24 文字の文字列値、6 桁以上の数字。許可は `7360x4912` だけ（明示のリスト）
- 試験を実行している PC の名前とユーザープロファイルのパス: 試験の実行時に `Environment` から読んで照合する。値はリポジトリに置かない

検査が効くことは、規則ごとに架空の値を入れた自己試験（`HardwareReplayRunner.cs` の `AnonymizationRulesRejectKnownBadShapes` と `AnonymizationRulesCloseTheKnownGaps`）で確かめる。失敗メッセージには値を出さず、位置（オフセット）だけを出す。

## push 前のローカル照合（必須）

この公開リポジトリへの push はすべて、push の前に **実値を持っている操作 PC で** 次を実行する。

```powershell
pwsh -NoProfile -File scripts/Test-ReplayFixtureLeak.ps1 -Base origin/main
```

- `-Base` は push の起点（既定は現在のブランチの upstream、無ければ `origin/main`）。`<Base>..HEAD` の **全 commit** を見る。履歴ごと公開されるので、途中の commit に入れて後で消した値も対象になる
- 実値の読み込み元（実行時に読み、メモリ上だけで使う。ディスクにもリポジトリにも書かない）
  - `%LOCALAPPDATA%\A0CameraStitcher\` の記録（ID・ハッシュ・run ID・承認参照・サイズ・日付・パス）
  - 同フォルダの原画像の EXIF（所有者・シリアル・日付・固有 ID・メーカーノート）、SHA-256、サイズ
  - レジストリ（Nikon の USB と WPD の機器キー）、登録された所有者
  - 環境（PC 名・ユーザープロファイルのパス）
- 照合範囲: commit メッセージ、追加行、追加行の中の hex と base64 の復号結果（`terminalResultHex` を含む）、`.b64` の復号結果。各値は、そのまま・ダッシュ付き GUID・UTF-8 の hex・base64 の 3 通りの桁合わせでも探す。ただし 6 文字未満の値は hex と base64 の形を作らず、6〜7 文字の値は base64 の桁合わせの一部を作らない。範囲内のバイナリファイルは一致として報告する。`-IncludeChangedFiles` を付けると、範囲が追加・変更したファイルの全文も見る。読むのは HEAD の内容ではなく作業ツリーのファイルで、未コミットの変更を含む（HEAD と作業ツリーが違うとき、HEAD の内容は見ない）
- 出力はラベル（`カテゴリ#番号`）と場所（commit・ファイル・行）だけで、値は出さない
- 終了コード: `0` = 一致なし。`1` = 一致あり（push しない。ラベルから、どの種類の値がどこにあるか分かる）。`2` = 検証不能（実値の読み込み元が無い、needle が作れない、エラー）。**`2` は「問題なし」ではない。push しない**
- 一致の扱い: 実際の採取日が地の文に出た、などの誤検出はありうるが、実値かどうかは必ず人が見て判断する。実値なら、push 前の自分の commit を作り直して消す（消す commit を追加するだけでは履歴に残る）。誤検出のまま通す場合は、理由を Issue かレビューに残す
- 動作確認: `pwsh -NoProfile -File scripts/Test-ReplayFixtureLeak.ps1 -SelfTest`（架空の値だけの一時リポジトリで、一致する場合・しない場合・検証不能の場合を確かめる。操作 PC でなくても動く）
- 実値を持たない PC（CI を含む）では `2` になる。その PC からは push しない
- 照合には既知の穴がある。Issue #243 を参照。fixture を足す前に塞ぐ

この手順は、変更の中身を問わず、この公開リポジトリへの push の前に必ず行う。Issue・PR に貼ってよいのは、カテゴリ別の件数と exit code だけ。一致の位置（commit・ファイル・行）や絶対パスは貼らない。

## 日付と、すでに公開されている値の扱い

- 日付を一律にずらすのは、時刻の間隔や 5 分の期限を壊さず再現するための正規化で、**秘匿のためではない**。ずらし幅を上の表に書いているので、ずらした日付から実際の採取日は復元できる。日付を伏せたい値は fixture に入れない
- `main` に既に公開されている値（PC 名・実日付・エポック・transaction ID・原画像の SHA-256・サイズ・profile ID・run ID。`docs/CURRENT_STATUS.md`、`docs/evidence/phase0/`、既存の試験にある）は、取り消せない既知の露出として扱う。履歴は書き換えない。fixture にも文書にも再掲・追記しない。新しく足した行にそれらが出ると、ローカル照合が一致として報告する。Issue・PR・commit メッセージも同じ扱いで、PC は別名（例: 操作 PC-1）で書く
- 現行ファイルに残っている実値を、通常の commit で合成値に置き換えるかどうか: ※要確認（人が判断する）

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
  - 匿名化の検査（形の検査。上の「形の検査」）。検査が効くことを、規則ごとに架空の値を使った例で確かめる
  - 実物の承認 profile から app の承認処理が同じ設定を作ること、一台撮影の保存（`HardwareSingleCameraViewModel`）、旧配置（`hybrid-tx-` フォルダ）の拒否
  - 二台撮影の Succeeded と Failed を `HardwareDualCaptureRecoveryOnlyWorkflow` と `HardwareOriginalExporter.ExportDualOriginalsAsync`、画面経由（`OperatorShellViewModel` の CaptureRecoveryOnly 保存）に流す
- C++（SDK なし構成の ctest）
  - `tests/dual_hardware_capture_backend_tests.cpp`: 実機の設定が二台のシャッター直前の検査（#222）を通る
  - `tests/hardware_camera_agent_tests.cpp`: 実物の event log・journal・応答と Agent の出力が一致する（#216）
  - `tests/dual_hardware_camera_agent_store_tests.cpp`: 実物の pair journal を読み戻し、同じ terminal result から同じバイト列を書く（#224 の空文字の書式）
  - `tests/wpd_dual_read_only_probe_tests.cpp`: firmware が異なる 2 台でも probe が通る

D810 の既知の性質と、fake の対応箇所の表は `docs/D810_PC_CONTROL_CAPABILITIES.md` にある。
