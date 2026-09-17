# A0 Camera Stitcher 次工程整理（2026-09-17）

## 1. 結論

現在の判定は`in-progress`である。ソフトウェアだけで確認できる安全境界は広く実装済みだが、次の三つは未完了である。

1. `CAM-A`一台によるPC直接保存の実機1回確認
2. D810二台の実機撮影・回収・保存の1回、10回、100回確認
3. 実写A0合成の品質基準、固定rig、権利処理済み画像corpusの決定と検証

表示中の`SIMULATED`画面は操作設計の確認用であり、実機試験結果には含めない。SingleCamera、DualCamera、A0合成品質、製品配布のいずれも、シミュレーション結果だけでは合格にしない。

基準sourceは`712ba9d298ef01ad413f825b83c3e4abfaef1f16`である。保存済み候補ではSDKなしRelease CTest 23/23、licensed focused CTest 6/6が合格しているため、この整理では再ビルド・再試験を行っていない。

## 2. 次回CAM-A最大1回の最小承認パッケージ

### 2.1 確定している候補

| 項目 | 固定値 |
| --- | --- |
| source SHA | `712ba9d298ef01ad413f825b83c3e4abfaef1f16` |
| source tree | `518d955ea780f2c292241363cc4f87b638d91b75` |
| 実行ファイル | `A0CameraStitcher.Phase0.exe` |
| bytes | `834048` |
| SHA-256 | `90635086ba26a5d460b860f372c2ada93b50fdb54d77210d4ab437b42441260e` |
| camera | D810一台、`CAM-A` |
| command mode | `pc-direct-capture-single`。WPFの`SIMULATED`画面ではない |
| capture上限 | 最大1回 |
| watchdog | transaction全体180秒 |
| retry / card fallback / delete / format | すべて0 |

SDK module、依存DLL、SDK/WPD mapは実識別子を記録せず、次のhashだけを照合する。

| 役割 | SHA-256 |
| --- | --- |
| x64 SDK module | `f8039dfa73de2603fee45f478eeaab2d09117e9c75a7a7e2ab555da22f69958f` |
| NkdPTP.dll | `678a692ac3f85f6694b568daf3e765ea2354b919e91f3cae6064b99f34291c6e` |
| NkRoyalmile.dll | `9c1961d813a1fe3a3e7a299ea523619c8d2fb16cb0953f3c4aa3e25b36adcc28` |
| dnssd.dll | `d0847ba9871d8dc05020e1d618540693c91cdba2519c3ecb215beb43510f055a` |
| SDK CAM-A map | `8ab46df8f14cae8f7b30087b6d30be60b283c06299c7f74c7d506c27d257b95b` |
| WPD CAM-A map | `71365be4cb333ce50bd6733120784971a138968816ae1d6f287f92da83b562a7` |

### 2.2 実行場所

事実:

- 実機用PCは`AOPC-22-NOTE`である。
- 保存済み候補EXEは、作成PCのprivate evidence領域で固定済みである。
- 2026-09-17時点で`AOPC-22-NOTE`へのSSHはtimeoutし、同PC上のEXE配置、起動引数、表示中processを確認できていない。

推奨:

- `AOPC-22-NOTE`の同一interactive Windows logon sessionで実行する。
- repositoryの作業treeから直接起動せず、専用の固定ローカル実行フォルダへ候補EXEと必要なlocal-only設定を配置する。
- コピー後にEXE、SDK module、依存DLL、SDK/WPD mapのhashを再確認する。一つでも違えば`Blocked`とし、新しい候補として再承認する。
- WPF画面を使う場合でも、上部が`SIMULATED`なら中止する。この最小パッケージはPhase 0 CLIのPC直接保存確認であり、WPF受入とは別である。

### 2.3 必要機材

- Nikon D810一台だけ。他のD810はUSBから外す。
- CAM-Aとして登録済みの対象body。
- JPEG Fine / L / 7360x4912が読み取り確認できる状態。
- 専用カード。今回のPC直接保存確認では事前・事後payload 0を必須にする。
- 安定したUSB接続、AC電源または十分なbattery、固定ローカル保存先と十分な空き容量。
- 他のNikon/cameraアプリが停止した`AOPC-22-NOTE`のinteractive session。

### 2.4 実行順序

1. `origin/main`、source SHA、EXE/hash、runtime/hash、map/hashを照合する。
2. A0/Nikon/camera関連processが0件であることを確認する。
3. Live View停止、SDK source/capture session終了、WPD session 0を確認する。
4. WPDだけを開く`spool-status`を一回実行し、CAM-A payload 0を確認して完全終了する。
5. 次の必須引数をすべて明示し、`pc-direct-capture-single`を一回だけ実行する。`<SDK_MAP>`と`<WPD_MAP>`はhash照合済みのlocal-only fileを指定し、実pathや実識別子を証跡へ転記しない。

   ```text
   pc-direct-capture-single --alias CAM-A --count 1 --camera-map <SDK_MAP> --wpd-camera-map <WPD_MAP> --single-camera-connected-confirmed --exclusive-camera-control-confirmed --pc-direct-save-confirmed
   ```

   `--transport`、card spool/delete authority flag、operator gate、fault scenarioは付けない。
6. SDK内でSaveMediaをPC/SDRAMへ一時変更し、readbackする。撮影、SDK Item回収後、SDK終了時に元値へ戻してreadbackする。
7. `.partial`、JPEG完全decode、7360x4912、size、SHA-256を確認し、atomic `original.jpg`へ確定後、再読込する。
8. process 0、SDK session終了、SaveMedia復元確認後だけ、WPDだけを開く事後`spool-status`へ進む。
9. 事後payload 0を確認して終了する。

### 2.5 即時停止条件

- D810が一台以外、CAM-A不一致、hash不一致、他process残留
- 事前payloadが0以外
- Live ViewまたはSDK/WPD sessionの終了を確認できない
- 180秒超過、応答不明、JPEG不正、寸法/size/hash/再読込不一致
- SaveMedia復元を確認できない
- 事後payloadが0以外

停止後は同じtransactionを再試行しない。processを強制終了せず、WPDを追加で開かず、`Blocked`または`FailedPartial`として取得済み証跡を保持する。

### 2.6 合格に必要な証跡

- `TerminalState=Complete`
- capture attempt 1以下、automatic retry 0
- 保存済み`original.jpg`のpath、bytes、SHA-256、7360x4912完全decode
- SaveMedia restore attempted/confirmed
- 事前・事後payload 0
- card fallback、camera delete、format 0
- 実識別子を含まないsummary/report/events
- 終端A0 process 0

## 3. Issue #42: A0品質基準の具体化

Issue #42はopenであり、下記は`candidate`であって承認値ではない。

| 合格項目 | 測定方法 | 初期candidate |
| --- | --- | --- |
| 出力寸法 | A0物理寸法、pixel寸法、orientationをmanifest照合 | landscape 180 DPI、`8426x5960`、orientation 1 |
| 局所有効DPI | input解像度とwarpのlocal Jacobianから全面・四隅・overlapを計算 | 全測定点で180 DPI以上を目標。未達点はfail |
| registration | 独立fiducial/edge oracleでoverlap内の誤差をpx集計 | p95 `<=0.5 px`、max `<=1.0 px` |
| calibration residual | calibrationに使っていない点でreprojection residualを測定 | RMS `<=0.25 px` |
| coverage / crop | validity maskと原稿content maskでhole、black wedge、欠落を確認 | uncovered 0、content loss 0、edge inward `<=0.3 mm` |
| seam | 文字、罫線、写真、halftoneを含む固定ROIで差分とblind review | 文字・細線の切断/二重像0。自動値はcorpus測定後に決める |
| 色 | 校正色票の独立測色値と合成結果をCIEDE2000で比較 | まずΔE00 median `<=2`、p95 `<=4`、max `<=8`を比較候補とし、実測で見直す |
| 明るさ差 | overlap両側のneutral patchとgradientを測定 | seam両側の輝度差p95 `<=3%`を比較候補とする |
| sharpness | slanted-edge MTF50と細線/小文字ROIを原画像と比較 | overlapのMTF50が弱い側原画像の80%以上を比較候補とする |
| JPEG | full decode、寸法、ICC、orientation、privacy metadata、hashを照合 | baseline JPEG、4:4:4、quality 0.95候補、serial/GPS/MakerNoteなし |
| 性能 | reference PC、warm/coldを分けてp50/p95/max、peak memoryを測定 | stitch-only p95 10秒、capture-to-product p95 35秒、app commit hard 5 GiB候補 |
| failure | damaged/over-limit/no-result fixtureでfail-closedを確認 | locked holdoutでfalse pass 0。測定不能はsuccessにしない |

推奨判定順は、`identity/profile -> decode -> coverage/crop -> geometry -> color/seam/sharpness -> encode後再検査 -> manifest commit`とする。平均値だけで合格させず、p95、max、局所worst caseを残す。

Product Ownerに必要な判断は、最初から全数値を確定することではない。まず上表を`evaluation profile v0.1`として実測に使う許可を出し、locked holdoutの結果後に最終承認または差戻しを行う二段階を推奨する。

## 4. Issue #43: 固定rig・光学条件の具体化

### 4.1 既知

- Nikon D810二台、USB接続、固定rig、静止した平面に近いA0原稿
- JPEG Fine / L / 7360x4912
- CAM-AからCAM-Bの順次撮影。実シャッター同期は保証しない
- 撮影設定は書き込まず、承認profileとのread-only照合を行う
- 撮影ごとの自由なhomography再推定、profile自動学習、上限外clampは禁止

### 4.2 evaluation profile v0.1候補

| 項目 | candidate |
| --- | --- |
| 配置 | D810 portrait二台、左右配置、CAM-Aを基準座標 |
| lens | 同一型式の60 mm級lens二本 |
| working distance | 約1.81 m |
| baseline | `534 +/- 2 mm` |
| overlap | `141 +/- 10 mm`、hard minimum `120 mm` |
| 位置再現 | 基準位置から`+/-0.5 mm`候補 |
| camera設定 | ISO 64、f/5.6、manual exposure、manual focus固定、固定WB、VR off、Auto ISO off |
| 原稿平面 | 全面flatness `<=0.50 mm`、overlap corridor `<=0.20 mm`候補 |
| 照明 | `5000 +/- 200 K`、CRI `>=95`、R9 `>=90`、`1500 +/- 300 lx`、min/max `>=0.90`、flicker `<=1%`候補 |
| profile有効期限 | 30日または1000 pairの早い方を比較候補 |

### 4.3 現物確認が必要な不足

- lensの正式型式、個体差、実焦点距離、歪み、絞り別MTF
- rig寸法、剛性、camera固定方法、sensor面と原稿面の角度
- 実working distance、四隅coverage、overlap、局所有効DPI
- focus固定方法、露出、WB、Picture Controlの正式profile
- platen方式、原稿保全、flatness、depth/parallaxの測定器とuncertainty
- 照明器具、配置、暖機時間、照度・色温度・CRI・flickerの測定方法
- calibration chartの寸法精度、色票、権利、保管方法
- rig移動、衝撃、camera/lens交換、focus変化、温度、quality drift時の再校正trigger

Product Ownerの判断は、`左右配置・60 mm級・180 DPIを評価開始案とするか`、`vacuum等の原稿保持を許可するか`、`照明と計測器へ必要な費用を掛けるか`の三点に絞る。数値の最終承認は光学計算と実測後に行う。

## 5. Issue #44: 権利処理済みテスト原稿とcorpus

### 5.1 自社作成する原稿候補

第三者著作物や顧客原稿を使わず、自社作成のvector masterを正本にする。

- 0.1/0.2/0.3/0.5/1.0 mmの縦横線、斜線、同心円、格子
- 4 ptから24 ptの自社作成dummy text、縦書き、英数字、罫線
- slanted-edge、Siemens star、checkerboard、fiducial、寸法scale
- neutral gray、RGB/CMYK patch、gradient、halftone、低texture、高contrast領域
- overlapを跨ぐ文字・細線・写真相当の自社生成pattern
- 四隅、端、余白、裁ち落とし確認mark

masterには作成者、会社帰属、作成日、version、source hash、PDF/SVG hash、印刷条件、利用目的を記録する。画像本体は承認済み外部保管とし、repositoryにはschema、匿名manifest、hash、権利記録だけを置く。

### 5.2 実写が必須な項目

synthetic画像だけでは、次を検証できないためD810実写pairが必要である。

- lens distortion、個体差、focus、被写界深度、回折
- rigの角度・距離・再設置差、原稿flatnessとparallax
- 照明むら、反射、flicker、露出差、white balance、色差
- 実overlap mask、seamの文字・細線横断、black wedge、crop
- JPEG codec、metadata、実寸decode、処理時間、peak memory
- 日を跨ぐ再設置、operator差、温度・時間drift

### 5.3 推奨する段階分け

1. `corpus design`: vector master、manifest schema、rights record、oracle定義をsoftware-onlyで作る。
2. `pilot`: calibration 12 pair、development 24 good pairと少数の意図的fault。最終合否には使わない。
3. `acceptance`: repository既存candidateのcalibration 60、development good 120、locked holdout good 300等を、費用と統計目的からEXP-04で再計算する。
4. `release confirmation`: locked holdoutと別原稿・別撮影日のsetを用意する。

calibration、development、locked holdout、release間で、原稿、派生画像、撮影日、rig状態を重複させない。実装が使うfeatureやseam costを、そのまま合否oracleへ流用しない。

## 6. SingleCamera / DualCamera / 合成MVP残件

| 領域 | 実装・証拠済み | 残件 | 次の扱い |
| --- | --- | --- | --- |
| PC直接保存Single | source 712ba9d候補、23/23と6/6、最大1回/no retry契約 | AOPC-22でのpre-spool、実capture 1回、post-spool、証跡判定 | 新しい1回承認後だけ実行 |
| Single Camera Agent | one-shot、10回、p95承認、100/100のAgent実績 | 実WPF 100件、Continuous Live View handoff 10回、USB切断、保存先障害 | Issue #11/#13。実機・operator gate |
| Dual CaptureRecoveryOnly | production backend、WPF経路、10回runner、100回内部coordinator、証跡writer | 実1回、10回、p95承認。100回のUI/CLI接続とhost lifetime適合、実100/100、異常系 | 1回→10回→p95→100回を順守。前段を飛ばさない |
| Dual 100回 | 内部coordinatorとfake-only testはmainに存在 | production起動経路は意図的に未接続。600秒host budgetでは100組を保証できない | 実10回p95と必要host時間を得てから設計・接続 |
| 合成基盤 | 固定3x3 warp、bilinear、linear feather、fixed crop、atomic JPEG、synthetic measurement | lens/profile生成、production residual、色、seam/blend選定、coverage/crop/quality gate、実寸benchmark、Review UI | #42/#43/#44後にIssue #45〜#49 |
| packaging/release | 対象外 | license/redistribution、installer、clean PC、release decision | M4。現在は進めない |

### 6.1 今すぐ実装しない理由

- Issue #45〜#49は`#42 -> #43 -> #44`で決まる値・現物・corpusに依存する。仮の数値を製品既定値として埋め込むと、未承認基準を実装済みと誤表示する。
- Dual 100回のproduction接続は、実10回p95と同一bindingを維持できるhost lifetimeの実測が前提である。内部coordinatorだけを画面へ接続しても、現行600秒budgetでは100組を安全に完走できる根拠がない。
- Issue #141のLive View停止の段階1/2はmainへ統合済みで、残段階は実測データ後の判断である。
- Post-MVP Issue #205は今回の対象外である。

## 7. 推奨順序

1. AOPC-22のSSH/interactive sessionを復旧し、processと起動modeをread-only確認する。
2. 上記CAM-A一回パッケージをexact SHA/hashで再提示し、新しい一回承認を得る。
3. one-shotの結果だけを評価し、失敗時は再試行せず停止する。
4. 並行して#42/#43を`evaluation profile v0.1`としてレビューし、実測開始の可否だけ決める。
5. #44のvector master、rights manifest、oracle設計を確定し、外部保管場所を決める。
6. 実測で#42/#43を最終化後、#45〜#49を順番に実装する。
7. Dualは実1回、10回、p95承認、host lifetime修正、100回、異常系の順で進める。

## 8. 今回行っていないこと

- 再ビルド、再テスト
- SSH timeout後の反復接続
- カメラ接続確認、SDK runtime open、WPD open、capture
- camera設定write、delete、format、automatic retry
- Issueのclose、label変更、優先度変更
- Post-MVP #205の実装
