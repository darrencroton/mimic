#ifndef TREE_HDF5_H
#define TREE_HDF5_H

#ifdef HDF5
#include <hdf5.h>

// Proto-Types //

void open_partition_hdf5(int output_id);
void load_unit_hdf5(int unit);
void close_partition_hdf5(void);

#endif
#endif
