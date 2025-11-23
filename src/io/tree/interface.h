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

/**
 * @brief   Set file endianness for byte swapping
 * @param   endianness  0=little-endian, 1=big-endian
 */
void set_file_endianness(int endianness);

/**
 * @brief   Get current file endianness setting
 * @return  Current endianness (0=little-endian, 1=big-endian)
 */
int get_file_endianness(void);

/**
 * @brief   Read from file with endianness handling
 * @param   ptr     Buffer to read into
 * @param   size    Size of each element
 * @param   nmemb   Number of elements to read
 * @param   stream  File stream to read from
 * @return  Number of elements successfully read
 */
size_t myfread(void *ptr, size_t size, size_t nmemb, FILE *stream);

/**
 * @brief   Write to file with endianness handling
 * @param   ptr     Buffer to write from
 * @param   size    Size of each element
 * @param   nmemb   Number of elements to write
 * @param   stream  File stream to write to
 * @return  Number of elements successfully written
 */
size_t myfwrite(void *ptr, size_t size, size_t nmemb, FILE *stream);

/**
 * @brief   Seek in file (wrapper for fseek)
 * @param   stream  File stream to seek in
 * @param   offset  Number of bytes to offset
 * @param   whence  Position to seek from (SEEK_SET, SEEK_CUR, SEEK_END)
 * @return  0 on success, -1 on error
 */
int myfseek(FILE *stream, long offset, int whence);

#endif /* IO_TREE_H */
