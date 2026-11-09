# Shin-Uchuu ASCII Validation (this branch)

**Purpose**: Record what the `shin-uchuu/ascii-validation` branch holds, why it is not on `main`, and how to use it again when the Shin-Uchuu packages are next re-validated or the Consistent-Trees ASCII catalogue is converted again.

This branch is `main` at commit `78334f8b` (the rename of the horizontal package to `shin-uchuu-ascii-horizontal` and of the vertical L-Halo package to `shin-uchuu`) plus this document. `main` keeps the two Shin-Uchuu packages a user can run, `shin-uchuu-ascii-horizontal` (horizontal, the full cut dataset) and `shin-uchuu` (vertical L-Halo, the same cut trees), and no longer carries the vertical Consistent-Trees ASCII subset package or the gate that compares the horizontal package with it. They live here, in working context, so they can be brought back whole.

## What is here and not on `main`

| Piece | Path on this branch | What it is |
|---|---|---|
| The ASCII subset package | `simulations/shin-uchuu-ascii/` (`simulation_info.yaml`, `halo_properties.yaml`, `converter_columns.yaml`, `shin-uchuu.a_list`, `plot_profile.yaml`, README) | The vertical `consistent_trees_ascii` package over a subset of the catalogue: 8,000,198 tree roots in 6,011,205 whole forests, 406,668,896 halos (its README gives the selection and the required `forests.list`, `locations.dat` and `tree_*.dat` files). Its `simulation_info.yaml` is the one package metadata file whose `tree_type` is `consistent_trees_ascii` for Shin-Uchuu, which is what `convert_trees.py --simulation-info` requires to convert the ASCII catalogue |
| Its run files and plot profile | `models/{halos-only,sage16,sage26}/input/<model>_shin-uchuu-ascii.yaml`, `models/sage16/plots/profiles/shin-uchuu-ascii_plot_profile.yaml` | The shipped runs of the subset package |
| The subset gate | `simulations/shin-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py` | Compares the vertical `shin-uchuu-ascii` run over the subset with the horizontal run over a version 3 conversion of exactly that subset, for `halos-only` and `sage16` with fixed and dynamic timesteps; all four legs passed bitwise on the subset migrated from its version 2 conversion (the horizontal package's README, *The subset gate*, records the counts). It skips while `MIMIC_SHIN_UCHUU_SUBSET_DATASET` is unset and fails if that is set without `MIMIC_SHIN_UCHUU_SUBSET_MANIFEST` |
| The fixture's original regeneration inputs | `simulations/shin-uchuu-ascii-horizontal/_tests/data/regenerate.sh` and `_tests/integration/test_schema_conformance.py`, as they stand here | Both read `shin-uchuu-ascii`'s metadata and converter profile, which is how the committed `worked_ascii` fixture and the pinned `column_mapping_sha256` were made |

## Why it left `main`

The package cannot be run from a clone. Its subset is defined by machine-local `forests.list`/`locations.dat` files drawn from the 11.61 TB catalogue held on OzSTAR, which no clone has; the full uncut box can never run vertical (one percolating forest holds a third of the tree roots); and a random sample of forests is not a defined volume, so the subset is no science product. Its value was evidence. `main` still gates each half of what the subset gate showed, separately: the Consistent-Trees ASCII route by `micro-uchuu-ascii` against `micro-uchuu-ascii-horizontal` over the complete micro-Uchuu catalogue, and Shin-Uchuu's two drivers by the reverse-direction gate of `shin-uchuu` against `shin-uchuu-ascii-horizontal` over the cut subset. The ASCII reader against a version 3 conversion at Shin-Uchuu scale is now recorded evidence on `main` (the horizontal package's README, the validation skill's gate table), re-runnable from this branch.

## External dependencies

- The subset's ASCII files (`forests.list`, `locations.dat`, the 2,744 `tree_*.dat`), on the Mac at `/Volumes/Internal/data/uchuu/shin-uchuu/shin-uchuu-ascii`, the target of the package's `snapshots` symlink.
- For the gate: a version 3 conversion of the subset and its conversion manifest, named by the two environment variables above. `convert_trees.py` makes both from the subset files, as the package README's *Converting the subset to horizontal HDF5* describes; the manifest is that conversion's `<workdir>/manifest.json`.
- For a reconversion of the catalogue: the full Shin-Uchuu Consistent-Trees ASCII catalogue on OzSTAR.

## Using it against a later `main`

The pieces are files `main` no longer has, so they restore onto a working branch without conflict. The gate builds its worktrees at `HEAD` and refuses run files that are not committed there, so commit the restored files on that branch before running it (the `snapshots` symlink is gitignored and stays local).

If `main` has changed since this branch was cut, bring the restored files into line with it before committing them, since the gate fails if they disagree: the package's shared metadata (cosmology, particle mass, the `unique_galaxy_id_multiplier` of 2×10¹⁰, the `halo_properties.yaml` fields) with `shin-uchuu-ascii-horizontal`'s, and each restored run file with `main`'s `<model>_shin-uchuu-ascii-horizontal.yaml`, from which it may differ only in `simulation.name` and `output.output_directory`:

```bash
git switch -c shin-uchuu/revalidate main
git checkout shin-uchuu/ascii-validation -- simulations/shin-uchuu-ascii \
    models/halos-only/input/halos-only_shin-uchuu-ascii.yaml \
    models/sage16/input/sage16_shin-uchuu-ascii.yaml \
    models/sage26/input/sage26_shin-uchuu-ascii.yaml \
    models/sage16/plots/profiles/shin-uchuu-ascii_plot_profile.yaml \
    simulations/shin-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py
# bring the restored files into line with main (above) and git add those edits, then:
git commit -m "Restore the shin-uchuu-ascii subset package and its gate"
ln -s /Volumes/Internal/data/uchuu/shin-uchuu/shin-uchuu-ascii simulations/shin-uchuu-ascii/snapshots
MIMIC_SHIN_UCHUU_SUBSET_DATASET=<dataset> MIMIC_SHIN_UCHUU_SUBSET_MANIFEST=<manifest.json> \
    make MODEL=halos-only SIMULATION=shin-uchuu-ascii-horizontal tests-scientific
```

To convert the ASCII catalogue again, pass `--simulation-info simulations/shin-uchuu-ascii/simulation_info.yaml` to `convert_trees.py` as `convert/mimic-convert/README.md` describes for the `shin-uchuu-ascii` route on this branch.

Nothing on this branch is merged to `main`.
