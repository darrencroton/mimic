# Event System Implementation Code Review

Date: 2026-03-18
Repository: `mimic`
Scope: Uncommitted implementation of the approved event-system plan in `docs/EVENT-SYSTEM-IMPROVEMENTS.md`

## Verdict

The implementation is close to the intended architecture and is a strong step forward. It generalises the merger-shaped event path into metadata-owned, producer-scoped infrastructure, keeps the input YAML simple, records event wiring in output metadata, and preserves the current SAGE merger workflow cleanly. That aligns well with `docs/VISION.md`, especially Principles 1, 3, 5, and 8, and it is much better on KISS and DRY than the original broadcast-plus-hand-filtering design.

I would not commit it unchanged yet.

The core design is good, and the changed validation surface passed, but I found three issues worth fixing first:

1. one correctness gap that can silently accept bad producer emissions
2. one architecture/scope leak where new synthetic test modules are shipped as normal runtime modules
3. one meaningful test-coverage gap relative to the approved plan

## Findings

### [P1] Producer-side event contract violations are still accepted silently

The new routing model validates consumers well, but it does not validate producers at emit time.

- `execute_phase()` enables emission for every `process_full_halo` module and sets `phase_event_state.current_producer_module_id = mod->module_id` even when `module_id` is `0` for non-producers in `src/core/module_registry.c:742-747`.
- `module_emit_event()` then writes `producer_module_id` and `event_id` into the queued event without checking whether the current module actually declared `events.emits`, or whether the supplied `event_id` is valid for that producer, in `src/core/module_registry.c:687-699`.
- The generator produces per-producer enums and consumer subscription tables, but not producer-side emitted-event lookup/count tables that would let the core fail fast on bad emissions. See `scripts/generate_module_registry.py:652-685` and `scripts/generate_module_registry.py:955-1006`.

Impact:

- A `process_full_halo` module that accidentally calls `module_emit_event()` without declaring `events.emits` will emit with `producer_module_id == 0`.
- A producer can also pass an arbitrary numeric `event_id`.
- In both cases, the event will usually just not match any subscription, so the failure mode is "silent wrong physics" rather than "clear startup/runtime error".

That is out of step with the approved plan and with Vision Principle 8 (fast failure and validation).

Recommendation:

- Generate producer-side emitted-event metadata.
- Make `module_emit_event()` reject:
  - `current_producer_module_id == 0`
  - `event_id <= 0`
  - any `(producer_module_id, event_id)` pair not declared in metadata

### [P2] The new synthetic event modules are test-only in intent, but runtime modules in practice

The plan explicitly called for dedicated sibling **test-only** modules under `_system`, but the implementation currently exposes them as normal runtime modules.

- They are compiled into normal builds in `Makefile:23-26`.
- They are registered in every build through `register_all_modules()` in `src/modules/_system/generated/module_init.c:517-519`.
- The plan describes them as test-only in `docs/EVENT-SYSTEM-IMPROVEMENTS.md:423-429`.

At the same time, the repository's validation/test tooling still treats `_system` specially:

- `scripts/validate_modules.py` skips underscore directories entirely in `scripts/validate_modules.py:165-170`.
- `scripts/generate_test_registry.py` has a special case for `_system/test_fixture` only, not the new event modules, in `scripts/generate_test_registry.py:120-130`.

Impact:

- Production builds now advertise infrastructure test modules as if they were part of the normal model-builder surface.
- The module-validation and test-registry tooling no longer agrees with the runtime registry about which modules exist.
- That is a DRY / single-source-of-truth leak and makes the runtime surface noisier than the plan intended.

Recommendation:

- Preferred: mark the new synthetic event modules as `is_utility: true` and keep them out of production registration.
- If they are intentionally meant to remain runtime-visible, then update validation/test-discovery tooling so the whole repository consistently treats them as first-class runtime modules.

### [P2] The dedicated generic routing tests promised by the plan were not actually landed

The implementation adds synthetic event modules, but not the synthetic routing test coverage the plan asked for.

The plan called for:

- no delivery to unsubscribed consumers
- one producer, two consumers, different subscriptions
- multiple events from one producer
- multiple producers in one phase

See `docs/EVENT-SYSTEM-IMPROVEMENTS.md:444-459`.

What landed instead:

- `test_event_consumer_alpha` and `test_event_consumer_beta` both subscribe to the same producer/event pair in:
  - `src/modules/_system/test_event_consumer_alpha/module_info.yaml:18-21`
  - `src/modules/_system/test_event_consumer_beta/module_info.yaml:17-20`
- The updated generic integration test only checks startup rejection for:
  - missing subscriptions
  - invalid processing mode
  in `tests/integration/test_processing_modes.py:288-401`
- I did not find a unit or integration test that actually runs `test_event_producer` plus the synthetic consumers to prove routing behavior end-to-end.

Impact:

- The new routing core is being exercised mainly through the SAGE pathway rather than through the generic synthetic harness that was added specifically to validate reusable infrastructure.
- That leaves important regression cases unproven, especially unsubscribed suppression and multi-producer routing.

Recommendation:

- Add at least one dedicated synthetic routing integration test before commit.
- Extend the synthetic metadata so the test surface can express:
  - one subscribed consumer
  - one unsubscribed consumer
  - more than one producer
  - more than one event from a single producer

## Positive Assessment

The main design choices are good.

- Event identity is now producer-scoped in `src/core/module_interface.h:141-178`.
- Consumer contracts are metadata-owned and validated in `scripts/generate_module_registry.py:489-586`.
- Runtime dispatch is filtered by resolved subscriptions in `src/core/module_registry.c:561-643`.
- SAGE modules now consume merger events without in-module event-code filtering:
  - `src/modules/sage_quasar_mode/sage_quasar_mode.c:70-99`
  - `src/modules/sage_starburst_feedback/sage_starburst_feedback.c:221-263`
- Output metadata now records resolved event wiring in `src/io/output/hdf5.c:843-947`.
- Developer and user docs were updated in a way that keeps the model-builder story simple.

Against the requested criteria:

- `VISION.md`: strong overall fit, especially Principles 1, 3, 5, and 8
- KISS: much simpler than the earlier YAML-routing-plus-pointer-payload direction
- DRY: materially improved because contracts now live in metadata rather than being duplicated between C and input YAML

## Validation Performed

I ran the following checks against the current worktree:

- `make check-generated` -> passed
- `make validate-modules` -> passed
- `make test-unit` -> passed
- `mimic_venv/bin/python3 tests/integration/test_processing_modes.py` -> passed
- `mimic_venv/bin/python3 src/modules/sage_resolve_mergers_and_disruption/_tests/test_integration_sage_merger_event_consumers.py` -> passed

Note:

- The plain system `python3` environment did not have `numpy`, so the Python integration tests were rerun successfully with `mimic_venv/bin/python3`.

## Recommended Pre-Commit Changes

1. Add producer-side emit validation so bad event emissions fail fast instead of disappearing silently.
2. Decide whether the synthetic event modules are truly test-only or truly runtime modules, then make build/validation/test tooling consistent with that choice.
3. Land at least one dedicated synthetic routing test that matches the plan's intended coverage.

## Overall Assessment

This is a good implementation direction and most of the hard architectural choices are right. The event system is substantially cleaner, more general, and more aligned with Mimic's purpose than the old merger-shaped broadcast system.

Once the three issues above are addressed, I would be comfortable with this being the new baseline.
