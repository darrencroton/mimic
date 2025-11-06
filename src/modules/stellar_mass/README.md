# Stellar Mass Module

## Purpose

Minimal proof-of-concept galaxy physics module demonstrating the module system infrastructure.

## Physics

Simple stellar mass prescription:

```
StellarMass = 0.1 * Mvir
```

Where:
- `StellarMass` is total stellar mass in units of 10^10 Msun/h
- `Mvir` is halo virial mass in units of 10^10 Msun/h
- 0.1 is the stellar efficiency parameter

## Implementation

This module demonstrates:
- Module interface implementation (`init`, `process_halos`, `cleanup`)
- Galaxy property updates via `GalaxyData` structure
- Integration with module registry system
- Physics-agnostic core interaction

## Files

- `stellar_mass.h` - Public interface (registration function)
- `stellar_mass.c` - Implementation (physics and module interface)
- `README.md` - This file

## Usage

The module is registered in `main.c`:

```c
stellar_mass_register();
module_system_init();
```

And executed automatically during tree processing in `process_halo_evolution()`.

## Future Extensions

This simplified model could be extended to:
- Depend on halo properties (circular velocity, concentration)
- Vary with redshift (cosmic SFR history)
- Include stellar population synthesis
- Track stellar mass in different components (disk, bulge, ICL)
