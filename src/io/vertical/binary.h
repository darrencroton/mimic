#ifndef VERTICAL_BINARY_H
#define VERTICAL_BINARY_H

/**
 * @file    vertical/binary.h
 * @brief   L-Halo binary merger-tree reader callbacks.
 */

/** @brief Open the binary file for this partition and read its tree-count table. */
void open_partition_binary(int output_id);
/** @brief Load halo data for one tree from the open binary file. */
void load_unit_binary(int unit);
/** @brief Close the open binary file handle. */
void close_partition_binary(void);

#endif /* VERTICAL_BINARY_H */
