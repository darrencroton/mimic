#ifndef IO_TREE_H
#define IO_TREE_H

/**
 * @brief   Load a specific unit (merger tree) from the open partition
 * @param   unit        Unit (tree) index to load
 *
 * Dispatches through the active reader (MimicConfig.reader).
 */
void load_unit(int unit);

/**
 * @brief   Open a partition and load its unit (tree) metadata table
 * @param   output_id   Output id of the partition
 *
 * Dispatches through the active reader (MimicConfig.reader).
 */
void open_partition(int output_id);

/**
 * @brief   Free the partition's unit metadata and close the input file
 */
void close_partition(void);

/**
 * @brief   Free the current unit's halo and tree memory
 */
void free_unit_halos(void);

#endif /* IO_TREE_H */
