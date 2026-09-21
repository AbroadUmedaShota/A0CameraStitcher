# 二プロセス Live View 試作（software-only）

## 結論

本試作は、統括と CAM-A/CAM-B の二 worker 間で一時フレームを IPC するだけである。Nikon SDK/WPD を起動せず、実機 Live View、撮影、カード操作、設定変更を行わない。SDK の同一 Module/同一 Source の制約を回避・緩和する実装ではない。

## 境界

- 統括が generation と capability token を一回だけ発行し、継承した統括所有ハンドルを持つ child だけを起動する。単独の command line 起動は pipe を開く前に拒否する。worker は自分の別 pipe へ、同じ generation/token を含む最大 256 KiB の最新フレームだけを返す。
- token は worker 起動専用で、camera Source ID・候補 ordinal・物理個体 ID を表さない。プロセス間で SDK Source ID が安定または同一とは仮定しない。
- 片 worker の終了・通信故障は統括を terminal にする。再接続、worker 再起動、フレーム再送、撮影 IPC の送信はしない。
- coordinator は capture/WPD への遷移を常に拒否する。Live View 停止・SDK source/module 解放の実証を worker process 終了で代用しない。
- IPC の `frame` は偽transportのバイト列であり、画面・ログとも実機 Live View と表示してはならない。

## Open 前フィルター

現行 `NikonSdkTransport::BeginDualSession` は `D810SourceIds` が各候補を Open して機種を判定する。この処理を worker に流用すると、worker 担当外の Source を Open する。`SelectAssignedSourceBeforeOpen` は列挙済みの worker-local assigned Source だけを選択し、重複・未検出なら Open 計画を返さない。

これは実機 identity 解決ではない。将来の実機試験では、各 worker が自分の session-local 映像で担当を確認し、worker-local Source を得る方法、二つの物理個体に対する対応、driver/module 共有可否を別途証明するまで実機経路を遮断する。

## 実機 PoC（未実行）

別承認後でも最大 5 回。各回は、二 worker の SDK 初期化、Source Open trace（担当外 Open 0）、両 Live View の停止、source/module 解放、全体 lease が保持されたこと、capture/WPD 未遷移を記録する。今回の試作はその承認・実行を含まない。
