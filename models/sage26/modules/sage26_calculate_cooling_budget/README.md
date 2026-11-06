# `sage26_calculate_cooling_budget`

Calculates the hot-halo cooling budget: the mass of hot gas that cools onto the disk this substep, before AGN suppression, with SAGE26's cooling diagnostics. The result is carried in package-private transport properties to `sage26_radio_mode_heating` and `sage26_apply_cooling`. This is the classic path, `CGMrecipeOn 0` (R0-R3); with `CGMrecipeOn 1` the module writes only its no-cooling transport and `sage26_regime_aware_cooling` owns the whole of SAGE26's regime-aware cooling (below).

## SAGE26 Source

A port of the first part of SAGE26's `cooling_recipe_hot` (`src/model_cooling_heating.c:135-314`) on the R0 path (`CGMrecipeOn 0`), up to its call of `do_AGN_heating`. The arithmetic is the shared `mimic_sage_hot_cooling_budget` (`shared/sage_cooling_physics.h`), called with `CGMrecipeOn 0`; the same file holds the `CGMrecipeOn 1` pieces (the clustering-mass table, the cold streams and bulk CGM cooling), which `sage26_regime_aware_cooling` calls. SAGE26's routine is split across three modules that run consecutively in `galaxy_physics`: this one (`:139-292`), `sage26_radio_mode_heating` (`do_AGN_heating`, `:297-299`) and `sage26_apply_cooling` (the routine's tail, `:301-313`, then `cool_gas_onto_galaxy`, `src/core_build_model.c:508-509`).

Every call first sets `tcool = 0`. When `HotGas > 0` and `Vvir > 0` the module:

1. forms SAGE26's `tcool = Rvir / Vvir` as a float division of the float `Rvir` and `Vvir` stored in a double, and `tff = sqrt(2) * tcool`, and writes both as float diagnostics with SAGE26's conversion `value * 1e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s` (a mass-rate factor applied to a time, so neither is a time in Gyr);
2. evaluates the cooling function at `T_vir = 35.9 * Vvir^2` K and `logZ = log10(MetalsHotGas / HotGas)` (a float division; `-10` without metals), returning 0 if `lambda <= 0`;
3. forms `x = m_p k_B T / lambda` in code units and `rho_rcool = x / tcool * (1.5 * 0.59)`, returning 0 if `rho_rcool <= 0`;
4. forms the cooling radius of an isothermal profile, `rcool = sqrt(rho0 / rho_rcool)` with `rho0 = HotGas / (4 pi Rvir)`, and writes `RcoolToRvir = rcool / Rvir`;
5. cools rapidly, `HotGas / tcool * dt`, when `rcool > Rvir`, otherwise quasi-statically, `(HotGas / Rvir) * (rcool / (2 tcool)) * dt` (with `HotGas / Rvir` a float division); then caps the mass to `HotGas` and floors it at 0.

The early returns come after `tcool` and `tff` are written and leave `RcoolToRvir`, `Cooling` and `CoolingRate` as they were. An outer-guard failure leaves `tff` and `RcoolToRvir` as they were and reaches SAGE26's tail with zero mass. `rcool` is left uncapped, so the rapid and static branches stay discontinuous by a factor 2 at `rcool == Rvir`, as SAGE26 keeps them.

`Mvir`, `Rvir` and `Vvir` are SAGE26 floats (`src/core_allvars.h:106-221`); the core's double halo values are read as those floats.

## Carried State

Package-private transport properties (`output: false`, reset once per snapshot at inheritance), written on every call:

- `CoolingGas` — the post-cap, pre-suppression cooling mass (0 on every early return);
- `CoolingX` — SAGE26's `x`, carried exactly to the radio module's Bondi rate (0 where not formed);
- `Rcool` — SAGE26's `rcool` (0 where not formed);
- `CoolingReturnPath` — the return path: -1 not evaluated (no positive interval), 0 the normal tail, 1 the outer guard, 2 `lambda <= 0`, 3 `rho_rcool <= 0`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, immediately before `sage26_radio_mode_heating` and `sage26_apply_cooling`
- Receives one galaxy at a time; every non-merged galaxy cools, Type 2 orphans included (`src/core_build_model.c:473-516`)
- Without a positive interval (the core's snapshot-zero birth, `SnapNum < 0` and `dT <= 0`) cooling does not run and the return path is -1

## `CGMrecipeOn 1`

With `CGMrecipeOn 1` SAGE26 calls `cooling_recipe_regime_aware` instead of `cooling_recipe_hot` (`src/model_cooling_heating.c:125-132`), and `sage26_regime_aware_cooling` runs it whole (campaign decision D38). This module then writes only `CoolingGas = CoolingX = Rcool = 0` and `CoolingReturnPath = -1` for every galaxy, and no galaxy property, so `sage26_radio_mode_heating` (its `CoolingGas > 0` gate) and `sage26_apply_cooling` (path -1, zero mass) write nothing either. Because such a run would cool nothing without the transaction, and route every galaxy through the hot branch without the classifier, `init()` with `CGMrecipeOn 1` refuses, naming the missing module, unless `sage26_regime_aware_cooling` is configured (`process_by_galaxy`) in the substep phase that holds this module and `sage26_determine_regime` (`process_full_halo`) in `pre_timestep`. With `CGMrecipeOn 0` neither is required.

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `HotGas`, `MetalsHotGas`, `Vvir`, `Rvir`
- Writes: `CoolingGas`, `CoolingX`, `Rcool`, `CoolingReturnPath`, `tcool`, `tff`, `RcoolToRvir` (with `CGMrecipeOn 1` only the four transport properties)

## Parameters

- `CGMrecipeOn` — required (campaign decision D33), 0 or 1; any other value, or a missing key, fails `init()` naming the parameter. The run files' value is 1 (R4), so on the shipped runs this module writes only its no-cooling transport.

The unit factors and `Hubble_h` are core-owned.

## Cooling Tables

The Sutherland & Dopita (1993) collisional-ionisation-equilibrium tables at eight metallicities, from primordial to super-solar, loaded from `CoolFunctions/` during `init()`. The eight stripped tables are byte-identical to SAGE26's (`src/auxdata/CoolFunctions`). The interpolation follows SAGE26's `get_metaldependent_cooling_rate` and `get_rate` (`src/core_cool_func.c:106-158`): `log10(T)` below 4.0 is raised to 4.0, while above the last grid point (8.5) the rate extrapolates linearly along the final interval, although SAGE26's comment says it clamps; `logZ` is clamped to the metallicity grid.

## Tests

`_tests/test_unit_sage26_calculate_cooling_budget_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`): the interpolation fixtures through `get_metaldependent_cooling_rate`, and the `cooling_recipe_hot` fixtures through the chained budget → radio → apply `process()` entries, with the carried state asserted after the budget step. The c11 fixtures (`cm_*`, `hs_*`, `cgm_*`: the clustering-mass table, `cooling_recipe_hot` with `CGMrecipeOn 1` and `cooling_recipe_cgm`) run through the shared helpers directly (`../_tests/oracle/README.md`, "The c11 Group"). Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_calculate_cooling_budget.c` also tests those `CGMrecipeOn 1` helpers on every platform, with expectations written independently of them: the table at every grid point, between grid points, at its clamps and either side of the cosmology selection; the streams' sigmoid from the stored snapshot, the cutoff at its equalities, both exponent clamps, `ratio <= 0` and the refusal of a stored `SnapNum` outside the redshift table (campaign decision D35); CGM cooling's profile and times, its `lambda > 0` guard, cap and floor. It also tests the module's `CGMrecipeOn` handling: with 1 the module writes only its no-cooling transport (and the radio and apply modules nothing), the refusals of a missing or unsupported `CGMrecipeOn`, and the configuration guard (`sage26_regime_aware_cooling`'s unit tests cover the guard's phase configurations).
