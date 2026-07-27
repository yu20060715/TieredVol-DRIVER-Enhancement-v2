#ifndef TIEREDVOL_COMMON_H
#define TIEREDVOL_COMMON_H

#include <string.h>

static inline int tiered_is_valid_name(const char *name) {
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static inline int tiered_is_valid_fs(const char *fs) {
    if (!fs || !*fs) return 0;
    const char *ok[] = {"ext4","ext3","xfs","btrfs","none",NULL};
    for (int i = 0; ok[i]; i++) if (strcmp(fs, ok[i]) == 0) return 1;
    return 0;
}

static inline int tiered_is_valid_mount(const char *mp) {
    if (!mp || mp[0] != '/' || strlen(mp) >= 4096) return 0;
    for (const char *p = mp; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '/' || *p == '.' ||
              *p == '_' || *p == '-'))
            return 0;
    }
    if (strstr(mp, "/../") || strcmp(mp, "/..") == 0 ||
        (strlen(mp) >= 3 && strncmp(mp + strlen(mp) - 3, "/..", 3) == 0))
        return 0;
    return 1;
}

#endif
