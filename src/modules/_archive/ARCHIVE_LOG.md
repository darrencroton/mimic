# Module Archive Log

This log tracks all modules that have been archived, including the date, reason, and replacement (if applicable).

## Archive Format

Each entry includes:
- **Module Name**: Original module name
- **Date Archived**: When module was moved to archive
- **Reason**: Why module was archived
- **Replaced By**: New module that supersedes this one (if applicable)
- **Last Active Version**: Git commit hash where module was last active
- **Notes**: Additional context or migration information

---

## Archived Modules

### simple_cooling

- **Date Archived**: 2025-11-13
- **Reason**: Proof-of-Concept module superseded by production SAGE physics implementation
- **Replaced By**: `sage_cooling` (implements full SAGE cooling physics with metallicity-dependent cooling rates)
- **Last Active Version**: `22e6ec15fd51e443fd79d63e8e57634ef822f743` (refactor: standardize module test naming and formatting)
- **Notes**:
  - Simple cooling was a PoC module demonstrating the module system architecture
  - Implemented basic Sutherland & Dopita (1993) cooling curve
  - No metallicity dependence or AGN feedback
  - Successfully validated module registration, parameter handling, and pipeline execution
  - Archived as part of Phase 4.2 SAGE physics implementation
  - See `docs/architecture/module-implementation-log.md` for implementation details

---

## Future Archives

Modules planned for archival:
- `simple_sfr` - Will be replaced by `sage_starformation_feedback` (Phase 4.2+)

---

**Maintenance**: Update this log whenever a module is archived. Include all required fields for historical tracking.
