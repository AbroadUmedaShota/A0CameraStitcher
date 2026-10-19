# 開発状況 2026-10-15（#306、#311〜#322）

本日のローカル候補の状況。mainには未反映。昨日までのClaudeの#311作業を確認し、未検証だった変更を仕上げた。所有者の方針は**先に保存画像の合成本体、その後にLive Viewの合成C**（#306）。`CURRENT_STATUS.md`の10月13日時点の「合成は後回し」は過去の記録で、現在の優先順ではない。

- #311: anchorの格子ずれ・ID誤記・鏡映をチャート模様で拒否する処理と回帰試験。全148チェック、関連CTestはRelease/Debugとも14/14成功
- #312: 明示したreference pairと既知配置から固定文書写像を計算。学習用のposeを流用せず、両側成功時だけ写像を返す。Release/Debugの数値計算試験成功
- #313: 所有者の回答「既存形式＋詳細レポート」に従い、既存profile v2のdraftと全観測・学習・独立検証のreportを生成。provenanceId＋fingerprintで結合。独立検証が欠ける入力ではprofileを発行しない。writerはRelease/Debug各44チェック、既存.NET共有vector163＋生成互換性7チェック成功
- #314: 7360×4912の合成JPEG18枚から検出、校正、draft、既存レンダラーのA0全域の評価プレビューまで成功。検出3882点のID取り違え0。固定写像と独立真値の差はCAM-A rms 0.0076 / max 0.0160 px、CAM-B rms 0.0062 / max 0.0176 px。詳細と再実行方法は[全工程の合成試験](M2_FULL_CALIBRATION_PIPELINE.md)
- #315: [1コマンドの開発用校正](M2_CALIBRATION_COMMAND.md)。用意したJPEGと2つのjobから、既存native CLIで観測・draft・詳細レポートを一式保存。入力jobのロック、段階ごとの失敗、非上書き公開、入力と成果物のhashを記録する。実CLIの46チェック、生成profileの.NET互換性7チェックと既存schemaが成功。実機操作や品質承認は追加しない
- #316: [文書座標の上限つき相似補正](M2_DOCUMENT_CORRECTION.md)の計算部。対応済みの点から平行移動・回転・倍率だけを推定し、明示したpolicy内の候補を固定Hへ合成。上限超過・反射・縮退・不成立の投影は適用可能な写像を返さない。新規304チェックはRelease/Debug成功、関連CTestはRelease 6/6・Debug 5/5成功。画像からの対応点検出、既存profileの上限との対応づけ、製品経路は後続
- #317: [raw画素→文書座標→上限つき補正](M2_RAW_DOCUMENT_CORRECTION.md)を接続。対応済みCAM-A/Bの画素を固定K/歪み/Hで戻し、全点・両側の成功時だけ#316へ渡す。逆写像の一意性が確認できないdomain、非収束、画像外/domain外、往復誤差超過は理由つきで拒否し、点を間引かない。新規411チェックはRelease/Debugとも成功、関連CTestはRelease 7/7・Debug 6/6成功。画像の特徴点検出・マッチング、profileの承認値との対応、製品経路は後続
- #318: [固定写像に誘導された画像patch照合](M2_IMAGE_PATCH_CORRECTION.md)を追加。指定した文書seed周辺の画像内容から対応を探索し、全seedの両方向照合が成立した場合だけ#317→#316へ接続。白紙・反復模様・探索端・未被覆・逆照合不一致では補正候補を出さない。新規90チェックはRelease/Debug成功、関連CTestはRelease 8/8・Debug 7/7成功。保存・再読込みした合成JPEGでは平均pixel誤差が未補正10.3465→補正後0.077375 DN、両構成の全5成果物もbyte一致。seed自動選定・汎用特徴点検出、製品経路・実写の受入は後続
- #319: [照合用seedの自動選択](M2_AUTOMATIC_PATCH_SEEDS.md)。固定gridから両側の探索support被覆とAのcontrast・二次元textureで候補を評価し、間隔/点数/広がりも確認。選択後は集合を固定し、照合失敗を見て点を捨てたり再選択しない。手指定seedなしに#318→#317→#316→既存rendererへ接続。計168チェック（追加78）はRelease/Debug成功、関連CTestはRelease 8/8・Debug 7/7成功。保存・再読込みした合成JPEGでは20候補→6点を選び、平均pixel値誤差は未補正10.3465→自動補正0.07725 DN。両構成の全7成果物もbyte一致。承認profile値・製品経路・実写の受入は後続
- #320: [保存JPEGの自動補正つきdraft評価CLI](M2_CORRECTION_EVAL.md)。任意のstrict jobで#319→#318→#317→#316を呼び、成立時は元画像から補正つきJPEGと詳細report、最後にhash結合したv2評価manifestを保存。数値的な不成立はreportのみ/exit 3、既存の補正なしCLI/v1は維持。7360×4912の人工JPEGで既知のraw移動(+3,-2px)→文書移動(+0.25,+0.375mm)を復元。独立した未移動JPEG対の基準renderに対する平均画素値誤差はbilinear 15.0258→1.575 DN、bicubic 18.2148→1.48594 DN。process-only試験1747チェック、証拠保存modeでは2041チェック、schema104チェック。実写の品質・承認値への結合・通常アプリ接続は後続。作業は専用branchのローカル変更でmain未反映

- #321: [上限つき明るさ・色補正](M2_COLOR_CORRECTION.md)のpure計算部。幾何補正後の対応色からBGR別gain/offsetを推定し、全点・全channel・明示上限が通った場合だけ、一度samplingしたCAM-Bの色をblend前に補正する。人工画像で平均誤差20.3302→0 DN。製品の承認値や実写の色品質、CLI/通常アプリへの接続は後続。#320の未commit変更を保持したローカル作業
- #322: [保存JPEGの位置・色補正つき評価](M2_COLOR_EVAL.md)。任意のstrict色jobを幾何jobと併用し、全選択seedで色を観測、成立時だけ単一samplingへ適用。色reportに全観測/推定/残差/違反と入力/profile/job/幾何reportのhashを記録し、成功は最後のv3 manifestへ結合。色拒否では両reportだけを保持し、JPEG/成功manifestなし。両構成の実CLI各5118チェック、schema各236チェック成功。人工JPEGの平均誤差は幾何のみ12.5687→色適用1.5 DN（bilinear）、12.782→1.55859 DN（bicubic）。全43成果物は構成間一致、従来21成果物も変更前と一致。実写・通常アプリ・承認条件への結合は後続。未commit・main未反映

#314統合時の関連Release CTestは7/7成功（144.66秒）。合成JPEG由来profileの.NET互換性7チェックと既存profile/sessionのJSON schemaも通過。重い18枚の全工程はReleaseだけで実行した。#317ではこの重い校正工程を再実行していない。

#320の関連CTestはRelease 11/11（9件＋CLI/schema 2件）、Debug 10/10成功。保存した全21成果物は構成間でbyte一致。実機・全体suite・重い18枚の校正工程は今回再実行していない。

#321の新規152チェックはRelease/Debugとも成功。関連CTestは両構成12/12（生成器依存分離36ケースを含む）。色補正未指定の既存評価CLIの全21成果物は、両構成とも変更前とbyte一致した。新しい色補正はdecoded bufferで検証した段階で、保存JPEGや実写の受入ではない。

これは合成入力での開発経路。実写の検出・色/継ぎ目/解像品質、最終リグ（#43）、品質条件（#42）、approved profile（#45/#49）、撮影ごとの補正（#46）、アプリ本体への接続（#274）は引き続き残る。二台の新しい実機系列も未完了。カメラ・SDK・WPDの操作は本日の作業に含めていない。

Live Viewの合成は#306とADR-0036草案（#307）の後続。今回のdraftを操作者の通常画面のCに使わない。Cの取得方式の規則改定・許諾確認・実機根拠は、保存画像の合成の成功だけでは解決しない。
