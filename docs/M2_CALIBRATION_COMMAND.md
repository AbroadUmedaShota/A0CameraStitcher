# JPEGからdraft校正一式を作る開発用コマンド（#315）

[JPEG検出器](M2_FIDUCIAL_DETECTOR.md)と[draft writer](M2_DRAFT_CALIBRATION_ARTIFACTS.md)を一度に実行する。入力は撮影済みのJPEGを指す検出jobと、reference pair・既知配置・数値policyを指定した校正job。出力は**既存形式のprofile v2のdraft＋詳細レポート**で、製品の撮影やLive Viewには接続しない。

平置きのチャートだけで校正する場合（#374）は `-Planar` を付け、校正jobに `a0.planar-calibration-job.v1` を渡す。jobと報告の中身は[平置きの推定](M2_PLANAR_CALIBRATION.md)。それ以外の手順と出力の形はこの文書と同じ。

## 準備と実行

Windows、PowerShell 7.4以降、既存の開発用nativeツール2つが必要。OpenCV DLLは検出器のexeの隣に置く。既存のCMake option `A0_BUILD_FIDUCIAL_DETECTOR=ON`、`A0_BUILD_DRAFT_CALIBRATOR=ON`でビルドし、製品へのOpenCVのリンク範囲は広げない。

```powershell
cmake -S . -B build/reference -G "Visual Studio 17 2022" -A x64 `
  -DA0_BUILD_FIDUCIAL_DETECTOR=ON -DA0_BUILD_DRAFT_CALIBRATOR=ON `
  -DOpenCV_DIR="<OpenCVのbuild/x64/vc16/lib>"
cmake --build build/reference --config Release --target A0CameraStitcher.FiducialDetector A0CameraStitcher.DraftCalibrator --parallel 1

pwsh -NoProfile -File scripts/Invoke-DraftCalibrationPipeline.ps1 `
  -DetectionJob <画像の隣にあるdetection-job.json> `
  -CalibrationJob <calibration-job.json> `
  -DetectorExecutable build/reference/Release/A0CameraStitcher.FiducialDetector.exe `
  -CalibratorExecutable build/reference/Release/A0CameraStitcher.DraftCalibrator.exe `
  -OutputDirectory <リポジトリの外かbuild/の下の新規フォルダ（親は存在すること）>
```

パスに空白がある場合は引用符で囲む。画像名の基準は**元のDetectionJobのフォルダ**。実行時のカレントフォルダや、保存したjob snapshotのフォルダには変えない。検出jobには3つ以上のoperator anchorを明示し、検出器がその対応をチャート模様で確認する。自動的なanchor決定やreference選択は追加しない。

両jobを読み取りロックし、元のバイト列をsnapshotに保存した状態でnativeツールを順次実行する。jobのサイズ上限は検出1 MiB、校正64 KiB。検出jobの入力契約、writerのvalidation必須条件、D810寸法・draft限定の条件は既存ツールが検査する。

記入済みの検出jobと校正jobは**リポジトリの外のフォルダ**に置く。記入すると撮影時刻・provenanceId・rigMeasurementsが入り、`*.json`は.gitignoreの対象外なので、リポジトリの中に置くとcommitに乗りうる。どちらかのjobがこのリポジトリの中、または祖先のフォルダに`.git`（フォルダかファイル）を持つ場所（別のworktree・メインのチェックアウト・submoduleを含む）にあれば、何も作らずにexit 2で止まり、stderrに`error=draft-calibration-pipeline-job-inside-repository phase=preflight`と出す。出力にはjobの写しとsessionが入るので、出力先も同じ判定で拒否する（`error=draft-calibration-pipeline-output-inside-repository phase=preflight`）。ただし、その作業ツリーの一番上の`build/`の下（.gitignoreの対象。合成試験の`-EvidenceParent build/reference`）は許す。シンボリックリンクやjunctionでリポジトリの外に見せかけた場所は見分けない。

nativeツールは長いパス（260文字以上）を扱わない。作業フォルダ`<出力名>.partial-<32桁>`の下で最も深い成果物（校正の`draft.partial-<プロセスID>/calibration-report.json`）が260文字以上になる出力先は、作業を始める前にexit 2で止まり、`error=draft-calibration-pipeline-output-path-too-long phase=preflight`と出す。出力先を浅いフォルダにする。

## 出力

```text
<新規出力フォルダ>/
  inputs/detection-job.json
  inputs/calibration-job.json
  observations/session.json
  observations/detection-report.json
  draft/profile.json
  draft/calibration-report.json
  logs/detection.stdout.txt
  logs/detection.stderr.txt
  logs/calibration.stdout.txt
  logs/calibration.stderr.txt
  pipeline-report.json
```

`draft/profile.json`を既存の評価用readerへ渡せる。詳細レポートとのprovenanceId・canonical fingerprintの結合、referenceの画像hash・残差、全学習・独立検証の記録は#313の形式のまま。新しいprofile形式は作らない。

`pipeline-report.json`（`a0.draft-calibration-pipeline.v1`）は実行の一覧。結果、各段階のexit code、品質`not-assessed`、retry 0、画像の参照方法、nativeが記録したprofile fingerprint、各snapshot/出力/logの相対名・サイズ・SHA-256を記録する。profile fingerprintの再実装ではなく、レポートの値を引き継ぐ。画像はコピーせず、元画像と元jobを別途保持する。画像の真正性や物理的な機体同一性をこの一覧だけで証明しない。

ログはローカルの診断用。nativeのエラーに入力パスが含まれる場合があるため、記入済みのjobと画像はリポジトリの外に、成果物はリポジトリの外かignoreされた`build/`等に保存し、公開Issueには数値と制御された結果だけを記録する。

## 成功・失敗と保存

| exit | 結果 | 保存 |
| --- | --- | --- |
| 0 | `draft-produced` | 観測・draft・詳細レポート・job snapshotを一組で公開 |
| 3 | `detection-failed` | 検出レポートを公開。sessionを作らず、校正は起動しない |
| 3 | `calibration-failed` | 観測と校正の診断レポートを公開。profileは無い |
| 2 | 入力・tool起動/契約・IOの失敗 | 完成フォルダとして公開しない。作業開始後のpartialは診断用に保持 |

引数の不足などPowerShell自体の呼び出しエラーはPowerShellの終了コードになる。

出力先の既存ファイル・既存フォルダは空でも拒否し、親フォルダも自動生成しない。専用の兄弟`<出力名>.partial-<random ID>`をWin32の排他的directory作成で確保する。ファイルをCREATE_NEWで保存・flush・再読し、最後に置換しないdirectory renameで公開する。同時実行で負けた側は自分のpartialだけを残す。

IO失敗時には、可能な範囲で`pipeline-error.json`と`logs/pipeline.stderr.txt`をpartialに残す。**完成した出力先へ移動していないpartialは成功として使わない**。rename競合時には、生成済みの候補の`pipeline-report.json`に加えてpublication失敗のerror記録がある。`publicationRequiresFinalDirectory=true`はこの区別を明示する。partialの自動再開・再試行・削除はしない。電源断後の永続性や実画像の同時変更への耐性を証明する試験ではない。

## 合成入力での確認

先に[#314の明示試験](M2_FULL_CALIBRATION_PIPELINE.md)で合成JPEGとjobを保存し、そのフォルダを検証scriptへ渡す。通常のCTestに重い処理は追加していない。

```powershell
pwsh -NoProfile -File scripts/Test-DraftCalibrationPipeline.ps1 `
  -FixtureDirectory build/reference/synthetic-full-calibration-20261015-final `
  -ToolDirectory build/reference/Release `
  -EvidenceParent build/reference
```

試験は実際のnativeツールを呼ぶ。JPEG18枚の成功出力を#314のlibrary実行とbyte照合し、全画像不変・両jobの書込みロック・各artifactの独立hash・空白/日本語/角括弧を含むパス・失敗時の後段非起動・非上書き・入力/IO失敗・並行実行を確認する。生成物は新規の合成試験フォルダ（`<EvidenceParent>/cpt-<12桁>`）に保持する。

フィクスチャはチェックアウトの`build/`の下にあり、そのままではjobがリポジトリの中と判定されて拒否される。そこで試験は、画像のパスを絶対パスに書き換えた検出jobと、校正jobのコピーを、ユーザーの一時フォルダの新しいフォルダ（`a0-cpt-jobs-<12桁>`）に置いて流す。全体が成功したらこのフォルダだけを消し（`jobs=removed`）、失敗したら診断のために残してパスを`jobs=`で表示する。出力のバイト列は画像の場所に依存しない（検出レポートには画像のファイル名とSHA-256だけが入る）。加えて、リポジトリの中のjob（追跡済みの例と、存在しないパス）・`.git`ファイルを持つ別の作業ツリーのjob・作業ツリーの中で`build/`の外の出力先・深すぎる出力先が、作業フォルダを作らずにexit 2と決まったstderrで止まることを確かめる。`EvidenceParent`が深すぎると、試験は流す前に止まる。

以前の版は、子のpwshを`$PSHOME`から起動していた。ストア版のPowerShellが起動できないのではなく、試験のフォルダ名（32桁のGUID）と出力名と`.partial-<32桁>`を足すと260文字近くになり、作業フォルダの作成（`CreateDirectoryW`、Win32エラー206）で止まっていた。深いフォルダを`EvidenceParent`に渡したときに起きる。今の版は試験のフォルダ名を短くし、子のpwshを実行中のpwsh（`[Environment]::ProcessPath`）から起動する。

2026-10-15: Windows / PowerShell 7.6.7 / native Releaseで46チェック・失敗0。生成したprofile/sessionは既存JSON schemaを通り、同じバイト列を既存.NET readerの生成artifact確認へ渡して7チェック成功（fingerprint・session SHA-256一致、製品のapproved使用gateでdraft拒否を含む）。native/CMake/.NETの実装変更は無い。jobの置き場所と出力先の深さの検査を足した版では、同じフィクスチャ・Windows / PowerShell 7.6.9 / native Releaseで55チェック・失敗0（増えた9つは上の拒否と`build/`の除外の確認）。

実写の検出・色/継ぎ目/解像品質、承認済みprofileの発行、撮影ごとの補正、製品アプリへの接続はこのツールの完了条件に含めない。
