# SAGE Reincorporation Module

The `sage_reincorporation` module returns ejected gas to the hot halo reservoir. Halos with virial velocities exceeding V_crit ≈ 445 km/s (tunable via `ReIncorporationFactor`) can gravitationally recapture supernova-ejected gas. The reincorporation rate is proportional to (V_vir/V_crit - 1) × M_ejected × V_vir/R_vir, meaning more massive halos reincorporate faster. This completes the gas cycling: hot → cold → stars → ejected → hot. Metallicity is preserved during transfer. Only central galaxies reincorporate; satellites cannot access the halo-scale ejected reservoir.

**Configuration Example**:
```
EnabledModules  sage_infall,sage_cooling,sage_starformation_feedback,sage_reincorporation
SageReincorporation_ReIncorporationFactor  1.0
```

**Additional Comments**:

**References**: Guo et al. (2011), Croton et al. (2006, 2016)
