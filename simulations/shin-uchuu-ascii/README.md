# Shin-Uchuu Simulation Package — Consistent-Trees ASCII (rehearsal subset)

This package runs Mimic against a **subset** of the Shin-Uchuu merger trees in Consistent-Trees ASCII format (`consistent_trees_ascii` reader). It exists to rehearse the Shin-Uchuu conversion end to end — including the tree-ordered vs. snapshot-ordered cross-format identity gate — before the full production box is converted. See [`docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`](../../docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md) for the full rehearsal design.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees readers (mirrors `micro-uchuu-ascii`'s ctrees bridge contract; see file header for the key unit difference from L-Halo binary — M_Crit200 in native Msun/h)
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998), extracted from the actual halo data rather than ctrees file headers (see the conversion plan, "a_list extraction")
- `snapshots/`: symlink to the subset ASCII tree data directory
- `_tests/`: not present (see "Maintenance notes" below)

**This package currently points at a SUBSET, not the full box.** The subset holds 8,000,198 tree roots in 6,011,205 whole forests (2.54% of the box's 315,004,242 z=0 halos, ≈416 million halos, ≈210 GB of ASCII), selected as a fixed-seed random sample of tractable forests, whose forest-size distribution is validated against the whole population, plus the top 20 forests by measured root `Mvir` within the byte-selected candidate pool. Both claims are bounded: the supplement is the most massive systems *in that pool*, not provably the global top 20 outside the excluded super-forest, and forest-size representativeness is a proxy for, not a direct test of, low halo mass. See `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`, "Subset Selection and Extraction". `simulations/shin-uchuu/` (the snapshot-ordered sibling) is converted from this same subset for the rehearsal. **This ASCII package is only ever usable on a subset**: the full 315,004,242-halo, 2744-file production box cannot be processed tree-ordered at all (see the conversion plan's memory analysis) — that limitation is this whole conversion effort's premise, not a rehearsal artifact to be later fixed.

**Data files required in `snapshots/`:**

- `forests.list` — subset forest/tree-root ids (one line per selected tree root)
- `locations.dat` — file id, byte offset, and filename for each selected tree root
- `tree_0_0_0.dat` … the full production file set, `tree_X_Y_Z.dat` for `X,Y,Z` in `0..13` (2744 = 14³ files) — the subset extraction preserves the production per-file layout and file count; every file contributes at least one selected tree, so `tree_0_0_0.dat` (declared as `input.tree_name`, used only to read the shared Consistent-Trees column header) is guaranteed present

**Setting up the snapshots symlink:**

```bash
ln -s /path/to/shin-uchuu-subset-ascii simulations/shin-uchuu-ascii/snapshots
```

`snapshots/` is machine-local and gitignored (`.gitignore` matches `simulations/*/snapshots`), so no symlink is committed with this package; create it locally after the subset has been extracted and transferred.

## Data provenance

Source: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR (login node `tooarrana`) — 2744 `tree_*.dat` files, 11.61 TB, Consistent-Trees ASCII format, 70 snapshots. Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) is the Uchuu/Planck-2015 family shared with `micro-uchuu-ascii`. The particle mass is **not** shared: Shin-Uchuu's 8.97×10⁵ Msun/h is 362× smaller than micro-Uchuu's 3.25×10⁸ Msun/h, and was confirmed for Shin-Uchuu specifically rather than carried over. Box size 140 Mpc/h.

The subset used here is extracted from that source with `scripts/convert/subset.py` (`plan-candidates` → `sample-roots` → `finalize` → `extract`), which selects whole forests without ever reading the bulk tree data, then copies only the selected byte ranges. See `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`, "Subset Selection and Extraction", for the full design and the round-trip verification already performed on real data (60,000 trees extracted and verified as a dry run of the mechanism this package's subset uses at larger scale).

## Cross-validation sibling

- `simulations/shin-uchuu/` — the snapshot-ordered HDF5 conversion of this same subset, produced by `scripts/convert/` (mirrors the `micro-uchuu-ascii` / `micro-uchuu-snapshot` pair). The cross-format identity gate (`test_cross_format_identity.py`, see the `micro-uchuu-snapshot` package for the worked reference) compares runs over these two packages snapshot for snapshot, which is why the shipped run files use the identical `output.snapshot_list` on both sides.

**Mirror maintenance:** `halo_properties.yaml` is an intentional mirror of `simulations/shin-uchuu/halo_properties.yaml` (both use the ctrees RawHalo contract, adjusted only for the 140 Mpc/h box's `Pos` range). Keep them in sync.

## Maintenance notes

- **Production re-point, not yet done.** After the full production conversion, `simulations/shin-uchuu/snapshots` is re-pointed at the production dataset (see that package's README). This ASCII package is not re-pointed the same way — it remains subset-only by design, since it cannot address the full box.
- **`Spin` range `[-1000, 1000]` is provisional** (decision D7 in the conversion plan). The rehearsal subset's Step 2b measurement is a lower bound only; nothing before a full production scan bounds the z=0 spin maximum from above. Recalibrate if the rehearsal or production run reports an out-of-range value.
- **`unique_galaxy_id_multiplier: 10000000000` (10¹⁰)** is set from the start in both this package and `simulations/shin-uchuu/`, ahead of when the production scale strictly requires it, so the rehearsal exercises the production identity multiplier end to end. See the comment in `simulations/shin-uchuu/simulation_info.yaml` for the confirmed forest-count bound and the still-open rank-count bound.
- **No `plot_profile.yaml`.** Unlike `micro-uchuu-ascii`, this package ships no plot profile: axis limits belong to a real run's dynamic range, and guessing them for a 140 Mpc/h box ahead of the rehearsal would bake in numbers nobody has checked. Add one from measured output when plotting this package matters.
- **`_tests/` is not shipped.** Unlike the micro-Uchuu exemplars, this package has no committed fixture-sized test data or integration scaffolding yet — the rehearsal subset itself is the first real exercise of this reader against Shin-Uchuu data, and fixture design should follow once the rehearsal's actual data shape is known.
