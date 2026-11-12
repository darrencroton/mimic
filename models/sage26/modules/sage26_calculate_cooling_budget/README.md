# `sage26_calculate_cooling_budget`

Calculates the hot-halo cooling budget of the classic path (`CGMrecipeOn 0`): the hot gas that cools onto the disk this substep, before AGN suppression, with SAGE26's cooling diagnostics. The mass is carried in package-private transport properties to `sage26_radio_mode_heating` and `sage26_apply_cooling`; together the three implement SAGE26's `cooling_recipe_hot`. With `CGMrecipeOn 1` (the run files' value) `sage26_regime_aware_cooling` owns the cooling and this module writes only a no-cooling transport.

## Physics

The isothermal cooling model of Croton et al. (2006), through `mimic_sage_hot_cooling_budget` (`shared/sage_cooling_physics.h`). When `HotGas > 0` and `Vvir > 0`: the cooling time is the halo dynamical time `tcool = Rvir / Vvir`; the cooling function is evaluated at `T_vir = 35.9 * Vvir^2` K and the hot gas's metallicity; the density at which the cooling time equals `tcool` sets the cooling radius `rcool` of an isothermal profile with `rho0 = HotGas / (4 pi Rvir)`. If `rcool > Rvir` the halo cools rapidly, `HotGas / tcool * dt`; otherwise quasi-statically, `(HotGas / Rvir) * (rcool / (2 tcool)) * dt`. The mass is capped at `HotGas` and floored at 0. `rcool` is not capped, so the two branches differ by a factor 2 at `rcool == Rvir`, as in SAGE26.

The cooling functions are the Sutherland & Dopita (1993) collisional-ionisation-equilibrium tables at eight metallicities, primordial to super-solar, loaded from `CoolFunctions/` at `init()` (`cooling_tables.c`). `log10(T)` below 4.0 is raised to 4.0, above the last grid point (8.5) the rate extrapolates linearly along the final interval, and `log10(Z)` is clamped to the grid.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, immediately before `sage26_radio_mode_heating` and `sage26_apply_cooling`
- Receives one galaxy at a time; every galaxy that has not merged cools, Type 2 orphans included
- Without a positive interval (the core's snapshot-zero birth) cooling does not run and the return path is -1
- With `CGMrecipeOn 1` the module writes `CoolingGas = CoolingX = Rcool = 0` and `CoolingReturnPath = -1` for every galaxy and no galaxy property, so the radio and apply modules write nothing either

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. Only with `CGMrecipeOn 1`: `sage26_regime_aware_cooling` must be configured as `process_by_galaxy` in the same substep phase as this module — otherwise no galaxy would cool.
2. Only with `CGMrecipeOn 1`: `sage26_determine_regime` must be configured in `pre_timestep` as `process_full_halo` — otherwise every `Regime` stays -1 and the regime-aware cooling sends every galaxy through the hot branch.

`init()` also fails, naming the file, if a cooling table cannot be opened or read. It does not check this module's own phase or position, nor the presence of the radio and apply modules (`sage26_apply_cooling`'s own `init()` requires this module before it).

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `HotGas`, `MetalsHotGas`, `Vvir`, `Rvir`
- Writes: the transport properties `CoolingGas` (the cooling mass before AGN suppression), `CoolingX` and `Rcool` (the cooling model's `x` and `rcool`, for the radio module's AGN heating) and `CoolingReturnPath` (-1 not evaluated, 0 normal, 1 no hot gas or `Vvir`, 2 `lambda <= 0`, 3 `rho_rcool <= 0`); and the diagnostics `tcool`, `tff` and `RcoolToRvir` (`CGMrecipeOn 0` only)

## Parameters

- `CGMrecipeOn`: required, 0 or 1; 0 runs the classic hot cooling, 1 (the run files' value) writes only the no-cooling transport and requires the CGM partners above. Any other value, or a missing key, fails initialisation naming the parameter.

## Notes

The transport properties are not output and reset once per snapshot. `tcool` and `tff = sqrt(2) * tcool` are written with SAGE26's mass-rate conversion (`value * 1e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s`), so neither is a time in Gyr. The early returns leave `RcoolToRvir`, `Cooling` and `CoolingRate` as they were. The `CGMrecipeOn 1` cooling pieces (the clustering-mass table, cold streams and bulk CGM cooling) live in the same shared header and are called by `sage26_regime_aware_cooling`.

## Tests

- `_tests/test_unit_sage26_calculate_cooling_budget.c`: the cooling tables (loading and interpolation in temperature and metallicity, out-of-range inputs), registration and lifecycle, the carried state on each return path, the hot-halo cooling branches, and the `CGMrecipeOn 1` no-cooling transport. The shared `CGMrecipeOn 1` cooling helpers are tested in `../../shared/_tests/test_unit_sage_cooling_physics.c`.
- `_tests/test_integration_sage26_calculate_cooling_budget.py`: a `CGMrecipeOn 0` run of the budget behind the infall chain, checking that `tcool` is positive exactly where a galaxy has hot gas and a virial velocity, that `tff` is `sqrt(2) tcool`, and the `tcool` unit conversion on orphans.
- The `CGMrecipeOn` refusals are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`.
