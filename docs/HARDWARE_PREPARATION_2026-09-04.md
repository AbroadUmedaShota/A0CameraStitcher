# 実機検証前のソフトウェア準備（2026-09-04）

## この文書の範囲

本書は手順と判定条件であり、カメラ操作の許可ではない。今回の実行はローカルsoftware-onlyまで。
push/PR/merge、hosted CI、SDK実行、接続台数照会、USB操作、撮影、配布は行わない。
リポジトリ公開化は別途承認された独立審査の対象。審査がBLOCKのため実行せず、privateのまま準備する。

- base: `2553c118e48cb82686677b962703322fa4f203dd`（PR #173統合済みmain）
- local branch: `fix/151-dual-export-publication`
- 対象: #151-M4のアプリ側export公開境界、既存toolchainの確認、fake Agent検証、実機開始手順。
- 原本・journal・他worktreeのdirtyは保持する。旧root mainでビルド・試験しない。
- ローカル実装commit: `474f522a4104dec2dab16bf2f388b791546ab18a`。
  Releaseの初回試験はcommit直前の同一source差分で実行した。実機対象SHA/SDK-enabled buildの認定ではない。

## M4-Aの契約

nativeのpartial書込、JPEG/size/hash/byte検証、locked non-replacing renameは変更しない。
アプリはexport job専用の `.a0-export-<job-id>.partial` directoryにnativeのJPEGを出力し、
元stitchをread lockしたまま候補をwrite lockし、bounded-bufferのbyte照合後に同じhandleを最終名へrenameする。
nativeが要求する `.jpg` extensionも維持する。既存Single publisherの実装をFoundationへ移し共用する。

- 不一致、adapter書込後例外、commit前cancel、既存finalとの衝突は失敗。final output pathを成功扱いしない。
- 元画像、stitched、既存final、失敗候補を削除しない。診断候補は作業directoryに残す。
- renameがcommit point。以後のcancelで公開済み結果を未公開失敗へ戻さない。
- 自動retryはしない。操作者による次回の明示exportは別job。
- 成功時にだけ空の作業directoryを除去する。競合差替えなどの診断ファイルがあればdirectoryごと保持する。

## ENV-A: SDKなしローカル検証

既存Visual Studio Build Tools 2022に同梱CMake 3.31.6-msvc6、MSVC 19.44.35228.0、
Windows SDK 10.0.26100.0、.NET 10.0.303を確認した。CMakeが通常PATHに無いことは、未インストールを意味しなかった。
プロセス内PATHのみ設定し、新規install、ライセンス受諾、machine-wide変更はしない。

隔離worktreeでのコマンド例（カメラ入口は呼ばない）:

```powershell
$env:PATH = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;' + $env:PATH
cmake -S . -B build/wpf-m2-adapter -A x64 -DNIKON_D810_SDK_ROOT=
cmake --build build/wpf-m2-adapter --config Release -- /m:1
ctest --test-dir build/wpf-m2-adapter -C Release --output-on-failure
pwsh -NoProfile -File scripts/Test-M3Simulated.ps1 -Configuration Release
pwsh -NoProfile -File scripts/Test-DualCameraWpfFlow.ps1 -Configuration Release
```

Debugも別configurationで同じ安全条件により確認する。共有native build directoryへ同時にbuildを走らせない。
`NIKON_D810_SDK_ROOT:PATH=`が空のfresh cacheであること、`A0_NIKON_SDK_AVAILABLE` compile definitionが未定義であることを確認する。
コードは `defined(...)` を見るため、値0で定義する方法は使わない。
SDKlessであっても任意のWPD commandは安全とは限らない。実行対象はfake/recording transportを使う既存testに限定する。
M2 adapterのsynthetic/validate/stitch/exportはローカル生成fixtureだけを使う。

成果物識別ではsource commit、差分、configuration、compiler、生成日時、SHA-256、byte数、DOS MZ/PE signatureを記録する。
zero-byteや未来timestampを用いてincremental buildを回避しない。SDKless成果物を実機用SDK-enabled binaryと取り違えない。

### ローカル検証証跡

| 検証 | Release | Debug |
| --- | --- | --- |
| SDKless native全target build | 成功 | 成功 |
| native CTest | 20/20、exit 0 | 19/20、exit 8（pipe test失敗） |
| `Test-M3Simulated.ps1` | exit 0（Foundation 37/37、Dual 21/21、OperatorShell 88/88） | exit 0（同件数が全件成功） |
| `Test-DualCameraWpfFlow.ps1` | exit 0（同梱Agentのhash一致とJPEG E2Eを含む） | exit 1（OperatorShell 86/88、模擬撮影待ち2件timeout） |

native fresh buildでは既存C4819等の警告がある。警告なしのbuildとは扱わない。
初回追加負例は修正前のexport path公開で失敗した。さらにbyte比較を一時的に無効化したmutationは
Dual 20/21で失敗し、同一長不一致の検出感度を確認した。mutationは復元済みで、上記Dual全件PASSは復元後の実装。
独立reviewは1回修正後PASS。flush後・rename前のcancelと、同一長／最終chunk不一致の負例を補強した。
このPASSはM4-Aのコード差分に限定し、公開・実機・SDK利用・mergeの承認ではない。

Release成果物（初回build、2026-09-04、ローカル確認用）のSHA-256:

| 成果物 | SHA-256 |
| --- | --- |
| M2Adapter.exe | `3BBEFFCE00A1FD210BA0836A145019B594CC1B0CA70F550FFF7320FAB6B2E5F8` |
| CameraAgent.exe | `965D3D46EE4A269C08D513C8418127C59F877E84CA4773B4D80E0D0A3907E29E` |
| DualCameraAgent.exe | `C17C0F6533BF54CF4474F93955F4BA67EA2C877D37EC9624CDC0F8040DB5A7DF` |
| Foundation.dll | `BA1E034E807F43812F64317A5B6F0A5F1317E1D619FA4F56D4E8F183C0475943` |
| OperatorShell.dll | `ADEDBA0304233C140ABBFD3E12F1B7324A1DB69130EDA4B75FDB2BC479D511C6` |

nativeのsourceはbaseから変更なし。3 exeはいずれもMZ/PEを確認し、byte数は順に205312／538112／578560。
バイナリはcommitしない。公開/実機候補は別途exact SHAで再buildし、ここに記載したSDKless buildを流用しない。

Debug成果物の識別（2026-09-04）:

| 成果物 | byte数 | SHA-256 |
| --- | ---: | --- |
| M2Adapter.exe | 1131520 | `390FD50E4B88451BA2FEFB7291E56FE760BAC09FC91AFBE9A4B5C43E498BACCC` |
| CameraAgent.exe | 3313152 | `8CE103B9B554F72181135AAB8EBBBDA5D10BF5516036CCE04399F1FADF6396CD` |
| DualCameraAgent.exe | 3390464 | `752ED1375E155D1684688F0BA1BEDB69D68EFBDA3C3BA593C6937074F18D9D7D` |
| Foundation.dll | 547840 | `BA28C8099EFBC86ED51D29FDAF43D2A169081C77BEE4D672E7CDF232819CEE45` |
| OperatorShell.dll | 15245312 | `C8BD703073D55DC10B4428B9756AFEEE745AD4A600E5599E61E9A4CDB0769087` |

DebugのFoundation／OperatorShellのAssemblyInformationalVersionは
`1.0.0+474f522a4104dec2dab16bf2f388b791546ab18a`。文書以外のsourceは当該commitと一致する。

## TEST-A: 既存ローカル失敗の扱い

以前の専用worktreeではDualCameraFlowTests 17/19、OperatorShellTests 80/88だった。
native adapterのzero-byte/未来timestamp、環境変数未設定と、fake Agent/pipe失敗6件は別事象。
前者の不正成果物は旧worktree整理時に除去済みで、今回のclean buildには使用しない。
後者は再現条件・exit・pipe・timingを確認し、原因を特定せず環境要因/flakyと断定しない。
timeoutの単純延長、skip、成功するまでの反復でPASSにしない。

今回のfresh worktreeと実体のあるSDKless native成果物では、Releaseの
`Test-M3Simulated.ps1` がexit 0となり、Foundation全件、Dual 21/21、OperatorShell 88/88が成功した。Debugも同じ全件が成功した。
過去のfake Agent失敗6件はこの実行では再現しなかった。根本原因を特定した／flakyを修正したとは扱わず、
timeoutやfake Agent実装に変更を加えていないこと、旧成果物との条件差を記録する。

一方、今回のDebug native CTestで新たに `dual_hardware_camera_agent_pipe_contracts` が
`FAIL: the close tombstone query must be delivered` となった。該当native source/testはbaseから差分0で、
M4-AのC#実装をリンクしない独立nativeテスト。Releaseでは同項目が通過した。
`tests/dual_hardware_camera_agent_pipe_tests.cpp` のpersistentテストは固定4500msのhost lifetime内に
capabilities／reserve／duplicate／query／close／queryを送る構成であり、期限不足と整合するが実測による原因確定は未実施。
タイムアウト延長・skip・成功するまでの再実行はしていない。Debug native全体は未合格として実機前の未解決条件に残す。
実行コマンドは `ctest --test-dir build/wpf-m2-adapter -C Debug --output-on-failure`、exit 8、全体317.35秒。
ログはローカル `build/wpf-m2-adapter/Testing/Temporary/LastTest.log` と `LastTestsFailed.log`。
失敗したDebug test exeのSHA-256は `2DB623C0C616631CEA9C907CED3B3E1E114AF8138AB36C7BD8F0E25420FAF499`。
ログは次のCTestで上書きされ得るため、再検証前に保全する。新規CIを起動して原因調査の代替にしない。

さらに `Test-DualCameraWpfFlow.ps1 -Configuration Debug` はOperatorShell 86/88で失敗した。
失敗名は `formal WPF dual-camera flow uses real JPEG product artifacts` と
`formal WPF maps active and failed explicit export progress`。
どちらもexport実行前の模擬capture完了待ちで `TimeoutException` となった。
同一sourceの直前のM3 Debug（adapter環境変数あり）は88/88だったが、専用WPF（環境変数なし・同梱adapter）では失敗している。
`WaitUntilAsync` の既定5秒と処理時間の関係が調査候補であり、実測原因は未確定。
この専用検証をM3の成功で置き換えず、再実行・skip・時間延長は行っていない。

以上より、M4-Aの局所修正と独立reviewはPASSだが、今回のsoftware-only全体結果は未合格。
Single／Dualいずれも実機開始Readyではない。次の調査はnative persistent hostの通信時間と、
WPF Debugのadapter選択／capture完了時間を測定して差を説明すること。製品仕様やhost寿命の変更は自動で行わない。

## 公開前審査の結果（ローカル準備とは別判定）

対象リポジトリのPRIVATE→PUBLIC承認は確認済み。ただし独立審査はBLOCKで、公開変更は未実施。
mainと履歴に加え、main外PR 30 head（追加53 text blob／70 commit）を限定パターン検査し、
検査対象の秘密情報パターン検出は0だった。完全な秘密情報不存在・権利処理の保証ではない。

- Actions全164 run: ログ検査済み53、取得不可53、未着手58。過去attempt 34枠も未検査。
- 取得不可を失効・非公開・安全確認済みとは扱わない。残りの取得可能ログとattemptの審査が必要。
- 履歴の個人／会社連絡先と、Issueの非汎用端末アカウント名は、第三者情報か／公開意図に含むか未確定。
  値はこの文書に転記しない。履歴改変・投稿編集は行っていない。
- 公開直前にrefs、Actions、artifact、保護設定の増分確認が必要。SDK・アプリの配布承認には拡張しない。

公開審査BLOCKはローカルsoftware-only準備の停止理由ではなく、公開実行のみを停止する。

## 共通の実機開始ゲート（すべて必要）

- [ ] target PC、担当操作者、実施日時、mode、範囲が指定され、個別実機操作が許可されている。
- [ ] exact commitとclean source、build hash、SDK-enabled構成が一致する。今回のSDKless buildの流用は禁止。
- [ ] SDK利用/配置/ライセンスがtarget PCで確認され、SDK-enabled zero-camera回帰が合格している。
- [ ] focused/full software回帰と独立reviewが合格し、重大な安全課題が解決または承認された限定条件で閉じている。
- [ ] accepted exact-SHA CI gateを満たす。前回PR #173の承認/CIを新候補へ流用しない。
- [ ] カメラ、レンズ、電源、USB、専用空spool、fixed-local保存先と空き容量が承認済み条件を満たす。
- [ ] 他cameraアプリと物理シャッターを操作しない排他同意。operator-session leaseが有効。
- [ ] pending journalは同一IDで確認し、状態不明のまま新規撮影をしない。journalや画像を消して解除しない。
- [ ] 原本とjournalの保管先、backup担当、匿名証拠の保存先が決まっている。

接続台数、実SDKの状態、空spool、target PCの容量は今回照会していない。チェック済みにはしない。

## SingleCamera: 最初の実機検証単位

必要機材は物理D810一台だけ（CAM-A）、登録済みWPD digest identity-v3、承認済みread-only profile、専用空spool。
他D810をつないだまま一台だけを使う運用は禁止。

1. 共通gateとread-only exact-one/identity/profile/settingsを承認された操作で確認する。
2. 実WPFからLive View開始・frame・明示停止・SDK closeを確認する。
3. 改めて撮影を許可し、最初はone-shot一件だけ。SDK exactly-one capture→WPD exact-one recovery→
   PC partial/JPEG/size/hash/atomic原本/reread→exact-object cleanup→empty-afterを確認する。
4. `StitchOutcome=NotApplicable`、7360×4912 canonical original、review→fixed-local byte-identical export、結果表示を確認する。
5. 成功と終了を確認して停止し、handoff10回/WPF100件/物理異常系は次の個別許可単位へ残す。

既存Camera Agent経路1/10/100とp95承認は履歴として有効だが、このWPF受入の代替ではない。
予約枠の提案は準備/説明/一件/証拠確認に45～60分（未承認の調整用目安）。撮影性能の保証値ではない。

## DualCamera: 最初の実機検証単位

必要機材はD810二台、CAM-A/B用WPD map、各専用空spool、外部承認CaptureRecoveryOnly profile、
目視割当を行う操作者。実リグ品質profileの偽装や通常合成経路への切替はしない。

1. 共通gateを満たしたexact binaryで、許可されたread-only coexistence probeを一回だけ行う。
   exact-two、CAM-A/B exact-one、両payload0、全WPD close、source/capture-session close、
   Module retained、WPD open中SDK operation0を匿名記録する。probeに撮影/deleteは含めない。
2. probe成功後も自動で撮影しない。操作者のone-shot許可後に、同一Agent-sessionで二候補を一台ずつLive View表示し、
   CAM-A/Bをexactly once割当、全Live View停止/source close、current Ready bindingを確認する。
3. `CaptureRecoveryOnly`一組だけ、pair preflight→CAM-A→CAM-Bを実行する。各原本7360×4912/size/hash/reread、
   exact cleanup/empty-afterを確認し、両PC原本を保持する。stitch Pending、A0品質Unapprovedを表示する。
4. 同一transactionの状態を確認し、実行を終了する。10/100 runner、600秒lifetime変更、通常capture-and-stitch、
   合成JPEG exportを追加しない。

予約枠の提案はprobe60分、別gateでbinding/one-shot/証拠確認60～90分（未承認の調整用目安）。
USBや電源の操作、撮影設定変更、異常注入はこの一組の許可に含めない。

## 即時中止と保全

カメラ台数/identity不一致、割当不明、profile不備/期限切れ、spool非empty、SDK error、close未確認、
180秒期限超過、複数/遅延/曖昧JPEG、保存/検証/cleanup失敗で停止する。A失敗時Bは開始せず、B失敗時はA原本を保持する。
WPD cleanup不明時はSDK API（End含む）を呼ばずAgentを隔離・terminal化し、再bindingを要求する。
原本/partial/journalは残す。他alias探索、未検証object削除、retry、bulk delete、format、vendor 0x9207は禁止。

共有証拠はmode、匿名alias、commit/build、時刻、件数、size/hash、状態、error category、close/cleanup結果。
実serial、raw identity、candidate ordinal/source object、preview、顧客画像、SDK配布物はcommit/外部添付しない。
原本そのものは承認されたローカル保管先だけに残す。

## 実機前に判定を残す安全項目

- #141: Issue本文の旧10秒予算は現行と異なる。現行は `live_view_frame=3秒` の独立予算と上限検証、
  budget isolation回帰を実装済み。SDK同期frame呼出しを瞬時中断できる保証ではないため、stop/SDK close確認を省略しない。
  実機応答性は別の受入条件であり、open Issueを理由に同じ修正を作り直さない。
- #150: native capture profileの最小setting coverage、profile消失時のSDK open前gate、診断の匿名化と成果物鮮度。
  外部profileが「承認済み」という表示だけで撮影を許可しない。
- #151残件: SDK callback前提、noexcept/例外境界、WPD列挙/cleanupの未監査項目。
  M4-AやSDKless CIの合格をこれらの実SDK安全性の証明にしない。

重大なBLOCKは当該実機gateを閉じる。画質/rig、使用PC、配布条件は推測で決定しない。
今回の完了範囲はlocal準備と個別承認事項の整理であり、実機開始許可ではない。

## 関連する正本

- [製品要件](PRODUCT_REQUIREMENTS.md)
- [Phase 0実機試験計画](PHASE0_TEST_PLAN.md)
- [Single実機実績](SINGLE_CAMERA_HARDWARE_RESULTS_2026-08-26.md)
- [Dual安全監査と限定例外](DUAL_HARDWARE_SAFETY_AUDIT_2026-08-31.md)
- [CI費用・一回承認の範囲](CI_COST_PROFILE.md)
