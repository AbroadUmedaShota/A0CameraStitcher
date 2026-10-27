# 両台撮影・未校正JPEG Merge・静止結果の確認 (#334)

所有者の「いい感じ」「開発を続けて」と「図面撮る前提だから今はある程度」という指示に基づく開発用の接続。仮配置のC表示に対する良好な感触を、最終図面の文字・直線・継ぎ目・色の受入へ読み替えない。承認済みrig profileを作ったり既存の製品用StitchJobの信頼ゲートを回避したりしない。

「両台撮影 → 仮Merge保存」は、現在のCの幅・高さ・重なり・B上下移動・revisionを保存してから両Liveをdrain/Stop/Closeする。Aを一回撮影して原画像のPC検証とCloseを確認し、続けてBを一回撮影する。A失敗時にBへ進まず、B失敗でもA原画像を失わず、自動再撮影・旧画像の補充は行わない。停止要求時から原画像二枚・合成JPEG・確認レポートの検証まで、共通180秒deadlineを使う。別カメラごとの180秒へ延長しない。従来の単独撮影ボタンも残す。

JPEGの幾何は `dx = round((previewWidth-overlap)*jpegWidth/previewWidth)`、`dy = round(vertical*jpegHeight/previewHeight)` とする仮の正規化座標変換。二台のJPEG寸法一致が必要で、上下は両画像が存在する高さにcropする。既存の純粋M2 rendererでfull-size BGRを合成する。ライブビューとJPEGの厳密な画角対応、回転・倍率・台形・レンズ・色・物理DPIは未検証であり、この式は校正結果ではない。誤差を閾値へclampして合格と扱わない。

原画像は `%LOCALAPPDATA%/A0CameraStitcher/wpd-captures` に個別保持し、結果は別の `wpd-pair-reviews/<run>/review/uncalibrated-review.jpg` と `uncalibrated-review.json` に保存する。入力はCAM-A/CAM-Bのcanonical original.jpgと、今回の捕捉結果のSHA-256で結び付ける。相互に同じphysical file、alias違い、hash違い、寸法違い、既存出力directoryは拒否する。入力とancestorをpinし、sourceをwrite/delete禁止のread handleで保持する。

JPEGはCREATE_NEWの所有handleへencodeし、flush、候補identity/size/hash/dimensions検証、handleによる置換禁止rename、保存後の全画素復号とsize/hash再読込を行う。独立した確認レポートもpartial/flush/置換禁止rename/再読込を行い、入力両方のpath/hash/size、pair ID、Cの元条件、実際のJPEG translationと寸法、出力hashを記録する。reportは `productQualityAccepted=false`、`physicalScaleVerified=false`、`liveToStillFieldOfViewVerified=false`。画像・EXIF・private pathをGitHubやリポジトリへ取り込まない。失敗時は取得済みoriginalと出力partialを保持する。

保存したJPEGから復号した縮小画像を、画面下段へ「撮影JPEGの合成結果（静止画／未校正・確認用）」として表示する。A/Bは再開し、「ライブCに戻す」「撮影結果を見る」で切り替える。静止結果を現在のライブ映像と表示しない。手動位置合わせを操作した場合もライブCへ戻る。通常起動に自動撮影はない。`--seconds 60 --pair-smoke` は一回の自動位置候補→実際の両台撮影UI command→原画像/合成JPEG保存→静止表示→両Live再開/終了を行う明示実機テスト。

本entryは開発previewを明示有効にしたbuildだけでcompileする。既存の承認済みprofile用 `StitchCanonicalPair` / `StitchCanonicalPairV2` は変更していない。人工JPEGのgeometry/feather/保存/再読込/原画像不変/誤hash・alias・same-file拒否/期限・途中partial保持を `still_review_contracts`、A→B・共通deadline・失敗時の原画像保持を `still_pair_contracts` で検証する。実機結果は開発状況記録を参照。最終図面の校正受入は別工程。

## 保存結果の拡大・微調整・再Merge (#336)

両台撮影・Merge保存後の「拡大確認・再Merge」で、保存したJPEGを独立画面に開く。過去の結果は `scripts/Open-StillReview.ps1 -Report '<選択したuncalibrated-review.jsonの絶対パス>'` で明示的に開ける。最新ファイルの自動選択や別pairの原画像補充はしない。カメラ接続・電源は不要で、カメラ制御libraryへlinkしない。この画面でシャッターやフォーカス命令は送らない。

「全体表示」「100%（等倍）」「200%」で表示倍率を選び、画像内をドラッグして移動する。「継ぎ目へ」で現在の重なり領域の中央を表示する。100%は保存JPEGの1pxを実画面1pxで表示し、Windows側のbitmap拡大を避けるためper-monitor DPI aware v2で起動する。表示計算は保存データを変更しない。200%は1pxを2×2pxに表示する。fit表示は画面に収め、余白は黒にする。

「Bを左/右/上/下へ1px」とX/Y入力は、縮小ライブCではなく原画像のpx単位。XはBの右方向、Yは下方向を正とする。入力変更だけでは表示画像は変わらず、「再Merge・別保存」で反映する。Xは1以上・原画像幅未満、Yの絶対値は原画像高さ未満で、既存rendererのcanvas/pixel予算も満たす必要がある。この数学的な範囲を光学品質の許容範囲とは解釈しない。「調整値を戻す」は表示中の保存結果の値へ戻す。

再Mergeは元のA/B original.jpgから一回だけ行い、Live由来の元geometry/revisionは来歴として保持する。実際のtranslationは新しい `a0.uncalibrated-still-review.v2` レポートへ記録する。`geometryBasis=manual-still-pixels`、元pair ID、入力二枚のhash/size/path、直前の親レポートのpath/hash、出力JPEGのhash/size/dimensionsを記録し、受入フラグ三つはfalseのまま。v1の初期結果も引き続き開ける。親の即時リンクのみを検証し、無制限の再帰的履歴走査はしない。

既存の結果folderを上書きせず、pairの下の新しい `revision-<id>` へ保存する。元画像・既存結果はread-only handleで検証し、新しい結果もCREATE_NEW/partial/flush/置換禁止rename/再読込を行う。各再Mergeに独立した180秒deadlineを与え、失敗時は現在の表示・入力・既存結果と診断partialを保持する。ファイル処理中も拡大・移動はでき、再保存やパラメーター変更は無効にする。閉じる要求時には開始済み保存の終了を待ち、強制終了や自動再実行を行わない。

読み込みはschema、重複/未知/欠損field、固定名・alias、絶対local path、reparse、品質フラグ、hash/size/dimensions、translationと出力canvas、即時親のpair/入力/geometryを照合する。異なるpair・改変・不完全なレポートを正常表示として扱わない。`still_review_contracts` でv1/v2互換、再revision、誤親・誤pair・不正値・改変拒否と保存不変を、`review_viewport_contracts` で等倍・2倍・fit余白・pan境界と描画pixelを検査する。

`A0CameraStitcher.StillReview --report '<選択レポート>' --smoke` は明示的なfile-only試験。実際のUI commandで倍率・drag・1px調整・再Mergeを行い、同時保存の重複要求を拒否し、GDI実描画のpixelを確認して終了する。実画像、EXIF、private pathは引き続きLocalAppData/ignoredログに留める。

## 仮配置での比較・回転・倍率・条件の復元 (#338)

現在の机上配置は実際の吊り下げ環境と異なるため、本画面は操作・処理の検証用とする。ここで図面の最終精度・物理DPI・校正値を承認せず、最終設置後に校正と実図面の評価を行う。

保存済みJPEGの表示に加え、「半透明50%」「差分」「Aのみ」「Bのみ」を切り替えられる。同じ原画像組を検証・全画素復号し、A基準へ写したBと比較する。半透明は重なり部分を等分に表示する。差分は対応位置の8-bit BGR絶対差で、重なりがない部分は格子を表示し、差ゼロの黒と区別する。比較表示はメモリ内だけで、原画像や保存JPEGを書き換えない。100%/200%とpanは全モードで使用可能。

X/Yは従来どおり原画像px単位の整数。回転はBの画像中心を軸にした角度（正が時計回り）で、ボタンは0.05度刻み、直接入力も可能。倍率欄は倍数（1が100%、1.01が101%）で、ボタンは0.01%相当の0.0001刻み。角度±180度、有限・正の倍率、既存canvas/pixel予算と被覆可能な重なりを検査する。これは数値・資源上の制約で、光学的な許容誤差ではない。自由なhomography推定・最終profile学習は行わない。

比較モードではボタンの調整を即座に表示する。直接入力後は「入力値をプレビュー反映」で半透明表示へ反映する。「保存JPEG」は直前に保存された結果を表示し、未保存の調整プレビューと区別する。「再Merge・条件保存」で原画像から合成し、新しいJPEGとレポートを別保存する。

回転/倍率で画像のない角が生じるため、B内部に収まる保守的な矩形とAとの和で被覆される出力範囲へ切り取る。非identity時はBの入力境界から1px内側を使い浮動小数点境界を避ける。結果寸法が小さくなる場合があり、最終図面の内容欠損の受入とは扱わない。製品rendererの未被覆拒否は変更しない。初期translationおよびrotation=0/scale=1では従来のcrop/出力を維持する。

回転または倍率を変更した保存結果は `a0.uncalibrated-still-review.v3`。`geometryBasis=manual-still-similarity`、`adjustment`（anchor=image-center、rotationDegrees、scale）と、union canvasからの四辺cropを追加する。元pair/入力hash/preview geometry/親report hashと三つのfalseフラグは維持する。実際の中心移動をtranslationへ記録し、行列のtranslation項と混同しない。doubleは17桁で保存し、読み込み時はschemaと幾何・crop・出力寸法を再計算して照合する。v1/v2も読める。

「保存条件を読込」で同じpairのレポートを明示選択し、X/Y・回転・倍率を調整プレビューへ復元できる。pair ID、A/Bの原画像path/hash、寸法が違えば拒否する。倍率は倍数のまま数値を往復させ、表示丸めで条件を変えない。別pair全体を開く場合は従来の `--report` または `Open-StillReview.ps1` を使う。別原稿への共通校正profileの使い回しは最終環境の別工程。

`review_alignment_contracts` は既知の中心回転・倍率、正負角度/倍率の被覆、独立した座標rampでの正しい補正の差分ゼロ・誤補正の非ゼロ、半透明と比較不能表示を検査する。`still_review_contracts` はv3保存/復元と角度/倍率/anchor/crop/承認flagの改変拒否を追加した。file-only `--smoke` は全比較モードの実GDI描画、回転/倍率、重複保存拒否、保存条件の正確な復元も検査する（ファイル選択ダイアログだけは明示テスト中に保存したレポートへ固定する）。

## 対応点でずれを測り、調整候補を確認する (#339)

「対応点を追加」を押し、A画像の目印、続いてB画像の同じ目印を左クリックする。共通して写っている線の交点や文字の角などを選ぶ。選択中は100%/200%の切替と右ドラッグで拡大・移動できる。点選択を終えた後は従来の左ドラッグも使える。画像外・格子・黒い余白は指定できない。選択途中の取消、完成した直前の点の取消、全点消去を用意し、最大32組とする。原画像の同じ座標を繰り返して追加することは拒否する。

点は表示座標ではなくA/B各原画像の座標で画面内に保持する。crop・表示倍率・pan・Bの回転倍率を逆変換して求めるので、表示の拡大や手動調整後も同じ観測点を使える。比較表示では緑のA、橙のBと番号を重ね、右側に各点のdx/dy/距離と用途別のRMS・最大距離を表示する（用途分離は#340）。残差は「Bを変換した位置 − Aの指定位置」、単位はA原画像px。保存JPEG表示時も右側の数値は調整値での測定であり、点の描画は調整プレビュー側だけに出す。

異なる2組以上の調整点を選んで「候補を計算」を押すと、反転なしの最小二乗similarityでX/Y・回転・倍率の候補を求め、適用前後のRMS・最大残差を示す。計算だけでは調整値を変更しない。「候補を適用」で初めてプレビューへ反映し、手動微調整後に「再Merge・条件保存」で既存どおり別revisionへ保存する。X/Yは既存画面と同じ整数に丸め、その実際に適用される条件で残差を計算する。点や調整値の変更・条件読込・保存で古くなった候補は再計算が必要。選択途中の点がある間は調整/条件変更/保存を拒否する。

RMSは指定した点の距離二乗の平均の平方根で、学習に使った点に対する当てはまりである。2組だけではほぼ一致しやすいため、離れた位置に3組以上を選び、指定していない場所の直線や文字も確認する。小さい残差だけで図面全体の合格や物理精度を保証しない。奥行きのある現在の被写体、台形・レンズ歪みをsimilarityだけで解消できるとは扱わない。数値不正、退化、既存の被覆crop/canvas制約違反は拒否し、自由homographyや校正profile学習はしない。

対応点は通常は画面内の記録で、#341の「作業を保存」で明示保存すると復元できる。未適用候補は保存せず、残差は復元後に再計算する。調整条件と合成JPEGの保存形式v1/v2/v3、元画像/以前の結果の保持、hash/親report来歴とfalseの承認フラグは維持する。この測定は校正レポートの代替ではない。`--smoke` の対応点は既知の表示座標ずれを人工的に指定する処理検証で、実被写体の精度測定には使用しない。

## 候補計算から外した検証点で確認する (#340)

対応点に「調整用」と「検証用」の用途を追加した。初期値は調整用。「次の点: 調整用／検証用」ボタンで次に追加する用途を切り替え、従来どおりA→Bと目印を選ぶ。「直前の点の用途変更」で最後に完成した点を切り替えられる。点の選択途中は用途変更できないため、完成させるか取消してから操作する。全32組の制限、画像外/重複/不正値拒否は両用途を通して維持する。

候補の計算は調整点だけで行い、異なる調整点が2組以上必要。検証点は何組あっても調整点の不足を補わず、候補計算に影響しない。用途を変更したり点を追加/取消した後の古い候補は適用できない。計算・確認・適用・手動微調整・別保存の流れは維持する。

右欄は調整点と検証点それぞれの点数、RMS、最大残差を分けて表示する。検証点が0組なら「未検証（検証点なし）」とし、RMS=0の成功表示にはしない。候補を計算すると現在の調整値と未適用候補の数値を同じ欄に表示する。各点のdx/dy/距離は現在の調整値に対する数値で、用途も表示する。マーカーのFは調整点、Vは検証点。A緑/B橙の色と番号は従来どおり。

調整点と別の場所にも検証点を置き、候補の改善がそちらでも確認できるかを見る。調整点でずれが0でも、検証点に5pxのずれが残る場合を区別できる。検証点は「今回の候補計算から外した点」という意味で、選び方の統計的な独立性や図面全域の品質合格を保証しない。閾値による自動合格は追加せず、最終環境の校正・実図面の確認は別工程とする。

点と用途は画面内に保持し、#341の明示操作で別の作業ファイルへ保存/復元できる。保存JPEGの条件は従来と同じgeometryと来歴で、作業ファイルを承認済み校正レポートとは扱わない。元画像、旧結果、未承認flags、カメラを開かないリンク隔離は維持する。人工座標のテストとnative `--smoke` は、検証点をずらしても候補が変わらないこと、用途変更での古い候補拒否、未検証表示と別保存を確認する。

## 対応点・用途・調整値の作業保存と再開 (#341)

画面を閉じる前に「作業を保存」を押すと、現在のX/Y・回転・倍率、完成済み対応点の原画像座標と調整/検証の用途、次の点の用途を保存する。JPEGの再Mergeは行わず、現在の画像と過去の結果も変更しない。選択途中のAだけの点は保存しないため、完成させるか取消してから保存する。未適用候補は保存対象に含めず、復元後に必要なら「候補を計算」で作り直す。

作業ファイルはpairの下の新しい `session-<id>/review-session.json`。画面下部に保存先を表示する。自動保存は行わない。保存中は変更/再保存/読込を無効化し、閉じる要求があれば開始した保存の終了を待つ。保存先の上書きをせず、CREATE_NEW、partial/flush/置換禁止rename/再読込で検証し、180秒deadlineと失敗時のpartial保持を適用する。

「作業を読込」で明示的に `review-session.json` を選択すると、現在と同じ原画像組の状態を復元する。pair ID、原画像path/hash/bytes、寸法を照合し、別の組なら変更せず拒否する。保存時に参照していたJPEG/レポートを保存JPEGの基準として開き直し、その上へ保存した調整値をプレビュー表示する。各点と用途、次の点の用途を戻し、残差は原座標と現在の計算から再計算する。

画面を閉じた後は `scripts/Open-StillReview.ps1 -Session '<明示したreview-session.json>'` または `A0CameraStitcher.StillReview --session '<明示したreview-session.json>'` で再開できる。従来の `-Report` / `--report` 起動も維持する。最新ファイルや別pairの原画像を自動選択しない。作業を保存していない点は閉じると破棄する。表示倍率・pan位置・未完了の点・未適用候補の復元は今回の対象外。

形式は `a0.still-review-session.v1`。参照reviewの絶対local path/SHA-256、pair ID、A/Bのalias/hash/bytes、原寸法、中心基準の調整値、次の点の用途、0～32組の点と用途を記録する。三つの品質受入flagsはfalse。既存の合成レポートv1/v2/v3は変更しない。残差を保存した数値として信用せず、読込後に再計算する。原画像を変更せず、校正profileや製品の成功レポートへ昇格しない。

読込時は256KiB上限、schema/重複/未知/欠損field、有限数値/用途/座標/重複/点数/被覆crop、参照reviewのhash、入力と参照JPEGのhash/bytes/寸法、即時親リンクを検証する。固定名・絶対local参照path・reparse拒否も従来と同じ。保存中は参照review・入力・基準JPEGと即時親をimmutable handleで保持し、変更された参照へ作業を結び付けない。

native `--report ... --smoke` は作業の明示保存、点/用途/調整値の変更、失敗読込での現在状態保持、作業の復元から再Merge/条件復元まで確認する。`--session ... --resume-smoke` は直前の明示smokeで保存した3点の作業を別プロセスで開き、調整2/検証1、原座標/用途/細かい調整値と再計算した検証残差5px、GDI描画と候補の自動再計算なしを確認して終了する。両方ともfile-onlyでカメラ操作は行わない。ファイル選択ダイアログだけはsmoke中に明示保存したファイルへ固定する。

## 二組の撮影フロー検証と合焦拒否の診断 (#343)

`--seconds 180 --pair-cycle-smoke` は同じwindow/leaseで最大二組を確認する明示試験。各組は既存の「両台撮影」commandを通り、各組の静止結果が実描画され、再開後のA/Bが新しく取得・描画され、さらに「ライブCに戻す」で新しいCの描画を確認してから次組へ進む。二組完了で終了し、一度でも失敗すれば次組へ進めず再撮影しない。通常起動に自動撮影はない。

`PairCycleProbe` は試験だけの純粋な進行判定。session/pair/saved数、busy/失敗状態、現在のLive/merge鮮度と、前組の静止/C描画数を使う。以前の描画数で再開を成功にせず、一つの段階からcapture commandを重複送信しない。撮影コマンドや保存処理、校正/承認APIには触れない。実機結果が揃うまでは二組の成功と扱わない。

初回のA shutter受付成功後に画像通知が得られず、別の読み取り診断とAF-only操作でAのOutOfFocus（0xA002）を確認した。撮影前と受付後のDeviceReadyでこの拒否を検出し、typedな理由をUIまで渡す。受付成功だけを画像到着・合焦の成功とせず、自動AFやシャッター再送は行わない。合焦拒否時も既存原画像や診断partialを保持し、撮影失敗後のライブ自動再開は無効のままとする。

`--capture-diagnostics` は登録A/BのreadinessとLive状態・禁止理由、露出時間（0x500D）・フォーカスモード（0x500A）・電池（0x5001）を読むだけの診断。既存のLive AFモードも読む。aliasを再確認し、hardware leaseと12秒の停滞watchdogを使って各sessionをCloseする。Start/AF/シャッター/設定writeは行わず、raw値をprivateログへ記録する。追加識別子は[公開PTP header](https://github.com/gphoto/libgphoto2/blob/v2.5.31/camlibs/ptp2/ptp.h)の数値事実を参照し、SDK資料は利用しない。

`--seconds 20 --af-prepare-smoke` は、別途承認済みのA/BのAF buttonを各一回だけ実行する準備確認。撮影・近遠drive・AFモードwriteは0で、両側の完了応答とLive終了を確認する。これは撮影処理への自動AF追加ではなく、光学的な合焦や図面品質の保証でもない。AFも拒否される場合は被写体の距離・明るさ・模様を確認し、撮影の再試行を進めない。

`--capture-event-diagnostics` は既存の読み取り状態診断に、Live offで各側四回のイベント読取を加える。通知コード/件数だけを記録し、storage ID/object handle/画像は出さない。イベント読取専用gateはCheckEvent/READ/ENDのみ。原画像のdownload・撮影・設定変更・Live開始・AFは行わず、通知を消費したことを画像帰属や撮影許可に置き換えない。最新の成功系列と別系列の拒否は [継続開発時開発状況](DEVELOPMENT_STATUS_20261019.md) に区別して記録する。

## 保存ペアから位置・色補正評価へ接続する (#352)

`scripts/Invoke-StillPairCorrection.ps1` は、明示選択した保存reviewの原画像二枚を、既存のdraft校正・幾何補正・任意の色補正評価へ渡すカメラ不要の開発用入口。PowerShell 7.2以上を使用する。撮影やLive Viewを開始せず、通常の撮影画面の動作も変更しない。元の手動translation/回転/倍率はreviewの来歴として保持し、この評価では明示したdraft profileと補正jobを適用する。

最初に `A0CameraStitcher.StillReview --report <絶対パス> --evaluation-inputs` を実行する。既存strict readerでv1/v2/v3のschema、品質未承認flags、A/B alias、原画像/保存JPEGのsize・hash・寸法と直前の親revisionを検証する。画面は作らず、失敗時にもdialogを出さない。戻る `a0.saved-review-evaluation-inputs.v1` JSONはローカルの橋渡し専用で、実ファイルの絶対パスを含むため公開しない。

scriptはreview・保存JPEG・親report・原画像・profile・jobのfile/ancestorを読み取り用に保持して検証し、既存 `StitchEval` を一度だけ呼ぶ。profile、幾何job、出力ラスタ、resampling、未使用の出力directoryと、既存評価CLIの除外対象product rootをすべて明示する。色jobは任意。人工fixtureのpolicyを実写用の承認値として自動採用しない。

```powershell
# 各変数は利用者が明示した通常local固定drive上の絶対パス、または評価条件。
pwsh -NoProfile -File scripts/Invoke-StillPairCorrection.ps1 `
  -Report $reviewReport -Inspector $stillReviewExe -Evaluator $stitchEvalExe `
  -ProfileFile $draftProfile -CorrectionJob $geometryJob -ColorJob $colorJob `
  -ProductRoot $excludedProductRoot -OutputDirectory $newEvaluationDirectory `
  -LeftUm $left -TopUm $top -RightUm $right -BottomUm $bottom `
  -Dpi $dpi -WidthPixels $width -HeightPixels $height -Resampling $resampling
```

幾何補正だけの評価では `-ColorJob` を省略する。`StitchEval` と `StillReview` はSDKなしで `A0_BUILD_STITCH_EVAL=ON` / `A0_BUILD_WPD_DUAL_PREVIEW=ON` を明示した開発buildから指定する。

| exit | 結果 |
| --- | --- |
| 0 | 既存の評価JPEG/report/manifestを保存・照合し、元reviewへのbindingも確定 |
| 3 | 幾何または色補正の数値的拒否。診断reportと拒否bindingを保持し、成功JPEG/manifestなし |
| 2 | review・引数・入力・子process・保存/照合等の失敗。既存入力と生成済みの診断候補を保持 |

最終 `review-evaluation-binding.json` は別の `a0.saved-review-evaluation-binding.v1`。選択したpair ID、review/reportと保存JPEGのhash、A/B原画像hash/size、profile/各jobのhash、実際の成果物の相対名/hash/sizeを結ぶ。実ファイルの絶対パスは書かない。`productApproved=false` / `quality=not-evaluated` のまま。既存の評価manifest形式は変更しない。

bindingはCREATE_NEWのpartialを保持handleでflushし、置換禁止rename・同handleでの再読を行って最後に確定する。exit0と最終bindingの両方を確認して初めて、この保存ペアに結び付いた評価成功と扱う。子のJPEG/manifestだけを見て本接続の成功としない。既存directory・原画像・過去review・profile・jobは上書きせず、自動再試行や最新画像の探索もしない。

`saved_review_evaluation_contracts` は製品renderer/readerをlinkしないprocess-only harnessから、実StillReview・実script・実StitchEvalを呼ぶ。人工7360×4912 JPEG、独立に記述したreview、空白/日本語を含むpathで幾何のみ/色併用の成功、直接CLIと同一JPEG、幾何/色の拒否、改変/誤pair/誤hash、既存出力、子の形式拒否、原画像と条件の不変を確認する。strict readerの既存試験も追加入口に適用する。実図面品質・承認profile・通常画面への組込・映像Cの校正反映は後続工程。

## 取り付けの向きと保存結果・再Merge (#361)

ライブCで `rotated-180`（設定は [WPD_PREVIEW_MERGE.md](WPD_PREVIEW_MERGE.md) の #361 の節）を選んで「両台撮影 → 仮Merge保存」すると、A/Bの `original.jpg` は撮ったまま保存し、撮影時のhashで結び付ける。合成JPEGを作るときだけ、復号したメモリ上のA/Bを180°回してからAを左に並べる。translation、crop、回転・倍率、対応点はすべて回した後の座標で表す。原画像ファイルの回転・書き換えはしない。

確認レポートv1/v2/v3には、`rotated-180` のときだけ `"mountOrientation":"rotated-180"` を加える。`normal` では欄を書かないので、従来と同じバイト列になる。欄の無い既存レポートは `normal` として読む。`"normal"` を明示した欄、未知の値、文字列以外、重複は拒否する。同じ状態を二通りのhashで表さないためで、schema名は進めていない。この欄を知らない旧版のreaderは未知欄として拒否するので、回したレポートを `normal` と取り違えない。向きの記録はレポートの欄だけで、出力JPEGの画素から向きを検証し直すことはしない。

再Merge（手動改訂）は親と同じ向きでなければ保存前に拒否し、出力folderも作らない。読込時も即時親と向きが違えば拒否する。StillReviewはレポートの向きで原画像を復号後に回し、半透明・差分・Aのみ・Bのみの表示と再Mergeに使う。画面上部の数値行の末尾に「取付: 180°回転（A/Bを回して表示・合成）」または「取付: 通常」を出す。「保存条件を読込」と「作業を読込」は向きの違うものを拒否する。作業ファイル `a0.still-review-session.v1` の形式は変えない。点の座標は参照レポートの向きで回した後の原画像pxで、`normal` では従来どおり原画像そのままの座標。

補正評価への受け渡し `a0.saved-review-evaluation-inputs.v1` には `mountOrientation`（`normal` か `rotated-180`）を常に加える。`Invoke-StillPairCorrection.ps1` はこの欄が二値のどちらかであることを確かめるだけで、StitchEvalには従来どおり回していない原画像を渡す。draft profileの射影変換が取り付けの向きを吸収するため、評価の画素は回さない。bindingの形式は変えず、向きはreview reportのhashを通じて結び付く。

`still_review_contracts` は左右で色の違う人工原画像を使い、`rotated-180` の合成の両端が元のAの右側・元のBの左側になること、`normal` では従来どおりAの左側が左端になること、レポートの記録と再読、欄の無い旧レポート、明示normal・未知値・重複・未知欄の拒否、親と違う向きの改訂の拒否（書込時・読込時）、原画像のbyte列とhashの不変を検査する。`saved_review_evaluation_contracts` は受け渡しの向きと、`rotated-180` のreviewから作った評価JPEGが回していない直接評価と同じhashになることを確認する。

## 撮影JPEGでの相似の推定と補正 (#365)

「両台撮影 → 仮Merge保存」は、Cの重なり・上下をJPEG画素に直した平行移動を初期値にして、向きを当てた後の復号済みA/Bで相似（平行移動・回転・倍率）を推定し、方針ファイルに従って未校正Mergeの幾何を決める。推定と判定は合成の前に一度だけ行う。原画像は回さず、書き換えない。StillReviewの再Merge（手動の改訂）では推定しない。設計の元は [M2_AUTO_OVERLAP.md](M2_AUTO_OVERLAP.md) の5.4節の6・7、7.2節、8.3・8.4節。

### 推定器

`EstimateOverlapSimilarity`（`a0_m2_overlap_estimation`）はカメラ・ファイル・時計に依存しない。

- 段: 最も粗い段が約920 px幅になる数だけ1/2にする。7360幅では4段。タイルは粗い段で32 px角、探索は±25 px。7360幅では256 px角・±200 px
- タイル: 初期値で決まる重なり帯に、最大4列×10行を等間隔に置く。AとBの両方で探索範囲が画像の内側に収まる位置だけを使う
- 照合: 勾配の強さの正規化相関を、#364と同じ部品（勾配・1/2縮小・平均を引いた相関）で多段に探す。粗い段の極大を8個、細かい2段では2個まで残す。端数は最良点と上下左右の相関の放物線で求める
- 検証（数値の閾値なし）: 最良点が唯一である。窓や範囲の端に無い。Bのタイルから範囲を同じにしてAを探し直し、同じ整数位置に戻る（往復照合）。Aのタイルの構造テンソルの二つの固有値がどちらも0でない（模様が二方向にある）
- 合議: 検証に通ったタイルのすべての二つ組で相似を作り、残差の二乗の中央値が最も小さいもの（最小メディアン法）を選ぶ。その相似から頑健な標準偏差（残差の中央値×1.4826）の2.5倍以内のタイルを「一致」とし、一致したタイルで最小二乗をやり直す。やり直した相似で標準偏差を求め直して一致を選び直し、選ぶタイルが変わらなくなるまで繰り返す（上限10回）。Bの端のレンズの歪みで最初の広がりが大きくても、外れたタイルを残さないため。1.4826と2.5は統計の定数で、画素の許容値や合否の値ではない。机の格子のような繰り返し模様で周期がずれたまま往復照合を通ったタイルや、別の深さの帯は、ここで外れる
- 成立の条件: 一致が3個以上で、2行以上にまたがる。足りなければ `insufficient-tiles`
- 開始位置: ライブの初期値に加え、長辺640 px以下に縮小した画像に#364の推定器を当てた上位2候補からも同じ手順を回す。一致の数が多い方、同数なら相似の残差RMSが小さい方を採る。完全に同点で結果が違えば `tied-starts`。初期値が探索範囲の外にずれると、偶然そろった少数のタイルでも相似が作れてしまうため、開始位置どうしを競わせる
- 乱数は使わない。同じ入力からは同じ結果を出す
- 当てはめは3通りを同じ一致タイルで出す。相似（4自由度）、倍率を1に固めた当てはめ（回転は相似のatan2の角度のまま、平行移動だけを最小二乗で求め直す。報告では `rigid`）、平行移動だけ。どれを当てるかは判定が決めるので、3通りとも整数のB中心で描けることを確かめ、どれかが描けなければ `outside-image` にする

### 判定

方針ファイルの値だけを使い、コードには既定値を持たない。回転と倍率をそれぞれの上限と比べる。

| 推定の結果 | 合成の幾何 | `geometryBasis` | `decision` | `scaleDecision` |
|---|---|---|---|---|
| \|回転\| ≤ 回転の上限、倍率の差 ≤ 倍率の上限 | 推定した相似（回転・倍率を含む） | `auto-still-similarity` | `corrected` | `corrected` |
| \|回転\| ≤ 回転の上限、倍率の差 > 倍率の上限 | 回転だけ当て、倍率は1。平行移動は倍率1で求め直した値（`rigid`） | `auto-still-rotation` | `corrected` | `scale-exceeded` |
| \|回転\| ≤ 回転の上限、方針ファイルに倍率の上限なし | 同上（倍率は1） | `auto-still-rotation` | `corrected` | `no-scale-policy` |
| \|回転\| > 回転の上限 | 推定した平行移動だけ。上限で切った部分的な補正はしない | `auto-still-translation` | `rotation-exceeded` | `scale-exceeded`・`no-scale-policy`・`not-applied`（倍率は上限の内側だが回転が超えたので当てない） |
| 方針ファイルなし | 推定した平行移動だけ。回転・倍率は記録と表示だけ | `auto-still-translation` | `no-policy` | `no-policy` |
| 推定できない | ライブから引き継いだ平行移動（#334と同じ合成） | `live-preview` | `undetermined` | `undetermined` |

- 上限と等しい値は補正する。回転は絶対値、倍率は「倍率を%にした値と100の差の絶対値」（1.01も0.99も差1%）を比べる
- 倍率が上限を超えた時に回転だけ当てるのは、回転の上限は所有者が別に置いた値で、倍率の超過は回転の補正の可否と関係しないため（#365のレビューの差し戻しで決めた）。回転を当てて倍率を1に固めた時は、倍率1のまま重なり帯に合うよう平行移動を求め直す。相似のB中心をそのまま使うと、倍率の差の分だけ重なり帯がずれる
- 倍率が上限を超えたら、回転の判定に関わらず `scale-exceeded` を記録して警告する。超えた項目は全部記録する
- 相似の後の残差（一致したタイルの最大）が方針ファイルの警告値を超えたら `fit-residual-exceeded` を記録して警告する。台形（傾き）かレンズの歪みの疑いで、合成は止めない。警告値が無ければ値を表示するだけ
- 推定した回転はBに当てる回転で、正が合成画面で見て時計回り。警告は「Bを合成画面で見て時計回り（反時計回り）にθ°戻す向き」と出す。カメラ本体をどちらへ回すかは取り付けで決まるので、設置手順で一度確かめる（※要確認）
- 回転の目標（設計案のN12）は置かない。上限の内側はすべて補正する（#362の※仮定）
- 回転が上限を超えた時も、撮影JPEGで推定した平行移動で合成する。設計案10節はライブの値で合成する案だが、上限の対象は回転と倍率なので平行移動は使う（※要確認）

### 方針ファイル

カメラ登録 `camera-map-wpd.json` と同じフォルダ（`%LOCALAPPDATA%\A0CameraStitcher\phase0`）の `wpd-merge-policy.json`。管理者が手で書く。

```json
{"schema":"a0.wpd-merge-policy.v1","rotationAutoCorrectionMaxDegrees":2,"scaleAutoCorrectionMaxPercent":1}
```

- `rotationAutoCorrectionMaxDegrees`（必須、度、0〜180）: これ以下の回転を補正する。2°は#362の所有者の決定による暫定値で、コードには入れていない
- `scaleAutoCorrectionMaxPercent`（任意、%、0以上）: 倍率の差がこれ以下なら、回転と一緒に倍率も補正する。欄が無ければ倍率は補正せず、値を表示するだけ。1%は#362の所有者の決定による当面の値で、コードには入れていない。0にすると倍率がちょうど1の時だけ補正する扱いになり、実際にはほぼ補正しない
- `similarityResidualMaxWarningPixels`（任意、JPEGの画素、0以上）: 相似の後の残差の警告値。値は未決定なので、決まるまでは書かない
- 未知の欄、重複、型の違い、nullの値、schemaの違い、4096 byte超、通常ファイル以外は起動を拒否し、hardware leaseやカメラのopenに進まない。先頭のUTF-8 BOMだけは許す。値は起動時に一度だけ読む。書き換えたらプレビューを起動し直す。消せば補正しない状態に戻る
- 倍率の欄は任意の欄として足したのでschemaは `a0.wpd-merge-policy.v1` のまま。欄の無い既存のファイルはそのまま読め、倍率を補正しない
- ログに `merge_policy=settings sha256=<hash> rotation_max_degrees=<値> scale_max_percent=<値|none> residual_warning_pixels=<値|none>` か `merge_policy=none rotation_correction=0 scale_correction=0` を残す。報告にはファイルのhashと値を記録する
- 作成例（PowerShell 7。Windows PowerShell 5.1の `-Encoding UTF8` はBOMを付けるが、BOMは許す）:

```powershell
Set-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'A0CameraStitcher\phase0\wpd-merge-policy.json') -Value '{"schema":"a0.wpd-merge-policy.v1","rotationAutoCorrectionMaxDegrees":2,"scaleAutoCorrectionMaxPercent":1}' -Encoding utf8NoBOM
```

### 報告 `a0.uncalibrated-still-review.v5`

- 判定の規則か推定器の出力を変えたら、報告のschemaの版と推定器のidを上げる。v5（推定器 `a0.overlap-similarity.v2`）は倍率の上限を足した時に上げた。v4（推定器v1、倍率の上限なし）は公開前の作業用の版で、読まない
- v1の欄に `geometryBasis` と `automaticAlignment` を足す。`decision` が `corrected`（`auto-still-similarity` か `auto-still-rotation`）の時だけ、v3と同じ形の `adjustment`（anchor=image-center・rotationDegrees・scale）と `crop` を持つ。`adjustment` は実際に当てた値で、倍率を1に固めた時の `scale` は1。推定値は `automaticAlignment.similarity` に残る。`translation` は実際に合成したB中心の整数位置
- `automaticAlignment`: `estimator`（`a0.overlap-similarity.v2`）、`status`（`estimated-not-judged`／`undetermined`）、`reason`（`none`・`no-tiles`・`insufficient-tiles`・`outside-image`・`tied-starts`）、`initialPosition`（`live-preview`／`still-wide-search`）、`tiles`（searched・matched・agreed）、`similarity`（x・y・rotationDegrees・scale）、`rigid`（x・y。回転は `similarity` と同じで倍率は1）、`translationOnly`（x・y）、`residualPixels`（similarityRms・similarityMax・rigidRms・rigidMax・translationOnlyRms・translationOnlyMax）、`decision`、`scaleDecision`、`warnings`（`rotation-exceeded`・`scale-exceeded`・`fit-residual-exceeded` の順）、`policy`（sha256・rotationAutoCorrectionMaxDegrees・scaleAutoCorrectionMaxPercent・similarityResidualMaxWarningPixels。任意の欄が方針ファイルに無ければnull）。推定できない時は `similarity`・`rigid`・`translationOnly`・`residualPixels` がnull、方針ファイルが無い時は `policy` がnull
- 読込時は記録した値から判定をやり直し、`decision`・`scaleDecision`・`warnings`・`geometryBasis`・`translation`・`adjustment` が合わなければ拒否する。品質の三つのfalseと向きの欄（#361）の扱いはv1〜v3と同じ。v1〜v3は従来どおり読める
- 手動の再Merge（v2/v3）はv5を親にでき、子はv5の欄を持たない
- 推定にも両台撮影の180秒の期限が掛かる。推定の後にも期限を確かめる

### 表示

- 両台撮影の後、保存先に続けて結果を一行で出す（補正した値、上限超えの警告と戻す向き、倍率を補正しなかった理由、方針ファイルなし、推定できず）。倍率が上限を超えたら「倍率が上限を超えたため倍率は補正していません」と倍率・100%との差・上限を出す。一行の末尾に一致タイルの数と、当てた幾何の残差RMSを付ける。ログに `pair_review_alignment`（状態・判定・倍率の判定・相似・`rigid`・当てた値・タイル数・残差・方針のhashと倍率の上限・時間ms・`quality_accepted=0`）を残す
- StillReviewは右欄の先頭に推定の詳細（補正の有無と倍率を当てたか、一致タイル、推定値と当てた値、3通りの残差、開始位置、方針、警告）を出す。相似をそのまま当てていないv5（方針なし・上限超え・倍率を1に固めた）を開くと、手動調整の入力欄に推定値を入れる。保存JPEGの表示は変えない。「調整値を戻す」は表示中のJPEGの値に戻す

### 時間と確認

- 7360×4912の推定は、操作PC-1のReleaseで開始位置3つを回して数秒。復号・合成・保存を足しても180秒の期限に収まる。Debugでは数十倍かかる
- `m2_overlap_similarity_contracts`: 7360×4912の人工画像で、正負の小さい回転と倍率を戻し、相似の後の残差が平行移動だけの時より小さいこと。推定倍率1.0015で、倍率の上限が無い方針・0.1%の方針では当てる倍率が1、回転は相似と同じ角度で、`rigid` の残差が相似以上・平行移動だけ未満になること。1%の方針では推定倍率を当てること。小さい画像で、初期値のずれを広い探索の開始位置で拾うこと、別の深さの帯が合議で外れること、同じ入力で同じ結果、模様の無い画像と一方向の縞では推定しないこと、不正な入力の拒否
- `still_review_contracts`: 方針ファイルの厳格な読込（倍率の欄の有無・null・負・型違い・重複・綴り違い）、上限ちょうど（倍率1.01と0.99は1%の内側、その次の倍精度の値は外側）・符号・上限超え・残差の警告の判定、回転が超えた時の倍率の判定と警告の並び、人工JPEG（倍率1.004）での倍率の上限あり（1%、`corrected`）・なし（`no-scale-policy`、倍率1）・超え（0.2%、`scale-exceeded`、倍率1）・回転超え・`no-policy`・`undetermined`（従来のv1と同じJPEGのbyte列になる）、継ぎ目の差が相似・`rigid`・平行移動だけの順に小さいこと、手動の子の改訂、書き換えたv5（倍率の判定・方針の倍率の値・当てた倍率・`rigid`・警告）とv4・推定器v1の拒否、`rotated-180`、原画像の不変
- 吊り下げリグで撮った保存ペア2組（`rotated-180`）に当てると、どちらも回転は約1°で、相似の後の残差は平行移動だけの時より小さかった。Bの端に近い列のタイルは合議から外れやすく、レンズの歪みが疑われる（※推定）。推定した倍率は2組とも100%との差が1%より小さく、倍率の上限1%の方針では倍率も補正した（上限を置く前と同じ合成）。倍率の上限が無い方針では倍率を1に固め、残差RMSは相似より少し大きく、平行移動だけより小さかった。実画像と細かい値は公開しない

## 継ぎ目を1本の道筋にした合成と、継ぎ目に沿った明るさ合わせ (#368)

#367 の段1。未校正Mergeの混ぜ方を、重なった帯の全域を半々に混ぜる線形featherから、継ぎ目で切り替える方式（`seam-cut`）に変える。幾何（平行移動、#365の相似）とcropは変えず、画素の混ぜ方と明るさだけが違う。線形featherは残し、報告の欄で区別する。レンズの歪み・傾きの校正（段2）、継ぎ目のそばの局所的な変形（段3）、品質の合否（段4）、ライブのC、製品経路の合成は対象外。

### 継ぎ目の道筋

`RenderPairSeamCut`（`a0_m2_seam_blend`、カメラ・ファイル・時計に依存しない）は、`RenderPair` と同じ入力（向きを当てたA/B、変換、crop）から、出力の各行に1列ずつの継ぎ目を求める。

- 帯: AとBの両方が写る出力画素。行ごとにその範囲を求め、左右の端から余裕（下の表）を取った内側だけを継ぎ目の候補にする
- 費用: 輝度 L（0.114B+0.587G+0.299R）を ln(1+L) にし、(1) AとBの対数輝度の差から、その場所の差の平均（64 px角のブロックの平均を双一次で補間）を引いた絶対値、(2) AとBの対数輝度の勾配の大きさの差の絶対値、の和。重みは1:1。対数にして局所の平均を引くのは、AとBの明るさの比があっても、紙の上の差が濃い線の上の差より大きく見えて継ぎ目が線に寄るのを防ぐため
- 道筋: 上の行から下の行へ、各行で左右±1 px まで動ける動的計画法で費用の合計が最小の道筋を1本選ぶ。同じ費用ならまっすぐ、左、右の順。最後の行で同点なら候補の中央に近い方、さらに同点なら左。乱数は使わず、同じ入力から同じ道筋を出す
- 位置がずれている所では、線や文字はAとBで別の場所にあるので費用が大きい。継ぎ目は余白を通り、線を横切る回数が少なくなる。帯を横切る線は必ずどこかで横切るので、そこには位置の残差の分の段違いが残る（二重写りにはならない）

### 合成

- 継ぎ目の左はAだけ、右はBだけを使う。継ぎ目を中心に混ぜる幅の中だけ、Bの重みを0から1へ直線で変える
- 帯の外（Aだけ・Bだけの所）は `RenderPair` と同じ画素になる（Bの明るさの補正を除く）

### 明るさ

- 各行で、継ぎ目を中心とする幅（半幅32 px）の窓のA/Bの輝度の和を求め、行方向に箱型の和（半径128行）を2回取ってから比 A/B を出す。比の和で取るので紙の明るい画素が効き、残差より広い窓なので同じ線がA・B両方の窓に入る
- 比はBに掛ける（Aが基準。#362・M2_AUTO_OVERLAP.md 11節の10）。継ぎ目で全量、そこから出力の右端へ向けて直線で1に戻す。継ぎ目から離れたBの中央を明るくしすぎず、白い紙の8 bitの飽和を継ぎ目の近くに限るため
- 3チャンネルに同じ倍率を掛けるので色の比は変えない（#362で色の比はほぼ同じと記録）。ある画素で倍率を掛けると255を超えるチャンネルがあれば、その画素だけ倍率を「最大のチャンネルが255になる値」（1未満にはしない）に下げる。1チャンネルだけを切り詰めて色が変わるのを避けるため。下げた画素の数を記録する
- 帯の中で比が変わる分（周辺減光）は、行ごとの比が継ぎ目の位置に追従する範囲で吸収する。全体に1つの値は掛けない

### 設計定数（承認値ではない）

コードの `kSeamBlendDesign` に置き、報告に毎回記録する。合否や信用の判定には使わない。読込側は記録として範囲だけを確かめる。

| 定数 | 値 | 理由 |
|---|---|---|
| 混ぜる幅 | 8 px | 道筋が1行に1 px動く段とJPEGの8×8ブロックの境目を目立たせない最小限の幅。位置の残差（#367で平均8〜16 px）より狭いので、残差があっても二重写りは幅の中に収まり、線は段違いとして出る。合成画像の2 pxがA0上で約0.2 mmの換算で、約1 mm弱 |
| 帯の端からの余裕 | 64 px | 帯の両端はどちらも画像の端で、双一次補間の端、レンズの歪み、周辺減光が最も大きい。明るさの窓と混ぜる幅が帯の外に出ないよう、窓の半幅の2倍にした。帯の幅（7360 px幅の撮影で数千 px）に比べて小さい |
| 明るさの窓の半幅 | 32 px | 残差より広いので、ずれていても同じ紙と線がAとBの窓に入る。継ぎ目から離れた所の比を混ぜない程度に狭い。費用の局所平均のブロック（64 px）にも使う |
| 行方向のなめらかさ | 半径128行の箱型を2回 | 周辺減光は数千行の幅で変わるので、線や文字の有無による行ごとのばらつきだけを消す |

### 報告 `blend`

読める版（v1〜v3・v5）のどれにも、`seam-cut` の時だけ `blend` を足す。欄の無い報告は線形featherで、従来と同じバイト列になる。schema名は進めていない（#361の `mountOrientation` と同じ扱い）。v5では `automaticAlignment` の後ろ、報告の末尾に置く。この欄を知らない旧版のreaderは未知欄として拒否するので、方式を取り違えない。

```json
"blend":{"method":"seam-cut","widthPixels":8,"edgeMarginPixels":64,
  "seam":{"startX":…,"steps":"0+-…","xMinimum":…,"xMaximum":…,"xMean":…,"differenceMean":…,"differenceMaximum":…},
  "brightness":{"windowHalfWidthPixels":32,"smoothingRows":128,"ratioMinimum":…,"ratioMaximum":…,"saturationLimitedPixels":…}}
```

- 座標は出力JPEGの画素。`steps` は2行目以降の各行の動き（`-`・`0`・`+`）で、長さは出力の高さ−1。道筋そのものを持つので、StillReviewは画像を作り直さずに継ぎ目を表示できる
- `differenceMean`・`differenceMaximum`: 各行の継ぎ目の画素での |A − 比×B| の輝度（8 bitの段）。品質の値ではない
- `ratioMinimum`・`ratioMaximum`: 継ぎ目で掛けた比の範囲
- 読込時は、`method` が `seam-cut` であること（`linear-feather` の明示は拒否。同じ状態を二通りのhashで表さない）、欄の過不足、`steps` の文字・長さ・画像の内側、`xMinimum`・`xMaximum`・`xMean` が道筋から計算し直した値と一致すること、設定値の範囲、比が正で最小≦最大、差が0以上で平均≦最大を確かめる。品質の三つのfalse、向き、v4の判定のやり直しは従来どおり
- 方式の記録は報告の欄だけで、出力JPEGの画素から方式を検証し直すことはしない

### 使う所

- 「両台撮影 → 仮Merge保存」は `seam-cut` で合成する。ログに `pair_review_blend`（継ぎ目のxの範囲、比の範囲、下げた画素数、時間ms、`quality_accepted=0`）を残す
- StillReviewの再Mergeは「合成: 継ぎ目」「合成: 線形」を切り替えて選ぶ。初期値は開いた報告の方式。「保存条件を読込」は条件の方式も読み込む。作業ファイル `a0.still-review-session.v1` の形式は変えず、方式は保存しない
- StillReviewの「継ぎ目を表示」は、保存JPEGの表示に継ぎ目を桃色の線で重ねる（既定は表示しない）。調整プレビュー（半透明・差分・Aのみ・Bのみ）には重ねない。線形featherの報告では線は無い。「継ぎ目へ」は `seam-cut` の報告では中央の行の継ぎ目を表示する。右欄の先頭に方式・継ぎ目のxの範囲・混ぜる幅・比の範囲・次の再Mergeの方式を出す
- 開発用の比較: `A0CameraStitcher.StillReview --report <報告> --blend-compare <新しい出力の接頭辞>` は、報告と同じ幾何で線形featherと `seam-cut` を作り、`<接頭辞>-feather.jpg`・`-seam.jpg` と、中央の行の継ぎ目を中心にした原寸1600×1000の切り出し `-feather-crop.jpg`・`-seam-crop.jpg` を新しいファイルとして書く（既存は上書きせず失敗する）。継ぎ目のxの範囲、比の範囲、時間、継ぎ目のそばの位置のずれの目安（下記）を1行に出す。線形featherの出力が保存JPEGと同じhashになるかも出す
- 位置のずれの目安（`ProbeSeamDisplacement`、診断のみ）: 出力の高さを24等分した行ごとに、継ぎ目を中心とするAの128 px角の勾配の強さを、配置したBの中で±32 pxの範囲で探す。最良が唯一で探索の端に無く、Bで見つけた所からAを探し戻して元の位置に戻る（往復照合、数値の閾値なし）ものだけを数える。継ぎ目は余白を通るので、模様の少ない所では数えられないことが多い。合否には使わない

### 時間と確認

- 7360×4912の撮影2枚で、Releaseの合成は線形featherの約1.5〜2倍、10秒前後。#365の推定（数秒）と復号・保存を足しても両台撮影の180秒の期限に収まる。合成の後にも期限を確かめる（従来どおり）
- `m2_seam_blend_contracts`: 道筋の符号化と復元・範囲外と不正な文字の拒否、既知のずれを入れた縦線の図で継ぎ目が曲がった空白の通り道を通ること、図面風の人工画像で既知のずれ（4, 2 px）を入れた時に継ぎ目がAとBの片方にだけ線のある画素を横切る数が、帯の中央の直線の半分以下で、同じ制約の最小値に近いこと、端の余裕、同じ入力で同じ出力、行方向に変わるゲインを入れた時に継ぎ目の両側の段差が消え比がゲインに追従すること、色の比と飽和、右端で比が1に戻ること、1°の回転と上下のずれ、位置の合ったペアで比が1・帯の外が `RenderPair` と同じ画素、位置のずれの目安が既知のずれを返すこと、上下配置・重なり無し・不正な設定と画素数の拒否
- `still_review_contracts`: 人工JPEGで `seam-cut` の報告の書込と再読、幾何と判定がfeatherの報告と同じこと、同じ入力で同じJPEGと報告のバイト列、featherの報告に `blend` が無いこと、手動の再Mergeでの方式の変更（親は不変）、`rotated-180`、未知の方式の拒否、書き換えた `blend`（`linear-feather` の明示、未知欄、`steps` の長さ・文字、`xMinimum`・`xMean`、負の比、幅0、品質のtrue）の拒否、比較の出力と上書きの拒否、原画像の不変
- `StillReview --smoke` は再Mergeを `seam-cut` で行い、保存JPEGに継ぎ目の線が実際に描かれたことをGDIの画素で確かめる
- 吊り下げリグで撮った保存ペア3組（平行移動だけ2組、相似の補正つき1組。`rotated-180`）を作り直して比べると、線形featherで帯の全体に出ていた二重写りが消え、継ぎ目の両側の明るさの段は目で分からなかった。継ぎ目を横切る線には位置の残差の分の段違いが残る（段2で扱う）。実画像と細かい値は公開しない
