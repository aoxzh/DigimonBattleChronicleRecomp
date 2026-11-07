/* Android only: the system document picker for the disc image (GameActivity.java). */
#ifndef DIGIMON_ANDROID_GLUE_H
#define DIGIMON_ANDROID_GLUE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Opens the picker; the result arrives asynchronously. */
void android_pick_disc(void);
/* 1 when a disc was picked since the last call: a /proc/self/fd path and its display name. */
int android_take_picked_disc(char *path, size_t path_size, char *name, size_t name_size);
#ifdef __cplusplus
}
#endif
#endif
