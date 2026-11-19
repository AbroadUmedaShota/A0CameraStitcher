# 固定の合成条件 v3 と報告 v8・v9 の書式

Issue #417（撮影ごとに図面の中身で左右のカメラの継ぎ目を合わせる）で、複数の実装が同時に書く条件 v3 と報告 v8 の欄を 1 か所に固めた表。設計の正本は `fixed-composite-registration.md` の 11 節（11.9 の配線、11.17 の警告、11.19〜11.22、11.25、11.27）。この文書は欄の名前・型・単位・必須か・埋める担当・予約かを決めるだけで、処理の説明は設計に任せる。実測の値、機体の識別子、パス、作業の日付は書かない。

状態: v3 と v8 は main に入るまで凍結しない。v8 は main に入ったので、その後の変更は報告 v9（4.13）にした（#437。v8 の読み手は残す）。表の中身は、設計の追補（11.16 以降）と所有者の決定（12 項）に合わせてある。所有者の確認済み: 古い試し撮りの記録（この書式より前の v8）は読めなくてよい。版は上げず、欄を省略可にもしない。v3 の `basePose` が `registration.envelope` の内にあることは手直し（`manual-adjustment`）の書き込みと読み込みでは求めず、条件を撮影やライブの C に使う読み手が求める。

## 1. 欄を足す時の決まり

1. 欄を足す・変える時は、書き手、読み手、試験の組み立て関数（`tests/registration_fixtures.hpp`）、この文書を同じ commit で変える。統合は tech_lead が行う
2. 読み手は厳格。未知の欄、重複した欄、欠けた欄、型の違いを拒否する。欄は省かない。「当てない」「まだ無い」は null で書く
3. 消費する担当が出た欄は、名前・型・単位の意味を変えない。意味を変える時は新しい名前の欄を足し、古い欄を同じ commit で全員から消す
4. 単位のある数の欄は名前に単位を付ける（`Mm`・`Px`・`Deg`・`Percent`）
5. 数は有限で、丸めない。書き方は 2 節
6. 符号（列挙）は kebab-case の文字列。符号を足す時は、書き手、読み手、TXT の文、試験を同じ commit で足す
7. 試験は v3・v8 の文字列を直書きせず、`registration_fixtures.hpp` の関数を通す
8. main に入った後の変更は新しい版（v4・v9）にし、古い読み手を残す。別の版を並べて作らない（設計 11.15 の「版の分かれ」）。例外として、閉じた符号の欄に符号を足すだけの変更は版を上げない。その時は次の 4 つを同じ commit で満たす。(a) 既にある欄の名前・型・単位・意味と、既にある符号の意味を変えない。新しい符号が使う欄は既にある欄だけ。(b) 古い読み手は知らない符号を拒否し、使う側は当てない・合成しない・原画像を残す側に倒れる。これを知らない符号を入れた試験の入力で確かめる。(c) 新しい読み手は今あるファイルを前と同じに読み、既存の試験が試験のコードを変えずに通る。(d) 足した符号と、それを読めない版の境目を表の注に書く

## 2. 書き方の共通事項

- 文字コードは UTF-8、BOM なし。条件は末尾の改行 1 つまで（v2 と同じ。書き手が最後に `\n` を付ける）。報告は v7 と同じ作り
- 空白を入れない 1 行の JSON。メンバーの並びはこの文書の表の上から下（正本の並び）。読み手は並びを問わないが、書き手は正本の並びで書き、読み直して同じ文字列になることを確かめる
- 数: 浮動小数は `std::ostringstream`、`std::locale::classic()`、`std::setprecision(17)` の既定の形式（`%.17g` 相当）。整数の欄は整数として書く。`-0` は書かず `0` と書く（※仮定: v2 の書き手と同じ扱い）。`NaN`・`inf` は書かない
- 真偽は `true`／`false`。null は `null`
- ハッシュは 64 桁の小文字 16 進。日時は `YYYY-MM-DDTHH:MM:SSZ`
- 単位つきの量は欄の名前の末尾で示す。`Px` は出力ラスタの画素、`Mm` は図面の mm、`Deg` は度、`Percent` は %

## 3. 条件 v3（`a0.fixed-composite-condition.v3`）

置き場所: 設定のフォルダの `wpd-fixed-composite-revisions/r<6 桁>.json`（設計 11.19.4）。大きさの上限は 16 KiB（`kMaximumFixedCompositeConditionV3Bytes`、※案）で、上限 + 1 byte を拒否する。型は `src/m2/include/a0/m2/experimental/fixed_composite_condition_v3.hpp` の `FixedCompositeConditionV3`。

「埋める担当」: C0＝この commit が固定値で書く、V3＝条件 v3 の担当、LC＝ライブの C の担当。

| JSON の名前 | C++ の名前 | 型 | 単位 | 必須 | 埋める担当 | 予約 |
|---|---|---|---|---|---|---|
| `schema` | `kFixedCompositeConditionSchemaV3` | 文字列（定数 `a0.fixed-composite-condition.v3`） | — | 必須 | C0 | |
| `approval` | （定数） | 文字列 `"unapproved"` のみ | — | 必須 | C0 | |
| `createdUtc` | `created_utc` | 日時 | — | 必須 | V3 | |
| `revision.sequence` | `revision.sequence` | 整数 1〜999999 | — | 必須 | V3 | |
| `revision.parentSha256` | `revision.parent_sha256` | ハッシュか null | — | 必須 | V3 | |
| `revision.change.kind` | `revision.change.kind` | `created`／`migrated-from-v2`／`saved-estimate`／`manual-adjustment`／`rollback`／`values`／`recalibrated` | — | 必須 | V3（`recalibrated` は #434） | |
| `revision.change.v2Sha256` | `change.v2_sha256` | ハッシュ | — | `migrated-from-v2` の時だけ | V3 | |
| `revision.change.reportSha256` | `change.report_sha256` | ハッシュ | — | `saved-estimate`・`recalibrated` の時だけ | V3 | `recalibrated` では取り直しの推定の記録（`a0.pair-recalibration-estimate.v1`）の SHA-256 |
| `revision.change.pairId` | `change.pair_id` | 文字列 | — | `saved-estimate`・`recalibrated` の時だけ | V3 | `recalibrated` では取り直しに使った撮影の組 |
| `revision.change.entered` | `change.entered` | `{seamDxPx, seamDyPx, rotationDeg}` 有限 | 出力 px・度 | `manual-adjustment` の時だけ | V3（画面の入口は #418 段 2） | |
| `revision.change.maximumSeamDisplacementPx` | `change.maximum_seam_displacement_px` | 有限 ≥ 0 | 出力 px | `manual-adjustment` の時だけ | V3 | |
| `revision.change.restoredSha256` | `change.restored_sha256` | ハッシュ | — | `rollback` の時だけ | V3 | |
| `source.profileFile` | `source.profile_file` | v2 と同じ | — | 必須 | V3 | |
| `source.profileFileSha256` | `source.profile_file_sha256` | ハッシュ | — | 必須 | V3 | |
| `source.profileFingerprintSha256` | `source.profile_fingerprint_sha256` | ハッシュ | — | 必須 | V3 | |
| `source.policySha256` | `source.policy_sha256` | ハッシュ（`wpd-registration-policy.json` の） | — | 必須 | V3 | |
| `mountOrientation` | `orientation` | `normal`／`rotated-180` | — | 必須 | V3 | |
| `outputRaster` | `raster` | v2 と同じ（`regionUm{left,top,right,bottom}`・`dpi`・`widthPixels`・`heightPixels`） | µm・DPI・px | 必須 | V3 | |
| `seam` | `seam` | v2 と同じ（`column`・`origin`） | 出力 px | 必須 | V3 | |
| `basePose` | `base_pose` | `{rxDeg, ryDeg, rzDeg, txMm, tyMm, tzPercent}` 有限 | 度・mm・% | 必須 | V3 | `registration.envelope` の内。手直しの記録は書く時も外を許し、条件を使う読み手が断る |
| `registration.envelope` | `registration.envelope` | `{positionMm, rotationDeg, tiltDeg, heightPercent}` 正の有限 | mm・度・度・% | 必須 | V3 | |
| `registration.limits` | `registration.limits` | 9 値（下の注） | 出力 px・個 | 必須 | V3 | |
| `registration.geometry` | `registration.geometry` | `{omegaMarginPx, guardPx, bandHalfWidthPx}` 整数 | 出力 px | 必須 | V3 | |
| `registration.surfaceEnvelope` | `registration.surface_envelope` | `{heightMm, tiltDeg}` 正の有限、か null | mm・度 | 必須（null 可） | V3 | |
| `registration.applyBeyondEnvelope` | `registration.apply_beyond_envelope` | 真偽 | — | 必須 | V3 | |
| `registration.surfaceScaleWarnPercent` | `registration.surface_scale_warn_percent` | 有限 ≥ 0、か null | % | 必須（null 可） | V3 | 値は予約（運用の値は null） |
| `registration.parallax` | `registration.parallax` | `{maxLiftMm}` 正の有限、か null | mm | 必須（null 可） | V3 | |
| `registration.live` | `registration.live` | `{limits}`（9 値、C の px）か null | C の px・個 | 必須（null 可） | V3 が書く、LC が使う | null の間はライブの推定は走らない（設計 11.21）。値を入れるかは所有者に確認中（設計 11.29 N4） |
| `seamGate` | `seam_gate` | `{limitPx, guardBandPx}` 正の有限 | 出力 px | 必須 | V3 | |
| `revisionRules.manualAdjustmentMaxSeamPx` | `revision_rules.manual_adjustment_max_seam_px` | 正の有限か null | 出力 px | 必須（null 可） | V3 | 予約（運用の値は null＝上限なし） |

注:

- `registration.limits` の 9 値は `RegistrationLimits` の名前を camelCase にした `brownLoweAlpha`、`brownLoweBeta`、`minimumUniqueBlocks`（整数）、`maximumPredictionSigmaPx`、`maximumResidualRmsPx`、`maximumResidualP95Px`、`maximumBinBiasPx`、`convergencePx`、`startAgreementPx`。開発の仮の値。`registration.live.limits` も同じ 9 値
- 種類ごとの `revision.change` の欄の組は固定。`created` と `values` は `kind` だけ、`migrated-from-v2` は `kind`＋`v2Sha256`、`saved-estimate` と `recalibrated` は `kind`＋`reportSha256`＋`pairId`、`manual-adjustment` は `kind`＋`entered`＋`maximumSeamDisplacementPx`、`rollback` は `kind`＋`restoredSha256`。この順で書く
- `recalibrated` は撮影の 1 組から CAM-B の写像を取り直した改訂（#434）。番号 2 以上だけで、`basePose` は全部 0（B の姿勢は profile の中にある）。親から変えてよいのは `source` の profile の 3 欄（`profileFile`・`profileFileSha256`・`profileFingerprintSha256`）・`outputRaster`・`seam`・`basePose` だけで、`mountOrientation`・`source.policySha256`・`registration`・`seamGate`・`revisionRules` は親の値を写す（方針ファイルは読み直さない。値を変えるのは `values` の役で、1 つの改訂に 2 つの変更を混ぜない）。profile を変えてよい種類はこれだけ。`rollback` で profile の違う改訂へ戻すのは、今のまま作る道具が断る。決まり 8 の例外で版は上げていない。この符号を知らない読み手（#434 より前）は改訂の列全体を拒否し（`condition_invalid`）、C を描かず合成しない。前の改訂へは倒れない。原画像は残る。報告 v8 が条件について持つのは `condition{sha256, sequence}` だけで、種類は持たないので v8 は変わらない
- `recalibrated` が指す profile の `calibration` の意味は `rig-profile-v2.md` 7 節の注（`toolId` が `a0-pair-reanchor`）。状態は常に draft で、`profileId` は null、親の承認は引き継がない
- `revision.parentSha256` が null なのは `sequence` が 1 の時だけ
- `registration.geometry.guardPx` は、推定の細の段のパッチ（`kRegistrationDesignDraft.fine_patch_px`、案で 64 px）の半分より大きい。半分以下だと推定の点が Ω の画素を読み、ゲートが推定の材料を測る事になる。読む時と作る時に弾く（`condition_invalid`）
- `registration.parallax` が null でない時、Ω の半幅 `(混ぜ幅 + 1) / 2 + omegaMarginPx`（混ぜ幅は `kSeamBlendDesign.width_pixels`）は層 2 の a（`kParallaxDesignDraft.weight_flat_px`、案で 48 px）以下。超えると層 2 が毎回拒むので、読む時と作る時に弾く
- 範囲、線、閾値にコードの既定値は置かない。欄が null の層は当てない（設計 11.13 の 6）
- v2 の `scaleAutoCorrectionMaxPercent` は v3 に残さない（倍率 k を使わない。設計 11.20.2）
- `registration.surfaceScaleWarnPercent` の欄は残すが、運用の値は null で、縮尺の狂いの警告は出さない（所有者の決定 10）。値が入っても警告の符号は無い
- `revisionRules.manualAdjustmentMaxSeamPx` の欄は残すが、運用の値は null で、手直しの量に上限は無い（所有者の決定 9）。上限で拒否する処理と警告は作らない

## 4. 報告 v8（`a0.uncalibrated-still-review.v8`）

型は `still_review.hpp` の `SavedFixedCompositeRegistration`（記録）、`capture_registration.hpp`、`registration_gate.hpp`、`parallax_correction.hpp`、`registration_mapping.hpp`。v7 と同じ書き方（`WriteFixedCompositeLensJson` の形）に合わせる。

### 4.1 最上位

| JSON の名前 | C++ の名前 | 型 | 必須 | 埋める担当 | 予約 |
|---|---|---|---|---|---|
| `schema` | （定数） | 文字列 `a0.uncalibrated-still-review.v8` | 必須 | C0 | |
| `pairId`・`state`・`productQualityAccepted`（常に false）・`physicalScaleVerified`・`liveToStillFieldOfViewVerified`・`automaticRetryCount`・`inputs`・`previewGeometry`・`mountOrientation`（`rotated-180` の時だけ）・`sourceDimensions`・`output` | v7 と同じ | v7 と同じ | v7 と同じ | J8 | |
| `geometryBasis` | | 文字列 `fixed-composite-registration` | 必須 | J8 | |
| `fixedCompositeRegistration` | `SavedFixedCompositeRegistration` | オブジェクト（4.2） | 必須 | J8 | |
| `blend` | `StillReviewResult::blend` | v7 と同じ（`WriteFixedSeamBlendJson`） | 必須 | J8 | |
| `warnings` | `warnings` | 4.4 の要素の列（空可） | 必須 | J8（導出は `DeriveRegistrationWarnings`） | |
| `assessment` | `assessment` | 4.5 | 必須 | J8（導出は `DeriveRegistrationAssessment`） | |

- `translation` は書かない（v7 と同じ）
- 最上位の並び: `schema`, `pairId`, `state`, `productQualityAccepted`, `physicalScaleVerified`, `liveToStillFieldOfViewVerified`, `automaticRetryCount`, `inputs`, `previewGeometry`, `mountOrientation`（あれば）, `sourceDimensions`, `output`, `geometryBasis`, `fixedCompositeRegistration`, `blend`, `warnings`, `assessment`
- 設計 11.27.2 の表は最上位の欄の 1 つを `seamBlend` と書いているが、v7 の JSON の名前は `blend`。「v7 と同じ」を優先して `blend` とする（※要確認）
- 依頼文は `surface`・`parallax`・`folds`・`geometryLevel`・`gate` を最上位と書いたが、追補の 11.27.2 に合わせて `fixedCompositeRegistration` の中に置く

### 4.2 `fixedCompositeRegistration`

| JSON の名前 | C++ の名前 | 型 | 必須 | 埋める担当 | 予約 |
|---|---|---|---|---|---|
| `condition` | `condition_sha256`・`condition_sequence` | `{sha256, sequence}` | 必須 | J8 | |
| `profile` | `profile_file` ほか | v7 と同じ（`file`・`fileSha256`・`fingerprintSha256`・`profileId`） | 必須 | J8 | |
| `raster` | `raster` | v7 と同じ | 必須 | J8 | |
| `seam` | `seam_column`・`seam_origin` | v7 と同じ | 必須 | J8 | |
| `layout` | `layout`（`RegistrationGeometry`） | `{mixingWidthPx, omegaMarginPx, guardPx, bandHalfWidthPx}` 整数 | 必須 | J8 | |
| `basePose` | `base_pose` | 条件の写し（3 節と同じ 6 値） | 必須 | J8 | |
| `limits` | `limits`（`RegistrationLines`） | 判定に使った線の写し（4.3） | 必須 | J8 | |
| `registration` | `registration.registration`（`RegistrationResult`） | オブジェクト（4.6） | 必須 | EST（構造体）、J8（書く） | `design`・`display.outputFrame` の中身 |
| `surface` | `registration.surface`（`SurfaceEstimate`） | オブジェクト（4.7） | 必須 | EST、J8 | |
| `parallax` | `registration.parallax`（`ParallaxResult`） | オブジェクト（4.8） | 必須 | PX1（構造体）、J8 | `design`・`profile` の要素の並び |
| `folds` | `registration.parallax.folds` | 線の列（空可） | 必須 | PX1 | 要素の欄 |
| `pointSource` | `registration.point_source` | null か オブジェクト（4.8.1） | 必須 | J8 | |
| `geometryLevel` | `registration.level` | オブジェクト（4.9） | 必須 | J8 | |
| `gate` | `registration.gate` | オブジェクト（4.10） | 必須 | GΩ（構造体）、J8 | `lift`・`wideSearch`・`design` の中身 |
| `timeBudget` | `registration.time_budget` | オブジェクト（4.11） | 必須 | J8 | |
| `geometryHash` | `registration.geometry_hash` | ハッシュ | 必須 | J8（関数の宣言は C0） | |

- この並びで書く。`geometryHash` は `gate.geometryHash` と一致する
- 型は `capture_registration.hpp` の `SavedFixedCompositeRegistration`。書き手は `FixedCompositeRegistrationJson`・`RegistrationWarningsJson`・`RegistrationAssessmentJson`、読み手は `ReadFixedCompositeRegistrationReport`（4.12）
- `layout`: その撮影の W・Ω・帯（条件の `registration.geometry` の 3 値と、合成の混ぜ幅）。Ω の半幅は `(mixingWidthPx + 1) / 2 + omegaMarginPx` で、幾何の hash に入る。StillReview の読み手は `mixingWidthPx` が `blend.widthPixels` と同じことを確かめる

### 4.3 `limits`

`{envelope{positionMm, rotationDeg, tiltDeg, heightPercent}, surfaceEnvelope{heightMm, tiltDeg}か null, applyBeyondEnvelope, surfaceScaleWarnPercent（数か null）, maxLiftMm（数か null）, seamGate{limitPx, guardBandPx}}`。条件の同じ名前の欄の写し。読み手は警告をこの値から作り直す。

### 4.4 警告の要素（`warnings`）

設計 11.17.2 の表。並びは `code`, `class`, `component`, `value`, `limit`, `unit`, `rowsPx`, `rowsMm`, `interpretations`, `remedy`, `remedyAmountMm`, `applied`, `source`。

| JSON の名前 | C++ の名前 | 型 |
|---|---|---|
| `code` | `code` | 5.1 の符号 |
| `class` | `warning_class` | `exceeded`／`unverified`／`notice` |
| `component` | `component` | 5.2 の符号 |
| `value` | `value` | 数（丸めない） |
| `limit` | `limit` | 数か null（`notice` は null がありうる） |
| `unit` | `unit` | `px`／`mm`／`deg`／`percent`／`rows`／`pixels` |
| `rowsPx` | `rows_px` | `{first, last}` か null（出力の行） |
| `rowsMm` | `rows_mm` | `{first, last}` か null（継ぎ目の上からの mm） |
| `interpretations` | `interpretations` | 文字列の列（読み方の符号。例 `surface-raised`・`camera-moved-along-baseline`） |
| `remedy` | `remedy` | 5.3 の符号 |
| `remedyAmountMm` | `remedy_amount_mm` | 数か null（`shift-drawing-horizontal` の時だけ数） |
| `applied` | `applied` | 真偽 |
| `source` | `source` | `condition`／`design` |

導き方（`DeriveRegistrationWarnings`。並びは 5.1 の符号の順、同じ符号の中は記録の順）:

- `camera-*`: 推定が確か（`verified`・`out-of-envelope`）な時の `components`（p̂ − p_b）が線を超えた成分。G3 では粗の段の平行移動 |(tx, ty)| を `camera-position` で見る。`applied` は段が G1・G2（G3 は true）
- `surface-height`・`surface-tilt`: 推定した紙面の |h0| と傾きが `surfaceEnvelope` を超えた時（exceeded）。G3 は粗の段の h0。`value` は h0（符号つき）
- `seam-lift`（notice）: G1 で `parallax.summary.maxShiftPx` が L を超えた時。`seam-lift-limit`（exceeded）: `parallax.lift.exceeded`（測った行の |−q / k| が `maxLiftMm` を超えた）。`value` は `lift.maximumMm`、`rowsPx` は超えた最初と最後の行、`applied` は段が G1 の時
- `fold-near-seam`: 折り目の線の `nearSeam`（`distanceFromSeamPx` が D_fold 以内）。`value` は符号つきの距離（線が継ぎ目の列の右なら正、横切るなら 0）、`limit` は D_fold、`remedyAmountMm` は |`shiftToClearPx`| × 画素の大きさ（無ければ null）
- `seam-displacement`: `gate.after.maximumPx` ＋ U が L を超えた時。`value` はその和、`limit` は L、`remedy` は `gate.cause` の直し方
- `seam-dx-unmeasured`（設計 11.30）: `gateFindings` に `dx-unmeasured` があれば unverified（`remedy` は `review-visually`）、無くても `gate.rows.unmeasured` があれば notice（`none`）。`value` は測れた行のうち基線の向きを測れなかった行の数（`rowsMeasured` − `rowsDxMeasured`、`rows`）、`rowsPx`・`rowsMm` は `unmeasured` の最初と最後。`seam-unmeasurable`: `gateFindings` に `unconfirmed-content`・`one-sided-content`・`nothing-measured`・`content-unmeasured` のどれかがあれば unverified（`interpretations` はその所見の列、`review-visually`）、無くても中身があるのに測れない行があれば notice（`interpretations` は空、`none`）。`value` は `rowsContentUnmeasured` ＋ `rowsUnconfirmed`、`rowsPx`・`rowsMm` は `contentUnmeasured` の最初と最後。ゲート本体の `unmeasurable-content`・`no-measurable-block`・`unpredicted-empty-block` からは出さない。`placement-unverified`: `gateFindings` の `placement-unverified` だけ（ゲート本体の `global-placement-unverified` からは出さない）。`placement-mismatch`: `findings` の `global-placement-mismatch`
- `gate-configuration`: `transition-outside-band`、または `pass` でないのに上のどれも出ない時（`interpretations` にゲート本体の所見の理由を並べる）。`gate-not-run`: `gate.state` が `not-run`（ゲートの段が例外で落ちた時は `interpretations` が `["gate-failed"]`）
- `seam-wide-search`: 広い探索の `offsetPx` がある時。`geometry-level`: 段が G1 でない時（推定か層 2 の段が例外で落ちた時は `interpretations` に `estimate-failed`・`layer-two-failed`）。`time-budget`: `gate` 以外の段が時間の予算で飛ばされた時（`reason` が `time-budget` の物だけ。`interpretations` に段の符号）
- 量の無い符号（`placement-*`、`gate-*`、`geometry-level`、`time-budget`）は `value` 0・`limit` null・`unit` `rows`
- まだ出さない符号（届く部品の担当と一緒に足す）: `surface-height` の notice（層 1 が継ぎ目で動かした量。紙面の推定の担当）、`output-edge-uncovered`（外周を塗る入口。11.23.3）、`gutter-in-band`（のどの線の帯の決め方。層 2 の担当）

### 4.5 `assessment`

`{state, exceeded[], unverified[], qualityAccepted, acceptanceEvidence}`。`state` は `within-criteria`／`outside-criteria`／`not-judged`。`exceeded`・`unverified` は警告の `code` の列。`qualityAccepted` と `acceptanceEvidence` は常に `false`。決め方は設計 11.17.1（`DeriveRegistrationAssessment`）。

### 4.6 `registration`

`{state, reasons[], hasEstimate, pose, poseSigma, covariance[36], components[], display, applied, design, diagnostics}`

- `state`: `verified`／`undetermined`／`out-of-envelope`（`RegistrationStateCode` の語）。`reasons`: 文字列の列
- `pose`・`poseSigma`: `{rxDeg, ryDeg, rzDeg, txMm, tyMm, tzPercent}`。推定が無い時（`hasEstimate` が false）は両方 null
- `covariance`: 36 個の数（行優先、順は rx ry rz tx ty tz）。推定が無い時は null
- `components`: `[{component, valueFromBase, limit, exceeded}]`。`component` は `position`／`rotation`／`tilt`／`height`、`valueFromBase` は p̂ − p_b から測った大きさ（`RegistrationComponentsFromBase`: |(Δtx, Δty)|、|Δrz|、|(Δrx, Δry)|、|Δtz|）。`exceeded` は値が線を超えた時だけ（等しいのは内）。推定が無い時は空。派生値（読み手が作り直す）
- `applied`: 段が G1 か G2 の時 true（派生値）
- `display`: `{positionMm, txMm, tyMm, rotationDeg, tiltDeg, rxDeg, ryDeg, heightPercent, heightMm, outputFrame}`（`PoseDisplay` の名前。p̂ − p_b）。`outputFrame` は null か `{cameraMotionMm{x, y}, lensDirection{x, y}, lensTiltDeg, rotationClockwiseDeg, surfaceHighSide{x, y}, baselineEquivalentMotionMm{x, y}}`（`OutputFrameDisplay`。図面の向き: x が右、y が下）
  - `cameraMotionMm`: B の中心が動いた向きと量（mm）。平行移動だけで B の視野の中心の図面が動いた量の逆
  - `lensDirection`・`lensTiltDeg`: B のレンズが傾いた向き（単位ベクトル、傾きが 0 なら 0）と |(rx, ry)|。傾きは pivot のまわりなので視野の中心は動かず、(rx, ry) だけで B の中心が動く向きの逆で決める
  - `rotationClockwiseDeg`: 画面で見た B の回り（x から y へ回る向きが正）
  - `surfaceHighSide`: 紙面の高い側（(g_x, g_y) の単位ベクトル、平らなら 0）
  - `baselineEquivalentMotionMm`: h0 と同じずれを平面の原点で作る B の動き（`surface-height` の 2 つ目の読み方）
- `applied`: 姿勢を合成に当てたか
- `design`: 推定器の設計定数。予約。決まるまで `{}`
- `diagnostics`: `{counts{outerIterations, points, inliers, gatePoints, uniqueBlocks, consistentBlocks}}`（`RegistrationDiagnostics` の整数。※仮）

### 4.7 `surface`

`{state, heightMm, slopeX, slopeY, tiltDeg, covariance[9], baselineEquivalentMm, scale{factor, minimum, maximum}, exceeded{height, tilt}, applied}`

- `state`: `estimated`／`not-estimated`／`disabled`（`disabled` は条件の `surfaceEnvelope` が null の時）
- `state` が `estimated` でない時、`heightMm`・`slopeX`・`slopeY`・`tiltDeg`・`baselineEquivalentMm`・`covariance`・`scale` は null、`exceeded` は両方 false、`applied` は false
- `scale` は推定した紙面の高さによる縮尺の目安（`factor` は継ぎ目の列の中央、`minimum`・`maximum` は出力全体）。縮尺の狂いの警告は出さないので、`exceeded` に `scale` は無い（追補の `exceeded{height, tilt, scale}` から外した。所有者の決定 10）

### 4.8 `parallax`

`{state, reasons[], baseline, kPxPerMm, design, grid, split, profile[], summary, lift, applied}`（型は `parallax_correction::ParallaxResult`）

- `state`: `applied`／`not-applied`／`disabled`。`reasons`: 文字列の列（5.4）
- `baseline`: `{x, y}`（e_b）、`kPxPerMm`: 数
- `design`: 層 2 の設計定数のうち、写像と警告が読む物 `{columnSpacingPx, columnRadiusPx, pointPatchPx, weightFlatPx, weightFalloffPx, maximumStrain, foldNearSeamPx}`（ほかの定数はコードの `kParallaxDesignDraft`）
- `grid`: `{seamColumn, firstRow, rowStep}`（ゲートの Ω の格子の行。`profile` の行 i は `firstRow + i × rowStep`）
- `split`: 検査用の行と直す用の行の分け方（設計 11.4・11.5）。`{mode, verifyPeriodRows, verifyPhase, rowsVerify, rowsCorrect, rowsWithHole, pointsExcluded}`。`mode` は `split`（`verifyPeriodRows` が 1 以上）か `hole`（0。分けない、全行が検査用で行ごとの穴）。`verifyPhase` は `verifyPeriodRows` 未満（`hole` の時は 0）。`rowsVerify`・`rowsCorrect`・`rowsWithHole` は派生値（`profile` の `role`・`hole` の数）、`pointsExcluded` は検査用の行の画素を読んだので層 2 のどの推定にも入れなかった点の数（`hole` の時は 0）
- `profile`: ゲートの行ごとに `[{y, qPx, sigmaPx, nEff, applied, points, liftMm, state, reason, role, hole}]`。`state` は `measured`／`filled`、`reason` は `filled` の理由か null。`liftMm` は −q / k。`role` は `verify`（検査用。ゲートが判定する）／`correct`（直す用。参考に測るだけ）で、読み手が行の番号 i から導き直す（i mod `verifyPeriodRows` = `verifyPhase` なら `verify`。`hole` の時は全行 `verify`）。`hole` はその行の推定が行ごとの穴を持ったか（`hole` の時は全行 true、`split` では継ぎ目の列の点が行を上下から挟まない縁の行だけ true）
- `summary`: `{maxShiftPx, maxShiftMm, atRow, maxStrain, columns{first, last}, points, pointsInput, pointsBaseline, rowsMeasured, rowsFilled, holeXPx, holeYPx, pointsCrossValidated, foldCandidates, foldResidualScalePx}`
- `lift`: `{limitMm（数か null）, maximumMm, atRow, exceeded, exceededRows, firstExceededRow, lastExceededRow}`（条件の `maxLiftMm` に対する、測った行の浮き）
- `baseline`・`kPxPerMm`・`grid`・`split`・`summary`・`lift` は `profile` に行がある時だけ値、無い時は null。`design` は行が無い時 `{}`
- 推定しない時（`UnestimatedParallax`）: 条件の `parallax` が null なら `state` が `disabled`・`reasons` が `["disabled"]`、推定が確かでなければ `not-applied`・`["pose-untrusted"]`、時間の予算で飛ばせば `not-applied`・`["time-budget"]`
- `applied` は段が G1 の時だけ true（派生値）
- G1 の `geometryLevel.mapping.parallax` は、この `profile` の `qPx`・`baseline`・`grid`・`design` の a と L から `ToParallaxProfile` で作った物と一致する（読み手が確かめる）
- `folds` の要素: `{kind, positionPx, angleDeg, stepSign, stepPx, stepMm, distanceFromSeamPx, crossesSeam, first{x, y}, last{x, y}, lengthPx, points, nearSeam, shiftToClearPx}`。`kind` は `fold`／`gutter`（今は全部 `fold`）、`stepSign` は −1・0・1、`shiftToClearPx` は数か null

### 4.8.1 `pointSource`

層 2 の点の出所（設計 11.31）。設計の定数が帯の測定の点（既定）の撮影では null。速い測定の点を選んだ撮影で層 2 を試した時だけオブジェクト。層 2 を試さなかった撮影（無効・推定が確かでない・時間の予算・段の失敗）は、選んでいても null。

`{fastMeasurerUsed, pointsUsed, edgeShare, edgeShareLine, fallbackReason}`

- `fastMeasurerUsed`: 層 2 に速い測定の点を渡したか。`pointsUsed`: 渡した点の数（渡さなかった時は 0）
- `edgeShare`: 内容のある点のうち、探索の端に当たった点の割合 [0, 1]。測定が例外を投げた時だけ null。`edgeShareLine`: その撮影が照らした線 [0, 1]（設計の `edge_share_line`）
- `fallbackReason`: 使わなかった理由。使った時は null。符号は 5.4
- 読み手が確かめる物: `fastMeasurerUsed` と `fallbackReason` の null は同値。使った時は `pointsUsed` が 1 以上で、`parallax.summary.pointsInput` と同じ（`parallax.profile` に行がある）、`edgeShare` が数で線以下。使わなかった時は `pointsUsed` が 0。`edge-share` は `edgeShare` が線を超えた時、`no-points` は線以下の時、`measurer-failed` は `edgeShare` が null の時だけ

### 4.9 `geometryLevel`

`{level, reasons[], applied{pose, surface, parallax}, beyondEnvelopeApplied, mapping, coarseShift}`。`level` は `G1`〜`G4`。

- `mapping`: その段で B の写像に当てた値。`{pose{rxDeg, ryDeg, rzDeg, txMm, tyMm, tzPercent}, surface{h0Mm, slopeX, slopeY}, parallax}`。`parallax` は null か `{baseline{x, y}, rowSpacingPx, firstRowPx, qPx[], aPx, lPx, seamColumn}`（`ParallaxProfile`）。合成とゲートはこの値で `RegisteredDocumentMappedFixedSeamInput` を作る
  - G1: `pose` が `registration.pose`、`surface` が推定した紙面（推定が無ければ 0）、`parallax` が層 2 の行から作った標本（当てない行は 0）
  - G2: G1 と同じで `parallax` が null
  - G3: `pose` が `basePose` に `coarseShift` の `translationXMm`・`translationYMm` を足した物、`surface` が `{coarseShift.surfaceHeightMm, 0, 0}`（条件の `surfaceEnvelope` が null なら h0 も 0）、`parallax` が null
  - G4: `pose` が `basePose`、`surface` が 0、`parallax` が null
- `coarseShift`: G3 の候補（粗の段の一様なずれ）。null か `{surfaceHeightMm, translationXMm, translationYMm}`。G3 以外の段でも、候補があれば書く
- `reasons`: `seam-outside-overlap-at-level`／`output-uncovered-at-level`（上の段の写像が安い検査に落ちた時）
- `applied`（派生値）: `pose` は G1〜G3、`surface` は `mapping.surface` が 0 でない時、`parallax` は `mapping.parallax` がある時
- `beyondEnvelopeApplied`（派生値）: 当てた段の量に範囲を超えた物があるか（G1・G2 は姿勢の成分と紙面、G1 は層 2 の `maxLiftMm` も、G3 は粗の段の 2 自由度）

### 4.10 `gate`

`{state, findings[], gateFindings[], cause, remedy, lift, rows, maxima, before, after, corrected, anchors, placement, wideSearch, design, geometryHash, seamGate}`

- `state`: `pass`／`fail`／`undetermined`／`not-run`。埋め込みの `a0.seam-gate.v1` の状態ではない（設計 11.17.1）。`pass` の時、埋め込みの状態は `pass` か、`fail` で理由がすべて GΩ が決め直す物（`unmeasurable-content`・`no-measurable-block`・`unpredicted-empty-block`・`global-placement-unverified`）で、埋め込みの所見の理由が `findings` と同じ順で同じ。`fail` の時、埋め込みは `fail` か、`cause` が `unmeasurable` で `pass`（設計 11.30）
- `findings`: `[{reason, remedy}]`（`seam_gate::SeamGateFinding`。ゲート本体の所見をそのまま）
- `gateFindings`: `[{reason, remedy}]`（`registration_gate::GateFinding`。GΩ の所見を次の順に: `unconfirmed-content`、`one-sided-content`、`nothing-measured`、`content-unmeasured`、`dx-unmeasured`、`placement-unverified`、`transition-outside-band`、`gate-undetermined`。設計 11.30）
- `cause`: `none`／`lift`／`pose`／`displacement`／`unmeasurable`。`remedy`: `GateCauseRemedy(cause)` の符号
- `lift`: null か `{pointsExceeding, baselineShare, signAgreementShare, heightEstimateMm（数か null）}`
- `rows`: `{dxMeasuredShare, rowsWithContent, rowsDxMeasured, unmeasured[{first, last}], outsideMeasurable[{first, last}], rowsBlank, rowsMeasured, rowsContentUnmeasured, rowsOneSided, rowsUnconfirmed, contentUnmeasuredShare, blank[{first, last}], contentUnmeasured[{first, last}], rowsCorrected, corrected[{first, last}]}`（`registration_gate::GateRows`。設計 11.30 の行の分類）。層 2 が行を分けた時（`parallax.split.mode` が `split` で段が G1）、`rowsCorrected` と `corrected` 以外の数と範囲は検査用の行だけで数える。`rowsCorrected` は直す用の行のうち参考の測定が持つ格子の行の数、`corrected` はその出力の行。分けない時は 0 と `[]`。`rowsWithContent` = `rowsUnconfirmed` ＋ `rowsMeasured` ＋ `rowsContentUnmeasured`、`rowsDxMeasured` ≤ `rowsMeasured`、`rowsOneSided` ≤ `rowsContentUnmeasured`、`dxMeasuredShare` = `rowsDxMeasured` / `rowsWithContent`、`contentUnmeasuredShare` = `rowsContentUnmeasured` / `rowsWithContent`（中身のある行が無ければ 0）。読み手はこれを確かめる。`unmeasured` は測れた行のうち基線の向きを測れなかった行、`contentUnmeasured` は中身があるのに測れない行と未確認の行、`blank` は白い行。`not-run` の時は 2 つの割合が null
- `placement`: `{verifiedBy}`。`anchors`／`estimate`／`none`（`registration_gate::PlacementBasis`。設計 11.30）。`not-run` の時は `none`
- `maxima`: `{twoDirectionalPx, oneDirectionalPx, oneDirectionalNormal, oneDirectionalComponent}`。数か null。`oneDirectionalNormal` は `{x, y}` か null、`oneDirectionalComponent` は `across-baseline`／`with-baseline` か null
- `before`・`after`: `{maximumPx, twoDirectionalPx, oneDirectionalPx}`。数か null。層 2 を当てない時は同じ値。行を分けた時は検査用の行の値（判定はこの値）
- `corrected`: 直す用の行を同じ対で参考に測った値。null か `{maximumPx, twoDirectionalPx, oneDirectionalPx, beforeMaximumPx, rowsWithContent, rowsMeasured, independent}`。数の 4 つは数か null、`independent` は常に false（層 2 がこの行の画素を読んだ点で直したので、独立に確かめた値ではない）。判定・所見・警告には入らない。ゲートが走り、段が G1 で `parallax.split.mode` が `split` の時だけ値（読み手が確かめる）。`rowsMeasured` ≤ `rowsWithContent` ≤ `rows.rowsCorrected`
- `anchors`: `{range}`。`range` は `{firstColumn, lastColumn}` か null
- `wideSearch`: null か `{radiusPx, blocks, uniqueBlocks, agreeingBlocks, offsetPx, offsetMm, rows}`。`offsetPx`・`offsetMm` は数か null、`rows` は `{first, last}` か null
- `design`: `not-run` の時は `{}`。それ以外は GΩ の設計定数 `{minimumBaselineComponent, liftRatio, liftShare, wideSearchRadiusPx, wideSearchHalfWidthPx, wideSearchBlockRows, wideSearchMinimumCorrelation, wideSearchSecondaryRatio, wideSearchConsistencyPx, wideSearchMinimumBlocks, wideSearchTriggerShare, maximumUnmeasuredContentShare}`（最後は [0, 1)。設計 11.30）
- `geometryHash`: `fixedCompositeRegistration.geometryHash` と同じ値
- `seamGate`: `a0.seam-gate.v1` のオブジェクト（`SeamGateReportJson`）か null（`not-run` の時は null）。メンバーは名前の順（JSON の読み手の正規の形）に並べ直して書く。読み手は `schema` と `state` だけを確かめ、中身は正規の形で持つ
- `not-run` の時は、`findings`・`gateFindings` が空、`maxima`・`before`・`after` の数と `anchors.range`・`lift`・`wideSearch`・`corrected` が null

### 4.11 `timeBudget`

`{plannedStages[], skipped[{stage, reason}], costModel{stageMs, tailReserveMs}}`。段の符号は 5.4。`reason` は `time-budget`（時間の予算で落とした）か `stage-failed`（段が例外を投げたので落として続けた。推定は G4、層 2 は G2 まで、ゲートは `not-run` で、ゲートと一緒に測る広い探索と、層 2 の前に測っていない帯も落とす。`std::bad_alloc` は落とさず呼び手へ）。`stageMs` は段の符号をメンバー名にした数（順は 5.4）。時刻の値（残りの時間、段ごとにかかった時間）は書かない（設計 11.25.4。v8 のバイトが段の選びだけで決まるように）。かかった時間は StillReview の結果（`StillReviewResult`）で呼び手に返し、ログに出す。

- 7 つの段のそれぞれが `plannedStages` か `skipped` のどちらかに 1 回だけ、5.4 の順に入る
- `gate` が `skipped` にあることと、`gate.state` が `not-run` であることは同じ

### 4.12 読み手の照合（`ReadFixedCompositeRegistrationReport`）

報告の文字列全体を受け取り、`fixedCompositeRegistration`・`warnings`・`assessment` の 3 つを読む（ほかの欄は StillReview の読み手が読む）。

1. 欄の名前・数・型・符号を厳格に読む（重複・未知・欠けた欄を拒否）
2. 段の写像が推定の値と合うこと（4.9 の G1〜G4）
3. 派生値を書き手と同じ関数で作り直し（`DeriveSavedFixedCompositeRegistration`: `components`、`surface.exceeded`・`surface.applied`、`geometryLevel.applied`・`beyondEnvelopeApplied`、`warnings`、`assessment`、2 つの `geometryHash`）、読んだ値と 1 つでも違えば拒否
4. 読んだ値を書き手で書き直した文字列が、読んだ文字列と（メンバーの並びを除いて）同じであること。数の字面（`16` と `16.0`、`-0` と `0`）が違っても拒否

書き手（`FixedCompositeRegistrationJson`）も 3 の派生値が合わない記録を書かない（`std::logic_error`）。

### 4.13 報告 v9（`a0.uncalibrated-still-review.v9`、#437 段 2）

開発用プレビューの窓が持つ「確定した位置」（ライブの合成と JPEG の合成が共有する CAM-B の位置。ADR-0046）を、撮影ごとに記録する版。条件 v3 の撮影の報告は、以後すべて v9 で書く（v8 は書かない）。v8 の読み手は残し、v8 の報告を今までどおり読む。決まり 8 により、欄を足す変更なので版を上げた（既にある欄だけを使う符号の追加ではない: 出どころ・通し番号・手で入れた量に新しい欄が要る）。

v8 との差はこれだけで、ほかの欄は名前・型・単位・並びとも v8 のまま。

| JSON の名前 | C++ の名前 | 型 | 必須 | 埋める担当 |
|---|---|---|---|---|
| `schema` | （定数） | 文字列 `a0.uncalibrated-still-review.v9` | 必須 | J8 |
| `fixedCompositeRegistration.confirmedPosition`（`pointSource` の後、`geometryLevel` の前） | `registration.confirmed_position` | null か 4.13.1 のオブジェクト | 必須（null 可） | J8（窓が渡した値をそのまま） |
| `fixedCompositeRegistration.confirmedPositionCheck`（上の直後） | `registration.confirmed_check` | null か `{used, seamDifferencePx}` | 必須（null 可） | J8 |
| `geometryLevel.level` | `registration.level.level` | `G1`〜`G4` か `confirmed` | 必須 | J8 |

- `confirmedPosition` が null なのは、窓が位置を渡さなかった撮影（校正の取り直しの見本の合成、窓を持たない呼び手）。この時 `confirmedPositionCheck` も null で、撮影は位置が無かった時と同じ。2 つは両方 null か両方オブジェクト
- 位置を渡さない撮影の v9 のテキストは、v8 のテキストに、スキーマの文字列と、`,"confirmedPosition":null,"confirmedPositionCheck":null`（`,"geometryLevel":` の直前）を足した物と、1 字も違わない（試験で固定）
- `placement` を名前に使わない。v8 の `gate.placement`（大域的な配置の確かめ方）と警告 `placement-unverified`・`placement-mismatch` が別の意味で使っている

#### 4.13.1 `confirmedPosition`

`{referenceCamera, targetCamera, serial, source, baseSource, conditionSha256, pose, surface, manualEntered}`

- `referenceCamera`: 文字列 `CAM-A`、`targetCamera`: 文字列 `CAM-B`（ADR-0045。値は 2 台のまま。読み手は他の値を拒否）
- `serial`: 整数（0 以上）。窓が撮影の要求の時に持っていた、位置の変化の通し番号
- `source`: `fixed-condition`／`auto-start`／`auto-button`／`after-capture`／`manual`。ライブのログの `source=` と同じ語
- `baseSource`: `source` が `manual` の時だけ符号（手で直す前の出どころ。`manual` 以外の 4 つのどれか）、ほかは null
- `conditionSha256`: 位置が属する条件のファイルの SHA-256（64 桁の小文字 16 進）
- `pose`: 条件の `basePose` と同じ 6 値（CAM-A に対する CAM-B の姿勢）。`surface`: `{h0Mm, slopeX, slopeY}`（紙面。層 2 は無い）
- `manualEntered`: `source` が `manual` の時だけ `{seamDxPx, seamDyPx, rotationDeg}`（手で入れた量の合計。継ぎ目の列に沿って出力 px、継ぎ目を横切る向きに出力 px、光軸まわりの度）、ほかは null。値は最小の刻み（左右・上下は 4 px、回りは 0.05°）の整数の数から 1 回で作った値で、読み手は値から整数の数を戻し、同じ値になる事を確かめる（その数が 3 つとも 0 の記録は書かない: 合計が 0 に戻った位置は手で直した位置ではない）
- 数は 2 節の書き方（`FormatRecordNumber`）。ライブのログと撮影の側のログ（`LivePlacementFields`）の 9 数も同じ関数で書くので、報告の文字列とログの文字列が一致する

#### 4.13.2 `confirmedPositionCheck`

`{used, seamDifferencePx}`

- `used`: 位置で合成を作ったか。`geometryLevel.level` が `confirmed` である事と同値（読み手が確かめる）
- `seamDifferencePx`: 数か null。原寸の推定が当たる時（G1 か G2 の候補が安い検査に通った時。位置を手で直した時はその候補を使わずに確かめるだけ）、その写像（層 2 を除く）と位置の、継ぎ目の列に沿った差の最大（出力 px）。当たらない時、位置が別の条件の物の時は null。測り方は `SeamDifferencePx`（出力の上で、継ぎ目の列の行 0・32・64… と最後の行）の 1 つで、画面の撮影の後の文の数とライブのログの `seam_difference_px=` も同じ関数から出す。報告の読み手は値を作り直さず（作り直すには profile の写像が要る）、記録の値として読み、範囲（有限で 0 以上、位置と条件が同じで推定が確かな時だけ非 null）だけ確かめる

#### 4.13.3 段 `confirmed` と候補の順

位置は、条件の SHA-256 が撮影の時に読んだ条件と同じで、条件の位置（`basePose`・紙面 0、ビットで同じ。-0 と +0 は同じ）でない時だけ候補になる（そうでなければ候補は v8 のまま）。

| 位置の出どころ | 候補の順（最初に安い検査を通った物） |
|---|---|
| 条件の位置、別の条件の位置、位置が無い | G1・G2・G3・G4（v8 のまま） |
| `auto-start`・`auto-button`・`after-capture` | G1・G2・`confirmed`・G4（G3 は作らない） |
| `manual` | `confirmed`・G4（G1・G2 は確かめるだけで使わない） |

- `confirmed` の写像は、`confirmedPosition` の `pose`・`surface` とビットで同じで、層 2 は無い（`geometryLevel.mapping.parallax` は null。`applied.pose` は true）
- 位置が推定の始め・事前分布・範囲の基準に入る事は無い。`registration` は位置を渡さない時とビットで同じ
- `confirmed` の時、層 2 の帯は捨て、ゲートは自分の帯で測る
- 写像が安い検査に落ちた時は `reasons`（`seam-outside-overlap-at-level`・`output-uncovered-at-level`）を残して次へ下げる。手で直した位置が落ちた時は G4（位置は手のまま、窓の側の扱い）
- 撮影の入口で断る位置: 条件の SHA-256 が撮影の条件と同じで、条件の位置でない位置のうち、(1) 出どころが `manual` で姿勢の 4 成分（位置・回り・傾き・高さ。条件の `basePose` からの差）のどれかが `registration.envelope` の外（ちょうど端は内。紙面の範囲は見ない）、(2) 姿勢か紙面のどれかが範囲の外で、条件の `applyBeyondEnvelope` が false、(3) 出どころが `fixed-condition` なのに条件の位置でない。推定も描画も安い検査も始める前に `std::invalid_argument` で断り、JPEG は作らない（書き手が後で落ちて、報告の無い JPEG が残る事を無くす）。範囲の外の自動の位置は、条件が `applyBeyondEnvelope` を認める時だけ使い、`beyondEnvelopeApplied` と警告で言う
- ゲートの広い探索の診断（`wide_search`）は、`confirmed` でも G3・G4 と同じに走らせる（推定が裏に付かない段のため）

読み手が確かめる事（`geometryLevel` の写像に加えて）:

- `used` と level `confirmed` が同値。level `confirmed` は、位置があり、その条件が撮影の条件と同じで、条件の位置でなく、写像が位置とビットで同じ
- `source` が `fixed-condition` の位置は条件の位置（位置と条件が同じ時。上の範囲の決まりの (3)）
- 位置と条件が同じで条件の位置でない時、撮影の入口と同じ範囲の決まり（上の「撮影の入口で断る位置」の 3 つ）に合わない位置の記録は、どの level でも拒否する（書き手も読み手も）
- `seamDifferencePx` が非 null なら、位置と条件が同じで、推定が確か（`verified`・`out-of-envelope`）
- 位置と条件が同じで条件の位置でない時、手の位置の level は `confirmed` か G4、ほかの位置の level は G3 でない

#### 4.13.4 警告と符号（v9 だけ）

警告の符号 `position-by-hand`・`position-from-live`・`estimate-beyond-envelope`、成分の符号 `confirmed-position`。v8 の読み手は拒否する。

- `position-by-hand`（notice、`remedy` `none`、`applied` true）: level が `confirmed` で位置の出どころが `manual`
- `position-from-live`（notice、`none`、`applied` true）: level が `confirmed` で出どころが `manual` 以外
- `estimate-beyond-envelope`（**unverified**、`remedy` `check-camera-mount`、`applied` false、`interpretations` に超えた成分の符号 `camera-position`・`camera-rotation`・`camera-tilt`・`camera-height`、推定の紙面が範囲の外なら `surface-height`・`surface-tilt`）: level が `confirmed` で、原寸の推定が確か（`Trusted`）で、その姿勢の成分か紙面（推定した時の高さ・傾き）が線を超えた時。適用しなかった推定が範囲の外だった事（設置が動いたかもしれない事）を、位置の成分の警告から消さないため。unverified なので `assessment` は `not-judged` で、ゲートが通っても `within-criteria` にならない（所有者の決定。写真が言う設置の動きと、画面で決めた位置で作った JPEG の食い違いを「基準内」と言わない）。JPEG は今のまま確定した位置で作る
- どれも `value` 0・`limit` null・`unit` `rows`（量の無い符号）、`component` `confirmed-position`
- level `confirmed` の `camera-*`・`surface-*`（exceeded）は、推定でなく位置の成分から、G1・G2 の推定の成分と同じ導き方で作る（`applied` true）。`beyondEnvelopeApplied` も位置の成分と紙面で見る
- `assessment` の導き方は変えない。手で直した事・ライブで決めた事は品質の証拠にも不合格の理由にもしない

#### 4.13.5 例（数は例の値）

```
..."pointSource":null,"confirmedPosition":{"referenceCamera":"CAM-A","targetCamera":"CAM-B","serial":9,"source":"manual","baseSource":"auto-button","conditionSha256":"<64 桁>","pose":{"rxDeg":0.25,"ryDeg":-0.125,"rzDeg":0.0625,"txMm":1.5,"tyMm":-0.5,"tzPercent":0.25},"surface":{"h0Mm":1,"slopeX":0.0078125,"slopeY":-0.00390625},"manualEntered":{"seamDxPx":12,"seamDyPx":-4,"rotationDeg":0.20000000000000001}},"confirmedPositionCheck":{"used":true,"seamDifferencePx":1.5},"geometryLevel":{"level":"confirmed",...
```

#### 4.13.6 ログ（撮影の側）

撮影の側の行は、窓が写した値でなく、JPEG の作り手が書いた記録（報告を読み直した物）から書く: `pair_review_confirmed_position given=1 serial=<n> source=<符号> base_source=<符号か none> reference_camera=CAM-A target_camera=CAM-B <9 数> manual_seam_dx_px=<数> manual_seam_dy_px=<数> manual_rotation_deg=<数>（手でない時は manual_entered=none） condition_sha256=<hash> condition_matches=0|1 used=0|1 seam_difference_px=<数か none> level=<符号> quality_accepted=0`。位置が無かった時は `pair_review_confirmed_position given=0 used=0 quality_accepted=0`。9 数は `LivePlacementFields`、2 台の名前は `LivePlacementCameraFields`（ライブの行 `C live_placement serial=… source=… <9 数> condition_sha256=… drawn=1 reference_camera=CAM-A target_camera=CAM-B …` も同じ 2 関数）が書き、数の字面は報告と同じ。

## 5. 符号

### 5.1 警告の符号（`code`）と `class`・`remedy`

設計 11.17.2 の 22 個から `surface-scale` を外した 21 個（所有者の決定 10）。`class` と `remedy` は設計の表のとおり。

| `code` | `class` | `remedy` |
|---|---|---|
| `camera-position` | exceeded | `check-camera-mount` |
| `camera-rotation` | exceeded | `check-camera-mount` |
| `camera-tilt` | exceeded | `check-camera-mount` |
| `camera-height` | exceeded | `check-camera-mount-or-focus` |
| `surface-height` | exceeded／notice | `place-drawing-again`（exceeded）／`none`（notice） |
| `surface-tilt` | exceeded | `place-drawing-again` |
| `output-edge-uncovered` | exceeded | `place-drawing-again` |
| `seam-lift` | notice | `none` |
| `seam-lift-limit` | exceeded | `shift-drawing-horizontal` か `place-drawing-again` |
| `fold-near-seam` | notice | `shift-drawing-horizontal` |
| `gutter-in-band` | notice | `shift-drawing-horizontal` |
| `seam-displacement` | exceeded | 原因で選ぶ（設計 11.22.3） |
| `seam-dx-unmeasured` | unverified／notice | `review-visually`（unverified）／`none`（notice）。設計 11.30 |
| `seam-unmeasurable` | unverified／notice | `review-visually`（unverified）／`none`（notice）。設計 11.30 |
| `placement-unverified` | unverified | `review-visually` |
| `placement-mismatch` | exceeded | `retake` |
| `gate-configuration` | unverified | `contact-technician` |
| `gate-not-run` | unverified | `review-visually` |
| `seam-wide-search` | notice | `none` |
| `geometry-level` | notice | `none` |
| `time-budget` | notice | `none` |
| `position-by-hand` | notice | `none`（v9 のみ。4.13.4） |
| `position-from-live` | notice | `none`（v9 のみ） |
| `estimate-beyond-envelope` | notice | `check-camera-mount`（v9 のみ） |

### 5.2 成分の符号（`component`）

`camera-position`、`camera-rotation`、`camera-tilt`、`camera-height`、`surface-height`、`surface-tilt`、`output-edge`、`seam-lift`、`fold`、`gutter`、`seam`、`placement`、`gate`、`geometry-level`、`time-budget`（※仮。設計は例だけを挙げている）。v9 だけ `confirmed-position`。

### 5.3 直し方の符号（`remedy`）

`shift-drawing-horizontal`、`place-drawing-again`、`check-camera-mount`、`check-camera-mount-or-focus`、`retake`、`retake-then-recalibrate`、`review-visually`、`contact-technician`、`none`。紙を押さえる・平らにする・重しや板を使う符号は作らない（所有者の決定 5）。

### 5.4 その他

- 段（`level`）: `G1`／`G2`／`G3`／`G4`、v9 だけ `confirmed`（4.13.3）。段を下げた理由の符号は `seam-outside-overlap-at-level`、`output-uncovered-at-level`（設計 11.18）。符号を足す時は 1 節の 6
- 層 2 の理由の符号: `not-implemented`（層 2 が入る前の固定値。今は書かない）、`disabled`、`pose-untrusted`、`time-budget`、`baseline-unmeasured`、`too-few-points`、`beyond-lift-line`、`layer-two-failed`（層 2 か、その下の写像 M0・帯の測定が例外を投げた）（`parallax_correction.hpp` の注）。行の `reason` の符号も同じ注
- 推定の理由の符号に増えた物（`registration.reasons`）: `repeating-only`、`coarse-ambiguous`、`time-budget`（`content_registration.hpp` の注）、`estimate-failed`（推定か、その設定の検査が例外を投げた。撮影の経路が書く）
- 折り目の種類: `fold`／`gutter`
- 層 2 の点の出所を使わなかった理由（`pointSource.fallbackReason`）: `edge-share`（探索の端に当たった点の割合が線を超えた）、`no-points`（止め方を通った点が無い）、`measurer-failed`（速い測定が例外を投げた）。符号を足す時は 1 節の 6
- 時間の予算の段（落とす順）: `wide-search-diagnostic`、`band-measurement`、`parallax`、`fold-refit`、`fine-and-fit`、`coarse`、`gate`（合成・符号化・書き込み・報告は落とさない）

## 6. 予約の欄

名前だけを決めて、中身を担当が決めるまで固定しない物。書式には出ても、中身は次の値で書く。

| 欄 | 今の書き方 | 決める担当 |
|---|---|---|
| 条件 `registration.live` | `null`（ライブの推定は接続済み。null の間は走らない） | 所有者（値の案は LC、設計 11.29 N4） |
| 条件 `revisionRules.manualAdjustmentMaxSeamPx` | `null`（上限なし） | 所有者の決定 9 により null のまま |
| 条件 `registration.surfaceScaleWarnPercent` の値 | `null`（警告なし） | 所有者の決定 10 により null のまま |
| 報告 `registration.design`・`parallax.design`・`gate.design` | `{}` | 設計定数が決まった順に各担当 |
| 報告 `registration.display.outputFrame` | `null` | EST |
| 報告 `parallax.profile` の要素の並び | `[]` | PX1 |
| 報告 `folds` の要素の欄 | `[]` | PX1 |
| 報告 `gate.lift`・`gate.wideSearch` の中身 | `null` | GΩ |

## 7. 設計・追補・依頼文との差

所有者の決定を優先した点。

- 縮尺の狂いの警告は出さない（決定 10）: 警告の符号 `surface-scale` を外し、`surface.exceeded` の `scale` を外した。条件の `surfaceScaleWarnPercent` と報告の `surface.scale` は残す
- 手直しの量に上限は設けない（決定 9）: `revisionRules.manualAdjustmentMaxSeamPx` は null＝上限なし。この値で拒否する入口、警告の符号は宣言しない。追補 11.19.3 の「null の間は `manual-adjustment` を受け付けない」は採らない
- 紙を押さえる運用はしない（決定 5）: 直し方の符号に当たる物は無い
- 合成の処理が 180 秒以上かかること自体が誤り（決定 11）: `timeBudget` は安全網として残す。「合成は未作成」を通常の状態として表す欄は無い
- 範囲を超えた推定は確かなら当てて警告（決定 2）、浮きの線は 1.0 mm（決定 3）: 条件の値であり、コードの既定値ではない

- 欄 `pointSource`（4.8.1）を v8 に足した。v8 は main に入るまで凍結しない（状態の節）ので版は上げず、1 節の決まり 1（書き手・読み手・試験の組み立て・この文書を同じ commit で）に従った。この欄を持たない v8 の報告は、読み手が欠けた欄として拒否する（※版の扱いは要確認。main に入った後に足すなら v9）

追補と依頼文の食い違いで追補に合わせた点（関数名など）は、実装の報告に別に挙げる。
