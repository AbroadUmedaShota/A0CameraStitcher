# 正規機械操作の入口（Windowsローカル）

この手順は `codex/dual-live-worker-poc-20260922` の開発候補に対するもの。main/releaseの状態は示さない。対象は保存済み確認結果の読取り・検証であり、実機操作、GUI制御、採用代行は未対応。GUI自動操作をこの正規経路の代用にしない。

## 開発ソースの契約v3（下記の凍結候補v2とは別）

開発ソースに `gui-status --instance <PID-UTC起動ticks>` を追加した。本体MainWindowの「バージョン」に表示される読取り専用instanceを明示する。Launcher/HardwareSingle画面にはendpointがなく、自動探索・別instanceへのfallbackをしない。古い `software-f9cafd4-01` は契約v2で、本操作を持たない。上書き更新しない。

v3のCLIは同一Windows利用者・同一ログオンセッションのMainWindowへ固定名Named Pipeで接続し、PIDと起動時刻・OSのpipe server PIDを検証する。本体も接続相手のOS process/sessionを確認する。接続と要求/応答は5秒の協調deadline、要求256 bytes/応答4096 bytes。同期OS呼出しの強制中断は保証しない。照会は同じDispatcher上でGUIの状態/操作可否を読むだけで、Commandを呼ばない。結果にはinstance/process identity、観測時刻、server契約version/build、Simulated/HardwareDual、画面state、busy、live-view状態、撮影/履歴ボタンの可否、終了処理中を含む。CLI envelopeのbuildとserverのbuildは別々に確認する。

`CanCapture`等がtrueでも業務承認ではない。正規AI経路は照会のみで、画像表示・撮影・次の準備・採用はまだ実行できない。読取りendpointの障害は取得不能でありカメラ故障や実機停止を意味しない。終了時はカメラの既存終了確認を維持した後にendpointを終了する。終了確認失敗で本体が残る場合はendpointも残り、既存の安全ゲートを変えない。同一アカウントの悪意あるプロセスを信頼するための署名/アプリ認証基盤ではなく、指定されたprocessを照会する契約である。

実pipe/CLIの合成状態試験と、実GUIインスタンスへの接続受入は区別する。新しいv3候補の梱包/GUI接続受入は未実施。以下の候補照合/describe手順は引き続きv2の検証済み候補に限る。

## 1. 候補を特定する

本手順を置いたリポジトリルートを作業ディレクトリとする。現ホストの検証済みソフトウェア候補は次の一つ。別worktreeに候補がない場合、最新版を推測して選択・再build・downloadしない。担当者に対象候補を確認する。

- 候補: `build/local-software-candidates/software-f9cafd4-01`
- source commit: `f9cafd4e2b01f90e484ec20c949b0d33a8ea3779`
- `candidate.manifest.json` SHA-256: `2A284F550F0AC68070E9333466407301D642C92728FB7A4D7218845D7BBE0117`
- Windows 11 x64、同一Windows利用者、PowerShell 7/.NET 10。SDK非同梱、framework-dependent、clean PC未受入。
- この記録は公開・配布・実機・品質・AI採用代行の承認ではない。ローカル管理者や同一アカウントによる悪意ある改変に対する署名検証の代替でもない。

まずread-onlyで固定候補と内容を照合する。期待hashは検証記録の固定値を使い、その場で計算した値を期待値へ代入しない。

```powershell
$a0Candidate = Join-Path (Get-Location) 'build/local-software-candidates/software-f9cafd4-01'
$a0ManifestPath = Join-Path $a0Candidate 'candidate.manifest.json'
$a0ExpectedManifestHash = '2A284F550F0AC68070E9333466407301D642C92728FB7A4D7218845D7BBE0117'
if ((Get-FileHash -LiteralPath $a0ManifestPath -ErrorAction Stop).Hash -cne $a0ExpectedManifestHash) { throw 'Candidate manifest mismatch' }
$a0Manifest = Get-Content -LiteralPath $a0ManifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
if ($a0Manifest.sourceCommit -cne 'f9cafd4e2b01f90e484ec20c949b0d33a8ea3779' -or $a0Manifest.sourceDirty -ne $false -or $a0Manifest.sdkIncluded -ne $false) { throw 'Candidate identity mismatch' }
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

JSONで `appId=a0-camera-stitcher-review-cli`、`version=2`、`status=ok`、`environment=Windows-local standalone read-only`、`operations=describe/reviews/verify-review` を確認する。buildは上記source commitに対応する。version/操作が違う場合、互換性を推測して続行しない。資格情報・昇格・ネットワーク接続は不要で、現在のWindowsアカウントが読める既存ファイルだけが対象。WSL/cloud/別ホストからの接続はこの手順の対象外。GUI instanceには接続しない。

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
| 起動中GUIの状態 | 開発ソースv3 gui-status | 明示instance読取り、実GUI接続未受入、v2候補に非同梱 |
| GUIの対象結果表示 | なし | 未実装、UIAを正式APIとは呼ばない |
| GUIと同じゲートで撮影/次の準備 | なし | 未実装、実機承認も必要 |
| AIによる採用代行 | なし | 別の明示依頼とアプリ側認可が必要 |
| 実GUI/二台同時Live View/撮影品質 | 別受入 | 未完、CLI成功では代替不可 |

実行証拠・試験回数と最新の差分は [操作者仕様](OPERATOR_UI_SPEC.md)、ロードマップ状態は `.autodev/plan.json` の `roadmap_execution` を参照する。製品全体の機械操作適合は未達である。
