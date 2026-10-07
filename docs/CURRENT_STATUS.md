# 現在の開発状況

更新日: 2026-10-07（#221 の 1 回目を数える所有者決定を反映。Partial・Deferred・次の安全な順番に残っていた 10 回・100 回の記述を ADR-0030 の最大 5 回に揃えた。過去の実績の記録は変えていない、#232）、2026-10-06（二台順次撮影 V-PAIR-001 の初回 1 組の合格を追記。#216 修正後の一台実機アプリ撮影の合格を追記）、2026-10-05（一台実機アプリ撮影の初回と原画像パス不一致を追記。計装 A の完了を追記。run-05 の結果・ADR-0031 の凍結・次の作業順を追記。main 着地前レビューを追記。2026-10-02 に AOPC-20-NOTE での最初の実機記録を追記。T1・T2 は 2026-10-01 に `codex/dual-live-worker-poc-20260922` へ commit 済み。以下の 2026-09-23 節は当時の記録）

## 2026-10-06 二台順次撮影の初回 1 組（V-PAIR-001、AOPC-31-NOTE、#221）

- 対象: main `cb3df7d`（#222 二台のシャッター直前確認、#224 結果の読み取り、#218 試験の修正を含む）。SDK 有効 Release の SHA-256 は WPF `5995c7571ad8314c0ac02d1bf8a6e95bec6267c7ef2d62ed5db8b1873de74dc9`、DualCameraAgent `769a17d3ef8694eda503c5f5f3d2f61ace0c109914d67f5716dd2027d5457ff3`。WPF を `--hardware-dual --wpd-camera-map <map> --capture-recovery-only --approved-capture-profile <profile> --dual-identity-proof <proof>` で起動した（`--capture-recovery-run-count` なし = 1 組）。画面操作は所有者の指示で UI Automation から行った。
- 準備（撮影なし）: D810 を 1 台ずつ接続し、`bind-identity --transport wpd --single-camera-connected-confirmed` で CAM-A（ファームウェア V1.11、一台試験と同じ個体）と CAM-B（V1.14）を登録。2 台接続の `inventory` は bound 2 / unbound 0。承認 profile は固定値 10 項目（`a0.dual-capture-profile.operator-approved.v1`）。`--dual-identity-proof` はこの経路では中身を読まないため、その旨のメモを置いた。`--read-only-sdk-probe`・`--read-only-wpd-probe`・`--read-only-coexistence-probe` は撮影に使ったビルドで 3 つとも `terminalState: Pass`、両カード payload 0（[probes](evidence/phase0/dual-probes-20261006-cb3df7d/probes.jsonl)）。
- 機体照合: 候補の Live View を見ながら、CAM-A にレンズキャップを付けて映像が覆われた候補を CAM-A、もう一方を CAM-B に割り当てた。照合の有効期限は確定から 5 分で、返答待ちの間に一度失効した。
- 1 回目（14:43、transaction `020a434cd48749339e0ff2faa832b820`、[CAM-A leg](evidence/phase0/dual-leg-CAM-A-run-1791265416981-2/events.jsonl)）: CAM-A のシャッター直前確認で `dual_jpeg_fine_not_confirmed`。D810 が `fileType` を広告しないため、二台経路の確認が実機で必ず失敗する不具合だった（#222）。シャッター 0、CAM-B 未開始、原画像 0、カード削除 0、自動再試行 0。さらに Agent が `bindingInvalidationReason` の None を空文字で書き WPF が読めなかったため、画面は「結果不明」のまま pending が解けなかった（#224）。
- #224 修正後、同一 ID の照会で 1 回目は `Failed: CaptureCameraA` として確定し、pending が解除された（シャッターなし）。
- 2 回目（17:47、transaction `d1d8cda50623482d984597cf156469e5`、[CAM-A leg](evidence/phase0/dual-leg-CAM-A-run-1791276431296-2/events.jsonl)、[CAM-B leg](evidence/phase0/dual-leg-CAM-B-run-1791276446458-4/events.jsonl)）: `Succeeded`、27 秒。CAM-A → CAM-B の順に各 1 回撮影し、各原画像を保存・検証してから該当 1 件だけ削除、両カードとも削除後に空を確認。自動再試行 0。原画像は CAM-A 17,983,817 B（SHA-256 `2f0549ac1ee787ce9c59a143d874157bffa4b2e526808521411c456511c842b4`）、CAM-B 18,082,209 B（`d947b9a6504c32aa821b054bad8ca18d18d66f2db7ac9c315168e13d432787f0`）。どちらも `Get-FileHash` で再計算し、アプリ表示と一致（2026-10-06 17:48）。合成は Pending、A0 品質は Unapproved のまま。
- 画面で「結果を採用して次へ」を押し、採用を記録した。
- 終了時: 採用の直後にアプリを閉じると「実機セッションの終了を確認できないため、この画面と実機の排他を保持しています。（状態: HardwareCameraAgentLaunchException）」で止まった。終了処理は Agent に何も送らず、撮影ホストの自然終了を 5 秒だけ待つ設計で（`DualCameraAgentLifecycle.cs` の `WaitForActivatedCaptureHostExitAsync`）、Agent の寿命は起動から 10 分固定（`hardware_camera_agent_pipe.cpp`）。撮影がすぐ終わったため寿命の途中だった。両 leg の SDK session・WPD はすでに閉じており、カメラ側に後始末は残っていなかった。10 分経過後に Agent の終了（`Get-Process` で不在）を確かめてから閉じるボタンをもう一度押すと、1 回で閉じた。強制終了はしていない。閉じた後も pending は空、journal は `Succeeded`、原画像 2 枚のサイズと SHA-256 は不変、委譲 marker 0。画面に「待ってもう一度閉じる」旨と理由が出ない表示の問題は別 Issue。
- 実機の時間: 所有者の決定（2026-10-06）により、1 回目（シャッター 0）も ADR-0030 の最大 5 組に数える。使用 2 組、残り 3 組。シャッターが切れたのは 2 回目の 1 組だけ。1 回目の失敗と合否（NFR-REL-001 の「最初の失敗で停止」）の関係は ADR-0032（#230、2026-10-07 承認）で確定した。1 回目は予算に数え、合否には数えない（2-B。原因の #222 は main `cb3df7d` で修正済みで、2 組目はその上で撮った）。
- 残り: 残りの組（最大 5 組まで）、時間の p95 レビュー。終了時の表示の改善は #225 で対応済み（実機確認待ち）。

## 2026-10-06 一台実機アプリ撮影の再実施と合格（V-1CAM-005、AOPC-31-NOTE）

- 対象: main `5ae3e0a`（#216 の修正 `b834825` とレビュー対応 `5ae3e0a`）。別 PC（AOPC-31-NOTE、VS2022 BuildTools、.NET SDK 10.0.401）で SDK 有効の Release をビルドした。CameraAgent の SHA-256 は `429c1805e253e37898a513130ef1cd28e20e9a03781d4581523a2c06daddaf71`、Phase0 CLI は `e2ad44d8e040ce8583185b3b720fb7528cf77149a71f54c3bc55667d3184de35`。WPF Release の出力フォルダ直下に CameraAgent を置き、`--hardware-single` で起動した。
- 準備（撮影なし）: D810 1 台。`inventory` は WPD・SDK とも 1 台。`bind-single-identity-v3 --alias CAM-A --single-camera-connected-confirmed` で `identity-v3-created`（撮影・Live View・設定変更・カード操作 0）。`spool-status`（[run-1791262091401-1](evidence/phase0/run-1791262091401-1/report.md)）は PayloadObjectCount 0 だが、旧 identity map を参照するため `wpd_identity_unbound` で `SpoolState: UNKNOWN`。空の判定はアプリの readiness（identity-v3、spool payload 0）で取った。read-only の `sdk-status`（[run-1791262290305-1](evidence/phase0/run-1791262290305-1/report.md)）で JPEG Fine・L(7360*4912)・exposureMode 3・1/6・F8・ISO 64・WB Preset 1・focusMode 1、設定 write 0 を確認した。`shootingMode=S` はレリーズモードの値で、露出モードではない。
- 撮影プロファイル: 画面の「観測値を30日プロファイルとして承認」で `single-cam-a-20261006` v1（期限 2026-11-05）を作った。画面操作は所有者の指示で UI Automation から行った。
- 1 回目（13:54、transaction `8d4966ff75bc44cca2c79592b9e224e8`、[run-1791262442681-1](evidence/phase0/run-1791262442681-1/transaction-events.jsonl)）: `FailedPartial / capture_command_failed`、`SDK command failed: 137`。137 は Maid3d1.h の `kNkMAIDResult_OutOfFocus` で、AF が合焦せずシャッターが切れなかった。撮影前の spool は空、原画像 0、カード削除 0、自動再試行 0。所有者がカメラを合焦できる対象へ向けた。
- 再承認: 次の readiness は `capture_settings_mismatch`。read-only `sdk-status`（[run-1791263056167-1](evidence/phase0/run-1791263056167-1/report.md)）では、絞りの表示は `8` のままで currentIndex が 8 から 9 に変わっていた。他の設定は同じ。レンズ側の操作で絞りの候補一覧がずれたためと推測する（※未確認）。同じ観測値で再承認し、ID と版は `single-cam-a-20261006` v1 のまま SHA-256 が `d564797dd3a2f0756618dd185f98c8bc65aa15c0b38c9f51e2add27f778432ed` になった。アプリは内容が変わっても同じ ID・版を使う。
- 2 回目（14:05、transaction `6f89595400034564a213409322190483`、[run-1791263130722-1](evidence/phase0/run-1791263130722-1/transaction-events.jsonl)）: `Complete`。SDK 撮影 1 回 → SDK close → WPD で候補ちょうど 1 件を回収 → PC 原画像を保存・検証（18,112,900 B、SHA-256 `22019cddcf5140cf6dbc1079e0d7424e8b14c15f05bb79038aae0cd1e6bc815f`）→ 該当 1 件だけ削除 → spool 空を確認。自動再試行 0。原画像は `<artifacts>/run-1791263130722-1/6f89595400034564a213409322190483/CAM-A/original.jpg` に着地し、アプリの transaction ID と一致した。
- アプリ側: 保持原画像は「CAM-A / 18,112,900 bytes / SHA-256確認済み」。「単体原画像を保存」で固定ローカルフォルダへ書き出し、書き出したファイルの SHA-256 を `Get-FileHash` で再計算して原画像と一致を確認した（2026-10-06 14:09）。続けて「採用して次の撮影を準備」で採用し、未確認の保存結果は 0 件になった。前回の `original_reread_failed` は再現しない。
- 運用上の注意: アプリは CameraAgent を常駐させる（`--serve-once` なし）。アプリを閉じた後も agent プロセスが残った。agent が待機中の間に read-only の `sdk-status` を 2 回実行し、どちらも operator-session lease を取得して SDK session を開閉した。session の重複は記録上ない。
- 2026-10-05 の run `run-1791195109792-1` の原画像は `hybrid-tx-` 配下にあるため、修正後の agent でも `transaction_original_invalid` になる。必要なら元の PC の artifacts から手動で取り出す。
- 残り: SingleCamera の実 WPF 100 件受入（#13）、Continuous Live View handoff 10 回（#11）、物理異常系。
- 2026-10-07 追記（#232）: 回数は ADR-0030 で最大 5 回に置き換わっている。一台の受入系列（最大 5 回、保存の確認を含む）と USB 抜去は #227、handoff は #11（最大 5 回へ書き換え予定）で扱う。#13 の 100 件は ADR-0030 で置き換わったため、閉じるか合成を含む統合受入へ移すかを #232 で提案した。数え方と合格線は ADR-0032（#230、2026-10-07 承認）で確定した。#225（終了時の案内）・#226（二台の原画像の保存）は main に着地済み。実機での確認は #227。

## 2026-10-05 一台実機アプリ撮影の初回（V-1CAM-005、AOPC-20-NOTE）

- 構成: D810 1 台（CAM-A）、`bind-single-identity-v3 --alias CAM-A --single-camera-connected-confirmed` で SDK/WPD 1/1 を結合（撮影なし）。撮影前の `spool-status` は PayloadObjectCount 0。承認済み撮影プロファイル `single-cam-a-20261005` v1（JPEG Fine・L 7360×4912・露出モード M・1/6 s・F8・ISO 64・WB プリセット 1、期限 2026-11-04、SHA-256 `4e18b8db…42f9`）。アプリは `build/wt-988d07a` の WPF Release に SDK 有効の CameraAgent（SHA-256 `AB1A7E98…DFD8`）を同梱して起動した。
- 実施: 19:11:49 に画面の「安全確認済みの一台実機transactionを一回開始」を UI Automation で 1 回だけ押した。Live View は使っていない。自動再試行は 0 回。
- カメラ側の結果: run `run-1791195109792-1`、transaction `0879c3a70db84b45a6cef3279bba6416`、terminalState Complete、所要 10.3 秒。SDK 撮影 1 回 → SDK close → WPD で候補ちょうど 1 件を回収 → PC 原画像を `.partial` 経由で保存・検証（17,842,102 B、SHA-256 `88c4b02fb1511cef1d64bc131180ae6caa4d25a92641fa743ff91050986048ce`）→ 該当 1 件だけ削除 → spool 空を確認。証拠は [run-1791195109792-1](evidence/phase0/run-1791195109792-1/transaction-events.jsonl)（実機識別子なし）。保存済み原画像の SHA-256 をローカルで再計算して一致を確認した（`sha256sum`、2026-10-05 19:14）。
- アプリ側の結果: 保持原画像は「recordはありますが、アプリ側の再検証に失敗しました。保存不可です。」、理由は `original_reread_failed: Camera Agent artifact path is not the canonical location for this run.`。export・採用には進んでいない。
- 原因: アプリは原画像の場所を `<artifacts>/<run>/<アプリの transaction ID>/<alias>/original.jpg` と決め打ちし、完全一致を要求する（来歴保証の設計）。一方、実機の hybrid 経路はフォルダ名に内部 ID `hybrid-tx-<時刻>` を使い、agent 側の正規位置検査は先頭フォルダ名を問わない。fake 経路の試験ではこの差が表に出なかった。修正方針は agent 側で、アプリが要求した transaction ID を hybrid のフォルダ名に使い、agent 側の検査もその ID との一致に締める（アプリ側の検査は緩めない）。修正・試験の後、SDK 有効ビルドで同じ手順をもう一度行う。
- 今回の原画像はローカルの artifacts に残したまま（commit しない）。カードは空に戻っている。

## 2026-10-05 run-05 と次の作業（AOPC-20-NOTE）

- run-05 の結果: 所有者の GO を受けて 12:14 に二台 run を 1 回だけ実施した（`build/sdk-dual-poc/Release` の上記ハッシュの exe）。worker 0 の enumerate は候補 2 を返し、その次の select が `worker_selection_invalidated` で失敗した。画面は fail-closed で隔離を維持した。journal には失敗ブロック（worker 0・select・分類・応答検証・ACK 書込み・close receipt 5 項目）と両 worker の exit ブロックが揃い、worker 0 は exit code 3、worker 1 は exit code 0・close receipt 5 項目すべて完全。Source open と Live View 開始には達していない。撮影・設定変更・WPD・カード操作・自動再試行は 0 回。実機 preview 枠は 5/5 を消費した。詳細と原因の読み（本命は既列挙 ID の AddChild 再通知 ※推定）は [二worker試作記録](DUAL_LIVE_WORKER_POC.md) の「二台実機run-05」節。
- 隔離の現状: 画面 process は隔離表示のまま生存しており、通常終了要求（12:17:57）は拒否された。`MarkerDiagnostic --read-only` は `process_active_or_unknown`。所有者の判断で marker の手動削除はしない。**このPCの再ログオン・再起動は、下の B が入るまで行わない。（補足: B が守るのは B を含むビルドの exe だけで、既存の exe は再ログオン後も素通りする。再ログオン後は `MarkerDiagnostic --read-only` が別 session の marker を `marker_ambiguous` で止めるため、別 session の marker の回復は C の仕様として所有者が決めるまで未定。したがって B が入った後も、C の方針が決まるまで再ログオン・再起動の制限を残す）** 現行コードは marker のファイル名に Windows session ID を含め、`RejectMarker` は現在の session の marker しか見ないため、再ログオン・再起動後は隔離が黙って外れる（※コード読み、未検証）。 追記（2026-10-05 13時台）: 画面process（PID 21104）は、起動スクリプトを待機させていたバックグラウンド実行が1時間の上限で停止した際に巻き込まれて終了した（※推定。操作者の承認による終了ではない。Orchestratorの運用上の落ち度として記録する）。workerは既に終了していたためSDK・カメラの状態への影響はない。終了後の `MarkerDiagnostic --read-only` は `eligible_for_human_review`（匿名SHA-256 `2909726e76cef52be3ff2dd967e109c7c31be655a3cc27efbcb7b8ab2e762564`、148 B）で、隔離markerは残っている。journalは30行で不変。回復は監査付き回復コマンド（C）の実装と所有者の承認を待つ。 marker の回復（2026-10-05 18:30、所有者の承認による手動経路 B）: 監査付き回復コマンド C の完成を待たず、所有者が「実害がないなら開発を進めたい」として手動回復を選んだ。Orchestrator が用意した 1 回限りのスクリプト（前提をすべて照合し、1 ファイルだけを削除、再試行なし）は auto mode の分類器に止められたため、所有者自身が実行した。記録: カメラ 2 台の電源 OFF・USB 切断後、D810 は `Get-PnpDevice -PresentOnly` 0 件・`Win32_PnPEntity`（Status OK）0 件、A0 関連 process 0、session id 2 が marker 名と一致、marker root の項目は `armed-session-2.marker` の 1 件だけ、`MarkerDiagnostic --read-only` は `eligible_for_human_review`（匿名 SHA-256 `2909726e…2564`、148 B）でファイルのハッシュも一致。証拠の控えを marker root の外（ローカルのみ・commit しない）に保存してから 1 件だけ削除し、診断は `marker_missing` になった（18:30:57）。監査付き回復コマンド C は実装・総合レビュー承認済みで、security レビューと MEDIUM 対応を残して後で仕上げる。
- 計装 A の完了（software-only、commit `853c075`、2026-10-05 に main へ着地）: worker が module の AddChild／RemoveChild を区間別に数え、18 個の数値を応答封筒 v2（`a0.preview-worker.v2`、`diag` を厳密に検査し、版ずれは SDK に触る前に fail-closed）で親へ返し、親が `topo_block_*` として journal に書く。画面操作の時刻は `preview_requested` で残る。分類は `worker_selection_invalidated` を `worker_selection_inventory_changed`／`worker_selection_topology_event` に分け（旧語は残り、60→62 語）、worker main の最上位例外を exit code 4 に分けた。試験は新モード 4 系列×5 回・既存 preview 系 23 件×3 回・独立検証すべて failures=0、SDK 有効構成のビルド exit 0（2026-10-05、報告の転記）、レビューは総合・設計適合・セキュリティ表層とも承認。journal の読み方の表と exit code 表は [二worker試作記録](DUAL_LIVE_WORKER_POC.md) の「機器イベント計装 A」節、正本は `docs/design/dual-preview-topology-diag.md`。 着地前の全体検証（HEAD `853c075`、SDK-stub `build/handoff-verify`、2026-10-05 16:18〜17:47。ディスクを飽和させる他プロセス `find.exe` が並走）: Debug 51/54、Release 54/55。Debug の `phase0_contracts`（期限 2 秒の時間依存試験）と `pc_direct_summary_v2_e2e`（pwsh ホスト自体の CLR 内部エラー）は環境起因と判定。両構成で落ちた `hardware_camera_agent_contracts`（25 分タイムアウト）は、`TestProductionContinuousLiveViewContracts` が製品の `StartContinuousLiveView` を通じて本番名・本番 root の lease を取り、この PC に実在する run-05 の marker で開始が拒否された後、フレーム読取りの合図を期限なしで待つためと特定した（同試験は marker ができる前の 11:22 には 27.95 秒で PASS）。A・B・C の回帰ではなく、単体試験が PC の本番 marker root に依存する既存の欠陥で、試験を test-root の lease に切り離し待ちに期限を付ける修正を残件とする。なお Release 実行は未コミットの回復コマンド C を取り込んでビルドしていた。
- ADR-0031: 2 プロセス × 2 module 構成の二台 Live View 実機 PoC を凍結した（Blocked、2026-10-05）。解除条件はベンダーの書面確認、または所有者による新しい実機予算の明示承認。二画面 Live View の目標は放棄しない。MVP は一台選択式 Live View のまま。根拠は [DECISIONS](DECISIONS.md) の ADR-0031 末尾。
- 次の software 作業の順番（実機操作なし）:
  1. D1／D2: run-05 記録の確定と ADR-0031 凍結の記録（完了）。
  2. B（Must）: 委譲 marker を全 session 分走査して拒否する修正（完了、`da678a6`）。運用 PC の exe は、B を含む SDK 有効ビルドへの置き換えを待つ。
  3. A（Must）: worker 側の topology 計装（完了、`853c075`。上の項目）。
  4. C（次。Should、E が承認されれば Must）: 監査付きの回復コマンド。承認のうえ、確定した marker 1 件だけを 1 回処理する。
  5. E-prep（Should）: SDK のみを使うプローブの実装。実行は別承認。
  6. D3（Could）: Nikon 窓口への照会文の作成。
- 所有者の決定（2026-10-05 16時台、口頭の一言で確定）: (1) 実機の時間は MVP 本線（一台の実アプリ workflow V-1CAM-005 → 二台順次撮影 V-PAIR-001 → 二台固定校正）へ回し、二台同時 Live View は凍結のまま。(2) SDK のみの topology プローブ（E）に実機枠を付ける（1 回、上限 2 回）。Source を開かない 2 段目は AGENTS.md の「SDK セッションは同時に 1 つ」に当たらないという解釈で可。(3) Nikon 開発者窓口への照会は行わない（D3 は取り下げ）。画面 process の終了可否は、process が時間上限停止に巻き込まれて消滅したため解消（上記の追記を参照）。この決定により、委譲 marker の監査付き回復コマンド C は Must に上がる（marker を回復しないと本線の実機作業もプローブもこの PC では始められない）。
- 残件（追加）: 二台 binding 経路の `PollDualInvalidation`（`src/phase0/nikon_sdk_transport.cpp` 1176 行付近。判定に使う `dual_topology_changed_` は同ファイル 3104 行付近の `ModuleEventProc` で、既知 ID の AddChild では立たない）は既知 ID の AddChild を許容しており、worker selection の「イベント 1 件で無効化」（`worker_preview_selection.hpp` の `ObserveTopology`）と判断が食い違う。どちらを正本にするかは A の計装データが出てから reviewer_architecture と security が判断する（今回コードは変えない）。
- 残件（計装 A、2026-10-05）: SDK 経路の実行時動作と callback スレッドの前提は実機 run 待ち（新しい実機 run には所有者の承認が要る）。試験の未カバー 2 点: exit code 4 が実プロセスの境界まで届くこと、v1 応答の拒否を実 IPC 越しに確かめること。LOW 2 点: worker 側の JSON 解析失敗の分類を `worker_envelope` へ正規化する、`kPreviewWorkerSchema` を共有ヘッダ `preview_topology_diag.hpp` に置いたことによる推移的依存。

## 2026-10-02 実機接続の最初の記録（AOPC-20-NOTE）

- WPD読取り列挙を2回実行した（コマンド `build\sdk-verify\Release\A0CameraStitcher.Phase0.exe inventory --transport wpd`、SDK有効Release、HEAD `22b4c4c`、exe SHA-256 `E0CA9D9F…C0B2`。撮影・設定変更・削除・vendor操作はない。実行前後ともA0関連processは0、`MarkerDiagnostic --read-only` は `marker_missing`）。
  - 1回目 2026-10-02 17:04:11、exit 0、303 ms。present だったD810は1台のみで、出力は `UNBOUND model=Nikon D810 firmware=V1.11 shootingMode=S`、`CameraCount: 1`。もう1台はPnPで Present=False（原因は電池切れ。操作者が電池を入れ直した）。
  - 2回目 2026-10-02 17:05:42（電池交換後）、exit 0、283 ms。出力は `UNBOUND model=Nikon D810 firmware=V1.11 shootingMode=S` と `UNBOUND model=Nikon D810 firmware=V1.14 shootingMode=S`、`CameraCount: 2`、`BoundCameraCount: 0`、`UnboundCameraCount: 2`、`IdentityMapChanged: false`。2台のファームウェア版が異なる（V1.11 と V1.14）。※要確認: 試験プロファイルで版の統一を前提にしているか。
- このPC（AOPC-20-NOTE）はSDKを `.tools/nikon/d810-remote-sdk`（ignored）に配置済みで、SDK有効ビルドは HEAD `d256dba` 以降で成立している。実機でのSDK操作（Live View を含む）はまだ1件も行っていない。

## 2026-10-05 main 着地と run-05 候補ビルド（AOPC-20-NOTE）

- 着地: `git push origin HEAD:main` で main を `3e9ae40` → `706eaab` に fast-forward（82 commit、PR なし、12:06）。ブランチ `codex/dual-live-worker-poc-20260922` も `706eaab` に同期。push 後の GitHub Actions の run は増えていない（最新は 2026-09-21 の pull_request）。
- run-05 候補ビルド: clean HEAD `706eaab` から新規フォルダで `cmake -S . -B build/sdk-dual-poc -G "Visual Studio 16 2019" -A x64 "-DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk" "-DA0_BUILD_DUAL_PREVIEW_POC=ON"`（12:07）。configure ログに「Nikon D810 licensed adapter enabled」「A0_BUILD_DUAL_PREVIEW_POC=ON (A0_NIKON_SDK_AVAILABLE=ON)」と試作を SDK 有効でビルドする旨の CMake Warning が出て、`ctest -N` は 35 件。Release build は PreviewWorker・PreviewCommissioning・MarkerDiagnostic・SingleWorkerPreview・Phase0 の 5 本とも exit 0、C4819/C4834 以外の警告 0（12:07〜12:11）。exe SHA-256: 試験画面 `11272FFC952DFE8870FCCEA9537B2ABD78323F253EA5108A79B93333F2570619`（196,096 B）、worker `3C62D4A622FCFFDADCD4953486C8B8820048EFA935DEF85BF4179E07F071E83D`（300,544 B）、MarkerDiagnostic `85747F450B0D43349B0E8C56B701697CC8117125BF24635E7AD3982BF3087B9A`、SingleWorkerPreview `40F450753E4C0AB1C145762BE37D9C87B96A8404F73EC6CF8FC3DB6F8A197847`、Phase0 `F5271550167DF4F561BFB6E98506CD7B12CA819BAE753B038341EB99AD73125A`。
- 注意: `build/sdk-verify/Release` にはビルドゲート導入前（2026-10-01）に SDK 有効でビルドした PreviewWorker／PreviewCommissioning の exe が残っている。run-05 には使わず、`build/sdk-dual-poc/Release` の上記ハッシュの exe だけを使う。
- 起動前点検（12:06、読取りのみ）: D810 2 台 present、A0 関連 process 0、`MarkerDiagnostic --read-only` は `marker_missing`。run-05 の実施は所有者の GO 待ち（当時。12:14 に実施し、結果は上の run-05 節）。

## 2026-10-05 main 着地前レビュー

- `codex/dual-live-worker-poc-20260922`（HEAD `0e5068e` 時点）を main へ着地させる前に `/review-code` の3観点レビューを行った。判定は性能 承認、セキュリティ 承認（LOW 7件、任意対応は下の2026-10-01節の残件に記録）、総合 差し戻し（H1・H2）。
- H1への対応: CMake option `A0_BUILD_DUAL_PREVIEW_POC` を追加し、`A0CameraStitcher.PreviewWorker`・`A0CameraStitcher.PreviewCommissioning` と二worker試作の試験targetをその下に置く。既定値はSDK有効ビルド（必要ファイルが揃って `A0_NIKON_SDK_AVAILABLE` が成立したビルド）でOFF、SDK-stubビルド（CI・ローカル試験）でON。`A0CameraStitcher.SingleWorkerPreview` と `A0CameraStitcher.MarkerDiagnostic` は常にビルドする。3構成の configure（2026-10-05、PowerShell、cmake 3.31.12、VS2019 BuildTools、リポジトリ直下）で `ctest -N` の登録件数を確認した: stub `cmake -S . -B build/gate-stub -G "Visual Studio 16 2019" -A x64` → 49件（option ON）、SDK既定 `cmake -S . -B build/gate-sdk -G "Visual Studio 16 2019" -A x64 "-DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk"` → 29件（option OFF、PreviewWorker／PreviewCommissioning と試作試験の .vcxproj は生成されない）、SDK＋明示ON（同コマンドに `"-DA0_BUILD_DUAL_PREVIEW_POC=ON"` を追加）→ 35件（PreviewWorker／PreviewCommissioning と commissioning・dispatcher・dual_live 系の試験は生成されるが、stub専用の4実行ファイル14試験は入れ子の `if(NOT A0_NIKON_SDK_AVAILABLE)` で生成されず、`CMake Warning` が configure ログに出る）。PowerShell で `-DNIKON_D810_SDK_ROOT=.tools/...` を引用符なしで渡すと `=.` で引数が割れて黙って stub 構成になるため、引用符が必須。H2への対応: 試作IPC試験の待ち時間 `kTimeout`（1500 ms）がプロセス起動を含む待ちにも使われていたため、`tests/dual_live_test_ipc.hpp` に `kSpawnTimeout`（15000 ms）を追加した。起動を含む待ち（`Connect`・`Reap`・helper の待機読取り・poc worker の最初の読取り）を `kSpawnTimeout` に、起動を2回またぐ待ち（integration の grant 待ち、helper の報告読取り）を `2 * kSpawnTimeout` に切り替えた。integration の worker 側の要求待ちも lease probe の起動をまたぐため `kSpawnTimeout` にした（この試験は worker の idle 期限を検査しない）。controller 側の定常 I/O と poc worker の2回目以降の読取りは 1500 ms のまま。「idle IPC deadline」試験は、最初の読取り（観測窓 `2 * kSpawnTimeout`）と1回応答した後（観測窓 `kTimeout * 3`、`static_assert(kTimeout * 3 < kSpawnTimeout)`）の2本にした。ctest の TIMEOUT は `dual_live_worker_poc_contracts` 10→60秒、`dual_live_session_integration` 15→120秒。`ctest --test-dir build/t1-stub -C Release -R '^(dual_live_worker_poc_contracts|dual_live_session_integration)$' -j 1 --repeat until-fail:10 --output-on-failure` は最終版で 10/10 PASS（2026-10-05 11:11、計 234.46 秒。poc 21.6〜21.9 秒、integration 1.5〜3.4 秒）。赤確認として、最初の読取りと grant 待ちだけを 1 ms にすると両試験が失敗することを確認し、復元した。
- 文書の追従: ARCHITECTURE・ROADMAP・PRODUCT_REQUIREMENTS の二台Live View記述をビルドゲートの実態に合わせ（「gateを通過するまで実装・有効化しない」を「製品機能として有効化しない。試作はCMake optionの下に置きSDK有効ビルドでは既定OFF」へ読み替えた。この読み替えは総合レビューが人の判断事項として挙げたもので、所有者へ可否を2回提示し、2026-10-05 の「開発を進めて下さい」の指示で続行した。個別の文言への明示承認ではない点を記録する）、MACHINE_OPERATION にカメラ制御入口の運用前提（委譲markerが残るPCでは製品ビルドも全拒否、回復手順は未承認、junction環境の注意、二台preview用のoption指定）を追記した。
- 着地はPRを作らず main への fast-forward の直接 push とする（所有者の指示で GitHub Actions CI は使用しない。ワークフローは `pull_request` と手動起動のみで、push では起動しない）。証跡は最終 HEAD でのローカル全体検証を次に記録する（2026-10-05、AOPC-20-NOTE、VS2019 BuildTools・cmake 3.31.12・.NET SDK 10.0.400、HEAD `4b27c90`、`git status --porcelain` 空、他のビルドなし）。(a) SDK-stub `build/handoff-verify` を `cmake -S . -B build/handoff-verify -G "Visual Studio 16 2019" -A x64` で再 configure（`A0_BUILD_DUAL_PREVIEW_POC:BOOL=ON`）、Debug/Release の全 build は exit 0。(b) `ctest --test-dir build/handoff-verify -C Debug -j 1 --output-on-failure` は 49/49 PASS（277.68 秒、11:22〜11:26）。(c) 同 `-C Release` の 1 巡目は 48/49（322.56 秒、11:27〜11:33）で、失敗は `phase0_contracts`（45.46 秒。失敗した Check は「an overdue SDK capture must still close the SDK session exactly once」と「must not begin WPD recovery…」）。この試験は main と同一内容で、watchdog 2 秒に対し SDK fake が 2.1 秒で返す設定のため、SDK を開く前の fake WPD baseline と証跡書き出しが 2 秒を超えると期限が先に切れて `sdk.opens` が 0 になる。当該実行は試験全体が通常の 2 倍以上遅く（単独では 19.35 秒）、ビルド直後の初回実行の I/O 遅延（常駐ウイルス対策のスキャン ※推定）と判定した。単独再実行 `ctest -R '^phase0_contracts$'` は PASS（19.35 秒、11:35）。負荷なしの Release 再巡は 49/49 PASS（329.29 秒、11:56〜12:01。`phase0_contracts` は 16.12 秒で PASS）。(d) M3: `scripts/Test-M3Simulated.ps1` Debug/Release と `scripts/Test-DualCameraWpfFlow.ps1` Release は、既知の 1 件（`HardwareSingleInvalidAgentStorageStopsBeforeLaunchAsync`、この PC のシンボリックリンク権限不足）で不合格。Foundation tests と DualCamera flow tests はスクリプト内で先に合格している（operator shell の段で停止）。試験 exe を直接実行した結果は、Release 96/97（11:52〜11:56、失敗は上記の権限の 1 件のみ）、Debug 95/97（11:48〜11:52。権限の 1 件に加え「single-camera CAM-B capture skips stitch and exports one original」が `transaction.json.<id>.partial` の共有違反 `IOException: being used by another process` で失敗）。後者の書き込み経路 `DurableSimulatedCaptureCoordinator.cs:261-265`（`.partial` に書いて `File.Move`、再試行なし）は main から変更されておらず、Release 側では再現しない。書き込み直後のファイルを常駐ウイルス対策が一瞬掴む断続的な環境要因と判定した（※推定）。Debug の再実行は 96/97（166 秒、12:02〜12:04。権限の 1 件のみ失敗で、`.partial` の共有違反は再現しなかった）。改名時の共有違反への有界な再試行は製品コードの残件として記録する（AGENTS.md の「撮影の自動再試行禁止」とは別の、ファイル改名の話）。(e) 試作 IPC 試験 2 件の `--repeat until-fail:10` 10/10 PASS は上記 H2 の記録を参照。着地後も二worker試作は製品機能ではなく、実機は各回の本人承認に限る。ADR-0031のgate（SDK文書が独立sessionの同時Live Viewを許可すること）は未達のまま。

## 2026-10-01 引き継ぎ後の現在地

- 引き継ぎ後の最初の作業として、二worker試作の失敗証跡をjournalへ固定語彙で永続化する計装T1をsoftware-onlyで実装した（commit `79d6687`）。失敗したworker番号・命令・57語の固定分類・応答検証・ACK書込み・worker申告close receipt・両workerのexit観測をjournalに残す。語彙と記録順は [二worker試作記録](DUAL_LIVE_WORKER_POC.md) の「失敗証跡のjournal永続化 T1」節を参照する。
- 合格の読み方を明文化した。判定行 `both_workers_close_verified` に加え、index 0と1のexitブロックが揃い両方の `worker_exit_code` が0であること。判定行だけでは部分書込みと区別できない。
- 制約: worker0が応答前にexitするCase 4のexit codeは再観測で取れる場合に限る。ビルド負荷下の `--exit-recheck` × 20（2026-10-01 19:13）で17回取得、3回は `worker_exit_not_observed_at_recheck`。受入基準は「数値、または両時点で未終了という固定イベント」とし、数値の決定的取得はT2/WU2へ送る（※仮定）。（T1当時の記録。T2で解消）
- 検証: `a0_preview_worker_owner_tests` の新規4系列と既存2系列をctestに登録。実装者実行と独立QA（7系列 `failures=0`、2026-10-01 18:13〜18:14、`build/t1-stub` SDK-stub・Release）、総合レビュー2回目承認・セキュリティ表層承認。全体検証（2026-10-01）: C++ CTestはDebug 45/45、Release 43/45で、失敗2件は本変更と無関係な試作IPC試験の断続的失敗（※要調査。2026-10-05 の着地前レビュー H2 で試験側の起動待ちを分けて解消）。M3 simulated／WPF flowはoperator shell tests 95/96 PASSで、残り1件はこのPCのシンボリックリンク権限不足（開発者モード無効）による環境差。詳細は[二worker試作記録](DUAL_LIVE_WORKER_POC.md)のT1節。
- T2「停止ブロックとclose待ちの整合」をsoftware-onlyで実装した（commit `d256dba`）。操作別の予算表を `preview_worker_timing.hpp` の1か所に置き、親のExchange期限を D = W + M で操作ごとに計算する。close送信とexit観測を分け、窓（E_ok 5 s / 0 ms / E_fail 30 s）の後も生存するworkerは `worker_stop_in_progress`（停止処理中）とする。操作期限（既定60 s）と受付寿命（既定170 s）を分け、期限後の命令は親がIPCの前に `owner_operation_deadline_expired` で拒否する。相方workerのclose応答（`worker_close_*`）と `licensed_adapter_unavailable` もjournalに残すようにした（分類60語）。Case 4のexit codeは25/25、Case 4bは15/15で決定的に取得できた。試験は新4系列をctestに登録し、実装者の修正ラウンド後 `--repeat until-fail:3` で18/18×3 PASS（2026-10-01 22:51）、独立QAは修正ラウンド前にRelease・Debugとも失敗0。詳細は[二worker試作記録](DUAL_LIVE_WORKER_POC.md)のT2節。
- 未検証・残件: 実SDK・実機での動作。SDK有効構成の本ビルドはHEAD `d256dba` で8実行ファイルともexit 0（試験用ctor拒否の実行確認は試験targetがSDK構成で生成されないため未実施）。親が期限で拒否した後にcloseがcleanになった場合、拒否分類が画面の `FirstFailure()` にしか残らずjournalに失敗ブロックが無い（※要対応・次回）。G・Close同期枠10 s・起動枠9 sは経験則で、実機journalの所要時間で検証が要る。journal書込み失敗の注入試験。セキュリティレビュー（2026-10-05、承認・LOW 7件）の任意対応: worker／試験画面／単体previewのmain冒頭で `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` を呼びDLL検索順を固定する（mainから引き継いだ既存の問題。SDKの実行時読込みが変わるため実機確認1回とセットで入れる）、親のパイプ接続に `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION` を付ける、`DisarmDualDelegation` へ渡す型付き証跡を定数trueではなくclose receiptの値から組み立てる、MarkerDiagnosticのmarker openに `FILE_SHARE_WRITE | FILE_SHARE_DELETE` を加える、ローカル候補の `.pdb` にビルドPCの絶対パスが入る件（`-p:ContinuousIntegrationBuild=true` か許可リストから除外）、今後の記録ではPC名を別名にする。
- 前任PCのrun-04隔離marker、実機preview枠4/5、本人判断パケットA/Bの扱いは変更していない。

## 2026-09-23 二台構成の現在地

- 二台同時Live Viewの実機run-04は、worker Aの候補列挙後に失敗し、両workerの完全close証拠を取得できなかった。委譲markerは解除せず隔離を維持。実機preview枠は4/5消費、残り1回。左右の新規frame・物理A/B確認・撮影/合成の実機受入は未達。詳細は [二worker試作記録](DUAL_LIVE_WORKER_POC.md) を優先する。
- ソフトウェア側では、採用保存結果が不明なときに結果画面を保持し、次原稿準備を阻止する修正を統合。SDKなしの確認画面専用テストは5件すべてPASS。run-04 marker用の読取り専用診断API/CLIはsynthetic test-rootで診断系列5/5 PASS、SDK有効構成のCLIもRelease build成功。ただし本番markerへのCLI実行、ファイル差替え競合の決定的試験、SDK有効exeの引数拒否実行は未確認。
- 「実つなぎ目へ移動」は2026-09-22に実レンダリング由来のseam座標をmanifest v2へ保存し、保存画像の100% viewerへ渡す実装とsoftware-only検証を完了済み。下の2026-09-21節にある「実seam位置への移動」は当時の残件であり、現在の未完条件は実WPF scroll/DPIの目視操作、実画像・実機品質の受入である。根拠は [操作UI仕様](OPERATOR_UI_SPEC.md#2026-09-22-実つなぎ目への移動) を参照する。
- この成果はマーカー解除や二台ライブビューの成功を意味しない。例外的な一件限定回復には、新鮮な物理隔離・対象/証拠の再照合と別の本人判断が必要。回復後のrun-05も別判断とし、撮影、配布、mergeへ自動移行しない。

## 今回の対応範囲と候補

- 最新の実機証拠: 本人承認により一台ずつ接続し、CAM-B `run-1789977134868-1`、CAM-A `run-1789977425037-1` が各1/1 Complete。原画像の保存・再検証・今回objectだけのcleanup・empty-afterまで成功、retryなし。ローカル証跡は `%LOCALAPPDATA%/A0CameraStitcher/acceptance/scene-cli-b-single` と `scene-cli-a-single`。机上風景でありDual同時接続撮影・合成・A0品質の受入ではない。
- 最新UX実装: `MainWindow` に保存済み原画像/合成結果の表示切替、全体/100%拡大用read-only viewer、Pending/Accepted永続化、採用保存後の次原稿準備を実装。現在結果ID・画像hashを採用前に照合し、保存失敗では結果画面を保持。撮り直し準備で撮影・旧画像削除を行わない。Foundation `--review-store` 2/2 PASS、Release build警告0/エラー0、UI `--review-ux` 3/3 PASS（採用保存後の応答失敗・明示再操作、画像改変拒否、旧原画像保持、再起動時記録、既存Dual成功/出力失敗経路）。テストselectorの変数名重複によるbuild失敗は修正後に再buildし、アプリ/テスト側DLLのSHA-256一致も確認。独立read-onlyレビューのP1/P2（合成hash、原画像のみmodeの初期選択、publish後cancel、partial記録）は修正・再確認済み。今回の実機操作なし。
- UX残件: 専用SingleCamera画面への展開、過去の未確認結果の再表示・再検証・再採用、実seam位置への移動、外部AI用の正式CLI/IPC、実画面/実機受入。二台同時ライブビューはFR-LV-003 / ADR-0031の技術gateを維持し、偽の二画面Liveは追加しない。
- 二台同時ライブの調査結果: 正規SDK `Module/ReadMe_Eng.txt` Limitationsは一moduleによる二台以上の制御を非対応と明記。現行module内で二つのSourceを同時openする案は採用しない。別process案も保証・排他安全性は未確認で、資料調査だけでは有効化しない（ADR-0031追記）。
- これより下の起動前・撮影前の記述は経緯。現時点の実機状況は上記の単体撮影証拠を優先する。

- 本人指示: STEP1〜4を完了させ、反復試験は最大5回程度にする。ADR-0030により新規10/100回工程を最大5回へ置換する。過去の試験実績は変更しない。
- base: `3e9ae406272027274fb454b8f9b4d9d63ac2569f`（PR #214統合済み）。作業branch: `codex/step4-five-run-acceptance-20260921`。
- 候補変更: Phase 0実機反復CLIは1〜5回、WPF専用反復は5組、Single handoff証跡はv2/5回。5回は初回を含み、同じ系列の追加起動・失敗補充をしない。
- ソフトウェア検証: 新候補のRelease CTest 24/24 PASS（261.47秒）、`Test-M3Simulated.ps1 -Configuration Release` PASS、`--five-run-acceptance` PASS、M2 pre-gate/corpus検証・更新JSON parse・`git diff --check` PASS。M3 buildは警告0/エラー0。C++ buildには既存C4819警告が残る。実機操作は含まない。
- AFの残留リスク: 直前baseの全体試験では撮影+AFが5秒timeoutし、focused経路は430msで合格した。今回の全体試験は合格したが、timeout値は変更せず、原因特定・修正済みとは主張しない。
- 独立read-onlyレビュー: 通常経路で5回超のdispatchは見つからず。HardwarePending時の集約証跡不在をADR-0030へ明記し、旧coordinator参照・README起動例を更新した。ローカル全体ログは`build/step4-m3-validation.log`、CTestログは`build/wpf-m2-adapter/Testing/Temporary/LastTest.log`（ignored）に保持する。
- STEP2/3: 本人指示によりAOPC-22-NOTEは使用しない。現在PC（AOPC-11-NOTE）で二台を検出し、各spoolのpayload 0件・read-only coexistence probe PASSを確認。ローカル証跡は `%LOCALAPPDATA%/A0CameraStitcher/acceptance/scene-pair-first-check`。これは撮影受入PASSではない。
- 本人承認: 机上の風景を各一枚・計一組だけ撮影し、失敗時は再試行しない。固定平面原稿ではないためA0合成品質の受入には使わない。画面で本人が割当した後、旧simulation journalの `attentionAcknowledgedAtUtc` が未対応で起動時検査に失敗し、撮影dispatch前に停止した。本人がアプリを閉じ、関連プロセスの終了を確認済み。
- 互換性修正: 上記日時だけをnullableな履歴項目として読み込み・再保存する。未知項目の拒否、FailedPartial、同一ID再実行禁止は維持し、実在の保存記録は変更しない。`FoundationTests --journal-compatibility` 3/3 PASS、OperatorShell Release build警告0/エラー0。今回の修正検証はsoftware-onlyで、修正版の実機起動・撮影は未実施。再開前に候補artifactを再固定し、二台のsession-local割当をやり直す。
- STEP4: 固定warp/feather/crop/原本保持は実装済み。承認済み実rig・品質基準、権利処理済み実写D810 pairが不足している。現存corpus exampleはvector契約5件、実写pair 0件。下段の旧10/100回ロードマップは履歴であり、今回のrun数には適用しない。

## 旧採用版の履歴（今回の候補・実行条件は冒頭を優先）

- 採用版: `main`のPR #212統合commit `f66e18269e44df19c06f24a182eb99fc608f359f`。Camera Control Pro 2公開仕様との比較資料と、read-only `spool-status`のD810個体照合をWPD撮影command広告から分離する改善を統合済み。
- 統合証拠: PR #212 head `af7454cec77c9129154edd539b54cc360ff9235f`は独立review PASS、GitHub software-only CI SUCCESS後に通常mergeした。実装commitは`46d97b66cc85f3350130ee9182b2a37c67b53849`。これはsoftware統合済みを示すが、未配布・実機未受入。
- 起動方法: 承認済み候補artifactと期待SHA-256をreadinessで照合した後、Phase 0 CLIの`spool-status --alias CAM-A`を使用する。READMEのDebug例は開発用であり、実機候補を自動選択しない。実機実行にはその時点で適用可能な対象・artifact SHA・回数・no-retry条件の承認照合が必要。
- 対応済み範囲: D810検出、local identity mapによるalias照合、inventory全session close、同一identityのread-only content open、全payload件数取得、checked close。撮影command広告は匿名診断に残すが、spool読取り成功条件には使わない。
- 未対応・未確認: 非広告D810を使う具体WPD COM分岐の実機確認、PC直接保存transaction、DualCamera実機受入、配布・実利用受入。`UNKNOWN`を`EMPTY`または成功へ丸めない。
- 次行動: merge commit `f66e1826...`から固定artifactを作成する。実機は別途適用可能な承認を照合し、CAM-A単体のread-only `spool-status`から開始する。結果がidentity/content/payload/close以外の未知点を示す場合にだけ追加解析を判断する。
- 人の判断が必要な事項: 実機操作・候補artifact差替え、未承認のtransport変更、配布、`HG-0001/0002/0005`。通常の可逆なsoftware修正・検証・PR更新はPM判断で継続する。

## 総合判定

`in-progress / Dual hardware HardwarePending`。2026-08-26にNikon D810一台、`SingleCamera`、`CAM-A`のCamera Agent撮影経路でone-shot 1/1、10/10 characterization、p95 `14.643秒`のProduct Owner承認、100/100耐久を完了した。原画像111件の再検証と安全指標も合格した。ただし、実WPF画面からの100回操作、Continuous Live View handoff 10回、物理USB切断・保存先障害は未検証であり、SingleCamera全体は`Partial`である。DualCameraは、production backendとWPF `CaptureRecoveryOnly`（撮影・原画像回収のみ）、ADR-0028のModule保持境界、pair-level preflight、read-only coexistence probe、および同一bindingの10回runnerをsoftware実装済みである。WPD cleanup未確認時はSDK APIを呼ばずAgentを隔離・terminal化して再bindingを要求する。一方、同一bindingの100回runnerは未実装であり、Dualの実機coexistence probe、one-shot、10回／100回の受入証拠は未完了である。既定600秒のserver/request稼働時間予算は実装済みだが、processの強制終了時刻の保証ではなく、安全な終了処理が予算を超える可能性がある。連続試験との適合を示す実機証拠はない。実装済みを実機Passへ読み替えず、DualCameraは`HardwarePending`、合成は`Pending`、A0品質は`Unapproved`を維持する。

2026-09-16に評価したSDK PC直接保存は、`main`のPR #199〜#201で安全入口・匿名診断・完了判定を実装したが、実機2回ともPC原本を保存できず未受入である。これは、上記dedicated single-slot spool経路の合格を取り消すものではなく、spool経路の合格をPC直接保存の合格へ読み替えるものでもない。

### 2026-09-16 PC直接保存の実機評価

| 対象 | source | 結果 | 観測された事実 |
|---|---|---|---|
| [run-1789528249365-1](evidence/phase0/run-1789528249365-1/report.md) | PR #199/#200統合後 `f2a24b83e2dff066b32d5681cfeb18a8c738fa85` | `FailedPartial / image_event_timeout` | capture 1回。post-baselineの同一Item IDを通知・列挙で1件観測したが`CaptureComplete`は0。PC `original.jpg`／`.partial`は0。SaveMedia復元、SDK close、事後spool 0を確認 |
| [run-1789540578257-1](evidence/phase0/run-1789540578257-1/report.md) | PR #201統合後 `7bb042c4693acc40fceb59b4ffb4d76123ad9b52` | `FailedPartial / image_event_timeout` | capture 1回。post-baseline採用候補0、forced enumeration 727/727成功、`CaptureComplete` 0。PC `original.jpg`／`.partial`は0。SaveMedia復元、SDK close、事後spool 0を確認 |

2回とも自動retry、card fallback、camera delete、formatは0で、実画像・実識別子・SDK配布物はrepositoryへ保存していない。2回目の`candidateCount=0`はpost-baseline採用候補が0だったことを示し、生のSDK通知が0だったとは断定しない。`cardUnchanged=false`は事後fingerprintが取得不能だったためであり、カード変更の証拠ではない。独立した事後WPD確認ではpayload 0だった。

旧失敗で残ったSDRAM ItemまたはItem ID再利用がbaselineで除外された可能性は仮説であり、現証拠では確定していない。PR #202（head `f42f4c110961806958f6319ac6762220fed1c16b`、merge `2efa30e7259c3c76e7f98bdba26127d4aa47207f`）で、SDRAM baseline非空時の撮影前停止と匿名診断追加をsoftware-only CI・独立レビュー後に`main`へ統合した。次工程は固定artifactと安全条件を再提示した上での実機評価であり、追加実機撮影には別の明示承認を要する。

### 二台撮影で、できていること・残っていること

| 項目 | ソフトウェアの状態 | 実機での確認 |
|---|---|---|
| 1組の撮影と原画像2枚の回収・保存（CaptureRecoveryOnly） | 実装済み。合成は行わない | 2026-10-06 に初回 1 組が Succeeded（冒頭の節） |
| 同じCAM-A/B割当で最大5組を順番に実行（ADR-0030） | 5回runner実装済み。HardwareDualの他の必須引数と併用し、`--capture-recovery-only --capture-recovery-run-count 5`で明示起動（受け付ける値は5だけ。省略すると1組）。失敗時停止、自動retryなし | 5組の受入・実測p95承認は未完了 |
| 同じCAM-A/B割当で100組を順番に実行 | 内部制御をWI-0017-SW01で準備。本番CLI/UIの100回開始は未対応で、引き続き拒否する | ADR-0030 により今回の受入から除外（100 組の耐久は主張しない） |
| 時間・SHA-256の集計とp95承認記録 | 記録処理は実装済み。100件の記録を扱えることと、100回の撮影を実行できることは別 | 実機実行や承認者の権限をソフトウェア集計だけでは証明しない |

根拠は[現行5回実行処理](../src/m3/OperatorShell/Hardware/CaptureRecoveryOnlyFiveRunCoordinator.cs)、[WPFからの操作](../src/m3/OperatorShell/ViewModels/OperatorShellViewModel.cs)、[集計・承認記録処理](../src/m3/OperatorShell/Hardware/CaptureRecoveryOnlyRunEvidence.cs)。2026-09-11までの記載はPR #183統合後の`main`の`56f3cb36182812969126a34cd12137105bf3840c`を読み取り照合したもので、新たな実機操作の結果ではない。

2026-09-12の[WI-0017-SW01](WI0017_HUNDRED_RUN_CORE.md)では、100回試験の内部制御と偽workflowによる検証を実機から分離した。開始前のp95承認照合、各回の記録保存、最初の失敗・未確定・割当失効・時間不足での停止を扱う。本書と要件中の「100回runner未実装」は、本番起動経路と実機での適合確認を含むrunner全体の残件を指す。内部部品の追加だけでこの残件やWI-0017を完了扱いにしない。

第三者向けには[Phase 0 二台カメラ・ショーケース](PHASE0_SHOWCASE.md)を入口とする。二台順次撮影のsoftware contractと安全停止は提示可能だが、実機二台撮影とA0品質の受入完了は主張しない。

## 確認済み

- 2026-09-06時点の検証対象source SHA `fd3ed8c0f294caa6dbea9dcf9f7e5e1f9072453d`でlicensed SDK adapterを有効化し、native Debug/Release CTest各20/20、M3 simulated Debug/Release、focused DualCamera WPF Debug/Releaseに合格した。経路検査上のcamera command、PnP、USB/WPD、実機操作は各0であり、runtime telemetryではない。これはIssue #10のsoftware-only検証であってDual実機受入ではない。
- [2026-08-31 DualCamera安全監査](DUAL_HARDWARE_SAFETY_AUDIT_2026-08-31.md): AOPC-22-NOTEのSDK-only／WPD-only読み取り専用ゲートはD810各2台、CAM-A/B map各1、両payload 0、全session close、process 0でPassした。安全修正`29b8671`はNative CTest 20/20、Foundation 37/37、OperatorShell 63/63、独立reviewでpatch `PASS`となりpush済み。ADR-0028のproduction pair preflightとModule保持read-only coexistence probeは実装・ソフトウェア回帰済みだが、同一exact SHAでの実機coexistence probeと撮影証跡は未完了である。ライセンス済みSDKのheader 22件、sample 6件、PDF 10件の静的調査でも、Module reload後に同型D810を一意照合できる正式なper-body property/APIは確認できなかった。同監査時点では同一bindingの10/100 runnerは未実装と記録された。その後10回runnerは実装済みとなったが、100回runnerとDual実機受入は未完了であり、readinessは`HardwarePending`を維持する。同監査での実撮影、delete、設定変更、USB操作は0件。
- 2026-08-29時点で、WPFの明示起動引数、承認済み`a0.dual-capture-profile.operator-approved.v1`、同一Agent内CAM-A/B割当から`CaptureRecoveryOnly`へのactivation、予約前durable snapshot、1 reserve／1 start、曖昧時same-ID照会、再起動時の再binding、CAM-B失敗時CAM-A原本保持、JPEG 7360×4912・size・SHA-256再検証、stitch `Pending`・A0品質`Unapproved`を実装した。通常`start-reserved-pair` schemaと通常合成経路は変更していない。この項目はsoftware-onlyで、実シャッター、実WPF操作、実機one-shotの合格を意味しない。
- [2026-08-26 SingleCamera実機結果](SINGLE_CAMERA_HARDWARE_RESULTS_2026-08-26.md): one-shot 1/1、10/10、p50 `14.036秒`、p95/max `14.643秒`、HG-0009承認、100/100初回成功、100回p50 `14.204秒`・p95 `14.430秒`・max `14.692秒`。計111原画像のJPEG寸法・size・SHA-256再検証に合格し、原画像消失・誤削除・曖昧採用・自動retry・復旧不能停止は各0件だった。
- PR #163でNikon SDK非同期バッファ寿命とSingleCamera WPD identity-v3経路を修正し、Release buildとfocused contractsを確認後にmainへマージした。GitHub ActionsはBilling制限により未実行であり、CI greenとは扱わない。
- 2026-08-17時点でDual専用schema `a0.camera-agent.hardware-dual.v2`の4操作、durable pair store、予約→開始→同一ID照会、.NETのReserved／terminal typed recovery、厳密なsemantic preflightを実装済みである。fake backend限定orchestratorはCAM-A→CAM-Bを各最大一回、自動retry 0、共有180秒deadlineで実行する。A失敗時はBを開始せず、B失敗時はA原本を保持する。terminalはtransaction ID別にatomic publish・再読込検証され、その後だけactiveを削除する。過去結果を残したまま次pairを予約でき、再起動後も同一IDで照会できる。Foundation 22/22、DualCamera 18/18、Operator Shell 22/22、SDK-less／licensed Debug/Release CTest各10/10、M3 Release/Debug、正式DualCamera WPF flowが合格した。当時はproduction Dual経路が未接続だったが、この制約は上記2026-08-29のsoftware実装で更新された。実機受入が未完了で`HardwarePending`である点は変わらない。
- 正式camera modelはNikon D810であり、明示的な`SingleCamera`または`DualCamera`をUSBで運用する。`SingleCamera`はCAM-A、WPD serial digest、SDK/WPD各exactly-one current session、canonical original一件、stitch `NotApplicable`、byte-identical `7360×4912` export、30日read-only profileとする。`DualCamera`は従来どおり固定平面A0原稿をCAM-A→CAM-Bで順次撮影・合成する。
- modeはactive transaction外で明示選択し、接続台数から推定しない。`DualCamera`の一台不足を`SingleCamera`へ自動降格せず、active中のmode変更を禁止する。
- D810一台の電源再投入後PnP再列挙とWPD側CAM-A continuityを匿名証拠化。SDK側の旧continuity結論はephemeral source ID使用のため無効化され、当時の二台identity-v2衝突により恒久identityを前提とする経路はBlockedとなった。この履歴は保持し、現在のDual方針はADR-0025のsession-local operator bindingとして区別する。
- 一台Live Viewを5分04秒・2,424 frame継続し、停止、SDK close、preview非保存を確認。
- read-only setting runでJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得。`FileType`はnot-advertised。
- 2026-08-07にD810一台を`CAM-A`として再列挙し、設定read-only、10 frame Live View、停止、SDK close、直後のOFF再確認に合格。設定変更、preview保存、撮影、WPD、deleteは0件。
- direct WPD/SDK capture入口をfake-onlyへ閉じ、承認済みhardware経路を`hybrid-capture-single`へ限定。
- 実SDK/WPDコマンドを同じWindowsログオンsession内の二つのPhase 0 processから同時実行しないnamed OS leaseを追加し、別process保持中の拒否を自動試験で確認。別ユーザーsession／serviceはMVP運用外。
- pair/hybrid transactionへ180秒の全体watchdogを適用し、SDK撮影が期限をまたいだ場合もWPD回収・保存・削除・成功状態へ進まないこと、PC保存中に期限をまたいだ場合は`.partial`を保持して`original.jpg` rename・削除・成功状態へ進まないことを確認。
- 2026-08-08時点の変更はSDK有効／なしの両構成でCTest 5/5、当時のM3 simulated Release警告0、Foundation 13/13、Operator Shell 1/1に合格した履歴証拠である。Nikon SDKはcommit対象外の`.tools`へ再配置済み。
- M2Pの光学計算、rig-profile trust、三状態setup-assessment、WI-0022Cの合成画像測定からbounded correction proposalへの接続がsoftware-only合格。shift／rotation／scale／exposure／colorを決定的に測定し、profile provenance mismatch、draft、malformed、over-limitをfail closedする。最終リグ、承認済み閾値、実写A0品質は証明しない。
- M3PのNamed Pipe、durable simulated transaction、起動同意・readiness・操作ロック・失敗復旧を含むWPF shellは、2026-08-10のrequirements 2.6.0に対するfresh Release build 0 warning/0 error、Foundation 19/19、Operator Shell 15/15、`Test-M3Simulated.ps1` Passでsoftware-only合格した。明示Single/Dual、required aliases固定、no-auto-fallback、Single original一件、stitch `NotApplicable`、明示export、起動時操作gateを含む。
- 実機一台WPF経路は、同一transaction ID・alias・承認profile ID/version/SHA/expiry・Live View handoff intentをdurable保存して結果照会するsoftware boundary、検証済みcanonical originalの明示byte-identical export、`TransactionNotFound`時のsupport-required保持まで実装・契約試験済みである。現在の`hardware.v1` Live View handoff結果は撮影後の有限一frame probeを取得後に停止・SDK closeするもので、継続streamの「再開」を主張しない。
- requirements 2.7.0の継続Live View v2 sliceはQA revise後のfresh SDK-less／licensed-SDK-enabled Debug/Release CTest各7/7、.NET Release build警告0・エラー0、Foundation 20/20、Operator Shell 17/17、M3 boundary script、非破壊showcaseに合格した。production backendへ偽SDK/clockを注入し、512 KiB frame境界、session所有権、二重start、heartbeat timeout、単列backpressure、stop/close失敗後のcapture拒否を確認した。実Named Pipeは1 MiB未満／丁度を受理し、1 MiB超過をdispatch前に拒否する。canonical Base64と解析済みv2 rejection envelopeもnegative test済みである。camera command 0のsoftware-onlyで、実機合格ではない。
- C++ `hardware.v1`は、operator-session lease、directory作成から初期journal確定までを含むgap-free transaction reservation/mutex、exactly-one・alias/profile/expiry相関、Live View OFFのWPD前・open SDK shutter-session内再確認、SDK/WPD非重複、canonical originalを同一handleで再検証してwrite/delete禁止のままexact WPD delete・empty-afterまで保持する境界、fixed-local path、no retry、bounded pipe/journalをsoftware contract化した。WI-0022C追加後の2026-08-10 SDK-less Debug/Release全CTestは7/7で、`hardware_camera_agent_contracts`を含め0 failureだった（このsliceのlicensed SDK buildは未実行）。camera commandは送っておらず、実D810、actual JPEG、WPF hardware操作の合格証拠ではない。
- `WI-0010A`のsoftware contractを実装した。MAID `SdkCommandTrace`は唯一のMAID entry boundaryで全`CapStart`を計数・分類し、photographic-setting CapSet、storage-routing CapSet、Live View-control CapSet、capture、unknown/non-capture CapStart、session状態、counter整合性をfail closedする。`sdk-status`は列挙とread-only statusだけを公開する`NikonSdkStatusExecutor`へ分離した。CAM-Aの既定経路はSingleCamera identity-v3をstrict loadし、その合格後だけWPD enumeratorを生成してread-only列挙し、exactly-one D810とcurrent WPD digest一致を検証する。これらが全て合格した後だけSDK executorを生成・列挙・open/probeする二段階routingである。missing／malformed／digest不一致／WPD 0台／複数台ではSDK factory・enumerate・open/probeが全て0となる。明示`--camera-map`だけがlegacy/Dual v2へ入り、CAM-BへSingle identityを流用しない。process routing proofはidentity列挙と禁止対象のWPD/capture/deleteを分けてv5 summaryへ記録する。修正後の実D810 v5再実行は未検証であり、過去のv4 evidenceは変更していない。
- このidentity-v3 route修正はfocused Phase 0／CLI safety／Camera Agent contract、SDK-less Debug/Release全CTest各7/7、licensed-SDK-enabled Debug/Release全CTest各7/7、M3 Release警告0・エラー0、非破壊showcaseに合格した。licensed構成を含めcamera commandは0件である。
- 2026-08-08の再起動後dual binding readinessでD810 PnP、SDK inventory、WPD inventoryを各2台匿名確認した。SDK/WPDともbound 0・unbound 2で`READY_FOR_IDENTITY_BINDING`。inventoryの列挙順自動割当を廃止し、旧SDK mapは値を読まず可逆隔離した。SDK有無各CTest 5/5も合格し、撮影、Live View、設定変更、card操作は実行していない。
- readinessは選択stageの台数とSDK・WPD・PnP件数の完全一致を要求する。現在の二台接続はDualでbinding待ち、Singleではexit 1で拒否され、余分なD810を許可しない。
- WI-0010A完了後、licensed Release binaryの`verify-dual-identity`をread-only identity確認として厳密に1回実行したが、SDK inventoryで`identity_collision`となりfail closedした。WPD inventory開始、map変更、cross-transport binding、cardアクセス、capture、Live View、設定write、delete、format、0x9207、retryはない。追加のSDK header／document調査でも、二台のD810本体を恒久的に区別するdocumented SDK propertyまたは安全なSDK/WPD相関アンカーは見つからず、当時の恒久identityを前提とするDual経路はBlockedだった。この意思決定gateは後のADR-0025でsession-local operator bindingへ置換したが、二台実機受入の合格を意味しない。一方、操作者報告では一台exactly-oneと`bind-single-identity-v3 --alias CAM-A`はSDK/WPD各1台、camera mutation 0でPassした。その直後の`sdk-status`はlegacy mapを参照してcamera open前にalias unavailableとなったため、このsoftware defectを上記identity-v3経路へ修正した。実機での再実行までは合格扱いにしない。
- 2026-08-08 17:31 JSTのCAM-A cross-transport bindingは後続の物理入替試験で無効化した。WPDは入替後の個体を未登録として区別した一方、SDKは旧CAM-Aへ誤一致した。原因はMAID source object IDを個体IDとしていた実装であり、当該SDK mapを値を読まず可逆隔離した。過去のSDK側CAM-A continuity主張も再検証対象である。
- SDK identityをdocumented MAID Source `Name`/`Interface`の境界付きlocal-only digest v2へ変更し、source object IDを除外、欠落・不正文字列・二台衝突をfail closedにした。SDK有無Release CTestは各5/5。17:49 JST、現在のCAM-B候補をSDK/WPDへ明示bindingし、各bound 1・unbound 0、Single `READY`を確認した。CAM-Aへ戻した際の別個体判定、再接続、port交換まではcheckpointであり完了扱いにしない。
- 18:14 JSTのCAM-A swap-back申告後もSDK/WPDはともにCAM-B、WPD firmwareは直前のCAM-Bと同じ`V1.11`だったため、本体交換は証拠化できずCAM-A登録を拒否した。WPD identityはPnP device IDから本体報告`WPD_DEVICE_SERIAL_NUMBER`のlocal-only digest v2へ強化し、現在のV1.11個体をCAM-Bへ再登録した。実serial/identityは出力していない。
- SDK/WPDを別々に登録する途中で本体が変わる余地を閉じるため、`bind-cross-transport-identity`を追加した。一つのcamera-control lease内でSDK sessionを完全closeしてからWPDを列挙し、両mapの競合を事前検証後にだけ双方を登録する。競合時の片側map未変更、台数0/複数拒否、引数guardを契約化し、SDK有無Release CTest各5/5と現在のCAM-Bでidempotent実機実行に合格。撮影・Live View・設定・card操作は0件。
- `verify-dual-identity`を追加し、SDK/WPDで台数2、CAM-A/B各1、unbound 0を同時に満たす場合だけ`Ready`とする匿名durable summaryを実装した。台数不一致、未登録、alias cardinality不一致を撮影前に拒否する。現在一台の実機`run-1786182987492-1`はSDK/WPD各1、CAM-B各1を記録し、想定どおり`camera_count_mismatch` / exit 5、map変更・撮影・Live View・設定・card操作0で停止した。
- `verify-dual-spools`を追加し、dual identity `Ready`後だけCAM-A→CAM-Bの順に二つのWPD cardをread-onlyで開き、全payload数が双方0の場合だけ撮影laneを`Ready`にする。片方でも非空なら両体を`spool_not_empty`で停止し、delete/format/vendor operation/retryは行わない。現在一台の`run-1786183481065-1`は`dual_identity_not_ready` / exit 5、`CardInspectionPerformed: false`、WPD session 0、撮影・削除0で停止した。SDK有無Release CTest各5/5。
- 実機`hybrid-capture-pair`の入口も共通dual identity検証へ接続した。Readyでない試行は匿名`dual-identity-verification-summary.json`を残し、card access・capture・retryなしでexit 5とする。SDK有効/無効Release buildとCTest各5/5に合格。現在一台のため、安全確認フラグを未確認のまま実機コマンドは実行していない。
- P3Aで3 CLIの入口をstrictなprovider config/proof loaderと`VerifyDualIdentitySoftwareContract`を共有するproduction preflightへ接続し、preflight Block判定をSDK/WPD transport生成より前へ移した。approved anonymous fake inventoryだけはpublic callerでsoftware-only `Ready`を確認できるが、Nikon production providerは未実装で`Vendor clarification required`である。default/legacy実CLIは`identity_strategy_unresolved`、明示opt-inもproduction inventory不在でBlockし、hardware Readyや実体同一性の受入れ証拠ではない。
- M3PでCAM-A→CAM-B durable simulated transactionを100/100実行し、両原画像、順序、各100回、retry 0、再起動時の未完了0を確認した。
- 実機用`hybrid-capture-pair`のsoftware contractは1/10/100組、100組集計、初回失敗停止、共有180秒watchdog、p50/p95/max匿名時間集計に合格した。時間値はPhase 0合否には使わない。
- CAM-A完了後・CAM-B開始前のアプリ停止をdurable event logから匿名診断し、完了原本保持、retry禁止、新規transaction必須を復元するrecovery summary contractに合格した。実プロセス停止試験は未実施。
- pair fault contractはCAM-A SDK close失敗時のB未開始と、CAM-B WPD recovery-open失敗時のA原本保持・B原本なし・cleanup/retryなしに合格した。実USB切断・電源断は未実施。
- `hybrid-fault-pair`を追加し、CAM-A/Bを明示選択して各bodyのSDK完全close後・WPD recovery前にUSB切断または電源断を行う実機入口を用意した。共通dual identity gate、両専用spool確認、operator-session lease、明示alias、operator gateを必須とする。fake契約はA異常時B未開始、B異常時A原本保持、未確定bodyの原本・delete・retryなし、匿名pair fault summary/reportを確認。SDK有効/無効Release buildとCTest各5/5、CLI不足引数2件のcamera-open前拒否に合格。実異常注入は未実施。
- `hybrid-interrupt-pair`を追加し、CAM-A verified original保存・exact cleanup後かつCAM-B開始前だけ中断専用gateをreadyにする。gateはprocess終了以外で正常復帰せず、continue markerとtimeoutをfail closedにしてCAM-Bを開始しない。再起動後の`report --run-id`で`after-CAM-A-before-CAM-B`、A原本保持、retry禁止、新規transaction必須を復元する。fake gate/pair契約とSDK有効/無効Release build・CTest各5/5、missing gate・alias/scenario指定のcamera-open前拒否に合格。実process終了は未実施。
- 19:28 JSTの最終read-only再確認`run-1786184898081-1`でもSDK/WPD各1台、CAM-B各1、CAM-A 0、unbound 0だった。`camera_count_mismatch` / exit 5で停止し、map変更、capture、Live View、設定変更、card access、実識別子出力は0。元CAM-A本体の接続・登録なしには二台実機試験を開始できない。
- 2026-08-09のoperator判断により、物理的な電源再投入・再起動と実power-off復旧subtestは`N/A / Skip`としてPhase 0合否項目から除外した。USB切断、software process再起動／pair境界中断、接続順・port確認、二台identity、empty spool、1/10/100 pairは残る。`power-off` CLIとfake安全contractは回帰保護として保持する。
- 2026-08-09に第三者向け[Phase 0 二台カメラ・ショーケース](PHASE0_SHOWCASE.md)と非破壊検証scriptを追加した。SDK有効／なしのRelease build、CTest各5/5、厳選した匿名証拠のschema・安全フラグ検証に合格し、`SOFTWARE_CONTRACT_READY_HARDWARE_PENDING`を出力した。camera command、撮影、Live View、設定変更、card accessは実行していない。
- Live View停止失敗は撮影前の`Idle → FailedPartial`としてjournalへ永続化し、再起動復元、連打防止、明示的新規撮影準備までのロックを自動試験で確認した。
- 2026-08-10のproduct owner指示をADR-0023として記録し、要件を2.6.0へ更新した。これは一台製品modeの実装開始判断であり、実WPF Camera Agent、実JPEG、canonical original export、実機one-shotの合格証拠ではない。

## Partial

- setting readback: native MAID command-traceとSingle identity-v3対応`sdk-status` process-routing software contractは実装・fresh test済み。identity-v3登録後の実D810 v5再実行、focus値の意味、FileType未広告の扱いは未検証／未確定。
- Live View: standaloneは合格だが、実撮影を含むContinuous Live View handoff（ADR-0030により最大5回、#11）は未実施。
- identity: 旧SDK CAM-A復元証拠はephemeral source ID使用のため無効。SingleCamera CAM-AはWPD digest＋exactly-one current SDK/WPDのidentity-v3を実装し、操作者報告の一台登録はPassした。旧v2 mapへ自動fallbackせず、欠落・破損・digest不一致・0台／複数台をstatus open前に拒否する。二台接続時のSDK Name/Interface v2衝突によりCAM-A/Bを恒久identityで区別できないことと、旧v2 mapの明示migration/invalidationは旧経路の制約として保持する。現在のDual laneでは恒久SDK body identityを採用しない。`HG-0003B`の意思決定はADR-0025のsession-local operator bindingとして解消済みであり、binding core・protocol・WPFはsoftware実装済み、二台実機受入は未完了である。Single identity-v3をDualへ流用しない。
- setup/correction: parameter contractとWI-0022Cのsynthetic measurement seamは合格。proposalは測定値・fixture/profile provenanceを保持し、profile envelope外を拒否し、profileを変更しない。実写A0品質、承認済みDual profile、実機性能は未検証である。
- M3P: requirements 2.6.0のfresh software contractは合格したが、実D810、actual JPEG、実WPF画面操作の統合証拠ではない。旧Dual UI Automationは保持し、screen reader、keyboard/focus、Single実画面walkthroughは残る。
- SingleCamera製品mode: 実D810のCamera Agent撮影、actual JPEG、one-shot、10回p95、100件耐久は合格済み（2026-08-26、ADR-0030以前の実績）。実WPF操作とUI経由fixed-local exportは2026-10-06に一回撮影で合格した（V-1CAM-005、#216）。最大5回の受入系列（#227）、handoff（最大5回、#11）、物理異常系は未検証である。

## Deferred / Waiting

| 項目 | 理由 | 再開条件 |
|---|---|---|
| M1A one-shot、10/10 | 2026-08-26に完了 | one-shot 1/1、10/10、p95承認、Camera Agent経路100/100を実績として維持 |
| M1A fault、handoff | software recoveryは合格。物理USB切断とContinuous Live View handoff（最大5回）は未実施 | 実機操作者の明示確認後に残試験を個別実施（USB抜去は#227、handoffは#11） |
| M1B二台試験 | session-local operator binding、production `CaptureRecoveryOnly` backend、WPF経路、5回runnerは実装済み。100回runnerは未実装で、ADR-0030により今回の受入には使わない。通常`start-reserved-pair`はADR-0028境界によりproduction binding hostでは使用しない | 2026-10-06にone-shotを含む2組を使用（1組目はソフト起因のFailed、2組目はSucceeded）。残り3組とUSB抜去を#227で行う。数え方と合格線はADR-0032（#230、2026-10-07 承認）で確定した。DualCameraの実機受入は未完了 |
| 実M2 | リグ・A0品質契約未承認 | `HG-0001/0002` |
| SingleCamera製品受入 | 実Camera Agent撮影1/10/100とHG-0009は完了。WPF end-to-end one-shotは2026-10-06に合格。最大5回の受入系列とLive View handoffは未完了 | UI操作・export・状態表示を含む最大5回の受入系列（#227）。旧計画の実WPF 100件（#13）はADR-0030で置き換わった |
| 配布 | native dependency再配布未承認 | `HG-0005` |

90 payloadを検出した旧カード状態は履歴として保持する。2026-08-26の実績は別の専用empty spoolで取得しており、旧カードへ削除・formatを行ったことを意味しない。今後もUSB切断、電源操作、保存障害は自動実行しない。

## 次の安全な順番

2026-10-07時点の作業順の正本はロードマップ #235。以下はADR-0030に合わせた要約。

1. ADR-0032（#230、2026-10-07 承認）で試行の数え方・USB抜去・保存の合格線を所有者が確定した
2. SingleCameraは、WPFからの最大5回の受入系列（保存の確認を含む）とUSB抜去を#227で、Continuous Live View handoff（最大5回）を#11で行う
3. DualCameraは、CaptureRecoveryOnlyの残り3組（各組で原画像 2 枚の書き出しを含む。書き出しは #226 で main に着地済み、実機確認は未実施）とUSB抜去を#227で行い、全時間値・最大値を記録する。p95は5標本の記述統計に限る
4. 100回runnerと100組の耐久はADR-0030により今回の範囲外とする
5. `HG-0001/0002`承認後にA0合成品質へ進む。CaptureRecoveryOnlyの合格を合成品質の合格に代用しない

## Human gates

- `HG-0001`: A0品質・補正上限
- `HG-0002`: 最終リグ・光学条件
- `HG-0005`: Nikon SDK/OpenCV等の再配布

解消済み:

- `HG-0003B`: ADR-0025のsession-local operator bindingとして解消。software実装済み、二台実機受入は別途未完了
- `HG-0009`: 2026-08-26にSingleCamera実測p95 `14.643秒`を承認

## 証拠の読み方

- `Pass`: その項目の明示contractをfresh evidenceで満たす。
- `Partial`: 一部のlayerまたは環境だけが確認済み。
- `Unverified`: 必要な実行証拠がない。
- software-only、旧one-camera、simulatedの結果を実WPF Camera Agent、一台製品受入、二台実機、A0品質、MVP受入へ読み替えない。
