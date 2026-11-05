# `sage26_apply_infall`

Distributes the prepared infall budget into the central's hot reservoir, or with `CGMrecipeOn 1` and a CGM-regime central its CGM reservoir, during substeps, including metal transport from ejected gas when relevant.

## SAGE26 Source

A port of SAGE26's `add_infall_to_hot` (`src/model_infall.c:351-414`), called with the per-substep share `infallingGas / effective_steps` (`src/core_build_model.c:488`). A negative share is taken from the ejected reservoir first, with a float metallicity; the residual, the already narrowed negative `EjectedGas` when the ejected gas runs out, then goes to the selected reservoir: the central's CGM pair with `CGMrecipeOn 1` and `Regime` 0 (`:371-385`), its hot pair otherwise (`:386-413`). On the selected pair a negative amount debits the metals only while they are positive and floors them at zero, the signed gas is added, and both fields are cleared if the gas becomes negative; a positive share adds gas and no metals. The other pair is untouched. The metal debit is one C expression, `metals += infallingGas * metallicity`, which SAGE26's build fuses into a multiply-add on each branch (`:359`, `:378`, `:392`, `:406`).

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `galaxy_physics`, after `sage26_prepare_infall_budget`
- Receives the full FoF workspace and acts on the FoF central

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_prepare_infall_budget` must be in `pre_timestep` as `process_full_halo` — `InfallingGas` is zero without it and no gas is added to the hot reservoir.

## Properties

- Reads: `Type`, `InfallingGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `CGMgas`, `MetalsCGMgas`, `Regime` (`CGMrecipeOn 1`)
- Writes: `HotGas`, `MetalsHotGas` or (`CGMrecipeOn 1`, Regime 0) `CGMgas`, `MetalsCGMgas`; `EjectedGas`, `MetalsEjectedGas`

## Parameters

- `CGMrecipeOn` (required, 0 or 1; any other value, or a missing key, fails initialisation naming it; the run files keep 0, R3's value, until c11 completes R4)

## Tests

`_tests/test_unit_sage26_apply_infall_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`; the `cgm_*` fixtures are `CGMrecipeOn 1`, "The c10 Group"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_apply_infall.c` holds platform-independent tests running each case at `CGMrecipeOn 1` with Regime 0 and 1 and at `CGMrecipeOn 0` on the same exact inputs (`test_cgm_*`), and the parameter's refusal.

## Notes

`InfallingGas` is a snapshot-scoped transport property prepared before substeps. Keep this module before cooling so newly available hot gas can participate in downstream baryonic physics.
