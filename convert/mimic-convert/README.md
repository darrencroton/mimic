# Mimic Convert: Merger-Tree Converters

**Purpose**: The manual for `convert/mimic-convert/`, the offline tools that turn forest-ordered (vertical) merger trees into the snapshot-ordered horizontal HDF5 input Mimic's horizontal driver reads. It sits beside `plot/mimic-plot/` as Mimic's other standalone tool; most Mimic users never need it, because the shipped horizontal packages already point at converted data.

The converters are standalone: they never touch Mimic source, simulation packages, run files or `snapshots/` symlinks, they open source data read-only, and every deletion they perform is of a manifest-owned intermediate they created under their own workdir. The on-disk contract they write is [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md), which lives in this directory.

**The route, in one paragraph.** `convert_trees.py` is the current tool. You point it at a source (L-Halo binary, Consistent-Trees forests-HDF5 or Consistent-Trees ASCII) and at the simulation package that describes it, then run six subcommands over one workdir: `inspect` resolves the package's profile against the source and estimates the output without writing anything; `ingest` freezes the configuration and streams verified canonical chunks; `transpose` sorts them per snapshot and remaps every link to 64-bit snapshot-local indices; `write` emits one `snapshot_NNN.h5` per snapshot plus `forests.h5`; `validate` runs the independent version 3 producer battery; and `report` writes `conversion_report.{json,txt}`. Every stage is restartable, and nothing is emitted that the battery has not checked.

It writes one format:

| Entry point | Sources | Output | Runnable by the current Mimic? |
|---|---|---|---|
| `convert_trees.py` | L-Halo binary, Consistent-Trees forests-HDF5, Consistent-Trees ASCII | horizontal-HDF5 **format version 3**, lossless, per [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md#version-3) | **The format, yes; a route, only where gated.** The reader and driver consume version 3; the parity-gated routes are listed in the [spec table](HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support). Full Uchuu is not claimed |

Beside it, `rewrite_trees.py` rewrites an existing version 3 dataset under a forest table (see [Rewriting a horizontal dataset](#rewriting-a-horizontal-dataset-with-rewrite_treespy)), `forest_census.py` measures a dataset's forests and builds the decided cut table, and `subset.py` selects a whole-forest subset of a very large Consistent-Trees catalogue.

**Conversion is not route validation.** A successful `convert_trees.py` run proves the dataset says exactly what its source says. Mimic's `horizontal_hdf5` reader and horizontal driver consume version 3, so a conforming dataset can be read, but a route is supported only where a recorded parity gate has shown its horizontal output bitwise identical, per `UniqueGalaxyID`, to the vertical reader of the same source format over the same files (except Shin-Uchuu, whose full catalogue cannot run vertical and whose row states its own evidence); each gate is `simulations/<package>/_tests/scientific/test_cross_format_identity.py`, and each package's README records its result. Running one also needs a simulation package whose every declared field matches the dataset's `/schema` (type, units and `h_convention`); undeclared `/schema` fields are validated and ignored. Because declared units are the source format's own, that means one package per simulation and source format; the shipped packages happen to declare every payload field their profile selects. The seven evidenced routes (mini-Millennium, Millennium, mini-Uchuu, micro-Uchuu from L-Halo, from forests-HDF5 and from Consistent-Trees ASCII, and Shin-Uchuu from Consistent-Trees ASCII) are recorded, with their packages, coverage, gated models and measured results, in the [V3 Runtime Support table](HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support) of the format specification, which is the one place that list is kept.

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
- **Cleanup** is opt-in and irreversible for the workdir: `transpose --consume-ingest` and `write --consume-transposed` delete verified, manifest-owned intermediates once their successor is verified. Source files are never touched.
- **Memory.** `--memory-budget-mb` (default 2048) is a ceiling on each budgeted working-buffer term — adapter reads and validation, the per-unit inventory, the ASCII rank pass and the transpose's external sort — each checked before it is allocated; a conversion whose terms cannot fit is refused rather than run. It does not bound total interpreter RSS, and concurrently resident terms are not summed. Measured with the default budget, the largest stage peaked at 2.38–4.82 GiB RSS across the real routes. The per-unit inventory costs 640 B per tree and must fit under the budget, so a source with many trees needs a larger one: the default is refused for the whole Millennium (14,329,882 trees, 9.2 GB) and mini-Uchuu (25,843,142 trees, 16.5 GB) simulations, which were converted with `--memory-budget-mb` 16384 and 32768 and peaked at 23.3 and 41.4 GiB RSS in `transpose`. The budget bounds working buffers rather than choosing the output: the same four Millennium files converted with 2048 and with 16384 MiB gave byte-identical output (65 of 65 files). `validate`/`report` take their own `--memory-budget-mb` (default 256) and `--spill-dir`; the whole-simulation conversions used 8192 for `report`.
- **The battery's two modes.** `validate` and `report` run the version 3 producer battery in one of two modes, which settle the same checks under the same names and failure kinds. The default, external-sort mode (`validate_v3.py`) handles every dataset, gapped or adjacent: one scan feeds four bounded external sorts held to `--memory-budget-mb`, spilling to `--spill-dir`. `--bounded` runs the bounded mode (`validate_bounded.py`) instead. It accepts adjacent datasets only — every snapshot file must declare `links_adjacent` 1, and a file that declares anything else is refused by name before any long pass (only the files' headers and dataset shapes are read first) — and it needs the conversion manifest, whose inventory sizes its `SourceHaloID` bitset. It sorts and spills nothing (`--spill-dir` is accepted and left untouched), and holds at most a ceiling it computes from the dataset's headers and the manifest, prints at start, and refuses when the ceiling exceeds `--memory-budget-mb`. Use the bounded mode for an adjacent dataset too large for the external sorts' spill or run time, a Shin-Uchuu-scale dataset in particular; use the default for datasets that declare `links_adjacent` 0 (some L-Halo conversions, Millennium's among them) and wherever its spill fits. A Shin-Uchuu dataset:

  ```bash
  mimic_venv/bin/python convert/mimic-convert/convert_trees.py validate --workdir <workdir> \
      --bounded --multiplier 20000000000 --memory-budget-mb <N> --spill-dir <LaCie path>
  ```

  (`report` takes the same options). The ceiling, in bytes, with `W` the widest snapshot's rows, `N` the dataset's halos, `H` the inventory's `total_halos`, `F` the headers' `n_forests_total`, `R` the bytes of one row of every `/halos` column and `B` = max(1, min(1,048,576, `W`)) the rows per block read (the one block size every read and window uses, the sidecar's forest rows included), is the larger of the row pass, 8`F` + `B`(4`R` + 256), and the pair pass, 64`W` + ⌈`W`/8⌉ + ⌈`H`/8⌉ + ⌈`N`/8⌉ + 8`F` + 256`B` + 2 MiB. The pair pass's 64 bytes per row of the widest snapshot are five int64 columns and two int32 counters of one snapshot and two int64 columns of the next; the fixed 2 MiB is a diagnostic workspace for the windows that count a bitset's set bits and find its first clear ones. At Shin-Uchuu's numbers (`W` 519,342,987, `N` = `H` 22,503,649,037, `F` 244,953,607 after the decided cut or 166,547,771 before) it is 41.16 GB (cut) or 40.53 GB (uncut); `python convert/mimic-convert/validate_bounded.py ceiling --widest-rows <W> --n-halos <N> --n-forests-total <F>` evaluates it, with the `--memory-budget-mb` it needs. On the two adjacent 406,668,896-halo Shin-Uchuu subset datasets (widest snapshot 9,006,294 rows) the two modes gave identical verdicts; the bounded mode printed a ceiling of 1.00–1.01 GB and peaked at 0.94–0.95 GB RSS in 172–188 s with nothing spilled, where the external-sort mode at 8192 MiB peaked at 6.89 GB RSS in 538–562 s and its spill directory reached 75.8–77.7 GB (sampled every 15 s). A dataset that declares `links_adjacent` 1 but holds progenitor links that skip a snapshot cannot pass (its `links-adjacent` or `topology-closure` check fails); the bounded mode holds such links in what the budget leaves above the ceiling, 96 B each, and stops with an error naming them when they do not fit. Like the budget of every stage, the ceiling bounds the battery's working arrays, not the interpreter's total RSS, nor the unchanged sidecar check's O(source files) per-file conservation tallies, which only a sidecar that cannot pass grows beyond the inventory's file count.

  | Obligation (checks) | Bounded algorithm |
  |---|---|
  | Manifest binding and source conservation (`manifest-binding`, `count-conservation`, `sidecar-content`) | Unchanged: the external-sort mode's own functions (registered names and SHA-256, the pinned a_list, the ingest and transpose counts, the sidecar read in blocks). |
  | Structure, headers and `/schema` agreement (`file-set`, `object-set`, `sidecar-object-set`, `schema-binding`, `header-values`, `run-scoped-headers`) | Unchanged: the external-sort mode's own functions, one file at a time. |
  | Every per-row invariant (`row-values`, `len-nonnegative`, `field-finiteness`, `position-bounds`, `link-targets`, `links-adjacent`) | Unchanged and per block: the external-sort mode's own block check on every row; the previous block's last `SourceHaloID` is carried into the next. |
  | Progenitor closure, chain membership and FoF integrity (`topology-closure`) | Per snapshot pair N−1 and N: N−1's `Descendant`, `FirstHaloInFOFgroup`, `NextProgenitor`, `NextHaloInFOFgroup` and `ForestIndex` (each link combined with its `*Snapshot` column into a global position) and two int32 in-link counters resident, with N's `FirstProgenitor` and `ForestIndex`. Every link into N−1 is judged against its target's own values and counted, as the external-sort mode's join does, a block of owners at a time. A link that does not join adjacent snapshots (only in a failing dataset) is held from the row pass and judged when its target is resident. |
  | Chain termination (`chain-cycles`) | Per snapshot, one chain pass per chain kind (`NextProgenitor`, `NextHaloInFOFgroup`) with a visited bitset: walks start at the heads (no in-link) and a halo is visited once every link into it was walked; a halo never visited lies on a cycle. The pass's state (the in-link counters and the bitset) is per snapshot, so a chain crossing a block boundary is walked whole. `NextProgenitor` links leaving a snapshot are contracted to a graph over those links alone and walked the same way. |
  | `SourceHaloID` uniqueness and the [1, total] density (`source-key-coverage`) | A one-bit-per-id bitset over the inventory's [1, `total_halos`]: an id set twice is a duplicate, a clear bit at the end a gap. |
  | The (`ForestIndex`, rank) position check and rank density (`identity`) | Per-forest halo totals (int64 per forest, from the row pass) become each forest's start position; a second one-bit-per-halo bitset indexed by start + rank catches a repeated pair, a rank at or beyond its forest's total breaks density, and `SourceHaloID` must be start + rank + 1. The ASCII forest density, the last `ForestIndex` and the header's maximum rank come from the same totals and the row pass. |
  | Identity bounds (`header-bounds`) | Unchanged. |

- **Storage.** Logical output is exactly 140 B/halo for the default schema on every adapter; on disk it is 141.67–337.22 B/halo, the excess being HDF5's whole-chunk allocation, which dominates small snapshots (measured before 2026-09-29, when every chunk held 65,536 rows; a chunk now holds at most the dataset's row count, so small snapshots cost less and the upper figure overestimates). Without consumption the workdir after `write` measured 434–447 B/halo on files 0–15 and 433 B/halo on the whole Millennium (329 GB) and mini-Uchuu (628 GB) simulations, plus a transient ≈467 B/halo transpose spill (355 GB and 678 GB measured) and a battery spill in `--spill-dir` of 147 GB and 286 GB. Size the volume from those figures, not from the output size alone: the `transpose` peak, ingest chunks plus spill plus the transposed output so far, is the binding term.

### Independent comparison tooling

`convert/mimic-convert/tests/run_generalisation_acceptance.py` is the harness the real-data evidence above was produced with: `build-dump`, `dump`, `convert`, `exec`, `compare` and `compare-extras` (each documented by its own `--help`), each able to append a measured entry (command, exit code, wall and CPU time, peak RSS, input identities) to a `--record` file. `compare` checks a version 3 dataset, its `SourceHaloID` included on every route, against the C dump harness's source-coordinate mode; `compare-extras` checks every selected extra against independent Python extraction, joining on the catalogue id (`SnapNum`, `MostBoundID`) for ASCII and on `SourceHaloID` for the L-Halo and forests-HDF5 routes. Its ASCII extractor streams each tree in `--block-rows` slices, counting rows in a first pass and keeping no tree whole, so its memory follows `--block-rows` rather than the largest forest; the 3.83 GiB peak once measured for real micro-Uchuu ASCII predates that change and describes the earlier whole-tree extractor.

The C harness `tests/unit/tools/dump_ctrees_topology.c` loads every forest through Mimic's own vertical reader and writes `mimic-source-dump v1` (its one mode, named by the required `--source-payload` flag), keyed by `(ForestIndex, HaloRankInForest)` and carrying every link, target snapshot and core payload field, for any vertical reader. The tool is valid only for the simulation it was built for, so build one per package into its own directory:

```bash
TOPOLOGY_DUMP_BUILD_DIR=/scratch/dump-mini-millennium \
    MODEL=halos-only SIMULATION=mini-millennium tests/unit/tools/build_topology_dump.sh
/scratch/dump-mini-millennium/dump_ctrees_topology --source-payload <run_file> <dump_path>
```

The run file must declare `output_format: binary`. `build_topology_dump.sh` compiles `-DHDF5` into only the vertical HDF5 reader sources, so `src/core/read_parameter_file.c` is built without it and its `#ifndef HDF5` guard rejects `output_format: hdf5` with `OutputFormat 'hdf5' requires HDF5 support`; the harness then exits 1 before creating the dump. The harness links no output writer and reads only the run file's `input` and `simulation` configuration, so the output format and snapshot list cannot change what it dumps.

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

## Forest census and the decided cut table with `forest_census.py`

`forest_census.py` measures how a horizontal-HDF5 dataset (format version 3, adjacent descendant links) distributes its halos over forests and over **effective descendant trees** (each tree is the set of halos whose descendant chain ends at one terminal halo, `Descendant == -1`). It then builds the **decided cut table**, which cuts every forest at its z = 0 FoF groups: each tree joins the forest of its terminal halo's FoF group at the dataset's final snapshot. It also measures what that table costs and how the cut dataset would partition. It reads the dataset and the catalogue's index files only, never the merger-tree source, and never holds a whole slab column of the dataset: every read is a bounded block (`--block-rows`, default 2²²). It writes nothing outside the **aggregate directory** you name. That directory is bound to the dataset it was produced from, so a later subcommand given another dataset refuses it. Each subcommand writes its own subdirectory with `summary.json` last, recording its results, the bytes it read, its peak resident set per phase where it measures one, and the size and formula of every aggregate it wrote. Delete the directory by hand when the census is done.

```bash
P=convert/mimic-convert/forest_census.py
mimic_venv/bin/python $P occupancy --dataset <dataset dir> --aggregate <dir>
mimic_venv/bin/python $P trees     --dataset <dataset dir> --aggregate <dir> \
    --forests-list <forests.list> --locations <locations.dat> [--conversion-report <json>]
mimic_venv/bin/python $P partition --aggregate <dir> [--ntask 1,2,4,8] [--nchunk 1,2,4,8,16,32]
mimic_venv/bin/python $P table     --dataset <dataset dir> --aggregate <dir> \
    --forests-list <forests.list> --locations <locations.dat> [--forest-index N ...]
mimic_venv/bin/python $P cut       --dataset <dataset dir> --aggregate <dir> --reserve-gib <GiB> \
    [--forest-index N ...] [--ntask 1,2,4,8] [--nchunk 1,2,4,8,16,32,64,128] \
    [--bytes-per-halo 1100] [--laptop-gib 16,32,64]
```

Run `occupancy`, `trees` and `partition` first; `table` needs `trees`, and `cut` needs all three. A rerun of an earlier subcommand does not invalidate a later one's summary, so rerun everything after it or start a fresh directory.

- **`occupancy`**: per slab, the forests present and their row counts; per forest, its total, peak occupancy and the slab of the peak; the widest slab, its top-10 and top-100 shares, and the forests whose occupancy ever exceeds 100,000, 250,000 and 1,000,000.
- **`trees`**: a terminal-root label (int32 root ordinal) for every halo, per-tree totals and per-slab occupancy, and the largest tree. With the catalogue's `forests.list` and `locations.dat` it checks the root set against the index files' tree roots (roots missing from `forests.list`, listed roots with no terminal halo, roots outside the last slab, forest disagreements). With a JSON whose `source_files` carry per-file `parsed_count` (the ASCII preparation's `manifest.json` of a `convert_trees.py` conversion of the same files, or any conversion record of that shape), it also sums the tree totals per source file through `locations.dat` and compares each file's sum with its parsed count. Agreement in both checks is necessary for the effective trees to be the catalogue's `#tree` blocks, but not sufficient.
- **`partition`**: an exact simulation of the horizontal driver's task and chunk partition (`src/core/horizontal_partition.c`), each forest weighted as the driver weighs it, by its peak occupancy (its largest row count in any one slab, which `occupancy` records), for every (ntask, nchunk) point: each range's rows in every slab, the widest range, and the heaviest forest as the floor no partition goes below.
- **`table`**: the decided table, in `forests.list` shape (one `<tree root id> <forest id>` row per catalogue tree), with a streamed JSON record: the dataset identity, the index files' md5, the rule ("the z = 0 FoF groups of every forest", the final snapshot and its scale factor), every piece of every split forest (id, original forest id, trees, halos, peak occupancy and its slab) and the table's md5. A forest ending in one z = 0 group keeps its id. A forest ending in several is cut into exactly its groups: the largest piece (by halos over all snapshots, ties by the smallest tree root id) keeps the forest's id, and every other piece takes a fresh id above the catalogue's maximum, in descending size with the same tie rule. The table is the partition that cuts every co-membership ended before the final snapshot and keeps every one present at it: no tree is split and every z = 0 FoF group is preserved. `--forest-index` restricts the cut to the named forests (every other forest keeps its id); its outputs go to `table-restricted/` and say so. `table` refuses, before writing anything, a census holding a tree that ends before the final snapshot (naming the count), a `trees` root correspondence that did not pass, index files that do not describe the census, and any final-slab central reference that is out of range, not its own central, or in another forest. The table's invariants are checked when it is written and whenever it is read (`census/cut_table.py`).
- **`cut`**: the decided table's cost over every forest it splits, built exactly as `table` builds it (both summaries carry the assignment's SHA-256). Per slab, it counts the halos promoted to central (none at the final snapshot, which is checked), the groups that lose members, and the remnants a group leaves in another piece (those of two or more members separately). It counts the descendants whose progenitor chain order changes, with a check of the recomputed chains against the stored `NextProgenitor` links, and the re-labelled halos. The affected-history bracket is the seeds and every halo on a seed's descendant path, and every halo of a piece holding a seed. It also simulates the driver's partition with the pieces installed, each weighted by its peak occupancy, for every grid point, with the per-process figure and the job figure (the sum over tasks of each task's widest chunk over all slabs, since one laptop runs every rank at once). Each laptop class gets a row at `--bytes-per-halo`, judged on the job figure against the class less `--reserve-gib`. Its output is `cut/summary.json` (`cut-restricted/` with `--forest-index`).

**Memory and disk at production scale.** These are measured on the Shin-Uchuu version 2 production dataset (22,503,649,037 halos in 70 slabs, 166,547,771 forests, 315,004,242 trees), on the Mac Studio, reading from an external disk. Peak RSS is from `/usr/bin/time -l`. Each module's docstring states the formula behind every term; the per-slab terms of `cut` grow with the widest slab (12 B per row of the slab for the labels and two dense row arrays).

| Subcommand | Wall-clock | Peak RSS | Aggregates written |
|---|---|---|---|
| `occupancy` | 17 min | 32.7 GB | 64.6 GB: per-slab (forest, count) pairs, 12 B per present forest per slab, plus 16 B per forest |
| `trees` (with index files) | 46 min | 41.7 GB | 190.3 GB: labels 4 B per halo (90.0 GB), per-slab (tree, count) pairs 8 B per present tree per slab (89.0 GB), 36 B per tree (11.3 GB) |
| `partition` | 189 s (measured) | 6.10 GB (measured) | under 2 MB of JSON |
| `table` | 18 to 25 min | 57.3 GB (see below) | 10.5 GB: the table, about 24 B per catalogue tree (7.6 GB), and its record, about 34 B per piece of a split forest (3.0 GB) |
| `cut` | 5,364 s, 89 min (measured) | 32.95 GB (measured; see below) | under 0.3 MB of JSON |

The whole directory therefore needs about 266 GB on disk. The peak RSS figures are measured with `MallocLargeCache=0`. With macOS's default allocator, freed large blocks stay resident in its cache and `/usr/bin/time` reported 66.7 GB for `table` and 109.4 GB for `cut`. The `trees` labels and per-slab pairs are its largest part, and both `table` and `cut` read them, so delete the directory only when the census is finished. At micro-Uchuu scale (22,580,924 halos) every subcommand runs in seconds, under 0.5 GB RSS.

## Rewriting a horizontal dataset with `rewrite_trees.py`

`rewrite_trees.py` rewrites an existing version 3 horizontal-HDF5 dataset of a Consistent-Trees ASCII catalogue into a version 3 dataset of the same halos, slab by slab, without reading the merger-tree source again. It accepts only datasets whose every `Descendant` link is adjacent (`links_adjacent` 1). It has two operations:

- **`noop`**: a version 3 dataset is rewritten under the identity table. The output must reproduce the input in every dataset value, dtype, header attribute, `/schema` attribute and sidecar value. This is the passes' permanent self-test.
- **`cut`**: a version 3 dataset is rewritten under a declared cut forest table (`--cut-table`, in `forests.list` shape, such as the decided table `forest_census.py table` writes). Every tree joins the table's forest. A cut forest's largest piece keeps its id, and the fresh pieces take ids above the catalogue's maximum, enumerated after every original forest. Hosts are resolved under the original partition, and every halo whose FoF central lands in another forest becomes a central of its own group. FoF chains, ranks and progenitor chain order are recomputed for the forests the table changes, and every other forest is reproduced exactly. The same table given to `convert_trees.py ingest --cut-table` converts the catalogue directly into its cut form. The two routes write datasets identical in every value, header attribute, `/schema` attribute and sidecar value; the HDF5 file layout may differ.

```bash
R=convert/mimic-convert/rewrite_trees.py
COMMON="--simulation-info <simulation_info.yaml> --a-list <a_list> \
    --forests-list <forests.list> --locations <locations.dat> --multiplier <M>"
mimic_venv/bin/python $R noop    --input <version 3 dataset dir> --output <workdir> --scratch <dir> $COMMON
mimic_venv/bin/python $R cut     --input <version 3 dataset dir> --output <workdir> --scratch <dir> $COMMON \
    --cut-table <forests.list-shaped table> [--cut-table-record <the census's record.json>]
mimic_venv/bin/python convert/mimic-convert/convert_trees.py validate --workdir <workdir> \
    --multiplier <M> --memory-budget-mb <MiB> --spill-dir <dir>
```

**Inputs.** Every location is explicit and nothing defaults into the repository:

- `--input` is the dataset directory, `--output` the workdir to create and `--scratch` an empty directory for the remap store and checkpoints. The output and scratch must lie outside the input and apart from each other.
- The a_list, the simulation metadata and both index files are always named.
- Both operations accept any input whose every slab's `column_mapping_sha256` and `/schema` declarations equal those of the shipped default `consistent_trees_ascii` profile. A package profile that is identical to the default, such as the Shin-Uchuu subset's, qualifies.
- The input's `/schema` is carried through, and the output sidecar's file ordinals follow the input's sidecar: `locations.dat`'s FileID does not record the order the converter was given the files, so each FileID is mapped through the ordinals the input sidecar implies.
- `--cut-table-record` (`cut` only) supplies the rule text and the dataset lineage for the manifest, and must name the table's md5 and the index files'.

**Refusals.** Each refusal exits 2 and names the offending value. These happen before anything is written:

- an input that is not version 3;
- `links_adjacent` 0, or a version 3 input of another source format;
- missing or inconsistent index files, or an a_list that is not the input's;
- simulation metadata whose physical header values differ from any input slab's header;
- an input whose `column_mapping_sha256` or `/schema` declarations differ from the shipped default profile's;
- for `noop`, a `--cut-table` or `--cut-table-record`;
- without `--resume`, a `--scratch` that is not empty, or an `--output` that already holds a run's `manifest.json` or is otherwise not empty;
- a multiplier that cannot encode the output's bounds (the installed table's, for a cut);
- two FileIDs naming the same tree file, or a FileID-to-file-ordinal mapping the input sidecar does not determine consistently;
- terminal halos that do not correspond to the index files' tree roots (refused, never repaired);
- a cut table violating any of its invariants;
- a cut piece held in one file whose FileID the input sidecar gives no conversion ordinal for;
- a record that is not the given table's or index files'.

A cut also checks two conditions per slab once the workdir exists, and refuses a halo whose FoF central lies in another forest of the input, and stored progenitor chains that the reference insertion loop over the input's own keys does not reproduce.

**Output and manifest.** The workdir has the converter's shape, so `convert_trees.py validate` and `report` run against it with their manifest binding, schema binding and count-conservation checks applied:

- `manifest.json` is a `mimic-generic-conversion` manifest (version 3). Its configuration records the operation, the input and its format version, the multiplier, the planned memory peak and the SHA-256 of every input file named. For a cut it also records the table's path, md5 and SHA-256, the rule text and the record's dataset lineage.
- `dataset/` holds exactly the snapshot files and `forests.h5`; point a run's `simulation_dir` here.
- `tree_inventory.npy` holds every tree's `locations.dat` file and row count.
- `rewrite_state.json` is the restart state.
- `rewrite_summary.json` records:
  - the counts and link totals;
  - the root correspondence and its verdict;
  - the per-tree inventory's halo totals per source file (its conservation verdict is `unchecked`: the rewriter is given no conversion record, and the battery's count conservation checks the totals);
  - the memory formulas, `planned_peak_bytes` and the measured `peak_rss_bytes`;
  - the remap store's peak;
  - for a cut, the per-snapshot promotions, the groups that lose members, the descendants whose progenitor order changes, and the pieces.
- `cut_pieces.npy` (cut only) lists every piece of every split forest.

Per-file conservation against a conversion record, such as the ASCII preparation manifest of a `convert_trees.py` conversion of the same files, is `forest_census.py trees --conversion-report`'s check.

**Resume.** Each snapshot is written under a temporary name, re-read and verified against the input, and renamed only when it passes. Every verified slab is recorded in the restart state before its recovery files are removed. `--resume` continues an interrupted run after its last verified slab. It refuses a changed input or configuration, the cut table's md5 included, and re-verifies a completed run. The scratch is emptied when the run completes; delete the empty directory by hand.

**Memory and storage, as measured.** The package docstring (`rewriter/__init__.py`) states every memory term per phase. The remap store holds 32 B per row of each resident slab for `noop` and 48 B for `cut`, over at most three slabs: 96 B and 144 B per row of the widest slab. The measurements below are on the Shin-Uchuu ASCII subset (`simulations/shin-uchuu-ascii`: 2,744 tree files, 406,668,896 halos in 70 slabs, widest slab 9,006,294 rows, 6,011,205 forests, 8,000,198 trees), on the Mac Studio, with input, output and scratch on one external disk, multiplier 2 × 10¹⁰. Peak RSS is from `/usr/bin/time -l` with `MallocLargeCache=0`, and the scratch peak was sampled every 30 s.

| Operation | Wall-clock | Peak RSS (planned) | Remap store peak | Scratch peak | Output dataset |
|---|---|---|---|---|---|
| `noop` (version 3 in) | 500 s | 2.08 GB | | | value-identical to the input |
| `cut`, decided table (6,011,205 → 7,206,195 forests) | 625 s | 4.17 GB (4.06 GB) | 1,296,188,984 B: 143.9 B per widest-slab row, 3.2 B per halo | 2.24 GB: 249 B per widest-slab row, 5.5 B per halo | 57.4 GB, 141.2 B per halo |

The scratch holds pass 0's checkpoints (the forest totals, roots and per-tree inventory), the resident slabs' remap entries and pass 1's checkpoint. At most one snapshot file is temporary in `dataset/` at a time, so a run needs about the output's size plus the scratch peak on its volume. At micro-Uchuu scale (22,580,924 halos) `cut` took 44 s at 0.40 GB. At production scale, `cut` under the decided table on the Shin-Uchuu dataset (3.16 TB, 22,503,649,037 halos, widest slab 519,342,987 rows) peaked at 211.6 GB RSS against its planned 223.6 GB.

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
| `ctrees_parser.py`  | the Consistent-Trees ASCII parse for the ASCII route's preparation: scratch record layouts; indexed/`#fields:` header dialects; `#tree` marker tracking; chunked reads; independent pre-count |
| `scatter.py`        | ASCII preparation: `forests.list` map + dense ForestIndex; per-snapshot scatter/concat; the preparation manifest; cleanup containment guard |
| `sort_index.py`     | ASCII preparation: per-snapshot sort + id index; verify-then-delete |
| `fixups.py`         | ASCII preparation: a_list adjacency validation; spin `J/Mvir` and Len conventions; `fix_upid` reference equivalent (no `fix_flybys`) |
| `rank_sort.py`      | external merge-sort rank core shared by the ASCII preparation's rank pass and the transpose |
| `links.py`          | ASCII preparation: FoF chains, descendant merge-join, progenitor chains (literal `assign_mergertree_indices` insertion semantics), within-forest ranks, identity fields |
| `hdf5_writer_v3.py` | `HorizontalV3Writer`: the version 3 emission the pipeline's `write` stage runs — one file per a_list snapshot plus `forests.h5`, write-verify-record, and the write-time binding to the recorded `simulation_info` and particle mass; shared header-metadata loading and naming helpers |
| `validate_v3.py`    | the bounded 20-check version 3 battery that `convert_trees.py validate` runs by default (its external-sort mode) — exact object and schema set, header agreement across files, source-key coverage, every link target and chain closure, identity density, field finiteness, positions inside the header box, count conservation and manifest binding — with hand-typed format tables independent of the writer |
| `validate_bounded.py` | the version 3 battery's bounded mode (`validate --bounded`): the same 20 checks over an adjacent dataset with no sort and no spill, within a printed ceiling; `ceiling` evaluates that ceiling for given dataset numbers |
| `report.py`         | conversion report emission (`conversion_report.{json,txt}`): battery outcomes, the recommended identity multiplier, source format, mapping, link-gap counts, index bounds and the runtime-compatibility section (format consumed, evidenced routes, limits) |
| `forest_census.py`, `census/` | the forest census of a version 3 horizontal dataset and the decided cut table: `occupancy` / `trees` / `partition` / `table` / `cut`; see [Forest census](#forest-census-and-the-decided-cut-table-with-forest_censuspy) |
| `rewrite_trees.py`, `rewriter/` | the horizontal-dataset rewriter of a Consistent-Trees ASCII catalogue's version 3 dataset: `noop` (the identity self-test) / `cut` (under a declared cut table); see [Rewriting a horizontal dataset](#rewriting-a-horizontal-dataset-with-rewrite_treespy) |
| `horizontal_dataset.py`, `source_index.py` | bounded column reads of a horizontal dataset and its identity record; the `forests.list` / `locations.dat` reader |
| `subset.py`         | whole-forest subset selection and byte-exact extraction from a very large ctrees dataset, driven entirely from the index files: `plan-candidates` / `sample-roots` / `calibrate-proxy` / `finalize` / `extract` |
| `HORIZONTAL-HDF5-FORMAT.md` | the normative on-disk contract the writer conforms to |
| `tests/`            | stdlib-unittest suite; synthetic fixture generator (`fixtures.py`); mock reference builder (`mock_reference.py`); the real-data comparison harness (`run_generalisation_acceptance.py`); committed golden fixtures under `tests/data/` |

## Tests

The converter suite runs with `make tests-converter`, which is also how `make tests` and CI run it:

```bash
make tests-converter                                             # the converter suite, as CI runs it
mimic_venv/bin/python -m unittest discover -s convert/mimic-convert/tests -v   # the same suite directly
```

The comparison harness's own self-tests (`test_generalisation_acceptance.py`) plant defects — dropped and duplicated rows, wrong target snapshots, reordered chains, changed values — and require the comparator to catch each one.

The dump harness is built and checked byte for byte against literal oracles by the same self-tests (`CDumpToolTests`).

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [docs/VISION.md](../../docs/VISION.md): architectural principles and design boundaries
- [docs/USER-GUIDE.md](../../docs/USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [docs/STYLE-GUIDE.md](../../docs/STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- [HORIZONTAL-HDF5-FORMAT.md](HORIZONTAL-HDF5-FORMAT.md): the horizontal-HDF5 on-disk contract these tools write
- [profiles/README.md](profiles/README.md): the mapping-profile grammar
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
