#ifndef IO_SAVE_BINARY_H
#define IO_SAVE_BINARY_H

/**
 * @brief   Write halos to binary output files for all snapshots
 * @param   filenr  File number being processed
 * @param   tree    Tree number being processed
 *
 * Writes all processed halos for the current tree to their respective
 * output files (one file per snapshot). Opens files on first write and
 * converts internal halo structures to output format.
 */
void save_halos(int filenr, int tree);

/**
 * @brief   Finalize binary output files by writing headers
 * @param   filenr  File number being processed
 *
 * Completes the output files after all halos have been written by seeking
 * to file beginning, writing tree/halo counts, and closing files.
 */
void finalize_halo_file(int filenr);

#endif /* IO_SAVE_BINARY_H */
