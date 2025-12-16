# SAGE Disk Instability Module

The `sage_disk_instability` module detects disk instabilities using the Mo, Mao & White (1998) criterion (M_disk > V_max² × R_eff / G) and transfers unstable stellar mass from disk to bulge, preserving metallicity. Current implementation handles stellar redistribution; gas processing via starbursts and black hole growth deferred pending `sage_mergers` module (avoids circular dependency with `collisional_starburst_recipe`). Disk scale radius calculated empirically as 0.03 × R_vir; full spin-dependent model planned for future version.

## Parameters

This module requires the following parameters in the input YAML file:

- **StarFormingDiskFactor** (double): Factor relating disk effective radius to scale radius
- **DiskInstabilityOn** (int): Enable disk instability physics (0=off, 1=on)

**Configuration Example**:
```
EnabledModules  sage_calculate_infall,sage_cooling,sage_starformation_feedback,sage_disk_instability
SageDiskInstability_DiskInstabilityOn  1
SageDiskInstability_StarFormingDiskFactor  3.0
```

**Additional Comments**: Starburst triggering and BH growth deferred pending sage_mergers module implementation (requires `collisional_starburst_recipe` and `grow_black_hole` functions).

**References**: Mo, Mao & White (1998), Efstathiou et al. (1982), Croton et al. (2006, 2016)
