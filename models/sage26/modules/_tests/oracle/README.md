# SAGE26 Function-Level Oracle

This directory holds the tooling that turns SAGE26's own physics routines into reference fixtures for the `sage26` package. The oracle compiles SAGE26's physics sources unchanged, calls one SAGE26 routine on an explicit input, and records exactly what it computed: the routine's return value, every field it writes, and every intermediate value the package carries between modules. A package module's unit test can then run the same input through its `process()` entry and compare field by field, bit for bit.

It is reference tooling, not Mimic code: it lives under a `_tests/` path, which the Makefile's package source scan excludes (`! -path '*/_tests/*'`), and it builds into its own executable that never links Mimic code.

| File | Purpose |
|---|---|
| [`build.sh`](build.sh) | Compiles SAGE26's physics sources and `oracle_c03.c` into `output/oracle/oracle_c03` |
| [`regenerate.sh`](regenerate.sh) | Checks the manifest, runs `build.sh`, rewrites every fixture output from its input |
| [`oracle_c03.c`](oracle_c03.c) | The oracle main for the c03 (baryon supply) routines |
| [`oracle_c04.c`](oracle_c04.c) | The oracle main for the c04 (cooling and radio heating) routines; `build.sh` builds it the same way into `output/oracle/oracle_c04` |
| [`fixtures.manifest`](fixtures.manifest) | Every fixture, grouped by owning module, with the count per module |

Fixtures live with the module they test, under `models/sage26/modules/<module>/_tests/oracle/`: `<name>.in` is the input, `<name>.out` the oracle's output. Both are committed test data. Inputs are written by hand or by a tool; outputs are written only by `regenerate.sh` and are never edited by hand.

## Oracle Environment

The fixtures were produced, and a bit-for-bit comparison against them is meaningful, only in this environment, which is the one that built the SAGE26 T1 reference:

- **Platform:** macOS 27.0.1 (build 26A434) on arm64 (Apple silicon), with the system C library and **macOS libm** (`pow`, `sqrt`).
- **Compiler:** Apple clang version 21.0.0 (clang-2100.4.1.1), target `arm64-apple-darwin27.0.0`.
- **Flags:** the SAGE26 reference build's recorded flags, unchanged except for the value of `ROOT_DIR`:

  ```text
  -DROOT_DIR='"<staged SAGE26 root>"' -DVERBOSE -DGSL_FOUND -DHDF5 -DGITREF_STR='"unknown"'
  -O2 -march=native -std=gnu99 -fPIC -I/opt/homebrew/Cellar/gsl/2.8/include
  -DUSE_BUFFERED_WRITE -I/opt/homebrew/include -g -Wextra -Wshadow -Wall
  -Wno-unused-local-typedefs
  ```

  `oracle_c03.c` and `oracle_c04.c` additionally get `-I<staged SAGE26 root>/src` for SAGE26's headers. The link uses GSL 2.8 (`-L/opt/homebrew/Cellar/gsl/2.8/lib -lgsl -lgslcblas` with an rpath) and `-lm`, as the reference link did; the reference also linked HDF5, which none of these sources call.
- **Floating-point contraction:** clang's default (`-ffp-contract=on`), under which a multiply and an add or subtract *within one C expression* may be fused into a single `fma` on arm64. No flag changes it, in SAGE26's build or here.
- **SAGE26 source:** commit `11b1ae851524baadb76cfd17e0b38f8a89c07224`, staged read-only at `/Volumes/Internal/results/sage26-campaign/reference-src`.

**A bit-for-bit claim holds only for the same compiler, the same flags, the same contraction behaviour and the same libm.** A different compiler or version, a different optimisation level or `-march`, a different `-ffp-contract`, or a different libm can change the last bits of a result legitimately. On any other platform the fixtures are still a close reference, but an exact-match test should report SKIP rather than FAIL.

`build.sh` refuses to build when `clang --version` or the staged source's commit differs from the above; `ORACLE_ALLOW_ANY_TOOLCHAIN=1` overrides that for experiments, and outputs built that way are not reference fixtures.

### Staged Source Equals the Reference Build's Source

It was checked that the staged source is the source the reference build compiled. The reference build compiled a copy of SAGE26 in its workspace (`/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/src`, its `ROOT_DIR`). A recursive `diff -rq` of the staged tree against that copy, excluding only the staged tree's `.git` and the copy's build products (`*.o`, `libsage.so`, `sage`), reports no difference, and the staged tree's `HEAD` is the commit above. As a further check, the machine code (`__TEXT,__text`) and read-only constants (`__TEXT,__const`) of each of the twelve objects `build.sh` produces are identical to those of the reference build's own objects for the same sources.

## What Gets Compiled

`build.sh` compiles the twelve sources SAGE26's own `tests/Makefile` links its unit tests against, in that order: `core_utils.c`, `model_misc.c`, `model_h2_chemistry.c`, `model_halo_properties.c`, `model_regimes.c`, `model_starformation_and_feedback.c`, `model_cooling_heating.c`, `model_reincorporation.c`, `model_infall.c`, `model_disk_instability.c`, `model_mergers.c`, `core_cool_func.c`. Each is compiled from the SAGE26 root as `src/<name>.c`, as the reference build did. Nothing is written in the SAGE26 tree; objects go to `output/oracle/obj/` and the executable to `output/oracle/oracle_c03` (the worktree's git-ignored `output/`). `SAGE26_SRC` and `ORACLE_OUT` override the two locations. `oracle_c04.c` is compiled with the same flags and linked against the same twelve objects, in the same way, into `output/oracle/oracle_c04`.

## Regenerating

```bash
sh models/sage26/modules/_tests/oracle/regenerate.sh
```

`regenerate.sh` first checks that `fixtures.manifest` is self-consistent and agrees exactly with the `.in` files on disk, then runs `build.sh`, then runs the matching oracle main on every listed fixture, and finally checks that no `.out` lacks its `.in`. Fixtures of the four c04 modules (`sage26_calculate_cooling_budget`, `sage26_radio_mode_heating`, `sage26_apply_cooling` and `sage26_finalize_cooling_heating`, the `C04_MODULES` list in `regenerate.sh`) run through `output/oracle/oracle_c04`; every other (c03) fixture runs through `output/oracle/oracle_c03`. Each run writes to a fresh temporary file under `ORACLE_OUT`, which is moved into place as `<name>.out` only if the oracle exited 0 and the file is a complete output: the header, format and routine lines, at least one typed field, every later line a typed field, and a final newline. Otherwise the script stops non-zero and leaves the old output untouched. The exit status alone is not enough: SAGE26's `ABORT(0)` (`R/src/macros.h`), which `read_cooling_functions()` uses on a missing or malformed cooling table, exits with status 0 and writes nothing. Outputs are deterministic: running it twice gives byte-identical outputs, and the c03 outputs are unaffected by the c04 group. To add a fixture, write its `.in`, add it to `fixtures.manifest` (and raise the module's count), and run `regenerate.sh`.

Run one fixture by hand with its matching oracle main, `output/oracle/oracle_c03 <input> <output>` for a c03 fixture or `output/oracle/oracle_c04 <input> <output>` for a c04 one.

## Fixture File Format

A fixture file is UTF-8 text, one statement per line. `#` starts a comment that runs to the end of the line; blank lines are ignored. The first statement is the format line, the second names the routine, and every other statement is one typed, named field:

```text
# free-text description and provenance
format sage26-oracle-fixture 1
routine reincorporate_gas
f64 params.ReIncorporationFactor 0x3fc851eb851eb852  # 0.19
i32 call.effective_steps 10
f32 galaxy[0].Vvir 0x43480000  # 200
```

| Type | Value | Meaning |
|---|---|---|
| `f32` | `0x` + exactly 8 hex digits | The IEEE-754 binary32 bit pattern of a `float` |
| `f64` | `0x` + exactly 16 hex digits | The IEEE-754 binary64 bit pattern of a `double` |
| `i32` | decimal | A 32-bit integer |

The hexadecimal value is the exact bit pattern, not a C99 `%a` literal; the decimal after `#` is for reading only and is never parsed. Names are unique within a file. The oracle rejects a missing field, a field of the wrong type, a malformed bit pattern (anything but `0x` and exactly 8 or 16 hex digits), and an input field its fixture reader for the selected routine does not consume, so a misspelt name cannot be silently ignored. The reader consumes a superset of what SAGE26 reads: some fields, such as `galaxy[i].Type`, are set for the Mimic side although the SAGE26 routine never reads them.

### Field Names

| Prefix | Meaning |
|---|---|
| `params.<Name>` | A `struct params` member of that name |
| `galaxy[<i>].<Name>` | Member `<Name>` of SAGE26's `struct GALAXY` at index `<i>` of the galaxy array |
| `halo.<Name>` | A `struct halo_data` member (the single halo the call reads) |
| `call.<name>` | A call-site quantity of `core_build_model.c` (redshift, substep count, Age entries, indices, the infall budget) |
| `return.<name>` | (output) the routine's return value |
| `intermediate.<name>` | (output) a value formed at the call site or carried between package modules |

Galaxy fields use SAGE26's names. Where the package's property differs: `EjectedMass` is the package's `EjectedGas`, `MetalsEjectedMass` is `MetalsEjectedGas`, `DiskScaleRadius`, `ICS`, `MetalsICS`, the ICS history fields, `CGMgas`, `MetalsCGMgas` and the other reservoirs keep their names. On the Mimic side `Mvir`, `Rvir` and `Vvir` are core-owned halo fields and `Spin` is the simulation package's halo property (`halo.Spin[j]` here). SAGE26 holds `Mvir`, `Rvir` and `Vvir` as `float`; the Mimic side is given the exactly widened `double` of the same bit pattern.

Core-owned quantities a routine reads are never derived by the oracle: `params.G` and `params.Hubble`, the cosmology (`params.Omega`, `params.OmegaLambda`), the reionization scale factors (`params.a0`, `params.ar`), `call.Zcurr`, the Age entries and the halo fields are all explicit inputs.

The oracle initialises every field its reader consumes, a superset of the fields the routine reads. Every other struct member is poisoned with all bits set before the call (NaN for floating point, -1 for integers), so a read of a member the input did not set would usually surface as NaN in the output; not always, since a NaN can be compared away (every ordered comparison with it is false) or overwritten before it reaches an output.

## Routines

Each fixture describes one call, with the arithmetic SAGE26's call site performs around it (`R/` is the staged SAGE26 root). All fixtures describe the R0 configuration (`ReionizationOn 1`, `CGMrecipeOn 0`, `BaryonFrac 0.17`, `ReIncorporationFactor 0.19`, `Reionization_z0 8`, `Reionization_zr 7`, Millennium cosmology and units), but each `.in` carries only the params its routine's reader consumes, so the R0 values of the others are implied rather than written. For example, `do_reionization` fixtures carry no `params.ReionizationOn`: SAGE26 evaluates that gate at the caller (`infall_recipe`, `strip_from_satellite`), and on the Mimic side it is the package's choice to run `sage26_reionization`; `add_infall_to_hot` and `reincorporate_gas` fixtures carry `params.CGMrecipeOn` but no reionization params; `get_disk_radius` fixtures carry no params at all. Likewise no fixture sets `Regime`: SAGE26 does not read it on R0, the oracle leaves it at its poison value -1, and -1 is what every R0 galaxy holds (it is initialised to -1 and only reclassified when `CGMrecipeOn` is 1).

| Owning module | Routine | Call-site arithmetic reproduced | Output |
|---|---|---|---|
| `sage26_reionization` | `do_reionization` (`R/src/model_infall.c:286`) | none | `return.reionization_modifier`; `intermediate.HaloBaryonFraction` = modifier × `BaryonFrac` |
| `sage26_prepare_infall_budget` | `infall_recipe` (`R/src/model_infall.c:67`) | the call at `R/src/core_build_model.c:415` | `return.InfallingGas`; the modifier and `HaloBaryonFraction` it used; per galaxy the ejected, ICS (with history), CGM and hot reservoirs after the call |
| `sage26_apply_infall` | `add_infall_to_hot`, non-CGM branch (`R/src/model_infall.c:351`) | `infallingGas / effective_steps` (`R/src/core_build_model.c:488`) | `intermediate.infallingGas_per_step`; the ejected and hot reservoirs |
| `sage26_reincorporation` | `reincorporate_gas` (`R/src/model_reincorporation.c:44`) | `deltaT = Age[SnapNum] - halo_age`, the `ReIncorporationFactor > 0.0` gate, `deltaT / effective_steps` (`R/src/core_build_model.c:479`, `:490-492`) | `intermediate.deltaT`, `intermediate.dt`, `intermediate.reincorporate_gas_called`; the ejected and hot reservoirs |
| `sage26_satellite_stripping` | `strip_from_satellite`, non-CGM routing (`R/src/model_infall.c:225`) | the Type 1 gate (`R/src/core_build_model.c:493-498`) | the satellite's modifier and `HaloBaryonFraction`; `intermediate.strip_from_satellite_called`; hot and CGM reservoirs of every galaxy |
| `sage26_set_disk_scale_radius` | `get_disk_radius` (`R/src/model_misc.c:203`) | the float store into `DiskScaleRadius` (`R/src/core_build_model.c:297`) | `return.r_disk` (double) and `galaxy[0].DiskScaleRadius` (float) |

`HaloBaryonFraction` is the double product `reionization_modifier * BaryonFrac`, rounded once. SAGE26 forms the same product as the leading term of `reionization_modifier * BaryonFrac * Mvir - S` in `infall_recipe` (`R/src/model_infall.c:128-130`) and `strip_from_satellite` (`:243-244`), so the package may carry it between modules and finish the expression with the halo's `Mvir`. **The finish must stay fused.** In the oracle build (and the reference build, whose machine code is identical) clang's default contraction compiles that expression on arm64 to an `fmul` (the product, rounded once) followed by a single fused multiply-subtract: `fnmsub` (`product * Mvir - S`) in `infall_recipe`, and `fnmadd` in `strip_from_satellite` (AArch64 `fnmadd d, n, m, a` computes `-(n * m) - a`; here `n` is the product, `m` the widened `Mvir` and the addend register `a` holds `-S`, an `fneg` of the widened float baryon sum, so the result is `S - product * Mvir` with the `-1.0 *` folded in), before the division by the substep count. The package must therefore finish `HaloBaryonFraction * Mvir - S` fused; an unfused finish rounds the product first and generally differs (a build of `model_infall.c` with `-ffp-contract=off` reproduces `return.InfallingGas` in only 1 of the 15 budget fixtures). The two ways to fuse it are equivalent only where the compiler contracts. An explicit `fma(HaloBaryonFraction, Mvir, -S)` (with the `-1.0 *` and the substep division applied as SAGE26 orders them for stripping) is one correctly rounded operation everywhere, so it reproduces the oracle on any platform whose `fma` is correctly rounded, as C requires. A single contractible C expression fuses only where the compiler contracts it: with Apple clang on arm64 it does so at Mimic's own flags (`-O2` without `-march`; checked: `hbf * mvir - s` compiles to the same `fnmsub` as `fma(hbf, mvir, -s)`), so in the oracle environment the two agree; it does not under `-ffp-contract=off`, and a compiler targeting baseline x86-64 (no FMA instruction without `-mfma` or a `-march` that has it, e.g. GCC's default) cannot fuse it, so there only the explicit `fma` reproduces the oracle. The modifier and product recorded for `infall_recipe` and `strip_from_satellite` come from evaluating `do_reionization` on the same galaxy before the call; it reads only that galaxy's `Mvir` and the params, which neither routine writes. That evaluation happens before the call-site gate, so for a stripping fixture whose galaxy is not stripped (`orphan_not_called`: a Type 2 orphan with `Mvir` 0, giving 1/inf = 0) the recorded modifier and `HaloBaryonFraction` are values SAGE26's call site never computes: they are not SAGE26 state and a test must not assert them.

Each `.in` file states in its comments what the fixture exercises and, for fixtures taken from the R0 reference output (`r0_snap<N>_*`), which galaxy rows of which snapshot supplied the values. Those are end-of-snapshot output values used as plausible inputs, not reconstructed call-time state; their Age and substep-count inputs are plausible values, not SAGE26's own.

## Scope

The oracle covers the routines above in the R0 configuration. Branches that need a different configuration (`CGMrecipeOn 1`, `ReionizationOn 0`, `ReIncorporationFactor <= 0`) and states R0 never produces (negative gas or mass reservoirs) are outside its fixtures; tiny negative *metal* reservoirs, which R0 does produce, are included. A few fixtures are constructed edge states R0 does not produce but SAGE26's code defines exactly: -0.0 metal reservoirs (one from a hot metal debit that narrows to -0.0), a zero-excess satellite with `Mvir` 0, and exact-consumption equalities. Each such `.in` says so in its comments. They were added until the mutation sweep recorded in this slice's validation record found no strict/non-strict mutant that any input it tried could distinguish on R0. The sweep swapped every ordering comparison on the R0 path of these routines, their helpers and the call-site gates, alongside negation and drop mutants, and probed every survivor with constructed counter-inputs and a random edge-value fuzz. Four kinds of mutant survive only because of R0's settings, and the record shows inputs outside R0 that distinguish them: `Vvir >= Vcrit` and `Vvir >= (float)Vcrit` in `reincorporate_gas` (equivalent at `ReIncorporationFactor` 0.19), its reincorporation cap written `>=` (equivalent while the substep `dt` is positive), and the `CGMrecipeOn` negations (equivalent while `Regime` is -1). This is evidence from the inputs tried, not a proof that no other input distinguishes a survivor. It does not cover SAGE26's driver-level code (`join_galaxies_of_progenitors`, `evolve_galaxies`), which is static in `core_build_model.c` and not compiled here.

## The c04 Group: Cooling and Radio Heating

`oracle_c04.c` covers SAGE26's hot-halo cooling, its Bondi radio-mode heating, the cooling transfer and the end-of-halo energy normalisation, in the R0 configuration (`AGNrecipeOn 2`, `CGMrecipeOn 0`, `RadioModeEfficiency 0.08`, Millennium units). It uses the fixture format above unchanged: every value is an `f32`, `f64` or `i32` field, and the new names below follow the existing prefixes. `C` is `R/src/model_cooling_heating.c` and `B` is `R/src/core_build_model.c`.

### Cooling Tables, Loaded Once per Process

Every c04 run calls SAGE26's own `read_cooling_functions()` (`R/src/core_cool_func.c:65-99`) exactly once, before the routine, with `ROOT_DIR` naming the staged source as `build.sh` sets it, so the tables come from `<staged SAGE26 root>/src/auxdata/CoolFunctions`. The loader adds `log10(0.02)` to its static metallicity grid each time it runs, so a second call in one process would shift the grid; one fixture per process makes that impossible.

### Routines

| Owning module | Routine | Call-site arithmetic reproduced | Output |
|---|---|---|---|
| `sage26_calculate_cooling_budget` | `get_metaldependent_cooling_rate` (`R/src/core_cool_func.c:140`), with its private `get_rate` | none; inputs `call.logTemp` and `call.logZ` | `return.lambda` |
| `sage26_calculate_cooling_budget` | `cooling_recipe_hot` (`C:135`), reaching `do_AGN_heating` (`C:587`) and the private Bondi branch of `agn_accretion_compute` (`C:526`) | `deltaT = Age[SnapNum] - halo_age` and `dt = deltaT / effective_steps` (`B:479`, `B:508`); the sequence `cooling_recipe` → `cool_gas_onto_galaxy` (`B:508-509`) | `return.coolingGas`; the state after the call; the carried intermediates below; the state after the sequence |
| `sage26_radio_mode_heating` | `do_AGN_heating` (`C:587`), called on its own with `call.coolingGas` (always positive, the gate at `C:297`), `call.x` and `call.rcool` | `deltaT` and `dt` as above | `return.coolingGas` (the suppressed mass); the state after the call |
| `sage26_apply_cooling` | `cool_gas_onto_galaxy` (`C:689`), called on its own with `call.coolingGas` | none | the state after the call |
| `sage26_finalize_cooling_heating` | `normalise_cooling_heating`: a **transcription** in `oracle_c04.c` of `B:590-604` | the whole block: `inv_deltaT = 1.0/deltaT` with `deltaT = Age[galaxies[0].SnapNum] - halo_age`, applied by `*=` to `Cooling` and `Heating` of every galaxy with `mergeType` not above 0 | per galaxy `Cooling` and `Heating` and the four diagnostics; `intermediate.deltaT`, `intermediate.inv_deltaT`, `intermediate.galaxy[i].deltaT` (that galaxy's own `Age[SnapNum] - halo_age`, which `B:591` does not use) and `intermediate.galaxy[i].normalised` (1 if it was multiplied) |

`evolve_galaxies` is static in `core_build_model.c`, which is not in the oracle's link, so the normalisation fixtures are a transcription of those lines, not a call of SAGE26 code. The normalisation's `OutflowRate` term belongs to c05; these fixtures neither set nor record it. Their inputs are `call.ngal`, `call.halo_age`, `call.Age[<snap>]` for each distinct `SnapNum`, and per galaxy `Type`, `SnapNum`, `mergeType`, `Cooling`, `Heating` and the four diagnostics; the oracle requires a galaxy with `mergeType > 0` to be `Type` 3. That rule is the Mimic package's convention, not SAGE26 behaviour: `sage26_resolve_mergers_and_disruption` sets `Type` 3 on the galaxy it merges or disrupts, while SAGE26 sets only `mergeType` when merging or disrupting (`R/src/model_mergers.c:311`, `:315`, `:902`) and sets `Type` 3 on a later snapshot (`B:243`). The transcribed SAGE26 lines read only `mergeType`.

The state written for `cooling_recipe_hot`, `do_AGN_heating` and `cool_gas_onto_galaxy` is always the same twelve fields of `galaxy[0]`: `HotGas`, `MetalsHotGas`, `ColdGas`, `MetalsColdGas`, `BlackHoleMass`, `r_heat`, `Cooling` and `Heating` (double), `tcool`, `tff`, `RcoolToRvir` and `CoolingRate`. Fields a routine does not write appear as it left them. The inputs of these three routines set the same sixteen fields: those twelve plus `Type`, `Mvir`, `Rvir` and `Vvir`. SAGE26's `r_heat` is the package's `Rheat`; the other names are the package's. The `cooling_recipe_hot` and `do_AGN_heating` inputs also carry `params.AGNrecipeOn` and `params.CGMrecipeOn`, which the oracle requires to be 2 and 0, the core-owned `params.G`, `params.Hubble_h`, `params.RadioModeEfficiency`, `params.UnitEnergy_in_cgs` and `params.UnitTime_in_s`, and (`cooling_recipe_hot` only) `params.UnitDensity_in_cgs`, each at the bit pattern SAGE26's `set_units` forms (`R/src/core_init.c:94-106`). Their time inputs are `call.Age_galaxy_snap`, `call.halo_age` and `call.effective_steps`, recorded as `intermediate.deltaT` and `intermediate.dt`.

### Carried Intermediates of `cooling_recipe_hot`

The package splits `cooling_recipe_hot` across three modules, so it carries some of the routine's local variables between them (`C:142-200`). No SAGE26 entry point exposes those locals, so the oracle obtains them as follows, with its flags fixed:

| Output field | Type | Meaning and source |
|---|---|---|
| `intermediate.return_path` | `i32` | The return path SAGE26 took: 0 the normal tail (`C:301-314`); 1 the outer guard `HotGas > 0 && Vvir > 0` failed (`C:141`); 2 the `lambda <= 0` early return (`C:159-161`); 3 the `rho_rcool <= 0` early return (`C:167-169`) |
| `intermediate.logTemp`, `intermediate.logZ` | `f64` | Lambda's arguments `log10(temp)` and `logZ`, from a **transcription** of `C:150-155` |
| `intermediate.lambda` | `f64` | SAGE26's own `get_metaldependent_cooling_rate` on those arguments |
| `intermediate.x`, `intermediate.rcool` | `f64` | `x` and `rcool`, from a **transcription** of `C:163-173` (`transcribe_cooling_locals`) |
| `intermediate.CoolingGas_postcap` | `f64` | The post-cap, pre-suppression cooling mass (`C:288-292`): the return value of a second `cooling_recipe_hot` call, on a copy of the input, with `AGNrecipeOn 0` (`C:314`) |
| `intermediate.<name>_absent` | `i32` | For each of `logTemp`, `logZ`, `lambda`, `x`, `rcool` and `CoolingGas_postcap`: 1 when the path leaves it undefined, and then `intermediate.<name>` is not written; 0 otherwise. The outer guard leaves all six undefined; `rho_rcool <= 0` leaves `rcool` and `CoolingGas_postcap` undefined |
| `intermediate.do_AGN_heating_entered` | `i32` | 1 if SAGE26 called `do_AGN_heating` (`C:297`: a positive post-cap mass, `AGNrecipeOn` being 2) |
| `intermediate.x_validated_up`, `intermediate.x_validated_down` (and the same for `rcool`) | `i32` | 1 if replacing the value by its upward (`nextafter(v, +inf)`), respectively downward (`nextafter(v, -inf)`), neighbouring double changes a double output of `do_AGN_heating` (below); 0 otherwise, and always 0 when `do_AGN_heating` was not entered |
| `intermediate.x_validated`, `intermediate.rcool_validated` | `i32` | 1 only if both the `_up` and the `_down` field are 1: the value is validated two-sided on this fixture |
| `intermediate.CoolingGas_suppressed` | `f64` | The AGN-suppressed mass `cool_gas_onto_galaxy` receives (`B:508-509`): the routine's return value |
| `intermediate.sequence.galaxy[0].<field>` | as above | The twelve fields after SAGE26's `cool_gas_onto_galaxy` has been applied to the returned mass, as `B:508-509` does. Every `cooling_recipe_hot` fixture is therefore also a sequence fixture |

**The transcription check.** On every fixture where `do_AGN_heating` is entered, the oracle feeds the transcribed `x` and `rcool`, with the post-cap mass, into SAGE26's own `do_AGN_heating` on a copy of the input. The return value and the five fields `do_AGN_heating` writes (`BlackHoleMass`, `HotGas`, `MetalsHotGas`, `r_heat`, `Heating`; `C:607-620`) must equal those of `cooling_recipe_hot` bit for bit, or the oracle refuses to write the fixture. Where `do_AGN_heating` is not entered, the `AGNrecipeOn 0` call must have produced exactly the state and return value of the real one. On the normal tail, `RcoolToRvir` must equal the transcribed `rcool / Rvir` narrowed to float.

**Validation of `x` and `rcool`.** A passing check does not constrain `x` or `rcool` on every path: `x` drops out when the Eddington cap (`C:557`) or the heating cap (`C:568-571`) binds, or when `BlackHoleMass` is 0, and `rcool` drops out of the suppression when `r_heat` is 0 (`C:592`). Even on a path where a value does reach the outputs, a one-ulp change of it is often absorbed by the roundings that follow, and in one direction only. So on each checked fixture the oracle reruns the check four times: with `x` replaced by its upward neighbouring double, then its downward one, and the same for `rcool`. Each rerun records whether the replacement changes a double output of `do_AGN_heating` (its return value or `Heating`) as `<name>_validated_up` or `<name>_validated_down`. The value is **validated** on that fixture only if both directions change an output (`<name>_validated` = 1, two-sided validation). The package's oracle tests assert `x` and `Rcool` only on fixtures where they are validated two-sided.

### Diagnostics and Early Returns

SAGE26's `init_galaxy` never initialises `CoolingRate` (`R/src/model_misc.c:172-198`), and the early returns leave several diagnostics as they were, so every fixture sets `tcool`, `tff`, `RcoolToRvir`, `CoolingRate`, `Cooling` and `Heating` explicitly. Unless an `.in` comment says otherwise they are negative bit patterns that no c04 routine writes (-11, -22, -33, -44, -55 and -66), and the output records them as SAGE26 leaves them:

- Every `cooling_recipe_hot` call first sets `tcool = 0` (`C:139`).
- The outer-guard failure (`C:141`) writes nothing else before the tail, which writes `CoolingRate` from a zero mass; `tff`, `RcoolToRvir` and `Cooling` keep their input values.
- The `lambda <= 0` and `rho_rcool <= 0` early returns come after `tcool` and `tff` are written (`C:145-148`) and leave `RcoolToRvir` (`C:175`), `Cooling` (`C:302`) and `CoolingRate` (`C:310`) at their input values. No fixture assumes a creation value SAGE26 does not define.

`lambda <= 0` is unreachable: every table value lies between -23.41 and -20.46 (log10 cgs, `R/src/core_cool_func.c:85-94`); `logZ` is clamped to the metallicity grid (`:142-146`) and `log10(T)` below 4.0 is raised to 4.0 (`:113-115`), so inside the table the rate is an interpolation between table values; and every table's last interval rises, so the extrapolation above 8.5 (`:118-128`) only raises it. Lambda is `pow(10.0, rate)` (`:158`) with `rate` at least about -23.41, a positive double, for every finite input. Infinite inputs are outside this argument (the conversion of an infinite `log10(T)` to the table index at `:117` is undefined in C) and are not R0 states. `rho_rcool <= 0` is reachable only by constructed inputs (a subnormal `Vvir` that overflows the float `Rvir / Vvir` to infinity, or a negative `Rvir`), and two fixtures cover it. `dt <= 0` is not a fixture case: on R0 the `Age[-1]` extension (`R/src/core_init.c:60-67`) makes `deltaT` positive at snapshot zero, and the package's `process()` skips or rejects a non-positive `dt`; the oracle refuses a fixture whose `dt` is not positive.

### Fixtures and Regenerating

Each `.in` file states in its comments what it exercises. Fixtures named `*_r0_snap<N>_*` take a real galaxy's values from the R0 reference output at snapshots 32 and 63 (end-of-snapshot values used as plausible inputs, not reconstructed call-time state). The R0 `do_AGN_heating`, `cool_gas_onto_galaxy` and interpolation fixtures take `x`, `rcool`, the masses and the table arguments from the oracle output of the R0 `cooling_recipe_hot` fixture of the same galaxy, and say so; the R0 normalisation fixtures convert the output's `Cooling` and `Heating` back to code units and multiply by the interval. The constructed edge states (a subnormal `Vvir`, a negative `Rvir`, `Vvir` of 10^5 to 10^6 km/s where the Eddington and reservoir caps bind, subnormal masses) are not R0 output but are defined exactly by SAGE26's code; each `.in` says so. `regenerate.sh` rewrites the c04 outputs together with c03's, as above.
