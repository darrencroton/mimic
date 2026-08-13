# D8: `Spin` Units-Label Reconciliation — Implementation Plan

**Status:** Frozen, ready for implementation.
**Owner reference:** `docs/dev/POST-PHASE-5-JOINT-REVIEW.md` §6 item 5 (D8); scoping evidence in `docs/dev/POST-PHASE-5-WORK.md` §2.1, "D8 implementation scope, established 2026-08-14".
**Gates:** `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` marks this the next step before the Shin-Uchuu rehearsal (item 6). This plan must land before that rehearsal, per the pathway's own ordering.

---

## 1. What this fixes

All eight simulation packages label the `Spin` halo property `dimensionless`. It is not. `Spin` is **specific angular momentum**, `J / Mvir`, with physical units `(Mpc/h)(km/s)`. Two independent lines of evidence already established this (recorded in `POST-PHASE-5-WORK.md` §2.1):

- `sage_set_disk_scale_radius.c:49` computes `lambda = spin_magnitude / (1.414 * vvir * rvir)` — for the result to be the dimensionless Bullock λ, `Spin` must carry units of `Vvir × Rvir` = `(Mpc/h)(km/s)`.
- The tracked `mini-millennium` baseline has `Spin` spanning −18.23 to 14.49 (median |Spin| ≈ 0.083) — far outside the ~0.03–0.05 range of a Bullock λ, but the right order of magnitude for specific angular momentum in that box.

Four of the eight packages (`mini-millennium`, `millennium`, `micro-uchuu`, `mini-uchuu` — the L-Halo-binary family) additionally carry a wrong **description**: "Dimensionless spin parameter (Bullock definition)". (`POST-PHASE-5-WORK.md` names only three of these; `mini-uchuu` was missed there — confirmed by direct inspection of `simulations/mini-uchuu/halo_properties.yaml:125-131` during this plan's preparation.)

`micro-uchuu` (the L-Halo-binary package, not to be confused with the ctrees siblings) also declares `range: [-200.0, 200.0]` for `Spin`, which its own data already violates (`simulations/micro-uchuu-ascii/README.md` and Phase 5 both record a measured maximum of 270.3 for this exact catalog in ctrees form).

### 1.1 Independent verification performed before writing this plan

The recorded risk (`POST-PHASE-5-WORK.md`) was that the L-Halo-binary Uchuu packages might disagree with the ctrees-format Uchuu packages — e.g. one family storing raw `J` and the other already-normalized `J/Mvir` — which would make "one label for all eight packages" scientifically wrong. This was checked directly against real data before committing to this plan, not assumed:

1. **Read the raw ctrees ASCII catalog** (`simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat`). Its header (line 23) documents `Jx/Jy/Jz` verbatim as "Halo angular momenta ((Msun/h) * (Mpc/h) * km/s (physical))." — raw angular momentum, magnitude ~1e16 for a massive halo. Mimic's reader (`read_ctrees_ascii.c:96-106`, shared with the HDF5 reader) parses exactly these three columns into `Spin[3]` and then divides by native `Mvir` (`apply_ctrees_value_conventions()`). A *separate*, unrelated scalar column literally named `Spin` in the ASCII file (a Peebles/Bullock-like value, e.g. `0.06066` in the sampled row) is not read by Mimic at all.
2. **Read the raw L-Halo binary catalog** (`simulations/micro-uchuu/snapshots/Uchuu100_Planck_lhalo_binary.*`, produced by the external `sage-model` converter from the same ctrees source, per `simulations/micro-uchuu/README.md`) directly via a byte-level struct unpack (104-byte `RawHalo` record, no reader code path involved). `Spin` magnitudes there are order 0.001–84 for a 200-halo sample of the file's most massive objects — the same order of magnitude as specific angular momentum, not raw `J` (~1e16).
3. **Matched the same physical halo across both formats** (identical `Mvir` and `Pos` to float precision — `Mvir = 30940.0` in `1e10 Msun/h`, `Pos = (84.1496, 6.3263, 67.6953)` Mpc/h, unique to 5 decimal places): the ASCII file's raw `Jx/Jy/Jz = (2.968e16, -2.605e16, -1.454e16)` divided by its raw `Mvir = 3.094e14 Msun/h` gives `(95.928, -84.195, -46.994)`. The L-Halo binary file's stored `Spin` for the identical halo is `(95.928, -84.195, -46.994)` — a match to 6 significant figures.
4. **Confirmed the offline converter matches too**: `scripts/convert/fixups.py:normalise_spin()` performs the identical `J[k] * (1.0/Mvir)` reciprocal-multiply in float64 before narrowing to float32, explicitly documented as matching `apply_ctrees_value_conventions` bit-for-bit. This is the path used for `micro-uchuu-snapshot` and will be used for `shin-uchuu`.

**Conclusion: all eight packages consistently store the same physical quantity — `J/Mvir` in `(Mpc/h)(km/s)` — under the same wrong `dimensionless` label.** There is no format-dependent inconsistency. The fix is a metadata/generator correction, not a data-correctness fix, and this plan proceeds on that basis.

### 1.2 What "byte-identical" must mean and why

`Spin`'s numeric values must not change by even one bit. The units label currently derives `h_convention: none` (no h-scaling applied anywhere, confirmed by tracing `scripts/generate_properties.py`'s `_effective_h_convention()`/`_linear_conversion_expr()` — today's conversion factor for `Spin` is a hardcoded `"1.0"` on both the catalog-input and output-write paths). The correct label change must reproduce that same `1.0` factor by construction, not by coincidence — see Slice 1's design.

---

## 2. Design

### 2.1 New unit registry entry and reference dimension

Add to `UNIT_REGISTRY` in `scripts/generate_properties.py`:

```python
"Mpc/h km/s": {"dimension": "specific_angular_momentum", "cgs": 3.08568e29, "h_convention": "carried"},
```

- `3.08568e29 = 3.08568e24 (Mpc/h → cm) × 1.0e5 (km/s → cm/s)` — the product of the two already-registered magnitudes.
- Naming convention follows the existing compound-unit precedent already in the registry, `"erg cm^3/s"` (space-separated juxtaposition = multiplication), rather than inventing a parenthesized style.
- `h_convention: carried` is the scientifically correct choice, not merely a convenient one: `Mimic`'s reference basis (`src/core/core_properties.yaml`) declares length (`Mpc/h`) as `carried` and velocity (`km/s`) as `none`; a product of the two inherits `carried` from its length factor.

Add to `reference_units:` in `src/core/core_properties.yaml`, alongside the existing `mass`/`length`/`velocity`/`time` entries:

```yaml
  specific_angular_momentum:
    label: Mpc/h km/s
    in_cgs: 3.08568e29
    h_convention: carried
```

**Why this (and not pinning `h_convention: none` on `Spin` in each package instead):** the alternative the scoping note allowed — pin `h_convention: none` explicitly per package — would preserve today's numeric behaviour too, but would leave the metadata self-contradictory (a unit that is manifestly h-dependent, declared h-independent), which defeats the entire purpose of this fix. Registering the dimension is the only option that is both numerically safe and scientifically honest.

**Why this is provably byte-neutral, traced through the generator:**

- **Catalog input path** (`normalize_catalog_contract()`, `scripts/generate_properties.py:531-566`): for a property whose `init_source` is `copy_from_tree*`, the generator computes `dimension = _unit_info(prop["units"])["dimension"]`, then looks up `target_unit = reference_units.get(dimension, {}).get("label", prop["units"])`. Once `specific_angular_momentum` exists in `reference_units`, `target_unit` resolves to `"Mpc/h km/s"` — the exact same string every package will declare for `Spin` (`Spin` is simultaneously the catalog field and the halo property in every package's single `halo_properties:` list, so source and target are the same YAML entry). Source and target units and h-conventions are therefore identical, so `_linear_conversion_expr()` returns the literal string `"1.0"` (scale `1.0/1.0`, `target_h == source_h`).
- **Output path** (`attach_output_conversions()`, `scripts/generate_properties.py:588-619`): once `specific_angular_momentum` is a recognized dimension, the function no longer skips it (today it does, since `dimensionless` isn't in `reference_units`); it computes `_linear_conversion_expr(ref["label"], ref["h_convention"], prop["units"], effective_h, ...)` with `ref["label"] == prop["units"] == "Mpc/h km/s"` and `ref["h_convention"] == effective_h == "carried"` — again `"1.0"`, and since the function only attaches `_output_convert` when the expression is *not* `"1.0"`, nothing is attached. Output stays exactly as it is today: the raw internal value, verbatim.
- Neither `generate_reference_units_h()` nor the run-local `output_schema.json`'s `"reference_units"` block enumerate this new dimension (both hardcode the four required keys `mass`/`length`/`velocity`/`time`), so adding it is additive and cannot perturb any other generated artifact.

This is why the acceptance criterion below can be **bit-for-bit output identity**, not "close enough" — the design produces exact identity, and the validation step proves it empirically rather than trusting the derivation alone.

### 2.2 Package changes

For all eight packages, change `Spin`'s `units: dimensionless` to `units: Mpc/h km/s` (no `h_convention:` override needed — it now derives correctly to `carried` from the registry). Update the four wrong descriptions (`mini-millennium`, `millennium`, `micro-uchuu`, `mini-uchuu`) from "Dimensionless spin parameter (Bullock definition)" to a description naming the actual quantity. The four ctrees-family packages already describe the quantity correctly (just drop "Dimensionless"). Suggested wording (adjust only to match each file's existing phrasing style, not the substance):

- `mini-millennium`, `millennium`, `micro-uchuu`, `mini-uchuu`: `Specific angular momentum (J/Mvir)`
- `micro-uchuu-ascii`, `micro-uchuu-hdf5`, `uchuu`: `Specific angular momentum (J/Mvir, normalised by the reader before bridging)`
- `micro-uchuu-snapshot`: `Specific angular momentum (J/Mvir, normalised by the producer before emission)`

Additionally, widen `micro-uchuu`'s (L-Halo binary) `Spin` range from `[-200.0, 200.0]` to `[-1000.0, 1000.0]` to match its three ctrees siblings, which cover the identical underlying catalog and already measure a maximum of 270.3 (`POST-PHASE-5-WORK.md` §2.1). No other package's range changes in this slice — `millennium`/`mini-millennium`/`mini-uchuu` staying at `[-20, 20]` and `uchuu` at `[-5000, 5000]` are separate, already-tracked decisions (§6 items 2 and 9), not this one.

### 2.3 Documentation touch-ups (mechanical, tied to this exact change)

- `docs/dev/SNAPSHOT-HDF5-FORMAT.md:82` currently reads `Dimensionless spin J/Mvir (normalisation applied by the producer; ...)` — self-contradictory prose (calls it dimensionless while naming the quantity as J/Mvir). Drop "Dimensionless"; this is a prose correction, not a structural/byte-layout change, so it does **not** require a `format_version` bump (the ratchet in `src/io/output/metadata_hdf5.c:110-115` and its snapshot-format analogue gate structural changes, not wording). This document freezes its own convention for exactly this situation (`docs/dev/SNAPSHOT-HDF5-FORMAT.md:5`, the `Errata` section at line 166): a wording correction that does not change which files on disk conform is recorded as a dated row in the `## Errata` table, not silently edited in place. Add both — fix the table's prose at line 82 AND append a new dated `Errata` row describing the correction, following the exact format of the three existing rows.
- `.agents/skills/mimic-properties/SKILL.md:80` (the tracked file — `.claude/skills` is a gitignored local symlink to this same path, not a separate tracked copy; do not edit through the `.claude/` path) lists every registered unit label; append `Mpc/h km/s` to keep the skill's own "first actions" reference accurate.
- `docs/dev/POST-PHASE-5-WORK.md` §2.1: correct "three" to "four" wrong descriptions (this plan's preparation found the doc missed `mini-uchuu`); mark the D8 row and the checklist entry (§6, table under "Shin-Uchuu readiness") as closed with a one-line pointer to this plan and the commit that implements it.
- `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: update the §6 table row for item 5 to closed, and move the "Next step" pointer to item 6 (the rehearsal), per the pathway's own stated ordering.
- `docs/dev/POST-PHASE-5-JOINT-REVIEW.md`: this document's §6 is the **authoritative** pre-Shin-Uchuu checklist (it explicitly supersedes `POST-PHASE-5-WORK.md`'s own §6 — see `MIMIC-DEVELOPMENT-PATHWAY.md`'s own Active Plans table). Item 5 there must be marked closed with a pointer to this plan and its landing commit, alongside its §4 `D8` decision-table row and the running §8 implementation-record paragraph. Missing this document while updating the other two would leave the authoritative checklist stale.
- `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`: this is the active, not-yet-executed plan for the package this fix is explicitly trying to protect (§1.2 above: get the label right before any Shin-Uchuu output exists). It carries the same wrong `dimensionless J/Mvir` wording in its own schema table (line 78) and prose (lines 163, 206, 208, 412-419, 445) describing the `Spin` field the future `shin-uchuu` package will declare. Correct line 78's schema-table entry to `Mpc/h km/s` at minimum; sweep the remaining `Spin`-adjacent lines for the same "dimensionless" wording and correct any that describe the units of the stored quantity (not lines that are only naming the underlying `Jx/Jy/Jz` raw-angular-momentum columns, which are correctly described as raw already). Leaving this doc stale would hand the next implementer (building `shin-uchuu` from this exact plan) the same wrong label this slice exists to remove.

---

## 3. What must NOT change

- Every `Spin` numeric value, in every package, in every existing test fixture and tracked baseline — bit-for-bit.
- `tests/data/output/baseline/binary/model_z0.000_0`, `model_uniquegalid_z0.000_0`, `model_uniquegalid_z0.020_0` (galaxy-record binary data) — untouched, since only metadata strings change.
- `tests/data/output/baseline/hdf5/model.hdf5` / `model_000.hdf5`'s `Snap063` halo/galaxy datasets — untouched. Only the `RunProperties/FieldMetadata` table's `Spin` row (`units`, `description` columns) changes.
- `models/sage16/modules/_tests/baseline/physics-binary/model_z0.000_0` (the model-owned full-physics baseline's galaxy-record binary data — a third tracked baseline, distinct from `tests/data/output/baseline/{binary,hdf5}`, gated by `models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py`) — untouched, since only its `metadata/output_schema.json` sidecar's `Spin` description/units change.
- Every non-`Spin` field's units, h-convention, and generated conversion expression.
- `millennium`/`mini-millennium`/`mini-uchuu`'s `Spin` range (`[-20, 20]`) and `uchuu`'s (`[-5000, 5000]`) — out of scope for this slice.
- HDF5 `format_version` / `hdf5_format_version` — this change alters no structural byte layout, so neither ratchet moves.

---

## Implementation Profiles

- Recommended for frontier/senior implementer: run Slice 1, then Slice 2, sequentially — do not batch. Slice 2's byte-identity proof is the load-bearing gate for the whole change and benefits from a clean, isolated diff to audit against.
- Recommended for standard implementer: run slices individually, exactly as ordered.
- Recommended for weaker implementer: run atomic slices one at a time; do not attempt to shortcut the bitwise proof procedure in Slice 2.

---

## Slice 1: Register the `specific_angular_momentum` unit dimension

### Intended Change
- Add the `"Mpc/h km/s"` entry to `UNIT_REGISTRY` in `scripts/generate_properties.py`.
- Add the `specific_angular_momentum` entry to `reference_units:` in `src/core/core_properties.yaml`.
- Append `Mpc/h km/s` to the registered-unit-labels list in `.agents/skills/mimic-properties/SKILL.md:80` (the tracked file; `.claude/skills` is a gitignored local symlink to this same path — see `.gitignore:55` and `.claude/skills -> ../.agents/skills` — never edit through it).
- Add one test function to `tests/integration/test_unit_contract_generation.py` (see Validation Plan) pinning that the new label's self-conversion is the literal identity string `"1.0"` — a durable regression guard for the exact invariant the rest of this plan depends on, independent of the one-time bitwise proof in Slice 2.
- This slice touches no simulation package YAML and no `Spin` declaration anywhere — the new dimension is unreferenced by any package until Slice 2, but it is **not** fully inert: see the Acceptance Criteria note on `unit_registry.h`.

### Acceptance Criteria
- Inputs: the four files listed above; no other generator or metadata file changes.
- Outputs: `make MODEL=sage16 SIMULATION=mini-millennium generate` succeeds; `make MODEL=sage16 SIMULATION=mini-millennium check-generated` passes.
- User-visible behaviour: none for any package's runtime output — no package declares the new label yet. **Correction from this plan's external review:** `src/include/generated/unit_registry.h` is NOT inert. `generate_unit_registry_h()` (`scripts/generate_properties.py:847-882`) iterates `sorted(UNIT_REGISTRY)` and emits one `if (strcmp(label, ...))` branch per registered label into three C functions (`mimic_unit_label_cgs`, `mimic_unit_label_carried`, `mimic_unit_label_h_convention`); adding `"Mpc/h km/s"` adds exactly three such branches (one per function), in registry-sort order. This is expected, benign (no C code looks up this label at runtime, since no package's `Spin` references it until Slice 2), and must be explicitly accounted for rather than treated as an unexplained diff. `property_defs.h`, `output_schema_writer.inc`, and every HDF5/binary schema emitter besides `unit_registry.h` must be unchanged except for the `Source MD5` header line every generated file carries (which moves because the YAML+generator inputs changed) — `generate_reference_units_h()` and the output-schema JSON's `reference_units` block both hardcode exactly the four keys `mass`/`length`/`velocity`/`time` and do not enumerate the new dimension.
- [ ] `src/include/generated/` is untracked by git (`.gitignore`) — do not use `git diff` to verify it. Instead, regenerate into a scratch copy from the pre-slice commit and compare it against the post-slice regeneration **with the `Source MD5:` header line stripped from both sides first** (every generated file's header line changes because the YAML+generator inputs changed — an unfiltered `diff -rq` will legitimately report every file as different, which is expected and must not be misread as a failure). Once that header line is excluded, every file must compare identical except `unit_registry.h`, which must differ by exactly three added `if (strcmp(label, "Mpc/h km/s") == 0)` branches and nothing else.
- [ ] `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium check-generated` exits 0.
- [ ] `make MODEL=sage16 SIMULATION=mini-millennium` builds clean (no new warnings) under the project's standard flags.
- [ ] The new `tests/integration/test_unit_contract_generation.py` test passes, asserting `_linear_conversion_expr("Mpc/h km/s", "carried", "Mpc/h km/s", "carried", ...) == "1.0"`.
- Behaviour that must not change: every currently-registered unit label's `cgs`/`h_convention`/`dimension`; the four required `reference_units` entries (`mass`/`length`/`velocity`/`time`) and their consumers (`generate_reference_units_h()`, the schema-writer's `reference_units` JSON block) — neither enumerates the new key, by design.

### Authorized Surface
- Files allowed to change:
  - `scripts/generate_properties.py`
  - `src/core/core_properties.yaml`
  - `.agents/skills/mimic-properties/SKILL.md`
  - `tests/integration/test_unit_contract_generation.py`
- Functions/classes/components allowed to change:
  - `UNIT_REGISTRY` (module-level dict literal only — no other function body changes)
- Tests allowed or expected to change:
  - `tests/integration/test_unit_contract_generation.py`: one new test function only, following the existing `test_identity_when_label_equals_reference`/`test_velocity_is_h_independent` pattern in that file.

### Explicit Non-Goals
- Do not touch any `simulations/*/halo_properties.yaml` file in this slice.
- Do not touch any tracked baseline fixture in this slice.
- Do not add a macro for the new dimension to `generate_reference_units_h()` — nothing in C consumes it directly; the generator's compile-time arithmetic is the only consumer.
- Do not edit anything under `.claude/` — it is a gitignored local convenience path, not the tracked skill file.

### Risk Flags
- Risky surfaces touched: `src/core/core_properties.yaml` (core property metadata — the mimic-properties skill's "highest bar" surface) and `scripts/generate_properties.py` (shared generator, affects every model/simulation pair).
- Approval needed before implementation: yes

### Validation Plan
- Tests to add/update: `tests/integration/test_unit_contract_generation.py` gets one new test function (see Intended Change); no other test should need updating in this slice.
- Commands to run:
  ```bash
  # Capture "before" generated output from the pre-slice commit in a scratch copy.
  # (If validation runs after implementation and the pre-slice tree is no longer checked
  # out, regenerate this from the pre-slice commit explicitly, e.g. via a throwaway
  # `git worktree add`, rather than assuming the working tree still holds it.)
  cp -r src/include/generated /tmp/d8-slice1-generated-before

  make MODEL=sage16 SIMULATION=mini-millennium generate
  make MODEL=sage16 SIMULATION=mini-millennium check-generated

  # src/include/generated/ is gitignored, so `git diff` shows nothing there and must not
  # be used to verify this slice. A plain `diff -rq` will also legitimately report EVERY
  # file as different, because every generated file's `Source MD5:` header line changes --
  # that alone is not a failure. Strip that one line from both sides before comparing:
  mkdir -p /tmp/d8-slice1-before-nomd5 /tmp/d8-slice1-after-nomd5
  for f in /tmp/d8-slice1-generated-before/*; do
    grep -v '^ \* Source MD5:' "$f" > "/tmp/d8-slice1-before-nomd5/$(basename "$f")"
  done
  for f in src/include/generated/*; do
    grep -v '^ \* Source MD5:' "$f" > "/tmp/d8-slice1-after-nomd5/$(basename "$f")"
  done
  diff -rq /tmp/d8-slice1-before-nomd5 /tmp/d8-slice1-after-nomd5
  # Expect exactly one file reported different: unit_registry.h. Inspect it directly:
  diff /tmp/d8-slice1-before-nomd5/unit_registry.h /tmp/d8-slice1-after-nomd5/unit_registry.h
  # Expect exactly three added `if (strcmp(label, "Mpc/h km/s") == 0)` branches (one in
  # each of the three functions). Nothing else. Every other file must now diff to nothing.

  make MODEL=sage16 SIMULATION=mini-millennium
  python3 tests/integration/test_unit_contract_generation.py   # or however this repo's Python test runner invokes it
  rm -rf /tmp/d8-slice1-generated-before /tmp/d8-slice1-before-nomd5 /tmp/d8-slice1-after-nomd5
  ```
- Lint (differential, via the `lint` skill): required — this slice changes linted Python files (`scripts/generate_properties.py`, `tests/integration/test_unit_contract_generation.py`).
- Manual checks: confirm the `unit_registry.h` diff is exactly the three expected branches and the MD5 line, nothing else; confirm `reference_units.h` and `output_schema_writer.inc` differ only in their `Source MD5` line.

### Rollback Path
- Revert the four-file diff (`scripts/generate_properties.py`, `src/core/core_properties.yaml`, `.agents/skills/mimic-properties/SKILL.md`, `tests/integration/test_unit_contract_generation.py`); `make generate` regenerates the prior state exactly (no other package references the new label, so nothing downstream depends on it).

---

## Slice 2: Relabel `Spin` across all eight packages, widen `micro-uchuu`'s range, refresh baselines and docs

### Intended Change
- In each of the eight `simulations/*/halo_properties.yaml` files, change `Spin`'s `units: dimensionless` to `units: Mpc/h km/s`.
- Fix the wrong description in `mini-millennium`, `millennium`, `micro-uchuu`, `mini-uchuu` (drop "Dimensionless spin parameter (Bullock definition)"); drop the stray "Dimensionless" prefix in the four ctrees-family packages' existing descriptions. Use the wording in §2.2 above, adapted to match each file's existing style.
- Widen `micro-uchuu`'s `Spin` `range:` from `[-200.0, 200.0]` to `[-1000.0, 1000.0]`.
- Fix `docs/dev/SNAPSHOT-HDF5-FORMAT.md:82`'s prose (drop "Dimensionless") and append the dated `Errata` row (§2.3).
- Regenerate for the default pair (`MODEL=sage16 SIMULATION=mini-millennium`) and refresh the six tracked baseline files (across five listed locations below — the last covers two files) that embed the old label/description. Two separate committed baseline families are affected — do not treat them as one (`tests/data/README.md` documents only the first; `models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py:21-28` documents the second):
  - `tests/data/output/baseline/binary/metadata/output_schema.json`
  - `tests/data/output/baseline/hdf5/metadata/output_schema.json`
  - `tests/data/output/baseline/hdf5/model.hdf5`
  - `tests/data/output/baseline/hdf5/model_000.hdf5`
  - `models/sage16/modules/_tests/baseline/physics-binary/metadata/output_schema.json` — the model-owned full-physics baseline sidecar (distinct from, and in addition to, the two core-baseline sidecars above; confirmed tracked via `git ls-files`, not gitignored). Refresh via the exact procedure documented in the test file itself: `cp tests/data/output/physics-binary/model_z0.000_0 models/sage16/modules/_tests/baseline/physics-binary/` and `cp tests/data/output/physics-binary/metadata/output_schema.json models/sage16/modules/_tests/baseline/physics-binary/metadata/` — but only the `metadata/output_schema.json` copy is expected to produce a diff; if the copied `model_z0.000_0` differs from the currently-committed one, treat that as a slice-blocking finding (Spin values must not move), not something to accept and commit.
- Update `docs/dev/POST-PHASE-5-WORK.md` (§2.1 "three" → "four"; close the D8 row and the §6 checklist item), `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` (close item 5, move "Next step" to item 6), `docs/dev/POST-PHASE-5-JOINT-REVIEW.md` (close §6 item 5, the §4 `D8` decision-table row, and note it in §8's running implementation record — this document's §6 is the authoritative checklist per `MIMIC-DEVELOPMENT-PATHWAY.md`'s own Active Plans table), and `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md` (correct the `Spin` units wording at line 78 and sweep the other `Spin`-adjacent lines listed in §2.3).

### Acceptance Criteria
- Inputs: the eight simulation packages' `halo_properties.yaml` files (Slice 1 already landed and is available); the six tracked baseline files (two core sidecars, two core `.hdf5` files, one model-owned physics-binary sidecar, one model-owned physics-binary data file); the five dev-plan/format-spec docs listed in §2.3.
- Outputs: every package regenerates cleanly for its own `MODEL`/`SIMULATION` pair; the default pair's full test tiers pass; the byte-identity proof (below) is green for both a raw L-Halo-binary path and a ctrees path.
- User-visible behaviour: `Spin`'s `units` field in every package's generated HDF5 `FieldMetadata` and run-local `output_schema.json` now reads `Mpc/h km/s` with an accurate description; every numeric `Spin` value anywhere is unchanged.
- [ ] All eight packages' `Spin` entries declare `units: Mpc/h km/s` and carry a description naming specific angular momentum, not the Bullock parameter.
- [ ] `micro-uchuu`'s `Spin` `range:` is `[-1000.0, 1000.0]`.
- [ ] No other package's `Spin` `range:` changed.
- [ ] For `MODEL=sage16 SIMULATION=mini-millennium`: a worktree-based bitwise tree-path check (the Phase 4b/Phase 5 procedure, repeated verbatim — see Validation Plan) shows **zero** byte differences in every `output/bitwise-after/model_*` file compared to `output/bitwise-before/`.
- [ ] For `MODEL=halos-only SIMULATION=micro-uchuu-ascii` (or `-hdf5`): the same before/after bitwise procedure against that package's committed fixture shows zero byte differences in `Spin` fields (this exercises the ctrees `apply_ctrees_value_conventions()` path, structurally distinct from the L-Halo `fread` path Slice 2's mini-millennium check exercises).
- [ ] The refreshed `tests/data/output/baseline/hdf5/model.hdf5` and `model_000.hdf5` files' `RunProperties/FieldMetadata` table show `Spin`'s `units` and `description` updated; the `Snap063` halo/galaxy datasets in those files are byte-identical to the pre-change committed versions (verify with `h5diff` or an explicit dataset-level `cmp` of the extracted arrays, not just "the test suite passed").
- [ ] `tests/data/output/baseline/binary/model_z0.000_0`, `model_uniquegalid_z0.000_0`, `model_uniquegalid_z0.020_0`, and `models/sage16/modules/_tests/baseline/physics-binary/model_z0.000_0` are byte-identical to their pre-change committed versions (confirm via `git diff --stat` showing no change to these four paths).
- [ ] `models/sage16/modules/_tests/baseline/physics-binary/metadata/output_schema.json`'s `Spin` entry shows the updated `units`/`description`, and nothing else in that file changed beyond `source_md5`.
- [ ] `make MODEL=sage16 SIMULATION=mini-millennium tests summary` passes with no new failures/warnings.
- [ ] `make MODEL=halos-only SIMULATION=micro-uchuu-ascii tests summary` (or the package's standard fixture-tier command) passes with no new failures/warnings.
- [ ] The full-physics baseline test (`models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py`) passes.
- [ ] `docs/dev/SNAPSHOT-HDF5-FORMAT.md`'s `Errata` table has a new dated row for this correction, in addition to the corrected prose at line 82.
- [ ] `docs/dev/POST-PHASE-5-JOINT-REVIEW.md` §6 item 5, its §4 `D8` row, and `docs/dev/POST-PHASE-5-WORK.md`'s equivalent entries are all marked closed, consistently, with no remaining document implying D8 is still open.
- Behaviour that must not change: everything in §3 above.

### Authorized Surface
- Files allowed to change:
  - `simulations/mini-millennium/halo_properties.yaml`
  - `simulations/millennium/halo_properties.yaml`
  - `simulations/micro-uchuu/halo_properties.yaml`
  - `simulations/mini-uchuu/halo_properties.yaml`
  - `simulations/micro-uchuu-ascii/halo_properties.yaml`
  - `simulations/micro-uchuu-hdf5/halo_properties.yaml`
  - `simulations/micro-uchuu-snapshot/halo_properties.yaml`
  - `simulations/uchuu/halo_properties.yaml`
  - `docs/dev/SNAPSHOT-HDF5-FORMAT.md`
  - `docs/dev/POST-PHASE-5-WORK.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/POST-PHASE-5-JOINT-REVIEW.md`
  - `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`
  - `tests/data/output/baseline/binary/metadata/output_schema.json`
  - `tests/data/output/baseline/hdf5/metadata/output_schema.json`
  - `tests/data/output/baseline/hdf5/model.hdf5`
  - `tests/data/output/baseline/hdf5/model_000.hdf5`
  - `models/sage16/modules/_tests/baseline/physics-binary/metadata/output_schema.json`
  - `models/sage16/modules/_tests/baseline/physics-binary/model_z0.000_0` (expected to be refreshed as a byte-identical no-op copy per §2's Intended Change; a genuine content change here is a slice-blocking finding, not an authorized outcome)
- Functions/classes/components allowed to change: none in C or Python — this slice is metadata-only plus regenerated/refreshed artifacts.
- Tests allowed or expected to change: none of the test *code* should need to change — only the baseline data/metadata artifacts listed above, refreshed via each baseline family's own documented copy procedure, not hand-edited.

### Explicit Non-Goals
- Do not touch `millennium`/`mini-millennium`/`mini-uchuu`'s `Spin` range (`[-20, 20]`) or `uchuu`'s (`[-5000, 5000]`) — those are separate, already-recorded decisions (§6 items 2 and 9).
- Do not touch any generated file under `src/include/generated/` by hand — only `make generate` may produce them.
- Do not modify `apply_ctrees_value_conventions()`, `bridge_halo_data_to_rawhalo()`, or `scripts/convert/fixups.py`'s `normalise_spin()` — the numeric computation is correct and untouched; only its label is wrong.
- `tests/data/output/physics-binary/` (gitignored scratch, distinct from the tracked `models/sage16/modules/_tests/baseline/physics-binary/` above — do not confuse the two) is regenerated as a normal, expected side effect of running `models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py`, which this slice's Validation Plan requires in order to refresh the tracked baseline. Do not hand-edit anything in it, do not add or commit anything from it beyond the two files the documented copy procedure names, and do not treat its regeneration as authorization to touch any other scratch output directory.
- Do not bump `hdf5_format_version` or the snapshot format's `format_version` — no structural change occurs.
- Do not edit `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md` beyond the `Spin`-units wording identified in §2.3 — no other content in that plan is in scope here.

### Risk Flags
- Risky surfaces touched: eight simulation packages' on-disk catalog contracts (property metadata — a "highest bar" surface per `mimic-properties`); six tracked baseline files across two independent baseline families (binary and HDF5 golden files, plus the model-owned physics-binary sidecar and its data file); a frozen format-contract document (`SNAPSHOT-HDF5-FORMAT.md`, prose-only touch plus its own `Errata` ledger, not a structural one).
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan
- Tests to add/update: none new; existing tiers must stay green.
- Commands to run:
  ```bash
  # Bitwise tree-path check, L-Halo family (Phase 4b Slice 4 / Phase 5 procedure, verbatim
  # for the run/compare mechanics; the loop below is strengthened to fail closed rather than
  # rely on a human reading empty output -- an external review of this plan found the
  # original `cmp ... || echo DIFF` form exits 0 even when a diff is printed):
  git worktree add output/bitwise-base <pre-slice-2-HEAD>
  (cd output/bitwise-base && make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium)
  # run build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml from both
  # the worktree binary and the post-slice-2 binary, each with output_directory rewritten to
  # output/bitwise-before/ and output/bitwise-after/ respectively
  before_count=$(find output/bitwise-before -name 'model_*' -type f | wc -l)
  after_count=$(find output/bitwise-after -name 'model_*' -type f | wc -l)
  [ "$before_count" -gt 0 ] || { echo "FAIL: no before/ files found"; exit 1; }
  [ "$before_count" -eq "$after_count" ] || { echo "FAIL: before has $before_count files, after has $after_count"; exit 1; }
  status=0
  for f in output/bitwise-before/model_*; do
    b="output/bitwise-after/$(basename "$f")"
    [ -f "$b" ] || { echo "FAIL: missing $b"; status=1; continue; }
    cmp -s "$f" "$b" || { echo "FAIL: DIFF $f"; status=1; }
  done
  [ "$status" -eq 0 ] || exit 1
  echo "PASS: $before_count files bitwise identical"
  git worktree remove output/bitwise-base

  # Ctrees-family spot check (structurally distinct read path -- exercises
  # apply_ctrees_value_conventions(), which the L-Halo check above never touches).
  #
  # TRAP, confirmed by this plan's external review: micro-uchuu-ascii is listed in
  # PRODUCTION_TEST_CONFIG_SIMULATIONS (scripts/discovery.py) so the plain generated
  # build/generated/test_inputs/halos-only/micro-uchuu-ascii/core/test_hdf5.yaml points
  # `simulation.config` at the PRODUCTION simulation_info.yaml -- i.e. the 12GB catalog
  # under simulations/micro-uchuu-ascii/snapshots/ -- NOT the tiny committed fixture.
  # Using it unmodified would silently require the production dataset to be mounted,
  # defeating the point of a fast fixture-tier spot check.
  #
  # The fix the repo's own fixture-backed integration test already applies is to
  # override `simulation.config` by hand after generation: see `_make_param()` in
  # simulations/micro-uchuu-ascii/_tests/integration/test_ascii_chunks.py, which loads
  # the generated test_hdf5.yaml, then points `config["simulation"]["config"]` at a
  # (possibly lightly rewritten) copy of `simulations/micro-uchuu-ascii/_tests/input/
  # test_simulation.yaml` -- the tiny, 3-forest committed fixture -- before writing the
  # run file out. This check needs no rewriting of that fixture (no forests_per_file
  # override is required here), so pointing `simulation.config` directly at the fixture
  # file is sufficient; the point to mirror is the override itself, not the exact
  # temp-file mechanics `_make_param()` uses to get there. Do not use the generated
  # test_hdf5.yaml unmodified:
  git worktree add output/bitwise-ctrees-base <pre-slice-2-HEAD>
  (cd output/bitwise-ctrees-base && make MODEL=halos-only SIMULATION=micro-uchuu-ascii generate && make MODEL=halos-only SIMULATION=micro-uchuu-ascii)
  make MODEL=halos-only SIMULATION=micro-uchuu-ascii generate
  make MODEL=halos-only SIMULATION=micro-uchuu-ascii
  # For each of the "before" (output/bitwise-ctrees-base) and "after" (this tree) builds:
  # take that tree's generated core/test_hdf5.yaml, override simulation.config to the
  # fixture path above and output.output_directory to a fresh scratch directory
  # (output/bitwise-ctrees-before/ or output/bitwise-ctrees-after/), write the result to
  # a temp run file, and run that tree's own `mimic` binary against it -- exactly the
  # `simulation: {config: ...}` / `output: {output_directory: ...}` keys `_make_param()`
  # already sets, so its logic is the reference implementation to mirror, not to import.
  # Then repeat the exact fail-closed comparison loop above (before_count/after_count/
  # cmp -s) against those two scratch directories instead of output/bitwise-before /
  # output/bitwise-after.
  git worktree remove output/bitwise-ctrees-base

  # Full default-pair regeneration and test tier -- MUST pass before any baseline is
  # touched; an external review of this plan found an earlier draft recorded the exit
  # code without gating on it:
  make MODEL=sage16 SIMULATION=mini-millennium generate
  make MODEL=sage16 SIMULATION=mini-millennium check-generated
  mkdir -p archive/test-logs
  make MODEL=sage16 SIMULATION=mini-millennium tests summary > archive/test-logs/d8-slice2-tests.log 2>&1
  rc=$?
  [ "$rc" -eq 0 ] || { echo "FAIL: test suite exit_code=$rc -- do NOT refresh any baseline, see archive/test-logs/d8-slice2-tests.log"; exit 1; }
  echo "PASS: full default-pair test suite green"

  # Capture the CURRENTLY COMMITTED baselines before overwriting them, so the "data
  # untouched" checks below have something to diff against:
  mkdir -p /tmp/d8-slice2-pre-refresh
  cp tests/data/output/baseline/hdf5/model.hdf5 tests/data/output/baseline/hdf5/model_000.hdf5 /tmp/d8-slice2-pre-refresh/

  # Baseline refresh, core (per tests/data/README.md):
  cp tests/data/output/binary/metadata/output_schema.json tests/data/output/baseline/binary/metadata/
  cp tests/data/output/hdf5/model_000.hdf5 tests/data/output/hdf5/model.hdf5 tests/data/output/baseline/hdf5/
  cp tests/data/output/hdf5/metadata/output_schema.json tests/data/output/baseline/hdf5/metadata/

  # Baseline refresh, model-owned full-physics (per the procedure documented in
  # models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py -- that test
  # ran as part of the full tier above, so tests/data/output/physics-binary/ is current):
  cp tests/data/output/physics-binary/model_z0.000_0 models/sage16/modules/_tests/baseline/physics-binary/
  cp tests/data/output/physics-binary/metadata/output_schema.json models/sage16/modules/_tests/baseline/physics-binary/metadata/
  git diff --stat models/sage16/modules/_tests/baseline/physics-binary/model_z0.000_0
  # must show no changes -- if it does, STOP: Spin values moved and this slice is not safe to land

  # Confirm data-only fields untouched, core baselines:
  git diff --stat tests/data/output/baseline/binary/model_z0.000_0 \
                   tests/data/output/baseline/binary/model_uniquegalid_z0.000_0 \
                   tests/data/output/baseline/binary/model_uniquegalid_z0.020_0
  # must show no changes
  h5diff tests/data/output/baseline/hdf5/model_000.hdf5 /tmp/d8-slice2-pre-refresh/model_000.hdf5 /Snap063
  h5diff tests/data/output/baseline/hdf5/model.hdf5 /tmp/d8-slice2-pre-refresh/model.hdf5 /Snap063
  # both must report no differences in the halo/galaxy datasets
  rm -rf /tmp/d8-slice2-pre-refresh

  # Per-package generate + check-generated for the remaining six packages
  # (micro-uchuu, mini-uchuu, micro-uchuu-hdf5, micro-uchuu-snapshot, uchuu, millennium):
  for sim in micro-uchuu mini-uchuu micro-uchuu-hdf5 micro-uchuu-snapshot uchuu millennium; do
    make MODEL=halos-only SIMULATION=$sim generate
    make MODEL=halos-only SIMULATION=$sim check-generated
  done
  ```
- Lint (differential, via the `lint` skill): required for the four Markdown dev-plan docs and the format-spec doc (Markdown line-length/hard-wrap rules), and for any YAML touched.
- Manual checks: read the full `git diff` of every `halo_properties.yaml` to confirm only `units`/`description`/(micro-uchuu's) `range` changed — no key reordering, no other field touched; manually inspect the refreshed `output_schema.json` sidecars' diffs to confirm only the `Spin` entry's `units`/`description` and the top-level `source_md5` changed.

### Rollback Path
- Revert the full slice diff (YAML, docs, and all six baseline files together — they are one coherent unit and must not be split across a revert).
- `make generate` regenerates cleanly from the reverted YAML for every package.

---

## Next Chat Prompt

```md
Plan file: docs/dev/D8-SPIN-UNITS-RECONCILIATION-PLAN.md
Slices or batch this session: Slice 1, then Slice 2 (sequential, not batched)

Read the full plan file first. If a selected slice or batch receipt is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use orchestrator as the controlling skill. Act as the Developer: keep implementation, validation, Git operations, and commits local. Use a read-only Reviewer only for investigation, evidence gathering, the hostile drift-audit skill, and an independent code-review skill pass.

For each selected slice, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. Both slices mark Risk Flags approval-needed: yes — stop and get my approval before coding on each one.
3. apply the scoped-implementation skill against the selected contract.
4. Slice 1: apply the drift-audit skill using a read-only Reviewer when available; otherwise perform Developer self-audit and record that provenance explicitly. Report the authorization gate result and who performed it before any quality review.
5. Slice 2 marks `Independent audit required: yes`. Per the orchestrator skill's own rule for that flag: commission separate read-only delegate launches for drift-audit and code-review, in that order. If no independent Reviewer can be launched for Slice 2, STOP and report to me rather than falling back to Developer self-audit — do not accept a self-audited Slice 2.
6. Slice 1 (or Slice 2 once its independent reviews are in hand): apply the code-review skill using a read-only Reviewer when available (Developer self-audit permitted only for Slice 1). Record who performed each review.
7. Surface drift and review findings to me, fix them, then re-run the relevant gate. If consecutive reviews return only minor findings and have clearly converged record residuals in the slice summary and proceed.
8. Ask me before committing. On my approval, commit the slice with the commit skill.

After both slices are committed, use the handoff skill to record state and audit provenance (Reviewer tool/label or Developer self-audit and fallback context).

Confirm before starting: plan file read, branch, and Slice 1. Then begin.
```
