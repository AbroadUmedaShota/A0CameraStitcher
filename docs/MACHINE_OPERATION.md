# 正規機械操作の入口（Windowsローカル）

この手順は `codex/dual-live-worker-poc-20260922` の開発候補に対するもの。main/releaseの状態は示さない。対象は保存済み確認結果の読取り・検証であり、実機操作、GUI制御、採用代行は未対応。GUI自動操作をこの正規経路の代用にしない。

## 契約v3の状態照会

開発ソースに `gui-status --instance <PID-UTC起動ticks>` を追加した。本体MainWindowの「バージョン」に表示される読取り専用instanceを明示する。Launcher/HardwareSingle画面にはendpointがなく、自動探索・別instanceへのfallbackをしない。古い `software-f9cafd4-01` は契約v2で、本操作を持たない。上書き更新しない。

v3のCLIは同一Windows利用者・同一ログオンセッションのMainWindowへ固定名Named Pipeで接続し、PIDと起動時刻・OSのpipe server PIDを検証する。本体も接続相手のOS process/sessionを確認する。接続と要求/応答は5秒の協調deadline、要求256 bytes/応答4096 bytes。同期OS呼出しの強制中断は保証しない。照会は同じDispatcher上でGUIの状態/操作可否を読むだけで、Commandを呼ばない。結果にはinstance/process identity、観測時刻、server契約version/build、Simulated/HardwareDual、画面state、busy、live-view状態、撮影/履歴ボタンの可否、終了処理中を含む。CLI envelopeのbuildとserverのbuildは別々に確認する。

`CanCapture`等がtrueでも業務承認ではない。正規AI経路は照会のみで、画像表示・撮影・次の準備・採用はまだ実行できない。読取りendpointの障害は取得不能でありカメラ故障や実機停止を意味しない。終了時はカメラの既存終了確認を維持した後にendpointを終了する。終了確認失敗で本体が残る場合はendpointも残り、既存の安全ゲートを変えない。同一アカウントの悪意あるプロセスを信頼するための署名/アプリ認証基盤ではなく、指定されたprocessを照会する契約である。

実pipe/CLIの合成状態試験と、実GUIインスタンスへの接続受入は区別する。旧v3候補 `software-a1aa215-01` の同梱CLI通信試験は完了したが、実GUI接続受入は未実施。以下はSDK非同梱候補の実機開始拒否と未取得表示を修正した新v3候補を固定指定する手順。新候補は生成・describe・ファイル照合済みで、変更していない既存の通信/保存検証系列（各5/5）は再実行していない。旧v2/v3候補は履歴として保持する。

## 1. 候補を特定する

本手順を置いたリポジトリルートを作業ディレクトリとする。今回の確認対象は次の候補に固定する。別worktreeに候補がない場合、最新版を推測して選択・再build・downloadしない。担当者に対象候補を確認する。

- 候補: `build/local-software-candidates/software-c415668-01`
- source commit: `c415668bbae4a858402960678aa529fc4e3857e6`
- `candidate.manifest.json` SHA-256: `F620F54BE5903057B1CB143908D4BF5E5AB84AFF1A64DBFF1F98E717CEE35752`
- Windows 11 x64、同一Windows利用者、PowerShell 7/.NET 10。SDK非同梱、framework-dependent、clean PC未受入。
- この記録は公開・配布・実機・品質・AI採用代行の承認ではない。ローカル管理者や同一アカウントによる悪意ある改変に対する署名検証の代替でもない。

まずread-onlyで固定候補と内容を照合する。期待hashは検証記録の固定値を使い、その場で計算した値を期待値へ代入しない。

```powershell
$a0Candidate = Join-Path (Get-Location) 'build/local-software-candidates/software-c415668-01'
$a0ManifestPath = Join-Path $a0Candidate 'candidate.manifest.json'
$a0ExpectedManifestHash = 'F620F54BE5903057B1CB143908D4BF5E5AB84AFF1A64DBFF1F98E717CEE35752'
if ((Get-FileHash -LiteralPath $a0ManifestPath -ErrorAction Stop).Hash -cne $a0ExpectedManifestHash) { throw 'Candidate manifest mismatch' }
$a0Manifest = Get-Content -LiteralPath $a0ManifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
if ($a0Manifest.sourceCommit -cne 'c415668bbae4a858402960678aa529fc4e3857e6' -or $a0Manifest.sourceDirty -ne $false -or $a0Manifest.sdkIncluded -ne $false -or $a0Manifest.cliContractVersion -ne 3) { throw 'Candidate identity mismatch' }
foreach ($a0Entry in $a0Manifest.files) {
    $a0File = Get-Item -LiteralPath (Join-Path $a0Candidate $a0Entry.path) -ErrorAction Stop
    if ($a0File.Length -ne $a0Entry.size -or (Get-FileHash -LiteralPath $a0File.FullName).Hash -cne $a0Entry.sha256) { throw 'Candidate file mismatch' }
}
$a0Cli = Join-Path $a0Candidate 'app/review-cli/A0CameraStitcher.M3.ReviewCli.exe'
```

これは記録されたファイルの同一性照合であり、実行中の改変を防ぐロックや任意パッケージを信頼するための汎用検証器ではない。不一致・欠落なら実行せず、対象を報告する。

## 2. 操作と権限を発見する

```powershell
& $a0Cli describe
if ($LASTEXITCODE -ne 0) { throw 'CLI discovery failed' }
```

JSONで `appId=a0-camera-stitcher-review-cli`、`version=3`、`status=ok`、`environment=Windows-local standalone read-only`、`operations=describe/reviews/verify-review/gui-status` を確認する。buildは上記source commitに対応する。version/操作が違う場合、互換性を推測して続行しない。資格情報・昇格・ネットワーク接続は不要。ファイル照会は現在のWindowsアカウントが読める既存ファイルだけが対象で、GUI接続は明示instanceのgui-statusだけ。WSL/cloud/別ホストからの接続はこの手順の対象外。describeではGUIへ接続しない。

## 3. 対象を明示して読む

以下は形式説明。実データを読む前に対象rootと目的を確認する。候補の配置先と製品保存先は別物であり、既定保存先を推測して全件走査しない。

```powershell
& $a0Cli reviews --root '<既存operator-reviewディレクトリの絶対path>' --offset 0 --limit 25
& $a0Cli verify-review --product-root '<operator-reviewの親の絶対path>' --result-id '<空でない小文字GUID N>' --expected-kind Simulated
```

実製品結果なら `--expected-kind Product` を明示する。Simulated成功をProductへ読み替えない。rootは固定ローカルdrive上の既存pathで、CLIはUNC/device/reparse等を拒否する。確認記録/lockがない場合は作成しない。

`reviews` は最大25件/page、最大走査1000件。ページ間更新によりoffset位置は変わり得る。`verify-review` はPendingの1件だけを対象に、固定同梱M2AdapterとGUI共通検証でmanifest・左右原画像・合成を検証し、相対path/hash/寸法等を返す。画像bytesやcamera identityを返さない。40秒の協調deadlineでありOS同期I/Oの強制停止保証ではない。

## 4. 結果を解釈する

終了コード0は当該照会の成功、2は拒否/失敗。stdoutのJSONにはrequestId、version、build、environment、観測時刻、operation、status、data/errorCodeがある。成功した空一覧と観測不能を区別する。`invalid_input`、`invalid_metadata`、`invalid_artifacts`、`access_denied`、`observation_unavailable`、`observation_timeout` は原因を直さず再送しない。JSON未取得/timeoutを成功・停止の証拠にしない。

画像検証成功はその観測時点の整合性だけであり、品質承認・実機成立・採用許可ではない。結果を採用する場合は人の明示操作時に再検証が必要。`accept`/`capture`は非公開で、CLIから撮影・設定write・SDK/WPD・削除・再合成・採用は行えない。

## 対応範囲と残件

| シナリオ | 正規経路 | 状態 |
| --- | --- | --- |
| 候補/契約発見 | 本手順＋manifest＋describe | SDK非同梱候補が対象 |
| 保存記録の一覧/1結果検証 | reviews / verify-review | 同梱CLIで合成fixture検証済み |
| 起動中GUIの状態 | v3 gui-status | 新候補に同梱、明示instance読取り、実GUI接続未受入 |
| GUIの対象結果表示 | 開発ソースv4 `gui-show-review` | 別パイプ契約とSimulatedの実WPF画面E2EはPASS。Productの実結果・人の目視は未受入。上記固定候補v3には含まれない |
| GUIと同じゲートで撮影/次の準備 | なし | 未実装、実機承認も必要 |
| AIによる採用代行 | なし | 別の明示依頼とアプリ側認可が必要 |
| 実GUI/二台同時Live View/撮影品質 | 別受入 | 未完、CLI成功では代替不可 |

実行証拠・試験回数と最新の差分は [操作者仕様](OPERATOR_UI_SPEC.md)、ロードマップ状態は `.autodev/plan.json` の `roadmap_execution` を参照する。製品全体の機械操作適合は未達である。

## 次工程の照会専用画面（開発中）

履歴画面に `readOnly` モードを追加し、そのモードでは採用ボタンを非表示・無効化し、採用イベントも拒否する。`--historical-review-window` のsoftware-only試験1回で画面ゲートと既存の画像検証を確認した。通常の人手による履歴・採用経路は変更しない。

開発ソースv4では、状態照会の読取り専用pipeを変えず、別の `gui-show-review --instance <MainWindowのPID-起動ticks> --result-id <小文字GUID N> --image <stitched|cam-a|cam-b>` を追加した。要求に保存先pathは含めず、GUIが既に保持するProduct/Simulated保存先からPendingの1結果を選ぶ。同一利用者・同一logon session、対象processのPID/起動ticksと接続先pipe server PIDを照合し、UIの `TryBeginHistoricalReview` を先に取得する。記録と原画像・合成画像を再検証し、記録を再読取りした後、選んだ画像のhashを同一streamで再照合する閲覧専用ウィンドウを開く。採用・撮影・設定write・WPD・削除はコマンドにない。終了または拒否で履歴操作ゲートを解放する。45秒の協調期限を設けるが、同期OS呼出しの強制中断は保証しない。応答喪失・期限切れから「画面は開いていない」と推定せず、自動再送しない。同一利用者の悪意あるprocessを認証する仕組みではない。

v4のsoftware証拠: CLI/Foundation Release buildとOperatorShell Release buildはいずれも警告0・エラー0。別process CLI→実Windows pipe→fake表示handlerの専用試験2回はPASSし、正常応答、画面not-ready、対象unavailable、不正コマンド・result ID、古いinstanceを確認した。隔離したSimulated保存先を使う実MainWindowの専用系列は4/5回実行（初回compile警告による不実行、直接client PASS、別process CLI PASS、既存status endpointの前後ゲート確認を加えてPASS）。指定CAM-A画像のWPF viewerが可視、履歴ゲートの保持/解放、未採用記録と原画像の不変、改変後の表示拒否を確認した。Productの実結果、操作者/AIの画像内容の目視、別logon session拒否の実測は**未実施**。この開発ソースは固定候補 `software-c415668-01` を変更せず、配布候補にもしていない。CLIの `Displayed` はウィンドウを開けたという報告であり、AI/人が画像内容を見たこと、品質・実機の受入、採用許可ではない。
