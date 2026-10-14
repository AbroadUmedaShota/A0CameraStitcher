# Dual Bound Live View v1 — 照合後の Live View

Status: binding host 側を実装済み（未ビルド・実機未確認）。capture host 側は未実装（#295）
Schema: `a0.camera-agent.hardware-dual-bound-live-view.v1`
Issue: #291 · 関連: ADR-0025、ADR-0028、`HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md`

照合（binding）が Ready になった後、候補の選び直しをせずに CAM-A／CAM-B の Live View を開くための wire protocol。読者は Camera Agent（native）と WPF の Agent クライアントを実装・保守する開発者で、binding v1 の封筒・session・失効の仕組みを知っている前提で書く。

## 目的と範囲

binding v1 は、alias を割り当てた候補の Live View を受け付けない。割当のときに行った停止と close（quiesce）を崩さないためである。この schema は、割り当て済みの候補を Ready の後にもう一度開く唯一の経路で、同じ安全規約に従う。

| 項目 | 内容 |
| --- | --- |
| 受ける host | binding host だけ（marker `HardwareBinding`）。production の `A0CameraStitcher.DualCameraAgent.exe` では、binding の dispatcher に controller を 1 つ付けて受ける |
| 受けない host | capture host（marker `Hardware`）。`activate-capture` の後の Live View は #295 で扱う。今の capture host にこの schema を送ったときの応答は v2 の解析による拒否になる見込みで、コードは※要確認 |
| 開ける状態 | binding が `Ready` のときだけ |
| 開く対象 | alias（`CAM-A`／`CAM-B`）で指す。候補の ordinal は wire に出さない |
| 変えないもの | binding v1・Dual v2 の既存の命令と形。Live View を使わないときに Agent へ送る命令の列 |

schema は host が増えても同じで、違うのは marker だけになる予定。どちらの host も、自分の marker でない要求は拒否する。

## 封筒

要求は binding v1 と同じ 6 項目で、どれも 1 回ずつ必須、余分な項目は拒否する。

| 項目 | 型 | 値 |
| --- | --- | --- |
| `schemaVersion` | string | `a0.camera-agent.hardware-dual-bound-live-view.v1` |
| `simulation` | boolean | `false` だけ |
| `marker` | string | 受ける host の marker。今は `HardwareBinding` |
| `requestId` | string | 1〜128 文字。英数字と `.` `-` `_` |
| `operation` | string | 下の 3 命令のどれか |
| `payload` | object | 命令ごとに決まった項目だけ |

`sessionId` は 32 文字の 16 進数（すべて `0` は不可）で、binding v1 の `begin-binding` が返したものを使う。

応答は次の 7 項目。

```json
{
  "schemaVersion": "a0.camera-agent.hardware-dual-bound-live-view.v1",
  "simulation": false,
  "marker": "HardwareBinding",
  "requestId": "lv-0001",
  "success": true,
  "resultCode": "BoundLiveViewStarted",
  "payload": { }
}
```

- `requestId` は要求のものを返す。読み取れない、または形が不正なときは `"rejected"` を返す
- 拒否の `payload` は `{"detail": "<説明>"}`。`HostLifetimeInsufficient` と `BindingInvalidated` は項目が増える（後述）
- binding host は、要求の `schemaVersion` がこの schema のときだけここへ回す。JSON として読めない要求や `schemaVersion` が違う要求は、binding v1 の解析と拒否の語彙で答える

## 命令

### `start-bound-live-view`

指定した alias の SDK source を開き、Live View を始める。

要求:

```json
{
  "schemaVersion": "a0.camera-agent.hardware-dual-bound-live-view.v1",
  "simulation": false,
  "marker": "HardwareBinding",
  "requestId": "lv-0001",
  "operation": "start-bound-live-view",
  "payload": {
    "sessionId": "0123456789abcdef0123456789abcdef",
    "cameraAlias": "CAM-A"
  }
}
```

成功（`BoundLiveViewStarted`）の `payload`:

```json
{
  "sessionId": "0123456789abcdef0123456789abcdef",
  "cameraAlias": "CAM-A",
  "state": "Live",
  "captureAdmissionRemainingMs": 214000
}
```

- 何かが開いている間の 2 回目の start は、切り替えとして扱わずに `BoundLiveViewAlreadyOpen` で拒否する。反対側へ移るときは、クライアントが先に `stop-bound-live-view` を送る
- SDK が start を拒否し、失効の事象も無く、何も開いたまま残っていなければ `LiveViewStartFailed`。binding は Ready のまま

### `get-bound-live-view-frame`

Live 中の alias の frame を 1 枚返す。

要求の `payload` は start と同じ（`sessionId`、`cameraAlias`）。

成功（`BoundLiveViewFrame`）の `payload`:

```json
{
  "sessionId": "0123456789abcdef0123456789abcdef",
  "cameraAlias": "CAM-A",
  "frameBytes": 183421,
  "frameBase64": "<base64>",
  "captureAdmissionRemainingMs": 213500
}
```

- `frameBytes` は base64 にする前の大きさで、上限は 262,144 バイト（256 KiB、binding v1 と同じ `kMaximumBindingLiveViewFrameBytes`）
- frame が空なら `LiveViewFrameUnavailable`、上限を超えれば `LiveViewFrameTooLarge`。どちらも Live View は開いたままで、切り詰めた frame は返さない。空の frame で止めないのは、始めた直後のカメラはまだ frame を出さないため
- 空の frame でも、SDK の失敗で失効の事象が積まれていれば `BindingInvalidated` になる

### `stop-bound-live-view`

Live View を止めてから SDK source を閉じる。冪等。

要求の `payload` は `sessionId` だけ。

```json
{ "sessionId": "0123456789abcdef0123456789abcdef" }
```

成功（`BoundLiveViewStopped`）の `payload`:

```json
{
  "sessionId": "0123456789abcdef0123456789abcdef",
  "state": "Closed",
  "liveViewStopped": true,
  "sdkSourceClosed": true,
  "wasOpen": true,
  "captureAdmissionRemainingMs": 213000
}
```

- 何も開いていなければ、SDK を呼ばずに `wasOpen=false` の成功を返す
- 停止 → close の順に 1 回ずつ試す。停止に失敗したら close は試さない（流れているかもしれない source を閉じないため）。どちらかが失敗する、または adapter の状態を読み直して閉じていなければ、binding を失効させて `BindingInvalidated` を返す
- controller が開いていないのに adapter に何かが開いていれば、どの候補を閉じるか決められないので、推測では閉じずに binding を失効させる

### 成功した応答に共通の `captureAdmissionRemainingMs`

3 命令の成功には、すべて `captureAdmissionRemainingMs` が付く。値は max(0, host の寿命の残り − 185,000) ミリ秒で、応答を作った時点のもの。定義は「時間」の節に書く。

## 拒否の評価順

上から順に評価し、最初に当たったもので答える。SDK を呼ぶのは 3 の失効の事象の確認と、8 の SDK 操作だけ（6 で開いているかを読むのは SDK の呼び出しではない）。

| 順 | 条件 | `resultCode` | 対象の命令 |
| --- | --- | --- | --- |
| 1 | 形式の誤り | 下の表 | すべて |
| 2 | `sessionId` が今の session と違う、または session が無い | `SessionMismatch` | すべて |
| 3 | 失効の事象が積まれている、または binding がすでに `Invalid` | `BindingInvalidated` | すべて |
| 4 | binding が `Ready` でない | `BindingNotReady` | すべて |
| 5 | host の寿命の残りが 20,000 ms 未満（仮の値） | `HostLifetimeInsufficient` | start |
| 6 | 何かが開いている（controller の記録か adapter の状態のどちらかで） | `BoundLiveViewAlreadyOpen` | start |
| 6 | Live でない、または alias が Live 中のものと違う | `BoundLiveViewNotActive` | frame |
| 7 | frame が空 / 上限超え | `LiveViewFrameUnavailable` / `LiveViewFrameTooLarge` | frame |
| 8 | start・frame の読み取り・停止・close の途中で SDK が失敗した | `BindingInvalidated`（理由は adapter が積んだもの、無ければ `SdkError`） | すべて |

順 1 の内訳（解析の順に評価する）:

| `resultCode` | 条件 |
| --- | --- |
| `UnexpectedField` | 封筒の項目が足りない、または余分にある |
| `InvalidFieldType` | 封筒の項目の型が違う（`payload` が object でない場合を含む） |
| `DualBoundLiveViewProtocolRequired` | `schemaVersion` が違う、`simulation` が `true`、または `marker` がこの host のものでない |
| `InvalidRequestId` | `requestId` の形が不正 |
| `UnsupportedOperation` | `operation` が 3 命令のどれでもない（`payload` の中身は見ない） |
| `UnexpectedField` | `payload` の項目が足りない、または余分にある（型の誤りは `InvalidFieldType`） |
| `InvalidSessionId` | `sessionId` が 32 文字の 16 進数でない、またはすべて `0` |
| `UnknownCameraAlias` | `cameraAlias` が `CAM-A`／`CAM-B` でない |

- 2 を 3 より前に置くのは、別の session を名乗る要求に、今の session の状態を知られず、次の要求が見るはずの失効の事象も消費させないため（binding v1 と同じ規則）
- start では 4 と 5 の間で、binding core から alias の ordinal を引く。ここで core が `BindingNotReady`／`UnknownCameraAlias` を返すことがあるが、Ready の binding は両方の alias を持つので、通常は起きない
- このほか、想定外の例外は `AgentFailure` で返す

### `HostLifetimeInsufficient` の `payload`

```json
{
  "detail": "the host has too little lifetime left to start a Live View",
  "captureAdmissionRemainingMs": 0
}
```

### この schema の `BindingInvalidated` の `payload`

binding v1 の `BindingInvalidated` に、閉じたかどうかの項目が加わる。

```json
{
  "sessionId": "0123456789abcdef0123456789abcdef",
  "state": "Invalid",
  "invalidationReason": "SdkError",
  "liveViewStopped": true,
  "sdkSourceClosed": false,
  "wasOpen": true
}
```

| 項目 | 意味 |
| --- | --- |
| `invalidationReason` | binding の最初の失効理由。`AgentRestart`／`UsbReconnect`／`CameraCountChanged`／`TopologyChanged`／`SdkManagerRecreated`／`SdkError` |
| `liveViewStopped` | 停止が成功した、または adapter が Live View を持っていない |
| `sdkSourceClosed` | adapter が開いている source は無いと報告している |
| `wasOpen` | 停止と close を試したときだけ付き、値は常に `true`。試していなければ項目ごと無い |

`liveViewStopped` と `sdkSourceClosed` は adapter の報告を読んだ値で、SDK がカメラの停止を確かめた証拠ではない。Nikon の adapter は SDK の例外のときに SDK の session を終わらせて source を閉じるが、停止の命令そのものが失敗したときは Live View を「開いたまま」と報告し続ける（`liveViewStopped=false`、`sdkSourceClosed=true`）。この印は、持ち主が session の終了（`cancel-binding`）に成功するか、次の列挙をするまで消えない。開始や frame 読み取りの例外では、session を終わらせたことをもって `liveViewStopped=true` と報告する（※実機で未確認）。

### そのほかの応答

| `resultCode` | いつ | binding |
| --- | --- | --- |
| `LiveViewStartFailed` | SDK が start を拒否し、失効の事象が無く、何も開いていない | Ready のまま |
| `BoundLiveViewUnavailable` | controller か SDK adapter を持たない binding host に、この schema を送った | 変わらない |

Nikon の adapter は SDK の例外をすべて失効の事象に変える。すでに Live View を持っているときだけ事象なしで start を拒否するが、その場合は順 6 で先に拒否される。そのため、実機の adapter で `LiveViewStartFailed` に至る経路は見当たらず、今のところ fake の adapter でだけ起きる（※要確認）。

## binding v1 の `activate-capture` との関係

binding host の `activate-capture` は、次の順で評価する。

1. capture への移行が有効でない → `CaptureTransitionUnavailable`
2. binding が `Ready` でない → `BindingNotReady`
3. bound Live View が閉じきっていない → `BoundLiveViewOpen`

3 の拒否では SDK を呼ばず、binding は Ready のまま。クライアントは `stop-bound-live-view` で閉じたことを確かめてから、もう一度 `activate-capture` を送る。閉じたかどうかは、controller の記録が `Closed` で、かつ adapter が source も Live View も開いていないと報告していることで判定する。controller の記録だけでは判定しない。

binding v1 の `cancel-binding` と `begin-binding` は、bound Live View が開いていても今のまま動く。どちらも adapter の SDK session ごと終わらせ、controller を `Closed` に戻す。`cancel-binding` の後始末を確かめられなければ、binding は失効する。

## 安全の不変条件

1. SDK の source と Live View は、同時に 1 つまで。開いているかどうかの正は adapter の報告で、controller の記録から推測しない
2. 撮影の前に閉じる。`activate-capture` は開いている間は受け付けない。capture host が WPD を開く前の確認（source も Live View も開いていないこと）は変えていない
3. no retry。SDK の失敗と失効の事象は、すべて binding を失効させる。開き直しも再試行もしない。復旧は照合のやり直しだけ
4. frame はメモリにだけ置く。応答の文字列の中にだけあり、ログ・journal・ファイルに書かない。数えるのは件数だけ
5. Live View で Agent の寿命を延ばさない。keep-alive の命令は無い
6. Live View を使わないときは、Agent へ送る命令の列も SDK の呼び出しも今と同じ。controller を付けても、この schema の要求が来ない限り何もしない

controller の状態は次の 3 つ。開く処理は 1 回の要求の中で終えるので、途中の状態は外に見えない。

| 状態 | 意味 | 移る契機 |
| --- | --- | --- |
| `Closed` | 何も開いていない | start の成功で `Live`。binding の失効で `Terminal` |
| `Live` | 1 つの alias の source と Live View が開いている | stop か `OnIdle` で閉じきれば `Closed`。閉じきれなければ binding を失効させて `Terminal` |
| `Terminal` | binding が失効した。新しい binding まで何も受けない | `begin-binding` か `cancel-binding` の成功で `Closed` |

## `OnIdle`（クライアントが黙ったとき）

WPF の異常終了やハングで Live View が開いたまま残らないように、host は自分で閉じる。

- この schema の要求が、今の session について 10,000 ms（仮の値）来なければ、停止 → close を 1 回ずつ試す
- 時計を進めるのは、`sessionId` が一致した要求だけ。拒否で答えた要求も数える。形式の誤りや別 session の要求は数えない
- 閉じきれなければ binding を失効させる（`SdkError`）。応答を返す先が無いので、クライアントは次の要求で `BindingInvalidated` を受け取る
- `Live` でないとき、binding が `Ready` でないときは何もしない
- pipe の待ち受けは最長 5,000 ms ごとに `OnIdle` を呼ぶ。止まるまでの最悪はおよそ 15 秒（10 秒 + 5 秒。要求の処理時間は含めない）
- 10 秒が実際の frame 取得の間隔に対して十分か、短すぎないかは実機で決める（※未検証）

## 時間

時間の定義は `HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md` の「二台の撮影にかかわる時間の定義」を正とする。この schema が足すのは、Agent の寿命と撮影の受付から導く次の 2 つだけで、新しい期限は作らない。

| 値 | 定義 | 起動からの時刻に直すと |
| --- | --- | --- |
| `captureAdmissionRemainingMs` | max(0, host の寿命の残り − 185,000) ms。185,000 は capture host の受付に要る残り（watchdog 180 秒、応答 3 秒、後始末 2 秒） | 起動から 415 秒（6 分 55 秒）で 0 になる |
| Live View を始められる残り | host の寿命の残りが 20,000 ms（仮の値）以上 | 起動から 580 秒まで start できる |

- host の寿命は binding host と capture host が共有する 600 秒（`AgentHostLifetime`）。Live View はこれを延ばさない
- `captureAdmissionRemainingMs` は「撮影を受け付けてもらえる残り」の目安で、受付そのものは capture host が自分で判定する（`dual_hardware_camera_agent.cpp` の `kDualMinimumHostAdmissionMilliseconds`）。185,000 は `dual_hardware_camera_agent.hpp`（`kDualMinimumHostAdmissionMilliseconds`）と `dual_bound_live_view.hpp`（`kBoundLiveViewCaptureAdmissionReserveMilliseconds`）に別々に書かれている。2 つが食い違うと `dual_bound_live_view_tests.cpp` のコンパイルが通らない
- 値は応答を作った時点のもの。クライアントは受け取ってからの経過時間（単調な時計）を引いて使う
- `captureAdmissionRemainingMs` が 0 でも Live View は使える。start を拒むのは寿命の残りが 20 秒を切ったときだけ
- 寿命の残りを読めないときは 0 として扱う（短い側に倒す）

## 実機で確かめていない点

コードはまだビルドしておらず、試験（`tests/dual_bound_live_view_tests.cpp`、ctest 名 `dual_bound_live_view_contracts`）も実行していない（※未検証）。実機の D810 では何も確かめていない。

| 点 | 状態 |
| --- | --- |
| Ready の後に、割り当て済みの候補の source を開き直して Live View を出せるか | ※未検証 |
| 連続した frame 取得の頻度と、SDK エラーの出る頻度 | ※未検証。一台の経路の実績はあるが、この経路では未測定 |
| 停止 → close の後、`activate-capture` → WPD の確認を通れるか | ※未検証 |
| Live 中の USB 抜去で失効し、`BindingInvalidated` が返るか | ※未検証 |
| `HostLifetimeInsufficient` の 20 秒と `OnIdle` の 10 秒 | 仮の値。実測で置き換える |
| `liveViewStopped=true` が、カメラ側で Live View が止まったことと一致するか | ※要確認 |
| 実機の adapter で `LiveViewStartFailed` が起きるか | ※要確認（上の「そのほかの応答」） |
| SDK の待ち時間（開く 10 秒、開始 10 秒、frame の読み取り 3 秒、停止 10 秒、close 10 秒）で、1 回の要求の応答時間が足りるか | ※未検証 |
