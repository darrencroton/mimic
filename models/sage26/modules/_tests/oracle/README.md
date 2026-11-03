# SAGE26 Function-Level Oracle

This directory holds the tooling that turns SAGE26's own physics routines into reference fixtures for the `sage26` package. The oracle compiles SAGE26's physics sources unchanged, calls one SAGE26 routine on an explicit input, and records exactly what it computed: the routine's return value, every field it writes, and every intermediate value the package carries between modules. A package module's unit test can then run the same input through its `process()` entry and compare field by field, bit for bit.

It is reference tooling, not Mimic code: it lives under a `_tests/` path, which the Makefile's package source scan excludes (`! -path '*/_tests/*'`), and it builds into its own executable that never links Mimic code.

| File | Purpose |
|---|---|
| [`build.sh`](build.sh) | Compiles SAGE26's physics sources and `oracle_c03.c` into `output/oracle/oracle_c03` |
| [`regenerate.sh`](regenerate.sh) | Checks the manifest, runs `build.sh`, rewrites every fixture output from its input |
| [`oracle_c03.c`](oracle_c03.c) | The oracle main for the c03 (baryon supply) routines |
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

  `oracle_c03.c` additionally gets `-I<staged SAGE26 root>/src` for SAGE26's headers. The link uses GSL 2.8 (`-L/opt/homebrew/Cellar/gsl/2.8/lib -lgsl -lgslcblas` with an rpath) and `-lm`, as the reference link did; the reference also linked HDF5, which none of these sources call.
- **Floating-point contraction:** clang's default (`-ffp-contract=on`), under which a multiply and an add or subtract *within one C expression* may be fused into a single `fma` on arm64. No flag changes it, in SAGE26's build or here.
- **SAGE26 source:** commit `11b1ae851524baadb76cfd17e0b38f8a89c07224`, staged read-only at `/Volumes/Internal/results/sage26-campaign/reference-src`.

**A bit-for-bit claim holds only for the same compiler, the same flags, the same contraction behaviour and the same libm.** A different compiler or version, a different optimisation level or `-march`, a different `-ffp-contract`, or a different libm can change the last bits of a result legitimately. On any other platform the fixtures are still a close reference, but an exact-match test should report SKIP rather than FAIL.

`build.sh` refuses to build when `clang --version` or the staged source's commit differs from the above; `ORACLE_ALLOW_ANY_TOOLCHAIN=1` overrides that for experiments, and outputs built that way are not reference fixtures.

### Staged Source Equals the Reference Build's Source

It was checked that the staged source is the source the reference build compiled. The reference build compiled a copy of SAGE26 in its workspace (`/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/src`, its `ROOT_DIR`). A recursive `diff -rq` of the staged tree against that copy, excluding only the staged tree's `.git` and the copy's build products (`*.o`, `libsage.so`, `sage`), reports no difference, and the staged tree's `HEAD` is the commit above. As a further check, the machine code (`__TEXT,__text`) and read-only constants (`__TEXT,__const`) of each of the twelve objects `build.sh` produces are identical to those of the reference build's own objects for the same sources.

## What Gets Compiled

`build.sh` compiles the twelve sources SAGE26's own `tests/Makefile` links its unit tests against, in that order: `core_utils.c`, `model_misc.c`, `model_h2_chemistry.c`, `model_halo_properties.c`, `model_regimes.c`, `model_starformation_and_feedback.c`, `model_cooling_heating.c`, `model_reincorporation.c`, `model_infall.c`, `model_disk_instability.c`, `model_mergers.c`, `core_cool_func.c`. Each is compiled from the SAGE26 root as `src/<name>.c`, as the reference build did. Nothing is written in the SAGE26 tree; objects go to `output/oracle/obj/` and the executable to `output/oracle/oracle_c03` (the worktree's git-ignored `output/`). `SAGE26_SRC` and `ORACLE_OUT` override the two locations.

## Regenerating

```bash
sh models/sage26/modules/_tests/oracle/regenerate.sh
```

`regenerate.sh` first checks that `fixtures.manifest` is self-consistent and agrees exactly with the `.in` files on disk, then runs `build.sh`, then runs `output/oracle/oracle_c03 <name>.in <name>.out` for every listed fixture, and finally checks that no `.out` lacks its `.in`. Outputs are deterministic: running it twice gives byte-identical outputs. To add a fixture, write its `.in`, add it to `fixtures.manifest` (and raise the module's count), and run `regenerate.sh`.

Run one fixture by hand with `output/oracle/oracle_c03 <input> <output>`.

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

The hexadecimal value is the exact bit pattern, not a C99 `%a` literal; the decimal after `#` is for reading only and is never parsed. Names are unique within a file. The oracle rejects a missing field, a field of the wrong type, and an input field the selected routine does not read, so a misspelt name cannot be silently ignored.

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

Every struct member a routine does not read is poisoned with all bits set before the call (NaN for floating point, -1 for integers), so a read of a member the input did not set would surface as NaN in the output.

## Routines

Each fixture describes one call, with the arithmetic SAGE26's call site performs around it (`R/` is the staged SAGE26 root). All fixtures use the R0 configuration (`ReionizationOn 1`, `CGMrecipeOn 0`, `BaryonFrac 0.17`, `ReIncorporationFactor 0.19`, `Reionization_z0 8`, `Reionization_zr 7`, Millennium cosmology and units).

| Owning module | Routine | Call-site arithmetic reproduced | Output |
|---|---|---|---|
| `sage26_reionization` | `do_reionization` (`R/src/model_infall.c:286`) | none | `return.reionization_modifier`; `intermediate.HaloBaryonFraction` = modifier × `BaryonFrac` |
| `sage26_prepare_infall_budget` | `infall_recipe` (`R/src/model_infall.c:67`) | the call at `R/src/core_build_model.c:415` | `return.InfallingGas`; the modifier and `HaloBaryonFraction` it used; per galaxy the ejected, ICS (with history), CGM and hot reservoirs after the call |
| `sage26_apply_infall` | `add_infall_to_hot`, non-CGM branch (`R/src/model_infall.c:351`) | `infallingGas / effective_steps` (`R/src/core_build_model.c:488`) | `intermediate.infallingGas_per_step`; the ejected and hot reservoirs |
| `sage26_reincorporation` | `reincorporate_gas` (`R/src/model_reincorporation.c:44`) | `deltaT = Age[SnapNum] - halo_age`, the `ReIncorporationFactor > 0.0` gate, `deltaT / effective_steps` (`R/src/core_build_model.c:479`, `:490-492`) | `intermediate.deltaT`, `intermediate.dt`, `intermediate.reincorporate_gas_called`; the ejected and hot reservoirs |
| `sage26_satellite_stripping` | `strip_from_satellite`, non-CGM routing (`R/src/model_infall.c:225`) | the Type 1 gate (`R/src/core_build_model.c:493-498`) | the satellite's modifier and `HaloBaryonFraction`; `intermediate.strip_from_satellite_called`; hot and CGM reservoirs of every galaxy |
| `sage26_set_disk_scale_radius` | `get_disk_radius` (`R/src/model_misc.c:203`) | the float store into `DiskScaleRadius` (`R/src/core_build_model.c:297`) | `return.r_disk` (double) and `galaxy[0].DiskScaleRadius` (float) |

`HaloBaryonFraction` is the double product `reionization_modifier * BaryonFrac`, rounded once. SAGE26 forms the same product as the leading term of `reionization_modifier * BaryonFrac * Mvir - ...` in `infall_recipe` and `strip_from_satellite`, so the package may carry it between modules and finish the expression with the halo's `Mvir`. The modifier and product recorded for `infall_recipe` and `strip_from_satellite` come from evaluating `do_reionization` on the same galaxy before the call; it reads only that galaxy's `Mvir` and the params, which neither routine writes.

Each `.in` file states in its comments what the fixture exercises and, for fixtures taken from the R0 reference output (`r0_snap<N>_*`), which galaxy rows of which snapshot supplied the values. Those are end-of-snapshot output values used as plausible inputs, not reconstructed call-time state; their Age and substep-count inputs are plausible values, not SAGE26's own.

## Scope

The oracle covers the routines above in the R0 configuration. Branches that need a different configuration (`CGMrecipeOn 1`, `ReionizationOn 0`, `ReIncorporationFactor <= 0`) and states R0 never produces (negative gas or mass reservoirs) are outside its fixtures; tiny negative *metal* reservoirs, which R0 does produce, are included. It does not cover SAGE26's driver-level code (`join_galaxies_of_progenitors`, `evolve_galaxies`), which is static in `core_build_model.c` and not compiled here.
