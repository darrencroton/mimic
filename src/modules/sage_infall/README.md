# SAGE Infall Module

The `sage_infall` module implements cosmological gas infall and satellite stripping from the SAGE model. Central galaxies accrete baryonic gas proportional to their dark matter halo growth, with reionization suppression reducing accretion in low-mass halos following Gnedin (2000). Satellites experience environmental stripping of excess hot gas, which transfers to their central galaxy. The module also consolidates satellite ejected gas and intracluster stars to centrals, preserving metallicity throughout all transfers.

**Configuration Example**:
```
EnabledModules  sage_infall
SageInfall_BaryonFrac  0.17
```

**Additional Comments**:

**References**: Croton et al. (2006, 2016)
