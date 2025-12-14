# SAGE Calculate Cooling Module

Calculates gas cooling budget from hot halo to cold disk based on metallicity-dependent cooling functions.

## Physics

Implements two cooling regimes based on the cooling radius (Rcool):

**Cold Accretion Regime (Rcool > Rvir)**
- Rapid cooling throughout the entire halo
- Cooling rate: `CoolingGas = HotGas × (Vvir / Rvir) × dt`
- Dominant in low-mass halos and high-redshift systems

**Hot Halo Cooling Regime (Rcool < Rvir)**
- Cooling occurs only within the cooling radius
- Cooling rate: `CoolingGas = (HotGas / Rvir) × (Rcool / (2 × tcool)) × dt`
- Dominant in massive halos with hot atmospheres

## Cooling Functions

Uses Sutherland & Dopita (1993) cooling function tables covering:
- **Temperature range**: 10^4 to 10^8.5 K
- **Metallicity range**: Primordial (Z=0) to super-solar (Z=2 Z_sun)
- **8 metallicity bins**: Z/Z_sun = [0, 10^-3, 10^-2, 10^-1.5, 10^-1, 10^-0.5, 10^0, 10^0.5]

Cooling tables are located in `CoolFunctions/` directory and loaded at module initialization.

## Dependencies

**Properties**:
- HotGas (read) - Hot gas reservoir mass
- MetalsHotGas (read) - Metal content in hot gas
- CoolingGas (write) - Calculated cooling for this substep

## Pipeline Position

Runs in `phase_1` each substep to calculate cooling budget.

**Execution order** (phase_1):
1. sage_calculate_cooling - Calculates cooling
2. sage_radio_mode_heating - AGN suppresses cooling
3. sage_add_cooling - Transfers cooling to cold gas

## Virial Temperature

Virial temperature calculated from virial velocity:
```
T_vir = 35.9 × Vvir^2  (K, km/s)
```

## References

- White & Frenk (1991) - Cooling flow model
- Sutherland & Dopita (1993) - Cooling function tables
- Croton et al. (2006, 2016) - SAGE implementation
