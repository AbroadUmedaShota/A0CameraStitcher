# 二プロセス Live View 試作（software-only）

## 結論

本試作は、統括と CAM-A/CAM-B の二 worker 間で模擬フレームを IPC する試験専用の実装である。Nikon SDK/WPDをリンクせず、実機 Live View、撮影、カード操作、設定変更を行わない。SDK の同一 Module/同一 Source の制約を回避・緩和する実装ではない。製品UI・実機Agent・既存leaseへの組込みは未実施。

## 境界

- テスト統括が generation と worker 別 token を渡し、限定継承した統括プロセス監視ハンドルを持つ child だけを起動する。監視handleなしの command line 起動は pipe を開く前に拒否する。worker は専用の常駐 pipe へ generation/token/alias/sequence/偽Open trace/模擬payload を返し、統括は応答を検証して最新一枚のmailboxへ渡す。tokenはテスト固定値であり、本番認証・悪意ある同一ユーザーprocessへの耐性の証明ではない。
- token は worker 起動専用で、camera Source ID・候補 ordinal・物理個体 ID を表さない。プロセス間で SDK Source ID が安定または同一とは仮定しない。
- 統括の観測処理は実worker process handleを確認し、片worker終了時にterminalへ移して残るworkerへ明示STOPを送る。受信失敗でもterminalとする。各操作前後の観測であり、本番用の常時監視serviceは未実装。再接続、worker再起動、フレーム再送、撮影IPC送信はしない。
- coordinator は capture/WPD への遷移を常に拒否する。Live View 停止・SDK source/module 解放の実証を worker process 終了で代用しない。
- IPC の `frame` は偽transportのバイト列であり、画面・ログとも実機 Live View と表示してはならない。
- 試作 wire は最大1 KiB、各I/O待機の期限は1500 ms。取消後も同じ時間内にcompletionが収束しなければ試験processを終了し、未完了OVERLAPPEDを解放しない。試験全体のCTest期限は10秒。これは偽payload専用であり、256 KiBの実フレーム伝送、SDK撮影の180秒watchdog、実機通信性能の証明ではない。
- 子processは試験専用Jobで管理し、失敗時も外側runner終了時に回収する。親死亡検証は別の監視専用workerではなく、通常と同一のIPC workerへ注入する。強制終了の対象は試験で生成した偽processだけであり、実カメラprocessへの強制終了方針ではない。
- pipe断が親process終了通知より先に観測される場合があるため、読取り失敗後に最大1500 msだけ親終了を観測して原因を判定する。これは追加I/O・再接続ではない。各I/O期限と終了収束期限は別で、合計が1500 ms以内という主張ではない。

## 2026-09-22 検証記録

- base: `7c5c2c6`、初期試作: `183a3cc`。変更は隔離branch `codex/dual-live-worker-poc-20260922` のみ。本体アプリ・SDK/WPDは未実行。
- 初回の狭いIPC試験はPASSだったが、独立レビューでwire受信とCoordinatorの未結線、実process死亡監視不足などを発見。初回PASSを完成証拠としない。
- focused CTest実行は計5回（初回PASS、途中3回FAIL、最終PASS）。途中失敗は応答喪失、および親終了通知とpipe断・Job終了反映の競合。条件を無変更で反復せず、原因を修正して実行した。上限到達後の追加試験なし。
- 最終Release build成功。`ctest --test-dir build/dual-live-worker-poc -C Release -R '^dual_live_worker_poc_contracts$' --output-on-failure`: **1/1 PASS、5.80秒、exit 0**。ローカル詳細ログは `build/dual-live-worker-poc/Testing/Temporary/LastTest.log`（ignored）。
- 確認項目: 同時常駐するA/Bの複数wire応答→個別token/alias/generation/sequence/偽Open traceの検証→最新mailbox、古いsequenceと別worker応答の拒否、実worker終了からterminal/兄弟STOP、実parent終了から同じworkerのexit 7、通信応答失敗からterminal/兄弟STOP、wire上限超過、不正sequence/token、無通信期限、監視handleなし起動、偽Source選別、Job内残存child 0。
- 独立read-onlyレビューで重大指摘の修正を確認。ただし長期安定性・本番認証・実画像の転送・実機安全性の受入ではない。

## 次の設計条件（実機経路は無効のまま）

1. worker-local Sourceと物理カメラA/Bの対応をどう確定し、再起動・再列挙で失効させるかを設計する。番号や列挙順のprocess間一致は仮定しない。
2. 現行のoperator-session-wide leaseを統括が保持し、その配下workerに限定委譲する契約を別途設計・検証する。試作でleaseを外したり、本番を複数session可に変えたりしない。
3. SDK/driver共有の実現可能性と両側stop/close証拠が得られるまで本体へ接続しない。worker process終了はSDK解放証明ではない。

## 後続Goal: 個体割当と限定委譲（2026-09-22）

本人の「後続の作業についてもgoal化して進めてください」によりGoalを作成。基点 `33bed76`、branch `codex/dual-live-worker-poc-20260922`。実機なしの設計・契約実装・レビュー・限定検証を進める。SDK/WPD起動、撮影、設定変更、AOPC-22-NOTE、本番有効化、merge/公開は含まない。

### 今回の契約モデル

`dual_live_session_contract.hpp` と専用 `dual_live_session_contracts` は純粋なoffline契約モデルであり、前段の二process IPC試作や実HardwareProcessLeaseへまだ結線していない。

- worker内でのみSource番号を保持し、instance/epochが異なる選択、未検出・重複候補、再Openをfake transport callback前に拒否。二workerのSource番号が同じでも異なっても物理個体対応を推論しない。
- binding receiptは模擬カメラの独立したfixture identityであり、SDK Source/serialの証明ではない。同じsynthetic body、worker instance、receiptの二重割当を拒否する。実環境にはその対応入力が未確立のため、モデルのReadyは`SimulatedReady`でしかない。
- 委譲grantはcontroller/epoch/alias/worker instance/receipt/個別capability/発行時刻/期限に限定。期限到達、時刻後退、連番再送、別workerの借用、lease不在/abandoned、親終了・topology変更で全grantを失効させる。後でleaseが得られても旧sessionは復活しない。
- quiesce開始でpreview権限を止め、両workerのpreview停止・Source close・Module closeの明示receiptが揃うまでClosedにしない。process終了だけはclose証拠にしない。Closed後は旧割当を再利用せず、遅延通知はClosedを破壊しない。
- `RealSdk`、`Wpd`、`Capture`は常時拒否。lease観測・時刻・close receiptはsimulationから注入している値で、実mutex所有、OS process認証、実SDK teardownの証拠ではない。

### 検証・現在地

- 新規モデルだけをRelease buildし、focused CTestは2回、いずれも1/1 PASS（最終0.74秒）。前段の5回IPC試験は再実行していない。
- fake Open/frame callbackと認可を接続し、親失効後の両worker callback停止を確認。独立レビューの発行時刻・Closed遅延通知に関する指摘は修正し再試験した。
- コマンド: `cmake --build build/dual-live-worker-poc --config Release --target a0_dual_live_session_contract_tests`、`ctest --test-dir build/dual-live-worker-poc -C Release -R '^dual_live_session_contracts$' --output-on-failure`。
- **Goalは進行中**。次は、この認可契約を試験専用IPCへ接続し、実カメラと無関係な専用名のOS排他で統括だけが所有することを検証する。モデル単体PASSを実processの権限委譲完了へ読み替えない。前段テストを根拠なく再実行せず、後続変更に必要な検証の反復は最大5回以内（現在2回）。

### 将来の排他・個体対応設計

実機経路へ接続する場合、統括が現在のoperator-session-wide leaseを唯一保持し、workerへ渡すのは所有そのものではなく親の生存期間に限ったpreview権限だけとする。子workerは独立lease取得/解放、leaseなし起動、親喪失後の操作を行わない。実HardwareProcessLeaseの既定名・既存一台制限は今回変更しない。

worker-local映像の操作者確認は候補だが、二workerが別物理bodyを開いた証拠は別途必要。親のSource token移送・列挙順対応・同一番号一致判定は禁止。担当外カメラをOpenしない選択方法が確立しない間は実機workerを有効化しない。全module close後は新generation・再割当が必要で、自動再接続しない。

### 別承認の実機PoC案（未実行）

まず資料と実装レビューで対象外Open防止・個体選択方法・既存排他との整合を確認する。成立しない場合は実機試験へ進まない。承認時に候補SHA、PC、対象カメラ、操作・期限・回数を固定する。以下は最大5回の上限案であり、必要な回だけ行い失敗補充しない。

1. 各workerの単体選択・映像確認・正常停止（単体A）。
2. 同じ確認を単体Bで実施。過去の撮影成功だけで新workerの合格にしない。
3. 二workerが別物理bodyを同時保持できるか、担当外Openなしで確認する。
4. 両側の新しい映像更新と、明示停止→両Source/Module closeを確認する。
5. 別に指定した正常終了または人手の接続変化シナリオで失効・両側停止・再利用禁止を確認する。故障注入内容は実行前に明示する。

全回で撮影・カード操作・WPD・カメラ設定変更をしない。識別不明、対象外Open、片側停止/close未確認、電力/温度異常は中止し、再試行・worker再起動・サービス操作・強制killを行わない。実機SDK/WPD非重複や撮影handoffは、このpreview-only試験の合格とは別工程とする。

## Open 前フィルター

現行 `NikonSdkTransport::BeginDualSession` は `D810SourceIds` が各候補を Open して機種を判定する。この処理を worker に流用すると、worker 担当外の Source を Open する。`SelectAssignedSourceBeforeOpen` は列挙済みの worker-local assigned Source だけを選択し、重複・未検出なら Open 計画を返さない。

これは実機 identity 解決ではない。将来の実機試験では、各 worker が自分の session-local 映像で担当を確認し、worker-local Source を得る方法、二つの物理個体に対する対応、driver/module 共有可否を別途証明するまで実機経路を遮断する。

## 実機 PoC（未実行）

別承認後でも最大 5 回。各回は、二 worker の SDK 初期化、Source Open trace（担当外 Open 0）、両 Live View の停止、source/module 解放、全体 lease が保持されたこと、capture/WPD 未遷移を記録する。今回の試作はその承認・実行を含まない。
