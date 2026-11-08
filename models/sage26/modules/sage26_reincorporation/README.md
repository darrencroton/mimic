# `sage26_reincorporation`

Returns gas from the ejected reservoir to the hot reservoir, or with `CGMrecipeOn 1` and a CGM-regime central its CGM reservoir, of FoF centrals whose virial velocity exceeds a critical velocity.

## SAGE26 Source

A port of SAGE26's `reincorporate_gas` (`src/model_reincorporation.c:44-66`). The critical velocity is `Vcrit = 354.53 * ReIncorporationFactor` km/s (`src/model_reincorporation.c:34`, `:46`), the escape-velocity argument for SN ejecta at about 501 km/s; R0 runs `ReIncorporationFactor 0.19` (`input/millennium.par:72`). A central with `Vcrit > 0`, `Vvir > Vcrit` and `Rvir > 0` reincorporates `(Vvir / Vcrit - 1) * EjectedGas / (Rvir / Vvir) * dt`, capped to its ejected gas, moving metals at the ejected metallicity. `Vvir` and `Rvir` are read as the floats SAGE26 holds, so `Rvir / Vvir` is a float division, as in SAGE26. The gas and its metal mass reach the destination through SAGE26's regime-aware `add_gas_to_hot_reservoir` (`src/model_misc.h:76-85`, the package's `mimic_sage_add_gas_to_hot_reservoir` in `shared/sage_cgm_routing.h`): the central's CGM pair with `CGMrecipeOn 1` and `Regime` 0, its hot pair otherwise. Only the destination depends on `CGMrecipeOn`; the amount, guards, cap, metallicity and ejected debits do not. The metal mass is a separately rounded product passed as the helper's argument (`src/model_reincorporation.c:64`), never fused into the addition.

The module keeps two guards SAGE26 lacks: an early return when `EjectedGas <= 0` and an error on a negative reincorporated mass. The early return now gates the CGM destination as well as the hot one. For `EjectedGas` +0.0 SAGE26 reincorporates +0.0, its ejected debits leave both ejected fields as they are, and its destination additions add +0.0 (`src/model_misc.h:79-80`, `:82-83`), which turns a -0.0 destination field (hot or CGM, gas or metals) into +0.0 where the guard leaves -0.0: equal in every value comparison, different only in an exact-zero field's sign bit. A negative or -0.0 `EjectedGas` would differ more. Against that, the package zeroes the central's pooled ejected gas when it is negative at infall preparation and clears it to +0.0 when negative infall exhausts it, as SAGE26 does (`src/model_infall.c:140-141`, `:365`; `sage26_prepare_infall_budget`, `sage26_apply_infall`). The negative-reincorporation error is unreachable behind the early return.

The substep `dt` is the central's `dT` divided by the substep count (`mimic_object_substep_dt`, `shared/time_parity.h`), read only after the guards above pass. At the core's first-snapshot boundary state (stored `SnapNum` below 0 with `dT` not positive) the module writes nothing; any other non-positive `dT` makes `process()` fail with an error naming the halo.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `galaxy_physics`, before cooling and star formation
- Receives the full FoF workspace and acts on the FoF central

## Properties

- Reads: `HaloNr`, `SnapNum`, `Type`, `Vvir`, `Rvir`, `dT`, `EjectedGas`, `MetalsEjectedGas`, `Regime` (`CGMrecipeOn 1`)
- Writes: `EjectedGas`, `MetalsEjectedGas`, and `HotGas`, `MetalsHotGas` or (`CGMrecipeOn 1`, Regime 0) `CGMgas`, `MetalsCGMgas`

## Parameters

- `ReIncorporationFactor` (required, accepted [0, 10]; a value outside that, or a missing key, fails initialisation naming it; the run files hold 0.19, SAGE26's value at `input/millennium.par:72` and so in every configuration R0–R6). At 0, `Vcrit` is 0 and the module writes nothing, which `init()` does not refuse; SAGE26 skips the call unless `ReIncorporationFactor > 0.0` (`src/core_build_model.c:490-492`).
- `CGMrecipeOn` (required, 0 or 1; any other value, or a missing key, fails initialisation naming it; the run files hold 1, SAGE26's R4, kept through R6)

## Ordering

**Advisory (no `init()` enforcement):**

1. Run before cooling modules in the galaxy-physics phase so reincorporated gas participates in the same substep's hot-halo cooling calculation.

`init()` checks only its two parameters (see Parameters); it refuses no placement or partner. Not refused:

- any phase, position or entry count of this module. The core does refuse a mode `module_info.yaml` does not list (it lists only `process_full_halo`).
- a position before `sage26_apply_infall`, or no `sage26_apply_infall` at all; SAGE26 applies the infall share before reincorporating (`src/core_build_model.c:488-492`), as the run files order the two.
- `CGMrecipeOn 1` without `sage26_determine_regime`: the central's `Regime` then keeps its creation value -1 (only the classifier writes it), so the gas goes to its hot pair.
- `ReIncorporationFactor` 0, with which the module writes nothing.

## Tests

`_tests/test_unit_sage26_reincorporation_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`; the `cgm_*` fixtures are `CGMrecipeOn 1`, "The c10 Group, Part 2"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_reincorporation.c` holds platform-independent tests of each destination against `CGMrecipeOn 0` on the same exact input (`test_cgm_reincorporation_destination`) and of the parameter's refusal. It also covers registration, the init/cleanup lifecycle and parameter reading, the reincorporated amount, the Type 0 restriction and the context's central at a non-zero index, the `Vcrit` threshold, the cap to the ejected gas, the transferred metallicity, zero ejected gas, mass conservation, several substeps, a NULL galaxy, and the reservoirs a normal call leaves. `_tests/test_integration_sage26_reincorporation.py` runs Mimic at `CGMrecipeOn 0` with this module alone in `galaxy_physics` and, in other cases, after `sage26_prepare_infall_budget` (in most of them also `sage26_reionization` and `sage26_apply_infall`). It checks that each run succeeds and writes output, that no memory leak is reported, that the ejected and hot fields are in the output and non-negative, and that the ejected and hot metallicities of Type 0 rows lie in [0, 1].
