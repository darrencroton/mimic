#ifndef CORE_PROTO_H
#define CORE_PROTO_H

#include "config.h"
#include "globals.h"
#include "types.h"
#include "memory.h"

/* Tree driver (src/core/build_model.c) */
void build_halo_tree(int halonr, int unit, int depth);
void process_halo_evolution(int halonr, int ngal);
int join_progenitor_halos(int halonr, int nstart, int unit);
int find_most_massive_progenitor(int halonr);
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
void save_halos(int filenr, int tree);
void finalize_halo_file(int filenr);
void prepare_halo_for_output(const struct Halo *g, struct HaloOutput *o);

/* Virial property helpers (src/core/virial.c) */
double get_virial_velocity(int halonr);
double get_virial_radius(int halonr);
double get_virial_mass(int halonr);

#endif /* #ifndef CORE_PROTO_H */
