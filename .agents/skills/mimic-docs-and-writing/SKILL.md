---
name: mimic-docs-and-writing
description: Maintaining Mimic's documentation of record and house writing style. Load when a task involves editing README.md, docs/VISION.md, docs/USER-GUIDE.md, docs/DEVELOPER-GUIDE.md, docs/STYLE-GUIDE.md, tests/README.md, plot/mimic-plot/README.md, any model/simulation/module README, files under docs/dev/ (plans), make check-docs failures or broken links/anchors, Markdown formatting rules (hard-wrapping, line length), writing commit-message or report prose about Mimic, or deciding what may be claimed about Mimic externally (papers, release notes) versus what must stay labeled open/candidate.
---

# Mimic Docs and Writing

Mimic's documentation is a designed system with strict role separation, a narrative doctrine that survived a same-day revert, and a validator (`make check-docs`). This skill is how to write inside that system without degrading it.

## When to use / when NOT to use

Use for: editing any doc of record, README conventions, the docs/dev reference rule, Markdown rules, link validation, external claims.

Do NOT use for:
- Skill files' own maintenance duty — the pre-commit "skill sweep" lives in `mimic-change-control`.
- The evidence behind a scientific claim — see the `mimic-scientific-method` skill (write only what was measured).
- Code comments and C/Python style — `docs/STYLE-GUIDE.md` directly (comments explain *why*, one line; `SAGE parity:` markers).

## First actions

1. Identify the owning document before writing a word (table below) — content in the wrong document is the main failure mode.
2. Read the surrounding sections and match their voice; each document has one audience.
3. After ANY doc edit: `make check-docs` (validates every internal link and anchor repo-wide, and rejects leftover `PONDER` review markers; skips `.git`, `.claude`, `archive`, `build`, `mimic_venv`, `sage-code`, test outputs).

## 1. The documents of record and their roles

| Document | Role | Audience question it answers |
|---|---|---|
| `README.md` | Problem-first front door; shortest path to a first result | "Should I use Mimic?" |
| `docs/VISION.md` | Architectural principles and boundaries; changes only when implemented behavior justifies it | "Why is it designed this way?" |
| `docs/USER-GUIDE.md` | Workflow-oriented: generate/configure/analyse catalogues; troubleshooting | "How do I use it successfully?" |
| `docs/DEVELOPER-GUIDE.md` | Extension workflows, APIs, metadata, testing; the Reference section is the one place for reference-manual prose | "How do I modify it?" |
| `docs/STYLE-GUIDE.md` | Naming, comments, metadata style, test style, review conventions | "What should contributions look like?" |
| `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md` (beside the converter manual `convert/mimic-convert/README.md`) | Normative on-disk contract for horizontal HDF5 input (`format_version` ratchet): version 3, the only supported version, in its own normative `Version 3` section; version 2 is kept unchanged under a historical heading. An edit that changes which files conform needs a version bump; an edit that only corrects inaccurate wording goes in that doc's dated Errata table. Never edit version 2's text to accommodate version 3 | "What must a horizontal-HDF5 input file contain?" |
| `convert/mimic-convert/README.md` | Converter manual: converting Consistent-Trees ASCII/HDF5, L-Halo and forests-HDF5 trees to horizontal HDF5 input | "How do I convert my trees to horizontal input?" |
| `tests/README.md`, `plot/mimic-plot/README.md` | Quick references for their subsystems, deferring depth to the guides | — |
| `models/<m>/README.md` | That package's science scope, pipeline, parameters, plots, references, citations | — |
| `simulations/<s>/README.md` | Data provenance, units, snapshot lists, fixtures, maintenance obligations | — |
| `models/<m>/modules/<mod>/README.md` | Short local contract: what/mode/pipeline position/properties/parameters/events/notes/references (the ~29-line `sage_resolve_mergers_and_disruption` README is the house model) | — |

Keep content in its owner and cross-link generously rather than repeating. When code, metadata, and docs disagree: fix the source of truth first — metadata or code, then generated artifacts, then documentation.

## 2. The narrative doctrine (learned the hard way)

**Mimic the framework is the focus — never one model.** A full documentation overhaul was reverted the same day it landed (2026-06-11) because it leaned model-centric; the accepted redo presents model packages as interchangeable and self-documenting. Operational rules:

- sage16 is "the current build default and worked example" — no pedestal phrases ("the main package", "the package to use for science"). Model prominence IS appropriate inside that model's own README.
- Write model references pattern-first: `models/<model>/README.md` is each package's canonical doc; "see `models/` for what's shipped". New packages must never require guide rewrites.
- Modularity means models AND simulations: wherever docs show `make MODEL=<name>`, include `SIMULATION=<name>`; running one model across different boxes/resolutions is a first-class workflow, not a footnote.
- Citation guidance defers to each package's README (the guides never hardcode one model's papers as "the" citation).

## 3. Mechanical rules

- **Never hard-wrap Markdown prose.** Full paragraphs are single long lines; renderers soft-wrap. Applies to ALL `.md` files including skills. (Manual wrapping renders poorly and makes diffs unreadable.)
- Code blocks and YAML/shell examples inside Markdown follow the 100-character guideline.
- Start major documents with a one-line purpose statement; end guides with the shared "Documentation Directory" section (copy an existing one).
- Do not duplicate generated or exhaustive structural lists (property tables, module inventories) in prose — link to the metadata or generated output instead; small, stable, deliberately illustrative excerpts are fine (STYLE-GUIDE rule).
- Anchors: `make check-docs` verifies `#section-anchors` against actual headings — renaming a heading breaks inbound links repo-wide; grep before renaming.

## 4. The docs/dev reference rule

`docs/dev/` is the owner's development pathway; plans there are ephemeral, so **nothing outside `docs/dev/` may reference anything inside it**: never cite a plan, brief, log, section or decision label from a guide, README, code comment, test or skill as the source of a fact, measurement, status or evidence; state the fact inline with its qualifiers or drop the sentence. Everything outside `docs/dev/` is also written for the reader of the released repository: it says what the code and data are and how to use them, never how the work was done (no procedure steps, review panels, plan identifiers or narrative of what was found and fixed). VISION owns principles.

## 5. External claims (papers, release notes, README statements)

The evidence bar applies to prose: every claim states what was measured, and unmeasured aspirations stay labeled open/candidate (`mimic-scientific-method` owns the bar). What the repo's record currently supports:

- **Physics-agnostic core with runtime-configurable modules** — supported: the core names no physics module; the empty pipeline and `halos-only` package run; infrastructure tests use the model-neutral fixture.
- **sage16 reproduces published SAGE** — supported, with the precise phrasing: near-bit-parity against Croton et al. (2016) SAGE on mini-Millennium, ≥98% of matched galaxies bit-identical per property at z=0, residuals at float-ULP level plus ~0.1% chaotic threshold flips (chronicle: `mimic-failure-archaeology`). Do not round this up to "identical".
- **Model and simulation interchangeability** — supported to the extent shipped: three model packages and sixteen simulation packages run through one framework; cross-format consistency is validated on the micro-Uchuu triplet. The one divergence this used to carry (`fix_flybys` at the final snapshot) has been removed — see `mimic-failure-archaeology` incident 9 — so the triplet agrees on population, Types and `MostBoundID` sets (measured at snap49 when the fix landed; `fix_flybys` never acted earlier), modulo a separate, unrelated, pre-existing float32-ULP reader precision difference (about 2 × 10⁻⁸ relative in `Mvir`) between the Consistent-Trees ASCII and HDF5 readers that remains and that `sage16` amplifies; halo-level agreement across the micro-Uchuu triplet is what is validated.
- **Reproducible output provenance** — supported: every run self-records pipeline, parameters, event contracts, versions, and schema (HDF5 `RunProperties`; run-local `metadata/`).
- **Horizontal processing** — only `format_version = 3` input is supported, and **on the gated routes only**; version 2 is no longer read (the evidence recorded across the two versions was the `sham` leg, byte-identical output on the micro-Uchuu version 2 dataset and on its version 3 reconversion, and `sage16` on Shin-Uchuu's uncut migration against its version 2 run, bitwise identical over 2,058,518,068 galaxies), each per-`UniqueGalaxyID` bitwise identical to its own source format's vertical reader over the same files; the seven routes, their models and coverage are the table under `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support` (mini-Millennium, Millennium, mini-Uchuu, and micro-Uchuu from L-Halo, from forests-HDF5 and from Consistent-Trees ASCII, plus Shin-Uchuu from Consistent-Trees ASCII; copy from it, do not restate it from memory). Say exactly that. **Shin-Uchuu is supported with limits**: its evidence differs in kind, because the full catalogue cannot run vertical (`sage16` on the uncut migrated dataset against the recorded version 2 run, and the vertical-against-horizontal subset gate), and its production dataset is the **cut** one (every forest partitioned into its z = 0 FoF groups), whose `sage16` galaxies equal the uncut run's outside the split forests and differ inside them by design, reported and not gated. Never generalise it to "Mimic runs version 3 data" without the route list, to `sage16` outside mini-Millennium, micro-Uchuu ASCII and Shin-Uchuu, to vertical parity over the full Shin-Uchuu catalogue, to `hod` or `sham` identity on either production dataset, or to equality across source formats. **Full Uchuu is not claimed** — int64 makes its slabs addressable, not resident; chunked sweeps now exist but full Uchuu stays unclaimed because it is storage-bound before it is memory-bound, and its largest forest bounds any chunk.
- **Distributed operation (`mpirun`, `NTask > 1`) and chunked slab streaming (`input.forest_chunks`)** — supported with limits, each per-`UniqueGalaxyID` bitwise identical to a serial run. Evidence by feature: chunking, the fixture for `halos-only`, `sage16` and `hod` (its audit omitted) plus micro-Uchuu for `sage16` and `halos-only`; distribution, the fixture for four models plus micro-Uchuu for `sage16`, `sham` and `hod` at `-np 4`, compared with serial runs made before distribution existed. The limits to name: forest-blocked version 3 input only (a dataset whose rows are not grouped by forest is refused at startup, such as an older Consistent-Trees ASCII conversion; the micro-Uchuu ASCII route has the same `-np 4` and `forest_chunks: 4` identity for `halos-only` and `sage16`); `post_snapshot` modules are excluded under chunking (`sham`, and `hod` with its audit, need `forest_chunks: 1`), and under distribution they must be `snapshot_distribution: collective`; a forest is never split, so the largest forest's share of the widest slab is a floor (1.60% micro-Uchuu, 61.86% Shin-Uchuu's uncut super-forest, which the production dataset's cut removed: `sage16` on the cut dataset at one task and `forest_chunks: 128` peaked at 6.170 GB RSS, a Mac measurement under `MallocLargeCache=0` that is only a **proxy** for the 16 GiB laptop class until a laptop of that class runs it); `G` is chosen by the user, not derived; measured peak-RSS figures are single runs on one macOS host, where the default allocator's cache of freed large blocks inflates RSS (with `MallocLargeCache=0` both models fall nearly as `1/G`, 0.15 at `G = 8`), so quote the accounted retention or the cache-disabled figures when reasoning about memory. Never claim full Uchuu from them, nor a Shin-Uchuu laptop run (the 6.170 GB figure is a proxy).
- **The vertical Shin-Uchuu (`shin-uchuu-lhalo`) and its converter** — supported with identity at **production scale for `sage16`** and at subset scale for `halos-only` and `sage16`: `convert/mimic-convert/write_vertical_trees.py` converts a forest-blocked version 3 dataset to L-Halo binary in the record a package declares. A vertical `sage16` run on the production dataset (361 files, 244,953,607 forests, 22,503,649,037 halos) under `mpirun -np 4` is bitwise identical to the cut horizontal run of `shin-uchuu`: 2,059,547,378 galaxies over ten output snapshots, `UniqueGalaxyID` sets identical with no duplicates, all 42 fields byte for byte, 313,753,033 galaxies at z = 0. At subset scale the cut Shin-Uchuu subset (406,668,896 halos, 7,206,195 forests) converted to L-Halo binary reads halo for halo, link for link and bit for bit as its horizontal dataset says (the acceptance harness's `compare`, 25 checks), and the package's cross-format identity gate passes `halos-only` and `sage16`, fixed and dynamic, bitwise in every field over eight output snapshots against the horizontal run of `shin-uchuu`; a vertical `sage16` run under `mpirun -np 4` equals the serial run bitwise. Say exactly that. No `hod` or `sham` identity or run is claimed (the vertical created-record identity space does not fit int64 at the production forest count, so the run logs that it cannot create records), no production-scale identity for `halos-only` (it ran on file 0 alone, without a comparison), and nothing about SAGE26. Keep the converter's capability, the production identity and the subset evidence in separate sentences, and quote the memory the largest forest needs (about 165 GB for `sage16`, 143 GB for `halos-only`) as a property of the package, never as a wall time.
- **Converter generalisation** — supported as **conversion only**: `convert/mimic-convert/convert_trees.py` converts L-Halo binary, Consistent-Trees forests-HDF5 and Consistent-Trees ASCII trees to lossless horizontal-HDF5 version 3, verified halo by halo, link by link and bit by bit against the source's own vertical reader on complete real mini-Millennium and micro-Uchuu data. Say exactly that. The whole Millennium and mini-Uchuu simulations were converted too (the producer battery passed), and full Uchuu only from a six-halo fixture (its production catalogue never converted). Conversion evidence is not runtime evidence: a runtime claim for a version 3 route rests on that route's parity gate (the bullet above), never on its conversion. Conversion capability, fixture evidence, production evidence and runtime evidence are different claims; keep them in separate sentences.
- Running version 3 on any route not listed above (full Uchuu included), sub-forest chunking, automatic chunk-count selection, the snapshot scope under chunking, embedded engines, or assisted model building is **planned/open** — label it planned or open.

When writing citations, follow each package README's list (sage16: Croton et al. 2016, ApJS 222, 22 and Croton et al. 2006).

## Provenance and maintenance

Verified against the live repo 2026-07-04 (doc roles cross-checked against the documents themselves; narrative doctrine against the recorded revert/redo history; validator behavior against `scripts/check_docs.py`). Re-verify:

```bash
make check-docs                                            # validator alive and repo clean
grep -n "PONDER\|SKIP_DIRS" scripts/check_docs.py | head -5
ls docs/                                                    # document set
wc -l models/sage16/modules/sage_resolve_mergers_and_disruption/README.md   # house-model README
git log --oneline -n1 8d0f39c6 432e4ca7                     # the revert/redo doctrine anchors
```

The horizontal-processing and converter-generalisation claims were re-derived 2026-10-02 from the conversion reports and the parity-gate results recorded in the package READMEs and the format specification's route table. Document roles and the narrative doctrine are owner-set and durable; the external-claims list must be re-derived from the repo's evidence whenever capabilities land (a claim is only as current as its measurement).
