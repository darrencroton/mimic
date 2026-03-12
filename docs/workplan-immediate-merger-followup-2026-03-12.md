# Workplan: Immediate Merger Handler — Post-Sign-off Follow-Up

Date: 2026-03-12
Status: Authoritative follow-up plan after sign-off blockers were completed
Branch base: `codex/merger-pathway-trace` (commit `53fb190`)
Audience: Dev team familiar with Mimic module/event architecture

## Current status

The split-pass merger pathway has been removed. `sage_handle_mergers_immediate`
is now the sole live SAGE merger path, and the pre-sign-off blockers identified
in the review have been completed by a separate sign-off team on 2026-03-12.

Completed sign-off work:
- `sage_handle_mergers_immediate` is now a proper metadata-driven directory
  module and advertises only `process_full_halo`.
- The runtime merger event chain now has explicit end-to-end integration
  coverage, including a negative configuration test for invalid processing mode.

Verification already completed for the current tree:
- `make check-generated`
- `make validate-modules`
- `make test-unit`
- `make test-integration`
- Quiet Millennium-pipeline smoke run using a redirected config under `ignore/`

This file is now the single authoritative plan for the remaining work after
sign-off. The completed sign-off items are retained at the bottom for
traceability only; they require no further action.

Primary sources:
- `src/modules/sage_handle_mergers_immediate/sage_handle_mergers_immediate.c`
- `src/modules/sage_handle_mergers_immediate/module_info.yaml`
- `src/modules/_shared/sage_merger_ops.h`
- `src/modules/sage_collisional_starburst.c`
- `src/modules/sage_quasar_mode.c`
- `src/modules/sage_disk_instability.c`
- `src/core/module_registry.c`
- `scripts/generate_module_registry.py`
- `sage-code/sage/model_mergers.c`
- `sage-code/sage/model_disk_instability.c`
- `input/millennium.yaml`

---

## Remaining Post-Sign-off Work

### Task 1: Post-Minor-Merger Disk Instability Recheck

#### Why it remains

After a minor-merger starburst, SAGE immediately rechecks the merger remnant for
disk instability inside `collisional_starburst_recipe()`:

```c
if (SageConfig.DiskInstabilityOn && mode == 0)
    if (mass_ratio < SageConfig.ThreshMajorMerger)
        check_disk_instability(merger_centralgal, centralgal, halonr, time, dt, step);
```

Mimic still does not reproduce that exact post-starburst follow-up in the merger
event path. `sage_collisional_starburst` handles merger-triggered starbursts in
`phase_2`, but disk instability currently runs only once in `phase_1`.

This is a real SAGE-fidelity gap, but it is not a sign-off blocker now that the
core immediate merger pathway is correct, protected by metadata, and covered by
automated event-chain tests.

#### Important revision to the earlier plan

The original workplan wording was too narrow. In SAGE, `check_disk_instability()`
does not only detect instability. It also drives the downstream physics in that
same context, including black-hole growth and collisional starburst behavior.

That means the Mimic follow-up should not merely "re-run `sage_disk_instability`"
or toggle `UnstableDiskGasFraction` after a merger starburst. Reusing the
phase-1 trigger flag across phases would be the wrong abstraction and would make
the design harder to reason about.

#### Recommended implementation direction

Keep this entirely in the module layer.

Preferred approach:
1. Extract the shared disk-instability calculation and the immediately coupled
   downstream physics needed in the merger context into a reusable helper in
   `src/modules/_shared/`.
2. In the merger-event path, after `sage_collisional_starburst` finishes a
   minor-merger starburst, run the post-starburst instability recheck directly
   on the live remnant when the relevant modules are configured.
3. Apply the same downstream consequences SAGE applies in that context, rather
   than trying to defer them to the next timestep or pass them through the
   phase-1 `UnstableDiskGasFraction` trigger path.

Avoid:
- Adding SAGE-specific scheduler logic to `src/core/`
- Reusing `sage_clear_disk_instability_triggers` as part of the fix
- Creating a cross-phase flag just to mimic a direct same-step physics call

#### Validation approach

Use the new immediate-merger event integration harness as a base, then add a
focused deterministic fixture where:
- a minor merger happens
- the merger starburst changes the remnant enough to cross the instability
  threshold
- the expected post-merger instability response is unambiguous

Broad Millennium statistics can still be used as a secondary smoke check, but
they should not be the primary acceptance mechanism.

#### Acceptance

- Mimic performs the post-minor-merger instability recheck under the same
  conditions as SAGE's merger-channel logic.
- The merger remnant receives the correct downstream instability physics in the
  same timestep.
- A deterministic automated test proves the behavior.
- Existing unit and integration suites remain green.

---

### Task 2: Audit Remaining Standalone Module Processing-Mode Metadata

#### Why it should be added

The immediate merger handler bug came from the generator default for standalone
`.c` modules:

```python
"supported_processing_modes": [
    "process_full_halo",
    "process_per_event",
    "process_by_galaxy",
]
```

That default still applies to the remaining standalone modules in `src/modules/`.
Several of those modules clearly enforce a narrower runtime contract in code. For
example:
- `sage_disk_instability` requires `ngal == 1`
- `sage_clear_disk_instability_triggers` requires `ngal == 1`
- `sage_calculate_star_formation` requires `ngal == 1`
- `sage_calculate_supernova_feedback` requires `ngal == 1`
- `sage_quasar_mode` supports a specific dual-mode contract (`process_by_galaxy`
  and `process_per_event`), not all three modes
- `sage_collisional_starburst` also supports a specific dual-mode contract

This is broader than the merger handler and should be cleaned up deliberately,
but it does not have to block sign-off for the merger-pathway work.

#### Recommended implementation direction

1. Audit every remaining standalone runtime module in `src/modules/`.
2. For each module, declare the exact supported processing modes in metadata.
3. Prefer proper directory modules with `module_info.yaml` where that is the
   cleanest option.
4. If the team chooses to generalize the generator instead, keep it metadata-
   driven and explicit. Do not add ad hoc inline overrides.
5. Add negative configuration tests where the supported-mode contract is easy to
   regress and high-value to protect.

#### Acceptance

- Every runtime module advertises only the modes it actually supports.
- Startup validation rejects invalid processing-mode configurations before
  execution.
- No duplicate or fragmented test registration is introduced during the cleanup.
- `make validate-modules`, `make test-unit`, and `make test-integration` stay
  green throughout.

---

## Completed Sign-off Work by Separate Team (2026-03-12)

These items were originally part of the follow-up list. They have already been
completed in the current branch and are retained here only as implementation
record.

### Completed Task A: Tighten Module Registration for `sage_handle_mergers_immediate`

Delivered:
- Promoted `sage_handle_mergers_immediate` into
  `src/modules/sage_handle_mergers_immediate/`
- Added `module_info.yaml` with explicit
  `supported_processing_modes: [process_full_halo]`
- Added module-owned test/doc metadata
- Regenerated `module_init.c` and test source metadata

Behavioral result:
- `sage_handle_mergers_immediate` now advertises only
  `PROCESSING_MODE_FULL_HALO`
- Invalid YAML configuration such as `process_by_galaxy` is rejected at startup
  before execution begins

### Completed Task B: Integration-Level Event Consumer Test

Delivered:
- Added `src/modules/sage_handle_mergers_immediate/_tests/test_integration_sage_merger_event_consumers.py`
- Registered the test through the module's own metadata, avoiding duplicate
  central registration
- Covered three production-path assertions:
  - invalid processing mode is rejected
  - phase-2 merger events reach `sage_quasar_mode`
  - phase-2 merger events reach `sage_collisional_starburst`

Validation used:
- deterministic temporary configs based on the standard test dataset
- like-for-like control runs with the relevant phase-2 consumer disabled
- comparisons on common merger remnants using `UniqueGalaxyID`

---

## Recommended Execution Order

1. Task 1: post-minor-merger disk-instability parity
2. Task 2: remaining standalone module metadata audit
