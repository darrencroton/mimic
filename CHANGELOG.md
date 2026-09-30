# Mimic Changelog

**Purpose**: Record what each release of Mimic changes for users and developers, what it claims with evidence, and what it does not claim.

Versions are git tags; `RunProperties/Version` in every HDF5 output and `metadata/` in every run directory record the exact build (from the first post-v1.2 build, the release name as well as the commit). There was no v1.1 release.

---

## Unreleased

- **Outputs name their release.** `RunProperties/Version/version` in every HDF5 output, `version` in the run-local `metadata/version_info.json`, and the startup banner carry the build's `git describe --tags` string: the tag name for a release build (`v1.2`), or the tag plus its distance and commit for a later build (`v1.2-3-g23da5ae9`, with `-dirty` when the tree had uncommitted changes). The commit, branch and dates are recorded as before; the attribute is additive, so `hdf5_format_version` stays at `1.2`. A build from a checkout without tags (a shallow CI clone) records the commit hash instead. The header is written by one script, `scripts/generate_git_version.sh`, for the Makefile and both test build scripts, which previously carried their own copies of the recipe.

---

## v1.2 (2026-09-30)

Mimic v1.2 adds a second processing driver and an input format for it. The **horizontal driver** processes merger trees one whole snapshot at a time, in increasing time order, alongside the original **vertical driver**, which processes one forest at a time. Horizontal input is a per-snapshot HDF5 format that Mimic reads through the `horizontal_hdf5` reader and that a new converter tool, `convert/mimic-convert/`, produces from the forest-ordered formats Mimic already reads. Both drivers share one processing model, one property system and one output writer, and the horizontal driver has been shown to reproduce the vertical driver's galaxy catalogue bit for bit on every route listed under Evidence below.

The release also carries a number of correctness fixes that change output relative to v1.0. Read "Changes that alter output" before comparing catalogues across versions.

### New capabilities

- **Horizontal processing.** `input.processing_order: horizontal` selects the horizontal driver; the default `vertical` is unchanged and every v1.0 run file still works. Startup validation rejects a reader and driver that do not match. Horizontal runs are HDF5-only, serial (`NTask == 1`) and do not support `--skip`; each restriction is rejected at configuration with a message that says so. See the User Guide, "Running Horizontal Input".
- **Horizontal HDF5 input format, versions 2 and 3.** The on-disk contract is `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`. Version 2 carries adjacent links only. Version 3 is lossless for any source Mimic reads: descendant links may skip snapshots, row indices are 64-bit, and each file's `/schema` group declares the source's native units and precision, which the reader checks against the simulation package at startup. Version 1 datasets are rejected outright (see below).
- **The merger-tree converter.** `convert/mimic-convert/convert_trees.py` converts L-Halo binary, Consistent-Trees forests-HDF5 and Consistent-Trees ASCII trees to version 3 through a package profile (`simulations/<package>/converter_columns.yaml`), with a read-only source inspector, a verified restartable pipeline, an independent producer validation battery and a conversion report. The legacy `convert_ctrees.py` still writes version 2 from Consistent-Trees ASCII. The manual is `convert/mimic-convert/README.md`, and the Developer Guide has a short orientation section.
- **Dynamic timestep scheme.** `TimestepScheme: dynamic` (default `fixed`, unchanged) sets the number of substeps per snapshot interval from the central halo's dynamical time, bounded by `MaxDynamicSubsteps` (default 200). Every HDF5 master file records `SubSteps` and `TimestepScheme` under `RunProperties`.
- **Galaxy identity across drivers.** `UniqueGalaxyID = halonr + multiplier × (forestnr_global + 1)`; the multiplier defaults to 10⁹ and can be raised per catalogue with `simulation.unique_galaxy_id_multiplier` for catalogues whose largest forest holds more than 10⁹ halos. Both drivers honour it and every HDF5 output file records the value used as `RunProperties/UniqueGalaxyIDMultiplier`.
- **Bounded retention for gapped input.** The horizontal driver keeps each snapshot's generation only until every later snapshot that one of its halos names as a descendant has been processed. The optional `input.retention_memory_ceiling_mb` stops a run before allocating a generation that would take the retained payload past the ceiling; it bounds admission, not process memory.
- **Eight new simulation packages.** Horizontal version 2: `micro-uchuu-horizontal`, `shin-uchuu`. Horizontal version 3: `mini-millennium-horizontal`, `micro-uchuu-lhalo-horizontal`, `micro-uchuu-hdf5-horizontal`, `millennium-horizontal`, `mini-uchuu-horizontal`. Vertical: `shin-uchuu-ascii`. Each package README gives its data provenance and, for a horizontal package, the conversion command that produced its dataset.
- **Memory profile.** The run memory profile reports peak RSS and the output-buffer, pool and galaxy terms; horizontal runs add the retention term.

### Changes that alter output

These change catalogue values or layout relative to v1.0. Each was deliberate, measured and baselined.

- **Eight core halo properties are double precision.** `Mvir`, `deltaMvir`, `CentralMvir`, `Rvir`, `Vvir`, `infallMvir`, `infallVvir` and `infallVmax` are written as float64 (float32 in v1.0). The change fixed a float comparison that froze an orphan galaxy's `Rvir` and `Vvir`, and two float locals in `sage16` reincorporation. The `sage16` physics baseline was regenerated; aggregate stellar, cold-gas, hot-gas, black-hole, cooling and heating totals on mini-Millennium agree with the v1.0 baseline to within 0.01 to 0.22 per cent.
- **`fix_flybys` removed from the Consistent-Trees ASCII reader.** The step demoted every FoF central except the most massive at each forest's final snapshot and merged unrelated groups; on Shin-Uchuu it put 33 per cent of z = 0 galaxies in one group. Runs over `consistent_trees_ascii` input change their z = 0 central and Type classification and agree with the `consistent_trees_hdf5` reader on population, Types and `MostBoundID` sets, as measured at z = 0 on micro-Uchuu when the fix landed. A pre-existing float32-level precision difference between the two readers (about 2 × 10⁻⁸ relative in `Mvir`) remains and is documented in the micro-Uchuu package READMEs.
- **Horizontal format version 1 rejected.** Version 1 datasets were written with `fix_flybys` applied. The reader accepts versions 2 and 3 only; any version 1 dataset must be regenerated.
- **Uchuu-family particle mass corrected** from 0.0325 to 0.0327 (10¹⁰ Msun/h) in `micro-uchuu`, `micro-uchuu-ascii`, `micro-uchuu-hdf5`, `mini-uchuu` and `uchuu` (the horizontal micro-Uchuu packages, new in this release, ship the corrected value). Mass-derived quantities (`Len`, the subhalo-mass fallback) shift by 0.6 per cent; topology is unchanged.
- **`Spin` is labelled as specific angular momentum** (`Mpc/h km/s`) in every package's metadata. Values are byte-identical to v1.0; only the label and its unit dimension changed.
- **HDF5 output layout.** `TotHalosPerSnap` is int64; `RunProperties/Version/hdf5_format_version` reads `1.2`. A horizontal run writes one partition file per requested output snapshot (`model_<snapnum>.hdf5`, zero-padded to three digits) plus the master, and its per-snapshot groups carry no `Ntrees` attribute or `TreeHalosPerSnap` dataset.
- **Unit checking.** Run-file scalar parameters whose declared units are dimensionally incompatible with the parameter's reference dimension are now rejected at startup.
- **Plot profiles.** List-form `xlim: [a, b]` overrides for the mass-function figures were silent no-ops in every shipped profile and are replaced by scalar `xmin`/`xmax`, so those figures' axes change. Evolution figures no longer skip when their first sampled snapshot is empty.

### Fixes

- The per-phase event buffer grows on demand; the fixed 4096-slot buffer aborted large runs.
- HDF5 detection in the Makefile works under GNU Make 4.x; `make` resolves Python from `mimic_venv` without activating it.
- Consistent-Trees readers stop with a clear error, instead of narrowing silently, when a forest or partition exceeds the `int` range.
- HDF5 version and run-property string attributes were written from literals shorter than their fixed 128- or 1024-byte string type, so the writer read past the end of each literal (an out-of-bounds read found by AddressSanitizer in every run; the bytes on disk were unaffected). Every fixed-width string attribute now goes through a zero-padded buffer. The unit-test build honours `EXTRA_CFLAGS` and `EXTRA_LDFLAGS`, so a sanitizer build instruments the C tests as well as the executable.

### New configuration surface

Run-file keys (all optional, defaults preserve v1.0 behaviour): `TimestepScheme`, `MaxDynamicSubsteps`, `input.processing_order`, `input.retention_memory_ceiling_mb`, `simulation.unique_galaxy_id_multiplier`. Make targets: `tests-converter`, `check-horizontal-fixture`, `tests-horizontal-v3`, `dump-ctrees-topology-tool`; `make tests` runs the first two. Build hooks: `EXTRA_CFLAGS` and `EXTRA_LDFLAGS` (see the Developer Guide for sanitizer builds). CI runs a second job over the committed version 3 fixtures on every push and pull request to `main`.

### Evidence

Every parity claim below compares the horizontal driver's HDF5 catalogue against the vertical driver's over the same source files, per `UniqueGalaxyID`, byte for byte in every output field, with no tolerance and no field excluded (`scripts/compare_cross_format_identity.py`; the gates live under each horizontal package's `_tests/scientific/`). Each package README records its own route.

| Horizontal package | Source and coverage | Models and timestep schemes gated |
| --- | --- | --- |
| `micro-uchuu-horizontal` (version 2) | Consistent-Trees ASCII, complete micro-Uchuu | `halos-only` and `sage16`, fixed and dynamic |
| `mini-millennium-horizontal` (version 3) | L-Halo binary, complete mini-Millennium, 29,291 gapped links | `halos-only` and `sage16`, fixed and dynamic |
| `micro-uchuu-lhalo-horizontal` (version 3) | L-Halo binary, complete micro-Uchuu | `halos-only`, fixed and dynamic |
| `micro-uchuu-hdf5-horizontal` (version 3) | Consistent-Trees forests-HDF5, complete micro-Uchuu | `halos-only`, fixed and dynamic |
| `millennium-horizontal` (version 3) | L-Halo binary, **files 0–15 only** | `halos-only`, fixed and dynamic |
| `mini-uchuu-horizontal` (version 3) | L-Halo binary, **files 0–15 only** | `halos-only`, fixed and dynamic |

Conversion evidence is separate from runtime evidence. `convert_trees.py` was verified halo by halo, link by link and bit by bit against each source's own vertical reader on complete mini-Millennium and micro-Uchuu data (all three source formats), and on files 0–15 of Millennium and mini-Uchuu; full Uchuu was converted only from a small committed fixture. The Shin-Uchuu production dataset (version 2, 22,503,649,037 halos) was converted and run end to end with `sage16` on a 512 GB host; its z = 0 central count and halo mass function matched their predicted targets exactly.

### Not claimed in v1.2

- Full Uchuu cannot be run: its largest snapshot cannot be held as a whole slab on any host this project has, and chunked slab streaming is not implemented.
- Nothing is claimed for whole-simulation Millennium or mini-Uchuu on the horizontal driver, and `sage16` is gated on the horizontal driver only for mini-Millennium and micro-Uchuu.
- No equality is claimed between packages built from different source formats of the same simulation.
- Multi-rank horizontal execution, `--skip` on horizontal runs, binary output from horizontal runs, snapshot-global physics modules and distributed snapshot processing are not implemented.
- No horizontal package runs the model-package test set: the `sage16` module tests read binary output, which a horizontal run cannot write, so a horizontal package's physics is covered by its parity gate instead. `micro-uchuu-hdf5-horizontal` has no committed test fixture yet, so its generic tiers skip (see `tests/README.md`).

### Developer-facing changes

- The converter lives at `convert/mimic-convert/`, parallel to `plot/mimic-plot/`, with its format specification beside it; `scripts/` holds build and developer tooling only.
- Source layout: `src/io/vertical/` (readers, formerly `src/io/tree/`), `src/io/horizontal/`, `src/core/vertical_driver.c` and `src/core/horizontal_driver.c`. `struct Halo.HaloNr` and the five tree-link roles are 64-bit through the input and driver seam.
- The property generator accepts `type: long long`; halo identity fields for horizontal input live on reader-owned slab arrays, outside the property system.
- Test framework helpers `tests/framework/parity_gate.py` and `tests/framework/schema_conformance.py` back every horizontal gate and schema test. The generic unit and integration tiers run on horizontal packages: `scripts/generate_test_inputs.py` shapes each generated run file by the package's declared `input.processing_order` (HDF5 output, no input file range), and a test that needs binary output, `--skip`, a file range or a vertical reader skips with a stated reason (`skip_if_selected_package_is_horizontal()` in `tests/framework/harness.py`; new tests take a run from `default_run_file()`). Six horizontal packages ship committed synthetic fixtures for this, each rebuilt by a `regenerate.sh` beside it: `mini-millennium-horizontal`, `millennium-horizontal`, `mini-uchuu-horizontal`, `micro-uchuu-lhalo-horizontal`, `micro-uchuu-horizontal` and `shin-uchuu`. `micro-uchuu-hdf5-horizontal` has none, and its run-file tests skip with the reason recorded in the generated manifest. Measured on the committed data, `make MODEL=halos-only SIMULATION=<package> tests-integration` gives 0 failures on all seven horizontal packages (39 to 42 before), skipping only the vertical-only tests; on the default pair the generated run files are byte-identical and the tier still gives 217 pass, 0 fail, 4 skip. CI's version 3 job also runs `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-integration`.
- `docs/dev/` is Mimic's own development pathway; nothing outside it depends on it. The four documents in `docs/` remain the documentation of record.

---

## v1.0 (2026-06-29)

First production baseline: the physics-agnostic core, runtime-configurable module pipeline, metadata-generated property system, the `sage16`, `sham` and `halos-only` model packages, the vertical readers for L-Halo binary and HDF5 and Consistent-Trees ASCII and HDF5, HDF5 and binary output with run provenance, the plotting suite and the three-tier test suite.
