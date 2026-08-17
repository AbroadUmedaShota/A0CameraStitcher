# A0 Camera Stitcher

社内名称「大判撮影ツール（A0対応）」の開発リポジトリです。製品は、Nikon D810一台で検証済み原画像を撮影・保存する`SingleCamera`と、固定したD810二台で静止平面原稿を順次撮影して一枚へ合成する`DualCamera`を明示選択できる構成を目標とします。接続台数からmodeを推定せず、二台構成の不足時に一台構成へ自動降格しません。

## 現在の段階

総合状態は`in-progress`です。ADR-0024により最初の`SingleCamera`をCAM-A専用へ固定し、WPD serial digest＋SDK/WPD各exactly-one current-sessionのidentity-v3、アプリ内30日read-only profile承認、操作者選択fixed-local folder、byte-identical `7360×4912` canonical original export、対話的継続Live View v2をsoftware実装しました。実WPFからD810を撮影・export・継続表示した合格証拠ではありません。実撮影は専用empty spoolと明示再開を待ち、10回characterization後のp95承認（`HG-0009`）と100件受入が残ります。DualCameraは二台前提を維持し、SDK identity collisionにより別laneでBlockedです。

DualCameraのsoftware-only側では、Dual専用schema `a0.camera-agent.hardware-dual.v2`の4操作（capabilities、pair予約、予約済みpair開始、同一ID結果照会）、durable pair store、厳密なidentity／capture profile／rig profile／operator confirmation／180秒deadlineの事前検証を実装済みです。fake backend限定でCAM-A→CAM-Bを各一回・自動retry 0で実行し、A失敗時はBを開始せず、B失敗時はA原本を保持し、複数terminal journalを再起動後も同一IDで照会できます。ただしproduction Dual Named Pipe／Agent host、実SDK・WPD・camera backend、製品composition／WPF実撮影は未接続で、既定経路は`PairDispatcherUnavailable`／`HardwarePending`のままです。

第三者向けの現在地、5分デモ、主張可能範囲は[Phase 0 二台カメラ・ショーケース](docs/PHASE0_SHOWCASE.md)に集約しています。要約すると、一台／二台のmode-aware application contractと二台順次撮影の安全なsoftware contractは提示可能です。committed済みの匿名証拠には一台接続時の記録がありますが、これは現在のlive接続状態を断定するものではありません。実アプリ一台撮影、実機二台撮影、A0品質の受入はいずれも未完了です。

PCへ`.partial`、JPEG・size検証、SHA-256、atomic rename、再読込検証を完了した`original.jpg`だけを製品上の正本とします。カメラカードは一過性の転送元で、永続保持を要件にしません。承認済みの専用empty/cleared card single-slot spoolでは、撮影前にJPEG以外も含むcamera payload objectが0件であることを確認し、その後にjust-recovered WPD objectだけを削除して再びpayload 0件を確認します。候補0件・複数件・遅延・無効画像、download/persist/delete失敗では削除せず、PC原本があれば保持して`FailedPartial`にします。existing cardのbulk delete/format、vendor operation、retryは禁止です。

## MVPの前提

- 対象: 静止した平面原稿。二台構成はA0級。一台構成は`7360×4912`原画像のbyte-identical保存だけを保証し、対象原稿サイズ・DPI・crop・lens補正・物理寸法は保証しない
- mode: `SingleCamera`または`DualCamera`をactive transaction外で明示選択し、開始時に固定する
- カメラ: `SingleCamera`は登録済みNikon D810を厳密に一台、`DualCamera`は登録済みD810二台と固定リグ
- 接続証拠: committed済み匿名記録にはD810一台のcheckpointがある。現在のlive接続台数・aliasはこのREADMEから断定せず、実行時のread-only inventoryで確認する
- 接続: Windows 11 x64 PCへUSB接続
- 制御: WPD baseline/recoveryと、カメラカードへ一回撮影するNikon SDK、および一台選択式SDK Live View
- 入力: FX JPEG Fine L
- 出力: `SingleCamera`はcanonical `original.jpg`のbyte-identicalな明示export（合成なし）、`DualCamera`は合成JPEG
- 時間目標: `DualCamera`はp95 10秒以内を暫定目標とする。`SingleCamera`はone-shot後の10回でp95を測定し、`HG-0009`で承認後に100件連続受入を行う
- 保存: PCへ確定・再読込検証済みの原画像を保持。カメラカードは一過性の転送元で、承認済みsingle-slot spoolではexact WPD objectだけを削除して空状態を確認する

## ドキュメント

- [Phase 0 二台カメラ・ショーケース](docs/PHASE0_SHOWCASE.md)
- [現在の開発状況](docs/CURRENT_STATUS.md)
- [製品要件](docs/PRODUCT_REQUIREMENTS.md)
- [アーキテクチャ](docs/ARCHITECTURE.md)
- [Phase 0実機検証計画](docs/PHASE0_TEST_PLAN.md)
- [Phase 0 readiness](docs/PHASE0_READINESS.md)
- [D810 PC制御項目](docs/D810_PC_CONTROL_CAPABILITIES.md)
- [M2オフラインpre-gate](docs/M2_PRE_GATE.md)
- [アプリ機能検証計画](docs/FEATURE_VERIFICATION_PLAN.md)
- [M3 simulated統合基盤](docs/M3_SIMULATED_FOUNDATION.md)
- [アプリ構成レビューと実装計画](docs/APP_ARCHITECTURE_REVIEW_AND_IMPLEMENTATION_PLAN.md)
- [Hardware Camera Agent v1](docs/HARDWARE_CAMERA_AGENT_V1.md)
- [ロードマップ](docs/ROADMAP.md)
- [意思決定記録](docs/DECISIONS.md)
- [旧D750参照会話（履歴のみ）](docs/REFERENCE_CONVERSATION.md)

## Phase 0 CLI

```powershell
pwsh -File .\scripts\Test-Phase0Readiness.ps1 -Stage Single
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 '-DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk'
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
build\Debug\A0CameraStitcher.Phase0.exe sdk-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe wpd-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe spool-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe wpd-correlation-status --alias CAM-A
build\Debug\A0CameraStitcher.Phase0.exe live-view --alias CAM-A --duration-seconds 300
build\Debug\A0CameraStitcher.Phase0.exe live-view-handoff --alias CAM-A --count 10 --frames 1
```

Phase 0Bの旧identity-v2登録は履歴診断用checkpointとしてのみ保持します。列挙順、USB port、衝突するSDK Name/Interface digest、旧mapを二台のproduction binding根拠にしません。

`inventory`はread-onlyであり、未登録個体を`CAM-A/B`へ自動割当てしません。`bind-cross-transport-identity`と旧identity-v2 mapはlegacy diagnostic／checkpointであり、production `Ready`の登録手順ではありません。`HG-0003B`でdocumented providerが承認され、CAM-A/Bそれぞれのlocal proofと二台inventoryのeach alias exactly onceが一致するまではdefault `Blocked / identity_strategy_unresolved`です。

```powershell
build\Debug\A0CameraStitcher.Phase0.exe bind-cross-transport-identity --alias CAM-A --single-camera-connected-confirmed # legacy diagnostic only
# production ReadyにはHG-0003B承認provider、CAM-A/B local proof、each alias exactly onceが別途必要
build\Debug\A0CameraStitcher.Phase0.exe verify-dual-identity # legacy map diagnostic; identity_strategy_unresolved until HG-0003B provider approval
# identity合格後、二台の専用spoolが双方emptyかread-only確認
build\Debug\A0CameraStitcher.Phase0.exe verify-dual-spools
```

`HG-0003B`承認provider、CAM-A/B local proof、each alias exactly once、二台の専用empty spool、全安全確認が揃った後だけ、実機pairを次の順で段階実行します。

```powershell
build\Debug\A0CameraStitcher.Phase0.exe hybrid-capture-pair --count 1 --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed
build\Debug\A0CameraStitcher.Phase0.exe hybrid-capture-pair --count 10 --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed
build\Debug\A0CameraStitcher.Phase0.exe hybrid-capture-pair --count 100 --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed
```

`hybrid-capture-pair`は最初に共通のdual identity検証を必ず実行し、SDK/WPD各2台、CAM-A/B各1、unbound 0でなければ匿名の事前確認証跡を残し、card accessとcaptureを行わずexit 5で停止します。合格後は各pairを一つの180秒watchdogで管理し、CAM-Aのverified PC originalとexact cleanupが完了した後だけCAM-Bを開始します。A/Bいずれかの失敗で直ちに停止し、自動retryは0です。匿名summaryは全attempted pairの所要時間sample数とnearest-rank p50/p95/maxをmsで保存しますが、Phase 0の合否には使いません。二台は順次撮影であり、実シャッター同期は保証しません。

二台異常系は、両カードをemptyと確認したうえで`hybrid-fault-pair`を使います。`--alias CAM-A|CAM-B`は省略不可です。選択bodyのSDK card captureと完全close後、WPD recovery open前にoperator gateがreadyとなった時だけ指定bodyのUSB切断または電源断を行います。CAM-A異常ではCAM-Bを開始せず、CAM-B異常ではCAM-Aの検証済みPC原本を保持します。いずれも未確定bodyの原本化・削除・自動retryを行わず、新しいrun IDのtransactionを要求します。

```powershell
build\Debug\A0CameraStitcher.Phase0.exe hybrid-fault-pair --alias CAM-A --scenario usb-disconnect --operator-gate pair_a_usb --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed
build\Debug\A0CameraStitcher.Phase0.exe hybrid-fault-pair --alias CAM-B --scenario power-off --operator-gate pair_b_power --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed
```

CAM-A完了後・CAM-B開始前のプロセス終了試験は`hybrid-interrupt-pair`を使います。CAM-Aのverified PC originalとexact cleanupが完了すると中断専用gateがreadyになります。その時点でPhase 0プロセスを終了し、`continue` markerは作成しません。このgateはmarkerやtimeoutで正常復帰せず、CAM-Bへ進めません。再起動後、開始時に表示されたrun IDで`report --run-id`を実行し、`after-CAM-A-before-CAM-B`、CAM-A原本保持、retry禁止、新規transaction必須を確認します。

```powershell
build\Debug\A0CameraStitcher.Phase0.exe hybrid-interrupt-pair --operator-gate pair_boundary_exit --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed
build\Debug\A0CameraStitcher.Phase0.exe report --run-id <表示されたrun-id>
```

`report --run-id`はpairのdurable event logから途中停止段階を匿名診断します。進行中の撮影を停止済みと誤判定しないよう、camera sessionを開かないreport処理自体もoperator-session camera-control leaseで直列化します。途中停止・terminal failure・不整合証跡を成功や自動retryへ変換せず、新規transactionを要求します。

`spool-status --alias CAM-A`はread-only WPD sessionを一回だけ開き、folder/functional nodeを除く全payload件数だけを匿名保存して閉じます。Object ID・名前・実識別子は保存せず、capture、vendor operation、settings、deleteは0件です。`wpd-correlation-status --alias CAM-A`はread-onlyでWPD full close/reopenとdevice/object datetimeを診断しますが、clock cutoffは帰属根拠に使いません。

`hybrid-fault-single`は空spoolでone-shotが合格した後だけ使用します。SDK one captureとSDK close後、WPD recovery open前にoperator gateを出し、`usb-disconnect`または`power-off`を一回だけ試験します。異常後は`FailedPartial`、delete/retry 0、新規transactionでのみ復旧する契約です。

2026-08-09のoperator判断により、物理的な電源再投入・再起動と実`power-off`復旧subtestはPhase 0の必須合否から除外しました。`power-off` CLIとfake contractは安全回帰用に残します。USB切断、software process再起動、二台identity、接続順・port確認、empty spool、1/10/100 pairはスキップしません。

旧`capture-single`、`capture-pair`、`stability`はfake contract専用です。`--transport sdk`または`wpd`はcamera sessionを開く前に拒否し、実機経路は確認付き`hybrid-capture-single`と`hybrid-capture-pair`だけに限定します。実SDK/WPDへ触れるコマンドはoperator-session-wide named OS leaseを保持するため、同じWindowsログオンsession内の別processとの同時実行もfail closedになります。別ユーザーsessionやserviceからの起動はMVP運用外とし、installer／運用policyで禁止します。

旧`CAM-A` continuity証拠のうちWPD側は履歴として保持しますが、SDK側の結論はephemeral MAID source object IDを使っていたため無効化しました。現在有効なidentity-v2 checkpointは`CAM-B`一台です。Standalone Live Viewは5分04秒・2,424 frame、停止、SDK close、preview非保存に成功し、別プロセスでの再起動後も1 frame取得と正常終了を確認していますが、これはidentity-v2 continuityや二台撮影の証拠には読み替えません。撮影を含むone-shot、10/10、handoff 10回は承認済みspool経路で今後実施します。

一台の設定read-only診断 [run-1786040075194-1](docs/evidence/phase0/run-1786040075194-1/report.md)では、SDKが返した値としてJPEG Fine、L 7360×4912、S、1/6秒、F8、ISO 64、WB Preset 1、focus opaque値1を取得しました。FileTypeはnot-advertisedです。撮影設定write、capture、Live View開始、WPD、deleteは行わずSDK sessionを閉じました。MAID control-plane callback登録は既存`CapSet`を使い得るため、証拠上で撮影設定writeと区別しています。native command-trace testとfocus値の意味確定が残るため、この検証はPartialです。

2026-08-07の一台再検証でも、[設定read-only](docs/evidence/phase0/run-1786077278290-1/report.md)、[10 frame Live View](docs/evidence/phase0/run-1786077291889-1/report.md)、[終了後OFF確認](docs/evidence/phase0/run-1786077302493-1/report.md)に合格しました。実撮影、WPD、card確認、deleteは実行していません。

Nikon SDKは本人同意済みで、`.tools/nikon/d810-remote-sdk`へローカル隔離配置し、CMake変数`NIKON_D810_SDK_ROOT`で参照します。SDK配布物、実カメラ識別子、実写画像はcommitしません。

## M2 pre-gate CLI

`A0CameraStitcher.OpticalPlanner.exe`は、明示入力されたDPI、frame回転、二枚の配置、重複pixel、cropからA0幾何候補を計算します。`a0_m2_setup`は、明示入力された目標値・自動補正上限から`ready`、`ready-auto-correction`、`physical-adjustment-required`を判定します。どちらも未承認の数値を既定値にせず、最終リグやA0品質の承認には使いません。詳細は[M2オフラインpre-gate](docs/M2_PRE_GATE.md)と[アプリ機能検証計画](docs/FEATURE_VERIFICATION_PLAN.md)を参照してください。

## M3 application shell

```powershell
pwsh -NoProfile -File .\scripts\Test-M3Simulated.ps1
dotnet run --project .\src\m3\OperatorShell\A0CameraStitcher.M3.OperatorShell.csproj -c Debug
dotnet run --project .\src\m3\OperatorShell\A0CameraStitcher.M3.OperatorShell.csproj -c Debug -- --simulated
```

引数なしでは起動モード選択画面を開きます。`--simulated`のWPF shellとNamed Pipe agentは実機非接続のfakeであり、画面、IPC、保存物に`Simulated`を常設して実D810、Nikon SDK、WPDへ接続しません。詳細は[M3 simulated統合基盤](docs/M3_SIMULATED_FOUNDATION.md)を参照してください。

実機一台画面はC++ Camera Agentを別processの`--serve-once`として起動し、WPF process内へNikon SDK/WPDを読み込みません。起動しただけではcameraへ接続せず、画面上の明示同意とread-only状態確認を要求します。別の実機画面とのoperator-session排他、client transaction IDのcamera access前永続化、同一IDの結果照会、no retry、SingleCameraでのstitch `NotApplicable`、FailedPartialを含む再読込済みoriginalのbyte-identical明示exportを実装しています。

```powershell
cmake -S . -B build-sdk -G "Visual Studio 17 2022" -A x64 '-DNIKON_D810_SDK_ROOT=.tools/nikon/d810-remote-sdk'
cmake --build build-sdk --config Release --target A0CameraStitcher.CameraAgent
dotnet run --project .\src\m3\OperatorShell\A0CameraStitcher.M3.OperatorShell.csproj -c Release -- --hardware-single --camera-agent .\build-sdk\Release\A0CameraStitcher.CameraAgent.exe
```

Camera Agentは`%LOCALAPPDATA%\A0CameraStitcher\camera-agent\approved-single-capture-profile.json`が存在し、CAM-A、期限、read-only observed settingsが一致する場合だけ`Ready`にします。WPFの「観測値を30日プロファイルとして承認」は現在の観測値をlocal profileへ保存しますが、camera settingは変更しません。identity-v3は`%LOCALAPPDATA%\A0CameraStitcher\phase0\single-identity-v3.json`です。これらはsoftware boundaryであり、製品撮影合格の主張ではありません。wire、journal、profile schemaの詳細は[Hardware Camera Agent v1](docs/HARDWARE_CAMERA_AGENT_V1.md)を参照してください。

## 公式根拠

- [Nikon SDK downloads](https://sdk.nikonimaging.com/apply/)
- [Nikon SDK information/FAQ](https://sdk.nikonimaging.com/information/en/)
- [Nikon D810 specifications](https://nij.nikon.com/products/lineup/slr/d810/spec.html)

## 取扱い

社内用privateリポジトリです。Nikon SDK、ライセンス対象資料、カメラ固有識別子、実写サンプル、顧客原稿、生成物、認証情報はコミットしません。
