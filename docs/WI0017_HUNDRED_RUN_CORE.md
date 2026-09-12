# WI-0017-SW01: 100回試験の内部制御（実機未使用・fake-only）

## 今回の範囲

同じカメラ割当を使う100回制御の内部部品を追加する。実際のカメラ、SDK、WPD、Live Viewは使用しない。CLI/UIの100回開始は引き続き未対応であり、`--capture-recovery-run-count 100` は拒否する。既存の1回・10回、v2通信形式、180秒のtransaction watchdog、600秒のhost既定値は変更しない。

これはWI-0017のソフトウェア準備だけであり、WI-0017全体の完了やDualCamera実機100/100の合格ではない。合成は`Pending`、A0品質は`Unapproved`のまま。テスト用の承認データは製品責任者の実測p95承認にはならない。

## 判断と実装

- 開始前に10回結果のファイルhash、p95、日時、別途記録された承認、対象runを照合し、承認を排他的に使用済みにする。集計時だけの承認確認では、撮影の開始を防げないためである。
- 各回の開始記録を保存してからworkflowを1回呼び、結果のtransaction ID、原画像のalias・size・SHA-256・時刻を保存してから次へ進む。ファイルを上書きせず、途中失敗後も使用済み承認と観測済みtransactionのclaimを保持する。
- 1つのsnapshotをそのまま渡し、副作用のない割当有効性チェックを使う。カメラ再列挙や再割当は行わない。同じworkflowに対する競合実行は待ち行列に入れず拒否する。
- 最初の失敗で停止する。未確定結果や割当失効は通常の成功集計へ入れず、途中経過として保存する。不明な応答を新規撮影や自動の復旧要求で置き換えない。未確定transactionの確認は既存workflowの同一ID復旧経路に残す。
- 残りhost時間は呼出元が渡す。開始時の値から単調時計の経過を差し引き、残り時間が増えたように見えても上限を延長しない。次の1組には既存180秒watchdog分より長い残り時間を要求し、終了確認では残り時間が正であることを要求する。p95を最悪時間の保証には使わない。
- 100件目にも割当・pending・時間の終了チェックを適用し、次のループがないことを理由に検査が抜けないようにする。

新しい証跡は`hundred-run-executions/<run-id>/`に開始・各回・中断を保存する。全100件成功または最初の通常失敗で停止した場合だけ、既存の`runs/<run-id>/{report.md,summary.json,transaction-events.jsonl}`へ集計する。いずれも`SoftwareAggregationOnly`、`hardwareExecutionVerified=false`、`productionRunner=false`と明示する。実機識別子や原画像本体は証跡へコピーしない。

## 検証方法

新規のSDK-less作業場所でCMakeの`NIKON_D810_SDK_ROOT`を空にしたことを確認してから、OperatorShellTestsをビルドする。

```powershell
cmake -S . -B build/wpf-m2-adapter -A x64 -DNIKON_D810_SDK_ROOT:PATH=
dotnet build tests/m3/OperatorShellTests/A0CameraStitcher.M3.OperatorShellTests.csproj -c Release --maxcpucount:1 --nodeReuse:false -p:UseSharedCompilation=false
./tests/m3/OperatorShellTests/bin/Release/net10.0-windows/A0CameraStitcher.M3.OperatorShellTests.exe --hundred-run-core
```

`--hundred-run-core`はテスト実行ファイル専用であり、アプリの起動引数ではない。テストは一時フォルダに作った匿名の結果と偽workflowを使う。カメラ接続の有無で結果を変えない。通常のOperatorShellTestsにも同じテスト群を含め、既存の1回・10回・100回起動拒否の回帰を確認する。

## 次に残る実機条件

本番起動経路との接続、同じAgent sessionの割当と本人操作、実機one-shot・10回計測、実測p95承認、host寿命との適合、100/100の実測、異常時試験は別途必要である。この内部部品や偽workflowの合格だけで実機を再開しない。
