# Physics Modules

**Purpose:** Runtime-configurable galaxy physics modules.

**Status:** Module system complete (Phase 3). Contains proof-of-concept modules for infrastructure validation.

**Current Modules:**
- `simple_cooling/` - Placeholder cooling module (Phase 3)
- `simple_sfr/` - Placeholder star formation module (Phase 3)
- `module_init.c` - Module registration (manual, will be automated in Phase 5)

**Note:** Core halo physics (virial calculations, tracking) are in `src/core/halo_properties/` as they are core infrastructure, not modular galaxy physics.

**Next Steps (Phase 4):** Production-quality cooling module based on published physics (e.g., Sutherland & Dopita 1993). See `docs/architecture/roadmap_v4.md` for details.
