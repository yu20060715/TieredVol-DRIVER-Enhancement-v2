#ifndef TIEREDVOL_WARMUP_H
#define TIEREDVOL_WARMUP_H

#include <stdint.h>

int tv_warmup_device(const char *path, uint64_t target_bytes);

#endif
