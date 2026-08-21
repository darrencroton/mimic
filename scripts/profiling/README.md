# CPU profiling harness

Component-level CPU attribution for a Mimic run, along the architectural boundaries in [`docs/VISION.md`](../../docs/VISION.md). This is the tooling that produced [`docs/dev/BENCHMARK-SAGE16-MINI-MILLENNIUM.md`](../../docs/dev/BENCHMARK-SAGE16-MINI-MILLENNIUM.md), committed so that the "re-measure before acting" instruction in [`docs/dev/OPTIMISATION-SPECTRUM.md`](../../docs/dev/OPTIMISATION-SPECTRUM.md) is actually executable.

**Platform: macOS only.** Attribution depends on `/usr/bin/sample`, whose call-graph output carries the `file.c:LINE` annotations the whole method rests on. There is no Linux path here; see [Re-measuring on Linux](#re-measuring-on-linux).

**Nothing here modifies tracked files or rebuilds the binary.** It profiles whatever `./mimic` currently is. The one file it writes into the working tree is the gitignored `scripts/profiling/nm_index.json`.

---

## Preconditions

1. **Check the input data actually loads.** `simulations/<sim>/snapshots` is a gitignored symlink. If it points somewhere wrong the run still exits 0, processes zero trees, and finishes in ~0.08 s — a silent no-op that looks like a fast run. `run_wall.py` warns when a repeat reports no galaxy-pool high-water, which is the symptom; confirm a real run before trusting any timing.
2. **Build with symbols.** The default `make` already uses `-g -O2`, which is what the published numbers describe. Record any deviation.
3. **Serialise everything.** Never run two `mimic` processes at once, and do not use the machine for anything else while measuring.

## Scripts

| Script | Role |
|---|---|
| `run_wall.py <run.yaml> <repeats> <label> [-o out.json]` | Serial wall-clock timer (`time.perf_counter` around `subprocess.run`). Aborts on a non-zero exit, and warns when a run reports no galaxy-pool high-water |
| `sample_runs.sh <run.yaml> <repeats> <outdir>` | Serialised sampled-profile collection. Starts `/usr/bin/sample -wait mimic` *before* launching the run so the attach covers startup. Aborts on a failed run, a failed sampler, a report with no call graph, or an output directory that already holds reports |
| `parse_sample.py <sample.txt>` | Parses `sample`'s `Call graph:` section — the `+ ! : \|` gutter, collapsed multi-address leaf frames, and `file.c:LINE` annotations. Builds the tree from the indent column and computes SELF = count − Σ(direct children). Run directly, it self-checks one report |
| `nm_index.py [-o out.json]` | `nm -U` over `build/obj/**/*.o` → symbol-to-translation-unit map for frames `sample` leaves unannotated. Indexes only defined text symbols, and omits symbols defined in more than one translation unit rather than guessing |
| `attribute.py <sample_dir> <out.json> [options]` | Assigns every self-sample to exactly one component and writes per-component, per-line, libm-payer and inclusive aggregates |

`REPO` is derived from the script location; override with the `MIMIC_REPO` environment variable.

## Use

```bash
python3 scripts/profiling/nm_index.py
scripts/profiling/sample_runs.sh ./models/sage16/input/sage16_mini-millennium.yaml 20 /tmp/samples
python3 scripts/profiling/run_wall.py ./models/sage16/input/sage16_mini-millennium.yaml 15 baseline \
    -o /tmp/wall.json
python3 scripts/profiling/attribute.py /tmp/samples /tmp/agg.json \
    --skip-first --wall-mean-s <measured_wall_mean_seconds>
```

`nm_index.py` writes to `scripts/profiling/nm_index.json` by default, which is exactly where `attribute.py` looks for it, so the two agree without being told. The index is a machine-local build artifact and is gitignored. Without it, frames the sampler could not annotate stay Unattributed, and `attribute.py` says so on stderr.

Pass the **sampler-free** wall mean to `--wall-mean-s`, computed over `run_wall.py`'s repeats *excluding* repeat 0, which reads the tree files before the OS file cache is warm. The realised sampling interval is coarser than the nominal 1 ms (~1.27 ms in the published run, because deep stacks slow the sampler), so percentages must be converted to milliseconds using measured wall time, never by multiplying sample counts by the nominal interval.

`--skip-first` drops the first sampled report for the same reason. The published run discarded the warm-up on both sides; say so whenever you report numbers.

`sample_runs.sh` refuses an output directory that already holds `sample_*.txt`, because `attribute.py` aggregates every report it finds and leftovers from a shorter previous collection would be silently mixed into the new profile. Move the old reports aside rather than deleting them.

## Attribution rules

Source-annotated frames are attributed by repository-relative source path to the VISION-level owner. Unannotated frames: `libsystem_m` → Math library, plus a secondary table naming the component that *called* it; `libhdf5` and syscalls beneath it → the I/O component that reached them; other kernel syscalls → the nearest source-annotated ancestor; platform, allocator and dyld frames → Runtime/system. Anything left goes to an explicit `Unattributed` bucket and is never folded into a named component.

## Trusting the result

The way this harness fails is not a crash. Each known failure produces a complete, credible-looking profile that is wrong, so `attribute.py` checks for them all and exits non-zero:

- **Source paths that do not sit under `REPO`.** Attribution dispatches on a path prefix, so a `REPO` mismatch sends every annotated frame to the coarser symbol-index fallback — which still resolves most frames by name, and still prints a plausible table. The `source_attribution.mapped_fraction` field records what actually mapped; below 90% the run fails and names the paths it could not place.
- **Reports with no `file:line` annotations at all**, which is what profiling a binary built without `-g` looks like. The mapped-fraction check has no premise to test there, so the absence is itself the failure.
- **A large `Unattributed` share**, meaning whole binaries matched no rule. Configurable with `--max-unattributed-pct`.

The symbol index is deliberately *not* fingerprinted against the binary. On a `-g` build it resolves essentially nothing — measured at 0.000% of self-samples on the published run, because `sample` annotates every frame the profile actually spends time in — so a stale index has no material blast radius. What matters is that annotations are present at all, which the second check above enforces directly.

## Interpreting the result

- **Per-component totals are trustworthy; per-line attribution inside a translation unit is not.** `-O2` folds static helpers into their enclosing function, so a hot "line" may be absorbing neighbouring inlined work. Confirm with a counter before optimising a specific line.
- **Inclusive figures use an outermost-occurrence rule** so recursion is not multiply-counted, and must not be summed with their callees.
- Sampling overhead was measured at +8.5% in the published run. It lands in the sampler, but stack-walk pauses are not uniform in call depth, so deeply recursive frames may be mildly over-represented.
- Warm file cache is the default after the first run. Discard the first repeat with `--skip-first` and say so.

## Re-measuring on Linux

`sample` does not exist there. The nearest equivalent is `perf record -g --call-graph dwarf` plus `perf report --no-children` for self-time, which gives per-symbol attribution; mapping symbols to translation units still works via `nm_index.py` only if its `nm -U` type letters are reinterpreted for ELF, and `perf` does not emit the same `file:line` annotations without `--source`, so the line-level table is not directly reproducible. Treat cross-platform comparisons of *component shares* as valid and line-level comparisons as not.
