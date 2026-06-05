# CentralMvir Semantics — Current Behaviour and a Deferred Option

**Status:** Reference note for a future science decision. No action required for v1.0.
**Date:** 2026-06-06
**Owner concept:** the `CentralMvir` halo property (`src/core/core_properties.yaml`).

---

## Why this note exists

During the dual-driver Phase 3 work (driver-neutral output buffering), `CentralMvir` changed from an output-only, tree-indexed field into a real `struct Halo` field stamped by the driver before physics. That change was necessary and is behaviour-preserving (byte-identical output). But it also turned `CentralMvir` into a value that physics modules can now *see* on the workspace, which raises a legitimate question the team wanted recorded rather than silently settled: **should `CentralMvir` track the input-catalog mass of the FOF central, or the evolved central mass?** This note captures both options so the decision can be made deliberately later instead of being implied by an implementation detail.

---

## What `CentralMvir` is today

`CentralMvir` is a **structural per-FoF-group constant**: the input-catalog virial mass of the FOF central halo, broadcast to every member (central and satellites) of that FoF group for the current snapshot.

Concretely it is `get_virial_mass(InputTreeHalos[fofhalo].FirstHaloInFOFgroup)` — the spherical-overdensity `Mvir` of the catalog FOF central, or its `Len × PartMass` fallback when no SO mass is available (`src/core/virial.c`).

### How it is produced now

- The tree driver computes the value once per FoF subhalo slice and stamps it onto every workspace member of that slice in `build_halo_tree()` (`src/core/build_model.c`), immediately after inheritance and **before** physics runs.
- Physics never writes `CentralMvir`, so the stamped value survives unchanged to output.
- The shared output-buffer marshaller (`src/core/output_buffer.c`) does **not** touch `CentralMvir`; it is carried to the output buffer by plain struct copy. This keeps the marshaller free of any specific-physical-field knowledge and therefore driver-neutral.

### Why it is stamped before physics

Before Phase 3, `CentralMvir` was computed only at output time and was absent from `struct Halo`, so no module could read it. Now that it is a real field, leaving it correct only at output would mean modules observe `0.0` (new halos) or the previous snapshot's value (inherited halos) during physics — a latent trap. Stamping it before physics makes the field physically correct whenever it is observable, at zero output cost and with byte-identical results.

### Output contract

This is **byte-identical** to the historical output: the value written to binary/HDF5 is still `(float)get_virial_mass(FirstHaloInFOFgroup)`. The Phase 3 change and the pre-physics stamp move *where* the value is assigned, not *what* it is.

---

## The deferred option: track the evolved central mass

`CentralMvir` could instead report the **evolved** mass of the live FOF central as physics sees it (i.e. the central workspace halo's `Mvir` after inheritance/processing), rather than the input-catalog mass.

### When this would matter

For the *catalog* FOF central subhalo on its main branch, the catalog mass and the evolved `Mvir` are computed from the same `get_virial_mass(central)` and coincide. The two definitions diverge mainly when:

- the field is read *during* physics by a future module that expects the host mass to reflect intermediate state, or
- one wants `CentralMvir` to reflect any in-pipeline adjustment to the central's mass rather than the fixed catalog value.

Today no physics module reads `CentralMvir` at all (the only consumer is the `quiescent_fraction` plot, which reads the output file), so the two definitions are observationally equivalent in current output. Modules that genuinely need the host mass already use `mimic_find_fof_central_index()` and read the live central's `Mvir` directly (`models/sage/shared/central_link.h`).

### Why it is deferred

Switching to the evolved-central definition would be a **deliberate science change**, not a behaviour-preserving refactor: it could change output values and would need its own review and a baseline refresh. The dual-driver standing rule is that behaviour-preserving extraction and intentional behaviour changes stay strictly separate. So this option belongs to the v1.0 optimisation/review sweep or to specific module work, gated on its own scientific justification — not to the Phase 3 extraction.

### What changing it would entail (sketch)

- Decide the precise definition (evolved central `Mvir` at end of timestep, or at a specific phase) and whether satellites should see the same broadcast value or a phase-local one.
- Stamp from the evolved FOF central workspace entry (post-physics, before marshalling) instead of from the catalog, or compute it inside the marshaller from a driver-supplied central index.
- Refresh the SAGE regression baseline and document the output change.
- Confirm the snapshot driver (when it exists) can reproduce the same definition for cross-format identity — the value must derive from per-FoF state both drivers can compute identically.

---

## Recommendation

Keep the current catalog-mass, byte-identical behaviour for v1.0. Revisit the evolved-central option only if a module needs a host mass that reflects in-pipeline state, and treat any change as a reviewed science change with a baseline refresh.
