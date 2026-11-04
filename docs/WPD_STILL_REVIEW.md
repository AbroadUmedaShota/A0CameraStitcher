# 両台撮影・未校正JPEG Merge・静止結果の確認 (#334)

> #396 で方針変更（ADR-0037 Accepted、[固定の合成の設計](design/fixed-composite.md)）。仮Merge保存の幾何はCの値でなく固定の合成条件（#399）から取る。**#400 で切り替え済み**（「仮Merge保存を固定の合成条件で行う (#400)」の節、報告は v6）。以下の各節のうち、Cの値・撮影ごとの相似・seam-cut の道筋から幾何を決める記述は、切り替え前の経緯と、v1〜v5 の報告・StillReview の手動の再Merge の説明として残す。

所有者の「いい感じ」「開発を続けて」と「図面撮る前提だから今はある程度」という指示に基づく開発用の接続。仮配置のC表示に対する良好な感触を、最終図面の文字・直線・継ぎ目・色の受入へ読み替えない。承認済みrig profileを作ったり既存の製品用StitchJobの信頼ゲートを回避したりしない。

「両台撮影 → 仮Merge保存」は、現在のCの幅・高さ・重なり・B上下移動・revisionを保存してから両Liveをdrain/Stop/Closeする。撮影の手順は「両台の事前確認（非撮影のpreflightとClose）→ A/B両方のWPDセッションを開いて撮影直前の状態を再確認 → Aのシャッター → Bのシャッター → Aの取り込みとPC検証 → Bの取り込みとPC検証 → 両方Close」で、これを一つの180秒transactionとして扱う（#389。ABの間に原稿が動く時間を減らすため、画像の取り込みを両方のシャッターの後に回す。セッションの持ち方と理由は [WPD_STILL_CAPTURE.md](WPD_STILL_CAPTURE.md) の「両台撮影の順番とセッション」）。Bのシャッターは、Aのシャッターの受付を確認した後にだけ送る。二つのシャッターを送った時刻の差を `shutter_gap_ms` としてログに残す。同時露光は保証しない。Aのシャッターが通らなければBは撮らない。Bのシャッターが通らない、またはどちらかの取り込みが失敗しても、取得済みの原画像は失わない（Bが露光済みなら、Aの取り込みが失敗してもBは一回だけ取り込む。再試行ではない）。自動再撮影・旧画像の補充は行わない。停止要求は未送信のシャッターだけを止め、送信済みの取り込みと保存は続ける。停止要求時から原画像二枚・合成JPEG・確認レポートの検証まで、共通180秒deadlineを使う。別カメラごとの180秒へ延長しない。従来の単独撮影ボタンも残す。

JPEGの幾何は `dx = round((previewWidth-overlap)*jpegWidth/previewWidth)`、`dy = round(vertical*jpegHeight/previewHeight)` とする仮の正規化座標変換。二台のJPEG寸法一致が必要で、上下は両画像が存在する高さにcropする。既存の純粋M2 rendererでfull-size BGRを合成する。ライブビューとJPEGの厳密な画角対応、回転・倍率・台形・レンズ・色・物理DPIは未検証であり、この式は校正結果ではない。誤差を閾値へclampして合格と扱わない。

原画像は `%LOCALAPPDATA%/A0CameraStitcher/wpd-captures` に個別保持し、結果は別の `wpd-pair-reviews/<run>/review/uncalibrated-review.jpg` と `uncalibrated-review.json` に保存する。入力はCAM-A/CAM-Bのcanonical original.jpgと、今回の捕捉結果のSHA-256で結び付ける。相互に同じphysical file、alias違い、hash違い、寸法違い、既存出力directoryは拒否する。入力とancestorをpinし、sourceをwrite/delete禁止のread handleで保持する。

JPEGはCREATE_NEWの所有handleへencodeし、flush、候補identity/size/hash/dimensions検証、handleによる置換禁止rename、保存後の全画素復号とsize/hash再読込を行う。独立した確認レポートもpartial/flush/置換禁止rename/再読込を行い、入力両方のpath/hash/size、pair ID、Cの元条件、実際のJPEG translationと寸法、出力hashを記録する。reportは `productQualityAccepted=false`、`physicalScaleVerified=false`、`liveToStillFieldOfViewVerified=false`。画像・EXIF・private pathをGitHubやリポジトリへ取り込まない。失敗時は取得済みoriginalと出力partialを保持する。

保存したJPEGから復号した縮小画像を、画面下段へ「撮影JPEGの合成結果（静止画／未校正・確認用）」として表示する。A/Bは再開し、「ライブCに戻す」「撮影結果を見る」で切り替える。静止結果を現在のライブ映像と表示しない。手動位置合わせを操作した場合もライブCへ戻る。通常起動に自動撮影はない。`--seconds 60 --pair-smoke` は一回の自動位置候補→実際の両台撮影UI command→原画像/合成JPEG保存→静止表示→両Live再開/終了を行う明示実機テスト。

本entryは開発previewを明示有効にしたbuildだけでcompileする。既存の承認済みprofile用 `StitchCanonicalPair` / `StitchCanonicalPairV2` は変更していない。人工JPEGのgeometry/feather/保存/再読込/原画像不変/誤hash・alias・same-file拒否/期限・途中partial保持を `still_review_contracts`、二つのシャッターが取り込みより前に続けて送られる順序・共通deadline・停止要求・失敗時の原画像保持を `still_pair_contracts`（手順）と `wpd_still_pair_contracts`（偽channelで実際に送るコマンド列とallowlist）で検証する。実機結果は開発状況記録を参照。最終図面の校正受入は別工程。

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

初回のA shutter受付成功後に画像通知が得られず、別の読み取り診断とAF-only操作でAのOutOfFocus（0xA002）を確認した。撮影前と受付後のDeviceReadyでこの拒否を検出し、typedな理由をUIまで渡す。受付成功だけを画像到着・合焦の成功とせず、自動AFやシャッター再送は行わない。両台撮影ではAの受付後のDeviceReadyを、Bのシャッターの後（Aの取り込みの先頭）に確認する。そのためAがOutOfFocusでもBのシャッターは既に送られているが、撮影前に両台のDeviceReadyを確認するので、起きるのは撮影前から受付後の間に状態が変わった場合に限る。合焦拒否時も既存原画像や診断partialを保持し、撮影失敗後のライブ自動再開は無効のままとする。

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

> #396 で方針変更（ADR-0037 Accepted、[固定の合成の設計](design/fixed-composite.md)）。撮影ごとの相似のうち平行移動と回転は合成に当てず、固定の合成条件（#399）で合成し、推定は診断として報告に残す。倍率だけは、左右の差が上限以内なら撮影ごとに当てる（中心は B の画像の中心を固定の写像で移した点。所有者の決定 2026-11-04、#396）（上限を超えたら当てずに不合格。回転は固定の値に縛り、補正の中心は継ぎ目の列の上に置き、推定できない時は当てずにゲートに任せる。所有者の決定（2026-11-04、#396））。#400 で切り替え済み（倍率の部分だけを上限以内で当て、ほかは診断。詳細は「仮Merge保存を固定の合成条件で行う (#400)」の節）。この節は切り替え前の経緯と、v5 の報告の説明として残す。

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
- StillReviewは右欄の先頭に推定の詳細（補正の有無と倍率を当てたか、一致タイル、推定値と当てた値、3通りの残差、開始位置、方針、警告）を出す。手動調整の入力欄の初期値は、開いた報告に保存されている幾何（実際に当てた値）。方針が補正しなかった推定値（方針なし・上限超え・倍率を1に固めた）も自動では入れない。推定値は「推定値を入力欄へ」を押した時だけ入力欄に入れ、調整プレビューに切り替えて状態欄に「推定値を入れました（未保存）」と出す。推定値の無い報告（v1〜v3、推定できなかったv5、手動の子）ではこのボタンを押せない。「作業を読込」は作業ファイルの調整値を、「保存条件を読込」は条件の報告に保存されている幾何を入れ、どちらも推定値は入れない。保存JPEGの表示は変えない。「調整値を戻す」は表示中のJPEGの値に戻す (#382)

### 時間と確認

- 7360×4912の推定は、操作PC-1のReleaseで開始位置3つを回して数秒。復号・合成・保存を足しても180秒の期限に収まる。Debugでは数十倍かかる
- `m2_overlap_similarity_contracts`: 7360×4912の人工画像で、正負の小さい回転と倍率を戻し、相似の後の残差が平行移動だけの時より小さいこと。推定倍率1.0015で、倍率の上限が無い方針・0.1%の方針では当てる倍率が1、回転は相似と同じ角度で、`rigid` の残差が相似以上・平行移動だけ未満になること。1%の方針では推定倍率を当てること。小さい画像で、初期値のずれを広い探索の開始位置で拾うこと、別の深さの帯が合議で外れること、同じ入力で同じ結果、模様の無い画像と一方向の縞では推定しないこと、不正な入力の拒否
- `still_review_contracts`: 方針ファイルの厳格な読込（倍率の欄の有無・null・負・型違い・重複・綴り違い）、上限ちょうど（倍率1.01と0.99は1%の内側、その次の倍精度の値は外側）・符号・上限超え・残差の警告の判定、回転が超えた時の倍率の判定と警告の並び、人工JPEG（倍率1.004）での倍率の上限あり（1%、`corrected`）・なし（`no-scale-policy`、倍率1）・超え（0.2%、`scale-exceeded`、倍率1）・回転超え・`no-policy`・`undetermined`（従来のv1と同じJPEGのbyte列になる）、継ぎ目の差が相似・`rigid`・平行移動だけの順に小さいこと、手動の子の改訂、書き換えたv5（倍率の判定・方針の倍率の値・当てた倍率・`rigid`・警告）とv4・推定器v1の拒否、`rotated-180`、原画像の不変
- 吊り下げリグで撮った保存ペア2組（`rotated-180`）に当てると、どちらも回転は約1°で、相似の後の残差は平行移動だけの時より小さかった。Bの端に近い列のタイルは合議から外れやすく、レンズの歪みが疑われる（※推定）。推定した倍率は2組とも100%との差が1%より小さく、倍率の上限1%の方針では倍率も補正した（上限を置く前と同じ合成）。倍率の上限が無い方針では倍率を1に固め、残差RMSは相似より少し大きく、平行移動だけより小さかった。実画像と細かい値は公開しない

## 設置ごとの固定の合成条件 (#399)

#396 の決定。合成の幾何（B を A に重ねる写像）は撮影ごとに変えず、設置ごとに一度決めた値をすべての撮影で使う。承認済みの rig profile がまだ無いので、開発経路では「固定の合成条件（未承認）」として扱う。この記録は rig profile ではなく、承認を表す欄も持たない。読む側は JPEG の経路（#400）と C の経路（#401）で、今回は書式、検証、作る関数、読み書きの関数と試験までを入れた。

### 作り方と置き場所

- 人が設置の時に明示して一度だけ作る。撮影ごとには作らない。保存済みの review の報告（`LoadSavedStillReview` が通るもの）を渡し、`MakeFixedCompositeCondition(review, 時刻, 倍率補正の上限)` が、その報告が実際に使った幾何を取り出して記録にする。自動の相似（#365）の報告は、推定値ではなく適用された相似（回転が上限超えなら平行移動だけ）。手動の報告（v2・v3）はその値。v1 の報告と、推定できなかった v5 の報告はライブの平行移動
- ファイルは `wpd-fixed-composite-condition.json`。`wpd-merge-policy.json` と同じフォルダ（`%LOCALAPPDATA%\A0CameraStitcher\phase0`）に置く想定。`LoadFixedCompositeCondition(ファイル, ハッシュ関数)` は、ファイルが無ければ「固定の合成条件が無い」（`std::nullopt`）を返し、あるのに読めない・厳密に読めない時は例外にする（壊れたファイルを「無い」と取り違えない）。戻り値にはファイルのバイト列の SHA-256 も付く（#400・#401 が報告に残す用）
- 書き込みの `WriteFixedCompositeCondition` は新しいファイルとしてだけ書く。検証、`.partial` への排他的な作成、書き戻して読み直し、置き換えなしの rename の順で、既にあるファイルは上書きしない（同じ内容でも拒否）。親のフォルダは作らない。既存の `.partial` があれば拒否し、消さない。失敗した時はこの呼び出しが作った `.partial` だけを消す。v1 は最初の記録の書式。記録を変えて保存し直す時（継ぎ目の列の変更など）は、#396 の決定（A3）のとおり、前の記録を消さずに残し、新しい記録が前の記録の SHA-256 を持つ。その親の欄を持つ書式と、どの記録を有効にするかは #403 で足す（v1 は親の欄を持たない）

### 書式 `a0.fixed-composite-condition.v1`

```json
{"schema":"a0.fixed-composite-condition.v1","approval":"unapproved","createdUtc":"2026-11-04T12:34:56Z","source":{"reviewReportSha256":"<64 桁の小文字 16 進>","pairId":"pair-1","method":"live-preview"},"mountOrientation":"normal","sourceDimensions":{"width":64,"height":48},"mapping":{"translation":{"x":32,"y":4},"rotationDegrees":0,"scale":1,"anchor":"image-center","crop":{"left":0,"top":4,"right":0,"bottom":4}},"output":{"width":96,"height":44},"scaleAutoCorrectionMaxPercent":1,"seam":{"column":48,"origin":"band-center-provisional"}}
```

数値は書式の説明用の人工の値で、実測値ではない。

| 欄 | 意味 |
|---|---|
| `approval` | 常に `unapproved`。ほかの値は読めない。`approved` に当たる値は持てない |
| `createdUtc` | 作った時刻（UTC、秒）。実在する日付と時刻だけ |
| `source.reviewReportSha256` / `pairId` | 元にした review の報告のバイト列の SHA-256 と pairId。パスは持たない（報告が移動しても記録は読める） |
| `source.method` | 写像の作り方。報告の `geometryBasis` と同じ名前: `live-preview`（v1、推定できなかった v5）、`manual-still-pixels`（v2）、`manual-still-similarity`（v3）、`auto-still-translation`・`auto-still-rotation`・`auto-still-similarity`（v5）。写像と食い違う名前（平行移動だけの方式で回転が 0 でない、など）は読めない |
| `mountOrientation` | `normal` か `rotated-180`。報告と違い、`normal` も省かず書く |
| `sourceDimensions` | 撮影画像の原寸の幅と高さ（A と B は同じ大きさ） |
| `mapping` | B の画像中心を A に対して `translation`（原寸の画素、整数）へ置き、`rotationDegrees`（度）と `scale` で中心まわりに回して拡大する。座標は向き（`mountOrientation`）を当てた後の原寸の画素。`crop` は出力の切り取りで、写像から求まる「覆われた長方形」と一致しなければ読めない |
| `output` | crop 後の合成の大きさ。写像から求まる値と一致しなければ読めない |
| `scaleAutoCorrectionMaxPercent` | 撮影ごとに倍率だけ補正してよい差の上限（%）。回転と平行移動は固定のままで、超えたらゲートで不合格にする（#396 の決定）。必須。所有者の決定: 1%（初期値）。値は記録を作る時に方針ファイル `wpd-merge-policy.json` の `scaleAutoCorrectionMaxPercent` から写す。コードの既定値にはしていない（記録を作る関数は引数で受け取る）。実機で測った後に見直す前提で、承認された光学的な閾値ではない。範囲は方針ファイルと同じ（有限、0 以上）。倍率の推定と補正そのものは #400 |
| `seam` | 継ぎ目の列。次節 |

`mapping` から `PairRenderParameters` を作る `FixedCompositeRenderParameters` は原寸用で、報告が描いたものと同じ行列と crop を返す。ライブビューの大きさへ縮める関数はここには無い（#398 の `ScaleFixedSeamCondition` を #401 が使う）。

### 継ぎ目の列

継ぎ目は縦にまっすぐな 1 本で、列は記録が持つ（#396 の再改定）。すべての撮影で同じ列を使い、人が C の画面でドラッグして変える（#403、設計待ち）。

- `seam.column`: crop 後の出力の画素の列（0 以上、`output.width` 未満）
- `seam.origin`: `band-center-provisional`（仮。帯の中央）か `operator`（人が動かした）。仮のときは `column` が帯の中央と一致していなければ読めない。人が動かしたら `operator` にする
- 帯は A と B が全出力行で覆っている列の範囲（列の全体が覆われた列だけ数える）。`FixedCompositeOverlapBand(写像, 幅, 高さ)` が写像と crop だけから計算し（画像は読まない）、`ProvisionalSeamColumn` が `(最初の列 + 最後の列の次) / 2`（切り捨て）を返す。例: 帯が 32〜63 列なら 48
- 検証は出力の幅の中にあることまで。混ぜ幅の半分を含めて帯に収まるかは、合成する側（#398 の `RenderPairFixedSeam`、`CanRenderFixedSeam`）が確かめる

### 読めない記録

壊れた JSON、重複キー、違う schema、足りない欄・余計な欄（どの階層でも）、型の違い、有限でない数・範囲外の数（`1e999` を含む）、整数の欄の小数や指数表記、大きさ 0、撮影画像を覆えない写像、未知の向き・方式・出どころ、`approved` などの承認の値、4096 byte 超、通常ファイル以外は、すべて同じ例外で拒否する。先頭の UTF-8 BOM だけは許す。

### 範囲外と将来

- 校正（#374）から作る場合の欄は、この版では持たない。校正由来の出どころが決まったら、その欄を足した新しい schema 名で出す（この版の reader は知らない欄を拒否するので、混ざらない）
- JPEG の経路の切り替えは #400（次の節で入れた）、C の経路の切り替えは #401（[WPD_PREVIEW_MERGE.md](WPD_PREVIEW_MERGE.md) の「固定の合成条件で描くC」）。継ぎ目の列を人が動かす操作と保存し直し方（#403）は含まない
- 記録を作る入口は #400 の開発用 CLI（`StillReview --make-fixed-composite-condition`）。C の画面から作り直す入口は #403 で作る

### 確認

- `m2_fixed_composite_condition_contracts`（`tests/fixed_composite_condition_tests.cpp`、カメラ・実ファイル不要）: 書式の全文、往復、方式ごとの作成（v1・v2・v3・v5 の各判定を人工の値で）、各拒否、帯の計算（回転・倍率つきの写像で、帯の列が全行で覆われ、外側の列が覆われないことを画素ごとに）、仮の継ぎ目と `operator` の扱い、倍率の上限の範囲（方針ファイルの検証と同じ受理）、一時フォルダでの書き込み・読み込み・上書きの拒否
- `still_review_contracts`: 実際に書いた v1・v2・v3・v5 の報告（回転のみ、倍率が上限超え、回転が上限超え、推定なし、`rotated-180`、seam-cut を含む）を `LoadSavedStillReview` で読み、記録の写像・crop・出力の大きさが報告と一致し、保存して読み直せること

## 仮Merge保存を固定の合成条件で行う (#400、親 #396)

「両台撮影 → 仮Merge保存」の合成は、Cの値でも撮影ごとの推定でもなく、固定の合成条件（#399）で決める。撮影の手順（シャッター、取り込み、180秒のdeadline、原画像の保存とhash）は変えていない。ライブのC（`RenderPreviewMerge`）はこの変更では触らず、#401 で切り替える。

### 何が固定で、何が撮影ごとに変わるか

| | 決め方 |
|---|---|
| 写像（平行移動・回転・基本の倍率） | 固定の合成条件。撮影ごとに変えない |
| 継ぎ目の列 | 固定の合成条件の列（縦一直線）。出どころ（`band-center-provisional`／`operator`）も報告に残す |
| 明るさの比 | 撮影ごとに、継ぎ目に沿って測って B に掛ける（#398 の関数の中。計算は #368 と同じ） |
| 倍率の補正 | 撮影ごとに、左右の倍率の差が上限以内なら B にだけ当てる（次の節）。回転と平行移動は直さない |

合成は `RenderPairFixedSeam`（#398）で行う。継ぎ目の左は A、右は B で、混ぜるのは継ぎ目を中心とした 8 px（`kSeamBlendDesign`、承認値ではない）だけ。

### 倍率の補正（ADR-0037 決定6、#396 の B7）

所有者の決定（2026-11-04、#396）: (1) 回転は固定の値に縛り、撮影ごとに変えるのは倍率だけ。(2) 倍率の補正の中心は継ぎ目の列の上に置き、継ぎ目の線を動かさない。(3) 倍率を推定できない時は補正せず、固定の写像のまま合成する（ゲートの判定に任せる）。以下はこの決定の実装で、当てはめの方法（回転を縛る方法、使うタイルの条件）は #400 の設計。

- **モデル**: 固定の写像 H に、1 つの係数 k を点 P を中心に掛ける。`x_A = P + k (H(b) − P)`。P は **B の画像の中心（原寸の (幅/2, 高さ/2)。写像の `anchor:"image-center"` と同じ取り方）を、固定の写像で出力へ移した点**（`FixedScaleCenter`。所有者の決定 2026-11-04、#396、所有者の言葉「B の中心に改める」。同日の最初の決定の「継ぎ目の列上」を置き換える）。ピントや高さの変化は B の光学中心のまわりにかかるので、この中心なら倍率だけの変化にずれが残らない。継ぎ目の線は動かない: 継ぎ目は出力の列で決まり、補正後の窓も左端を保つので、B をどこを中心に拡大しても列は変わらない。回転は H のまま、平行移動は H のまま（P を中心にした拡大が生む移動だけが加わる）。報告の `scaleCorrection.center` は、この P（A の画素）
- **推定**: #365 の推定器（`EstimateOverlapSimilarity`）を、固定の平行移動を初期位置にして走らせる。推定器の相似の当てはめは回転と平行移動も自由に動かすので、その倍率をそのまま使うと、回転や位置のずれが倍率に混ざる。そこで推定器が返した「合意したタイル」（A の点とそれに合う B の点）だけを使い、回転は H に縛ったまま、推定の中だけ平行移動を自由にして k を求め直す。`q = H(b) − P`、`t = a − P` として `t = k q + d`、`k = Σ(q−q̄)·(t−t̄) / Σ|q−q̄|²`、`d = t̄ − k q̄`（`DecideFixedScale`）。平行移動を縛ったまま `t = k q` を解くと、倍率でないずれ（揺れ、1 周期のずれ）がタイルの偏りに応じて k に混ざるため（#400 のレビュー H1）。**当てるのは k だけ**で、`d`（倍率で説明できなかったずれ、P（B の画像の中心を移した点）を原点にした量）は `scaleCorrection.unexplainedShift` に診断として残し、合成には使わない。当てはめが退化する判定は `Σ|q−q̄|² > 0`（使えるタイルが 3 つ以上・2 行以上の条件は従来どおり）。補正の中心 P は `FixedScaleCenter` の 1 か所で決めている（B の画像の中心を固定の写像で移した点。所有者の決定 2026-11-04、#396）。B の中心のまわりの純粋な拡大なら `d` はほぼ 0 になり、`d` が大きい時は B が動いた（揺れ、1 周期のずれ）か、倍率の中心が B の中心でない。推定器自身の相似の値は診断として報告に残すだけで、合成には使わない
- **使うタイル**: 推定器の合意に入っていて、かつ一致の位置が「H が置く位置」から推定器の探索半径以内のもの。繰り返し模様で 1 周期ずれて一致したタイルは、数 % の倍率では説明できない距離にあるので使わない。使えるタイルが 3 つ未満、または 2 行未満なら「推定できない」（`too-few-tiles-near-fixed-mapping`）
- **判定**: `|k×100 − 100|`（`StillScaleDifferencePercent`、#365 と同じ比べ方）が、固定の合成条件の `scaleAutoCorrectionMaxPercent` 以下なら補正する。**ちょうど上限は補正する**。上限を超えたら補正せず（k=1、上限で切った部分的な補正はしない）、報告と画面に「倍率の差が上限を超えた。ピントか高さを確かめてください」を残す。ゲートでの不合格は WP-04 でつなぐ（今回は報告と画面の案内まで）。推定できない時（推定器が決められない、タイルが足りない、当てはめが退化する）は補正せず、理由を報告に残す。補正すると継ぎ目が重なりの外に出る時も補正せず、`seam-outside-overlap` と残す
- **補正した時の範囲**: 拡大した B から覆える窓を計算し直す（`FixedCompositeScaledGeometry`）。窓の左端は固定の条件と同じなので、継ぎ目の列の意味は変わらない。出力の大きさは、k に応じて右端と上下が少し変わる
- 値の出どころ: 上限は固定の合成条件の記録が持つ値。コードには 1% も既定値も入れていない。この節の判定に承認された閾値は無い（上限は所有者の初期値で、実機で測った後に見直す前提）

### 倍率の上限は記録の値が正

倍率の上限は、方針ファイル（`wpd-merge-policy.json`）と固定の合成条件の記録の 2 か所にある。**合成で使うのは記録の値**（設置の時に写した値）で、撮影ごとに方針ファイルの値へ切り替えない。上限を変えたい時は、方針ファイルを直してから固定の合成条件を作り直す（既存の記録は上書きしないので、新しい記録として作る）。

方針ファイルとの照合は分類して報告に残すだけで、合成の結果は変わらない（`policyLimit.check`）。

| `check` | 意味 | 画面と報告 |
|---|---|---|
| `same` | 方針ファイルの値が記録と同じ | 注記なし |
| `differs` | 値が違う | 「方針ファイルの倍率の上限が固定の合成条件と違います。固定の合成条件を作り直してください」と、方針ファイルの値と記録の値を両方出す。合成は記録の値で行う |
| `policy-absent`／`policy-unreadable`／`policy-without-scale-limit` | 方針ファイルが無い・読めない・倍率の上限の欄が無い | 「方針ファイルの倍率の上限を確かめられないため、固定の合成条件の記録の値で判定しました」。合成は止めない |

WpdDualPreview は方針ファイルを起動時に一度読む（読めない内容なら起動しない、#365 から変わらない）ので、撮影時に出るのは `same`／`differs`／`policy-absent`／`policy-without-scale-limit` の 3 通りと考えてよい（`policy-unreadable` は報告の書式が表せる値として持つ）。

### 合成しない時

固定の合成条件が無い、壊れている、取り付けの向きや原寸の大きさが撮影と合わない、継ぎ目の列がその撮影の重なりの外にある、のどれかなら**合成しない**。この時も原画像 2 枚の保存、hash、180 秒の期限は今までどおりで、合成のJPEGと報告（`uncalibrated-review.*`）は作らない。理由は、画面（撮影の状態欄）、ログ（`pair_composite_skipped`）、撮影の記録（`OriginalsSavedCompositeSkipped` と理由の符号）に出す。

| 理由の符号 | 画面の文 |
|---|---|
| `condition-absent` | 固定の合成条件がありません。設置の時に作ってください |
| `condition-invalid` | 固定の合成条件を読めません（内容が正しくありません）。設置の時に作り直してください |
| `orientation-differs` | 固定の合成条件の取り付けの向きが、この撮影と合いません |
| `size-differs` | 固定の合成条件の画像の大きさが、この撮影と合いません |
| `seam-outside-overlap` | 固定の合成条件の継ぎ目の列が、この撮影の重なりの外にあります |

壊れた条件は例外で撮影を止めず、合成しない側に倒す（原画像は保存済み）。

### 報告 `a0.uncalibrated-still-review.v6`

v1〜v5 の reader と書き出しのバイト列は変えていない。`LoadSavedStillReview` は v6 も読む。v6 の形は固定で、`blend` は必須。

- 共通の欄: `schema`、`pairId`、`state`、品質の三つの false、`automaticRetryCount`、`inputs`、`previewGeometry`、`mountOrientation`（`rotated-180` の時だけ）、`sourceDimensions`、`output`
- `translation`・`adjustment`（`rotationDegrees`・`scale`）: **固定の写像**。`geometryBasis` は `fixed-composite`
- `crop`: 実際に描いた窓（倍率を当てた時は計算し直した値）
- `fixedComposite.condition.sha256`: 使った条件ファイルのバイト列の SHA-256
- `fixedComposite.seam`: `column`（crop 後の出力画素）、`origin`（`band-center-provisional`／`operator`）
- `fixedComposite.scaleCorrection`: `status`（`applied`／`exceeded`／`not-estimated`／`seam-outside-overlap`）、`reason`、`limitPercent`、`estimatedScale`、`differencePercent`、`appliedScale`、`center`（補正の中心 P: B の画像の中心を固定の写像で移した点、A の画素）、`usedTiles`、`residualPixels`、`unexplainedShift`、`notice`（`exceeded` などの案内文。`applied` は空）
- `fixedComposite.policyLimit`: `check`、`policyPercent`、`conditionPercent`、`notice`
- `fixedComposite.diagnostic`: #365 の推定（`status`、`reason`、`initialPosition`＝`fixed-mapping`／`still-wide-search`、`tiles`、`similarity`、`residualPixels`）と、固定の写像からの動き `deltaFromFixed`（平行移動 x・y、回転、倍率の比）。**合成の幾何には使っていない**
- `blend`: `method` は `fixed-seam`。`widthPixels`、`edgeMarginPixels`、`differenceMean`、`differenceMaximum`、`brightness`（`ratioMinimum`・`ratioMaximum`・`saturationLimitedPixels` ほか）。継ぎ目の列は `fixedComposite.seam` に一度だけ書く

reader は、導けるものを全部計算し直して照合する（`status` と `reason` と数値の整合、`difference` の再計算、中心の位置、窓の crop と出力の大きさ、継ぎ目の列が帯の中、`band-center-provisional` なら帯の中央、`notice` の文、`deltaFromFixed`）。食い違う報告、知らない欄、余分な欄は拒否する。

#368 の道筋を選ぶ seam-cut を通した診断は、報告に入れていない。撮影のdeadline（180秒）の中で、推定と合成に加えてもう一度、重なりの全行で道筋を探す描画を走らせることになり、実機の撮影サイズで測れていないため。同じ診断は、保存した報告から `A0CameraStitcher.StillReview --report <報告> --blend-compare <出力の接頭辞>`（期限なし、ファイルだけ）で得られる。v6 の報告にも使え、固定の写像と倍率の補正を当てた後の幾何で比べる。

### 固定の合成条件を作る入口（開発用）

```
A0CameraStitcher.StillReview --make-fixed-composite-condition <保存済みの報告> --settings-folder <フォルダ>
```

- 保存済みの報告（`uncalibrated-review.json`）を明示して、設置の時に一度、人が実行する。自動では作らない
- フォルダ内の `wpd-merge-policy.json` の `scaleAutoCorrectionMaxPercent` を記録に写す。方針ファイルが無い、読めない、その欄が無い時は作らず、理由を出す（コードの既定値は無い）
- `wpd-fixed-composite-condition.json` が既にあれば作らない（#399 の書き込みのとおり上書きしない）。固定の合成条件で作った報告からは作れない（設置の時の報告を指定する）
- 実装は `MakeFixedCompositeConditionFile`（`fixed_composite_tool.hpp`）。保存場所は引数で明示し、既定のフォルダには触れない

### 表示

- WpdDualPreview: 撮影の状態欄に、継ぎ目の列と出どころ、倍率の補正、方針ファイルとの照合、注記を 1 行で出す。撮影結果の画像（表示用の縮小コピー。保存したJPEGには描かない）には、継ぎ目の列に桃色の縦線を重ねる
- StillReview: 倍率の補正を当てた報告では、入力欄と「調整値での残差」は固定の写像で計算されるため、状態欄とパネルに「倍率の補正 ×k は入力欄に含まれません」と出す。横のパネルに、条件の SHA-256、継ぎ目の列と出どころ、倍率の補正、方針ファイルとの照合、診断（固定の位置からの動き）を出す。「継ぎ目を表示」で、保存JPEGに継ぎ目の縦線を桃色で重ねる（v6 は `blend` が固定の継ぎ目として読まれるので、既存の表示がそのまま使える）。表示の入切は既存のボタン
- StillReview の手動の再Merge（#336・#338）は、保存済みの組を調べる道具として残す。v6 の報告を親にして再Mergeすると、固定の写像（倍率の補正なし）を初期値にした手動の改訂（v2／v3）が親を残した別保存になる

### 試験

- `m2_fixed_scale_correction_contracts`（`tests/fixed_scale_correction_tests.cpp`）: 倍率を当てた写像の性質（回転が同じ、拡大が k 倍、P が動かない、継ぎ目の線上の点が縦線のまま）、厳密なモデルでの判定（ちょうど上限は補正、上限をわずかに下げると不合格、0.78125% など 2 進で正確な値）、推定できない各場合（推定器が決められない、タイル不足、1 行だけ、1 周期ずれ、合意の外、中心にしか無い、反転）、方針ファイルの分類、生成した画像での推定（上限以内・以下側・ちょうど 1・上限超え）と、補正後に継ぎ目の列が動かず食い違いが縮むこと、内容の違う 2 組で中心と回転が同じこと
- `still_review_contracts`: v6 を実際に書いて読む往復、改ざんした報告の拒否、倍率の上限超えと案内文、方針ファイル 3 通り（同じ・違う・無い）、内容の違う組で写像と継ぎ目の列が同じ、条件の無い・壊れた・向きや大きさが違う・継ぎ目が重なりの外の各場合に合成せず出力フォルダも作らず原画像が残ること、他の指定との排他、条件を作る入口（方針ファイルの上限が写る、上書きしない、方針ファイルが無い／上限が無い時は作らない）、v6 を親にした手動の再Merge

### 未検証

- 実機の撮影サイズ（D810 の約 3600 万画素）での所要時間。推定と合成は #365・#368 と同じ量の処理だが、180 秒の期限内に収まるかは未測定
- 繰り返し模様での誤推定。探索半径を使った除外は 1 周期が半径より大きい時だけ効く。周期が半径以内の模様では、倍率が誤って推定されうる。推定の残差（`scaleCorrection.residualPixels`）を報告に残しているが、これで弾く閾値は決めていない（※要確認）
- 吊り下げのままの撮影で B が組ごとに動く場合は、固定の写像からのずれとして `deltaFromFixed` に出る。ゲート（#385）でどう扱うかは WP-04

## 継ぎ目を1本の道筋にした合成と、継ぎ目に沿った明るさ合わせ (#368)

> #396 で方針変更（ADR-0037 Accepted、[固定の合成の設計](design/fixed-composite.md)）。書き出す合成は、行ごとに曲がる道筋でなく縦一直線の継ぎ目（#398。列は固定の合成条件に保存した列を使い、報告に記録する。列の操作は #403）で行う。明るさの比の計算は同じものを使う。#400 で切り替え済み（v6 の報告）。この節の seam-cut は、StillReview の手動の再Merge と `--blend-compare` に残る。

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

- （#400 まで）「両台撮影 → 仮Merge保存」は `seam-cut` で合成していた。#400 以降は固定の継ぎ目（v6）で、`seam-cut` は StillReview の手動の再Merge と `--blend-compare` に残る。ログに `pair_review_blend`（継ぎ目のxの範囲、比の範囲、下げた画素数、時間ms、`quality_accepted=0`）を残す
- StillReviewの再Mergeは「合成: 継ぎ目」「合成: 線形」を切り替えて選ぶ。初期値は開いた報告の方式。切り替えても入力欄の幾何は変わらないので、方式だけを変えて再Mergeした子は親と同じ幾何になる。「保存条件を読込」は条件の方式も読み込む。作業ファイル `a0.still-review-session.v1` の形式は変えず、方式は保存しない
- StillReviewの「継ぎ目を表示」は、保存JPEGの表示に継ぎ目を桃色の線で重ねる（既定は表示しない）。調整プレビュー（半透明・差分・Aのみ・Bのみ）には重ねない。線形featherの報告では線は無い。「継ぎ目へ」は `seam-cut` の報告では中央の行の継ぎ目を表示する。右欄の先頭に方式・継ぎ目のxの範囲・混ぜる幅・比の範囲・次の再Mergeの方式を出す
- 開発用の比較: `A0CameraStitcher.StillReview --report <報告> --blend-compare <新しい出力の接頭辞>` は、報告と同じ幾何で線形featherと `seam-cut` を作り、`<接頭辞>-feather.jpg`・`-seam.jpg` と、中央の行の継ぎ目を中心にした原寸1600×1000の切り出し `-feather-crop.jpg`・`-seam-crop.jpg` を新しいファイルとして書く（既存は上書きせず失敗する）。継ぎ目のxの範囲、比の範囲、時間、継ぎ目のそばの位置のずれの目安（下記）を1行に出す。線形featherの出力が保存JPEGと同じhashになるかも出す
- 位置のずれの目安（`ProbeSeamDisplacement`、診断のみ）: 出力の高さを24等分した行ごとに、継ぎ目を中心とするAの128 px角の勾配の強さを、配置したBの中で±32 pxの範囲で探す。最良が唯一で探索の端に無く、Bで見つけた所からAを探し戻して元の位置に戻る（往復照合、数値の閾値なし）ものだけを数える。継ぎ目は余白を通るので、模様の少ない所では数えられないことが多い。合否には使わない

### 時間と確認

- 7360×4912の撮影2枚で、Releaseの合成は線形featherの約1.5〜2倍、10秒前後。#365の推定（数秒）と復号・保存を足しても両台撮影の180秒の期限に収まる。合成の後にも期限を確かめる（従来どおり）
- `m2_seam_blend_contracts`: 道筋の符号化と復元・範囲外と不正な文字の拒否、既知のずれを入れた縦線の図で継ぎ目が曲がった空白の通り道を通ること、図面風の人工画像で既知のずれ（4, 2 px）を入れた時に継ぎ目がAとBの片方にだけ線のある画素を横切る数が、帯の中央の直線の半分以下で、同じ制約の最小値に近いこと、端の余裕、同じ入力で同じ出力、行方向に変わるゲインを入れた時に継ぎ目の両側の段差が消え比がゲインに追従すること、色の比と飽和、右端で比が1に戻ること、1°の回転と上下のずれ、位置の合ったペアで比が1・帯の外が `RenderPair` と同じ画素、位置のずれの目安が既知のずれを返すこと、上下配置・重なり無し・不正な設定と画素数の拒否
- `still_review_contracts`: 人工JPEGで `seam-cut` の報告の書込と再読、幾何と判定がfeatherの報告と同じこと、同じ入力で同じJPEGと報告のバイト列、featherの報告に `blend` が無いこと、手動の再Mergeでの方式の変更（親は不変）、`rotated-180`、未知の方式の拒否、書き換えた `blend`（`linear-feather` の明示、未知欄、`steps` の長さ・文字、`xMinimum`・`xMean`、負の比、幅0、品質のtrue）の拒否、比較の出力と上書きの拒否、原画像の不変
- `StillReview --smoke` は再Mergeを `seam-cut` で行い、保存JPEGに継ぎ目の線が実際に描かれたことをGDIの画素で確かめる
- 入力欄の初期値（#382）: `still_review_contracts` は人工JPEGの `rotation-exceeded`・`scale-exceeded`・`no-policy`・`corrected`・推定なしの報告で、入力欄の初期値が保存されている幾何、推定値が相似の値（描けない値と推定なしは無し）になること、保存されている幾何で方式だけ変えた子が親と同じ幾何、推定値で作った子が推定値を持つことを確かめる。`StillReview --smoke` は既存の段の前に、開いた直後の入力欄が保存されている幾何であること、方式だけを切り替えた再Mergeの子が親と同じ幾何になること、作業の読込で親に戻ってから「推定値を入力欄へ」を押して再Mergeした子に推定値が手動として入ること（推定値の無い報告ではボタンの拒否）を確かめる
- 吊り下げリグで撮った保存ペア3組（平行移動だけ2組、相似の補正つき1組。`rotated-180`）を作り直して比べると、線形featherで帯の全体に出ていた二重写りが消え、継ぎ目の両側の明るさの段は目で分からなかった。継ぎ目を横切る線には位置の残差の分の段違いが残る（段2で扱う）。実画像と細かい値は公開しない

## 縦一直線の継ぎ目の合成関数 (#398、親 #396)

`RenderPairFixedSeam`（`a0_m2_seam_blend`、カメラ・ファイル・時計に依存しない）は、#368 と同じ混ぜ方・明るさ合わせで、道筋を探さずに、呼び出し側が渡した出力の列を全行の継ぎ目にして合成する。**この節の時点では JPEG と C の経路は切り替えていなかった**（JPEG は #400、C は #401 で、それぞれの節に書いた）。この関数は新しい入口を足しただけで、`RenderPairSeamCut` と既存の経路の出力は変わらない。

- 入力: 向きを当てたA/B、`PairRenderParameters`（写像とcrop）、継ぎ目の列（crop後の出力画素）、`SeamBlendSettings`。列は呼び出し側が渡す（#399 の設置ごとの固定の合成条件の列。人が C の画面で変える #403）。関数は列を動かさないので、同じ列を渡せば画像の内容が変わっても継ぎ目はその列のまま
- 合成: `RenderPairSeamCut` と同じ内部処理を共有する。継ぎ目の左はA、右はB、混ぜる幅の中だけ線形。明るさは継ぎ目の周りの窓でAとBの比を行ごとに求め、行方向にならしてBに掛け、右端へ向けて1へ戻す。`RenderPairSeamCut` は処理の前半（帯の測定と道筋の探索）だけが違い、後半（明るさの測定と混ぜ）は同じ関数を通る。分割の前後で出力が同じことは、固定した6通りの入力のハッシュ（画像・継ぎ目・行ごとの比）で確かめる
- 失敗: 混ぜ幅の重みが0と1の間になる列（継ぎ目の列±(ceil(幅/2)−1)）が、どこかの行でAとBの両方に写らない、または帯の外に出る時は `std::invalid_argument` で合成しない。`edge_margin_pixels` はこの関数では使わない（値は報告にそのまま写す）。余裕は道筋の探索を帯の縁から離すための値で、渡された列は重なりそのものに対して確かめるため。縁からの余裕を見たい場合は、列を決める側（#399・#403）が決める
- `CanRenderFixedSeam`: 同じ入力で、その列なら合成できるかを、画像を作らずに返す（帯の標本化だけ）。#403 のドラッグできる範囲の計算など、列を決める側が先に確かめるために使える。レイアウト・バッファ・設定の不正は例外のまま
- 報告の欄: `SeamBlendRecord::fixed_seam`（この関数だけが真）。`seam_x` は全行が同じ列。報告の書式は #400 で決めた（v6、`method` は `fixed-seam`）
- 縮尺: `ScaleFixedSeamCondition` は、撮影画像の画素で表した条件（写像・crop・継ぎ目の列）を、ライブビューの大きさへ縮める。写像は S·H·S⁻¹（S は幅・高さの比）で、Bの置き場所は縮尺の比のまま厳密に対応する。cropの端と継ぎ目は全体の座標で縮尺の比を掛けて整数画素に丸める。継ぎ目は縮めた画素の0.5以内、cropの端は内側へ丸め、キャンバスの端で止めるので、動く量は縮めた画素の2未満で、縮めた撮影窓の外へは出ない。ライブビューが撮影と同じ視野を縮めたものという対応は正規化座標の仮定で、光学的には未検証（#334）。C側の使い方は #401 で決める
- 試験（`m2_seam_blend_contracts`）: 平らな合成画像で左はAだけ・右はBだけ・混ぜ幅の中だけ混ざる、模様をずらしても同じ列のまま・Bの置き場所が変わらない、B全体を0.8倍にした登録済みの図で比が窓の和の比（行方向に2回ならす）に一致し継ぎ目の段が消える、同じ入力で同じバイト列、重なりの端が行ごとに動く回転ペアで重なりから外れる列と混ぜ幅の半分がはみ出す列が失敗、`CanRenderFixedSeam` が合成の成否と一致、探索の結果が真っすぐな時にその列を渡すと `RenderPairSeamCut` と同じ出力、縮めた条件の写像・継ぎ目・窓が縮尺の比で対応
