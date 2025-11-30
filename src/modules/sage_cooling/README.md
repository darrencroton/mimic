# SAGE Cooling & AGN Heating Module

The `sage_cooling` module implements gas cooling from hot halos onto cold disks with AGN radio-mode feedback. Hot gas cools using metallicity-dependent Sutherland & Dopita (1993) tables, with two regimes: rapid cold accretion (r_cool > R_vir) or gradual hot halo cooling. Black holes grow via three accretion modes (empirical, Bondi-Hoyle, cold cloud) with Eddington limiting, producing radio-mode heating that suppresses cooling in massive galaxies. The module tracks cumulative cooling and heating energies, maintaining energy conservation throughout.

**Configuration Example**:
```
EnabledModules  sage_infall,sage_cooling
SageCooling_RadioModeEfficiency  0.01
SageCooling_AGNrecipeOn  1
```

**Additional Comments**: 

**References**: White & Frenk (1991), Sutherland & Dopita (1993), Croton et al. (2006, 2016)
