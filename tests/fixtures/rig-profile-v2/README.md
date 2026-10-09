# rig profile v2 共通試験ベクトル（#281）

すべて合成データ。実リグの承認・品質判定を表すものではない。画像・物理識別子を含まない。

`template.json` は公開 draft 雛形と同じ内容、`calibrated-draft.json` は所有者の4ブロックが null の校正済み draft、`approved.json` は承認検証を試験する合成の sentinel。寸法・写像の組は既存 `Test-RigProfileV2Schema.ps1` の sentinel に合わせた。

各 `.canonical.txt` と `.sha256` は製品 reader を使わず、PowerShell により schema の `properties` 順、明示した5つの整数欄、`BitConverter.DoubleToUInt64Bits`、UTF-8/LF、SHA-256 から独立して作成した。行順の正本は設計9.2節。数値はすべて binary64 で正確に表せる値で、丸め境界は reader の別の契約試験で確認する。

| ベクトル | 正規化 UTF-8 byte 数 | SHA-256 |
| --- | ---: | --- |
| template | 695 | `fb3d55df2172d4b0f4888a1b482945bff1a93a24d0c6416d2b19c8fddd453777` |
| calibrated-draft | 4312 | `f133057b1e9cbd0ad76574e56f10ff3f3a20069cdae56fa737ebed84d9f62113` |
| approved | 5726 | `e0a485d44e026470b60d9a04db9c00b79e8e819434b9f8b006f1c14ab4702d49` |

正規化ファイルは末尾にも LF を含む。C++ と .NET の両試験は同じ JSON・正規化本文・hash を読み、全 byte と hash を比較する。これらの値を実写の provenance として流用しない。
