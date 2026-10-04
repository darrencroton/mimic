#ifndef CORE_PROTO_H
#define CORE_PROTO_H

#include "config.h"
#include "globals.h"
#include "types.h"
#include "memory.h"

/* Every halo index, and every count or workspace offset derived from one, is
   int64_t on both drivers: a horizontal slab index can exceed int32, and the
   vertical driver shares these types so there is one index type throughout. */

struct FoFWorkspace;        /* core/fof_workspace.h */
struct OutputBufferSegment; /* core/output_buffer.h */

/* Shared driver adapters (src/core/halo_evolution.c); each driver passes its
   own FoF workspace descriptor, whose count is the FoF group's row count. */
void process_halo_evolution(struct HaloInputView view, struct FoFWorkspace *ws, int64_t halonr);
int64_t count_fof_subhalos(struct HaloInputView view, int64_t first_fof_halo);
struct HaloInitPayload make_halo_init_payload(struct HaloInputView view, int64_t halonr);

/**
 * @brief   Close one subhalo slice just joined into the workspace
 *
 * Stamps the FoF central's catalog virial mass (get_virial_mass()) as CentralMvir
 * on workspace rows [workspace_start, ws->count) and fills @p segment for the
 * output marshaller (source halo, its snapshot, the slice's workspace range, no
 * output yet).
 *
 * @param   view             Input view the slice's source halo lives in
 * @param   ws               Workspace whose count already includes the slice
 * @param   workspace_start  First workspace row of the slice
 * @param   source_halo      Input index of the slice's subhalo
 * @param   segment          Output segment to fill
 */
void record_subhalo_slice(struct HaloInputView view, struct FoFWorkspace *ws,
                          int64_t workspace_start, int64_t source_halo,
                          struct OutputBufferSegment *segment);

/**
 * @brief   Evaluate a driver's created-record identity space and log the verdict
 *
 * Applies mimic_created_record_space_fits() (galaxy_id.h) and logs units,
 * rows_per_unit, its source, the radix and the verdict at INFO, in the one
 * format both drivers share. The verdict is recorded, never acted on.
 *
 * @param   driver                "vertical" or "horizontal" (a string literal; stored)
 * @param   reader_name           Active reader's registered name, for the log line
 * @param   units                 The run's unit count
 * @param   rows_per_unit         Rows per unit the encoding reserves
 * @param   rows_per_unit_source  Where rows_per_unit came from, for the log line
 * @return  The space with unit = -1; the driver publishes each unit's number itself
 */
struct RecordIdentitySpace record_identity_space_evaluate(const char *driver,
                                                          const char *reader_name, int64_t units,
                                                          int64_t rows_per_unit,
                                                          const char *rows_per_unit_source);

/* Vertical driver (src/core/build_model.c) */
void build_halo_tree(int64_t halonr, int unit, int depth);
/* The vertical driver's one FoF workspace descriptor. Defined beside the unit
   lifecycle in src/io/vertical/interface.c, which sizes it in load_unit() and
   releases it in free_unit_halos(); the driver sets its identity space after
   each load. */
struct FoFWorkspace *vertical_fof_workspace(void);
int64_t join_progenitor_halos(struct HaloInputView view, int64_t halonr, int64_t nstart, int unit);
int64_t find_most_massive_progenitor(struct HaloInputView view, int64_t halonr);
void free_vertical_driver_scratch(void);

/* Initialization (src/core/init.c) */
void init(void);
void set_units(void);
void read_snap_list(void);
double time_to_present(double z);
double integrand_time_to_present(double a, void *param);

/* Configuration (src/core/read_parameter_file.c) */
void read_parameter_file(const char *fname);
const char *timestep_scheme_name(enum TimestepScheme scheme);

/* Timestep helpers (src/core/timestep.c) */
int compute_dynamic_substeps(double time_interval, double t_dyn, int substeps_per_tdyn,
                             int max_dynamic_substeps);

/* Output writers (src/io/output/) */
void save_halos(int filenr, int tree, struct HaloInputView view);
void finalize_halo_file(int filenr);
void prepare_halo_for_output(struct HaloInputView view, const struct Halo *g, struct HaloOutput *o);

/* Virial property helpers (src/core/virial.c) */
double get_virial_velocity(struct HaloInputView view, int64_t halonr);
double get_virial_radius(struct HaloInputView view, int64_t halonr);
double get_virial_mass(struct HaloInputView view, int64_t halonr);

/* The mass-based virial helpers the catalogue functions above delegate to live
 * in the module-facing core/virial.h, so a module reaches them without proto.h. */
#include "virial.h"

/* Horizontal driver (src/core/horizontal_driver.c) */
struct InheritanceProgenitorGalaxy; /* core/inheritance.h */

void run_horizontal_driver(void);

/* Incomplete-output cleanup for horizontal runs. The horizontal driver keeps
 * two registrations with independent lifetimes: the partition file currently
 * being written, armed just before that file is created and released as soon as
 * it closes cleanly (the vertical driver's own per-partition discipline), and the
 * master file, armed at run start. main.c writes the master only after
 * run_horizontal_driver() returns, so the master registration outlives the driver
 * and is disarmed by horizontal_driver_clear_output_paths() once
 * write_master_file() has succeeded; any failure before that point runs
 * horizontal_driver_remove_incomplete_outputs() from bye(), which removes whatever
 * is still armed — the in-flight partition and the master — while every partition
 * file that already closed survives as final output. Both are no-ops for a
 * vertical run, which registers nothing. */
void horizontal_driver_remove_incomplete_outputs(void);
void horizontal_driver_clear_output_paths(void);

/* Horizontal-side counterparts of the vertical driver's progenitor lookup
 * (build_model.c). They take the retained generations as one bundle so the
 * descendant slab and a progenitor slab cannot be transposed, resolve every link
 * through its target-snapshot column, and are declared here — as the
 * vertical-side pair is — so the horizontal packages' unit tests can drive them
 * directly with synthetic slabs. The most massive progenitor is named by
 * generation and row, because a chain may span several retained generations
 * whose slab indices overlap. */
struct HorizontalProgenitorRef horizontal_find_most_massive_progenitor(
    struct HaloInputView view, const struct HorizontalGatherContext *lookup, int64_t halonr);
int64_t horizontal_count_progenitor_galaxies(struct HaloInputView view,
                                             const struct HorizontalGatherContext *lookup,
                                             int64_t halonr);
void horizontal_gather_progenitor_galaxies(struct HaloInputView view,
                                           const struct HorizontalGatherContext *lookup,
                                           int64_t halonr,
                                           struct HorizontalProgenitorRef first_occupied,
                                           struct InheritanceProgenitorGalaxy *progenitors);

/* Retention horizon of one loaded slab: the latest snapshot any of its halos
 * names as its descendant's, or the slab's own snapshot when none of them has a
 * descendant (an empty snapshot included). The driver releases the generation
 * once that snapshot has been processed. Declared for the packages' unit tests. */
struct SnapshotSlab; /* io/horizontal/reader.h */
int64_t horizontal_generation_horizon(const struct SnapshotSlab *slab);

#endif /* #ifndef CORE_PROTO_H */
