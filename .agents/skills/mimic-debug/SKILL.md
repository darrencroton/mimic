---
name: mimic-debug
description: Debugging Mimic runtime issues — module startup failures, crashes, memory leaks, unexpected output, or stale generated code. Load when diagnosing any runtime or build-time problem.
---

# Mimic Debug

## First Steps (Always)

```bash
# 1. Validate module metadata before building — catches most startup failures
make validate-modules

# 2. If metadata changed, regenerate before building
make generate && make

# 3. After any change to module_info.yaml, property YAML, or generated code:
make clean && make    # stale generated code is a common silent failure cause
```

## Capturing Full Diagnostic Output

```bash
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml 2>&1 | tee /tmp/mimic-debug.log
```

`--debug` enables `DEBUG_LOG` output (most verbose). `--verbose` adds timestamp and file:line context. Pipe through `tee` to retain the log.

## Module Startup Failures

| Symptom | Likely cause | Fix |
|---|---|---|
| "Unknown module" at startup | module name in YAML doesn't match `module.name` in `module_info.yaml` | Align names |
| "Invalid processing mode" | `supported_processing_modes` value misspelled or wrong | Check template for valid values |
| "Unknown property" | property listed in `dependencies.properties` not defined | Add to the appropriate property YAML and regenerate |
| "Invalid compilation requirement" | lowercase feature name in `compilation_requires` | Use uppercase: `HDF5`, `MPI`, `GSL` |
| Stale generated code errors | generated files out of sync with metadata | `make clean && make generate && make` |

## Memory Debugging

The custom allocator tracks five categories: `MEM_GALAXIES`, `MEM_HALOS`, `MEM_TREES`, `MEM_IO`, `MEM_UTILITY`.

```bash
# Check for leaks during a debug run (allocator reports on cleanup)
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml 2>&1 | grep -i "leak\|alloc\|free"

# Valgrind (slower but definitive)
valgrind --leak-check=full --track-origins=yes \
  ./mimic models/sage16/input/sage16_mini-millennium.yaml
```

Module-owned allocations via `mymalloc_cat(size, MEM_GALAXIES)` must be paired with `myfree(ptr)` in `cleanup()`. The allocator flags any category with outstanding allocations at shutdown.

For `consistent_trees_hdf5` runs, expect `MEM_IO` to include the fixed `CTREES_READ_WINDOW_BYTES` slab window (`128 MiB` per rank) plus task-range `ForestInfo` and HDF5 field-cache scaffolding. A debug run should still report zero outstanding allocations after `close_partition_ctrees_hdf5()` frees the read window, field cache, and `ForestInfo` cache.

## OutputBuffer

The `OutputBuffer` must be heap-allocated (`mymalloc_cat`) — never stack. Writing to a stack-allocated buffer produces silent corruption that only appears when the stack frame is reused.

## ProcessedHalos Growth

`ProcessedHalos` accumulates entries across the run including orphans; `MAXHALOFAC` is an estimate, not a hard cap. Seeing the array grow through a long run is expected behaviour, not a leak.

## Unexpected Physics Output

1. Check `make validate-modules` — property dependency mismatches cause silent zero-initialisation.
2. Check `module.dependencies.parameters` — a missing parameter name means the validator won't catch a missing YAML entry, and `model_get_double()` will return -1.
3. For SAGE parity failures, compare against the baseline: `tests/data/output/baseline/`. A diff here is a real physics regression unless the baseline was intentionally regenerated.
4. Check phase ordering in the input YAML — `modules.phases:` order is user-controlled and not validated for scientific correctness.
