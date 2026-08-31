# DualCamera実機撮影 安全監査（2026-08-31）

## 判定

`HardwarePending`。実機撮影は開始していない。SDK-only／WPD-onlyの読み取り専用ゲートは合格し、2026-08-31にADR-0028のSDK Module保持限定例外が承認された。pair-level preflightとproduction adapter coexistence probeは実装済みで、ソフトウェア回帰で検証済みである。ただし、このexact SHAでのAOPC-22-NOTE実機coexistence probeと、one-shot以降の実機証跡は未完了であり、実シャッター操作は開始しない。

## 確認済みの範囲

- 対象branch: `feat/dual-real-capture-backend`
- 読み取り専用実機ゲートの基準commit: `d4bc288092e0ab7c40d3d280baad8050e6db150a`
- 安全修正と回帰証拠の基準commit: `29b86715121f8606e2ea7f244a9079038fae0ddb`
- AOPC-22-NOTEの専用clean worktreeでSDK-only probeを一回実行し、D810 2台、SDK session／source／Module完全終了、process 0を確認した。
- SDK完全終了後、WPD-only probeを一回実行し、D810 2台、CAM-A/B map各1台、両カードpayload 0件、全WPD session close、process 0を確認した。
- `29b8671`はNative Debug full build、CTest 20/20、Foundation 37/37、OperatorShell 63/63、独立read-only reviewに合格し、branchへpush済みである。review判定はpatch `PASS`、Dual hardware readiness `BLOCK`である。
- 撮影、Live View、設定変更、WPD object削除、format、vendor operation、自動retry、USB操作は行っていない。

これは撮影前の読み取り専用ゲートの合格であり、DualCamera撮影のPassではない。

## 解消済みの設計判断と残る技術blocker

### 1. SDK Module保持限定例外

CAM-A/Bの目視割当tokenは一つのSDK Module session内だけで有効である。`EndDualSession`でModuleを完全終了するとtokenは破棄される。一方、tokenを再列挙せずCAM-AからCAM-Bへ使うにはModuleを保持する必要がある。10／100 pairへの再利用は将来の設計意図であり、現sliceの完了範囲ではない。

現backendはSDK sourceを閉じてもModuleを保持したままWPDへ進む。ADR-0028はcontrolled Dual `CaptureRecoveryOnly`に限りこのModule保持を承認した。ただしWPDを開く前に全Live View、SDK source、SDK capture sessionを完全終了し、WPD open中のSDK API operationを0にする。Agent/binding-session terminal teardownまたはbinding invalidationではModuleをunloadしてbindingをinvalidにする。

AOPC-22-NOTE上のライセンス済みSDK資料を静的・読み取り専用で追加確認した。header 22件、sample source 6件、PDF資料10件の範囲では、同型D810二台で必ず異なり、USB再接続またはSDK Module unload／reload後も不変で、WPD側CAM-A/Bと安全に相関できる正式なper-body property/APIは見つからなかった。`Name`、`Interface`、Module内source ID、candidate ordinal、USB port、firmware／versionは恒久identityへ使用しない。SDK、WPD、Camera Agent、Live View、撮影等の実行系操作は0件である。

### 2. pair開始前の一括preflight

production backendは、CAM-A撮影前にWPD上のD810 exact 2、CAM-A/B map exact-one、両カードpayload 0、全session closeを一括確認するpair-level preflightを実装した。preflight不合格または例外時は`ConfirmedUndispatched`で予約を維持し、CAM-A/Bの撮影を開始しない。これはソフトウェア回帰で検証済みであり、実機でのPass証跡ではない。

### 3. 10回／100回runner

現WPFは一つのbinding activationにつき一pairだけを実行する。二回目は`CaptureAlreadyActivated`で停止する。10／100回の逐次実行、初回失敗停止、匿名結果一覧、p50／p95／max、p95承認artifactを扱うproduction runnerは未実装である。Native hostの固定600秒寿命も100回試験と両立しない。したがってpair preflight/coexistence probeの実装済み状態は、10／100回実機試験の開始条件を満たすものではない。

## 実装済みの安全修正

実機操作0のsoftware変更として、次を実装した。

- canonical `original.jpg`のatomic publish、再読込、JPEG 7360x4912、size、SHA-256の検証を完了した後だけ、exact WPD objectを削除可能にした。不正寸法や保存失敗ではPC取得済み原画像とWPD objectを保持する。
- `approvalBasis`を自由記述から固定コード`operator-approved-capture-recovery-only-v1`へ変更し、識別子・氏名・path等の永続混入を拒否した。
- activation済みAgentの自然終了を確認できない場合、WPFはProcess handleと排他を保持した`Blocking`で停止する。cancel、kill、reserve／start再送、自動retryは行わない。
- 実production canonical publisherを用いる回帰で、正常時だけdelete 1、不正寸法・既存canonicalでは`FailedPartial`、PC原画像保持、delete 0、撮影1回、retry 0、全session closeを確認した。
- activation済みAgentが非0 exit codeで自然終了する回帰で、exact exit code保持、排他解放、cancel／capture再送 0を確認した。test hostのterminal通知はresponse frame書込み完了後へ固定した。

これらは個別の安全欠陥を修正する。ADR-0028に対応するpair-level preflightとproduction adapter read-only coexistence probeも実装済みで、ソフトウェア契約・回帰で検証済みである。WPD cleanup未確認時はSDK API（`End`を含む）を呼ばずAgentを隔離・terminal化し、binding失効理由を上位へ返して再bindingを必須にする。実機前の読み取り専用coexistence証跡、および実機one-shot以降の証跡は未完了である。

## 選択肢

1. SDK Module保持を明示的な例外として承認し、source close中のWPD共存を実機で別途検証する。**選択・承認済み（ADR-0028）**。SDK/WPD source/capture session分離は維持し、Moduleはbinding tokenだけに限定する。
2. 各camera legでSDKを完全終了し、操作者が毎回再bindingする。分離要件は守れるが、10／100回の無人耐久試験にはならない。
3. SDK unloadを越えて同じD810を安全に選べる、文書化済みの恒久identity／SDK-WPD correlationを確立する。現行の厳格条件を維持できるが、今回確認したSDK資料には利用可能な方式がない。

選択済みの1により、同じbindingでのone-shotへ進むための設計経路を再開できる。ただし実シャッター操作は、以下の技術gateを完了するまで停止する。10／100回はproduction runnerとhost lifetime方針の実装・検証後の別scopeであり、この例外承認、coexistence probe、またはone-shotからは開始しない。Module保持は合成、A0品質、実シャッター同期、releaseを承認しない。

## 再開条件

- ADR-0028と要件・Agent contract・Phase 0計画・generated requirements/planの同期
- pair-level両カードpreflight、production adapter read-only coexistence probe、focused／全回帰、独立reviewのソフトウェア証跡をexact SHAで固定する
- AOPC-22-NOTEで同じexact SHAのread-only coexistence probeを実行し、source/capture session close、Module retained、WPD open中SDK operation 0、WPD closeを実機で確認する。WPD cleanup未確認時はSDK APIを呼ばずAgentを隔離・terminal化し、再binding要求を記録する
- AOPC-22-NOTEでexact SHA、clean、関連process 0
- SDK source/capture sessionとWPD sessionの分離、Module保持境界、両カード空、WPD cleanup確認の再確認
- CAM-A/B物理目視対応のoperator coordination

再開後の現scopeでは、coexistence probe合格後にone-shotを最初に一回だけ行う。10回、実測p95承認、100回は同一binding runnerとhost lifetime方針を別途実装・検証し、明示的に再開するまで`NotRun`／`HardwarePending`とする。
