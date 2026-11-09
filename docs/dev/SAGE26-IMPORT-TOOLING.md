# SAGE26 Import Tooling (this branch)

**Purpose**: Record what the `sage26/import-tooling` branch holds, why it is not on `main`, and how to use it again when the `sage26` package is next measured against SAGE26 (for example at SAGE26's v1.0).

This branch is `main` as it stood before the v1.6 cleanup (commit `5d9e6ebf`) plus this document. `main` keeps the `sage26` package user-facing: its modules, properties, run files, tests, baseline and figures, with no trace of how the import was verified. Everything that verified it lives here, in its working context, so it can be brought back whole.

## What is here and not on `main`

| Piece | Path on this branch | What it is |
|---|---|---|
| Galaxy-by-galaxy comparator | `models/sage26/tools/compare_sage26.py`, `tools/field_map.yaml`, `tools/README.md` | Compares a `sage26` HDF5 run with a SAGE26 reference output galaxy by galaxy through the field map, with a `pass`/`fail` verdict. The README gives the matching rules, tolerances and exit codes |
| Comparator controls | `models/sage26/tools/controls/`, `models/sage26/modules/_tests/test_scientific_compare_sage26.py` | 23 synthetic known-good and known-bad controls (no reference needed) and 17 reference-derived ones; the self-test runs the synthetic set |
| Core-alignment set | `models/sage26/tools/core-alignment/` (`series`, `series.sha256`, `apply.sh`, `items.md`, ten patches) | Ten patches that give Mimic's core SAGE26's behaviours and replay its `rand()` draws, applied only in a throwaway clone; `items.md` records each item's measured effect. The same ten changes are commits on the `sage26/core-alignment` branch |
| Function-level oracle | `models/sage26/modules/_tests/oracle/` (`build.sh`, `regenerate.sh`, `oracle_c*.c`, `fixtures.manifest`, README) | Compiles SAGE26's own physics sources into small mains that record what each routine computes on an explicit input |
| Oracle fixtures and tests | `models/sage26/modules/<module>/_tests/oracle/*.in,*.out` (1,044 pairs), `_tests/test_unit_*_oracle.c` (24), `modules/_tests/sage26_oracle_*.h`, `test_unit_sage26_oracle_reader.c` | Each module's oracle test runs the fixtures through `process()` and compares bit for bit; they compare only on the pinned Apple clang build (`oracle_environment_mismatch()`) and SKIP elsewhere |
| The verification record | `models/sage26/README.md` (*Departures*, *Agreement*, *R0–R6*, *Caveats*), the module READMEs' *SAGE26 Source* and *Differences* sections, the `R/src/<file>:<line>` citations in every source and test | The line-by-line correspondence with SAGE26 at commit `11b1ae851524baadb76cfd17e0b38f8a89c07224`, which `main` no longer carries |

## External dependencies

- The staged SAGE26 source at `11b1ae8` (`sage-home/SAGE26`): `oracle/build.sh` and `controls/run_controls.sh` read it from `SAGE26_SRC` (default `/Volumes/Internal/results/sage26-campaign/reference-src`). A fresh clone of SAGE26 at that commit serves the same purpose.
- The T1 reference: SAGE26's `model_0.hdf5` from its default configuration on mini-Millennium, whose SHA-256 the comparator pins as `T1_REFERENCE_SHA256`. It is not in any repository; regenerate it by running SAGE26 at `11b1ae8` with its `input/millennium.par` on the mini-Millennium trees.
- The campaign dossier (`/Users/dcroton/Local/git-repos/campaigns/sage26/`, archived at `/Volumes/Internal/results/sage26-campaign/dossier.git`) holds the decisions, evidence and per-item measurements behind `items.md`.

## Using it against a later `main`

The pieces are directories and files that `main` does not have, so they can be restored onto a working branch without conflict:

```bash
git checkout -b sage26/remeasure main
git checkout sage26/import-tooling -- models/sage26/tools models/sage26/modules/_tests/oracle \
    models/sage26/modules/_tests/sage26_oracle_cgm.h models/sage26/modules/_tests/sage26_oracle_cooling.h \
    models/sage26/modules/_tests/sage26_oracle_fire.h models/sage26/modules/_tests/sage26_oracle_fixture.h \
    models/sage26/modules/_tests/sage26_oracle_h2.h models/sage26/modules/_tests/sage26_oracle_hydrogen.h \
    models/sage26/modules/_tests/sage26_oracle_join.h models/sage26/modules/_tests/sage26_oracle_sizes.h \
    models/sage26/modules/_tests/test_unit_sage26_oracle_reader.c \
    models/sage26/modules/_tests/test_scientific_compare_sage26.py
for m in models/sage26/modules/sage26_*/; do
  git checkout sage26/import-tooling -- "$m/_tests/oracle" "$m"/_tests/test_unit_*_oracle.c 2>/dev/null
done
```

Then register the oracle tests again in each module's `module_info.yaml` (`unit:` becomes a list with the `_oracle.c` entry) and the reader self-test and comparator self-test in `modules/_tests/module_info.yaml`, run `make MODEL=sage26 SIMULATION=mini-millennium generate`, and the tiers pick them up. The core-alignment patches were written against the core at `d0f51d62`; `apply.sh` checks each against `series.sha256` and will refuse a patch that no longer applies, so re-base the set (or cherry-pick the `sage26/core-alignment` commits) onto the new `main` in a throwaway clone first.

When SAGE26 moves (its v1.0): regenerate the T1 reference from the new SAGE26, re-stage its source for the oracle, regenerate the fixtures with `regenerate.sh` and diff them against these (a physics change shows as a changed `.out`), carry each change into the owning module on `main`, and re-measure with the comparator, core-aligned and on Mimic's own core. The claim on `main` (`models/sage26/README.md`, *Agreement with SAGE26*, and the CHANGELOG's *Claims and limits*) is then updated from the measurement.

Nothing on this branch is merged to `main`.
