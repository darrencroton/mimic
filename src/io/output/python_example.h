#ifndef IO_OUTPUT_PYTHON_EXAMPLE_H
#define IO_OUTPUT_PYTHON_EXAMPLE_H

/**
 * @brief Write a run-specific Python example to the output directory.
 *
 * Generates example_Mvir_Len_plot.py in output_dir with hardcoded values
 * for the output format, file base name, snapshot number, and cosmology.
 * The script can be run directly with no arguments.
 *
 * @param output_dir  Path to the Mimic output directory.
 */
void write_python_example(const char *output_dir);

#endif /* IO_OUTPUT_PYTHON_EXAMPLE_H */
