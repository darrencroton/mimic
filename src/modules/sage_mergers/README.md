# SAGE Mergers Module

The `sage_mergers` module implements galaxy merger physics including dynamical friction timescales (Binney & Tremaine 1987), galaxy combination with mass/metal transfer, black hole growth (Kauffmann & Haehnelt 2000), quasar-mode feedback, merger-induced starbursts (Somerville et al. 2001), and morphological transformations. Major mergers (mass ratio > 0.3) trigger violent relaxation converting disks to bulges. Satellites can be tidally disrupted, transferring stars to intracluster light. The module provides physics functions for core tree processing to call when mergers occur.

**Configuration Example**:
```
EnabledModules  sage_infall,sage_cooling,sage_starformation_feedback,sage_mergers
SageMergers_ThreshMajorMerger  0.3
SageMergers_BlackHoleGrowthRate  0.01
SageMergers_QuasarModeEfficiency  0.001
```

**Additional Comments**: Merger detection and triggering handled by core, not this module (physics-agnostic architecture). Star formation history (SFH) arrays and disk instability checks deferred pending future design work.

**References**: Binney & Tremaine (1987), Somerville et al. (2001), Kauffmann & Haehnelt (2000), Croton et al. (2006, 2016)
