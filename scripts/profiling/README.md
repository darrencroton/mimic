# CPU profiling harness

Component-level CPU attribution for a Mimic run, along the architectural boundaries in [`docs/VISION.md`](../../docs/VISION.md). This is the tooling that produced [`docs/dev/BENCHMARK-SAGE16-MINI-MILLENNIUM.md`](../../docs/dev/BENCHMARK-SAGE16-MINI-MILLENNIUM.md), committed so that the "re-measure before acting" instruction in [`docs/dev/OPTIMISATION-SPECTRUM.md`](../../docs/dev/OPTIMISATION-SPECTRUM.md) is actually executable.

**Platform: macOS only.** Attribution depends on `/usr/bin/sample`, whose call-graph output carries the `file.c:LINE` annotations the whole method rests on. There is no Linux path here; see [Re-measuring on Linux](#re-measuring-on-linux).

**Nothing here modifies the repository or rebuilds the binary.** It profiles whatever `./mimic` currently is.

---

## Preconditions

1. **Check the input data actually loads.** `simulations/<sim>/snapshots` is a gitignored symlink. If it points somewhere wrong the run still exits 0, processes zero trees, and finishes in ~0.08 s — a silent no-op that looks like a fast run. Confirm a real run first: the default pair should report a non-zero galaxy-pool high-water in its run profile.
2. **Build with symbols.** The default `make` already uses `-g -O2`, which is what the published numbers describe. Record any deviation.
3. **Serialise everything.** Never run two `mimic` processes at once, and do not use the machine for anything else while measuring.

## Scripts

| Script | Role |
|---|---|
| `run_wall.py <run.yaml> <repeats> <label> [out.json]` | Serial wall-clock timer (`time.perf_counter` around `subprocess.run`); records per-repeat wall time, return code, and peak RSS |
| `sample_runs.sh <run.yaml> <repeats> <outdir>` | Serialised sampled-profile collection. Starts `/usr/bin/sample -wait mimic` *before* launching the run so the attach covers startup. Aborts on any non-zero exit |
| `parse_sample.py <sample.txt>` | Parses `sample`'s `Call graph:` section — the `+ ! : \|` gutter, collapsed multi-address leaf frames, and `file.c:LINE` annotations. Builds the tree from the indent column and computes SELF = count − Σ(direct children) |
| `nm_index.py` | `nm -Uj` over `build/obj/**/*.o` → symbol-to-translation-unit map, for frames `sample` leaves unannotated |
| `attribute.py <sample_dir> <out.json> [wall_mean_s]` | Assigns every self-sample to exactly one component and writes per-component, per-line, libm-payer and inclusive aggregates |

`REPO` is derived from the script location; override with the `MIMIC_REPO` environment variable.

## Use

```bash
python3 scripts/profiling/nm_index.py > /tmp/nm_index.json
scripts/profiling/sample_runs.sh ./models/sage16/input/sage16_mini-millennium.yaml 20 /tmp/samples
python3 scripts/profiling/run_wall.py ./models/sage16/input/sage16_mini-millennium.yaml 15 baseline /tmp/wall.json
python3 scripts/profiling/attribute.py /tmp/samples /tmp/agg.json <measured_wall_mean_seconds>
```

Pass the **sampler-free** wall mean to `attribute.py`. The realised sampling interval is coarser than the nominal 1 ms (~1.27 ms in the published run, because deep stacks slow the sampler), so percentages must be converted to milliseconds using measured wall time, never by multiplying sample counts by the nominal interval.

## Attribution rules

Source-annotated frames are attributed by source path to the VISION-level owner. Unannotated frames: `libsystem_m` → Math library, plus a secondary table naming the component that *called* it; `libhdf5` and syscalls beneath it → Output I/O; other kernel syscalls → the nearest source-annotated ancestor; platform, allocator and dyld frames → Runtime/system. Anything left goes to an explicit `Unattributed` bucket and is never folded into a named component.

## Interpreting the result

- **Per-component totals are trustworthy; per-line attribution inside a translation unit is not.** `-O2` folds static helpers into their enclosing function, so a hot "line" may be absorbing neighbouring inlined work. Confirm with a counter before optimising a specific line.
- Sampling overhead was measured at +8.5% in the published run. It lands in the sampler, but stack-walk pauses are not uniform in call depth, so deeply recursive frames may be mildly over-represented.
- Warm file cache is the default after the first run. Discard the first repeat and say so.

## Re-measuring on Linux

`sample` does not exist there. The nearest equivalent is `perf record -g --call-graph dwarf` plus `perf report --no-children` for self-time, which gives per-symbol attribution; mapping symbols to translation units still works via `nm_index.py`, but `perf` does not emit the same `file:line` annotations without `--source`, so the line-level table is not directly reproducible. Treat cross-platform comparisons of *component shares* as valid and line-level comparisons as not.
