# 正規機械操作の入口（Windowsローカル）

この手順は `codex/dual-live-worker-poc-20260922` の開発候補に対するもの。main/releaseの状態は示さない。対象は保存済み確認結果の読取り・検証と、v4候補での指定したPending画像の閲覧専用表示である。実機操作・採用代行は未対応。GUI自動操作をこの正規経路の代用にしない。

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

## カメラ制御入口の運用前提（2026-10-05）

本番名のlease（`HardwareProcessLease`、`A0CameraStitcher.Phase0.CameraControl.v1`）を取るカメラ制御入口は、Phase 0 CLI、`A0CameraStitcher.CameraAgent`、`A0CameraStitcher.DualCameraAgent`、`A0CameraStitcher.SingleWorkerPreview` である。これらはlease取得時に委譲markerを検査し、次の条件で起動を拒否する。

| 状態 | 結果 | 備考 |
| --- | --- | --- |
| `%LOCALAPPDATA%\A0CameraStitcher\Phase0\DualDelegation\armed-session-*.marker` に当たるエントリ（どの session ID でも、ファイル・ディレクトリ・junction・非正規名の別を問わず）が1件でも残っている | `camera_control_delegation_quarantined` で停止 | 二台preview試作だけでなく、main の製品ビルドのカメラ制御入口もすべて拒否する |
| 上記ディレクトリ、またはその祖先（ドライブroot〜`%LOCALAPPDATA%`）にreparse point（junction・symbolic link）がある、固定ローカルドライブでない、作成できない | `camera_control_marker_failed` で停止 | プロファイルや `AppData` をjunctionで別ドライブへ移したPCで起きる見込み（※要検証） |

- 前任PC（AOPC-11-NOTE）には run-04 の委譲markerが残っている。そのPCで main の製品ビルドを動かすと、上記の入口は全拒否になる。markerの承認済み回復手順（[二worker試作記録](DUAL_LIVE_WORKER_POC.md) の本人判断パケットA）は未承認・未実装であり、markerを手で削除・編集して回避しない。
- marker名はWindows session IDを含む。2026-10-05 の修正（B）以前のビルドは現在の session ID の marker しか見ないため、再ログオン・再起動で session ID が変わると残存 marker を素通りする（コード読み。実行では※未検証）。B 以降のビルドは marker root の `armed-session-*.marker` を全件走査し、1件でもあれば `camera_control_delegation_quarantined` で止める。注意: B が守るのは B を含むビルドの実行ファイルだけで、`build/sdk-dual-poc/Release` など B 以前の既存 exe は再ログオン後も素通りする。「B が入った」とは、この PC で本番 lease を取る exe（Phase0 CLI・CameraAgent・DualCameraAgent・SingleWorkerPreview・PreviewCommissioning・PreviewWorker）をすべて B 入りで作り直した後を指す。また再ログオン後は `MarkerDiagnostic --read-only` が別 session の marker を `marker_ambiguous` で止める（現 session の候補1件だけを扱う設計）。別 session の marker を回復対象にする扱いは監査付き回復コマンド（C）の仕様として所有者が決めるまで未定で、それまで再ログオン・再起動の制限を残す。別ユーザープロファイル間の marker root は MVP の運用契約の範囲外。
- 運用規則（2026-10-05 追加）: カメラ制御の exe（Phase0 CLI・CameraAgent・DualCameraAgent・SingleWorkerPreview・PreviewCommissioning・PreviewWorker・MarkerDiagnostic・回復コマンド）を起動するのは運用者アカウントだけとし、AI エージェントやサンドボックス用アカウントから起動しない（別アカウントは自分の LocalAppData を見るため、運用者の marker が見えない）。ユーザープロファイルや LocalAppData の場所が変わった場合（レジストリの付け替え・一時プロファイルでのログオン・プロファイル移行）は復旧事象として扱い、古い root の marker を人が確認する。B 入りの exe への置き換えが終わるまで Windows Update の自動再起動を一時停止する。
- AOPC-20-NOTE では 2026-10-02 のWPD読取り列挙2回（`A0CameraStitcher.Phase0.exe inventory --transport wpd`、exit 0）でlease取得が通り、`MarkerDiagnostic --read-only` は `marker_missing` だった（[現在の開発状況](CURRENT_STATUS.md) の2026-10-02節）。
- 起動前の確認には読取り専用の `A0CameraStitcher.MarkerDiagnostic --read-only` を使う。markerの作成・削除は行わない。
- 二worker Live View試作（`A0CameraStitcher.PreviewWorker`・`A0CameraStitcher.PreviewCommissioning`）は、SDK有効ビルドでは既定でビルドされない。二台previewを動かすには、構成時に次のように明示し、各回の人の承認を得る。ADR-0031のgateは未達であり、製品機能ではない。

```powershell
cmake -S . -B build/sdk-dual-poc -G "Visual Studio 16 2019" -A x64 "-DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk" "-DA0_BUILD_DUAL_PREVIEW_POC=ON"
# PowerShell では -D の値を引用符で囲む。囲まないと "=." で引数が割れ、黙って SDK なしの stub 構成になる。
# configure ログに「Nikon D810 licensed adapter enabled」と CMake Warning（試作を SDK 有効でビルドする旨）が出ることを確認する。
cmake --build build/sdk-dual-poc --config Release
```

## 次工程の照会専用画面（開発中）

履歴画面に `readOnly` モードを追加し、そのモードでは採用ボタンを非表示・無効化し、採用イベントも拒否する。`--historical-review-window` のsoftware-only試験1回で画面ゲートと既存の画像検証を確認した。通常の人手による履歴・採用経路は変更しない。

開発ソースv4では、状態照会の読取り専用pipeを変えず、別の `gui-show-review --instance <MainWindowのPID-起動ticks> --result-id <小文字GUID N> --image <stitched|cam-a|cam-b>` を追加した。要求に保存先pathは含めず、GUIが既に保持するProduct/Simulated保存先からPendingの1結果を選ぶ。同一利用者・同一logon session、対象processのPID/起動ticksと接続先pipe server PIDを照合し、UIの `TryBeginHistoricalReview` を先に取得する。記録と原画像・合成画像を再検証し、記録を再読取りした後、選んだ画像のhashを同一streamで再照合する閲覧専用ウィンドウを開く。採用・撮影・設定write・WPD・削除はコマンドにない。終了または拒否で履歴操作ゲートを解放する。45秒の協調期限を設けるが、同期OS呼出しの強制中断は保証しない。応答喪失・期限切れから「画面は開いていない」と推定せず、自動再送しない。同一利用者の悪意あるprocessを認証する仕組みではない。

v4のsoftware証拠: CLI/Foundation Release buildとOperatorShell Release buildはいずれも警告0・エラー0。別process CLI→実Windows pipe→fake表示handlerの専用試験2回はPASSし、正常応答、画面not-ready、対象unavailable、不正コマンド・result ID、古いinstanceを確認した。隔離したSimulated保存先を使う実MainWindowの専用系列は4/5回実行（初回compile警告による不実行、直接client PASS、別process CLI PASS、既存status endpointの前後ゲート確認を加えてPASS）。指定CAM-A画像のWPF viewerが可視、履歴ゲートの保持/解放、未採用記録と原画像の不変、改変後の表示拒否を確認した。Productの実結果、操作者/AIの画像内容の目視、別logon session拒否の実測は**未実施**。この開発ソースは固定候補 `software-c415668-01` を変更せず、配布候補にもしていない。CLIの `Displayed` はウィンドウを開けたという報告であり、AI/人が画像内容を見たこと、品質・実機の受入、採用許可ではない。

## 2026-09-24の限定作業結果

`scripts/New-LocalSoftwareCandidate.ps1` をv4契約に合わせ、候補ディレクトリ作成前にソースCLIの `describe` を照合できる `-PreflightOnly` を追加した。変更は `dab9c0be3fc2b1e31f8087309cab957d2af8719e` にコミット済み。構文検査・`git diff --check`・事前検証はPASSし、事前検証では候補・native buildの両ディレクトリが未作成であることを確認した。

SDK root空のレシピで新規候補 `build/local-software-candidates/software-dab9c0b-v4-01` を1件だけ作成した。`candidate.manifest.json` SHA-256は `2BC584D763DB74B45F5AA78E0DDDA6E6777263CA4D9C930FAF12C0CC723DFCB5`。source commitは上記、23ファイルのsize/hash照合は全件PASS、`sdkIncluded=false`、`cliContractVersion=4`、同梱CLIの `describe.version=4` と `gui-show-review` を確認した。`hardwareAccepted/guiAccepted/redistributionApproved=false` は維持。候補系列は5/5を消費し、旧候補は変更していない。nativeビルドには既存のコードページ警告C4819が出たが生成は成功した。

GUI表示の限定試験1回は、**同梱CLI→開発ソースのSimulated MainWindow**でPASSした。CAM-Aの検証済み画像viewerの可視、履歴ゲートの保持/復帰、記録・原画像の不変、改変後の表示拒否、撮影/採用/実機操作0を確認した。開発GUIを使う試験ハーネスは同梱GUIではないため、計画した「同梱CLI→同梱GUI」のE2EとGUI受入は**未達**。この差をCLI応答だけで埋めない。新候補生成の残枠は0であり、追加生成や同条件の再試験は行わない。Product実結果、画像内容の人/AI目視、clean PC、実機・品質・配布・mergeも未受入。

## 次の業務時間の限定作業（実施前の計画記録）

以下は2026-09-23に記した当時の計画であり、現在の未実施タスクリストではない。2026-09-24の実施結果と未達範囲は直前の節を正とする。

1. `git remote`・branch・dirty・source SHA、既存候補と進行中担当、run-04隔離条件を再確認する。物理接続状態は推測しない。実機操作を要する場合はこの作業から切り離して停止する。
2. 候補生成レシピ [`scripts/New-LocalSoftwareCandidate.ps1`](../scripts/New-LocalSoftwareCandidate.ps1) は現時点で `describe.version == 3`、manifestの `cliContractVersion = 3`、READMEの「CLIはGUI commandを送れない」を固定している。v4ソースに対してこのまま実行すると、候補ディレクトリ生成後に契約不一致で停止し、不完全候補を残す。先にレシピの検査・manifest・READMEをv4の `gui-show-review` の閲覧限定契約へ修正し、差分を確認して通常ブランチにコミットする。既存v2/v3候補やmanifestは上書きしない。
3. 修正後のclean source commitを固定し、Windows 11 x64・PowerShell 7・.NET 10 Desktop・Visual Studio 2022 Build Tools/CMake・Gitを確認する。固定ローカルドライブの新規一意名を使い、SDK root空のレシピで**SDK非同梱のローカル候補を1件だけ**生成する。失敗や応答不明なら成果ディレクトリとprocess状態を確認し、同じ名前で再実行・上書きしない。候補系列は既に4/5を消費しているため、追加生成は残り1件に収める。
4. 候補のmanifest・全ファイルのsize/hash・source commit・`sdkIncluded=false`・`cliContractVersion=4`を再読取りし、同梱CLIの `describe` でv4と `gui-show-review` を確認する。隔離したSimulated fixture・同一Windows利用者/sessionで、**同梱CLI→同梱GUI→指定した未採用結果の閲覧専用画像**を限定1回確認し、結果ID/画像種別、可視viewer、履歴ゲートの保持と復帰、記録・原画像の不変、改変画像の拒否、撮影/採用/実機操作0を記録する。CLIの応答だけで可視・画像内容の評価を代用しない。同条件の失敗を自動再試行しない。

候補生成とSimulated E2Eが合格しても、clean PC、Product実結果、操作者/AIの画像内容の目視、二台同時Live View、撮影・合成品質、配布・mergeは別の受入である。run-04の委譲markerは保持し、残り実機preview枠1回、run-05、撮影、SDK/WPD、marker解除には進まない。業務時間の具体的定義に既存の本人指定があればそれを優先する。
