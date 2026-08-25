# 2026-08-26 水曜テスト準備計画

> 開発に詳しくない方向けの概要は、[水曜テスト かんたん共有版](WEDNESDAY_TEST_OVERVIEW_2026-08-26.md)をご覧ください。このページは開発者向けの詳細手順です。

承認日: 2026-08-24

計画マイルストーン: `M3T`

対象: Windows 11 x64 / Nikon D810 exactly-one `SingleCamera`

判定境界: 内部テスト準備であり、MVP、Dual実機、A0品質、配布、リリースの完了判定ではない

## 1. 水曜の到達点

水曜の必須到達点は、D810一台を使った次の安全な操作経路である。

1. WPFアプリを候補SHAから起動する。
2. `SingleCamera`を明示選択し、D810 exactly-oneのidentity-v3 read-only preflightを通す。
3. Live Viewを開始・停止し、停止とSDK session closeを確認する。
4. 専用spoolのpayloadがJPEG、動画、generic fileを含め0件であることを確認する。
5. 操作者の明示再開後、no-retryのone-shotを最大1回実行する。
6. PC上で`.partial`から有効な`7360x4912` JPEG、SHA-256、atomic canonical original、rereadを確認する。
7. 上記確定後だけ今回回収したWPD objectをexact deleteし、empty-afterを確認する。
8. reviewと明示exportを確認し、アプリ終了後にCamera Agent、SDK session、leaseが残らないことを確認する。

合成画面は、権利確認済みのサンプル画像によるsoftware-only確認に限定する。実D810二台撮影、実A0品質評価、actual shutter synchronizationは水曜の必須範囲に含めない。

## 2. 体制と所有範囲

最低推奨はフロント1名、その他2名、機材操作者1名である。その他1名しか確保できない場合は、合成画面の確認を外してSingleCamera one-shotへさらに縮小する。

| Lane | 主担当 | 所有範囲 | 完了条件 |
| --- | --- | --- | --- |
| F: Front / WPF | フロントエンジニア1名 | SingleCameraの接続、readiness、Live View、撮影、review、export、失敗、終了の表示と操作。原則XAMLとUI testを所有し、PR #128統合前にshutdown code-behindを並行編集しない | 未完成Dual経路が`HardwarePending`で無効、連打やmode fallbackがなく、全状態がtruthfulに表示される |
| C: Camera / Native | その他メンバーA | WPD evidence integrity、identity-v3 read-only route、SDK/WPD session境界、Issue #131 | `read > requested chunk`をfail closedし、対象試験とSDK-less/SDK-enabled契約試験が通る |
| I: App / Integration | その他メンバーB | Issue #94 / PR #128、clean worktree統合、candidate SHA、CI、実行ファイルhash、既知問題 | shutdown完走、orphan Agentなし、候補SHAのソフトウェア検証結果とmanifestが確定する |
| Q: QA / Evidence | I担当が兼務可 | テスト手順、停止条件、schema parse、匿名化scan、結果記録 | Go/No-Go項目が埋まり、失敗を成功へ昇格しない |
| O: Operator / Product owner | 機材担当 | D810一台、専用empty card、USB、固定チャート、保存先、SDK許諾、実機操作の明示再開 | 実機開始条件を確認し、camera commandは操作者の合図後だけ実行される |

同じファイルまたは同じ不具合を複数Laneで並行編集しない。統合担当が候補SHAと最終差分の唯一の所有者になる。

## 3. GitHubのクリティカルパス

| 優先 | Issue / PR | 水曜判定 | 対応 |
| --- | --- | --- | --- |
| 必須 | #131 WPD `IStream::Read`返却長 | 未完了なら実撮影No-Go | Camera担当が最優先で修正・negative test追加 |
| 必須 | #94 / PR #128 shutdown順序 | CI未検証のまま実撮影No-Go | Billing復旧後にexact PR SHAのCIを再実行し、green後に統合 |
| 必須 | #7 SingleCamera identity-v3 / one-shot | 実機実行契約 | read-only preflight、empty spool、operator resume後のone-shotをここへ記録 |
| 条件付き | #130 Dual UI無期限wait | 未完了ならDualを無効化 | 水曜SingleCameraには入れず、Dual software画面を操作不可にする |
| 対象外 | #85 DualBinding同一ユーザーsquatting | 水曜Dual不可の理由 | 実Dual hostとセットで後続対応 |
| 対象外 | #10 Dual実capture backend | 水曜Dual不可の理由 | #7 one-shot合格後の別lane |

新規実装Issueは作らない。既存Issueを実行契約として使い、UIとcandidate assemblyの未昇格候補は`.autodev/backlog/generated.json`へ保持する。

## 4. 日程

### 8月24日 月曜

- テスト対象をSingleCamera exactly-oneへ凍結する。
- 最新`origin/main`からclean worktreeと担当別branchを作る。
- GitHub Actionsのbilling / spending limitを復旧する。
- #131とPR #128の差分、試験、競合範囲を確認する。
- D810一台、専用empty card、USB、保存先、権利確認済みチャートの準備状況を確認する。

Exit gate: 担当、branch、機材準備、未解決blockerが一覧化されている。

### 8月25日 火曜 午前

- #131をfocused test付きで統合する。
- PR #128をexact-SHA CI green後に統合する。
- フロントがSingleCamera導線とDual無効表示を統合する。
- `main`または候補branchのSDK-less C++ Debug/Release、M3 simulated Debug/Release、formal WPF flowを実行する。

Exit gate: 実機前に必要なコードblockerが解消し、候補SHAが一意である。

### 8月25日 火曜 午後

- 実シャッターを切らないread-only preflightだけを行う。
- software-onlyで起動、mode選択、Live View mock、capture state、review、export、failure、shutdownを通す。
- 実行ファイルhash、toolchain、テスト結果、既知問題をcandidate manifestへ記録する。
- 17:00 JSTに候補版を凍結する。以後はテスト中止級の修正だけを別SHAで明示する。

Exit gate: 下記Go条件を全て満たすか、No-Go理由が明示されている。

### 8月26日 水曜

1. 候補SHAと実行ファイルhashを照合する。
2. software smokeを実行する。
3. D810一台だけを接続し、identity-v3 read-only preflightを実行する。
4. dedicated spoolの全payload 0を確認する。
5. 操作者が明示再開し、one-shotを最大1回実行する。
6. canonical original、review、export、shutdownを確認する。
7. one-shotが完全Passした場合だけ、別判断でWI-0012の10回characterizationへ進む。

## 5. Go / No-Go

### Go

- #131の修正とnegative testが候補SHAに含まれる。
- #94のshutdown修正が候補SHAに含まれ、Camera AgentとSDK sessionの解放を確認済みである。
- 候補SHAの必要ソフトウェア試験がgreenである。
- D810は一台だけ接続され、identity-v3 read-only preflightが一致する。
- 専用spoolのpayloadが0件である。
- 固定チャート、保存先、USB、SDK利用条件が準備済みである。
- 操作者が実機作業の再開を明示する。

### No-Go / 即時停止

- `read > requested chunk`を拒否しない候補である。
- shutdown後にAgent、SDK session、leaseが残る。
- D810が0台または複数台、identity不一致、profile期限切れである。
- spoolに既存payloadがある。
- JPEG候補が0件、複数件、遅延、または曖昧である。
- watchdog、session close、persist、reread、delete、empty-afterのいずれかが失敗する。
- 自動retry、bulk delete、format、camera-setting write、vendor operationが要求される。

失敗時は存在するPC artifactとcanonical originalを保持し、`FailedPartial`として終了する。同じtransactionを自動再実行しない。

## 6. 証跡

候補版manifestには次だけを記録する。

- commit SHA、branch、build日時、toolchain
- 実行ファイルのSHA-256
- 実行したコマンドとPass/Fail/Blocked
- Issue #94、#131の検証結果
- read-only preflight、spool 0、capture 0/1、delete 0/1、retry 0
- canonical originalの検証結果と匿名run ID
- shutdown後のprocess/session/lease結果
- known limitationsとNo-Go理由

camera serial、SDK archive、licensed binary、credential、実画像、顧客原本、非匿名identityはcommit、PR、Issue、CI artifactへ含めない。

## 7. 完了判定

`M3T`は次の三状態で記録する。

- `Ready`: 火曜17:00のGo条件を満たし、水曜の実機開始が可能。
- `Blocked`: billing、コードblocker、identity、spool、機材のいずれかが未解消。
- `Tested`: 水曜one-shotの結果をPassまたはFailedPartialとして証跡化済み。

`Tested`であっても、M1Aの10回、HG-0009、100回、Dual実機、A0品質、MVP、releaseは未完了のままとする。
