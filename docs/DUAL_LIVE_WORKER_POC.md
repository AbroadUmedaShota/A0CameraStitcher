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

## Open 前フィルター

現行 `NikonSdkTransport::BeginDualSession` は `D810SourceIds` が各候補を Open して機種を判定する。この処理を worker に流用すると、worker 担当外の Source を Open する。`SelectAssignedSourceBeforeOpen` は列挙済みの worker-local assigned Source だけを選択し、重複・未検出なら Open 計画を返さない。

これは実機 identity 解決ではない。将来の実機試験では、各 worker が自分の session-local 映像で担当を確認し、worker-local Source を得る方法、二つの物理個体に対する対応、driver/module 共有可否を別途証明するまで実機経路を遮断する。

## 実機 PoC（未実行）

別承認後でも最大 5 回。各回は、二 worker の SDK 初期化、Source Open trace（担当外 Open 0）、両 Live View の停止、source/module 解放、全体 lease が保持されたこと、capture/WPD 未遷移を記録する。今回の試作はその承認・実行を含まない。
