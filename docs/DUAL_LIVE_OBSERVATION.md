# 二台ライブビューの観測記録の評価（#327）

[`Test-DualLiveObservation.ps1`](../scripts/Test-DualLiveObservation.ps1)は、保存済みの匿名ログを読み、**指定した同じ時間窓で両カメラのframe受信が継続した記録があるか**を検査する。カメラ接続、SDK/WPD呼び出し、別processの起動、frame処理、ファイル書き込みは行わない。PowerShell 7.4以降が必要。

これは実機試作に向けた観測結果の評価ツールである。Nikon通信仕様を使う実装でも、agent用の新たな通信protocolでもない。#326の利用・公開条件とADR-0036の確認を省略せず、既存SDK/WPD/worker/controller・通常画面は変更しない。

## 入力

UTF-8（BOMなし）のJSONを使う。最大4 MiB、最大32,768イベント。未知の欄、重複key、大文字小文字違い、型違いを拒否する。整数は指数・小数表記を受け付けず、0〜9,007,199,254,740,991の範囲とする。

```json
{
  "schemaVersion": "a0.dual-live-observation.v1",
  "source": "simulation",
  "clock": "controller-monotonic-ms",
  "window": {
    "startMs": 100,
    "endMs": 300,
    "maxFrameGapMs": 100,
    "minFramesPerCamera": 3
  },
  "events": [
    {"alias":"CAM-A","type":"opened","atMs":0},
    {"alias":"CAM-B","type":"opened","atMs":10},
    {"alias":"CAM-A","type":"frame","atMs":100,"sequence":1},
    {"alias":"CAM-B","type":"frame","atMs":100,"sequence":1},
    {"alias":"CAM-A","type":"frame","atMs":200,"sequence":2},
    {"alias":"CAM-B","type":"frame","atMs":200,"sequence":2},
    {"alias":"CAM-A","type":"frame","atMs":300,"sequence":3},
    {"alias":"CAM-B","type":"frame","atMs":300,"sequence":3},
    {"alias":"CAM-A","type":"stopped","atMs":310},
    {"alias":"CAM-B","type":"stopped","atMs":320},
    {"alias":"CAM-A","type":"closed","atMs":330},
    {"alias":"CAM-B","type":"closed","atMs":340}
  ]
}
```

上は人工記録用のpolicyで、製品のfps・継続時間の受入値ではない。評価窓の始点は終点より小さくし、最大間隔は1 ms以上、最低受信数は2〜32,768で明示する。sourceは`simulation`または`hardware`。`hardware`の文字だけで実機証拠に昇格しない。

aliasは`CAM-A`・`CAM-B`だけで、個体識別子や画像を含めない。atMsは一つのcontrollerが観測する単調時計上の相対msとする。カメラごとの時計・壁時計を混在させない。全イベントはatMsの非減少順。同じmsの複数受信は許容する。

各aliasは`opened → frame（任意回数）→ stopped → closed`の順を守り、再openしない。sequenceはframeだけに必須の正整数で、alias内で厳密増加する。欠番は許容し、重複・逆行を拒否する。opened/closedは呼び出し要求時刻ではなく、成功が確認された時刻の記録とする。実際のtransportからこれを生成する部分は未実装。

## 判定と限界

- 評価窓全体を両aliasのopen〜stopが覆うことを要求する。窓の始点・終点も含む。
- 窓内の受信数をaliasごとに数える。最大受信間隔には「窓の始点→最初の受信」「最後の受信→窓の終点」も含める。開始・終了付近の停止を見落とさない。
- 二台のclosed記録がなければ成功にしない。片側が最初のframeのままでも、もう一方の更新で補わない。
- 受信記録の整合性の評価に限る。画像内容の変化、ログ生成器の正しさ、個体とのbinding、二台が別の実カメラか、lease所有、SDK/WPD排他、通信仕様の利用条件、停止APIの実際の成功を独立に証明しない。物理的なシャッター同期も証明しない。

出力は`a0.dual-live-observation-result.v1`のJSON一つ。入力スナップショットのSHA-256、source、明示policy、alias別受信数・最大間隔・窓被覆・close記録、失敗理由を出す。構文等が不正なら部分成功情報を出さず、理由を固定文字列にする。入力の未知の値やpath、例外の本文は出さない。

| exit code | outcome | 意味 |
| --- | --- | --- |
| 0 | BothUpdatedObserved | 指定窓・policyを満たす二台の更新・closeが記録されている |
| 3 | CriteriaNotMet | 読める記録だが、片側不足、窓被覆不足、受信間隔超過、close未確認等がある |
| 2 | InvalidObservation | IO、形式、時刻逆行、sequence再使用、不正な状態遷移等で評価できない |

すべての結果で`hardwareReady=false`、`physicalIdentityVerified=false`、`shutterSynchronized=false`。成功は二台ライブビューの実現・製品受入・A0品質承認ではない。

```powershell
pwsh -NoProfile -NonInteractive -File scripts/Test-DualLiveObservation.ps1 -Path <private-observation.json>
pwsh -NoProfile -NonInteractive -File scripts/Test-DualLiveObservationContracts.ps1
ctest --test-dir build/reference -C Release -R '^dual_live_observation_contracts$' --output-on-failure
```

契約試験は人工記録をignoredなbuild配下へ保存する。実機の収集は行わない。未知欄・時刻・再送・途中停止・欠落・境界・入力不変・CLI exit codeと、hardwareラベルでもReadyにならないことを検査する。

開発時のローカル確認では421チェックが成功し、Release/DebugのCTest各1/1も成功した（11.75秒 / 10.36秒）。上記JSON例も実CLIへ渡し、各alias 3受信・最大間隔100 ms・全close記録あり・hardwareReady=falseを確認した。未コミット・main未反映。
