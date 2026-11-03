# Core-alignment set

## Purpose

The core-alignment set is a series of temporary changes that make Mimic's core behave as SAGE26 does. It exists so that the T1 parity measurement can be taken core-aligned: the `sage26` package is compared with the SAGE26 reference on a Mimic whose core differences from SAGE26 have been removed. The set is applied only on a branch that is never merged. It never lands on `integration/sage26` or `main`, and nothing in it is a proposal to change the core.

## Format

- One patch per item, named `NN-<slug>.patch`, where `NN` is a two-digit sequence number and `<slug>` is a short lower-case description (for example `01-example-item.patch`).
- Each patch is made against the integration head that the later alignment node pins, so it applies to that commit with `git apply`.
- Each patch begins with a header comment, placed before the first `diff --git` line (which `git apply` ignores), giving the item's reason and the SAGE26 behaviour it reproduces.
- `series` lists the patch file names in application order, one per line. Blank lines name no patch. The file committed with this scaffolding is empty (zero bytes): the set holds no items yet.
- The patches and `series` live in this directory.

## Applying the set

`apply.sh` is POSIX `sh`. Run it from the root of a git worktree that is on a throwaway branch and has no uncommitted changes:

```sh
sh models/sage26/instrument/core-alignment/apply.sh
```

- It refuses with exit status 2 when the worktree is not clean, or when its branch is `integration/sage26` or `main`.
- It applies each patch named in `series`, in order, with `git apply --index`, so the changes are staged. It commits nothing.
- If any patch fails, it restores the worktree to its starting commit and clean state with `git reset --hard HEAD` (safe, because the worktree was clean at the start) and exits 2 naming the patch.
- Otherwise it prints each applied patch's SHA-256 and exits 0. With an empty `series` it applies nothing, says so, and exits 0.

## Status of the set

The set is fixed only by the owner's approval of a later instrument node. Until then `series` is empty and no item patch exists in this directory.
