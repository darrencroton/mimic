#ifndef IO_SAVE_HDF5_H
#define IO_SAVE_HDF5_H

#include "config.h"
#include "globals.h"
#include "types.h"

extern void calc_hdf5_props(void);
extern void write_hdf5_attrs(int n, int filenr);
extern void free_hdf5_ids(void);
extern void write_master_file(void);
extern void prep_hdf5_file(char *fname);
extern void save_halos_hdf5(int filenr, int tree);
extern void write_hdf5_halo_batch(struct HaloOutput *halo_batch, int num_halos,
                                  int n, int filenr);

#endif /* #ifndef IO_SAVE_HDF5_H */
