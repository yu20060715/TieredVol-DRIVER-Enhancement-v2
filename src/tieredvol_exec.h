#ifndef TIEREDVOL_EXEC_H
#define TIEREDVOL_EXEC_H

#include <stddef.h>

int tv_exec_run(const char *path, char *const argv[]);
int tv_exec_quiet(const char *path, char *const argv[]);
int tv_exec_sudo(char *const argv[], int quiet);
int tv_exec_capture(const char *path, char *const argv[], char *out, size_t outsize);
int tv_exec_with_stdin(const char *path, char *const argv[], const char *stdin_data);

#endif
