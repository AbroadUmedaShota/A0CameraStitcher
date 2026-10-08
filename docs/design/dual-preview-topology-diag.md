# 二worker試作の機器イベント計装と委譲 marker 回復 — 設計（手順 0）

状態: 設計確定・実装済み（2026-10-05、A 実装は未 commit→commit 予定。相違点は補遺を参照）（2026-10-05）
対象: software-only 作業 A（機器イベント計装・分類の分割・journal・worker exit code）と C（監査付き回復コマンド）。B（lease 取得時に全 session の marker を走査して拒否）は別途実装中で、本書は B が入った状態を前提にする
正本: 本書（計装の表・応答封筒 v2・journal 語彙・回復コマンドの lease の扱い）。run の経緯は `docs/DUAL_LIVE_WORKER_POC.md`、判断の記録は `docs/DECISIONS.md`
実装担当: A は Coder A、C は Coder C。B の実装者と C の実装者が同じ `hardware_process_lease.cpp` を触るため、C は B の着地後に着手する（12 節）
範囲外: 選択規則（fail-closed）の変更、F0〜F6 の修正、実機 run、run-04／run-05 の marker への実行

## 0. 前提と置いた仮定

- run-05 の失敗は `worker_selection_invalidated`（worker 0 の select、`CheckInventory` 内）。原因は推定で、機器イベントの種別・回数・時点は journal に残っていない（`DUAL_LIVE_WORKER_POC.md` run-05 節）
- 実機 preview 枠は 5/5 を消費済み。A の計装でデータを取るには、別の本人判断で新しい実機 run を承認する必要がある（※要確認）
- 親（`A0CameraStitcher.PreviewCommissioning`）と worker（`A0CameraStitcher.PreviewWorker`）は同じ commit・同じビルドディレクトリから作り、実行前に両 exe の SHA-256 を記録する。実行時の hash 照合はしない（worker は親 exe と同じフォルダから起動される）
- 性能の目標値（※仮定）: 計数は 1 イベントあたり定数時間で、callback 内でメモリを確保しない。新しい待ちは足さない。操作別の予算表（`preview_worker_timing.hpp`）は変えない。非 frame 応答は親の上限 4096 byte に収める
- データ整合性: journal は control thread だけが書く単一 writer で、行の順序は `sequence` で決まる。応答の `diag` は応答 1 通の中で一貫したスナップショットとする。C の「marker 削除」と「監査記録」は原子的にできないため、意図記録 → 削除 → 結果記録の順で、途中で止まった状態を人が判定できるようにする

## 1. 推奨案

### A（計装）

1. worker の SDK transport（`NikonSdkTransport::Impl`）に計数器 `WorkerTopologyCounters` を持たせ、`ModuleEventProc` で AddChild／RemoveChild を数える。数える区間は OpenModule 中／WaitForSourceIds 中／snapshot 後の 3 つで、close の合図で凍結する
2. 18 個の数値を worker の全応答に `diag` として載せる。封筒は要求・応答とも schema `a0.preview-worker.v2` に上げる
3. 親は `diag` を厳密に検査し、形が違えば `worker_reply_invalid`（ACK なし・隔離維持）とする。値は journal に数値で書くだけで、判定には使わない
4. `CheckInventory` 由来の分類を `worker_selection_inventory_changed`／`worker_selection_topology_event` に分ける
5. journal に `preview_requested` と `topo_block_*` を追記する。既存の行と順序は変えない
6. worker main の `catch (...)` を exit code 4 に分ける

理由:

1. 既存スタックとの親和性: journal は「固定語彙と数値だけ」の方針で、ID も自由文も持たない。18 個の固定数値ならこの方針にそのまま乗る
2. 開発速度: 親 `Exchange` → `PreviewWorkerFailureObservation` → `RecordCloseOutcomeToJournal` の経路がすでにあり、`diag` はその上に 1 フィールド足すだけで済む
3. スケーラビリティ: 固定 18 値で応答が約 0.5 KB 増えるだけ（※推定、4.5 節）。イベント列を返す案は件数に比例して増え、phase0 の実機記録（`run-1789528249365-1`）では Source 側の AddChild が 1 run で 785 件届いている
4. 運用コスト: 記録は親の journal 1 本に集まる。worker 側で別ファイルを書くと、隔離中に `Sleep(INFINITE)` で止まる worker の書きかけファイルを突き合わせる手間が出る
5. 版ずれの安全性: 要求も v2 に上げると、古い worker は最初の要求を SDK に触る前に `worker_authority` で拒否する。応答だけ上げる案では、古い worker が enumerate で SDK を読み込んでから親に拒否される

### C（回復コマンド）

新しい CLI `A0CameraStitcher.MarkerRecovery` を作る。camera-control の mutex は回復関数の内側だけで握り、`HardwareProcessLease` には手を入れない。dry-run と実行の 2 段階に分け、実行時は排他ハンドルで再照合してから `FileDispositionInfo` で削除し、追記専用の監査記録を残す。詳細は 9 節。

### 代替案と不採用理由

| 観点 | 案A 応答封筒に diag（採用） | 案B worker が自前の journal を書く | 案C イベント列を ID の hash 付きで返す |
|---|---|---|---|
| 開発速度 | 速い。既存の parse と journal 経路に乗る | 遅い。worker 側に書込み器と後始末が要る | 中。可変長の parse と上限設計が要る |
| 運用コスト | 低。記録は親の journal 1 本 | 中。2 本を時刻で突き合わせる | 中。run ごとに行数が変わる |
| スケーラビリティ | 固定 18 値、約 0.5 KB（※推定） | worker 数に比例してファイルが増える | イベント数に比例し、4096 byte 上限に当たり得る |
| 学習コスト | 低 | 中 | 中 |
| 既存スタックとの親和性 | 高。固定語彙・数値のみの方針どおり | 低。隔離中の worker に書きかけファイルが残る | 低。hash でも ID は識別子で、識別子を残さない方針に反する |

- 失敗応答にだけ `diag` を載せる案: 不採用。worker 1 の close 応答（SDK を読み込んでいない基準値）と成功 run の累計も要る。応答の形が status で変わると、親の「形が完全一致」という検査が複雑になる
- 応答だけ v2 にする案: 不採用（理由 5）
- 分類を「集合一致かつ `!valid_` なら topology_event」とだけ決める案: 不採用。例外や先行の拒否で無効になった selection まで topology_event と誤って書くため、無効化の原因を持たせる（5 節）

## 2. 拡張点と、拡張しないと決めた箇所

- 拡張点: `kPreviewTopologyDiagFields`（キーと journal 名の表）を正本 1 か所にする。項目を足すときは表・`static_assert`・試験の独立集合を同時に変える。項目追加は封筒の互換を壊すため schema の版も上げる
- 拡張しない: 親による `diag` の値域検査（命令コードの範囲、0〜2 の三値など）はしない。形（18 キー・uint32）だけを検査し、値域は worker 側の単体試験で守る。親が値の意味に依存すると、診断項目の修正が親の判定まで波及するため
- 拡張しない: 成功応答（select／start／frame）ごとの journal 記録。close 応答の値が累計なので足りる

## 3. topology カウンタ（正本の表）

### 3.1 数える場所と区間

数えるのは worker の `NikonSdkTransport::Impl::ModuleEventProc` だけ。SourceEventProc（カード・画像の Item）は対象外。

| 区間 | 始まり | 終わり |
|---|---|---|
| Idle（数えない） | 起動時 | `BeginWorkerPreviewSelection` の入口 |
| Opening | `BeginWorkerPreviewSelection` の入口で `Reset` | `OpenModule` の復帰 |
| InventoryWait | `OpenModule` の復帰 | `WaitForSourceIds` の復帰 |
| PostSnapshot | `Snapshot`（selection を作る直前） | 凍結 |
| Frozen（以後一切変えない） | dispatcher の `CloseForShutdown` 入口で `Mark(close)`、または transport の `Close()`／`EndDualSession()` 入口で `Freeze()` | なし |

- `Reset` は enumerate 1 回につき 1 回だけ（dispatcher は Fresh 段階でしか enumerate を受けない）。Frozen 状態では `Reset` も何もしない
- close 中の Pump で届くイベントは数えない。Source／Module の後始末で出る通知は原因の切り分けに使えず、失敗時の値を薄めるため
- `OpenModule` が例外で終わった場合も、そこまでの Opening の値は失敗応答に載る

### 3.2 18 項目

すべて `std::uint32_t`。JSON キーは応答の `diag` 内の名前、journal 名は `PreviewRunJournal` の event 名（`[a-z_]`、48 字以内、数字なし）。

| # | JSON キー | journal 名 | 意味 | 数える時点 | 単位・値 |
|---|---|---|---|---|---|
| 1 | `openAdd` | `topo_open_add` | 届いた AddChild の件数。EnumChildren が出す通知を含む | Opening | 件 |
| 2 | `openRemove` | `topo_open_remove` | 届いた RemoveChild の件数 | Opening | 件 |
| 3 | `inventoryAdd` | `topo_inventory_add` | 届いた AddChild の件数 | InventoryWait | 件 |
| 4 | `inventoryRemove` | `topo_inventory_remove` | 届いた RemoveChild の件数 | InventoryWait | 件 |
| 5 | `inventoryPumps` | `topo_inventory_pumps` | `WaitForSourceIds` が呼んだ Pump の回数 | InventoryWait | 回 |
| 6 | `snapshotChildren` | `topo_snapshot_children` | snapshot を決めた最後の Children 取得が返した ID の個数（`module_sources_` と合わせる前） | Snapshot 時に 1 回 | 個 |
| 7 | `snapshotEventIds` | `topo_snapshot_event_ids` | snapshot 時点の `module_sources_`（イベントで知った ID の集合）の個数 | Snapshot 時に 1 回 | 個 |
| 8 | `snapshotMs` | `topo_snapshot_ms` | `Reset` から `Snapshot` までの経過 | Snapshot 時に 1 回 | ms |
| 9 | `postAddKnown` | `topo_post_add_known` | snapshot 集合に含まれる ID の AddChild | PostSnapshot（各命令中） | 件 |
| 10 | `postAddUnknown` | `topo_post_add_unknown` | snapshot 集合に含まれない ID の AddChild | PostSnapshot | 件 |
| 11 | `postRemoveKnown` | `topo_post_remove_known` | snapshot 集合に含まれる ID の RemoveChild | PostSnapshot | 件 |
| 12 | `postRemoveUnknown` | `topo_post_remove_unknown` | snapshot 集合に含まれない ID の RemoveChild | PostSnapshot | 件 |
| 13 | `postAddKnownDistinct` | `topo_post_add_known_distinct` | AddChild を 1 回以上受けた既知 ID の種類数 | PostSnapshot | 個（0〜2） |
| 14 | `postFirstEventOp` | `topo_post_first_event_op` | snapshot 後の最初の Add／Remove が届いたときの命令コード（3.3 節） | 最初の 1 件で確定 | コード |
| 15 | `postFirstEventMs` | `topo_post_first_event_ms` | `Snapshot` から最初の Add／Remove までの経過。14 が 0 のときは 0 | 最初の 1 件で確定 | ms |
| 16 | `checkValidBeforePump` | `topo_check_valid_before_pump` | 最後の在庫確認で、Pump の前に selection が有効だったか | 在庫確認ごとに上書き。拒否された確認の値が最後に残る | 0 未実施／1 有効／2 無効 |
| 17 | `checkSetEqual` | `topo_check_set_equal` | 最後の在庫確認で、現在集合が snapshot 集合と一致したか | 同上 | 0 未実施／1 一致／2 不一致 |
| 18 | `checkCurrentCount` | `topo_check_current_count` | 最後の在庫確認での現在集合（Children ∪ `module_sources_`）の個数 | 同上 | 個 |

- 「在庫確認」は `WorkerPreviewInventory` の 1 回の呼び出し。select／start／frame／suspend／resume の入口と、`StartLiveView`・`ReadLiveViewFrame` の内部から呼ばれる。Pump または Children が例外で終わった確認は記録しない（16〜18 は前回の値のまま）
- 16・17 を三値にしたのは、0 を「確認していない」に割り当て、「確認して無効だった」と区別するため
- 既存の分類表（62 語）・operation 表（`failure_operation_*`）・既存の固定イベントとの重複はない。`topo_` と `preview_requested` が src／tests に出現しないことを確認した（`grep -rn 'preview_requested\|topo_' src tests --include=*.cpp --include=*.hpp` の出力 0 行、2026-10-05）

### 3.3 命令コード（`postFirstEventOp`）

| コード | 意味 |
|---|---|
| 0 | snapshot 後のイベントなし |
| 1 | enumerate（現行コードでは snapshot 後に SDK 呼び出しがないため出ない。出たらコードの変更かスレッド前提の破れ） |
| 2 | select |
| 3 | start |
| 4 | frame |
| 5 | suspend（`StopLiveView` と `SuspendSelectedWorkerPreview` の両方） |
| 6 | resume |
| 7 | 欠番。close は凍結の合図なので報告されない |
| 8 | 命令の外（dispatcher が idle の間）または transport の所有スレッド以外で届いた。スレッド前提が破れている印 |

命令コードは dispatcher が知っているので、dispatcher が `MarkWorkerTopologyOperation(op)` で transport に伝える（3.7 節）。transport の公開メソッドから推測しない。`StopLiveView` は suspend と close の両方から呼ばれ、メソッド名では区別できないため。

### 3.4 既知／未知 ID の判定

- snapshot 集合 S は `WaitForSourceIds` が返した ID 集合で、`WorkerPreviewSelection` の inventory と同じもの。`Snapshot` は S を昇順に並べて最大 2 個を固定長配列に保持する（確保なし）
- 既知 = イベントの ID が S に含まれる。未知 = 含まれない。未知の ID 自体は保持しない
- `postAddKnownDistinct` は S の添字ごとのビット（2 bit）で数える
- S が 2 個でない場合、直後の selection 構築が `worker_inventory_not_pair` で失敗し、enumerate は失敗応答になる。そのため PostSnapshot のイベントは発生しない
- ID は worker プロセスのメモリ上だけに置く。`diag`・journal・画面・標準出力・例外文のどれにも書かない。`Values()` は 18 個の数しか返さない

### 3.5 飽和・時計

- 件数・回数・個数はすべて飽和加算。4294967295 に達したらそれ以上増やさない
- ms は `std::chrono::steady_clock` の差を `duration_cast<milliseconds>` で切り捨て、4294967295 で頭打ちにする。負の差は 0
- 180 秒の試作で飽和に達することはないが、規則として持つ

### 3.6 callback スレッドの前提（※要確認）

- 前提: `ModuleEventProc` は、transport の所有スレッドが MAID を呼んでいる最中（Pump の `kNkMAIDCommand_Async`、または Children 取得などの完了待ち）に、同じスレッドで呼ばれる
- 根拠はコードだけ: 既存の `ModuleEventProc` は `module_sources_`（`std::set`）を lock なしで書き換えており、`RunCompleted` には単一スレッドの debug assert がある。既存コードがすでにこの前提に立っている。SDK 資料でも実機でも、配送スレッドは確認していない
- 計数器は既存と同じく lock なしの通常変数にする。前提が破れていれば `module_sources_` も競合するので、計数器だけ atomic にしても transport 全体の安全は戻らない
- 検出: snapshot 後の最初のイベントが所有スレッド以外、または dispatcher が idle の間に届いたら `postFirstEventOp = 8` にする。所有スレッドの ID は `Reset` の時点で `GetCurrentThreadId()` を記録し、`ModuleEventProc` で比較する。最初の 1 件しか見ないため、命令中の別スレッド配送の 2 件目以降は検出できない
- 8 が一度でも出た run は、他の値を解釈する前に F0（8 節）を扱う

### 3.7 実装の置き場所

新規ヘッダ 2 つ（どちらも SDK・Windows に依存しない header-only）:

```cpp
// src/phase0/include/a0/phase0/preview_topology_diag.hpp  （親と worker の共有契約）
namespace a0::phase0::experimental {
inline constexpr std::size_t kPreviewTopologyDiagFieldCount = 18;
struct PreviewTopologyDiagField { std::string_view json_key; std::string_view journal_event; };
inline constexpr std::array<PreviewTopologyDiagField, kPreviewTopologyDiagFieldCount>
    kPreviewTopologyDiagFields{{ {"openAdd", "topo_open_add"}, /* 3.2 節の順 */ }};
struct PreviewTopologyDiag { std::array<std::uint32_t, kPreviewTopologyDiagFieldCount> values{}; };
enum class PreviewTopologyOperation : std::uint32_t {
    idle = 0, enumerate = 1, select = 2, start = 3, frame = 4, suspend = 5, resume = 6, close = 7 };
inline constexpr std::uint32_t kTopologyFirstEventOutsideCommand = 8;
std::optional<PreviewTopologyOperation> TopologyOperationOf(std::string_view operation) noexcept;
std::string SerializePreviewTopologyDiag(const PreviewTopologyDiag&);        // 表の順で {"openAdd":0,...}
bool ParseUint32Lexeme(std::string_view lexeme, std::uint32_t& out) noexcept; // 4.3 節
template<class Failure> PreviewTopologyDiag ParsePreviewTopologyDiag(const JsonValue& diag);
}

// src/phase0/include/a0/phase0/worker_topology_counters.hpp  （worker 側の状態機械）
class WorkerTopologyCounters final {
public:
    using Clock = std::chrono::steady_clock;
    void Reset(Clock::time_point now) noexcept;          // Idle -> Opening（Frozen なら何もしない）
    void BeginInventoryWait() noexcept;                  // Opening -> InventoryWait
    void CountInventoryPump() noexcept;
    void Snapshot(std::uint32_t children, std::uint32_t event_ids,
                  std::span<const std::uint32_t> ids, Clock::time_point now) noexcept; // -> PostSnapshot
    void Mark(PreviewTopologyOperation op) noexcept;     // close なら Freeze
    void Observe(bool added, std::uint32_t id, Clock::time_point now, bool on_owner_thread) noexcept;
    void RecordCheck(bool valid_before_pump, bool set_equal, std::size_t current_count) noexcept;
    void Freeze() noexcept;
    PreviewTopologyDiag Values() const noexcept;
};
```

`Observe` の規則（Frozen・Idle では何もしない）:

```text
Opening        : added ? ++openAdd : ++openRemove
InventoryWait  : added ? ++inventoryAdd : ++inventoryRemove
PostSnapshot   : k = S での添字（なければ未知）
                 added ? (既知 ? ++postAddKnown, mask |= bit(k) : ++postAddUnknown)
                       : (既知 ? ++postRemoveKnown : ++postRemoveUnknown)
                 最初の 1 件なら firstOp = (!on_owner_thread || current_op == idle) ? 8 : current_op
                                firstMs = ClampMs(now - snapshot_time)
```

`RecordCheck` は PostSnapshot かつ凍結前だけ値を書く。

transport（SDK 有効ビルドの `Impl`）の結線:

- `BeginWorkerPreviewSelection`: `ClaimSession` の後に所有スレッド ID を記録して `Reset`。`OpenModule` の後に `BeginInventoryWait`。`WaitForSourceIds` に任意の出力引数 `SourceIdWaitTrace*`（既定 nullptr、他の呼び出し元は変えない）を足し、Pump ごとに `CountInventoryPump`、最後の Children の個数を返す。selection 構築の直前に `Snapshot(last_children, module_sources_.size(), ids, now)`
- `ModuleEventProc`: 既存の処理の前に `self->topology_.Observe(added, id, steady_clock::now(), GetCurrentThreadId() == self->topology_owner_thread_)`。AddChild／RemoveChild 以外では呼ばない
- `WorkerPreviewInventory`: Pump の前に `valid_before = worker_selection_->Valid()`。現在集合を作った後に `RecordCheck(valid_before, worker_selection_->SameInventory(current), current.size())`、その後に既存の `CheckInventory`
- `Close()`・`EndDualSession()` の入口で `Freeze()`
- 公開メソッド: `PreviewTopologyDiag WorkerTopologyDiagnostics() const noexcept` と `void MarkWorkerTopologyOperation(PreviewTopologyOperation) noexcept`。SDK なし（gated）ビルドでは全 0 を返し、何もしない。`ThrowGated()` を呼ばない（dispatcher が毎命令で呼ぶため）

dispatcher（`WorkerPreviewDispatcher`。テンプレートの Transport 要件に上の 2 メソッドを足す）:

- 認証済みの命令の operation が決まった直後、close 以外は `Mark(op)`。`Handle` を抜けるときに RAII で `Mark(idle)` に戻す（Frozen なら効かない）
- `CloseForShutdown()` の先頭で `Mark(close)`（凍結）
- 失敗の catch では、`CloseForShutdown()` を呼ぶ前に `WorkerTopologyDiagnostics()` を取り、その値を応答に載せる
- 成功・close の応答は、応答を組み立てる時点の値を載せる（close は凍結済みの値になる）

## 4. 応答封筒 v2

### 4.1 形

要求（親 → worker）: schema を `a0.preview-worker.v2` に変える。フィールドは 6 個のまま（schema, epoch, capability, sequence, operation, candidate）。

応答（worker → 親）: 7 フィールド。

```json
{"schema":"a0.preview-worker.v2","epoch":"…","workerPid":1234,"sequence":2,
 "status":"failed","payload":{"error":"worker_selection_topology_event","close":{…}},
 "diag":{"openAdd":2,"openRemove":0,"inventoryAdd":2,"inventoryRemove":0,"inventoryPumps":1,
         "snapshotChildren":2,"snapshotEventIds":2,"snapshotMs":468,"postAddKnown":2,
         "postAddUnknown":0,"postRemoveKnown":0,"postRemoveUnknown":0,"postAddKnownDistinct":2,
         "postFirstEventOp":2,"postFirstEventMs":19950,"checkValidBeforePump":1,
         "checkSetEqual":1,"checkCurrentCount":2}}
```

（値は形の例で、実測ではない）

- status が ok／failed／closed／quarantined のどれでも `diag` は必須
- `diag` のキーは 3.2 節の 18 個ちょうど。順序は意味を持たない（worker は表の順で出す）

### 4.2 worker 側

- dispatcher は要求の schema が `a0.preview-worker.v2` 以外なら `worker_authority` で拒否する。この拒否は SDK に触る前に起き（`sdk_attempted_` が false のまま）、応答は v2 の failed で `diag` は全 0
- `Reply(status, payload, diag)` で 7 番目のフィールドを付ける

### 4.3 親 `ParsePreviewWorkerReply` の拒否規則

次のどれか 1 つでも当てはまれば `TransportError("worker_reply_invalid")` を投げる。

1. 封筒が 7 フィールドでない（v1 の 6 フィールド、`diag` 欠落、余分なフィールドを含む）
2. `schema` が `a0.preview-worker.v2` でない
3. `diag` が object でない（array・string・number・boolean・null）
4. `diag` のフィールド数が 18 でない、または 18 個のキーのどれかが欠けている（キー名違いは「欠けている」に当たる）。重複キーは JSON parser が拒否する
5. 値が number でない（`"1"`・`true`・`null`・object）
6. 値の字句が非負整数の 10 進表記でない。`ParseUint32Lexeme` の条件: 長さ 1〜10、すべて `0`〜`9`、先頭 0 は `"0"` のときだけ、uint64 に変換して 4294967295 以下。`-1`・`1.0`・`1e3`・`1E3`・`4294967296` はすべて拒否する。parser は符号・小数・指数を number の字句として受け付けるので、字句で判定する
7. 既存の検査（epoch・workerPid・sequence・status と operation の組・payload・close receipt）

拒否後の親の動き（既存の `Exchange` の経路で、新しいコードは要らない）:

- `response_received = true`、`response_validated = false`、分類は `worker_reply_invalid`
- ACK を書かない（ACK は parse 成功の後でしか書かない）
- `delivery_failed` が true のままなので、その子にはそれ以後 close を含め何も送らない
- close 段階の窓は E_fail（`waited_for_worker_cleanup`）、判定は false、委譲 marker は残り、画面は隔離を表示する

### 4.4 同一 commit の前提と版ずれ

| 組み合わせ | 起きること |
|---|---|
| 親 v2・worker v2（同一 commit） | 通常動作 |
| 親 v2・worker v1（古い worker） | worker が最初の要求を schema 不一致で拒否。SDK は読み込まれない。親は v1 応答を拒否して隔離 |
| 親 v1・worker v2（古い親） | worker が v1 要求を `worker_authority` で拒否。SDK は読み込まれない。親は 7 フィールドの v2 応答を拒否して隔離 |

どの不一致も SDK に触る前に止まり、隔離側に倒れる。それでも運用上は両 exe の hash を同じビルドから記録する（0 節）。

### 4.5 サイズ

- `diag` の最大長: キー名の合計 260 字＋1 項目あたり引用符・コロン・値 10 桁・カンマで 14 字 × 18 ＋ 括弧とフィールド名で約 530 byte（※推定、手計算）
- 非 frame 応答は最大でも約 1.5 KB で、親の上限 4096 byte に収まる
- frame 応答は hex 512 KiB ＋ 封筒で、親の上限 512 KiB ＋ 4096 byte に収まる

## 5. 分類の分割（60 → 62 語）

`WorkerPreviewSelection` に無効化の原因と拒否済みの印を持たせる。

```cpp
enum class InvalidationCause { none, topology_event, other };
// ObserveTopology: cause_ == none なら cause_ = topology_event、その後 Invalidate
// Invalidate（外部・例外・Close）: cause_ == none なら cause_ = other
// Reject: rejected_ = true、Invalidate
void CheckInventory(std::vector<std::uint32_t> current) {
    std::sort(current.begin(), current.end());
    if (rejected_)                        Reject("worker_selection_invalidated");       // 拒否後は再分類しない
    if (current != inventory_)            Reject("worker_selection_inventory_changed"); // 集合不一致を優先
    if (cause_ == InvalidationCause::topology_event)
                                          Reject("worker_selection_topology_event");
    if (!valid_)                          Reject("worker_selection_invalidated");       // イベント以外の無効化
}
bool Valid() const noexcept;                                       // 計装用
bool SameInventory(std::vector<std::uint32_t> current) const;      // 計装用（sort して比較）
```

- `Suspend`／`Resume` の内部、close()／open() の後の `if (!valid_)` は `worker_selection_invalidated` のまま変えない。その間に届いたイベントは計装の `postFirstEventOp`（5／6）で読む
- `OpenSelected`／`Suspend`／`Resume` の入口の `CheckInventory` も同じ規則に従う
- 依頼の文面「集合一致で `!valid_` なら topology_event」から 1 点だけ広げた。既存試験の `changed.CheckInventory({71, 97})` → `CheckInventory({71, 83})` のように、集合不一致で拒否された selection の 2 回目の確認は、集合が一致していても `!valid_` になる。文面どおりだと topology_event と書いてしまうが、原因はイベントではない
- 分類表 `kPreviewWorkerFailureCategories` の worker プロトコル層に 2 語を足す（15 → 17、全体 60 → 62）。`worker_selection_invalidated` は残す。過去の journal（run-05 を含む）の読み取り用であり、上の規則でも引き続き出る
- 過去の journal の `worker_selection_invalidated` は、旧ビルドでは「集合不一致」と「イベントによる無効化」のどちらかで、区別できない

## 6. journal の順序と位置

### 6.1 追加する event

| event | value | 書く場所 |
|---|---|---|
| `preview_requested` | 候補の序数（0／1） | `ControlThread::Run` の `Command::Preview` で、`owner` と `item.candidate` の検査（既存の throw）の直後、`owner->Preview(...)` の直前。投げる版の `Record` を使う（書けなければ `FailAndClose`。enumerate の記録と同じ扱い） |
| `topo_block_enumerate` | worker 番号（0／1） | `worker_a_enumerated`／`worker_b_enumerated` の直後 |
| `topo_block_failure` | 失敗した worker 番号（0／1。失敗観測がない、または owner 全体の段階なら 2） | 判定が close_unconfirmed の初回だけ。worker 1 の exit ブロックの後 |
| `topo_block_close` | worker 番号（0／1） | `RecordCloseOutcomeToJournal` の初回だけ。`topo_block_failure` の後（closed のときは worker 1 の exit ブロックの後）、worker 0 → 1 の順 |
| `topo_unavailable` | 理由（下表） | ブロック見出しの直後に、18 行の代わりに 1 行 |
| `topo_open_add` 〜 `topo_check_current_count` | 各値 | ブロック見出しの直後に 3.2 節の順で 18 行 |

`topo_unavailable` の理由:

| value | 意味 |
|---|---|
| 0 | 内部の防御（失敗観測がない、など） |
| 1 | 要求を書き切っていない（未送信・送信途中・close 未試行） |
| 2 | 応答を受け取っていない |
| 3 | 応答を受け取ったが拒否した（`worker_reply_invalid`） |

### 6.2 ブロックの形

```text
topo_block_<enumerate|failure|close>(worker)
  └ 次のどちらか
     a) topo_open_add, topo_open_remove, … , topo_check_current_count（18 行、表の順）
     b) topo_unavailable(reason)（1 行）
```

読み手の規則: 見出しの次の行が `topo_unavailable` なら値なし、そうでなければ続く 18 行が値。

### 6.3 位置（run-05 と同じ形の例）

```text
run_started
worker_a_enumerated(2)
topo_block_enumerate(0) + 18 行           ← 追記
preview_requested(序数)                   ← 追記（tickCount64 が画面操作の時刻）
operation_failed
close_unconfirmed
失敗ブロック（failure_worker_index … close_receipt_*）
worker 0 の exit ブロック（… worker_close_not_sent）
worker 1 の exit ブロック（… worker_close_receipt_*）
topo_block_failure(0) + 18 行             ← 追記（失敗応答の diag）
topo_block_close(0) + topo_unavailable(1)  ← 追記（worker 0 には close を送っていない）
topo_block_close(1) + 18 行               ← 追記（worker 1 の close 応答の diag）
```

- 既存の行は 1 行も消さず、相対順序も変えない。新しい行は既存の 2 行の間か末尾に入るだけ
- T1／T2 で決めた「exit ブロックの直後にその worker の close 要約」の並びは保たれる（topo ブロックは両 exit ブロックの後にまとめて置く）
- closed のときの並び: `both_workers_close_verified` → exit 0 → exit 1 → `topo_block_close(0)` → `topo_block_close(1)`

### 6.4 `RecordCloseOutcomeToJournal` との整合

- topo ブロックは `state.details_recorded` が false の初回の分岐の中だけで書く。再 Close では判定行と再観測の行だけになり、topo ブロックは重複しない
- 各ブロックを個別に try で囲み、書けなければ `complete = false`（既存の失敗・exit ブロックと同じ）。関数は noexcept のまま
- データの出どころ: 失敗は `PreviewWorkerFailureObservation::topology`、close は `PreviewWorkerExitObservation::close_reply->topology`、enumerate は `PreviewWorkerOwner::LastTopology(worker)`（いずれも新設の `std::optional<PreviewTopologyDiag>`）
- 理由の決め方: `response_validated` なら値あり。そうでなく `response_received` なら 3、`request_written`（close は `sent`）なら 2、それ以外は 1。close は `close_reply` がない／`attempted` が false／`sent` が false のとき 1
- `topo_block_enumerate` と `worker_*_enumerated` は新しい helper `RecordEnumerateOutcome(journal, worker, count, topology)` にまとめ、main はそれを呼ぶ（既存行の名前と値は変えない）。enumerate 段の並びを試験できるようにするため

行数の増え方: run-05 の形で約 60 行（19 ＋ 1 ＋ 19 ＋ 2 ＋ 19）。各行は write-through と flush を伴う。close 系のブロックは `owner->Close()` の後に書くので exit 窓には影響しない。enumerate ブロックの 19 行は候補表示を数百 ms 遅らせ得る（※推定）。

## 7. worker main の exit code

`catch (...) { return 3; }` を `return 4;` に分ける。定数名は `kWorkerMainExceptionExitCode`（`preview_worker_main.cpp` 内）。

| code | 意味 | SDK |
|---|---|---|
| 0 | 明示 close が完了（`dispatcher.Completed()`）し、pipe ループが 0 を返した | close 済み |
| 2 | 引数・bootstrap・委譲の検査で明示的に拒否（`return 2`） | 触っていない |
| 3 | dispatcher は動いたが完了しなかった。失敗応答の後の自己 close、配送失敗、権限喪失や受付寿命切れでの自己 close、pipe ループ内の例外（`RunWorkerPreviewNamedPipeServer` が捕捉） | dispatcher が close を試行済み |
| 4 | main の `catch (...)` に例外が届いた。bootstrap の読取り・JSON 解析、handle 引数、`ValidateWorkerDelegation`、transport の構築、host の前提検査（`worker_authority_missing`） | 触っていない |
| なし | `safe_to_exit` が false。`Sleep(INFINITE)` で残り、人の回復を待つ | 終了未確認 |

- 4 に分ける理由: run-05 の worker 0 は exit 3 で、現行では「dispatcher が失敗応答の後に閉じた」と「bootstrap 中の例外」を区別できない。4 を分ければ 3 は前者だけになる
- `RunWorkerPreviewNamedPipeServer` の内側の例外は内部で捕捉されるため、main の catch に届く例外は SDK を使う前のものに限られる（コードから読んだ結論）
- 親は exit 0 だけを clean と数え、値の意味では分岐しない。親側の変更は不要
- 既存試験 `worker_delegation_authorization_tests` の「偽の bootstrap → exit 2」は明示拒否の経路なので変わらない
- `DUAL_LIVE_WORKER_POC.md` へこの表を転記するのは文書担当

## 8. 読み方の表

候補:

- (1) 遅延配送・再通知: module が既列挙 ID の AddChild を後から配送する。自分の照会（Pump・Children）が再通知を誘発する場合を含む
- (2) 外部の再列挙: 他の WPD クライアントや OS の再列挙で、同じ ID の Remove → Add が起きる
- (3) ID の実変化: 切断・再接続などで一時 ID が実際に変わる
- (4) module 間の干渉: 二つの worker の module instance が互いのイベントを誘発する。run-05 は worker 1 が SDK を読み込んでいないので対象外。両 worker が module を読み込む回で初めて検討対象になる

読むのは `topo_block_failure` の値（失敗がなければ `topo_block_close`）と、`worker_a_enumerated`・`preview_requested` の tickCount64。

| 観測 | 支持する候補 | 修正の方向 |
|---|---|---|
| `postFirstEventOp = 8` | どれでもない。スレッド前提の破れ | F0 を先に扱い、他の値は解釈しない |
| 既知 Add ≥ 1、未知 Add = 0、Remove（既知・未知）= 0、`checkSetEqual = 1`、`checkValidBeforePump = 1` | (1)。Remove が配送されない再列挙なら (2) も残る | F1 を先に検討。F2 は同じ観測が繰り返された後の別判断 |
| 上の行で `postAddKnownDistinct = 2` | (1) の「全 child の再通知」寄り | 同上 |
| 上の行で `inventoryAdd ≥ snapshotChildren` かつ `inventoryPumps` が小さい（EnumChildren の後の照会でも同じ ID が再通知されている） | (1) のうち自分の照会が誘発する型 | F1 が効きにくい可能性がある。F2 の判断材料にする |
| 既知 Remove ≥ 1 かつ既知 Add ≥ 1、未知 Add = 0、`checkSetEqual = 1` | (2) | F3 ＋ 規則は維持 |
| 既知 Remove ≥ 1、`checkSetEqual = 2`、`checkCurrentCount < 2` | (2) で戻る前に確認した、または物理的な切断 | F3・F4 |
| 未知 Add ≥ 1（既知 Remove の有無を問わず）、`checkSetEqual = 2` | (3) | F4 |
| snapshot 後のイベント 0、`checkValidBeforePump = 1`、`checkSetEqual = 2` | (3) でイベントが届かなかった、または Children とイベントの食い違い | F4 ＋ F6 |
| snapshot 後のイベント 0、`checkValidBeforePump = 2` | どれでもない（イベント以外の無効化。分類は `worker_selection_invalidated`） | F6 |
| 相手 worker の `topo_block_close`（両 worker が module を読み込んだ回）にも同じ時間帯の snapshot 後イベントがある | (4) | F5 |
| `postFirstEventOp = 2` で、`postFirstEventMs` と（`preview_requested` − `worker_a_enumerated`）の tick 差が数百 ms 以内（※経験則。IPC と画面処理の遅れを含む） | 補助情報: select の最初の Pump で配送された。イベントがいつ発生したかは分からない | 上の行の判断を変えない |

修正の方向（どれも今回は実装しない。選ぶのは A のデータを見た後）:

- F0: callback で触る状態（`module_sources_`・計数器）のスレッド安全化。他の F より先
- F1: snapshot の前に静穏期間を待つ。最初の非空在庫の後も Pump を続け、一定回数または一定時間イベントがなくなってから snapshot を取る。閾値は A のデータから決め、今はコードに固定しない。fail-closed は緩めない。enumerate の予算 W（22 s）の見直しを伴う
- F2: 既知 ID の Add だけ（Remove なし・集合一致）を無効化として扱わない。binding 経路（10 節）に揃える方向で、fail-closed を緩めるため architect と security の判断が要る。Coder の裁量では入れない
- F3: 運用手順。run 中は他の WPD クライアント（エクスプローラーの自動再生・写真アプリ等）を止め、チェックリストに記録する。規則は変えない
- F4: fail-closed を維持し、ケーブル・ハブ・給電・USB 省電力を物理側で調べる。software では緩めない
- F5: worker 間で module の読み込みを直列化する、または時間差を付ける。あるいは ADR-0031 の二プロセス方式の範囲で見直す
- F6: 無効化経路のコード調査

## 9. 監査付き回復コマンド（C）と lease の扱い

### 9.1 問題

B が入ると、`HardwareProcessLease` の構築時に marker root の `armed-session-*.marker` が 1 件でもあれば `camera_control_delegation_quarantined` で拒否する。回復コマンドは marker が存在する状態で camera-control の排他を取る必要があるため、通常の lease では取れない。B に回避の引数を足せば、どの呼び出し元からも B を外せることになる。

### 9.2 決定: 新 CLI `A0CameraStitcher.MarkerRecovery`

| 観点 | 案A 新 CLI（採用） | 案B MarkerDiagnostic に回復 mode を追加 | 案C `HardwareProcessLease` に回復用の引数 |
|---|---|---|---|
| 開発速度 | 中。target を 1 つ足す | 速い | 速い |
| 運用コスト | 低。承認対象を exe hash で特定できる | 中。読取り専用 exe の hash の意味が変わる | 高。全呼び出し元が回避経路を持つ |
| スケーラビリティ | 該当なし（一回限りの操作） | 同左 | 同左 |
| 学習コスト | 低。名前で用途が分かる | 中。読むだけのはずの道具に破壊操作が混ざる | 高 |
| 既存スタックとの親和性 | 高。MarkerDiagnostic の「SDK・WPD・lease を開始しない読取り専用」の契約を保てる | 低。既存文書の `--read-only` の契約と衝突 | 低。B の目的を打ち消す |

- ライブラリの入口は `hardware_process_lease.{hpp,cpp}` に置く。B が足した `WalkMarkerCandidates`、既存の `ParseDelegationMarker`・`ReadDiagnosticSnapshot`・`ProcessAbsent`・`ExistingSafeDirectoryTree` を同じ翻訳単位で再利用し、規則が二重化しないようにする
- CLI は `src/phase0/marker_recovery_main.cpp`。CMake option `A0_BUILD_MARKER_RECOVERY` を新設し、SDK 有効ビルドでは既定 OFF、SDK なしビルドでは既定 ON（試験用）。SDK 有効ビルドで作るときは `-DA0_BUILD_MARKER_RECOVERY=ON` を明示し、exe の SHA-256 を承認パケットに記録する

公開する API（固定 status だけを返す）:

```cpp
struct DualDelegationMarkerRecoveryResult final {
    std::string status;            // 9.9 節の固定語
    std::string anonymous_sha256;  // marker を読めたときだけ
    unsigned long long size{};
    bool session_match{};
};
[[nodiscard]] DualDelegationMarkerRecoveryResult DryRunDualDelegationMarkerRecovery(
    const DualDelegationMarkerRecoveryOptions& options, std::string_view expected_sha256 = {});
[[nodiscard]] DualDelegationMarkerRecoveryResult ExecuteDualDelegationMarkerRecovery(
    const DualDelegationMarkerRecoveryOptions& options, std::string_view expected_sha256,
    bool operator_attested_cameras_disconnected);
```

`DualDelegationMarkerRecoveryOptions` は SDK 有効ビルドでは空。SDK なしビルドだけ、test root・試験用 lease 名・PnP／process の探査関数の差し替え・現在時刻の差し替えを持つ（`#if !defined(A0_NIKON_SDK_AVAILABLE)`）。

### 9.3 B の抜け道にしない構造

1. `HardwareProcessLease` の構築子は変えない。引数・フラグ・別の構築子を足さない。B の走査は唯一の取得経路に残る
2. 回復用の排他は `hardware_process_lease.cpp` の無名名前空間にある RAII（例 `RecoveryCameraControlHold`）で、`CreateMutexW(L"Local\\" + lease 名)` と `WaitForSingleObject(0)` だけを行う。回復関数の中で作って中で捨てる。関数の外に出る型はない
3. この排他は lease ではない。`ArmDualDelegation`・`RegisterDualWorkers`・`DelegationEpoch` を持たず、SDK・WPD・worker の API はこれを受け取れない（それらは `HardwareProcessLease&` を要求する）
4. できる変更は、人が hash で承認した 1 件の marker を、同じ排他ハンドルで削除することだけ。marker の作成・書換え・directory の削除・一括削除はしない
5. 前提条件は排他を握った後に全部取り直す。どれかが外れたら marker を残して止まる
6. 成功しても「回復済み」の状態をどこにも保存しない。次の `HardwareProcessLease` 取得は B の走査を通常どおり行う
7. SDK 有効ビルドの lease 名は本番名だけ。test root・試験用 lease 名・探査の差し替えはコンパイルされない
8. 監査記録は同じユーザー権限のプロセスによる改ざんを防げない。脅威モデルは「同じユーザーの悪意あるプロセス」ではなく「操作の取り違え・古い状態での実行」で、それを止める目的で置く（同じユーザー権限なら marker を手で消すこともでき、そこは境界外）

### 9.4 前提条件

| # | 条件 | 外れたときの status |
|---|---|---|
| P1 | marker root が既存で、固定ローカル drive 上、祖先がすべて reparse でない directory、各名前の末尾が点・空白でなく `:` を含まない（作成しない） | `marker_root_untrusted` |
| P2 | `armed-session-*.marker` の一致がちょうど 1 件で、列挙が正常に終わる | 0 件 `marker_missing`、2 件以上 `marker_ambiguous`、列挙失敗 `marker_unavailable` |
| P3 | 名前が正規形 `armed-session-<数字>.marker`（数字は 1〜10 桁、先頭 0 は `0` のみ、値は 4294967295 以下） | `marker_name_noncanonical` |
| P4 | 通常の disk file、reparse でない、1〜255 byte、厳格な v2 構文、2 回読んで内容・属性・file index・更新時刻が同じ | `marker_invalid`／`marker_changed` |
| P5 | 内容の匿名 SHA-256 が `--expect-sha256`（小文字 hex 64 字）と一致（実行時は必須、dry-run は指定時だけ） | `hash_mismatch` |
| P6 | 記録された ownerPid・workerAPid・workerBPid がすべて不在（既存 `ProcessAbsent`。存在・権限不足・不明はすべて不在と見なさない） | `process_active_or_unknown` |
| P7 | A0 関連プロセスが 0。`CreateToolhelp32Snapshot` の全 session のプロセスで、実行ファイル名が `A0CameraStitcher.` で始まり（大文字小文字を区別しない）、自分の PID 以外のもの | `a0_process_present`、取得失敗 `process_list_unavailable` |
| P8 | PnP で D810 が 0 台。`SetupDiGetClassDevsW` を `DIGCF_ALLCLASSES` と `DIGCF_PRESENT` で呼び、hardware ID に `VID_04B0` を含むか、friendly name・device description に `D810` を含む装置がない（大文字小文字を区別しない。Nikon の全 USB 装置で止まる側に倒す。VID は※要確認） | `camera_present`、取得失敗 `pnp_unavailable` |
| P9 | camera-control の mutex を待ち時間 0 で取れる。`WAIT_ABANDONED` は直前に所有者が異常終了した印なので、その回は止める | `camera_control_busy`／`camera_control_abandoned` |
| P10 | 実行時だけ: `--confirm-cameras-disconnected` がある（操作者の電源 OFF／USB 切断の申告。証明ではない） | `operator_attestation_missing` |
| P11 | 実行時だけ: 監査記録に、同じ hash・同じ toolSha256・status `recovery_eligible` の dry_run 記録が 30 分以内（※仮定）にあり、同じ hash の `execute_intent` がどこにもない | `dry_run_record_missing`／`already_executed`、監査を読めなければ `audit_unavailable` |

- marker 名の session ID が現在の session と違っても受け付け、`sessionMatch = 0` として出力と監査に残す。B は別 session の marker を見つけるために入れたもので、再ログオン・再起動後の marker を回復できないと永久に止まる。別 session に生きた A0 プロセスがあれば P6／P7 で止まる（※要確認: security の同意）
- 既存の `InspectDualDelegationMarkerReadOnly` は現在 session の名前以外を `marker_ambiguous` にする。回復はこの規則を流用しない。読取り専用診断の規則も変えない

### 9.5 dry-run

```text
A0CameraStitcher.MarkerRecovery --dry-run [--expect-sha256 <hex64>]
```

順序: 引数 → P1 → P2 → P3 → P4 → P5（指定時）→ P6 → P7 → P8 → P9（取って即座に放す）→ 監査に dry_run 記録を追記 → 出力。

- marker には触らない（開くのは読取り共有の handle だけ）。marker root に何も作らない
- 書くのは監査記録の 1 行だけ。書けなければ status は `audit_unavailable`
- 成功の status は `recovery_eligible`。これは削除の許可ではなく、人が判断する材料
- 標準出力は 1 行の JSON: `{"status":"…","anonymousSha256":"…","size":N,"sessionMatch":0|1}`

### 9.6 実行

```text
A0CameraStitcher.MarkerRecovery --execute --expect-sha256 <hex64> --confirm-cameras-disconnected
```

1. 引数を検査する（P10）
2. 監査記録を読み、P11 を確かめる
3. mutex を待ち時間 0 で取り、関数を抜けるまで持つ（P9）
4. P1〜P8 を取り直す
5. marker を排他で開く: `CreateFileW(path, GENERIC_READ | DELETE, 0 /*共有なし*/, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)`。失敗（共有違反を含む）は `marker_open_failed`
6. 同じハンドルで再照合する:
   - `GetFinalPathNameByHandleW` の正規化パスが期待するパスと一致（大文字小文字を区別しない）
   - `GetFileType == FILE_TYPE_DISK`
   - `GetFileInformationByHandle` で directory でも reparse でもなく、`nNumberOfLinks == 1`、サイズ 1〜255、volume serial と file index が手順 4 で読んだものと一致
   - `ReadFile` で全内容を読み、厳格な v2 構文、SHA-256 が期待値と一致、その内容の 3 つの PID がすべて不在
   - 外れたら `marker_identity_changed`（hash だけが違えば `hash_mismatch`）。ハンドルを閉じるだけで削除しない
7. 証拠控え（※要確認、9.7 節）を作る。失敗したら `evidence_copy_failed` で削除しない
8. 監査に `execute_intent` を追記して flush する。書けなければ `audit_unavailable` で削除しない
9. `FILE_DISPOSITION_INFO info{TRUE}; SetFileInformationByHandle(h, FileDispositionInfo, &info, sizeof info)`。失敗したら監査に `execute_result: delete_failed` を書こうとし（その書込みの失敗は無視）、止まる
10. `CloseHandle(h)`。共有なしで開いているため、この時点で削除が確定する
11. marker root を再列挙する。一致 0 件かつ「見つからない」で終われば成功、それ以外は `post_delete_root_not_empty`
12. 監査に `execute_result` を追記する。成功なのにこの追記に失敗したら `executed_audit_incomplete`
13. mutex を放して終了

- パスで `DeleteFileW` を呼ばない。照合と削除の間にパスを差し替えられる隙間を作らないため
- 再試行しない。どの段で止まっても、もう一度 dry-run から人の判断をやり直す

### 9.7 監査記録

- 場所: `%LOCALAPPDATA%\A0CameraStitcher\Phase0\DualDelegationRecovery\audit.jsonl`。marker root の兄弟 directory で、B の走査パターンには一致しない。この directory は既存の安全な作成規則（固定 drive・reparse でない祖先）で作ってよい（marker root は作らない）
- 書込み: `FILE_APPEND_DATA` だけの handle（`FILE_WRITE_DATA` なし）、`OPEN_ALWAYS`、`FILE_SHARE_READ`、`FILE_FLAG_WRITE_THROUGH`、行ごとに `FlushFileBuffers`。file が reparse なら `audit_unavailable`
- 読取り: 別の読取り handle。1 MiB を超える、または 1 行でも解析できなければ `audit_unavailable`（※仮定: 上限値）
- 1 行の形:

```json
{"schema":"a0.marker-recovery-audit.v1","record":"dry_run","utc":"2026-10-05T03:14:15Z",
 "status":"recovery_eligible","anonymousSha256":"<hex64>","size":143,"sessionMatch":1,
 "operatorAttested":0,"toolSha256":"<実行中の exe の SHA-256>"}
```

- `record` は `dry_run`／`execute_intent`／`execute_result`
- 書かないもの: パス、PID、nonce、epoch、session ID の数字、ユーザー名、カメラの個体情報
- 証拠控え（本人判断パケット A の「保護されたローカル証拠控え」に対応、※要確認）: `DualDelegationRecovery\evidence-<hash 先頭 16 字>.bak` に marker の内容をそのまま `CREATE_NEW`・write-through で書き、読み直して一致を確かめる。nonce・epoch・PID を含むので、commit もリポジトリ外への持ち出しもしない。人が「hash とサイズの監査記録だけで足りる」と決めたら手順 7 ごと外す

### 9.8 1 回限り

- marker の内容は arm ごとの乱数 nonce を含むので、hash は marker 1 件に一意
- 同じ hash の `execute_intent` が 1 行でもあれば実行を拒否する（`already_executed`）。intent だけあって result がない（途中で落ちた）場合も拒否し、人が状態を確かめる
- CLI に繰り返しの処理はない。失敗後の自動再実行もしない
- run-04（前任 PC）と run-05（現 PC）の marker は、それぞれ別の PC・別の hash・別の承認になる（※要確認: 各 PC の marker の現状）

### 9.9 status と exit code

| status | 段 | exit |
|---|---|---|
| `recovery_eligible` | dry-run の成功 | 0 |
| `executed_marker_absent` | 実行の成功 | 0 |
| `marker_root_untrusted`, `marker_missing`, `marker_ambiguous`, `marker_unavailable`, `marker_name_noncanonical`, `marker_invalid`, `marker_changed`, `hash_mismatch`, `process_active_or_unknown`, `process_list_unavailable`, `a0_process_present`, `pnp_unavailable`, `camera_present`, `camera_control_busy`, `camera_control_abandoned`, `audit_unavailable` | 両方 | 2 |
| `operator_attestation_missing`, `dry_run_record_missing`, `already_executed`, `marker_open_failed`, `marker_identity_changed`, `evidence_copy_failed`, `delete_failed`, `post_delete_root_not_empty`, `executed_audit_incomplete` | 実行 | 2 |
| 使い方の誤り（引数不足・不明な引数・hash の形式違い） | — | 2（usage を stderr に出す） |

### 9.10 ビルドと test root

- SDK なしビルドだけ `--test-root <marker root の絶対パス>` と `--test-lease <試験用 lease 名>` を受け付ける（compile 定義 `A0_MARKER_RECOVERY_TEST_ROOT`）。監査 directory は test root の兄弟 `DualDelegationRecovery`
- SDK 有効ビルドでは引数解析にこの 2 つが存在せず、指定すると「不明な引数」で exit 2。ファイルにも mutex にも触らない
- 試験用 lease 名は既存の `IsTestLeaseName` を満たすものだけ

## 10. `PollDualInvalidation` との矛盾（記録して判断を先送りする）

二台 binding 経路（`BeginDualSession`／`PollDualInvalidation`、`dual_manager_active_`）は、`ModuleEventProc` で既知 ID の AddChild を変化として扱わず、未知 ID の AddChild とすべての RemoveChild だけを `dual_topology_changed_` にする。worker 経路（`WorkerPreviewSelection::ObserveTopology`）は既知 ID の AddChild を含むすべてのイベントで選択を無効にし、コメントに「既列挙 ID の Add は poll の間の切断・再接続かもしれない」と理由を書いている。同じ module イベントに対して、二つの経路が逆の安全判断をしている。今回はどちらにも寄せない。worker 経路で既知 ID の Add が本当に届くのかを run-05 は推定でしか示しておらず、A の計装で `postAddKnown`・`postRemove*`・集合一致を見るまでは、worker を緩める側にも binding を締める側にも根拠がない。加えて、worker 側を緩めれば fail-closed の規則を証拠なしに弱め、binding 側を締めれば binding 経路の既存の契約と試験の前提を変える。どちらも security レビューを伴う別判断で、選択規則を変えないという今回の範囲（不変条件 I-5）の外にある。A のデータで「既知 Add だけ・Remove なし・集合一致」が繰り返し観測されたら F2 として統一を検討し、Remove や未知 ID が混ざるなら binding 側の許容を見直す。それまで両経路のコードとコメントは変えない。

## 11. 受入試験 T-a〜T-h

T-a〜T-g は Coder A、T-h は Coder C。SDK なしの Release／Debug の両方で通す。

### T-a 計数器の区間と既知／未知（新規 `a0_worker_topology_counters_tests`、全構成でビルド）

注入（時刻は固定の time_point で与える）:

```text
Reset(t0)
Observe(Add,11) ×2, Observe(Remove,99)                 ; Opening
BeginInventoryWait(); CountInventoryPump() ×3; Observe(Add,11)
Snapshot(children=2, event_ids=2, ids={12,11}, t0+468ms)
Mark(select)
Observe(Add,11,t0+20418ms), Observe(Add,11), Observe(Add,12), Observe(Add,77)
Observe(Remove,12), Observe(Remove,55)
RecordCheck(valid_before=true, set_equal=false, count=3)
```

assert: openAdd 2、openRemove 1、inventoryAdd 1、inventoryRemove 0、inventoryPumps 3、snapshotChildren 2、snapshotEventIds 2、snapshotMs 468、postAddKnown 3、postAddUnknown 1、postRemoveKnown 1、postRemoveUnknown 1、postAddKnownDistinct 2、postFirstEventOp 2、postFirstEventMs 19950、checkValidBeforePump 1、checkSetEqual 2、checkCurrentCount 3。

### T-b 飽和・頭打ち・ID の非流出

- 飽和加算の helper: 4294967294 に 2 回足して 4294967295 で止まる
- ms の helper: 2^33 ms → 4294967295、負の差 → 0
- `SerializePreviewTopologyDiag`: 全項目 4294967295 で 18 キーちょうど、各値 10 桁以下。`Observe` に ID 3735928559 を渡した後の直列化文字列に `3735928559` が含まれない（件数がその値になり得ない注入にする）
- `static_assert`: `PreviewTopologyDiag` は `std::array<std::uint32_t, 18>` だけを持つ。`kPreviewTopologyDiagFields` は 18 個で、JSON キーと journal 名がそれぞれ重複しない

### T-c 凍結・reset・idle とスレッドの検出

- `Mark(close)` の後の `Observe`・`RecordCheck`・`Snapshot`・`Reset` は値を変えない
- `Reset` 前（Idle）のイベントは数えない
- PostSnapshot で `Mark(idle)` の後に `Observe` → `postFirstEventOp = 8`
- select 中に `on_owner_thread = false` の `Observe` → `postFirstEventOp = 8`
- 2 件目以降のイベントで `postFirstEventOp`・`postFirstEventMs` が変わらない

### T-d 分類の分割（`worker_preview_selection_contracts` の更新と `--category-table`）

既存試験の期待値を次に改める:

| 試験の場面 | 新しい期待 |
|---|---|
| `changed.CheckInventory({71, 97})` | `worker_selection_inventory_changed` |
| その直後の `changed.CheckInventory({71, 83})` | `worker_selection_invalidated`（拒否後） |
| `removed_readded`（Remove 71 → Add 71 → OpenSelected） | `worker_selection_topology_event` |
| `duplicate_add`（Add 71 → OpenSelected） | `worker_selection_topology_event` |
| `exceptional` の 2 回目の OpenSelected（callback 例外の後） | `worker_selection_invalidated` |
| fault 1（Suspend 後に Add 71）の最後の Resume | `worker_selection_topology_event` |
| fault 3 の `Resume(token, {71,97})` | `worker_selection_inventory_changed` |
| fault 0・2・3・4 の最後の Resume | `worker_selection_invalidated` |

追加:

- ObserveTopology の後に集合不一致の `CheckInventory` → `worker_selection_inventory_changed`（集合優先）
- Suspend の close() 内で ObserveTopology → `worker_selection_invalidated`
- Resume の open() 内で ObserveTopology → `worker_selection_invalidated`
- `Valid()`・`SameInventory()` が状態を変えない
- `--category-table`: 表が 62 語、試験側の独立集合（`tests/preview_worker_owner_tests.cpp` 777 行付近）に 2 語を足して一致、重複なし、全語が `ValidEvent` を通る、`worker_selection_invalidated` が残っている

### T-e worker の封筒 v2 と exit code（`worker_preview_dispatcher_contracts`、`preview_commissioning_contracts`、`worker_delegation_authorization_contracts`）

両試験の Fake transport に `WorkerTopologyDiagnostics()`（設定可能な値を返す）と `MarkWorkerTopologyOperation()`（呼び出し列を記録）を足す。

- 要求 schema v1 → failed 応答（v2・7 フィールド）、error `worker_authority`、`BeginWorkerPreviewSelection` が呼ばれていない、diag 全 0
- enumerate 成功 → schema v2、7 フィールド、diag の 18 キーが Fake の値と一致。Mark の列が [enumerate, idle]
- select 失敗で、Fake が `Close()` の中で自分の diag 値を変える → 応答の diag は変更前の値。Mark の列に close が入る
- close → closed 応答に diag がある
- 256 KiB の frame → 応答全体が 512 KiB ＋ 4096 byte 以下
- `worker_delegation_authorization_contracts`: 長さは正しいが JSON として壊れた bootstrap を渡した実 `A0CameraStitcher.PreviewWorker` が exit 4。既存の偽 bootstrap → exit 2 は変わらない

### T-f 親の拒否規則（`preview_worker_owner_reply_contracts` と単体の表）

単体の表（`ParsePreviewWorkerReply` に直接渡す。期待はすべて `worker_reply_invalid`）:

- schema v1 の 6 フィールド、schema v1 に diag を足した 7 フィールド、v2 で diag 欠落、8 フィールド
- diag が array／string／null、17 キー、19 キー、キー名違い、重複キー
- 値が `-1`、`1.0`、`1e3`、`1E3`、`4294967296`、`18446744073709551616`、`true`、`"1"`、`null`、`{}`
- ok／failed／closed／quarantined の各 status で diag 欠落
- 受理: `0` と `4294967295`、キー順を入れ替えた diag

統合（既存の fake worker 子プロセスの仕組み）:

- fake 子が select に 17 キーの diag で応答する
- assert: `FirstFailure` が operation select・category `worker_reply_invalid`・`response_received` true・`response_validated` false・`ack_write_completed` false。fake 子は ACK バイトを受け取らない（タイムアウト付きの読取りで 0 byte）。`Close()` が false。その子への close は送られない。試験用 marker が残り、次の owner の構築が拒否される
- journal に `topo_block_failure(0)` と `topo_unavailable(3)` が続けて出る

### T-g journal の順序と語彙（`--journal-contract`・`--journal-vocabulary`）

一時フォルダの実 `PreviewRunJournal` に、合成した観測を helper 経由で書いて行列を照合する。

- g1 closed: `both_workers_close_verified` → exit 0 → exit 1 → `topo_block_close(0)` ＋ 18 行（表の順・値一致）→ `topo_block_close(1)` ＋ 18 行
- g2 run-05 の形: worker 0 の select 失敗（validated・diag D）、worker 0 は close 未試行、worker 1 は closed（diag Z）→ 6.3 節の並び（close_unconfirmed 以降）と完全一致
- g3 理由: 失敗が要求送信後・応答なし → `topo_unavailable(2)`、応答拒否 → 3、要求未送信（owner 側の分類）→ 1、失敗観測なし → `topo_block_failure(2)` ＋ `topo_unavailable(0)`
- g4 2 回目の `RecordCloseOutcomeToJournal`: 判定行と再観測行だけで、`topo_` で始まる行が 1 行も増えない
- g5 `RecordEnumerateOutcome`: `worker_a_enumerated(2)` → `topo_block_enumerate(0)` ＋ 18 行。worker 1 は `worker_b_enumerated` → `topo_block_enumerate(1)`
- g6 語彙: 18 個の counter 名、`topo_block_enumerate`／`topo_block_failure`／`topo_block_close`／`topo_unavailable`、`preview_requested` が全部 `ValidEvent` を通る（48 字以内・`[a-z_]`）、互いに重複せず、62 語の分類表・`failure_operation_*`（8 語＋unknown）・既存の固定イベントの一覧（試験側に列挙）と重ならない
- g7 ID の非流出: 計数器に ID 3735928559 を通した diag を書いた journal に、その数字が出ない
- main の `preview_requested` の位置は 1 行の追加なので、レビューで確かめる（自動試験なし）

### T-h 回復コマンド（`hardware_process_lease_delegation_tests --recovery`、新しい ctest `hardware_process_lease_recovery_contracts`）

SDK なしビルド。test root に合成の v2 marker を置き、試験用 lease 名と差し替えた探査関数・時刻を使う。

| # | 注入 | assert |
|---|---|---|
| h1 | 単一の正規 marker（現在 session 名）、探査はすべて正常 | dry-run が `recovery_eligible`、hash が内容の SHA-256、`sessionMatch` 1。marker の内容・更新時刻・file index が不変。監査に dry_run が 1 行。marker root に新しいファイルなし |
| h2 | 別 session の正規名 | `recovery_eligible`、`sessionMatch` 0 |
| h3 | `armed-session-01.marker`／`armed-session-x.marker`／`armed-session-4294967296.marker` | `marker_name_noncanonical` |
| h4 | marker 2 件／marker 名の directory／256 byte／v2 構文違反 | `marker_ambiguous`／`marker_invalid`／`marker_invalid`／`marker_invalid` |
| h5 | A0 プロセスあり／一覧取得失敗／カメラあり／PnP 失敗 | `a0_process_present`／`process_list_unavailable`／`camera_present`／`pnp_unavailable` |
| h6 | workerAPid に生きた子プロセスの PID | `process_active_or_unknown` |
| h7 | 別スレッドが試験用 mutex を保持／保持したまま終了（abandoned） | `camera_control_busy`／`camera_control_abandoned` |
| h8 | dry-run なしで実行／hash 違い／`--confirm-cameras-disconnected` なし／31 分前の dry_run 記録 | `dry_run_record_missing`／`hash_mismatch`／`operator_attestation_missing`／`dry_run_record_missing`。marker 不変 |
| h9 | 試験が marker を読取り共有で開いたまま実行 | `marker_open_failed`、marker 不変、監査に `execute_intent` なし |
| h10 | dry-run 後に marker を新しい nonce で書き換え／`CreateHardLinkW` で link を追加 | `hash_mismatch`／`marker_identity_changed`、marker 不変 |
| h11 | 監査ファイルの位置に directory／読取り専用属性の監査ファイル | `audit_unavailable`、marker 不変 |
| h12 | 正常な dry-run → 実行 | `executed_marker_absent`。marker root に `armed-session-*.marker` なし。監査が dry_run → execute_intent → execute_result の順で同じ hash。証拠控えを作る設定なら内容が元と一致。同じ hash での 2 回目の実行は `already_executed`。回復前は `HardwareProcessLease(試験用 lease, 0 ms, test root)` が `camera_control_delegation_quarantined` で拒否され、回復後は取得できる |
| h13 | h1・h12 の標準出力と監査 | 合成した nonce・epoch・PID・session ID の数字・パスの断片が出ない |
| h14 | CLI: 引数なし／不明な引数／`--execute` だけ／大文字や 63 字の hash／空の test root | 前 4 つは usage で exit 2、空の test root は `marker_missing` で exit 2（探査に到達しない） |
| h15 | SDK 有効ビルド | 回復 target の compile 定義に `A0_MARKER_RECOVERY_TEST_ROOT` がない。`--test-root X --dry-run` が usage で exit 2 で、X 配下が不変（SDK 有効ビルドでの実行が要る。カメラには触らない） |

手動確認 M-1（本人判断パケット A の範囲。読取りだけでカメラ制御なし）: 運用 PC で、カメラ接続中の dry-run が `camera_present`、電源 OFF・USB 切断後は P8 を通ることを確かめ、PnP 探査を実機の装置名で検証する（※要確認: 実施の承認）。

## 12. 実装ガイドライン

### 順序

1. A は B と独立に着手できる。A は `hardware_process_lease.cpp` を触らない
2. C は B の着地（commit）後に着手する。`WalkMarkerCandidates` など B が足した helper を同じ翻訳単位で使うため
3. C は security レビューを通してから merge する。A の parser 変更は reviewer_security の表層レビューを通す
4. 文書（`DUAL_LIVE_WORKER_POC.md` の exit code 表・語彙 62 語・journal の並び、C の手順）への転記は文書担当が行う。Coder は本書を正本にする

### A が触るファイル

| ファイル | 変更 |
|---|---|
| `src/phase0/include/a0/phase0/preview_topology_diag.hpp`（新規） | キー表・値・命令コード・直列化・字句検査・diag の parse |
| `src/phase0/include/a0/phase0/worker_topology_counters.hpp`（新規） | 計数器 |
| `src/phase0/include/a0/phase0/nikon_sdk_transport.hpp`／`src/phase0/nikon_sdk_transport.cpp` | 3.7 節の結線、公開 2 メソッド、gated ビルドの全 0 |
| `src/phase0/include/a0/phase0/worker_preview_selection.hpp` | 5 節 |
| `src/phase0/include/a0/phase0/worker_preview_dispatcher.hpp` | schema v2、Mark、diag の取得順 |
| `src/phase0/include/a0/phase0/preview_worker_reply.hpp` | v2 parse、`PreviewWorkerReply::topology` |
| `src/phase0/include/a0/phase0/preview_worker_owner.hpp`／`src/phase0/preview_worker_owner.cpp` | 要求 v2、観測と close 応答に topology、`LastTopology(worker)` |
| `src/phase0/include/a0/phase0/preview_worker_failure_journal.hpp`／`src/phase0/preview_worker_failure_journal.cpp` | 分類表 62 語、`RecordTopologyBlock`、`RecordEnumerateOutcome`、close 記録の topo ブロック、ヘッダコメントの並び |
| `src/phase0/preview_commissioning_main.cpp` | `preview_requested`、`RecordEnumerateOutcome` |
| `src/phase0/preview_worker_main.cpp` | exit code 4 |
| tests（`a0.preview-worker.v1` を含む 16 か所ほか） | v2 と diag への追従、T-a〜T-g |
| `CMakeLists.txt` | `a0_worker_topology_counters_tests` の追加 |

### C が触るファイル

| ファイル | 変更 |
|---|---|
| `src/phase0/include/a0/phase0/hardware_process_lease.hpp`／`src/phase0/hardware_process_lease.cpp` | 9.2 節の API、`RecoveryCameraControlHold`、探査、監査 |
| `src/phase0/marker_recovery_main.cpp`（新規） | CLI |
| `CMakeLists.txt` | `A0_BUILD_MARKER_RECOVERY`、target、`setupapi` のリンク、試験用の compile 定義 |
| `tests/hardware_process_lease_delegation_tests.cpp` | `--recovery`（T-h） |

### 命名とパターン

- C++ の型・関数: `PreviewTopologyDiag`、`kPreviewTopologyDiagFields`、`WorkerTopologyCounters`、`PreviewTopologyOperation`、`WorkerTopologyDiagnostics()`、`MarkWorkerTopologyOperation()`、`RecordTopologyBlock()`、`RecordEnumerateOutcome()`
- JSON キーは camelCase、journal 名は `topo_` ＋ snake_case。両方とも `kPreviewTopologyDiagFields` から引き、文字列リテラルを他の場所に書かない
- callback 内（`Observe`）で確保・例外・ログ出力をしない。`noexcept` を付ける
- 再ビルドで `a0_phase0_core` を含む他の exe（CameraAgent・DualCameraAgent など）の hash も変わる。過去に記録した hash はそのビルドの記録として残し、新しい hash を別に記録する

## 13. 守るべき不変条件

- I-1 計数は制御を変えない。`Observe`・`RecordCheck`・`Mark` は noexcept で、callback 内で確保しない。選択の判定は計数器の値を読まない
- I-2 source ID は worker プロセスのメモリから出ない。`diag`・journal・画面・標準出力・例外文に ID を書かない
- I-3 `diag` は診断専用。親の判定（ACK・close の送信・判定・disarm・marker）は `diag` の値に依存しない。依存するのは形の正しさ（契約）だけ
- I-4 v2 の全応答に 18 キーちょうどの `diag` がある。それ以外は `worker_reply_invalid`、ACK なし、隔離維持
- I-5 fail-closed の規則は変えない。snapshot 後のイベントは 1 件でも selection を無効にし、集合不一致も無効にし、自動の再列挙・再試行はしない
- I-6 journal の既存の行と相対順序は変えない。追加行は 6 節の位置にだけ入り、詳細ブロックは 1 run に 1 回
- I-7 親と worker は同じ commit からビルドする。schema は要求・応答とも v2
- I-8 計数器は enumerate の入口でだけ reset し、close の合図で凍結する。凍結後は何も変えない
- I-9 `HardwareProcessLease` の構築子（B）は変えない。marker が 1 件でもあれば取得を拒否し、回避の引数を持たない
- I-10 回復用の排他は回復関数の内側だけに存在し、外に出る型を持たず、SDK・WPD・worker の権限を与えない
- I-11 削除は、検証した 1 件を同じ排他ハンドルの `FileDispositionInfo` で行う。パス指定の削除・一括削除・directory の削除をしない
- I-12 監査記録は追記だけで、削除の前に意図を書く。書けなければ削除しない
- I-13 回復は marker の hash ごとに 1 回で、再試行しない
- I-14 SDK 有効ビルドには test root・試験用 lease・探査の差し替えの経路がない

## 14. リスク

- R-1 callback のスレッド前提が外れていると、計数器も `module_sources_` も競合する。検出は `postFirstEventOp = 8` の 1 件目だけ（3.6 節）
- R-2 既知 Add だけが観測された場合、(1) と Remove が届かない (2) は区別できない。F3 の手順で他の WPD クライアントを止めた状態の run が要る
- R-3 データを取るには新しい実機 run の承認が要る（preview 枠 5/5 消費済み）。承認がなければ A は software 上の準備で止まる
- R-4 schema を上げると、fake・試験の改修範囲が広い（tests に `a0.preview-worker.v1` が 16 か所）。取りこぼしはコンパイルか試験の失敗で表に出る
- R-5 journal の行が約 60 行増え、enumerate 直後の候補表示が数百 ms 遅れ得る（※推定）
- R-6 再起動後に記録 PID が無関係のプロセスに再利用されていると、C は `process_active_or_unknown` で止まり続ける。止まる側の安全な失敗で、扱いは人が決める
- R-7 PnP 探査が装置を取りこぼす可能性がある。VID と名前の両方で照合し、操作者の申告（P10）と手動確認 M-1 で補う
- R-8 C は B の着地を待つ。B の最終形が作業ツリーの現状（`WalkMarkerCandidates`・`RejectAnyArmedSessionMarker`）と違えば、9 節の再利用の前提を見直す
- R-9 証拠控えは nonce を含む。採用するなら、ローカル保持だけで commit しないことを手順に明記する

## 15. ※要確認の一覧

1. `ModuleEventProc` が transport の所有スレッドで、MAID 呼び出しの中で呼ばれるか（3.6 節。SDK 資料と実機で未確認）
2. module 層で既列挙 ID の AddChild 再通知が実際に起きるか（run-05 の推定。A のデータ待ち）
3. 次の実機 run の承認（preview 枠 5/5 消費済み）
4. P8 の Nikon USB vendor ID `VID_04B0`
5. 証拠控えを作るか、監査の hash とサイズだけにするか（本人判断パケット A）
6. dry-run から実行までの有効時間 30 分（仮の値）
7. 監査ファイルの読取り上限 1 MiB（仮の値）
8. 別 session の正規名 marker を回復対象に含めること（`sessionMatch = 0`）への security の同意
9. 各 PC の marker の現状（前任 PC AOPC-11-NOTE の run-04 marker、現 PC AOPC-20-NOTE の run-05 marker）。C はそれぞれ別の承認で 1 回ずつ
10. `CreateToolhelp32Snapshot` が他 session のプロセスの実行ファイル名も返すこと（P7）
11. B の最終形（実装中。9 節は作業ツリーの現状を前提にしている）
12. `postFirstEventMs` と journal の tick 差の照合幅「数百 ms」（経験則）
13. 手動確認 M-1 の実施承認

## 補遺（2026-10-05、security レビューの結果）
- 実装の詳細化（総合レビューの指摘で追記、2026-10-05）: `inventoryPumps` は Pump を呼ぶ直前に数える（例外で終わった Pump も 1 回）。補助型として `PreviewTopologyField`（18 項目の添字 enum）、`PreviewTopologyBlock`／`PreviewTopologyUnavailable`（`RecordTopologyBlock` の引数）を追加し、`kPreviewWorkerSchema` は共有ヘッダ `preview_topology_diag.hpp` に置く。T-g の g1〜g7 は owner 試験の新モード `--topology-journal` にまとめ、`--reply-diag-table`／`--reply-diag-contract` と `worker_topology_counters_contracts` を加えた 4 系列を ctest に登録した。
- 読み方の規則（6 節の補足）: 「見出しの次の 18 行が値」に加え、18 行の event 名が 3.2 節の表の順と一致することを機械的に照合する。値の行を書いている途中で `Record` が失敗した場合（`complete=false`）は、行数だけで読むと次の見出しと取り違えるため。

- 実装の順序依存（単体検証の指摘、2026-10-05）: dispatcher は enumerate の命令の印（Mark）を transport の Reset より先に付け、Reset は実行中の命令の印を消さない。この順序により snapshot 後の同じ命令中に届いたイベントは命令コード 1（enumerate）で記録される。順序を変えるとコードが 8（idle／所有スレッド外）に化けるため、`tests/worker_topology_counters_tests.cpp` の enumerate ケースがこの順序を固定している。設計上の前提として明記する。
- 防御的分岐: 親が `response_validated=true` なのに topology を持たない応答を記録しようとした場合は理由 0 に倒す（通常フローでは到達しない defense-in-depth。`--topology-journal` の g3f が固定）。

- `topo_block_failure` の値が 2（owner 全体の段階、例: disarm の失敗）のとき、続く `topo_unavailable(1)` は「worker への要求が存在しない（送信を試みていない）」の意味であり、「要求の書き込みが途中で止まった」ではない（設計適合レビューの指摘を受けた Orchestrator の裁定、2026-10-05。※仮定。6.4 節の一般規則をそのまま適用した実装を変えず、読み手の誤読を本注記で防ぐ。architect が 0 固定を望む場合は `RecordFailureTopology` の `index==2` 分岐 1 か所で変更できる）。


- C の対象範囲: root 直下に正規名 `armed-session-<数字>.marker` がちょうど 1 件であること。session ID は問わないが、現在の session と異なるときは人が明示的に確認する（security 承認。※要確認 8 を解消）。非正規名・ディレクトリ・reparse point が 1 件でもあれば、C は処理せず停止理由として返す。
- 証拠控え（nonce を含む写し等）は marker root の外に置く。root の中に置くと `armed-session-*.marker` の glob に一致し、本物を処理した後も隔離が解けなくなる。
- 読取り専用診断は現在の session の候補 1 件だけを `eligible_for_human_review` にする設計のままなので、再起動後は `marker_ambiguous` を返す。C はこの状態の marker も対象にできる必要がある（dry-run で候補名と session 一致／不一致を状態語で返す）。
- 本番の marker root の DACL には別アカウント（サンドボックス用グループ）の読取り権限が継承されている（2026-10-05 確認）。marker の内容（nonce・epoch・PID）は読めるが書換え・削除はできず、worker のなりすましにも届かない。将来、root 作成時に保護付き DACL を付ける案を残す（既存ディレクトリの ACL 変更は本番状態の書換えなので所有者承認が要る）。
