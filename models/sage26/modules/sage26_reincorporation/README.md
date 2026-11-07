# `sage26_reincorporation`

Returns gas from the ejected reservoir to the hot reservoir, or with `CGMrecipeOn 1` and a CGM-regime central its CGM reservoir, of FoF centrals whose virial velocity exceeds a critical velocity.

## SAGE26 Source

A port of SAGE26's `reincorporate_gas` (`src/model_reincorporation.c:44-66`). The critical velocity is `Vcrit = 354.53 * ReIncorporationFactor` km/s (`src/model_reincorporation.c:34`, `:46`), the escape-velocity argument for SN ejecta at about 501 km/s; R0 runs `ReIncorporationFactor 0.19` (`input/millennium.par:72`). A central with `Vcrit > 0`, `Vvir > Vcrit` and `Rvir > 0` reincorporates `(Vvir / Vcrit - 1) * EjectedGas / (Rvir / Vvir) * dt`, capped to its ejected gas, moving metals at the ejected metallicity. `Vvir` and `Rvir` are read as the floats SAGE26 holds, so `Rvir / Vvir` is a float division, as in SAGE26. The gas and its metal mass reach the destination through SAGE26's regime-aware `add_gas_to_hot_reservoir` (`src/model_misc.h:76-85`, the package's `mimic_sage_add_gas_to_hot_reservoir` in `shared/sage_cgm_routing.h`): the central's CGM pair with `CGMrecipeOn 1` and `Regime` 0, its hot pair otherwise. Only the destination depends on `CGMrecipeOn`; the amount, guards, cap, metallicity and ejected debits do not. The metal mass is a separately rounded product passed as the helper's argument (`src/model_reincorporation.c:64`), never fused into the addition.

The module keeps two guards SAGE26 lacks: an early return when `EjectedGas <= 0` and an error on a negative reincorporated mass. The early return now gates the CGM destination as well as the hot one. For `EjectedGas` +0.0 SAGE26 reincorporates +0.0, its ejected debits leave both ejected fields as they are, and its destination additions add +0.0 (`src/model_misc.h:79-80`, `:82-83`), which turns a -0.0 destination field (hot or CGM, gas or metals) into +0.0 where the guard leaves -0.0: equal in every value comparison, different only in an exact-zero field's sign bit. A negative or -0.0 `EjectedGas` would differ more, but none occurs on the R4 domain: the R4 reference output (`/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/R4/model_0.hdf5`, SHA-256 `2b606ee7ac82559e32c7c91161f1edb44f89288ec57016e80da884e118ed0807`) has no negative and no -0.0 `EjectedMass` or `MetalsEjectedMass` in any of its 64 snapshots (1519062 rows, checked with h5py, testing `< 0` and the sign bit of exact zeros), and the ejected gas is floored at infall preparation and cleared to +0.0 when infall exhausts it (`src/model_infall.c:140-141`, `:365`). The negative-reincorporation error is unreachable behind the early return.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `galaxy_physics`, before cooling and star formation
- Receives the full FoF workspace and acts on the FoF central

## Properties

- Reads: `HaloNr`, `SnapNum`, `Type`, `Vvir`, `Rvir`, `dT`, `EjectedGas`, `MetalsEjectedGas`, `Regime` (`CGMrecipeOn 1`)
- Writes: `EjectedGas`, `MetalsEjectedGas`, and `HotGas`, `MetalsHotGas` or (`CGMrecipeOn 1`, Regime 0) `CGMgas`, `MetalsCGMgas`

## Parameters

- `ReIncorporationFactor` (R0: `0.19`)
- `CGMrecipeOn` (required, 0 or 1; any other value, or a missing key, fails initialisation naming it; the run files hold 1, SAGE26's R4, kept through R6)

## Ordering

**Advisory (no `init()` enforcement):**

1. Run before cooling modules in the galaxy-physics phase so reincorporated gas participates in the same substep's hot-halo cooling calculation.

## Tests

`_tests/test_unit_sage26_reincorporation_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`; the `cgm_*` fixtures are `CGMrecipeOn 1`, "The c10 Group"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_reincorporation.c` holds platform-independent tests of each destination against `CGMrecipeOn 0` on the same exact input (`test_cgm_reincorporation_destination`) and of the parameter's refusal.
