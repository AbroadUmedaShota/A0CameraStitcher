# 製品要件

継続開発時追加指示: 最終吊り下げ環境は明日準備予定のため、それ以外の安定性・停止復旧を先行する。#345では両台撮影の既存180秒deadline内にA/B双方の非撮影preflightとCloseを追加し、両方成功するまで露光しない。実撮影時も各側を再確認し、StoreRemoved/reset等の拒否は維持。ウィンドウを閉じる要求で未送信の露光を止め、送信済み一枚の保存は継続する。#335ではc4=10分を変えず、実カメラ側終了から明示再開の動作を検証する。通常起動の自動再開・設定writeを追加しない。

開発時継続指示: 仮配置Cへの「いい感じ」「開発を続けて」と「図面撮る前提だから今はある程度」を受け、[両台撮影・確認用JPEG Merge・静止結果確認](WPD_STILL_REVIEW.md) を開発用に接続する。A/B原画像を保持し、Cの条件から仮の正規化座標変換を使う。画角対応・校正・物理DPI・図面の最終品質は未承認のまま、別の未校正確認レポートへ記録する。製品用承認profile/APIのゲートを緩めない。

同日の「続けて」に基づく #336 では、保存したpairを明示レポートで開き、全体/100%/200%表示と移動、原画像px単位の手動translation・撮り直しなしの再Mergeを開発用に追加する。原画像と以前の結果を保持し、新しい結果のv2レポートに元pairと直前レポートのhashを記録する。カメラ操作を行わない独立した確認画面であり、光学校正や製品品質の承認ではない。

追加の本人決定「それで行きましょう」（#338）: 現在の机上配置は最終環境と大きく違うため、操作・比較・調整条件の保存/復元と人工/保存画像での検証を先に整備する。未校正の保存画像画面へ半透明/差分/A/B切替と手動位置・中心回転・倍率を追加し、被覆する切り取り範囲を別revisionへ記録する。最終吊り下げ環境の幾何・レンズ・色・物理寸法の校正や図面品質受入は別工程とし、仮配置から承認を推定しない。

同方針の継続開発（#339）: 原画像座標で手動対応点を指定して残差を測り、最小二乗similarityの候補を明示操作で確認・適用できるようにする。指定点のRMS/最大残差は当てはまりの指標で、図面全体の精度受入ではない。点は画面内だけで保持し、別保存した条件・JPEGの既存来歴と未承認フラグを維持する。

続く #340 では、候補の計算に使う調整点と、計算から除外する検証点を分ける。用途別の点数/RMS/最大残差と候補適用前後を表示し、検証点なしは「未検証」とする。用途変更後の候補は再計算が必要。検証点に対するずれの数値も、図面全体の光学的な合格や統計的な独立性の保証とは扱わない。

#341 の継続開発で、対応点・用途・調整値の明示的な作業保存/復元を追加する。検証済みreviewと入力hashにひも付けた別のprivate作業ファイルを使い、既存JPEG/レポートは保持する。残差は再計算し、未適用候補は自動復元しない。最新ファイルの自動選択や品質承認とは分ける。

#342 の本人指示により、A/Bそれぞれのライブビューに水準器の左右・前後の受信整数角度と左右の傾き目盛りを表示する。既存フレームのヘッダーだけを読み、追加命令・設定変更はしない。取得不可・2秒以上の更新切れ・停止時は数値と目盛りを隠す。精度・センサー有効性は未検証の参考値とし、大きな前後傾斜・真下向きでは使えない場合があることを表示する。図面面との平行や合成品質の合否を決めない。

#335/#337 の継続開発では、通常画面を失敗時にも残し、停止理由と明示的なライブ停止/再開を追加する。再開は両workerのjoin、全OpenのClose、Start試行に対するStop成功が確認できたときだけ許可する。登録A/Bを再照合して新しいchannelを使い、古い映像/水準器は出さない。自動再接続・Start再送・撮影失敗後の自動再試行・失敗履歴の成功化はしない。本人は両台のc4ライブビューが10分と確認済みだが、時間の一致だけで温度や電池等の影響を除外せず、本体設定変更と長時間受入を別に記録する。

続く本人の選択「設定は10分のままで進めてほしい」により、c4は変更せず、終了後に理由を確認して明示再開する運用を採用する。この工程の受入を10分制限の回避や無中断の長時間表示へ置き換えない。

#343 は本人が次工程に同意した、同じ画面での二組の撮影フロー検証。明示試験optionだけで各組の静止結果描画と、再開後の新しいA/B・ライブC描画を確認して次組へ進む。各組の180秒deadline、原画像保持、未校正flags、失敗時の中断を維持する。初回のA受付成功後に画像通知が得られず、読み取り診断と既存AF操作でAのOutOfFocusを確認したため、撮影前後のDeviceReadyで合焦拒否を先に検出し、明確に表示する。診断の露出時間/フォーカスモード/電池は読み取りだけで、撮影内の自動AF・設定変更・自動再撮影は追加しない。

継続開発時の継続では、#343の事前StoreAdded通知（0x4004）を公開定義に照合して画像通知と区別する。撮影条件の再読取一回と、その後二回の事前イベント確認を必須にし、再変化・未回収画像・未知イベントでは撮影を拒否する。撮影後の同通知は失敗とする。設定write・カード操作・自動再撮影は追加しない。

開発時追加指示: 「機械的にある程度自動マージ＋手動の微調整」により、未校正Cにボタン式の平行移動候補推定と1px単位の手動調整を追加する。候補を目視確認できるよう表示し、フレームごとの再推定・rig profile学習・最終品質承認とは扱わない。詳細は[開発用C](WPD_PREVIEW_MERGE.md)。

開発時次工程の本人指示: A/B別の撮影とローカルJPEG保存、映像MergeとMerge後の表示確認を進める。現在は仮配置で壁に固定した資料を使い、最終的には机上を真上から撮る固定フレームへ移す想定。既存の撮影禁止の限定試作範囲をこの開発工程だけ拡張し、SDKなしの公開資料によるno-AF SDRAM撮影候補を調査・検証する。両Live View停止・Close、単発撮影、厳密な新規画像帰属、原子的原画像保存を要求する。0x9207、設定書き込み、カード削除・format、シャッター自動再試行は禁止のまま。Cはまず明示した固定パラメーターによる未校正の開発用合成確認とし、承認済みプロファイル・最終色幾何品質と同一視しない。最終設置後に再校正する。

開発時追加指示: 「AFと微調整の2種類でok」により、二台ライブビューの各側にAF単発と近く/遠くへの小さなフォーカス駆動を追加する（#331）。AF位置変更、露出・AFモード設定変更、撮影は含まない。AF完了応答を光学的合焦成功と同一視しない。

開発時追加指示: 「いちいち止めないで良いので、2台同時ライブビューまで走りきってください」により、既存A/B登録を使うSDKなしWPD開発用プレビュー（#330）の実機Start/Frame/Stopと二台sessionを承認済みとして実施する。この限定範囲では過去の一台Live制限・各回の再承認待ちより本指示を優先する。詳細と制約は[WPD A/B同時プレビュー](WPD_DUAL_PREVIEW.md)。ADR-0036全体、撮影・設定変更、製品統合の承認とはしない。

2026-09-21本人指示: STEP4（実写A0合成受入）までを今回の対象とし、受入反復は初回one-shotを含めて各系列最大5回へ変更する（ADR-0030）。旧ADRの10回/100回指定より本変更を優先する。過去の実機合格記録は履歴として保持する。

2026-10-13の所有者の決定（#293。合成は後回しにし、それ以外は100%を目指す）:

- D1: 合成ができるまでリリースしない。合成なしの二台は製品として出さず、二台は現在の評価経路（`CaptureRecoveryOnly`）のままとする。製品の位置づけを変えるADRは作らない。
- D2: 配布は社内のPCだけとする。Nikon SDKは同梱せず、使用許諾に同意した利用者が各PCに置く（ADR-0007、`CON-004`の内容と矛盾しない。具体化を`CON-004`の本文に追記した）。
- D3: 二台の実機受入は新しい系列で最大5組とする。USB抜去の合格線「削除0」は「抜去が原因の削除0」と読む。
- D5: 実機日1を前倒しする。日程の決定であり、要件は変えない。
- 二台のライブビューは、当面は`FR-LV-004`の形（選んだ側をLive、反対側を静止画。#291）、最終目標は`FR-LV-003`（#292）とする。撮影の後も同じ表示を使う#295は、#291の設計の確認（2026-10-13）で分けた後半であり、所有者の決定そのものではない。二台とも動く映像をMVPに含めるかは決まっていない（#234、`HG-0010`）。

開発時の所有者の指示: 保存JPEGの微小回転・倍率差検証（#325）を区切りに、開発順を「1台のPCで複数カメラの同時制御（まずD810二台とも継続更新するLive View）→ 映像Merge → 画像Merge」に変更する。従来の保存画像合成先行の順序より、この指示を優先する。既存の静止画像合成実装は保持する。使用許諾の調査は所有者から依頼を受けて実施し、通信仕様の利用・公開条件は[#326](https://github.com/AbroadUmedaShota/A0CameraStitcher/issues/326)で未解決事項として追跡する。これはADR-0036の未承認条項、WPD session上限の改定、実機操作の承認を代替しない。詳細は[移行計画](DUAL_LIVE_LICENSE_REVIEW_20261016.md)を参照。

開発時の再確認・記録訂正: 所有者は「2台同時制御ライブビューが最優先」と明示した。切り替え方式でも用途上実用になるという説明を、同時ライブビューを必須条件から外す指示と解釈したのは助手の誤りであり、その優先順位変更は撤回する。最優先の完成対象は1台のPCでD810二台を同時制御し、両方のLive Viewが継続更新されること（#292）。#291/#295の片側Live・切り替え方式やログ検証器だけで、この完成条件を満たしたとは扱わない。開発順は「二台同時制御Live View → 映像Merge → 画像Merge」を維持する。

開発時の到達順の明確化: 所有者は「まずAとBを同時にライブビューできるようにする → 映像をMergeする（C）→ 撮影して、jpgをMergeする」と指定した。最初にA/Bの二画面の継続更新を実現し、次に表示専用Cを作り、その後にLive View全終了を確認して撮影し、保存JPGをMergeする。Cの表示に先立って撮影JPGを作る工程へ優先順位を入れ替えない。厳密なframe/シャッター同期はこの指示では保証しない。

## 1. 目的

製品は、固定したNikon D810一台で静止平面原稿を一枚の原画像として撮影・保存する`SingleCamera`と、D810二台でA0級原稿を分割撮影して一枚の大画像へ合成する`DualCamera`を提供する。二台A0合成を主用途として維持しつつ、一台だけを用意できる環境でも撮影、Live View、read-only設定確認、結果確認、明示保存を安全に行える状態を目指す。

## 2. MVP利用条件

- 操作者は社内の電子化作業担当者。
- 被写体は平面に近い静止原稿。
- カメラ、レンズ、原稿台、照明は固定する。
- 操作者はactive transaction外で`SingleCamera`または`DualCamera`を明示選択する。接続台数からmodeを推定せず、`DualCamera`の一台不足を`SingleCamera`へ自動降格しない。
- 初期`SingleCamera`運用は`CAM-A`専用とし、SDKとWPDの双方でD810が厳密に一台だけ列挙され、登録済みWPD serial digestと一致する構成に限定する。`DualCamera`はD810二台をWindows 11 x64 PCへUSB接続し、Agent sessionごとに操作者がLive Viewを確認して`CAM-A`と`CAM-B`を明示割当する。
- 撮影中に他のテザー撮影ソフトはカメラを占有しない。
- active transaction中は操作者も物理シャッターを操作せず、active modeがその段階で処理するcameraをPhase 0ツールが排他的に使用する。
- JPEG Fine Lを使用する。
- ハードウェア同期装置は使用しない。
- 現行のNikon Camera Remote SDK経路ではsessionを同時に1台だけ開く。二台同時Live Viewは、ADR-0031のSDK capability・安全性gateを満たすまで有効化しない。注: 二worker Live View試作は開発用のCMake option `A0_BUILD_DUAL_PREVIEW_POC`（SDK有効ビルドでは既定OFF）の下に置く実験であり、本要件上の有効化には当たらず、gate未達の間の実機表示は各回の人の承認に限る。二台モードの当面のライブビュー（`FR-LV-004`）は、同時に開くSDK sessionを1つのままにして、選んだ側をLive、反対側を静止画で見せるもので、二台同時Live Viewには当たらない。

## 3. 機能要件

### カメラ制御

- `FR-MODE-001`: active transaction外で`SingleCamera`または`DualCamera`を明示選択し、mode、required camera aliases、profile IDをtransaction開始時に固定できる。active中のmode変更、接続台数からのmode推定、および`DualCamera`から`SingleCamera`への自動降格を禁止する。
- `FR-CAP-001`: 接続されたD810を列挙し、active modeが要求するaliasをfail closedで解決できる。初期`SingleCamera`は`CAM-A`だけを対象とし、登録済みWPD serial digestとSDK/WPD各exactly-one current-session projectionを要求する。`DualCamera`はAgent session開始時にSDK candidate source objectが厳密に二つであることを確認し、二候補を同時表示せず一台ずつLive View表示して、操作者が各候補を`CAM-A`または`CAM-B`へexactly onceで明示割当する。同一候補の二重割当、候補数が二以外、alias不足、Live View停止またはSDK source/capture session closeを確認できない場合はbindingを拒否する。ADR-0028のcontrolled `CaptureRecoveryOnly`だけは、source/capture sessionを閉じた後のopaque binding token保持だけを目的にSDK Moduleを残せる。
- `FR-CAP-002`: `SingleCamera`はWPD serial digestを永続identity、SDKを`exactly-one-current-session`選択として扱い、SDKの衝突するName/Interfaceを永続化しない。`DualCamera`の承認済みrelaxationは、candidate source object、candidate ordinal、列挙順、USB portを永続identityにせず、同じAgent sessionのmemory-only `DualIdentitySessionBinding`としてだけ`CAM-A/B`を保持する。Agent restart、USB reconnect、camera countまたはtopology変化、SDK manager再生成、任意のSDK error、またはWPD cleanup未確認で即時invalidにして、再binding完了までは`HardwarePending`とする。WPD cleanup未確認時はSDK API（`End`を含む）を呼ばずAgentを隔離・terminal化し、失効理由を上位へ返す。このinvalidationとAgent/binding-session terminal teardownではSDK Moduleをunloadする。WPD cleanup確認済みの有効bindingだけをpair terminal後も保持し、同一Agent-sessionの連続runでは再列挙しない。同一bindingの反復runnerはADR-0030の最大5回を上限とする。100回runnerの製品接続は今回の受入条件から外す。既定600秒はserver/request稼働時間予算であり、processの強制終了時刻ではない。安全な終了処理が予算を超える可能性があり、連続試験との適合を示す実機証拠は未取得である。最大5回の実機実施と合格には`FR-CAP-007`の段階gateを満たすことを要求する。Single identityをDualへ流用しない。安定した`a0.camera-agent.hardware-dual.v2`の既存payload/result shapeは変更せず、binding専用`a0.camera-agent.hardware-dual-binding.v1`とCaptureRecoveryOnly開始・同一ID照会・終了専用`a0.camera-agent.hardware-dual-capture-recovery-only.v1`を分離する。v2 capabilitiesにはCaptureRecoveryOnly操作名だけをadditiveに広告する。照合（binding）には確定からの期限は無く、失効の事象の正本は[HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md](HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md)とする。撮影開始スナップショットの5分、Agentの寿命600秒、撮影の受付に要る残り185秒の定義は[二台の撮影にかかわる時間の定義](HARDWARE_CAMERA_AGENT_DUAL_BINDING_V1.md#二台の撮影にかかわる時間の定義)に従う（#294）。
- `FR-CAP-003`: 2026-08-06に承認されたdedicated single-slot spoolをactive modeが要求する各cameraで使い、`SDK exactly-one card capture → WPD exact-one recovery → PC原本の再読込検証までの確定 → just-recovered object delete → empty-after確認`を完了する。`SingleCamera`は選択alias一台で完了する。`DualCamera`は再列挙せずsession bindingが保持するsource objectを`CAM-A`後に`CAM-B`の順で処理し、各aliasに対応するWPD objectをexactly oneで回収する。
- `FR-CAP-004`: device datetime cutoffによる画像帰属は採用しない。専用empty/cleared cardをsingle-slot transient spoolとして使い、撮影前にJPEG・NEF・動画・generic fileなど全camera payload objectが0件であることを確認し、SDK one capture後の唯一のexact JPEG objectをWPDで回収できる。PC `.partial`、JPEG・size検証、SHA-256、atomic rename、`original.jpg`再読込検証後に限り、そのobjectだけを削除して全payload 0件を再確認する。候補0件・複数件・遅延・曖昧画像、download/persist/delete失敗は削除せず、PC原本があれば保持して`FailedPartial`にする。`DualCamera`で対応するWPD aliasからexactly oneを回収できない場合も`FailedPartial`とし、他aliasの探索、他objectの削除、自動retryを行わない。
- `FR-CAP-005`: modeにかかわらず同時に一つのcapture transaction、一つのSDK source/capture session、または一つのWPD sessionだけを開く。SDK source/capture sessionとWPD sessionを重複させず、WPD access前に全Live ViewとSDK source/capture sessionをcloseする。ADR-0028のcontrolled Dual `CaptureRecoveryOnly`だけはModuleをmemory-only binding tokenのために保持できるが、WPD open中のSDK API、Live View、capture、enumeration、settings writeはすべて禁止し、WPD close後までSDK operationを開始しない。
- `FR-CAP-006`: タイムアウト、USB切断、片側失敗、曖昧画像を検出し、自動再試行せず、取得済み原画像と診断情報を保持して`FailedPartial`で終了できる。
- `FR-CAP-007`: 製品責任者がDual実機の撮影・回収方式を評価する目的で明示承認した場合に限り、承認済みrig profileなしで`CaptureRecoveryOnly`を開始できる。この経路もcurrent-sessionの`DualIdentitySessionBinding=Ready`、承認済みread-only capture profile、全Live View停止・SDK source/capture session close、ADR-0028のModule保持共存probe合格、pair-level exact-two/alias 1:1/両card empty/全WPD close preflight、CAM-A→CAM-B、canonical PC原本検証、exact-object cleanup、180秒watchdog、no retryを必須とする。WPD cleanup未確認時はSDK API（`End`を含む）を呼ばずAgentを隔離・terminal化し、失効理由を上位へ返して再bindingを必須とする。production adapterのpair-level preflight、read-only coexistence probe、CaptureRecoveryOnly backend・WPF経路を検証した後、対象と候補を照合して実機試験を行う。ADR-0030により最初のone-shotを含む一つの系列を最大5組とし、最初の失敗・未確定・割当失効で停止する。初回成功を確認して残りの組へ進み、6組目・補充撮影・自動retryを行わない。5標本の全時間値・最大値を記録し、p95は小標本の記述統計に限る。100組への昇格工程は要求しない。software合格を実機受入や性能承認へ読み替えない。rig profileを入力・偽装せず、合成・再合成・製品exportを開始せず、結果を`StitchOutcome=Pending`かつA0品質未承認として記録する。通常の`start-reserved-pair`はv2 payload/result shapeを維持し、ADR-0028例外の対象外であるproduction binding hostでは既存shapeの`PairDispatcherUnavailable`を返す。CaptureRecoveryOnlyの開始・同一ID照会・終了は別schema `a0.camera-agent.hardware-dual-capture-recovery-only.v1`を使う。ADR-0032で解釈を確定（2026-10-07）: 「組」はtransaction作成で数え、#226の保持済み原画像のbyte-identicalな書き出しは「製品export」に当たらない。
  二台の正常系列は#227の実機日に5組目（カメラ側の拒否`OutOfFocus`）で不合格となった。所有者の決定（#227、2026-10-13 #293 D3）により、二台の実機受入は新しい系列で最大5組として行う。
- `FR-LV-001`: `SingleCamera`の`CAM-A`または`DualCamera`で選択した一台について、Nikon SDK経由の対話的Live View開始、継続frame取得、停止を実行できる。Dual session bindingでは二候補を一台ずつ表示し、candidate ordinalはそのsession内だけで使用する。有限probeを継続Live Viewの合格証拠にせず、プレビュー画像は原画像・合成入力・撮影transactionの候補または保存証拠に使用しない。 ADR-0036で改定（Proposed、#307）: 「合成入力」を「保存する合成の入力」に絞り、承認済みrig profileで描く表示専用の合成Cを除く案。Cは保存せず、撮影・保存の合成の入力にもしない。承認までは本文のとおり。
- `FR-LV-002`: 承認済みspool transaction前に選択中Live Viewの停止とSDK source/capture session closeを確認する。SDK one capture、WPD recovery、PC `original.jpg`の再読込検証までの確定、single-object delete、empty-after確認が成功した後だけ、操作者が選択していた一台のSDK Live Viewを再開できる。ADR-0028のModule保持中もWPD open中のLive View再開は認めない。
- `FR-LV-003`: 将来の`DualCamera`は、SDK vendor documentationと実機PoCで二つの独立したsource/capture sessionによるLive View同時取得が安全に確認された場合に限り、CAM-AとCAM-Bを左右に並べた横長の確認画面を提供できる。各paneにはalias、`Live`/`映像停止`、最終frame受信時刻を表示し、片側停止を両側のLive表示または成功として偽装しない。厳密なframe同期、リアルタイム合成、previewの原画像・合成入力への採用は保証しない。実装・有効化の前に、二台開始/停止/例外/USB切断/撮影handoffで全sessionを停止・closeできること、WPD open中のSDK operation 0、binding再確認、capture transactionとの排他、および最大5回の明示承認済み実機検証を満たす。所有者は2026-10-13にこの目標（二台とも動く映像）を確認した（#292）。実現方式（1つのSDKを2プロセスで使う、HDMI出力の取り込み、WPD経由のPTPライブビュー命令）は決まっていない。HDMIの案はUSBだけの境界を、WPD経由の案はvendor operationを範囲外とする要件とADRを変えるため、どちらも改定を要する。MVPに含めるかは決まっていない（#234、`HG-0010`）。 ADR-0036で改定（Proposed、#307）: 所有者は2026-10-14に、操作者の画面を2台の映像を合成した1枚（C）にし、両側を動かすためにWPD経由のPTPライブビュー命令の案（B案）を今のスコープに入れると決めた（#306）。左右の二paneは技術担当者向けに残す案。B案の実現性は実機で未確認で、前提として所有者によるNikon SDKの使用許諾の確認が要る。承認までは本文のとおり。
- `FR-LV-004`: `DualCamera`の当面のライブビューとして、左右のpaneのうち操作者が選んだ1台をLive、反対側をそのカメラの最終frameの静止画で表示し、静止画の側には受信時刻を出す。「両方を更新」ボタンを押した時だけ1台ずつframeを取り直し、選んでいた側のLiveに戻る。自動の交互表示はせず、画面に「同時」「同期」とは書かない。同時に開くSDK sessionは1つのままとし（`FR-CAP-005`、ADR-0031）、previewを撮影・合成の入力や保存の証拠に使わない。撮影の後も同じ表示を使えるようにする（#295。#291の設計の確認（2026-10-13）で分けた後半で、所有者の決定が及ぶのは#291の当面の形まで）。所有者の決定（2026-10-13、#234、#291）。MVPの区分（`mvp=true`、優先度must）は所有者が決めたものではない。所有者の方針（2026-10-13、#293）で合成を除く100%の範囲に#291が入るため、`mvp=true`と置いた（※仮定）。ADR-0025・ADR-0031・[操作者画面仕様](OPERATOR_UI_SPEC.md)の改定と実装は#291・#295で行う（※未実装）。静止画が前の原稿を写している可能性がある時の扱い（注意の表示だけにするか、2段階の操作にするか）は決まっていない（`HG-0011`）。 ADR-0036で改定（Proposed、#307）: この当面の形は、B案（#292）が成り立つまでのCのframeの出どころとして残す案。自動の交互表示はしないを維持する。片側が静止画のCの扱いは`HG-0011`に含める。承認までは本文のとおり。

#### 実装状況と生成記録の区分（2026-09-12）

上記の実装状況は、PR #184/#185後のコードと[開発状況](CURRENT_STATUS.md)、[Phase 0Bの試験ゲート](PHASE0_TEST_PLAN.md#phase-0b-二台順次撮影)に基づく。実機確認待ちは維持し、この文書更新自体を撮影再開の承認としない。10回runnerの実装は、実機受入や100回runnerの完成を意味しない。

[正規化要件](../.autodev/requirements/normalized.json)、[計画](../.autodev/plan.json)、[未決事項](../.autodev/requirements/unresolved_questions.json)は、2026-10-14にADR-0030・ADR-0032と2026-10-13の所有者の決定に合わせて再正規化した（#297）。それまで残っていた「one-shot前まで・10/100 runner未実装」という2026-08-31生成時点の説明は解消した。ADR-0030により100/100初回成功は今回の受入条件ではない。mode別の全時間値・最大値の記録と、既存のhuman／transport gateは縮小・完了扱いにしない。

#### PC直接保存の評価境界（2026-09-16）

SDKのPC直接保存は、dedicated single-slot spoolを置き換える候補経路として評価中であり、現行MVPの受入済み経路ではない。PR #199〜#202を含む`main`で安全入口、SDRAM事前gate、匿名診断、Item完了判定を実装したが、PR #202統合前の実機2回はいずれも`FailedPartial / image_event_timeout`でPC原本0件だった。従来spool経路のSingleCamera合格と、PC直接保存の未受入を分けて扱う。

PC直接保存を採用するには、撮影前SDRAM empty、exact-oneのpost-baseline SDK Item、完全JPEG download、`original.jpg.partial`からatomic `original.jpg`への確定と再読込検証、SaveMedia復元、SDK close、事後card payload 0、retry／fallback／delete／format 0を同一transactionで証明する。PR #202のSDRAM事前gateと追加診断は2026-09-17に`main`へ統合したが、それだけでは本項の実機受入完了を意味しない。

### キャリブレーションと合成

- `FR-STI-001`: active modeで必要な各cameraのレンズ歪み補正値を保存できる。
- `FR-STI-002`: `DualCamera`固定リグの幾何変換、重複領域、seam、crop領域をprofileとして保存できる。
- `FR-STI-003`: `DualCamera`では承認済みrig profileの固定変換を適用し、撮影ごとの差が承認済み自動補正範囲内の場合だけ一時的な残差補正を適用できる。撮影ごとに自由なhomographyを再推定せず、補正結果でprofileを自動更新しない。初期`SingleCamera`は`7360×4912` canonical JPEGを一切画像処理せずbyte-identicalに扱い、crop、lens補正、DPI、物理原稿寸法を保証しない。
- `FR-STI-004`: `DualCamera`では左右の露出・色差を補正し、継ぎ目を目立ちにくく合成できる。
- `FR-STI-005`: mode別の自動補正量または補正後品質が承認済み範囲を超えた場合、成功扱いにせず、物理調整または再キャリブレーションが必要な理由と測定値を出せる。初期`SingleCamera`の補正は`NotApplicable`とする。
- `FR-STI-006`: 設置アシスタントは、active modeに対応する承認済みprofileを使って、`補正不要`、`自動補正して続行`、`物理調整が必要`を判定できる。`DualCamera`では全画角、重複、カメラ設定整合、および位置・回転・倍率・露出・色を評価する。`SingleCamera`では選択cameraのidentity、設定、画角、cropおよび承認済み一台品質条件だけを評価し、二台間の重複・seam・相対補正を要求しない。閾値を内蔵せず承認済みprofileから受け取り、draft・不整合・期限切れのprofileでは本番撮影を許可しない。

### 保存と操作

- `FR-DATA-001`: PCに確定・再読込検証済みの、active modeで取得した各cameraの`original.jpg`を成功・失敗にかかわらず保持する。PC原本が唯一の製品正本であり、カメラカードは一過性の転送元とする。承認済みsingle-slot spoolではPC原本確定後にexact just-recovered WPD objectだけを削除し、empty-afterを確認する。existing cardのbulk delete/formatは禁止する。
- `FR-DATA-002`: 撮影トランザクション、カメラ別時刻、状態、ファイルサイズ、SHA-256、結果、エラーを記録する。
- `FR-EXP-001`: `DualCamera`では合成JPEGを指定フォルダへ保存できる。`SingleCamera`では検証済み`7360×4912` canonical `original.jpg`をbyte-identicalな単一撮影出力として、操作者が選択したfixed-local folderへ明示保存できる。network、UNC、device path、ADS、removable、reparse chainを拒否し、合成済みとは表示しない。
- `FR-UI-001`: 一画面の撮影ダッシュボードで、明示選択したmode、起動セッションの排他同意、modeが要求するcameraの接続・identity・設定・card・Live View状態、profile ID・版・期限、設置と自動補正可否、mode別処理進捗、撮影・合成・保存を分離した結果、赤Blocker・黄Caution・青Infoを表示できる。`SingleCamera`で非required cameraの不在をBlockerにしない。
- `FR-UI-002`: `Ready`または`ReadyWithCorrection`の場合だけ追加確認なしの一回操作で撮影し、active transaction中のmode変更、競合操作、二重開始を禁止できる。標準導線は「位置合わせ → 撮影 → 詳細確認 → 採用 → 次の原稿」とし、撮影後は結果確認へ移動して明示操作まで留まる。原画像は採用前に自動保存し、保存済み・合成完了・人の採用済みを分離する。採用記録の成功後だけ次の準備へ進み、撮り直しは旧画像を保持した新しい撮影IDで行う。CaptureRecoveryOnlyでは原画像の確認であり、合成・A0品質の採用とは扱わない。設置・校正、read-onlyカメラ設定、結果確認後の明示書き出し、read-onlyの新規撮影準備を提供する。別job再合成は`DualCamera`だけに提供し、`SingleCamera`では`StitchOutcome=NotApplicable`として理由を表示する。画面配置・状態契約は[操作者画面仕様](OPERATOR_UI_SPEC.md#2026-09-21-改訂設計撮影ごとの詳細確認)に従う。本改訂は設計合意であり実装完了ではない。
- `FR-UI-003`: 現行の一台選択式Live Viewの開始・停止、対象カメラ、接続状態、撮影前停止と撮影後再開の失敗を表示できる。ADR-0031 gateが未達の間、二台同時Live Viewは操作可能に見せない。gate達成後の二台表示では、左右各paneのalias・Live/停止・最終frame時刻を表示する。gate未達の間の二台モードの表示は`FR-LV-004`に従う。previewを原画像・合成入力として扱わない。

## 4. 非機能要件

- `NFR-PLAT-001`: Windows 11 x64で動作する。
- `NFR-PERF-001`: mode別に撮影開始から製品JPEG確定までの全時間値・中央値・最大値を最大5標本で記録する。p95を併記しても小標本の記述統計とし、性能目標の新規承認を推定しない。SingleCameraの既存承認値は当時の条件だけに適用する。
- `NFR-REL-001`: ADR-0030によりSingleCameraとDualCameraそれぞれ最大5回の初回試行で受入を確認する。予定5回が全て成功し、誤pair・原画像消失・曖昧画像採用・retry・回復不能停止が0件であることを要求する。最初の失敗で停止し、不足分を補充しない。mode間で結果を代用しない。100回相当の耐久性は主張しない。ADR-0032で改定（2026-10-07）: 試行はtransaction作成で数え、シャッター0のソフト起因の拒否は条件付きで回数の予算に数え合否に数えない。
- `NFR-OBS-001`: mode、required aliases、Live View開始・初回frame・停止、WPD baseline/open/close/reopen、SDK card capture/open/close、画像検出、転送、`.partial`、JPEG・size検証、SHA-256、原子的保存、再読込検証、single-object delete、empty-after、Live View再開、合成または`NotApplicable`理由、export、エラーの時刻を記録する。ADR-0028のcontrolled Dual経路では、SDK Module retained、SDK source/capture session close、WPD open/close、Agent/binding-session terminal Module unload、coexistence probeの結果とWPD open中SDK operation 0を匿名で記録する。datetime診断結果、handoffの要求数・試行数・完了数・終端状態、WPD capture command未送信に加え、profile ID・版、baseline校正残差、提案・適用・拒否した一時補正量、拒否理由を確認可能にする。Dual bindingの公開・保存可能な証拠は匿名alias、provider、version、`confirmedAt`、`invalidationReason`だけとする。
- `NFR-SEC-001`: 原画像と履歴をローカル保存し、MVPではクラウド送信しない。実識別子とSDK配布物をコミットしない。Dual bindingのpreview、raw identifier、serial、source objectまたはcandidate ordinalを保存・公開しない。
- `NFR-MEM-001`: 推奨32GB、最低16GBのPCでピークメモリを計測し、上限を実機PoCで確定する。合成を除く経路のピークメモリは、操作PC-1で計測して記録する（#293）。

## 5. 制約

- `CON-001`: USB接続したNikon D810を、明示modeに応じて厳密に一台または二台使用する。初期`SingleCamera`はSDK/WPD双方でexactly-one physical D810を要求する。`DualCamera`は同じAgent sessionで厳密に二候補を要求し、操作者によるmemory-only CAM-A/B bindingが完了している間だけ使用する。三台以上は対象外とする。
- `CON-002`: ハードウェアシャッター同期を追加せず、実シャッター開口時刻差の上限を保証しない。
- `CON-003`: attempted hybridのdevice datetime cutoffはhardware evidenceでRejectedであり、2026-08-06に承認された専用empty/cleared card single-slot spool以外の帰属・削除経路を実装・実行しない。SDK source/capture sessionとWPD sessionは重複させず、vendor operation（`0x9207`を含む）、existing cardのbulk delete/format、retryは禁止する。deleteはPC原本の再読込検証後のexact just-recovered WPD objectだけとし、zero/multiple/late/invalid/download/persist/delete failureでは実行しない。Dual binding完了時は全candidate Live View停止とSDK source/capture session closeを確認してから内部`DualIdentitySessionBinding`を`Ready`にし、撮影中はbound source objectを再列挙しない。ADR-0028のcontrolled `CaptureRecoveryOnly`ではModuleだけを保持できるが、WPD open中のSDK API operationは0とし、Agent/binding-session terminalまたはinvalidationでModuleをunloadしてbindingをinvalidにする。 ADR-0036で改定（Proposed、#307）: vendor operationのうち、WPD経由のライブビューの3命令（開始・画像の取得・終了）だけを例外とする案。撮影とカードに触る命令（`0x9207`を含む）、retry、SDKとWPDの重なりの禁止は変えない。承認までは本文のとおり。
- `CON-004`: Nikon SDK、ライセンス対象バイナリ・資料、実カメラ識別子をリポジトリへ含めない。内部評価利用と製品再配布を別々に承認する。製品の配布ではNikon SDKを同梱せず、使用許諾に同意した利用者が各PCに置く。配布先は社内のPCに限る（所有者の決定 2026-10-13、#293 D2）。OpenCV等の他のnative依存の再配布と導入物の範囲は`HG-0005`で未承認のまま。
- `CON-005`: 動体、手持ち、近距離立体物、リアルタイム動画は対象外。

## 6. MVP対象外

- NEF/RAW現像
- 16-bit TIFF/BigTIFF出力
- Live Viewプレビュー画像の原画像・合成入力への使用
- GPU必須処理
- 遠景・球面・円筒パノラマ
- 三台以上のカメラ
- 自動露出、オートISO、AFを使った撮影
- カメラファームウェア変更
- 同一トランザクションの自動再試行
- 接続台数によるmode自動選択、`DualCamera`から`SingleCamera`への自動降格
- 初期`SingleCamera`でD810二台を接続したまま片方だけを操作すること
- 別契約が承認される前のアプリからの撮影設定write
- existing cardのbulk deleteまたはformat
- vendor operation（`0x9207`を含む）の実装・送信
- 撮影ごとの自由なhomography再推定、補正結果によるリグプロファイルの自動学習・自動更新
- モーター等を使ったカメラ位置・角度・撮影距離の自動物理調整

## 7. MVP受入条件

MVPの受入は、以下をすべて満たした時点で人間が判定する。

1. 操作者が`SingleCamera`または`DualCamera`を明示選択でき、active中に変更されず、`DualCamera`が一台不足しても自動降格しない。
2. 共通安全契約として、原画像を失わず、失敗時も取得済み画像を保持し、SDK source/capture sessionとWPD sessionの非重複、exact-object cleanup、no retry、operator-session排他、180秒watchdogを満たす。ADR-0028のcontrolled Dual `CaptureRecoveryOnly`だけはbinding token保持のModule残留を認めるが、WPD open中のSDK operationは0とする。
3. `SingleCamera`では登録済みWPD serial digestとSDK/WPD各exactly-one current sessionから`CAM-A`一台だけを解決し、一操作で`7360×4912` canonical original一件を確定する。他aliasの処理とstitch jobを開始せず、`StitchOutcome=NotApplicable`を表示して、操作者が選択したfixed-local folderへbyte-identicalに明示保存できる。
4. `DualCamera`ではAgent sessionごとに厳密に二候補を一台ずつLive View表示し、操作者が`CAM-A`、`CAM-B`を明示割当して全Live View停止とSDK source/capture session closeを確認する。ADR-0028のcontrolled `CaptureRecoveryOnly`ではModuleをbinding tokenだけのために残せる。そのmemory-only bindingが有効な間だけbound source objectを再列挙せず順次撮影し、対応WPD aliasから各画像をexactly oneで回収して同一transactionとして確定できる。
5. `DualCamera`では承認済みテスト原稿で位置ずれ、二重像、色差、継ぎ目が合意済み基準内であり、再合成できる。
6. mode別に今回の最大5回受入と全時間値・最大値を確認し、USB再接続など個別異常系の結果を記録する。初回one-shotを5回に含め、失敗時はその系列を不合格として止める。SingleCameraとDualCameraの証拠は相互代用しない。ADR-0032で改定（2026-10-07）: シャッター0のソフト起因の拒否の扱い、USB抜去の別系列、保存の合格線はADR-0032に従う。
   2026-10-13の所有者の決定（#293 D3）: 二台の正常系列は新しい系列で最大5組とし、USB抜去の不変条件「カード上のobjectの削除0」は「抜去が原因の削除0」と読む（抜去より前に正常に済んだexact cleanupは数えない）。
7. 未解決の安全・SDK利用・再配布・mode別品質・リリースゲートがない。
8. 一台選択式Live Viewが表示でき、撮影時にSDK source/capture sessionとWPD sessionを重複させず、安全に停止、WPD baseline、SDK一回card capture、WPD recovery、再開できる。ADR-0028のModule保持中もWPD open中にSDK operationを行わない。
9. active modeに対応する承認済みprofileを使って設置状態を判定し、範囲内の差だけを自動補正し、範囲外では成功扱いにせず物理調整または再キャリブレーションを案内できる。

二台A0の数値品質基準は`HG-0001/0002`で確定する。`HG-0003B`はADR-0025のsession-local operator binding relaxationとして解消済みだが、Issue #9のcore・binding protocol・WPF・capture backendと実機の最大5回受入が合格するまでは`HardwarePending`であり、操作者の誤割当riskは残る。実シャッター同期は保証しない。一台の初期出力・profileはADR-0024を維持し、今回のrun数はADR-0030で最大5回へ更新した。2026-08-26のHG-0009承認は当時の条件で保持し、新しい条件の性能承認を推定しない。リリースは合成ができてから行い、合成なしの二台は製品として出さない（#293 D1）。
