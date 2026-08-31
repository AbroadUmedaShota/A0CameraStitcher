# DualCamera実機撮影 安全監査（2026-08-31）

## 判定

`Blocked`。実機撮影は開始していない。SDK-only／WPD-onlyの読み取り専用ゲートは合格したが、現行要件を同時に満たすproduction撮影経路がない。

## 確認済みの範囲

- 対象branch: `feat/dual-real-capture-backend`
- 読み取り専用実機ゲートの基準commit: `d4bc288092e0ab7c40d3d280baad8050e6db150a`
- 安全修正と回帰証拠の基準commit: `29b86715121f8606e2ea7f244a9079038fae0ddb`
- AOPC-22-NOTEの専用clean worktreeでSDK-only probeを一回実行し、D810 2台、SDK session／source／Module完全終了、process 0を確認した。
- SDK完全終了後、WPD-only probeを一回実行し、D810 2台、CAM-A/B map各1台、両カードpayload 0件、全WPD session close、process 0を確認した。
- `29b8671`はNative Debug full build、CTest 20/20、Foundation 37/37、OperatorShell 63/63、独立read-only reviewに合格し、branchへpush済みである。review判定はpatch `PASS`、Dual hardware readiness `BLOCK`である。
- 撮影、Live View、設定変更、WPD object削除、format、vendor operation、自動retry、USB操作は行っていない。

これは撮影前の読み取り専用ゲートの合格であり、DualCamera撮影のPassではない。

## 停止理由

### 1. SDK完全終了とsession-local token

CAM-A/Bの目視割当tokenは一つのSDK Module session内だけで有効である。`EndDualSession`でModuleを完全終了するとtokenは破棄される。一方、tokenを再列挙せずCAM-AからCAM-B、さらに10／100 pairへ使うにはModuleを保持する必要がある。

現backendはSDK sourceを閉じてもModuleを保持したままWPDへ進む。そのため、次の二条件を同時に満たせない。

- WPDを開く前にSDK session／source／Moduleを完全終了する。
- 同じ目視割当tokenを再列挙・再bindingなしで使い続ける。

AOPC-22-NOTE上のライセンス済みSDK資料を静的・読み取り専用で追加確認した。header 22件、sample source 6件、PDF資料10件の範囲では、同型D810二台で必ず異なり、USB再接続またはSDK Module unload／reload後も不変で、WPD側CAM-A/Bと安全に相関できる正式なper-body property/APIは見つからなかった。`Name`、`Interface`、Module内source ID、candidate ordinal、USB port、firmware／versionは恒久identityへ使用しない。SDK、WPD、Camera Agent、Live View、撮影等の実行系操作は0件である。

### 2. pair開始前の一括preflight

production backendは一台ずつの`Capture(alias)` APIであり、CAM-A撮影前にWPD上のD810 exact 2、CAM-A/B map exact-one、両カードpayload 0、全session closeを一括確認するpair-level APIがない。CAM-Bの非空をCAM-A撮影後に初めて検出し得るため、現状ではshutter gateとして不十分である。

### 3. 10回／100回runner

現WPFは一つのbinding activationにつき一pairだけを実行する。二回目は`CaptureAlreadyActivated`で停止する。10／100回の逐次実行、初回失敗停止、匿名結果一覧、p50／p95／max、p95承認artifactを扱うproduction runnerは未実装である。Native hostの固定600秒寿命も100回試験と両立しない。

## 実装済みの安全修正

実機操作0のsoftware変更として、次を実装した。

- canonical `original.jpg`のatomic publish、再読込、JPEG 7360x4912、size、SHA-256の検証を完了した後だけ、exact WPD objectを削除可能にした。不正寸法や保存失敗ではPC取得済み原画像とWPD objectを保持する。
- `approvalBasis`を自由記述から固定コード`operator-approved-capture-recovery-only-v1`へ変更し、識別子・氏名・path等の永続混入を拒否した。
- activation済みAgentの自然終了を確認できない場合、WPFはProcess handleと排他を保持した`Blocking`で停止する。cancel、kill、reserve／start再送、自動retryは行わない。
- 実production canonical publisherを用いる回帰で、正常時だけdelete 1、不正寸法・既存canonicalでは`FailedPartial`、PC原画像保持、delete 0、撮影1回、retry 0、全session closeを確認した。
- activation済みAgentが非0 exit codeで自然終了する回帰で、exact exit code保持、排他解放、cancel／capture再送 0を確認した。test hostのterminal通知はresponse frame書込み完了後へ固定した。

これらは個別の安全欠陥を修正するが、SDK full-closeとtoken継続の設計矛盾は解消しない。

## 選択肢

1. SDK Module保持を明示的な例外として承認し、source close中のWPD共存を実機で別途検証する。最短だが、現行のSDK/WPD分離要件を変更する。
2. 各camera legでSDKを完全終了し、操作者が毎回再bindingする。分離要件は守れるが、10／100回の無人耐久試験にはならない。
3. SDK unloadを越えて同じD810を安全に選べる、文書化済みの恒久identity／SDK-WPD correlationを確立する。現行の厳格条件を維持できるが、今回確認したSDK資料には利用可能な方式がない。

現行の安全要件を弱めない判断として、one-shotを含む実シャッター操作は引き続き停止する。短期one-shotだけなら2は安全側へ実装可能だが、legごとの操作者再bindingが必要で、同じbindingの10／100回耐久にはならない。自動10／100回を成立させるには3のメーカー根拠が必要である。1は現時点の証拠では採用せず、例外を検討する場合も製品責任者のADRと追加の実機共存・異常系証拠を必須とする。

## 再開条件

- 上記いずれかの設計決定と仕様更新
- pair-level両カードpreflightの実装とnegative test
- 同一条件のfocused／全回帰合格
- 新しい独立レビューの`PASS`
- AOPC-22-NOTEでexact SHA、clean、関連process 0
- SDK/WPD分離条件と両カード空の再確認
- CAM-A/B物理目視対応のoperator coordination

再開後もone-shotを最初に一回だけ行い、Pass後に10回、実測p95の明示承認後だけ100回へ進む。
