#ifndef CMD_CREATE_H
#define CMD_CREATE_H

#include <stddef.h>
#include "tieredvol_discover.h"

int cmd_create(int argc, char *argv[]);
void make_target(char *out, size_t sz, const char *disk);
void cleanup_create(const char *name, disk_t *valid, int valid_disks);

#endif