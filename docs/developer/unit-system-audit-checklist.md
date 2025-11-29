# Module Unit System Audit Checklist

**Purpose**: Systematic checklist for auditing physics modules for unit correctness

**Use**: Complete this checklist for each module to ensure unit system compliance

**Last Updated**: 2025-11-29

---

## Instructions

1. Copy this template for each module you're auditing
2. Complete all sections systematically
3. Document findings and issues
4. Prioritize fixes (blocking vs minor)
5. Create follow-up tasks for issues found

---

## Module Information

**Module Name**: ________________

**Location**: `src/modules/________________`

**Auditor**: ________________

**Date**: ________________

**Purpose**: _(Brief description of what this module does)_

---

## Audit Sections

### 1. Gravitational Constant Usage

**Does module use gravitational constant?** ☐ Yes ☐ No

If YES:
- [ ] Uses `ctx->params->G` (pre-computed value)
- [ ] Does NOT compute G from scratch
- [ ] No dimensional analysis for G conversion
- [ ] Printed G value for verification (if verbose mode)

**G Value Check**:
```bash
# Add to module init:
printf("[module_name] G = %.6f\n", ctx->params->G);
# Expected: ~43.0071
```

**Issues Found**:
```
(List any G-related issues)
```

---

### 2. Unit Conversions

- [ ] All conversions use `ctx->params->Unit*_in_cgs`
- [ ] No hardcoded unit conversion factors (e.g., 1e10, 3.086e24)
- [ ] Conversion points documented in code comments
- [ ] Only converts at specific boundaries (not mid-calculation)

**Conversion Points Identified**:
```
Location (file:line) | From Units | To Units | Purpose
---------------------|------------|----------|--------
Example: cooling.c:165 | Physical (CGS) | Code units | Cooling coefficient
(Add entries for this module)
```

**Hardcoded Factors Found**:
```
(List any hardcoded conversion factors that should use ctx->params)
```

**Issues Found**:
```
(List any conversion issues)
```

---

### 3. Input Assumptions

- [ ] Assumes ALL input properties in code units
- [ ] No implicit physical unit assumptions
- [ ] Cosmological parameters via `ctx->params` (Omega, OmegaLambda, Hubble_h)
- [ ] No global variable access (uses ModuleContext exclusively)

**Properties Accessed**:
```
Core Properties (halo_properties.yaml):
- [ ] Mvir, Rvir, Vvir (virial properties)
- [ ] Pos, Vel (position/velocity)
- [ ] infallMvir, infallVvir, infallVmax (infall properties)
- [ ] Other: ________________

Galaxy Properties (model_properties.yaml):
- [ ] ColdGas, HotGas, EjectedMass
- [ ] StellarMass, BulgeMass
- [ ] MetalsColdGas, MetalsHotGas, etc.
- [ ] Other: ________________
```

**Unit Assumptions Documented**:
- [ ] Module header documents unit conventions
- [ ] Function comments specify expected units
- [ ] Clear which inputs are code units vs physical

**Issues Found**:
```
(List any input assumption issues)
```

---

### 4. Output Properties

- [ ] All module outputs in code units (unless explicit `output_convert`)
- [ ] Output ranges physically reasonable
- [ ] Properties validated in scientific tests
- [ ] No unit conversion in module (I/O layer handles this)

**Properties Modified by Module**:
```
Property Name | Action | Units | Range Check
--------------|--------|-------|------------
Example: ColdGas | Modified | 10^10 Msun/h | [0, 10000]
(Add entries for this module)
```

**Range Validation**:
```bash
# Check property ranges in scientific tests:
assert all(data['PropertyName'] >= min_value)
assert all(data['PropertyName'] <= max_value)
```

**Issues Found**:
```
(List any output unit issues)
```

---

### 5. Physical Constants

- [ ] Uses constants from `src/include/constants.h`
- [ ] All constants in CGS (GRAVITY, BOLTZMANN, PROTONMASS, etc.)
- [ ] No magic numbers for physical constants
- [ ] Solar mass, parsec, etc. from constants.h

**Constants Used**:
```
Constant | Source | Value | Units
---------|--------|-------|------
Example: BOLTZMANN | constants.h | 1.3806e-16 | erg/K
(Add entries for this module)
```

**Magic Numbers Found**:
```
(List any hardcoded physical constants that should be from constants.h)
```

**Issues Found**:
```
(List any constant-related issues)
```

---

### 6. Shared Utilities

- [ ] Checked `src/modules/shared/` for existing utilities
- [ ] Uses shared calculations where available
- [ ] No duplication of physics calculations
- [ ] New utilities added to `shared/` if reusable

**Shared Utilities Used**:
- [ ] metallicity.h (calculate_metallicity, etc.)
- [ ] disk_radius.h (calculate_disk_radius, etc.)
- [ ] reionization.h (calculate_reionization_modifier)
- [ ] Other: ________________

**Potential Shared Utilities**:
```
(List calculations that could be moved to shared/ for reuse)
```

**Issues Found**:
```
(List any utility-related issues or duplications)
```

---

### 7. Code Organization

- [ ] Module in `src/modules/` directory (not core)
- [ ] Helper functions in `src/modules/shared/` if used by multiple modules
- [ ] No physics logic in core directories
- [ ] Metadata in `module_info.yaml` complete

**File Locations**:
```
Main module: src/modules/________________/________________.c
Header: src/modules/________________/________________.h
Metadata: src/modules/________________/module_info.yaml
Tests: tests/unit/test_unit_________________.c
```

**Core-Physics Separation**:
- [ ] No galaxy property access outside modules/ (except auto-generated I/O)
- [ ] No physics calculations in src/core/, src/io/, src/util/
- [ ] Auto-generated code correctly includes this module's properties

**Issues Found**:
```
(List any organization or separation issues)
```

---

### 8. Documentation

- [ ] Unit assumptions documented in module header
- [ ] Complex conversions explained in comments
- [ ] References to papers include unit conventions
- [ ] README or inline docs explain physics clearly

**Documentation Completeness**:
```
File Header:
- [ ] Purpose/description clear
- [ ] Unit conventions stated
- [ ] References listed

Function Comments:
- [ ] Parameters documented (with units)
- [ ] Return values documented (with units)
- [ ] Side effects noted

Code Comments:
- [ ] Complex calculations explained
- [ ] Unit conversions annotated
- [ ] Physics assumptions stated
```

**Issues Found**:
```
(List any documentation gaps)
```

---

### 9. Testing

- [ ] Unit tests exist for module
- [ ] Integration tests verify module in full pipeline
- [ ] Scientific tests validate physics accuracy
- [ ] Unit-specific tests verify correct unit handling

**Test Coverage**:
```
Unit Tests (tests/unit/):
- [ ] Test file exists: test_unit_________________
- [ ] Tests G value if used
- [ ] Tests unit conversion logic
- [ ] Tests range validation

Integration Tests (tests/integration/):
- [ ] Module in full pipeline test
- [ ] Properties verified in output

Scientific Tests (tests/scientific/):
- [ ] Physics validation against SAGE/reference
- [ ] Mass/energy conservation checked
- [ ] Property ranges verified
```

**Test Results**:
```bash
# Run module tests:
make test-unit
make test-integration
make test-scientific

# Document results:
PASS/FAIL: ________________
```

**Issues Found**:
```
(List any testing gaps or failures)
```

---

### 10. Specific Physics Checks

**For Cooling Modules**:
- [ ] Cooling coefficient correctly converted from physical to code units
- [ ] Temperature calculations use consistent units
- [ ] Cooling function lookup tables in correct units
- [ ] AGN heating energy calculations in code units

**For Star Formation Modules**:
- [ ] Star formation rate in code units (10^10 Msun/h / time_unit)
- [ ] Timescales (dynamical, depletion) in code units
- [ ] Feedback energy calculations consistent
- [ ] Mass conservation verified

**For Merger Modules**:
- [ ] Merger timescales use ctx->params->G (not recomputed)
- [ ] Dynamical friction formula uses code units
- [ ] Mass ratios dimensionless (unit-independent)
- [ ] Orbital parameters in code units

**For Reincorporation Modules**:
- [ ] Reincorporation timescale in code units
- [ ] Ejected gas tracking in code units
- [ ] No implicit time unit assumptions

**For Disk Instability Modules**:
- [ ] Disk scale radius in code units (Mpc/h)
- [ ] Stability criterion dimensionless
- [ ] Angular momentum conserved (units consistent)

**Module-Specific Checks**:
```
(Add checks specific to this module's physics)
```

**Issues Found**:
```
(List any physics-specific unit issues)
```

---

## Summary

### Issues Found

**Blocking Issues** (must fix before release):
```
1.
2.
3.
```

**Important Issues** (should fix soon):
```
1.
2.
3.
```

**Minor Issues** (nice to fix):
```
1.
2.
3.
```

### Recommendations

```
(Overall recommendations for this module)
```

### Comparison to SAGE

If ported from SAGE:
- [ ] Compared implementation to SAGE source
- [ ] Verified same unit conventions used
- [ ] Tested against SAGE reference output
- [ ] Documented any intentional differences

**SAGE Comparison Results**:
```
Property | Mimic | SAGE | Match? | Notes
---------|-------|------|--------|------
Example: ColdGas | 1.5 | 1.5 | YES | -
(Add comparison results)
```

---

## Follow-Up Actions

**Immediate**:
- [ ] Fix blocking issues
- [ ] Re-run tests
- [ ] Verify fixes

**Short-term**:
- [ ] Fix important issues
- [ ] Improve documentation
- [ ] Add missing tests

**Long-term**:
- [ ] Fix minor issues
- [ ] Refactor if needed
- [ ] Extract shared utilities

---

## Sign-Off

**Auditor**: ________________

**Date**: ________________

**Status**: ☐ Approved ☐ Issues Found ☐ Needs Re-audit

**Notes**:
```
(Any additional notes or observations)
```
