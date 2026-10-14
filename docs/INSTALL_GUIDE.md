# 社内 PC への導入・アンインストール・診断情報の回収

社内の技術担当者（PC への導入と不具合対応をする人）向けの手順書です。開発の手順（ビルドと試験）は [README](../README.md) の「Phase 0 CLI」「M3 application shell」を、撮影の操作は `docs/OPERATOR_GUIDE.md`（#298 で作成中）を見てください。この文書はそれらと重ならないように、PC に入れる・外す・調べるところだけを扱います。

前提（所有者の決定 2026-10-13、#293 の D2）:

- 配布先は社内の PC だけです。
- Nikon SDK はアプリに同梱しません。使用許諾に同意した利用者が、各 PC に自分で置きます。
- インストーラはありません。ビルドしたフォルダをそのまま置きます。

「※要確認」は、コードや既存の記録で裏付けが取れていない箇所です。手順として決まるまでは、所有者に確かめてから進めてください。

## 1. 動作環境

| 項目 | 内容 | 根拠 |
| --- | --- | --- |
| OS | Windows 11 x64 | `AGENTS.md` の Development |
| .NET | .NET 10 の Windows Desktop Runtime（x64） | アプリは `net10.0-windows`、framework-dependent で publish する（`--self-contained false`） |
| Visual C++ ランタイム | Visual C++ 再頒布可能パッケージ（x64）が要る見込み。※要確認（CMake に `CMAKE_MSVC_RUNTIME_LIBRARY` の指定が無く、既定の動的リンクと読める。clean PC での確認は未実施） | `CMakeLists.txt` |
| カメラ | Nikon D810。一台構成は 1 台、二台構成は 2 台と固定リグ | `README.md` の「MVPの前提」 |
| 接続 | USB で PC に直結 | 同上 |
| 保存先 | `%LOCALAPPDATA%` が固定ローカルドライブにあり、途中に junction・シンボリックリンクが無いこと | 下の 4 章 |
| 利用者 | カメラを操作する Windows アカウント 1 つで使う。別のユーザーのセッションやサービスから起動しない | `AGENTS.md`、`docs/MACHINE_OPERATION.md` |

clean PC で上の一覧どおりに動くかは、まだ確かめていません（範囲外。手順書ができた後に別の Issue で受け入れる）。

## 2. 配布物を作る（ビルドする PC で）

### 2.1 SDK 抜きの候補と、実機用の候補の違い

| 種類 | 作り方 | 実機で撮影できるか |
| --- | --- | --- |
| SDK 抜きの候補 | `pwsh -NoProfile -File scripts/New-LocalSoftwareCandidate.ps1` | できない。`app` の一つ上に `candidate.manifest.json`（`sdkIncluded: false`）ができ、起動モード選択画面と `--hardware-single` の両方で SingleCamera の開始が止まる。画面には「SDK非同梱のソフトウェア確認用候補。カメラ未照会」と出る |
| 実機用の候補 | 下の 2.2（SDK を有効にしてビルドする） | 実行時に SDK の場所を伝えれば、準備状態の確認へ進める |

SDK 抜きの候補は、simulated の動作確認と保存記録の閲覧には使えます。実機の PC へ置くのは実機用の候補です。

### 2.2 実機用の候補を作る

実機用の候補を作るスクリプトはまだありません。以下は `scripts/New-LocalSoftwareCandidate.ps1` と同じ流れを、SDK を有効にして手で行うものです。※要確認（この組み合わせで配布物を作った記録は無い）

前提条件:

- Visual Studio 2022 Build Tools（C++）、CMake 3.25 以上、.NET 10 SDK、git
- ビルドする PC に、使用許諾に同意して取得した Nikon SDK がある（リポの中なら `.tools/nikon/d810-remote-sdk`。`.tools/` は git の対象外）
- 作業ツリーに未コミットの変更が無い

手順（リポのルートで PowerShell 7 を使う）:

1. ビルドに使った commit を控えます。

   ```powershell
   git rev-parse HEAD
   git status --porcelain   # 何も出ないこと
   ```

2. SDK を有効にして native を構成します。`-D` の引数は引用符で囲みます。囲まないと黙って SDK 無しの構成になります。

   ```powershell
   cmake -S . -B build/install-native -G "Visual Studio 17 2022" -A x64 '-DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk'
   ```

   - 出力に `Nikon D810 licensed adapter enabled from ignored local SDK root` が出れば SDK 有効です。
   - `NIKON_D810_SDK_ROOT is incomplete or ambiguous; building the gated stub` が出たら、SDK の置き方が足りないか、同じファイルが 2 か所にあります。CMake は SDK root の下で `Module/Win/Header Files/Maid3.h` と `Module/Win/Binary Files/x64/Type0014.md3` をちょうど 1 つずつ探し、同じフォルダに `Maid3d1.h`・`NkTypes.h`・`NkdPTP.dll`・`NkRoyalmile.dll`・`dnssd.dll` があるかを見ます。このまま進めると、できた Agent は実機を操作できません。

3. native の実行ファイルをビルドします。並列ビルドでリンクのファイルロックが競合した時は、対象を 1 つずつビルドし直してください。

   ```powershell
   cmake --build build/install-native --config Release --target A0CameraStitcher.M2Adapter A0CameraStitcher.CameraAgent A0CameraStitcher.DualCameraAgent A0CameraStitcher.Phase0 A0CameraStitcher.MarkerDiagnostic
   ```

4. WPF アプリを publish します。`M2AdapterBuildDirectory` に 3 の構成を指定すると、3 つの Agent・M2Adapter が `app` に入ります。

   ```powershell
   dotnet publish src/m3/OperatorShell/A0CameraStitcher.M3.OperatorShell.csproj -c Release --self-contained false -o build/install-candidate/app "-p:M2AdapterBuildDirectory=$((Resolve-Path build/install-native).Path)"
   ```

5. 運用で使うコマンドラインの道具を、`app` とは別のフォルダに置きます。

   ```powershell
   New-Item -ItemType Directory build/install-candidate/cli
   Copy-Item build/install-native/Release/A0CameraStitcher.Phase0.exe, build/install-native/Release/A0CameraStitcher.MarkerDiagnostic.exe build/install-candidate/cli/
   ```

6. `app` の中身を確かめます。次の 10 ファイルがそろっていること、`build/install-candidate` に `candidate.manifest.json` が無いことを見ます。

   ```text
   A0CameraStitcher.CameraAgent.exe
   A0CameraStitcher.DualCameraAgent.exe
   A0CameraStitcher.M2Adapter.exe
   A0CameraStitcher.M3.Foundation.dll
   A0CameraStitcher.M3.Foundation.pdb
   A0CameraStitcher.M3.OperatorShell.exe
   A0CameraStitcher.M3.OperatorShell.dll
   A0CameraStitcher.M3.OperatorShell.pdb
   A0CameraStitcher.M3.OperatorShell.deps.json
   A0CameraStitcher.M3.OperatorShell.runtimeconfig.json
   ```

7. 配布物の各ファイルの SHA-256 と、1 の commit を、社内の記録に残します。公開の Issue に書いてよいのは実行ファイル（`.exe`）の SHA-256 だけで、パスは含めません（6.2 の 8）。

   ```powershell
   Get-ChildItem build/install-candidate -Recurse -File | Get-FileHash -Algorithm SHA256
   ```

配布物に SDK のファイル（`Type0014.md3` や同じフォルダの DLL）が入っていないことも確かめてください。SDK のパスは Agent に埋め込まれません（埋め込むとビルドが失敗するようにしてある）。

SDK のヘッダを使ってビルドした Agent を社内の別 PC へ配ってよいかは、使用許諾の読み方の問題です。※要確認（所有者と法務の判断。D2 は「SDK を同梱しない」までを決めている）

## 3. 導入する（使う PC で）

### 3.1 アプリを置く

1. .NET 10 Windows Desktop Runtime（x64）を入れます。入っているかは次で見られます。

   ```powershell
   dotnet --list-runtimes   # Microsoft.WindowsDesktop.App 10.x が出ること
   ```

2. 前に入れたアプリを上書きで入れ替える場合は、先に 5.2 の 1（アプリと Agent を閉じて終了を確かめる）と 4.2 の 4（二台の未確定の記録が無いこと）を済ませます。未確定の記録が残っていたら入れ替えを止めて、所有者に知らせます。
3. 2.2 で作った `build/install-candidate` を、固定ローカルドライブのフォルダへコピーします。置き場所は PC ごとに決めてかまいません。この文書では `<導入先>` と書きます（例: `C:\A0CameraStitcher\<候補名>`）。ネットワークドライブ・USB メモリ・junction の先には置かないでください。Agent は「WPF の実行ファイルと同じフォルダ」にあることを条件に起動されます。
4. `<導入先>` に書き込めるのを Administrators と SYSTEM だけにし、撮影する人のアカウントには読み取りと実行だけを付けます。`C:\` の直下に自分で作ったフォルダは、既定で `Authenticated Users` の変更権限を継承します。ほかのアカウントが Agent を差し替えられる状態にしないため、継承を外します。手順を流す導入担当と撮影する人が別のアカウントでも撮影する人が起動できるように、撮影する人のアカウントは SID で指定します（今サインインしている人の名前で指定すると、導入担当にだけ権限が付きます。Microsoft アカウントや Entra ID のアカウントでは名前が解決できないこともあります。※要確認）。

   1. 撮影する人のアカウントでサインインし、SID を調べます。`S-1-5-21-` で始まる値が出ます。この値は公開の場（Issue・PR・commit）に貼らないでください（6.1）。

      ```powershell
      whoami /user
      ```

   2. 管理者の PowerShell で、継承を外して権限を付け直します。`<撮影する人の SID>` には 1 で調べた値（`S-1-5-21-...` の形）をそのまま入れます。先頭の `*` はコマンドの側で付けます。

      ```powershell
      $dir = '<導入先>'
      $sid = '<撮影する人の SID>'
      icacls $dir /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' "*${sid}:(OI)(CI)RX"
      ```

      撮影する人に変更（`M`）は付けません。付けると、そのアカウントで動くほかのプロセスが Agent を差し替えられます。アプリと Agent が `<導入先>` に書き込まずに動くこと（読み取りと実行だけで起動から書き出しまで通ること）は ※要確認 です。

   3. フォルダと Agent の実行ファイルの両方で、付いている権限を確かめます。

      ```powershell
      foreach ($p in '<導入先>',
                     '<導入先>\app\A0CameraStitcher.CameraAgent.exe',
                     '<導入先>\app\A0CameraStitcher.DualCameraAgent.exe') {
          $p
          (Get-Acl -LiteralPath $p).Access |
              Select-Object IdentityReference, FileSystemRights, AccessControlType, IsInherited
      }
      ```

   - 3 つとも、出るのが `BUILTIN\Administrators`（`FullControl`）・`NT AUTHORITY\SYSTEM`（`FullControl`）・撮影する人（`ReadAndExecute, Synchronize`）の 3 行だけなら問題ありません。実行ファイルの行は `IsInherited` が `True` になります。
   - 撮影する人の行に `Write`・`Modify`・`FullControl` が付いている、または `Users`・`Authenticated Users`・`Everyone`・導入担当のアカウントが出たら、2 をやり直します。コピーのときに元の ACL を持ち込んだファイル（`IsInherited` が `False` の行がある）は、`icacls '<ファイル>' /reset` で親の権限に戻してから確かめ直します。※要確認（この手順で持ち込みの ACL が消えることは試していない）
   - 出力には撮影する人のアカウント名が出るので、公開の場に貼らないでください（6.1）。
   - `icacls` の戻り値が 0 以外のときは、管理者の PowerShell でない・フォルダの所有者でない・SID の書き方が違う、のいずれかの可能性があります。※要確認（この ACL の組み合わせでアプリを起動した記録は無い）
   - この後、`<導入先>` の入れ替え（3.1 の 2・3）と削除（5.2 の 4）は、管理者の PowerShell で行います。撮影する人のアカウントでは書き込めません。

5. `<導入先>` とその親フォルダに junction・シンボリックリンクが無く、固定ローカルドライブにあることを確かめます。同じ確かめ方を 3.2 の SDK のフォルダと `%LOCALAPPDATA%` にも使えます。

   ```powershell
   function Test-A0ReparseFree([string]$Path) {
       $item = Get-Item -LiteralPath $Path -Force
       $found = $false
       while ($item) {
           if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { "reparse point: $($item.FullName)"; $found = $true }
           $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
       }
       "drive type: $([IO.DriveInfo]::new([IO.Path]::GetPathRoot($Path)).DriveType)"
       if (-not $found) { 'no reparse point' }
   }
   Test-A0ReparseFree '<導入先>'
   Test-A0ReparseFree $env:LOCALAPPDATA
   ```

   `drive type: Fixed` と `no reparse point` が出れば問題ありません。出力にはユーザー名を含むパスが出ることがあるので、公開の場に貼らないでください（6.1）。

6. `<導入先>\app` の一つ上（`<導入先>`）に `candidate.manifest.json` が無いことを確かめます。SDK 抜きの候補の manifest が残っていると、実機の開始が止まります。

### 3.2 Nikon SDK を置く

利用者が使用許諾に同意して取得した SDK を、各 PC に置きます。リポや配布物からはコピーしません。

アプリが受け付ける置き方（`src/phase0/nikon_sdk_runtime_path.cpp` の検査）:

- `Type0014.md3` が、`...\Module\Win\Binary Files\x64\` で終わるフォルダにある（大文字小文字は区別しない）
- 同じフォルダに `NkdPTP.dll`・`NkRoyalmile.dll`・`dnssd.dll` がある
- 絶対パスで、固定ローカルドライブにある（`\\` で始まるパスとリムーバブルドライブは不可）
- ドライブのルートからファイルまでの途中に junction・シンボリックリンクが無い

SDK を展開したときの階層を崩さずに置けば、上の形になるはずです。置き場所は利用者が決めます（例: `C:\NikonSDK\d810-remote-sdk`）。Agent はこのフォルダの DLL を読み込むので、3.1 の 4・5 と同じ方法で、書き込めるのが Administrators と SYSTEM だけ（撮影する人は読み取りと実行だけ）であること、途中に junction が無いことを確かめてください。

### 3.3 SDK の場所をアプリに伝える

アプリは環境変数 `NIKON_D810_SDK_MODULE_PATH` だけを見ます。値は `Type0014.md3` の絶対パスです。WPF から起動する Agent はこの環境変数を引き継ぐので、WPF を起動する前に設定します。

既定は、その場だけ設定する方法です（PowerShell のウィンドウを閉じると消える）。WPF は、設定したのと同じウィンドウから起動します。

```powershell
$sdkRoot = 'C:\NikonSDK\d810-remote-sdk'   # 3.2 で置いた場所
$modules = @(Get-ChildItem -LiteralPath $sdkRoot -Recurse -File -Filter Type0014.md3 |
    Where-Object { $_.FullName.Replace('\', '/') -match '/Module/Win/Binary Files/x64/Type0014\.md3$' })
if ($modules.Count -ne 1) { throw 'Type0014.md3 (x64) must be unique.' }
$env:NIKON_D810_SDK_MODULE_PATH = $modules[0].FullName
# このウィンドウを閉じずに、4 章の確認と WPF の起動を続ける
```

例外として、所有者が可とした場合に限り、ユーザー環境変数に常設できます（新しく開いたウィンドウから有効）。常設すると、このユーザーで動くほかのプロセスもこの DLL の場所を読むことになります。常設してよいかは ※要確認（所有者の判断。まだ決まっていない）。

```powershell
# 所有者が常設を可とした場合だけ
[Environment]::SetEnvironmentVariable('NIKON_D810_SDK_MODULE_PATH', $modules[0].FullName, 'User')
```

失敗したとき: 未設定・パスの誤り・DLL の不足・ネットワーク上のパス・junction は、どれも撮影の前に止まります。Agent の結果には `sdk_load_failed`（`Nikon D810 SDK runtime module is unavailable, untrusted, or incomplete`）が出ます。3.2 の 4 条件を順に見直してください。

`NIKON_D810_SDK_ROOT` は別物です。ビルドのとき CMake に渡す変数で、実行時は Phase 0 CLI の `preflight` が SDK root の有無を表示するためにだけ読みます。アプリの撮影経路は読みません。

### 3.4 PC ごとに作り直すもの

次の記録は PC の `%LOCALAPPDATA%\A0CameraStitcher` に保存されるので、別の PC からは持ち込めません。新しい PC では作り直します。作り方は README と `docs/OPERATOR_GUIDE.md` に従い、この文書では場所だけ示します。

| 記録 | 場所（`%LOCALAPPDATA%\A0CameraStitcher\` の下） | 作るコマンド・画面 |
| --- | --- | --- |
| 一台の機体の結合 | `phase0\single-identity-v3.json` | `A0CameraStitcher.Phase0.exe bind-single-identity-v3 --alias CAM-A --single-camera-connected-confirmed` |
| 一台の承認済み撮影プロファイル | `camera-agent\approved-single-capture-profile.json` | WPF 一台画面の「観測値を30日プロファイルとして承認」 |
| WPD の機体の対応表 | `phase0\camera-map-wpd.json` | D810 を 1 台ずつつないで `A0CameraStitcher.Phase0.exe bind-identity --alias CAM-A --transport wpd --single-camera-connected-confirmed`（CAM-B も同じ） |

既に結合のファイルがある PC（アプリを入れ替えただけの PC など）では、`bind-*` のコマンドを流さないでください。既存の結合を作り直すかどうかは所有者が決めます。上書きしたときの挙動は ※要確認。流す前に次で有無を見ます（`True` が出たものは流さない）。

```powershell
$p0 = Join-Path $env:LOCALAPPDATA 'A0CameraStitcher\phase0'
'single-identity-v3.json', 'camera-map-wpd.json', 'camera-map.json' |
    ForEach-Object { '{0}: {1}' -f $_, (Test-Path -LiteralPath (Join-Path $p0 $_)) }
```

二台の撮影には、このほかに承認済みの二台プロファイルと `--dual-identity-proof` のファイルを起動引数で渡します（README の「実機二台（CaptureRecoveryOnly）」）。`--dual-identity-proof` に何を置くべきかは README でも ※要確認 のままです。

## 4. 起動を確かめる

### 4.1 simulated（カメラも SDK も要らない）

1. `<導入先>\app\A0CameraStitcher.M3.OperatorShell.exe --simulated` を起動します。
2. 画面に「模擬動作（実機未接続）」が出ることを確かめます。実機・SDK・WPD には接続しません。
3. 引数なしで起動すると起動モード選択画面が出ます。画面下部の「実行候補を読み取り確認（SDK・カメラは起動・照会しません）」の下に出る表示で、配置を確かめられます。この確認は SDK を読み込まず、カメラにも触れません。

   | 表示 | 意味 | 対処 |
   | --- | --- | --- |
   | 実行ファイルあり（SDK・実機接続は未確認） | Agent が `app` にあり、manifest が無い | 次の 4.2 へ |
   | SDK非同梱のソフトウェア確認用候補。カメラ未照会 | SDK 抜きの候補を置いた | 2.2 の実機用の候補に置き換える |
   | 実行ファイル未検出（SDK・実機接続は未確認） | `A0CameraStitcher.CameraAgent.exe` が `app` に無い | 2.2 の 6 で中身を見直す |
   | 候補manifestが不正です。実機開始不可 | `<導入先>\candidate.manifest.json` が壊れている | 実機用の候補には manifest を置かない |

simulated は `%LOCALAPPDATA%\A0CameraStitcher\m3-simulated`、`%LOCALAPPDATA%\A0CameraStitcher\dual-camera-test-synthetic-products`、`%TEMP%\A0CameraStitcher\simulated-exports` に書き込みます。

開発者がリポで simulated の試験一式を流すときは `pwsh -NoProfile -File scripts/Test-M3Simulated.ps1` を使います。これはビルドと試験を行うスクリプトで、導入先の PC で使うものではありません。

### 4.2 カメラ接続の準備状態（読み取りだけ）

ここから先はカメラをつなぎます。どれも読み取りだけで、撮影・削除・設定変更は行いません。実機の操作は所有者の指示と条件に従ってください。

この間、エクスプローラーで D810 を開かないでください。カメラをつないだときに出る自動再生の通知は、何も選ばずに閉じます。エクスプローラー・自動再生・写真アプリはほかの WPD クライアントとしてカメラを開くので、アプリの SDK・WPD のセッションと重なります（`docs/DUAL_LIVE_WORKER_POC.md` の F3）。

SDK（WPF・Agent）と WPD（Phase 0 CLI の `spool-status`）は同時に開きません。どちらかを動かす前に、もう一方のプロセスが終わっていることを次で確かめます（何も出なければよい）。以下では「プロセスの終了を確かめる」と書きます。

```powershell
Get-Process -Name 'A0CameraStitcher*' -ErrorAction SilentlyContinue
```

1. 委譲 marker が残っていないかを見ます。

   ```powershell
   & '<導入先>\cli\A0CameraStitcher.MarkerDiagnostic.exe' --read-only
   ```

   `{"status":"marker_missing",...}` なら問題ありません（このときの終了コードは 2 で、異常ではない）。それ以外が出たら、カメラ制御の入口がすべて止まります。marker を手で消さず、所有者に知らせてください（`docs/MACHINE_OPERATION.md` の「カメラ制御入口の運用前提」）。

2. 一台構成: 先に Phase 0 CLI でカードの空きを見て、その後で WPF を起動します。順番を逆にしないでください。
   1. WPF と Agent を閉じ、プロセスの終了を確かめます。
   2. カードの空きを見ます（`SpoolState: EMPTY` なら空）。このコマンドは WPD の読み取りセッションを 1 回開いて閉じます。

      ```powershell
      & '<導入先>\cli\A0CameraStitcher.Phase0.exe' spool-status --alias CAM-A
      ```

      `spool-status` は WPD の機体の対応表を参照します。一台の結合（`single-identity-v3.json`）だけの PC では、`wpd_identity_unbound` で `SpoolState: UNKNOWN` になった記録があります（`docs/CURRENT_STATUS.md`）。その場合は、次の WPF の準備状態確認の判定を使います。

   3. プロセスの終了を確かめてから、WPF を `--hardware-single` で起動し、画面の準備状態確認が「撮影準備OK」になることを確かめます。「撮影不可」なら、表示された理由を 6.3 の表と照らします。

      ```powershell
      & '<導入先>\app\A0CameraStitcher.M3.OperatorShell.exe' --hardware-single
      ```

   WPF を起動した後にもう一度 `spool-status` を流す場合は、WPF を閉じて Agent の終了を確かめてからにします。

3. 二台構成: 撮影の前に、WPF を閉じてプロセスの終了を確かめたうえで、Agent の読み取りの probe を順に流し、すべて `terminalState: Pass` であることを確かめます（README の「実機二台（CaptureRecoveryOnly）」、`docs/PHASE0_TEST_PLAN.md` の P0-B1）。どの probe の後も、プロセスの終了を確かめてから次へ進みます。

   ```powershell
   & '<導入先>\app\A0CameraStitcher.DualCameraAgent.exe' --read-only-sdk-probe
   & '<導入先>\app\A0CameraStitcher.DualCameraAgent.exe' --read-only-wpd-probe --wpd-camera-map "$env:LOCALAPPDATA\A0CameraStitcher\phase0\camera-map-wpd.json"
   & '<導入先>\app\A0CameraStitcher.DualCameraAgent.exe' --read-only-coexistence-probe --wpd-camera-map "$env:LOCALAPPDATA\A0CameraStitcher\phase0\camera-map-wpd.json"
   ```

4. 二台構成では、アプリを起動する前に未確定の記録が無いことも確かめます（#227 の事前確認）。場所は `%LOCALAPPDATA%\A0CameraStitcher\dual-camera-hardware-products` です。

   | 確認 | 撮影してよい状態 |
   | --- | --- |
   | `agent-pair-journal\active` | 無い |
   | `recovery-state\pending-transaction.json` | 無い、または中の `pendingRequest` が `null` |
   | `recovery-state\pending-capture-recovery-only.json` | 無い、または中の `pendingRequest` が `null` |

   次のコマンドで読み取りだけで確かめられます。`journal active: False` で、2 つのファイルがどちらも `none` か `pendingRequest is null: True` なら撮影してよい状態です。

   ```powershell
   $dp = Join-Path $env:LOCALAPPDATA 'A0CameraStitcher\dual-camera-hardware-products'
   'journal active: {0}' -f (Test-Path -LiteralPath (Join-Path $dp 'agent-pair-journal\active'))
   foreach ($f in 'pending-transaction.json', 'pending-capture-recovery-only.json') {
       $p = Join-Path $dp "recovery-state\$f"
       if (-not (Test-Path -LiteralPath $p)) { '{0}: none' -f $f; continue }
       $j = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
       $has = $j.PSObject.Properties.Name -contains 'pendingRequest'
       '{0}: pendingRequest is null: {1}' -f $f, ($has -and $null -eq $j.pendingRequest)
   }
   ```

   `ConvertFrom-Json` がエラーになった、`pendingRequest` の項目が無い（`False` と出る）ときも、撮影してよい状態とは扱いません。項目名は `src/m3/OperatorShell/Hardware/HardwareDualTransactionSnapshotStore.cs`（camelCase で保存）に合わせています。

   どれかに当たったら撮影を始めず、所有者に知らせます。ファイルを手で消すのは破壊的な操作で、所有者の明示の判断があるときだけです。

Phase 0 CLI は、`--artifacts` と `--reports` を指定しないと、今いるフォルダの下に `artifacts\phase0` と `docs\evidence\phase0` を作って証跡を書きます。決まった作業フォルダに移ってから実行し、そのフォルダも 5 章の「消してはいけないもの」として扱ってください。

## 5. アンインストール

アプリが作るデータには、製品の正本である原画像と、原因を調べるための証跡が入っています。アンインストールではアプリの本体だけを外し、データのフォルダには触れません。

### 5.1 消してよいもの・消してはいけないもの

| 区分 | 対象 | 扱い |
| --- | --- | --- |
| 消してよい | `<導入先>`（`app` と `cli`。アプリの本体と道具） | 5.2 の手順で消す |
| 消してよい | ユーザー環境変数 `NIKON_D810_SDK_MODULE_PATH`（3.3 の例外で常設した場合だけ） | 5.2 の手順で消す |
| この文書では扱わない | ビルドする PC のリポの `build/` の下 | 消すかどうかは開発者の判断です。二台 preview 試作など、実機の記録を `build/` の下に書いた例があるので、この手順書では消す手順を示しません |
| 消してはいけない | `%LOCALAPPDATA%\A0CameraStitcher` とその下すべて | 原画像（`original.jpg`）、retained の `.partial`、診断の証跡、未確定の journal を含む。下の表 |
| 消してはいけない | 操作者が選んだ書き出し先（既定は `%LOCALAPPDATA%\A0CameraStitcher\Exports`） | 書き出した原画像 |
| 消してはいけない | Phase 0 CLI を実行した作業フォルダの `artifacts\phase0` と `docs\evidence\phase0` | 実機の証跡 |
| アプリは扱わない | Nikon SDK を置いたフォルダ | 使用許諾に従って利用者が扱う。※要確認 |
| 所有者が決める | simulated だけのデータ（`m3-simulated`、`dual-camera-test-synthetic-products`、`%TEMP%\A0CameraStitcher\simulated-exports`） | 実機の画像は入らないが、消すかどうかはこの文書では決めない |

`%LOCALAPPDATA%\A0CameraStitcher` の下のフォルダ（コードで確かめた名前）:

| フォルダ | 中身 |
| --- | --- |
| `phase0\` | 機体の結合（`camera-map.json`、`camera-map-wpd.json`、`single-identity-v3.json`） |
| `phase0\camera-agent\artifacts\` | 一台の撮影の原画像と run ごとの証跡 |
| `phase0\camera-agent\reports\` | 一台の Agent の報告 |
| `phase0\camera-agent\transactions\` | 一台の transaction の状態 |
| `camera-agent\approved-single-capture-profile.json` | 一台の承認済み撮影プロファイル |
| `hardware-single\` | 一台画面の状態（`app-state.json`、`preferences.json`、`handoff-evidence\`、`operator-review\`） |
| `Exports\` | 一台の書き出しの既定の場所 |
| `dual-camera-hardware-products\transactions\<撮影ID>\` | 二台の原画像（`CAM-A\original.jpg`、`CAM-B\original.jpg`）と `diagnostics\` |
| `dual-camera-hardware-products\agent-pair-journal\` | 二台の Agent の journal（未確定の組は `active\`） |
| `dual-camera-hardware-products\recovery-state\` | 二台の結果不明の組を照会するための記録 |
| `dual-camera-hardware-products\capture-recovery-only-evidence\` | 5 組モードの証跡（`runs\`、`approvals\`） |
| `dual-camera-hardware-products\operator-review\` | 結果の確認記録 |
| `Phase0\DualDelegation\` | 委譲 marker の置き場所（Windows では `phase0` と同じフォルダ） |
| `m3-simulated\`、`dual-camera-test-synthetic-products\` | simulated の記録 |

### 5.2 手順

1. アプリを閉じます。二台の Agent は起動から 10 分で自然に終わるので、閉じたあと Agent が終わるのを待ちます。残っているプロセスは次で見られます（何も出なければよい）。

   ```powershell
   Get-Process -Name 'A0CameraStitcher*' -ErrorAction SilentlyContinue
   ```

   プロセスを強制終了しないでください。撮影中に止めると、結果不明の組が残ります。

2. 不具合を調べる予定があれば、先に 6 章で診断情報を回収します。
3. 4.2 の 4 の表で、二台の未確定の記録が残っていないかを見ます。残っていたらアンインストールを止めて所有者に知らせます。データのフォルダは残すので、記録が消えることはありませんが、次に入れたアプリで照会する必要があります。
4. `<導入先>` を消します。PowerShell 7（`pwsh`）で実行します。消す前に次を確かめ、1 つでも当たったら消さずに止めます。
   - `<導入先>` が絶対パスであること
   - `<導入先>` が固定ローカルドライブにあること（3.1 の 5 と同じ条件）
   - `%LOCALAPPDATA%` の下ではなく、`%LOCALAPPDATA%\A0CameraStitcher` を含んでもいないこと
   - `<導入先>` とその親、中のフォルダ・ファイルに `ReparsePoint`（junction・シンボリックリンク）が付いていないこと
   - `<導入先>` の直下が `app` と `cli` だけであること

   ```powershell
   $target = '<導入先>'
   if (-not [IO.Path]::IsPathFullyQualified($target)) { throw 'not an absolute path' }
   $full  = [IO.Path]::GetFullPath($target).TrimEnd('\')
   $local = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\')
   if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($full)).DriveType -ne 'Fixed') { throw 'not a fixed local drive' }
   if (($full + '\').StartsWith($local + '\', 'OrdinalIgnoreCase')) { throw 'target is under LOCALAPPDATA' }
   if (($local + '\').StartsWith($full + '\', 'OrdinalIgnoreCase')) { throw 'target contains LOCALAPPDATA' }
   $item = Get-Item -LiteralPath $full -Force
   for ($d = $item; $d; $d = $d.Parent) {
       if ($d.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "reparse point: $($d.FullName)" }
   }
   if (@(Get-ChildItem -LiteralPath $full -Recurse -Force -Attributes ReparsePoint).Count -ne 0) { throw 'reparse point inside target' }
   $names = @(Get-ChildItem -LiteralPath $full -Force | ForEach-Object Name | Sort-Object)
   if (($names -join ',') -ne 'app,cli') { throw "unexpected entries: $($names -join ',')" }
   Remove-Item -LiteralPath $full -Recurse -WhatIf
   ```

   `-WhatIf` の出力に出たパスが `<導入先>` の下だけであることを目で確かめてから、同じウィンドウで `-WhatIf` を外して実行します。

   ```powershell
   Remove-Item -LiteralPath $full -Recurse
   ```

5. 3.3 の例外で環境変数を常設していたら外します。

   ```powershell
   [Environment]::SetEnvironmentVariable('NIKON_D810_SDK_MODULE_PATH', $null, 'User')
   ```

6. Nikon SDK は、使用許諾に従って利用者が扱います。アプリの手順では消しません。

データのフォルダ（`%LOCALAPPDATA%\A0CameraStitcher` と書き出し先）を、いつ・誰の判断で片付けるかは、この文書の範囲外です。片付ける場合も、原画像と証跡の保管先を所有者が決めてからにしてください。

## 6. 不具合のときに診断情報を回収する

### 6.1 公開してはいけない情報

このリポと Issue は公開されています。次の情報は、公開の Issue・PR・commit に書かず、貼らないでください。

- PC 名（公開の場では「操作 PC-1」のように書く）
- Windows のユーザー名、およびそれを含む絶対パス（`%LOCALAPPDATA%` のように変数の形で書く）
- カメラのシリアル番号などの実機の識別子（公開の場では CAM-A / CAM-B と書く）
- 実際に撮影した画像と、その画像の SHA-256
- 実撮影の ID と、実機での記録の日時

回収したファイルには、これらが入っている前提で扱います（たとえば書き出し先のパスや画像の hash）。

回収物の保管先と、見てよい人は所有者が決めます。※要確認（まだ決まっていない）。決まるまでは、回収物をメールに添付したり、チャットに貼ったりしないでください。

### 6.2 手順

1. アプリを閉じ、Agent が終わるのを待ちます（5.2 の 1 と同じ）。撮影中のプロセスを止めて回収しないでください。
2. 委譲 marker の状態を控えます。出力は固定の語と匿名の hash だけで、パスや識別子は出ません。

   ```powershell
   & '<導入先>\cli\A0CameraStitcher.MarkerDiagnostic.exe' --read-only
   ```

3. 診断のフォルダをコピーします。移動ではなくコピーです。拡張子の許可リスト（`.json`・`.log`・`.txt`・`.md`）に当たるファイルだけを写し、それ以外（原画像、retained の `.partial`、画像形式のファイル、拡張子の無いファイルなど）は写しません。機体の結合のファイルも除きます。junction の先はたどりません（`/XJ`）。`<回収先>` は、所有者が決めた保管先の下の空のフォルダにします。

   ```powershell
   function Copy-A0Diagnostics([string]$SourceRoot, [string[]]$Targets, [string]$Destination) {
       foreach ($t in $Targets) {
           $src = Join-Path $SourceRoot $t
           if (-not (Test-Path -LiteralPath $src)) { continue }
           robocopy $src (Join-Path $Destination $t) *.json *.log *.txt *.md /S /XJ /COPY:DT /R:0 /W:0 `
               /XF 'camera-map*.json' 'single-identity-v3.json' | Out-Null
           if ($LASTEXITCODE -ge 8) { Write-Warning "copy failed: $t ($LASTEXITCODE)" }
       }
   }

   $out = '<回収先>'
   $targets = @(
       'phase0\camera-agent\reports',
       'phase0\camera-agent\transactions',
       'phase0\camera-agent\artifacts',
       'hardware-single',
       'dual-camera-hardware-products\agent-pair-journal',
       'dual-camera-hardware-products\recovery-state',
       'dual-camera-hardware-products\transactions',
       'dual-camera-hardware-products\capture-recovery-only-evidence',
       'dual-camera-hardware-products\operator-review'
   )
   Copy-A0Diagnostics -SourceRoot (Join-Path $env:LOCALAPPDATA 'A0CameraStitcher') -Targets $targets -Destination (Join-Path $out 'localappdata')
   ```

   robocopy は終了コード 0〜7 が成功です。8 以上なら、その対象だけ回収できていません。許可リストの 4 種類で調べるのに足りるかは ※要確認（足りないときに何を足すかは、開発担当と所有者が決める）。

4. Phase 0 CLI を使っていたら、同じウィンドウで、その作業フォルダの証跡も同じ関数で写します。`artifacts\phase0` には原画像が入りますが、許可リストに当たらないので写りません。

   ```powershell
   Copy-A0Diagnostics -SourceRoot '<Phase 0 CLI の作業フォルダ>' -Targets @('docs\evidence\phase0', 'artifacts\phase0') -Destination (Join-Path $out 'phase0-workdir')
   ```

5. 回収先の中身を拡張子ごとに数え、想定外のファイルが無いことを確かめます。

   ```powershell
   Get-ChildItem -LiteralPath $out -Recurse -File | Group-Object Extension -NoElement | Sort-Object Name
   $bad = @(Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object {
       $_.Extension -notin '.json', '.log', '.txt', '.md' -or $_.Name -like 'camera-map*.json' -or $_.Name -eq 'single-identity-v3.json' })
   "unexpected files: $($bad.Count)"
   ```

   一覧に出る拡張子が `.json`・`.log`・`.txt`・`.md` だけで、`unexpected files: 0` なら次へ進みます。1 以上なら回収物を渡さず、所有者に知らせます。

6. どのビルドで起きたかを控えます。2.2 の 7 で記録した commit と、導入先の実行ファイルの SHA-256 を照らします。出力にパスを含めないよう、ファイル名だけにします。

   ```powershell
   Get-ChildItem '<導入先>\app\*.exe' | Get-FileHash -Algorithm SHA256 |
       Select-Object Algorithm, Hash, @{ n = 'Name'; e = { Split-Path $_.Path -Leaf } }
   ```

7. 回収先をまとめて、所有者が決めた保管先で開発担当に渡します。メールへの添付やチャットへの貼り付けはしません。

   ```powershell
   Compress-Archive -Path "$out\*" -DestinationPath "$out.zip"
   ```

8. 公開の Issue に書いてよいのは次だけです。ファイルそのものは貼りません。
   - 状態の名前（`FailedPartial` など）、エラーコード（`sdk_load_failed` など）、件数、終了コード
   - `%LOCALAPPDATA%` の形に置き換えたパス
   - 実行ファイル（`.exe`）の SHA-256

   画像の SHA-256 は貼りません。コマンドの出力を貼るときは、先にユーザー名や PC 名を含む部分を置き換えます。次は置き換えの例です。置き換えた後の文字列にユーザー名や PC 名が残っていないことを目で確かめてから貼ります。

   ```powershell
   $text = Get-Clipboard -Raw   # 貼りたい出力をコピーしておく
   $text = $text.Replace($env:LOCALAPPDATA, '%LOCALAPPDATA%').Replace($env:USERPROFILE, '%USERPROFILE%')
   $text = $text.Replace($env:USERNAME, '<user>').Replace($env:COMPUTERNAME, '操作 PC-1')
   $text | Set-Clipboard
   ```

### 6.3 よく出る症状と対処

| 症状・メッセージ | 原因 | 対処 |
| --- | --- | --- |
| `sdk_load_failed`（`Nikon D810 SDK runtime module is unavailable, untrusted, or incomplete`） | `NIKON_D810_SDK_MODULE_PATH` が無い・誤り、DLL の不足、ネットワーク上のパス、junction | 3.2 と 3.3 を見直す。WPF を起動したウィンドウに環境変数が入っているかも確かめる |
| 「SDK非同梱のソフトウェア確認用候補。カメラ未照会」 | SDK 抜きの候補を置いた | 2.2 の実機用の候補に置き換える |
| 「実行ファイル未検出（SDK・実機接続は未確認）」 | Agent が `app` に無い | 2.2 の 6 の一覧と照らす |
| 「A0 Camera Stitcher — 実機操作は既に使用中」（終了コード 3） | 同じ Windows セッションで別の実機画面が開いている | 先に開いている実機画面を閉じ、Agent が終わってから開く |
| 「Camera Agentの保存先を安全に確認できないため、起動せずに停止しました。」 | `%LOCALAPPDATA%` の途中に junction がある、固定ローカルドライブでない | プロファイルを移動・リダイレクトしていないかを確かめる。直し方は ※要確認 |
| `camera_control_delegation_quarantined` | 委譲 marker が残っている | marker を手で消さず、所有者に知らせる（`docs/MACHINE_OPERATION.md`） |
| `camera_control_marker_failed` | marker の置き場所に junction がある、作れない | 同上 |
| 「A0 Camera Stitcher — 起動引数エラー」 | 起動引数の組み合わせが足りない（例: `--hardware-dual` に `--wpd-camera-map` が無い） | メッセージに出た引数を README の起動例と照らす |
| `Phase0CliPreflight: BLOCKED` | 環境変数 `NIKON_D810_SDK_ROOT` が無いか存在しないパス、または SDK の module を読めない | `preflight` は SDK root と module の両方を見る。両方の環境変数を設定してから流す |
| CMake の `NIKON_D810_SDK_ROOT is incomplete or ambiguous; building the gated stub` | ビルド時に SDK のファイルが見つからない・重複している | 2.2 の 2 を見直してビルドし直す |

## 関連文書

- [README](../README.md): 開発の手順、Phase 0 CLI、M3 application shell の起動例
- `docs/OPERATOR_GUIDE.md`（#298）: 撮影の操作
- [正規機械操作の入口](MACHINE_OPERATION.md): カメラ制御入口の運用前提、委譲 marker
- [Phase 0 readiness](PHASE0_READINESS.md): 開発者向けの readiness スクリプト
- [Hardware Camera Agent v1](HARDWARE_CAMERA_AGENT_V1.md): 一台の Agent の保存物
