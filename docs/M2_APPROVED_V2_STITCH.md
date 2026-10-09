# approved-v2 の製品 stitch 入口（#282、#274 先行）

`#280` の文書 renderer と `#281` の strict reader を、native の保存経路と .NET の照合に接続する。実リグの承認、品質判定、撮影、profile 選択 UI を追加する変更ではない。

## 入力と適用値

native API は `StitchCanonicalPairV2(OfflineStitchV2Request)`。request は2つの canonical original、未使用の job directory、approved profile ファイル、UTC 秒の assessment、期待する profile fingerprint、明示した kernel、job/capture ID、UTC 秒の completion を要求する。

CLI `stitch-v2` の引数は次の10個だけ。未知・重複・不足、未対応 kernel は拒否する。

| option | 値 |
| --- | --- |
| `--camera-a` / `--camera-b` | 7360×4912 の別々の `original.jpg` |
| `--job-directory` | 未使用の job directory。親は既存の通常 directory |
| `--profile-file` | approved の schema 2.0.0 JSON |
| `--expected-profile-sha256` | 呼び出し側が確認した snapshot の fingerprint |
| `--assessed-at` | `yyyy-MM-ddTHH:mm:ssZ` |
| `--resampling` | `bilinear` / `bicubic-catmull-rom` |
| `--stitch-job-id` / `--capture-transaction-id` | 非zero、32桁の小文字hex |
| `--completed-at` | 実在する UTC 秒。assessment 以降 |

ファイルの場所は通常の local fixed-drive absolute path。device、UNC、ADS、曖昧な component、reparse の入口を拒否し、既存の ancestor と leaf を handle で固定する。profile は 256 KiB、JPEG は 64 MiB の byte 上限を持つ。

native は profile の locked bytes を自分で解析・検証する。draft、v1、期限切れ、不正値と expected fingerprint の不一致は job 作成前に拒否する。期待 hash は意図との照合にだけ使い、manifest には実際に適用した不変 snapshot から native が計算した hash を書く。profile、両入力を読み取りロックのまま保持し、再読の一致を確認する。

画像は保存順のまま復号し、`RenderDocumentPair` の単一の写像・標本化へ渡す。撮影ごとの新しい homography や補正値は推定しない。

## 公開と記録

v1/v2 は job reservation、flush、candidate 検査、非置換 rename、manifest 公開・再読を同じ実装で行う。失敗候補は保持し、既存の output・manifest・partial を置換しない。

v2 の JPEG encoder は `CREATE_NEW` の専用 handle を保持する。JFIF APP0 を units=1、X/Y density=profile DPI にそろえ、Orientation を書かず、同じ handle の byte を再読する。生成 byte の SHA・サイズ・file identity を保持し、flush と公開前の immutable snapshot と照合する。別の有効な JPEG に差し替わった場合も公開せず、診断候補を保持する。

manifest は既存 `a0.stitch-job-manifest.v2`。profile version は `2.0.0`、fingerprint に評価時刻は含めない。engine ID は `a0.m2.offline-stitcher`、version は `2.0.0+bilinear` または `2.0.0+bicubic-catmull-rom`。同じ profile で kernel を変えた場合も実行内容を区別できる。legacy の `1.0.0` は維持する。

## .NET

`M2OfflineStitcherProcessAdapter.StitchV2Async` は `RigProfileV2` の期待 snapshot とファイルの両方を受ける追加 API。既存の `IOfflineStitcherAdapter` / `DualCameraProductFlow` の v1 試験 API は変更しない。

起動前に file の snapshot と期待 fingerprint を照合し、profile と両 original をロックしたまま native を呼ぶ。exact stdout、job/capture/time、profile ID/version/hash、kernel を表す engine、入力 hash/size、出力 path/dimensions/hash/size、seam の欄を検査する。新 job directory と output/manifest も固定し、JFIF byte、Orientation、native の manifest 再検証と complete decode、入力の再検証を終えてから artifact を返す。

## 試験と完了境界

共通の `approved-product-crop.json` は合成の sentinel。実寸の合成入力を4×4の被覆部分へ切り出すことで、両 kernel の反復 byte 一致、入力/profile 不変、DPI、保存、manifest、.NET の接続を検査する。C++ 試験の入力は非一定の色パターン。実写や A0 全領域の処理時間・品質の証拠ではない。

```powershell
cmake -S . -B build/stub -G "Visual Studio 17 2022" -A x64
cmake --build build/stub --config Debug --target m2_offline_stitcher_v2_contracts
ctest --test-dir build/stub -C Debug -R '^m2_offline_stitcher_v2_contracts$' --output-on-failure
dotnet run --project tests/m3/ApprovedV2StitchTests/A0CameraStitcher.M3.ApprovedV2StitchTests.csproj -c Debug -- tests/fixtures/rig-profile-v2 build/stub/Debug/A0CameraStitcher.M2Adapter.exe
```

SDKなしの関連 native target build と CTest は Debug/Release とも11/11 PASS（全体63件の再実行ではない）。新 native 統合は両構成196項目、.NET 統合は両構成96項目 PASS。既存 Foundation は両構成43/43、.NET reader は両構成163項目 PASS。既存 `offline_stitcher_tests.cpp` の C4819 警告は維持し、新規のビルドエラーはない。

通常ファイルの有効な JPEG 差し替えは、保存した修正前 library と現行試験を組み合わせて再現した（3項目中2失敗）。修正後は3項目 PASS。同一 file ID の有効な JFIF 版変更も、DPI の独立検査を通したうえで生成 byte の SHA 違反として拒否した。偽の子プロセスは無変更の正常対照を通し、profile・両入力の write/delete アクセスが共有違反になることを実測した。15種類の偽成功では、期待した具体的な拒否理由と、子が実際に fixture を生成した証跡を確認している。

#273 の draft 評価 exe は [評価合成](M2_DRAFT_STITCH_EVAL.md) に別入口として追加した。#274 の圧縮後基準点 p95 0.05 px の評価、UI の profile 選択・setup 接続、#46 の補正 pipeline は後続。最初の approved-v2 が出る #45 までは、ADR-0034 の移行規則に従って v1 試験経路を残す。品質・実機の受入を自動で承認しない。
