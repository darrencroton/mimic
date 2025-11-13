# Archived Modules

**Purpose**: This directory contains physics modules that have been retired from active development but are preserved for historical reference.

## Why Archive Modules?

Modules are archived when:
- They are replaced by production-quality implementations (e.g., PoC modules replaced by SAGE modules)
- They are superseded by better physics implementations
- They are no longer maintained or scientifically relevant

## Archive Policy

**Archived modules**:
- ✅ Are preserved in git history for reference
- ✅ Are excluded from compilation (underscore prefix `_archive`)
- ✅ Are excluded from auto-discovery systems
- ✅ Have their module_info.yaml preserved for documentation
- ❌ Are NOT compiled into mimic executable
- ❌ Are NOT available in `EnabledModules` parameter
- ❌ Are NOT maintained or updated

## Archive Process

When archiving a module:

1. **Move module to archive**:
   ```bash
   git mv src/modules/MODULE_NAME src/modules/_archive/MODULE_NAME
   ```

2. **Update documentation**:
   - Remove from user-facing module lists
   - Add entry to `ARCHIVE_LOG.md`
   - Update references to note module is archived

3. **Regenerate code**:
   ```bash
   make generate-modules
   make clean && make
   ```

4. **Verify**:
   - Confirm module doesn't appear in `make generate-modules` output
   - Confirm tests still pass
   - Confirm no compilation references remain

## Accessing Archived Modules

To examine archived module code:
- Browse this directory
- Check out specific git commits where module was active
- Read `ARCHIVE_LOG.md` for archival rationale

## See Also

- `ARCHIVE_LOG.md` - Log of all archived modules with dates and reasons
- `docs/developer/module-archiving-guide.md` - Complete archiving guide
- `docs/architecture/vision.md` - Architectural principles (Principle #1: Physics-Agnostic Core)

---

**Note**: The underscore prefix `_archive` ensures this directory is excluded from module discovery systems, maintaining the physics-agnostic core architecture.
