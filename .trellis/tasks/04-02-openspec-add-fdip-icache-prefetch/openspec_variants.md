# OpenSpec Variants

This file records how duplicate OpenSpec changes from the root repo and
sub-worktrees were merged into a single Trellis task.

- Logical change id: `add-fdip-icache-prefetch`
- Canonical variant: `.worktrees/fdip-phase2-xsdev/openspec/changes/add-fdip-icache-prefetch`
- Selection rule: collapse copies identical to root, then sort by
  `task_done > ownership_overlap > file_count > root_bonus > latest_mtime`.

## Variants

| Source | Path | Selected | Progress | Files | Ownership Tokens | Same As Canonical |
|--------|------|----------|----------|-------|------------------|-------------------|
| `fdip-phase2-xsdev` | `.worktrees/fdip-phase2-xsdev/openspec/changes/add-fdip-icache-prefetch` | yes | 38/116 | 16 | fdip | yes |
| `fdip-rtl-align-xsdev` | `.worktrees/fdip-rtl-align-xsdev/openspec/changes/add-fdip-icache-prefetch` | no | 34/52 | 12 | fdip | no |
| `impl-control-pc-tail-halfword` | `.worktrees/impl-control-pc-tail-halfword/openspec/changes/add-fdip-icache-prefetch` | no | 0/88 | 4 | - | no |
| `impl-control-pc-views` | `.worktrees/impl-control-pc-views/openspec/changes/add-fdip-icache-prefetch` | no | 0/42 | 4 | - | no |
| `root` | `openspec/changes/add-fdip-icache-prefetch` | no | 0/88 | 6 | - | no |

## Notes

- Worktree variants may be stale copies, branch-local refinements, or
  the actively maintained version for that branch.
- Keep only the canonical task active in Trellis; consult alternate
  variant directories when branch-local work diverges from the root view.
