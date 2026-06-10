#ifndef IO_TREE_H
#define IO_TREE_H

/**
 * @brief   Load a specific merger tree from file
 * @param   treenr      Tree number to load
 * @param   TreeType    Type of tree format (binary, HDF5, etc.)
 */
void load_tree(int treenr, enum Valid_TreeTypes TreeType);

/**
 * @brief   Load merger tree metadata table from file
 * @param   filenr      File number to load
 * @param   my_TreeType Type of tree format (binary, HDF5, etc.)
 */
void load_tree_table(int filenr, enum Valid_TreeTypes my_TreeType);

/**
 * @brief   Free merger tree metadata table
 * @param   my_TreeType Type of tree format (binary, HDF5, etc.)
 */
void free_tree_table(enum Valid_TreeTypes my_TreeType);

/**
 * @brief   Free all halo and tree memory
 */
void free_halos_and_tree(void);

#endif /* IO_TREE_H */
