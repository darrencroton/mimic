#ifndef CORE_PROTO_H
#define CORE_PROTO_H

#include "config.h"
#include "globals.h"
#include "types.h"
#include "memory.h"

/* Shared driver adapters (src/core/halo_evolution.c); each driver passes its
   own FoF workspace. */
void process_halo_evolution(struct HaloInputView view, struct Halo *workspace, int halonr,
                            int ngal);
int count_fof_subhalos(struct HaloInputView view, int first_fof_halo);
struct HaloInitPayload make_halo_init_payload(struct HaloInputView view, int halonr);

/* Tree driver (src/core/build_model.c) */
void build_halo_tree(int halonr, int unit, int depth);
int join_progenitor_halos(struct HaloInputView view, int halonr, int nstart, int unit);
int find_most_massive_progenitor(struct HaloInputView view, int halonr);
void free_tree_driver_scratch(void);

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
double get_virial_velocity(struct HaloInputView view, int halonr);
double get_virial_radius(struct HaloInputView view, int halonr);
double get_virial_mass(struct HaloInputView view, int halonr);

/* Snapshot driver (src/core/snapshot_driver.c) */
struct InheritanceProgenitorGalaxy; /* core/inheritance.h */

void run_snapshot_driver(void);

/* Incomplete-output cleanup for snapshot-ordered runs. The snapshot driver
 * registers both of its output paths (the single partition file and the master
 * file) when it creates the partition, and — unlike the tree driver, whose
 * registry is cleared as each partition completes — keeps them registered after
 * run_snapshot_driver() returns. main.c writes the master only after that
 * return, so the registration is disarmed by snapshot_driver_clear_output_paths()
 * once write_master_file() has succeeded; any failure before that point runs
 * snapshot_driver_remove_incomplete_outputs() from bye() and leaves no output
 * behind. Both are no-ops for a tree-ordered run, which registers nothing. */
void snapshot_driver_remove_incomplete_outputs(void);
void snapshot_driver_clear_output_paths(void);

/* Snapshot-side counterparts of the tree driver's progenitor lookup
 * (build_model.c). They take the previous generation as one bundle so the
 * current and previous slabs cannot be transposed, and are declared here — as
 * the tree-side pair is — so the fixture package's unit tests can drive them
 * directly with two synthetic slabs. */
int snapshot_find_most_massive_progenitor(struct HaloInputView view,
                                          const struct SnapshotGatherContext *prev, int halonr);
int64_t snapshot_count_progenitor_galaxies(struct HaloInputView view,
                                           const struct SnapshotGatherContext *prev, int halonr);
void snapshot_gather_progenitor_galaxies(struct HaloInputView view,
                                         const struct SnapshotGatherContext *prev, int halonr,
                                         int first_occupied,
                                         struct InheritanceProgenitorGalaxy *progenitors);

#endif /* #ifndef CORE_PROTO_H */
