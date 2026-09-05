# Debug検証の原因切り分けと公開前監査（2026-09-06）

## 対象と実測

起点は `573930695b77d53cacf1bfbdcf337662a37a6e73`（M4-A実装は `474f522a4104dec2dab16bf2f388b791546ab18a`）。
専用branch `fix/151-dual-export-publication` を継続使用する。remote mainは `2553c118e48cb82686677b962703322fa4f203dd`。
SDKless、既存Visual Studio 2022／CMake、.NET 10によるローカル試験に限定した。
限定修正commit: `483f9a4712e19b3aafe4274b418b72bd5c8d1c32`。

9月4日のDebugはnative 19/20、専用WPF 86/88。9月6日の通常条件ではnativeの6通信が
28／34／15／15／64／9 ms、WPFの模擬captureは2415／2500 msで完了した。
今回の成功だけでは過去の失敗を解消済みと判断できないため、次の制御された遅延をテストにだけ注入した。

| 実験 | 旧実装の結果 | 確認できた機構 |
| --- | --- | --- |
| native: close応答後に4600 ms待ってquery | 成功 | 既に待機中のacceptは最大5000 ms待つため、4500 ms直後に必ず接続が消える契約ではない |
| native: close応答後に5600 ms待ってquery | exit 1、`the close tombstone query must be delivered` | 固定寿命を過ぎてhostが終了すると、正しいtombstoneがあっても通信テストは失敗する |
| WPF: fake captureだけ6秒遅延 | 旧5秒待機はTimeoutException、処理自体は6777 msで正常完了 | 機能テストのUI pollingに、製品仕様にない一律5秒完了条件が混在している |

9月4日のOSスケジューリングやI/O待ちの実測は無いため、当日の遅延要因までは特定していない。
一方、上記の失敗を発生させるテスト設計上の条件は直接再現した。成功までの再試行、skip、製品timeoutの拡大はしていない。
診断用の環境変数・出力は最終製品コードに残さない。

前回の「M3と専用WPFのadapter環境変数の差が原因候補」という説明を訂正する。
`FormalDualCameraWpfFlowAsync` 自体が環境変数を消去して同梱adapterを選ぶため、同テストの原因説明には使えない。

## 限定修正

- native: 既存のtest-only failure injectionにhost寿命時計を追加する。複数通信・永続化の機能検証中は時計を制御し、
  activity後も起動時の絶対期限で終了することを時計の前進で検証する。
  実時間5600 msの遅延を回帰として保持し、テスト内の4.5秒と実I/O処理時間を結び付けない。
  製品では注入なしの `GetTickCount64`、600秒寿命、frame/ACKの実時間制限、accept粒度を維持する。
- WPF: `AsyncRelayCommand` の既存guard・例外通知・busy解除を含む処理をinternalのawait可能な入口へ抽出する。
  公開ICommand入口は同じ処理を呼ぶ。native連携機能テストはcommand完了を待ってから元の状態／JPEG／原本保持をassertする。
  30秒はテストハング検知の上限であり、性能目標や実機撮影の許容時間ではない。通常UI待機の5秒は変更しない。
  実JPEGのfake captureへ5200 msの遅延を残し、重複実行拒否、例外通知、busy解除も回帰で検証する。

## 検証結果

focused native（5600 ms遅延を含む）: 修正後exit 0。
Debug M3はFoundation 37/37、Dual 21/21、OperatorShell 89/89でexit 0。
専用WPF Debugもexit 0（同梱Single/Dual Agent hash一致、JPEG capture/stitch/restitch/export E2E）。
OperatorShellはcommand完了の回帰を1件追加したため88から89件になった。
これらはcommit直前の同一実行sourceで検証し、commit前の最後の差分はnative testの説明コメントのみ。
Releaseは `483f9a4` のsourceでbuildし、native CTestは単一実行20/20、exit 0（499.82秒）。
Release M3もFoundation 37/37、Dual 21/21、OperatorShell 89/89でexit 0。
専用WPF Releaseもexit 0。同梱Single/Dual Agent hash一致と実JPEG E2Eが成功し、
Releaseの最終exitはnative/M3/WPFすべて0となった。
Release native全体ログは `build/diagnostics-2026-09-06/release-all-LastTest.log` に保全した。
native buildには既存C4819警告があり、警告なしとは扱わない。SDK compile definitionは未定義である。
9月4日のCTestログは `build/diagnostics-2026-09-06/previous-debug-LastTest.log` へ保全した。

Debug全体の初回コマンドでは、今回だけ付けた `--timeout 180` により、変更していない
`m2_stitch_metric_contracts` が180.27秒で打ち切られた（19/20、exit 8）。対象のDual pipeは成功。
同metricは9月4日に169.06秒かかり、既存CMakeには180秒制限がない。
内部でschema fixtureごとにPowerShellの `Test-Json` を起動する構成を確認した。
コマンド側の追加上限は既存契約ではなく、これを付けない標準条件で同testを一回確認し、195.38秒で成功した（exit 0）。
この初回失敗を削除したり、製品の許容時間を変えたりしない。
Debugの全20項目に成功証拠が揃ったが、単一実行の20/20とは記載しない。
初回ログは `build/diagnostics-2026-09-06/debug-command-limit-LastTest.log`、
標準metricは `build/diagnostics-2026-09-06/debug-standard-metric-LastTest.log` に保全した。

独立reviewerはexact commit `483f9a4712e19b3aafe4274b418b72bd5c8d1c32` の5ファイルを読み取り専用で審査し、
重大・中程度の指摘なし、実装の判定は **PASS**。
test clockの注入範囲、絶対期限境界、Single/Bindingの実時計維持、WPFのguard/例外/busy解除を確認した。
その後、上記Release全体も完了し、reviewerが同一SHAの結果とsource差分0を再確認した。
Missing validation解消、追加の必須ソフト検証なし、最終 **PASS**。
この実装PASSを公開、実機、SDK、push/mergeの承認に置き換えない。

### ローカルRelease成果物の識別

以下はSDKless・実機利用不可のローカル検証用。5件とも非zeroかつMZ/PE signatureを確認した。
Foundation/OperatorShellのProductVersionは `1.0.0+483f9a4712e19b3aafe4274b418b72bd5c8d1c32`。
バイナリはcommitしない。9月4日の古いhashと混同しない。

| 成果物 | bytes | SHA-256 |
| --- | ---: | --- |
| M2Adapter.exe | 205312 | `3BBEFFCE00A1FD210BA0836A145019B594CC1B0CA70F550FFF7320FAB6B2E5F8` |
| CameraAgent.exe | 538112 | `AC706271DDFEDA67B61B180F95C39B3B5E660D8F143D9897A55951E0060FF5BA` |
| DualCameraAgent.exe | 579072 | `0D3E8F4FF94004DAE2CAC7C3A125C08F455D1B946A9D9BFFA8165018A80F642B` |
| Foundation.dll | 517120 | `3FE892B64A39D06AFD103C5BDC2992DBEC1967C1C578FA59A1943AE99C5F3077` |
| OperatorShell.dll | 15171072 | `13E9CD528D07787D5475AF3E94B6EF28E389A24933470D9BBD5E87D01DD50AC7` |

## 実機前の判断

この修正はsoftware-onlyの検証信頼性に関するもの。Single／Dualの実機開始可否は引き続き個別に判断する。
SDK-enabled成果物・対象PC・操作者・個別操作承認、未解決の安全課題、承認されたexact-SHA CIは別の条件として残る。
実機操作、SDK実行、push／PR／merge、hosted CI、配布は今回実施しない。

| 実機モード | 今回準備した範囲 | 次の開始条件 |
| --- | --- | --- |
| Single | SDKlessソフト検証、1台WPF Live View→明示許可後one-shotの手順 | 対象PC/操作者、SDK-enabled exact build、安全項目/CIの確認、個別操作許可 |
| Dual | SDKlessソフト検証、read-only probe→別許可でbinding/一組CaptureRecoveryOnlyの手順 | 共通条件に加えCAM-A/B map、専用spool、限定profile、#150/#151の実機関連安全項目の判定 |

SingleもDualも、ソフト試験成功だけで実機開始Readyとは扱わない。
具体的な停止・原本保全条件は [実機準備手順](HARDWARE_PREPARATION_2026-09-04.md) に従う。

## 公開前監査の更新

9月6日にlogs APIからZIP本文を取得し、`ZipArchive`で全entryを開いて検査した。
認証はGitHub APIリクエストだけに付け、storageへのredirectへ転送しない。
本文・一致した値は保存せず、run ID/attempt/HTTP/entry数/分類件数だけをローカルに記録した。

| 対象 | 本文全entry検査 | 実際に空だったZIP | 取得エラー／上限超過／未検査 |
| --- | ---: | ---: | ---: |
| current run 164枠 | 109 | 55 | 0 |
| 過去attempt 34枠 | 4 | 30 | 0 |
| 合計198枠（ID+attempt重複0） | 113 | 85 | 0 |

全198枠はHTTP 200。空ZIPは22 bytes・entry 0を本文から確認した。
取得後のlive run一覧164枠ともID/attemptが全件一致し、artifact 0・release 0も再確認した。
`gh run view --log` の「not found」をHTTP 404／権限不足／失効とは解釈しない。
chunked transferやContent-Length不明も今回本文まで読んだため、ヘッダだけの推定から更新できた。
初回の並列監査manifestは処理不備で198 process-errorとなり無効。停止後、親担当が逐次取得で検査した。
その無効manifestや先のヘッダ分類を成功証拠には使用しない。

限定パターン（GitHub/AWS token、private key、secret等の代入、email）の候補は0。
Windows user path候補1件はcurrent run `33562505905` attempt 3の汎用アカウント名だった。
これは任意の秘密情報、一般氏名・電話・住所、自由文の機密性や権利処理を包括的に保証する監査ではない。
空ZIPについても過去に存在した本文の安全性を保証しない。

main/PR履歴、Issue/コメント等の9月4日監査は前文書の範囲を引き継ぐ。
今回のログ範囲で本文未検査は解消したが、公開直前のrefs/Actions/artifact/保護設定の差分確認は別に必要。
通常のowner metadataや `license=null` だけを追加の一律ヒューマンゲートにはしない。
ライセンスがないsourceの再配布条件は、visibility変更とは別の論点として残す。

具体的なオーナー判断は **Issue #101の第三者投稿にある非汎用Windowsユーザー名を含むpath** の扱い。
投稿者はownerではないcollaboratorで、当該値の公開意図は確認できていない。値は転記しない。
選択肢は (1) 当該情報を含めた公開を明示判断する、または
(2) privateを維持し、投稿者への確認／匿名化案を別途承認してから再審査する。
第三者投稿編集・履歴改変・visibility変更は今回行わない。リポジトリはPRIVATEのまま。
リポジトリ公開の既承認と、SDK／アプリの配布承認を混同しない。

公開するとActions履歴・ログも公開されるため、コードだけを審査対象にしなかった。
影響の根拠は [GitHub公式のvisibility変更説明](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/managing-repository-settings/setting-repository-visibility)。
