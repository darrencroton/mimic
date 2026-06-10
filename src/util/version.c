/**
 * @file    version.c
 * @brief   Implementation of Mimic version tracking functionality
 *
 * This file implements the functions for gathering version information and
 * creating a metadata file in the output directory. This helps with
 * reproducibility by ensuring that output data can always be traced back
 * to the exact code version that produced it.
 */

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "git_version.h"
#include "globals.h"
#include "types.h"
#include "error.h"
#include "io.h"
#include "version.h"

/* No version number is defined for Mimic */

/* Maximum length for various string buffers */
#define MAX_CMD_LENGTH 1024
#define MAX_OUTPUT_LENGTH 512

/**
 * @brief   Gets the current date and time as a formatted string
 *
 * @param   buffer    Output buffer to store the date string
 * @param   size      Size of the buffer
 *
 * The date is formatted as YYYY-MM-DDThh:mm:ssZ (ISO 8601 format)
 */
static void get_current_datetime(char *buffer, size_t size) {
  time_t now;
  struct tm *tm_info;

  time(&now);
  tm_info = localtime(&now);

  strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", tm_info);
}

/**
 * @brief   Executes a command and captures its output
 *
 * @param   command   The command to execute
 * @param   output    Buffer to store the command output
 * @param   size      Size of the output buffer
 * @return  0 on success, non-zero on error
 */
static int execute_command(const char *command, char *output, size_t size) {
  FILE *fp;

  fp = popen(command, "r");
  if (fp == NULL) {
    ERROR_LOG("Failed to execute command: %s", command);
    return 1;
  }

  if (fgets(output, size, fp) == NULL) {
    pclose(fp);
    return 1;
  }

  /* Remove trailing newline if present */
  size_t len = strlen(output);
  if (len > 0 && output[len - 1] == '\n') {
    output[len - 1] = '\0';
  }

  pclose(fp);
  return 0;
}

/**
 * @brief   Gets compiler information
 *
 * @param   compiler_buffer  Buffer to store compiler information
 * @param   size             Size of the buffer
 */
static void get_compiler_info(char *compiler_buffer, size_t size) {
#ifdef __GNUC__
  snprintf(compiler_buffer, size, "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(__clang__)
  snprintf(compiler_buffer, size, "clang %d.%d.%d", __clang_major__, __clang_minor__,
           __clang_patchlevel__);
#else
  strncpy(compiler_buffer, "unknown", size);
#endif
}

/**
 * @brief   Gets detailed system information including OS version
 *
 * @param   system_buffer   Buffer to store system information
 * @param   size            Size of the buffer
 * @return  0 on success, non-zero on error
 */
static int get_system_info(char *system_buffer, size_t size) {
  struct utsname system_info;
  char os_version[MAX_OUTPUT_LENGTH] = "";

  if (uname(&system_info) != 0) {
    ERROR_LOG("Failed to get system information");
    strncpy(system_buffer, "unknown", size);
    return 1;
  }

  /* Get more detailed OS version information */
  if (strcmp(system_info.sysname, "Darwin") == 0) {
    /* For macOS, use sw_vers command to get precise version */
    if (execute_command("sw_vers -productVersion 2>/dev/null", os_version, sizeof(os_version)) ==
        0) {
      snprintf(system_buffer, size, "macOS %s %s", os_version, system_info.machine);
    } else {
      /* Fallback to basic information */
      snprintf(system_buffer, size, "macOS %s %s", system_info.release, system_info.machine);
    }
  } else if (strcmp(system_info.sysname, "Linux") == 0) {
    /* For Linux, try to get distribution information */
    FILE *os_release = fopen("/etc/os-release", "r");
    if (os_release) {
      char line[256];
      char distro_name[128] = "";
      char distro_version[128] = "";

      while (fgets(line, sizeof(line), os_release)) {
        if (strncmp(line, "NAME=", 5) == 0) {
          char *value = line + 5;
          /* Remove quotes if present */
          if (value[0] == '"') {
            value++;
            char *end = strchr(value, '"');
            if (end)
              *end = '\0';
          }
          strncpy(distro_name, value, sizeof(distro_name) - 1);
          /* Remove newline if present */
          char *nl = strchr(distro_name, '\n');
          if (nl)
            *nl = '\0';
        } else if (strncmp(line, "VERSION_ID=", 11) == 0) {
          char *value = line + 11;
          /* Remove quotes if present */
          if (value[0] == '"') {
            value++;
            char *end = strchr(value, '"');
            if (end)
              *end = '\0';
          }
          strncpy(distro_version, value, sizeof(distro_version) - 1);
          /* Remove newline if present */
          char *nl = strchr(distro_version, '\n');
          if (nl)
            *nl = '\0';
        }
      }
      fclose(os_release);

      if (distro_name[0] && distro_version[0]) {
        snprintf(system_buffer, size, "%s %s %s %s", system_info.sysname, distro_name,
                 distro_version, system_info.machine);
      } else {
        snprintf(system_buffer, size, "%s %s %s", system_info.sysname, system_info.release,
                 system_info.machine);
      }
    } else {
      /* Fallback to basic information */
      snprintf(system_buffer, size, "%s %s %s", system_info.sysname, system_info.release,
               system_info.machine);
    }
  } else {
    /* For other OSes, use the standard uname information */
    snprintf(system_buffer, size, "%s %s %s", system_info.sysname, system_info.release,
             system_info.machine);
  }

  return 0;
}

/**
 * @brief   Gets the current username
 *
 * @param   user_buffer     Buffer to store the username
 * @param   size            Size of the buffer
 * @return  0 on success, non-zero on error
 */
static int get_username(char *user_buffer, size_t size) {
  uid_t uid = geteuid();
  struct passwd *pw = getpwuid(uid);

  if (pw == NULL) {
    ERROR_LOG("Failed to get username");
    strncpy(user_buffer, "unknown", size);
    return 1;
  }

  strncpy(user_buffer, pw->pw_name, size);
  return 0;
}

/**
 * @brief   Calculates an MD5 checksum of a file's contents
 *
 * @param   filepath       Path to the file
 * @param   digest_buffer  Buffer to store the MD5 checksum string
 * @param   size           Size of the buffer
 * @return  0 on success, non-zero on error
 */
static int calculate_file_md5_checksum(const char *filepath, char *digest_buffer, size_t size) {
  char command[MAX_CMD_LENGTH];

  /* Use md5 on macOS, md5sum on Linux systems */
#ifdef __APPLE__
  snprintf(command, MAX_CMD_LENGTH, "md5 -q \"%s\" 2>/dev/null", filepath);
#else
  snprintf(command, MAX_CMD_LENGTH, "md5sum \"%s\" 2>/dev/null | awk '{print $1}'", filepath);
#endif

  if (execute_command(command, digest_buffer, size) != 0) {
    ERROR_LOG("Failed to calculate digest for file: %s", filepath);
    strncpy(digest_buffer, "unavailable", size);
    return 1;
  }

  return 0;
}

/**
 * @brief   Creates a version metadata file in the specified output directory
 *
 * @param   output_dir         Path to the output directory
 * @param   parameter_file     Path to the parameter file used for this run
 *
 * This function creates a JSON metadata file in the output directory
 * containing information about the Mimic version, build environment,
 * git commit details, runtime parameters, and system information.
 *
 * If git information cannot be retrieved, those fields will be marked
 * as unavailable in the output.
 *
 * @return  0 on success, non-zero on error
 */
int create_version_metadata(const char *output_dir, const char *parameter_file) {
  char metadata_dir[MAX_STRING_LEN + 15];  /* +15 for "/metadata" and null terminator */
  char metadata_path[MAX_STRING_LEN + 50]; /* Extra space for the complete file path */
  FILE *metadata_file;

  /* Buffers for various metadata values */
  char current_time[64];
  char compiler_info[MAX_OUTPUT_LENGTH];
  char system_info[MAX_OUTPUT_LENGTH];
  char username[MAX_OUTPUT_LENGTH];
  char parameter_digest[MAX_OUTPUT_LENGTH];

  /* Get current date and time */
  get_current_datetime(current_time, sizeof(current_time));

  /* Get compiler and system information */
  get_compiler_info(compiler_info, sizeof(compiler_info));
  get_system_info(system_info, sizeof(system_info));

  /* Get username */
  get_username(username, sizeof(username));

  /* Calculate parameter file MD5 checksum */
  calculate_file_md5_checksum(parameter_file, parameter_digest, sizeof(parameter_digest));

  /* Make sure metadata directory exists */
  snprintf(metadata_dir, sizeof(metadata_dir), "%s/metadata", output_dir);
  if (ensure_directory_exists(metadata_dir) != 0) {
    ERROR_LOG("Failed to create metadata directory: %s", metadata_dir);
    return 1;
  }

  /* Create metadata JSON file */
  snprintf(metadata_path, sizeof(metadata_path), "%s/version_info.json", metadata_dir);
  metadata_file = fopen(metadata_path, "w");

  if (metadata_file == NULL) {
    ERROR_LOG("Failed to create metadata file: %s", metadata_path);
    return 1;
  }

  /* Write metadata in JSON format. Git identity comes from the build-time
   * git_version.h header, so it always describes the compiled executable
   * (a runtime `git` query would describe the current working directory). */
  fprintf(metadata_file, "{\n");
  fprintf(metadata_file, "  \"git_commit\": \"%s\",\n", GIT_COMMIT);
  fprintf(metadata_file, "  \"git_branch\": \"%s\",\n", GIT_BRANCH);
  fprintf(metadata_file, "  \"build_date\": \"%s\",\n", __DATE__);
  fprintf(metadata_file, "  \"run_date\": \"%s\",\n", current_time);
  fprintf(metadata_file, "  \"parameters\": {\n");
  fprintf(metadata_file, "    \"file_path\": \"%s\",\n", parameter_file);
  fprintf(metadata_file, "    \"parameter_md5_checksum\": \"%s\"\n", parameter_digest);
  fprintf(metadata_file, "  },\n");
  fprintf(metadata_file, "  \"compiler\": \"%s\",\n", compiler_info);
  fprintf(metadata_file, "  \"system\": \"%s\",\n", system_info);
  fprintf(metadata_file, "  \"user\": \"%s\"\n", username);
  fprintf(metadata_file, "}\n");

  fclose(metadata_file);

  INFO_LOG("Version metadata saved to %s", metadata_path);
  return 0;
}
