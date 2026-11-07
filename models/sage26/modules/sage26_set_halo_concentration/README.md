# `sage26_set_halo_concentration`

Assigns each new FoF central its NFW halo concentration at progenitor joining, as SAGE26 does with `ConcentrationOn 3`: the concentration inverted from `Vmax/Vvir`, from the galaxy's current velocities if it was a central and from its infall-frozen velocities if it was a satellite. With `ConcentrationOn 0` it writes nothing.

## SAGE26 Source

SAGE26 makes the assignment inside progenitor joining, in the FoF-central branch, before it sets `Type = 0` (`src/core_build_model.c:290-295`):

```c
if(run_params->ConcentrationOn > 0) {
    galaxies[ngal].Concentration =
        (float)get_halo_concentration(ngal, run_params->ZZ[halos[halonr].SnapNum], galaxies, run_params);
}
```

The branch is reached only by a galaxy that was a Type 0 or 1 before joining (`:240`), whose progenitor is the first-occupied one (`:252`) and whose halo is its FoF's first (`:284`). `get_halo_concentration` (`src/model_halo_properties.c:327-362`) returns 0 for mode 0, and for mode 3 inverts the pair its Type at the call selects (`:339-350`): Type 0 takes the galaxy's float `Vmax` and `Vvir`, any other Type the float `infallVmax` and `infallVvir`, each widened to double on its own. The inversion is `concentration_from_vmax_vvir` (`:288-319`): 0 when `Vvir <= 0.0 || Vmax <= 0.0`; `ratio_sq = (Vmax / Vvir) * (Vmax / Vvir)`; 0 when `ratio_sq < 1.0`; otherwise exactly 60 bisection steps of `A c / mu(c)` against `ratio_sq` on `c` in [1, 200], with `A` the literal 0.21621 and `mu(c) = ln(1 + c) - c / (1 + c)`, moving `c_lo` only when `f_mid < ratio_sq`; it returns the bracket's midpoint. The package's helpers are `shared/sage_halo_concentration.h`, which the module calls with the narrowed velocities and the old Type.

**SAGE26's quirks, kept.** The 60 steps have no tolerance; a tie `f_mid == ratio_sq` moves `c_hi`; there is no upper-ratio rejection (a `ratio_sq` beyond `f(200)`, about 10.04, saturates at 200) and no floor (a ratio below 1 gives 0, overwriting an earlier value). `A c / mu(c)` is not monotonic on [1, about 2.16] (about 1.119 at `c = 1`, a minimum of about 0.99997 near 2.1626), although the source comment says it is: the seventh midpoint is 2.5546875, where `f` is 1.0050276914764331 on the oracle platform, and the eighth, 1.77734375, has a larger `f`, so every `ratio_sq` from 1 up to that value returns exactly 1.0 and the next larger returns 2.5546875 or more. No concentration strictly between 1 and 2.5546875 is produced. A satellite keeps its last central value; a former satellite that becomes a central again uses its infall-frozen velocities on that snapshot; a galaxy created this snapshot keeps its creation value 0. The source comments that call the constant 0.2162, the relation monotonic, mode 3 Duffy et al. (2008) and mode 3's satellites tabulated (`:285`, `:290-299`, `:324-325`, `:352`) are stale and not ported.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`, once
- Receives the full FoF workspace

Mimic's core performs progenitor joining (`src/core/inheritance.c`) and gives the joined FoF central `Type` 0; the module makes SAGE26's concentration write once per FoF group afterwards. The run files place it immediately after `sage26_initialise_merger_clock` and before `sage26_set_disk_scale_radius`, SAGE26's order inside the FoF-central branch (the clock at `:288`, the concentration at `:290-294`, the disk and bulge radii at `:297-298`); the three modules touch disjoint state.

With `ConcentrationOn` 3, for each row whose new `Type` is 0:

| `PreviousType` | Action |
|---|---|
| 0 | `Concentration` = `(float)` the inversion of the current `Vmax` and `Vvir` |
| 1 | `Concentration` = `(float)` the inversion of `infallVmax` and `infallVvir` |
| -1 (created this snapshot) | kept: the creation value 0 (`init_galaxy` is not followed by the call, `src/core_build_model.c:347-351`) |
| 2 or 3 | kept: joining passes a former orphan over (`:240`) |

Every row whose new `Type` is not 0 keeps its value (`Concentration` is inherited, with no `init_repeat`). The core's double `Vmax`, `Vvir`, `infallVmax` and `infallVvir` are each narrowed to SAGE26's float first. Mode 3 does not read the redshift SAGE26 passes.

`init()` refuses any placement but exactly one `pre_timestep` entry as `process_full_halo` with no entry in a substep phase or `post_timestep`, naming the module. With `ConcentrationOn` 3 it also refuses a run without `sage26_record_transition_state` in `post_timestep` as `process_full_halo`, naming it: that module records `PreviousType`, and without it every galaxy would hold -1 and no central would ever be assigned. With `ConcentrationOn` 0 the module reads nothing and that check does not apply.

## Properties

- Reads: `Type`, `PreviousType`, `Vmax`, `Vvir`, `infallVmax`, `infallVvir`
- Writes: `Concentration` (only with `ConcentrationOn` 3; with 0 every row keeps its creation value 0)

## Parameters

- `ConcentrationOn`: required, with no default (campaign decision D33); a missing key fails `init()`. 0 (no concentration; SAGE26 R0-R4, `R4.par:57`) or 3 (the `Vmax/Vvir` inversion; SAGE26 R5, `R5.par:57`, and SAGE26's default, `src/core_read_parameter_file.c:105`). Modes 1 (the Ishiyama et al. 2021 table) and 2 (`Vmax/Vvir` for every Type) are not ported; they and any other value are refused, naming the parameter. The run files hold 3 (R5, kept at R6).

## Tests

- `_tests/test_unit_sage26_set_halo_concentration.c`: platform-independent behavioural tests, each expectation a literal or SAGE26's inversion written in the test on the float pair its dispatch selects: a previous central takes its current pair and a returning satellite its infall pair (pairs whose results differ); a new Type 0 with `PreviousType` -1 or 2 and every new Type 1 or 2 keep a seeded value; the float store of a double with a visible rounding, after narrowing the core's doubles; ratio below 1, exactly 1 and beyond `f(200)`; `ConcentrationOn` 0 writes nothing; the missing key, the values 1, 2 and 7, each misplacement and the recorder's absence at 3 are refused, naming what they refuse. Three joining compositions run the core's inheritance (`inherit_descendant_halos`) and then the module: a Type 1 that becomes its FoF central takes its infall pair (its current pair's result differs), a central whose halo grew takes the updated pair (its pre-join pair's result differs), and a central that becomes a satellite keeps its value.
- `_tests/test_unit_sage26_set_halo_concentration_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`, "The c12 Group"): the `inv_*` and `ghc_*` fixtures (written by `oracle_c12`) through the shared helpers directly, and the `tr_join_*` fixtures (SAGE26's progenitor joining at `ConcentrationOn 3`, written by `oracle_c06`) through the joining harness (`../_tests/sage26_oracle_join.h`: the core's inheritance, then this module between the merger clock and the disk radius), every recorded member of each joined galaxy and a wiring assertion per path (current pair, infall pair, kept). Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
- The helpers themselves are tested by `shared/_tests/test_unit_sage_halo_concentration.c`.

## References

- Navarro, J. F., Frenk, C. S. & White, S. D. M. (1997), ApJ, 490, 493
