# SAGE26 T1 parity instrument

This directory holds the instrument that measures a Mimic `sage26` run against the SAGE26 reference galaxy by galaxy and gives a numerical verdict of `pass`, `fail` or `error` against T1's thresholds:

- `compare_sage26.py`: the comparator. It needs Python 3.9 or later with NumPy and h5py (the `mimic_venv` environment) and nothing else.
- `field_map.yaml`: the pairing of every dataset SAGE26 writes with its Mimic property, or the reason it is left uncompared.
- `README.md`: this file, the instrument's specification.

The comparison rules below are fixed by the owner's approval of the instrument plan and are reproduced verbatim. The comparator implements exactly these rules; where they leave a reading open, the PM's ruling recorded under [Readings settled by the PM](#readings-settled-by-the-pm) applies.

## Command line

```bash
./mimic_venv/bin/python models/sage26/instrument/compare_sage26.py \
    --reference FILE --reference-sha256 HEX \
    --candidate DIR --partitions K \
    [--basename model] [--snapshots 63,37,32,27,23,20,18,16] \
    --json OUT.json --outliers OUT.csv
```

- `--reference FILE`: one SAGE26 worker file (`model_0.hdf5`, groups `Snap_<N>` of one-dimensional datasets named as SAGE26 writes them). The T1 reference is `/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/ref/model_0.hdf5`.
- `--reference-sha256 HEX`: the SHA-256 the reference must have. The T1 reference's digest, `19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18`, is held in the comparator as `T1_REFERENCE_SHA256`.
- `--candidate DIR`: a Mimic HDF5 run directory holding the partition files `<basename>_<NNN>.hdf5`, each with `Snap<NNN>/Galaxies` compound tables in which vector properties are 3-element float arrays. Other files in the directory (the master file `<basename>.hdf5`, `metadata/`) are ignored.
- `--partitions K`: the number of partition files the run must have: 8 for mini-Millennium (files 0–7).
- `--basename`: the partition basename, `model` by default.
- `--snapshots`: the snapshots to compare, by default T1's eight.
- `--json`, `--outliers`: the report and the outliers CSV to write.

The field map is always the `field_map.yaml` beside `compare_sage26.py`; there is no option to name another. To run the comparator against an altered map, copy both files into one directory and run the copy.

A typical T1 measurement of the package's own vertical mini-Millennium run:

```bash
make MODEL=sage26 SIMULATION=mini-millennium
./mimic models/sage26/input/sage26_mini-millennium.yaml
./mimic_venv/bin/python models/sage26/instrument/compare_sage26.py \
    --reference /Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/ref/model_0.hdf5 \
    --reference-sha256 19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18 \
    --candidate output/sage26-mini-millennium --partitions 8 \
    --json output/instrument/t1.json --outliers output/instrument/t1_outliers.csv
```

## Exit status

| Status | Verdict | Meaning |
|---|---|---|
| 0 | `pass` | Every threshold of rule 8 holds at every compared snapshot. Printed `pass (not T1-eligible)` when `t1_eligible` is false (rule 10). |
| 1 | `fail` | At least one threshold does not hold. |
| 2 | `error` | The instrument cannot measure (rule 9). NumPy or h5py failing to import, a usage error, and any unexpected internal failure are also `error`, never `fail`. |

The last line printed to standard output is `verdict: <verdict>`, followed for an `error` by its message (`verdict: error: <message>`), on every path except two: `--help` prints the help text and exits 0, and a usage error (argparse rejects a malformed command line before the comparator runs) prints argparse's usage message to standard error and exits 2. Neither writes a JSON. A `pass` or `fail` also prints, before the verdict line, one line per snapshot with N_ref, N_cand, M and the failing thresholds. That table and the verdict line are printed only after the JSON and the CSV are both in place, and a failure to print them cannot change those files; the exit status is still the verdict's. Every `error` other than a usage error writes the JSON described under [Outputs](#outputs) (reading 7 below), including an `error` caused by NumPy or h5py failing to import (the command line is parsed first, so the JSON's path is known) and an unexpected internal failure. The one case in which no JSON is written is a JSON path that cannot itself be written; the verdict line then says so (reading 10 below).

## Field map

`field_map.yaml` has one entry for each of the 81 datasets SAGE26 writes in a `Snap_<N>` group, in SAGE26's output order. An entry is either `uncompared` with its reason (exactly the eight datasets of rule 7), or a mapping with these keys:

| Key | Meaning |
|---|---|
| `kind` | `float` or `int`: rule 6 and the float gates of rule 8, or exact equality as 64-bit integers. |
| `mimic` | The Mimic output property, the `Galaxies` compound field. |
| `component` | For `Pos`, `Vel` and `Spin` only: the index (0, 1, 2) into Mimic's 3-element array. |
| `source` | The metadata file that declares the Mimic property: `model` (`models/sage26/model_properties.yaml`), `core` (`src/core/core_properties.yaml`) or `simulation` (`simulations/mini-millennium/halo_properties.yaml`). |
| `sage26_type`, `mimic_type` | The storage type each side must have (`int32`, `int64`, `float32`, `float64`; per element for a vector). |
| `factor` | Multiplies the Mimic value before rounding (rule 6). Exactly 1 for every `model` property. |
| `unit_check` | For `core` and `simulation` mappings: SAGE26's written unit checked against Mimic's output unit from source, with file and line citations. |
| `factor_citation` | Required only for a factor other than 1: the SAGE26 writer line and the Mimic metadata or code line that establish the conversion. |

Every factor is 1: each core and simulation unit check found SAGE26's written unit equal to Mimic's output unit. No entry remaps a sentinel or applies an offset.

The comparator's guarantee that a `sage26` package property has factor 1 rests on each entry's `source` label being correct: the comparator enforces factor 1 for every entry labelled `model`, but it does not read the metadata files, so it does not cross-check a `source` label against them. The labels were checked against `models/sage26/model_properties.yaml`, `src/core/core_properties.yaml` and `simulations/mini-millennium/halo_properties.yaml` when the map was written; whoever edits the map must keep them correct.

The comparator rejects a map, as `error`, that breaks rule 7's structure: an unknown or missing key, a type that does not suit its kind, an integer with a factor, a `model` property with a factor other than 1, a factor other than 1 without `factor_citation`, a core or simulation mapping without `unit_check`, an uncompared set other than the eight, `SnapNum` or `Type` not mapped as an `int` property, or `Posx`, `Posy` or `Posz` not mapped as a `float` property whose `sage26_type` is `float32`. It does not check the `component` index of a `Pos`, `Vel` or `Spin` entry beyond its being 0, 1 or 2.

The file is written in a strict YAML subset that the comparator parses without a YAML library: full-line comments only, two-space indentation, `format: 1` and `fields:` at the top, one block per dataset, and each value a bare token or a double-quoted JSON string. Anything outside that subset is an `error`, never a silent misreading.

## Comparison rules

1. *Reference identity.* The instrument hashes the reference file and stops with `error` unless it equals `--reference-sha256`.
2. *Candidate completeness.* The candidate directory must hold exactly the files `<basename>_000.hdf5` … `<basename>_<K−1>.hdf5` and no other file matching `<basename>_*.hdf5`; every partition must hold a `Snap<NNN>/Galaxies` table for every requested snapshot, with the same compound field names, types and shapes in every partition. A snapshot's candidate galaxies are the rows of all K partitions together, in any order. Any violation is an `error`.
3. *Snapshots.* Each requested snapshot is compared separately: SAGE26 `Snap_<N>` against Mimic `Snap<NNN>` with the same N. A requested snapshot missing on the reference side is an `error`. Every reference dataset in a compared `Snap_<N>` must be one-dimensional with the same length, and every mapped field's actual storage type and shape on each side must equal the type the field map declares for that side (a 3-element array for a vector component's Mimic property); any mismatch is an `error`. Signed integer fields of the declared types are widened losslessly to int64 for comparison; no value is ever narrowed, and no integer is converted to or from a float or an unsigned type. Every row's `SnapNum` must equal its group's N on both sides; a row that does not is an `error` (a snapshot mix-up).
4. *Matching.* Galaxies are matched one to one, per snapshot, by the key (`Posx`, `Posy`, `Posz`, `Type`): the three position components as float32 bit patterns (−0.0 read as +0.0) and `Type` as an integer. A key value occurring more than once on either side matches nothing, and its galaxies count as unmatched on both sides. A galaxy with a non-finite position component is unmatched. `Len` is not in the key (Mimic's core and SAGE26 treat orphan `Len` differently, a core difference); it is compared as an integer property. The counts of duplicate-key and non-finite-position galaxies are reported per side.
5. *Denominators.* N_ref and N_cand are the galaxy counts of the snapshot on each side; M is the number of matched pairs. Each property's within-tolerance fraction is over the M matched pairs (T1's denominator). Rule 8's second float gate uses the cohort of matched pairs where |r| > 1e-10 or |m| > 1e-10, or either value is not finite (M_sig): the pairs outside the zero band of rule 6, so a candidate cannot dilute this gate with values inside that band. M_sig excludes pairs whose reference value is finite and non-zero but within ±1e-10 (and whose candidate value is too); those pairs are still judged by rule 6's relative test within the M-pair fraction, and the second gate does not cover a sparse discrepancy confined to that band.
6. *Float properties and arithmetic.* For each mapped float property: r is the SAGE26 value as stored, widened to float64; m is the Mimic value as stored, widened to float64, multiplied by the field map's factor in float64 (rule 7), then rounded to the SAGE26 dataset's precision with round-to-nearest-even when SAGE26 stores it narrower (float32 for every SAGE26 float field except `g_max`), then widened back to float64. All later arithmetic is float64. A pair is *within tolerance* when: both are NaN; or both are the same infinity; or r = 0 and |m| ≤ 1e-10; or r is finite and non-zero, m is finite, and |m − r| ≤ 1e-6 · |r|. Otherwise (NaN or infinity on one side only, opposite infinities, a factor that overflows to infinity) it is outside. −0.0 equals +0.0. A pair is *bit-identical* when the two values, at SAGE26's stored precision, have equal bit patterns after −0.0 normalisation (two NaNs count as bit-identical only with equal bit patterns).
7. *Field map.* `field_map.yaml` gives every one of the 81 datasets SAGE26 writes at the reference's `Snap_63` exactly one entry: either a mapping (the Mimic property, a component index for `Pos`, `Vel` and `Spin`, the kind `float` or `int`, the SAGE26 dataset's storage type, the Mimic property's storage type, and a factor), or `uncompared` with its reason. The factor is exactly 1, with no sentinel or offset transformation, for every property the `sage26` package itself declares, because the package writes SAGE26's values (campaign decision D5); a factor other than 1 is allowed only for a core or simulation property whose unit differs from SAGE26's written unit, is the exact unit conversion, and cites the SAGE26 writer line and the Mimic metadata or code line that establish it. No sentinel remapping is allowed for any field. The uncompared entries are exactly these eight, with these reasons: `GalaxyIndex`, `CentralGalaxyIndex` (SAGE26 identity encodings; Mimic's `UniqueGalaxyID` and `UniqueCentralGalaxyID` encode identity differently by design, and matching uses the physical key of rule 4); `SAGEHaloIndex`, `SAGETreeIndex` (SAGE26's forest-local halo and tree numbering, which Mimic's core does not reproduce); `mergeType`, `mergeIntoID`, `mergeIntoSnapNum` (SAGE26 writes them by backpatching a galaxy's earlier output record after it merges in the following step; Mimic writes each record once and a merged galaxy leaves the output, so no counterpart exists); `g_max` (on SAGE26's default path it is written as zero for every galaxy, being assigned only by a disabled recipe). For each uncompared field the instrument reports per snapshot the number of non-zero values and the minimum and maximum; for the four whose SAGE26 creation value is fixed (`mergeType` and `g_max`: 0; `mergeIntoID` and `mergeIntoSnapNum`: −1) it also reports the number that differ from that creation value.
8. *Thresholds, per compared snapshot.* Count: |N_cand − N_ref| ≤ 0.001 · N_ref. Matching: M ≥ 0.999 · N_ref and M ≥ 0.999 · N_cand. Every compared float property: at least 90% of the M matched pairs within tolerance, **and** at least 90% of the M_sig pairs within tolerance (a property with M_sig = 0 passes this second gate). Every compared integer property (`SnapNum`, `Type`, `Len`, `Regime`, `FFBRegime`, and SAGE26's `SimulationHaloIndex` against Mimic's `MostBoundID`): at least 99.5% of the M matched pairs exactly equal, compared as 64-bit integers. Each threshold is inclusive (exactly 90% or 99.5% passes); fractions are computed as integer counts compared with the threshold times the denominator in exact rational arithmetic, never as rounded decimals.
9. *Verdict.* `pass` when every threshold holds at every compared snapshot; `fail` when any does not; `error` when the instrument cannot measure (rules 1–3, an unreadable file, a field the map names missing on either side, a reference dataset with no map entry, a map entry naming a dataset or property absent, a snapshot with M = 0). Exit status 0, 1 and 2 respectively. An `error` writes no verdict tables.
10. *T1 eligibility.* The JSON records the reference path and its SHA-256, the snapshot set, the partition count and the candidate's files with their SHA-256, and a field `t1_eligible` that is true only when the reference SHA-256 equals `T1_REFERENCE_SHA256` and the snapshot set is exactly T1's eight. A `pass` with `t1_eligible` false is printed as `pass (not T1-eligible)`.
11. *Reported.* Besides the gated fractions of rule 8 (the within-tolerance fractions over M and over M_sig and the integer exact-equality fraction, all of them also reported), the instrument reports, ungated, per snapshot and property: the bit-identical fraction, M_sig, and the median and 99th percentile of |m − r| / |r| over matched pairs with finite non-zero r and finite m (and of |m| over pairs with r = 0); for floats Mimic stores wider, `within_tolerance_unrounded`, the within-tolerance fraction computed from the float64 value before rounding. A statistic over an empty set is `null` in the JSON; non-finite values are written as the strings `"nan"`, `"inf"` and `"-inf"`. The outliers CSV lists, for every matched pair outside tolerance on any property, the snapshot, `Type`, position, `SimulationHaloIndex`, the property, r and m.
12. *Scope.* The instrument's `pass` is the numerical result of rules 1–11 only. T1's further obligations, that every galaxy outside 1e-6 is accounted for by cause, that no systematic difference is left unexplained, that the result is measured core-aligned through the approved alignment set, and the unaligned per-galaxy and population comparisons, belong to the final T1 evidence node, which uses this instrument's JSON and outliers CSV as its inputs.

## Readings settled by the PM

These points are left open by the rules above; the PM ruled on each before the comparator was written, and the comparator implements the ruling.

1. **The `|m|` statistic where r = 0 (rule 11).** It is taken over matched pairs with r = 0 **and m finite**, parallel to the relative statistic. The JSON also reports, as `pairs_nonfinite_candidate`, how many r = 0 pairs have a non-finite m.
2. **Rows of the outliers CSV (rule 11).** The CSV lists every matched pair of a float property outside rule 6's tolerance and every matched pair of an integer property that is not exactly equal, one row per pair and property. The columns are rule 11's (`snapshot`, `Type`, `Posx`, `Posy`, `Posz`, `SimulationHaloIndex`, `property`, `r`, `m`) plus `kind` (`float` or `int`). Type, position and `SimulationHaloIndex` are the reference galaxy's; `property` is the SAGE26 dataset name; `m` is the value rule 6 compares (after the factor and rounding).
3. **Percentiles (rule 11).** The median and 99th percentile are NumPy's defaults, `numpy.median` and `numpy.percentile(values, 99)`, with linear interpolation between order statistics.
4. **Denominator of `within_tolerance_unrounded` (rule 11).** It is over the M matched pairs, with the count reported.
5. **Storage type and byte order (rule 3).** A storage type is its numeric kind and width (signed integer or IEEE float, and byte count). A dataset of the declared width in either byte order is the declared type and is read losslessly.
6. **The matching key's Mimic positions (rule 4).** The key is taken from each side's value as rule 6 treats it: SAGE26's stored value, and Mimic's stored value times the factor, rounded to SAGE26's precision. The comparator stops with `error` unless the map declares `Posx`, `Posy` and `Posz` as `float32` on the SAGE26 side, so the key never narrows a stored value.
7. **What an `error` writes (rule 9).** The JSON is still written, holding the verdict, the error message and the input identities (reference path and digests, snapshots, partition count, candidate directory, and the candidate file digests and field-map digest when they were reached), but no per-snapshot tables. The outliers CSV is not written.
8. **Where the field map comes from.** The comparator reads the `field_map.yaml` in its own directory; the command line has no map option.
9. **Non-zero values of uncompared fields (rule 7).** NaN counts as non-zero.
10. **A JSON path that cannot be written (rule 9).** The run is an `error` (exit 2), no JSON is written, and the verdict line says the JSON could not be written; no outliers CSV is written either.

## Outputs

**JSON** (`--json`). Top level: `verdict`; `reference` (`path`, `sha256` as computed, `expected_sha256` as given); `snapshots`; `partitions`; `candidate` (`directory`, `basename`, and `files`, each with `name` and `sha256`); `field_map` (`path`, `sha256`); `t1_eligible`; for an error, `error`; for a pass or fail, `failures` (every failed threshold, as `Snap <N>: <name>`) and `snapshot_tables`, one per compared snapshot, holding:

- `n_ref`, `n_cand`, `matched` (M), and `sides`, per side the counts of non-finite-position galaxies, duplicate-key galaxies and unmatched galaxies (rule 4);
- `thresholds`: every rule 8 threshold with `passed` and its integer counts and denominator: `count`, `matching_vs_n_ref`, `matching_vs_n_cand`, `float:<dataset>:over_M`, `float:<dataset>:over_M_sig`, `int:<dataset>`; and `failed_thresholds`, `passed`;
- `properties`, per mapped dataset: for a float, `within_tolerance` and its fraction over M, `M_sig`, `within_tolerance_sig` and its fraction over M_sig, `bit_identical` and its fraction, `relative_difference` (`pairs`, `median`, `p99`), `abs_candidate_where_reference_zero` (`pairs`, `pairs_nonfinite_candidate`, `median`, `p99`), `within_tolerance_unrounded` and its fraction where Mimic stores the property wider than SAGE26, and `passed_over_M`, `passed_over_M_sig`, `passed`; for an integer, `exact_equal`, `bit_identical` (the same count) and their fractions, and `passed`;
- `uncompared`, per uncompared dataset over the snapshot's reference galaxies: `nonzero`, `min`, `max`, and for `mergeType`, `g_max`, `mergeIntoID` and `mergeIntoSnapNum` the `creation_value` and `differ_from_creation`.

Fractions are reported as decimals for reading; the gates are decided only on the integer counts (rule 8). A statistic over an empty set is `null`; non-finite values are the strings `"nan"`, `"inf"` and `"-inf"`.

**Outliers CSV** (`--outliers`), written only for a pass or fail. The outputs are written transactionally: the CSV goes first to a temporary file beside `--outliers` (`.<name>.<random>.tmp` in the same directory), the JSON is written next, and only then is the temporary file renamed onto `--outliers`. On any error, including a failure to write the JSON or to rename, the temporary file is removed and the JSON says `error`, so an `error` run never creates or replaces a file at `--outliers` and any older file there is left untouched. A CSV therefore belongs to a run only when the JSON that run wrote says `pass` or `fail`. The file holds one row per matched pair and property outside tolerance (reading 2 above), ordered by snapshot as requested, then by the field map's order, then by the reference row. Floats are written as the shortest decimal that round-trips the float64 value, and non-finite values as `nan`, `inf` and `-inf`. On a run far from parity the file is large (about 570 MB for the `sage26` package before any SAGE26 physics is ported).

## Scope

The instrument's `pass` is the numerical result of rules 1–11 only (rule 12). T1's further obligations, that every galaxy outside 1e-6 is accounted for by cause, that no systematic difference is left unexplained, that the result is measured core-aligned through the approved alignment set, and the unaligned per-galaxy and population comparisons, belong to the final T1 evidence node, which uses this instrument's JSON and outliers CSV as its inputs.

The instrument compares output only. It does not read Mimic's or SAGE26's parameters, and it never corrects a difference: a core difference such as the particle mass (mini-Millennium's `particle_mass` is 0.0860657 in Mimic's `simulations/mini-millennium/simulation_info.yaml` and 0.086 in the T1 reference's header) shows up as an outlier, for the alignment set or the evidence node to explain.
