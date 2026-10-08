# Mimic Convert: Merger-Tree Converters

**Purpose**: The manual for `convert/mimic-convert/`, the offline tools that turn forest-ordered (vertical) merger trees into the snapshot-ordered horizontal HDF5 input Mimic's horizontal driver reads. It sits beside `plot/mimic-plot/` as Mimic's other standalone tool; most Mimic users never need it, because the shipped horizontal packages already point at converted data.

The converters are standalone: they never touch Mimic source, simulation packages, run files or `snapshots/` symlinks, they open source data read-only, and every deletion they perform is of a manifest-owned intermediate they created under their own workdir. The on-disk contract they write is [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md), which lives in this directory.

**The route, in one paragraph.** `convert_trees.py` is the current tool. You point it at a source (L-Halo binary, Consistent-Trees forests-HDF5 or Consistent-Trees ASCII) and at the simulation package that describes it, then run six subcommands over one workdir: `inspect` resolves the package's profile against the source and estimates the output without writing anything; `ingest` freezes the configuration and streams verified canonical chunks; `transpose` sorts them per snapshot and remaps every link to 64-bit snapshot-local indices; `write` emits one `snapshot_NNN.h5` per snapshot plus `forests.h5`; `validate` runs the independent version 3 producer battery; and `report` writes `conversion_report.{json,txt}`. Every stage is restartable, and nothing is emitted that the battery has not checked.

There are two entry points, and they write different format versions:

| Entry point | Sources | Output | Runnable by the current Mimic? |
|---|---|---|---|
| `convert_trees.py` (current) | L-Halo binary, Consistent-Trees forests-HDF5, Consistent-Trees ASCII | horizontal-HDF5 **format version 3**, lossless, per [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md#version-3) | **The format, yes; a route, only where gated.** The reader and driver consume version 3; the parity-gated routes are listed in the [spec table](HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support). Full Uchuu is not claimed |
| `convert_ctrees.py` (legacy) | Consistent-Trees ASCII | horizontal-HDF5 **format version 2**, per [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md) | **Yes.** This is how the Shin-Uchuu production dataset was produced, and why the tool is kept (`micro-uchuu-ascii-horizontal` is now a version 3 conversion by `convert_trees.py`'s `consistent_trees_ascii` route) |

**Conversion is not route validation.** A successful `convert_trees.py` run proves the dataset says exactly what its source says. Mimic's `horizontal_hdf5` reader and horizontal driver consume version 3, so a conforming dataset can be read, but a route is supported only where a recorded parity gate has shown its horizontal output bitwise identical, per `UniqueGalaxyID`, to the vertical reader of the same source format over the same files; each gate is `simulations/<package>/_tests/scientific/test_cross_format_identity.py`, and each package's README records its result. Running one also needs a simulation package whose every declared field matches the dataset's `/schema` (type, units and `h_convention`); undeclared `/schema` fields are validated and ignored. Because declared units are the source format's own, that means one package per simulation and source format; the shipped packages happen to declare every payload field their profile selects. The six evidenced routes (mini-Millennium, Millennium, mini-Uchuu, and micro-Uchuu from L-Halo, from forests-HDF5 and from Consistent-Trees ASCII) are recorded, with their packages, coverage, gated models and measured results, in the [V3 Runtime Support table](HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support) of the format specification, which is the one place that list is kept.

Nothing else is claimed. **Full Uchuu is not claimed**: it exceeds whole-slab memory, and although Mimic's chunked sweeps (`input.forest_chunks`) now bound a run's memory by a chunk, full Uchuu stays unclaimed because it is storage-bound before it is memory-bound, and its largest forest bounds any chunk. Every `convert_trees.py` stage prints a `runtime support:` line naming these routes, and every conversion report for a measured version 3 dataset opens with `FORMAT CONSUMED BY THE CURRENT MIMIC; ROUTE NOT VALIDATED BY THIS CONVERSION` (an unmeasured or non-version-3 dataset opens with its own warning), so a green conversion cannot be mistaken for a validated route. The report's `runtime_compatibility.runnable_by_current_mimic` flag means *format consumed* — `true` when every emitted file declares format version 3 — never *route validated*.

## Requirements

Python 3.9+, `numpy`, `pandas`, `h5py`, `PyYAML` — all installed into `mimic_venv` by `pip install -r requirements.txt` from the repository root. Run every command below from the repository root.

## Converting trees with `convert_trees.py` (format version 3)

Figures quoted in this section were measured on complete real conversions of mini-Millennium, micro-Uchuu, Millennium and mini-Uchuu, unless stated otherwise; the figures taken before the whole Millennium and mini-Uchuu simulations were converted say so.

### Commands

Six subcommands, run in order over one `--workdir`. The mini-Millennium sequence:

```bash
C="mimic_venv/bin/python convert/mimic-convert/convert_trees.py"
S=simulations/mini-millennium
W=output/convert/mini-millennium-v3

# Read-only: resolve the profile against the inventory; counts, layout, widths,
# estimated output size. Writes nothing.
$C inspect --source-format lhalo_binary \
    --simulation-info $S/simulation_info.yaml --a-list $S/mini-millennium.a_list \
    --column-map $S/converter_columns.yaml --halo-properties $S/halo_properties.yaml \
    --source-dir $S/snapshots --tree-name trees_063 --first-file 0 --last-file 7

# Freeze the configuration into the workdir and stream verified canonical chunks.
$C ingest --workdir $W --source-format lhalo_binary \
    --simulation-info $S/simulation_info.yaml --a-list $S/mini-millennium.a_list \
    --column-map $S/converter_columns.yaml --halo-properties $S/halo_properties.yaml \
    --source-dir $S/snapshots --tree-name trees_063 --first-file 0 --last-file 7

$C transpose --workdir $W            # per-snapshot sort + 64-bit link remapping
$C write --workdir $W --simulation-info $S/simulation_info.yaml
$C validate --workdir $W             # version 3 producer battery; exit 1 on FAIL
$C report --workdir $W               # battery + conversion_report.{json,txt}
```

The other routes change only the source format and the inventory flags in the [routes table](#routes-for-the-shipped-simulation-packages). For forests-HDF5, `--info-file` replaces `--source-dir`/`--tree-name`/`--halo-properties`, and `particle_mass` comes from `--simulation-info`. For ASCII, `--forests-list` and one `--tree-file` per file (in inventory order) replace the file range; `--pool-size` and `--chunksize` tune the ASCII preparation.

- **`--source-format` is never inferred.** The profile's `source_format` and the package's `input.tree_type` must both agree with it. Without `--column-map` the format's shipped default profile (`convert/mimic-convert/profiles/<source_format>.yaml`) is used and the output says so; a named profile that fails is fatal, never replaced by the default. An option meant for another format is rejected, never ignored.
- **`write` needs `--simulation-info` again** and refuses one whose content differs from what `ingest` pinned, so a dataset's header values cannot silently come from another package. What it does **not** check is that the named package matches the source data itself: mini-Millennium and Millennium share `tree_name` and particle mass, so pairing one package's metadata with the other's files would convert self-consistently and wrongly. Name the matching package.
- **`validate`/`report` default `--multiplier` to 10⁹**, the identity multiplier the header bounds are checked against; pass the package's `unique_galaxy_id_multiplier` when it differs.
- The emitted dataset is `<workdir>/write/attempt_NNN/` (`snapshot_NNN.h5` for every a_list snapshot, empty ones included, plus `forests.h5`). `report` writes `conversion_report.json`/`.txt` into the workdir, including link-gap counts, index bounds, the measured `links_adjacent`, the runtime-compatibility section (format capability, the evidenced routes and this dataset's own limits), and a consumer payload-metadata fragment explicitly marked incomplete for runtime use.

### Routes for the shipped simulation packages

Every package carries its own profile, `simulations/<package>/converter_columns.yaml`. A profile selects source fields and, for L-Halo, describes the whole on-disk record; it never changes a run YAML, `halo_properties.yaml` or `snapshots/` symlink.

| Package | `--source-format` | Inventory flags | Files declared / present locally | Evidence recorded |
|---|---|---|---|---|
| `mini-millennium` | `lhalo_binary` | `--source-dir`, `--tree-name trees_063`, `--halo-properties`, `--first-file 0 --last-file 7` | 8 / 8 | real, complete: 1,533,122 halos |
| `millennium` | `lhalo_binary` | as above, `--first-file 0 --last-file 511 --memory-budget-mb 16384` | 512 / 512 | real, complete: 760,667,000 halos |
| `micro-uchuu` | `lhalo_binary` | `--tree-name Uchuu100_Planck_lhalo_binary`, files 0–3 | 4 / 4 | real, complete: 22,580,924 halos |
| `mini-uchuu` | `lhalo_binary` | `--tree-name Uchuu400_Planck_lhalo_binary`, `--first-file 0 --last-file 127 --memory-budget-mb 32768` | 128 / 128 | real, complete: 1,451,359,554 halos |
| `uchuu` | `consistent_trees_hdf5` | `--info-file <dir>/mergertree_info.h5 --first-file 0 --last-file 1999` | 2000 / 0 | **committed fixture only** (`simulations/uchuu/_tests/data/mergertree_info.h5`, 6 halos); production never converted |
| `micro-uchuu-hdf5` | `consistent_trees_hdf5` | `--info-file <dir>/MicroUchuu_mergertree_info.h5 --first-file 0 --last-file 0` | 1 / 1 | real, complete: 22,580,924 halos (13 GB, ExternalLink-backed) |
| `micro-uchuu-ascii` | `consistent_trees_ascii` | `--forests-list <dir>/forests.list --tree-file <dir>/tree_0_0_0.dat` | 1 / 1 | real, complete: 22,580,924 halos |

**The `uchuu` row's inventory flags are the production shape, not what the fixture evidence ran.** The committed fixture has one file, not 2,000, so reproducing that evidence means narrowing `--first-file`/`--last-file` to what is actually present: `--info-file simulations/uchuu/_tests/data/mergertree_info.h5 --first-file 0 --last-file 0`. Copy-pasting the row's production flags against the fixture fails, since files 1–1999 do not exist there.

**Name the range you hold.** `simulation_info.yaml`'s `first_file`/`last_file` is the package's whole catalogue, not a request. The requested range must lie inside it, and every requested file must exist: a copy of Millennium that stops at file 15 cannot be asked for file 16. On the `ingest` route the dependency pin runs before the adapter is even built, so that is the error a user actually sees: `source dependency .../trees_063.16 cannot be pinned: [Errno 2] No such file or directory` (`conversion_manifest.py`, `pin_dependency`). A direct adapter caller (`inspect`, or the adapter's own inventory build) instead sees `requested source file .../trees_063.16 (ordinal 16) is missing; a conversion fails rather than narrowing to the files that are present`. A partial conversion must be a range starting at file 0 if its identities are to match a whole-simulation conversion's; changing an earlier inventory member changes every later `SourceHaloID` and `ForestIndex`.

**Three kinds of evidence, kept separate:**

- **Conversion capability** — all three adapters exist, are covered by the converter test suite, and convert every route above.
- **Fixture evidence** — full Uchuu was converted only from its committed six-halo ExternalLink fixture.
- **Production evidence** — complete real conversions of mini-Millennium and micro-Uchuu in all three source formats, each compared halo by halo, link by link and bit by bit against an independent C dump through Mimic's own vertical reader and against independent Python source extraction; and complete conversions of the whole Millennium (760,667,000 halos) and mini-Uchuu (1,451,359,554 halos) simulations, each passing the producer battery. The independent dump comparison was run on those two only over their files 0–15, before the whole simulations were converted; over the whole simulations the comparison between the vertical and horizontal readers is the package's runtime parity gate. **No full-Uchuu production conversion has been performed**: the ≈37 TB source is not mounted, and at the measured ≈141.67 B/halo its output alone is ≈25.7 TB before working storage (an extrapolation from the package README's ≈181.5 × 10⁹ halos, which is itself unverified).

### What it preserves

Version 3 exists so that conversion is a change of layout, not of content:

- **Topology.** All five links, as stored by the source, with their chain order. L-Halo and forests-HDF5 chains are carried exactly as stored; ASCII chains are the existing reference reconstruction (host fix-ups, no `fix_flybys`). Nothing is regenerated by mass.
- **Gaps.** A descendant link that skips snapshots is a valid forward gap, recorded as a gap. The format stores every link as an int64 row index **plus** an int32 target-snapshot column (`DescendantSnapshot`, `FirstProgenitorSnapshot`, `NextProgenitorSnapshot`), and the header's `links_adjacent` is measured over the whole dataset. No halo is inserted to close a gap and no gapped link is dropped. Real mini-Millennium carries 29,291 gapped descendant links (maximum span 2) and converts with all of them intact.
- **Native units and precision.** Each payload field keeps the adapter's own storage type and units, declared per file in `/schema`: L-Halo `M_Crit200` stays float32 in `1e10 Msun/h`; Consistent-Trees mass is float32 `Msun/h`. The converter never round-trips a value through another unit basis, so payloads compare bit for bit against the source's own vertical reader.
- **Source identity.** Every row carries a positive, globally unique int64 `SourceHaloID`, the prefix sum of source halo counts over the declared inventory order: for L-Halo and forests-HDF5 the units are a tree or `ForestInfo` row in (file ordinal, unit ordinal) order with rows in source order, and the id inverts exactly back to its physical source row; for ASCII the unit is the whole forest, in `ForestIndex` order with rows in `HaloRankInForest` order, and the id inverts to (`ForestIndex`, `HaloRankInForest`), not to a physical row. On every route it is therefore the halo's 1-based position in (`ForestIndex`, `HaloRankInForest`) order of the complete catalogue (a sampled prelinked conversion keeps its parent's positions, so its ids have gaps), and every slab is grouped by forest. `ForestIndex` and `HaloRankInForest` follow the selected source representation's own convention (for L-Halo: file-prefix tree number and original within-tree row), so `UniqueGalaxyID` is **source-relative** — equal to the vertical reader's for the same source format, and not promised equal across source formats. `MostBoundID` is carried as signed catalog data, never used as a key; duplicates are legal.
- **Width.** Links, offsets, counts and remapping keys are int64 throughout the converter, so a snapshot above `INT32_MAX` halos is representable. Overflow aborts before anything is narrowed or written. This has been exercised with synthetic and sparse fixtures above 2³¹ rows and 2⁵³ keys; no real dataset of that width has been converted.

Rows within a snapshot are in ascending `SourceHaloID`, a storage order and not a progenitor priority, so row *i* of a version 3 file is not row *i* of a version 2 file from the same source. Compare the two by identity, never by position.

### Selecting additional fields

Any numeric source field can be carried into the output by adding an entry under `extra_fields` in a profile, with no code change. The grammar, validation rules and the digest that identifies a selection are in [`profiles/README.md`](profiles/README.md). A minimal L-Halo example, taken from `profiles/lhalo_binary_extras_example.yaml`:

```yaml
extra_fields:
  - name: M_Mean200
    sources:
      - field: M_Mean200
    type: float
    units: 1e10 Msun/h
    h_convention: carried
    description: Mass within the radius enclosing 200x the mean matter density
  - name: PosX
    sources:
      - field: Pos
        component: 0
    type: float
    units: Mpc/h
    h_convention: carried
    description: First component of the stored position vector, selected on its own
```

Types are `int`, `long long`, `float`, `double`, `vec3_int` and `vec3_float`; integers never pass through floating point, and each extra keeps its declared precision and native units. Changing a selection, a type, a unit or a description changes the profile's `column_mapping_sha256`, which every output file records. On real data, six mini-Millennium extras and three micro-Uchuu forests-HDF5 extras agreed bit for bit with independent source extraction over every row. A unit outside `scripts/generate_properties.py`'s registry converts, but a Mimic simulation package cannot declare it until that unit is added there.

### Restart, cleanup and memory

- **Restart.** Every stage records its state in `<workdir>/manifest.json` (`conversion_manifest.py`, manifest version 3), which embeds the full schema, digest, adapter identity, dtype descriptors, source inventory including every HDF5 file an ExternalLink references, and artifact checksums. `ingest` resumes at chunk granularity and proves every already-registered chunk is still exactly what the source yields before appending, so a resume cannot duplicate or drop a halo or change a `SourceHaloID`. `transpose` and `write` are all-or-nothing per attempt; an interrupted attempt's directory is discarded before the next begins. A resumed `ingest` needs only `--workdir`, because it reads the embedded configuration, not the profile file; one that names a configuration must name the recorded one exactly, and a same-width schema substitution or a changed source dependency is refused before anything is touched. `--pool-size` and `--chunksize` are tuning, not configuration: they are recorded under the manifest's `tuning` key outside the configuration digest, so a resume may change them, and the values a completed `ingest` actually used stay recorded. A version 3 manifest written before tuning was recorded separately (that is, by the converter before 2026-09-28) is refused by name rather than resumed; such a conversion is started again in a fresh workdir. Re-running a completed stage re-verifies only that stage's own artifacts — `validate` is the check of the whole dataset. That proof has a cost at scale: a resumed `ingest` re-hashes every registered chunk from disk and re-reads the source from its start, re-serialising and hashing every batch to compare, so a crash late in a very large ingest costs roughly two passes over the written chunks plus a full source re-read before new chunks are appended (`DEFAULT_SAVE_EVERY_CHUNKS` bounds only how many unregistered chunks are lost).
- **Keep Finder out of the workdir.** The converter refuses a stage directory that holds anything it did not write, and macOS writes a `.DS_Store` into any folder open in a Finder window, which failed a mini-Uchuu `transpose` after 2 h 2 min; the stage then reruns from the start. Do not browse the workdir or the spill directory while a conversion runs.
- **Cleanup** is opt-in and irreversible for the workdir: `transpose --consume-ingest` and `write --consume-transposed` delete verified, manifest-owned intermediates once their successor is verified. Source files are never touched, and there is no release, transfer or batch mode for these routes.
- **Memory.** `--memory-budget-mb` (default 2048) is a ceiling on each budgeted working-buffer term — adapter reads and validation, the per-unit inventory, the ASCII rank pass and the transpose's external sort — each checked before it is allocated; a conversion whose terms cannot fit is refused rather than run. It does not bound total interpreter RSS, and concurrently resident terms are not summed. Measured with the default budget, the largest stage peaked at 2.38–4.82 GiB RSS across the real routes. The per-unit inventory costs 640 B per tree and must fit under the budget, so a source with many trees needs a larger one: the default is refused for the whole Millennium (14,329,882 trees, 9.2 GB) and mini-Uchuu (25,843,142 trees, 16.5 GB) simulations, which were converted with `--memory-budget-mb` 16384 and 32768 and peaked at 23.3 and 41.4 GiB RSS in `transpose`. The budget bounds working buffers rather than choosing the output: the same four Millennium files converted with 2048 and with 16384 MiB gave byte-identical output (65 of 65 files). `validate`/`report` take their own `--memory-budget-mb` (default 256) and `--spill-dir`; the whole-simulation conversions used 8192 for `report`.
- **Storage.** Logical output is exactly 140 B/halo for the default schema on every adapter; on disk it is 141.67–337.22 B/halo, the excess being HDF5's whole-chunk allocation, which dominates small snapshots (measured before 2026-09-29, when every chunk held 65,536 rows; a chunk now holds at most the dataset's row count, so small snapshots cost less and the upper figure overestimates). Without consumption the workdir after `write` measured 434–447 B/halo on files 0–15 and 433 B/halo on the whole Millennium (329 GB) and mini-Uchuu (628 GB) simulations, plus a transient ≈467 B/halo transpose spill (355 GB and 678 GB measured) and a battery spill in `--spill-dir` of 147 GB and 286 GB. Size the volume from those figures, not from the output size alone: the `transpose` peak, ingest chunks plus spill plus the transposed output so far, is the binding term.

### Independent comparison tooling

`convert/mimic-convert/tests/run_generalisation_acceptance.py` is the harness the real-data evidence above was produced with: `build-dump`, `dump`, `convert`, `exec`, `compare` and `compare-extras` (each documented by its own `--help`), each able to append a measured entry (command, exit code, wall and CPU time, peak RSS, input identities) to a `--record` file. `compare` checks a version 3 dataset, its `SourceHaloID` included on every route, against the C dump harness's source-coordinate mode; `compare-extras` checks every selected extra against independent Python extraction, joining on the catalogue id (`SnapNum`, `MostBoundID`) for ASCII and on `SourceHaloID` for the L-Halo and forests-HDF5 routes. Its ASCII extractor streams each tree in `--block-rows` slices, counting rows in a first pass and keeping no tree whole, so its memory follows `--block-rows` rather than the largest forest; the 3.83 GiB peak once measured for real micro-Uchuu ASCII predates that change and describes the earlier whole-tree extractor.

The C harness `tests/unit/tools/dump_ctrees_topology.c` has two modes. Its default is the version 1 topology dump the version 2 cross-check consumes, described under [Reference-topology proof](#reference-topology-proof); `--source-payload` writes `mimic-source-dump v1`, keyed by `(ForestIndex, HaloRankInForest)` and carrying every link, target snapshot and core payload field, for any vertical reader. The tool is valid only for the simulation it was built for, so build one per package into its own directory:

```bash
TOPOLOGY_DUMP_BUILD_DIR=/scratch/dump-mini-millennium \
    MODEL=halos-only SIMULATION=mini-millennium tests/unit/tools/build_topology_dump.sh
/scratch/dump-mini-millennium/dump_ctrees_topology --source-payload <run_file> <dump_path>
```

The run file must declare `output_format: binary` (see [Reference-topology proof](#reference-topology-proof) for why).

## Legacy: Consistent-Trees ASCII to version 2 with `convert_ctrees.py`

`convert_ctrees.py` writes `format_version = 2` from Consistent-Trees ASCII only. It is kept, unchanged in its defaults, because a version 2 dataset is still in use: the Shin-Uchuu production dataset (`simulations/shin-uchuu/`) was produced with it. `micro-uchuu-ascii-horizontal` is now a version 3 conversion by `convert_trees.py`'s `consistent_trees_ascii` route. For any new conversion, use `convert_trees.py` above. The on-disk output contract is the version 2 part of [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md).

**Status.** Validated end to end on the real micro-Uchuu ASCII tree: a fresh default conversion reproduces 22,580,924 halos, 50 populated snapshots, 440,651 forests and `max_halo_rank_in_forest = 350074`, the producer validation battery passes all 15 checks, and the cross-check against a Mimic `halos-only` reference run passes all eight checks with zero unexplained mismatches — the seven always-run checks (`reference-sanity`, `identity-forest`, `identity-creation`, `fof-central`, `mostboundid-positive`, `values`, `occupancy`; `CHECK_NAMES` in `crosscheck.py`) plus the chain-order check `topology-chains`, which runs only when `--reference-topology` is passed and compared links, `ForestIndex`/`HaloRankInForest` and `MostBoundID` over a complete dump of all 22,580,924 halos. `fix_flybys` is not part of the conversion: it collapsed independent FoF groups at each forest's final snapshot, which is why version 1 was withdrawn (see the format specification's [Versioning Policy](HORIZONTAL-HDF5-FORMAT.md#versioning-policy)), and a fixture forest with two independent FoF groups surviving as self-central at its maximum snapshot guards against its return.

The pipeline has five phases: Phase 0–1 `scatter` (forest map, then per-snapshot scratch binaries), Phase 2 `sort` (per-snapshot sort and id index), Phase 3 `fixups` (adjacency validation, spin and `Len` conventions, the `fix_upid` equivalent) and `links` (FoF chains, descendant and progenitor links, within-forest ranks, identity fields), and Phase 4 `write` (HDF5 emission), followed by `report` and the standalone battery and cross-check.

### Usage

The commands below are the micro-Uchuu examples, one per phase. They write to `output/convert/micro-uchuu`, which is right for a 22.6-million-halo, 2.4 GB conversion and wrong for a large one: size the workdir's volume from the [storage figures](#storage-per-stage) first. For a conversion at production scale, with batch mode and every flag it needs, the worked example is [`simulations/shin-uchuu/README.md`](../../simulations/shin-uchuu/README.md#how-the-production-dataset-was-made).

```bash
# Phase 0 + 1: forest map, scatter ctrees files into per-snapshot scratch binaries
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py scatter \
    --workdir output/convert/micro-uchuu \
    --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml \
    simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat

# Phase 2: per-snapshot sort by halo id + id index
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py sort \
    --workdir output/convert/micro-uchuu

# Phase 3 steps 1-3 and 5 (step 4, fix_flybys, removed in v2): adjacency
# validation, spin/Len conventions, fix_upid equivalent (reference semantics)
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py fixups \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml

# Phase 3 steps 6-9: FoF chains, descendant/progenitor links, within-forest
# ranks, identity fields (always all snapshots — FirstProgenitor flows forward
# through a per-snapshot pending buffer). --memory-budget-mb bounds the rank
# pass's working set (default 2048); it trades memory against spill I/O and
# changes no emitted value
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py links \
    --workdir output/convert/micro-uchuu

# Phase 4: emit snapshot_NNN.h5 + forests.h5 per convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md
# (one file per a_list snapshot, including empty ones; default <workdir>/hdf5)
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py write \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml

# Consumptive deletion (off by default; see "Consumptive deletion of
# intermediates" below). Add --consume-intermediates to fixups, links and write
# when the workdir cannot hold every intermediate at once. IRREVERSIBLE.
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py fixups --consume-intermediates \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml

# Producer validation battery (standalone; non-zero exit on any failure;
# --manifest is required — count conservation against the independent
# pre-counts is a mandatory part of the battery)
mimic_venv/bin/python convert/mimic-convert/validate.py output/convert/micro-uchuu/hdf5 \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --manifest output/convert/micro-uchuu/manifest.json

# Conversion report (runs the battery, writes conversion_report.{json,txt};
# exits 1 if validation failed). --multiplier defaults to 1e9, which is correct
# for micro-Uchuu (max_halo_rank_in_forest = 350074); pass the package's
# unique_galaxy_id_multiplier as an integer when it is larger
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py report \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list

# Cross-check vs a halos-only reference run: 'prepare' writes a scratch run
# file listing all snapshots, 'run-reference' captures the run log + exit
# code, 'compare' runs the check
mimic_venv/bin/python convert/mimic-convert/crosscheck.py prepare \
    --run-file models/halos-only/input/halos-only_micro-uchuu-ascii.yaml \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list
mimic_venv/bin/python convert/mimic-convert/crosscheck.py run-reference \
    --mimic ./mimic --run-file output/convert/micro-uchuu/reference_run.yaml \
    --log output/convert/micro-uchuu/reference_run.log
mimic_venv/bin/python convert/mimic-convert/crosscheck.py compare \
    output/convert/micro-uchuu/hdf5 output/convert/micro-uchuu/reference-output \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml \
    --reference-topology output/convert/micro-uchuu/topology.dump  # optional, see below
```

Canonical metadata comes from explicit `--simulation-info` and `--a-list` paths, keeping the converter simulation-agnostic. Observed `(SnapNum, scale)` pairs from the data are cross-validated against the a_list (absolute tolerance 1e-4; an unknown pair aborts the run).

**The order is enforced by the code, not by convention.** `write` refuses any snapshot whose recorded status is not `linked`, and `sort` → `fixups` → `links` are what carry a snapshot to that status, so a `write` issued before `links` fails outright. Every flag is per invocation and carries no manifest state: an omitted flag is silent, and must be repeated on every invocation (every batch, in batch mode) that needs it.

**Emitting to a final data location.** The commands above use `write`'s default output directory, `<workdir>/hdf5`. To place a dataset somewhere permanent instead, pass `write --output-dir <dir>` and emit there directly — do **not** move the files afterwards. The manifest records the emitted paths, and the battery's `manifest-binding` check compares the directory against them, so a post-hoc `mv` breaks validation. `report` reads the dataset location from `manifest["outputs_dir"]`, so it validates the real destination with no extra argument; `validate.py` and `crosscheck.py compare` take the dataset directory as their positional argument, so pass the destination in place of `<workdir>/hdf5` in those two commands.

### Workdir layout

```text
<workdir>/
  manifest.json            resume manifest: source files (size/mtime/md5, independent
                           pre-count, parsed count, per-snapshot counts and id checksums),
                           every intermediate the converter created, snapshot status
  forest_max_snap.npy      per-forest max-snapshot table (Nx2 int64: forest_id, max snap)
  forest_index_table.npy   dense ForestIndex -> ctrees forest id (ascending forest id);
                           emitted as forests.h5 by the Phase 4 writer
  conversion_report.json   durable conversion report (totals, per-snapshot counts,
  conversion_report.txt    identity bounds, observed pairs, validation outcomes,
                           recommended identity multiplier)
  hdf5/
    snapshot_NNN.h5        emitted dataset, one file per a_list snapshot (empty
                           snapshots included), per convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md
    forests.h5             /ForestID sidecar (dense ForestIndex -> ctrees forest id)
  scratch/
    snap_NNN.bin           concatenated per-snapshot records (always deleted after sort verifies)
    snap_NNN_sorted.bin    records sorted by ascending halo id
    snap_NNN.idx           sorted int64 id array for Phase 3 merge-joins
    snap_NNN_fixed.bin     fixed records (120-byte dtype: frozen fields + Len +
                           MostBoundID; Jx/Jy/Jz now carry normalised Spin)
    snap_NNN_links.bin     link/identity records (36-byte dtype: Descendant,
                           FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup,
                           NextHaloInFOFgroup int32; ForestIndex,
                           HaloRankInForest int64), row-aligned with the fixed file
    snap_NNN_pending_fp.bin pending FirstProgenitor buffer for snapshot NNN
                           (int32, written while snapshot NNN-1 is resident)
    links_identity_*/      transient per-invocation rank/identity scratch: the
                           external merge sort's spill runs plus two int64
                           arrays (ForestIndex, HaloRankInForest) indexed by
                           global position, 8 B/halo each. Created by the links
                           stage itself and removed by it on both the success
                           and the failure path, so it is never a manifest
                           intermediate. Removal is attempted, not guaranteed:
                           if it fails the stage keeps ownership, retries, and
                           warns naming the directory and its size
    roots_src_I.npy        observed #tree roots per source file
    forest_max_src_I.npy   per-file forest max-snapshot aggregates
```

Scratch records use the frozen 108-byte packed little-endian dtype defined in `ctrees_parser.py` (`RECORD_DTYPE`); the manifest records the dtype tag and refuses to resume across a dtype change.

Re-running `scatter` skips source files whose manifest entry is complete and unchanged (size + mtime), so a crashed run resumes where it stopped. A source entry is `completed` or, in batch mode, `consumed`; `deferred` and `pending` are classified per run from the inventory plus what is on disk and are deliberately never written to the manifest, so a file that arrives later needs no state cleared. Per-file conservation — the pandas-independent row pre-count must equal the parsed and scattered row count exactly — is enforced before a file is recorded as complete. The manifest is bound to its input identities (a_list, forests.list, and the ordered source set are checksummed at first run); changing any of them, or changing a source file after snapshots were finalized, refuses to resume — use a fresh workdir. Every intermediate is verified against its registered content checksum before it is consumed, skip-trusted, or deleted, and non-finite input values (NaN/inf, or float64 values that overflow float32) abort the parse.

### Consumptive deletion of intermediates

Five of the intermediates above are retained by default — the sorted, index, fixed, links and pending-buffer files — so the workdir holds all five for every snapshot at once, a measured 277 bytes per halo on micro-Uchuu. (The other two are not flag-gated: `snap_NNN.bin` is deleted once sort verifies its successors, and the link stage removes `links_identity_*/` itself, on its success and its failure path alike; that removal is *attempted* rather than guaranteed, so one that does not leave the directory absent keeps the stage's ownership, is retried and is warned about, never assumed.) `--consume-intermediates` deletes each one at the point its **terminal** consumer is finished with it. It is **off by default and irreversible**: turning it on trades resumability for storage, and is only worth doing when the volume cannot hold the full set.

The flag is per invocation and belongs on `fixups`, `links` and `write`. With it off, those stages delete nothing they do not already delete. The emitted dataset is bitwise identical either way.

| Intermediate | Terminal consumer | Deleted by | Point of deletion |
|---|---|---|---|
| `snap_NNN.bin` (concatenated) | `sort` | `sort` | after the sorted file and index verify — **always**, flag or no flag |
| `snap_NNN_sorted.bin` | `fixups` | `fixups` | once snapshot N's fixed output is verified and registered |
| `snap_NNN.idx` | `link_one_snapshot(N-1)` | `links` | once snapshot **N−1** is linked; an index whose predecessor is not a recorded snapshot has no consumer at all and goes as soon as linking starts |
| `snap_NNN_pending_fp.bin` | `link_one_snapshot(N)` | `links` | once snapshot N is linked |
| `snap_NNN_fixed.bin` | the **writer** | `write` | once snapshot N's emitted HDF5 is verified and recorded |
| `snap_NNN_links.bin` | the **writer** | `write` | once snapshot N's emitted HDF5 is verified and recorded |
| `links_identity_*/` | the link stage itself | `links` | on both the success and the failure path, flag or no flag; never a manifest intermediate. A removal that fails is retried and warned about, not silently assumed |

Two of those consumer relations are easy to get wrong, and both are load-bearing. The fixed file is read **twice** — by `links` through `_load_fixed` and by the writer through `_load_snapshot_scratch` — so the writer, not `links`, is its terminal consumer; the same holds for the links file. And `snap_NNN.idx` is read by the link of the snapshot *below* it, to resolve descendants, so the highest snapshot's index is consumed perfectly normally while `snap_000.idx` is the one with no consumer.

The mechanics follow the converter's delete-after-verify discipline throughout: the successor is re-read from disk and verified, registered in the manifest, and the manifest saved, *before* the predecessor is removed; the removal itself goes through `Manifest.remove_intermediate`, which re-checks ownership, workdir containment and the registered content checksum, and the manifest is saved again. `remove_intermediate` is the only place in the converter that unlinks a manifest-owned intermediate, so no deletion here can bypass that guard. A crash between the unlink and the second save leaves an entry recorded `present` with no file on disk; the next run converges it to `removed` and continues, in either flag state, because those bytes are already gone.

**What consumption costs you.** Deletion is bounded by re-run reachability, not by last read. Which re-runs stay reachable depends on the snapshot's recorded status, so the promises below are stated per status rather than in general — a skip that is claimed more broadly than it holds is worse than no claim at all.

- **`sort`** does the work at `concatenated`, skips at `sorted` and `fixed`, and skips at `linked` **only when consumptive deletion has actually taken one of that snapshot's artifacts** — with nothing consumed it refuses a `linked` snapshot exactly as it always did, so a flag-off workdir behaves as before. Its own two outputs — the sorted file and the index — are verified outright at `sorted`, where neither consumer can have run yet, and are accepted as consumed only at `fixed` and `linked`. The fixed file does not exist yet at `sorted`, must still be on disk at `fixed` — where it is verified outright, because the writer that consumes it runs only once *every* snapshot is linked — and is accepted as consumed only at `linked`, where the links file joins it on the same terms. An artifact that merely went missing, or whose checksum moved, is still the hard error it has always been at every status.
- **`fixups`** does the work at `sorted`, skips at `fixed`, and skips at `linked` under the same consumption gate, on the same split: strict verification of the fixed file at `fixed`, fixed and links accepted as consumed at `linked`.
- **`links`** requires every snapshot at `fixed` or `linked`. When they are all `linked` and **any** fixed input is recorded consumed it skips, because the rank pass streams **every** fixed file and so cannot run again once even one is gone — an interrupted writer is enough. Before skipping it verifies every links output still on disk, then drains any deletion an earlier flag-off run or an interrupted writer left undone, so turning the flag on late still reclaims the indexes and pending buffers. Only while **every** fixed input is present does the pass run as before, including its refuse-not-repair comparison of the run-scoped identity values.
- **`write`** skips a snapshot whose emitted file is already recorded and unchanged, and needs no scratch to do it.
- **A `links` run interrupted part-way still resumes**: an index or pending buffer is deleted only after the snapshot that reads it has been linked.
- **`finalize`, and a non-batch `scatter`** (which finalizes at the end), skip a snapshot at `concatenated`, `sorted` or `fixed`, accepting a consumed sorted file or index — the same helper `sort` uses, so the two cannot drift apart. **They do not handle a snapshot at `linked`**: such a snapshot falls through to the concat path, which needs the per-source worker scratch that finalization itself deleted, and the run aborts naming that file. This is not a consequence of consumptive deletion — it reproduces exactly with the flag off. The rule it implies is simply: **once `links` has run, do not re-run `finalize` or a non-batch `scatter` on that workdir.**

One rule decides every "accepted as consumed" above: **an artifact is accepted as consumed exactly at the statuses where its terminal consumer has provably run**, and is verified outright everywhere else. `sorted` is taken by `fixups`, which saves the `fixed` status before removing it; `idx` is taken by `links`, which will not start until every snapshot is at least `fixed`; `fixed` and `links` are taken by the writer, which runs only once every snapshot is `linked`. A manifest claiming a consumption earlier than that describes a premature deletion, and is refused rather than skipped.

**The supported sequences**, then, are the documented order — `scatter` → `release` → `finalize` → `sort` → `fixups` → `links` → `write` → `report` — and a re-run of any stage from `sort` onward at any point after it, with one exception the bullets above already state: with nothing consumed, `sort` and `fixups` refuse a snapshot `links` has carried to `linked`, so on a **flag-off** workdir those two stages are re-runnable only before `links` has run; `links` and `write` are re-runnable in either flag state. What is not supported is re-entering `finalize` or a non-batch `scatter` after `links`, for the reason above.

**What it forecloses.** Once a stage's inputs are gone that stage cannot be re-executed, only skipped — if an emitted file is later deleted or corrupted, the conversion must be re-run from the last surviving stage, and if nothing survives, from the source. That is the whole cost, and it is why the flag is opt-in.

`release` is unaffected either way: it verifies the per-source sidecars and worker scratch, none of which are in the table above, and still refuses a source whose own intermediates finalization has deleted.

### Storage per stage

Measured stage by stage on micro-Uchuu (22,580,924 halos), peak workdir in bytes per halo; the peaks are a 1 Hz sampler's maximum, so the fast stages carry a few B/halo of run-to-run jitter and the figures are the maximum observed.

| Stage | flag OFF, B/halo | flag ON, B/halo |
|---|---:|---:|
| scatter (incl. finalize concat) | 111.95 | 111.96 |
| sort | 119.67 | 119.67 |
| fixups | 236.99 | 131.40 |
| links | **300.99** | **192.99** |
| terminal workdir intermediates | 276.99 | **0.99** |
| emitted dataset | 107.87 | 107.87 |
| **peak total (workdir + emitted)** | **384.86** | **192.99** |

`write` and `report` set no new peak. Emitted bytes per halo fall as snapshots grow, because small snapshots carry proportionally more HDF5 chunk overhead; the Shin-Uchuu README's production envelope shows how these per-halo terms project to a much larger dataset.

### Batch mode: the interleaved consumptive transfer

For a source too large to stage locally in one piece, `scatter --batch` supports the cycle `transfer batch → scatter → release → delete → transfer next batch`, resuming correctly at every step, with `finalize` last of all — every batch must be released before the conversion is finalized. Batch mode is off by default and changes nothing outside itself; inside it, the two different reasons a source file can be legitimately absent are told apart:

| State | Meaning | Effect |
|---|---|---|
| `deferred` | in the frozen inventory, bytes not transferred yet | skipped for now, not an error; the run scatters what has arrived and exits **without finalizing** |
| `consumed` | scatter completed, intermediates verified, bytes released by `release` | satisfies resume without being re-stat-ed or re-scattered; its recorded identity (size, mtime, md5, counts, checksums, observed pairs) stays the frozen record of what was processed |

Rules the cycle depends on:

- **Pass `--pool-size` on every batch-mode `scatter`.** It defaults to `1` and the flag is per invocation with no manifest state, so an omission on any one batch scatters that batch serially and is not detectable afterwards from the manifest — the emitted intermediates are identical either way. See [Scatter parallelism](#scatter-parallelism---pool-size).
- **The complete ordered inventory is frozen once, at first run, and every batch-mode invocation must supply all of it** through the positional `tree_files` argument. Passing only the subset currently on disk changes the frozen set and is refused. There is no new index artifact and no second copy of the list. "First run" includes a batch-mode scatter issued *before any bytes have arrived* — it scatters nothing, reports every entry as deferred, and still writes the frozen inventory and the metadata identities to the manifest, so the very next invocation is already guarded.
- **Batch mode never finalizes, and release must come before finalize.** Finalization deletes the worker intermediates a later `release` has to verify, so finalizing when the last batch completes would make that batch impossible to release — and that is enforced, not merely advised: `release` **refuses** a source whose intermediates finalization has already deleted. Once they are gone the rows live in the concatenated snapshot, which the sort stage deletes in turn, so there is no artifact the release path could verify instead; releasing anyway would authorize deleting irreplaceable source bytes with nothing checked. Finalization is reachable only through `finalize`, which refuses to run while any entry is deferred. Outside batch mode `scatter` still finalizes automatically.
- **Consumption is an explicit operator action, never inferred from a missing file.** A `completed` entry whose bytes are gone but which was never released is an error naming the file: nothing verified that its intermediates survived. `release` is the way out (it verifies the intermediates, not the source bytes, so it still works once the bytes are gone). The converter never deletes source data itself.
- **`release` refuses** an entry that is not `completed`, an entry already `consumed`, a source whose on-disk size/mtime no longer match what was scattered, any registered intermediate that does not verify, and any source-owned intermediate that finalization has already deleted (release before finalizing, not after — including after a finalization that was interrupted part-way). Nothing is skipped: every intermediate the source produced is verified, or the release is refused. It is atomic across the files it is given, so a refusal on any of them leaves the persisted manifest untouched.
- Root-coverage validation at finalize reads each source file's observed roots from its registered sidecar, so it still sees every file's roots when no source byte is left on disk.
- A conversion driven this way emits a dataset byte-identical to a single all-at-once run, and a manifest identical in provenance and every per-source content field; only the lifecycle state differs (`consumed` versus `completed`).

### Scatter parallelism: `--pool-size`

**`scatter` is the only subcommand that takes `--pool-size`, and it defaults to `1`.** At `1` — or whenever at most one source file is pending, whatever the flag says — `run_scatter` takes the serial branch and parses the inventory one file at a time in the parent process. Only above `1` *and* with more than one file pending does it build a `multiprocessing.Pool` of `min(pool_size, len(pending))` workers, each loading its own copy of the forest map once through the `_init_scatter_worker` initializer rather than receiving it pickled per task. The flag changes nothing that is emitted: the dataset and the manifest's per-source content fields are identical whichever branch runs, and a resumed run may use a different value from the one that was interrupted.

The default suits a single-file source such as micro-Uchuu's one `tree_0_0_0.dat`, which cannot use a pool at all. It is the wrong default for a many-file source. Measured over 2,744 `tree_*.dat` files (210.57 GB of data, one run each), scatter ran at **46.8 MB/s serial against 71.9 MB/s at `--pool-size 8`**; 8 is the only pooled value measured, so choose yours against the host's core count and the volume's read bandwidth. For comparison, the single-file micro-Uchuu scatter measured 91.3–92.0 MB/s over four warm repeats (plan against the slowest), CPU-bound on the parse path rather than on the pool.

### Memory and scale of each stage

Every stage streams, so peak memory follows the **largest snapshot** rather than the total halo count. Figures below are labelled with the scale they were measured at: *micro-Uchuu scale* is 22,580,924 halos; *rehearsal scale* is a 406,668,896-halo whole-forest subset of Shin-Uchuu (`simulations/shin-uchuu-ascii/`), whose largest snapshot holds 9,006,294 halos. When repeated runs spread and the cause has not been measured, plan against the highest figure observed.

- **Scatter** loads the forest map once per worker process through a `Pool` initializer, and saves the manifest on a bounded interval (`save_every_n_files`, default 25) rather than after every source file; a per-file rewrite grows quadratically with file count, since the manifest reaches ≈38 KB per source file.
- **Fix-ups** keeps the satellite chain resolution as a sequential per-satellite scan doing reference-order in-place rewrites, because exact `fix_upid` parity is load-bearing; it measured ≈1.28 µs per satellite at rehearsal scale.
- **The rank pass (`links`)** ranks through the external merge sort in `rank_sort.py` under `--memory-budget-mb`, keeps `(ForestIndex, HaloRankInForest)` in on-disk arrays, holds only the adjacent snapshot pair it is linking, and verifies identity with one bit per halo. At rehearsal scale and the default 2 GiB budget it peaked at 9.76–10.01 GB RSS over three runs, against 4.55 GB at micro-Uchuu scale — 2.1–2.2× the memory for 18× the halos. Transient spill and identity arrays go to disk under the workdir.
- **The producer validation battery** streams the emitted dataset, holding one snapshot for the per-snapshot checks and the adjacent pair for progenitor closure, and settles identity with a one-bit-per-halo bitset. It peaked at 0.374–0.399 GB RSS at micro-Uchuu scale and 3.245–3.258 GB at rehearsal scale (three runs each), with a 50.8 MB bitset and nothing spilled at rehearsal scale.
- **The topology cross-check** walks snapshots in ascending order holding one snapshot of each side, keeps the cross-snapshot `UniqueGalaxyID` suppression set as a disk-backed sorted union, and partitions the reference-topology dump by snapshot on disk. It peaked at 3.61–3.62 GB RSS at micro-Uchuu scale; at rehearsal scale three runs gave 10.09 GB (cold) and 15.84–16.56 GB (warm), a spread that is measured and unexplained, so plan against 16.56 GB. Both on-disk structures live under `TMPDIR`: set it to a volume with room for roughly 72 bytes per dumped halo before passing `--reference-topology` on a large dataset.
- **The cross-check does not scale to a production catalogue**, because its reference side is a vertical `halos-only` run over the same data, and the Consistent-Trees ASCII reader preallocates room for 1,000 halos (≈152,000 B) per tree before it reads a forest (`load_unit_ctrees_ascii` in `src/io/vertical/read_ctrees_ascii.c`). The Shin-Uchuu super-forest's 104,845,278 tree roots would need ≈15.9 TB for that alone. Run it at micro-Uchuu scale, or on a whole-forest subset built with [`subset.py`](#building-a-subset-of-a-very-large-dataset); no cross-check artifact belongs in a production storage envelope.
- **Not locked:** concurrent converter invocations on one workdir.

### Production-scale runs

The largest conversion this tool has run is the Shin-Uchuu production catalogue: 2,744 source files, 11.61 TB of Consistent-Trees ASCII, 22,503,649,037 halos. Its full command sequence — batch-mode scatter with `--pool-size`, `release` per batch, `finalize`, consumptive deletion on `fixups`, `links` and `write`, `write --output-dir`, and `report --multiplier` — together with its storage envelope and the per-snapshot memory term, is recorded as a worked example in [`simulations/shin-uchuu/README.md`](../../simulations/shin-uchuu/README.md#how-the-production-dataset-was-made). Start there for any conversion that will not fit comfortably on one volume.

## Building a subset of a very large dataset

`subset.py` selects a tractable, representative **whole-forest** subset of a ctrees dataset far too large to convert in one pass, and extracts it byte-exactly. It never reads the bulk tree data: forests are ranked from `forests.list`, `locations.dat` and a `stat` size inventory, then one root row is read per candidate tree, then only the selected byte ranges are copied. `simulations/shin-uchuu-ascii/` is a subset built this way.

Stages alternate between the analysis machine and the machine holding the data, because root-row sampling needs the source bytes and those must not be transferred in bulk. `subset.py` is **numpy-only** for exactly this reason — the data node need not have pandas.

```bash
# 1. local: per-tree and per-forest tables + the top-M candidate pool by byte extent
mimic_venv/bin/python convert/mimic-convert/subset.py plan-candidates \
    --index <dir with forests.list, locations.dat, filesizes.tsv> --out <work> --m <M>

# 2. on the data node: one root row per candidate (ship candidates.npy AND filemap.json)
mimic_venv/bin/python convert/mimic-convert/subset.py sample-roots \
    --candidates <work>/candidates.npy --filemap <work>/filemap.json \
    --trees <tree dir> --a-list <scale factor list> --out <work>/root_values.npy

# 3. local: tractability gates, strata, file-coverage closure, selection manifest
mimic_venv/bin/python convert/mimic-convert/subset.py finalize \
    --tree-table <work>/tree_table.npy --forest-table <work>/forest_table.npy \
    --candidates <work>/candidates.npy --root-values <work>/root_values.npy \
    --filemap <work>/filemap.json --out <work>/selection \
    --target-trees <n> --k <supplement size> --seed <fixed>

# 4. on the data node: stream the selected ranges out, then verify before transferring
mimic_venv/bin/python convert/mimic-convert/subset.py extract \
    --selection <work>/selection --trees <tree dir> --out <subset dir>
```

**Whole forests, always.** `fix_upid` works with per-forest scope, so a partial forest converts differently from the same forest in a full run. When a source file would otherwise contribute no selected tree — which `read_locations()` will not tolerate, since it asserts file ids are contiguous from 0 and that the file count is a perfect cube — the gap is closed by adding the smallest complete forest touching it, never a lone tree, iterating until closed.

**`extract` verifies before you pay for the transfer**, and the verification is not optional: body md5s against the source ranges, marker placement, the rewritten count line, that no body contains a `#tree` marker and every body ends on a newline (an extent off by even one byte is caught here rather than by the converter after transfer), and one-to-one root coverage in the emitted index files.

**`calibrate-proxy` is not a production step.** Byte extent is a *proxy* for root mass, and the recovery fraction that measures how good a proxy it is needs the true top-`K` forests over every tree. Run it on a calibration dataset small enough to sample exhaustively (`plan-candidates --m 0`, then `sample-roots`), and carry the calibrated relative depth to the production dataset as `M = ceil(depth × n_trees)`.

Exit codes: **0** success; **1** the run completed but a `finalize` acceptance assertion or an `extract` verification failed; **2** fatal — a violated invariant, bad input, or an unreadable artifact.

## Reference-topology proof

The seven always-run checks of the version 2 cross-check (`CHECK_NAMES` in `crosscheck.py`) establish identity, rank, and central resolution by matching galaxies to halos. They do not, by themselves, directly compare the *order* of the converter's `FirstProgenitor`/`NextProgenitor`/`NextHaloInFOFgroup` chains against another implementation reading the same source data — rank equality constrains the underlying sort but does not prove chain construction.

`tests/unit/tools/dump_ctrees_topology.c` closes that gap: a read-only harness that loads a Consistent-Trees-ASCII package through Mimic's own `consistent_trees_ascii` reader (the same reader code the converter's algorithm mirrors) and dumps every halo's link fields, by stable ctrees id, to a plain-text file. Build it with:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-ascii dump-ctrees-topology-tool
tests/unit/tools/build/dump_ctrees_topology <run_param_file> <output_dump_path>
```

**The run file must declare `output_format: binary`.** `build_topology_dump.sh` compiles `-DHDF5` into only three sources — `io/vertical/registry.c`, `io/vertical/hdf5.c`, and `io/vertical/read_ctrees_hdf5.c` — so `src/core/read_parameter_file.c` is built without it and its `#ifndef HDF5` guard rejects `output_format: hdf5` with `OutputFormat 'hdf5' requires HDF5 support`. The harness exits 1 before creating the dump file. This is purely a compile-flag consequence of the harness's deliberately minimal source set: the harness links no output writer and would never have written galaxies anyway. `crosscheck.py prepare` inherits whatever the source run file declares, and every committed `halos-only` run file uses `hdf5`, so the prepared `reference_run.yaml` cannot be passed to the harness directly. Copy it and override only the output format and directory. This cannot change the dumped topology: the harness reads only the `input`/`simulation` configuration, and emits every halo of every forest tagged with that halo's own `SnapNum` — it never consults the output snapshot list at all.

```bash
mimic_venv/bin/python - <<'PY'
import pathlib, yaml
w = pathlib.Path("output/convert/micro-uchuu")
d = yaml.safe_load((w / "reference_run.yaml").read_text())
d["output"]["output_format"] = "binary"
d["output"]["output_directory"] = str(w / "topology-scratch-output")
(w / "topology_run.yaml").write_text(yaml.safe_dump(d))
PY
```

Diff the parsed YAML of the two files afterwards and confirm `output_format` and `output_directory` are the only differences, so the harness is provably reading the same tree as the reference run.

The dump format is three header lines (format marker, column names, NA-sentinel value) followed by one row per halo: `forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id next_fof_id`, all fields int64, with the NA sentinel (`INT64_MIN`) marking "no link". Nothing else may appear: a `#` line after the header means a malformed dump (two runs concatenated, a re-run appended with `>>`) and is rejected rather than skipped. The harness exits non-zero if it could not write the dump completely, so a full disk cannot produce a short dump that looks finished.

Pass the dump to `crosscheck.py compare --reference-topology <dump>` to run the additional `topology-chains` check. It first asserts **coverage** — the dump must name every converter halo exactly once at every snapshot, with no duplicate `|MostBoundID|` — because without that the check would compare cleanly over whatever subset a truncated dump happened to contain and report `PASS`. Then, per halo, it compares:

- the five **links** (`Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup`), resolving each converter link index to an id via the target snapshot's ascending-`|MostBoundID|` order and comparing it against the dump's own recorded id — the chain-**order** proof;
- the two **identity** fields (`ForestIndex`, `HaloRankInForest`), which extends rank conformance from `identity-creation`'s first-appearance subset to every halo, including halos that never seed a galaxy;
- the halo's own `MostBoundID`, since matching is by magnitude and the `mostboundid-positive` check asserts only positivity, not the exact value, over the matched Type 0/1 population.

Failures are reported as one counted summary line per (snapshot, field) with example ctrees ids, never one line per halo.

## Module map

All paths are relative to `convert/mimic-convert/`.

| File | Role |
|---|---|
| `convert_trees.py`  | generic CLI (format version 3): `inspect` / `ingest` / `transpose` / `write` / `validate` / `report` over any of the three source formats |
| `inspect_sources.py` | read-only source inspection: per-source/per-snapshot counts, link-span summary, identity bounds, available fields, source dependencies |
| `column_schema.py`  | mapping-profile grammar and validation, the canonical schema, `column_mapping_sha256`, and the consumer payload-metadata fragment |
| `adapters/`         | one adapter per source format (`lhalo_binary.py`, `ctrees_hdf5.py`, `ctrees_ascii.py`) streaming canonical batches with source-keyed topology; `base.py` is their shared contract, `source_inventory.py` the read-only inventory and dependency pinning, `topology.py` the whole-unit structural validation and batch assembly shared by the L-Halo and forests-HDF5 adapters |
| `profiles/`         | shipped default and worked-example mapping profiles; grammar in [`profiles/README.md`](profiles/README.md) |
| `transpose.py`, `source_keys.py` | bounded per-snapshot sort, source-key joins and 64-bit link remapping with independent topology closure checks |
| `conversion_manifest.py`, `pipeline.py` | manifest version 3 stage state (schema, inventory, dependencies, checksums) and the resumable ingest/transpose/write orchestration |
| `runtime_routes.py` | the evidenced version 3 routes every `convert_trees.py` stage and report prints, mirroring the format specification's V3 Runtime Support table |
| `errors.py`         | `ConverterError`, the converter's single fatal error type |
| `convert_ctrees.py` | legacy ASCII-to-version-2 CLI: per-phase subcommands |
| `ctrees_parser.py`  | frozen record dtype; indexed/`#fields:` header dialects; `#tree` marker tracking; chunked reads; independent pre-count |
| `scatter.py`        | Phase 0 forests.list map + dense ForestIndex; Phase 1 scatter/concat; resume manifest; cleanup containment guard |
| `sort_index.py`     | Phase 2 per-snapshot sort + id index; verify-then-delete |
| `fixups.py`         | Phase 3 steps 1–3 and 5 (step 4, `fix_flybys`, was removed in v2): a_list adjacency validation; spin `J/Mvir` and Len conventions; `fix_upid` reference equivalent |
| `rank_sort.py`      | external merge-sort rank core shared by the version 2 rank pass and the version 3 transpose |
| `links.py`          | Phase 3 steps 6–9: FoF chains, descendant merge-join, progenitor chains (literal `assign_mergertree_indices` insertion semantics), within-forest ranks, identity fields |
| `hdf5_writer.py`    | Phase 4: `snapshot_NNN.h5` + `forests.h5` emission per the frozen version 2 contract (empty snapshots included; write-verify-record); shared header-metadata loading and naming helpers |
| `hdf5_writer_v3.py` | `HorizontalV3Writer`: the version 3 emission the pipeline's `write` stage runs — one file per a_list snapshot plus `forests.h5`, write-verify-record, and the write-time binding to the recorded `simulation_info` and particle mass |
| `validate.py`       | version 2 producer validation battery (standalone CLI) and the format-version dispatch: structural conformance, all six format invariants, progenitor round-trip closure, FoF chain walk, identity density, header bounds, count conservation vs the independent pre-counts |
| `validate_v3.py`    | the bounded 20-check version 3 battery that `convert_trees.py validate` runs — exact object and schema set, header agreement across files, source-key coverage, every link target and chain closure, identity density, field finiteness, positions inside the header box, count conservation and manifest binding — with hand-typed format tables independent of the writer |
| `report.py`         | conversion report emission (`conversion_report.{json,txt}`) including battery outcomes and the recommended identity multiplier; the version 3 report adds source format, mapping, link-gap counts, index bounds and the runtime-compatibility section (format consumed, evidenced routes, limits) |
| `subset.py`         | whole-forest subset selection and byte-exact extraction from a very large ctrees dataset, driven entirely from the index files: `plan-candidates` / `sample-roots` / `calibrate-proxy` / `finalize` / `extract` |
| `crosscheck.py`     | seven-check cross-check vs a halos-only reference run (`reference-sanity`, `identity-forest`, `identity-creation`, `fof-central`, `mostboundid-positive`, `values`, `occupancy` — matching by \|MostBoundID\|, identity decode, FoF central, positive `MostBoundID`, bit-exact values, occupancy predicate), an optional eighth `topology-chains` check against a reference-topology dump (coverage, links, identity, ids), + reference-run plumbing |
| `HORIZONTAL-HDF5-FORMAT.md` | the normative on-disk contract both writers conform to |
| `tests/`            | stdlib-unittest suite; synthetic fixture generator (`fixtures.py`); mock reference builder (`mock_reference.py`); the real-data comparison harness (`run_generalisation_acceptance.py`); committed golden fixtures under `tests/data/` |

## Tests

The converter suite runs with `make tests-converter`, which is also how `make tests` and CI run it:

```bash
make tests-converter                                             # the converter suite, as CI runs it
mimic_venv/bin/python -m unittest discover -s convert/mimic-convert/tests -v   # the same suite directly
```

The suite covers both entry points. The comparison harness's own self-tests (`test_generalisation_acceptance.py`) plant defects — dropped and duplicated rows, wrong target snapshots, reordered chains, changed values — and require the comparator to catch each one.

The dump harness's own format test lives with the simulation package it reads, not under `convert/mimic-convert/tests/`: `simulations/micro-uchuu-ascii/_tests/integration/test_topology_dump_format.py`, part of `make SIMULATION=micro-uchuu-ascii MODEL=halos-only tests-integration`.

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [docs/VISION.md](../../docs/VISION.md): architectural principles and design boundaries
- [docs/USER-GUIDE.md](../../docs/USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [docs/STYLE-GUIDE.md](../../docs/STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- [HORIZONTAL-HDF5-FORMAT.md](HORIZONTAL-HDF5-FORMAT.md): the horizontal-HDF5 on-disk contract these tools write
- [profiles/README.md](profiles/README.md): the mapping-profile grammar
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
