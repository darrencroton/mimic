#ifndef UTIL_MEMORY_H
#define UTIL_MEMORY_H

#include <stddef.h>

/* Memory categories for component-level tracking */
typedef enum {
  MEM_UNKNOWN = 0, /* Unclassified; used as default before category is known */
  MEM_GALAXIES,    /* Galaxy struct allocations */
  MEM_HALOS,       /* Halo workspace allocations */
  MEM_TREES,       /* Merger-tree read buffers */
  MEM_IO,          /* File I/O buffers and HDF5 scratch */
  MEM_UTILITY,     /* Miscellaneous utility allocations */
  MEM_MAX_CATEGORY /* Sentinel for bounds checking */
} MemoryCategory;

/* Memory reporting levels */
#define MEMORY_REPORT_NONE 0
#define MEMORY_REPORT_MINIMAL 1
#define MEMORY_REPORT_DETAILED 2

/* Configuration */
#ifndef DEFAULT_MAX_MEMORY_BLOCKS
#define DEFAULT_MAX_MEMORY_BLOCKS 50000 /* Increased for deep copy of galaxy data per halo */
#endif

/* Memory allocation utilities */
void init_memory_system(unsigned long max_blocks);
void *mymalloc(size_t size);
void *mymalloc_cat(size_t size, MemoryCategory category);
void *myrealloc(void *ptr, size_t size);
void *myrealloc_cat(void *ptr, size_t size, MemoryCategory category);
void myfree(void *ptr);

/* Memory reporting and debugging */
void set_memory_reporting(int level);
void print_allocated(void);
void print_allocated_by_category(void);
void print_memory_brief(void);
void check_memory_leaks(void);
int validate_memory_block(void *ptr);
int validate_all_memory(void);
void cleanup_memory_system(void);

#endif /* UTIL_MEMORY_H */
