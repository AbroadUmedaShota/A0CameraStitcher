## Issue

- Closes #
- Draft PR base: `codex/main-feature-integration`

## Summary

## 変更内容

<!-- 何をどう変えたか。ファイル単位ではなく、振る舞いの変化で書く -->

-

## Requirement / WorkItem

- Requirement IDs:
- WorkItem ID:

## 動作確認

<!-- 実行したコマンドと結果を貼る。「確認した」だけでは受け付けない -->

```powershell

```

- [ ] `pwsh -File .\scripts\Test-M3Simulated.ps1 -Configuration Release`
- [ ] `git diff --check`
- [ ] Hardware checks, if applicable

## 影響範囲

<!-- 触った所有領域と、そこから影響が出うる範囲 -->

- 所有領域:
- 影響が出うる範囲:
- [ ] この PR の所有領域外にある shared file を変更していない

## Software / Hardware 境界

- [ ] software-only（camera command を送らず、実機・SDK・WPD へアクセスしない）
- [ ] hardware-required（実機を操作する。human gate を下に紐付ける）

実機を操作した場合は、実行した command と evidence の保存先を書く:

## Verification

- [ ] Automated checks
- [ ] No camera serials, customer images, SDK archives, or licensed binaries are committed
- [ ] Human gate is linked when required
- [ ] .NET テストを追加／削除した場合、`scripts/Test-M3Simulated.ps1` の期待件数を同じ PR で更新した（該当なしならチェック）

## Risks and rollback

## Reviewer

- Reviewer:
- [ ] Draft のまま作成し、merge と push は Sol に任せる
