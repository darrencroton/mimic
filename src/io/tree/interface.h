#ifndef IO_TREE_H
#define IO_TREE_H

/**
 * @brief   Load a specific merger tree from file
 * @param   treenr      Tree number to load
 *
 * Dispatches through the active reader (MimicConfig.reader).
 */
void load_tree(int treenr);

/**
 * @brief   Load merger tree metadata table from file
 * @param   filenr      File number to load
 *
 * Dispatches through the active reader (MimicConfig.reader).
 */
void load_tree_table(int filenr);

/**
 * @brief   Free merger tree metadata table and close the input file
 */
void free_tree_table(void);

/**
 * @brief   Free all halo and tree memory
 */
void free_halos_and_tree(void);

#endif /* IO_TREE_H */
