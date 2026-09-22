# Pre-Change ASCII Converter Baseline Reference (Slice 1)

**Status:** Slice 1 deliverable of
[`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).
Captures the existing, unmodified ASCII-to-v2 converter pipeline's
wall-clock, peak RSS and output totals on real micro-Uchuu data, at this
slice's starting commit `f5cc6004` -- before any converter generalisation
code exists. Later slices' ">20% regression" gate compares against these
numbers. This is a measurement of current behaviour into a disposable
workdir; no converter/runtime code changed to produce it, and the workdir
was deleted after capture (nothing under this slice's authorized surface
depends on it persisting).

This slice's other deliverables (`inspect_sources.py`,
`adapters/source_inventory.py`) do not touch `ctrees_parser.py`, `scatter.py`,
`fixups.py`, `links.py`, `hdf5_writer.py`, `report.py`, or
`convert_ctrees.py` -- the entire ASCII pipeline exercised below. So the
code that produced this baseline **is** the plan's base commit's code,
without needing a separate `git checkout`.

## Command sequence

The six-phase `convert_ctrees.py` sequence -- `scatter`, `sort`, `fixups`,
`links`, `write`, `report` -- from `scripts/convert/README.md`'s "micro-Uchuu
development examples", run with no flags beyond what that README specifies,
wrapped in `/usr/bin/time -l` per phase (macOS; reports wall-clock and
`maximum resident set size` in bytes). **Not captured**: the standalone
`validate.py` producer-battery invocation and the `crosscheck.py`
prepare/run-reference/compare block that README section also documents for
the same sequence -- neither is needed to reproduce any figure quoted below
(the `report` phase already runs the validation battery internally, which is
where "validation PASS" below comes from), so this is the six conversion
phases only, not literally every command in that README section. Workdir:
`output/convert/slice1-baseline-capture` (a symlink to
`/Volumes/Internal/results/mimic/convert/slice1-baseline-capture`), deleted
after capture.

```bash
WORKDIR=output/convert/slice1-baseline-capture
A_LIST=simulations/micro-uchuu-ascii/micro-uchuu.a_list
SIM_INFO=simulations/micro-uchuu-ascii/simulation_info.yaml

/usr/bin/time -l mimic_venv/bin/python scripts/convert/convert_ctrees.py scatter \
    --workdir "$WORKDIR" \
    --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \
    --a-list "$A_LIST" --simulation-info "$SIM_INFO" \
    simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat
/usr/bin/time -l mimic_venv/bin/python scripts/convert/convert_ctrees.py sort --workdir "$WORKDIR"
/usr/bin/time -l mimic_venv/bin/python scripts/convert/convert_ctrees.py fixups \
    --workdir "$WORKDIR" --a-list "$A_LIST" --simulation-info "$SIM_INFO"
/usr/bin/time -l mimic_venv/bin/python scripts/convert/convert_ctrees.py links --workdir "$WORKDIR"
/usr/bin/time -l mimic_venv/bin/python scripts/convert/convert_ctrees.py write \
    --workdir "$WORKDIR" --a-list "$A_LIST" --simulation-info "$SIM_INFO"
/usr/bin/time -l mimic_venv/bin/python scripts/convert/convert_ctrees.py report \
    --workdir "$WORKDIR" --a-list "$A_LIST"
```

Captured 2026-09-22 on host `djcmacstudio`, `mimic_venv` stack (pandas,
numpy, h5py as pinned in `requirements.txt`), reading from
`/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii/tree_0_0_0.dat`
(md5 `45b72a4f910831482a7bf3e9d2163ab3`, per the run's own
`conversion_report.txt` source-files section).

## Totals: reproduced exactly

`conversion_report.txt`'s own summary line:

```text
totals: 22580924 halo(s) in 50 populated snapshot(s); 0 flyby demotion(s); 0 Len==0 halo(s)
forests: n_forests_total=440651, max_halo_rank_in_forest=350074, recommended identity multiplier=1000000000
```

| Figure | Plan's target | This run |
|---|---|---|
| Halos | 22,580,924 | **22,580,924** |
| Populated snapshots | 50 | **50** |
| Forests | 440,651 | **440,651** |
| `max_halo_rank_in_forest` | 350074 | **350074** |
| Producer validation | PASS | **PASS** (`report: ... validation PASS`) |

Exact match on every figure the plan names. No source-identity discrepancy
was found.

## Wall-clock and peak RSS, per phase

`/usr/bin/time -l`'s `real` (wall-clock, seconds) and `maximum resident set
size` (bytes, macOS `getrusage(RUSAGE_SELF).ru_maxrss`, already
byte-granular on Darwin):

| Phase | Wall-clock (s) | Peak RSS (bytes) | Peak RSS (GiB) |
|---|---|---|---|
| scatter | 123.33 | 2,587,049,984 | 2.41 |
| sort | 15.36 | 2,106,228,736 | 1.96 |
| fixups | 13.01 | 2,163,834,880 | 2.02 |
| links | 44.21 | **4,544,200,704** | **4.23** |
| write | 10.11 | 1,364,066,304 | 1.27 |
| report | 6.07 | 375,455,744 | 0.35 |
| **total** | **212.09** | **4,544,200,704** (peak of any single phase) | **4.23** |

The `links` phase is both the second-slowest and the peak-memory phase, in
line with the implementation plan's framing of the rank/link pass as the
scale-critical stage. `links`'s own log line records its budget accounting
independently: `budget 2147483648 B (1 sorted run(s), 0 merge pass(es)); peak
spill 1083884352 B, identity stores 361294784 B on disk` -- one sorted run
means the default 2048 MiB `--memory-budget-mb` was not exceeded for this
dataset size, so no external merge pass was needed here (see C4).

**Later regression gate:** any post-generalisation ASCII-default run on the
same micro-Uchuu data should be compared against **212.09 s total
wall-clock** and **4,544,200,704 bytes (4.23 GiB) peak RSS**, per phase where
possible (the `links` phase is the tightest budget and the one most likely
to regress if the canonical-adapter bridge work in Slice 5 adds overhead to
the default zero-extra-fields path).

## Scope and disposal

- This run used **default flags only** -- no `--consume-intermediates`, no
  `--batch`/`--pool-size` (single-process, matching a typical dev/CI
  invocation, not the production Shin-Uchuu batch sequence).
- The workdir was **8.1 GB** at completion (scratch + sorted + fixed-up +
  linked intermediates + `hdf5/` output + manifest); it was deleted after
  this capture, per this slice's non-goal ("no production file rewrite" --
  the exception is capped to "a measurement of current behaviour into a
  disposable workdir", not a workdir that persists past the measurement).
- `conversion_report.json`/`.txt` were copied out before deletion and are
  the source of every number quoted above; they are not committed here
  (regeneratable exactly from the command sequence above against this same
  commit) to avoid checking in an 8 GB-workdir-derived artifact whose only
  citable content is already quoted in full in this document.
