# v2 profile reader と fingerprint（#281、#274 先行）

ADR-0034 の schema 2.0.0 を C++ と .NET で読み、文書全体の同一性を同じ SHA-256 で表す。正本は `docs/design/rig-profile-v2.md` 9・10節と `docs/schemas/rig-profile.v2.schema.json`。

## 入り口と承認の境界

C++ は `a0::m2::RigProfileV2`（`rig_profile_v2.hpp`）、.NET は Foundation の `RigProfileV2`。`Parse` が形と値を検証して不変の snapshot を返す。未知・欠落・重複欄、非有限数、v1、固定規約や D810 寸法の違い、係数の省略、物理識別子を拒否する。整数欄の小数・指数表記を拒否し、UTC 秒の書式だけでなく実在する日付も検査する。

draft は雛形か完全な校正結果のどちらか。所有者が決める outputRaster・correctionEnvelope・qualityContract・approval はすべて null。`ValidateCalibrationForEvaluation` は校正済み draft と別入力 raster を検証し、profile の欄を変更しない。雛形や approved を評価用 draft として受け付けない。

`ValidateApprovedForUse` は approved に加えて `measuredAt <= approvedAt <= assessedAt < validUntil` を要求する。ファイルの status は承認記録の整合性を表すもので、所有者の決定を新たに作る機能ではない。

領域は raw JSON の十進表記から厳密に µm 整数へ変換する。binary64 への丸めが 1 µm 刻みの不正値を隠さない。DPI の整数 ceiling、用紙内の領域、engine の寸法・画素数上限、射影の非特異性・正の分母、放射の単調性を検査する。射影領域には ceiling で増えた raster の外縁も含む。これらは数式の有効性の検査で、実写の品質合否ではない。

## 同一性

`CanonicalFingerprintText` は全欄を設計9.2節の順で出力する。null ブロックは1行、カメラは CAM-A、CAM-B、行列は行優先。文字列は ASCII、数は最近接偶数の binary64 bits、整数は十進表記、負の zero は正の zero。UTF-8、全行 LF、最終行も LF。`FingerprintSha256` はこの byte 列を hash する。評価時刻とファイルのパスは含めない。

C++ の reader/hash 境界は `a0_m2_rig_profile_v2`。renderer が reader や BCrypt に依存する向きにはしない。生成器も製品 reader に依存しない。`.NET` は Foundation の SHA-256 を使う。

## 試験

`tests/fixtures/rig-profile-v2` の3種類の共通 JSON、独立して作成した正規化本文と hash を両実装から照合する。`number-cases.json` は subnormal・underflow・負の zero・最近接偶数の丸め境界を含む。各言語の試験は不正な形・数値・時刻・draft と approved の境界、外部 raster の幾何検証も確認する。

```powershell
cmake -S . -B build/stub -G "Visual Studio 17 2022" -A x64
cmake --build build/stub --config Debug --target m2_rig_profile_v2_contracts
ctest --test-dir build/stub -C Debug -R '^m2_rig_profile_v2_contracts$' --output-on-failure
dotnet run --project tests/m3/RigProfileV2Tests/A0CameraStitcher.M3.RigProfileV2Tests.csproj -c Debug -- tests/fixtures/rig-profile-v2
```

SDK なし Debug/Release の関連 native target のビルドと CTest は両構成とも 10/10 PASS。対象は新 reader、v1 renderer/offline stitcher、文書 renderer/MTF、published command、link isolation、schema・corpus・pre-gate。全体 CTest の再実行ではない。新 native 契約は両構成 105 項目、.NET reader は両構成 163 項目、既存 Foundation は両構成 43/43 PASS。3本文/hash と12数値ベクトルが一致した。既存 `offline_stitcher_tests.cpp` の C4819 警告は残るが、新 reader のビルドエラーはない。

独立レビューで検出した非負欄の負 underflow（`-1e-400` が binary64 の zero に丸まる場合）を raw 十進の符号検査で両言語とも拒否した。一般のレンズ係数欄では最近接丸めの underflow を許可し、正規化 zero として記録する。回帰試験は測定残差・補正上限と符号付きの厳密な zero を含む。

## 完了境界

この変更は reader・使用検証・fingerprint とその契約試験まで。製品 adapter への JSON 接続、manifest の照合、入力 JPEG 寸法の検査、画像 I/O、#273 の評価 exe は後続。v1 の試験経路は ADR-0034 の移行規則に従って維持する。実リグの approved profile、DPI・品質閾値、kernel の採用を決定していない。
