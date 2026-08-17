# 開発参加ルール

複数の担当が同じ base branch へ並行して作業します。private plan では branch protection を使えないため、`.github/CODEOWNERS`・CI・「統合は Sol だけが行う」という運用ルールで代替します。これらは自動では止まらないため、読んで守ることが前提です。

先に読むもの:

- [AGENTS.md](AGENTS.md) — リポジトリの scope、Sources Of Truth、Safety And Data
- [docs/PRODUCT_REQUIREMENTS.md](docs/PRODUCT_REQUIREMENTS.md) — 製品要件
- [docs/ROADMAP.md](docs/ROADMAP.md) — マイルストーンと現在地

## 役割

| 役割 | 担当 |
| --- | --- |
| Sol | 唯一の integration owner。`codex/main-feature-integration` への merge と push を行うのは Sol だけ |
| Tera | 整理担当 |
| Luna / 人間担当 | bounded implementer。割り当てられた 1 Issue の所有領域だけを実装する |

Sol / Tera / Luna は役割名で、GitHub アカウントと 1:1 では対応しません。`.github/CODEOWNERS` には実在するアカウントだけを書きます。

## 作業単位

1 Issue = 1 branch = 原則 1 Draft PR。

- branch 名は Issue 本文の `Branch / PR` 節に書かれたものを使う。指定がなければ `codex/issue-<number>-<slug>`
- 分岐元と Draft PR の base はどちらも `codex/main-feature-integration`。`main` から分岐しない
- PR は Draft で作成する。自分では merge しない
- 1 Issue に PR を 2 本以上作らない。分割が必要になったら Issue を分ける

```powershell
git fetch origin codex/main-feature-integration
git switch -c codex/issue-3-team-governance origin/codex/main-feature-integration
```

## 所有領域と shared file

各 Issue は「所有領域」節で変更してよいファイル範囲を宣言します。自分の Issue が所有していないファイルは変更しません。

- shared file の同時編集は禁止。同じファイルを所有する別 Issue が open のうちは触らない
- 触る必要が出たら、勝手に編集せず Issue へ記録して Sol の判断を待つ
- 修正 loop は原則 1 回。レビュー指摘への対応は 1 回の push にまとめる。2 回以上必要になったら、Issue の分割か scope の見直しを先に提案する

レーンの区分は [.github/CODEOWNERS](.github/CODEOWNERS) を参照してください。

## human gate

次の 3 種類は自動で進めません。Issue に `human-gate` ラベルを付けて停止し、人の承認を得てから再開します。

- 実機操作 — 実 D810 の camera open、撮影、設定 write、カード削除、Live View
- 外部公開 — `main` への push、release、リポジトリ外への配布・投稿
- 破壊的操作 — 既存 evidence の削除・書き換え、force push、camera card の bulk delete / format

判断そのものを記録したいときは Specification decision / Release decision / UX review の各 Issue フォームを使います。

## コミットしないもの

[AGENTS.md](AGENTS.md) の Safety And Data と同じ規則です。

- カメラの serial number — committed fixture では redacted alias を使う
- credential、API key、`.env`
- Nikon SDK の配布物（archive、header、`.dll` / `.lib`）
- licensed binary
- 実撮影画像・顧客原稿

画像とバイナリは `.gitignore` で除外済みです（例外は `docs/assets/*.png` のみ）。`git add -f` で除外を回避しないでください。

## 検証

PR を出す前にローカルで実行します。

```powershell
pwsh -File .\scripts\Test-M3Simulated.ps1 -Configuration Release
pwsh -File .\scripts\Test-DualCameraWpfFlow.ps1 -Configuration Release
```

Native レーンを変更した場合は CMake ビルドと CTest も回します。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

### .NET テスト件数の同期規則

`scripts/Test-M3Simulated.ps1` はテスト件数をリテラル文字列で assert します。

- `Foundation tests: 22/22 passed.`
- `DualCamera flow tests: 18/18 passed.`
- `Operator shell tests: 22/22 passed.`

.NET テストを追加または削除した PR は、同じ PR の中で `scripts/Test-M3Simulated.ps1` の期待件数を更新してください。更新しないと、テストが全部通っていてもこの assert で落ちます。

この 1 行の数値更新は所有領域の例外として認めます。自分の PR が壊す assert を、同じ PR で直すためです。数値だけを更新し、assert の構造や他の行は変更しないでください。スクリプトを件数非依存にする恒久対応は別 Issue の担当であり、この規則の回避には使いません。

## ラベル

実在するラベルだけを使います。Issue フォームが自動で付けるもの以外は手動で付けてください。

| 区分 | ラベル |
| --- | --- |
| 優先度 | `priority:P0` `priority:P1` `priority:P2` `priority:P3` |
| 領域 | `area:team` `area:ci` `area:app` `area:camera-agent` `area:identity` `area:single-camera` `area:dual-camera` `area:docs` |
| 環境 | `software-only` `hardware-required` |
| 安全境界 | `safety:S1` `safety:S2` `safety:S3` |
| 状態 | `ready` `active` `blocked` `done` `human-gate` `safety-stop` |
| 種別 | `enhancement` `bug` `documentation` `question` `duplicate` `invalid` `wontfix` |
| 決定 gate | `spec-decision` `release-gate` `ux-review` |
| 運用 | `autodev:managed` |

`autodev:` が付くのは `autodev:managed` だけです。`autodev:human-gate` のような名前のラベルは存在しません。

## CODEOWNERS の位置づけ

[.github/CODEOWNERS](.github/CODEOWNERS) はレーンの所有を宣言しますが、private plan では review 必須として強制されません。「誰に聞くか」を示す運用ルールであって gate ではない、と理解してください。

実際に効く gate は次の 2 つです。

1. Sol だけが `codex/main-feature-integration` へ統合する
2. Windows software-only CI が PR を検証する（Issue #4 で追加予定）

## Issue と PR

Issue は [.github/ISSUE_TEMPLATE/](.github/ISSUE_TEMPLATE) のフォームから作成します。blank issue は無効です。

| フォーム | 用途 |
| --- | --- |
| Implementation task | 実装作業 |
| Bug report | 不具合報告と修正 |
| Requirements submit | 製品要件の提出・変更 |
| Specification decision | 技術・製品仕様の決定 gate |
| UX review | 操作フロー・文言の決定 |
| Release decision | MVP release の承認 |

PR は [.github/pull_request_template.md](.github/pull_request_template.md) の各欄を埋めてください。空欄のまま Draft を外さないでください。
