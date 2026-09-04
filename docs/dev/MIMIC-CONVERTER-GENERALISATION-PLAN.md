# Mimic Converter Generalisation Plan

**Status:** Concept note. Not scheduled. Captures open questions and a candidate design direction from a session discussion held alongside the Shin-Uchuu production conversion (2026-09-04), so they are not re-litigated or lost. Deliberately left unresolved until promoted to a requirements brief — no scope is committed here.

**Date:** 2026-09-04

---

## Goal

The `scripts/convert/` ctrees-ASCII-to-snapshot-HDF5 converter was built and validated for one simulation family (Uchuu/Shin-Uchuu, via `consistent_trees_ascii`), and its owner intends to generalise it so it can convert other simulations. This note captures two things that surfaced while discussing that intent, using the completed Shin-Uchuu production run as a concrete worked example: (1) the converter's column selection is currently hardcoded rather than declared, which is both the generalisation's central design question and the reason a later "I want that column back" request is expensive today; and (2) a candidate cheaper mechanism for adding a column to an *already-converted* dataset without repeating the full multi-day pipeline. Neither is designed, built, or validated. This document exists so a future session promoting this work starts from the reasoning already done rather than rediscovering it.

## Motivation: the concrete case that raised this

Mimic's own property system already treats a simulation package's available fields as declared metadata (`simulations/<sim>/halo_properties.yaml`, `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`, the `mimic-properties` skill) — but the converter that *produces* the on-disk data for that package does not work the same way. Its column selection is a single frozen struct, `RECORD_DTYPE` in `scripts/convert/ctrees_parser.py`:

```python
RECORD_DTYPE = np.dtype([
    ("id", "<i8"), ("desc_id", "<i8"), ("desc_scale", "<f8"), ("pid", "<i8"), ("upid", "<i8"),
    ("snap", "<i4"), ("Mvir", "<f4"), ("X", "<f4"), ("Y", "<f4"), ("Z", "<f4"),
    ("VX", "<f4"), ("VY", "<f4"), ("VZ", "<f4"), ("Jx", "<f4"), ("Jy", "<f4"), ("Jz", "<f4"),
    ("vrms", "<f4"), ("vmax", "<f4"), ("tree_root_id", "<i8"), ("forest_id", "<i8"),
], align=False)
```

This is 18 fields out of the 61 columns the real Shin-Uchuu source data actually carries — confirmed directly from `tree_0_0_0.dat`'s own header line on `nt.swin.edu.au` during the production run (`head -c 4000 tree_0_0_0.dat`; the file's own indexed header names and numbers each one). The other 43 columns (shape parameters, tidal quantities, pseudo-evolution-corrected masses, multiple spin/energy diagnostics, internal Consistent-Trees bookkeeping IDs) are parsed past and discarded at the very first read. Nothing downstream of that point — `scatter.py`'s worker scratch, `sort_index.py`'s sorted records, `fixups.py`'s fixed dtype, `links.py`'s identity/rank pass, `hdf5_writer.py`'s emitted HDF5 — ever sees them again, and none of the intermediates survive the run anyway (consumptive deletion removes them once each stage's successor verifies).

The measured consequence, from the completed production run (`docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md` → "Production run complete"): the source was 11.61 TB / 22,503,649,037 halos ≈ 516 bytes/halo as ctrees ASCII text; the emitted dataset is 2.0 TB for the same halo count ≈ 89 bytes/halo as HDF5 — roughly a 5.8× reduction, dominated by (a) discarding those 43 unused columns, (b) ASCII-to-binary numeric encoding efficiency, and (c) replacing ctrees' large global tree-scope IDs with compact `int32` snapshot-local array indices during Phase 3's link remapping. No scientific content Mimic uses was lost — but if a future need calls for one of the 43 discarded columns, recovering it today means hand-editing the frozen dtype in five files (`ctrees_parser.py`, `scatter.py`, `sort_index.py`, `fixups.py`, `links.py`, `hdf5_writer.py`), bumping `docs/dev/SNAPSHOT-HDF5-FORMAT.md`'s `format_version` ratchet, adding the field to the simulation package's `halo_properties.yaml`, running `make generate`, and re-clearing the micro-Uchuu acceptance gate (producer battery + topology cross-check) before trusting the rebuilt converter again — real work, not a config change.

## Design direction: declarative column mapping

Generalising the converter to a second simulation forces the same question a column-restoration request does: *which source columns exist, and which do we keep?* Different Consistent-Trees runs are not guaranteed to carry the same extended column set (some configurations track fewer of the shape/tidal/pseudo-evolution diagnostics than others), so the generalisation work already has to stop assuming one fixed, hardcoded layout.

The candidate direction: replace `RECORD_DTYPE` and its downstream propagation with a declared, per-simulation mapping — "these named source columns become these named output fields" — read from configuration rather than compiled into a Python literal. If that mechanism is built to be genuinely arbitrary (any recognised source column to any declared output field, not just a fixed list with a size knob), then both problems are solved by the same piece of work: a new simulation with a different available column set is a new mapping, and a later "keep this column too" request against an existing simulation is an edit to that simulation's mapping rather than a five-file surgical change.

This does **not** remove the Mimic-side step. A genuinely new output field still needs registering in the target model/simulation package's `halo_properties.yaml` (units, precision, range) and a `make generate` pass — that is inherent to the framework's metadata-as-structural-truth principle (`docs/VISION.md`) and is orthogonal to whatever the converter's own column-selection mechanism looks like.

## A candidate cheaper path: topping up an already-converted dataset

A separate question came up: if a column is wanted for a simulation that has *already* been fully converted (as Shin-Uchuu now has), must the whole multi-day pipeline be repeated, or is there something cheaper?

**The naive version does not scale.** The obvious-sounding approach — for each of the dataset's halos, locate its row in the raw ctrees source and copy the wanted field across — fails if implemented as one independent lookup per halo. At Shin-Uchuu's measured production scale (22,503,649,037 halos) even an optimistic ~1 ms of per-lookup overhead (network/filesystem seek, dispatch) totals over 260 days before any useful data is read; in practice, scattered small random reads at that volume would plausibly cost months to years, particularly over a network mount. This is very much in the territory the person raising the idea was themselves worried about.

**The tractable version keeps the same idea but changes the granularity.** Instead of one lookup per halo, do one sequential pass per source file (mirroring the existing converter's own Phase 1 `scatter.py` pattern, but extracting only `id` plus the wanted new field — far less parsing per row than the full record), then a single bulk join against the *existing* output using an id-indexed structure built from the much smaller emitted dataset (2.0 TB versus 11.61 TB). This reuses the same class of technique `links.py`'s external-merge rank pass already implements for a much larger problem, applied here to something narrower: no tree topology is being rebuilt, so `finalize`/`sort`/`fixups`/`links` are not needed at all — only a lighter extraction pass and a join.

**Rough character, not a measurement.** Reading all 11.61 TB of source at least once is unavoidable regardless of approach, since nothing already-computed retains the discarded columns; this dominates the cost and would plausibly land in the same order of magnitude as this run's own measured transfer-plus-scatter time (a few days, per the production run's own record cited above), not the full ~5.76-day P1+P2 wall clock, since the topology-rebuilding stages are skipped entirely. The peak-memory character should also be much lower than `links`' measured ~225-300 GB, since there is no identity/rank-sort mechanic involved — making this plausibly the kind of job that could run at lower priority in the background rather than demanding a cleared machine. None of this is measured or built; it is a design sketch, not a plan.

## Open questions

- **Exact scope of "generalise."** This note only examined column selection. Whether generalisation should also revisit other currently Shin-Uchuu-specific assumptions (the two supported header dialects, the `snap_idx`/`snap_num` duality, the frozen scratch/fixed/links dtypes' fixed byte layouts, the batch-mode transfer machinery) is unexamined here.
- **Whether "other simulations" means other Consistent-Trees configurations only, or other tree-finder formats entirely.** The declarative-mapping direction above assumes the source is still Consistent-Trees ASCII with a possibly-different column set; a genuinely different upstream format (a different halo finder's tree output) is a larger question not addressed here.
- **Config schema for the column mapping.** Not designed — could mirror `halo_properties.yaml`'s own shape, live alongside it, or be a new converter-local file. Needs its own design pass.
- **Whether the "top-up" join mechanism is worth building as first-class, reusable tooling, or is a one-off script if and when a real need for it arises.** Recorded as a candidate, not a commitment either way.
- **Not actually open, recorded so it isn't re-litigated:** `docs/dev/SNAPSHOT-HDF5-FORMAT.md` already answers whether an added field needs a version bump — "additive changes (new optional datasets or attributes) also require a version bump; version 1 consumers are entitled to assume the exact object set specified here." Any column-restoration or generalisation work bumps `format_version`, full stop; the open question, if there is one, is only whether a lighter-weight *additive* versioning lane would be worth proposing as a change to that frozen contract — a bigger, separate question this note does not raise a case for.
- **Whether this work should land before `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md` (step 2 of the development pathway) or can be deferred without blocking it.** Recorded as a candidate to weigh at that decision point in `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` → "The Ordered Road", not decided here.

## Relationship to Other Plans

- **Arose from:** `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`, whose completed production conversion is the concrete worked example throughout this note (real column counts, real size measurements, real cost-of-change assessment against the frozen dtype).
- **Would need to update:** `scripts/convert/README.md`, the converter's own operational manual — any replacement for `RECORD_DTYPE`'s hardcoding is primarily a `scripts/convert/` change, and this is where its usage would need documenting.
- **Would need to respect:** `docs/dev/SNAPSHOT-HDF5-FORMAT.md`, the frozen on-disk contract any output-field change moves against, including its `format_version` ratchet (see "Open questions" above).
- **Would need to route through:** Mimic's property system (`mimic-properties` skill; `simulations/<sim>/halo_properties.yaml`) for any genuinely new output field, and the `mimic-simulations-and-readers` skill for the simulation-package side of adding or generalising a data source, regardless of how the converter's own column selection is generalised.
- **Candidate sequencing relative to:** `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` → "The Ordered Road", step 2 (`MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`) — see that document's note on this candidate insertion point, and the open question above.
