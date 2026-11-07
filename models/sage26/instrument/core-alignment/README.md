# Core-alignment set

## Purpose

The core-alignment set is a series of temporary changes that make Mimic's core behave as SAGE26 does. It exists so that the T1 parity measurement can be taken core-aligned: the `sage26` package is compared with the SAGE26 reference on a Mimic whose core differences from SAGE26 have been removed. The set is applied only on a branch or clone that is never merged. It never lands on `integration/sage26` or `main`, and nothing in it is a proposal to change the core.

## Status of the set

The set is frozen by the owner's approval of the i02-alignment-set node's plan. It holds ten patches, listed in `series` in application order with their SHA-256s in `series.sha256`. Patches 01 to 07 are x01's core-alignment items, 08 and 09 are x02's random-number items, and 10 adds SAGE26's discarded FFB-classifier `rand()`. The set is not edited to make it apply at a later head (see "Applying the set for T1").

Patches 01 to 09 are byte-identical copies of the patches x01 and x02 measured, each written with `git show <commit>:<path>` and checked against its SHA-256. Their headers cite the bases they were made on (x01's `b0071b07` for 01 to 07, x02's `5e91c808` for 08 and 09) and carry x01's and x02's "Reading for i02" paragraphs; they were not edited. They apply unchanged, in order, at the launch base `66a3e9498561e7f2f2207267a94986e9b097b03c`, which the i02 node pins and on which it measured the set. Patch 10 was written by the i02 node at that launch base with 01 to 09 applied; it also applies to the launch base alone.

| # | Patch | Source commit and path | SHA-256 | Measured on |
|---|---|---|---|---|
| 01 | `01-particle-mass.patch` | `a3063d5836ebe86bfe5c96e4cc7fcd27fe177aa1:campaign-reports/x01-core-alignment/patches/01-particle-mass.patch` | `ef734759c5a0a3af6d67947cfca16b24b6f326faac7e19e204d16201d165bc72` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 02 | `02-cosmic-time.patch` | `a3063d5836ebe86bfe5c96e4cc7fcd27fe177aa1:campaign-reports/x01-core-alignment/patches/02-cosmic-time.patch` | `0287fd00235e2dd40a9ed6e7b4ac7889e0c4d56dc620b92ac3c52cde3e9ee10d` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 03 | `03-written-dt.patch` | `a3063d5836ebe86bfe5c96e4cc7fcd27fe177aa1:campaign-reports/x01-core-alignment/patches/03-written-dt.patch` | `9cbdc747792c8f904bdac9ed1b28eeb3767fe0e30f27c5d7a79dbdcd127f91ee` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 04 | `04-substep-count.patch` | `a3063d5836ebe86bfe5c96e4cc7fcd27fe177aa1:campaign-reports/x01-core-alignment/patches/04-substep-count.patch` | `7d3d19ab38b6a0fb8c2bdd470a0650f36f99fdf1d3769be24445fc51813e4c65` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 05 | `05-float-halo-state.patch` | `a3063d5836ebe86bfe5c96e4cc7fcd27fe177aa1:campaign-reports/x01-core-alignment/patches/05-float-halo-state.patch` | `644f5fb4f1a431ed3c607cfc0fbdce66bda6d4de8e87d8c785105a2b8fa266e9` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 06 | `06-inheritance-resets.patch` | `a3063d5836ebe86bfe5c96e4cc7fcd27fe177aa1:campaign-reports/x01-core-alignment/patches/06-inheritance-resets.patch` | `38abd04677cb70e58e0c455215b952e300a63872003c54943dec5bd6a8409f51` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 07 | `07-written-halo-values-stacked.patch` | `a3063d5836ebe86bfe5c96e4cc7fcd27fe177aa1:campaign-reports/x01-core-alignment/patches/07-written-halo-values-stacked.patch` | `9141dcf750acc88c40fb1effe4c1a933878cbd4609ab36666f845557390d8694` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 08 | `08-cgm-useful-draw.patch` | `8fd27634e4543825c4b077b776369dab3d895ed6:campaign-reports/x02-rng-alignment/patches/08-cgm-useful-draw.patch` | `1ab0498c9a2e28a79a7807de336abd0f7c11573f90062556f1135502a89741c2` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 09 | `09-creation-draws.patch` | `8fd27634e4543825c4b077b776369dab3d895ed6:campaign-reports/x02-rng-alignment/patches/09-creation-draws.patch` | `eb46418407feb6de3ef0379b7c93e65541512b36aee1d3e758c3b5c3f5a877c6` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |
| 10 | `10-ffb-discarded-draw.patch` | written by the i02 node (its Slice 1 commit, recorded in `measurement.md`) | `a5f75f89ff545814ed84431c689d9fe64746357ef4f2814b13016b94b868a425` | `66a3e9498561e7f2f2207267a94986e9b097b03c` |

`series.sha256` is the authority for the digests; this table repeats it.

## Format

- One patch per item, named `NN-<slug>.patch`, where `NN` is a two-digit sequence number and `<slug>` is lower-case letters, digits and hyphens (for example `01-particle-mass.patch`).
- Each patch begins with a header comment, placed before the first `diff --git` line (which `git apply` ignores), giving the item's reason and the SAGE26 behaviour it reproduces.
- `series` lists the patch file names in application order, one per line. Blank lines name no patch; any other line must be a bare `NN-<slug>.patch` (no directory, no carriage return).
- `series.sha256` lists, for each `series` entry in the same order, one line `<64 lower-case hex>  <name>`: the patch file's SHA-256, two spaces and its name (the format `shasum -a 256` prints and `shasum -a 256 -c series.sha256` checks from this directory).
- The patches, `series` and `series.sha256` live in this directory.

## Applying the set

`apply.sh` is POSIX `sh`. Run it from the root of a git worktree that is on a throwaway branch or a detached HEAD and has no uncommitted changes:

```sh
sh models/sage26/instrument/core-alignment/apply.sh
```

- It refuses with exit status 2 when the worktree is not clean (`git status --porcelain --untracked-files=all --ignore-submodules=none -- . ':(exclude)docs/dev'` prints anything), or when its branch is `integration/sage26` or `main`. A detached HEAD is allowed; any other failure to read the branch is refused.
- Before applying anything it reads `series` and `series.sha256` by redirection from the files (never through a here-document) and refuses with exit status 2, naming the line or entry and applying nothing, unless every non-blank line of either file is well formed, the two files name the same patches in the same order, and each patch file's SHA-256 equals its `series.sha256` line.
- It applies each patch named in `series`, in order, with `git apply --index`, so the changes are staged. It commits nothing.
- If any patch fails, it restores the worktree to its starting commit and clean state with `git reset --hard HEAD` (safe, because the worktree was clean at the start) and exits 2 naming the patch.
- Otherwise, provided the number applied equals the number of series entries, it prints a heading and one line `<SHA-256>  <name>` per applied patch and exits 0. With an empty `series` and an empty `series.sha256` it applies nothing, says so, and exits 0.

## Applying the set for T1

The final T1 measurement applies the set to the final integration head by this procedure:

1. Make a sparse clone of the final integration head: `git clone --shared --no-checkout`, then `git sparse-checkout set --no-cone '/*' '!/docs/dev/'` before checking out the head detached.
2. From the clone's root, before creating any link or untracked file, run `sh models/sage26/instrument/core-alignment/apply.sh`. It must exit 0 and print ten digest lines equal, in order, to `series.sha256`.
3. Commit the staged changes in the clone only. That commit is never pushed and never merged.
4. Build with `MODEL=sage26 SIMULATION=mini-millennium` and run the vertical run file `models/sage26/input/sage26_mini-millennium.yaml` unchanged.
5. Compare with `models/sage26/instrument/compare_sage26.py` against the T1 reference with `--partitions 8` at T1's default snapshots.

The set is measured under the vertical driver only: the horizontal driver cannot follow SAGE26's per-forest `rand()` stream (campaign decision D1). If `apply.sh` refuses at a later head, the set is not edited to make it apply: the refusal goes to the campaign manager.

## Record

- `measurement.md`: the i02 node's measurement of the full set against SAGE26's R6 and T1 references, the consumption map of SAGE26's `rand()` stream, and each item's contribution at R6 left out of the set.
- `items.md`: per item, what it changes, the SAGE26 behaviour it reproduces, why Mimic differs, its measured contribution, how to apply it, and its provenance.
