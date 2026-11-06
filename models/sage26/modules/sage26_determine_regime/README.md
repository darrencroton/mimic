# `sage26_determine_regime`

Classifies every live row of the FoF group as CGM regime (`Regime` 0) or hot-halo regime (`Regime` 1) from its halo mass against the Dekel & Birnboim (2006) shock mass, once per snapshot, before the infall budget.

## SAGE26 Source

A port of SAGE26's `determine_and_store_regime` (`src/model_regimes.c:63-100`), which SAGE26 calls once per FoF group between progenitor joining and `infall_recipe`, and only when `CGMrecipeOn` is 1 (`src/core_build_model.c:407-415`). For every row whose `mergeType` is not positive:

1. `Mshock = MShockMsun * Hubble_h / 1.0e10`, the double `MSUN_TO_CODE_MASS` in that operation order (`src/macros.h:47`), in 1e10 Msun/h;
2. `mass_ratio = Mvir / Mshock` in double, from the float `Mvir` SAGE26 holds (`:74`);
3. `mass_ratio <= 0.0`: `Regime = 0`, with no random draw (`:77-78`);
4. otherwise `x = log10(mass_ratio) / 0.1` and `hot_fraction = 1 / (1 + exp(-x))` in double (`:82-89`), a sigmoid 0.1 dex wide about the shock mass, and `Regime = (u < hot_fraction) ? 1 : 0` for a uniform deviate `u` (`:94-95`).

A NaN `mass_ratio` is not `<= 0.0`, so it takes the draw branch, as SAGE26's test implies, and is classified 0: every ordered comparison with its NaN `hot_fraction` is false. There is no `Type` restriction (centrals, satellites and orphans are all classified; an orphan, whose `Mvir` is 0, takes the no-draw branch), no hysteresis, no redshift term and no reuse of a previous regime, and classification moves no gas: satellites keep whatever reservoirs they hold.

**The deviate (campaign decision D1).** SAGE26 draws `u = (double)rand() / (double)RAND_MAX` from libc's process-global stream (`src/model_regimes.c:94`), so a galaxy's draw depends on every earlier draw of the run. The package replaces that one draw, its one deliberate departure from SAGE26 here, with the keyed deviate of `shared/sage_keyed_draw.h`: a pure function of the row's `UniqueGalaxyID`, the current snapshot (`ctx->snapshot_number`), the CGM purpose salt and ordinal 0, strictly inside (0, 1), compared with `hot_fraction` exactly as SAGE26 compares its draw. It has no seed and no process-global state, and `UniqueGalaxyID` is built identically by both drivers, so the vertical and horizontal drivers classify a galaxy alike. A run therefore never reproduces SAGE26's libc trajectory; the arithmetic around the draw is SAGE26's, bit for bit.

**Mixed precision.** `Mshock`, `mass_ratio`, `x` and `hot_fraction` are doubles, as in SAGE26; `Mvir` is narrowed to the float SAGE26 holds before the division. No expression has a multiply feeding an add or subtract, so none can contract to a fused multiply-add, and SAGE26's object has none in this routine (the oracle README's "The c10 Group").

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`, once
- Receives the full FoF workspace

`init()` refuses any placement but exactly one `pre_timestep` entry with none in a substep phase or `post_timestep`, and, when `sage26_prepare_infall_budget` runs in `pre_timestep`, an entry after it: SAGE26 classifies before `infall_recipe`, whose regime-aware routing reads the central's regime. The run files place it after `sage26_reionization`, whose result it does not read, and immediately before `sage26_prepare_infall_budget`.

**Rows.** SAGE26 skips only rows with `mergeType > 0`; the package's mark of such a row (merged or disrupted, never output) is `Type` 3, and a row without a galaxy is skipped as every dispatcher does. At `pre_timestep` neither occurs: the core discards an inherited `Type` 3 row at joining (`src/core/inheritance.c`) and only the merger pass, a substep phase, sets `Type` 3. In SAGE26 the join guard's `Type` 3 rows never reach the call either: the guard `continue`s before `ngal++` (`src/core_build_model.c:241-244`, `:341`), so such a row is never inside `[0, ngal)` when `determine_and_store_regime(ngal, ...)` runs (`:411`). A row in the initial-boundary state (stored `SnapNum` -1, `dT` 0; campaign decision D37) is classified like any other: the classifier reads no snapshot-indexed table.

## Properties

- Reads: `Type`, `Mvir`, `UniqueGalaxyID`
- Writes: `Regime` (only with `CGMrecipeOn` 1; with 0 it keeps its creation value -1)

## Parameters

Both are required, with no default (campaign decision D33); a missing key fails `init()`.

- `CGMrecipeOn`: 0 (no classification; SAGE26 R0-R3) or 1 (the classifier; SAGE26 R4). Any other value is refused, naming it. The run files hold 1 (R4): with this classifier, the regime-aware routing and `sage26_regime_aware_cooling` the package runs SAGE26's R4.
- `MShockMsun`: the shock mass in physical Msun (no `h`), SAGE26's default 6.0e11 (`src/core_read_parameter_file.c:114`), which R4 uses. A non-positive or non-finite value is refused, naming it.

## Test Seam

`sage26_determine_regime.h` declares `sage26_determine_regime_set_test_seam()`. With a seam installed, `process()` takes each deviate from the seam instead of the keyed draw and reports every classified row's values (`Mshock`, `mass_ratio`, `x`, `hot_fraction`, the deviate, whether it requested one and the `Regime` written); `NULL` restores the keyed draw. A run never installs one.

## Tests

- `_tests/test_unit_sage26_determine_regime.c`: platform-independent behavioural tests through the seam, each expectation a literal or SAGE26's own expression written in the test: `CGMrecipeOn` 0 writes nothing and requests no deviate; `Mvir` +0, -0 and negative give 0 with no deviate, NaN requests one; mass ratios 2, about 1 and 1/2 with deviates one step below, equal to and one step above the sigmoid value; the exact ratio 1 with the 0.5 tie; an initial-boundary row; every `Type` and the skipped rows; the production keyed draw against the campaign's frozen reference deviates; both parameters' refusals and the placement checks.
- `_tests/test_unit_sage26_determine_regime_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`, written by `oracle_c10`) through `process()`, supplying SAGE26's own deviates through the seam in request order, and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
- The keyed draw itself is tested by `shared/_tests/test_unit_sage_keyed_draw.c`.

## References

- Dekel, A. & Birnboim, Y. (2006), MNRAS, 368, 2
- Voit, G. M. (2015), ApJL, 808, L30
