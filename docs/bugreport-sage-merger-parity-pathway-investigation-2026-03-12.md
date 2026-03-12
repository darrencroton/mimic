# Bug Report: SAGE Merger Parity Pathway Produces Only Minimal Full-Run Differences

Date: 2026-03-12
Status: Runtime pathway traced
Branch: `codex/merger-pathway-trace`

## Summary

We added a parity-preserving SAGE merger/disruption pathway to Mimic so that
mergers and disruptions are handled immediately, in-loop, instead of through
the existing split three-module path. The expectation was that this change
would create a noticeable divergence in full Millennium runs if merger
ordering was a major source of Mimic vs SAGE differences.

That did not happen.

The opt-in parity profile runs successfully, uses the intended immediate
handler, and produces output files that differ from the default profile.
However, the observed differences are extremely small and sparse relative to
expectation.

## What Has Already Been Done

1. Built deterministic parity fixtures and trace helpers.
2. Wrote a parity contract from vendored SAGE source behavior.
3. Implemented `sage_handle_mergers_immediate` as a single-pass parity module.
4. Kept the existing split path intact:
   - `sage_update_merger_time`
   - `sage_merge_galaxies`
   - `sage_disrupt_satellites`
5. Added an opt-in runtime profile:
   - `input/millennium-merger-parity.yaml`
6. Verified the parity profile runs successfully and that the default profile
   remains unchanged.

## Current Observation

Comparing the default Millennium run against the parity-profile Millennium run:

- The output HDF5 files are not identical.
- The parity-profile HDF5 metadata confirms that
  `sage_handle_mergers_immediate` is active in the new run.
- Differences appear only in later snapshots and only in a small number of
  galaxies.
- Broad diagnostic plots are visually almost unchanged.

This is surprising because a meaningful change in merger ordering was expected
to cascade more strongly through galaxy growth histories.

## Working Hypotheses

1. The old and new pathways may still be functionally converging for almost all
   real Millennium cases, despite the fixture-level parity difference.
2. The immediate pathway may be active, but some critical routing or event
   dispatch step may not be happening as expected in the full runtime path.
3. The ordering-sensitive cases may be much rarer in Millennium than expected,
   though current intuition argues against that.

## Immediate Investigation Goal

Add explicit, pathway-specific logging to both the split merger path and the
immediate parity path, then run both configurations and inspect the logs to
confirm:

1. Which merger-processing pathway is executing.
2. Which satellites are considered eligible.
3. Which targets are resolved.
4. Whether mergers vs disruptions are being decided as expected.
5. Whether per-event merger consumers are being reached from the expected path.

## Success Criteria For This Investigation

- Runtime logs show the old path and new path entering different code routes.
- The logs include enough information to trace the first few relevant
  merger/disruption decisions in each run.
- We can explain whether the minimal output difference is expected behavior or
  evidence of a bug in pathway execution.

## Trace Findings

Temporary runtime tracing was added and exercised with separate debug runs for
the old split pathway and the new immediate pathway.

### What the logs confirmed

1. The default profile really does execute the old split path:
   - `sage_update_merger_time`
   - `sage_merge_galaxies`
   - `sage_disrupt_satellites`

2. The parity profile really does execute the new immediate path:
   - `sage_handle_mergers_immediate`

3. In both runs, merger events are emitted and the per-event consumers are
   reached:
   - `sage_quasar_mode`
   - `sage_collisional_starburst`

4. The first traced real merger/disruption cases line up closely between the
   two pathways. The source satellites, targets, substeps, and merger ratios
   seen in the early runtime traces are effectively the same.

### Aggregate counts from the full debug runs

Old split path:
- flagged merges: 24517
- flagged disruptions: 21159
- executed merges: 24517
- executed disruptions: 21159

Immediate parity path:
- executed merges: 24517
- executed disruptions: 21159
- redirects: 0

## Current Interpretation

This strongly suggests the parity module is not being skipped or miswired.
It is running, and it is producing the same aggregate number of merger and
disruption actions as the old path on the Millennium configuration used here.

The most important current result is `redirects=0` in the traced parity run.
That means the immediate one-hop consumed-target redirection logic, which was a
major suspected source of divergence, did not trigger at all in this full run.

That does not prove the two pathways are mathematically identical, because the
HDF5 outputs still differ slightly. But it does explain why the differences are
so small: the full Millennium run, at least under this configuration, is not
hitting the runtime situations that were expected to create large pathway
divergence.

## Revised Status

The current issue is no longer “is the parity pathway active?”

It is active.

The current issue is now:

Why does the real Millennium run produce so few ordering-sensitive cases, and
why does the immediate parity logic appear to collapse back to nearly the same
behavior as the split path for almost all events?

Note: the temporary trace instrumentation used for this investigation was later
removed from the committed branch after the findings were captured in the debug
logs under `ignore/test-logs/`.
