# SAGE Cooling & AGN Heating Module

The `sage_cooling` module implements gas cooling from hot halos onto cold disks with AGN radio-mode feedback. Hot gas cools using metallicity-dependent Sutherland & Dopita (1993) tables, with two regimes: rapid cold accretion (r_cool > R_vir) or gradual hot halo cooling. Black holes grow via three accretion modes (empirical, Bondi-Hoyle, cold cloud) with Eddington limiting, producing radio-mode heating that suppresses cooling in massive galaxies. The module tracks cumulative cooling and heating energies, maintaining energy conservation throughout.

## Parameters

This module requires the following parameters in the input YAML file:

- **AGNrecipeOn** (int): AGN feedback mode (0=off, 1=empirical, 2=Bondi-Hoyle, 3=cold cloud accretion)
- **CoolFunctionsDir** (string, path): Directory containing cooling function tables (metal-dependent cooling rates)
- **RadioModeEfficiency** (double): Efficiency of radio-mode AGN feedback (fraction of accretion energy coupled to gas heating)

**Configuration Example**:
```
EnabledModules  sage_infall,sage_cooling
SageCooling_RadioModeEfficiency  0.01
SageCooling_AGNrecipeOn  1
```

**Additional Comments**: 

**References**: White & Frenk (1991), Sutherland & Dopita (1993), Croton et al. (2006, 2016)
