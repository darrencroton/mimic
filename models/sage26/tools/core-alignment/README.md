# Core-alignment set

## Purpose

The core-alignment set is a series of ten patches that make Mimic reproduce SAGE26's core-level choices: the particle mass, the cosmic-time integrator, the written `dT`, the substep count, float storage of halo state, inheritance resets, the written halo values and SAGE26's libc `rand()` stream. Mimic's core keeps its own behaviour in each of these on purpose (see `items.md`); the set exists only to measure parity, so that the `sage26` package can be compared with SAGE26 on a Mimic whose core differences from SAGE26 have been removed. With the set applied the package reproduces SAGE26's output bit for bit (the package README's [Agreement with SAGE26](../../README.md#agreement-with-sage26) reports the measurement).

The set is applied only in a disposable clone, never in a checkout anyone works in, and the result is never merged. Nothing in it is a proposal to change Mimic.

## The Patches

`series` lists the patches in application order; `series.sha256` pins each by its SHA-256. Each patch begins with a header comment stating the SAGE26 behaviour it reproduces, the Mimic behaviour it replaces, what it changes, and what it would change in other packages. Together they change eight files: `Makefile`, `simulations/mini-millennium/simulation_info.yaml`, `src/core/init.c`, `src/core/halo_evolution.c`, `src/core/inheritance.c`, `src/io/output/util.c`, `models/sage26/shared/sage_keyed_draw.h` and `models/sage26/modules/sage26_determine_ffb_regime/sage26_determine_ffb_regime.c`.

| # | Patch | What it does |
|---|---|---|
| 01 | `01-particle-mass.patch` | Sets mini-Millennium's `particle_mass` to SAGE26's 0.086 (the simulation package's value is 0.0860657). |
| 02 | `02-cosmic-time.patch` | Computes the cosmic-time table with GSL's `gsl_integration_qag`, as SAGE26 does, instead of the core's adaptive Simpson rule; the build then needs GSL. |
| 03 | `03-written-dt.patch` | Writes `dT` rounded through float, as SAGE26 does; the stored double `dT` is unchanged. |
| 04 | `04-substep-count.patch` | Replaces the run file's substep count with SAGE26's floored adaptive rule, N = min(30, max(10, ceil(10 dT / t_dyn))). |
| 05 | `05-float-halo-state.patch` | Rounds each store of `Mvir`, `deltaMvir`, `Rvir`, `Vvir`, `Vmax` and the infall values through float, as SAGE26 stores them. |
| 06 | `06-inheritance-resets.patch` | Keeps an orphan's `Len` and its `Cooling`, `Heating`, `QuasarModeBHaccretionMass` and `SupernovaOutflowRate`, zeroing the four only on the main branch, as SAGE26 does. |
| 07 | `07-written-halo-values-stacked.patch` | Writes `MostBoundID` (as its absolute value), `Spin`, `VelDisp`, `Rvir` and `Vvir` from the host halo for every row, as SAGE26's writer does; it applies only after 03. |
| 08 | `08-cgm-useful-draw.patch` | Draws the CGM regime classifier's deviate from libc `rand()`, as SAGE26 does, instead of the package's keyed deviate. |
| 09 | `09-creation-draws.patch` | Discards two `rand()` values at each galaxy creation, as SAGE26's `init_galaxy` does; it applies only after 05. |
| 10 | `10-ffb-discarded-draw.patch` | Discards one `rand()` value for each row the FFB classifier classifies, as SAGE26 does. |

`items.md` documents each item in full: the Mimic and SAGE26 source lines it concerns, why Mimic differs, its measured contribution and its dependencies.

## Format

- One patch per item, named `NN-<slug>.patch`, where `NN` is a two-digit sequence number and `<slug>` is lower-case letters, digits and hyphens (for example `01-particle-mass.patch`).
- Each patch's header comment is placed before the first `diff --git` line, which `git apply` ignores.
- `series` lists the patch file names in application order, one per line. Blank lines name no patch; any other line must be a bare `NN-<slug>.patch` (no directory, no carriage return).
- `series.sha256` lists, for each `series` entry in the same order, one line `<64 lower-case hex>  <name>`: the patch file's SHA-256, two spaces and its name (the format `shasum -a 256` prints and `shasum -a 256 -c series.sha256` checks from this directory).
- The patches, `series` and `series.sha256` live in this directory.

## Applying the Set

`apply.sh` is POSIX `sh`. Run it from the root of a disposable clone that is on a throwaway branch or a detached HEAD and is fully clean:

```sh
sh models/sage26/tools/core-alignment/apply.sh
```

- It refuses with exit status 2, applying nothing, when the clone is not clean (`git status --porcelain --untracked-files=all --ignore-submodules=none` prints anything: any change to a tracked file, or any untracked file that is not ignored), when it is not run from the clone's root, or when its branch is `integration/sage26` or `main`. A detached HEAD is allowed; any other failure to read the branch is refused.
- Before applying anything it reads `series` and `series.sha256` and refuses with exit status 2, naming the line or entry, unless every non-blank line of either file is well formed, the two files name the same patches in the same order, and each patch file's SHA-256 equals its `series.sha256` line.
- It applies each patch named in `series`, in order, with `git apply --index`, so the changes are staged. It commits nothing.
- If any patch fails, it restores the clone to its starting commit with `git reset --hard HEAD` and exits 2 naming the patch. The reset discards only what the script staged, because the whole clone was clean when it started.
- Otherwise it prints one line `<SHA-256>  <name>` per applied patch, which must equal `series.sha256` line for line, and exits 0. With an empty `series` and an empty `series.sha256` it applies nothing, says so, and exits 0.

A parity measurement with the set:

1. Make a disposable clone of the commit to measure, checked out detached: `git clone --shared --no-checkout <repository> <clone>`, then `git -C <clone> checkout --detach <commit>`.
2. From the clone's root, before creating any link or untracked file, run `sh models/sage26/tools/core-alignment/apply.sh`. It must exit 0 and print ten digest lines equal to `series.sha256`.
3. Commit the staged changes in the clone only. That commit is never pushed and never merged.
4. Build with `make MODEL=sage26 SIMULATION=mini-millennium` (GSL must be installed, for patch 02) and run the vertical run file `models/sage26/input/sage26_mini-millennium.yaml` unchanged.
5. Compare the run with the T1 reference using `models/sage26/tools/compare_sage26.py` with `--partitions 8` at its default snapshots (see [`../README.md`](../README.md)).

The set reproduces SAGE26 under the vertical driver in one process only: the horizontal driver, and MPI ranks, consume SAGE26's `rand()` stream in a different order. A patch whose context no longer matches a later commit makes `apply.sh` fail at that patch and restore the clone; the patch must then be remade against the new code, its header kept, and `series.sha256` updated.
